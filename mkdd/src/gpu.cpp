// The GPU's front end: the hardware registers GX drives (command processor,
// pixel engine, the processor interface's FIFO) and the command stream that
// GX, and the engine's own mesh drawers, write to the write-gather pipe at
// 0xCC008000.
//
// The pipe writes each 32-byte burst to the CPU's FIFO in guest memory, as the
// hardware does. While the command processor is linked to that FIFO (normal
// drawing), it decodes the burst at once, so the CP reads as idle, its read
// pointer is always its write pointer, and the CPU never waits on it. Unlinked
// (GXBeginDisplayList), the bursts only build the display list in memory. Draw-done and token
// commands raise the pixel engine's interrupts, as the hardware does when the
// GPU reaches them. What the commands mean for drawing is the renderer's job
// (renderer.cpp): the decoder keeps the registers and hands primitives, state
// changes and EFB copies to the recorder (recorder.cpp), which reads what
// they need from guest memory as it is at that command.
//
// The command formats and register layouts are the hardware's, as documented
// by YAGCD and Dolphin (VideoCommon: OpcodeDecoding.h, CPMemory.h,
// VertexLoader_*.h, PixelEngine.h; GPL-2.0-or-later; facts only, our code).
#include "gpu.hpp"
#include "hw.hpp"
#include "priority.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "os.hpp"
#include "gx.hpp"
#include "recorder.hpp"
#include "video.hpp"
#include "qilin/rt/report.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// ---- registers
uint16_t g_cp[0x40] = {};  // command processor, 16-bit, at 0xCC000000
uint16_t g_pe[0x40] = {};  // pixel engine, at 0xCC001000
uint32_t g_pi[0x40] = {};  // processor interface, 32-bit, at 0xCC003000
uint16_t g_token = 0;
bool g_finish_pending = false, g_token_pending = false;

constexpr unsigned kPeCtrl = 0x0A / 2, kPeToken = 0x0E / 2;
constexpr unsigned kPiFifoBase = 0x0C / 4, kPiFifoEnd = 0x10 / 4, kPiFifoWrite = 0x14 / 4, kPiFifoReset = 0x18 / 4;
// The PI's FIFO pointers are physical addresses in MEM1, 32-byte aligned (the
// GameCube's GXSetCPUFifo masks the write pointer to bits 5-25); bit 26 of the
// write pointer is its wrap flag, never an address bit.
constexpr uint32_t kFifoAddress = 0x03FFFFE0u;

// ---- the decoder's state
std::array<uint32_t, 256> g_cpmem{};  // CP registers set by LOAD_CP_REG (VCD at 0x50/0x60, VAT at 0x70/0x80/0x90 + n)
std::array<uint32_t, 256> g_bp{};     // BP registers (24-bit)
std::vector<uint32_t> g_xf(0x1058);   // XF memory and registers
Stats g_stats;

gx::Registers regs() { return {g_cpmem.data(), g_xf.data(), g_bp.data()}; }
bool g_xfb_copied = false;  // set by the XFB copy just decoded (MKDD_DFF)
bool g_dff_recording = false;  // MKDD_DFF is recording a frame
std::atomic<uint32_t> g_gp_fifo_base{0}, g_gp_fifo_end{0};  // the CPU's FIFO while linked to the GPU (for MKDD_DFF)
uint32_t g_primitive_serial = 0;  // every primitive decoded
uint32_t g_dl_address = 0;        // the display list being decoded (physical), or 0
uint32_t g_bp_from[256] = {};     // where each BP register was last written: a place in a display list (physical), or 0 for the FIFO
const char* bp_source(uint32_t reg, char* text, size_t room) {
    if (g_bp_from[reg & 255]) std::snprintf(text, room, "a display list, at %08X; the register holds %06X", 0x80000000u | g_bp_from[reg & 255], g_bp[reg & 255]);
    else std::snprintf(text, room, "the FIFO; the register holds %06X", g_bp[reg & 255]);
    return text;
}
size_t g_dl_call_at = 0;          // MKDD_DFF: where in the commands the call to it is

std::vector<uint8_t> g_fifo;  // bytes written to the pipe, not yet a whole command
size_t g_fifo_at = 0;

// Tags for position-matrix loads (gpu.hpp's load_position_matrix, tag_matrix_at):
// the guest's side says where in the stream of bytes sent to the decoder a load
// begins, who makes it and under which view matrix; the decoder, reaching an XF
// load at that place, hands them to the recorder with it. A ring between the two
// threads (one writer, one reader); when it is full a load goes untagged, which
// only means its draws aren't blended between frames.
struct MatrixTag {
    uint64_t at;  // the load command's offset in the stream
    uint32_t tag, id;
    uint8_t context;
    float view[12];
};
constexpr size_t kTagRing = 1u << 14;
MatrixTag g_tag_ring[kTagRing];
std::atomic<uint64_t> g_tags_written{0}, g_tags_read{0};
std::atomic<uint64_t> g_forwarded{0};  // bytes sent on to the decoder (the guest's side; whole bursts)
uint64_t g_stream_base = 0;           // the stream offset of g_fifo's first byte (the decoder's side)

// What the guest is drawing under (gx.hpp's kContext...): set and cleared by mtxtag.cpp's wrappers
// on the game's drawing thread, the only one that loads matrices.
uint8_t g_matrix_context = 0;
void push_tag(uint64_t at, uint32_t tag, const float* view, uint32_t id = 0, uint8_t context = 0) {
    const uint64_t w = g_tags_written.load(std::memory_order_relaxed);
    if (w - g_tags_read.load(std::memory_order_acquire) >= kTagRing) return;
    MatrixTag& t = g_tag_ring[w % kTagRing];
    t.at = at, t.tag = tag, t.id = id, t.context = context;
    std::memcpy(t.view, view, sizeof t.view);
    g_tags_written.store(w + 1, std::memory_order_release);
}
// The tag for the load at stream offset `at`, if there is one (tags for places already passed are dropped).
const MatrixTag* take_tag(uint64_t at) {
    uint64_t r = g_tags_read.load(std::memory_order_relaxed);
    const uint64_t w = g_tags_written.load(std::memory_order_acquire);
    const MatrixTag* found = nullptr;
    while (r != w && g_tag_ring[r % kTagRing].at <= at) {
        if (g_tag_ring[r % kTagRing].at == at) found = &g_tag_ring[r % kTagRing];
        ++r;
    }
    g_tags_read.store(r, std::memory_order_release);
    return found;  // (the writer can't reuse the entry before the next take_tag frees more)
}

unsigned component_size(unsigned fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }  // u8/s8, u16/s16, float
unsigned index_size(unsigned how) { return how == 2 ? 1 : 2; }                  // 2 index8, 3 index16

