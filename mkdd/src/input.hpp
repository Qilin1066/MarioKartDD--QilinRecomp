// What the window's thread knows of the player's input, for the guest (wpad.cpp).
#pragma once

#include <atomic>
#include <cstdint>

namespace mkdd {
struct GcPad { bool connected; uint16_t buttons; int8_t stick_x, stick_y, c_x, c_y; uint8_t l, r; };  // buttons: the SDK's PAD_BUTTON_* bits (LEFT 0x0001, RIGHT 0x0002, DOWN 0x0004, UP 0x0008, Z 0x0010, R 0x0020, L 0x0040, A 0x0100, B 0x0200, X 0x0400, Y 0x0800, START 0x1000); sticks -128..127 before the SDK's clamp; l, r 0..255
GcPad gc_pad(int port);            // a consistent snapshot
void gc_rumble(int port, bool on); // PADControlMotor
}

namespace mkdd {

// The Wii Remote's buttons held (a bit per mkdd::Control, settings.hpp),
// written by the main thread after each poll from the player's bindings; and
// whether the guest wants the pad to rumble.
inline std::atomic<uint32_t> g_controls{0};
inline std::atomic<bool> g_rumble{false};
// A second player, when the settings give one a controller (settings.hpp,
// second_player) and that controller is there: a second Wii Remote, with
// buttons, rumble and a pointer of its own (its controller's right stick).
inline std::atomic<bool> g_player2{false};
inline std::atomic<uint32_t> g_controls2{0};
inline std::atomic<bool> g_rumble2{false};
inline std::atomic<int32_t> g_pointer2_x{0}, g_pointer2_y{0};

// Where the player points (the Wii Remote's pointer, from the mouse or the
// right stick): -1 to 1 across the picture, y up, in thousandths; and
// whether it's on the picture at all.
inline std::atomic<int32_t> g_pointer_x{0}, g_pointer_y{0};
inline std::atomic<bool> g_pointer_on{true};

}  // namespace mkdd
