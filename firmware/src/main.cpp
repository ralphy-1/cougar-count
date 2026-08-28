#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "lane.h"
#include "pending.h"
#include "wire.h"
#include "wrap.h"      // reached()
#include "net.h"

// --- beam edges -------------------------------------------------------------
//
// Interrupts hand work to the main loop and do nothing else. An ISR that talked
// to the network would block for seconds with interrupts disabled, and every
// beam edge during that time would simply not exist.

struct Edge {
  uint8_t  pin;
  bool     broken;
  uint32_t t_ms;
};

static const uint8_t EDGE_SLOTS = 64;            // power of two, so the wrap is a mask
static volatile Edge     edges_[EDGE_SLOTS];
static volatile uint8_t  edgeHead_ = 0;
static volatile uint8_t  edgeTail_ = 0;

static void IRAM_ATTR onBeamEdge(void* arg) {
  const uint8_t pin = (uint8_t)(uintptr_t)arg;
  const uint8_t next = (uint8_t)((edgeHead_ + 1) & (EDGE_SLOTS - 1));
  if (next == edgeTail_) return;                 // full; the loop is far behind

  edges_[edgeHead_].pin    = pin;
  edges_[edgeHead_].broken = (digitalRead(pin) == (BEAM_BROKEN_LEVEL ? HIGH : LOW));
  edges_[edgeHead_].t_ms   = millis();
  edgeHead_ = next;
}

static bool takeEdge(Edge& out) {
  noInterrupts();
  const bool has = (edgeTail_ != edgeHead_);
  if (has) {
    // Field by field. Copy-assigning a volatile struct is not guaranteed to do
    // what it looks like it does, and this is the one place where a silently
    // wrong copy would mean beam edges attributed to the wrong beam.
    out.pin    = edges_[edgeTail_].pin;
    out.broken = edges_[edgeTail_].broken;
    out.t_ms   = edges_[edgeTail_].t_ms;
    edgeTail_ = (uint8_t)((edgeTail_ + 1) & (EDGE_SLOTS - 1));
  }
  interrupts();
  return has;
}

// --- state ------------------------------------------------------------------

static Lane        lanes_[LANE_COUNT];
static Pending     queue_;
static Preferences store_;
static Due         heartbeat_;
static Due         flushRetry_;

static bool     queueDirty_    = false;
static uint32_t queueTouchedAt_ = 0;

// --- flash ------------------------------------------------------------------

static void loadQueue() {
  int8_t buf[Pending::CAPACITY];
  const size_t n = store_.getBytes("pending", buf, sizeof(buf));
  if (n > 0) {
    queue_.deserialize(buf, n);
    Serial.printf("[queue] restored %u crossings from flash\n", (unsigned)queue_.size());
  }
}

static void saveQueue() {
  int8_t buf[Pending::CAPACITY];
  const size_t n = queue_.serialize(buf, sizeof(buf));
  store_.putBytes("pending", buf, n);
  queueDirty_ = false;
}

// Flash wears out. A busy door would write on every person, so hold off until
// the queue has been still for a moment -- which in practice means "the rush
// is over" or "the network just came back".
static void maybeSaveQueue(uint32_t now_ms) {
  if (!queueDirty_) return;
  if (now_ms - queueTouchedAt_ < QUEUE_SAVE_IDLE_MS) return;
  saveQueue();
}

static void touchQueue(uint32_t now_ms) {
  queueDirty_     = true;
  queueTouchedAt_ = now_ms;
}

// --- pin plumbing -----------------------------------------------------------

static bool locateBeam(uint8_t pin, uint8_t& lane, bool& isA) {
  for (uint8_t i = 0; i < LANE_COUNT; i++) {
    if (LANE_PINS[i].a == pin) { lane = i; isA = true;  return true; }
    if (LANE_PINS[i].b == pin) { lane = i; isA = false; return true; }
  }
  return false;
}

static void recordCrossing(Cross c, uint32_t now_ms) {
  if (c == Cross::None) return;
  queue_.push((int8_t)c);
  touchQueue(now_ms);
  Serial.printf("[count] %s  queued=%u dropped=%lu\n",
                c == Cross::In ? "in" : "out",
                (unsigned)queue_.size(), (unsigned long)queue_.dropped());
}

// --- simulation -------------------------------------------------------------
#if SIMULATE
// Feeds the real lanes real edges, so everything downstream -- stuck detection,
// the queue, auth, the writes -- runs exactly as it will with sensors attached.
static uint32_t nextFakeAt_ = 0;

