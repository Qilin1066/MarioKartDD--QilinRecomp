#include "settings.hpp"

#include <cctype>
#include <fstream>
#include <initializer_list>
#include <set>
#include <span>
#include <sstream>
#include <system_error>

#include "qilin/host/pad.hpp"
#include "qilin/rt/file.hpp"
#include "qilin/rt/system.hpp"

namespace mkdd {
namespace host = qilin::host;

namespace {

constexpr const char* kControlNames[kControlCount] = {"a", "b", "x", "y", "z", "l", "r", "start", "up", "down", "left", "right",
                                                      "stick_up", "stick_down", "stick_left", "stick_right",
                                                      "c_up", "c_down", "c_left", "c_right"};
constexpr const char* kControlLabels[kControlCount] = {
    "A", "B", "X", "Y", "Z", "L (trigger)", "R (trigger)", "Start", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Stick up", "Stick down", "Stick left", "Stick right", "C-stick up", "C-stick down", "C-stick left", "C-stick right"};
struct PadName {
    const char* name;   // in the file
    const char* label;  // in the menu
    uint32_t bit;
};
constexpr PadName kPadNames[] = {
    {"Up", "Pad Up", host::kPadUp},       {"Down", "Pad Down", host::kPadDown},  {"Left", "Pad Left", host::kPadLeft},
    {"Right", "Pad Right", host::kPadRight}, {"A", "Pad A", host::kPadA},           {"B", "Pad B", host::kPadB},
    {"X", "Pad X", host::kPadX},          {"Y", "Pad Y", host::kPadY},           {"Start", "Pad Start", host::kPadStart},
    {"Back", "Pad Back", host::kPadBack}, {"Guide", "Pad Guide", host::kPadGuide},
    {"LB", "Pad LB", host::kPadLB},       {"RB", "Pad RB", host::kPadRB},        {"LT", "Pad LT", host::kPadLT},
    {"RT", "Pad RT", host::kPadRT},       {"L3", "Pad L3", host::kPadL3},        {"R3", "Pad R3", host::kPadR3},
    {"LSUp", "Left stick up", host::kPadLSUp},       {"LSDown", "Left stick down", host::kPadLSDown},
    {"LSLeft", "Left stick left", host::kPadLSLeft}, {"LSRight", "Left stick right", host::kPadLSRight},
    {"RSUp", "Right stick up", host::kPadRSUp},       {"RSDown", "Right stick down", host::kPadRSDown},
    {"RSLeft", "Right stick left", host::kPadRSLeft}, {"RSRight", "Right stick right", host::kPadRSRight},
};
constexpr const char* kPadList =
    "Up Down Left Right A B X Y Start Back Guide LB RB LT RT L3 R3 LSUp LSDown LSLeft LSRight RSUp RSDown RSLeft RSRight";

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool parse_binding(const std::string& text, Binding& b, std::string& why) {
    const std::string t = trim(text);
    const std::string l = lower(t);
    if (l == "mouse") {
        b = {Binding::kMouse, 0, {}};
        return true;
    }
    const size_t colon = t.find(':');
    if (colon == std::string::npos) {
        why = "'" + t + "' isn't an input: write pad:<input>, key:<key> or mouse";
        return false;
    }
    const std::string kind = lower(trim(t.substr(0, colon))), name = trim(t.substr(colon + 1));
    if (kind == "pad") {
        for (const PadName& p : kPadNames)
            if (lower(name) == lower(p.name)) {
                b = {Binding::kPad, p.bit, {}};
                return true;
            }
        why = "'" + name + "' isn't a pad input (" + kPadList + ")";
        return false;
    }
    if (kind == "key") {
        if (name.empty()) {
            why = "key: needs a key's name";
            return false;
        }
        if (key_reserved(name)) {
            why = "key:" + name + " is kept for the program (Escape and F1 open the settings menu, F3 the frame times, F9 "
                  "captures frames for a report, F11 is full screen)";
            return false;
        }
        b = {Binding::kKey, 0, name};
        return true;
    }
    why = "'" + kind + "' isn't a kind of input (pad, key, mouse)";
    return false;
}

Binding pad(uint32_t bit) { return {Binding::kPad, bit, {}}; }
Binding key(const char* name) { return {Binding::kKey, 0, name}; }

const char* scaling_name(host::Scaling s) {
    switch (s) {
        case host::Scaling::integer: return "integer";
        case host::Scaling::stretch: return "stretch";
        case host::Scaling::fit: break;
    }
    return "fit";
}
// In their enumerations' order.
constexpr const char* kFrameRates[] = {"original", "60", "display", "unlimited"};
constexpr const char* kHuds[] = {"edges", "middle", "stretched"};
constexpr const char* kOutputs[] = {"stereo", "surround", "device"};

}  // namespace

const char* control_name(Control c) { return kControlNames[c]; }
const char* control_label(Control c) { return kControlLabels[c]; }

const char* pad_button_name(uint32_t bit) {
    for (const PadName& p : kPadNames)
        if (p.bit == bit) return p.name;
    return nullptr;
}

bool key_reserved(const std::string& name) {
    const std::string l = lower(trim(name));
    return l == "escape" || l == "f1" || l == "f3" || l == "f9" || l == "f11";
}

std::string binding_text(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "mouse";
        case Binding::kKey: return "key:" + b.key;
        case Binding::kPad:
            if (const char* n = pad_button_name(b.pad)) return std::string("pad:") + n;
    }
    return "?";
}

std::string binding_label(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "Mouse";
        case Binding::kKey: return b.key;
        case Binding::kPad:
            for (const PadName& p : kPadNames)
                if (p.bit == b.pad) return p.label;
    }
    return "?";
}

