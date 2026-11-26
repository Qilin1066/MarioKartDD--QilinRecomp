// The settings file (mkdd/src/settings.cpp): what it writes reads back
// the same, edits are read, and mistakes are reported with their line.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "settings.hpp"
#include "qilin/host/pad.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool same(const mkdd::Settings& a, const mkdd::Settings& b) {
    if (a.preset != b.preset || a.fullscreen != b.fullscreen || a.monitor != b.monitor || a.aspect_w != b.aspect_w ||
        a.aspect_h != b.aspect_h || a.hud != b.hud || a.scaling != b.scaling || a.smooth != b.smooth || a.smaa != b.smaa ||
        a.fsr != b.fsr || a.sharpness != b.sharpness || a.look != b.look || a.scanlines != b.scanlines || a.mask != b.mask ||
        a.scale != b.scale || a.anisotropy != b.anisotropy || a.true_color != b.true_color || a.vsync != b.vsync ||
        a.frame_rate != b.frame_rate || a.low_latency != b.low_latency || a.gpu != b.gpu ||
        a.performance_overlay != b.performance_overlay || a.match_refresh != b.match_refresh ||
        a.variable_refresh != b.variable_refresh || a.volume != b.volume || a.mute_unfocused != b.mute_unfocused ||
        a.sound_output != b.sound_output || a.sound_device != b.sound_device || a.stick_deadzone != b.stick_deadzone ||
        a.stick_threshold != b.stick_threshold || a.trigger_threshold != b.trigger_threshold ||
        a.rumble_strength != b.rumble_strength || a.language != b.language || a.sound != b.sound || a.progressive != b.progressive ||
        a.eu_rgb60 != b.eu_rgb60 || a.component_cable != b.component_cable)
        return false;
    for (unsigned p = 0; p < mkdd::kPlayers; ++p) {
        if (a.players[p].controller != b.players[p].controller || a.players[p].rumble != b.players[p].rumble) return false;
        for (unsigned c = 0; c < mkdd::kControlCount; ++c)
            if (a.players[p].controls[c] != b.players[p].controls[c]) return false;
    }
    return true;
}

// The error a text gives, or "" if it reads.
std::string error_of(const std::string& text) {
    mkdd::Settings s;
    std::string e;
    return mkdd::parse_settings(text, s, e) ? "" : e;
}
bool error_has(const std::string& text, const char* part) { return error_of(text).find(part) != std::string::npos; }

}  // namespace

