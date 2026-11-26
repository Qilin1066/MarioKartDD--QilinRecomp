// Development: a probe of the game's response time, from a press to the
// first picture presented that can show it (MKDD_LATENCY).
//
// MKDD_LATENCY=<game seconds>[:<button>] presses a D-pad direction (Down,
// then Up, so a menu's cursor goes back and forth; or the button named) on
// port 1 once a second from then on, each at a random moment of the host's
// clock, as a player's thumb would, for a fifth of a second. Each press is
// followed through the pipeline and logged:
//   sampled   the main thread's next look at the input (set_gc_pad)
//   read      the guest's PADRead that sees it (its pass reads the pads
//             before it draws, and calculates after: the pass after it is
//             the first whose drawing can show it)
//   frame     that drawing's frame handed to the main thread (the recorder's)
//   drawn     the main thread has drawn it
//   presented the first picture presented that holds it (submitted: the
//             display's own wait to the screen comes after, a vblank at most
//             with vsync, and can't be seen without one)
// Every 20 presses, the averages and ranges. Without the variable, nothing.
#pragma once

#include <cstdint>

#include "input.hpp"

namespace mkdd::latency {

bool enabled();
uint64_t now_ns();
// Main thread, as it sets port 1: the probe's press, from its moment on.
void sample(GcPad& pad, double game_seconds);
// The probe's buttons (for the pilot, which otherwise owns port 1's pad).
void probe_apply(GcPad& pad);
// Guest, PADRead: port 1's pad as the game reads it.
void pad_read(const GcPad& pad);
// Recorder: a frame handed over (its number from 1, in order).
void frame_handed(uint64_t number);
// Main thread: a frame taken and drawn; a picture presented, `newest` the
// number of the newest frame in it (blended or whole).
void frame_drawn(uint64_t number);
void presented(uint64_t newest);
// The dumps' pictures (MKDD_DUMP), to check which picture first shows a press.
uint64_t pictures();
// MKDD_LATENCY_CHECK=1 (offscreen): while a press is followed, each picture presented is
// read back and which of its 8x8 cells changed is logged, to see which first shows the
// press (the timings are then not the game's: read-backs take milliseconds).
bool checking();
void check_picture(const uint8_t* rgba, uint32_t width, uint32_t height);

}  // namespace mkdd::latency
