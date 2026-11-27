// The guest's side of drawing (recorder.hpp).
//
// As the command processor decodes a primitive, its vertices are read here
// (direct or through the CP's arrays, in the vertex attribute table's
// formats), the XF and BP state it draws with is snapshotted, and its
// textures are decoded from guest memory as they are at that command. EFB
// copies are recorded in order with the draws, and the copy to the external
// frame buffer ends a frame, which goes to the renderer.
//
// Formats, scales and the fixed-function rules are the hardware's, as
// Dolphin documents them (VideoCommon: VertexLoader*.cpp, CPMemory.h,
// XFMemory.h, BPMemory.h, VertexShaderManager.cpp, BPFunctions.cpp,
// RenderState.cpp). Facts only; the code is ours.
#include "recorder.hpp"
#include "latency.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "gx.hpp"
#include "hud.hpp"
#include "qilin/rt/system.hpp"
#include "texture.hpp"

namespace mkdd::gx {
namespace rt = qilin::rt;

namespace {

RecorderStats g_stats;
std::unique_ptr<Frame> g_frame = std::make_unique<Frame>();

// The switches read for every primitive, read once at start-up (a function's own static is a guard to
// check each time it runs).
bool env_set(const char* name) {
    const char* e = std::getenv(name);
    return e && *e && *e != '0';
}
const bool g_check_decode = env_set("MKDD_CHECK_DECODE");  // the fast decoders and incremental states checked
const bool g_force_full = env_set("MKDD_FULL_VERTICES");   // always the full vertex layout (tests both)
const bool g_join_on = [] { const char* e = std::getenv("MKDD_JOIN"); return !(e && *e == '0'); }();  // MKDD_JOIN=0: a draw a primitive

// Every shape seen, by its words, and by id for the renderer's threads.
struct ShapeHash {
    size_t operator()(const ShapeWords& w) const {
        uint64_t h = 0x9E3779B97F4A7C15ull;
        for (uint32_t x : w) h = (h ^ x) * 0x100000001B3ull;
        return size_t(h ^ (h >> 29));
    }
};
std::unordered_map<ShapeWords, uint32_t, ShapeHash> g_shape_ids;  // the guest thread's
std::mutex g_shapes_lock;
std::deque<ShapeWords> g_shapes;  // by id
bool g_dirty = true;
uint32_t g_state = 0;
int32_t g_tev_regs[4][4] = {}, g_konst[4][4] = {};
float g_last_normal[3] = {0, 0, 1}, g_last_tangent[3] = {}, g_last_binormal[3] = {};

// ---- vertices

float as_float(uint32_t v) { return std::bit_cast<float>(v); }
// (A load and a byte swap: MSVC doesn't see one in shifts and ors, and these read every vertex.)
uint16_t be16(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return v;
}
uint32_t be32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    if constexpr (std::endian::native == std::endian::little) v = std::byteswap(v);
    return v;
}

// Forced inline: one per vertex attribute (MSVC spells it differently).
#if defined(_MSC_VER) && !defined(__clang__)
#define QILIN_RECORDER_INLINE __forceinline
#else
#define QILIN_RECORDER_INLINE [[gnu::always_inline]] inline
#endif

unsigned component_size(unsigned fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
QILIN_RECORDER_INLINE float component(const uint8_t* p, unsigned fmt, float scale, bool scale_bytes) {
    switch (fmt) {
        case 0: return scale_bytes ? p[0] * scale : p[0];
        case 1: return scale_bytes ? static_cast<int8_t>(p[0]) * scale : static_cast<int8_t>(p[0]);
        case 2: return be16(p) * scale;
        case 3: return static_cast<int16_t>(be16(p)) * scale;
        default: return as_float(be32(p));
    }
}
QILIN_RECORDER_INLINE float normal_component(const uint8_t* p, unsigned fmt) {
    // Fixed scales: the fraction is the type's. (Times the power of two's inverse: the same float as the
    // division, which MSVC keeps as a division.)
    switch (fmt) {
        case 0: return float(p[0]) * (1.0f / 128.0f);
        case 1: return float(static_cast<int8_t>(p[0])) * (1.0f / 64.0f);
        case 2: return float(be16(p)) * (1.0f / 32768.0f);
        case 3: return float(static_cast<int16_t>(be16(p))) * (1.0f / 16384.0f);
        default: return as_float(be32(p));
    }
}

// MKDD_DFF: the span of each vertex array the primitive being recorded reads.
uint32_t g_use_lo[16], g_use_hi[16];
void note_array(unsigned array, uint32_t offset, uint32_t bytes) {
    g_use_lo[array] = std::min(g_use_lo[array], offset);
    g_use_hi[array] = std::max(g_use_hi[array], offset + bytes);
}

const uint8_t* array_data(const Registers& r, unsigned array, uint32_t index, uint32_t bytes) {
    const uint32_t base = r.cp[0xA0 + array], stride = r.cp[0xB0 + array];
    if (g_dff_use) [[unlikely]] note_array(array, index * stride, bytes);
    const uint32_t a = (base + index * stride) & 0x03FFFFFFu;
    if (in_memory(a, bytes)) return rt::guest_ptr(0x80000000u | a);
    rt::fatal(nullptr, "vertex array %u: element %u (%u bytes at %08X) is outside MEM1", array, index, bytes, base + index * stride);
}

void read_color(const uint8_t* p, unsigned fmt, uint8_t* out) {
    switch (fmt) {
        case 0: {  // RGB565
            const unsigned v = be16(p);
            out[0] = uint8_t((((v >> 11) & 31) << 3) | (((v >> 11) & 31) >> 2));
            out[1] = uint8_t((((v >> 5) & 63) << 2) | (((v >> 5) & 63) >> 4));
            out[2] = uint8_t(((v & 31) << 3) | ((v & 31) >> 2));
            out[3] = 255;
            break;
        }
        case 1: case 2: out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 255; break;  // RGB888, RGB888x
        case 3: {  // RGBA4444
            const unsigned v = be16(p);
            for (int i = 0; i < 4; ++i) out[i] = uint8_t(((v >> (12 - 4 * i)) & 15) * 17);
            break;
        }
        case 4: {  // RGBA6666
            const uint32_t v = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
            for (int i = 0; i < 4; ++i) {
                const unsigned c = (v >> (18 - 6 * i)) & 63;
                out[i] = uint8_t((c << 2) | (c >> 4));
            }
            break;
        }
        default: out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = p[3]; break;  // RGBA8888
    }
}
unsigned color_size(unsigned fmt) { return fmt == 0 || fmt == 3 ? 2 : fmt == 1 || fmt == 4 ? 3 : 4; }

// An indexed array, resolved once per primitive: a lookup is then a compare
// and a multiply-add (array_data checks and translates every element).
struct Array {
    const uint8_t* host = nullptr;
    uint32_t stride = 0, last = 0;  // the highest index whose element lies in memory
    bool any = false;               // false: no element does (array_data reports it)
};

struct Format {
    uint32_t vcd_lo, vcd_hi, a, b, c;
    unsigned pos_fmt, pos_n, nrm_fmt, col_fmt[2], tc_fmt[8], tc_n[8];
    float pos_scale, tc_scale[8];
    bool nbt, nbt3, dequant;
    Array arrays[12];  // 0 position, 1 normal, 2-3 colours, 4-11 texture coordinates
};

// The array's elements of `bytes` each, as far as they stay inside MEM1.
Array resolve_array(const Registers& r, unsigned array, uint32_t bytes) {
    Array a;
    const uint32_t base = r.cp[0xA0 + array] & 0x03FFFFFFu, stride = r.cp[0xB0 + array];
    const uint32_t end = kMem1Size;
    if (!in_memory(base, bytes)) return a;
    a.host = rt::guest_ptr(0x80000000u | base);
    a.stride = stride;
    a.last = stride == 0 ? 0xFFFFFFFFu : (end - base - bytes) / stride;
    a.any = true;
    return a;
}
Format format_of(const Registers& r, unsigned vat) {
    Format f{};
    f.vcd_lo = r.cp[0x50];
    f.vcd_hi = r.cp[0x60];
    f.a = r.cp[0x70 + vat];
    f.b = r.cp[0x80 + vat];
    f.c = r.cp[0x90 + vat];
    f.pos_n = (f.a & 1) ? 3 : 2;
    f.pos_fmt = (f.a >> 1) & 7;
    f.pos_scale = 1.0f / float(1u << ((f.a >> 4) & 31));
    f.nbt = (f.a >> 9) & 1;
    f.nrm_fmt = (f.a >> 10) & 7;
    f.col_fmt[0] = (f.a >> 14) & 7;
    f.col_fmt[1] = (f.a >> 18) & 7;
    f.dequant = (f.a >> 30) & 1;
    f.nbt3 = (f.a >> 31) & 1;
    const unsigned n[8] = {(f.a >> 21) & 1, f.b & 1, (f.b >> 9) & 1, (f.b >> 18) & 1, (f.b >> 27) & 1, (f.c >> 5) & 1, (f.c >> 14) & 1, (f.c >> 23) & 1};
    const unsigned fm[8] = {(f.a >> 22) & 7, (f.b >> 1) & 7, (f.b >> 10) & 7, (f.b >> 19) & 7, (f.b >> 28) & 7, (f.c >> 6) & 7, (f.c >> 15) & 7, (f.c >> 24) & 7};
    const unsigned fr[8] = {(f.a >> 25) & 31, (f.b >> 4) & 31, (f.b >> 13) & 31, (f.b >> 22) & 31, f.c & 31, (f.c >> 9) & 31, (f.c >> 18) & 31, (f.c >> 27) & 31};
    for (int i = 0; i < 8; ++i) {
        f.tc_n[i] = n[i] ? 2 : 1;
        f.tc_fmt[i] = fm[i];
        f.tc_scale[i] = 1.0f / float(1u << fr[i]);
    }
    // The indexed arrays the format reads, with the bytes an element takes.
    const auto indexed = [](unsigned how) { return how >= 2; };
    if (indexed((f.vcd_lo >> 9) & 3)) f.arrays[0] = resolve_array(r, 0, component_size(f.pos_fmt) * f.pos_n);
    if (indexed((f.vcd_lo >> 11) & 3))
        f.arrays[1] = resolve_array(r, 1, component_size(f.nrm_fmt) * (f.nbt && f.nbt3 ? 9 : f.nbt ? 9 : 3));
    for (unsigned c = 0; c < 2; ++c)
        if (indexed((f.vcd_lo >> (13 + 2 * c)) & 3)) f.arrays[2 + c] = resolve_array(r, 2 + c, color_size(f.col_fmt[c]));
    for (unsigned t = 0; t < 8; ++t)
        if (indexed((f.vcd_hi >> (2 * t)) & 3)) f.arrays[4 + t] = resolve_array(r, 4 + t, component_size(f.tc_fmt[t]) * f.tc_n[t]);
    return f;
}

// Read one vertex; returns the bytes it took.
size_t read_vertex(const Registers& r, const Format& f, const uint8_t* p, Vertex& v, uint32_t& has) {
    const uint8_t* const start = p;
    std::memset(&v, 0, sizeof v);
    has = 0;
    uint8_t mtx[9];
    const uint32_t ia = r.cp[0x30], ib = r.cp[0x40];
    mtx[0] = uint8_t(ia & 63);
    for (int i = 0; i < 4; ++i) mtx[1 + i] = uint8_t((ia >> (6 + 6 * i)) & 63);
    for (int i = 0; i < 4; ++i) mtx[5 + i] = uint8_t((ib >> (6 * i)) & 63);
    if (f.vcd_lo & 1) {
        mtx[0] = *p++ & 63;
        has |= kHasPosMatrix;
    }
    for (int i = 0; i < 8; ++i)
        if (f.vcd_lo & (2u << i)) {
            mtx[1 + i] = *p++ & 63;
            has |= kHasTexMatrix0 << i;
        }
    // Direct data is here; indexed data is in an array.
    const auto source = [&](unsigned how, unsigned array, uint32_t bytes) -> const uint8_t* {
        if (how == 1) {
            const uint8_t* d = p;
            p += bytes;
            return d;
        }
        uint32_t index;
        if (how == 2) index = *p++;
        else index = be16(p), p += 2;
        const Array& a = f.arrays[array];
        if (a.any && index <= a.last) [[likely]] {
            if (g_dff_use) [[unlikely]] note_array(array, index * a.stride, bytes);
            return a.host + size_t(index) * a.stride;
        }
        return array_data(r, array, index, bytes);  // reports it
    };
    // Position.
    {
        const unsigned how = (f.vcd_lo >> 9) & 3;
        if (how == 0) rt::fatal(nullptr, "a vertex format without a position");
        const unsigned cs = component_size(f.pos_fmt);
        const uint8_t* d = source(how, 0, cs * f.pos_n);
        for (unsigned k = 0; k < f.pos_n; ++k) v.pos[k] = component(d + k * cs, f.pos_fmt, f.pos_scale, f.dequant);
    }
    // Normal, and with NBT the tangent and binormal.
    {
        const unsigned how = (f.vcd_lo >> 11) & 3;
        if (how) {
            const unsigned cs = component_size(f.nrm_fmt);
            float* dst[3] = {v.normal, v.tangent, v.binormal};
            const unsigned vectors = f.nbt ? 3 : 1;
            if (how != 1 && f.nbt && f.nbt3) {  // an index for each vector
                for (unsigned k = 0; k < 3; ++k) {
                    const uint8_t* d = source(how, 1, cs * 9);
                    for (unsigned j = 0; j < 3; ++j) dst[k][j] = normal_component(d + (3 * k + j) * cs, f.nrm_fmt);
                }
            } else {
                const uint8_t* d = source(how, 1, cs * 3 * vectors);
                for (unsigned k = 0; k < vectors; ++k)
                    for (unsigned j = 0; j < 3; ++j) dst[k][j] = normal_component(d + (3 * k + j) * cs, f.nrm_fmt);
            }
            has |= kHasNormal | (f.nbt ? kHasTangent | kHasBinormal : 0);
            std::memcpy(g_last_normal, v.normal, 12);
            if (f.nbt) std::memcpy(g_last_tangent, v.tangent, 12), std::memcpy(g_last_binormal, v.binormal, 12);
        }
        if (!(has & kHasNormal)) std::memcpy(v.normal, g_last_normal, 12);
        if (!(has & kHasTangent)) std::memcpy(v.tangent, g_last_tangent, 12), std::memcpy(v.binormal, g_last_binormal, 12);
    }
    // Colours: with only one, both channels get it; with none, white.
    uint8_t col[2][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
    for (unsigned c = 0; c < 2; ++c) {
        const unsigned how = (f.vcd_lo >> (13 + 2 * c)) & 3;
        if (!how) continue;
        read_color(source(how, 2 + c, color_size(f.col_fmt[c])), f.col_fmt[c], col[c]);
        has |= kHasColor0 << c;
    }
    if ((has & kHasColor0) && !(has & kHasColor1)) std::memcpy(col[1], col[0], 4);
    if (!(has & kHasColor0) && (has & kHasColor1)) std::memcpy(col[0], col[1], 4);
    std::memcpy(v.color0, col[0], 4);
    std::memcpy(v.color1, col[1], 4);
    // Texture coordinates.
    for (unsigned t = 0; t < 8; ++t) {
        const unsigned how = (f.vcd_hi >> (2 * t)) & 3;
        if (!how) continue;
        const unsigned cs = component_size(f.tc_fmt[t]);
        const uint8_t* d = source(how, 4 + t, cs * f.tc_n[t]);
        for (unsigned k = 0; k < f.tc_n[t]; ++k) v.uv[t][k] = component(d + k * cs, f.tc_fmt[t], f.tc_scale[t], f.dequant);
        has |= kHasUv0 << t;
    }
    v.matrices[0] = mtx[0] | (uint32_t(mtx[1]) << 8) | (uint32_t(mtx[2]) << 16) | (uint32_t(mtx[3]) << 24);
    v.matrices[1] = mtx[4] | (uint32_t(mtx[5]) << 8) | (uint32_t(mtx[6]) << 16) | (uint32_t(mtx[7]) << 24);
    v.matrices[2] = mtx[8];
    return size_t(p - start);
}

// A vertex format compiled to steps, once per primitive: a vertex then runs
// only the attributes its format has, with formats, sizes and the default
// matrix indices worked out (read_vertex's rules; it remains for the one
// layout this doesn't take, NBT with an index per vector).
struct Step {
    enum Kind : uint8_t { kPosMtx, kTexMtx, kPos, kNormal, kColor, kUv } kind;
    uint8_t how, fmt, n, slot, cs;  // n: components (or normal vectors); slot: matrix, colour or UV number
    float scale;
};
struct Plan {
    Step steps[24];
    unsigned count = 0;
    uint8_t mtx[9];
    uint32_t has = 0;  // what the attributes give
    bool usable = true;
    // A decoder specialised to this layout where it is one of the game's common ones (decode_fused); else null.
    size_t (*fused_small)(const Registers&, const Format&, const Plan&, const uint8_t*, uint32_t, SmallVertex*) = nullptr;
    size_t (*fused_tiny)(const Registers&, const Format&, const Plan&, const uint8_t*, uint32_t, TinyVertex*) = nullptr;
    size_t (*fused_full)(const Registers&, const Format&, const Plan&, const uint8_t*, uint32_t, Vertex*) = nullptr;
};
void choose_fused(Plan& pl);
Plan plan_of(const Registers& r, const Format& f) {
    Plan pl;
    const uint32_t ia = r.cp[0x30], ib = r.cp[0x40];
    pl.mtx[0] = uint8_t(ia & 63);
    for (int i = 0; i < 4; ++i) pl.mtx[1 + i] = uint8_t((ia >> (6 + 6 * i)) & 63);
    for (int i = 0; i < 4; ++i) pl.mtx[5 + i] = uint8_t((ib >> (6 * i)) & 63);
    const auto add = [&](Step st) { pl.steps[pl.count++] = st; };
    if (f.vcd_lo & 1) add({Step::kPosMtx, 0, 0, 0, 0, 0, 0}), pl.has |= kHasPosMatrix;
    for (uint8_t i = 0; i < 8; ++i)
        if (f.vcd_lo & (2u << i)) add({Step::kTexMtx, 0, 0, 0, i, 0, 0}), pl.has |= kHasTexMatrix0 << i;
    const unsigned pos_how = (f.vcd_lo >> 9) & 3;
    if (pos_how == 0) rt::fatal(nullptr, "a vertex format without a position");
    add({Step::kPos, uint8_t(pos_how), uint8_t(f.pos_fmt), uint8_t(f.pos_n), 0, uint8_t(component_size(f.pos_fmt)), f.pos_scale});
    if (const unsigned how = (f.vcd_lo >> 11) & 3) {
        if (how != 1 && f.nbt && f.nbt3) pl.usable = false;
        add({Step::kNormal, uint8_t(how), uint8_t(f.nrm_fmt), uint8_t(f.nbt ? 3 : 1), 0, uint8_t(component_size(f.nrm_fmt)), 0});
        pl.has |= kHasNormal | (f.nbt ? kHasTangent | kHasBinormal : 0);
    }
    for (uint8_t c = 0; c < 2; ++c)
        if (const unsigned how = (f.vcd_lo >> (13 + 2 * c)) & 3) {
            add({Step::kColor, uint8_t(how), uint8_t(f.col_fmt[c]), 0, c, uint8_t(color_size(f.col_fmt[c])), 0});
            pl.has |= kHasColor0 << c;
        }
    for (uint8_t t = 0; t < 8; ++t)
        if (const unsigned how = (f.vcd_hi >> (2 * t)) & 3) {
            add({Step::kUv, uint8_t(how), uint8_t(f.tc_fmt[t]), uint8_t(f.tc_n[t]), t, uint8_t(component_size(f.tc_fmt[t])), f.tc_scale[t]});
            pl.has |= kHasUv0 << t;
        }
    choose_fused(pl);
    return pl;
}

// One vertex by its plan, straight into the frame's layout (Vertex, or
// SmallVertex when the format has no tangents and at most four texture
// coordinates); returns the bytes it took.
template <class V>
size_t decode_vertex(const Registers& r, const Format& f, const Plan& pl, const uint8_t* p, V& v) {
    constexpr bool kFull = std::is_same_v<V, Vertex>;
    const uint8_t* const start = p;
    uint8_t mtx[9];
    std::memcpy(mtx, pl.mtx, 9);
    std::memset(v.uv, 0, sizeof v.uv);
    v.pos[2] = 0;
    uint8_t col[2][4] = {{255, 255, 255, 255}, {255, 255, 255, 255}};
    const auto fetch = [&](unsigned how, unsigned array, uint32_t bytes) -> const uint8_t* {
        if (how == 1) {
            const uint8_t* d = p;
            p += bytes;
            return d;
        }
        uint32_t index;
        if (how == 2) index = *p++;
        else index = be16(p), p += 2;
        const Array& a = f.arrays[array];
        if (a.any && index <= a.last) [[likely]] {
            if (g_dff_use) [[unlikely]] note_array(array, index * a.stride, bytes);
            return a.host + size_t(index) * a.stride;
        }
        return array_data(r, array, index, bytes);  // reports it
    };
    bool normal = false;
    for (unsigned i = 0; i < pl.count; ++i) {
        const Step& st = pl.steps[i];
        switch (st.kind) {
            case Step::kPosMtx: mtx[0] = *p++ & 63; break;
            case Step::kTexMtx: mtx[1 + st.slot] = *p++ & 63; break;
            case Step::kPos: {
                const uint8_t* d = fetch(st.how, 0, st.cs * st.n);
                if (st.fmt == 4) {  // float, the common case
                    for (unsigned k = 0; k < st.n; ++k) v.pos[k] = as_float(be32(d + 4 * k));
                } else {
                    for (unsigned k = 0; k < st.n; ++k) v.pos[k] = component(d + k * st.cs, st.fmt, st.scale, f.dequant);
                }
                break;
            }
            case Step::kNormal: {
                const uint8_t* d = fetch(st.how, 1, st.cs * 3 * st.n);
                float nbt[3][3];
                if (st.fmt == 4) {  // float
                    for (unsigned k = 0; k < st.n; ++k)
                        for (unsigned j = 0; j < 3; ++j) nbt[k][j] = as_float(be32(d + 4 * (3 * k + j)));
                } else {
                    for (unsigned k = 0; k < st.n; ++k)
                        for (unsigned j = 0; j < 3; ++j) nbt[k][j] = normal_component(d + (3 * k + j) * st.cs, st.fmt);
                }
                std::memcpy(v.normal, nbt[0], 12);
                normal = true;
                std::memcpy(g_last_normal, nbt[0], 12);
                if (st.n == 3) {
                    std::memcpy(g_last_tangent, nbt[1], 12), std::memcpy(g_last_binormal, nbt[2], 12);
                    if constexpr (kFull) std::memcpy(v.tangent, nbt[1], 12), std::memcpy(v.binormal, nbt[2], 12);
                }
                break;
            }
            case Step::kColor: read_color(fetch(st.how, 2 + st.slot, st.cs), st.fmt, col[st.slot]); break;
            case Step::kUv: {
                const uint8_t* d = fetch(st.how, 4 + st.slot, st.cs * st.n);
                if constexpr (!kFull) {
                    if (st.slot >= std::extent_v<decltype(V::uv)>) [[unlikely]]
                        rt::fatal(nullptr, "texture coordinate %u in the compact vertex layout", unsigned(st.slot));
                }
                if (st.fmt == 4) {  // float
                    for (unsigned k = 0; k < st.n; ++k) v.uv[st.slot][k] = as_float(be32(d + 4 * k));
                } else {
                    for (unsigned k = 0; k < st.n; ++k) v.uv[st.slot][k] = component(d + k * st.cs, st.fmt, st.scale, f.dequant);
                }
                break;
            }
        }
    }
    if (!normal) std::memcpy(v.normal, g_last_normal, 12);
    if constexpr (kFull)
        if (!(pl.has & kHasTangent)) std::memcpy(v.tangent, g_last_tangent, 12), std::memcpy(v.binormal, g_last_binormal, 12);
    if ((pl.has & kHasColor0) && !(pl.has & kHasColor1)) std::memcpy(col[1], col[0], 4);
    if (!(pl.has & kHasColor0) && (pl.has & kHasColor1)) std::memcpy(col[0], col[1], 4);
    std::memcpy(v.color0, col[0], 4);
    std::memcpy(v.color1, col[1], 4);
    v.matrices[0] = mtx[0] | (uint32_t(mtx[1]) << 8) | (uint32_t(mtx[2]) << 16) | (uint32_t(mtx[3]) << 24);
    v.matrices[1] = mtx[4] | (uint32_t(mtx[5]) << 8) | (uint32_t(mtx[6]) << 16) | (uint32_t(mtx[7]) << 24);
    v.matrices[2] = mtx[8];
    return size_t(p - start);
}

// Each of `count` vertices' element of an attribute (`size` bytes a vertex,
// the attribute at `off`): inline (How 1) or by an 8- or 16-bit index (2, 3)
// into array `a`, given to body(i, element). The way of fetching is a template
// argument so each loop has none of the others' branches.
template <unsigned How, class Body>
void each_at(const Registers& r, const Array& a, unsigned array, uint32_t bytes, const uint8_t* data, unsigned size, unsigned off,
             uint32_t count, Body& body) {
    const bool dff = g_dff_use;
    const uint8_t* const host = a.host;
    const uint32_t stride = a.stride, last = a.any ? a.last : 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* p = data + size_t(i) * size + off;
        if constexpr (How == 1) {
            body(i, p);
        } else {
            const uint32_t index = How == 2 ? p[0] : be16(p);
            if (a.any && index <= last) [[likely]] {
                if (dff) [[unlikely]] note_array(array, index * stride, bytes);
                body(i, host + size_t(index) * stride);
            } else {
                body(i, array_data(r, array, index, bytes));  // reports it
            }
        }
    }
}
template <class Body>
void each_element(unsigned how, const Registers& r, const Array& a, unsigned array, uint32_t bytes, const uint8_t* data, unsigned size,
                  unsigned off, uint32_t count, Body& body) {
    switch (how) {
        case 1: each_at<1>(r, a, array, bytes, data, size, off, count, body); break;
        case 2: each_at<2>(r, a, array, bytes, data, size, off, count, body); break;
        default: each_at<3>(r, a, array, bytes, data, size, off, count, body); break;
    }
}

// A primitive's vertices by its plan, an attribute at a time across all of
// them (decode_vertex's results, the same bytes): each inner loop has one
// format and one way of fetching, rather than a switch per attribute per
// vertex. Every vertex of a primitive has the same size, so each attribute
// sits at the same offset in each. Returns the bytes the vertices took.
template <class V>
size_t decode_columns(const Registers& r, const Format& f, const Plan& pl, const uint8_t* data, uint32_t count, V* out) {
    constexpr bool kFull = std::is_same_v<V, Vertex>;
    // Each step's offset in a vertex, and the vertex's size.
    unsigned at[24], size = 0;
    for (unsigned i = 0; i < pl.count; ++i) {
        const Step& st = pl.steps[i];
        at[i] = size;
        if (st.kind == Step::kPosMtx || st.kind == Step::kTexMtx) size += 1;
        else if (st.how == 2) size += 1;
        else if (st.how == 3) size += 2;
        else size += st.kind == Step::kPos ? st.cs * st.n : st.kind == Step::kNormal ? st.cs * 3u * st.n
                     : st.kind == Step::kColor ? st.cs : st.cs * st.n;
    }
    // The defaults for what the format doesn't give.
    uint32_t m0 = 0, m1 = 0;
    for (int k = 0; k < 4; ++k) m0 |= uint32_t(pl.mtx[k]) << (8 * k), m1 |= uint32_t(pl.mtx[4 + k]) << (8 * k);
    for (uint32_t i = 0; i < count; ++i) {
        V& v = out[i];
        std::memset(v.uv, 0, sizeof v.uv);
        v.pos[0] = v.pos[1] = v.pos[2] = 0;  // (every byte of a vertex is written: the frame's vectors don't zero them)
        std::memcpy(v.normal, g_last_normal, 12);
        if constexpr (kFull) std::memcpy(v.tangent, g_last_tangent, 12), std::memcpy(v.binormal, g_last_binormal, 12);
        std::memset(v.color0, 255, 4), std::memset(v.color1, 255, 4);
        v.matrices[0] = m0, v.matrices[1] = m1, v.matrices[2] = pl.mtx[8];
    }
    for (unsigned s = 0; s < pl.count; ++s) {
        const Step& st = pl.steps[s];
        const unsigned off = at[s];
        // Each vertex's element of the step's array, inline or by index, given to `body`.
        const auto each = [&](unsigned array, uint32_t bytes, auto&& body) {
            each_element(st.how, r, f.arrays[array], array, bytes, data, size, off, count, body);
        };
        switch (st.kind) {
            case Step::kPosMtx:
                for (uint32_t i = 0; i < count; ++i)
                    out[i].matrices[0] = (out[i].matrices[0] & ~0xFFu) | (data[size_t(i) * size + off] & 63u);
                break;
            case Step::kTexMtx: {
                const unsigned slot = 1u + st.slot, word = slot / 4, shift = 8 * (slot % 4);
                for (uint32_t i = 0; i < count; ++i)
                    out[i].matrices[word] = (out[i].matrices[word] & ~(0xFFu << shift)) | ((data[size_t(i) * size + off] & 63u) << shift);
                break;
            }
            case Step::kPos: {
                const uint32_t bytes = st.cs * st.n;
                if (st.fmt == 4 && st.n == 3) {
                    each(0, bytes, [&](uint32_t i, const uint8_t* d) {
                        out[i].pos[0] = as_float(be32(d)), out[i].pos[1] = as_float(be32(d + 4)), out[i].pos[2] = as_float(be32(d + 8));
                    });
                } else {
                    each(0, bytes, [&](uint32_t i, const uint8_t* d) {
                        for (unsigned k = 0; k < st.n; ++k) out[i].pos[k] = component(d + k * st.cs, st.fmt, st.scale, f.dequant);
                    });
                }
                break;
            }
            case Step::kNormal: {
                const uint32_t bytes = st.cs * 3u * st.n;
                if (st.n == 1) {
                    if (st.fmt == 4) {
                        each(1, bytes, [&](uint32_t i, const uint8_t* d) {
                            for (unsigned j = 0; j < 3; ++j) out[i].normal[j] = as_float(be32(d + 4 * j));
                        });
                    } else {
                        each(1, bytes, [&](uint32_t i, const uint8_t* d) {
                            for (unsigned j = 0; j < 3; ++j) out[i].normal[j] = normal_component(d + j * st.cs, st.fmt);
                        });
                    }
                } else {  // normal, tangent, binormal
                    each(1, bytes, [&](uint32_t i, const uint8_t* d) {
                        float nbt[3][3];
                        for (unsigned k = 0; k < 3; ++k)
                            for (unsigned j = 0; j < 3; ++j) nbt[k][j] = normal_component(d + (3 * k + j) * st.cs, st.fmt);
                        std::memcpy(out[i].normal, nbt[0], 12);
                        if constexpr (kFull) std::memcpy(out[i].tangent, nbt[1], 12), std::memcpy(out[i].binormal, nbt[2], 12);
                        if (i + 1 == count) std::memcpy(g_last_tangent, nbt[1], 12), std::memcpy(g_last_binormal, nbt[2], 12);
                    });
                }
                std::memcpy(g_last_normal, out[count - 1].normal, 12);
                break;
            }
            case Step::kColor:
                each(2u + st.slot, st.cs, [&](uint32_t i, const uint8_t* d) { read_color(d, st.fmt, st.slot ? out[i].color1 : out[i].color0); });
                break;
            case Step::kUv: {
                if constexpr (!kFull) {
                    if (st.slot >= std::extent_v<decltype(V::uv)>) [[unlikely]]
                        rt::fatal(nullptr, "texture coordinate %u in the compact vertex layout", unsigned(st.slot));
                }
                const uint32_t bytes = st.cs * st.n;
                const unsigned a = 4u + st.slot, slot = st.slot;
                const float scale = st.scale;
                if (st.fmt == 4 && st.n == 2) {
                    each(a, bytes, [&](uint32_t i, const uint8_t* d) {
                        out[i].uv[slot][0] = as_float(be32(d)), out[i].uv[slot][1] = as_float(be32(d + 4));
                    });
                } else if (st.fmt == 3 && st.n == 2) {  // signed 16-bit pairs: the game's models
                    each(a, bytes, [&](uint32_t i, const uint8_t* d) {
                        out[i].uv[slot][0] = float(int16_t(be16(d))) * scale;
                        out[i].uv[slot][1] = float(int16_t(be16(d + 2))) * scale;
                    });
                } else {
                    each(a, bytes, [&](uint32_t i, const uint8_t* d) {
                        for (unsigned k = 0; k < st.n; ++k) out[i].uv[slot][k] = component(d + k * st.cs, st.fmt, scale, f.dequant);
                    });
                }
                break;
            }
        }
    }
    // One colour given: both are it (decode_vertex's rule).
    const bool c0 = pl.has & kHasColor0, c1 = pl.has & kHasColor1;
    if (c0 != c1)
        for (uint32_t i = 0; i < count; ++i)
            c0 ? std::memcpy(out[i].color1, out[i].color0, 4) : std::memcpy(out[i].color0, out[i].color1, 4);
    return size_t(size) * count;
}

// The game's models come in a handful of layouts, every attribute by a 16-bit
// index (a race frame: 30,000 primitives of 4.6 vertices, nine in ten in six
// layouts). For those, a decoder compiled for the layout: one pass a vertex,
// each attribute at a constant offset with a constant format, and every byte
// of the vertex written once (decode_columns' results, the same bytes).
enum Code : uint8_t { kCPosMtx, kCTexMtx0, kCTexMtx1, kCPosF3, kCNrmS16, kCNrmF, kCColRGBA8, kCUv0S16, kCUv1S16 };
constexpr unsigned code_bytes(Code c) { return c == kCPosMtx || c == kCTexMtx0 || c == kCTexMtx1 ? 1 : 2; }
template <Code... C>
constexpr std::array<unsigned, sizeof...(C)> code_offsets() {
    std::array<unsigned, sizeof...(C)> at{};
    unsigned o = 0, i = 0;
    ((at[i++] = o, o += code_bytes(C)), ...);
    return at;
}
struct FusedIn {
    const Registers& r;
    const Format& f;
    // The element an index in the vertex points at (array_data reports one outside memory).
    QILIN_RECORDER_INLINE const uint8_t* element(unsigned array, uint32_t bytes, const uint8_t* p) const {
        const Array& a = f.arrays[array];
        const uint32_t index = be16(p);
        if (a.any && index <= a.last) [[likely]] return a.host + size_t(index) * a.stride;
        return array_data(r, array, index, bytes);
    }
};
template <Code C, unsigned At, class V>
QILIN_RECORDER_INLINE void fused_step(const FusedIn& in, const uint8_t* p, V& v) {
    if constexpr (C == kCPosMtx) {
        v.matrices[0] = (v.matrices[0] & ~0xFFu) | (p[At] & 63u);
    } else if constexpr (C == kCTexMtx0) {
        v.matrices[0] = (v.matrices[0] & ~0xFF00u) | ((p[At] & 63u) << 8);
    } else if constexpr (C == kCTexMtx1) {
        v.matrices[0] = (v.matrices[0] & ~0xFF0000u) | ((p[At] & 63u) << 16);
    } else if constexpr (C == kCPosF3) {
        const uint8_t* d = in.element(0, 12, p + At);
        v.pos[0] = as_float(be32(d)), v.pos[1] = as_float(be32(d + 4)), v.pos[2] = as_float(be32(d + 8));
    } else if constexpr (C == kCNrmS16) {
        const uint8_t* d = in.element(1, 6, p + At);
        for (unsigned j = 0; j < 3; ++j) v.normal[j] = float(static_cast<int16_t>(be16(d + 2 * j))) * (1.0f / 16384.0f);
    } else if constexpr (C == kCNrmF) {
        const uint8_t* d = in.element(1, 12, p + At);
        for (unsigned j = 0; j < 3; ++j) v.normal[j] = as_float(be32(d + 4 * j));
    } else if constexpr (C == kCColRGBA8) {  // the only colour: both channels are it
        const uint8_t* d = in.element(2, 4, p + At);
        std::memcpy(v.color0, d, 4), std::memcpy(v.color1, d, 4);
    } else {
        constexpr unsigned slot = C == kCUv0S16 ? 0 : 1;
        const uint8_t* d = in.element(4 + slot, 4, p + At);
        const float scale = in.f.tc_scale[slot];
        v.uv[slot][0] = float(int16_t(be16(d))) * scale;
        v.uv[slot][1] = float(int16_t(be16(d + 2))) * scale;
    }
}
template <class V, Code... C>
size_t decode_fused(const Registers& r, const Format& f, const Plan& pl, const uint8_t* data, uint32_t count, V* out) {
    constexpr bool kFull = std::is_same_v<V, Vertex>;
    constexpr unsigned kSize = (code_bytes(C) + ...);
    static constexpr auto kAt = code_offsets<C...>();
    constexpr bool kNormal = ((C == kCNrmS16 || C == kCNrmF) || ...), kColor = ((C == kCColRGBA8) || ...);
    uint32_t m0 = 0, m1 = 0;
    for (int k = 0; k < 4; ++k) m0 |= uint32_t(pl.mtx[k]) << (8 * k), m1 |= uint32_t(pl.mtx[4 + k]) << (8 * k);
    const uint32_t m2 = pl.mtx[8];
    const FusedIn in{r, f};
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* const p = data + size_t(i) * kSize;
        V& v = out[i];
        if constexpr (!kNormal) std::memcpy(v.normal, g_last_normal, 12);
        if constexpr (kFull) std::memcpy(v.tangent, g_last_tangent, 12), std::memcpy(v.binormal, g_last_binormal, 12);
        if constexpr (!kColor) std::memset(v.color0, 255, 4), std::memset(v.color1, 255, 4);
        std::memset(v.uv, 0, sizeof v.uv);
        v.matrices[0] = m0, v.matrices[1] = m1, v.matrices[2] = m2;
        [&]<size_t... I>(std::index_sequence<I...>) { (fused_step<C, kAt[I]>(in, p, v), ...); }(std::make_index_sequence<sizeof...(C)>{});
    }
    if constexpr (kNormal) std::memcpy(g_last_normal, out[count - 1].normal, 12);
    return size_t(kSize) * count;
}
struct Fused {
    Code codes[6];
    unsigned n;
    decltype(Plan::fused_small) small;
    decltype(Plan::fused_tiny) tiny;
    decltype(Plan::fused_full) full;
};
template <Code... C>
constexpr Fused fused() { return {{C...}, sizeof...(C), &decode_fused<SmallVertex, C...>, &decode_fused<TinyVertex, C...>, &decode_fused<Vertex, C...>}; }
// The layouts, as counted in race frames (one view and four) and the menus.
constexpr Fused kFused[] = {
    fused<kCPosF3, kCColRGBA8, kCUv0S16>(),
    fused<kCPosF3, kCNrmS16, kCUv0S16>(),
    fused<kCPosF3, kCColRGBA8, kCUv0S16, kCUv1S16>(),
    fused<kCPosF3, kCNrmF>(),
    fused<kCPosMtx, kCTexMtx0, kCPosF3, kCNrmS16, kCUv0S16>(),
    fused<kCPosF3, kCNrmS16>(),
    fused<kCPosF3, kCNrmF, kCUv0S16>(),
    fused<kCPosMtx, kCPosF3, kCNrmS16, kCUv0S16>(),
    fused<kCPosF3, kCUv0S16>(),
    fused<kCPosMtx, kCTexMtx1, kCPosF3, kCNrmS16, kCUv0S16>(),  // (an eighth of a race frame's primitives)
};
void choose_fused(Plan& pl) {
    if (!pl.usable || pl.count > 6) return;
    Code codes[6];
    for (unsigned i = 0; i < pl.count; ++i) {
        const Step& st = pl.steps[i];
        const bool idx16 = st.how == 3;
        switch (st.kind) {
            case Step::kPosMtx: codes[i] = kCPosMtx; break;
            case Step::kTexMtx: if (st.slot > 1) return; codes[i] = st.slot ? kCTexMtx1 : kCTexMtx0; break;
            case Step::kPos: if (!idx16 || st.fmt != 4 || st.n != 3) return; codes[i] = kCPosF3; break;
            case Step::kNormal:
                if (!idx16 || st.n != 1 || (st.fmt != 3 && st.fmt != 4)) return;
                codes[i] = st.fmt == 3 ? kCNrmS16 : kCNrmF;
                break;
            case Step::kColor:
                if (!idx16 || st.fmt != 5 || st.slot != 0) return;
                codes[i] = kCColRGBA8;
                break;
            case Step::kUv:
                if (!idx16 || st.fmt != 3 || st.n != 2 || st.slot > 1) return;
                codes[i] = st.slot ? kCUv1S16 : kCUv0S16;
                break;
        }
    }
    for (const Fused& k : kFused)
        if (k.n == pl.count && std::equal(codes, codes + pl.count, k.codes)) {
            pl.fused_small = k.small, pl.fused_tiny = k.tiny, pl.fused_full = k.full;
            return;
        }
}
template <class V>
size_t decode_best(const Registers& r, const Format& f, const Plan& pl, const uint8_t* data, uint32_t count, V* out) {
    if (!g_dff_use) [[likely]] {  // (the capture notes the elements read: decode_columns does)
        if constexpr (std::is_same_v<V, SmallVertex>) {
            if (pl.fused_small) return pl.fused_small(r, f, pl, data, count, out);
        } else if constexpr (std::is_same_v<V, TinyVertex>) {
            if (pl.fused_tiny) return pl.fused_tiny(r, f, pl, data, count, out);
        } else {
            if (pl.fused_full) return pl.fused_full(r, f, pl, data, count, out);
        }
    }
    return decode_columns(r, f, pl, data, count, out);
}

// decode_best, and with MKDD_CHECK_DECODE=1 checked against
// decode_vertex vertex by vertex (byte for byte, the last normals too): a
// fatal error at the first difference. For tests and after changing either.
template <class V>
size_t decode_checked(const Registers& r, const Format& f, const Plan& pl, const uint8_t* data, uint32_t count, V* out) {
    if (!g_check_decode) [[likely]] return decode_best(r, f, pl, data, count, out);
    float last[3][3];
    std::memcpy(last[0], g_last_normal, 12), std::memcpy(last[1], g_last_tangent, 12), std::memcpy(last[2], g_last_binormal, 12);
    const size_t used = decode_best(r, f, pl, data, count, out);
    float after[3][3];
    std::memcpy(after[0], g_last_normal, 12), std::memcpy(after[1], g_last_tangent, 12), std::memcpy(after[2], g_last_binormal, 12);
    std::memcpy(g_last_normal, last[0], 12), std::memcpy(g_last_tangent, last[1], 12), std::memcpy(g_last_binormal, last[2], 12);
    std::vector<V> ref(count);
    size_t ref_used = 0;
    for (uint32_t i = 0; i < count; ++i) ref_used += decode_vertex(r, f, pl, data + ref_used, ref[i]);
    if (ref_used != used) rt::fatal(nullptr, "MKDD_CHECK_DECODE: %llu bytes by columns, %llu by vertex",
                                    static_cast<unsigned long long>(used), static_cast<unsigned long long>(ref_used));
    for (uint32_t i = 0; i < count; ++i)
        if (std::memcmp(&ref[i], &out[i], sizeof(V)) != 0)
            rt::fatal(nullptr, "MKDD_CHECK_DECODE: vertex %u of %u differs", i, count);
    if (std::memcmp(after[0], g_last_normal, 12) || std::memcmp(after[1], g_last_tangent, 12) || std::memcmp(after[2], g_last_binormal, 12))
        rt::fatal(nullptr, "MKDD_CHECK_DECODE: the last normals differ");
    return used;
}

// ---- state

// The parts of a State, by what they're made from: a new state copies the one
// before and redoes only the parts whose registers changed since (most draws
// change a matrix or two, not 5 KB of state).
enum : uint32_t {
    kPartPos = 1,       // XF 0x000-0x0FF
    kPartNormal = 2,    // XF 0x400-0x45F
    kPartPost = 4,      // XF 0x500-0x5FF
    kPartLights = 8,    // XF 0x600-0x67F
    kPartXfRegs = 16,   // XF 0x1000 on (and the fog, which reads the viewport)
    kPartBp = 32,       // BP, the TEV colour registers and konstants
    kPartAll = 63,
};
uint32_t g_parts = kPartAll;  // changed since the last state was built
// The load each matrix slot holds (the frame's loads), and the view last given.
uint32_t g_slot_load[kMatrixSlots] = {kNoLoad, kNoLoad, kNoLoad, kNoLoad, kNoLoad, kNoLoad, kNoLoad, kNoLoad, kNoLoad, kNoLoad};
uint16_t g_view = kNoView;
void build_lights(const uint32_t* xf, State& s);
void build_xf_regs(const uint32_t* xf, State& s);
void build_bp(const Registers& r, State& s);

void build_state(const Registers& r, State& s, uint32_t parts) {
    if (parts == kPartAll) std::memset(&s, 0, sizeof s);
    const uint32_t* xf = r.xf;
    if (parts & kPartPos)
        for (int i = 0; i < 256; ++i) s.posmtx[i / 4][i % 4] = as_float(xf[i]);
    if (parts & kPartNormal)
        for (int i = 0; i < 32; ++i)
            for (int j = 0; j < 3; ++j) s.nrmmtx[i][j] = as_float(xf[0x400 + 3 * i + j]);
    if (parts & kPartPost)
        for (int i = 0; i < 256; ++i) s.postmtx[i / 4][i % 4] = as_float(xf[0x500 + i]);
    if (parts & kPartLights) build_lights(xf, s);
    if (parts & kPartXfRegs) build_xf_regs(xf, s);
    if (parts & (kPartBp | kPartXfRegs)) build_bp(r, s);
}

// The blocks of a State (States::kBlock bytes each) that rebuilding `parts` may change.
static_assert(offsetof(State, posmtx) == 0 && offsetof(State, nrmmtx) < offsetof(State, postmtx) &&
              offsetof(State, postmtx) < offsetof(State, lights) && offsetof(State, lights) < offsetof(State, projection) &&
              offsetof(State, projection) < offsetof(State, pixel_center) && offsetof(State, xf) < offsetof(State, bp) &&
              offsetof(State, bp) < offsetof(State, texdims) && offsetof(State, fog_range) < sizeof(State),
              "blocks_of takes the parts' members to lie in this order");
constexpr uint32_t blocks_in(size_t from, size_t to) {  // the blocks bytes [from, to) lie in
    uint32_t m = 0;
    for (size_t b = from / States::kBlock; b * States::kBlock < to; ++b) m |= 1u << b;
    return m;
}
constexpr uint32_t blocks_of(uint32_t parts) {
    uint32_t m = 0;
    if (parts & kPartPos) m |= blocks_in(offsetof(State, posmtx), offsetof(State, nrmmtx));
    if (parts & kPartNormal) m |= blocks_in(offsetof(State, nrmmtx), offsetof(State, postmtx));
    if (parts & kPartPost) m |= blocks_in(offsetof(State, postmtx), offsetof(State, lights));
    if (parts & kPartLights) m |= blocks_in(offsetof(State, lights), offsetof(State, projection));
    if (parts & kPartXfRegs) m |= blocks_in(offsetof(State, projection), offsetof(State, bp));
    if (parts & (kPartBp | kPartXfRegs)) m |= blocks_in(offsetof(State, bp), sizeof(State));
    return m;
}
static_assert(blocks_of(kPartAll) == (1u << States::kBlocks) - 1 && blocks_of(kPartPos) == 0xFu && blocks_in(0, 0) == 0);
// The state as last built: each new one is made from it, then kept in the frame's blocks.
State g_built{};
// Within the parts, what was written since the last state: XF words [g_pos_lo, g_pos_hi) of the position
// matrices, and BP registers by bit. A state then copies those alone (most draws load a matrix or two
// and change a few colours).
uint32_t g_pos_lo = 0, g_pos_hi = 256;
uint64_t g_bp_dirty[4] = {~0ull, ~0ull, ~0ull, ~0ull};
void build_bp_derived(const Registers& r, State& s);
void build_whole(const Registers& r, State& s) {
    build_state(r, s, kPartAll);
    g_pos_lo = 256, g_pos_hi = 0;
    std::fill(std::begin(g_bp_dirty), std::end(g_bp_dirty), 0ull);
}
// build_state(r, s, parts) on the state last built, copying only what was written; returns the blocks it may have changed.
uint32_t build_changed(const Registers& r, State& s, uint32_t parts) {
    uint32_t blocks = blocks_of(parts & (kPartNormal | kPartPost | kPartLights | kPartXfRegs));
    if ((parts & kPartPos) && g_pos_lo < g_pos_hi) {
        std::memcpy(&s.posmtx[0][0] + g_pos_lo, r.xf + g_pos_lo, (g_pos_hi - g_pos_lo) * 4);  // (floats by their bits)
        blocks |= blocks_in(offsetof(State, posmtx) + 4 * g_pos_lo, offsetof(State, posmtx) + 4 * g_pos_hi);
    }
    build_state(r, s, parts & (kPartNormal | kPartPost | kPartLights));
    if (parts & kPartXfRegs) build_xf_regs(r.xf, s);
    if (parts & kPartBp)
        for (unsigned w = 0; w < 4; ++w)
            for (uint64_t m = g_bp_dirty[w]; m; m &= m - 1) {
                const unsigned reg = 64 * w + unsigned(std::countr_zero(m));
                s.bp[reg / 4][reg % 4] = r.bp[reg];
                blocks |= blocks_in(offsetof(State, bp) + 4 * reg, offsetof(State, bp) + 4 * reg + 4);
            }
    if (parts & (kPartBp | kPartXfRegs)) {
        // (The write mask, 0xFE, changes without a BP write's notice: set by its own write, cleared by the next.)
        s.bp[0xFE / 4][0xFE % 4] = r.bp[0xFE];
        build_bp_derived(r, s);
        blocks |= blocks_in(offsetof(State, bp) + 4 * 0xFE, sizeof(State));
    }
    g_pos_lo = 256, g_pos_hi = 0;
    std::fill(std::begin(g_bp_dirty), std::end(g_bp_dirty), 0ull);
    return blocks;
}

void build_lights(const uint32_t* xf, State& s) {
    for (int l = 0; l < 8; ++l) {
        const uint32_t* L = xf + 0x600 + 16 * l;
        auto& d = s.lights[l];
        for (int k = 0; k < 4; ++k) d.color[k] = float((L[3] >> (24 - 8 * k)) & 255);
        for (int k = 0; k < 3; ++k) d.cosatt[k] = as_float(L[4 + k]);
        for (int k = 0; k < 3; ++k) d.distatt[k] = as_float(L[7 + k]);
        if (std::fabs(d.distatt[0]) < 0.00001f && std::fabs(d.distatt[1]) < 0.00001f && std::fabs(d.distatt[2]) < 0.00001f)
            d.distatt[0] = 0.00001f;
        for (int k = 0; k < 3; ++k) d.pos[k] = as_float(L[10 + k]);
        double n = 0;
        for (int k = 0; k < 3; ++k) n += double(as_float(L[13 + k])) * as_float(L[13 + k]);
        n = 1.0 / std::sqrt(n);
        for (int k = 0; k < 3; ++k) {
            const float x = static_cast<float>(as_float(L[13 + k]) * n);
            d.dir[k] = std::isfinite(x) ? x : 0.0f;
        }
    }
}

void build_xf_regs(const uint32_t* xf, State& s) {
    // Projection.
    const float* p = reinterpret_cast<const float*>(xf + 0x1020);
    float m[16] = {};
    if (xf[0x1026] == 0) {  // perspective
        m[0] = p[0], m[2] = p[1], m[5] = p[2], m[6] = p[3], m[10] = p[4], m[11] = p[5], m[14] = -1.0f;
    } else {
        m[0] = p[0], m[3] = p[1], m[5] = p[2], m[7] = p[3], m[10] = p[4], m[11] = p[5], m[15] = 1.0f;
    }
    std::memcpy(s.projection, m, sizeof m);
    // The viewport: half-pixel correction and the depth range.
    const float wd = as_float(xf[0x101A]), ht = as_float(xf[0x101B]), z_range = as_float(xf[0x101C]), far_z = as_float(xf[0x101F]);
    constexpr float kCenter = 7.0f / 12.0f - 0.5f;
    s.pixel_center[0] = kCenter * (2.0f / (2.0f * wd));
    s.pixel_center[1] = kCenter * (2.0f / (2.0f * ht));
    s.pixel_center[2] = 1.0f;
    s.pixel_center[3] = 0.0f;
    if (z_range < 0.0f || z_range > far_z || std::fabs(z_range) > 16777215.0f || std::fabs(far_z) > 16777215.0f) {
        // Out of the depth range Vulkan can express: the shader applies it.
        s.pixel_center[2] = std::fabs(z_range) / 16777215.0f;
        s.pixel_center[3] = z_range < 0.0f ? far_z / 16777215.0f : 1.0f - far_z / 16777215.0f;
    }
    for (int c = 0; c < 2; ++c)
        for (int k = 0; k < 4; ++k) {
            s.materials[c][k] = int32_t((xf[0x100A + c] >> (24 - 8 * k)) & 255);
            s.materials[2 + c][k] = int32_t((xf[0x100C + c] >> (24 - 8 * k)) & 255);
        }
    s.xf[0][0] = xf[0x1009] & 3;
    s.xf[0][1] = xf[0x100E];
    s.xf[0][2] = xf[0x100F];
    s.xf[0][3] = xf[0x1010];
    s.xf[1][0] = xf[0x1011];
    s.xf[1][1] = xf[0x1012] & 1;
    s.xf[1][2] = xf[0x103F] & 15;
    for (int i = 0; i < 8; ++i) s.xf[2 + i / 4][i % 4] = xf[0x1040 + i];
    for (int i = 0; i < 8; ++i) s.xf[4 + i / 4][i % 4] = xf[0x1050 + i];
}

void build_bp(const Registers& r, State& s) {
    for (int i = 0; i < 256; ++i) s.bp[i / 4][i % 4] = r.bp[i];
    build_bp_derived(r, s);
}
// What the shaders take from BP registers worked out (and the fog's part of the viewport).
void build_bp_derived(const Registers& r, State& s) {
    const uint32_t* xf = r.xf;
    for (unsigned m2 = 0; m2 < 8; ++m2) {
        const uint32_t image0 = r.bp[(m2 < 4 ? 0x88 : 0xA8) + m2 % 4];
        s.texdims[m2][0] = int32_t((image0 & 0x3FF) + 1);
        s.texdims[m2][1] = int32_t(((image0 >> 10) & 0x3FF) + 1);
        s.texdims[m2][2] = int32_t((r.bp[0x30 + 2 * m2] & 0xFFFF) + 1);
        s.texdims[m2][3] = int32_t((r.bp[0x31 + 2 * m2] & 0xFFFF) + 1);
    }
    std::memcpy(s.tev_regs, g_tev_regs, sizeof g_tev_regs);
    std::memcpy(s.konst, g_konst, sizeof g_konst);
    // Indirect matrices: 11-bit signed S0.10 entries; the scale is 2^(scale - 17).
    for (int i = 0; i < 3; ++i) {
        const uint32_t A = r.bp[0x06 + 3 * i], B = r.bp[0x07 + 3 * i], C = r.bp[0x08 + 3 * i];
        const auto s11 = [](uint32_t v) { return int32_t(v << 21) >> 21; };
        const int scale = int(((A >> 22) & 3) | (((B >> 22) & 3) << 2) | (((C >> 22) & 1) << 4));
        s.indmtx[2 * i][0] = s11(A), s.indmtx[2 * i][1] = s11(B), s.indmtx[2 * i][2] = s11(C);
        s.indmtx[2 * i + 1][0] = s11(A >> 11), s.indmtx[2 * i + 1][1] = s11(B >> 11), s.indmtx[2 * i + 1][2] = s11(C >> 11);
        s.indmtx[2 * i][3] = 17 - scale;
    }
    // Fog: the shader's terms, as Dolphin's PixelShaderManager derives them.
    const uint32_t f1 = r.bp[0xEE], f2 = r.bp[0xEF], f3 = r.bp[0xF0], f4 = r.bp[0xF1];
    const auto fog_float = [](uint32_t v) {  // 1 sign, 8 exponent, 11 mantissa: a float's top bits
        return std::bit_cast<float>(((v & 0xFFFFF) << 12));
    };
    const uint32_t b_mag = f2 & 0xFFFFFF;
    const uint32_t b_shf = f3 & 31;
    const float a = fog_float(f1);
    const float c = fog_float(f4);
    s.fog[0][0] = a;  // the shader: ze = a * 2^24 / (b_mag - (z >> b_shift)), then minus c
    s.fog[0][1] = c;
    s.misc[0][0] = int32_t(b_mag);
    s.misc[0][1] = int32_t(b_shf);
    for (int k = 0; k < 3; ++k) s.misc[1][k] = int32_t((r.bp[0xF2] >> (16 - 8 * k)) & 255);
    // Fog's range adjustment: the screen's centre in -1..1, and the K table (scaled by 4 / 256).
    const uint32_t range = r.bp[0xE8];
    const float wd2 = 2.0f * as_float(xf[0x101A]);
    s.fog[0][2] = ((float(int(range & 0x3FF)) - 342.0f) / wd2) * 2.0f - 1.0f;
    s.fog[0][3] = wd2;
    for (int i = 0; i < 5; ++i) {
        const uint32_t k = r.bp[0xE9 + i];
        s.fog_range[(2 * i) / 4][(2 * i) % 4] = float(k & 0xFFF) / 64.0f;
        s.fog_range[(2 * i + 1) / 4][(2 * i + 1) % 4] = float((k >> 12) & 0xFFF) / 64.0f;
    }
    // Z texture bias.
    s.misc[2][0] = int32_t(r.bp[0xF4] & 0xFFFFFF);
}

// ---- pipeline state

constexpr uint8_t kSrcFactor[8] = {0, 1, 4, 5, 6, 7, 8, 9};  // GX source factors as Vulkan's
constexpr uint8_t kDstFactor[8] = {0, 1, 2, 3, 6, 7, 8, 9};

PipelineKey pipeline_key(const uint32_t* bp, bool& alpha_target) {
    PipelineKey k;
    const uint32_t blend = bp[0x41], pe = bp[0x43], dst_alpha = bp[0x42];
    alpha_target = (pe & 7) == 1;  // RGBA6_Z24
    const bool color_update = (blend >> 3) & 1;
    const bool alpha_update = ((blend >> 4) & 1) && alpha_target;
    const bool constant_alpha = ((dst_alpha >> 8) & 1) && alpha_update;
    uint8_t src = 1, dst = 0, src_a = 1, dst_a = 0;
    if (blend & 1) {
        k.blend = 1;
        if ((blend >> 11) & 1) {  // subtract: dst - src, factors one
            k.op = k.op_alpha = 2;  // VK_BLEND_OP_REVERSE_SUBTRACT
            src = dst = src_a = dst_a = 1;
            if (constant_alpha) k.op_alpha = 0, src_a = 1, dst_a = 0;
        } else {
            unsigned s = (blend >> 8) & 7, d = (blend >> 5) & 7;
            if (!alpha_target) {  // no destination alpha: it reads as 1
                if (s == 6) s = 1;
                else if (s == 7) s = 0;
                if (d == 6) d = 1;
                else if (d == 7) d = 0;
            }
            src = kSrcFactor[s];
            dst = kDstFactor[d];
            const unsigned sa = s == 2 ? 6 : s == 3 ? 7 : s;  // colour factors become alpha factors
            const unsigned da = d == 2 ? 4 : d == 3 ? 5 : d;
            src_a = kSrcFactor[sa];
            dst_a = kDstFactor[da];
            if (constant_alpha) src_a = 1, dst_a = 0;
        }
    } else if ((blend >> 1) & 1) {
        const unsigned op = (blend >> 12) & 15;
        if (op != 3) k.logic = uint8_t(op + 1);  // anything but copy (the renderer says if it has to approximate)
    }
    k.src = src, k.dst = dst, k.src_alpha = src_a, k.dst_alpha = dst_a;
    k.write_mask = uint8_t((color_update ? 7 : 0) | (alpha_update ? 8 : 0));
    return k;
}

// The alpha test (BP 0xF3: two compares and how they combine) can't fail.
constexpr bool alpha_test_passes(uint32_t test) {
    const uint32_t a = (test >> 16) & 7, b = (test >> 19) & 7;  // 7 always, 0 never
    switch ((test >> 22) & 3) {
        case 0: return a == 7 && b == 7;                         // and
        case 1: return a == 7 || b == 7;                         // or
        case 2: return (a == 7 && b == 0) || (a == 0 && b == 7);  // xor
        default: return (a == 7 && b == 7) || (a == 0 && b == 0);  // xnor
    }
}
static_assert(alpha_test_passes((7u << 16) | (7u << 19)) && alpha_test_passes((1u << 22) | (7u << 16) | (4u << 19)) &&
              !alpha_test_passes((6u << 16) | (7u << 19)) && !alpha_test_passes((1u << 22) | (6u << 16) | (3u << 19)));

void viewport_and_scissor(const Registers& r, Draw& d) {
    const float wd = as_float(r.xf[0x101A]), ht = as_float(r.xf[0x101B]), z_range = as_float(r.xf[0x101C]);
    const float x_orig = as_float(r.xf[0x101D]), y_orig = as_float(r.xf[0x101E]), far_z = as_float(r.xf[0x101F]);
    const uint32_t tl = r.bp[0x20], br = r.bp[0x21], off = r.bp[0x59];
    const int left = int((tl >> 12) & 0x7FF), top = int(tl & 0x7FF), right = int((br >> 12) & 0x7FF), bottom = int(br & 0x7FF);
    const int x_off = int(off & 0x3FF) << 1, y_off = int((off >> 10) & 0x3FF) << 1;
    // The hardware's coordinates wrap at 1024 (and carry 342 of offset): of
    // the ways the scissor can be read, take the one that covers most of the viewport.
    const float vl = std::min(x_orig - wd, x_orig + wd), vr = std::max(x_orig - wd, x_orig + wd);
    const float vt = std::min(y_orig - ht, y_orig + ht), vb = std::max(y_orig - ht, y_orig + ht);
    struct Range { int off, start, end; };
    struct Ranges {  // (on the stack: this runs for every draw)
        Range r[9];
        unsigned n = 0;
        const Range* begin() const { return r; }
        const Range* end() const { return r + n; }
    };
    const auto ranges = [](int start, int end, int offset, int dim) {
        Ranges out;
        for (int extra = -4096; extra <= 4096; extra += 1024) {
            const int o = offset + extra, s = std::clamp(start - o, 0, dim), e = std::clamp(end - o + 1, 0, dim);
            if (s < e) out.r[out.n++] = {o, s, e};
        }
        return out;
    };
    Range bx{x_off, 0, 0}, by{y_off, 0, 0};
    long best = -1, best_area = -1;
    if (left <= right && top <= bottom)
        for (const Ranges xs = ranges(left, right, x_off, 640), ys = ranges(top, bottom, y_off, 528); const Range& x : xs)
            for (const Range& y : ys) {
                const float x0 = std::clamp(float(x.start + x.off), vl, vr), x1 = std::clamp(float(x.end + x.off), vl, vr);
                const float y0 = std::clamp(float(y.start + y.off), vt, vb), y1 = std::clamp(float(y.end + y.off), vt, vb);
                const long cover = long((x1 - x0) * (y1 - y0)), area = long(x.end - x.start) * (y.end - y.start);
                if (cover > best || (cover == best && area > best_area)) best = cover, best_area = area, bx = x, by = y;
            }
    d.scissor[0] = bx.start;
    d.scissor[1] = by.start;
    d.scissor[2] = bx.end - bx.start;
    d.scissor[3] = by.end - by.start;
    float x = (x_orig - float(bx.off)) - wd, y = (y_orig - float(by.off)) + ht;
    float w = 2.0f * wd, h = -2.0f * ht;
    if (w < 0) x += w, w = -w;
    if (h < 0) y += h, h = -h;
    float min_depth = (far_z - z_range) / 16777216.0f, max_depth = far_z / 16777216.0f;
    if (z_range < 0.0f || z_range > far_z || std::fabs(z_range) > 16777215.0f || std::fabs(far_z) > 16777215.0f) {
        if (z_range < 0.0f) min_depth = 16777215.0f / 16777216.0f, max_depth = 0.0f;
        else min_depth = 0.0f, max_depth = 16777215.0f / 16777216.0f;
    }
    d.viewport[0] = x, d.viewport[1] = y, d.viewport[2] = w, d.viewport[3] = h;
    d.viewport[4] = max_depth;  // Vulkan's minDepth: depth at the near plane (reversed range)
    d.viewport[5] = min_depth;
}

// ---- textures for a draw

void bind_textures(const Registers& r, Draw& d) {
    for (auto& t : d.textures) t = -1;
    const uint32_t gen = r.bp[0x00];
    const unsigned stages = ((gen >> 10) & 15) + 1, ind_stages = (gen >> 16) & 7;
    bool used[8] = {};
    for (unsigned s = 0; s < stages; ++s) {
        const uint32_t order = r.bp[0x28 + s / 2] >> ((s & 1) ? 12 : 0);
        if ((order >> 6) & 1) used[order & 7] = true;
    }
    for (unsigned i = 0; i < ind_stages; ++i) used[(r.bp[0x27] >> (6 * i)) & 7] = true;
    for (unsigned m = 0; m < 8; ++m) {
        if (!used[m]) continue;
        uint32_t w, h;
        d.textures[m] = texture_for(r.bp, m, *g_frame, w, h);
        const uint32_t mode0 = r.bp[(m < 4 ? 0x80 : 0xA0) + m % 4], mode1 = r.bp[(m < 4 ? 0x84 : 0xA4) + m % 4];
        Sampler& s = d.samplers[m];
        s.wrap_s = uint8_t(mode0 & 3);
        s.wrap_t = uint8_t((mode0 >> 2) & 3);
        s.mag_linear = uint8_t((mode0 >> 4) & 1);
        s.mip = uint8_t((mode0 >> 5) & 3);
        s.min_linear = uint8_t((mode0 >> 7) & 1);
        s.lod_bias = int8_t((mode0 >> 9) & 0xFF);
        s.min_lod = uint8_t(mode1 & 0xFF);
        s.max_lod = uint8_t((mode1 >> 8) & 0xFF);
        s.max_aniso = uint8_t(std::min<uint32_t>((mode0 >> 19) & 3, 2));
        if (s.wrap_s == 3) s.wrap_s = 0;
        if (s.wrap_t == 3) s.wrap_t = 0;
    }
}

// ---- the frame queue

// Never destroyed: the GPU thread (detached) may be waiting in submit() at exit,
// where glibc's pthread_cond_destroy would block (see gpu.cpp).
std::mutex& g_queue_lock = *new std::mutex;
std::condition_variable& g_queue_cv = *new std::condition_variable;
std::deque<std::unique_ptr<Frame>> g_queue;
bool g_rendering = false;

}  // namespace

