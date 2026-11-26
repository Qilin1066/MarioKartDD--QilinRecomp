// The controller library's replacements (dolphin/pad, and the serial
// interface below it, dolphin/si): four controller ports, each a standard
// GameCube controller while the host has one for it (input.hpp: the player's
// pad or keyboard through the host's bindings), empty otherwise.
//
// Replaced at the library's API, so there is no serial hardware: the game's
// JUTGamePad (lifted) calls PADRead once a frame and does the rest itself,
// including the SDK's clamps (PADClamp, PADClampCircle: pure, lifted).
//
// A port follows the SDK's states. It starts disabled; PADInit, PADReset and
// PADRecalibrate reset the ports named, one after the other, and a port with
// a controller is enabled when its reset ends. Until then PADRead reports it
// as not ready; a disabled port reads as "no controller", which is what makes
// the game reset it again, so a controller plugged in later is found on the
// game's next try. One that goes away is disabled by the read that misses it.
//
// The Logitech wheel library (linked into the SDK and used by JUTGamePad
// when SIProbe names a wheel) sits on the SI steering calls: no port ever
// holds a wheel, and they say so.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "boot.hpp"
#include "input.hpp"
#include "later.hpp"
#include "os.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

constexpr uint32_t kPorts = 4;
// PAD_ERR_* (pad.h).
constexpr int8_t kErrNone = 0, kErrNoController = -1, kErrNotReady = -2;
// What SIProbe says of a port (si.h): a standard controller, or no answer.
constexpr uint32_t kTypeController = 0x09000000u, kTypeNoResponse = 0x00000008u;
// The buttons a standard controller has: the D-pad, Z, R, L, A, B, X, Y, START.
constexpr uint16_t kButtons = 0x1F7F;
constexpr uint32_t kStatusSize = 12;  // PADStatus: buttons, four stick bytes, two triggers, analogue A and B, err

constexpr uint32_t bit(uint32_t port) { return 0x80000000u >> port; }  // PAD_CHANn_BIT

uint32_t g_enabled = 0, g_resetting = 0;
uint32_t g_spec = 5, g_analog_mode = 0;
uint64_t g_reset_free = 0;  // when the reset in flight ends: the next one starts then
uint32_t g_sampling_rate = 0;

// A reset asks the port what is there and, of a controller, its origin: two
// short transfers, about a millisecond together (an estimate, not measured).
uint64_t reset_ticks() { return ticks_from_us(1000); }

void disable(uint32_t port) {
    if (devices_log() && (g_enabled & bit(port))) rt::log("pad: port %u disabled", port);
    g_enabled &= ~bit(port);
    gc_rumble(static_cast<int>(port), false);
}

void reset(uint32_t mask) {
    mask &= 0xF0000000u & ~g_resetting;
    for (uint32_t port = 0; port < kPorts; ++port) {
        if (!(mask & bit(port))) continue;
        g_resetting |= bit(port);
        disable(port);
        const uint64_t now = time_base();
        g_reset_free = std::max(g_reset_free, now) + reset_ticks();
        run_after(g_reset_free - now, [port](Cpu&) {
            g_resetting &= ~bit(port);
            if (gc_pad(static_cast<int>(port)).connected) {
                if (devices_log()) rt::log("pad: port %u has a controller", port);
                g_enabled |= bit(port);
            }
        });
    }
}

// A stick or trigger the controller sends with four bits in this analogue mode.
uint8_t coarse(uint8_t v) { return v & 0xF0; }
int8_t coarse_stick(int8_t v) { return static_cast<int8_t>(coarse(static_cast<uint8_t>(v + 128)) - 128); }

// PADSetAnalogMode chooses what the controller's last four bytes carry: the C
// stick, the triggers and the pressure of A and B, at eight bits or four. A
// standard controller's A and B have no pressure sensors and read 0.
void write_status(uint32_t s, const GcPad& p) {
    int8_t cx = p.c_x, cy = p.c_y;
    uint8_t l = p.l, r = p.r;
    switch (g_analog_mode) {
    case 1: cx = coarse_stick(cx), cy = coarse_stick(cy); break;
    case 2: cx = coarse_stick(cx), cy = coarse_stick(cy), l = coarse(l), r = coarse(r); break;
    case 3: break;
    case 4: l = r = 0; break;
    default: l = coarse(l), r = coarse(r); break;  // 0, 5, 6, 7 (and what a controller makes of a mode it doesn't know)
    }
    rt::poke16(s, p.buttons & kButtons);
    rt::poke8(s + 2, static_cast<uint8_t>(p.stick_x));
    rt::poke8(s + 3, static_cast<uint8_t>(p.stick_y));
    rt::poke8(s + 4, static_cast<uint8_t>(cx));
    rt::poke8(s + 5, static_cast<uint8_t>(cy));
    rt::poke8(s + 6, l);
    rt::poke8(s + 7, r);
    rt::poke8(s + 8, 0);
    rt::poke8(s + 9, 0);
    rt::poke8(s + 10, static_cast<uint8_t>(kErrNone));
}
void write_error(uint32_t s, int8_t err) {
    std::memset(rt::guest_ptr(s), 0, kStatusSize);
    rt::poke8(s + 10, static_cast<uint8_t>(err));
}