int main() {
    using mkdd::Binding;
    namespace host = qilin::host;
    const mkdd::Settings d = mkdd::default_settings();

    {
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings(mkdd::settings_text(d), s, e) && same(s, d), "the defaults read back as written");
    }
    {
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("", s, e) && same(s, d), "an empty file means the defaults");
    }
    {
        // The defaults themselves (docs/runtime.md's tables).
        const auto& one = d.players[0].controls;
        const auto& four = d.players[3].controls;
        check(one[mkdd::kCtlA].size() == 2 && one[mkdd::kCtlA][0] == Binding{Binding::kPad, host::kPadA, {}} &&
                  one[mkdd::kCtlA][1] == Binding{Binding::kKey, 0, "X"},
              "player one's A: pad A and the X key");
        check(one[mkdd::kCtlB][0].pad == host::kPadX && one[mkdd::kCtlX][0].pad == host::kPadB && one[mkdd::kCtlY][0].pad == host::kPadY,
              "the face buttons by position: pad X is B, pad B is X");
        check(one[mkdd::kCtlZ].size() == 3 && one[mkdd::kCtlL][0].pad == host::kPadLT && one[mkdd::kCtlR][0].pad == host::kPadRT,
              "Z on both shoulder buttons, L and R on the triggers");
        check(one[mkdd::kCtlStickUp][0].pad == host::kPadLSUp && one[mkdd::kCtlCLeft][0].pad == host::kPadRSLeft &&
                  one[mkdd::kCtlUp][0].pad == host::kPadUp,
              "the stick, the C-stick and the D-pad each on their own");
        bool keys = false;
        for (unsigned c = 0; c < mkdd::kControlCount; ++c)
            for (const Binding& b : four[c]) keys = keys || b.kind != Binding::kPad;
        check(!keys && four[mkdd::kCtlA].size() == 1 && four[mkdd::kCtlA][0].pad == host::kPadA,
              "the other players have the pad's defaults and no keys");
        check(d.language == 0 && d.sound == 1 && d.progressive && !d.eu_rgb60, "the console's defaults: English, stereo, progressive");
        check(d.component_cable, "a component cable by default (the game offers progressive scan)");
        check(d.frame_rate == mkdd::FrameRate::display && d.aspect_w == 0 && d.hud == mkdd::Hud::edges && d.low_latency && !d.smaa && !d.fsr,
              "the display's defaults");
    }
    {
        mkdd::Settings s;
        std::string e;
        const bool ok = mkdd::parse_settings(
            "[Display]\n"
            "render_resolution = 3   # sharper\n"
            "\n"
            "[Player2]\n"
            "  Z = pad:Y , KEY:Right Shift,mouse\n"
            "start =\n"
            "stick_up = pad:lsup, key:W\n",
            s, e);
        check(ok && s.scale == 3, "render_resolution is read, sections and names in any case");
        const auto& c = s.players[1].controls;
        check(ok && c[mkdd::kCtlZ].size() == 3 && c[mkdd::kCtlZ][0].pad == host::kPadY && c[mkdd::kCtlZ][1].kind == Binding::kKey &&
                  c[mkdd::kCtlZ][1].key == "Right Shift" && c[mkdd::kCtlZ][2].kind == Binding::kMouse,
              "a control takes pads, keys with spaces in their names, and the mouse");
        check(ok && c[mkdd::kCtlStart].empty(), "an empty list leaves a control unused");
        check(ok && c[mkdd::kCtlStickUp].size() == 2 && c[mkdd::kCtlStickUp][0].pad == host::kPadLSUp, "a stick's direction by name");
        mkdd::Settings untouched = s;
        untouched.scale = d.scale;
        for (unsigned k : {unsigned(mkdd::kCtlZ), unsigned(mkdd::kCtlStart), unsigned(mkdd::kCtlStickUp)})
            untouched.players[1].controls[k] = d.players[1].controls[k];
        check(ok && same(untouched, d), "controls not mentioned, and the other players, keep their defaults");
    }
    check(error_of("[player1]\njab = pad:A\n").rfind("line 2: 'jab' isn't one of a GameCube controller's controls", 0) == 0,
          "an unknown control is reported with its line");
    check(error_of("[player1]\na = pad:Q\n").rfind("line 2: 'Q' isn't a pad input", 0) == 0, "an unknown pad input");
    check(error_of("[player1]\na = Z\n").rfind("line 2: 'Z' isn't an input", 0) == 0, "an input without its kind");
    check(error_of("[player1]\na = wheel:3\n").rfind("line 2: 'wheel' isn't a kind of input", 0) == 0, "an unknown kind of input");
    check(error_has("[player1]\na = key:\n", "needs a key's name"), "a key without a name");
    check(error_has("[player5]\na = pad:A\n", "there's no section [player5]") && error_has("[player0]\n", "no section") &&
              error_has("[player12]\n", "no section") && error_has("[players]\n", "no section"),
          "four players, no more");
    check(error_has("[controls]\na = pad:A\n", "[controls] has no 'a'"), "a control outside a player's section says where they go");
    for (const char* k : {"f1", "escape", "F3", "F9", "f11"})
        check(error_has(std::string("[player1]\nstart = key:") + k + "\n", "kept for the program"), "a key of the program's can't be bound");
    check(error_of("[player1]\na = pad:A\na = pad:B\n").rfind("line 3: 'a' is set twice", 0) == 0, "a control set twice");
    check(error_of("[player1]\na = pad:A\n[player2]\na = pad:A\n").empty(), "the same control in two players' sections is two settings");
    check(error_of("[player1]\r\na = pad:A\r\n").empty(), "Windows line endings");
    check(error_has("[player1]\ncontroller = same\n[player3]\ncontroller = same\n", "players 1 and 3 cannot use the same explicit controller"),
          "one device cannot be assigned to two players");
    {
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("[player1]\ncontroller = None\n[player2]\ncontroller = none\n[player3]\ncontroller =\nrumble = false\n", s, e) &&
                  s.players[0].controller == "none" && s.players[1].controller == "none" && s.players[2].controller.empty() &&
                  !s.players[2].rumble && s.players[0].rumble,
              "no controller for two players isn't a clash; automatic; rumble per player");
    }
    check(error_has("[player2]\nrumble = maybe\n", "rumble is true or false"), "a boolean that isn't");

    // [display]
    check(error_of("[display]\nrender_resolution = 9\n").rfind("line 2: render_resolution is a whole number from 1 to 8", 0) == 0,
          "a resolution out of range");
    check(error_has("[display]\nrender_resolution = 2x\n", "whole number") && error_has("[display]\nrender_resolution = -1\n", "whole number") &&
              error_has("[display]\nrender_resolution =\n", "whole number") && error_has("[display]\nrender_resolution = 99999\n", "whole number"),
          "a resolution that isn't a number");
    {
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("[display]\nrender_resolution = auto\nmatch_refresh = false\nvariable_refresh = true\n", s, e) &&
                  s.scale == 0 && !s.match_refresh && s.variable_refresh,
              "auto resolution and the refresh switches");
        check(mkdd::render_scale(s, 480) == 1 && mkdd::render_scale(s, 481) == 2 && mkdd::render_scale(s, 1080) == 3 &&
                  mkdd::render_scale(s, 2160) == 5 && mkdd::render_scale(s, 9000) == 8 && mkdd::render_scale(s, 0) == 3,
              "auto: the smallest multiple of 480 lines that fills the window, 1 to 8");
        s.scale = 4;
        check(mkdd::render_scale(s, 480) == 4, "a set resolution is itself");
    }
    {
        mkdd::Settings s;
        std::string e;
        const bool ok = mkdd::parse_settings("[display]\npreset = desktop\nanisotropy = 4\n", s, e);
        check(ok && s.scale == 0 && s.anisotropy == 4 && s.preset == "desktop", "a preset, then a key that overrides it");
        check(!mkdd::parse_settings("[display]\npreset = huge\n", s, e) && e.find("presets are") != std::string::npos,
              "an unknown preset");
        check(error_has("[display]\nanisotropy = 3\n", "1, 2, 4, 8 or 16") && error_has("[display]\nanisotropy = 32\n", "from 1 to 16"),
              "anisotropy is a power of two up to 16");
        mkdd::Settings o, l, t;
        std::string eo;
        check(mkdd::parse_settings("[display]\npreset = original\n", o, eo) && !o.true_color && o.scale == 1 &&
                  mkdd::parse_settings("[display]\npreset = low-end\n", l, eo) && l.true_color &&
                  mkdd::parse_settings("[display]\npreset = original\ntrue_color = true\n", t, eo) && t.true_color && s.true_color,
              "true colour: off in the original preset alone, and a key of its own");
        check(mkdd::parse_settings("[display]\nsmaa = true\nfsr = true\npreset = original\n", o, eo) && !o.smaa && !o.fsr &&
                  mkdd::parse_settings("[display]\npreset = original\nsmaa = true\n", o, eo) && o.smaa,
              "the original preset turns SMAA and FSR off; a key after it turns one on");
    }
    {
        // The picture's shape.
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("[display]\naspect = 16:9\nhud = middle\n", s, e) && s.aspect_w == 16 && s.aspect_h == 9 &&
                  s.hud == mkdd::Hud::middle,
              "an aspect ratio and the HUD's place");
        check(mkdd::aspect_ratio(s, 2.0f) > 1.777f && mkdd::aspect_ratio(s, 2.0f) < 1.778f, "a set shape is itself");
        check(mkdd::parse_settings("[display]\naspect = AUTO\n", s, e) && s.aspect_w == 0 && s.aspect_h == 0, "auto");
        check(mkdd::aspect_ratio(s, 16.0f / 9.0f) > 1.777f && mkdd::aspect_ratio(s, 1.25f) > 1.333f && mkdd::aspect_ratio(s, 1.25f) < 1.334f &&
                  mkdd::aspect_ratio(s, 6.0f) == 4.0f,
              "auto is the screen's shape, no narrower than 4:3 and no wider than 4:1");
        check(mkdd::parse_settings("[display]\naspect = 4:3\n", s, e) && mkdd::parse_settings("[display]\naspect = 64:27\n", s, e) &&
                  mkdd::parse_settings("[display]\naspect = 32:9\n", s, e) && mkdd::parse_settings("[display]\naspect = 4:1\n", s, e) &&
                  mkdd::parse_settings("[display]\naspect = 2560:1080\n", s, e),
              "4:3 to 4:1, by any whole numbers");
        for (const char* bad : {"5:4", "1:1", "9:16", "5:1", "16:0", "0:9", "16", "16:", ":9", "16:9:1", "16x9", "1.78:1", "-16:9", "16:99999", "wide"})
            check(error_has(std::string("[display]\naspect = ") + bad + "\n", "aspect is auto or width:height"), "a shape that isn't one is refused");
        check(error_has("[display]\nhud = centre\n", "hud is edges, middle or stretched"), "an unknown HUD place");
    }
    {
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("[display]\nframe_rate = original\n", s, e) && s.frame_rate == mkdd::FrameRate::original &&
                  mkdd::parse_settings("[display]\nframe_rate = 60\n", s, e) && s.frame_rate == mkdd::FrameRate::sixty &&
                  mkdd::parse_settings("[display]\nframe_rate = Display\n", s, e) && s.frame_rate == mkdd::FrameRate::display &&
                  mkdd::parse_settings("[display]\nframe_rate = unlimited\nlow_latency = false\n", s, e) &&
                  s.frame_rate == mkdd::FrameRate::unlimited && !s.low_latency,
              "the frame rates, and low latency");
        check(error_has("[display]\nframe_rate = 120\n", "frame_rate is original, 60, display or unlimited"), "a frame rate that isn't offered");
        check(error_has("[display]\nlow_latency = 1\n", "low_latency is true or false"), "low latency is a switch");
        check(error_has("[display]\nsmaa = on\n", "smaa is true or false") && error_has("[display]\nfsr = yes\n", "fsr is true or false"),
              "SMAA and FSR are switches");
        check(error_has("[display]\nsharpness = 101\n", "from 0 to 100") && error_has("[display]\nsharpness = sharp\n", "from 0 to 100"),
              "sharpness out of range");
        check(mkdd::parse_settings("[display]\nsmaa = true\nfsr = true\nsharpness = 0\n", s, e) && s.smaa && s.fsr && s.sharpness == 0,
              "SMAA, FSR and its sharpness");
        const host::Picture p = mkdd::picture_of(s);
        check(p.smaa && p.fsr && p.sharpness == 0.0f && p.low_latency == s.low_latency && p.vsync, "and the display is told");
    }
    check(error_has("[display]\nlook = vhs\n", "look is plain or crt"), "an unknown look");
    check(error_has("[display]\nscanlines = 101\n", "from 0 to 100") && error_has("[display]\nmask = 101\n", "from 0 to 100"),
          "scanlines and mask out of range");
    check(error_has("[display]\nscaling = zoom\n", "scaling is integer, fit or stretch"), "an unknown scaling");
    check(error_has("[display]\ninternal_scale = 2\n", "[display] has no 'internal_scale'"), "a key that isn't one is named");
    check(error_has("[display]\nvsync = true\nVSYNC = false\n", "set twice"), "duplicate scalar settings rejected");
    check(error_of("\xEF\xBB\xBF[display]\nrender_resolution = AUTO\n").empty(), "BOM and case-insensitive auto");

    // [sound]
    {
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("[sound]\nvolume = 65\nmute_unfocused = true\noutput = surround\ndevice = Speakers (USB Audio #2)\n", s, e) &&
                  s.volume == 65 && s.mute_unfocused && s.sound_output == host::SoundOutput::surround && s.sound_device == "Speakers (USB Audio",
              "volume, muting, output; without quotes a # starts a comment, in a value too");
        check(mkdd::parse_settings("[sound]\ndevice = \"Speakers (USB Audio #2)\"   # the second one\n[display]\nmonitor = \"  LG \"\"27\"\" #1 \"\n", s, e) &&
                  s.sound_device == "Speakers (USB Audio #2)" && s.monitor == "  LG \"27\" #1 ",
              "in double quotes a value keeps its #, its spaces, and a quote written twice");
        check(mkdd::parse_settings("[display]\ngpu = \"\"\nmonitor = \"plain\"\nvsync = \"false\" # quoted anything\n[player2]\na = \"key:#, pad:B\"\n", s, e) &&
                  s.gpu.empty() && s.monitor == "plain" && !s.vsync && s.players[1].controls[mkdd::kCtlA].size() == 2 &&
                  s.players[1].controls[mkdd::kCtlA][0].key == "#",
              "any value may be quoted: an empty one, a switch, a list with the # key");
        check(mkdd::parse_settings("# device = \"x\n[sound] # volume = 3\n  # [display]\nvolume = 7 # = \"\n", s, e) && s.volume == 7,
              "comments with quotes and equals signs in them are comments");
        check(mkdd::parse_settings("[sound]\noutput = device\n", s, e) && s.sound_output == host::SoundOutput::device &&
                  mkdd::parse_settings("[sound]\noutput = stereo\n", s, e) && s.sound_output == host::SoundOutput::stereo,
              "the other outputs");
    }
    check(error_has("[sound]\nvolume = 101\n", "from 0 to 100") && error_has("[sound]\nvolume = loud\n", "from 0 to 100"), "volume range");
    check(error_has("[sound]\noutput = 7.1\n", "output is stereo, surround or device"), "an unknown output");
    check(error_has("[sound]\nmute_unfocused = sometimes\n", "true or false"), "muting is a switch");
    check(error_has("[sound]\nbass = 3\n", "[sound] has no 'bass'"), "an unknown sound setting");
    check(error_of("[sound]\ndevice = \"Speakers #2\n").rfind("line 2: a value that opens with a quote needs its closing quote", 0) == 0 &&
              error_has("[sound]\ndevice = \"a\"\"\n", "closing quote"),
          "a quote left open is an error, not a guess");
    check(error_has("[sound]\ndevice = \"Speakers\" 2\n", "unexpected text after the closing quote"), "text after a closing quote");

    // [controls]
    check(error_has("[controls]\nstick_threshold = 0\n", "from 5 to 95") && error_has("[controls]\ntrigger_threshold = 96\n", "from 5 to 95"),
          "threshold ranges");
    check(error_has("[controls]\nstick_deadzone = 51\n", "from 0 to 50") && error_has("[controls]\nrumble_strength = 101\n", "from 0 to 100"),
          "dead zone and rumble strength ranges");
    {
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("[controls]\nstick_deadzone = 0\nstick_threshold = 20\ntrigger_threshold = 95\nrumble_strength = 0\n", s, e) &&
                  s.stick_deadzone == 0 && s.stick_threshold == 20 && s.trigger_threshold == 95 && s.rumble_strength == 0,
              "and their ends");
    }

    // [console]
    {
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("[console]\nlanguage = English\nsound = mono\nprogressive = false\n", s, e) && s.language == 0 &&
                  s.sound == 0 && !s.progressive,
              "the console's settings");
    }
    check(error_has("[console]\nlanguage = german\n", "language is english"), "a language not on the disc");
    check(error_has("[console]\nlanguage = 1\n", "language is english"), "nor a number for one");
    check(error_has("[console]\nsound = surround\n", "sound is mono or stereo"), "the console's sound is mono or stereo");
    check(error_has("[console]\nprogressive = 480p\n", "progressive is true or false"), "progressive scan is a switch");
    {
        // The cable is the hardware and progressive the SRAM flag: each on its own.
        mkdd::Settings s;
        std::string e;
        check(mkdd::parse_settings("[console]\nprogressive = false\n", s, e) && !s.progressive && s.component_cable,
              "progressive off leaves the cable plugged in");
        check(mkdd::parse_settings("[console]\ncomponent_cable = false\n", s, e) && !s.component_cable && s.progressive,
              "no cable leaves the saved progressive flag as it is");
        mkdd::Settings back;
        check(mkdd::parse_settings(mkdd::settings_text(s), back, e) && !back.component_cable && back.progressive,
              "both read back as written");
    }
    check(error_has("[console]\ncomponent_cable = yes\n", "component_cable is true or false"), "the cable is a switch");
    check(error_has("[console]\nwidescreen = true\n", "[console] has no 'widescreen'"), "the GameCube has no widescreen setting");

    // The file's shape.
    check(error_of("scale = 2\n").rfind("line 1: 'scale' comes before any [section]", 0) == 0, "a setting outside a section");
    check(error_of("[audio]\n").rfind("line 1: there's no section [audio]", 0) == 0, "an unknown section");
    check(error_has("[display\n", "a section is written [name]"), "a section left open");
    check(error_has("[display]\nfullscreen\n", "expected name = value"), "a line that isn't a setting");
    {
        mkdd::Settings s = d;
        s.preset = "desktop", s.fullscreen = true, s.monitor = "\"Odd\" #1 monitor ", s.aspect_w = 64, s.aspect_h = 27, s.hud = mkdd::Hud::stretched;
        s.scaling = host::Scaling::integer, s.smooth = false, s.smaa = true, s.fsr = true, s.sharpness = 35, s.look = host::Look::crt;
        s.scanlines = 80, s.mask = 0, s.scale = 5, s.anisotropy = 8, s.true_color = false, s.vsync = false;
        s.frame_rate = mkdd::FrameRate::unlimited, s.low_latency = false, s.gpu = "AMD Radeon RX 6600", s.performance_overlay = true;
        s.match_refresh = false, s.variable_refresh = true;
        s.volume = 65, s.mute_unfocused = true, s.sound_output = host::SoundOutput::device, s.sound_device = "Speakers (Realtek Audio) #2";
        s.stick_deadzone = 5, s.stick_threshold = 20, s.trigger_threshold = 30, s.rumble_strength = 45;
        s.players[0].controller = "guid:n:70616431:0", s.players[1].controller = "guid:n:70616432:0", s.players[3].controller = "none";
        s.players[2].rumble = false;
        s.players[0].controls[mkdd::kCtlR] = {{Binding::kPad, host::kPadRB, {}}, {Binding::kKey, 0, "Left Shift"}};
        s.players[3].controls[mkdd::kCtlCUp] = {{Binding::kMouse, 0, {}}, {Binding::kPad, host::kPadRSDown, {}}, {Binding::kKey, 0, "#"}};
        s.players[3].controller = "guid:n:23#:0", s.gpu = " #GPU";
        s.players[2].controls[mkdd::kCtlStart].clear();
        s.sound = 0, s.progressive = false, s.component_cable = false;
        mkdd::Settings back;
        std::string e;
        check(mkdd::parse_settings(mkdd::settings_text(s), back, e) && same(back, s), "every setting reads back as written");
    }
    {
        // The data folder's file: written with the defaults, then kept.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "mkdd_settings_test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const mkdd::Settings s = mkdd::load_settings(dir);
        check(same(s, d) && std::filesystem::exists(dir / "mkdd.ini"), "a missing mkdd.ini is written with the defaults");
        mkdd::Settings changed = s;
        changed.look = host::Look::crt;
        changed.players[1].controls[mkdd::kCtlA] = {{Binding::kKey, 0, "Q"}};
        const mkdd::Settings again = mkdd::save_settings(dir, changed) ? mkdd::load_settings(dir) : s;
        check(again.look == host::Look::crt && same(again, changed), "saving, then loading, keeps a change");
        check(!std::filesystem::exists(dir / "mkdd.ini.tmp"), "the temporary file is gone");
        std::filesystem::remove_all(dir);
    }
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