void set_rendering(bool on) {
    std::lock_guard l(g_queue_lock);
    g_rendering = on;
}

void submit(std::unique_ptr<Frame> frame) {
    std::unique_lock l(g_queue_lock);
    if (!g_rendering) return;
    // Two frames ahead at most: the CPU waits, as it does on a full FIFO.
    const uint64_t t0 = now_ns();
    g_queue_cv.wait(l, [] { return g_queue.size() < 2 || !g_rendering; });
    g_stats.wait_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
    g_queue.push_back(std::move(frame));
    cpu_copies_queued(*g_queue.back());  // (the game's read of them waits from now on: texture.hpp)
    g_queue_cv.notify_all();
}

static std::vector<std::unique_ptr<Frame>> g_spare_frames;  // drawn, to reuse (under g_queue_lock)

void recycle_frame(std::unique_ptr<Frame> frame) {
    if (!frame) return;
    frame->clear();
    std::lock_guard l(g_queue_lock);
    if (g_spare_frames.size() < 3) g_spare_frames.push_back(std::move(frame));
}

static std::unique_ptr<Frame> spare_frame() {
    std::lock_guard l(g_queue_lock);
    if (g_spare_frames.empty()) return std::make_unique<Frame>();
    auto f = std::move(g_spare_frames.back());
    g_spare_frames.pop_back();
    return f;
}

