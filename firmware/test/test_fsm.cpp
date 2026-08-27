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

static const bool A = true, B = false;

// One person walking through: first beam breaks, second breaks, first clears,
// second clears. `transit` is the whole thing end to end.
static Cross walkThrough(CrossingFSM& f, bool firstBeam, uint32_t t0, uint32_t transit) {
  const bool secondBeam = !firstBeam;
  f.feed(firstBeam,  true,  t0);
  f.feed(secondBeam, true,  t0 + transit * 4 / 10);
  f.feed(firstBeam,  false, t0 + transit * 7 / 10);
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
    CrossingFSM f = fresh();
    f.feed(A, true, 1000);
    f.tick(4100);                       // past lingerMax with the beam still broken
    f.feed(A, false, 4200);             // they finally move
    f.tick(4600);
    check(f.isIdle(), "tick abandons a crossing that stalled");
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

  printf("\n%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
  return failures != 0;
}
