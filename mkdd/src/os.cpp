// The OS library's replacements (dolphin/os, base, db, and the debug UART of
// exi): start-up, arenas, interrupts, caches, time, SRAM, reset. Arguments
// arrive in r3..., results go in r3 (and r4 for 64-bit values), as the EABI
// passes them.
#include "os.hpp"
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "audio.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <vector>

#include "boot.hpp"
#include "build.hpp"
#include "priority.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// ---- low memory the OS keeps, found in the code named.
constexpr uint32_t kCurrentContextPhysical = 0x800000C0u, kCurrentContext = 0x800000D4u;  // OSSetCurrentContext, 800E7930
constexpr uint32_t kFpuContext = 0x800000D8u;                                             // OSClearContext, 800E7AF8
constexpr uint32_t kUserMask = 0x800000C4u, kOsMask = 0x800000C8u;                        // __OSMaskInterrupts, 800E8B44
constexpr uint32_t kInterruptTable = 0x80003040u;                                         // __OSInterruptInit, 800E87F8: 32 handlers
constexpr uint32_t kSystemTimeOffset = 0x800030D8u;                                       // __OSGetSystemTime, 800EC35C: 64 bits

void nothing(Cpu&) {}
void returns_zero(Cpu& c) { c.r[3] = 0; }
void returns_one(Cpu& c) { c.r[3] = 1; }

// ---- SRAM (OSRtc.c): the console's settings, kept where the SDK keeps its
// copy (build::kSramControl), so a lock hands out the pointer it always did.
// The copy is filled from the port's settings instead of the clock chip's SRAM
// over EXI. Layout (the code at 800EA764 and the accessors after it): checksum
// +0, its inverse +2, counter bias +0xC, language +0x12, flags +0x13 (bits 0-1
// video mode, bit 2 stereo, bit 7 progressive), then the extended part at
// +0x14 (wireless pad IDs +0x30, Game Boy Player mode +0x3C).
unsigned g_language = 0;
bool g_stereo = true, g_progressive = false;
constexpr uint32_t kSram = build::kSramControl, kSramFlags = kSram + 0x13;
constexpr uint32_t kSramEnabled = kSram + 0x44, kSramLocked = kSram + 0x48, kSramSync = kSram + 0x4C;

void sram_checksum() {  // over the four 16-bit words from +0xC, as UnlockSram does
    uint16_t sum = 0, inverse = 0;
    for (uint32_t a = kSram + 0x0C; a < kSram + 0x14; a += 2) {
        sum = static_cast<uint16_t>(sum + rt::peek16(a));
        inverse = static_cast<uint16_t>(inverse + static_cast<uint16_t>(~rt::peek16(a)));
    }
    rt::poke16(kSram, sum);
    rt::poke16(kSram + 2, inverse);
}
void init_sram() {  // __OSInitSram
    std::memset(rt::guest_ptr(kSram), 0, 0x50);
    rt::poke8(kSram + 0x12, static_cast<uint8_t>(g_language));
    rt::poke8(kSramFlags, static_cast<uint8_t>((g_stereo ? 0x04 : 0) | (g_progressive ? 0x80 : 0)));
    sram_checksum();
    rt::poke32(kSram + 0x40, 64);  // offset: nothing waits to be written
    rt::poke32(kSramSync, 1);
}
void lock_sram(Cpu& c, uint32_t part) {  // -> the SRAM, or NULL while it's locked; interrupts stay off until the unlock
    if (rt::peek32(kSramLocked)) {
        c.r[3] = 0;
        return;
    }
    rt::poke32(kSramEnabled, interrupts_enabled());
    set_interrupts_enabled(false);
    rt::poke32(kSramLocked, 1);
    c.r[3] = kSram + part;
}
void unlock_sram(Cpu& c, bool commit, uint32_t part) {  // -> written (always: there's no chip to wait for)
    if (commit) {
        if (part == 0) {
            const uint8_t flags = *rt::guest_ptr(kSramFlags);
            if ((flags & 3) > 2) rt::poke8(kSramFlags, flags & 0xFCu);
            sram_checksum();
        }
    }
    rt::poke32(kSramLocked, 0);
    set_interrupts_enabled(rt::peek32(kSramEnabled) != 0);
    deliver_interrupts(c);
    c.r[3] = 1;
}
void __OSLockSram(Cpu& c) { lock_sram(c, 0); }
void __OSLockSramEx(Cpu& c) { lock_sram(c, 0x14); }
void __OSUnlockSram(Cpu& c) { unlock_sram(c, c.r[3] != 0, 0); }
void __OSUnlockSramEx(Cpu& c) { unlock_sram(c, c.r[3] != 0, 0x14); }
void __OSSyncSram(Cpu& c) { c.r[3] = 1; }
uint8_t sram_flags() { return *rt::guest_ptr(kSramFlags); }
void set_sram_flag(uint8_t bit, bool on) {
    const uint8_t flags = static_cast<uint8_t>((sram_flags() & ~bit) | (on ? bit : 0));
    if (flags == sram_flags()) return;
    rt::poke8(kSramFlags, flags);
    sram_checksum();
}
void OSGetSoundMode(Cpu& c) { c.r[3] = (sram_flags() & 0x04) ? 1 : 0; }  // 0 mono, 1 stereo
void OSSetSoundMode(Cpu& c) {
    set_sram_flag(0x04, c.r[3] & 1);
    g_game_set_stereo.store(c.r[3] & 1, std::memory_order_relaxed);
}
void OSGetProgressiveMode(Cpu& c) { c.r[3] = sram_flags() >> 7; }
void OSSetProgressiveMode(Cpu& c) {
    set_sram_flag(0x80, c.r[3] & 1);
    g_game_set_progressive.store(c.r[3] & 1, std::memory_order_relaxed);
}
void OSGetWirelessID(Cpu& c) { c.r[3] = rt::peek16(kSram + 0x30 + 2 * (c.r[3] & 3)); }  // (channel)
void OSSetWirelessID(Cpu& c) { rt::poke16(kSram + 0x30 + 2 * (c.r[3] & 3), static_cast<uint16_t>(c.r[4])); }
void OSGetGbsMode(Cpu& c) { c.r[3] = rt::peek16(kSram + 0x3C); }
void OSSetGbsMode(Cpu& c) { rt::poke16(kSram + 0x3C, static_cast<uint16_t>(c.r[3])); }