std::unique_ptr<Frame> next_frame(unsigned timeout_ms) { return next_frame_us(uint64_t{timeout_ms} * 1000); }
std::unique_ptr<Frame> next_frame_us(uint64_t timeout_us) {
    std::unique_lock l(g_queue_lock);
    if (!g_queue_cv.wait_for(l, std::chrono::microseconds(timeout_us), [] { return !g_queue.empty(); })) return nullptr;
    auto f = std::move(g_queue.front());
    g_queue.pop_front();
    g_queue_cv.notify_all();
    return f;
}

void state_changed() { g_dirty = true, g_parts = kPartAll, cp_changed(); }
void matrix_loaded(uint32_t address, const uint32_t* words, uint32_t tag, const float* view, uint32_t id, uint8_t context) {
    Frame& f = *g_frame;
    if (view) {
        // The frame's views, each once: a handful (a camera a player, and whatever 2D is drawn under).
        uint16_t found = kNoView;
        if (g_view != kNoView && std::memcmp(f.views[g_view].m, view, 48) == 0) found = g_view;
        for (size_t i = 0; found == kNoView && i < f.views.size(); ++i)
            if (std::memcmp(f.views[i].m, view, 48) == 0) found = uint16_t(i);
        if (found == kNoView && f.views.size() < 64) {
            found = uint16_t(f.views.size());
            View& v = f.views.emplace_back();
            std::memcpy(v.m, view, 48);
            v.has_projection = false;
        }
        g_view = found;
    }
    if (address % 12 != 0 || address / 12 >= kMatrixSlots) return;  // a texture matrix, or not a whole slot
    Load& l = f.loads.emplace_back();
    l.tag = tag, l.id = id, l.context = context, l.slot = uint16_t(address / 12), l.view = g_view;
    std::memcpy(l.mv, words, 48);
    g_slot_load[l.slot] = uint32_t(f.loads.size() - 1);
    g_dirty = true, g_parts |= kPartPos;  // (the same matrix again is still another load: the state's info changes)
}