// The size of one vertex in format `vat`, from the vertex descriptor and the
// attribute table (VertexLoaderBase::GetVertexSize's rules).
unsigned vertex_size_of(unsigned vat) {
    const uint32_t lo = g_cpmem[0x50], hi = g_cpmem[0x60];
    const uint32_t a = g_cpmem[0x70 + vat], b = g_cpmem[0x80 + vat], c = g_cpmem[0x90 + vat];
    unsigned size = std::popcount(lo & 0x1FFu);  // position and texture matrix indices
    auto part = [&](unsigned how, unsigned direct) { return how == 0 ? 0u : how == 1 ? direct : index_size(how); };
    size += part((lo >> 9) & 3, ((a & 1) ? 3 : 2) * component_size((a >> 1) & 7));  // position: xy or xyz
    {
        const unsigned how = (lo >> 11) & 3, nbt = (a >> 9) & 1, index3 = (a >> 31) & 1;
        if (how == 1) size += (nbt ? 9 : 3) * component_size((a >> 10) & 7);
        else if (how) size += index_size(how) * (nbt && index3 ? 3 : 1);
    }
    static constexpr unsigned kColor[8] = {2, 3, 4, 2, 3, 4, 0, 0};  // 565, 888, 888x, 4444, 6666, 8888
    size += part((lo >> 13) & 3, kColor[(a >> 14) & 7]);
    size += part((lo >> 15) & 3, kColor[(a >> 18) & 7]);
    // Texture coordinates: count bit and format for each of the eight.
    const unsigned tc_count[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    const unsigned tc_fmt[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    for (unsigned i = 0; i < 8; ++i) size += part((hi >> (2 * i)) & 3, (tc_count[i] ? 2 : 1) * component_size(tc_fmt[i]));
    return size;
}
// Kept for each format until a CP register changes (cp_written): a display
// list is hundreds of short primitives in one format.
uint16_t g_vertex_size[8];
uint8_t g_vertex_size_known = 0;  // a bit a format
unsigned vertex_size(unsigned vat) {
    if (!(g_vertex_size_known & (1u << vat))) g_vertex_size[vat] = uint16_t(vertex_size_of(vat)), g_vertex_size_known |= uint8_t(1u << vat);
    return g_vertex_size[vat];
}
void cp_written() {
    g_vertex_size_known = 0;
    gx::cp_changed();
}
// One CP register changed: only what reads it is made again. The vertex sizes read the descriptors and
// the attribute tables (0x50-0x9F); the recorder's format and plan those, the matrix indices (0x30, 0x40)
// and vertex arrays 0-11 (0xA0-0xAB, 0xB0-0xBB). Arrays 12-15 hold the indexed loads' matrices, read by
// the loads themselves: a race frame changes array 12's base 600 times, between shapes of one format.
void cp_register_written(uint32_t reg) {
    if (reg >= 0x50 && reg < 0xA0) g_vertex_size_known = 0;
    if (reg == 0x30 || reg == 0x40 || (reg >= 0x50 && reg < 0xA0) || (reg & 0xEF) - 0xA0 < 12) gx::cp_changed();
}
static_assert((0xACu & 0xEF) - 0xA0 >= 12 && (0xBBu & 0xEF) - 0xA0 < 12 && (0xBCu & 0xEF) - 0xA0 >= 12 && (0xA0u & 0xEF) - 0xA0 == 0);

const bool g_trace = [] {
    const char* t = std::getenv("MKDD_TRACE");
    return t && *t && *t != '0';
}();

// The GPU reaches a draw-done or token command only after the work queued
// before it, and games rely on that: GX writes draw-done, then installs the
// callback for it. So the pixel engine's interrupts come a short GPU latency
// after the command is decoded, as timed events (Dolphin likewise schedules
// them at least 500 CPU cycles out).
constexpr uint64_t kGpuLatency = kTimerHz / 20000;  // 50 us
struct PeEvent {
    uint64_t at;
    bool finish;  // or a token
    uint16_t token;
    bool interrupt;
};
std::vector<PeEvent> g_pe_events;  // the GPU thread adds, the guest's timer polls them
std::mutex g_pe_lock;

void hand_over();
void poll_pe() {
    hand_over();  // what the guest has written reaches the GPU before the guest can wait on it
    const uint64_t now = time_base();
    std::lock_guard l(g_pe_lock);
    for (size_t i = 0; i < g_pe_events.size();) {
        const PeEvent e = g_pe_events[i];
        if (e.at > now) {
            ++i;
            continue;
        }
        g_pe_events.erase(g_pe_events.begin() + static_cast<long>(i));
        if (e.finish) {
            g_finish_pending = true;
            if (g_pe[kPeCtrl] & 2) raise_interrupt(19);  // __OS_INTERRUPT_PI_PE_FINISH
        } else {
            g_token = e.token;
            if (e.interrupt) {
                g_token_pending = true;
                if (g_pe[kPeCtrl] & 1) raise_interrupt(18);  // __OS_INTERRUPT_PI_PE_TOKEN
            }
        }
    }
}
uint64_t pe_deadline() {
    std::lock_guard l(g_pe_lock);
    uint64_t t = UINT64_MAX;
    for (const auto& e : g_pe_events) t = std::min(t, e.at);
    return t;
}

void pe_finish() {
    if (g_trace) rt::log("GPU: draw done");
    {
        std::lock_guard l(g_pe_lock);
        g_pe_events.push_back({time_base() + kGpuLatency, true, 0, false});
    }
    wake_idle();  // a guest thread may be asleep waiting for it
}
void pe_token(uint16_t token, bool interrupt) {
    if (g_trace) rt::log("GPU: token %04X%s", token, interrupt ? " with interrupt" : "");
    {
        std::lock_guard l(g_pe_lock);
        g_pe_events.push_back({time_base() + kGpuLatency, false, token, interrupt});
    }
    wake_idle();
}

void on_bp(Cpu& c, uint32_t reg, uint32_t value) {
    // A BP write is masked by the mask register (0xFE), which then resets.
    const uint32_t before = g_bp[reg];
    if (reg != 0xFE) {
        const uint32_t mask = g_bp[0xFE] ? g_bp[0xFE] : 0xFFFFFFu;
        g_bp[reg] = (g_bp[reg] & ~mask) | (value & mask);
        g_bp[0xFE] = 0;
    } else {
        g_bp[0xFE] = value;
        return;
    }
    gx::bp_written(reg, g_bp[reg], g_bp[reg] != before);
    switch (reg) {
    case 0x43:                                                      // the EFB's pixel format
        if ((before & 7) != (g_bp[reg] & 7)) gx::record_pixel_format(before & 7, g_bp[reg] & 7);
        break;
    case 0x45: pe_finish(); break;                                  // draw done
    case 0x47: pe_token(static_cast<uint16_t>(value), false); break;  // token
    case 0x48: pe_token(static_cast<uint16_t>(value), true); break;   // token with interrupt
    case 0x52:                                                      // EFB copy
        if (value & (1u << 14)) {
            ++g_stats.xfb_copies, g_xfb_copied = true;
            if (g_dff_recording) gx::mark_captured_frame();  // this copy ends the captured frame
        }
        else ++g_stats.texture_copies;
        gx::record_copy(regs(), value);
        break;
    case 0x63:                                                      // TMEM preload
        if (value & 0x7FFF) rt::fatal(&c, "GX preloads TMEM (BP 63 = %06X): not handled", value);
        break;
    case 0x65: gx::record_tlut_load(regs(), value); break;          // palette load
    default: break;
    }
    (void)c;
}

void decode(Cpu& c, const uint8_t* data, size_t size, size_t& used, bool in_display_list);

void call_display_list(Cpu& c, uint32_t address, uint32_t size) {
    // A physical address in MEM1, cached at +0x80000000. The GPU ignores the
    // top three bits, so a virtual address (GXCallDisplayList passes the
    // caller's pointer) reaches the same memory.
    const uint32_t physical = address & 0x03FFFFE0u;
    size &= ~31u;
    if (!gx::in_memory(physical, size)) rt::fatal(&c, "GX display list at %08X (%u bytes) is outside MEM1", address, size);
    ++g_stats.display_lists;
    size_t used = 0;
    g_dl_address = physical;
    decode(c, rt::guest_ptr(0x80000000u | physical), size, used, true);
    g_dl_address = 0;
}

// The last primitive decoded, for bad_command: a wrong vertex size is the
// usual way the decoder loses its place.
struct LastPrimitive {
    uint8_t op = 0;
    uint32_t count = 0, size = 0, vcd_lo = 0, vcd_hi = 0, vat[3] = {};
} g_last_primitive;
// And the last CP register writes, oldest first, for the same report.
struct CpWrite {
    uint8_t reg;
    uint32_t value;
    bool in_display_list;
};
CpWrite g_cp_writes[8] = {};
unsigned g_cp_write_count = 0;

[[noreturn]] void bad_command(Cpu& c, const uint8_t* base, size_t used, size_t left, bool in_display_list) {
    const uint8_t* at = base + used;
    char before[3 * 48 + 1] = {}, bytes[3 * 16 + 1] = {};
    const size_t back = used < 48 ? used : 48;
    for (size_t i = 0; i < back; ++i) std::snprintf(before + 3 * i, 4, "%02X ", at[i - back]);
    for (size_t i = 0; i < left && i < 16; ++i) std::snprintf(bytes + 3 * i, 4, "%02X ", at[i]);
    const LastPrimitive& l = g_last_primitive;
    char cp[8 * 24 + 1] = {};
    for (unsigned i = 0, n = 0; i < 8 && i < g_cp_write_count; ++i) {
        const CpWrite& w = g_cp_writes[(g_cp_write_count - std::min(g_cp_write_count, 8u) + i) % 8];
        n += static_cast<unsigned>(std::snprintf(cp + n, sizeof cp - n, "%02X=%08X%s ", w.reg, w.value, w.in_display_list ? "(DL)" : ""));
    }
    rt::fatal(&c,
              "GX command stream: unknown command %02X %s(before: %s| from it: %s); the decoder has lost its place. "
              "The last primitive: %02X, %u vertices of %u bytes (VCD %08X %08X, VAT %08X %08X %08X); the last CP writes: %s",
              at[0], in_display_list ? "in a display list " : "", before, bytes, l.op, l.count, l.size, l.vcd_lo, l.vcd_hi,
              l.vat[0], l.vat[1], l.vat[2], cp);
}

uint32_t be32(const uint8_t* p) {  // (a load and a byte swap, which MSVC doesn't find in shifts and ors)
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return v;
}

// Decode whole commands from data[used..size); stop at one that isn't complete.
// ---- MKDD_DFF=<file>@<seconds>: the first whole frame after that much
// game time, as a Dolphin FIFO log (.dff, version 6), for Dolphin's FIFO
// player to render as the hardware would: a reference to compare our
// renderer's picture with. It holds the registers at the frame's start, the
// frame's commands from one XFB copy to the next, the console's RAM (MEM1)
// as the frame starts, and, as Dolphin's recorder does, each piece of
// memory the GPU reads (vertex arrays, textures, palettes, display lists,
// indexed loads) that changed since, at the place in the commands where it
// was read. With <file>.prims, tools/analysis/dff_cut.py cuts the frame short
// after any primitive, to see the EFB part way through.
void dff_note_memory(uint32_t physical, uint32_t size);
struct DffCapture {
    std::string path;
    double at = 0;
    int state = 0;  // 0 waiting, 1 recording the frame, 2 written
    std::vector<uint8_t> bytes;
    std::array<uint32_t, 256> bp{}, cp{};
    std::vector<uint32_t> xf;
    std::vector<uint8_t> mem1;   // MEM1 as the frame starts
    std::vector<uint8_t> seen1;  // as the GPU last read it (the updates applied)
    struct Update {
        uint32_t at, address;  // where in the commands; physical
        std::vector<uint8_t> bytes;
    };
    std::vector<Update> updates;
    std::string prims;  // <file>.prims: each primitive's serial and where it is, for tools/analysis/dff_cut.py
};
DffCapture* g_dff = [] () -> DffCapture* {
    const char* e = std::getenv("MKDD_DFF");
    if (!e || !*e) return nullptr;
    auto* d = new DffCapture;
    const std::string spec = e;
    const size_t atsign = spec.rfind('@');
    d->path = spec.substr(0, atsign);
    if (atsign != std::string::npos) d->at = std::atof(spec.c_str() + atsign + 1);
    return d;
}();

}  // namespace

namespace {
// A FIFO log's frame, loaded: its memory and registers put in place, its commands returned
// with where the BP and CP registers are (they're set again before each replay).
struct LoadedDff {
    std::vector<uint8_t> file, commands;
    uint64_t bp_at = 0, cp_at = 0;
    uint32_t u32(uint64_t at) const { uint32_t v = 0; if (at + 4 <= file.size()) std::memcpy(&v, file.data() + at, 4); return v; }
    void registers() const {
        for (int b = 0; b < 256; ++b) g_bp[size_t(b)] = u32(bp_at + 4ull * b), g_cpmem[size_t(b)] = u32(cp_at + 4ull * b);
        cp_written();
    }
};
LoadedDff load_dff(const char* dff_path) {
    LoadedDff out;
    std::vector<uint8_t>& file = out.file;
    if (FILE* f = std::fopen(dff_path, "rb")) {
        uint8_t buf[1 << 16];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + n);
        std::fclose(f);
    }
    const auto u32 = [&](uint64_t at) { uint32_t v = 0; if (at + 4 <= file.size()) std::memcpy(&v, file.data() + at, 4); return v; };
    const auto u64 = [&](uint64_t at) { uint64_t v = 0; if (at + 8 <= file.size()) std::memcpy(&v, file.data() + at, 8); return v; };
    if (file.size() < 128 || u32(0) != 0x0d01f1f0u) rt::fatal(nullptr, "%s isn't a FIFO log", dff_path);
    const uint64_t bp_at = u64(12), cp_at = u64(24), xfm_at = u64(36), xfr_at = u64(48), frame_at = u64(60);
    out.bp_at = bp_at, out.cp_at = cp_at;
    const uint64_t fifo_at = u64(frame_at), upd_at = u64(frame_at + 20);
    const uint32_t fifo_size = u32(frame_at + 8), updates = u32(frame_at + 28);
    if (fifo_at + fifo_size > file.size()) rt::fatal(nullptr, "%s is cut short", dff_path);
    // The memory as the frame starts, then the later updates (their order in
    // the frame doesn't matter for timing).
    for (uint32_t i = 0; i < updates; ++i) {
        const uint64_t e = upd_at + 24ull * i;
        const uint32_t address = u32(e + 4), size = u32(e + 16);
        const uint64_t data = u64(e + 8);
        if (data + size > file.size()) rt::fatal(nullptr, "%s is cut short", dff_path);
        if (gx::in_memory(address, size)) std::memcpy(rt::guest_ptr(0x80000000u | address), file.data() + data, size);
    }
    out.registers();
    for (int i = 0; i < 4096; ++i) g_xf[size_t(i)] = u32(xfm_at + 4ull * i);
    for (int i = 0; i < 88; ++i) g_xf[0x1000 + size_t(i)] = u32(xfr_at + 4ull * i);
    out.commands.assign(file.begin() + long(fifo_at), file.begin() + long(fifo_at + fifo_size));
    return out;
}
}  // namespace

