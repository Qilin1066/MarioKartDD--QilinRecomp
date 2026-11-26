// The game's two position-matrix loaders, natively: the same bytes to the
// write-gather pipe as the originals, and for the recorder who is loading and
// under which view matrix, so a matrix can be found again in the next frame
// and pictures drawn between the two (interp.hpp; docs/enhancements.md has
// why these two functions are every door but the indexed loads).
//
// Both are leaves, read from the retail code (GM4E01 rev 0):
//
//   J3DFifoLoadPosMtxImm(Mtx m, u32 id)   80067A64, 0x84 bytes, 10 callers
//     stb 0x10; sth 11; sth (id * 4) & 0xFFFF; then twelve lwz/stw of m.
//     J3D passes id already times three, so id * 4 is the XF address.
//   GXLoadPosMtxImm(Mtx m, u32 id)        800C20D8, 0x50 bytes, 65 callers
//     stb 0x10; stw (id * 4) | 0x000B0000; then six psq_l/psq_st pairs of m
//     through GQR0 (floats as they are).
//
// What the originals leave in r0 and r4-r6 (and GX's in f0-f5) is not
// reproduced: they are volatile and no caller reads them after the call. The
// paired-single copy would quieten a signalling NaN where this copies the
// bits; a matrix holds none. tools/check/mtxtag_check.py compares the bytes
// both ways when a DOL is given.
//
// Not active until the plan (data/GM4E01/plan.tsv) marks the two ranges
// replaced and main.cpp gives replacements() to rt::provide: until then the
// lifted originals run, nothing is tagged, and pictures between frames are
// not offered (interp::supported()).
#include <atomic>
#include <cstdint>
#include <span>

#include "gpu.hpp"
#include "interp.hpp"
#include "qilin/rt/system.hpp"

namespace mkdd::mtxtag {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

constexpr uint32_t kViewMatrix = 0x803ACF90u;   // j3dSys.mViewMtx: the camera being drawn (48 bytes)
constexpr uint32_t kShapePacket = 0x803ACFD0u;  // j3dSys + 0x40: the J3DShapePacket being drawn (J3DShapePacket::draw, 80050608)

std::atomic<bool> g_installed{false};

void J3DFifoLoadPosMtxImm(Cpu& c) {
    // The packet's address is a multiple of four: bits 0 and 1 mark the other kinds of tag.
    load_position_matrix(c, c.r[3], c.r[4] << 2, rt::peek32(kShapePacket) & ~3u, kViewMatrix);
}
// Call sites of GXLoadPosMtxImm that draw many things of a kind, one call each: which thing it
// is stands in a register the caller keeps across the call (read from each function's code: the
// register is set once, from an argument, and not written again before the call). With it a
// particle or a course object pairs with itself in the frame before; without it only the n-th
// load of the site with the n-th, which fails for all of them when one appears or goes (in the
// demo races: nine particles in ten, and every simple model on one frame in ten).
struct Site {
    uint32_t lr;      // the return address
    uint8_t reg;      // the register that tells
    bool pointer;     // its value is the address of the thing's address (a list iterator)
};
constexpr Site kSites[] = {
    {0x8007D3A0u, 31, false},  // JPADrawBillboard(work, particle): the particle, r4 kept in r31
    {0x8007D4C0u, 31, false},  // JPADrawRotBillboard: r31
    {0x8007DDA8u, 29, false},  // JPADrawDirection: r29
    {0x8007E1B4u, 30, false},  // JPADrawRotDirection: r30
    {0x8007E408u, 30, false},  // JPADrawDBillboard: r30
    {0x80200140u, 31, true},   // stParticleDraw::drawParentBillBoard(iterator&): the list link the iterator is at
    {0x80200870u, 31, true},   // stParticleDraw::drawParentDirectional
    {0x80183758u, 27, false},  // ExModel::simpleDraw: the model (this), kept in r27
};

void GXLoadPosMtxImm(Cpu& c) {
    // Who called: one call site is one kind of thing drawn (a particle system's
    // draw, a 2D pane, the course). Bit 1 says so.
    uint32_t id = 0;
    for (const Site& s : kSites)
        if (s.lr == c.lr) id = s.pointer ? rt::peek32(c.r[s.reg]) : c.r[s.reg];
    load_position_matrix(c, c.r[3], c.r[4] << 2, (c.lr & ~3u) | 2u, kViewMatrix, id);
}

// Race2D's marks that follow a kart: the lifted function runs as it is (the plan's `wrap`),
// and the matrices loaded while it does carry the context (its J2D panes' loads: the caller a
// tag holds is J2D's own, the same for every picture on the screen).
template <uint32_t Address>
void follows_world_wrapper(Cpu& c) {
    const uint8_t before = set_matrix_context(gx::kContextFollowsWorld);
    rt::wrapped(Address)(c);
    set_matrix_context(before);
}
const rt::Provided kWrappers[] = {
    {0x8014573Cu, "Race2D::drawPlayerMark", follows_world_wrapper<0x8014573Cu>},
    {0x80146CF0u, "Race2D::drawAlarm", follows_world_wrapper<0x80146CF0u>},
    {0x80147B1Cu, "Race2D::drawBomb", follows_world_wrapper<0x80147B1Cu>},
};

const rt::Provided kReplacements[] = {
    {0x80067A64u, "J3DFifoLoadPosMtxImm", J3DFifoLoadPosMtxImm},
    {0x800C20D8u, "GXLoadPosMtxImm", GXLoadPosMtxImm},
};

}  // namespace

std::span<const rt::Provided> replacements() {
    g_installed.store(true, std::memory_order_relaxed);
    return kReplacements;
}
std::span<const rt::Provided> wrappers() { return kWrappers; }
bool installed() { return g_installed.load(std::memory_order_relaxed); }

// The lens flare (TFlareLine::draw, 8026F5B0, 0x350 bytes) loads its own
// matrices: its two calls to GXLoadPosMtxImm return to 8026F668 and 8026F794.
// The marks over karts, the alarm and the bomb marks (Race2D::drawPlayerMark,
// drawAlarm, drawBomb) can't be told this way: they draw through J2DPicture,
// so the caller the tag holds is J2D's own, the same for every picture. Their
// wrappers above give their loads a context instead.
bool follows_world(const gx::Frame&, const gx::Draw& d) {
    if (gx::draw_context(d) == gx::kContextFollowsWorld) return true;
    const uint32_t tag = gx::draw_tag(d);
    return (tag & 3u) == 2u && tag - 0x8026F5B0u < 0x350u;
}

}  // namespace mkdd::mtxtag
