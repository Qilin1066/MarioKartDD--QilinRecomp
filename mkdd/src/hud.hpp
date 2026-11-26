// The HUD on a picture wider than the game drew it for (hud.cpp): the frame is
// stretched across the screen with everything flat in it, and what isn't
// meant to fill the width is squeezed back to its own proportions about a
// point that depends on where it is. Only gx.hpp is needed, so the recorder's
// tests link it without the game's patches (enhance.cpp gives it the widening).
#pragma once

#include <cstdint>

#include "gx.hpp"

namespace mkdd::hud {

// Where flat parts go: kept at the nearer side of their view (a part in the
// middle third keeps its place), all in the middle 4:3, or left stretched.
// The settings' Hud, in its order.
enum class Place : uint8_t { edges, middle, stretched };
void set_place(Place p);

// How many times wider than drawn the frame now ending will be shown (1:
// nothing to do). Asked once per frame, on the thread that records.
void set_widening_source(float (*source)());

// Whether a flat draw follows something in the 3D scene (a mark over a kart,
// a flare): such a part is squeezed about its own centre wherever it is. The
// recorder's matrix tags answer this once they exist; without a source,
// nothing does.
void set_follows_world(bool (*source)(const gx::Frame&, const gx::Draw&));

// A finished frame, before it goes to the renderer: its flat draws placed.
void place(gx::Frame& frame);

// The rule itself, in EFB pixels: where a part spanning [x0, x1] in a view
// spanning [left, right] is squeezed about; false if it's left as it is (it
// fills the view's width, or the HUD is stretched).
bool anchor_of(Place p, float left, float right, float x0, float x1, bool follows_world, float& anchor);

}  // namespace mkdd::hud
