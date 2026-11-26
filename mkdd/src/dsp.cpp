// The DSP and the audio interface, as registers (GameCube).
//
// DSP (0xCC005000, 16-bit registers; a 32-bit access is two of them, high
// half first):
//   00/02 mail to the DSP (writing the low half sends it; bit 15 of the high
//   half reads set until the DSP has taken it), 04/06 mail from the DSP (bit
//   15 set while unread; reading the low half takes it),
//   0A control: 0 reset, 1 interrupt the DSP, 2 halt, 3/4 AI DMA flag/enable,
//   5/6 ARAM flag/enable, 7/8 DSP flag/enable, 9 ARAM DMA busy, 10 init code,
//   11 init. Writing 1 to a flag clears it. The PI's DSP line is raised while
//   any flag is set with its enable.
//   12 ARAM size, 16 mode (bit 0 reads set: ready), 1A refresh,
//   20/22 ARAM DMA main-memory address, 24/26 its ARAM address, 28/2A its
//   length in bytes (bit 15 of 28: from ARAM); writing 2A does the copy at once
//   and raises the ARAM flag,
//   30/32 audio DMA source, 36 bit 15 enable and bits 0-14 its length in
//   32-byte blocks, 3A blocks left.
//
// The DSP's program is run, not imitated: the game's microcode is lifted to
// C++ at build time (recomp/src/dsp) and runs on dsp_core.hpp's DSP each time
// the CPU gives it something to do (a mail sent or taken, the interrupt, the
// halt released) and until it can only wait again: inside the register access,
// on the game's thread, so the game's busy-waits end at once and a run is the
// same every time. MKDD_DSP_THREAD=1 runs it on a host thread of its own
// instead ("the DSP's thread", below): an experiment, off by default, because
// it only pays where a core is free for it (docs/audio.md has the numbers). Only the DSP's ROM is ours: its task
// loader (the mails 80F3xxxx that hand it a microcode; the entry at 0x80B5 a
// microcode jumps to when the CPU switches tasks; reset at 0x8000), which
// finds the lifted program by the image's hash, and the stub
// __OSInitAudioSystem uploads, which only has to answer.
//
// ARAM, the 16 MB behind the DSP, is here (aram_base): the microcode reads
// and writes it through the accelerator, the AR library through the DMA
// registers above or directly.
//
// The audio DMA plays 32-byte blocks (8 stereo samples, right then left,
// big-endian) at the AI's DMA rate; when the last block of a buffer has
// played it reloads from the source registers and raises the AI DMA flag.
// JAudio answers that flag by queueing the next buffer and asking the DSP
// for the next frame, so this clock drives all of the sound.
//
// AI (0xCC006C00, 32-bit): 00 control (0 counter run, 1 stream rate,
// 2 interrupt enable, 3 interrupt flag, 4 interrupt valid, 5 counter reset,
// 6 DMA rate: 1 32 kHz, 0 48 kHz), 04 volume, 08 sample counter, 0C
// interrupt timing.
//
// After Dolphin's DSP.cpp, AudioInterface.cpp and its notes on the ROM: the
// register map and the loader's protocol are facts taken from there; no code is.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "audio.hpp"
#include "dsp_core.hpp"
#include "hw.hpp"
#include "os.hpp"
#include "priority.hpp"
#include "qilin/rt/report.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

constexpr uint16_t kReset = 1u << 0, kHalt = 1u << 2, kAid = 1u << 3, kAram = 1u << 5, kDspFlag = 1u << 7;
constexpr uint16_t kInitCode = 1u << 10, kInit = 1u << 11;
constexpr uint16_t kFlags = kAid | kAram | kDspFlag;
constexpr uint16_t kMicrocodeBits = 0x0C07;  // reset, assert, halt, init code, init: the DSP's side

AudioStats g_stats;
bool g_trace = false;  // MKDD_AUDIO_DUMP or MKDD_AUDIO_TRACE
AudioSink g_sink = nullptr;
void* g_sink_user = nullptr;

// The DSP's side.
enum class Microcode { Rom, Init, Lifted };
std::atomic<Microcode> g_ucode{Microcode::Rom};
dsp::Core g_core;
std::unique_ptr<uint8_t[]> g_aram;
uint16_t g_dsp_bits = kHalt | kInit;  // control as the DSP keeps it
uint64_t g_init_code_clear = 0;      // time base at which the init code bit reads clear
uint32_t g_greeting = 0;             // the ROM's or the stub's first mail, due when the DSP next runs
uint16_t g_cpu_mail_high = 0;        // the CPU's mailbox, high half as written

