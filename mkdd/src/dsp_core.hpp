// The GameCube DSP as the recompiled microcode sees it: registers, the two
// memories, the hardware registers at 0xFFxx (mailboxes, the DMA to and from
// main memory, the accelerator's raw port into ARAM) and the arithmetic every
// instruction is built from. The microcode itself is C++ generated at build
// time from the player's DOL by qilin-dsp (recomp/src/dsp); it is one function
// per program, written in terms of this header, and runs until the DSP halts
// or can only wait for the CPU.
//
// The instruction set and its flags are those of the "GameCube DSP User's
// Manual" kept with Dolphin; details the manual leaves out (the address
// registers' wrapping, the product's rounding, the multiplier's unsigned modes,
// the extended loads' shared bus) are Dolphin's facts. The code is ours.
//
// Nothing here knows the game's runtime: tests drive a Core on its own.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mkdd::dsp {

struct Core;

// One lifted microcode.
struct Program {
    uint32_t hash;    // of the image, as ector_hash gives it
    uint32_t bytes;   // its length
    void (*run)(Core&);
    const char* name;
};
// The table qilin-dsp generates (dsp_no_ucode.cpp where there is no DOL).
extern const Program* const g_programs;
extern const size_t g_program_count;

// The hash emulators name microcode by: xor each byte in, rotate left 3.
uint32_t ector_hash(const uint8_t* p, size_t n);

inline constexpr size_t kAramSize = 16u << 20;

struct Regs {
    int64_t ac[2] = {0, 0};  // 40 bits, kept sign-extended
    int64_t prod = 0;        // the product as the sum of its parts, 40 bits sign-extended
    uint16_t ar[4] = {0, 0, 0, 0}, ix[4] = {0, 0, 0, 0}, wr[4] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF};
    uint16_t axl[2] = {0, 0}, axh[2] = {0, 0};
    uint16_t sr = 0, config = 0;
    uint16_t stack[4][8] = {};  // call, data, loop address, loop counter
    uint8_t sp[4] = {0, 0, 0, 0};
    uint32_t pad = 0;
    bool operator==(const Regs&) const = default;
};

struct Core {
    enum : uint16_t {
        C = 0x0001, O = 0x0002, Z = 0x0004, S = 0x0008, AS = 0x0010, TB = 0x0020, LZ = 0x0040, OS = 0x0080,
        IE = 0x0200, EIE = 0x0800, AM = 0x2000, SXM = 0x4000, SU = 0x8000
    };

    Regs r;
    uint16_t pc = 0;
    bool halted = true;     // the control register's halt bit
    bool ext_int = false;   // the CPU asserted the external interrupt; taken when SR's EIE allows
    bool waiting = false;   // run() came back because only the CPU can move the DSP on

    // Idle detection: a taken backward jump with nothing stored since the same jump, and the same
    // registers both times, is a loop only the CPU can end.
    bool progress = false, idle_seen = false;
    uint16_t idle_edge = 0xFFFF;
    Regs idle_regs;

    // Mailboxes: bit 31 is the "waiting to be read" flag of each.
    uint32_t cpu_mail = 0;  // from the CPU
    uint32_t dsp_mail = 0;  // to the CPU

    uint16_t ifx[0x100] = {};  // hardware registers as last written
    uint32_t acc_start = 0, acc_end = 0, acc_current = 0;

    uint16_t dram[0x1000] = {};
    uint16_t iram[0x1000] = {};
    const Program* program = nullptr;

    uint64_t instructions_hint = 0;  // (statistics) backward jumps taken
    uint64_t dma_bytes = 0, aram_reads = 0, aram_writes = 0;

    // The host's side. All are called rarely (a DMA, a mail, an error), never per sample.
    uint8_t* aram = nullptr;                                        // kAramSize bytes
    uint8_t* (*memory)(void* user, uint32_t address, uint32_t bytes) = nullptr;  // main memory for a DMA
    void (*irq)(void* user) = nullptr;                              // DIRQ: the DSP interrupt at the CPU
    bool (*rom)(void* user, Core&, uint16_t address) = nullptr;     // a jump into the ROM; false: stop running
    void (*fatal)(void* user, const char* message) = nullptr;       // must not return
    void* user = nullptr;

