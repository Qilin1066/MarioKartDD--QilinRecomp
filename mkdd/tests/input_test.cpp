// The controller ports (mkdd/src/input.cpp): a player's bindings applied to
// a controller and the keyboard give the GameCube controller expected, the
// ports hold what is put in them, and scripts read and play back.
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "input.hpp"
#include "input_map.hpp"
#include "settings.hpp"

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

// A player's bindings with each key given a code: its place in `keys`.
mkdd::BoundControls bind_player(const mkdd::Player& p, std::vector<std::string>& keys) {
    mkdd::BoundControls b;
    for (unsigned c = 0; c < mkdd::kControlCount; ++c)
        for (const mkdd::Binding& in : p.controls[c]) {
            int code = -1;
            if (in.kind == mkdd::Binding::kKey) {
                code = int(keys.size());
                keys.push_back(in.key);
            }
            if (in.kind != mkdd::Binding::kPad) b.has_keys = true;
            b.controls[c].push_back({in.kind, in.pad, code});
        }
    return b;
}

}  // namespace

int main() {
    namespace host = qilin::host;
    using mkdd::GcPad;
    const mkdd::Settings d = mkdd::default_settings();
    std::vector<std::string> names;
    const mkdd::BoundControls one = bind_player(d.players[0], names);
    std::vector<std::string> none;
    const mkdd::BoundControls two = bind_player(d.players[1], none);
    check(one.has_keys && !two.has_keys && none.empty(), "by default the keyboard plays the first port alone");

    std::set<std::string> down;
    mkdd::KeySource keys;
    keys.key_down = [&](int code) { return code >= 0 && down.count(names[size_t(code)]) != 0; };
    const auto map = [&](const mkdd::BoundControls& b, const host::PadState& pad, uint8_t* clicks = nullptr) {
        uint8_t local = 0;
        return mkdd::map_gc_pad(b, pad, keys, d.stick_deadzone, d.trigger_threshold, clicks ? *clicks : local);
    };

    host::PadState rest;
    rest.connected = true;
    {
        const GcPad g = map(one, rest);
        check(g.buttons == 0 && g.stick_x == 0 && g.stick_y == 0 && g.c_x == 0 && g.c_y == 0 && g.l == 0 && g.r == 0,
              "a controller at rest: nothing held, both sticks at the centre");
    }
    {
        // The pad by position: an Xbox pad's A, X, B, Y are the GameCube's A, B, X, Y.
        host::PadState p = rest;
        p.buttons = host::kPadA;
        check(map(one, p).buttons == mkdd::kGcA, "pad A (bottom) is A");
        p.buttons = host::kPadX;
        check(map(one, p).buttons == mkdd::kGcB, "pad X (left) is B, as the GameCube's B is beside its A");
        p.buttons = host::kPadB;
        check(map(one, p).buttons == mkdd::kGcX, "pad B (right) is X");
        p.buttons = host::kPadY | host::kPadRB | host::kPadStart | host::kPadUp | host::kPadLeft;
        check(map(one, p).buttons == (mkdd::kGcY | mkdd::kGcZ | mkdd::kGcStart | mkdd::kGcUp | mkdd::kGcLeft),
              "Y, a shoulder button as Z, Start and the D-pad");
        p.buttons = host::kPadLB;
        check(map(one, p).buttons == mkdd::kGcZ, "the other shoulder button is Z too");
    }
    {
        // Sticks: nothing inside the dead zone; from its edge the console's own dead zone is already behind.
        host::PadState p = rest;
        p.lx = 0.10f;
        check(map(one, p).stick_x == 0, "inside the dead zone the stick is at the centre");
        p.lx = 0.16f;
        const GcPad just = map(one, p);
        check(just.stick_x > mkdd::kGcStickDead && just.stick_x <= mkdd::kGcStickDead + 2 && just.stick_y == 0,
              "just past it, just past the console's dead zone");
        p.lx = 1.0f;
        check(map(one, p).stick_x == mkdd::kGcStickFull && map(one, p).stick_y == 0, "the edge is full lock");
        p.lx = -1.0f, p.ly = 0;
        check(map(one, p).stick_x == -mkdd::kGcStickFull, "and the other way");
        p.lx = 0, p.ly = 0.575f;  // half way between the dead zone and the edge
        const GcPad half = map(one, p);
        check(half.stick_x == 0 && half.stick_y >= 56 && half.stick_y <= 59, "half the travel is half the range, up is positive");
        p.lx = 0.7071f, p.ly = -0.7071f;
        const GcPad diag = map(one, p);
        check(diag.stick_x == 71 && diag.stick_y == -71, "a diagonal at the edge keeps its direction");
        p = rest;
        p.rx = -1.0f;
        check(map(one, p).c_x == -mkdd::kGcStickFull && map(one, p).stick_x == 0, "the right stick is the C-stick");
    }
    {
        // Triggers: analogue, with the click past the threshold and its release 5 points lower.
        host::PadState p = rest;
        uint8_t clicks = 0;
        p.rt = 0.25f;
        GcPad g = map(one, p, &clicks);
        check(g.r > mkdd::kGcTriggerDead && g.r < 100 && !(g.buttons & mkdd::kGcR), "a trigger part way: analogue, no click");
        p.rt = 0.52f;
        g = map(one, p, &clicks);
        check((g.buttons & mkdd::kGcR) && clicks == 2, "past the threshold it clicks");
        p.rt = 0.47f;
        check((map(one, p, &clicks).buttons & mkdd::kGcR) != 0, "jitter under the threshold doesn't let go");
        p.rt = 0.40f;
        check(!(map(one, p, &clicks).buttons & mkdd::kGcR) && clicks == 0, "released below the hysteresis");
        p.rt = 0.47f;
        check(!(map(one, p, &clicks).buttons & mkdd::kGcR), "and jitter can't press it again");
        p.rt = 1.0f, p.lt = 1.0f;
        g = map(one, p, &clicks);
        check(g.r == mkdd::kGcTriggerFull && g.l == mkdd::kGcTriggerFull && (g.buttons & mkdd::kGcL) && (g.buttons & mkdd::kGcR),
              "both all the way: full, with their clicks");
    }
    {
        // The keyboard: all or nothing.
        down = {"Left", "Up", "X", "Space"};
        const GcPad g = map(one, host::PadState{});
        check(g.buttons == (mkdd::kGcA | mkdd::kGcR) && g.r == mkdd::kGcTriggerFull, "keys: A, and R all the way with its click");
        check(g.stick_x == -71 && g.stick_y == 71, "two arrow keys: the stick's corner, at the edge's distance");
        down = {"Right"};
        check(map(one, host::PadState{}).stick_x == mkdd::kGcStickFull, "one arrow key: full lock");
        down = {"Left", "Right"};
        check(map(one, host::PadState{}).stick_x == 0, "opposite keys cancel");
        down = {"X"};
        check(map(two, host::PadState{}).buttons == 0, "the second port has no keys");
        host::PadState gone;  // not connected, with stale state
        gone.buttons = host::kPadA, gone.lx = 1.0f;
        down.clear();
        const GcPad off = map(one, gone);
        check(off.buttons == 0 && off.stick_x == 0, "a controller that isn't connected presses nothing");
    }
    {
        // A stick's direction or a trigger bound to a button, and a button bound to a stick.
        mkdd::Settings s = d;
        s.players[0].controls[mkdd::kCtlA] = {{mkdd::Binding::kPad, host::kPadRT, {}}};
        s.players[0].controls[mkdd::kCtlB] = {{mkdd::Binding::kPad, host::kPadRSUp, {}}};
        s.players[0].controls[mkdd::kCtlStickLeft] = {{mkdd::Binding::kPad, host::kPadLeft, {}}};
        s.players[0].controls[mkdd::kCtlX] = {{mkdd::Binding::kMouse, 0, {}}};
        std::vector<std::string> k;
        const mkdd::BoundControls b = bind_player(s.players[0], k);
        host::PadState p = rest;
        p.buttons = host::kPadRT | host::kPadRSUp | host::kPadLeft;  // as the display reports them past their thresholds
        keys.mouse = true;
        const GcPad g = map(b, p);
        keys.mouse = false;
        check((g.buttons & (mkdd::kGcA | mkdd::kGcB | mkdd::kGcX)) == (mkdd::kGcA | mkdd::kGcB | mkdd::kGcX),
              "a trigger, a stick's direction and the mouse each press a button");
        check(g.stick_x == -mkdd::kGcStickFull, "a D-pad button on the stick is full lock");
    }
    {
        // The ports.
        check(!mkdd::gc_pad(0).connected && !mkdd::gc_pad(3).connected, "before anything is set every port is empty");
        GcPad g{};
        g.connected = true, g.buttons = mkdd::kGcStart | mkdd::kGcLeft, g.stick_x = -128, g.stick_y = 127, g.c_x = -1, g.c_y = 5, g.l = 255, g.r = 1;
        mkdd::set_gc_pad(2, g);
        const GcPad r = mkdd::gc_pad(2);
        check(r.connected && r.buttons == g.buttons && r.stick_x == -128 && r.stick_y == 127 && r.c_x == -1 && r.c_y == 5 && r.l == 255 && r.r == 1,
              "a port reads back what was put in it, to the last bit");
        check(!mkdd::gc_pad(1).connected && !mkdd::gc_pad(-1).connected && !mkdd::gc_pad(4).connected, "the others, and ports there aren't, are empty");
        mkdd::gc_rumble(2, true);
        mkdd::gc_rumble(7, true);
        check(mkdd::gc_rumble_wanted(2) && !mkdd::gc_rumble_wanted(0) && !mkdd::gc_rumble_wanted(7), "rumble is per port");
        mkdd::gc_rumble(2, false);
        check(!mkdd::gc_rumble_wanted(2), "and stops");
    }
    {
        // Scripts.
        std::vector<mkdd::ScriptStep> script;
        std::string e;
        check(mkdd::parse_input_script("Start@12,A@14.5,Stick:-100:50@20+3,R@20+3,C:0:-128@1,LT:200@2+0.5", script, e) && script.size() == 6,
              "a script reads");
        const auto at = [&](double now) {
            GcPad g{};
            mkdd::apply_input_script(script, now, g);
            return g;
        };
        check(at(11.9).buttons == 0 && at(12.0).buttons == mkdd::kGcStart && at(12.19).buttons == mkdd::kGcStart && at(12.2).buttons == 0,
              "a press lasts a fifth of a second from its time");
        const GcPad drift = at(21.0);
        check(drift.stick_x == -100 && drift.stick_y == 50 && (drift.buttons & mkdd::kGcR) && drift.r == 255 && at(23.0).stick_x == 0,
              "a stick and a trigger held for a duration");
        check(at(1.1).c_y == -128 && at(2.2).l == 200 && !(at(2.2).buttons & mkdd::kGcL), "the C-stick, and a trigger without its click");
        check(mkdd::parse_input_script("", script, e) && script.empty(), "an empty script is no presses");
        check(!mkdd::parse_input_script("Jump@3", script, e) && e.find("isn't a button") != std::string::npos, "an unknown button is named");
        check(!mkdd::parse_input_script("A", script, e) && e.find("what@seconds") != std::string::npos, "a press needs a time");
        check(!mkdd::parse_input_script("A@soon", script, e) && !mkdd::parse_input_script("A@3+", script, e) &&
                  !mkdd::parse_input_script("A@3x", script, e) && !mkdd::parse_input_script("A@-1", script, e),
              "times and durations must be numbers");
        check(!mkdd::parse_input_script("Stick:200:0@1", script, e) && !mkdd::parse_input_script("Stick:1@1", script, e) &&
                  !mkdd::parse_input_script("LT:256@1", script, e) && !mkdd::parse_input_script("Stick:1:2:3@1", script, e),
              "sticks and triggers out of range or misshapen are refused");
        check(mkdd::parse_input_script("Reset@5+1,A@5", script, e) && !mkdd::script_reset(script, 4.9) && mkdd::script_reset(script, 5.5) &&
                  !mkdd::script_reset(script, 6.0) && at(5.1).buttons == mkdd::kGcA && at(5.5).buttons == 0,
              "a script holds the reset button for its duration, apart from the controller");
    }
    {
        // The console's reset button: its inputs looked up as a player's are.
        mkdd::Settings s = d;
        std::string e;
        check(mkdd::parse_settings("[controls]\nreset = key:Backspace, pad:Back\n", s, e), "a reset binding reads");
        std::vector<std::string> reset_keys;
        mkdd::Player holder;
        holder.controls[0] = s.reset;
        const mkdd::BoundControls bound = bind_player(holder, reset_keys);
        std::set<std::string> held;
        mkdd::KeySource k;
        k.key_down = [&](int code) { return code >= 0 && held.count(reset_keys[size_t(code)]) != 0; };
        host::PadState pads[2];
        pads[0].connected = pads[1].connected = true;
        check(!mkdd::reset_held(bound.controls[0], pads, 2, k), "nothing pressed: not held");
        held.insert("Backspace");
        check(mkdd::reset_held(bound.controls[0], pads, 2, k), "its key holds it");
        held.clear();
        pads[1].buttons = host::kPadBack;
        check(mkdd::reset_held(bound.controls[0], pads, 2, k), "its pad button on any controller holds it");
        pads[1].connected = false;
        check(!mkdd::reset_held(bound.controls[0], pads, 2, k), "but not on one that isn't connected");
        pads[0].buttons = host::kPadStart;
        k.mouse = true;
        check(!mkdd::reset_held(bound.controls[0], pads, 2, k), "other inputs don't");
        check(!mkdd::reset_held(mkdd::BoundControls{}.controls[0], pads, 2, k), "unbound, nothing holds it");
    }
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