// The pad by where its buttons are, not by their letters: the GameCube's A is
// the large button under the thumb (an Xbox pad's A), B below and to its left
// (X), X to its right (B), Y above (Y); Z is a shoulder button, L and R the
// triggers. The keyboard plays the first port.
void default_controls(Settings& s, unsigned player) {
    auto& c = s.players[player].controls;
    const bool keys = player == 0;
    const auto set = [&](Control which, std::initializer_list<uint32_t> pads, const char* name) {
        c[which].clear();
        for (uint32_t bit : pads) c[which].push_back(pad(bit));
        if (keys) c[which].push_back(key(name));
    };
    set(kCtlA, {host::kPadA}, "X");
    set(kCtlB, {host::kPadX}, "Z");
    set(kCtlX, {host::kPadB}, "C");
    set(kCtlY, {host::kPadY}, "S");
    set(kCtlZ, {host::kPadRB, host::kPadLB}, "D");
    set(kCtlL, {host::kPadLT}, "A");
    set(kCtlR, {host::kPadRT}, "Space");
    set(kCtlStart, {host::kPadStart}, "Return");
    set(kCtlUp, {host::kPadUp}, "T");
    set(kCtlDown, {host::kPadDown}, "G");
    set(kCtlLeft, {host::kPadLeft}, "F");
    set(kCtlRight, {host::kPadRight}, "H");
    set(kCtlStickUp, {host::kPadLSUp}, "Up");
    set(kCtlStickDown, {host::kPadLSDown}, "Down");
    set(kCtlStickLeft, {host::kPadLSLeft}, "Left");
    set(kCtlStickRight, {host::kPadLSRight}, "Right");
    set(kCtlCUp, {host::kPadRSUp}, "I");
    set(kCtlCDown, {host::kPadRSDown}, "K");
    set(kCtlCLeft, {host::kPadRSLeft}, "J");
    set(kCtlCRight, {host::kPadRSRight}, "L");
}

Settings default_settings() {
    Settings s;
    for (unsigned p = 0; p < kPlayers; ++p) default_controls(s, p);
    return s;
}

bool apply_preset(Settings& s, const std::string& name) {
    if (name == "original" || name == "low-end") {  // the console's picture: 1x, its filtering (a Pi 4 does exactly this)
        s.scale = 1, s.anisotropy = 1, s.vsync = true, s.smaa = false, s.fsr = false;
        s.true_color = name == "low-end";  // original: the console's 18-bit colour and its dither; low-end: 24 bits (no cost)
    } else if (name == "desktop") {  // a desktop GPU: the window's resolution, 16x anisotropic
        s.scale = 0, s.anisotropy = 16, s.vsync = true;
        s.true_color = true;
    } else {
        return false;
    }
    s.preset = name;
    return true;
}