    // Run the program until the DSP halts or waits.
    void run();
    // Start `p` at `start` with the reset state of the registers the boot ROM leaves.
    void boot(const Program* p, uint16_t start);

    // ---- used by the generated code ----

    [[noreturn]] void die(const char* what, uint32_t a = 0, uint32_t b = 0) const;
    [[noreturn]] void bad_pc() const;

    static int64_t sx40(int64_t v) { return static_cast<int64_t>(static_cast<uint64_t>(v) << 24) >> 24; }

    // Flags of a 40-bit result.
    void flags64(int64_t v, bool carry = false, bool overflow = false) {
        const uint32_t lo = static_cast<uint32_t>(v);
        uint16_t f = r.sr & ~uint16_t(0x3F);
        f |= static_cast<uint16_t>(carry) | static_cast<uint16_t>(overflow ? (O | OS) : 0);
        f |= static_cast<uint16_t>((v == 0) << 2) | static_cast<uint16_t>((v < 0) << 3);
        f |= static_cast<uint16_t>((v != static_cast<int32_t>(lo)) << 4);
        f |= static_cast<uint16_t>((((lo ^ (lo << 1)) >> 31) ^ 1u) << 5);
        r.sr = f;
    }
    // Flags of a 16-bit result (the logic instructions on acX.m, TSTAXH); AS from the whole accumulator.
    void flags16(uint16_t v, bool above32) {
        uint16_t f = r.sr & ~uint16_t(0x3F);
        f |= static_cast<uint16_t>((v == 0) << 2) | static_cast<uint16_t>((v >> 15) << 3);
        f |= static_cast<uint16_t>(above32 << 4);
        f |= static_cast<uint16_t>((((v ^ (v << 1)) >> 15) & 1u) ^ 1u) << 5;
        r.sr = f;
    }
    static bool above32(int64_t v) { return v != static_cast<int32_t>(v); }

    int64_t add(int64_t a, int64_t b) {
        const int64_t res = sx40(a + b);
        flags64(res, static_cast<uint64_t>(a) > static_cast<uint64_t>(res), ((a ^ res) & (b ^ res)) < 0);
        return res;
    }
    // a - b; the carry is "no borrow".
    int64_t sub(int64_t a, int64_t b) {
        const int64_t res = sx40(a - b);
        flags64(res, static_cast<uint64_t>(a) >= static_cast<uint64_t>(res), ((a ^ res) & (-b ^ res)) < 0);
        return res;
    }
    int64_t neg(int64_t a) {
        const int64_t res = sx40(-a);
        flags64(res, a == 0, a == -(int64_t(1) << 39));
        return res;
    }
    int64_t mov(int64_t v) {
        v = sx40(v);
        flags64(v);
        return v;
    }
    // Shifts: logical ones work on the 40 bits, arithmetic ones on the sign-extended value.
    int64_t lsl(int64_t a, unsigned n) { return mov(static_cast<int64_t>(static_cast<uint64_t>(a) << n)); }
    int64_t lsr(int64_t a, unsigned n) { return mov(static_cast<int64_t>((static_cast<uint64_t>(a) & 0xFFFFFFFFFFull) >> n)); }
    int64_t asr(int64_t a, unsigned n) { return mov(a >> n); }
    // The variable shifts: bit 6 of the count selects right, by 64 minus the low six bits.
    int64_t lsrn(int64_t a, uint16_t count) {
        const unsigned n = count & 0x3F;
        if (n == 0) return mov(a);
        return (count & 0x40) ? lsr(a, 64 - n) : lsl(a, n);
    }
    int64_t asrn(int64_t a, uint16_t count) {
        const unsigned n = count & 0x3F;
        if (n == 0) return mov(a);
        return (count & 0x40) ? asr(a, 64 - n) : lsl(a, n);
    }

