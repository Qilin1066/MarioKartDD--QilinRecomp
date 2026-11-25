#pragma once
#include <cstdint>
namespace mkdd {
struct GcPad { bool connected; uint16_t buttons; int8_t stick_x, stick_y, c_x, c_y; uint8_t l, r; };  // buttons: the SDK's PAD_BUTTON_* bits (LEFT 0x0001, RIGHT 0x0002, DOWN 0x0004, UP 0x0008, Z 0x0010, R 0x0020, L 0x0040, A 0x0100, B 0x0200, X 0x0400, Y 0x0800, START 0x1000); sticks -128..127 before the SDK's clamp; l, r 0..255
GcPad gc_pad(int port);            // a consistent snapshot
void gc_rumble(int port, bool on); // PADControlMotor
}

// The four controller ports between the window's thread and the guest
// (input.cpp). The main thread writes each port's pad after every poll, made
// from the player's bindings (input_map.hpp) or a script (MKDD_INPUT); the
// guest's PADRead reads it, and PADControlMotor says whether a port's
// controller should rumble. Ports are 0 to 3; any other reads as an empty
// port. While the settings menu is open every port reads as its controller
// at rest (still connected).
namespace mkdd {

inline constexpr int kGcPorts = 4;
enum GcButton : uint16_t {
    kGcLeft = 0x0001, kGcRight = 0x0002, kGcDown = 0x0004, kGcUp = 0x0008, kGcZ = 0x0010, kGcR = 0x0020, kGcL = 0x0040,
    kGcA = 0x0100, kGcB = 0x0200, kGcX = 0x0400, kGcY = 0x0800, kGcStart = 0x1000,
};

// The main thread's side.
void set_gc_pad(int port, const GcPad& pad);
bool gc_rumble_wanted(int port);

}  // namespace mkdd

// TEMPORARY: what the base's wpad.cpp (the Wii Remote, not this game's) reads,
// kept only so the tree builds until the devices layer replaces that file with
// PADRead over gc_pad. Nothing writes these any more. Remove with wpad.cpp.
#include <atomic>
namespace mkdd {
inline std::atomic<uint32_t> g_controls{0}, g_controls2{0};
inline std::atomic<bool> g_rumble{false}, g_rumble2{false}, g_player2{false}, g_pointer_on{false};
inline std::atomic<int32_t> g_pointer_x{0}, g_pointer_y{0}, g_pointer2_x{0}, g_pointer2_y{0};
}  // namespace mkdd