// --render-dff: the log's frame decoded and recorded `loops` times, each frame it makes given
// to `draw` (the last loop's is the picture). It starts as Dolphin's FIFO player starts, so
// that the two pictures compare loop for loop: an empty EFB (no colour, no alpha, the far
// depth), then one clearing copy of the whole EFB under the log's own registers. The first
// loop is then a frame drawn after nothing; from the second on, a loop begins with what the
// one before left (the depth too, where the frame's last copy doesn't clear it), as the
// game's frames do.
void replay_dff(const char* dff_path, unsigned loops, const std::function<void(gx::Frame&)>& draw) {
    const LoadedDff dff = load_dff(dff_path);
    Cpu c = rt::new_cpu();
    gx::set_rendering(true);
    {
        std::vector<uint8_t> start;
        const auto bp = [&](uint32_t reg, uint32_t value) {
            const uint32_t w = (reg << 24) | (value & 0xFFFFFF);
            start.insert(start.end(), {0x61, uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)});
        };
        const auto whole_efb_clear = [&] {
            bp(0x49, 0), bp(0x4A, (527u << 10) | 639u), bp(0x4D, 0x140), bp(0x4B, 0);
            bp(0x52, (12u << 3) | (1u << 11));  // to a texture (RGBA8, at address 0), clearing
        };
        dff.registers();
        gx::state_changed();
        bp(0x41, 0x18), bp(0x40, 0x1F);                    // colour, alpha and depth all written
        bp(0x4F, 0), bp(0x50, 0), bp(0x51, 0xFFFFFF);      // with nothing, and the far depth
        whole_efb_clear();
        size_t used = 0;
        decode(c, start.data(), start.size(), used, false);
        start.clear();
        dff.registers();
        gx::state_changed();
        whole_efb_clear();
        used = 0;
        decode(c, start.data(), start.size(), used, false);
    }
    for (unsigned i = 0; i < loops; ++i) {
        dff.registers();
        gx::state_changed();
        size_t used = 0;
        decode(c, dff.commands.data(), dff.commands.size(), used, false);
        while (auto f = gx::next_frame(0)) {
            draw(*f);
            gx::recycle_frame(std::move(f));
        }
    }
}

