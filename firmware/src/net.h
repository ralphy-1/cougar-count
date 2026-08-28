#pragma once
#include <stdint.h>
#include "crossing_fsm.h"

// Everything that touches the network. Deliberately thin: it makes no decisions
// about counting, only about connections. Anything worth testing lives in
// wire.h, lane.h or pending.h instead, all of which run on a laptop.
namespace Net {

void begin();

// Keeps wifi up and the ID token fresh. Non-blocking -- call it every loop and
// let it make one step of progress at a time. Nothing here may ever busy-wait,
// because a stalled loop is a lane that stops noticing a stuck beam.
void service(uint32_t now_ms);

bool ready();

// One atomic +1 on the counter this crossing belongs to. Returns false on any
// failure, and the caller keeps the crossing queued.
bool sendCrossing(Cross c);

// Server timestamp into updated_at. This is what tells the website the system
// is alive; without it a healthy but empty gym looks identical to a dead board.
bool sendHeartbeat();

}  // namespace Net
