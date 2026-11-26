// The settings file: mkdd.ini in the game's data folder (host/paths.hpp),
// as the other Qilin recomps keep theirs: [display], [sound], [controls],
// [player1] to [player4] and [console] sections, written with every key and
// its comments the first time the game runs. The settings menu (F1 or Esc in
// the game) changes them and saves them back; the file can be edited by hand
// too. A preset sets the enhancements' defaults and any key after it
// overrides one. A line that can't be read stops the program, naming the
// line, rather than being ignored.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "qilin/host/audio.hpp"
#include "qilin/host/picture.hpp"

namespace mkdd {

inline constexpr unsigned kPlayers = 4;  // the GameCube's controller ports

// The GameCube controller's controls, in the menu's order. L and R are the
// analogue triggers (with the click at their end); the stick and C-stick
// directions are analogue too. A key or a button on one presses it fully.
enum Control : unsigned {
    kCtlA, kCtlB, kCtlX, kCtlY, kCtlZ, kCtlL, kCtlR, kCtlStart,
    kCtlUp, kCtlDown, kCtlLeft, kCtlRight,                      // the D-pad
    kCtlStickUp, kCtlStickDown, kCtlStickLeft, kCtlStickRight,  // the main stick
    kCtlCUp, kCtlCDown, kCtlCLeft, kCtlCRight,                  // the C-stick
    kControlCount
};
const char* control_name(Control c);   // as the file names it: "stick_up"
const char* control_label(Control c);  // as the menu shows it: "Stick up"

// One input that presses a control.
struct Binding {
    enum Kind { kPad, kKey, kMouse } kind = kPad;
    uint32_t pad = 0;  // kPad: a qilin::host::PadButton bit (a button, a trigger or a stick's direction)
    std::string key;   // kKey: the key's name, as SDL names keys ("X", "Space", "Left")
    bool operator==(const Binding& o) const { return kind == o.kind && pad == o.pad && key == o.key; }
};
// As the file writes it ("pad:A", "key:Space", "mouse"), and as the menu
// shows it ("Pad A", "Space", "Mouse").
std::string binding_text(const Binding& b);
std::string binding_label(const Binding& b);
// The pad inputs that can be bound, by the file's names.
const char* pad_button_name(uint32_t bit);  // null if it isn't one
// Keys kept for the program: Escape and F1 (the menu), F3 (frame times), F9
// (a capture for a report), F11 (full screen).
bool key_reserved(const std::string& name);

// One controller port.
struct Player {
    // The controller that plays it: empty, automatic (the controllers in the
    // order each first had a button pressed); a device's identity, that one;
    // "none", no controller (the port's keys still play it).
    std::string controller;
    bool rumble = true;
    std::vector<Binding> controls[kControlCount];
};

// Frames a second: the game's own (60; LAN play draws every second one in its
// fullest games), 60 with every frame drawn, the display's refresh rate, or
// as many as the computer draws. Above 60 needs pictures drawn between the
// game's own (enhance.hpp, docs/enhancements.md).
enum class FrameRate : uint8_t { original, sixty, display, unlimited };
// The HUD where the picture is wider than 4:3: at the screen's edges, in the
// middle 4:3, or stretched with the picture.
enum class Hud : uint8_t { edges, middle, stretched };

struct Settings {
    // [display]
    std::string preset = "original";  // original, low-end, desktop
    bool fullscreen = false;          // borderless, at the desktop's mode (F11 or Alt+Enter switch it)
    std::string monitor;              // the display's name (empty: the main one)
    // The picture's shape, width:height: 4:3 the original, wider ones show more
    // at the sides (up to 4:1); 0:0 is "auto", the screen's own shape. Wider
    // than 4:3 needs the game patched (enhance.hpp): until then it's 4:3.
    unsigned aspect_w = 0, aspect_h = 0;
    Hud hud = Hud::edges;
    qilin::host::Scaling scaling = qilin::host::Scaling::fit;
    bool smooth = true;               // bilinear; off: nearest pixel
    bool smaa = false;                // SMAA 1x anti-aliasing
    bool fsr = false;                 // AMD FSR 1 where the picture is shown larger than it's drawn
    unsigned sharpness = 80;          // FSR's sharpening, 0-100
    qilin::host::Look look = qilin::host::Look::plain;
    unsigned scanlines = 60;          // CRT: the gaps between raster lines, 0-100 (percent dark)
    unsigned mask = 30;               // CRT: the phosphor stripes, 0-100
    unsigned scale = 0;               // render_resolution: 1-8x the console's 640x480; 0: auto (follows the window's size, main.cpp)
    unsigned anisotropy = 1;          // texture filtering: 1 (the console's), 2, 4, 8 or 16
    bool true_color = true;           // 24-bit colour; false: the console's 18 bits, dithered (the "original" preset)
    bool vsync = true;                // at the display's refresh (no tearing); off: the lowest latency
    FrameRate frame_rate = FrameRate::display;
    bool low_latency = true;          // with vsync: at most one frame waiting for the display
    std::string gpu;                  // the graphics device's name (empty: the best one)
    bool performance_overlay = false; // frame rate, 1% and 0.1% lows and frame times in a corner (F3 switches it)
    bool match_refresh = true;        // fullscreen: a refresh that's a multiple of 60 Hz when the desktop's isn't
    bool variable_refresh = false;    // a G-Sync/FreeSync/VRR display: no mode switch, no locking to the refresh
    // [sound]
    unsigned volume = 100;            // 0-100
    bool mute_unfocused = false;
    qilin::host::SoundOutput sound_output = qilin::host::SoundOutput::stereo;
    std::string sound_device;         // the playback device by its name; empty: the system's default
    // [controls]
    unsigned stick_deadzone = 15;     // both sticks: this much of their travel from the centre reads as the centre, 0-50
    unsigned stick_threshold = 50;    // a stick's direction bound to a button presses it past this, 5-95
    unsigned trigger_threshold = 50;  // L and R click (their digital press) past this much of a trigger's travel, 5-95
    unsigned rumble_strength = 100;   // 0-100
    Player players[kPlayers];         // [player1] to [player4]
    // [console]: what the game reads as the GameCube's own settings (its SRAM). The os layer
    // answers OSGetLanguage, OSGetSoundMode, OSGetProgressiveMode and OSGetEuRgb60Mode from these.
    unsigned language = 0;            // OS_LANG_*: 0 English, the only one the USA disc has (files/MRAM_Locale/English)
    unsigned sound = 1;               // OSGetSoundMode: 0 mono, 1 stereo
    bool progressive = true;          // OSGetProgressiveMode: 480p rather than 480i
    bool eu_rgb60 = false;            // OSGetEuRgb60Mode: PAL consoles only; never set for this disc, and not in the file
};

Settings default_settings();
// One player's controls back to the defaults (docs/runtime.md): the pad's for
// every player, the keyboard's for the first.
void default_controls(Settings& s, unsigned player);
// Sets a preset's enhancements (and its name); false for an unknown one.
bool apply_preset(Settings& s, const std::string& name);
// The picture as the display takes it.
qilin::host::Picture picture_of(const Settings& s);
// The shape asked for: width over height, or `screen` (the screen's own, at
// least 4:3) for auto.
float aspect_ratio(const Settings& s, float screen);
// The render resolution for a window `height_pixels` high: the setting, or
// for auto the smallest whole multiple whose 480 lines fill it (1-8).
unsigned render_scale(const Settings& s, int height_pixels);

// Read settings from text. On a problem: false, with `error` saying what
// and where ("line 12: ...").
bool parse_settings(const std::string& text, Settings& out, std::string& error);

// The file's text for the given settings, with comments explaining each part.
std::string settings_text(const Settings& s);

// mkdd.ini in `folder`: read it, or write the defaults there if it's
// missing. A file that doesn't read stops the program.
Settings load_settings(const std::filesystem::path& folder);
// Write it back (through a temporary file, so a failure leaves the old one);
// false if it can't be written.
bool save_settings(const std::filesystem::path& folder, const Settings& s);
std::filesystem::path settings_file(const std::filesystem::path& folder);

}  // namespace mkdd