int bench_recording(const char* dff_path, unsigned times) {
    const LoadedDff dff = load_dff(dff_path);
    const std::vector<uint8_t>& commands = dff.commands;
    Cpu c = rt::new_cpu();
    gx::set_rendering(true);  // frames are queued (and taken back below), as when drawing
    std::vector<double> ms;
    uint64_t draws = 0, vertices = 0, states = 0, state_bytes = 0, hashed_before = 0;
    for (unsigned i = 0; i < times + 5; ++i) {
        if (i == 5) hashed_before = gx::recorder_stats().hashed_bytes.load();  // five to warm the caches up, not counted
        dff.registers();
        gx::state_changed();
        const uint64_t t0 = gx::now_ns();
        size_t used = 0;
        decode(c, commands.data(), commands.size(), used, false);
        const uint64_t t1 = gx::now_ns();
        while (auto f = gx::next_frame(0)) {
            if (i >= 5) draws += f->draws.size(), vertices += f->vertices.size() + f->small.size(), states += f->states.size(), state_bytes += f->states.blocks() * gx::States::kBlock;
            gx::recycle_frame(std::move(f));
        }
        if (i >= 5) ms.push_back(double(t1 - t0) / 1e6);
    }
    std::sort(ms.begin(), ms.end());
    double sum = 0;
    for (double v : ms) sum += v;
    std::printf("recording %s, %u times: median %.3f ms, fastest %.3f, slowest %.3f, mean %.3f; "
                "%llu draws, %llu vertices and %llu states (%llu KB) a frame, %llu KB of textures checked\n",
                dff_path, times, ms[ms.size() / 2], ms.front(), ms.back(), sum / double(ms.size()),
                static_cast<unsigned long long>(draws / times), static_cast<unsigned long long>(vertices / times),
                static_cast<unsigned long long>(states / times), static_cast<unsigned long long>(state_bytes / times / 1024),
                static_cast<unsigned long long>((gx::recorder_stats().hashed_bytes.load() - hashed_before) / times / 1024));
    return 0;
}

namespace {

void dff_write(const DffCapture& d) {
    FILE* f = std::fopen(d.path.c_str(), "wb");
    if (!f) rt::fatal(nullptr, "MKDD_DFF: can't write %s", d.path.c_str());
    auto u32le = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u64le = [&](uint64_t v) { std::fwrite(&v, 8, 1, f); };
    auto u8s = [&](uint8_t v, int n) { for (int i = 0; i < n; ++i) std::fputc(v, f); };
    constexpr uint32_t kMem1 = gx::kMem1Size, kTex = 1u << 20;
    // Layout: header (128), the frame (64), BP, CP, XF memory, XF registers,
    // TMEM, the commands, the updates (24 each, MEM1 first), MEM1.
    const uint64_t frame_at = 128, bp_at = frame_at + 64, cp_at = bp_at + 1024, xfm_at = cp_at + 1024,
                   xfr_at = xfm_at + 4096 * 4, tex_at = xfr_at + 88 * 4, fifo_at = tex_at + kTex,
                   upd_at = fifo_at + d.bytes.size(), mem1_at = upd_at + 24 * (1 + d.updates.size());
    u32le(0x0d01f1f0), u32le(6), u32le(1);                    // id, version, loader version
    u64le(bp_at), u32le(256), u64le(cp_at), u32le(256);
    u64le(xfm_at), u32le(4096), u64le(xfr_at), u32le(88);
    u64le(frame_at), u32le(1), u32le(0);                      // one frame; flags: a GameCube's (bit 0 would say Wii)
    u64le(tex_at), u32le(kTex), u32le(kMem1), u32le(0);       // no MEM2
    std::fwrite("GM4E01\0\0", 1, 8, f);
    u8s(0, 24);
    // The FIFO the player streams the commands through: the game's own, as it was when a burst
    // last went to the GPU. (Not the PI's registers as they are now: while the game builds a
    // display list they point the pipe at the list's buffer, base 0, and a player given that
    // has no FIFO to stream through; one capture in a few caught the game there.)
    const uint32_t fifo_base = g_gp_fifo_base.load(std::memory_order_relaxed), fifo_end = g_gp_fifo_end.load(std::memory_order_relaxed);
    u64le(fifo_at), u32le(uint32_t(d.bytes.size())), u32le(fifo_base), u32le(fifo_end), u64le(upd_at),
        u32le(uint32_t(1 + d.updates.size())), u8s(0, 32);
    for (uint32_t v : d.bp) u32le(v);
    for (uint32_t v : d.cp) u32le(v);
    for (int i = 0; i < 4096; ++i) u32le(d.xf[size_t(i)]);
    for (int i = 0; i < 88; ++i) u32le(d.xf[0x1000 + size_t(i)]);
    u8s(0, int(kTex));
    std::fwrite(d.bytes.data(), 1, d.bytes.size(), f);
    u32le(0), u32le(0x00000000u), u64le(mem1_at), u32le(kMem1), std::fputc(4, f), u8s(0, 3);  // MEM1, at the first command
    uint64_t data_at = mem1_at + kMem1;
    for (const auto& u : d.updates) {  // what the GPU read that had changed: type 4, "vertex array"
        u32le(u.at), u32le(u.address), u64le(data_at), u32le(uint32_t(u.bytes.size())), std::fputc(4, f), u8s(0, 3);
        data_at += u.bytes.size();
    }
    std::fwrite(d.mem1.data(), 1, kMem1, f);
    for (const auto& u : d.updates) std::fwrite(u.bytes.data(), 1, u.bytes.size(), f);
    std::fclose(f);
    size_t updated = 0;
    for (const auto& u : d.updates) updated += u.bytes.size();
    rt::log("MKDD_DFF: wrote %s (%llu bytes of commands; %llu memory updates, %llu bytes)", d.path.c_str(),
            static_cast<unsigned long long>(d.bytes.size()), static_cast<unsigned long long>(d.updates.size()),
            static_cast<unsigned long long>(updated));
    // The primitives' places, and the frame's last XFB copy (the registers a cut log ends with).
    if (FILE* pf = std::fopen((d.path + ".prims").c_str(), "w")) {
        std::fprintf(pf, "# serial kind(fifo: offset in the commands; dl: physical address) location size\n%s", d.prims.c_str());
        std::fprintf(pf, "xfb %06X %06X %06X %06X %06X %06X\n", g_bp[0x49], g_bp[0x4A], g_bp[0x4B], g_bp[0x4D], g_bp[0x4E], g_bp[0x52]);
        std::fclose(pf);
    }
}

// After each top-level command: record it while the frame is being captured;
// at an XFB copy, start or finish.
void dff_after(const uint8_t* p, size_t n) {
    DffCapture& d = *g_dff;
    if (d.state == 1) d.bytes.insert(d.bytes.end(), p, p + n);
    if (!g_xfb_copied) return;
    g_xfb_copied = false;
    if (d.state == 0 && frame_count() / 60.0 >= d.at) {
        d.bp = g_bp, d.cp = g_cpmem, d.xf = g_xf;
        const auto* m1 = static_cast<const uint8_t*>(rt::guest_ptr(0x80000000u));
        d.mem1.assign(m1, m1 + gx::kMem1Size);
        d.seen1 = d.mem1;
        // The commands begin with the TEV colour registers and konstants, both kinds (BP memory
        // holds one per address): a replay then has every one the frame's draws may read.
        uint32_t tev[16];
        gx::tev_register_writes(regs(), tev);
        for (uint32_t w : tev) {
            const uint8_t cmd[5] = {0x61, uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)};
            d.bytes.insert(d.bytes.end(), cmd, cmd + 5);
        }
        d.state = 1;
        g_dff_recording = gx::g_dff_use = true;
        gx::g_dff_use_hook = dff_note_memory;
    } else if (d.state == 1) {
        dff_write(d);
        d.state = 2;
        g_dff_recording = gx::g_dff_use = false;
        gx::g_dff_use_hook = nullptr;
        d.mem1 = d.seen1 = {};
    }
}

