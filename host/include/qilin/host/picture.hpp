// How the window shows the picture (display.hpp), apart from the display so
// the settings can name it without Vulkan's headers.
#pragma once

#include <cstdint>

namespace qilin::host {

// How the picture fills the window: whole multiples of the console's picture
// (the sharpest), the largest that keeps its shape, or the whole window.
enum class Scaling : uint8_t { integer, fit, stretch };
// What it's drawn through: as it is, or a CRT television's raster lines and
// phosphor stripes.
enum class Look : uint8_t { plain, crt };

struct Picture {
    Scaling scaling = Scaling::fit;
    bool smooth = true;       // bilinear; off: nearest pixel
    // Before the look (post.hpp): SMAA 1x anti-aliasing at the picture's own
    // size, then AMD FSR 1 (EASU, and RCAS at `sharpness`, 0..1) up to the
    // size it's shown at, where that is larger and the scaling isn't integer.
    bool smaa = false, fsr = false;
    float sharpness = 0.8f;
    Look look = Look::plain;
    float scanlines = 0.6f;   // CRT: how dark the gaps between raster lines are, 0..1
    float mask = 0.3f;        // CRT: the phosphor stripes' strength, 0..1
    bool vsync = true;        // at the display's refresh, no tearing; off: the lowest latency
    // With vsync, where the GPU can wait for a frame to reach the screen
    // (VK_KHR_present_wait): at most one frame waiting behind the one shown.
    bool low_latency = true;
    // Fullscreen on a display whose refresh isn't a whole multiple of 60 Hz
    // (75, 144, 165 Hz): switch to the highest mode at the same resolution
    // that is (120 Hz on a 144 Hz monitor), so every frame is shown alike.
    bool match_refresh = true;
    // A G-Sync, FreeSync or VRR display with it on in the driver: no mode
    // switch and no locking to the refresh; the display follows the game's
    // own rate. (No program can see the driver's setting, so it's a switch.)
    bool variable_refresh = false;
};

}  // namespace qilin::host
