// The audio RAM replacements (ar.cpp) over synthetic guest memory: the
// allocator, transfers both ways and when their callback comes; and what the
// expansion interface says of its ports (exi.cpp).
#include <cstring>

#include "boot.hpp"
#include "card.hpp"
#include "device_harness.hpp"

namespace rt = qilin::rt;
using namespace harness;

namespace {
bool card[2] = {};
}
namespace mkdd {
uint8_t* aram_base();  // ar.cpp's own block in this build
bool card_present(uint32_t slot) { return slot < 2 && card[slot]; }
}

int main() {
    rt::init({{}, {}});
    auto cpu = rt::new_cpu();
    constexpr uint32_t lengths = 0x80100000, from = 0x80200000, to = 0x80300000, id = 0x80100100, cb = 0x80200040;
    const auto call_in = [&](std::span<const rt::Provided> table, const char* name, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        cpu.r[3] = a, cpu.r[4] = b, cpu.r[5] = c, cpu.r[6] = d;
        for (const auto& r : table)
            if (std::strcmp(name, r.name) == 0) { r.fn(cpu); return cpu.r[3]; }
        std::fprintf(stderr, "unknown replacement: %s\n", name);
        ++failures;
        return 0xDEADBEEFu;
    };
    const auto ar = [&](const char* name, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0, uint32_t d = 0) {
        return call_in(mkdd::ar_replacements(), name, a, b, c, d);
    };
    const auto exi = [&](uint32_t channel, uint32_t device) { return call_in(mkdd::exi_replacements(), "EXIGetID", channel, device, id, 0); };
    const auto us = [](uint64_t n) { return n * mkdd::kTimerHz / 1000000; };

    check(ar("ARInit", lengths, 3) == 0x4000 && ar("ARInit", lengths, 3) == 0x4000, "ARInit gives the first free address, each time");
    check(ar("ARGetSize") == 0x01000000, "16 MB of ARAM");
    check(ar("ARAlloc", 0x1000) == 0x4000 && ar("ARAlloc", 0x2000) == 0x5000 && rt::peek32(lengths) == 0x1000 &&
          rt::peek32(lengths + 4) == 0x2000, "blocks are stacked, their lengths noted in the caller's array");
    check(ar("ARRegisterDMACallback", cb) == 0 && ar("ARRegisterDMACallback", cb) == cb, "registering returns the callback before");

    for (uint32_t i = 0; i < 0x2000; ++i) rt::poke8(from + i, static_cast<uint8_t>(i * 7 + (i >> 8)));
    ar("ARStartDMA", 0, from, 0x4000, 0x2000);
    check(std::memcmp(mkdd::aram_base() + 0x4000, rt::guest_ptr(from), 0x2000) == 0, "main memory to ARAM");
    advance(cpu, us(100));
    check(callbacks.empty(), "8 KiB isn't done after 100 microseconds");
    advance(cpu, us(40));
    check(callbacks.size() == 1 && callbacks[0].fn == cb, "it is after 130 (63 MB a second), and the callback says so once");
    ar("ARStartDMA", 1, to, 0x4000, 0x2000);
    check(std::memcmp(rt::guest_ptr(to), rt::guest_ptr(from), 0x2000) == 0, "ARAM to main memory");
    advance(cpu, us(200));
    check(callbacks.size() == 2, "each transfer ends with one callback");

    // The registers hold addresses and the length in 32-byte units.
    std::memset(mkdd::aram_base() + 0x5000, 0, 0x100);
    std::memset(rt::guest_ptr(from), 0x5A, 0x100);
    ar("ARStartDMA", 0, from + 0x11, 0x5007, 0x47);
    const uint8_t* a = mkdd::aram_base() + 0x5000;
    check(a[0] == 0x5A && a[0x3F] == 0x5A && a[0x40] == 0, "odd addresses and lengths are rounded down to 32 bytes");
    advance(cpu, us(10));
    check(callbacks.size() == 3, "a short transfer is a short wait");
    ar("ARRegisterDMACallback", 0);
    ar("ARStartDMA", 0, from, 0x4000, 0x20);
    advance(cpu, us(10));
    check(callbacks.size() == 3, "without a callback a transfer just ends");
    ar("ARStartDMA", 0, from, 0x4000, 0x20);  // (a second one starting shows the first had ended)

    check(exi(0, 2) == 1 && rt::peek32(id) == 0, "serial port 1 answers as a bare connector: no Broadband Adapter");
    check(exi(0, 0) == 0 && exi(1, 0) == 0, "an empty card slot has no ID");
    card[0] = true;
    check(exi(0, 0) == 1 && rt::peek32(id) == 16 && exi(1, 0) == 0, "a slot with a card gives the card's size");
    return failures ? 1 : 0;
}
