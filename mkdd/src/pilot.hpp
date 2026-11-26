// Development: a pilot for the first controller port in runs without a
// player (pilot.cpp: MKDD_PILOT, MKDD_PILOT_RECORD, MKDD_PILOT_AFTER). PADRead
// calls these; without those variables they do nothing.
#pragma once

#include <cstdint>

#include "input.hpp"

namespace mkdd {

void pilot_frame();                  // once per PADRead
bool pilot_pad(GcPad& pad);          // true: `pad` is now the pilot's own, whole
void pilot_status(uint32_t status);  // the first port's PADStatus, just written: the pilot may steer in it

}  // namespace mkdd