void xf_changed(uint32_t address, uint32_t count) {
    const uint32_t end = address + count;
    const auto overlaps = [&](uint32_t lo, uint32_t hi) { return address < hi && end > lo; };
    uint32_t parts = 0;
    if (overlaps(0x000, 0x100)) parts |= kPartPos, g_pos_lo = std::min(g_pos_lo, address), g_pos_hi = std::max(g_pos_hi, std::min(end, 0x100u));
    if (overlaps(0x400, 0x460)) parts |= kPartNormal;
    if (overlaps(0x500, 0x600)) parts |= kPartPost;
    if (overlaps(0x600, 0x680)) parts |= kPartLights;
    if (overlaps(0x1000, 0x1060)) parts |= kPartXfRegs;
    if (parts) g_dirty = true, g_parts |= parts;  // the rest of XF memory isn't in the state
}

static void record_primitive_timed(const Registers& r, uint8_t opcode, const uint8_t* data, uint32_t count);
// The frame's last packet is a draw of triangles that the next primitive may
// join (record_primitive_timed): its place in the frame's draws, and what the
// vertices of a primitive joining it must be like.
static struct {
    bool valid = false;
    uint32_t draw = 0, has = 0;
    uint8_t small = 0;  // the vertex layout (kLayout...)
} g_join;
// The vertex format last used and its plan, kept while the CP registers
// they're made from stay as they are (cp_changed): a model's display list is
// hundreds of short strips in one format.
static struct {
    bool valid = false;
    unsigned vat = 0;
    Format fmt;
    Plan plan;
} g_kept;
void cp_changed() { g_kept.valid = false; }
static bool g_capture_next = false;
void mark_captured_frame() { g_capture_next = true; }
static uint32_t g_primitive_serial = 0;
void set_primitive_serial(uint32_t n) { g_primitive_serial = n; }
const char* (*g_bp_source)(uint32_t, char*, size_t) = nullptr;
void* g_decoding_cpu = nullptr;
bool g_dff_use = false;
void (*g_dff_use_hook)(uint32_t, uint32_t) = nullptr;
static void record_copy_timed(const Registers& r, uint32_t command);

