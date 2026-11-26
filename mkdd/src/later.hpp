// A device finishing something after a time (a memory card's write, an ARAM
// transfer, a controller's reset): the work runs as an interrupt once the
// guest's clock has passed that many ticks, in the order the times come due
// (and, for equal times, the order asked). Over os.hpp's timer sources.
#pragma once

#include <cstdint>
#include <functional>

#include "qilin/rt/system.hpp"

namespace mkdd {

void run_after(uint64_t ticks, std::function<void(qilin::rt::Cpu&)> work);
// Milliseconds and microseconds of the guest's clock, in its ticks.
uint64_t ticks_from_us(uint64_t us);

// MKDD_DEVICES_LOG=1: the devices log what the game asks of them (card calls
// and their results, controller resets and rumble, ARAM transfers in sum).
bool devices_log();
// Development: MKDD_WATCH=<address>[><offset>...][:1|2|4],... (hexadecimal; each
// > follows the pointer read so far and adds the offset) logs each of these guest
// values when it changes, looked at once a frame (at PADRead), with the game
// time in seconds: to follow the game's state in runs without a picture.
void watch_guest_values();

}  // namespace mkdd
