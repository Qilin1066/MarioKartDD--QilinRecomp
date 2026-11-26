// Enhancements that need the game itself changed: a picture wider than 4:3
// and frame rates above the game's own. The settings, the settings file and
// the menu have them (settings.hpp); what they ask for arrives here, and
// nothing is done with it yet. The settings menu shows each such option
// disabled, with why, until its *_supported() says otherwise.
//
// What is known of the game (USA, GM4E01; docs/enhancements.md has the
// analysis and the design): it draws a 4:3 picture and has no widescreen mode;
// it takes one fixed step of its logic per retrace in every mode, 59.94 a
// second, with no time step anywhere, so more pictures than that have to be
// drawn between the game's own. enhance.cpp holds the first of the game
// patches (the cameras' shape); it is not built or called yet.
//
// To implement one: replace its two functions here with declarations, define
// them beside the game patches (they are called on the main thread, the
// setters whenever the settings change and once at start), and make the
// *_supported() function return true once the patch is in and checked.
#pragma once

#include "settings.hpp"

namespace mkdd::enhance {

// A picture `aspect` wide (width over height, 4:3 to 4:1): the game's
// cameras show more at the sides (Hor+: the vertical field of view stays),
// what it culls against the view follows, and the HUD is placed as `hud` says.
// While this is false the picture stays 4:3 and set_aspect is not called.
inline bool aspect_supported() { return false; }
inline void set_aspect(float /*aspect*/, Hud /*hud*/) {}

// Frames a second above the game's own: `rate`, with the display's refresh
// in Hz for FrameRate::display (0: unknown). While this is false the game
// runs at its own rate (FrameRate::original) and set_frame_rate is not called.
inline bool frame_rate_supported() { return false; }
inline void set_frame_rate(FrameRate /*rate*/, double /*display_hz*/) {}

}  // namespace mkdd::enhance
