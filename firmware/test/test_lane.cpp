// Host-side tests for Lane and Pending. No hardware, no PlatformIO:
//
//   c++ -std=c++17 -o /tmp/test_lane firmware/test/test_lane.cpp \
//       firmware/src/lane.cpp firmware/src/pending.cpp \
//       firmware/src/crossing_fsm.cpp -I firmware/src && /tmp/test_lane
//
// A separate binary from test_fsm.cpp on purpose: that file is being edited by
// more than one person, and a shared harness would mean a merge conflict every
// time either side adds a test.

#include "lane.h"
#include "pending.h"
#include <cstdio>
#include <cstring>

static int failures = 0;

static void check(bool ok, const char* what) {
  printf("%s  %s\n", ok ? "  ok  " : "FAILED", what);
  if (!ok) failures++;
}

static const bool A = true, B = false;
static const uint32_t STUCK = 300000;      // five minutes

static Lane fresh() {
  Lane l;
  l.begin(120, 3000, 250, STUCK);
  return l;
}

// One person through the lane; only the last edge may return a count.
static Cross walkThrough(Lane& l, bool firstBeam, uint32_t t0, uint32_t transit) {
  const bool secondBeam = !firstBeam;
  l.feed(firstBeam,  true,  t0);
  l.feed(secondBeam, true,  t0 + transit * 4 / 10);
  l.feed(firstBeam,  false, t0 + transit * 7 / 10);
  return l.feed(secondBeam, false, t0 + transit);
}