host::Picture picture_of(const Settings& s) {
    host::Picture p;
    p.scaling = s.scaling;
    p.smooth = s.smooth;
    p.smaa = s.smaa;
    p.fsr = s.fsr;
    p.sharpness = float(s.sharpness) / 100.0f;
    p.look = s.look;
    p.scanlines = float(s.scanlines) / 100.0f;
    p.mask = float(s.mask) / 100.0f;
    p.vsync = s.vsync;
    p.low_latency = s.low_latency;
    p.match_refresh = s.match_refresh;
    p.variable_refresh = s.variable_refresh;
    return p;
}

float aspect_ratio(const Settings& s, float screen) {
    constexpr float original = 4.0f / 3.0f;
    if (s.aspect_w == 0 || s.aspect_h == 0) return screen > original ? (screen < 4.0f ? screen : 4.0f) : original;
    return float(s.aspect_w) / float(s.aspect_h);
}

unsigned render_scale(const Settings& s, int height_pixels) {
    if (s.scale) return s.scale < 8 ? s.scale : 8;
    if (height_pixels <= 0) return 3;
    const int k = (height_pixels + 479) / 480;
    return unsigned(k < 1 ? 1 : k > 8 ? 8 : k);
}

bool parse_settings(const std::string& text, Settings& out, std::string& error) {
    Settings s = default_settings();
    std::istringstream in(text);
    std::string line, section;
    std::set<std::string> seen;
    for (int n = 1; std::getline(in, line); ++n) {
        const auto fail = [&](const std::string& why) {
            error = "line " + std::to_string(n) + ": " + why;
            return false;
        };
        if (n == 1 && line.starts_with("\xEF\xBB\xBF")) line.erase(0, 3);
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return fail("a section is written [name]");
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section != "display" && section != "sound" && section != "controls" && section != "console" &&
                !(section.size() == 7 && section.starts_with("player") && section[6] >= '1' && section[6] <= '4'))
                return fail("there's no section [" + section + "] (display, sound, controls, player1 to player4, console)");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected name = value");
        const std::string name = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
        if (section.empty()) return fail("'" + name + "' comes before any [section]");
        if (!seen.insert(section + "." + name).second) return fail("'" + name + "' is set twice");
        const std::string v = lower(value);
        bool ok = true;
        const auto boolean = [&](bool& to) {
            if (v == "true") to = true;
            else if (v == "false") to = false;
            else ok = false;
            return ok ? true : fail(name + " is true or false");
        };
        const auto number = [&](unsigned& to, unsigned lo, unsigned hi) {
            unsigned x = 0;
            ok = !v.empty() && v.size() <= 4;
            for (char ch : v) ok = ok && std::isdigit(static_cast<unsigned char>(ch)), x = x * 10 + unsigned(ch - '0');
            if (!ok || x < lo || x > hi) return fail(name + " is a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
            to = x;
            return true;
        };
        // One of `names` (in the enumeration's order): its index.
        const auto choice = [&](std::span<const char* const> names, unsigned& to) {
            std::string list;
            for (size_t i = 0; i < names.size(); ++i) {
                if (v == names[i]) {
                    to = unsigned(i);
                    return true;
                }
                list += (i == 0 ? "" : i + 1 == names.size() ? " or " : ", ") + std::string(names[i]);
            }
            return fail(name + " is " + list);
        };
        if (section == "display") {
            unsigned pick = 0;
            if (name == "preset") {
                if (!apply_preset(s, v)) return fail("the presets are original, low-end and desktop");
            } else if (name == "fullscreen") {
                if (!boolean(s.fullscreen)) return false;
            } else if (name == "monitor") {
                s.monitor = value;
            } else if (name == "aspect") {
                if (v == "auto") {
                    s.aspect_w = s.aspect_h = 0;
                } else {
                    // W:H, whole numbers of up to four digits; from 4:3 (the game's own) to 4:1.
                    const size_t colon = v.find(':');
                    unsigned w = 0, h = 0;
                    bool good = colon != std::string::npos && colon >= 1 && colon <= 4 && v.size() >= colon + 2 && v.size() <= colon + 5;
                    for (size_t i = 0; good && i < v.size(); ++i) {
                        if (i == colon) continue;
                        good = std::isdigit(static_cast<unsigned char>(v[i])) != 0;
                        unsigned& part = i < colon ? w : h;
                        if (good) part = part * 10 + unsigned(v[i] - '0');
                    }
                    if (!good || w == 0 || h == 0 || w * 3 < h * 4 || w > h * 4)
                        return fail("aspect is auto or width:height, from 4:3 to 4:1 (16:10, 16:9, 21:9, 32:9)");
                    s.aspect_w = w, s.aspect_h = h;
                }
            } else if (name == "hud") {
                if (!choice(kHuds, pick)) return false;
                s.hud = Hud(pick);
            } else if (name == "scaling") {
                if (v == "integer") s.scaling = host::Scaling::integer;
                else if (v == "fit") s.scaling = host::Scaling::fit;
                else if (v == "stretch") s.scaling = host::Scaling::stretch;
                else return fail("scaling is integer, fit or stretch");
            } else if (name == "smooth") {
                if (!boolean(s.smooth)) return false;
            } else if (name == "smaa") {
                if (!boolean(s.smaa)) return false;
            } else if (name == "fsr") {
                if (!boolean(s.fsr)) return false;
            } else if (name == "sharpness") {
                if (!number(s.sharpness, 0, 100)) return false;
            } else if (name == "look") {
                if (v == "plain") s.look = host::Look::plain;
                else if (v == "crt") s.look = host::Look::crt;
                else return fail("look is plain or crt");
            } else if (name == "scanlines") {
                if (!number(s.scanlines, 0, 100)) return false;
            } else if (name == "mask") {
                if (!number(s.mask, 0, 100)) return false;
            } else if (name == "render_resolution") {
                if (v == "auto" || v == "0") s.scale = 0;
                else if (!number(s.scale, 1, 8)) return false;
            } else if (name == "anisotropy") {
                if (!number(s.anisotropy, 1, 16)) return false;
                if (s.anisotropy & (s.anisotropy - 1)) return fail("anisotropy is 1, 2, 4, 8 or 16");
            } else if (name == "true_color") {
                if (!boolean(s.true_color)) return false;
            } else if (name == "vsync") {
                if (!boolean(s.vsync)) return false;
            } else if (name == "frame_rate") {
                if (!choice(kFrameRates, pick)) return false;
                s.frame_rate = FrameRate(pick);
            } else if (name == "low_latency") {
                if (!boolean(s.low_latency)) return false;
            } else if (name == "gpu") {
                s.gpu = value;
            } else if (name == "performance_overlay") {
                if (!boolean(s.performance_overlay)) return false;
            } else if (name == "match_refresh") {
                if (!boolean(s.match_refresh)) return false;
            } else if (name == "variable_refresh") {
                if (!boolean(s.variable_refresh)) return false;
            } else {
                return fail("[display] has no '" + name +
                            "' (preset fullscreen monitor aspect hud scaling smooth smaa fsr sharpness look scanlines mask "
                            "render_resolution anisotropy true_color vsync low_latency frame_rate gpu performance_overlay "
                            "match_refresh variable_refresh)");
            }
            continue;
        }
        if (section == "sound") {
            unsigned pick = 0;
            if (name == "volume") {
                if (!number(s.volume, 0, 100)) return false;
            } else if (name == "mute_unfocused") {
                if (!boolean(s.mute_unfocused)) return false;
            } else if (name == "output") {
                if (!choice(kOutputs, pick)) return false;
                s.sound_output = host::SoundOutput(pick);
            } else if (name == "device") {
                s.sound_device = value;
            } else {
                return fail("[sound] has no '" + name + "' (volume mute_unfocused output device)");
            }
            continue;
        }
        if (section == "console") {
            if (name == "language") {
                if (v != "english") return fail("language is english (the only one on the USA disc)");
                s.language = 0;
            } else if (name == "sound") {
                if (v == "mono") s.sound = 0;
                else if (v == "stereo") s.sound = 1;
                else return fail("sound is mono or stereo");
            } else if (name == "progressive") {
                if (!boolean(s.progressive)) return false;
            } else {
                return fail("[console] has no '" + name + "' (language sound progressive)");
            }
            continue;
        }
        if (section == "controls") {
            if (name == "stick_deadzone") {
                if (!number(s.stick_deadzone, 0, 50)) return false;
            } else if (name == "stick_threshold") {
                if (!number(s.stick_threshold, 5, 95)) return false;
            } else if (name == "trigger_threshold") {
                if (!number(s.trigger_threshold, 5, 95)) return false;
            } else if (name == "rumble_strength") {
                if (!number(s.rumble_strength, 0, 100)) return false;
            } else {
                return fail("[controls] has no '" + name +
                            "' (stick_deadzone stick_threshold trigger_threshold rumble_strength; a player's controls are in "
                            "[player1] to [player4])");
            }
            continue;
        }
        // [player1] to [player4]
        Player& p = s.players[section[6] - '1'];
        if (name == "controller") {
            p.controller = v == "none" ? "none" : value;
            continue;
        }
        if (name == "rumble") {
            if (!boolean(p.rumble)) return false;
            continue;
        }
        unsigned c = 0;
        while (c < kControlCount && name != kControlNames[c]) ++c;
        if (c == kControlCount)
            return fail("'" + name + "' isn't one of a GameCube controller's controls (a b x y z l r start up down left right "
                        "stick_up stick_down stick_left stick_right c_up c_down c_left c_right), controller or rumble");
        p.controls[c].clear();
        std::istringstream items(value);
        for (std::string item; std::getline(items, item, ',');) {
            if (trim(item).empty()) continue;
            Binding b;
            std::string why;
            if (!parse_binding(item, b, why)) return fail(why);
            p.controls[c].push_back(b);
        }
    }
    for (unsigned a = 0; a < kPlayers; ++a)
        for (unsigned b = a + 1; b < kPlayers; ++b)
            if (!s.players[a].controller.empty() && s.players[a].controller != "none" &&
                s.players[a].controller == s.players[b].controller) {
                error = "players " + std::to_string(a + 1) + " and " + std::to_string(b + 1) + " cannot use the same explicit controller";
                return false;
            }
    error.clear();
    out = std::move(s);
    return true;
}