// ---- start-up
void DBInit(Cpu&) {}  // no debugger

bool g_enabled = false;         // interrupts: one flag, as MSR[EE] is (os.hpp)
uint32_t g_mask = 0xFFFFFFFFu;  // the OS's interrupt mask (the two words in low memory, ORed): all masked until OSInit
void set_mask(uint32_t user) {
    rt::poke32(kUserMask, user);
    g_mask = user | rt::peek32(kOsMask);
    dsp_interrupt_enables(g_mask);
}

// OSInit (800E5B8C), in its order, less what only the hardware needs: the
// performance counters, exception vectors, caches and memory protection.
void OSInit(Cpu& c) {  // once: the game calls it again
    static bool done = false;
    if (done) return;
    done = true;
    namespace b = build;
    const uint64_t start = time_base();  // __OSStartTime (system time: the offset is 0)
    rt::poke32(b::kStartTime, static_cast<uint32_t>(start >> 32));
    rt::poke32(b::kStartTime + 4, static_cast<uint32_t>(start));
    set_interrupts_enabled(false);
    rt::poke32(b::kBootInfo, 0x80000000u);
    // BI2, where the apploader left it: the debug flag and the pad specification.
    const uint32_t bi2 = rt::peek32(0x800000F4u);
    if (!bi2) rt::fatal(&c, "OSInit: no BI2 in low memory (boot.cpp places it)");
    rt::poke32(b::kBi2DebugFlag, bi2 + 0x0C);
    rt::poke32(b::kPadSpec, rt::peek32(bi2 + 0x24));
    rt::poke8(0x800030E8u, static_cast<uint8_t>(rt::peek32(bi2 + 0x0C)));
    rt::poke8(0x800030E9u, static_cast<uint8_t>(rt::peek32(bi2 + 0x24)));
    // The arena: from low memory, or the linker's bounds where it gives none.
    const uint32_t lo = rt::peek32(0x80000030u), hi = rt::peek32(0x80000034u);
    rt::poke32(b::kArenaLo, lo ? lo : b::kArenaLoValue);
    rt::poke32(b::kArenaHi, hi ? hi : 0x81700000u);
    // __OSInterruptInit: no handlers, everything masked.
    std::memset(rt::guest_ptr(kInterruptTable), 0, 32 * 4);
    rt::poke32(kUserMask, 0);
    rt::poke32(kOsMask, 0);
    set_mask(0xFFFFFFE0u);
    rt::call(c, b::kEXIInit);
    rt::call(c, b::kSIInit);
    init_sram();
    // __OSThreadInit is init_threads (threads.cpp), done before the game starts.
    rt::call(c, b::kInitAudioSystem);  // lifted: it runs a stub program on the DSP (dsp.cpp) and resets it
    rt::log("OSInit: Dolphin OS, console type %08X, %u MB, arena %08X-%08X", rt::peek32(0x8000002Cu), rt::peek32(0x80000028u) >> 20,
            rt::peek32(b::kArenaLo), rt::peek32(b::kArenaHi));
    // ClearArena: after a cold boot, the arena is zeroed.
    std::memset(rt::guest_ptr(rt::peek32(b::kArenaLo)), 0, rt::peek32(b::kArenaHi) - rt::peek32(b::kArenaLo));
    set_interrupts_enabled(true);  // as OSInit does before DVDInit (800E5EF4)
    rt::call(c, b::kDVDInit);
    // DVDInquiryAsync's callback stores the drive's device code (the inquiry's
    // word at +2) in low memory, with the top bit: a retail drive answers 0.
    rt::poke16(0x800030E6u, 0x8000);
}

void OSGetConsoleType(Cpu& c) {  // 800E5A00
    const uint32_t type = rt::peek32(0x8000002Cu);
    c.r[3] = type ? type : 0x10000002u;
}

// The debug serial port (EXI's UART), where the C library's console output
// (OSReport, printf) goes. A retail console has none and the output is lost;
// here it's the game's own log, kept as lines with a prefix.
std::string g_console;
void InitializeUART(Cpu& c) { c.r[3] = 0; }  // (baud) -> 0: ready
void WriteUARTN(Cpu& c) {                    // (buffer, count) -> 0
    g_console.append(reinterpret_cast<const char*>(rt::guest_ptr(c.r[3])), c.r[4]);
    for (size_t nl; (nl = g_console.find('\n')) != std::string::npos; g_console.erase(0, nl + 1)) {
        std::string line = g_console.substr(0, nl);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        rt::log("game: %s", line.c_str());
    }
    c.r[3] = 0;
}

void OSRegisterVersion(Cpu& c) { rt::log("%s", reinterpret_cast<const char*>(rt::guest_ptr(c.r[3]))); }

void OSPanic(Cpu& c) {  // OSPanic(file, line, format, ...): the format isn't expanded
    rt::fatal(&c, "OSPanic in %s:%u: %s", reinterpret_cast<const char*>(rt::guest_ptr(c.r[3])), c.r[4],
              reinterpret_cast<const char*>(rt::guest_ptr(c.r[5])));
}

void OSGetPhysicalMemSize(Cpu& c) { c.r[3] = rt::peek32(0x80000028u); }
// Memory protection (the MI's four ranges) traps nothing here: a stray access
// isn't caught by it, on the console or in this port.
void OSProtectRange(Cpu&) {}
void OSGetFontEncode(Cpu& c) { c.r[3] = 0; }  // OS_FONT_ENCODE_ANSI: an NTSC console that isn't Japanese (800E8724)

