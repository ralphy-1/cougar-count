#pragma once
#include <stdint.h>
#include <stddef.h>

// Crossings that happened but have not reached the database yet.
//
// The wifi will drop. When it does, people keep walking through the door, and
// those people have to go somewhere until the network comes back. This is that
// somewhere.
//
// Only the direction is kept. Not the time, not the lane. The server owns the
// counting -- each entry becomes one atomic +1 against in_total or out_total --
// so a queued crossing is a single byte, and a full queue is 256 of them.
class Pending {
public:
  static const uint16_t CAPACITY = 256;

  void clear();

  // Drops the OLDEST entry when full. A crossing from twenty minutes ago
  // matters less than the one that just happened, and silently keeping the old
  // one would mean the number stays wrong for longer.
  void push(int8_t dir);

  bool  peek(int8_t& dir) const;   // false when empty
  void  pop();

  uint16_t size()  const { return count_; }
  bool     empty() const { return count_ == 0; }
  bool     full()  const { return count_ == CAPACITY; }

  // How many crossings have been thrown away for lack of room. Never resets on
  // its own. If this is ever non-zero the day's total is knowingly short, and
  // that belongs in diagnostics rather than being quietly forgotten.
  uint32_t dropped() const { return dropped_; }

  // Persistence is somebody else's problem -- these hand over plain bytes and
  // take them back. Nothing in this file knows what flash is, which is the only
  // reason it can be tested on a laptop.
  size_t serialize(int8_t* out, size_t max) const;   // oldest first
  void   deserialize(const int8_t* in, size_t n);

private:
  int8_t   ring_[CAPACITY] = {0};
  uint16_t head_    = 0;      // index of the oldest entry
  uint16_t count_   = 0;
  uint32_t dropped_ = 0;
};
