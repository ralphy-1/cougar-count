// Compiling this IS the test. config.h carries static_asserts that reject a
// beam wired to a strapping, flash, USB or UART pin, and reject two beams
// sharing a pin. Both doors are checked, because they have different pin maps
// and only one of them is built at a time on real hardware.
//
//   c++ -std=c++17 -D DEVICE_IS_ENTRANCE -fsyntax-only -I firmware/src firmware/test/test_config.cpp
//   c++ -std=c++17 -D DEVICE_IS_EXIT     -fsyntax-only -I firmware/src firmware/test/test_config.cpp

#include "config.h"

static_assert(LANE_COUNT >= 1, "a door with no lanes counts nobody");
static_assert(HEARTBEAT_MS < 6u * 60u * 1000u,
              "the website hides a count older than six minutes; the heartbeat must beat that");
static_assert(TRANSIT_MIN_MS < LINGER_MAX_MS,
              "the fastest allowed crossing must be faster than the slowest");
static_assert(STUCK_AFTER_MS > LINGER_MAX_MS,
              "a stuck beam must outlast an ordinary stall, or every dawdler blocks a lane");
