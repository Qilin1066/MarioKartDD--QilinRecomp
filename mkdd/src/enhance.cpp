// What the picture and frame-rate options need from inside the game
// (docs/enhancements.md has the evidence for every address here).
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
// widescreen TV does an anamorphic picture, and hud.cpp puts the flat parts
// back in proportion.
//
// Threads: the settings arrive on the main thread (set_aspect) and the frame's
// shape is read there (shown_aspect); the game's thread takes a change up at
// the end of a frame (end_frame, called by the recorder through hud.cpp), so
// the cameras never change under a frame being drawn.
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>

#include "enhance.hpp"
#include "hud.hpp"

namespace mkdd::enhance {

namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// Small data (r13 = 803D1420).
constexpr uint32_t kAspect = 0x803CBE08u;      // SystemData::scAspect, stored at 801D65D0
constexpr uint32_t kAspectDiv2 = 0x803CBE0Cu;  // SystemData::scAspectDiv2, stored at 801D6660
constexpr uint32_t kDisplay = 0x803CBD70u;     // System::mspDisplay (the JFWDisplay)
constexpr uint32_t kRaceMgr = 0x803CB7E8u;     // RaceMgr's manager: set as its constructor starts (801879E4), cleared by its destructor (8018A164)
constexpr uint32_t kKartCtrl = 0x803CC588u;    // KartCtrl's instance: set by makeKartCtrl (8029F64C) and never cleared

constexpr uint32_t kDrawInterval = 0x38;  // JFWDisplay: draw every n-th pass of the main loop (8007ADB4)
constexpr uint32_t kRaceInfo = 0x38;      // RaceMgr: its RaceInfo
constexpr uint32_t kCameraCount = 0x20;   // RaceInfo: cameras (a signed half word; 80189280)
constexpr uint32_t kCameras = 0x200;      // KartCtrl: KartCam* for each camera (802A0278)
constexpr uint32_t kCameraAspect = 0x168; // KartCam: the aspect DoPort gave it (8029E194)

constexpr float kOriginal = 4.0f / 3.0f;  // the shape the unwidened frame is shown at

std::atomic<float> g_wanted{1.0f};  // the settings' (main thread)
std::atomic<float> g_shown{1.0f};   // the newest finished frame's
float g_widening = 1.0f;            // what the getters give and the cameras hold (the game's thread)

float peekf(uint32_t a) { return std::bit_cast<float>(rt::peek32(a)); }
void pokef(uint32_t a, float f) { rt::poke32(a, std::bit_cast<uint32_t>(f)); }
bool in_ram(uint32_t a) { return a >= 0x80000000u && a < 0x81800000u - 0x200u; }

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

// A race in progress holds copies of the getters' values: each camera's is
// rescaled, and its clipper follows at the camera's next step (CheckClipper
// compares). RaceMgr's constructor makes KartCtrl and its cameras before it
// returns (801880A0, 801880AC), and no frame ends inside it, so at a frame's
// end a race's pointers are whole; what they hold is still checked, because a
// wrong one would be a write into something else.
void rescale_cameras(float change) {
    const uint32_t race = rt::peek32(kRaceMgr), ctrl = rt::peek32(kKartCtrl);
    if (!race) return;
    if (!in_ram(race) || !in_ram(ctrl) || !in_ram(rt::peek32(race + kRaceInfo)))
        rt::fatal(nullptr, "enhance: a race is on (RaceMgr %08X) but KartCtrl is %08X: the cameras can't be widened", race, ctrl);
    const int count = static_cast<int16_t>(rt::peek16(rt::peek32(race + kRaceInfo) + kCameraCount));
    for (int i = 0; i < std::min(count, 4); ++i) {
        const uint32_t cam = rt::peek32(ctrl + kCameras + 4 * static_cast<uint32_t>(i));
        const float now = in_ram(cam) ? peekf(cam + kCameraAspect) : 0.0f;
        if (!(now > 0.5f && now < 16.0f))  // 608/448 to 608/223, times up to 3
            rt::fatal(nullptr, "enhance: camera %d (%08X) has aspect %g: not a KartCam", i, cam, double(now));
        pokef(cam + kCameraAspect, now * change);
    }
}

}  // namespace

void set_aspect(float aspect, Hud hud) {
    g_wanted.store(std::clamp(aspect / kOriginal, 1.0f, 3.0f), std::memory_order_relaxed);  // 4:1 over 4:3 is 3
    hud::set_place(hud == Hud::edges ? hud::Place::edges : hud == Hud::middle ? hud::Place::middle : hud::Place::stretched);
    hud::set_widening_source(end_frame);
}

float shown_aspect() { return kOriginal * g_shown.load(std::memory_order_relaxed); }

bool in_race() { return rt::peek32(kRaceMgr) != 0; }
float widening() { return g_widening; }

float end_frame() {
    const bool race = in_race();
    const float drawn = race ? g_widening : 1.0f;
    g_shown.store(drawn, std::memory_order_relaxed);
    if (const float wanted = g_wanted.load(std::memory_order_relaxed); wanted != g_widening) {
        if (race) rescale_cameras(wanted / g_widening);
        g_widening = wanted;
    }
    return drawn;
}

// The game's own frame skipping, undone: LAN play with the fullest screens sets
// the display to draw on every second pass of the main loop (RaceApp::calc,
// 801CD9A4) to spare the console's GPU; its logic runs on every pass either
// way.
void draw_every_tick() {
    const uint32_t display = rt::peek32(kDisplay);
    if (display && rt::peek32(display + kDrawInterval) > 1) rt::poke32(display + kDrawInterval, 1);
}

std::span<const rt::Provided> replacements() { return kReplacements; }

}  // namespace mkdd::enhance
