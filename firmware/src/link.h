#pragma once
#include <stdint.h>
#include <stddef.h>

// The message the exit board sends to the entrance board, and the rules for
// deciding whether an arriving one is real and new.
//
// Only the entrance board is on the campus network. The exit board reaches it
// over ESP-NOW, a direct radio link between the two ESP32s, which is why the IT
// request is for one registered device instead of two.
//
// Nothing here touches a radio. Framing and duplicate detection are exactly the
// parts that fail in ways you cannot see from the outside -- a count arriving
// twice looks identical to two people -- so they run on a laptop.

static const uint8_t LINK_MAGIC   = 0xC5;
static const uint8_t LINK_VERSION = 1;

enum LinkKind : uint8_t {
  LINK_CROSSING = 1,   // one person, direction in `dir`
  LINK_HEALTH   = 2,   // lane health only, no count
  LINK_ACK      = 3,   // the entrance board confirming it has taken `seq`
};

struct LinkFrame {
  uint8_t  kind;
  int8_t   dir;         // +1 in, -1 out, 0 for anything that is not a crossing
  uint16_t bootId;      // changes every restart, so sequence numbers cannot collide
  uint16_t seq;
  uint8_t  lanesOk;     // bit per lane, 1 = healthy
  uint8_t  laneCount;
};

static const size_t LINK_FRAME_BYTES = 12;

// Returns bytes written, or 0 if the buffer is too small.
size_t linkEncode(const LinkFrame& in, uint8_t* out, size_t max);

// Returns false for anything that is not a well formed frame of a version we
// understand: wrong magic, wrong version, wrong length, or a failed checksum.
// A radio in a public building will hear all sorts of things.
bool linkDecode(const uint8_t* in, size_t len, LinkFrame& out);

uint16_t linkCrc(const uint8_t* data, size_t len);

// Decides whether an arriving crossing has already been counted.
//
// The exit board holds each crossing until the entrance board acknowledges it,
// and retries if the acknowledgement does not arrive. That retry is the whole
// reason this exists: a lost ACK means the same person is sent twice, and
// without this the gym would gain a phantom occupant every time the radio
// hiccupped.
class Dedup {
public:
  void reset();

  // True if this is new and should be counted. A duplicate must still be
  // acknowledged by the caller, or the sender retries it forever.
  bool accept(uint16_t bootId, uint16_t seq);

private:
  bool     have_    = false;
  uint16_t bootId_  = 0;
  uint16_t lastSeq_ = 0;
};