// ---- error handlers (crash screens for CPU exceptions): kept in the OS's
// own table, never called; a fault in the guest stops the runtime with its
// own report. A handler for floating-point exceptions (16) makes the OS turn
// them on in every thread's context: lifted code raises none, so that's only said.
void OSSetErrorHandler(Cpu& c) {  // (error, handler) -> the previous handler
    const uint32_t e = c.r[3] & 0xFFFFu;
    if (e >= 17) rt::fatal(&c, "OSSetErrorHandler for error %u", e);
    const uint32_t was = rt::peek32(build::kErrorTable + 4 * e);
    rt::poke32(build::kErrorTable + 4 * e, c.r[4]);
    if (e == 16 && c.r[4] && !was)
        rt::log("the game installs a handler for floating-point exceptions (enable bits %02X): none are raised here",
                rt::peek32(build::kFpscrEnableBits) & 0xF8u);
    c.r[3] = was;
}

// ---- CPU registers (base)
uint32_t g_hid2 = 0xA0000000u;  // load/store quantised and paired singles on (__OSPSInit)
uint32_t g_msr = 0x00002032u;   // FP available, the rest as the OS runs; EE is g_enabled
void PPCMfmsr(Cpu& c) { c.r[3] = g_msr | (g_enabled ? 0x8000u : 0); }
void PPCMtmsr(Cpu& c) {
    g_msr = c.r[3] & ~0x8000u;
    g_enabled = (c.r[3] & 0x8000u) != 0;
    deliver_interrupts(c);
}
void PPCSync(Cpu&) {}
void PPCHalt(Cpu&) {  // where exit() ends
    rt::log("the game exited");
    before_exit();
    std::fflush(nullptr);
    std::_Exit(0);  // other guest threads are parked on host threads: no static destructors
}
void PPCMfhid2(Cpu& c) { c.r[3] = g_hid2; }
void PPCMthid2(Cpu& c) { g_hid2 = c.r[3]; }

// ---- interrupts
std::deque<std::function<void(Cpu&)>> g_pending;
struct Timer {
    std::function<void()> poll;
    std::function<uint64_t()> deadline;
};
std::vector<Timer> g_timers;
bool g_in_interrupt = false;
void OSDisableInterrupts(Cpu& c) {
    c.r[3] = g_enabled;
    g_enabled = false;
}
void OSEnableInterrupts(Cpu& c) {
    const bool was = g_enabled;
    g_enabled = true;
    deliver_interrupts(c);
    c.r[3] = was;
}
void OSRestoreInterrupts(Cpu& c) {
    const bool was = g_enabled;
    g_enabled = c.r[3] != 0;
    deliver_interrupts(c);
    c.r[3] = was;
}

// ---- interrupt handlers, by the OS's numbering (__OSDispatchInterrupt,
// 800E8C54): 0-4 memory protection, 5 audio DMA, 6 ARAM, 7 DSP, 8 audio
// streaming, 9-16 EXI (three for each of channels 0 and 1, two for channel
// 2), 17 CP, 18 PE token, 19 PE finish, 20 SI, 21 DVD, 22 the reset switch, 23
// PI error, 24 VI, 25 debugger, 26 high-speed port. The table is the OS's own,
// in low memory; a mask bit per interrupt, bit 31 for interrupt 0.
void __OSSetInterruptHandler(Cpu& c) {  // (interrupt, handler) -> the previous handler
    const uint32_t n = c.r[3] & 0xFFFFu;
    if (n >= 32) rt::fatal(&c, "__OSSetInterruptHandler(%u)", n);
    const uint32_t was = rt::peek32(kInterruptTable + 4 * n);
    rt::poke32(kInterruptTable + 4 * n, c.r[4]);
    c.r[3] = was;
}
void __OSGetInterruptHandler(Cpu& c) {
    const uint32_t n = c.r[3] & 0xFFFFu;
    c.r[3] = n < 32 ? rt::peek32(kInterruptTable + 4 * n) : 0;
}
// The OS keeps the devices' own enables in step with its mask: for the DSP
// group (5 audio DMA, 6 ARAM, 7 DSP) those are bits of the DSP control register.
void __OSMaskInterrupts(Cpu& c) {  // (mask) -> the mask before
    const uint32_t was = rt::peek32(kUserMask);
    set_mask(was | c.r[3]);
    c.r[3] = was;
}
uint32_t g_held = 0;  // raised while masked: the hardware's line stays up, so they arrive when unmasked
void __OSUnmaskInterrupts(Cpu& c) {
    const uint32_t was = rt::peek32(kUserMask);
    set_mask(was & ~c.r[3]);
    for (uint32_t due = g_held & ~g_mask, n = 0; due; ++n)
        if (due & (0x80000000u >> n)) {
            due &= ~(0x80000000u >> n), g_held &= ~(0x80000000u >> n);
            raise_interrupt(n);
        }
    c.r[3] = was;
}

// ---- contexts: the guest's thread switching is native, so only the words in
// low memory are left, which lifted interrupt handlers set around their callbacks.
void OSSetCurrentContext(Cpu& c) {
    rt::poke32(kCurrentContext, c.r[3]);
    rt::poke32(kCurrentContextPhysical, c.r[3] & 0x3FFFFFFFu);
}
void OSGetCurrentContext(Cpu& c) { c.r[3] = rt::peek32(kCurrentContext); }
void OSClearContext(Cpu& c) {  // mode and state (+0x1A0, +0x1A2); it no longer owns the FPU
    rt::poke16(c.r[3] + 0x1A0, 0);
    rt::poke16(c.r[3] + 0x1A2, 0);
    if (rt::peek32(kFpuContext) == c.r[3]) rt::poke32(kFpuContext, 0);
}
void OSGetStackPointer(Cpu& c) { c.r[3] = c.r[1]; }
// The running thread's floating-point registers into a context: FPRs +0x90,
// FPSCR +0x194, the paired singles' second halves +0x1C8 (the crash screen's use).
void OSFillFPUContext(Cpu& c) {
    const uint32_t context = c.r[3];
    for (uint32_t i = 0; i < 32; ++i) {
        const uint64_t a = std::bit_cast<uint64_t>(c.ps0[i]), b = std::bit_cast<uint64_t>(c.ps1[i]);
        rt::poke32(context + 0x90 + 8 * i, static_cast<uint32_t>(a >> 32)), rt::poke32(context + 0x94 + 8 * i, static_cast<uint32_t>(a));
        rt::poke32(context + 0x1C8 + 8 * i, static_cast<uint32_t>(b >> 32)), rt::poke32(context + 0x1CC + 8 * i, static_cast<uint32_t>(b));
    }
    rt::poke32(context + 0x194, c.fpscr);
}