int main() {
  // --- a healthy lane behaves exactly like the FSM underneath --------------
  {
    Lane l = fresh();
    check(l.isHealthy(),                              "a new lane is healthy");
    check(walkThrough(l, A, 1000, 450) == Cross::In,  "and counts a crossing");
    check(walkThrough(l, B, 2000, 450) == Cross::Out, "in both directions");
  }

  // --- something parked in a beam -----------------------------------------
  {
    // A bin against the wall, a propped door, a floor mat. The beam reads
    // blocked and never changes again, so no edge ever arrives -- tick() is the
    // only thing that can notice.
    Lane l = fresh();
    l.feed(A, true, 1000);
    l.tick(1000 + STUCK);
    check(l.isHealthy(), "a beam broken for exactly the threshold is not yet stuck");
    l.tick(1000 + STUCK + 1);
    check(!l.isHealthy(), "a beam broken past the threshold marks the lane unhealthy");
  }
  {
    Lane l = fresh();
    l.feed(A, true, 1000);
    l.tick(1000 + STUCK + 1);
    check(l.blockedForMs(1000 + STUCK + 1) == STUCK + 1, "and it reports how long");
  }

  // --- an unhealthy lane must not contribute ------------------------------
  {
    // The important one. A lane with a stuck beam still sees the OTHER beam
    // being crossed all day. Those are real people, but their direction cannot
    // be trusted with half the evidence missing, so they must not be counted.
    Lane l = fresh();
    l.feed(A, true, 1000);
    l.tick(1000 + STUCK + 1);
    check(!l.isHealthy(), "lane is unhealthy");

    bool quiet = true;
    for (int i = 0; i < 5; i++) {
      const uint32_t t = 400000 + i * 1000;
      quiet = quiet && (l.feed(B, true,  t)       == Cross::None);
      quiet = quiet && (l.feed(B, false, t + 400) == Cross::None);
    }
    check(quiet, "an unhealthy lane never emits a crossing");
  }

  // --- suppression is load bearing, not decoration -------------------------
  {
    // The window this covers: the beam has cleared, so a whole crossing can now
    // complete underneath, but the lane has not been ticked back to healthy yet.
    // The FSM genuinely returns In here. The lane must swallow it, because the
    // counts either side of a five minute blockage cannot be trusted.
    Lane l = fresh();
    l.feed(A, true, 1000);
    l.tick(1000 + STUCK + 1);
    check(!l.isHealthy(),                              "unhealthy after a stuck beam");

    l.feed(A, false, 400000);                          // object removed, no tick yet
    check(!l.isHealthy(),                              "still untrusted until ticked");
    check(walkThrough(l, A, 400100, 450) == Cross::None,
          "a real crossing through an untrusted lane is suppressed");
  }

  // --- recovery -----------------------------------------------------------
  {
    Lane l = fresh();
    l.feed(A, true, 1000);
    l.tick(1000 + STUCK + 1);
    l.feed(A, false, 400000);                          // the object is moved
    l.tick(400010);
    check(l.isHealthy(),                               "a tick with both beams clear restores the lane");
    check(walkThrough(l, A, 500000, 450) == Cross::In, "and it counts again");
  }
  {
    // Recovery needs BOTH beams clear. Waking up with a body still in the
    // doorway would start timing from the wrong beam and invent a direction --
    // worse than losing a count, because it moves the number the wrong way.
    Lane l = fresh();
    l.feed(A, true, 1000);
    l.feed(B, true, 2000);
    l.tick(2000 + STUCK + 1);
    check(!l.isHealthy(),          "both beams stuck is unhealthy");

    l.feed(A, false, 400000);
    l.tick(400010);
    check(!l.isHealthy(),          "one beam clearing is not enough to recover");
    l.feed(B, false, 400100);
    l.tick(400110);
    check(l.isHealthy(),           "the second one is");
  }
  {
    // The re-arm. A crossing completed while the lane was untrusted left the
    // machine sitting out a refractory period for a count that was thrown away.
    // If recovery resumed that machine instead of re-arming it, the first real
    // person after recovery would be swallowed by a quiet period belonging to
    // somebody who was never counted.
    Lane l = fresh();
    l.feed(A, true, 1000);
    l.tick(1000 + STUCK + 1);
    l.feed(A, false, 400000);
    walkThrough(l, A, 400100, 450);                    // suppressed; ends at 400550
    l.tick(400560);
    check(l.isHealthy(),                               "recovered");
    check(walkThrough(l, A, 400600, 450) == Cross::In,
          "and the very next person counts, not swallowed by a stale refractory");
  }

  // --- a beam that flickers is not a stuck beam ---------------------------
  {
    // Ordinary traffic re-breaks a beam constantly. Only a CLEAR->BROKEN
    // transition restarts the clock, so this must never trip.
    Lane l = fresh();
    for (int i = 0; i < 200; i++) {
      const uint32_t t = 1000 + i * 2000;     // a crossing every two seconds
      walkThrough(l, A, t, 450);
    }
    check(l.isHealthy(), "a busy lane is never mistaken for a stuck one");
  }
  {
    // The other half of that rule. A noisy sensor can re-assert "still broken"
    // without ever clearing. If any broken edge restarted the clock, a beam
    // blocked all afternoon would look brand new every time it twitched, and
    // the lane would never be marked stuck at all.
    Lane l = fresh();
    l.feed(A, true, 1000);
    for (int i = 1; i <= 20; i++) l.feed(A, true, 1000 + i * 30000);   // ten minutes
    l.tick(1000 + 20 * 30000);
    check(!l.isHealthy(), "a beam repeating 'still broken' is still stuck");
  }

  // --- Pending ------------------------------------------------------------
  {
    Pending q;
    q.clear();
    check(q.empty(),                "a new queue is empty");

    int8_t d = 0;
    check(q.peek(d) == false,       "peeking an empty queue reports nothing");

    q.push(1); q.push(-1); q.push(1);
    check(q.size() == 3,            "three crossings queued");
    check(q.peek(d) && d == 1,      "oldest first");
    q.pop();
    check(q.peek(d) && d == -1,     "then the next");
    q.pop(); q.pop();
    check(q.empty(),                "and then it is empty again");
  }
  {
    // Overflow drops the oldest. A crossing from twenty minutes ago matters
    // less than the one that just happened.
    Pending q;
    q.clear();
    for (int i = 0; i < Pending::CAPACITY; i++) q.push(1);
    check(q.full(),                 "the queue fills");
    check(q.dropped() == 0,         "with nothing lost yet");

    q.push(-1);
    check(q.size() == Pending::CAPACITY, "and stays at capacity");
    check(q.dropped() == 1,              "reporting the loss");
  }
  {
    // Which END gets dropped, told apart properly. Filling with identical
    // values cannot distinguish dropping the oldest from refusing the newest --
    // both leave a full queue and a dropped count of one.
    Pending q;
    q.clear();
    for (int i = 0; i < Pending::CAPACITY - 1; i++) q.push(1);
    q.push(-1);                     // a marker at the newest end
    check(q.full(),                 "queue full with a marker at the tail");

    q.push(1);                      // one more forces a drop
    check(q.dropped() == 1,         "one crossing lost");

    int8_t d = 0, last = 0;
    uint16_t n = 0;
    while (q.peek(d)) { last = d; q.pop(); n++; }
    check(n == Pending::CAPACITY,   "still holding a full queue afterwards");
    check(last == 1,                "the newest push survived, so the OLDEST was dropped");
  }
  {
    // dropped() is a record of counts that are gone for good. Reloading the
    // queue from flash does not un-lose them, so clear() must not wipe it --
    // otherwise a reboot would quietly erase the evidence that the day's total
    // is short.
    Pending q;
    q.clear();
    for (int i = 0; i < Pending::CAPACITY + 5; i++) q.push(1);
    check(q.dropped() == 5,         "five crossings lost to overflow");
    q.clear();
    check(q.empty(),                "clear empties the queue");
    check(q.dropped() == 5,         "but does not erase the record of what was lost");
  }
  {
    // Survives a power cut: the caller hands the bytes to flash and back.
    Pending q;
    q.clear();
    q.push(1); q.push(-1); q.push(-1); q.push(1);

    int8_t buf[Pending::CAPACITY];
    const size_t n = q.serialize(buf, sizeof(buf));
    check(n == 4,                   "four crossings serialize to four bytes");

    Pending restored;
    restored.deserialize(buf, n);
    check(restored.size() == 4,     "and come back");

    const int8_t expect[] = {1, -1, -1, 1};
    bool same = true;
    for (int i = 0; i < 4; i++) {
      int8_t d = 0;
      same = same && restored.peek(d) && d == expect[i];
      restored.pop();
    }
    check(same,                     "in the same order they went in");
  }
  {
    // Serialize after the ring has moved on. A fresh queue has its oldest entry
    // at array index zero, so reading the array straight through happens to
    // give the right answer -- meaning the round-trip test above proves nothing
    // about a queue that has been running since six this morning.
    Pending q;
    q.clear();
    for (int i = 0; i < 10; i++) q.push(1);
    for (int i = 0; i < 7; i++) q.pop();       // oldest entry is now well inside
    q.push(-1);
    q.push(-1);

    int8_t buf[Pending::CAPACITY];
    const size_t n = q.serialize(buf, sizeof(buf));
    check(n == 5,                   "five entries left after seven were sent");

    const int8_t expect[] = {1, 1, 1, -1, -1};
    bool same = true;
    for (size_t i = 0; i < n; i++) same = same && buf[i] == expect[i];
    check(same,                     "serialize starts at the oldest entry, not the start of the array");
  }
  {
    // Handed more than fits, keep the newest -- same rule as push().
    Pending q;
    int8_t buf[Pending::CAPACITY + 10];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (i < 10) ? -1 : 1;
    q.deserialize(buf, sizeof(buf));
    check(q.size() == Pending::CAPACITY, "an oversized restore is truncated");

    int8_t d = 0;
    check(q.peek(d) && d == 1,           "keeping the newest end of it");
  }

  printf("\n%s\n", failures ? "SOME TESTS FAILED" : "all tests passed");
  return failures != 0;
}
