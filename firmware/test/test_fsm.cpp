// Host-side tests for CrossingFSM. No hardware, no PlatformIO -- these are pure
// logic and they compile with any C++ compiler:
//
//   c++ -std=c++17 -o /tmp/test_fsm firmware/test/test_fsm.cpp \
//       firmware/src/crossing_fsm.cpp -I firmware/src && /tmp/test_fsm
//
// Every test is a sequence of beam edges with timestamps, which is exactly what
// the sensors produce. Time is a number we make up, so a crossing that takes
// four seconds runs instantly.

#include "crossing_fsm.h"
#include <cstdio>

static int failures = 0;

static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "  ok  " : "FAILED", what);
  if (!ok) failures++;
}

// Like check(), but silent when it passes. For invariants asserted inside
// helpers, where a line of output per beam edge would bury the real test names.
static void quietCheck(bool ok, const char* what) {
  if (!ok) check(false, what);
}

static const bool A = true, B = false;

// One person walking through: first beam breaks, second breaks, first clears,
// second clears. `transit` is the whole thing end to end.
//
// Only the LAST edge may return a count. The FSM decides when both beams are
// clear, so a crossing returned by an earlier edge is a bug even when the final
// value happens to be right -- keep the intermediate values instead of dropping
// them on the floor.
static Cross walkThrough(CrossingFSM& f, bool firstBeam, uint32_t t0, uint32_t transit) {
  const bool secondBeam = !firstBeam;
  const Cross mid[] = {                      // braced init: evaluated in order
    f.feed(firstBeam,  true,  t0),
    f.feed(secondBeam, true,  t0 + transit * 4 / 10),
    f.feed(firstBeam,  false, t0 + transit * 7 / 10),
  };
  for (Cross c : mid)
    quietCheck(c == Cross::None, "a count arrived before both beams were clear");
  return f.feed(secondBeam, false, t0 + transit);
}

static CrossingFSM fresh() {
  CrossingFSM f;
  f.begin(120, 3000, 250);   // transitMin, lingerMax, refractory
  return f;
}