// The write-gather pipe's address register: writing it resets the pipe, as on
// the hardware (Dolphin: GPFifo::ResetGatherPipe).
void (*g_wpar_reset)() = nullptr;
void PPCMtwpar(Cpu&) {
    if (g_wpar_reset) g_wpar_reset();
}

// ---- caches: the host's are coherent, so only dcbz's zeroing is left to do.
void DCZeroRange(Cpu& c) {
    const uint32_t start = c.r[3] & ~31u, end = (c.r[3] + c.r[4] + 31) & ~31u;
    if (c.r[4]) std::memset(rt::guest_ptr(start), 0, end - start);
}

// ---- the locked cache: 16 KB at 0xE0000000 (the runtime backs it). Its DMA
// is a copy, done at once, so there's never a queue to wait for. Enabling it
// sets HID2's LCE bit, which code reads to know it's there: the movie decoder
// (THPVideoDecode, 800F0848) refuses to decode without it.
constexpr uint32_t kHid2Lce = 0x10000000u;
void LCEnable(Cpu&) { g_hid2 |= kHid2Lce; }
void LCDisable(Cpu&) { g_hid2 &= ~kHid2Lce; }
uint32_t blocks(uint32_t n) { return n == 0 ? 128 : n; }  // 0 means 128 blocks of 32 bytes
void LCStoreBlocks(Cpu& c) { std::memmove(rt::guest_ptr(c.r[3] | 0x80000000u), rt::guest_ptr(c.r[4]), 32 * blocks(c.r[5])); }
void LCStoreData(Cpu& c) {  // (dest, srcLC, bytes) -> DMA transactions issued
    const uint32_t n = (c.r[5] + 31) / 32;
    std::memmove(rt::guest_ptr(c.r[3] | 0x80000000u), rt::guest_ptr(c.r[4]), 32 * n);
    c.r[3] = (n + 127) / 128;
}

// ---- time: the time base counts at a quarter of the bus clock, from 1 January 2000 (time_base).
void OSGetTime(Cpu& c) {
    const uint64_t t = time_base();
    c.r[3] = static_cast<uint32_t>(t >> 32);
    c.r[4] = static_cast<uint32_t>(t);
}
void OSGetTick(Cpu& c) { c.r[3] = static_cast<uint32_t>(time_base()); }
void __OSGetSystemTime(Cpu& c) {  // the time base plus the offset in low memory (0 since boot)
    const uint64_t t = time_base() + (uint64_t{rt::peek32(kSystemTimeOffset)} << 32 | rt::peek32(kSystemTimeOffset + 4));
    c.r[3] = static_cast<uint32_t>(t >> 32);
    c.r[4] = static_cast<uint32_t>(t);
}

// OSTicksToCalendarTime(ticks (r3:r4), OSCalendarTime* (r5)): the time base
// (from 1 January 2000) as a date. OSCalendarTime is ten words: sec, min,
// hour, mday (1-31), mon (0-11), year, wday (0 = Sunday), yday (0-365), msec,
// usec.
void OSTicksToCalendarTime(Cpu& c) {
    const int64_t ticks = static_cast<int64_t>(uint64_t(c.r[3]) << 32 | c.r[4]);
    const uint32_t td = c.r[5];
    constexpr int64_t kPerSecond = int64_t(kTimerHz), kPerDay = kPerSecond * 86400;
    int64_t days = ticks / kPerDay, rest = ticks % kPerDay;
    if (rest < 0) rest += kPerDay, --days;
    const int64_t secs = rest / kPerSecond, sub = rest % kPerSecond;
    const int64_t z = days + days_from_civil(2000, 1, 1);
    int64_t year;
    unsigned mon, mday;
    civil_from_days(z, year, mon, mday);
    const auto put = [&](unsigned field, int64_t v) { rt::poke32(td + 4 * field, static_cast<uint32_t>(v)); };
    put(0, secs % 60);
    put(1, secs / 60 % 60);
    put(2, secs / 3600);
    put(3, mday);
    put(4, mon - 1);
    put(5, year);
    put(6, ((z % 7) + 11) % 7);  // 1970-01-01 was a Thursday
    put(7, z - days_from_civil(year, 1, 1));
    put(8, sub * 1000 / kPerSecond);
    put(9, sub * 1000000 / kPerSecond % 1000);
}