// The ROM's task loader: parameter mails, then the boot.
uint32_t g_rom_param = 0, g_rom_iram = 0, g_rom_length = 0;

// The CPU's side.
uint16_t g_control = 0;
bool g_line_posted = false;
uint16_t g_ar[16] = {};  // ARAM registers 0x5012..0x502A as written

// Audio DMA.
uint32_t g_dma_source = 0;
uint16_t g_dma_control = 0;  // bit 15 enable, bits 0-14 blocks
uint32_t g_dma_current = 0, g_dma_blocks = 0;
uint64_t g_dma_t0 = 0;       // time base at the start of the playing buffer
uint32_t g_dma_rate = 32000;

// AI.
uint32_t g_ai_control = 0, g_ai_volume = 0, g_ai_counter = 0, g_ai_timing = 0;
uint64_t g_ai_counter_t0 = 0;

// Lateness, to tell a busy host from a fault of ours (time-base ticks; the worst since last asked):
// the longest stretch without the timers being polled at all (the game's thread not running, or
// running with no safe point), how late a finished DMA buffer was noticed, and how long JAudio
// took from the AI DMA interrupt to asking the DSP for the next frame.
uint64_t g_last_poll = 0, g_poll_gap_worst = 0, g_dma_late_worst = 0, g_aid_raised = 0, g_request_worst = 0;

uint64_t dma_end() { return g_dma_t0 + uint64_t(g_dma_blocks) * 8 * kTimerHz / g_dma_rate; }
unsigned ai_stream_rate() { return (g_ai_control & 2) ? 48000 : 32000; }

// --- interrupts ---

uint32_t os_interrupt_for(uint16_t flag) { return flag == kAid ? 5 : flag == kAram ? 6 : 7; }
uint16_t active() { return (g_control >> 1) & g_control & kFlags; }

void service(Cpu& c) {
    for (int round = 0; round < 64; ++round) {
        const uint16_t a = active();
        if (!a) break;
        bool ran = false;
        for (uint16_t flag : {kAid, kAram, kDspFlag})
            if ((a & flag) && call_interrupt_handler(c, os_interrupt_for(flag))) {
                ran = true;
                break;
            }
        if (!ran) break;  // masked by the OS: poll() posts it again when that changes
        if (round == 63) rt::fatal(&c, "the DSP interrupt (control %04X) is still raised after its handler ran 64 times", g_control);
    }
    g_line_posted = false;
}
void update_line() {
    if (g_line_posted || !active()) return;
    g_line_posted = true;
    post_interrupt(service);
}
void raise_flag(uint16_t flag) {
    g_control |= flag;
    update_line();
}

// --- the DSP's thread ---
//
// One worker runs the microcode whenever the game's side has kicked it, and parks when the
// microcode can only wait. What the game can see of the DSP stays exact where the game looks:
// a read of either mailbox's "waiting" bit first lets the DSP get as far as it can by itself
// (settle_until), so a busy-wait on a mail ends on its first poll, as when the microcode ran
// inside the register access. What changes is only when the DSP's interrupt arrives: when the
// work is done, at the game thread's next safe point, instead of at once.
// After working, the worker spins for a moment before it sleeps: the game's side sends its
// mails in bursts, and waking a sleeping thread for each would cost more than the work.

bool g_threaded = false;
// Never destroyed: the worker is detached and may be waiting on them at exit,
// where glibc's pthread_cond_destroy would block (see gpu.cpp).
std::mutex& g_dsp_mutex = *new std::mutex;
std::condition_variable& g_dsp_wake = *new std::condition_variable;
bool g_dsp_asleep = false;                // (under the mutex) the worker waits on g_dsp_wake
std::atomic<bool> g_dsp_work{false};      // something for the microcode to look at
std::atomic<bool> g_dsp_busy{false};      // work pending or the worker inside the microcode
std::atomic<bool> g_dsp_irq{false};       // the microcode asked for the CPU's interrupt
std::atomic<uint64_t> g_dsp_ns{0}, g_dsp_runs{0}, g_dsp_mails_out{0};
uint64_t g_settle_ns = 0, g_settle_worst_ns = 0, g_wakes = 0;  // (the game's thread's)

