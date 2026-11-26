// The controller ports (input.hpp) and the bindings that fill them (input_map.hpp).
#include "input.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>

#include "input_map.hpp"
#include "qilin/host/analog.hpp"

namespace mkdd {
namespace host = qilin::host;

namespace {

// A port's pad in one word, so a reader never sees half of one: buttons (bit
// 15, which the SDK doesn't use, is "connected"), then the six analogue bytes.
std::atomic<uint64_t> g_pads[kGcPorts];
std::atomic<bool> g_motor[kGcPorts];

uint64_t pack(const GcPad& p) {
    return uint64_t((p.buttons & 0x7FFFu) | (p.connected ? 0x8000u : 0u)) | uint64_t(uint8_t(p.stick_x)) << 16 |
           uint64_t(uint8_t(p.stick_y)) << 24 | uint64_t(uint8_t(p.c_x)) << 32 | uint64_t(uint8_t(p.c_y)) << 40 |
           uint64_t(p.l) << 48 | uint64_t(p.r) << 56;
}
GcPad unpack(uint64_t v) {
    GcPad p{};
    p.connected = (v & 0x8000u) != 0;
    p.buttons = uint16_t(v & 0x7FFFu);
    p.stick_x = int8_t(uint8_t(v >> 16)), p.stick_y = int8_t(uint8_t(v >> 24));
    p.c_x = int8_t(uint8_t(v >> 32)), p.c_y = int8_t(uint8_t(v >> 40));
    p.l = uint8_t(v >> 48), p.r = uint8_t(v >> 56);
    return p;
}

constexpr struct {
    Control control;
    uint16_t bit;
} kButtons[] = {{kCtlA, kGcA}, {kCtlB, kGcB}, {kCtlX, kGcX}, {kCtlY, kGcY}, {kCtlZ, kGcZ}, {kCtlStart, kGcStart},
                {kCtlUp, kGcUp}, {kCtlDown, kGcDown}, {kCtlLeft, kGcLeft}, {kCtlRight, kGcRight}};

// How far a pad's input is pressed, 0 to 1: a trigger or a stick's direction by its travel
// (the sticks with their dead zone gone), a button all or nothing.
float pad_amount(uint32_t bit, const host::PadState& pad, float lx, float ly, float rx, float ry) {
    switch (bit) {
        case host::kPadLT: return pad.lt;
        case host::kPadRT: return pad.rt;
        case host::kPadLSUp: return std::max(ly, 0.0f);
        case host::kPadLSDown: return std::max(-ly, 0.0f);
        case host::kPadLSLeft: return std::max(-lx, 0.0f);
        case host::kPadLSRight: return std::max(lx, 0.0f);
        case host::kPadRSUp: return std::max(ry, 0.0f);
        case host::kPadRSDown: return std::max(-ry, 0.0f);
        case host::kPadRSLeft: return std::max(-rx, 0.0f);
        case host::kPadRSRight: return std::max(rx, 0.0f);
        default: return (pad.buttons & bit) ? 1.0f : 0.0f;
    }
}

// A stick's two axes for the console, from how far each direction is pressed.
void gc_stick(float right, float up, int8_t& x, int8_t& y) {
    const float m = std::sqrt(right * right + up * up);
    if (m <= 0.0f) {
        x = y = 0;
        return;
    }
    // (Two keys held make a diagonal of length 1.4: the corner, at the edge's distance.)
    const float reach = float(kGcStickDead) + std::min(m, 1.0f) * float(kGcStickFull - kGcStickDead);
    x = int8_t(std::clamp(std::lround(right / m * reach), -128L, 127L));
    y = int8_t(std::clamp(std::lround(up / m * reach), -128L, 127L));
}

}  // namespace

GcPad gc_pad(int port) {
    if (port < 0 || port >= kGcPorts) return GcPad{};
    return unpack(g_pads[port].load(std::memory_order_relaxed));
}
void gc_rumble(int port, bool on) {
    if (port >= 0 && port < kGcPorts) g_motor[port].store(on, std::memory_order_relaxed);
}
void set_gc_pad(int port, const GcPad& pad) {
    if (port >= 0 && port < kGcPorts) g_pads[port].store(pack(pad), std::memory_order_relaxed);
}
bool gc_rumble_wanted(int port) { return port >= 0 && port < kGcPorts && g_motor[port].load(std::memory_order_relaxed); }

GcPad map_gc_pad(const BoundControls& bound, const host::PadState& pad, const KeySource& keys, unsigned stick_deadzone,
                 unsigned trigger_threshold, uint8_t& clicks) {
    float lx = pad.lx, ly = pad.ly, rx = pad.rx, ry = pad.ry;
    host::stick_deadzone(lx, ly, stick_deadzone);
    host::stick_deadzone(rx, ry, stick_deadzone);
    // How far a control is pressed: the furthest of its inputs.
    const auto amount = [&](Control c) {
        float most = 0.0f;
        for (const BoundInput& in : bound.controls[c]) {
            const float a = in.kind == Binding::kPad     ? (pad.connected ? pad_amount(in.pad, pad, lx, ly, rx, ry) : 0.0f)
                            : in.kind == Binding::kMouse ? (keys.mouse ? 1.0f : 0.0f)
                                                         : (keys.key_down && in.key >= 0 && keys.key_down(in.key) ? 1.0f : 0.0f);
            most = std::max(most, a);
        }
        return std::min(most, 1.0f);
    };
    GcPad out{};
    // A button: a stick's direction or a trigger bound to one presses it where the display says it's
    // pressed (its threshold and hysteresis), which is what pad.buttons holds.
    for (const auto& b : kButtons)
        for (const BoundInput& in : bound.controls[b.control]) {
            const bool on = in.kind == Binding::kPad     ? pad.connected && (pad.buttons & in.pad) != 0
                            : in.kind == Binding::kMouse ? keys.mouse
                                                         : keys.key_down && in.key >= 0 && keys.key_down(in.key);
            if (on) out.buttons |= b.bit;
        }
    gc_stick(amount(kCtlStickRight) - amount(kCtlStickLeft), amount(kCtlStickUp) - amount(kCtlStickDown), out.stick_x, out.stick_y);
    gc_stick(amount(kCtlCRight) - amount(kCtlCLeft), amount(kCtlCUp) - amount(kCtlCDown), out.c_x, out.c_y);
    const float press = float(std::clamp(trigger_threshold, 5u, 95u)) / 100.0f;
    const auto trigger = [&](Control c, uint8_t held, uint16_t bit, uint8_t& value) {
        const float t = amount(c);
        value = t < 0.02f ? uint8_t(0) : uint8_t(std::lround(float(kGcTriggerDead) + t * float(kGcTriggerFull - kGcTriggerDead)));
        if (t > ((clicks & held) ? press - 0.05f : press) || t >= 1.0f) clicks |= held, out.buttons |= bit;
        else clicks &= uint8_t(~held);
    };
    trigger(kCtlL, 1, kGcL, out.l);
    trigger(kCtlR, 2, kGcR, out.r);
    return out;
}

bool parse_input_script(const std::string& text, std::vector<ScriptStep>& out, std::string& error) {
    static constexpr struct {
        const char* name;
        uint16_t bit;
    } kNames[] = {{"A", kGcA}, {"B", kGcB}, {"X", kGcX}, {"Y", kGcY}, {"Z", kGcZ}, {"L", kGcL}, {"R", kGcR}, {"Start", kGcStart},
                  {"Up", kGcUp}, {"Down", kGcDown}, {"Left", kGcLeft}, {"Right", kGcRight}};
    out.clear();
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find(',', at), text.size());
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        const auto fail = [&](const char* why) {
            error = "\"" + item + "\": " + why;
            return false;
        };
        const size_t sep = item.find('@');
        if (sep == std::string::npos || sep == 0) return fail("expected what@seconds");
        ScriptStep step;
        char* rest = nullptr;
        step.at = std::strtod(item.c_str() + sep + 1, &rest);
        if (rest == item.c_str() + sep + 1 || step.at < 0) return fail("expected a time in seconds after @");
        if (*rest == '+') {
            const char* from = rest + 1;
            step.duration = std::strtod(from, &rest);
            if (rest == from || step.duration <= 0) return fail("expected a duration in seconds after +");
        }
        if (*rest) return fail("unexpected text after the time");
        const std::string what = item.substr(0, sep);
        // A whole number in a range, from `from` up to a ':' or the end.
        const auto number = [&](size_t from, long lo, long hi, long& value, size_t& next) {
            char* stop = nullptr;
            if (from >= what.size()) return false;
            value = std::strtol(what.c_str() + from, &stop, 10);
            next = size_t(stop - what.c_str());
            return stop != what.c_str() + from && value >= lo && value <= hi;
        };
        const size_t colon = what.find(':');
        const std::string head = what.substr(0, colon);
        if (colon == std::string::npos) {
            for (const auto& n : kNames)
                if (what == n.name) step.buttons = n.bit;
            if (!step.buttons) return fail("isn't a button (A B X Y Z L R Start Up Down Left Right), Stick:x:y, C:x:y, LT:n or RT:n");
        } else if (head == "Stick" || head == "C") {
            long x = 0, y = 0;
            size_t next = 0;
            if (!number(colon + 1, -128, 127, x, next) || next >= what.size() || what[next] != ':' ||
                !number(next + 1, -128, 127, y, next) || next != what.size())
                return fail("a stick is Stick:x:y or C:x:y, each from -128 to 127");
            (head == "C" ? step.c : step.stick) = true;
            step.x = int8_t(x), step.y = int8_t(y);
        } else if (head == "LT" || head == "RT") {
            long n = 0;
            size_t next = 0;
            if (!number(colon + 1, 0, 255, n, next) || next != what.size()) return fail("a trigger is LT:n or RT:n, from 0 to 255");
            (head == "LT" ? step.lt : step.rt) = true;
            step.trigger = uint8_t(n);
        } else {
            return fail("isn't a button (A B X Y Z L R Start Up Down Left Right), Stick:x:y, C:x:y, LT:n or RT:n");
        }
        out.push_back(step);
    }
    error.clear();
    return true;
}

void apply_input_script(const std::vector<ScriptStep>& script, double now, GcPad& pad) {
    for (const ScriptStep& s : script) {
        if (now < s.at || now >= s.at + s.duration) continue;
        pad.buttons |= s.buttons;
        if (s.buttons & kGcL) pad.l = 255;
        if (s.buttons & kGcR) pad.r = 255;
        if (s.stick) pad.stick_x = s.x, pad.stick_y = s.y;
        if (s.c) pad.c_x = s.x, pad.c_y = s.y;
        if (s.lt) pad.l = s.trigger;
        if (s.rt) pad.r = s.trigger;
    }
}

}  // namespace mkdd