// ---- reset. The game registers functions to run before a reset
// (OSRegisterResetFunction: an OSResetFunctionInfo is the function at +0 and
// its priority at +4); OSResetSystem (800E9D64) calls each with FALSE until
// all return TRUE, then each with TRUE, and resets. Here a hot reset or a
// restart starts the program again; a reset to the console's menu closes it.
std::vector<uint32_t> g_reset_functions;  // guest OSResetFunctionInfo, by priority
void OSRegisterResetFunction(Cpu& c) {
    const uint32_t info = c.r[3], priority = rt::peek32(info + 4);
    auto at = g_reset_functions.begin();
    while (at != g_reset_functions.end() && rt::peek32(*at + 4) <= priority) ++at;
    g_reset_functions.insert(at, info);
}
bool call_reset_functions(Cpu& c, bool final) {
    bool all = true;
    for (const uint32_t info : std::vector(g_reset_functions)) {
        c.r[3] = final;
        rt::call(c, rt::peek32(info));
        if (!c.r[3]) all = false;
    }
    return all;
}
[[noreturn]] void leave(const char* why) {
    rt::log("the game chose %s: closing", why);
    before_exit();
    std::fflush(nullptr);
    std::_Exit(0);  // guest threads are parked on host threads: no static destructors
}
void OSResetSystem(Cpu& c) {  // (reset: 0 restart, 1 hot reset, 2 shutdown; resetCode; forceMenu)
    const uint32_t reset = c.r[3], code = c.r[4], menu = c.r[5];
    if (reset > 1) rt::fatal(&c, "OSResetSystem(%u, %08X, %u): only a restart (0) and a hot reset (1) are modelled", reset, code, menu);
    const Cpu saved = c;
    // As the OS's first call: no other thread runs while the reset functions
    // do, though interrupts come (what they wait for), at a lifted loop too.
    constexpr uint32_t kOSDisableScheduler = 0x800EB1B8u;
    rt::call(c, kOSDisableScheduler);
    for (unsigned tries = 0; !call_reset_functions(c, false); ++tries) {
        if (tries == 10000) rt::fatal(&c, "OSResetSystem: a reset function never says it's ready");
        poll_loop(c);  // what it waits for arrives as an interrupt
    }
    set_interrupts_enabled(false);
    call_reset_functions(c, true);
    c = saved;
    if (reset == 1 && menu) leave("the console's menu");
    rt::log("the game chose to reset (code %08X): starting again", code);
    std::fflush(nullptr);
    restart_program();
}
void OSGetResetCode(Cpu& c) { c.r[3] = 0; }  // a cold boot
// The reset switch: the host says when it's held (os.hpp's g_reset_button).
void OSGetResetButtonState(Cpu& c) { c.r[3] = g_reset_button.load(std::memory_order_relaxed) ? 1 : 0; }

const rt::Provided kReplacements[] = {
    // db.c
    {build::kDBInit, "DBInit", DBInit},
    {0x800F07B0u, "DBPrintf", nothing},
    // OS.c
    {build::kOSInit, "OSInit", OSInit},
    {0x800E5A00u, "OSGetConsoleType", OSGetConsoleType},
    {0x800E6398u, "OSRegisterVersion", OSRegisterVersion},
    // EXIUart.c and the debugger's console behind MSL's __write_console
    {0x800C56F4u, "InitializeUART", InitializeUART},
    {0x800C5764u, "WriteUARTN", WriteUARTN},
    {0x801116DCu, "__TRK_write_console (no debugger)", returns_zero},
    {0x80111798u, "__read_console (no debugger: an I/O error)", returns_one},  // stdin's read procedure, in MSL's FILE tables
    // OSError.c, OSFont.c, OSMemory.c
    {0x800E80F8u, "OSPanic", OSPanic},
    {0x800E8224u, "OSSetErrorHandler", OSSetErrorHandler},
    {0x800E8724u, "OSGetFontEncode", OSGetFontEncode},
    {0x800E92D8u, "OSGetPhysicalMemSize", OSGetPhysicalMemSize},
    {0x800E938Cu, "OSProtectRange", OSProtectRange},
    // PPCArch.c
    {0x800A9600u, "PPCMfmsr", PPCMfmsr},
    {0x800A9608u, "PPCMtmsr", PPCMtmsr},
    {0x800A9638u, "PPCSync", PPCSync},
    {0x800A9640u, "PPCHalt", PPCHalt},
    {0x800A96CCu, "PPCMfhid2", PPCMfhid2},
    {0x800A96D4u, "PPCMthid2", PPCMthid2},
    {0x800A96DCu, "PPCMtwpar", PPCMtwpar},
    // OSInterrupt.c
    {0x800E877Cu, "OSDisableInterrupts", OSDisableInterrupts},
    {0x800E8790u, "OSEnableInterrupts", OSEnableInterrupts},
    {0x800E87A4u, "OSRestoreInterrupts", OSRestoreInterrupts},
    {0x800E87C8u, "__OSSetInterruptHandler", __OSSetInterruptHandler},
    {0x800E87E4u, "__OSGetInterruptHandler", __OSGetInterruptHandler},
    {0x800E8B44u, "__OSMaskInterrupts", __OSMaskInterrupts},
    {0x800E8BCCu, "__OSUnmaskInterrupts", __OSUnmaskInterrupts},
    // OSCache.c
    {0x800E7064u, "DCEnable", nothing},
    {0x800E7078u, "DCInvalidateRange", nothing},
    {0x800E70A4u, "DCFlushRange", nothing},
    {0x800E70D4u, "DCStoreRange", nothing},
    {0x800E7104u, "DCFlushRangeNoSync", nothing},
    {0x800E7130u, "DCStoreRangeNoSync", nothing},
    {0x800E715Cu, "DCZeroRange", DCZeroRange},
    {0x800E7188u, "ICInvalidateRange", nothing},
    {0x800E71BCu, "ICFlashInvalidate", nothing},
    {0x800E71CCu, "ICEnable", nothing},
    {0x800E72ACu, "LCEnable", LCEnable},
    {0x800E72E4u, "LCDisable", LCDisable},
    {0x800E730Cu, "LCStoreBlocks", LCStoreBlocks},
    {0x800E7330u, "LCStoreData", LCStoreData},
    {0x800E73DCu, "LCQueueWait", nothing},
    // OSContext.c
    {0x800E7930u, "OSSetCurrentContext", OSSetCurrentContext},
    {0x800E798Cu, "OSGetCurrentContext", OSGetCurrentContext},
    {0x800E7AF0u, "OSGetStackPointer", OSGetStackPointer},
    {0x800E7AF8u, "OSClearContext", OSClearContext},
    {0x800E7F4Cu, "OSFillFPUContext", OSFillFPUContext},
    // OSReset.c, OSResetSW.c
    {0x800E9C28u, "OSRegisterResetFunction", OSRegisterResetFunction},
    {0x800E9D64u, "OSResetSystem", OSResetSystem},
    {0x800EA01Cu, "OSGetResetCode", OSGetResetCode},
    {0x800EA140u, "OSGetResetButtonState", OSGetResetButtonState},
    {0x800EA3D8u, "OSGetResetSwitchState", OSGetResetButtonState},
    // OSRtc.c
    {0x800EA6ACu, "__OSLockSram", __OSLockSram},
    {0x800EA708u, "__OSLockSramEx", __OSLockSramEx},
    {0x800EAAA0u, "__OSUnlockSram", __OSUnlockSram},
    {0x800EAAC4u, "__OSUnlockSramEx", __OSUnlockSramEx},
    {0x800EAAE8u, "__OSSyncSram", __OSSyncSram},
    {0x800EAAF8u, "OSGetSoundMode", OSGetSoundMode},
    {0x800EAB78u, "OSSetSoundMode", OSSetSoundMode},
    {0x800EAC1Cu, "OSGetProgressiveMode", OSGetProgressiveMode},
    {0x800EAC8Cu, "OSSetProgressiveMode", OSSetProgressiveMode},
    {0x800EAD30u, "OSGetWirelessID", OSGetWirelessID},
    {0x800EADB4u, "OSSetWirelessID", OSSetWirelessID},
    {0x800EAE60u, "OSGetGbsMode", OSGetGbsMode},
    {0x800EAED0u, "OSSetGbsMode", OSSetGbsMode},
    // OSTime.c
    {0x800EC33Cu, "OSGetTime", OSGetTime},
    {0x800EC354u, "OSGetTick", OSGetTick},
    {0x800EC35Cu, "__OSGetSystemTime", __OSGetSystemTime},
    {0x800EC5B4u, "OSTicksToCalendarTime", OSTicksToCalendarTime},
};

}  // namespace

