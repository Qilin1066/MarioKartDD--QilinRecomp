// The GameCube DSP as the recompiled microcode sees it: registers, the two
// memories, the hardware registers at 0xFFxx (mailboxes, the DMA to and from
// main memory, the accelerator's raw port into ARAM) and the arithmetic every
// instruction is built from. The microcode itself is C++ generated at build
// time from the player's DOL by qilin-dsp (recomp/src/dsp); it is one function
// per program, written in terms of this header, and runs until the DSP halts
// or can only wait for the CPU.
//
// For speed the generated function keeps the registers (Regs) in a local
// copy, which the compiler can hold in machine registers because nothing
// takes its address: so the arithmetic here is free functions on values, and
// only memory, the stacks and the devices are the Core's. (With the registers
// in the Core, every store to data memory made the compiler reload them.)
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

#if defined(_MSC_VER)
#define MKDD_DSP_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define MKDD_DSP_INLINE inline __attribute__((always_inline))
#else
#define MKDD_DSP_INLINE inline
#endif

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

// Status register bits.
enum : uint16_t {
    kC = 0x0001, kO = 0x0002, kZ = 0x0004, kS = 0x0008, kAS = 0x0010, kTB = 0x0020, kLZ = 0x0040, kOS = 0x0080,
    kIE = 0x0200, kEIE = 0x0800, kAM = 0x2000, kSXM = 0x4000, kSU = 0x8000
};

// The registers but the stacks. Accumulators are 40 bits kept sign-extended;
// the product is the sum of its parts, 40 bits sign-extended.
struct Regs {
    int64_t ac0 = 0, ac1 = 0, prod = 0;
    uint16_t ar0 = 0, ar1 = 0, ar2 = 0, ar3 = 0, ix0 = 0, ix1 = 0, ix2 = 0, ix3 = 0;
    uint16_t wr0 = 0xFFFF, wr1 = 0xFFFF, wr2 = 0xFFFF, wr3 = 0xFFFF;
    uint16_t axl0 = 0, axl1 = 0, axh0 = 0, axh1 = 0;
    uint16_t sr = 0, config = 0;
    bool operator==(const Regs&) const = default;
};

// ---- arithmetic on values. Each flag-setting function has a form without the
// status register, for where the lifter found the flags unused. ----

MKDD_DSP_INLINE int64_t sx40(int64_t v) { return static_cast<int64_t>(static_cast<uint64_t>(v) << 24) >> 24; }
MKDD_DSP_INLINE bool above32(int64_t v) { return v != static_cast<int32_t>(v); }

// Flags of a 40-bit result.
MKDD_DSP_INLINE void flags64(uint16_t& sr, int64_t v, bool carry = false, bool overflow = false) {
    const uint32_t lo = static_cast<uint32_t>(v);
    unsigned f = sr & ~0x3Fu;
    f |= static_cast<unsigned>(carry) | (overflow ? unsigned(kO | kOS) : 0u);
    f |= static_cast<unsigned>(v == 0) << 2 | static_cast<unsigned>(v < 0) << 3;
    f |= static_cast<unsigned>(v != static_cast<int32_t>(lo)) << 4;
    f |= (((lo ^ (lo << 1)) >> 31) ^ 1u) << 5;  // the top two bits of the low 32 are equal
    sr = static_cast<uint16_t>(f);
}
// Flags of a 16-bit result (the logic instructions on acX.m, TSTAXH); AS from the whole accumulator.
MKDD_DSP_INLINE void flags16(uint16_t& sr, uint16_t v, bool above) {
    unsigned f = sr & ~0x3Fu;
    f |= static_cast<unsigned>(v == 0) << 2 | static_cast<unsigned>(v >> 15) << 3;
    f |= static_cast<unsigned>(above) << 4;
    f |= ((((unsigned(v) ^ (unsigned(v) << 1)) >> 15) & 1u) ^ 1u) << 5;
    sr = static_cast<uint16_t>(f);
}

