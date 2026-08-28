#pragma once
#include <stdint.h>
#include "crossing_fsm.h"

// One walkway: two beams and the state machine that watches them.
//
// The entrance door is one Lane. The exit door is two, one per leaf, because
// people walk out side by side and a single machine watching all four beams
// would see an interleaved mess.
//
// What Lane adds on top of CrossingFSM is knowing when to stop trusting itself.
// If something is parked in a beam -- a propped door, a bin, a floor mat -- that
// beam reads blocked forever. The FSM alone would sit there quietly counting
// nothing, and nothing anywhere would say so. A silently wrong number is the
// worst outcome this project has, so a Lane declares itself unhealthy instead.
class Lane {
public:
  // stuckAfter  a beam held broken this long is not a person, it is an object
  void begin(uint32_t transitMin, uint32_t lingerMax, uint32_t refractory,
             uint32_t stuckAfter);

  // Same contract as CrossingFSM::feed -- the only thing that can return a
  // count. An unhealthy lane always returns None.
  Cross feed(bool isA, bool broken, uint32_t t_ms);

  // Call every loop. Two jobs: a stuck beam produces no edges at all, so
  // without this nothing would ever notice it -- and this is the ONLY place a
  // lane can become healthy again.
  void tick(uint32_t now_ms);

  bool isHealthy() const { return healthy_; }

  // For diagnostics: how long the longest-held beam has been broken. Zero when
  // both are clear.
  uint32_t blockedForMs(uint32_t now_ms) const;

private:
  void noteIfStuck(uint32_t now_ms);    // can only take health away
  void reviewHealth(uint32_t now_ms);   // can also give it back

  CrossingFSM fsm_;

  bool     aBroken_ = false;
  bool     bBroken_ = false;
  uint32_t aSince_  = 0;          // when the beam last went from clear to broken
  uint32_t bSince_  = 0;
  bool     healthy_ = true;

  // Kept so the FSM can be re-armed from scratch when the lane recovers.
  uint32_t transitMin_ = 120;
  uint32_t lingerMax_  = 3000;
  uint32_t refractory_ = 250;
  uint32_t stuckAfter_ = 300000;  // five minutes
};