    // Accumulator parts.
    uint16_t acl(int i) const { return static_cast<uint16_t>(r.ac[i]); }
    uint16_t acm(int i) const { return static_cast<uint16_t>(r.ac[i] >> 16); }
    uint16_t ach(int i) const { return static_cast<uint16_t>(r.ac[i] >> 32); }
    // acX.m as a store or move reads it: saturated in 40-bit mode.
    uint16_t acm_sat(int i) const {
        const int64_t v = r.ac[i];
        if ((r.sr & SXM) && above32(v)) return v > 0 ? 0x7FFF : 0x8000;
        return static_cast<uint16_t>(v >> 16);
    }
    void set_acl(int i, uint16_t v) { r.ac[i] = (r.ac[i] & ~int64_t(0xFFFF)) | v; }
    void set_acm_raw(int i, uint16_t v) { r.ac[i] = (r.ac[i] & ~int64_t(0xFFFF0000)) | (int64_t(v) << 16); }
    void set_ach(int i, uint16_t v) { r.ac[i] = (r.ac[i] & 0xFFFFFFFFll) | (int64_t(static_cast<int8_t>(v)) << 32); }
    // A load into acX.m: the whole accumulator in 40-bit mode.
    void set_acm(int i, uint16_t v) {
        if (r.sr & SXM) r.ac[i] = int64_t(static_cast<int16_t>(v)) << 16;
        else set_acm_raw(i, v);
    }
    int64_t ax(int i) const { return static_cast<int32_t>((uint32_t(r.axh[i]) << 16) | r.axl[i]); }
    // The logic instructions: acX.m changes, the flags are the 16-bit ones.
    void logic(int i, uint16_t v) {
        set_acm_raw(i, v);
        flags16(v, above32(r.ac[i]));
    }

    // The multiplier. mode 0: both signed. With SR's SU set, mode 1: both unsigned; mode 2: a unsigned.
    int64_t mul(uint16_t a, uint16_t b, int mode = 0) const {
        int64_t p;
        if (mode == 1 && (r.sr & SU)) p = static_cast<int64_t>(uint32_t(a) * uint32_t(b));
        else if (mode == 2 && (r.sr & SU)) p = int64_t(a) * static_cast<int16_t>(b);
        else p = int64_t(static_cast<int16_t>(a)) * static_cast<int16_t>(b);
        return (r.sr & AM) ? p : p * 2;
    }
    // ax0's part times ax1's part: an .l operand is the unsigned one.
    int64_t mulx(int s, int t) const {
        const uint16_t a = s ? r.axh[0] : r.axl[0], b = t ? r.axh[1] : r.axl[1];
        if (!s && !t) return mul(a, b, 1);
        if (!s) return mul(a, b, 2);
        if (!t) return mul(b, a, 2);
        return mul(a, b, 0);
    }
    void set_prod(int64_t v) { r.prod = sx40(v); }
    // The product with its low word rounded away, to even.
    int64_t prod_rounded() const {
        const int64_t p = r.prod;
        return ((p & 0x10000) ? p + 0x8000 : p + 0x7FFF) & ~int64_t(0xFFFF);
    }

    // Address registers: stepping wraps by the wrap register.
    uint16_t ar_inc(int i) const {
        const uint32_t a = r.ar[i], w = r.wr[i];
        uint32_t n = a + 1;
        if ((n ^ a) > ((w | 1) << 1)) n -= w + 1;
        return static_cast<uint16_t>(n);
    }
    uint16_t ar_dec(int i) const {
        const uint32_t a = r.ar[i], w = r.wr[i];
        uint32_t n = a + w;
        if (((n ^ a) & ((w | 1) << 1)) > w) n -= w + 1;
        return static_cast<uint16_t>(n);
    }
    uint16_t ar_add(int i, uint16_t index) const {
        const uint32_t a = r.ar[i], w = r.wr[i];
        const int32_t ix = static_cast<int16_t>(index);
        const uint32_t mx = (w | static_cast<uint32_t>(ix)) << 1;
        uint32_t n = a + static_cast<uint32_t>(ix);
        const uint32_t d = (n ^ a ^ static_cast<uint32_t>(ix)) & mx;
        if (ix >= 0) {
            if (d > w) n -= w + 1;
        } else if ((((n + w + 1) ^ n) & d) <= w) {
            n += w + 1;
        }
        return static_cast<uint16_t>(n);
    }
    uint16_t ar_sub(int i, uint16_t index) const {
        const uint32_t a = r.ar[i], w = r.wr[i];
        const int32_t ix = static_cast<int16_t>(index);
        const uint32_t mx = (w | static_cast<uint32_t>(ix)) << 1;
        uint32_t n = a - static_cast<uint32_t>(ix);
        const uint32_t d = (n ^ a ^ ~static_cast<uint32_t>(ix)) & mx;
        if (static_cast<uint32_t>(ix) > 0xFFFF8000u) {
            if (d > w) n -= w + 1;
        } else if ((((n + w + 1) ^ n) & d) <= w) {
            n += w + 1;
        }
        return static_cast<uint16_t>(n);
    }

