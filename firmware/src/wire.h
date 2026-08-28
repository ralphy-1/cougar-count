#pragma once
#include <stdint.h>
#include "crossing_fsm.h"

// The exact bytes that go to Firebase, and the timers that decide when.
//
// Kept apart from net.cpp so the two things most likely to be wrong -- the
// payloads and the scheduling -- can be tested on a laptop. Nothing here knows
// what wifi is.

// Which counter a crossing belongs to. Cross::None has no path; the caller must
// not send anything.
const char* counterPath(Cross c);

// Atomic server-side increment. This is NOT a read-modify-write, and that is
// the whole point: reading in_total and writing value+1 reintroduces the race
// that split counters exist to prevent -- the entrance and the exit landing in
// the same millisecond and cancelling a person out. Firebase resolves this
// sentinel server-side, so two boards can never lose a count to each other.
extern const char* const INCREMENT_BODY;

// Server time, not the board's. The security rules require updated_at to land
// within five minutes of server time, which this satisfies by definition -- and
// an ESP32's own clock is wrong until NTP answers, which may be never if the
// network is down.
extern const char* const HEARTBEAT_BODY;
extern const char* const HEARTBEAT_PATH;

// A periodic timer that survives the millis() wrap.
class Due {
public:
  void begin(uint32_t periodMs, uint32_t now_ms);

  // True at most once per period, and arms the next one from NOW rather than
  // from the last deadline. If the loop is blocked for ten minutes we want one
  // heartbeat afterwards, not five hundred queued up.
  bool due(uint32_t now_ms);

  // Push the next firing out by a full period. Used after a failed send, so a
  // dead network is retried at the normal rate instead of in a tight loop.
  void postpone(uint32_t now_ms);

  // Fire again in exactly this long. Callers wanted "now" and "in 50ms" and
  // were getting them by passing a doctored timestamp to postpone(), which read
  // like a bug even when it worked.
  void armIn(uint32_t now_ms, uint32_t delayMs);

private:
  uint32_t period_ = 0;
  uint32_t next_   = 0;
};