// (Timed by the decoder, a batch of commands at a time: two clock readings a
// primitive cost as much as a five-vertex strip.)
void record_primitive(const Registers& r, uint8_t opcode, const uint8_t* data, uint32_t count) {
    record_primitive_timed(r, opcode, data, count);
}
void record_copy(const Registers& r, uint32_t command) { record_copy_timed(r, command); }

static void record_primitive_timed(const Registers& r, uint8_t opcode, const uint8_t* data, uint32_t count) {
    if (count == 0) return;
    Frame& f = *g_frame;
    if (const unsigned vat = opcode & 7; !g_kept.valid || g_kept.vat != vat) {
        g_kept.fmt = format_of(r, vat);
        g_kept.plan = plan_of(r, g_kept.fmt);
        g_kept.vat = vat, g_kept.valid = true;
    }
    const Format& fmt = g_kept.fmt;
    uint32_t has = 0;
    const Plan& plan = g_kept.plan;
    if (g_dff_use) [[unlikely]]
        std::fill(std::begin(g_use_lo), std::end(g_use_lo), ~0u), std::fill(std::begin(g_use_hi), std::end(g_use_hi), 0u);
    // A compact layout when nothing needs the rest: no tangents or binormals in the format, and
    // none sent before (emboss texgen reads the last ones), and no texture coordinates past the
    // second (TinyVertex: a race's every draw) or the fourth (SmallVertex: the menus' models).
    // Decoding can't change the answer: the last tangents only change in a format that has them.
    const auto beyond = [](unsigned uvs) { return kHasTangent | kHasBinormal | (((1u << (8 - uvs)) - 1) * (kHasUv0 << uvs)); };
    constexpr uint32_t kBig = kHasTangent | kHasBinormal | (((1u << (8 - kSmallUvs)) - 1) * (kHasUv0 << kSmallUvs));
    static_assert(kBig == (kHasTangent | kHasBinormal | (kHasUv0 << 4) | (kHasUv0 << 5) | (kHasUv0 << 6) | (kHasUv0 << 7)));
    static constexpr float kZero3[3] = {};
    const bool force_full = g_force_full;
    const auto zero_bytes = [](const float* v) {  // (all 12 bytes: -0 isn't zero here)
        uint32_t w[3];
        std::memcpy(w, v, 12);
        return (w[0] | w[1] | w[2]) == 0;
    };
    const bool no_last_nbt = zero_bytes(g_last_tangent) && zero_bytes(g_last_binormal);
    const auto layout_of = [&](uint32_t what, bool no_nbt) {
        if (force_full || !no_nbt || (what & kBig)) return kLayoutFull;
        return (what & beyond(kTinyUvs)) ? kLayoutSmall : kLayoutTiny;
    };
    uint8_t small;
    uint32_t first;
    const auto decode_into = [&](auto& vertices) {
        first = uint32_t(vertices.size());
        vertices.resize(first + count);
        data += decode_checked(r, fmt, plan, data, count, vertices.data() + first);
    };
    if (plan.usable) {  // straight into the frame's vertices
        has = plan.has;
        small = layout_of(has, no_last_nbt);
        if (small == kLayoutTiny) decode_into(f.tiny);
        else if (small == kLayoutSmall) decode_into(f.small);
        else decode_into(f.vertices);
    } else {  // the general reader (rare formats), then the layout
        thread_local std::vector<Vertex> decoded;
        decoded.resize(count);
        for (uint32_t i = 0; i < count; ++i) data += read_vertex(r, fmt, data, decoded[i], has);
        small = layout_of(has, std::memcmp(g_last_tangent, kZero3, 12) == 0 && std::memcmp(g_last_binormal, kZero3, 12) == 0);
        const auto compact = [&](auto& vertices) {
            first = uint32_t(vertices.size());
            vertices.resize(first + count);
            for (uint32_t i = 0; i < count; ++i) {
                const Vertex& v = decoded[i];
                auto& o = vertices[first + i];
                std::memcpy(o.pos, v.pos, 12);
                std::memcpy(o.normal, v.normal, 12);
                std::memcpy(o.color0, v.color0, 4);
                std::memcpy(o.color1, v.color1, 4);
                std::memcpy(o.uv, v.uv, sizeof o.uv);
                std::memcpy(o.matrices, v.matrices, 12);
            }
        };
        if (small == kLayoutTiny) {
            compact(f.tiny);
        } else if (small == kLayoutSmall) {
            compact(f.small);
        } else {
            first = uint32_t(f.vertices.size());
            f.vertices.insert(f.vertices.end(), decoded.begin(), decoded.end());
        }
    }
    if (g_dff_use) [[unlikely]]
        for (unsigned a = 0; a < 16; ++a)
            if (g_use_lo[a] < g_use_hi[a]) dff_use_memory(r.cp[0xA0 + a] + g_use_lo[a], g_use_hi[a] - g_use_lo[a]);
    auto& ix = f.indices;
    const unsigned type = (opcode >> 3) & 7;
    // Sized once, then written through a pointer.
    const auto grow = [&](size_t n) {
        const size_t at = ix.size();
        ix.resize(at + n);
        return ix.data() + at;
    };
    // The indices that draw vertices [base, base + n) of triangle type `kind` as a list.
    const auto triangles = [&](unsigned kind, uint32_t base, uint32_t n) {
        switch (kind) {
            case 0: case 1: {  // quads (and the second quad kind)
                uint32_t* o = grow(size_t(n / 4) * 6);
                for (uint32_t i = 0; i + 3 < n; i += 4, o += 6) {
                    const uint32_t v = base + i;
                    o[0] = v, o[1] = v + 1, o[2] = v + 2, o[3] = v, o[4] = v + 2, o[5] = v + 3;
                }
                break;
            }
            case 2: {  // triangles
                uint32_t* o = grow(size_t(n / 3) * 3);
                for (uint32_t i = 0; i < n / 3 * 3; ++i) o[i] = base + i;
                break;
            }
            case 3: {  // strip: every other triangle's base two vertices swapped, so all wind alike
                if (n < 3) break;
                uint32_t* o = grow(size_t(n - 2) * 3);
                for (uint32_t i = 0; i + 2 < n; ++i, o += 3) {
                    const uint32_t v = base + i;
                    if (i & 1) o[0] = v + 1, o[1] = v, o[2] = v + 2;
                    else o[0] = v, o[1] = v + 1, o[2] = v + 2;
                }
                break;
            }
            default: {  // fan
                if (n < 3) break;
                uint32_t* o = grow(size_t(n - 2) * 3);
                for (uint32_t i = 1; i + 1 < n; ++i, o += 3) o[0] = base, o[1] = base + i, o[2] = base + i + 1;
                break;
            }
        }
    };
    // A primitive drawn with exactly the state of the draw before it joins that
    // draw: no register the state reads was written in between (g_dirty), no
    // copy came between, and its vertices are of the same kind. This game's
    // models are strips of five vertices on average, 5,000 primitives a course,
    // and a draw each cost more to record and to draw than their triangles.
    // The joined draw is an indexed triangle list, in the primitives' order.
    // (Not while MKDD_DFF captures, which numbers draws by primitive;
    // MKDD_JOIN=0 gives every primitive its own draw.)
    if (g_join_on && type <= 4 && !g_dirty && g_join.valid && g_join.has == has && g_join.small == small && !g_dff_use &&
        !f.packets.empty() && f.packets.back().kind == Packet::kDraw && f.packets.back().index == g_join.draw) {
        Draw& d = f.draws[g_join.draw];
        if (d.direct) {  // the first to join it: its own vertices as indices
            const uint32_t v0 = d.first_index, n = d.index_count;
            d.first_index = uint32_t(ix.size());
            triangles(d.direct == 2 ? 3 : 2, v0, n);
            d.direct = 0;
        }
        triangles(type, first, count);
        d.index_count = uint32_t(ix.size()) - d.first_index;
        return;
    }
    if (g_dirty) {
        // From the frame's last state where there is one, redoing what changed.
        // (Into the state last built, then into the frame: only the blocks that differ are copied.)
        const uint32_t parts = f.states.empty() ? uint32_t(kPartAll) : g_parts;
        if (parts == kPartAll) build_whole(r, g_built), f.states.push(g_built);
        else f.states.push(g_built, build_changed(r, g_built, parts));
        if (g_check_decode && parts != kPartAll) [[unlikely]] {  // MKDD_CHECK_DECODE: the same as built whole
            State whole, kept;
            build_state(r, whole, kPartAll);
            f.states.resolve(f.states.size() - 1, kept);
            if (std::memcmp(&whole, &kept, sizeof whole) != 0)
                rt::fatal(nullptr, "MKDD_CHECK_DECODE: a state built from the one before differs (parts %X)", parts);
        }
        g_state = uint32_t(f.states.size() - 1);
        g_dirty = false, g_parts = 0;
        // Its info: the slots' loads and the view; the view's projection, if this is the first perspective one under it.
        StateInfo& info = f.state_info.emplace_back();
        std::memcpy(info.load, g_slot_load, sizeof info.load);
        info.view = g_view;
        // (The one that reaches farthest, if several are used under it: the scene's own, not a
        // close-up's; far = p5 / p4.)
        if (g_view != kNoView && r.xf[0x1026] == 0) {
            View& v = f.views[g_view];
            const float p4 = as_float(r.xf[0x1024]), p5 = as_float(r.xf[0x1025]);
            const float far_z = p4 != 0 ? std::fabs(p5 / p4) : 0.0f;
            const float had = v.has_projection && v.projection[4] != 0 ? std::fabs(v.projection[5] / v.projection[4]) : -1.0f;
            if (far_z > had) {
                std::memcpy(v.projection, r.xf + 0x1020, 24);
                v.has_projection = true;
            }
        }
    }
    Draw d{};
    d.state = g_state;
    d.has = has;
    d.first_index = uint32_t(f.indices.size());
    switch (type) {
        case 0: case 1: triangles(type, first, count); break;  // quads
        // Lists and strips draw the vertices as they are, in order: no indices.
        // Vulkan's strips wind alternate triangles as GX's do (each odd
        // triangle's vertices are a rotation of GX's), and the shaders have no
        // flat inputs, so the provoking vertex doesn't matter.
        case 2: d.direct = 1, d.first_index = first, d.index_count = count / 3 * 3; break;    // triangles
        case 3: d.direct = 2, d.first_index = first, d.index_count = count >= 3 ? count : 0; break;  // strip
        case 4: triangles(type, first, count); break;  // fan
        case 5: d.direct = 1, d.first_index = first, d.index_count = count / 2 * 2; break;            // lines
        case 6: d.direct = 2, d.first_index = first, d.index_count = count >= 2 ? count : 0; break;  // line strip
        default: d.direct = 1, d.first_index = first, d.index_count = count; break;                  // points
    }
    if (!d.direct) d.index_count = uint32_t(ix.size()) - d.first_index;
    if (d.index_count == 0) return;
    bool alpha_target;
    d.pipeline = pipeline_key(r.bp, alpha_target);
    d.pipeline.small_vertices = small;
    d.pipeline.no_clip = uint8_t(r.xf[0x1005] & 1);  // GXSetClipMode(GX_CLIP_DISABLE)
    d.shape = shape_id(r.bp);
    f.alpha_target = alpha_target;
    d.pipeline.topology = type <= 4 ? 0 : type <= 6 ? 1 : 2;
    d.line_width = uint8_t(r.bp[0x22] & 255);         // in sixths of a pixel
    d.point_size = uint8_t((r.bp[0x22] >> 8) & 255);
    static constexpr uint8_t kCull[4] = {0, 2, 1, 3};  // none, back, front, all as Vulkan's
    d.cull = d.pipeline.topology == 0 ? kCull[(r.bp[0x00] >> 14) & 3] : 0;
    const uint32_t z = r.bp[0x40];
    d.depth_test = z & 1;
    d.depth_func = uint8_t((z >> 1) & 7);  // GX's and Vulkan's compare ops number alike
    d.depth_write = (z >> 4) & 1;
    if ((r.bp[0x43] & 7) >= 3) d.depth_test = 0, d.depth_write = 0;  // no Z in this pixel format
    // Z textures change the depth written only when Z is compared after texturing
    // (BP 0x43 bit 6 clear); then the shader writes depth.
    if (((r.bp[0xF5] >> 2) & 3) != 0 && d.depth_test && !((r.bp[0x43] >> 6) & 1)) d.pipeline.depth_out = 1;
    // Z compared before texturing: the depth is written whatever the alpha test
    // then says. Only a test that can fail needs the pipeline that orders it so.
    else if (d.depth_test && d.depth_write && ((r.bp[0x43] >> 6) & 1) && !alpha_test_passes(r.bp[0xF3])) d.pipeline.depth_out = 2;
    viewport_and_scissor(r, d);
    bind_textures(r, d);
    d.primitive = g_primitive_serial;
    if (const uint32_t row = r.cp[0x30] & 63; row % 3 == 0 && row / 3 < kMatrixSlots && g_slot_load[row / 3] != kNoLoad)
        d.tag = f.loads[g_slot_load[row / 3]].tag, d.context = f.loads[g_slot_load[row / 3]].context;
    f.packets.push_back({Packet::kDraw, uint32_t(f.draws.size())});
    g_join = {type <= 4, uint32_t(f.draws.size()), has, small};
    f.draws.push_back(d);
}