void run_core() {
    g_dsp_runs.fetch_add(1, std::memory_order_relaxed);
    if (g_trace) {
        const auto t0 = std::chrono::steady_clock::now();
        g_core.run();
        g_dsp_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                           std::memory_order_relaxed);
    } else {
        g_core.run();
    }
}

void dsp_thread() {
    set_this_thread_priority(Priority::high);
    rt::set_thread_role("the DSP worker (the sound microcode)");
    for (;;) {
        if (g_dsp_work.exchange(false)) {
            run_core();
            if (!g_dsp_work.load()) g_dsp_busy.store(false);
            // (A kick between those two lines sets busy again after work: see kick().)
            if (g_dsp_work.load()) g_dsp_busy.store(true);
            continue;
        }
        // Nothing to do: stay awake for a quarter of a millisecond, then sleep.
        const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(250);
        bool found = false;
        while (std::chrono::steady_clock::now() < until) {
            for (int i = 0; i < 64 && !found; ++i) found = g_dsp_work.load(std::memory_order_relaxed);
            if (found) break;
            std::this_thread::yield();
        }
        if (found) continue;
        std::unique_lock lock(g_dsp_mutex);
        g_dsp_asleep = true;
        g_dsp_wake.wait(lock, [] { return g_dsp_work.load(); });
        g_dsp_asleep = false;
    }
}
// The game's side changed something the microcode may be waiting for.
void kick() {
    g_dsp_busy.store(true);
    g_dsp_work.store(true);
    bool asleep;
    {
        std::lock_guard lock(g_dsp_mutex);
        asleep = g_dsp_asleep;
    }
    if (asleep) {
        ++g_wakes;
        g_dsp_wake.notify_one();
    }
}
// Wait until `done()` or the DSP can do no more by itself.
template <class Done>
void settle_until(Done done) {
    if (!g_threaded || done() || !g_dsp_busy.load()) return;
    const auto t0 = g_trace ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    for (unsigned spins = 0; !done() && g_dsp_busy.load(); ++spins)
        if (spins > 200) std::this_thread::yield();
    if (g_trace) {
        const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        g_settle_ns += ns;
        g_settle_worst_ns = std::max(g_settle_worst_ns, ns);
    }
}
// Stop the worker inside the microcode, wherever it is, for a change it must not run across (reset).
void settle_stopped() {
    if (!g_threaded) return;
    g_core.stop = true;
    settle_until([] { return false; });
    g_core.stop = false;
}

// --- the DSP's side ---

constexpr uint16_t kAssert = 1u << 1;

void mail_from_dsp(uint32_t mail) {
    g_dsp_mails_out.fetch_add(1, std::memory_order_relaxed);
    g_core.dsp_mail = mail | 0x80000000u;
}

void set_microcode(Microcode m) {
    g_ucode = m;
    g_core.program = nullptr;
    g_core.dsp_mail = 0;
    g_core.cpu_mail = 0;
    g_core.ext_int = false;
    g_rom_param = 0;
    // The ROM's loader greets, and so does the stub the OS uploads to try the DSP; each when it runs.
    g_greeting = m == Microcode::Rom ? 0x8071FEEDu : 0x80544348u;
}

const dsp::Program* find_program(Cpu* c, uint32_t address, uint32_t length, uint32_t* hash) {
    *hash = dsp::ector_hash(dsp::physical(c, address, length), length);
    for (size_t i = 0; i < dsp::g_program_count; ++i)
        if (dsp::g_programs[i].hash == *hash && dsp::g_programs[i].bytes == length) return &dsp::g_programs[i];
    return nullptr;
}
void load_image(Cpu* c, const dsp::Program* p, uint32_t address) {
    const uint8_t* image = dsp::physical(c, address, p->bytes);
    std::memset(g_core.iram, 0, sizeof g_core.iram);
    for (uint32_t i = 0; i < p->bytes / 2; ++i) g_core.iram[i] = static_cast<uint16_t>((image[2 * i] << 8) | image[2 * i + 1]);
}

// Have the microcode run until it can only wait for the CPU (or halts): on its thread, or here.
void run_dsp() {
    if (g_core.halted) return;
    if (g_greeting) mail_from_dsp(std::exchange(g_greeting, 0));
    if (g_ucode != Microcode::Lifted) return;
    if (g_threaded) kick();
    else run_core();
}

