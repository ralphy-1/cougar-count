#pragma once
#include <stdint.h>

// Which door this build is for. Set by platformio.ini, not by editing a file
// before every upload -- that is how a board ends up flashed as the wrong door
// and the count runs backwards all week.
#if !defined(DEVICE_IS_ENTRANCE) && !defined(DEVICE_IS_EXIT)
#error "Build with -D DEVICE_IS_ENTRANCE or -D DEVICE_IS_EXIT (see platformio.ini)"
#endif

// --- pins -------------------------------------------------------------------
//
// ESP32-C3 pins that are NOT free, and why:
//   GPIO 2, 8, 9    strapping pins, read at boot -- a sensor holding one low
//                   puts the chip into download mode instead of running
//   GPIO 11-17      internal flash
//   GPIO 18, 19     USB D-/D+
//   GPIO 20, 21     UART0, which is the serial monitor
//
// Everything below comes from what is left.
//
// Beam A is ALWAYS the outside beam, at both doors. That single convention is
// why one firmware runs on both boards: A breaking before B means someone came
// in from the hallway, wherever the board is bolted.

struct BeamPins { uint8_t a; uint8_t b; };

#if defined(DEVICE_IS_ENTRANCE)
  #define DEVICE_NAME "entrance"
  static const uint8_t LANE_COUNT = 1;              // one 91 cm door
  static constexpr BeamPins LANE_PINS[LANE_COUNT] = {
    { 4, 5 },
  };
#else
  #define DEVICE_NAME "exit"
  static const uint8_t LANE_COUNT = 2;              // double doors, one lane per leaf
  static constexpr BeamPins LANE_PINS[LANE_COUNT] = {
    { 4, 5 },
    { 6, 7 },
  };
#endif

// Checked by the compiler rather than by whoever is holding the soldering iron.
// A beam wired to a strapping pin does not fail loudly -- the board just boots
// into download mode whenever that beam happens to be blocked at power-up, which
// looks like a dead device and nothing else.
constexpr bool pinIsUsable(uint8_t p) {
  return p <= 21
      && p != 2 && p != 8 && p != 9        // strapping
      && !(p >= 11 && p <= 17)             // internal flash
      && p != 18 && p != 19                // USB D-/D+
      && p != 20 && p != 21;               // UART0, the serial monitor
}

constexpr bool everyPinUsable() {
  for (uint8_t i = 0; i < LANE_COUNT; i++)
    if (!pinIsUsable(LANE_PINS[i].a) || !pinIsUsable(LANE_PINS[i].b)) return false;
  return true;
}

constexpr bool everyPinDistinct() {
  for (uint8_t i = 0; i < LANE_COUNT * 2; i++) {
    const uint8_t x = (i % 2 == 0) ? LANE_PINS[i / 2].a : LANE_PINS[i / 2].b;
    for (uint8_t j = i + 1; j < LANE_COUNT * 2; j++) {
      const uint8_t y = (j % 2 == 0) ? LANE_PINS[j / 2].a : LANE_PINS[j / 2].b;
      if (x == y) return false;
    }
  }
  return true;
}

static_assert(everyPinUsable(),
              "A beam is assigned to a pin that is strapping, flash, USB or UART on the ESP32-C3");
static_assert(everyPinDistinct(),
              "Two beams are assigned to the same pin");

// The sensors are NPN open-collector with a pull-up, so the pin sits HIGH when
// the beam is clear and is pulled LOW when something breaks it.
static const bool BEAM_BROKEN_LEVEL = false;

// --- timing -----------------------------------------------------------------
// These are the values the host tests run against. Changing one here without
// running ./run_tests.sh means the tested behaviour and the shipped behaviour
// are no longer the same thing.
static const uint32_t TRANSIT_MIN_MS = 120;      // faster is not a person
static const uint32_t LINGER_MAX_MS  = 3000;     // slower is someone standing there
static const uint32_t REFRACTORY_MS  = 250;      // deaf period after a count
static const uint32_t STUCK_AFTER_MS = 300000;   // a beam held this long is an object

// The page refuses to show a count older than six minutes, so this must stay
// comfortably under that. Three heartbeats fit in the window, which means a
// single dropped write on flaky wifi does not blank the site.
static const uint32_t HEARTBEAT_MS = 120000;

// --- behaviour --------------------------------------------------------------
// Retry cadence for the pending queue when the network is refusing.
static const uint32_t FLUSH_RETRY_MS = 5000;

// How long the queue must be unchanged before it is written to flash. Flash has
// a finite number of erase cycles and a busy door would otherwise write on
// every single person.
static const uint32_t QUEUE_SAVE_IDLE_MS = 10000;

// The exit board holds each crossing until the entrance board acknowledges it,
// and resends this often until it does. Short, because an unacknowledged
// crossing is a person who is not on the website yet.
static const uint32_t LINK_RETRY_MS = 400;

// Lane health goes across the radio on its own schedule too, so a stuck beam on
// the exit door is visible to the board doing the writing even during a quiet
// spell with no crossings to carry it.
static const uint32_t HEALTH_EVERY_MS = 30000;

// The exit board reports its health every HEALTH_EVERY_MS. Three missed reports
// and we stop vouching for it -- silence and a blocked beam look identical from
// the entrance board, and both mean people are being missed.
static const uint32_t PEER_SILENT_MS = HEALTH_EVERY_MS * 3;

// Generate synthetic crossings instead of reading pins. Lets the whole pipeline
// -- lanes, queue, auth, writes, heartbeat -- be exercised end to end before any
// sensor exists. Set to 0 for real hardware.
#define SIMULATE 1