void decode(Cpu& c, const uint8_t* data, size_t size, size_t& used, bool in_display_list) {
    while (used < size) {
        const uint8_t* p = data + used;
        const size_t left = size - used;
        const uint8_t op = p[0];
        size_t n;
        // By the opcode's top five bits: one jump through a table instead of a chain of compares (a race
        // frame: 34,000 BP writes, 28,000 primitives, 21,000 CP writes, 9,000 XF loads, 2,000 NOP runs).
        switch (op >> 3) {
        case 0x00 >> 3:  // NOPs, in a run (display lists are padded to 32 bytes with them)
            if (op != 0x00) bad_command(c, data, used, left, in_display_list);
            n = 1;
            while (n < left && p[n] == 0x00) ++n;
            break;
        case 0x08 >> 3: {  // LOAD_CP_REG: register, value
            if (op != 0x08) bad_command(c, data, used, left, in_display_list);
            if (left < 6) return;
            const uint32_t v = be32(p + 2);
            // Vertex formats and arrays: read by each primitive, not part of a draw's state. (The game writes
            // the same values again nine times in ten.)
            if (g_cpmem[p[1]] != v) g_cpmem[p[1]] = v, cp_register_written(p[1]);
            g_cp_writes[g_cp_write_count++ % 8] = {p[1], v, in_display_list};
            n = 6;
            break;
        }
        case 0x10 >> 3: {  // LOAD_XF_REG: (count - 1) << 16 | address, then the values
            if (op != 0x10) bad_command(c, data, used, left, in_display_list);
            if (left < 5) return;
            const uint32_t h = be32(p + 1), count = ((h >> 16) & 0xF) + 1, addr = h & 0xFFFF;
            if (left < 5 + 4 * count) return;
            bool changed = false;
            for (uint32_t i = 0; i < count; ++i)
                if (addr + i < g_xf.size()) {
                    const uint32_t v = be32(p + 5 + 4 * i);
                    changed |= g_xf[addr + i] != v;
                    g_xf[addr + i] = v;
                }
            if (changed) gx::xf_changed(addr, count);  // the same values again change nothing
            if (addr < 120 && count == 12) {  // a position matrix: with its tag, if the loader gave one
                const MatrixTag* t = in_display_list || data != g_fifo.data() ? nullptr : take_tag(g_stream_base + used);
                gx::matrix_loaded(addr, &g_xf[addr], t ? t->tag : 0, t ? t->view : nullptr, t ? t->id : 0, t ? t->context : uint8_t(0));
            }
            n = 5 + 4 * count;
            break;
        }
        case 0x20 >> 3: case 0x28 >> 3: case 0x30 >> 3: case 0x38 >> 3: {  // LOAD_INDX_A-D
            if (op & 7) bad_command(c, data, used, left, in_display_list);
            if (left < 5) return;
            ++g_stats.indexed_loads;
            // Matrices from CP arrays 12-15 into XF memory: index, then size - 1 and the address.
            const uint32_t v = be32(p + 1), index = v >> 16, words = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
            const unsigned array = 12 + (op - 0x20) / 8;
            const uint32_t src = (g_cpmem[0xA0 + array] + index * g_cpmem[0xB0 + array]) & 0x03FFFFFFu;
            if (!gx::in_memory(src, 4 * words)) rt::fatal(&c, "GX indexed load from %08X is outside MEM1", src);
            if (g_dff_recording) gx::dff_use_memory(src, 4 * words);
            const uint8_t* m = rt::guest_ptr(0x80000000u | src);
            bool changed = false;
            for (uint32_t i = 0; i < words; ++i)
                if (addr + i < g_xf.size()) {
                    const uint32_t word = be32(m + 4 * i);
                    changed |= g_xf[addr + i] != word;
                    g_xf[addr + i] = word;
                }
            if (changed) gx::xf_changed(addr, words);
            if (addr < 120 && words == 12) gx::matrix_loaded(addr, &g_xf[addr], src | 1u, nullptr);  // tagged by where it comes from
            n = 5;
            break;
        }
        case 0x40 >> 3:  // CALL_DL: address, size; 0x44: metrics
            if (op == 0x44) {
                n = 1;
                break;
            }
            if (op != 0x40) bad_command(c, data, used, left, in_display_list);
            if (left < 9) return;
            if (in_display_list) rt::fatal(&c, "GX display list calls another display list");
            if (g_dff_recording) {
                char line[96];
                g_dl_call_at = g_dff->bytes.size();
                std::snprintf(line, sizeof line, "call fifo %llu %08X %u\n", static_cast<unsigned long long>(g_dff->bytes.size()),
                              unsigned(be32(p + 1) & 0x03FFFFE0u), unsigned(be32(p + 5) & ~31u));
                g_dff->prims += line;
                gx::dff_use_memory(be32(p + 1) & 0x03FFFFE0u, be32(p + 5) & ~31u);
            }
            call_display_list(c, be32(p + 1), be32(p + 5));
            n = 9;
            break;
        case 0x48 >> 3:  // invalidate the vertex cache
            if (op != 0x48) bad_command(c, data, used, left, in_display_list);
            n = 1;
            break;
        case 0x60 >> 3:  // LOAD_BP_REG: register in the top byte, 24-bit value
            if (op != 0x61) bad_command(c, data, used, left, in_display_list);
            if (left < 5) return;
            g_bp_from[p[1]] = in_display_list ? g_dl_address + uint32_t(used) : 0;
            on_bp(c, p[1], be32(p + 1) & 0xFFFFFFu);
            n = 5;
            break;
        case 0x80 >> 3: case 0x88 >> 3: case 0x90 >> 3: case 0x98 >> 3:
        case 0xA0 >> 3: case 0xA8 >> 3: case 0xB0 >> 3: case 0xB8 >> 3: {  // a primitive: count, then the vertices
            if (left < 3) return;
            const uint32_t count = uint32_t{p[1]} << 8 | p[2];
            const unsigned vertex = vertex_size(op & 7);
            const size_t bytes = size_t{count} * vertex;
            if (g_trace && !in_display_list && used == 0) [[unlikely]]
                rt::log("GPU: primitive %02X, %u vertices of %u bytes (VCD %08X %08X, VAT %08X %08X %08X)", op, count,
                        vertex, g_cpmem[0x50], g_cpmem[0x60], g_cpmem[0x70 + (op & 7)], g_cpmem[0x80 + (op & 7)],
                        g_cpmem[0x90 + (op & 7)]);
            if (left < 3 + bytes) return;
            g_last_primitive = {op, count, vertex, g_cpmem[0x50], g_cpmem[0x60],
                                {g_cpmem[0x70 + (op & 7)], g_cpmem[0x80 + (op & 7)], g_cpmem[0x90 + (op & 7)]}};
            ++g_primitive_serial;
            if (g_dff_recording) [[unlikely]] {
                char line[96];
                if (in_display_list)
                    std::snprintf(line, sizeof line, "%u dl %08X %llu %llu\n", g_primitive_serial, unsigned(g_dl_address + used),
                                  static_cast<unsigned long long>(3 + bytes), static_cast<unsigned long long>(g_dl_call_at));
                else
                    std::snprintf(line, sizeof line, "%u fifo %llu %llu\n", g_primitive_serial,
                                  static_cast<unsigned long long>(g_dff->bytes.size()), static_cast<unsigned long long>(3 + bytes));
                g_dff->prims += line;
            }
            gx::set_primitive_serial(g_primitive_serial);
            gx::record_primitive(regs(), op, p + 3, count);
            ++g_stats.draws;
            g_stats.vertices += count;
            n = 3 + bytes;
            break;
        }
        default: bad_command(c, data, used, left, in_display_list);
        }
        used += n;
        if (g_dff && !in_display_list && g_dff->state < 2) dff_after(p, n);
    }
}