// ---- development: a pilot for runs without a player. A race can't be
// driven by a script of timed presses, so the first port's stick can follow a
// path instead: the positions a kart passed through, taken from the game
// itself.
//   MKDD_PILOT_RECORD=<file>[:kart]  writes the kart's position (x z, a line every 10 frames) while a
//                                    race runs: kart 1 in a Grand Prix is one of the game's own drivers
//   MKDD_PILOT=<file>                steers port 1's kart (kart 0) along such a path, closed into a lap;
//                                    the script still holds A
// Positions are read through the supported build's addresses (names from the disc's map):
// RaceMgr::sRaceManager, KartCtrl's instance, its kart bodies and their position.
constexpr uint32_t kRaceManager = 0x803CB7E8u, kKartCtrl = 0x803CC588u, kBodies = 0xA0, kBodyPosition = 0x23C;

bool kart_position(uint32_t kart, float& x, float& z) {
    const uint32_t ctrl = rt::peek32(kKartCtrl);
    if (rt::peek32(kRaceManager) == 0 || ctrl < 0x80000000u || ctrl >= 0x81800000u) return false;
    const uint32_t body = rt::peek32(ctrl + kBodies + 4 * kart);
    if (body < 0x80000000u || body >= 0x81800000u) return false;
    const auto f = [](uint32_t a) { return std::bit_cast<float>(rt::peek32(a)); };
    x = f(body + kBodyPosition), z = f(body + kBodyPosition + 8);
    return true;
}
void pilot_record() {
    static const char* spec = std::getenv("MKDD_PILOT_RECORD");
    if (!spec) return;
    static uint32_t kart = 0, frame = 0;
    static FILE* out = [] {
        std::string name = spec;
        if (const size_t colon = name.rfind(':'); colon != std::string::npos && colon > 1 && name.find_first_not_of("0123456789", colon + 1) == std::string::npos) {
            kart = static_cast<uint32_t>(std::atoi(name.c_str() + colon + 1));
            name.resize(colon);
        }
        FILE* f = std::fopen(name.c_str(), "w");
        if (!f) rt::fatal(nullptr, "MKDD_PILOT_RECORD: can't write %s", name.c_str());
        return f;
    }();
    float x, z;
    if (!kart_position(kart, x, z) || ++frame % 10) return;
    std::fprintf(out, "%.1f %.1f\n", x, z);
    std::fflush(out);
}
// The first port's stick, if the pilot has it: set in the status PADRead has just written.
void pilot_steer(uint32_t status) {
    static const char* name = std::getenv("MKDD_PILOT");
    if (!name) return;
    struct Point {
        float x, z;
    };
    static const std::vector<Point> path = [] {
        std::vector<Point> out;
        FILE* f = std::fopen(name, "r");
        if (!f) rt::fatal(nullptr, "MKDD_PILOT: can't read %s", name);
        for (Point p; std::fscanf(f, "%f %f", &p.x, &p.z) == 2;) out.push_back(p);
        std::fclose(f);
        if (out.size() < 8) rt::fatal(nullptr, "MKDD_PILOT: %s holds no path", name);
        return out;
    }();
    static const float sign = [] { const char* e = std::getenv("MKDD_PILOT_SIGN"); return e && *e == '-' ? -1.0f : 1.0f; }();
    static float last_x = 0, last_z = 0, heading_x = 0, heading_z = 0;
    static size_t nearest = 0;
    static uint32_t reversing = 0;  // frames left of backing out
    static bool racing = false;
    float x, z;
    if (!kart_position(0, x, z)) {
        racing = false;
        return;
    }
    const auto distance2 = [&](size_t i) { return (path[i].x - x) * (path[i].x - x) + (path[i].z - z) * (path[i].z - z); };
    if (!racing) {  // a new race: wherever the kart is on the path
        nearest = 0;
        for (size_t i = 1; i < path.size(); ++i)
            if (distance2(i) < distance2(nearest)) nearest = i;
        heading_x = heading_z = 0;
    } else {  // along from where it was, never back
        for (size_t step = 1; step <= 6; ++step)
            if (distance2((nearest + step) % path.size()) < distance2(nearest)) nearest = (nearest + step) % path.size(), step = 0;
        const float dx = x - last_x, dz = z - last_z;
        if (dx * dx + dz * dz > 1.0f && !reversing) heading_x = dx, heading_z = dz;  // (not while backing out)
    }
    racing = true;
    last_x = x, last_z = z;
    // Stuck (against a wall, in the scenery): hardly moved in a second with A held. Back out for a
    // second and a half with B, then go on.
    static float mark_x = 0, mark_z = 0;
    static uint32_t since_mark = 0;
    const uint16_t buttons = rt::peek16(status);
    if (reversing) {
        --reversing;
        rt::poke16(status, static_cast<uint16_t>((buttons & ~0x0100u) | 0x0200u));
        rt::poke8(status + 2, 0);
        mark_x = x, mark_z = z, since_mark = 0;
        return;
    }
    if (!(buttons & 0x0100) || ++since_mark >= 60) {
        if ((buttons & 0x0100) && since_mark >= 60 && (x - mark_x) * (x - mark_x) + (z - mark_z) * (z - mark_z) < 150.0f * 150.0f)
            reversing = 90;
        mark_x = x, mark_z = z, since_mark = 0;
    }
    const Point& target = path[(nearest + 4) % path.size()];
    const float tx = target.x - x, tz = target.z - z;
    if (heading_x == 0 && heading_z == 0) return;  // not moving yet: straight on
    const float angle = std::atan2(heading_x * tz - heading_z * tx, heading_x * tx + heading_z * tz);
    const long stick = std::lround(std::clamp(sign * angle * 200.0f, -100.0f, 100.0f));
    rt::poke8(status + 2, static_cast<uint8_t>(static_cast<int8_t>(stick)));
}

