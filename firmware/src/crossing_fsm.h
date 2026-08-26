#pragma once
#include <stdint.h>

// One instance watches ONE lane: two beams, A and B, across a single walkway.
// The exit door has two leaves and therefore two instances, because two people
// can walk through side by side and a single state machine watching all four
// beams would see an interleaved mess.
//
// A crossing is only counted when a person breaks one beam, then the other,
// then clears both -- in a plausible amount of time. Direction comes from which
// beam broke first, never from which door the board happens to be bolted to.
// Beam A is the OUTSIDE beam at both doors, which is what lets one piece of
// code run on both boards unchanged.
enum class Cross : int8_t { None = 0, In = 1, Out = -1 };

class CrossingFSM {
public:
  // transitMin  too fast to be a person -- a swinging door, a bag, a glitch
  // lingerMax   too slow to be a crossing -- someone standing in the doorway
  // refractory  quiet period after a count, so one person is never counted twice
  void begin(uint32_t transitMin, uint32_t lingerMax, uint32_t refractory);

  // Call on every beam edge. Returns a crossing, or None. This is the ONLY
  // function that can return a count.
  Cross feed(bool isA, bool broken, uint32_t t_ms);

  // Call every loop. Cleans up crossings that stalled and never finished.
  // Deliberately returns nothing: a bug in timing logic must never be able to
  // invent a person who did not walk through the door.
  void tick(uint32_t now_ms);

  uint32_t lastTransitMs() const { return lastTransit_; }
  bool     isIdle()        const { return st_ == St::Idle; }

private:
  enum class St : uint8_t { Idle, PendingIn, PendingOut, Refractory };
  void reset(uint32_t now_ms);

  St       st_             = St::Idle;
  bool     aBroken_        = false;
  bool     bBroken_        = false;
  bool     secondSeen_     = false;   // did the OTHER beam ever break?
  uint32_t tFirstBreak_    = 0;
  uint32_t tRefractoryEnd_ = 0;
  uint32_t lastTransit_    = 0;

  uint32_t transitMin_ = 120;
  uint32_t lingerMax_  = 3000;
  uint32_t refractory_ = 250;
};