void rom_mail(Cpu& c, uint32_t mail) {
    if (g_rom_param == 0) {
        if ((mail & 0xFFFF0000u) == 0x80F30000u) g_rom_param = mail;
        else mail_from_dsp(0xFEEE0000u | (mail & 0xFFFF));
        return;
    }
    const uint32_t param = std::exchange(g_rom_param, 0);
    switch (param) {
        case 0x80F3A001: g_rom_iram = mail; break;           // microcode in main memory
        case 0x80F3A002: g_rom_length = mail & 0xFFFF; break;  // its length in bytes
        case 0x80F3B002:                                      // data memory to load: JAudio's needs none
            if (mail & 0xFFFF) rt::fatal(&c, "a DSP task asks for %u bytes of data memory at boot: not handled", mail & 0xFFFF);
            break;
        case 0x80F3C002:  // where it goes in instruction memory
            if (mail & 0xFFFF) rt::fatal(&c, "a DSP task loads at %04X in instruction memory: not handled", mail & 0xFFFF);
            break;
        case 0x80F3D001: {  // its start: boot it
            uint32_t hash = 0;
            const dsp::Program* p = find_program(&c, g_rom_iram, g_rom_length, &hash);
            g_stats.ucode_crc = hash;
            if (!p)
                rt::fatal(&c, "the DSP was given a microcode that was not lifted into this build (hash %08X, %u bytes at %08X)", hash,
                          g_rom_length, g_rom_iram);
            rt::log("dsp: microcode %08X (%s) booted at %04X", hash, p->name, mail & 0xFFFF);
            settle_stopped();
            g_ucode = Microcode::Lifted;
            const bool halted = g_core.halted;
            g_core.boot(p, static_cast<uint16_t>(mail));
            g_core.halted = halted;
            load_image(&c, p, g_rom_iram);
            run_dsp();
            break;
        }
        default: rt::fatal(&c, "the DSP's boot loader got parameter %08X: not known", param);
    }
}

// The microcode jumped into the ROM. 0x8000 is reset. 0x80B5 is the loader's entry for a task
// switch, with what the CPU mailed the old task in registers: the new image's address in main
// memory (ix0:ix1), length (ix3) and place (ix2), its start (ar0), and data memory to load first
// (ax0 the address, ax1.l the length, ax1.h the place): __DSP_exec_task's order.
bool rom_entry(void*, dsp::Core& core, uint16_t address) {
    if (address == 0x8000) {
        set_microcode(Microcode::Rom);
        mail_from_dsp(std::exchange(g_greeting, 0));
        return false;
    }
    if (address != 0x80B5) rt::fatal(nullptr, "the DSP microcode jumped to %04X in the DSP's ROM: only the loader's entries are known", address);
    const dsp::Regs& r = core.r;
    const uint32_t image = (uint32_t(r.ix0) << 16) | r.ix1, length = r.ix3;
    if (r.ix2) rt::fatal(nullptr, "a DSP task loads at %04X in instruction memory: not handled", r.ix2);
    uint32_t hash = 0;
    const dsp::Program* p = find_program(nullptr, image, length, &hash);
    g_stats.ucode_crc = hash;
    if (!p)
        rt::fatal(nullptr, "the DSP was switched to a microcode that was not lifted into this build (hash %08X, %u bytes at %08X)", hash, length,
                  image);
    load_image(nullptr, p, image);
    const uint32_t data = (uint32_t(r.axh0) << 16) | r.axl0, data_words = r.axl1 / 2u, at = r.axh1;
    if (data_words) {
        if (at + data_words > 0x1000) rt::fatal(nullptr, "a DSP task's saved data memory (%u words at %04X) doesn't fit", data_words, at);
        const uint8_t* d = dsp::physical(nullptr, data, data_words * 2);
        for (uint32_t i = 0; i < data_words; ++i) core.dram[at + i] = static_cast<uint16_t>((d[2 * i] << 8) | d[2 * i + 1]);
    }
    core.program = p;
    core.pc = r.ar0;
    return true;
}

void mail_to_dsp(Cpu& c, uint32_t mail) {
    ++g_stats.mails_in;
    if (g_trace) dsp::trace::mail_to_dsp(mail, g_ucode == Microcode::Lifted);
    switch (g_ucode) {
        case Microcode::Rom: rom_mail(c, mail); break;
        case Microcode::Init: break;
        case Microcode::Lifted:
            g_core.cpu_mail = mail | 0x80000000u;
            run_dsp();
            break;
    }
}

