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
#include <cstring>

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
            if (gc_pad(static_cast<int>(port)).connected) g_enabled |= bit(port);
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
            motor |= bit(port);
        }
    }
    c.r[3] = motor;
}
void PADControlMotor(Cpu& c) {  // (port, command): 0 stop, 1 rumble, 2 stop hard
    const uint32_t port = port_of(c, "PADControlMotor");
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
