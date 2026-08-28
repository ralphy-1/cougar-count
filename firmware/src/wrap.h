#pragma once
#include <stdint.h>

// millis() wraps to zero every ~49.7 days and these boards run for months, so a
// deadline can be a small number while `now` is still huge. Comparing the two
// directly reads that as "long past" and expires the timer instantly.
//
// Subtracting first and asking whether the gap is under half the range stays
// correct through the wrap, and is fully defined unsigned arithmetic. Valid as
// long as the two are under ~24 days apart, which every timeout in this project
// is by orders of magnitude.
static inline bool reached(uint32_t now_ms, uint32_t deadline_ms) {
  return (now_ms - deadline_ms) < 0x80000000u;
}