MKDD_DSP_INLINE int64_t add(int64_t a, int64_t b) { return sx40(a + b); }
MKDD_DSP_INLINE int64_t add(uint16_t& sr, int64_t a, int64_t b) {
    const int64_t res = sx40(a + b);
    flags64(sr, res, static_cast<uint64_t>(a) > static_cast<uint64_t>(res), ((a ^ res) & (b ^ res)) < 0);
    return res;
}
// a - b; the carry is "no borrow".
MKDD_DSP_INLINE int64_t sub(int64_t a, int64_t b) { return sx40(a - b); }
MKDD_DSP_INLINE int64_t sub(uint16_t& sr, int64_t a, int64_t b) {
    const int64_t res = sx40(a - b);
    flags64(sr, res, static_cast<uint64_t>(a) >= static_cast<uint64_t>(res), ((a ^ res) & (-b ^ res)) < 0);
    return res;
}
MKDD_DSP_INLINE int64_t neg(int64_t a) { return sx40(-a); }
MKDD_DSP_INLINE int64_t neg(uint16_t& sr, int64_t a) {
    const int64_t res = sx40(-a);
    flags64(sr, res, a == 0, a == -(int64_t(1) << 39));
    return res;
}
MKDD_DSP_INLINE int64_t mov(int64_t v) { return sx40(v); }
MKDD_DSP_INLINE int64_t mov(uint16_t& sr, int64_t v) {
    v = sx40(v);
    flags64(sr, v);
    return v;
}
MKDD_DSP_INLINE void tst(int64_t) {}
MKDD_DSP_INLINE void tst(uint16_t& sr, int64_t v) { flags64(sr, v); }
// Shifts: logical ones work on the 40 bits, arithmetic ones on the sign-extended value.
MKDD_DSP_INLINE int64_t lsl(int64_t a, unsigned n) { return sx40(static_cast<int64_t>(static_cast<uint64_t>(a) << n)); }
MKDD_DSP_INLINE int64_t lsr(int64_t a, unsigned n) { return sx40(static_cast<int64_t>((static_cast<uint64_t>(a) & 0xFFFFFFFFFFull) >> n)); }
MKDD_DSP_INLINE int64_t asr(int64_t a, unsigned n) { return a >> n; }
MKDD_DSP_INLINE int64_t lsl(uint16_t& sr, int64_t a, unsigned n) { return mov(sr, lsl(a, n)); }
MKDD_DSP_INLINE int64_t lsr(uint16_t& sr, int64_t a, unsigned n) { return mov(sr, lsr(a, n)); }
MKDD_DSP_INLINE int64_t asr(uint16_t& sr, int64_t a, unsigned n) { return mov(sr, asr(a, n)); }
// The variable shifts: bit 6 of the count selects right, by 64 minus the low six bits.
MKDD_DSP_INLINE int64_t lsrn(int64_t a, uint16_t count) {
    const unsigned n = count & 0x3F;
    if (n == 0) return a;
    return (count & 0x40) ? lsr(a, 64 - n) : lsl(a, n);
}
MKDD_DSP_INLINE int64_t asrn(int64_t a, uint16_t count) {
    const unsigned n = count & 0x3F;
    if (n == 0) return a;
    return (count & 0x40) ? asr(a, 64 - n) : lsl(a, n);
}
MKDD_DSP_INLINE int64_t lsrn(uint16_t& sr, int64_t a, uint16_t count) { return mov(sr, lsrn(a, count)); }
MKDD_DSP_INLINE int64_t asrn(uint16_t& sr, int64_t a, uint16_t count) { return mov(sr, asrn(a, count)); }