std::string settings_text(const Settings& s) {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::string t =
        "# Mario Kart: Double Dash!! QilinRecomp settings. The settings menu (F1 or Esc in the game) changes these and saves\n"
        "# them here; any text editor works too. Delete the file to get the defaults back.\n"
        "# Each line is name = value; # starts a comment.\n"
        "\n"
        "[display]\n"
        "# A preset sets the enhancements below it: original (the console's picture), low-end (a Raspberry Pi\n"
        "# 4-class board), desktop (the window's resolution, 16x anisotropic filtering). Keys after it override it.\n"
        "preset = " + s.preset + "\n"
        "# Borderless full screen (F11 or Alt+Enter switch it), and on which display (its name; empty: the main one).\n"
        "fullscreen = " + b(s.fullscreen) + "\n"
        "monitor = " + s.monitor + "\n"
        "# The picture's shape, width:height: 4:3 is the original; a wider one (16:10, 16:9, 21:9, 32:9, any up to\n"
        "# 4:1) shows more at the sides; auto is the screen's own. hud: where the game's HUD goes on a wider\n"
        "# picture: edges, middle (the middle 4:3) or stretched. Not yet: both need the game's cameras and HUD\n"
        "# patched, and until that is done the picture is 4:3 whatever is set here.\n"
        "aspect = " + (s.aspect_w && s.aspect_h ? std::to_string(s.aspect_w) + ":" + std::to_string(s.aspect_h) : std::string("auto")) + "\n"
        "hud = " + kHuds[unsigned(s.hud)] + "\n"
        "# The picture's size: integer (whole multiples of the console's, the sharpest), fit (the largest that\n"
        "# keeps its shape) or stretch (the whole window). smooth: filtered; false: nearest pixel.\n"
        "scaling = " + scaling_name(s.scaling) + "\n"
        "smooth = " + b(s.smooth) + "\n"
        "# smaa: SMAA anti-aliasing (smooths jagged edges). fsr: AMD FSR 1 upscaling where the picture is shown\n"
        "# larger than it's drawn (not at whole multiples), with its sharpening, 0 to 100.\n"
        "smaa = " + b(s.smaa) + "\n"
        "fsr = " + b(s.fsr) + "\n"
        "sharpness = " + std::to_string(s.sharpness) + "\n"
        "# Its look: plain, or crt (a CRT television: raster lines and phosphor stripes). For crt, how dark the\n"
        "# gaps between lines are and how strong the stripes, 0 to 100.\n"
        "look = " + (s.look == host::Look::crt ? "crt" : "plain") + "\n"
        "scanlines = " + std::to_string(s.scanlines) + "\n"
        "mask = " + std::to_string(s.mask) + "\n"
        "# The resolution the game is drawn at, as a multiple of the GameCube's 640x480: 1 to 8, or auto (the\n"
        "# smallest that fills the window line for line, taken when the game starts). Higher is sharper and\n"
        "# needs a faster GPU: the work grows with the square (8x is 64 times 1x's pixels). MKDD_SCALE in the\n"
        "# environment overrides it.\n"
        "render_resolution = " + (s.scale ? std::to_string(s.scale) : std::string("auto")) + "\n"
        "# Texture filtering at an angle: 1 (the console's), 2, 4, 8 or 16.\n"
        "anisotropy = " + std::to_string(s.anisotropy) + "\n"
        "# Colour depth: true, 24-bit colour (smooth gradients); false, the console's 18 bits with its dither\n"
        "# pattern where the game draws that way.\n"
        "true_color = " + b(s.true_color) + "\n"
        "# vsync: true, no tearing; false, the lowest latency. low_latency: with vsync, at most one frame\n"
        "# waiting for the display.\n"
        "vsync = " + b(s.vsync) + "\n"
        "low_latency = " + b(s.low_latency) + "\n"
        "# Frames a second: original (the game's own: 60 with one or two players, 30 with three or four), 60,\n"
        "# display (the display's refresh rate) or unlimited. Not yet: above the game's own needs the game\n"
        "# patched, and until that is done it runs as original whatever is set here.\n"
        "frame_rate = " + kFrameRates[unsigned(s.frame_rate)] + "\n"
        "# The graphics device, by name (empty: the best one).\n"
        "gpu = " + s.gpu + "\n"
        "# Frame rate, 1% and 0.1% lows and recent frame times in a corner (F3 switches it).\n"
        "performance_overlay = " + b(s.performance_overlay) + "\n"
        "# Fullscreen on a display whose refresh isn't a multiple of 60 Hz (75, 144, 165 Hz): switch to the\n"
        "# highest refresh at the same resolution that is (120 Hz on a 144 Hz monitor), so frames are even.\n"
        "match_refresh = " + b(s.match_refresh) + "\n"
        "# A G-Sync, FreeSync or VRR display with it on in its driver: no switch and no locking to the\n"
        "# refresh; the display follows the game's own 59.94 frames a second.\n"
        "variable_refresh = " + b(s.variable_refresh) + "\n"
        "\n"
        "[sound]\n"
        "# Playback volume, 0 to 100, and optional muting while another window has the focus.\n"
        "volume = " + std::to_string(s.volume) + "\n"
        "mute_unfocused = " + b(s.mute_unfocused) + "\n"
        "# output, from the next start: stereo; surround (5.1 from the game's Dolby Surround signal: choose\n"
        "# Surround in the game's own sound options too); or device (surround where the device has six\n"
        "# speakers or more, else stereo). device: the playback device by its name (empty: the system's\n"
        "# default, followed when it changes).\n"
        "output = " + kOutputs[unsigned(s.sound_output)] + "\n"
        "device = " + s.sound_device + "\n"
        "\n"
        "[controls]\n"
        "# stick_deadzone: how much of a stick's travel from its centre reads as the centre, 0-50 percent.\n"
        "# stick_threshold: where a stick's direction counts as pressed when it's bound to a button, 5-95.\n"
        "# trigger_threshold: where L and R click (the press at the end of the GameCube's triggers), 5-95 percent\n"
        "# of a trigger's travel. Releases are 5 points lower to resist jitter. rumble_strength: 0-100.\n"
        "stick_deadzone = " + std::to_string(s.stick_deadzone) + "\n"
        "stick_threshold = " + std::to_string(s.stick_threshold) + "\n"
        "trigger_threshold = " + std::to_string(s.trigger_threshold) + "\n"
        "rumble_strength = " + std::to_string(s.rumble_strength) + "\n"
        "\n"
        "# The four controller ports. controller: which controller plays the port: empty, automatic (the\n"
        "# controllers take the ports in the order each first has a button pressed); a device's identity, as\n"
        "# the settings menu writes it; or none. rumble: the controller's rumble, true or false.\n"
        "# Each of the GameCube controller's controls takes a list of inputs, separated by commas; an empty\n"
        "# list leaves it unused. The settings menu's Controls page sets them by pressing them.\n"
        "#   pad:<input>   the port's controller, by an Xbox pad's names: A B X Y (by position: bottom, right,\n"
        "#                 left, top) Start Back Guide LB RB LT RT L3 R3, the D-pad Up Down Left Right, the left\n"
        "#                 stick LSUp LSDown LSLeft LSRight and the right stick RSUp RSDown RSLeft RSRight\n"
        "#   key:<name>    a key, as SDL names it: A-Z, 0-9, Up, Down, Left, Right, Space, Return, Backspace,\n"
        "#                 Tab, Left Shift, Right Ctrl, F2 ... (not Escape, F1, F3, F9 or F11: the program's)\n"
        "#   mouse         the mouse's left button\n"
        "# A stick or a trigger bound to the stick's directions, the C-stick's or L and R is analogue; a key or\n"
        "# a button there presses all the way. A port with keys is always connected; the others while their\n"
        "# controller is.\n";
    for (unsigned p = 0; p < kPlayers; ++p) {
        const Player& pl = s.players[p];
        t += "\n[player" + std::to_string(p + 1) + "]\n";
        t += "controller = " + pl.controller + "\n";
        t += std::string("rumble = ") + b(pl.rumble) + "\n";
        for (unsigned c = 0; c < kControlCount; ++c) {
            t += kControlNames[c];
            t += " =";
            for (size_t i = 0; i < pl.controls[c].size(); ++i) t += (i ? ", " : " ") + binding_text(pl.controls[c][i]);
            t += "\n";
        }
    }
    t += "\n"
         "[console]\n"
         "# What the game reads as the GameCube's own settings, from the next start.\n"
         "# language: english (the only one on the USA disc).\n"
         "language = english\n"
         "# sound: mono or stereo (the game's own sound options add Surround).\n"
         "sound = " + std::string(s.sound == 0 ? "mono" : "stereo") + "\n"
         "# progressive: true, progressive scan (480p); false, interlaced, as on a television.\n"
         "progressive = " + b(s.progressive) + "\n";
    return t;
}