void set_console(unsigned language, bool stereo, bool progressive) {
    g_language = language, g_stereo = stereo, g_progressive = progressive;
}

std::span<const rt::Provided> os_replacements() { return kReplacements; }

void post_interrupt(std::function<void(Cpu&)> work) { g_pending.push_back(std::move(work)); }

bool call_interrupt_handler(Cpu& c, uint32_t n) {
    if (g_mask & (0x80000000u >> n)) return false;
    const uint32_t h = rt::peek32(kInterruptTable + 4 * n);
    if (!h) rt::fatal(&c, "interrupt %u has no handler", n);
    guest_callback(c, h, n, rt::peek32(kCurrentContext));
    return true;
}
void raise_interrupt(uint32_t n) {
    post_interrupt([n](Cpu& c) {
        if (!call_interrupt_handler(c, n)) g_held |= 0x80000000u >> n;  // masked: held until it's unmasked
    });
}
bool interrupts_enabled() { return g_enabled; }
uint32_t interrupt_mask() { return g_mask; }

namespace {
const auto g_boot = std::chrono::steady_clock::now();
// On the console the time base counts from 1 January 2000, 00:00 local time
// (the boot ROM sets it from the clock chip and SRAM's counter bias), so
// OSGetTime is the date.
// Here: the PC's local time at start-up, then a steady clock.
const uint64_t g_epoch = [] {
    const std::time_t now = std::time(nullptr);
    const std::tm local = *std::localtime(&now);  // before any other thread
    const int64_t days = days_from_civil(local.tm_year + 1900, unsigned(local.tm_mon + 1), unsigned(local.tm_mday)) -
                         days_from_civil(2000, 1, 1);
    const int64_t seconds = days * 86400 + local.tm_hour * 3600 + local.tm_min * 60 + std::min(local.tm_sec, 59);
    return static_cast<uint64_t>(std::max<int64_t>(seconds, 0)) * kTimerHz;
}();
}  // namespace

// Days since 1970-01-01 of a date in the proleptic Gregorian calendar, and back
// (Howard Hinnant's algorithms: exact for every date, no tables).
int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}
void civil_from_days(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int64_t>(yoe) + era * 400 + (m <= 2);
}

namespace {
std::atomic<int64_t> g_paused_ns{0};  // time spent parked, left out of the time base
std::atomic<bool> g_pause_asked{false};
std::mutex g_pause_lock;
std::condition_variable g_pause_cv;
bool g_parked = false;  // under g_pause_lock
}  // namespace

uint64_t time_base() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_boot).count() -
                    g_paused_ns.load(std::memory_order_relaxed);
    return g_epoch + static_cast<uint64_t>(ns) * 81u / 2000u;  // 40.5 MHz
}

uint64_t steady_ns_at(uint64_t t) {
    const int64_t since_boot = static_cast<int64_t>((t - g_epoch) * 2000u / 81u) + g_paused_ns.load(std::memory_order_relaxed);
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(g_boot.time_since_epoch()).count() + since_boot);
}

void request_pause(bool on) {
    {
        std::lock_guard l(g_pause_lock);
        g_pause_asked.store(on, std::memory_order_relaxed);
    }
    g_pause_cv.notify_all();
}

bool game_parked() {
    std::lock_guard l(g_pause_lock);
    return g_parked;
}

void park_if_requested() {
    if (!g_pause_asked.load(std::memory_order_relaxed)) return;
    std::unique_lock l(g_pause_lock);
    const auto t0 = std::chrono::steady_clock::now();
    g_parked = true;
    g_pause_cv.wait(l, [] { return !g_pause_asked.load(std::memory_order_relaxed); });
    g_parked = false;
    g_paused_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(),
                          std::memory_order_relaxed);
}

void add_timer_source(std::function<void()> poll, std::function<uint64_t()> deadline) {
    g_timers.push_back({std::move(poll), std::move(deadline)});
}
void on_wpar_write(void (*reset)()) { g_wpar_reset = reset; }