static void draw_done();
static uint32_t g_bp_now[256] = {};  // the BP registers as written since the last copy to the frame buffer (draw_done foretells from them)
void bp_written(uint32_t reg, uint32_t value, bool changed) {
    g_bp_now[reg & 0xFF] = value;
    if (reg == 0x45) draw_done();  // (the early frame end, if asked for)
    if (reg == 0x66) textures_invalidated(), g_join.valid = false;  // (a texture may be new under the same registers)
    if (!changed) return;  // the state is what it was (commands are acted on by gpu.cpp either way)
    g_parts |= kPartBp;
    g_bp_dirty[(reg & 0xFF) / 64] |= 1ull << (reg % 64);
    if (reg >= 0xE0 && reg <= 0xE7) {  // TEV colour registers and konstants: 11-bit signed, the type bit says which
        const unsigned i = (reg - 0xE0) / 2;
        const bool konst = (value >> 23) & 1;
        int32_t(&dst)[4] = konst ? g_konst[i] : g_tev_regs[i];
        const auto s11 = [](uint32_t v) { return int32_t(v << 21) >> 21; };
        if (((reg - 0xE0) & 1) == 0) dst[0] = s11(value), dst[3] = s11(value >> 12);  // red, alpha
        else dst[2] = s11(value), dst[1] = s11(value >> 12);                       // blue, green
    }
    g_dirty = true;
}