// The write-gather pipe collects 32 bytes before it sends them on, so the GPU
// sees a command only once a burst is full. Timing depends on that: GX writes
// a draw-done command, installs the callback for it and only then flushes
// (GXFlush pads the pipe with 32 bytes of NOPs).
uint8_t g_gather[32];
unsigned g_gathered = 0;
// Whole bursts on their way to the GPU thread (the guest's side: only one guest thread runs at a time).
uint8_t g_outgoing[256];
size_t g_outgoing_size = 0;

void send_burst(Cpu& c);
void hand_over();
void consume(Cpu& c, const uint8_t* bytes, size_t n);

// The GPU thread (v0.17.0): the guest's thread only queues the pipe's bursts;
// this one decodes and records them, as the console's GPU works beside its
// CPU. It reaches draw-done and token commands when it gets to them, and the
// guest learns of them then, as on the hardware. Memory it reads (display
// lists, vertex arrays, textures) is read when it gets there too, which games
// allow for: they sync with the GPU before reusing it. MKDD_GPU_THREAD=0
// decodes on the guest's thread.
const bool g_gpu_threaded = [] {
    const char* e = std::getenv("MKDD_GPU_THREAD");
    return !(e && *e == '0');
}();
// Bytes queued before the guest waits. Small, as the hardware's lag is: with
// 1 MB the decoder fell whole draws behind and read vertex arrays and
// matrices the game had already rewritten (corrupt fights); at 16 KB it trails
// by a few draws, which the game allows for. Measured since (2026-11-21, 7,800
// frames, fights among them): when the decoder reaches a frame's draw-done the
// game has written at most 359 bytes past it, state and no draws, so it waits
// there every frame before sending the next one's; and no captured frame reads
// any memory twice with different contents. Within a frame the lag is then
// harmless, and the bound stays as the margin it was chosen for.
constexpr size_t kMaxIncoming = 16u << 10;
// Never destroyed: the GPU thread is detached and may still be waiting on them
// when static destructors run at exit, and glibc's pthread_cond_destroy blocks
// while a thread waits (CI's Linux runs hung there; Windows doesn't block).
std::mutex& g_gpu_lock = *new std::mutex;
std::condition_variable& g_gpu_cv = *new std::condition_variable;
bool g_gpu_busy = false;  // the GPU thread is decoding a batch (under g_gpu_lock)
std::atomic<uint64_t> g_fifo_wait_ns{0};
std::vector<uint8_t> g_incoming;  // bursts not yet decoded
Cpu* g_guest_cpu = nullptr;       // for the decoder's error reports
std::mutex g_stats_lock;
Stats g_stats_seen;  // g_stats as of the last batch, for other threads

void pipe_write(Cpu& c, uint32_t value, unsigned size) {
    if (size == 4 && g_gathered <= 28) [[likely]] {  // most stores: a word, inside the burst
        const uint32_t be = std::byteswap(value);
        std::memcpy(g_gather + g_gathered, &be, 4);
        if ((g_gathered += 4) == 32) send_burst(c);
        return;
    }
    for (unsigned i = 0; i < size; ++i) {  // a write can straddle two bursts
        g_gather[g_gathered++] = static_cast<uint8_t>(value >> (8 * (size - 1 - i)));
        if (g_gathered == 32) send_burst(c);
    }
}

// Bit 26 of the PI's write pointer is its wrap flag (GXSetCPUFifo clears it,
// __GXSaveCPUFifoAux masks it off), never an address bit.
constexpr uint32_t kFifoWrapped = 0x04000000u;  // (kFifoAddress is with the PI's registers)
constexpr unsigned kCpControl = 0x02 / 2, kCpWriteLo = 0x34 / 2, kCpWriteHi = 0x36 / 2;

// The CPU FIFO's write pointer after a burst at `pointer`: it wraps after the
// block at the end pointer, and only there (as Dolphin's GPFifo).
constexpr uint32_t next_write_pointer(uint32_t pointer, uint32_t base, uint32_t end) {
    const uint32_t w = pointer & kFifoAddress;
    return w == end ? base | kFifoWrapped : (w + 32) | (pointer & kFifoWrapped);
}
static_assert(next_write_pointer(0x00100000u, 0x00100000u, 0x00100FE0u) == 0x00100020u);
static_assert(next_write_pointer(0x00100FC0u | kFifoWrapped, 0x00100000u, 0x00100FE0u) == (0x00100FE0u | kFifoWrapped));
static_assert(next_write_pointer(0x00100FE0u, 0x00100000u, 0x00100FE0u) == (0x00100000u | kFifoWrapped));  // the end block, then wrap

// CP control bit 4 links the command processor to the CPU's FIFO: then the
// GPU reads what the pipe writes. Unlinked (a CPU FIFO that isn't the GP's:
// this game's SDK has no GXBeginDisplayList, J3D builds its lists in memory
// itself, but GXSetCPUFifo allows it), the bursts go to memory only.
bool linked() { return (g_cp[kCpControl] & 0x10) != 0; }