#if defined(_WIN32)
// Windows: an event for wakes and, per thread, a high-resolution waitable
// timer (Windows 10 1803 and later) for the deadline: it ends within about
// half a millisecond of the time asked, where a condition variable's wait
// ends on the 1 ms timer tick, up to 2 ms late. So the yielding before a
// retrace is 0.5 ms of a core instead of 1.5.
namespace {
std::atomic<bool> g_idle_woken{false};
HANDLE idle_event() {
    static const HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    return e;
}
struct IdleTimer {
    HANDLE h = nullptr;
    bool fine = false;
    IdleTimer() {
        h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        fine = h != nullptr;
        if (!h) h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);  // older Windows: the 1 ms tick's
    }
};
}  // namespace
static void idle_sleep_for(uint64_t ns, bool on_time) {
    using clock = std::chrono::steady_clock;
    const auto end = clock::now() + std::chrono::nanoseconds(ns);
    if (g_idle_woken.exchange(false, std::memory_order_acq_rel)) return;
    thread_local IdleTimer timer;
    const auto slack = timer.fine ? std::chrono::microseconds(500) : std::chrono::microseconds(1500);
    const auto os_end = on_time ? end - slack : end;
    const HANDLE event = idle_event();
    for (auto now = clock::now(); now < os_end && timer.h && event; now = clock::now()) {
        LARGE_INTEGER due;
        due.QuadPart = -std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(os_end - now).count() / 100);
        if (!SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) break;
        const HANDLE both[2] = {event, timer.h};
        const DWORD r = WaitForMultipleObjects(2, both, FALSE, INFINITE);
        if (g_idle_woken.exchange(false, std::memory_order_acq_rel)) return;
        if (r != WAIT_OBJECT_0) break;  // the timer (or a failure): on to the last stretch
        // The event without the flag: a wake already taken. Wait on.
    }
    if (!on_time) return;
    while (clock::now() < end) {  // the last stretch, as below
        if (g_idle_woken.exchange(false, std::memory_order_acq_rel)) return;
        std::this_thread::yield();
    }
}
void wake_idle() {
    rt::g_poll_due.store(true, std::memory_order_relaxed);  // (a guest thread busy in a loop sees it at its next back edge)
    g_idle_woken.store(true, std::memory_order_release);
    if (const HANDLE e = idle_event()) SetEvent(e);
}
#else
namespace {
std::mutex g_idle_lock;
std::condition_variable g_idle_cv;
std::atomic<bool> g_idle_woken{false};  // written under g_idle_lock; read without it while yielding
// How early an OS sleep may end up late: within ~100 us.
constexpr auto kSleepSlack = std::chrono::microseconds(250);
}  // namespace
static void idle_sleep_for(uint64_t ns, bool on_time) {
    using clock = std::chrono::steady_clock;
    const auto end = clock::now() + std::chrono::nanoseconds(ns);
    {
        std::unique_lock l(g_idle_lock);
        const auto os_end = on_time ? end - kSleepSlack : end;
        if (clock::now() < os_end)
            g_idle_cv.wait_until(l, os_end, [] { return g_idle_woken.load(std::memory_order_relaxed); });
        if (g_idle_woken.exchange(false, std::memory_order_relaxed) || !on_time) return;
    }
    // The last stretch: yield until the deadline or a wake. A retrace posted
    // a millisecond late is a frame paced a millisecond late; this costs at
    // most kSleepSlack of a core per deadline.
    while (clock::now() < end) {
        if (g_idle_woken.load(std::memory_order_relaxed)) {
            std::lock_guard l(g_idle_lock);
            g_idle_woken.store(false, std::memory_order_relaxed);
            return;
        }
        std::this_thread::yield();
    }
}
void wake_idle() {
    rt::g_poll_due.store(true, std::memory_order_relaxed);  // (a guest thread busy in a loop sees it at its next back edge)
    {
        std::lock_guard l(g_idle_lock);
        g_idle_woken.store(true, std::memory_order_relaxed);
    }
    g_idle_cv.notify_all();
}
#endif

namespace {
std::atomic<uint64_t> g_idle_ns{0};
}
void idle_sleep(uint64_t ns, bool on_time) {
    const auto t0 = std::chrono::steady_clock::now();
    idle_sleep_for(ns, on_time);
    g_idle_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                        std::memory_order_relaxed);
}
uint64_t idle_time_ns() { return g_idle_ns.load(std::memory_order_relaxed); }

// ---- the poll clock (start_poll_clock): its thread sets rt::g_poll_due for
// lifted loops when the next deadline comes. The guest's thread publishes the
// deadline wherever it works it out; the clock waits for it, woken early if a
// sooner one is published.
namespace {
std::atomic<uint64_t> g_poll_points{0}, g_clock_sets{0};
std::atomic<uint64_t> g_due_at{UINT64_MAX};  // the next deadline, as the guest's thread last published it
std::atomic<uint64_t> g_published_at{0};     // when (time base)
std::atomic<uint64_t> g_clock_until{0};      // what the clock waits for
std::atomic<bool> g_clock_running{false};
#if defined(_WIN32)
HANDLE clock_event() {
    static const HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    return e;
}
void wake_clock() {
    if (const HANDLE e = clock_event()) SetEvent(e);
}
#else
// Never destroyed: the clock thread is detached and may be waiting on them at
// exit, where glibc's pthread_cond_destroy would block (see gpu.cpp).
std::mutex& g_clock_lock = *new std::mutex;
std::condition_variable& g_clock_cv = *new std::condition_variable;
bool g_clock_woken = false;  // under g_clock_lock
void wake_clock() {
    {
        std::lock_guard l(g_clock_lock);
        g_clock_woken = true;
    }
    g_clock_cv.notify_one();
}
#endif
void publish_deadline(uint64_t t, uint64_t now) {
    const uint64_t was = g_due_at.exchange(t, std::memory_order_relaxed);
    g_published_at.store(now, std::memory_order_relaxed);
    if (t < was && t < g_clock_until.load(std::memory_order_relaxed) && g_clock_running.load(std::memory_order_relaxed)) wake_clock();
}
}  // namespace

void note_deadline(uint64_t t) {
    if (t < g_due_at.load(std::memory_order_relaxed)) publish_deadline(t, time_base());
}