// Accumulator parts.
MKDD_DSP_INLINE uint16_t acl(int64_t a) { return static_cast<uint16_t>(a); }
MKDD_DSP_INLINE uint16_t acm(int64_t a) { return static_cast<uint16_t>(a >> 16); }
MKDD_DSP_INLINE uint16_t ach(int64_t a) { return static_cast<uint16_t>(a >> 32); }
// acX.m as a store or move reads it: saturated in 40-bit mode.
MKDD_DSP_INLINE uint16_t acm_sat(uint16_t sr, int64_t a) {
    if ((sr & kSXM) && above32(a)) return a > 0 ? 0x7FFF : 0x8000;
    return static_cast<uint16_t>(a >> 16);
}
MKDD_DSP_INLINE int64_t set_acl(int64_t a, uint16_t v) { return (a & ~int64_t(0xFFFF)) | v; }
MKDD_DSP_INLINE int64_t set_acm_raw(int64_t a, uint16_t v) { return (a & ~int64_t(0xFFFF0000)) | (int64_t(v) << 16); }
MKDD_DSP_INLINE int64_t set_ach(int64_t a, uint16_t v) { return (a & 0xFFFFFFFFll) | (int64_t(static_cast<int8_t>(v)) << 32); }
// A load into acX.m: the whole accumulator in 40-bit mode.
MKDD_DSP_INLINE int64_t set_acm(uint16_t sr, int64_t a, uint16_t v) {
    return (sr & kSXM) ? int64_t(static_cast<int16_t>(v)) << 16 : set_acm_raw(a, v);
}
MKDD_DSP_INLINE int64_t ax(uint16_t h, uint16_t l) { return static_cast<int32_t>((uint32_t(h) << 16) | l); }
// The logic instructions: acX.m changes, the flags are the 16-bit ones.
MKDD_DSP_INLINE int64_t logic(int64_t a, uint16_t v) { return set_acm_raw(a, v); }
MKDD_DSP_INLINE int64_t logic(uint16_t& sr, int64_t a, uint16_t v) {
    a = set_acm_raw(a, v);
    flags16(sr, v, above32(a));
    return a;
}

// The multiplier. mode 0: both signed. With SR's SU set, mode 1: both unsigned; mode 2: a unsigned
// (the MULX family, where an ax?.l operand is the unsigned one).
MKDD_DSP_INLINE int64_t mul(uint16_t sr, uint16_t a, uint16_t b, int mode = 0) {
    int64_t p;
    if (mode == 1 && (sr & kSU)) p = static_cast<int64_t>(uint32_t(a) * uint32_t(b));
    else if (mode == 2 && (sr & kSU)) p = int64_t(a) * static_cast<int16_t>(b);
    else p = int64_t(static_cast<int16_t>(a)) * static_cast<int16_t>(b);
    return (sr & kAM) ? p : p * 2;
}
// The product with its low word rounded away, to even.
MKDD_DSP_INLINE int64_t round_low(int64_t p) { return ((p & 0x10000) ? p + 0x8000 : p + 0x7FFF) & ~int64_t(0xFFFF); }

