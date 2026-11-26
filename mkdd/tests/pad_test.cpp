// The controller replacements (pad.cpp) over synthetic guest memory: ports
// coming and going through the SDK's reset protocol, the status PADRead
// assembles from the host's GcPad, rumble, and the wheel calls saying "none".
#include <cstring>

#include "boot.hpp"
#include "input.hpp"
#include "device_harness.hpp"

namespace rt = qilin::rt;
using namespace harness;

namespace {
mkdd::GcPad pads[4] = {};
bool rumble[4] = {};
}
namespace mkdd {
GcPad gc_pad(int port) { return pads[port]; }
void gc_rumble(int port, bool on) { rumble[port] = on; }
void pilot_frame() {}  // (pilot.hpp: no pilot here)
bool pilot_pad(GcPad&) { return false; }
void pilot_status(uint32_t) {}
}

int main() {
    rt::init({{}, {}});
    auto cpu = rt::new_cpu();
    constexpr uint32_t status = 0x80100000, steering = 0x80100100;
    const auto call = [&](const char* name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) {
        cpu.r[3] = a, cpu.r[4] = b, cpu.r[5] = c;
        for (const auto& r : mkdd::pad_replacements())
            if (std::strcmp(name, r.name) == 0) { r.fn(cpu); return cpu.r[3]; }
        std::fprintf(stderr, "unknown replacement: %s\n", name);
        ++failures;
        return 0xDEADBEEFu;
    };
    const auto err = [&](uint32_t port) { return static_cast<int8_t>(*rt::guest_ptr(status + port * 12 + 10)); };
    const auto read = [&] { std::memset(rt::guest_ptr(status), 0xAA, 48); return call("PADRead", status); };

    pads[0] = {true, 0xFFFF, -128, 127, -37, 90, 0x7B, 0xFF};
    check(read() == 0 && err(0) == -1 && err(3) == -1, "before PADInit every port reads as no controller");
    call("PADSetSpec", 5);
    call("PADSetAnalogMode", 3);
    check(call("PADInit") == 1, "PADInit succeeds");
    check(read() == 0 && err(0) == -2 && err(1) == -2 && err(2) == -2 && err(3) == -2, "ports are not ready while they reset");
    check(rt::peek16(status) == 0 && *rt::guest_ptr(status + 2) == 0, "a port that isn't ready reads as zeros");
    advance(cpu, ms(1));
    check(read() == 0x80000000u && err(0) == 0 && err(1) == -2, "ports finish their resets one after the other");
    advance(cpu, ms(5));
    check(read() == 0x80000000u && err(0) == 0 && err(1) == -1 && err(2) == -1 && err(3) == -1,
          "the port with a controller is enabled and can rumble; the others are empty");
    check(rt::peek16(status) == 0x1F7F, "only a controller's own buttons are reported");
    check(static_cast<int8_t>(*rt::guest_ptr(status + 2)) == -128 && static_cast<int8_t>(*rt::guest_ptr(status + 3)) == 127 &&
          static_cast<int8_t>(*rt::guest_ptr(status + 4)) == -37 && static_cast<int8_t>(*rt::guest_ptr(status + 5)) == 90,
          "sticks pass through signed");
    check(*rt::guest_ptr(status + 6) == 0x7B && *rt::guest_ptr(status + 7) == 0xFF && *rt::guest_ptr(status + 8) == 0 && *rt::guest_ptr(status + 9) == 0,
          "analogue mode 3: triggers at eight bits, no pressure on A and B");
    check(*rt::guest_ptr(status + 11) == 0xAA, "a status is twelve bytes with one of padding, left alone");

    call("PADControlMotor", 0, 1);
    check(rumble[0], "rumble starts on an enabled port");
    call("PADControlMotor", 0, 2);
    check(!rumble[0], "stop hard stops it");
    call("PADControlMotor", 1, 1);
    check(!rumble[1], "an empty port has no motor to start");

    // A second controller is plugged in: found by the reset the game issues on "no controller".
    pads[1] = {true, 0x0100, 5, -5, 0, 0, 0, 0};
    check(read() == 0x80000000u && err(1) == -1, "a new controller isn't seen until its port is reset");
    check(call("PADReset", 0x40000000u) == 1 && (read(), err(1)) == -2 && err(0) == 0, "PADReset makes only that port not ready");
    advance(cpu, ms(1));
    check(read() == 0xC0000000u && err(1) == 0 && rt::peek16(status + 12) == 0x0100, "the new controller reads after its reset");

    // The first is pulled out while it rumbles.
    call("PADControlMotor", 0, 1);
    pads[0].connected = false;
    check(read() == 0x40000000u && err(0) == -1 && !rumble[0], "a controller that goes reads as none, and its rumble stops");
    pads[0].connected = true;
    check((read(), err(0)) == -1, "and stays disabled until reset");
    call("PADRecalibrate", 0x80000000u);
    advance(cpu, ms(1));
    check((read(), err(0)) == 0, "PADRecalibrate brings it back like a reset");

    // The other analogue modes send some fields with four bits.
    call("PADSetAnalogMode", 0);
    check(read() == 0 && err(0) == -1 && err(1) == -1, "changing the analogue mode disables every port, as the SDK does");
    call("PADReset", 0xC0000000u);
    advance(cpu, ms(2));
    check((read(), err(0)) == 0 && *rt::guest_ptr(status + 6) == 0x70 && *rt::guest_ptr(status + 7) == 0xF0 &&
          static_cast<int8_t>(*rt::guest_ptr(status + 4)) == -37, "mode 0: triggers at four bits, the C stick at eight");
    call("PADSetAnalogMode", 1);
    call("PADReset", 0x80000000u);
    advance(cpu, ms(1));
    check((read(), err(0)) == 0 && *rt::guest_ptr(status + 6) == 0x7B && static_cast<int8_t>(*rt::guest_ptr(status + 4)) == -48 &&
          static_cast<int8_t>(*rt::guest_ptr(status + 5)) == 80, "mode 1: the C stick at four bits, triggers at eight");
    call("PADSetAnalogMode", 4);
    call("PADReset", 0x80000000u);
    advance(cpu, ms(1));
    check((read(), err(0)) == 0 && *rt::guest_ptr(status + 6) == 0 && *rt::guest_ptr(status + 7) == 0, "mode 4: no triggers");

    check(call("SIProbe", 0) == 0x09000000u && call("SIProbe", 2) == 0x00000008u, "SIProbe: a standard controller, or no response");

    // The wheel library's calls: no wheel anywhere.
    callbacks.clear();
    check(call("SIResetSteeringAsync", 2, 0x80200000u) == 0 && callbacks.empty(), "a wheel reset is accepted and answers later");
    check(static_cast<int32_t>(call("SIResetSteeringAsync", 2, 0x80200000u)) == -2, "a second reset of the port meanwhile is refused");
    advance(cpu, ms(2));
    check(callbacks.size() == 1 && callbacks[0].fn == 0x80200000u && callbacks[0].a0 == 2 && callbacks[0].a1 == 0xFFFFFFFFu,
          "the reset's callback says: no controller");
    std::memset(rt::guest_ptr(steering), 0, 16);
    check(static_cast<int32_t>(call("SIReadSteering", 1, steering)) == -1 && *rt::guest_ptr(steering + 8) == 0xFF && *rt::guest_ptr(steering) == 0,
          "reading a wheel gives the error and nothing else");
    callbacks.clear();
    check(call("SISetSteeringSamplingCallback", 0x80200040u) == 0, "the sampling callback is set (none before)");
    advance(cpu, ms(100));
    check(callbacks.size() >= 11 && callbacks.size() <= 13 && callbacks[0].fn == 0x80200040u, "it runs twice a field while a port is polled");
    check(call("SISetSteeringSamplingCallback", 0) == 0x80200040u, "and is returned when replaced");
    callbacks.clear();
    advance(cpu, ms(100));
    check(callbacks.empty(), "no sampling without a callback");
    return failures ? 1 : 0;
}
