#include "crossing_fsm.h"

void CrossingFSM::begin(uint32_t transitMin, uint32_t lingerMax, uint32_t refractory) {
  transitMin_ = transitMin;
  lingerMax_  = lingerMax;
  refractory_ = refractory;
  st_ = St::Idle;
  aBroken_ = bBroken_ = secondSeen_ = false;
}

void CrossingFSM::reset(uint32_t now_ms) {
  // Beam states are NOT cleared here -- they describe the physical world, and
  // the world does not change just because we finished a decision.
  secondSeen_     = false;
  tFirstBreak_    = 0;
  tRefractoryEnd_ = now_ms + refractory_;
  st_             = St::Refractory;
}

Cross CrossingFSM::feed(bool isA, bool broken, uint32_t t_ms) {
  // Leave refractory based on the beam states as they were BEFORE this edge,
  // then let the edge be handled normally below. See the note at the bottom of
  // this file -- getting this order wrong is not a small bug.
  if (st_ == St::Refractory && t_ms >= tRefractoryEnd_ && !aBroken_ && !bBroken_)
    st_ = St::Idle;

  if (isA) aBroken_ = broken; else bBroken_ = broken;

  switch (st_) {
    case St::Refractory:
      break;                                  // still cooling down, ignore

    case St::Idle:
      if (broken) {                           // first beam of a new crossing
        tFirstBreak_ = t_ms;
        secondSeen_  = false;
        st_ = isA ? St::PendingIn : St::PendingOut;
      }
      break;

    case St::PendingIn:
    case St::PendingOut: {
      // Which beam are we still hoping to see break?
      const bool waitingOnA = (st_ == St::PendingOut);
      if (broken && isA == waitingOnA) secondSeen_ = true;

      if (!aBroken_ && !bBroken_) {           // fully clear -- decide now
        const uint32_t transit = t_ms - tFirstBreak_;
        Cross out = Cross::None;
        if (secondSeen_ && transit >= transitMin_ && transit <= lingerMax_) {
          lastTransit_ = transit;
          out = (st_ == St::PendingIn) ? Cross::In : Cross::Out;
        }
        reset(t_ms);
        return out;
      }
      break;
    }
  }
  return Cross::None;
}

void CrossingFSM::tick(uint32_t now_ms) {
  switch (st_) {
    case St::PendingIn:
    case St::PendingOut:
      // Someone stepped in and stopped, or a beam is stuck. Give up on it.
      if (now_ms - tFirstBreak_ > lingerMax_) reset(now_ms);
      break;
    case St::Refractory:
      if (now_ms >= tRefractoryEnd_ && !aBroken_ && !bBroken_) st_ = St::Idle;
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Why the refractory check sits ABOVE the beam-state assignment in feed():
//
// The obvious way to write it is to record the new beam state first, then ask
// whether we can leave refractory. That version is broken in a way that reading
// it will not reveal.
//
// After a crossing we sit in Refractory with both beams clear. The next person
// breaks beam A. If we store that break first, the exit condition "both beams
// clear" is now false -- and it is false BECAUSE of the very event we are
// trying to react to. We stay in Refractory and discard the break. The same
// thing happens to the next person, and the one after that. feed() alone never
// escapes.
//
// What makes it nasty is that tick() also checks the refractory exit, and tick()
// does not touch beam state, so in the real firmware it quietly rescues us --
// but only if it happens to run in the gap between the beams clearing and the
// next person arriving. So the counter works on a quiet afternoon and starts
// dropping people whenever the loop is busy, which is exactly when the gym is
// busy. Correctness would depend on scheduling luck.
//
// Checking first and storing second removes the coupling entirely.
// ---------------------------------------------------------------------------