std::filesystem::path settings_file(const std::filesystem::path& folder) { return folder / "mkdd.ini"; }

bool save_settings(const std::filesystem::path& folder, const Settings& s) {
    const std::filesystem::path file = settings_file(folder), tmp = file.string() + ".tmp";
    // Flush and close before replacement; either failure leaves the old file intact.
#if defined(_WIN32)
    FILE* out = _wfopen(tmp.c_str(), L"wb");
#else
    FILE* out = std::fopen(tmp.c_str(), "wb");
#endif
    if (!out) return false;
    const std::string text = settings_text(s);
    bool ok = std::fwrite(text.data(), 1, text.size(), out) == text.size();
    ok = qilin::rt::sync_file(out) && ok;
    ok = std::fclose(out) == 0 && ok;
    std::error_code ec;
    if (ok) {
        std::filesystem::rename(tmp, file, ec);
        ok = !ec;
    }
    if (!ok) std::filesystem::remove(tmp, ec);
    return ok;
}

Settings load_settings(const std::filesystem::path& folder) {
    const std::filesystem::path file = settings_file(folder);
    std::string text;
    bool read = false;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) {
            std::stringstream buf;
            buf << in.rdbuf();
            text = buf.str();
            read = !in.bad() && !buf.bad();
        }
    }
    if (read) {
        Settings s;
        std::string error;
        if (!parse_settings(text, s, error))
            qilin::rt::fatal(nullptr, "%s, %s (fix it, or delete the file to get the defaults back)", file.string().c_str(),
                             error.c_str());
        qilin::rt::log("settings: read %s", file.string().c_str());
        return s;
    }
    std::error_code read_ec;
    if (std::filesystem::exists(file, read_ec) || read_ec)
        qilin::rt::fatal(nullptr, "cannot read %s (the existing settings have been kept)", file.string().c_str());
    const Settings s = default_settings();
    if (!save_settings(folder, s)) qilin::rt::fatal(nullptr, "cannot write %s", file.string().c_str());
    qilin::rt::log("settings: wrote the defaults to %s", file.string().c_str());
    return s;
}

}  // namespace mkdd
