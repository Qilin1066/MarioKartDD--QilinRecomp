// Enhancements that need the game itself changed (enhance.cpp;
// docs/enhancements.md has the game's side of each): a picture wider than
// 4:3; frame rates above the game's own, which are the renderer's pictures
// between frames (interp.hpp) and need only the two tagged matrix loaders in
// the build. The settings menu shows an option disabled, with why, while its
// *_supported() is false.
#pragma once

#include <span>

#include "interp.hpp"
#include "qilin/rt/system.hpp"
#include "settings.hpp"

namespace mkdd::enhance {

// A picture `aspect` wide (width over height, 4:3 to 4:1) during races: the
// game's cameras show more at the sides (Hor+: the vertical field of view
// stays), what it culls against the view follows, and the HUD is placed as
// `hud` says (hud.hpp). Menus, movies and everything else outside a race
// stay 4:3. From the main thread, at start and whenever the settings change;
// the game takes it up at the end of the frame it is drawing.
inline bool aspect_supported() { return true; }
void set_aspect(float aspect, Hud hud);
// The shape to show the newest recorded frame at: the aspect asked for if it
// was drawn during a race, else 4:3.
float shown_aspect();

// Frames a second above the game's own: `rate`, with the display's refresh
// in Hz for FrameRate::display (0: unknown). The game itself always runs at
// its own 59.94; display and unlimited show pictures between its frames
// (interp.hpp, which this forwards to; main.cpp's loop presents them).
// Supported once mtxtag::replacements() has been given to the runtime.
inline bool frame_rate_supported() { return interp::supported(); }
inline void set_frame_rate(FrameRate rate, double display_hz) { interp::set_frame_rate(rate, display_hz); }

// The game's side, on its own thread.
// A race is on (or the award scene, or the staff roll, which use its cameras).
bool in_race();
// How many times wider than 4:3 the cameras are set now.
float widening();
// The end of a frame (the recorder asks through hud.hpp): the widening the
// frame was drawn with, 1 outside a race; then a change asked for meanwhile
// is given to the cameras, for the next frame.
float end_frame();
// Undo LAN play's draw-on-every-second-pass; once a pass of the main loop,
// before JFWDisplay::beginRender counts it. (Nothing calls it yet.)
void draw_every_tick();
// System::getPerspAspect, getPerspAspectDiv2, getPerspAspectDiv4.
std::span<const qilin::rt::Provided> replacements();

}  // namespace mkdd::enhance
