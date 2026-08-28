#include "wire.h"
#include "wrap.h"

const char* const INCREMENT_BODY = "{\".sv\":{\"increment\":1}}";
const char* const HEARTBEAT_BODY = "{\".sv\":\"timestamp\"}";
const char* const HEARTBEAT_PATH = "/gym/live/updated_at.json";
const char* const LANES_OK_PATH  = "/gym/live/all_lanes_ok.json";

const char* boolBody(bool v) { return v ? "true" : "false"; }

const char* counterPath(Cross c) {
  switch (c) {
    case Cross::In:  return "/gym/live/in_total.json";
    case Cross::Out: return "/gym/live/out_total.json";
    default:         return nullptr;
  }
}

void Due::begin(uint32_t periodMs, uint32_t now_ms) {
  period_ = periodMs;
  next_   = now_ms + periodMs;
}

bool Due::due(uint32_t now_ms) {
  if (!reached(now_ms, next_)) return false;
  next_ = now_ms + period_;
  return true;
}

void Due::postpone(uint32_t now_ms) {
  next_ = now_ms + period_;
}

void Due::armIn(uint32_t now_ms, uint32_t delayMs) {
  next_ = now_ms + delayMs;
}
