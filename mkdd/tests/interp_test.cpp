// Interpolation and widened HUD integration without a window, GPU or game data.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <thread>
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
void add_hud(gx::Frame& f, float x0 = 20, float y0 = 20, float x1 = 100, float y1 = 60,
             int top = 0, int height = 480) {
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
    d.first_index = uint32_t(f.vertices.size());
    d.viewport[2] = 640, d.viewport[3] = 480;
    d.scissor[1] = top, d.scissor[2] = 640, d.scissor[3] = height;
    f.states.push(s);
    f.state_info.push_back(info(gx::kNoView));
    f.vertices.resize(f.vertices.size() + 3);
    const float xy[3][2] = {{x0, y0}, {x1, y0}, {x0, y1}};
    for (unsigned i = 0; i < 3; ++i) {
        auto& v = f.vertices[d.first_index + i];
        v = {};
        v.pos[0] = xy[i][0], v.pos[1] = xy[i][1];
    }
    f.packets.push_back({gx::Packet::kDraw, uint32_t(f.draws.size())});
    f.draws.push_back(d);
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

void split_hud_boundary() {
    widening = 4.0f / 3;
    mkdd::hud::set_widening_source([] { return widening; });
    mkdd::hud::set_place(mkdd::hud::Place::edges);
    gx::Frame f;
    // Both sprites extend beyond their own player's scissor. Their unclipped
    // bounds overlap vertically and are close horizontally, but they are not
    // pieces of one player's line of text.
    add_hud(f, 100, 210, 200, 250, 0, 240);
    add_hud(f, 210, 220, 310, 260, 240, 240);
    mkdd::hud::place(f);
    const auto upper = f.states[f.draws[0].state], lower = f.states[f.draws[1].state];
    const float k = 1.0f / widening;
    check(std::fabs(upper.projection[0][3] + 1.0f) < 1e-5f, "upper player's HUD keeps its own left anchor");
    check(std::fabs(lower.projection[0][3] - (-k - 0.1875f * (1 - k))) < 1e-5f,
          "lower player's HUD keeps its own centre anchor");
    mkdd::hud::set_widening_source(nullptr);
}

void countdown_overlay() {
    widening = 4.0f / 3;
    mkdd::hud::set_widening_source([] { return widening; });
    mkdd::hud::set_place(mkdd::hud::Place::edges);
    for (float end : {430.0f, 470.0f, 560.0f}) {
        gx::Frame f;
        // The track and a character icon share the right anchor. A large
        // countdown animation overlaps both, but belongs to the centre.
        add_hud(f, 460, 160, 620, 360);
        add_hud(f, 500, 220, 524, 244);
        add_hud(f, 220, 180, end, 300);
        mkdd::hud::place(f);
        const float right = 1.0f - 2.0f / widening;
        for (unsigned i : {0u, 1u})
            check(std::fabs(f.states[f.draws[i].state].projection[0][3] - right) < 1e-5f,
                  "countdown overlap does not change the map or its icon's right anchor");
        const float k = 1.0f / widening, centre = (220 + end) / 640 - 1;
        check(std::fabs(f.states[f.draws[2].state].projection[0][3] - (-k + centre * (1 - k))) < 1e-5f,
              "the map does not pull the countdown away from its own centre");
    }
    mkdd::hud::set_place(mkdd::hud::Place::middle);
    gx::Frame f;
    add_hud(f, 0, 100, 220, 140);
    add_hud(f, 220, 100, 440, 140);
    add_hud(f, 440, 100, 640, 140);
    mkdd::hud::place(f);
    for (const auto& d : f.draws)
        check(std::fabs(f.states[d.state].projection[0][0] - 2.0f / (640 * widening)) < 1e-5f,
              "a full-width group of HUD sprites remains squeezed in middle placement");
    mkdd::hud::set_widening_source(nullptr);
}

void materials(gx::Frame& f, float a, float b) {
    auto s = f.states[0];
    f.states.clear();
    s.posmtx[30][0] = s.posmtx[31][1] = s.posmtx[32][2] = 1.0f;
    s.posmtx[30][3] = a;
    s.bp[0x94 / 4][0x94 % 4] = 0x100;
    f.states.push(s);
    s.posmtx[30][3] = b;
    s.bp[0x94 / 4][0x94 % 4] = 0x200;
    f.states.push(s);
    f.state_info.push_back(f.state_info[0]);
}

void texture_materials() {
    gx::Frame prev, cur;
    add_view(prev), add_view(cur);
    add_model(prev, 0, 0), add_model(cur, 0, 0.2f);
    materials(prev, 0.1f, 0.2f);
    materials(cur, 0.12f, 0.22f);
    interp::Blender b;
    check(b.pair(prev, cur), "multi-material frames pair");
    std::vector<gx::State> out(cur.states.size());
    b.states(cur, 0.5f, out.data());
    check(std::fabs(out[0].posmtx[30][3] - 0.11f) < 1e-5f, "first material scrolls from its own previous state");
    check(std::fabs(out[1].posmtx[30][3] - 0.21f) < 1e-5f, "second material never scrolls from the first material");
    const auto first = cur.states[0];
    // A changed material count cannot be matched by ordinal.
    const auto only = cur.states[1];
    cur.states.clear();
    cur.states.push(only);
    cur.state_info.resize(1);
    b.pair(prev, cur);
    b.states(cur, 0.5f, out.data());
    check(out[0].posmtx[30][3] == 0.22f, "ambiguous material counts keep the new texture transform");
    cur.states.push(first);
    cur.state_info.push_back(cur.state_info[0]);
    b.pair(prev, cur);
    b.states(cur, 0.5f, out.data());
    check(out[0].posmtx[30][3] == 0.22f && out[1].posmtx[30][3] == 0.12f,
          "reordered image bindings never blend different materials");
}

void recorded_shape() {
    mkdd::hud::set_widening_source([] { return widening; });
    mkdd::hud::set_place(mkdd::hud::Place::stretched);
    gx::Frame wide, menu;
    widening = 2;
    mkdd::hud::place(wide);  // no HUD and stretched still need shape metadata
    widening = 1;
    mkdd::hud::place(menu);
    check(wide.widening == 2 && menu.widening == 1, "queued frames retain independent shapes");
    check(std::fabs(wide.aspect() - 8.0f / 3) < 1e-6f, "an older frame keeps its own presentation aspect");
    add_view(wide), add_view(menu);
    add_model(wide, 0, 0), add_model(menu, 0, 0.2f);
    interp::Blender b;
    check(!b.pair(wide, menu), "aspect changes never blend incompatible projections");
    wide.clear();
    check(wide.widening == 1, "recycled frames reset their shape");
    mkdd::hud::set_widening_source(nullptr);
}

float twice_wide() { return 2; }
bool follows_world(const gx::Frame&, const gx::Draw&) { return true; }
void callback_handoff() {
    mkdd::hud::set_place(mkdd::hud::Place::edges);
    std::atomic<bool> start{false};
    std::thread setter([&] {
        while (!start.load(std::memory_order_acquire)) {}
        for (unsigned i = 0; i < 20000; ++i) {
            mkdd::hud::set_widening_source(i & 1 ? twice_wide : nullptr);
            mkdd::hud::set_follows_world(i & 1 ? follows_world : nullptr);
        }
    });
    start.store(true, std::memory_order_release);
    bool valid = true;
    for (unsigned i = 0; i < 20000; ++i) {
        gx::Frame f;
        add_hud(f);
        mkdd::hud::place(f);
        valid = valid && (f.widening == 1 || f.widening == 2);
    }
    setter.join();
    check(valid, "callback changes use one valid snapshot for each frame");
    mkdd::hud::set_widening_source(nullptr);
    mkdd::hud::set_follows_world(nullptr);
}
}  // namespace

int main() {
    widened_interpolation();
    independent_views();
    split_hud_boundary();
    countdown_overlay();
    texture_materials();
    recorded_shape();
    callback_handoff();
    std::printf("interp_test: %d failures\n", failures);
    return failures ? 1 : 0;
}


