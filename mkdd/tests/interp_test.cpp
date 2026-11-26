// Interpolation and widened HUD integration without a window, GPU or game data.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "hud.hpp"
#include "interp.hpp"

// Only the Blender is exercised: no game matrix-loader replacements are installed.
namespace mkdd::mtxtag { bool installed() { return false; } }

namespace {
namespace gx = mkdd::gx;
namespace interp = mkdd::interp;
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
void identity(float* m) {
    std::fill(m, m + 12, 0.0f);
    m[0] = m[5] = m[10] = 1.0f;
}
gx::StateInfo info(uint16_t view, uint32_t load = gx::kNoLoad) {
    gx::StateInfo i{};
    std::fill(std::begin(i.load), std::end(i.load), gx::kNoLoad);
    i.view = view;
    i.load[0] = load;
    return i;
}
void add_view(gx::Frame& f) {
    gx::View v{};
    identity(v.m);
    v.has_projection = true;
    v.projection[0] = v.projection[2] = 2.0f;
    v.projection[4] = -1.0f / 399.0f;
    v.projection[5] = -400.0f / 399.0f;
    f.views.push_back(v);
}
void add_model(gx::Frame& f, uint16_t view, float x) {
    gx::Load l{};
    l.tag = 0x80001000u;
    l.view = view;
    identity(l.mv);
    l.mv[3] = x;
    gx::State s{};
    std::memcpy(s.posmtx, l.mv, sizeof l.mv);
    s.nrmmtx[0][0] = s.nrmmtx[1][1] = s.nrmmtx[2][2] = 1.0f;
    const auto& p = f.views[view].projection;
    s.projection[0][0] = p[0], s.projection[1][1] = p[2];
    s.projection[2][2] = p[4], s.projection[2][3] = p[5];
    s.projection[3][2] = -1.0f;
    f.state_info.push_back(info(view, uint32_t(f.loads.size())));
    f.loads.push_back(l);
    f.states.push(s);
}
void add_hud(gx::Frame& f) {
    gx::State s{};
    s.posmtx[0][0] = s.posmtx[1][1] = s.posmtx[2][2] = 1;
    s.projection[0][0] = 2.0f / 640;
    s.projection[0][3] = -1;
    s.projection[1][1] = 2.0f / 480;
    s.projection[1][3] = -1;
    s.projection[3][3] = 1;
    s.pixel_center[0] = s.pixel_center[1] = 1;
    gx::Draw d{};
    d.state = uint32_t(f.states.size());
    d.direct = 1;
    d.index_count = 3;
    d.viewport[2] = 640, d.viewport[3] = 480;
    d.scissor[2] = 640, d.scissor[3] = 480;
    f.states.push(s);
    f.state_info.push_back(info(gx::kNoView));
    f.vertices.resize(3);
    const float xy[3][2] = {{20, 20}, {100, 20}, {20, 60}};
    for (unsigned i = 0; i < 3; ++i) {
        f.vertices[i] = {};
        f.vertices[i].pos[0] = xy[i][0], f.vertices[i].pos[1] = xy[i][1];
    }
    f.draws.push_back(d);
    f.packets.push_back({gx::Packet::kDraw, 0});
}
float widening = 1;
void widened_interpolation() {
    mkdd::hud::set_widening_source([] { return widening; });
    for (float w : {4.0f / 3, 16.0f / 9, 8.0f / 3})
        for (auto place : {mkdd::hud::Place::edges, mkdd::hud::Place::middle}) {
            widening = w;
            mkdd::hud::set_place(place);
            gx::Frame prev, cur;
            add_view(prev), add_view(cur);
            add_model(prev, 0, 0), add_model(cur, 0, 0.5f);
            add_hud(prev), add_hud(cur);
            mkdd::hud::place(prev), mkdd::hud::place(cur);
            check(cur.draws[0].state >= 2, "widening created a separate HUD state");
            check(cur.state_info.size() == cur.states.size(), "HUD states retain interpolation metadata");
            interp::Blender b;
            const bool paired = b.pair(prev, cur);
            check(paired && b.counts().paired == 1, "widescreen and interpolation work together");
            if (!paired) continue;
            std::vector<gx::State> out(cur.states.size());
            b.states(cur, 0.5f, out.data());
            check(std::fabs(out[0].posmtx[0][3] - 0.25f) < 1e-5f, "model moves halfway in a widened frame");
            const auto hud_state = cur.states[cur.draws[0].state];
            check(std::memcmp(&hud_state, &out[cur.draws[0].state], sizeof hud_state) == 0, "interpolation leaves the placed HUD unchanged");
        }
    widening = 1;
    mkdd::hud::set_widening_source(nullptr);
}
void independent_views() {
    // One of player one's two identically tagged objects disappears. Player two's
    // object must still pair, regardless of the other view's visibility.
    for (uint16_t players : {uint16_t(2), uint16_t(4)}) {
        gx::Frame prev, cur;
        for (uint16_t v = 0; v < players; ++v) add_view(prev), add_view(cur);
        for (uint16_t v = 0; v < players; ++v) {
            add_model(prev, v, float(v));
            if (v == 0) add_model(prev, v, 0.8f);
            add_model(cur, v, float(v) + 0.2f);
        }
        interp::Blender b;
        check(b.pair(prev, cur), "split-screen frames can blend");
        check(b.counts().paired == players - 1u && b.counts().alone == 1, "visibility changes affect only their own player");
        std::vector<gx::State> out(cur.states.size());
        b.states(cur, 0.5f, out.data());
        for (uint16_t v = 1; v < players; ++v)
            check(std::fabs(out[v].posmtx[0][3] - (float(v) + 0.1f)) < 1e-5f, "other players' objects keep moving smoothly");
        b.states(cur, 1.0f, out.data());
        const auto own = cur.states[1];
        check(std::memcmp(&own, &out[1], sizeof own) == 0, "phase one remains the exact real frame");
    }
}
}  // namespace

int main() {
    widened_interpolation();
    independent_views();
    std::printf("interp_test: %d failures\n", failures);
    return failures ? 1 : 0;
}


