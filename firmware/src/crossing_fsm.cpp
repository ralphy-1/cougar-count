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
// whether we can leave refractory. That version counts the first person of the
// day correctly and then never counts anyone again, forever.
//
// After a crossing we sit in Refractory with both beams clear. The next person
// arrives and breaks beam A. If we store that break first, the exit condition
// "both beams clear" is now false -- and it is false BECAUSE of the very event
// we are trying to react to. We stay in Refractory. That break is discarded.
// The person walks through, the beams clear, and we are still in Refractory
// with nothing pending. The same thing happens to the next person, and the one
// after that.
//
// Nothing crashes. Nothing logs an error. The counter just quietly stops at 1.
// ---------------------------------------------------------------------------