// The TEV colour registers and konstants as the BP writes that would set them
// (register, then value with its type bit): one BP address holds either, so a
// snapshot of BP memory keeps only the one written last. A frame capture
// starts with these (gpu.cpp), the snapshot's own kind last.
void tev_register_writes(const Registers& r, uint32_t out[16]) {
    unsigned n = 0;
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned half = 0; half < 2; ++half) {
            const uint32_t reg = 0xE0 + 2 * i + half;
            const bool snapshot_konst = (r.bp[reg] >> 23) & 1;
            for (int pass = 0; pass < 2; ++pass) {
                const bool konst = pass == 0 ? !snapshot_konst : snapshot_konst;
                const int32_t* v = konst ? g_konst[i] : g_tev_regs[i];
                const uint32_t lo = uint32_t(half == 0 ? v[0] : v[2]) & 0x7FFu, hi = uint32_t(half == 0 ? v[3] : v[1]) & 0x7FFu;
                out[n++] = (reg << 24) | (konst ? 0x800000u : 0u) | (hi << 12) | lo;
            }
        }
}

// RGB8 (0) and RGBA6 (1) are two readings of one pixel's 24 bits, and the renderer turns
// its picture from one into the other. RGB565 (2, with 16-bit depth, the antialiased
// modes' format) lays a pixel out differently again, in a way with no source to check
// against: the picture is left as it is there, and said once.
void record_pixel_format(uint32_t from, uint32_t to) {
    if (from > 1 || to > 1) {
        if ((from == 2 || to == 2) && from <= 2 && to <= 2) {
            static bool said = false;
            if (!said) rt::log("gx: the EFB's pixel format changes to or from RGB565 (%u to %u): what was drawn before is kept as it was (the console would reinterpret its bits)", from, to);
            said = true;
        }
        return;
    }
    g_frame->packets.push_back({Packet::kConvert, from == 0 ? 0u : 1u});
}

void record_tlut_load(const Registers& r, uint32_t command) {
    const uint32_t source = bp_address(r.bp[0x64]);
    const uint32_t tmem = (command & 0x3FF) << 9, bytes = ((command >> 10) & 0x7FF) * 32;
    load_tlut(source, tmem, bytes);
    g_join.valid = false;  // the same registers, another palette
}



// ---- MKDD_SURVEY=<file>: where a frame's 3D drawing falls across the picture, and where its
// cameras are. For checking that what the game culls on the CPU follows a widened picture: with
// the cameras widened, the middle 1 / widening of the width is what a 4:3 picture showed, and
// things culled against that would leave the bands beside it thin. Every 600 frames: of the
// perspective draws' vertices (one in four) on screen, how many fall in each eighth of the half
// width from the middle out; and of the draws, how many lie wholly within each eighth's outer
// edge (by their outermost vertex) and wholly beyond each eighth's inner edge (by their
// innermost). And every 20th frame, each view's eye and the direction it looks.
namespace {
struct Survey {
    FILE* out = nullptr;
    uint64_t frames = 0, vertices[8] = {}, within[8] = {}, beyond[8] = {}, draws = 0;
};
Survey* const g_survey = [] () -> Survey* {
    const char* path = std::getenv("MKDD_SURVEY");
    FILE* f = path && *path ? std::fopen(path, "w") : nullptr;
    if (!f) return nullptr;
    auto* s = new Survey;
    s->out = f;
    return s;
}();

void survey(const Frame& f, uint64_t frame_number) {
    Survey& s = *g_survey;
    for (const Draw& d : f.draws) {
        const State st = f.states[d.state];  // (a copy: the frame keeps them as blocks)
        if (st.projection[3][2] != -1.0f || d.pipeline.topology != 0) continue;
        float lo = 2.0f, hi = -1.0f;
        const auto vertex = [&](uint32_t i) {
            const float* p;
            uint32_t row;
            if (d.pipeline.small_vertices == kLayoutTiny) p = f.tiny[i].pos, row = f.tiny[i].matrices[0] & 0xFFu;
            else if (d.pipeline.small_vertices) p = f.small[i].pos, row = f.small[i].matrices[0] & 0xFFu;
            else p = f.vertices[i].pos, row = f.vertices[i].matrices[0] & 0xFFu;
            if (row > 61) return;
            float v[3];
            for (int k = 0; k < 3; ++k) v[k] = st.posmtx[row + k][0] * p[0] + st.posmtx[row + k][1] * p[1] + st.posmtx[row + k][2] * p[2] + st.posmtx[row + k][3];
            const float w = -v[2];
            if (!(w > 0)) return;
            const float x = std::fabs((st.projection[0][0] * v[0] + st.projection[0][2] * v[2]) / w);
            const float y = std::fabs((st.projection[1][1] * v[1] + st.projection[1][2] * v[2]) / w);
            lo = std::min(lo, x), hi = std::max(hi, x);
            if (x < 1.0f && y < 1.0f) ++s.vertices[unsigned(x * 8.0f)];
        };
        if (d.direct) {
            for (uint32_t i = 0; i < d.index_count; i += 4) vertex(d.first_index + i);
        } else {
            for (uint32_t i = 0; i < d.index_count; i += 4) vertex(f.indices[d.first_index + i]);
        }
        if (hi < 0) continue;
        ++s.draws;
        for (unsigned b = 0; b < 8; ++b) {
            if (hi < float(b + 1) / 8.0f) ++s.within[b];
            if (lo >= float(b) / 8.0f && lo < 1.0f) ++s.beyond[b];
        }
    }
    if (frame_number % 20 == 0)
        for (size_t v = 0; v < f.views.size(); ++v) {
            const float* m = f.views[v].m;
            const float eye[3] = {-(m[0] * m[3] + m[4] * m[7] + m[8] * m[11]), -(m[1] * m[3] + m[5] * m[7] + m[9] * m[11]),
                                  -(m[2] * m[3] + m[6] * m[7] + m[10] * m[11])};
            std::fprintf(s.out, "view %llu %u of %u: eye %.0f %.0f %.0f looks %.2f %.2f %.2f\n", static_cast<unsigned long long>(frame_number),
                         unsigned(v), unsigned(f.views.size()), double(eye[0]), double(eye[1]), double(eye[2]), double(-m[8]), double(-m[9]), double(-m[10]));
        }
    if (++s.frames < 600) return;
    std::fprintf(s.out, "survey to frame %llu: %llu draws;", static_cast<unsigned long long>(frame_number), static_cast<unsigned long long>(s.draws));
    const auto row = [&](const char* name, const uint64_t* h) {
        std::fprintf(s.out, " %s", name);
        for (unsigned i = 0; i < 8; ++i) std::fprintf(s.out, " %llu", static_cast<unsigned long long>(h[i]));
        std::fprintf(s.out, ";");
    };
    row("vertices by eighth", s.vertices), row("draws wholly within", s.within), row("draws wholly beyond", s.beyond);
    std::fprintf(s.out, "\n");
    std::fflush(s.out);
    FILE* keep = s.out;
    s = Survey{};
    s.out = keep;
}
}  // namespace

// ---- the frame's end: at its copy to the frame buffer, or early (request_frame_end)
static float g_drawn_widening = 1.0f;
static void end_frame(Frame& f) {
    f.captured = g_capture_next;
    g_capture_next = false;
    f.serial = g_stats.frames.load(std::memory_order_relaxed) + 1;
    if (g_survey) [[unlikely]] survey(f, g_stats.frames.load(std::memory_order_relaxed));
    textures_end_frame(f);
    // A confirming frame contains only the previous picture's real copy.
    // It must keep that picture's shape and must not advance camera settings again.
    if (f.confirming) f.widening = g_drawn_widening;
    else {
        hud::place(f);
        g_drawn_widening = f.widening;
    }
    g_stats.frames.fetch_add(1, std::memory_order_relaxed);
    static uint64_t passes = 0;  // (MKDD_LATENCY numbers the passes' frames: a confirming copy isn't one)
    const bool counted = !f.confirming;
    submit(std::move(g_frame));
    if (counted) latency::frame_handed(++passes);
    g_frame = spare_frame();
    g_dirty = true;
    g_join.valid = false;
    std::fill(std::begin(g_slot_load), std::end(g_slot_load), kNoLoad);  // loads don't outlive their frame
    g_view = kNoView;
}

// The early frame end (docs/enhancements.md, "Latency"): the game's pass draws, ends its
// drawing in JFWDisplay::endRender (a draw done, BP 0x45), calculates, waits for the
// retrace and only then copies the picture to the frame buffer, in the next pass's
// beginRender. The frame is ended at that draw done instead, with the copy the next pass
// will make foretold: the last one's, to the frame buffer after it in the game's rotation.
// - Kept (set_copy_kept: the game's own frame rate): the foretold copy doesn't clear the
//   EFB, and the real one, when it comes, ends a frame of its own (Frame::confirming), so
//   the picture shown is always the real copy's; the main loop waits for it at the
//   retrace. A wrong foretelling then costs nothing: the confirming copy is the right one.
//   (The copy filter is foretold from the render mode beginRender will use, read in
//   endRender; still wrong where AppMgr::calc, after endRender, switches the render mode,
//   System::changeMovieRenderMode and changeNormalRenderMode at a movie's start and end,
//   or the EFB's format: the log names what differed.)
// - Not kept (pictures between frames, which start from a frame that cleared the EFB):
//   the foretold copy clears, and the real one is left out; a wrong one is counted, the
//   picture it made stays (the EFB is cleared), and after three in a row the early end stops.
namespace {
std::atomic<bool> g_keep_real{false};
bool g_foretold_kept = false;
const bool g_early_on = [] {
    const char* e = std::getenv("MKDD_EARLY_END");
    return !(e && *e == '0') && !std::getenv("MKDD_DFF");  // (a capture ends its frame at the copy)
}();
std::atomic<uint64_t> g_end_requests{0};
std::atomic<int> g_next_filter{-1};  // (request_frame_end's: read by draw_done after the request's acquire)
uint64_t g_end_served = 0;
Copy g_last_xfb{};
uint32_t g_xfb_dests[4] = {};  // the last copies' frame buffers, newest first
unsigned g_xfb_seen = 0, g_wrong_in_a_row = 0;
bool g_foretold = false, g_early_stopped = false;
Copy g_foretold_copy{};
uint64_t g_early_ends = 0, g_wrong = 0, g_drawn_after = 0;

uint32_t g_last_xfb_command = 0;
uint64_t g_foretold_serial = 0;  // the frame the last foretold copy ended (Frame::serial)
std::mutex g_real_lock;
RealCopy g_real;

// What differs between two copies, named (for the log): "" if nothing.
std::string copy_difference(const Copy& real, const Copy& told) {
    std::string s;
    char b[96];
    const auto add = [&](const char* name, auto x, auto y) {
        if (x == y) return;
        std::snprintf(b, sizeof b, "%s%s %lld, foretold %lld", s.empty() ? "" : "; ", name, static_cast<long long>(x), static_cast<long long>(y));
        s += b;
    };
    add("dest", real.dest, told.dest);
    for (int i = 0; i < 4; ++i) add("src", real.src[i], told.src[i]);
    add("width", real.dest_width, told.dest_width), add("height", real.dest_height, told.dest_height);
    add("half scale", real.half_scale, told.half_scale), add("from depth", real.from_depth, told.from_depth);
    add("EFB alpha", real.efb_alpha, told.efb_alpha), add("clear", real.clear, told.clear);
    add("clear colour", real.clear_color, told.clear_color), add("clear alpha", real.clear_alpha, told.clear_alpha);
    add("clear depth", real.clear_depth, told.clear_depth), add("clear z", real.clear_z, told.clear_z);
    for (int i = 0; i < 4; ++i) add("clear rgba", real.clear_rgba[i], told.clear_rgba[i]);
    add("gamma x10", std::lround(real.gamma * 10), std::lround(told.gamma * 10));
    for (int i = 0; i < 3; ++i) add("filter", real.filter[i], told.filter[i]);
    add("clamp top", real.clamp_top, told.clamp_top), add("clamp bottom", real.clamp_bottom, told.clamp_bottom);
    return s;
}

bool same_copy(const Copy& a, const Copy& b) {
    return std::memcmp(a.src, b.src, sizeof a.src) == 0 && a.dest == b.dest && a.dest_width == b.dest_width &&
           a.dest_height == b.dest_height && a.half_scale == b.half_scale && a.from_depth == b.from_depth && a.efb_alpha == b.efb_alpha &&
           a.clear == b.clear && a.clear_color == b.clear_color && a.clear_alpha == b.clear_alpha && a.clear_depth == b.clear_depth &&
           std::memcmp(a.clear_rgba, b.clear_rgba, 4) == 0 && a.clear_z == b.clear_z && a.gamma == b.gamma &&
           std::memcmp(a.filter, b.filter, 3) == 0 && a.clamp_top == b.clamp_top && a.clamp_bottom == b.clamp_bottom;
}
// The frame buffer the next copy goes to: the game's rotation of them (one, two or three).
bool next_dest(uint32_t& dest) {
    if (g_xfb_seen < 4) return false;
    for (unsigned k = 1; k <= 3; ++k)
        if (g_xfb_dests[0] == g_xfb_dests[k]) {  // a cycle of k: the next is the one k-1 before the newest
            dest = g_xfb_dests[k - 1];
            return true;
        }
    return false;
}
}  // namespace