void send_burst(Cpu& c) {
    g_gathered = 0;
    // The burst goes to memory at the PI's write pointer, which wraps at the end.
    const uint32_t base = g_pi[kPiFifoBase] & kFifoAddress, end = g_pi[kPiFifoEnd] & kFifoAddress;
    if (end > base) {
        const uint32_t w = g_pi[kPiFifoWrite] & kFifoAddress;
        if (!gx::in_memory(w, 32)) rt::fatal(&c, "the CPU's FIFO writes to %08X, outside MEM1", w);
        std::memcpy(rt::guest_ptr(0x80000000u | w), g_gather, 32);
        g_pi[kPiFifoWrite] = next_write_pointer(g_pi[kPiFifoWrite], base, end);
    }
    if (!linked()) return;  // building a display list: the GPU doesn't see it
    // Linked, the GP FIFO's write pointer follows the CPU's.
    const uint32_t cp_write = g_pi[kPiFifoWrite] & kFifoAddress;
    g_cp[kCpWriteLo] = static_cast<uint16_t>(cp_write);
    g_cp[kCpWriteHi] = static_cast<uint16_t>(cp_write >> 16);
    g_gp_fifo_base.store(base, std::memory_order_relaxed), g_gp_fifo_end.store(end, std::memory_order_relaxed);
    g_forwarded.store(g_forwarded.load(std::memory_order_relaxed) + 32, std::memory_order_relaxed);
    if (!g_gpu_threaded) return consume(c, g_gather, 32);
    // To the GPU thread, a few bursts at a time: a fight's frame is 13,000 of
    // them, and each trip takes the queue's lock and may wake the thread.
    g_guest_cpu = &c;
    std::memcpy(g_outgoing + g_outgoing_size, g_gather, 32);
    if ((g_outgoing_size += 32) == sizeof g_outgoing) hand_over();
}

// The guest's bursts not yet in the GPU thread's queue go there now: when
// the batch is full, and before the guest could wait for the GPU or read its
// registers (every timer poll: poll_pe).
void hand_over() {
    if (g_outgoing_size == 0) return;
    std::unique_lock l(g_gpu_lock);
    // A bounded queue, as the hardware's FIFO is: the CPU waits when it's full.
    if (g_incoming.size() >= kMaxIncoming) {  // (timed for the report: the guest held up by the GPU thread)
        const auto t0 = std::chrono::steady_clock::now();
        g_gpu_cv.wait(l, [] { return g_incoming.size() < kMaxIncoming; });
        g_fifo_wait_ns.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()),
                                 std::memory_order_relaxed);
    }
    const bool was_empty = g_incoming.empty();
    g_incoming.insert(g_incoming.end(), g_outgoing, g_outgoing + g_outgoing_size);
    g_outgoing_size = 0;
    if (was_empty) g_gpu_cv.notify_all();
}

// The GPU's side of the FIFO: decode what has arrived (the GPU thread, or the
// guest's own with MKDD_GPU_THREAD=0).
void consume(Cpu& c, const uint8_t* bytes, size_t n) {
    g_fifo.insert(g_fifo.end(), bytes, bytes + n);
    g_stats.fifo_bytes += n;
    gx::g_bp_source = bp_source, gx::g_decoding_cpu = &c;
    {  // (timed here, a batch at a time, less any wait for the main thread inside)
        auto& st = gx::recorder_stats();
        const uint64_t t0 = gx::now_ns(), w0 = st.wait_ns.load(std::memory_order_relaxed);
        decode(c, g_fifo.data(), g_fifo.size(), g_fifo_at, false);
        st.record_ns.fetch_add(gx::now_ns() - t0 - (st.wait_ns.load(std::memory_order_relaxed) - w0), std::memory_order_relaxed);
    }
    if (g_fifo_at == g_fifo.size()) {
        g_stream_base += g_fifo.size();
        g_fifo.clear();
        g_fifo_at = 0;
    } else if (g_fifo_at > 65536) {
        g_stream_base += g_fifo_at;
        g_fifo.erase(g_fifo.begin(), g_fifo.begin() + static_cast<long>(g_fifo_at));
        g_fifo_at = 0;
    }
    std::lock_guard l(g_stats_lock);
    g_stats_seen = g_stats;
}

void gpu_thread() {
    set_this_thread_priority(Priority::high);  // a frame's draws wait on it
    rt::set_thread_role("the GPU thread (decoding the game's commands)");
    std::vector<uint8_t> batch;
    for (;;) {
        Cpu* c;
        {
            std::unique_lock l(g_gpu_lock);
            if (g_gpu_busy) g_gpu_busy = false, g_gpu_cv.notify_all();  // (abort_fifo may be waiting for it)
            g_gpu_cv.wait(l, [] { return !g_incoming.empty(); });
            batch.swap(g_incoming);
            g_gpu_busy = true;
            c = g_guest_cpu;
        }
        g_gpu_cv.notify_all();  // room again
        consume(*c, batch.data(), batch.size());
        batch.clear();
    }
}

// The PI's FIFO reset (0xCC003018, bit 0): __GXAbort and GXAbortFrame write 1,
// wait 50 timer ticks and write 0. It resets the GPU's side: what the command
// processor hasn't read yet is dropped, a command it was part way through
// too, and the SDK then points both FIFOs at their base again
// (__GXCleanGPFifo). What was drawn stays drawn, as in the EFB. The gather
// pipe is the CPU's and keeps its bytes (only a WPAR write empties it).
void abort_fifo() {
    g_outgoing_size = 0;
    size_t dropped = 0;
    if (g_gpu_threaded) {
        std::unique_lock l(g_gpu_lock);
        dropped = g_incoming.size();
        g_incoming.clear();
        g_gpu_cv.wait(l, [] { return !g_gpu_busy; });  // the batch it has is decoded: the GPU had read it
    }
    dropped += g_fifo.size() - g_fifo_at;
    g_fifo.clear(), g_fifo_at = 0;
    g_stream_base = g_forwarded.load(std::memory_order_relaxed);  // what was sent is decoded or dropped
    rt::log("GPU: the FIFO is reset (GXAbortFrame), %llu bytes of commands dropped", static_cast<unsigned long long>(dropped));
}

uint32_t read_reg(Cpu& c, uint32_t a, unsigned size) {
    hand_over();
    const uint32_t off = a & 0xFFFF;
    if (a >= 0xCC000000u && a < 0xCC000080u && size == 2) {  // command processor
        const unsigned i = (off & 0x7F) / 2;
        if (i == 0) return 0x000C;  // status: read and command idle, no overflow
        if (off >= 0x30 && off <= 0x3A) {  // RW distance 0; the read pointer is the write pointer
            if (off == 0x30 || off == 0x32) return 0;
            return g_cp[(off & 2) ? kCpWriteHi : kCpWriteLo];
        }
        return g_cp[i];
    }
    if (a >= 0xCC001000u && a < 0xCC001080u && size == 2) {  // pixel engine
        const unsigned i = (off & 0x7F) / 2;
        if (i == kPeToken) return g_token;
        if (i == kPeCtrl) return (g_pe[i] & 3u) | (g_token_pending ? 4u : 0u) | (g_finish_pending ? 8u : 0u);
        return g_pe[i];
    }
    if (a >= 0xCC003000u && a < 0xCC003100u && size == 4) return g_pi[(off & 0xFF) / 4];
    rt::fatal(&c, "load of %u bytes from hardware register %08X: not handled", size, a);
}

void write_reg(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    if ((a & ~31u) == 0xCC008000u) return pipe_write(c, v, size);  // the gather block: any address in it
    const uint32_t off = a & 0xFFFF;
    if (a >= 0xCC000000u && a < 0xCC000080u && size == 2) {
        g_cp[(off & 0x7F) / 2] = static_cast<uint16_t>(v);
        return;
    }
    if (a >= 0xCC001000u && a < 0xCC001080u && size == 2) {
        const unsigned i = (off & 0x7F) / 2;
        if (i == kPeCtrl) {  // bits 2 and 3 acknowledge the token and finish interrupts
            if (v & 4) g_token_pending = false;
            if (v & 8) g_finish_pending = false;
            g_pe[i] = static_cast<uint16_t>(v & 3);
            return;
        }
        g_pe[i] = static_cast<uint16_t>(v);
        return;
    }
    if (a >= 0xCC003000u && a < 0xCC003100u && size == 4) {
        if ((off & 0xFF) / 4 == kPiFifoReset && (v & 1)) abort_fifo();
        g_pi[(off & 0xFF) / 4] = v;
        return;
    }
    rt::fatal(&c, "store of %u bytes (%08X) to hardware register %08X: not handled", size, v, a);
}

}  // namespace