uint16_t read_mail_low() {
    const uint32_t mail = g_core.dsp_mail.load();
    if (mail & 0x80000000u) {
        if (g_trace) dsp::trace::mail_from_dsp(mail);
        g_core.dsp_mail.fetch_and(0x7FFFFFFFu);
        run_dsp();  // it may have been waiting for exactly this
    }
    return static_cast<uint16_t>(mail);
}

uint16_t microcode_control_write(uint16_t v) {
    if (g_trace && (v & (kReset | kAssert) || ((v ^ g_dsp_bits) & (kHalt | kInit)))) dsp::trace::event("control written", v);
    if ((v & (kReset | kHalt)) || ((g_dsp_bits & kInit) && !(v & kInit))) settle_stopped();
    if (v & kReset) {
        set_microcode(Microcode::Rom);
        v &= ~kReset;
    }
    if ((g_dsp_bits & kInit) && !(v & kInit)) {
        set_microcode(Microcode::Init);
        v |= kInitCode;
        g_init_code_clear = time_base() + 130;
    }
    g_dsp_bits = v;
    g_core.halted = (v & kHalt) != 0;
    if (v & kAssert) {
        if (g_ucode == Microcode::Lifted) {
            g_core.ext_int = true;
            ++g_stats.command_lists;
            if (g_trace) dsp::trace::interrupt_dsp(g_core.cpu_mail);
            if (g_aid_raised && (g_core.cpu_mail.load() & 0xFFFF)) {  // a command: the next frame's request
                g_request_worst = std::max(g_request_worst, time_base() - g_aid_raised);
                g_aid_raised = 0;
            }
        } else {
            g_dsp_bits &= ~kAssert;
        }
    }
    run_dsp();
    return g_dsp_bits;
}
uint16_t microcode_control_read() {
    if ((g_dsp_bits & kInitCode) && time_base() >= g_init_code_clear) g_dsp_bits &= ~kInitCode;
    if (g_ucode == Microcode::Lifted) {  // the interrupt reads set until the microcode takes it; HALT may be its own
        g_dsp_bits = static_cast<uint16_t>((g_dsp_bits & ~(kAssert | kHalt)) | (g_core.ext_int ? kAssert : 0) | (g_core.halted ? kHalt : 0));
    }
    return g_dsp_bits;
}

// --- ARAM DMA ---

void aram_dma(Cpu& c) {
    const uint32_t main_address = (uint32_t(g_ar[7]) << 16) | g_ar[8], aram_address = (uint32_t(g_ar[9]) << 16) | g_ar[10];
    const uint32_t count = ((uint32_t(g_ar[11]) << 16) | g_ar[12]) & 0x7FFFFFFFu;
    const bool from_aram = (g_ar[11] & 0x8000) != 0;
    if (count) {
        uint8_t* m = dsp::physical(&c, main_address, count);
        uint8_t* a = aram_base();
        for (uint32_t done = 0; done < count;) {  // (the address wraps at ARAM's size)
            const uint32_t at = (aram_address + done) & static_cast<uint32_t>(kAramSize - 1);
            const uint32_t n = std::min<uint32_t>(count - done, static_cast<uint32_t>(kAramSize) - at);
            if (from_aram) std::memcpy(m + done, a + at, n);
            else std::memcpy(a + at, m + done, n);
            done += n;
        }
    }
    const uint32_t m2 = main_address + count, a2 = aram_address + count;
    g_ar[7] = static_cast<uint16_t>(m2 >> 16), g_ar[8] = static_cast<uint16_t>(m2);
    g_ar[9] = static_cast<uint16_t>(a2 >> 16), g_ar[10] = static_cast<uint16_t>(a2);
    g_ar[11] &= 0x8000, g_ar[12] = 0;
    raise_flag(kAram);
}

// --- audio DMA ---

void play(Cpu* c, uint32_t source, uint32_t blocks) {
    ++g_stats.dma_periods;
    const uint32_t frames = blocks * 8;
    g_stats.frames += frames;
    if (!g_sink && !g_trace) return;
    const uint8_t* p = dsp::physical(c, source, blocks * 32);
    static thread_local std::vector<int16_t> out;
    out.resize(size_t(frames) * 2);
    for (uint32_t i = 0; i < frames; ++i) {  // right, left in the buffer
        const auto s = [&](uint32_t k) { return static_cast<int16_t>((p[k] << 8) | p[k + 1]); };
        out[2 * i] = s(4 * i + 2);
        out[2 * i + 1] = s(4 * i);
    }
    if (g_trace) dsp::trace::samples(out.data(), frames, g_dma_rate);
    if (g_sink) g_sink(out.data(), frames, g_dma_rate, g_sink_user);
}

