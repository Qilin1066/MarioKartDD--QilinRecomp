// The DSP core and the microcode lifter, without game data.
//
// 1. dsp_core.hpp's arithmetic on values worked out by hand from the DSP's
//    documented rules: flags, the 40-bit modes, the multiplier, the address
//    registers' wrapping, the raw port.
// 2. A program written for this test (dsp_synth.txt), lifted by qilin-dsp at
//    build time and run here with this file as the CPU: mails with and
//    without the interrupt, DMA both ways, block and single loops, a filter
//    by MADD with extended loads, saturating stores, a ring, the accelerator,
//    a computed jump, an interrupt handler, the idle detection and HALT. What
//    it must leave in memory is computed here in plain C++.
#include <cstdio>
#include <cstring>
#include <vector>

#include "dsp_core.hpp"

namespace {

using mkdd::dsp::Core;

int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)
#define CHECK_EQ(a, b)                                                                                         \
    do {                                                                                                       \
        const long long a_ = static_cast<long long>(a), b_ = static_cast<long long>(b);                        \
        if (a_ != b_) {                                                                                        \
            std::printf("FAILED %s:%d: %s = %llX, expected %llX\n", __FILE__, __LINE__, #a, a_, b_);           \
            ++g_failed;                                                                                        \
        }                                                                                                      \
    } while (0)

void arithmetic() {
    using namespace mkdd::dsp;
    uint16_t sr = 0;
    const int64_t max40 = (int64_t(1) << 39) - 1, min40 = -(int64_t(1) << 39);
    // Add: carry when the unsigned 40-bit sum wraps, overflow when the signed one does.
    CHECK_EQ(add(sr, 1, 2), 3);
    CHECK_EQ(sr & 0x3F, kTB);
    CHECK_EQ(add(sr, -1, 1), 0);
    CHECK_EQ(sr & 0x3F, kC | kZ | kTB);
    CHECK_EQ(add(sr, max40, 1), min40);
    CHECK_EQ(sr & 0xBF, kO | kOS | kS | kAS | kTB);
    add(sr, 1, 1);
    CHECK_EQ(sr & 0xBF, kOS | kTB);  // the sticky overflow stays
    CHECK_EQ(add(max40, 1), min40);  // the forms without flags give the same values
    sr = 0;
    // Subtract and compare: carry is "no borrow".
    CHECK_EQ(sub(sr, 5, 5), 0);
    CHECK_EQ(sr & 0x3F, kC | kZ | kTB);
    CHECK_EQ(sub(sr, 3, 5), -2);
    CHECK_EQ(sr & 0x3F, kS | kTB);
    CHECK_EQ(sub(sr, min40, 1), max40);
    CHECK_EQ(sr & 0x3F, kC | kO | kAS | kTB);
    CHECK_EQ(sub(min40, 1), max40);
    sr = 0;
    CHECK_EQ(neg(sr, 0), 0);
    CHECK_EQ(sr & 0x3F, kC | kZ | kTB);
    CHECK_EQ(neg(sr, min40), min40);
    CHECK(sr & kO);
    sr = 0;
    // "Top two bits equal" and "above 32 bits" of a result.
    flags64(sr, 0x40000000);
    CHECK_EQ(sr & 0x3F, 0);
    flags64(sr, 0x100000000ll);
    CHECK_EQ(sr & 0x3F, kAS | kTB);
    flags16(sr, 0x4000, false);
    CHECK_EQ(sr & 0x3F, 0);
    flags16(sr, 0xC000, true);
    CHECK_EQ(sr & 0x3F, kS | kAS | kTB);
    // Shifts on the 40 bits.
    CHECK_EQ(lsr(sr, -1, 4), 0x0FFFFFFFFFll);
    CHECK_EQ(lsr(-1, 4), 0x0FFFFFFFFFll);
    CHECK_EQ(asr(sr, -256, 4), -16);
    CHECK_EQ(lsl(sr, max40, 1), -2);
    CHECK_EQ(lsl(max40, 1), -2);
    CHECK_EQ(lsrn(sr, 0x100, 0x7C), 0x10);  // bit 6: right by 64 - 60
    CHECK_EQ(lsrn(sr, 0x100, 0x04), 0x1000);
    CHECK_EQ(asrn(sr, -0x100, 0x7C), -0x10);
    CHECK_EQ(asrn(-0x100, 0x7C), -0x10);
    CHECK_EQ(lsrn(sr, 0x100, 0x40), 0x100);
    // LSRN and ASRN (ac0 by ac1.m) have the opposite sense: a plain count is a right shift.
    CHECK_EQ(lsrn_ac(sr, 0x100, 0x04), 0x10);
    CHECK_EQ(lsrn_ac(0x100, 0x7C), 0x1000);
    CHECK_EQ(lsrn_ac(-1, 0x01), 0x7FFFFFFFFFll);
    CHECK_EQ(asrn_ac(sr, -0x100, 0x04), -0x10);
    CHECK_EQ(asrn_ac(-0x100, 0x7C), -0x1000);
    CHECK_EQ(lsrn_ac(0x100, 0x00), 0x100);

    // Accumulator parts and the two modes.
    int64_t a = set_ach(0, 0x12FF);
    CHECK_EQ(a, -(int64_t(1) << 32));
    CHECK_EQ(ach(a), 0xFFFF);
    a = set_acm(0, a, 0x8000);  // 16-bit mode: only the middle
    CHECK_EQ(acm(a), 0x8000);
    CHECK_EQ(ach(a), 0xFFFF);
    a = set_acl(a, 0x1234);
    CHECK_EQ(acl(a), 0x1234);
    a = set_acm(kSXM, a, 0x7000);  // 40-bit mode: the whole accumulator
    CHECK_EQ(a, 0x70000000);
    CHECK_EQ(set_acm(kSXM, a, 0x8000), -0x80000000ll);
    CHECK_EQ(acm_sat(kSXM, 0x180000000ll), 0x7FFF);
    CHECK_EQ(acm_sat(kSXM, -0x180000000ll), 0x8000);
    CHECK_EQ(acm_sat(0, 0x180001234ll), 0x8000);  // (16-bit mode: the plain middle word)
    CHECK_EQ(acm_sat(kSXM, 0x1ABCD1234ll), 0x7FFF);
    CHECK_EQ(acm_sat(0, 0x1ABCD1234ll), 0xABCD);
    CHECK_EQ(acm_sat(kSXM, 0x12341234ll), 0x1234);
    sr = 0;
    a = logic(sr, 0x1ABCD1234ll, 0);
    CHECK_EQ(a, 0x100001234ll);
    CHECK_EQ(sr & 0x3F, kZ | kAS | kTB);  // zero is the middle word's; "above" the whole's
    CHECK_EQ(ax(0xFFFF, 0x0001), -0xFFFF);

    // The multiplier: doubled unless AM; unsigned operands only with SU and the mode that allows them.
    CHECK_EQ(mul(0, 0x4000, 0x4000), 0x20000000);
    CHECK_EQ(mul(kAM, 0x4000, 0x4000), 0x10000000);
    CHECK_EQ(mul(kAM, 0xFFFF, 2), -2);
    CHECK_EQ(mul(kAM, 0xFFFF, 0xFFFF, 1), 1);
    CHECK_EQ(mul(kAM | kSU, 0xFFFF, 0xFFFF, 1), 0xFFFE0001ll);
    CHECK_EQ(mul(kAM | kSU, 0xFFFF, 2, 2), 0x1FFFE);
    CHECK_EQ(mul(kAM | kSU, 0xFFFF, 0xFFFF, 2), -0xFFFF);
    CHECK_EQ(mul(kAM | kSU, 0xFFFF, 2, 0), -2);
    CHECK_EQ(sx40((int64_t(1) << 39) + 5), min40 + 5);
    // Rounding the low word away, to even.
    CHECK_EQ(round_low(0x00018000), 0x00020000);
    CHECK_EQ(round_low(0x00028000), 0x00020000);
    CHECK_EQ(round_low(0x00028001), 0x00030000);

    // Address registers. With wr = 2^k - 1 the low k bits wrap in place.
    for (unsigned k = 1; k <= 8; ++k) {
        const uint16_t w = static_cast<uint16_t>((1u << k) - 1);
        for (uint16_t base : {uint16_t(0x0400), uint16_t(0x0F00)}) {
            for (unsigned low = 0; low <= w; ++low) {
                const uint16_t at = static_cast<uint16_t>(base | low);
                CHECK_EQ(ar_inc(at, w), base | ((low + 1) & w));
                CHECK_EQ(ar_dec(at, w), base | ((low - 1) & w));
                for (int step : {1, 2, 3, -1, -2}) {
                    if (static_cast<unsigned>(step < 0 ? -step : step) > w) continue;
                    CHECK_EQ(ar_add(at, w, static_cast<uint16_t>(step)), base | ((low + static_cast<unsigned>(step)) & w));
                    CHECK_EQ(ar_sub(at, w, static_cast<uint16_t>(step)), base | ((low - static_cast<unsigned>(step)) & w));
                }
            }
        }
    }
    CHECK_EQ(ar_inc(0xFFFF, 0xFFFF), 0);
    CHECK_EQ(ar_add(0xFFFF, 0xFFFF, 0x8000), 0x7FFF);
    CHECK_EQ(ar_dec(0, 0xFFFF), 0xFFFF);

    // Stacks and the block loop's bookkeeping.
    Core c;
    c.loop_begin(0x0123, 0x0130, 2);
    CHECK(c.loop_hit(0x0130) && !c.loop_hit(0x0131));
    CHECK(c.loop_again() && c.loop_start() == 0x0123);
    CHECK(!c.loop_again() && c.sp[0] == 0 && c.sp[2] == 0 && c.sp[3] == 0);

    // The raw port: bytes, words and nibbles of ARAM; the second of two loads on one page.
    std::vector<uint8_t> aram(mkdd::dsp::kAramSize);
    c.aram = aram.data();
    aram[0x10] = 0x12, aram[0x11] = 0x34, aram[0x12] = 0xAB;
    c.acc_end = 0xFFFFFFFFu;
    c.ifx[0xD1] = 0x0A;
    c.acc_current = 0x08;
    CHECK_EQ(c.hw_read(0xFFD3), 0x1234);
    CHECK_EQ(c.acc_current, 9);
    c.ifx[0xD1] = 0x05;
    c.acc_current = 0x12;
    CHECK_EQ(c.hw_read(0xFFD3), 0xAB);
    c.ifx[0xD1] = 0x00;
    c.acc_current = 0x25;
    CHECK_EQ(c.hw_read(0xFFD3), 0xB);
    c.ifx[0xD1] = 0x0A;
    c.hw_write(0xFFD8, 0x8000), c.hw_write(0xFFD9, 0x0040);
    c.hw_write(0xFFD3, 0xBEEF);
    CHECK(aram[0x80] == 0xBE && aram[0x81] == 0xEF);
    c.dram[0x0100] = 1, c.dram[0x0120] = 2, c.dram[0x0500] = 3;
    CHECK_EQ(c.rd_second(0x0100, 0x0120), 1);
    CHECK_EQ(c.rd_second(0x0100, 0x0500), 3);
    CHECK_EQ(mkdd::dsp::ector_hash(reinterpret_cast<const uint8_t*>("\x01\x02"), 2), 0x50);
}

// ---- the lifted program ----

std::vector<uint8_t> g_mem(0x1000), g_aram(mkdd::dsp::kAramSize);
int g_irqs = 0;

void program() {
    CHECK_EQ(mkdd::dsp::g_program_count, 1);
    Core c;
    c.aram = g_aram.data();
    c.memory = [](void*, uint32_t address, uint32_t bytes) -> uint8_t* {
        const uint32_t a = address & 0x0FFFFFFFu;
        if (a + bytes > g_mem.size()) std::printf("FAILED: a DMA outside the test's memory (%08X)\n", address), std::exit(1);
        return g_mem.data() + a;
    };
    c.irq = [](void*) { ++g_irqs; };
    c.boot(mkdd::dsp::g_programs, 0);
    c.halted = false;

    constexpr unsigned n = 20;
    int16_t x[n];
    for (unsigned i = 0; i < n; ++i) x[i] = static_cast<int16_t>(i * 3301 - 31000 + (i % 3) * 700);
    constexpr uint32_t at = 0x100;
    for (unsigned i = 0; i < n; ++i) g_mem[at + 2 * i] = static_cast<uint8_t>(x[i] >> 8), g_mem[at + 2 * i + 1] = static_cast<uint8_t>(x[i]);

    auto take_mail = [&] {
        const uint32_t m = c.dsp_mail;
        c.dsp_mail &= 0x7FFFFFFFu;
        c.run();
        return m;
    };
    auto send = [&](uint32_t m) {
        c.cpu_mail = m | 0x80000000u;
        c.run();
    };

    c.run();
    CHECK(c.waiting && !c.halted);
    CHECK_EQ(g_irqs, 1);
    CHECK_EQ(c.dsp_mail, 0x88880001u);
    c.run();  // nothing has changed: it must still be waiting, at once
    CHECK(c.waiting);
    CHECK_EQ(take_mail(), 0x88880001u);
    send(n);
    CHECK_EQ(c.cpu_mail, n);  // taken
    send(0x80000000u | at);
    // It has done all its sums and waits for its second mail to be read.
    CHECK_EQ(g_irqs, 2);
    CHECK_EQ(take_mail(), 0x88880002u);
    CHECK(c.waiting);

    int16_t out[n] = {};
    for (unsigned i = 0; i + 2 < n; ++i) {
        const int64_t p = int64_t(0x4000) * x[i] + int64_t(0x2000) * x[i + 1] - int64_t(0x2000) * x[i + 2];
        out[i] = static_cast<int16_t>(p >> 16);
        CHECK_EQ(c.dram[0x200 + i], static_cast<uint16_t>(out[i]));
    }
    for (unsigned i = 0; i < 8; ++i) {
        const int64_t v = mkdd::dsp::sx40((int64_t(x[i]) * 0x7FFF) << 3);
        const uint16_t expect = v != static_cast<int32_t>(v) ? (v > 0 ? 0x7FFF : 0x8000) : static_cast<uint16_t>(v >> 16);
        CHECK_EQ(c.dram[0x280 + i], expect);
    }
    for (unsigned i = 0; i < 4; ++i) CHECK_EQ(c.dram[0x40 + i], 0x0A08 + i);
    CHECK_EQ(c.dram[0x44], 0);
    for (unsigned i = 0; i < 16; ++i) {
        CHECK_EQ((g_aram[0x200 + 2 * i] << 8) | g_aram[0x201 + 2 * i], static_cast<uint16_t>(out[i]));
        CHECK_EQ((g_mem[at + 0x40 + 2 * i] << 8) | g_mem[at + 0x41 + 2 * i], static_cast<uint16_t>(out[i]));
    }
    for (unsigned i = 0; i < 4; ++i) CHECK_EQ(c.dram[0x300 + i], static_cast<uint16_t>(out[2 + i]));
    CHECK_EQ(c.dram[0x310], static_cast<uint16_t>(out[0] >= out[1] ? out[0] : out[1]));
    CHECK_EQ(c.dram[0x311], 0xF842);
    CHECK_EQ(c.dram[0x312], 0x1124);
    CHECK_EQ(c.dram[0x313], 0xFFF8);
    CHECK_EQ(c.dram[0x314], 0x4211);
    CHECK_EQ(c.dram[0x315], 0x0FFF);

    // An interrupt with an even mail: answered, and it waits again. Then an odd one: answered, halt.
    c.cpu_mail = 0x80000004u;
    c.ext_int = true;
    c.run();
    CHECK_EQ(g_irqs, 3);
    CHECK_EQ(take_mail(), 0x88880E0Eu);
    CHECK(c.waiting && !c.halted && !c.ext_int);
    c.cpu_mail = 0x80000007u;
    c.ext_int = true;
    c.run();
    CHECK_EQ(take_mail(), 0x88880D0Du);
    CHECK(c.halted);
    CHECK_EQ(c.sp[0], 0);
    CHECK_EQ(c.sp[1], 0);
}

}  // namespace

int main() {
    arithmetic();
    program();
    if (g_failed) {
        std::printf("%d checks failed\n", g_failed);
        return 1;
    }
    std::printf("the DSP core and the lifted test program: all passed\n");
    return 0;
}