uint32_t port_of(Cpu& c, const char* who) {
    if (c.r[3] >= kPorts) rt::fatal(&c, "%s: there is no controller port %u", who, c.r[3]);
    return c.r[3];
}

// ---- PAD
void PADInit(Cpu& c) {  // -> TRUE; the first call resets every port
    static bool done = false;
    if (!done) reset(0xF0000000u);
    done = true;
    c.r[3] = 1;
}
void PADReset(Cpu& c) {  // (mask) -> TRUE
    reset(c.r[3]);
    c.r[3] = 1;
}
void PADRecalibrate(Cpu& c) {  // (mask) -> TRUE: a reset that also takes the sticks' rest position anew
    reset(c.r[3]);
    c.r[3] = 1;
}
void PADRead(Cpu& c) {  // (PADStatus[4]) -> the ports whose controllers can rumble
    watch_guest_values();
    pilot_record();
    const uint32_t status = c.r[3];
    uint32_t motor = 0;
    for (uint32_t port = 0; port < kPorts; ++port) {
        const uint32_t s = status + port * kStatusSize;
        if (g_resetting & bit(port)) {
            write_error(s, kErrNotReady);
        } else if (!(g_enabled & bit(port))) {
            write_error(s, kErrNoController);
        } else if (const GcPad p = gc_pad(static_cast<int>(port)); !p.connected) {
            disable(port);
            write_error(s, kErrNoController);
        } else {
            write_status(s, p);
            if (port == 0) pilot_steer(s);
            motor |= bit(port);
        }
    }
    c.r[3] = motor;
}
void PADControlMotor(Cpu& c) {  // (port, command): 0 stop, 1 rumble, 2 stop hard
    const uint32_t port = port_of(c, "PADControlMotor");
    if (devices_log()) {
        static uint32_t last[kPorts] = {~0u, ~0u, ~0u, ~0u};
        if (last[port] != c.r[4]) rt::log("pad: port %u motor %u%s", port, c.r[4], g_enabled & bit(port) ? "" : " (no controller)");
        last[port] = c.r[4];
    }
    if (g_enabled & bit(port)) gc_rumble(static_cast<int>(port), c.r[4] == 1);
}
void PADSetSpec(Cpu& c) { g_spec = c.r[3]; }
void PADSetAnalogMode(Cpu& c) {  // (mode): as in the SDK, every port is disabled until its next reset
    g_analog_mode = c.r[3];
    for (uint32_t port = 0; port < kPorts; ++port)
        if (g_enabled & bit(port)) disable(port);
}