static void simulate(uint32_t now_ms) {
  if (!reached(now_ms, nextFakeAt_)) return;
  nextFakeAt_ = now_ms + (uint32_t)random(1500, 6000);

  const uint8_t lane = (uint8_t)random(0, LANE_COUNT);
  const bool    in   = random(0, 100) < 55;      // a gentle inbound bias
  const bool    first = in;                       // A first means inbound
  const uint32_t transit = (uint32_t)random(300, 900);

  lanes_[lane].feed(first,  true,  now_ms);
  lanes_[lane].feed(!first, true,  now_ms + transit * 4 / 10);
  lanes_[lane].feed(first,  false, now_ms + transit * 7 / 10);
  recordCrossing(lanes_[lane].feed(!first, false, now_ms + transit), now_ms);
}
#endif

// --- the loop ---------------------------------------------------------------

static void drainEdges() {
  Edge e;
  while (takeEdge(e)) {
    uint8_t lane; bool isA;
    if (!locateBeam(e.pin, lane, isA)) continue;
    recordCrossing(lanes_[lane].feed(isA, e.broken, e.t_ms), e.t_ms);
  }
}

static void flushQueue(uint32_t now_ms) {
  if (queue_.empty() || !Net::ready()) return;
  if (!flushRetry_.due(now_ms)) return;

  // One per pass. Sending the whole backlog in a single loop iteration would
  // block for as long as it takes, and a lane cannot notice a stuck beam while
  // the loop is not running.
  int8_t dir = 0;
  if (!queue_.peek(dir)) return;

  if (Net::sendCrossing((Cross)dir)) {
    queue_.pop();
    touchQueue(now_ms);
    if (!queue_.empty()) flushRetry_.armIn(now_ms, 50);   // keep draining, one per pass
  } else {
    flushRetry_.postpone(now_ms);
  }
}

static bool wasHealthy_[LANE_COUNT];

static void reportHealth() {
  for (uint8_t i = 0; i < LANE_COUNT; i++) {
    const bool healthy = lanes_[i].isHealthy();
    if (healthy == wasHealthy_[i]) continue;
    Serial.printf("[lane %u] %s\n", i, healthy ? "recovered" : "BLOCKED - not counting");
    wasHealthy_[i] = healthy;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\ncougar-count %s, %u lane(s)%s\n",
                DEVICE_NAME, LANE_COUNT, SIMULATE ? ", SIMULATED" : "");

  for (uint8_t i = 0; i < LANE_COUNT; i++)
    lanes_[i].begin(TRANSIT_MIN_MS, LINGER_MAX_MS, REFRACTORY_MS, STUCK_AFTER_MS);

  store_.begin("cougar", false);
  loadQueue();

#if !SIMULATE
  for (uint8_t i = 0; i < LANE_COUNT; i++) {
    const uint8_t pins[2] = { LANE_PINS[i].a, LANE_PINS[i].b };
    for (uint8_t k = 0; k < 2; k++) {
      pinMode(pins[k], INPUT_PULLUP);
      attachInterruptArg(digitalPinToInterrupt(pins[k]), onBeamEdge,
                         (void*)(uintptr_t)pins[k], CHANGE);
    }
  }
#endif

  Net::begin();

  const uint32_t now = millis();
  heartbeat_.begin(HEARTBEAT_MS, now);
  flushRetry_.begin(FLUSH_RETRY_MS, now);

  for (uint8_t i = 0; i < LANE_COUNT; i++) wasHealthy_[i] = true;

  // Say we are alive immediately rather than two minutes from now, so a board
  // that has just been power cycled does not look dead to the website.
  heartbeat_.armIn(now, 0);
}

void loop() {
  const uint32_t now = millis();

  Net::service(now);

#if SIMULATE
  simulate(now);
#else
  drainEdges();
#endif

  for (uint8_t i = 0; i < LANE_COUNT; i++) lanes_[i].tick(now);
  reportHealth();

  flushQueue(now);

  // A failed heartbeat retries in five seconds, not two minutes. The website
  // stops showing a number after six minutes of silence, so there is room for
  // exactly two failures before the site goes blank.
  if (heartbeat_.due(now) && !Net::sendHeartbeat()) heartbeat_.armIn(now, 5000);

  maybeSaveQueue(now);
}