void set_copy_kept(bool on) { g_keep_real.store(on, std::memory_order_relaxed); }
RealCopy real_copy() {
    std::lock_guard l(g_real_lock);
    return g_real;
}
bool early_frame_end() { return g_early_on; }
void request_frame_end(int vfilter) {
    if (!g_early_on) return;
    g_next_filter.store(vfilter, std::memory_order_relaxed);
    g_end_requests.fetch_add(1, std::memory_order_release);
}

static void log_copy(const char* what, const Copy& c) {
    static const bool on = std::getenv("MKDD_EARLY_LOG") != nullptr;
    if (on)
        qilin::rt::log("early: %s %08X src %d,%d %dx%d dest %ux%u clear %d%d%d%d rgba %02X%02X%02X%02X z %06X gamma %.1f filter %u,%u,%u clamp %d%d half %d depth %d alpha %d (frames %llu)",
                       what, c.dest, c.src[0], c.src[1], c.src[2], c.src[3], c.dest_width, c.dest_height, c.clear, c.clear_color, c.clear_alpha,
                       c.clear_depth, c.clear_rgba[0], c.clear_rgba[1], c.clear_rgba[2], c.clear_rgba[3], c.clear_z, c.gamma, c.filter[0],
                       c.filter[1], c.filter[2], c.clamp_top, c.clamp_bottom, c.half_scale, c.from_depth, c.efb_alpha,
                       static_cast<unsigned long long>(g_stats.frames.load()));
}
static void note_xfb_copy(const Copy& c) {
    log_copy("copy", c);
    g_last_xfb = c;
    std::memmove(g_xfb_dests + 1, g_xfb_dests, 3 * sizeof g_xfb_dests[0]);
    g_xfb_dests[0] = c.dest;
    ++g_xfb_seen;
}

static Copy copy_state(const uint32_t* bp, uint32_t command);
// At a draw done: the frame ends here if the game's thread asked (it was in endRender).
static void draw_done() {
    if (g_end_requests.load(std::memory_order_acquire) == g_end_served) return;
    g_end_served = g_end_requests.load(std::memory_order_relaxed);
    g_foretold = false;  // (one foretold and never made: the game cleared instead of copying, a picture dropped; the rotation stays)
    uint32_t dest;
    if (g_early_stopped || !next_dest(dest)) return;
    Frame& f = *g_frame;
    // The last copy's, with the copy filter the next beginRender will set (its render
    // mode's, read in endRender: a movie's start or end and the menus change it), else the
    // one set since. The rest is the last one's: the source, destination, format and clear
    // registers are written again by copies to textures in mid-frame, and GXCopyDisp
    // writes its own before it copies.
    Copy c = g_last_xfb;
    if (const int v = g_next_filter.load(std::memory_order_relaxed); v >= 0)
        c.filter[0] = uint8_t(v), c.filter[1] = uint8_t(v >> 8), c.filter[2] = uint8_t(v >> 16);
    else
        std::memcpy(c.filter, copy_state(g_bp_now, g_last_xfb_command).filter, sizeof c.filter);
    c.dest = dest;
    g_foretold_copy = c, g_foretold = true;
    g_foretold_kept = g_keep_real.load(std::memory_order_relaxed);
    if (g_foretold_kept) c.clear = false;  // (the real copy clears, a frame later)
    f.packets.push_back({Packet::kCopy, uint32_t(f.copies.size())});
    f.copies.push_back(c);
    f.foretold = true;
    g_foretold_serial = g_stats.frames.load(std::memory_order_relaxed) + 1;  // (end_frame gives the frame this serial)
    log_copy("foretold", c);
    if (++g_early_ends == 1 || g_early_ends % 3600 == 0)
        qilin::rt::log("render: early frame ends %llu, foretold wrong %llu, drawing between %llu", static_cast<unsigned long long>(g_early_ends),
                       static_cast<unsigned long long>(g_wrong), static_cast<unsigned long long>(g_drawn_after));
    end_frame(f);
}

// A copy to the frame buffer: true if the early end made it already (it isn't made again).
static bool early_copy_made(const Copy& c, Frame& f) {
    if (!g_foretold) return false;
    log_copy("came", c);
    g_foretold = false;
    bool drawn = false;
    for (const Packet& p : f.packets) drawn = drawn || p.kind != Packet::kCopy;
    if (drawn) {  // (a pass that drew after the frame's end: its copy is its own picture, made as ever)
        ++g_drawn_after;
        return false;
    }
    const bool right = same_copy(c, g_foretold_copy);
    if (!right && ++g_wrong <= 5)
        qilin::rt::log("render: the early frame end foretold the wrong copy to %08X (%s)%s", c.dest, copy_difference(c, g_foretold_copy).c_str(),
                       g_foretold_kept ? ": the real one is shown" : "");
    if (g_foretold_kept) {  // made, ending a frame of its own: the picture shown
        f.confirming = true;
        return false;
    }
    note_xfb_copy(c);
    if (right) {
        g_wrong_in_a_row = 0;
        return true;
    }
    {  // (the renderer shows that frame with it from now on: real_copy)
        std::lock_guard l(g_real_lock);
        g_real = {g_foretold_serial, c};
    }
    if (++g_wrong_in_a_row == 3) {
        g_early_stopped = true;
        qilin::rt::log("render: the early frame end stops: three wrong copies in a row");
    }
    return true;  // (the EFB was cleared by the one made: this one can't be made again)
}

// A copy's state from the BP registers and its command (all but its texture, which
// record_copy_timed looks up): for a real copy, and for a foretold one (draw_done).
static Copy copy_state(const uint32_t* bp, uint32_t command) {
    Copy c{};
    const uint32_t src = bp[0x49], size = bp[0x4A];
    c.src[0] = int32_t(src & 0x3FF);
    c.src[1] = int32_t((src >> 10) & 0x3FF);
    c.src[2] = int32_t((size & 0x3FF) + 1);
    c.src[3] = int32_t(((size >> 10) & 0x3FF) + 1);
    c.dest = bp_address(bp[0x4B]);
    c.to_xfb = (command >> 14) & 1;
    c.half_scale = (command >> 9) & 1;
    c.clear = (command >> 11) & 1;
    c.from_depth = (bp[0x43] & 7) == 3;  // Z24: the EFB copy reads depth
    c.efb_alpha = (bp[0x43] & 7) == 1;   // RGBA6_Z24: the only format that keeps alpha
    static constexpr float kGamma[4] = {1.0f, 1.7f, 2.2f, 2.2f};
    c.gamma = kGamma[(command >> 7) & 3];
    // The copy filter, which every EFB copy (to a texture or the frame buffer)
    // applies: GX's vertical filter, used for deflicker and to blend
    // alternate-line patterns. Weights w0-w3 in BP 0x53, w4-w6 in BP 0x54.
    {
        const uint64_t w = uint64_t(bp[0x53] & 0xFFFFFF) | (uint64_t(bp[0x54] & 0xFFFFFF) << 24);
        const auto tap = [&](unsigned i) { return unsigned((w >> (6 * i)) & 63); };
        c.filter[0] = uint8_t(tap(0) + tap(1));
        c.filter[1] = uint8_t(tap(2) + tap(3) + tap(4));
        c.filter[2] = uint8_t(tap(5) + tap(6));
    }
    // Not a copy of depth: its bytes are one number, not three channels to
    // blend with the lines around (Dolphin leaves the filter out of them too).
    // This game keeps its deflicker weights set all frame and copies depth
    // in mid-frame (the heat shimmer, the Z the CPU reads back).
    if (c.from_depth && !c.to_xfb) c.filter[0] = 0, c.filter[1] = 64, c.filter[2] = 0;
    c.clamp_top = command & 1;
    c.clamp_bottom = (command >> 1) & 1;
    const uint32_t blend = bp[0x41];
    const bool alpha_target = (bp[0x43] & 7) == 1;
    c.clear_color = (blend >> 3) & 1;
    c.clear_alpha = ((blend >> 4) & 1) && alpha_target;
    c.clear_depth = (bp[0x40] >> 4) & 1;
    const uint32_t ar = bp[0x4F], gb = bp[0x50];
    c.clear_rgba[0] = uint8_t(ar & 255), c.clear_rgba[3] = uint8_t((ar >> 8) & 255);
    c.clear_rgba[1] = uint8_t((gb >> 8) & 255), c.clear_rgba[2] = uint8_t(gb & 255);
    if (!alpha_target) c.clear_rgba[3] = 255;
    c.clear_z = bp[0x51] & 0xFFFFFF;
    if (c.to_xfb) {
        // The frame buffer's height after vertical scaling (BP 0x4E: 256 / scale).
        const uint32_t yscale = bp[0x4E] & 0x1FF;
        const float scale = ((command >> 10) & 1) && yscale ? 256.0f / float(yscale) : 1.0f;
        c.dest_width = uint32_t(c.src[2]);
        c.dest_height = uint32_t(std::lround(float(c.src[3]) * scale));
        c.texture = -1;
    } else {
        c.format = ((command >> 3) & 15) / 2 + (((command >> 3) & 1) * 8) + (((command >> 15) & 1) ? 0x100 : 0);  // + intensity flag
        c.dest_width = uint32_t(c.half_scale ? c.src[2] / 2 : c.src[2]);
        c.dest_height = uint32_t(c.half_scale ? c.src[3] / 2 : c.src[3]);
    }
    return c;
}

static void record_copy_timed(const Registers& r, uint32_t command) {
    Frame& f = *g_frame;
    Copy c = copy_state(r.bp, command);
    if (c.to_xfb) {
        g_last_xfb_command = command;
        std::memcpy(g_bp_now, r.bp, sizeof g_bp_now);  // (the state the next one is foretold from starts here)
    } else {
        c.texture = texture_from_copy(c.dest, c.dest_width, c.dest_height, f);
        // A copy of one tile or less in the 32-bit layout (RGBA8, Z24X8) also
        // goes to guest memory, for the CPU: the game copies 2x2 pixels of
        // depth and reads them to see whether a player's mark or the sun is
        // hidden (ZCaptureNode, doldecomp's name). Larger copies stay on the
        // GPU: this game reads none of them (docs/graphics.md).
        c.to_memory = c.dest_width <= 4 && c.dest_height <= 4 && (c.format & 0xFF) == 6 && in_memory(c.dest & 0x03FFFFFFu, 64);
    }
    if (c.to_xfb && early_copy_made(c, f)) return;
    f.packets.push_back({Packet::kCopy, uint32_t(f.copies.size())});
    f.copies.push_back(c);
    if (c.to_xfb) {
        note_xfb_copy(c);
        end_frame(f);
    }
}

RecorderStats& recorder_stats() { return g_stats; }
uint64_t now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t shape_id(const uint32_t* bp) {
    // The draw before's, mostly (a material's draws share one): compared before the table is searched.
    static ShapeWords last{};
    static uint32_t last_id = kNoLoad;
    const ShapeWords w = shape_of(bp);
    if (last_id != kNoLoad && w == last) return last_id;
    const auto [it, added] = g_shape_ids.try_emplace(w, uint32_t(g_shape_ids.size()));
    if (added) {
        std::lock_guard lock(g_shapes_lock);
        g_shapes.push_back(w);
    }
    last = w, last_id = it->second;
    return it->second;
}
uint32_t intern_shape(const ShapeWords& w) {
    const auto [it, added] = g_shape_ids.try_emplace(w, uint32_t(g_shape_ids.size()));
    if (added) {
        std::lock_guard lock(g_shapes_lock);
        g_shapes.push_back(w);
    }
    return it->second;
}
ShapeWords shape_words(uint32_t id) {
    std::lock_guard lock(g_shapes_lock);
    if (id >= g_shapes.size()) rt::fatal(nullptr, "the renderer asked for shape %u, which the recorder never made", id);
    return g_shapes[id];
}

}  // namespace mkdd::gx
