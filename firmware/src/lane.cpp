#include "lane.h"

// Unsigned subtraction, so it stays correct across the millis() wrap. Same
// reasoning as reached() in crossing_fsm.cpp.
static inline uint32_t since(uint32_t now_ms, uint32_t then_ms) {
  return now_ms - then_ms;
}

void Lane::begin(uint32_t transitMin, uint32_t lingerMax, uint32_t refractory,
                 uint32_t stuckAfter) {
  transitMin_ = transitMin;
  lingerMax_  = lingerMax;
  refractory_ = refractory;
  stuckAfter_ = stuckAfter;

  aBroken_ = bBroken_ = false;
  aSince_  = bSince_  = 0;
  healthy_ = true;
  fsm_.begin(transitMin_, lingerMax_, refractory_);
}

uint32_t Lane::blockedForMs(uint32_t now_ms) const {
  uint32_t worst = 0;
  if (aBroken_) worst = since(now_ms, aSince_);
  if (bBroken_) {
    const uint32_t b = since(now_ms, bSince_);
    if (b > worst) worst = b;
  }
  return worst;
}

// Losing trust is instant; regaining it is not. Only tick() can bring a lane
// back, never feed(). Recovering inside feed() would mean the same edge that
// releases the beam is also the edge that restores the lane, which leaves no
// moment where the lane is known-bad-but-clear -- and that is exactly the moment
// the suppression below has to cover.
void Lane::noteIfStuck(uint32_t now_ms) {
  if (healthy_ && blockedForMs(now_ms) > stuckAfter_) healthy_ = false;
}

void Lane::reviewHealth(uint32_t now_ms) {
  noteIfStuck(now_ms);
  if (healthy_) return;

  // Recovery requires BOTH beams clear, not merely the stuck one releasing.
  // Coming back while a body is still in the doorway would start timing a
  // crossing from the wrong beam, which invents a direction -- worse than
  // losing a count, because a wrong direction moves the number the wrong way.
  if (!aBroken_ && !bBroken_) {
    healthy_ = true;
    // Re-armed rather than resumed. Crossings that happened while the lane was
    // untrusted left the machine mid-cycle -- most likely sitting out a
    // refractory period for a count that was thrown away. Without this, the
    // first real person after recovery gets swallowed by a quiet period that
    // belongs to a crossing nobody counted.
    fsm_.begin(transitMin_, lingerMax_, refractory_);
  }
}

Cross Lane::feed(bool isA, bool broken, uint32_t t_ms) {
  // Bookkeeping first: only a clear->broken transition restarts the clock, so
  // a repeated "still broken" edge cannot keep resetting it and hide a beam
  // that has been blocked all afternoon.
  if (isA) {
    if (broken && !aBroken_) aSince_ = t_ms;
    aBroken_ = broken;
  } else {
    if (broken && !bBroken_) bSince_ = t_ms;
    bBroken_ = broken;
  }

  // Judge health before the FSM speaks, so a lane that has just gone bad cannot
  // slip one last count out on the same edge that condemned it. Note this can
  // only ever take health away -- see reviewHealth().
  noteIfStuck(t_ms);

  const Cross c = fsm_.feed(isA, broken, t_ms);
  return healthy_ ? c : Cross::None;
}

void Lane::tick(uint32_t now_ms) {
  reviewHealth(now_ms);
  fsm_.tick(now_ms);
}