uint64_t next_timer_deadline() {
    uint64_t t = UINT64_MAX;
    for (const auto& timer : g_timers) t = std::min(t, timer.deadline());
    publish_deadline(t, time_base());
    return t;
}
bool has_pending_interrupts() { return !g_pending.empty(); }
void set_interrupts_enabled(bool on) { g_enabled = on; }
bool in_interrupt() { return g_in_interrupt; }

namespace {
bool run_pending(Cpu& c) {
    for (const auto& timer : g_timers) timer.poll();
    bool ran = false;
    while (!g_pending.empty()) {
        auto work = std::move(g_pending.front());
        g_pending.pop_front();
        g_enabled = false;  // handlers run with interrupts off
        g_in_interrupt = true;
        work(c);
        g_in_interrupt = false;
        g_enabled = true;
        ran = true;
    }
    if (ran) next_timer_deadline();  // handlers set timers (the next audio DMA): published for the poll clock
    return ran;
}
}  // namespace

void deliver_interrupts(Cpu& c) {
    if (!g_enabled || g_in_interrupt) return;
    if (run_pending(c)) preempt(c);
}

void poll_loop(Cpu& c) {
    if (g_enabled && !g_in_interrupt) {
        if (run_pending(c)) {
            preempt(c);
            return;
        }
    } else {
        for (const auto& timer : g_timers) timer.poll();  // the hardware's state moves on; delivery waits
    }
    // Nothing came: wait for the next timed event, a millisecond at most, rather than spin.
    // Only a timer's own deadline needs meeting on time; the millisecond is a bound.
    const uint64_t now = time_base(), timer = next_timer_deadline(), next = std::min(timer, now + kTimerHz / 1000);
    if (next > now) idle_sleep((next - now) * 2000 / 81, next == timer);
}

uint64_t poll_point_count() { return g_poll_points.load(std::memory_order_relaxed); }
uint64_t poll_clock_count() { return g_clock_sets.load(std::memory_order_relaxed); }

void poll_point(Cpu& c) {
    rt::g_poll_due.store(false, std::memory_order_relaxed);
    g_poll_points.fetch_add(1, std::memory_order_relaxed);
    deliver_interrupts(c);
    next_timer_deadline();  // published: the clock's next wait
}

namespace {
// Wait until the time base reaches `until`, or wake_clock.
void clock_wait(uint64_t until) {
    const uint64_t now = time_base();
    if (until <= now) return;
    const int64_t ns = static_cast<int64_t>(std::min<uint64_t>(until - now, kTimerHz) * 2000 / 81);
#if defined(_WIN32)
    // A high-resolution waitable timer (as idle_sleep_for's): within half a
    // millisecond of the deadline, where a sleep ends on the 1 ms tick.
    thread_local IdleTimer timer;
    const HANDLE event = clock_event();
    LARGE_INTEGER due;
    due.QuadPart = -std::max<int64_t>(1, ns / 100);
    if (!timer.h || !event || !SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        return;
    }
    const HANDLE both[2] = {event, timer.h};
    WaitForMultipleObjects(2, both, FALSE, INFINITE);
#else
    std::unique_lock l(g_clock_lock);
    g_clock_cv.wait_for(l, std::chrono::nanoseconds(ns), [] { return g_clock_woken; });
    g_clock_woken = false;
#endif
}

void clock_main() {
    set_this_thread_priority(Priority::high);  // late, it makes a deadline late
    constexpr uint64_t kRetry = kTimerHz / 1000, kStale = kTimerHz / 250;  // 1 ms, 4 ms
    const auto set = [] {
        rt::g_poll_due.store(true, std::memory_order_relaxed);
        g_clock_sets.fetch_add(1, std::memory_order_relaxed);
    };
    for (uint64_t flagged_stale = UINT64_MAX;;) {
        const uint64_t now = time_base(), due = g_due_at.load(std::memory_order_relaxed);
        const uint64_t published = g_published_at.load(std::memory_order_relaxed);
        uint64_t until;
        if (now >= due) {
            // Due: the flag, and again a millisecond after the guest takes it
            // while it stays undelivered (the guest has interrupts off). Not
            // taken: the guest's thread is asleep or parked, and wakes itself.
            if (rt::g_poll_due.load(std::memory_order_relaxed)) {
                until = now + kStale;
            } else {
                set();
                until = now + kRetry;
            }
        } else if (now >= published + kStale && flagged_stale != published) {
            // Nothing published for 4 ms: a timer may have been set since
            // without a word (an audio DMA started by the game's own code).
            set();
            flagged_stale = published;
            until = due;
        } else {
            until = flagged_stale == published ? due : std::min(due, published + kStale);
        }
        g_clock_until.store(until, std::memory_order_relaxed);
        clock_wait(until);
    }
}
}  // namespace

void start_poll_clock() {
    // MKDD_POLL=0: no safe points in loops, as before them; MKDD_POLL=ms: the
    // flag every millisecond, deadline or not (development: to measure against).
    const char* mode = std::getenv("MKDD_POLL");
    if (mode && *mode == '0') return;
    if (mode && std::strcmp(mode, "ms") == 0) {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                rt::g_poll_due.store(true, std::memory_order_relaxed);
                g_clock_sets.fetch_add(1, std::memory_order_relaxed);
            }
        }).detach();
        return;
    }
    g_clock_running.store(true, std::memory_order_relaxed);
    std::thread(clock_main).detach();
}

bool deliver_interrupts_idle(Cpu& c) {
    const bool was = g_enabled;
    g_enabled = true;
    const bool ran = run_pending(c);
    g_enabled = was;
    return ran;
}

void guest_callback(Cpu& c, uint32_t fn, uint32_t a0, uint32_t a1) {
    const Cpu saved = c;
    // A frame of its own below the interrupted code's, chained to it for backtraces.
    c.r[1] = (saved.r[1] - 0x100u) & ~15u;
    rt::poke32(c.r[1], saved.r[1]);
    c.r[3] = a0;
    c.r[4] = a1;
    c.lr = 0;
    rt::call(c, fn);
    c = saved;
}

}  // namespace mkdd
