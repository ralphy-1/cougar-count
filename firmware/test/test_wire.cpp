// Host-side tests for the wire format and the send timers.
//
//   c++ -std=c++17 -o /tmp/test_wire firmware/test/test_wire.cpp \
//       firmware/src/wire.cpp -I firmware/src && /tmp/test_wire

#include "wire.h"
#include <cstdio>
#include <cstring>

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "  ok  " : "FAILED", what);
  if (!ok) failures++;
}

int main() {
  // --- payloads ------------------------------------------------------------
  {
    // If this ever becomes a plain number, the two-writer race is back and it
    // will only show up when the gym is busy.
    check(strcmp(INCREMENT_BODY, "{\".sv\":{\"increment\":1}}") == 0,
          "the counter write is a server-side increment, not a value");
    check(strstr(INCREMENT_BODY, "increment") != nullptr,
          "and says so on the wire");
    check(strcmp(HEARTBEAT_BODY, "{\".sv\":\"timestamp\"}") == 0,
          "the heartbeat is server time, not the board's clock");
  }

  // --- routing -------------------------------------------------------------
  {
    check(strcmp(counterPath(Cross::In),  "/gym/live/in_total.json")  == 0, "In goes to in_total");
    check(strcmp(counterPath(Cross::Out), "/gym/live/out_total.json") == 0, "Out goes to out_total");
    check(counterPath(Cross::None) == nullptr, "None has nowhere to go and must not be sent");
  }
  {
    // The security rules only permit +1 on these two paths. A typo here would
    // be rejected by the server rather than corrupting anything, but it would
    // fail silently forever, so pin the exact strings.
    check(strstr(counterPath(Cross::In), "/gym/live/") != nullptr, "In writes under gym/live");
    check(strcmp(HEARTBEAT_PATH, "/gym/live/updated_at.json") == 0, "heartbeat path is updated_at");
  }

  {
    check(strcmp(LANES_OK_PATH, "/gym/live/all_lanes_ok.json") == 0, "lane health path");
    check(strcmp(boolBody(true),  "true")  == 0, "true is JSON true, not a quoted string");
    check(strcmp(boolBody(false), "false") == 0, "and false is JSON false");
    // The rules validate isBoolean(). "true" in quotes would be rejected by the
    // server and the page would never hear that a door had gone blind.
    check(boolBody(true)[0] != '"', "not quoted, or the rules reject it");
  }

  // --- Due -----------------------------------------------------------------
  {
    Due d;
    d.begin(1000, 5000);
    check(!d.due(5999),  "not due before the period is up");
    check(d.due(6000),   "due exactly on time");
    check(!d.due(6001),  "and not again immediately after");
    check(d.due(7000),   "due again a period later");
  }
  {
    // A blocked loop must not produce a burst. If the board is stuck for ten
    // minutes, we want ONE heartbeat afterwards, not every one it missed.
    Due d;
    d.begin(1000, 0);
    check(d.due(600000), "a long stall fires once");
    check(!d.due(600001), "and does not queue up the missed ones");
    check(!d.due(600999), "still nothing");
    check(d.due(601000),  "back on the normal cadence");
  }
  {
    Due d;
    d.begin(1000, 0);
    check(d.due(1000),   "fires");
    d.postpone(1000);    // pretend the send failed
    check(!d.due(1999),  "postpone holds off a retry");
    check(d.due(2000),   "until a full period has passed");
  }
  {
    Due d;
    d.begin(60000, 1000);
    d.armIn(1000, 0);
    check(d.due(1000),   "armIn(0) fires immediately");
    d.armIn(1000, 50);
    check(!d.due(1049),  "armIn(50) waits");
    check(d.due(1050),   "and then fires");
    check(!d.due(1051),  "and afterwards is back on the full period");
    check(d.due(61050),  "which is a minute later");
  }
  {
    // Across the millis() wrap. A deadline just past the wrap is a tiny number
    // while now is still huge; a plain >= would call it due immediately and the
    // board would hammer the network once every 49.7 days.
    Due d;
    const uint32_t nearWrap = 0xFFFFFF00u;
    d.begin(1000, nearWrap);                 // next_ wraps to 0xFFFFFF00+1000
    check(!d.due(nearWrap + 500), "not due before the deadline, even across the wrap");
    check(d.due(nearWrap + 1000), "due on time on the far side of the wrap");
  }

  printf("\n%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
  return failures != 0;
}
