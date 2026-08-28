#include <Arduino.h>
#include <Preferences.h>
// esp_random() moved headers between ESP-IDF 4 and 5, which Arduino core 2.x
// and 3.x follow respectively.
#if __has_include(<esp_random.h>)
  #include <esp_random.h>
#else
  #include <esp_system.h>
#endif

#include "config.h"
#include "lane.h"
#include "pending.h"
#include "wire.h"
#include "wrap.h"      // reached()
#include "net.h"
#include "radio.h"

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

static uint16_t bootId_ = 0;
static Due      healthBeat_;

#if defined(DEVICE_IS_EXIT)
// Stop and wait. One crossing is in the air at a time, held in the queue until
// the entrance board says it has taken it. Slower than streaming, and the only
// arrangement where a lost frame costs nothing.
static uint16_t nextSeq_     = 1;
static uint16_t inFlightSeq_ = 0;
static bool     awaitingAck_ = false;
#endif

static uint8_t laneMask() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < LANE_COUNT; i++)
    if (lanes_[i].isHealthy()) mask |= (uint8_t)(1u << i);
  return mask;
}

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

#if defined(DEVICE_IS_ENTRANCE)

// This board is the only one on the campus network, so everything ends up here:
// its own crossings and the exit board's.
static void collectFromPeer(uint32_t now_ms) {
  int8_t dir = 0;
  while (Radio::takeCrossing(dir)) {
    queue_.push(dir);
    touchQueue(now_ms);
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
    if (!queue_.empty()) flushRetry_.armIn(now_ms, 50);   // keep draining
  } else {
    flushRetry_.postpone(now_ms);
  }
}

#else   // DEVICE_IS_EXIT

// Nothing here reaches Firebase. The exit board's whole job is to get crossings
// across the room to the entrance board, which is why only one MAC address had
// to be registered with IT.
static void flushQueue(uint32_t now_ms) {
  if (awaitingAck_ && Radio::ackSeen(inFlightSeq_)) {
    queue_.pop();
    awaitingAck_ = false;
    touchQueue(now_ms);
  }

  if (queue_.empty()) return;
  if (!flushRetry_.due(now_ms)) return;

  int8_t dir = 0;
  if (!queue_.peek(dir)) return;

  // The sequence number is chosen once and reused for every retry of the same
  // crossing. That is what lets the far end tell a retry from a second person.
  if (!awaitingAck_) {
    inFlightSeq_ = nextSeq_++;
    awaitingAck_ = true;
  }

  Radio::sendCrossing(dir, inFlightSeq_, laneMask(), LANE_COUNT);
  flushRetry_.armIn(now_ms, LINK_RETRY_MS);
}

#endif

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

  // A fresh boot id every restart, so the far board can tell that our sequence
  // numbers have started over rather than jumped backwards.
  bootId_ = (uint16_t)esp_random();
  Radio::begin(bootId_);

#if defined(DEVICE_IS_ENTRANCE)
  Net::begin();
#endif

  const uint32_t now = millis();
  heartbeat_.begin(HEARTBEAT_MS, now);
  healthBeat_.begin(HEALTH_EVERY_MS, now);
  flushRetry_.begin(FLUSH_RETRY_MS, now);

  for (uint8_t i = 0; i < LANE_COUNT; i++) wasHealthy_[i] = true;

  // Say we are alive immediately rather than two minutes from now, so a board
  // that has just been power cycled does not look dead to the website.
  heartbeat_.armIn(now, 0);
}

void loop() {
  const uint32_t now = millis();

#if defined(DEVICE_IS_ENTRANCE)
  Net::service(now);
  collectFromPeer(now);
#endif

#if SIMULATE
  simulate(now);
#else
  drainEdges();
#endif

  for (uint8_t i = 0; i < LANE_COUNT; i++) lanes_[i].tick(now);
  reportHealth();

  flushQueue(now);

#if defined(DEVICE_IS_ENTRANCE)
  // A failed heartbeat retries in five seconds, not two minutes. The website
  // stops showing a number after six minutes of silence, so there is room for
  // exactly two failures before the site goes blank.
  if (heartbeat_.due(now) && !Net::sendHeartbeat()) heartbeat_.armIn(now, 5000);
#else
  // Say how the lanes are doing even when nobody is walking through. A quiet
  // afternoon must not look the same as a door that stopped reporting.
  if (healthBeat_.due(now)) Radio::sendHealth(laneMask(), LANE_COUNT);
#endif

  maybeSaveQueue(now);
}
