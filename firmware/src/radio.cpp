#include "radio.h"
#include "config.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "Copy firmware/src/secrets.h.example to firmware/src/secrets.h and fill it in."
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

namespace {

const uint8_t PEER_MAC[6] = ESPNOW_PEER_MAC;

uint16_t bootId_ = 0;

// Set from the receive callback, read from the loop.
volatile bool     havePending_   = false;
volatile int8_t   pendingDir_    = 0;
volatile uint16_t lastAckSeq_    = 0;
volatile bool     haveAck_       = false;
volatile uint8_t  peerLanesOk_   = 0;
volatile uint8_t  peerLaneCount_ = 0;
volatile bool     peerHeard_     = false;
volatile uint32_t peerHeardAt_   = 0;

Dedup dedup_;

bool sendFrame(const LinkFrame& f) {
  uint8_t buf[LINK_FRAME_BYTES];
  const size_t n = linkEncode(f, buf, sizeof(buf));
  if (n == 0) return false;
  return esp_now_send(PEER_MAC, buf, n) == ESP_OK;
}

void sendAck(uint16_t seq) {
  LinkFrame ack{};
  ack.kind   = LINK_ACK;
  ack.seq    = seq;
  ack.bootId = bootId_;
  sendFrame(ack);
}

// The callback signature changed between Arduino-ESP32 2.x and 3.x. Getting it
// wrong does not fail at link time -- register_recv_cb takes a function pointer
// and the mismatch shows up as a board that simply never receives anything.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  #define RECV_ARGS const esp_now_recv_info_t* info, const uint8_t* data, int len
#else
  #define RECV_ARGS const uint8_t* info, const uint8_t* data, int len
#endif

void onReceive(RECV_ARGS) {
  (void)info;
  LinkFrame f{};
  if (len < 0 || !linkDecode(data, (size_t)len, f)) return;   // noise, or not ours

  if (f.kind == LINK_ACK) {
    lastAckSeq_ = f.seq;
    haveAck_    = true;
    return;
  }

  peerLanesOk_   = f.lanesOk;
  peerLaneCount_ = f.laneCount;
  peerHeard_     = true;
  peerHeardAt_   = millis();

  if (f.kind == LINK_HEALTH) return;
  if (f.kind != LINK_CROSSING) return;

  // Acknowledge first and unconditionally. A duplicate that goes unacknowledged
  // is retried forever, and the retry is indistinguishable from the original.
  sendAck(f.seq);

  if (!dedup_.accept(f.bootId, f.seq)) return;   // already counted
  if (havePending_) return;                      // loop is behind; the sender will retry

  pendingDir_  = f.dir;
  havePending_ = true;
}

}  // namespace

namespace Radio {

// ESP-NOW only reaches a board sitting on the same wifi channel, and the
// entrance board's channel is whatever the campus access point decided. The
// entrance board gets it for free by being connected. The exit board is not
// connected to anything -- that is the entire point of it -- so it has to go
// and find out.
//
// Known fragility: if the access point moves channel, the link goes quiet until
// the exit board is restarted. Worth revisiting once we know how the FLC's
// wifi actually behaves; a periodic rescan is the obvious fix but it costs two
// seconds of deafness each time.
static void matchPeerChannel() {
#if defined(DEVICE_IS_EXIT)
  const int found = WiFi.scanNetworks(false, false);
  for (int i = 0; i < found; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) {
      const uint8_t ch = (uint8_t)WiFi.channel(i);
      esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
      Serial.printf("[radio] locked to channel %u to match %s\n", ch, WIFI_SSID);
      WiFi.scanDelete();
      return;
    }
  }
  WiFi.scanDelete();
  Serial.println("[radio] WARNING: could not find the network to match its channel");
#endif
}

void begin(uint16_t bootId) {
  bootId_ = bootId;

  WiFi.mode(WIFI_STA);
  matchPeerChannel();

  if (esp_now_init() != ESP_OK) {
    Serial.println("[radio] esp_now_init failed");
    return;
  }

  esp_now_set_pmk((const uint8_t*)ESPNOW_PMK);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, PEER_MAC, 6);
  memcpy(peer.lmk, ESPNOW_LMK, 16);
  peer.channel = 0;                  // whatever channel the station is on
  peer.encrypt = true;
  peer.ifidx   = WIFI_IF_STA;

  if (esp_now_add_peer(&peer) != ESP_OK)
    Serial.println("[radio] add_peer failed");

  esp_now_register_recv_cb(onReceive);
}

bool sendCrossing(int8_t dir, uint16_t seq, uint8_t lanesOk, uint8_t laneCount) {
  LinkFrame f{};
  f.kind      = LINK_CROSSING;
  f.dir       = dir;
  f.bootId    = bootId_;
  f.seq       = seq;
  f.lanesOk   = lanesOk;
  f.laneCount = laneCount;
  return sendFrame(f);
}

bool sendHealth(uint8_t lanesOk, uint8_t laneCount) {
  LinkFrame f{};
  f.kind      = LINK_HEALTH;
  f.bootId    = bootId_;
  f.lanesOk   = lanesOk;
  f.laneCount = laneCount;
  return sendFrame(f);
}

bool ackSeen(uint16_t seq) {
  return haveAck_ && lastAckSeq_ == seq;
}

bool takeCrossing(int8_t& dir) {
  if (!havePending_) return false;
  dir = pendingDir_;
  havePending_ = false;
  return true;
}

bool peerAllLanesOk(uint32_t now_ms, uint32_t staleAfterMs) {
  if (!peerHeard_) return false;                       // never heard from at all
  if (now_ms - peerHeardAt_ > staleAfterMs) return false;   // gone quiet
  if (peerLaneCount_ == 0 || peerLaneCount_ > 8) return false;

  const uint8_t all = (uint8_t)((1u << peerLaneCount_) - 1u);
  return (peerLanesOk_ & all) == all;
}

}  // namespace Radio