int main() {
  // --- direction ---------------------------------------------------------
  {
    CrossingFSM f = fresh();
    check(walkThrough(f, A, 1000, 450) == Cross::In, "A then B counts as In");
  }
  {
    CrossingFSM f = fresh();
    check(walkThrough(f, B, 1000, 450) == Cross::Out, "B then A counts as Out");
  }

  // --- things that are not people ----------------------------------------
  {
    CrossingFSM f = fresh();
    check(f.feed(A, true, 1000) == Cross::None, "one beam broken is not yet a crossing");
    check(!f.isIdle(),                          "and the FSM is waiting, not idle");
  }
  {
    // An arm reaching through the outer beam and pulling back. B never breaks.
    CrossingFSM f = fresh();
    f.feed(A, true, 1000);
    check(f.feed(A, false, 1300) == Cross::None, "one beam alone never counts");
  }
  {
    CrossingFSM f = fresh();
    check(walkThrough(f, A, 1000, 80) == Cross::None, "too fast to be a person");
  }
  {
    CrossingFSM f = fresh();
    check(walkThrough(f, A, 1000, 4000) == Cross::None, "too slow to be a crossing");
  }

  // --- giving up on a stalled crossing -----------------------------------
  {
    // Someone steps into the doorway and stands there.
    //
    // Asserting only that we end up idle does not test tick() at all: the beam
    // finally clearing runs the same reset, so the whole test passes with the
    // abandon logic deleted. What tick() actually buys is WHEN the refractory
    // period starts. Abandon it at 4100 and the quiet period is long over by
    // the time the beam clears; leave it pending and the clear itself starts a
    // fresh 250ms of deafness for a crossing that was never counted.
    CrossingFSM f = fresh();
    f.feed(A, true, 1000);
    f.tick(4100);                       // past lingerMax with the beam still broken
    check(f.feed(A, false, 20000) == Cross::None, "a stall is never counted");
    f.tick(20000);                      // they finally move, much later
    check(f.isIdle(), "tick abandons a stall, so the doorway is live the moment it clears");
  }

  // --- the refractory tests: these are the ones that catch the real bug ---
  {
    // Two people, one after the other, with NO tick() in between. If the
    // refractory check runs after the beam state is stored, the second person's
    // first beam break is what keeps us deaf to the second person, and this
    // returns None.
    CrossingFSM f = fresh();
    check(walkThrough(f, A, 1000, 450) == Cross::In, "first of two people");
    check(walkThrough(f, A, 2000, 450) == Cross::In, "second of two people");
  }
  {
    CrossingFSM f = fresh();
    walkThrough(f, A, 1000, 450);                        // ends at t=1450
    check(walkThrough(f, A, 1750, 450) == Cross::In,     "a crossing 300ms after the last one");
  }
  {
    // Deliberate: someone entering inside the 250ms deaf window is dropped.
    // Documented so that if this test ever fails, it is a decision being
    // changed and not an accident.
    CrossingFSM f = fresh();
    walkThrough(f, A, 1000, 450);                        // ends at t=1450
    check(walkThrough(f, A, 1500, 300) == Cross::None,   "someone inside the refractory window is lost");
  }

  {
    // The other half of the refractory rule, and the one the comment block in
    // crossing_fsm.cpp is really about: leaving Refractory requires both beams
    // CLEAR, not just the clock expiring. Drop that condition and the FSM can
    // wake up in the middle of a body and start timing a crossing from the
    // wrong beam -- which is worse than losing a count, because it invents a
    // direction. Here two people walk IN, overlapping, starting inside the
    // deaf window; both are dropped, but nothing may ever come back as Out.
    CrossingFSM f = fresh();
    check(walkThrough(f, A, 1000, 450) == Cross::In, "a counted crossing to open the window");

    const Cross edges[] = {                  // refractory runs to t=1700
      f.feed(A, true,  1600),                // second person breaks A (deaf)
      f.feed(B, true,  1750),                // ...and B, now past the clock
      f.feed(A, false, 1850),
      f.feed(A, true,  1900),                // third person is right behind
      f.feed(B, false, 1950),
      f.feed(B, true,  2100),
      f.feed(A, false, 2200),
      f.feed(B, false, 2300),
    };
    bool quiet = true;
    for (Cross c : edges) quiet = quiet && (c == Cross::None);
    check(quiet, "a body still in the beams cannot end the refractory period");
  }
  {
    // begin() re-arms a live machine; the firmware re-runs it when thresholds
    // change, and it must not inherit half a crossing from the old settings.
    CrossingFSM f = fresh();
    f.feed(A, true, 1000);                               // mid-crossing
    f.begin(120, 3000, 250);
    check(f.isIdle(), "begin() clears a half-finished crossing");
    check(walkThrough(f, A, 2000, 450) == Cross::In, "and counts normally afterwards");
  }

  // --- millis() wraps every ~49.7 days and these boards run for months -----
  {
    // The crossing itself is measured with unsigned subtraction, so it already
    // survives the wrap. Worth pinning so it stays that way.
    CrossingFSM f = fresh();
    check(walkThrough(f, A, 0xFFFFFF00u, 400) == Cross::In, "a crossing that straddles the wrap");
  }
  {
    // The refractory deadline is an absolute timestamp, so it wraps to a small
    // number while `now` is still huge. A plain `now >= deadline` reads that as
    // "long past" and opens the door immediately -- double-counting one person
    // as two, once every 49.7 days, which is not a bug anyone would find by
    // staring at it.
    CrossingFSM f = fresh();
    check(walkThrough(f, A, 0xFFFFFFF0u - 450u, 450) == Cross::In, "a crossing just before the wrap");
    check(walkThrough(f, A, 0xFFFFFFF5u, 300) == Cross::None,      "the refractory survives the wrap");
  }

  printf("\n%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
  return failures != 0;
}