bool dma_on() { return (g_dma_control & 0x8000) != 0; }

void poll() {
    if (g_dsp_irq.load(std::memory_order_relaxed) && g_dsp_irq.exchange(false)) raise_flag(kDspFlag);
    if (active() && !g_line_posted) {  // raised while the OS had it masked
        const uint32_t mask = interrupt_mask();
        for (uint16_t flag : {kAid, kAram, kDspFlag})
            if ((active() & flag) && !(mask & (0x80000000u >> os_interrupt_for(flag)))) {
                update_line();
                break;
            }
    }
    if (!dma_on()) return;
    const uint64_t now = time_base();
    const uint64_t gap = g_last_poll ? now - g_last_poll : 0;
    g_last_poll = now;
    g_poll_gap_worst = std::max(g_poll_gap_worst, gap);
    if (now >= dma_end() + kTimerHz / 10) {  // more than 100 ms behind (a stall): start again from now
        rt::log("audio: %llu ms behind, skipping ahead (the timers were last polled %llu ms before this: %s)",
                static_cast<unsigned long long>((now - dma_end()) * 1000 / kTimerHz), static_cast<unsigned long long>(gap * 1000 / kTimerHz),
                gap >= kTimerHz / 20 ? "the game's thread did not run, or ran without a safe point" : "the DMA fell behind while the timers were polled");
        g_dma_t0 = now;
        return;
    }
    if (now < dma_end()) return;
    g_dma_late_worst = std::max(g_dma_late_worst, now - dma_end());
    g_aid_raised = now;
    play(nullptr, g_dma_current, g_dma_blocks);
    g_dma_t0 = dma_end();
    g_dma_current = g_dma_source;
    g_dma_blocks = g_dma_control & 0x7FFF;
    if (!g_dma_blocks) rt::fatal(nullptr, "the audio DMA reloaded a buffer of 0 blocks");
    raise_flag(kAid);
}
uint64_t deadline() {
    uint64_t t = UINT64_MAX;
    if (dma_on()) t = dma_end();
    return t;
}

// --- registers ---

