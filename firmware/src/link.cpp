#include "link.h"

// CRC-16/CCITT-FALSE. Not for security -- the radio link is encrypted for that.
// This is here to reject noise and half-received frames, which a checksum does
// and a length check does not.
uint16_t linkCrc(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

size_t linkEncode(const LinkFrame& in, uint8_t* out, size_t max) {
  if (max < LINK_FRAME_BYTES) return 0;

  out[0] = LINK_MAGIC;
  out[1] = LINK_VERSION;
  out[2] = in.kind;
  out[3] = (uint8_t)in.dir;
  out[4] = (uint8_t)(in.bootId & 0xFF);
  out[5] = (uint8_t)(in.bootId >> 8);
  out[6] = (uint8_t)(in.seq & 0xFF);
  out[7] = (uint8_t)(in.seq >> 8);
  out[8] = in.lanesOk;
  out[9] = in.laneCount;

  const uint16_t crc = linkCrc(out, 10);
  out[10] = (uint8_t)(crc & 0xFF);
  out[11] = (uint8_t)(crc >> 8);
  return LINK_FRAME_BYTES;
}

bool linkDecode(const uint8_t* in, size_t len, LinkFrame& out) {
  if (len != LINK_FRAME_BYTES)  return false;
  if (in[0] != LINK_MAGIC)      return false;
  if (in[1] != LINK_VERSION)    return false;

  const uint16_t want = (uint16_t)(in[10] | ((uint16_t)in[11] << 8));
  if (linkCrc(in, 10) != want)  return false;

  out.kind      = in[2];
  out.dir       = (int8_t)in[3];
  out.bootId    = (uint16_t)(in[4] | ((uint16_t)in[5] << 8));
  out.seq       = (uint16_t)(in[6] | ((uint16_t)in[7] << 8));
  out.lanesOk   = in[8];
  out.laneCount = in[9];
  return true;
}

void Dedup::reset() {
  have_ = false;
}

bool Dedup::accept(uint16_t bootId, uint16_t seq) {
  // A different bootId means the other board restarted and began counting from
  // one again. Its sequence numbers say nothing about ours.
  if (!have_ || bootId != bootId_) {
    have_    = true;
    bootId_  = bootId;
    lastSeq_ = seq;
    return true;
  }

  if (seq == lastSeq_) return false;              // the retry we expected

  // Unsigned difference, so this stays right when the counter wraps past 65535.
  // Anything more than half the range behind is treated as old rather than as
  // an enormous jump forward.
  const uint16_t ahead = (uint16_t)(seq - lastSeq_);
  if (ahead >= 0x8000) return false;              // stale, or a replayed frame

  lastSeq_ = seq;
  return true;
}
