#include "pending.h"

void Pending::clear() {
  head_  = 0;
  count_ = 0;
  // dropped_ deliberately survives. It is a record of counts already lost, and
  // reloading the queue does not un-lose them.
}

void Pending::push(int8_t dir) {
  if (full()) {
    pop();
    dropped_++;
  }
  const uint16_t tail = (uint16_t)((head_ + count_) % CAPACITY);
  ring_[tail] = dir;
  count_++;
}

bool Pending::peek(int8_t& dir) const {
  if (empty()) return false;
  dir = ring_[head_];
  return true;
}

void Pending::pop() {
  if (empty()) return;
  head_ = (uint16_t)((head_ + 1) % CAPACITY);
  count_--;
}

size_t Pending::serialize(int8_t* out, size_t max) const {
  const size_t n = (count_ < max) ? count_ : max;
  for (size_t i = 0; i < n; i++)
    out[i] = ring_[(head_ + i) % CAPACITY];
  return n;
}

void Pending::deserialize(const int8_t* in, size_t n) {
  clear();
  const size_t take = (n < CAPACITY) ? n : CAPACITY;
  // Keep the NEWEST when handed more than fits, for the same reason push()
  // drops the oldest.
  const size_t skip = n - take;
  for (size_t i = 0; i < take; i++)
    push(in[skip + i]);
}