namespace {
// Save the bytes of [physical, +size) that changed since the GPU last read them,
// to be written to memory before the command being decoded (a display list's
// primitive: before the call to the list).
void dff_note_memory(uint32_t physical, uint32_t size) {
    if (!g_dff || g_dff->state != 1 || size == 0) return;
    DffCapture& d = *g_dff;
    uint32_t a = physical & 0x03FFFFFFu;
    if (a >= gx::kMem1Size) return;
    size = std::min(size, gx::kMem1Size - a);
    const uint8_t* now = rt::guest_ptr(0x80000000u | a);
    uint8_t* old = d.seen1.data() + a;
    if (std::memcmp(now, old, size) == 0) return;
    uint32_t lo = 0, hi = size;
    while (now[lo] == old[lo]) ++lo;
    while (now[hi - 1] == old[hi - 1]) --hi;
    std::memcpy(old + lo, now + lo, hi - lo);
    const uint32_t at = uint32_t(g_dl_address != 0 ? g_dl_call_at : d.bytes.size());
    d.updates.push_back({at, a + lo, std::vector<uint8_t>(now + lo, now + hi)});
}
}  // namespace

void init_gpu() {
    // Writing WPAR resets the pipe: bytes not yet sent as a burst are dropped.
    // GX does it after switching the CPU's FIFO (GXBeginDisplayList), so
    // GXFlush's padding stays out of the new FIFO.
    on_wpar_write([] { g_gathered = 0; });
    map_mmio({0xCC000000u, 0xCC000080u, "the command processor", read_reg, write_reg});
    map_mmio({0xCC001000u, 0xCC001080u, "the pixel engine", read_reg, write_reg});
    map_mmio({0xCC003000u, 0xCC003100u, "the processor interface", read_reg, write_reg});
    map_mmio({0xCC008000u, 0xCC008020u, "the write-gather pipe", read_reg, write_reg});
    rt::g_pipe_write = pipe_write;  // the stores' direct route (cpu.hpp)
    add_timer_source(poll_pe, pe_deadline);
    if (g_gpu_threaded) std::thread(gpu_thread).detach();  // runs until the process exits
}
uint64_t fifo_wait_ns() { return g_fifo_wait_ns.load(std::memory_order_relaxed); }

void sync_decoder() {
    static uint64_t synced = ~0ull;  // the bytes forwarded when it last returned (the guest's thread only)
    const uint64_t forwarded = g_forwarded.load(std::memory_order_relaxed);
    if (!g_gpu_threaded || (forwarded == synced && g_outgoing_size == 0)) return;
    hand_over();
    std::unique_lock l(g_gpu_lock);
    g_gpu_cv.wait(l, [] { return g_incoming.empty() && !g_gpu_busy; });
    synced = forwarded;
}

void load_position_matrix(Cpu& c, uint32_t matrix, uint32_t xf_address, uint32_t tag, uint32_t view, uint32_t id) {
    // Where the command will start in the decoder's stream: the bursts sent on so far and the
    // bytes gathered since. (Unlinked, nothing reaches the decoder and there is nothing to tag.)
    if (linked()) {
        float v[12];
        for (unsigned i = 0; i < 12; ++i) v[i] = std::bit_cast<float>(rt::peek32(view + 4 * i));
        push_tag(g_forwarded.load(std::memory_order_relaxed) + g_gathered, tag, v, id, g_matrix_context);
    }
    pipe_write(c, 0x10, 1);
    pipe_write(c, (11u << 16) | (xf_address & 0xFFFFu), 4);
    for (unsigned i = 0; i < 12; ++i) pipe_write(c, rt::peek32(matrix + 4 * i), 4);
}

void tag_matrix_at(uint64_t stream_offset, uint32_t tag, const float view[12], uint32_t id, uint8_t context) {
    push_tag(stream_offset, tag, view, id, context);
}
uint8_t set_matrix_context(uint8_t context) {
    const uint8_t before = g_matrix_context;
    g_matrix_context = context;
    return before;
}

void decode_commands(const uint8_t* bytes, size_t size) {
    static Cpu c = rt::new_cpu();
    consume(c, bytes, size);
}

Stats gpu_stats() {
    std::lock_guard l(g_stats_lock);
    return g_gpu_threaded ? g_stats_seen : g_stats;
}

int bench_handover(const char* dff_path, unsigned times) {
    if (!g_gpu_threaded) rt::fatal(nullptr, "--bench-pipe: the GPU thread is off (MKDD_GPU_THREAD=0)");
    const LoadedDff dff = load_dff(dff_path);
    const std::vector<uint8_t>& commands = dff.commands;
    Cpu c = rt::new_cpu();
    gx::set_rendering(true);
    // The game's own FIFO, as the log has it, linked to the GPU.
    const uint64_t frame_at = [&] { uint64_t v = 0; std::memcpy(&v, dff.file.data() + 60, 8); return v; }();
    const uint32_t base = dff.u32(frame_at + 12) & kFifoAddress, end = dff.u32(frame_at + 16) & kFifoAddress;
    if (!(end > base) || !gx::in_memory(base, end - base + 32)) rt::fatal(nullptr, "--bench-pipe: %s has no FIFO", dff_path);
    g_pi[kPiFifoBase] = base, g_pi[kPiFifoEnd] = end, g_pi[kPiFifoWrite] = base;
    g_cp[kCpControl] |= 0x10;
    // Until the GPU thread has decoded everything handed over (taking the frames it makes).
    const auto drained = [] {
        for (;;) {
            while (auto f = gx::next_frame(0)) gx::recycle_frame(std::move(f));
            std::unique_lock l(g_gpu_lock);
            if (g_incoming.empty() && !g_gpu_busy) return;
            l.unlock();
            std::this_thread::yield();
        }
    };
    std::vector<double> guest_ms, all_ms, decode_ms;
    for (unsigned i = 0; i < times + 5; ++i) {
        drained();
        dff.registers();
        gx::state_changed();
        const uint64_t t0 = gx::now_ns(), r0 = gx::recorder_stats().record_ns.load();
        size_t k = 0;
        for (; k + 4 <= commands.size(); k += 4) pipe_write(c, be32(commands.data() + k), 4);
        for (; k < commands.size(); ++k) pipe_write(c, commands[k], 1);
        while (g_gathered) pipe_write(c, 0, 1);  // (the last burst out, as GXFlush pads it)
        hand_over();
        const uint64_t t1 = gx::now_ns();
        drained();
        const uint64_t t2 = gx::now_ns();
        if (i >= 5) guest_ms.push_back(double(t1 - t0) / 1e6), all_ms.push_back(double(t2 - t0) / 1e6), decode_ms.push_back(double(gx::recorder_stats().record_ns.load() - r0) / 1e6);
    }
    std::sort(guest_ms.begin(), guest_ms.end()), std::sort(all_ms.begin(), all_ms.end()), std::sort(decode_ms.begin(), decode_ms.end());
    std::printf("pipe %s, %u times, %llu bytes: the guest's stores and hand-over median %.3f ms (fastest %.3f); "
                "until decoded median %.3f ms (fastest %.3f); the GPU thread decoding median %.3f ms\n",
                dff_path, times, static_cast<unsigned long long>(commands.size()), guest_ms[guest_ms.size() / 2], guest_ms.front(),
                all_ms[all_ms.size() / 2], all_ms.front(), decode_ms[decode_ms.size() / 2]);
    return 0;
}

}  // namespace mkdd