    // Data memory.
    uint16_t rd(uint16_t a) {
        if (a < 0x1000) [[likely]] return dram[a];
        return rd_slow(a);
    }
    void wr(uint16_t a, uint16_t v) {
        progress = true;
        if (a < 0x1000) [[likely]] dram[a] = v;
        else wr_slow(a, v);
    }
    uint16_t rd_slow(uint16_t a);
    void wr_slow(uint16_t a, uint16_t v);
    uint16_t hw_read(uint16_t a);            // a is 0xFFxx
    void hw_write(uint16_t a, uint16_t v);
    uint16_t irom(uint16_t a) const {        // ILRR: instruction memory as data
        if (a < 0x1000) return iram[a];
        die("the microcode reads instruction memory at %04X: outside the RAM (the DSP's ROM is not here)", a);
    }
    // The extended loads from two address registers share the bus: with both in one 1K page
    // the second load sees the first's word.
    uint16_t rd_second(uint16_t first, uint16_t second) { return ((first ^ second) >> 10) ? rd(second) : rd(first); }

    // Stacks.
    void push(int k, uint16_t v) {
        static constexpr uint8_t depth[4] = {8, 4, 4, 4};
        if (r.sp[k] >= depth[k]) die("the DSP's stack %u overflowed at %04X", static_cast<uint32_t>(k), pc);
        r.stack[k][r.sp[k]++] = v;
    }
    uint16_t pop(int k) {
        if (r.sp[k] == 0) die("the DSP's stack %u underflowed at %04X", static_cast<uint32_t>(k), pc);
        return r.stack[k][--r.sp[k]];
    }
    void loop_begin(uint16_t start, uint16_t end, uint16_t count) {
        push(0, start);
        push(2, end);
        push(3, count);
    }
    // After the instruction that ends at `end`: is it the end of the innermost block loop?
    bool loop_hit(uint16_t end) const { return r.sp[2] && r.stack[2][r.sp[2] - 1] == end; }
    // Count the loop down: true to go round again (to loop_start()), false when it is over.
    bool loop_again() {
        if (--r.stack[3][r.sp[3] - 1] != 0) return true;
        --r.sp[0];
        --r.sp[2];
        --r.sp[3];
        return false;
    }
    uint16_t loop_start() const { return r.stack[0][r.sp[0] - 1]; }

    // The external interrupt, where SR may have just allowed it.
    bool int_due() const { return ext_int && (r.sr & EIE); }
    void take_int(uint16_t return_pc) {
        ext_int = false;
        push(0, return_pc);
        push(1, r.sr);
        r.sr &= ~uint16_t(EIE);
    }
    void set_sr(uint16_t v) { r.sr = v & ~uint16_t(0x0100); }

    // At a taken backward jump: true if the DSP is provably waiting for the CPU.
    bool idle(uint16_t edge) {
        ++instructions_hint;
        if (progress || edge != idle_edge) [[likely]] {
            progress = false;
            idle_edge = edge;
            idle_seen = false;
            return false;
        }
        return idle_slow();
    }
    bool idle_slow();

    void dma();                  // DSBL was written
    uint16_t accelerator_read();  // ACDRAW
    void accelerator_write(uint16_t v);
};

}  // namespace mkdd::dsp
