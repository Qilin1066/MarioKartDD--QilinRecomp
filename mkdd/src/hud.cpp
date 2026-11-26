// The HUD's place on a widened picture (docs/enhancements.md, "The flat
// squeeze"). The game's cameras are widened and its 608x448 frame is shown
// stretched; the 3D comes out right, and whatever is drawn flat (orthographic
// projection: the HUD, J2D) is too wide by the same factor. A flat draw is
// squeezed across by 1/widening about an anchor, by scaling the first row of
// its state's projection (x' = x / w + anchor (1 - 1/w) in clip space; w is 1
// in an orthographic projection), in a copy of the state that only it uses.
//
// What's left alone: draws that fill their view's width (fades, wipes, the
// screen effects' quads, letterbox bars, backgrounds: nine tenths or more),
// and draws a later texture copy of the frame reads back (an effect's work
// area, not the HUD).
//
// A view is the draw's viewport cut by its scissor box, so a player's HUD in
// a half or a quarter of the screen keeps to it. Draws on one line (most of
// their height shared, no more than 3% of the view's width apart) are one
// part and move together, so a line of text or an icon and its number
// aren't pulled apart.
#include "hud.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <numeric>

namespace mkdd::hud {
namespace {

std::atomic<Place> g_place{Place::edges};
float (*g_widening)() = nullptr;
bool (*g_follows)(const gx::Frame&, const gx::Draw&) = nullptr;

constexpr float kFills = 0.9f;     // of the view's width: not a part of the HUD
constexpr float kLineGap = 0.03f;  // of the view's width: pieces of one line

struct Flat {
    uint32_t draw;
    float left, right;     // the view, EFB pixels
    float x0, x1, y0, y1;  // the draw's extent, EFB pixels
    float sign;            // the vertex shader's flip of x (the viewport's width negative)
    bool follows;
};

// The extent of a draw's vertices as the vertex shader places them.
template <class V>
void extent(const gx::Frame& f, const gx::Draw& d, const gx::State& s, const V* v, float lo[2], float hi[2]) {
    const auto one = [&](const V& p) {
        const uint32_t row = p.matrices[0] & 63u;
        float view[4] = {0, 0, 0, 1};
        for (unsigned k = 0; k < 3; ++k) {
            const float* m = s.posmtx[std::min(row + k, 63u)];
            view[k] = m[0] * p.pos[0] + m[1] * p.pos[1] + m[2] * p.pos[2] + m[3];
        }
        for (unsigned a = 0; a < 2; ++a) {
            const float* r = s.projection[a];
            const float c = r[0] * view[0] + r[1] * view[1] + r[2] * view[2] + r[3];
            lo[a] = std::min(lo[a], c), hi[a] = std::max(hi[a], c);
        }
    };
    if (d.direct)
        for (uint32_t i = 0; i < d.index_count; ++i) one(v[d.first_index + i]);
    else
        for (uint32_t i = 0; i < d.index_count; ++i) one(v[f.indices[d.first_index + i]]);
}

}  // namespace

void set_place(Place p) { g_place.store(p, std::memory_order_relaxed); }
void set_widening_source(float (*source)()) { g_widening = source; }
void set_follows_world(bool (*source)(const gx::Frame&, const gx::Draw&)) { g_follows = source; }

bool anchor_of(Place p, float left, float right, float x0, float x1, bool follows_world, float& anchor) {
    const float width = right - left;
    if (p == Place::stretched || !(width > 0)) return false;
    if (std::min(x1, right) - std::max(x0, left) >= kFills * width) return false;
    const float centre = 0.5f * (x0 + x1);
    if (follows_world) anchor = centre;
    else if (p == Place::middle) anchor = 0.5f * (left + right);
    else anchor = centre < left + width / 3 ? left : centre > right - width / 3 ? right : centre;
    return true;
}

void place(gx::Frame& f) {
    const float w = g_widening ? g_widening() : 1.0f;  // (asked every frame: the source keeps its own time by it)
    const Place p = g_place.load(std::memory_order_relaxed);
    if (!(w > 1.001f) || p == Place::stretched) return;

    thread_local std::vector<Flat> flats;
    flats.clear();
    for (uint32_t pi = 0; pi < f.packets.size(); ++pi) {
        if (f.packets[pi].kind != gx::Packet::kDraw) continue;
        const gx::Draw& d = f.draws[f.packets[pi].index];
        const gx::State& s = f.states[d.state];
        if (s.projection[3][3] != 1.0f || d.index_count == 0) continue;  // perspective: the cameras' business
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        if (d.pipeline.small_vertices) extent(f, d, s, f.small.data(), lo, hi);
        else extent(f, d, s, f.vertices.data(), lo, hi);
        if (!std::isfinite(lo[0]) || !std::isfinite(hi[0])) continue;
        Flat fl{};
        fl.draw = f.packets[pi].index;
        fl.sign = s.pixel_center[0] < 0 ? -1.0f : 1.0f;
        const float vx = d.viewport[0], vw = d.viewport[2];
        const float a = vx + (fl.sign * lo[0] + 1) * 0.5f * vw, b = vx + (fl.sign * hi[0] + 1) * 0.5f * vw;
        fl.x0 = std::min(a, b), fl.x1 = std::max(a, b);
        const float flip = s.pixel_center[1] < 0 ? 1.0f : -1.0f;  // (the shader's, then Vulkan's y down)
        const float ya = d.viewport[1] + (1 - flip * lo[1]) * 0.5f * d.viewport[3], yb = d.viewport[1] + (1 - flip * hi[1]) * 0.5f * d.viewport[3];
        fl.y0 = std::min(ya, yb), fl.y1 = std::max(ya, yb);
        fl.left = std::max(vx, float(d.scissor[0]));
        fl.right = std::min(vx + vw, float(d.scissor[0] + d.scissor[2]));
        if (!(fl.right > fl.left)) continue;
        if (std::min(fl.x1, fl.right) - std::max(fl.x0, fl.left) >= kFills * (fl.right - fl.left)) continue;
        // Read back by a texture copy later in the frame: something's work area.
        bool copied = false;
        for (uint32_t pj = pi + 1; pj < f.packets.size() && !copied; ++pj) {
            if (f.packets[pj].kind != gx::Packet::kCopy) continue;
            const gx::Copy& c = f.copies[f.packets[pj].index];
            copied = !c.to_xfb && !c.from_depth && float(c.src[0]) < fl.x1 && float(c.src[0] + c.src[2]) > fl.x0 &&
                     float(c.src[1]) < fl.y1 && float(c.src[1] + c.src[3]) > fl.y0;
        }
        if (copied) continue;
        fl.follows = g_follows && g_follows(f, d);
        flats.push_back(fl);
    }
    if (flats.empty()) return;

    // Lines: a part is the draws that sit side by side in one view.
    thread_local std::vector<uint32_t> part;
    part.resize(flats.size());
    std::iota(part.begin(), part.end(), 0u);
    const auto root = [&](uint32_t i) {
        while (part[i] != i) i = part[i] = part[part[i]];
        return i;
    };
    for (uint32_t i = 0; i < flats.size(); ++i)
        for (uint32_t j = i + 1; j < flats.size(); ++j) {
            const Flat& a = flats[i];
            const Flat& b = flats[j];
            if (a.left != b.left || a.right != b.right || a.follows || b.follows) continue;
            const float shared = std::min(a.y1, b.y1) - std::max(a.y0, b.y0);
            if (shared < 0.5f * std::min(a.y1 - a.y0, b.y1 - b.y0) || !(shared > 0)) continue;
            if (std::max(a.x0, b.x0) - std::min(a.x1, b.x1) > kLineGap * (a.right - a.left)) continue;
            part[root(i)] = root(j);
        }
    thread_local std::vector<float> x0, x1;
    x0.assign(flats.size(), 1e30f), x1.assign(flats.size(), -1e30f);
    for (uint32_t i = 0; i < flats.size(); ++i) {
        const uint32_t r = root(i);
        x0[r] = std::min(x0[r], flats[i].x0), x1[r] = std::max(x1[r], flats[i].x1);
    }

    struct Made {
        uint32_t from, to;
        float anchor;
    };
    thread_local std::vector<Made> made;  // states already copied for an anchor, this frame
    made.clear();
    const float k = 1.0f / w;
    for (uint32_t i = 0; i < flats.size(); ++i) {
        const Flat& fl = flats[i];
        const uint32_t r = root(i);
        float at;
        if (!anchor_of(p, fl.left, fl.right, x0[r], x1[r], fl.follows, at)) continue;
        gx::Draw& d = f.draws[fl.draw];
        const float anchor = fl.sign * ((at - d.viewport[0]) / d.viewport[2] * 2 - 1);  // in clip space, before the flip
        const auto same = std::find_if(made.begin(), made.end(), [&](const Made& m) { return m.from == d.state && m.anchor == anchor; });
        if (same != made.end()) {
            d.state = same->to;
            continue;
        }
        const uint32_t to = uint32_t(f.states.size());
        f.states.push_back(gx::State(f.states[d.state]));
        float* row = f.states.back().projection[0];
        for (unsigned c = 0; c < 4; ++c) row[c] *= k;
        row[3] += anchor * (1 - k);
        made.push_back({d.state, to, anchor});
        d.state = to;
    }
}

}  // namespace mkdd::hud
