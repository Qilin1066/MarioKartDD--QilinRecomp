// From a player's inputs to a GameCube controller (input.hpp's GcPad): the
// settings' bindings applied to one controller, the keyboard and the mouse,
// and the scripts that play a port in runs without a player. No SDL, no
// window: the main loop hands in what the display read (input_test.cpp).
//
// What the game is given is a controller as the console's serial interface
// reports it, before the SDK's PADClamp: a stick at rest reads 0, and past the
// player's dead zone it starts where the SDK's own dead zone (15) ends and
// reaches 100 at the edge, a little past the SDK's limit (72 + 15), so a worn
// stick still reaches full lock. A trigger runs from the SDK's dead zone (30)
// to 220, with its click (the L or R button) past the settings' threshold.
// A key or a button on an analogue control is the whole way.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "input.hpp"
#include "qilin/host/pad.hpp"
#include "settings.hpp"

namespace mkdd {

inline constexpr int kGcStickFull = 100, kGcStickDead = 15, kGcTriggerDead = 30, kGcTriggerFull = 220;

// A binding with its key looked up (the display's key code; -1: no such key).
struct BoundInput {
    Binding::Kind kind = Binding::kPad;
    uint32_t pad = 0;
    int key = -1;
};
struct BoundControls {
    std::vector<BoundInput> controls[kControlCount];
    bool has_keys = false;  // a key or the mouse plays this port: it is always connected
};

// What a port's pad isn't: the keyboard and the mouse, as of the last poll.
struct KeySource {
    std::function<bool(int)> key_down;  // by key code; null: no keyboard
    bool mouse = false;                 // the left button
};

// One port's controller from its bindings. `clicks` keeps the triggers'
// clicks between calls (bit 0 L, bit 1 R), for their release 5 points below
// the press. `connected` is the caller's to set.
GcPad map_gc_pad(const BoundControls& bound, const qilin::host::PadState& pad, const KeySource& keys, unsigned stick_deadzone,
                 unsigned trigger_threshold, uint8_t& clicks);

// A script for one port (MKDD_INPUT for the first, MKDD_INPUT2 to MKDD_INPUT4):
// items separated by commas, each `what@seconds` or `what@seconds+duration`
// (0.2 s without one), in seconds of game time:
//   A B X Y Z L R Start Up Down Left Right   a button (L and R: the trigger all the way, with its click)
//   Stick:x:y   C:x:y                        a stick held there, -128 to 127 each way (y up)
//   LT:n   RT:n                              a trigger at n, 0 to 255, without its click
// "Start@12,A@14.5,Stick:-100:0@20+3,R@20+3".
struct ScriptStep {
    double at = 0, duration = 0.2;
    uint16_t buttons = 0;
    bool stick = false, c = false, lt = false, rt = false;
    int8_t x = 0, y = 0;
    uint8_t trigger = 0;
};
// False, with `error`, for an item that doesn't read.
bool parse_input_script(const std::string& text, std::vector<ScriptStep>& out, std::string& error);
// The steps due at `now`, onto `pad`.
void apply_input_script(const std::vector<ScriptStep>& script, double now, GcPad& pad);

}  // namespace mkdd