uint16_t dsp_read16(Cpu& c, uint32_t off) {
    switch (off) {
        // The "waiting" bits are what the game busy-waits on: first let the DSP get as far as it can.
        case 0x00:
            settle_until([] { return !(g_core.cpu_mail.load(std::memory_order_relaxed) & 0x80000000u); });
            return static_cast<uint16_t>(g_core.cpu_mail.load() >> 16);
        case 0x02: return static_cast<uint16_t>(g_core.cpu_mail.load());
        case 0x04:
            settle_until([] { return (g_core.dsp_mail.load(std::memory_order_relaxed) & 0x80000000u) != 0; });
            return static_cast<uint16_t>(g_core.dsp_mail.load() >> 16);
        case 0x06: return read_mail_low();
        case 0x0A: return (g_control & ~kMicrocodeBits) | (microcode_control_read() & kMicrocodeBits);
        case 0x30: return static_cast<uint16_t>(g_dma_source >> 16);
        case 0x32: return static_cast<uint16_t>(g_dma_source);
        case 0x36: return g_dma_control;
        case 0x3A: {
            if (!dma_on()) return 0;
            const uint64_t now = time_base();
            const uint64_t played = now <= g_dma_t0 ? 0 : (now - g_dma_t0) * g_dma_rate / (8 * kTimerHz);
            const uint64_t left = played >= g_dma_blocks ? 0 : g_dma_blocks - played;
            return static_cast<uint16_t>(left > 0 ? left - 1 : 0);
        }
        default:
            if (off == 0x16) return g_ar[2] | 1;  // ARAM is ready
            if (off >= 0x12 && off < 0x2C) return g_ar[(off - 0x12) / 2];
            rt::fatal(&c, "load from DSP register %04X: not handled", off);
    }
}
void dsp_write16(Cpu& c, uint32_t off, uint16_t v) {
    switch (off) {
        case 0x00: g_cpu_mail_high = v; return;
        case 0x02: mail_to_dsp(c, (uint32_t(g_cpu_mail_high) << 16) | v); return;
        case 0x04: case 0x06: rt::fatal(&c, "the CPU wrote %04X to the DSP's outgoing mailbox", v);
        case 0x0A: {
            if (v & 0xF000) rt::fatal(&c, "DSP control written with %04X: the top bits must be 0", v);
            // Flags first (a 1 clears one; ARAM busy kept; the enables from the write): the DSP may run
            // on this write and raise its interrupt anew.
            g_control = static_cast<uint16_t>((g_control & 0x0200) | (v & 0x0150) | (g_control & kFlags & ~v));
            const uint16_t ucode = microcode_control_write(v) & kMicrocodeBits;
            if (v & kReset) g_dma_control = 0;
            g_control = static_cast<uint16_t>((g_control & ~kMicrocodeBits) | ucode);
            update_line();
            return;
        }
        case 0x30: g_dma_source = (g_dma_source & 0xFFFF) | (uint32_t(v & 0x1FFF) << 16); return;
        case 0x32: g_dma_source = (g_dma_source & 0xFFFF0000u) | (v & 0xFFE0); return;
        case 0x36: {
            const bool was = dma_on();
            g_dma_control = v;
            if (!was && dma_on()) {  // starts with the source as it is now; asks for the next at once
                g_dma_current = g_dma_source;
                g_dma_blocks = v & 0x7FFF;
                if (!g_dma_blocks) rt::fatal(&c, "the audio DMA was started with 0 blocks");
                g_dma_t0 = time_base();
                note_deadline(dma_end());  // the poll clock: set by the game's code, not an interrupt
                if (g_trace) dsp::trace::event("audio DMA started: source", g_dma_source), dsp::trace::event("audio DMA started: blocks", g_dma_blocks);
                raise_flag(kAid);
            } else if (g_trace && was != dma_on()) {
                dsp::trace::event("audio DMA control written", v);
            }
            return;
        }
        default:
            if (off >= 0x12 && off < 0x2C) {
                g_ar[(off - 0x12) / 2] = v;
                if (off == 0x2A) aram_dma(c);
                return;
            }
            rt::fatal(&c, "store of %04X to DSP register %04X: not handled", v, off);
    }
}
uint32_t dsp_read(Cpu& c, uint32_t a, unsigned size) {
    const uint32_t off = a & 0x1FF;
    if (size == 2) return dsp_read16(c, off);
    if (size == 4) {
        const uint32_t hi = dsp_read16(c, off);
        return (hi << 16) | dsp_read16(c, off + 2);
    }
    rt::fatal(&c, "load of %u bytes from DSP register %04X", size, off);
}
void dsp_write(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    const uint32_t off = a & 0x1FF;
    if (size == 2) return dsp_write16(c, off, static_cast<uint16_t>(v));
    if (size == 4) {
        dsp_write16(c, off, static_cast<uint16_t>(v >> 16));
        return dsp_write16(c, off + 2, static_cast<uint16_t>(v));
    }
    rt::fatal(&c, "store of %u bytes to DSP register %04X", size, off);
}

uint32_t ai_counter() {
    uint32_t n = g_ai_counter;
    if (g_ai_control & 1) n += static_cast<uint32_t>((time_base() - g_ai_counter_t0) * ai_stream_rate() / kTimerHz);
    return n;
}
void ai_counter_restart(uint32_t value) {
    g_ai_counter = value;
    g_ai_counter_t0 = time_base();
}
uint32_t ai_read(Cpu& c, uint32_t a, unsigned size) {
    if (size != 4) rt::fatal(&c, "load of %u bytes from AI register %08X", size, a);
    switch (a & 0x1F) {
        case 0x00: return g_ai_control;
        case 0x04: return g_ai_volume;
        case 0x08: return ai_counter();
        case 0x0C: return g_ai_timing;
        default: rt::fatal(&c, "load from AI register %08X: not handled", a);
    }
}
void ai_write(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    if (size != 4) rt::fatal(&c, "store of %u bytes to AI register %08X", size, a);
    switch (a & 0x1F) {
        case 0x00: {
            if (v & 4) rt::fatal(&c, "the AI's sample-counter interrupt was enabled (%08X): not handled", v);
            if (g_trace && ((v ^ g_ai_control) & 0x42)) dsp::trace::event("AI control written (rates changed)", v);
            const uint32_t counter = ai_counter();
            if ((v ^ g_ai_control) & 1) ai_counter_restart(counter);  // run or stop, keeping the count
            g_ai_control = (v & 0x57) & ~0x08u;
            if (v & 0x20) ai_counter_restart(0);
            const unsigned rate = (v & 0x40) ? 32000 : 48000;
            if (rate != g_dma_rate) {
                g_dma_rate = rate;
                rt::log("audio: DMA rate %u Hz", rate);
            }
            return;
        }
        case 0x04:
            if (g_trace) dsp::trace::event("AI volume written", v);
            g_ai_volume = v;
            return;
        case 0x08: ai_counter_restart(v); return;
        case 0x0C: g_ai_timing = v; return;
        default: rt::fatal(&c, "store of %08X to AI register %08X: not handled", v, a);
    }
}

}  // namespace

