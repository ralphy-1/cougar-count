#pragma once
#include <stdint.h>
#include "link.h"

// ESP-NOW between the two boards. Thin on purpose: framing and duplicate
// detection live in link.h where they can be tested, and this file only moves
// bytes.
//
// The link is encrypted. Without that, anyone within radio range and holding
// twenty dollars of hardware could inject crossings and move the number at
// will -- and a gym entrance is about as public a place as exists on campus.
namespace Radio {

void begin(uint16_t bootId);

// Exit board. Fire and forget at this level: delivery is decided by whether an
// acknowledgement comes back, not by whether the radio accepted the frame.
bool sendCrossing(int8_t dir, uint16_t seq, uint8_t lanesOk, uint8_t laneCount);
bool sendHealth(uint8_t lanesOk, uint8_t laneCount);

// True once the entrance board has acknowledged this exact sequence number.
bool ackSeen(uint16_t seq);

// Entrance board. Returns true and fills `out` when a crossing arrives that has
// not been seen before. Duplicates are acknowledged and discarded here, because
// a duplicate that goes unacknowledged is retried forever.
bool takeCrossing(int8_t& dir);

// Health reported by the far board, or 0xFF if it has never been heard from.
uint8_t peerLanesOk();
bool    peerHeardFrom();

}  // namespace Radio
