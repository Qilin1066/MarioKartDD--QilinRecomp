// A player's pad, as the host layer reports it (display.hpp).
#pragma once

#include <cstdint>

namespace qilin::host {

// The pad's inputs as buttons, by an Xbox pad's names (any SDL gamepad maps
// onto them). The triggers and each stick's four directions count as buttons
// too, past a threshold (display.hpp, set_input_thresholds), so the settings
// can bind them like any other. The keyboard is read key by key (key_down).
enum PadButton : uint32_t {
    kPadUp = 1u << 0, kPadDown = 1u << 1, kPadLeft = 1u << 2, kPadRight = 1u << 3,  // the D-pad
    kPadA = 1u << 4, kPadB = 1u << 5, kPadX = 1u << 6, kPadY = 1u << 7,  // by position: south, east, west, north
    kPadStart = 1u << 8, kPadBack = 1u << 9, kPadGuide = 1u << 10,
    kPadClick = 1u << 11,  // the mouse's left button
    kPadLB = 1u << 12, kPadRB = 1u << 13,  // the shoulder buttons
    kPadLT = 1u << 14, kPadRT = 1u << 15,  // the triggers
    kPadL3 = 1u << 16, kPadR3 = 1u << 17,  // the sticks, pressed in
    kPadLSUp = 1u << 18, kPadLSDown = 1u << 19, kPadLSLeft = 1u << 20, kPadLSRight = 1u << 21,  // the left stick
    kPadRSUp = 1u << 22, kPadRSDown = 1u << 23, kPadRSLeft = 1u << 24, kPadRSRight = 1u << 25,  // the right stick
};

// One controller, as of the last poll: the buttons above, and the analogue
// inputs as the device gives them (no dead zone): sticks -1 to 1 (y up),
// triggers 0 to 1.
struct PadState {
    bool connected = false;
    uint32_t buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};

}  // namespace qilin::host