namespace dsp {
uint8_t* physical(Cpu* c, uint32_t address, uint32_t size) {
    const uint32_t a = address & 0x1FFFFFFFu;  // the DSP's DMA ignores the cached/uncached segment bits
    const uint32_t end = a + size;
    if (a < 0x01800000u && end <= 0x01800000u) return rt::guest_ptr(0x80000000u | a);
    rt::fatal(c, "the DSP reached %u bytes at %08X: outside main memory", size, address);
}
}  // namespace dsp

namespace dsp::trace {
Lateness take_lateness() {
    const auto us = [](uint64_t ticks) { return static_cast<uint32_t>(ticks * 1000000 / kTimerHz); };
    return {us(std::exchange(g_poll_gap_worst, 0)), us(std::exchange(g_dma_late_worst, 0)), us(std::exchange(g_request_worst, 0))};
}
}  // namespace dsp::trace

uint8_t* aram_base() {
    if (!g_aram) g_aram = std::make_unique<uint8_t[]>(kAramSize);  // zeroed
    return g_aram.get();
}

void init_audio() {
    map_mmio({0xCC005000u, 0xCC005200u, "the DSP", dsp_read, dsp_write});
    map_mmio({0xCC006C00u, 0xCC006C20u, "the audio interface", ai_read, ai_write});
    add_timer_source(poll, deadline);
    g_core.aram = aram_base();
    g_core.memory = [](void*, uint32_t address, uint32_t bytes) { return dsp::physical(nullptr, address, bytes); };
    static bool started = false;  // (once: a restart of the game comes here again)
    if (!started) {
        started = true;
        const char* threaded = std::getenv("MKDD_DSP_THREAD");
        g_threaded = threaded && threaded[0] == '1';
        if (g_threaded) std::thread(dsp_thread).detach();
    }
    settle_stopped();
    if (g_threaded) {
        g_core.irq = [](void*) {  // (on the DSP's thread: the game's thread raises it at its next safe point)
            g_dsp_irq.store(true);
            wake_idle();
        };
    } else {
        g_core.irq = [](void*) { raise_flag(kDspFlag); };
    }
    g_core.rom = rom_entry;
    g_trace = dsp::trace::init();
    if (g_trace) g_core.dma_hook = [](void*, unsigned control, unsigned at, unsigned words, const uint8_t* p) { dsp::trace::dma(control, at, words, p); };
    g_core.fatal = [](void*, const char* message) { rt::fatal(nullptr, "%s", message); };
    set_microcode(Microcode::Rom);
}
void dsp_interrupt_enables(uint32_t os_mask) {
    uint16_t enables = 0;
    if (!(os_mask & (0x80000000u >> 5))) enables |= kAid << 1;
    if (!(os_mask & (0x80000000u >> 6))) enables |= kAram << 1;
    if (!(os_mask & (0x80000000u >> 7))) enables |= kDspFlag << 1;
    g_control = static_cast<uint16_t>((g_control & ~0x0150u) | enables);
    update_line();
}
void set_audio_sink(AudioSink sink, void* user) {
    g_sink = sink;
    g_sink_user = user;
}
const AudioStats& audio_stats() {
    g_stats.dsp_runs = g_dsp_runs.load(std::memory_order_relaxed);
    g_stats.dsp_ns = g_dsp_ns.load(std::memory_order_relaxed);
    g_stats.mails_out = g_dsp_mails_out.load(std::memory_order_relaxed);
    g_stats.settle_ns = g_settle_ns;
    g_stats.wakes = g_wakes;
    g_stats.settle_worst_ns = std::exchange(g_settle_worst_ns, 0);
    return g_stats;
}

}  // namespace mkdd