// ---- SI
void SIInit(Cpu&) {}
void SIProbe(Cpu& c) {  // (port) -> its device's type
    const uint32_t port = port_of(c, "SIProbe");
    c.r[3] = gc_pad(static_cast<int>(port)).connected ? kTypeController : kTypeNoResponse;
}
// How often the hardware samples the controllers, in milliseconds (0: once a
// field). The host's input is sampled when PADRead asks, so it changes nothing.
void SISetSamplingRate(Cpu& c) { g_sampling_rate = c.r[3]; }
void SIRefreshSamplingRate(Cpu&) {}

// ---- the steering wheel. Its sampling callback runs on the serial
// interface's polling interrupt, which comes twice a field while any port is
// being polled.
uint32_t g_steering_sampling = 0, g_steering_reset[kPorts] = {};
uint64_t g_next_sampling = 0;
bool g_sampling_source = false;
uint64_t sampling_ticks() { return kTimerHz * 1001 / 120000; }

void poll_sampling() {
    if (!g_steering_sampling || !g_enabled) return;
    const uint64_t now = time_base();
    if (now < g_next_sampling) return;
    g_next_sampling += sampling_ticks();
    if (g_next_sampling <= now) g_next_sampling = now + sampling_ticks();  // late: the samples missed aren't made up
    post_interrupt([](Cpu& c) {
        if (g_steering_sampling) guest_callback(c, g_steering_sampling, 0, 0);
    });
}
uint64_t sampling_deadline() { return g_steering_sampling && g_enabled ? g_next_sampling : UINT64_MAX; }

void SIResetSteeringAsync(Cpu& c) {  // (port, callback(port, err)) -> 0, or -2 while one is in flight
    const uint32_t port = port_of(c, "SIResetSteeringAsync"), cb = c.r[4];
    if (devices_log()) rt::log("pad: wheel reset asked of port %u", port);
    if (g_steering_reset[port]) {
        c.r[3] = static_cast<uint32_t>(kErrNotReady);
        return;
    }
    if (cb) {
        g_steering_reset[port] = cb;
        run_after(reset_ticks(), [port](Cpu& cc) {
            const uint32_t fn = g_steering_reset[port];
            g_steering_reset[port] = 0;
            guest_callback(cc, fn, port, static_cast<uint32_t>(kErrNoController));
        });
    }
    c.r[3] = 0;
}
void SIReadSteering(Cpu& c) {  // (port, status*) -> err; the status's err is its ninth byte
    port_of(c, "SIReadSteering");
    if (c.r[4]) rt::poke8(c.r[4] + 8, static_cast<uint8_t>(kErrNoController));
    c.r[3] = static_cast<uint32_t>(kErrNoController);
}
void SISetSteeringSamplingCallback(Cpu& c) {  // (callback) -> the one before
    const uint32_t was = g_steering_sampling;
    g_steering_sampling = c.r[3];
    g_next_sampling = time_base() + sampling_ticks();
    if (!g_sampling_source) {
        g_sampling_source = true;
        add_timer_source(poll_sampling, sampling_deadline);
    }
    c.r[3] = was;
}
void SIControlSteering(Cpu& c) { port_of(c, "SIControlSteering"); }  // (port, command, force): nothing to command

const rt::Provided kReplacements[] = {
    {0x800ED5BCu, "PADReset", PADReset},
    {0x800ED6CCu, "PADRecalibrate", PADRecalibrate},
    {0x800ED7E0u, "PADInit", PADInit},
    {0x800ED930u, "PADRead", PADRead},
    {0x800EDC30u, "PADControlMotor", PADControlMotor},
    {0x800EDCE8u, "PADSetSpec", PADSetSpec},
    {0x800EE4A0u, "PADSetAnalogMode", PADSetAnalogMode},
    {0x800EEFF4u, "SIInit", SIInit},
    {0x800EFF3Cu, "SIProbe", SIProbe},
    {0x800EFF60u, "SISetSamplingRate", SISetSamplingRate},
    {0x800F0044u, "SIRefreshSamplingRate", SIRefreshSamplingRate},
    {0x800F00B4u, "SIResetSteeringAsync", SIResetSteeringAsync},
    {0x800F0450u, "SIReadSteering", SIReadSteering},
    {0x800F0620u, "SISetSteeringSamplingCallback", SISetSteeringSamplingCallback},
    {0x800F0674u, "SIControlSteering", SIControlSteering},
};

}  // namespace

std::span<const rt::Provided> pad_replacements() { return kReplacements; }

}  // namespace mkdd