// Address registers: stepping wraps by the wrap register.
MKDD_DSP_INLINE uint16_t ar_inc(uint32_t a, uint32_t w) {
    uint32_t n = a + 1;
    if ((n ^ a) > ((w | 1) << 1)) n -= w + 1;
    return static_cast<uint16_t>(n);
}
MKDD_DSP_INLINE uint16_t ar_dec(uint32_t a, uint32_t w) {
    uint32_t n = a + w;
    if (((n ^ a) & ((w | 1) << 1)) > w) n -= w + 1;
    return static_cast<uint16_t>(n);
}
MKDD_DSP_INLINE uint16_t ar_add(uint32_t a, uint32_t w, uint16_t index) {
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
MKDD_DSP_INLINE uint16_t ar_sub(uint32_t a, uint32_t w, uint16_t index) {
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

struct Core {
    Regs r;
    uint16_t stack[4][8] = {};  // call, data, loop address, loop counter
    uint8_t sp[4] = {0, 0, 0, 0};
    uint16_t pc = 0;
    bool halted = true;     // the control register's halt bit
    bool ext_int = false;   // the CPU asserted the external interrupt; taken when SR's EIE allows
    bool waiting = false;   // run() came back because only the CPU can move the DSP on

    // Idle detection: a taken backward jump with nothing stored since the same jump, and the same
    // registers both times, is a loop only the CPU can end.
    bool progress = false, idle_seen = false;
    uint16_t idle_edge = 0xFFFF;
    Regs idle_regs;
    uint16_t idle_stack[4][8] = {};
    uint8_t idle_sp[4] = {0, 0, 0, 0};

    // Mailboxes: bit 31 is the "waiting to be read" flag of each.
    uint32_t cpu_mail = 0;  // from the CPU
    uint32_t dsp_mail = 0;  // to the CPU

    uint16_t ifx[0x100] = {};  // hardware registers as last written
    uint32_t acc_start = 0, acc_end = 0, acc_current = 0;

    uint16_t dram[0x1000] = {};
    uint16_t iram[0x1000] = {};
    const Program* program = nullptr;

    uint64_t back_jumps = 0;  // (statistics) backward jumps taken
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

    // Data memory.
    MKDD_DSP_INLINE uint16_t rd(uint16_t a) {
        if (a < 0x1000) [[likely]] return dram[a];
        return rd_slow(a);
    }
    MKDD_DSP_INLINE void wr(uint16_t a, uint16_t v) {
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
    MKDD_DSP_INLINE uint16_t rd_second(uint16_t first, uint16_t second) { return ((first ^ second) >> 10) ? rd(second) : rd(first); }

    // Stacks.
    void push(int k, uint16_t v) {
        static constexpr uint8_t depth[4] = {8, 4, 4, 4};
        if (sp[k] >= depth[k]) die("the DSP's stack %u overflowed (last known place %04X)", static_cast<uint32_t>(k), pc);
        stack[k][sp[k]++] = v;
    }
    uint16_t pop(int k) {
        if (sp[k] == 0) die("the DSP's stack %u underflowed (last known place %04X)", static_cast<uint32_t>(k), pc);
        return stack[k][--sp[k]];
    }
    void loop_begin(uint16_t start, uint16_t end, uint16_t count) {
        push(0, start);
        push(2, end);
        push(3, count);
    }
    // After the instruction that ends at `end`: is it the end of the innermost block loop?
    bool loop_hit(uint16_t end) const { return sp[2] && stack[2][sp[2] - 1] == end; }
    // Count the loop down: true to go round again (to loop_start()), false when it is over.
    bool loop_again() {
        if (--stack[3][sp[3] - 1] != 0) return true;
        --sp[0];
        --sp[2];
        --sp[3];
        return false;
    }
    uint16_t loop_start() const { return stack[0][sp[0] - 1]; }

    // The external interrupt, where SR may have just allowed it.
    bool int_due(uint16_t sr) const { return ext_int && (sr & kEIE); }
    // Enter it: the place to come back to and SR go on the stacks; the new SR is returned.
    uint16_t take_int(uint16_t sr, uint16_t return_pc) {
        ext_int = false;
        push(0, return_pc);
        push(1, sr);
        return sr & ~uint16_t(kEIE);
    }

    // At a taken backward jump: false if the DSP has certainly done something since it last took
    // this jump; otherwise the caller stores its registers back and asks idle_slow().
    MKDD_DSP_INLINE bool maybe_idle(uint16_t edge) {
        ++back_jumps;
        if (progress || edge != idle_edge) [[likely]] {
            progress = false;
            idle_edge = edge;
            idle_seen = false;
            return false;
        }
        return true;
    }
    bool idle_slow();  // true: provably waiting for the CPU

    void dma();                  // DSBL was written
    uint16_t accelerator_read();  // ACDRAW
    void accelerator_write(uint16_t v);
};

}  // namespace mkdd::dsp
