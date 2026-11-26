// What the picture and frame-rate options need from inside the game
// (docs/enhancements.md has the evidence for every address here, and the rest
// of the design: this file holds only the hooks that reading the code settled).
//
// The picture's shape. The game has one source for its cameras' aspect ratio:
// SystemData's scAspect (608/448, the whole screen and each quarter of it) and
// scAspectDiv2 (608/223, half the screen), two floats its static constructor
// computes from the viewport sizes, read only through System's three getters.
// KartCam::DoPort copies a getter's value into each camera when a race is set
// up or reset; the projection (GrafPort::setPort), the camera's clipper
// (KartCam::CheckClipper, every tick), the screen positions the HUD follows
// karts by (SiUtil::getScreenPos) and the lens flare (TFlareLine) all take it
// from there. So Hor+ is the getters' value times how much wider the picture is
// shown, with the vertical field of view, the 608x448 frame and everything
// drawn flat into it left alone: the display stretches the frame, as a
// widescreen TV does an anamorphic picture.
//
// Not in the build yet (CMakeLists.txt doesn't list it): the three getters
// must be marked replaced in the plan before replacements() can be given to
// rt::provide, which stops on a lifted function.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>

#include "qilin/rt/system.hpp"

namespace mkdd::enhance {

// Until enhance.hpp (the host's) declares them.
void set_widening(float times, bool running);
float widening();
void draw_every_tick();
std::span<const qilin::rt::Provided> replacements();

namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// Small data (r13 = 803D1420).
constexpr uint32_t kAspect = 0x803CBE08u;      // SystemData::scAspect, stored at 801D65D0
constexpr uint32_t kAspectDiv2 = 0x803CBE0Cu;  // SystemData::scAspectDiv2, stored at 801D6660
constexpr uint32_t kDisplay = 0x803CBD70u;     // System::mspDisplay (the JFWDisplay)
constexpr uint32_t kRaceMgr = 0x803CB7E8u;     // RaceMgr's manager, null outside a race
constexpr uint32_t kKartCtrl = 0x803CC588u;    // KartCtrl's instance

constexpr uint32_t kDrawInterval = 0x38;  // JFWDisplay: draw every n-th pass of the main loop (8007ADB4)
constexpr uint32_t kRaceInfo = 0x38;      // RaceMgr: its RaceInfo
constexpr uint32_t kCameraCount = 0x20;   // RaceInfo: cameras (a signed half word; 80189280)
constexpr uint32_t kCameras = 0x200;      // KartCtrl: KartCam* for each camera (802A0278)
constexpr uint32_t kCameraAspect = 0x168; // KartCam: the aspect DoPort gave it (8029E194)

float g_widening = 1.0f;  // how many times wider than the game's own picture it is shown

float peekf(uint32_t a) { return std::bit_cast<float>(rt::peek32(a)); }
void pokef(uint32_t a, float f) { rt::poke32(a, std::bit_cast<uint32_t>(f)); }

void give(Cpu& c, float f) { c.ps0[1] = c.ps1[1] = f; }  // lfs f1 fills both halves

// 801D0F50, 801D0F58, 801D0F60: each a load of the float and a return.
void getPerspAspect(Cpu& c) { give(c, peekf(kAspect) * g_widening); }
void getPerspAspectDiv2(Cpu& c) { give(c, peekf(kAspectDiv2) * g_widening); }
void getPerspAspectDiv4(Cpu& c) { give(c, peekf(kAspect) * g_widening); }

const rt::Provided kReplacements[] = {
    {0x801D0F50u, "System::getPerspAspect", getPerspAspect},
    {0x801D0F58u, "System::getPerspAspectDiv2", getPerspAspectDiv2},
    {0x801D0F60u, "System::getPerspAspectDiv4", getPerspAspectDiv4},
};

}  // namespace

float widening() { return g_widening; }

// `times`: the picture's width over the width the game drew it for (16:9 on a
// 4:3 frame: 4/3). `running`: the game has started, so cameras may exist;
// called from the game's thread between passes of its main loop then.
void set_widening(float times, bool running) {
    times = std::clamp(times, 1.0f, 3.0f);  // 32:9 over 4:3 is 2.67
    const float change = times / g_widening;
    g_widening = times;
    if (!running || change == 1.0f) return;
    // A race in progress holds copies: each camera's is rescaled, and its
    // clipper follows at the camera's next step (CheckClipper compares).
    const uint32_t race = rt::peek32(kRaceMgr), ctrl = rt::peek32(kKartCtrl);
    if (!race || !ctrl) return;
    const int count = static_cast<int16_t>(rt::peek16(rt::peek32(race + kRaceInfo) + kCameraCount));
    for (int i = 0; i < std::min(count, 4); ++i) {
        const uint32_t cam = rt::peek32(ctrl + kCameras + 4 * static_cast<uint32_t>(i));
        if (cam) pokef(cam + kCameraAspect, peekf(cam + kCameraAspect) * change);
    }
}

// The game's own frame skipping, undone: LAN play with the fullest screens sets
// the display to draw on every second pass of the main loop (RaceApp::calc,
// 801CD9A4) to spare the console's GPU; its logic runs on every pass either
// way. Called once a pass, before JFWDisplay::beginRender counts it.
void draw_every_tick() {
    const uint32_t display = rt::peek32(kDisplay);
    if (display && rt::peek32(display + kDrawInterval) > 1) rt::poke32(display + kDrawInterval, 1);
}

std::span<const rt::Provided> replacements() { return kReplacements; }

}  // namespace mkdd::enhance
