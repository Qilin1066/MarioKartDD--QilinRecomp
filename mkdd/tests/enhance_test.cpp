// The widened picture without the game (enhance.cpp, hud.cpp): the aspect
// getters and the cameras' rescaling over guest memory laid out as the game's
// (the offsets enhance.cpp names), and the HUD's placing on frames recorded
// through the real recorder from GX register writes, as mkdd_render_test
// makes them. No window, no GPU: the frames are inspected, not drawn.
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "enhance.hpp"
#include "gx.hpp"
#include "hud.hpp"
#include "qilin/rt/system.hpp"
#include "recorder.hpp"

namespace {

namespace rt = qilin::rt;
namespace gx = mkdd::gx;
namespace hud = mkdd::hud;
namespace enhance = mkdd::enhance;

int g_failures = 0;
void check(bool ok, const std::string& what) {
    if (!ok) ++g_failures, std::printf("FAILED: %s\n", what.c_str());
}
bool close(float a, float b, float tolerance = 0.02f) { return std::fabs(a - b) <= tolerance; }
uint32_t f2u(float f) { return std::bit_cast<uint32_t>(f); }
float peekf(uint32_t a) { return std::bit_cast<float>(rt::peek32(a)); }
void pokef(uint32_t a, float f) { rt::poke32(a, f2u(f)); }

// ---- The game's side ----

constexpr uint32_t kAspect = 0x803CBE08u, kAspectDiv2 = 0x803CBE0Cu, kRaceMgrPtr = 0x803CB7E8u, kKartCtrlPtr = 0x803CC588u,
                   kDisplayPtr = 0x803CBD70u;
constexpr uint32_t kRaceMgr = 0x80400000u, kRaceInfo = 0x80401000u, kKartCtrl = 0x80402000u, kCams = 0x80403000u, kDisplay = 0x80404000u;
constexpr float kWhole = 608.0f / 448.0f, kHalf = 608.0f / 223.0f;
uint32_t cam(unsigned i) { return kCams + 0x400 * i; }

float getter(uint32_t address) {
    for (const rt::Provided& p : enhance::replacements())
        if (p.address == address) {
            rt::Cpu c = rt::new_cpu();
            p.fn(c);
            check(c.ps0[1] == c.ps1[1], "a getter fills both halves of f1");
            return float(c.ps0[1]);
        }
    check(false, "no replacement at " + std::to_string(address));
    return 0;
}

void race(int cameras, std::initializer_list<float> aspects) {
    rt::poke32(kRaceMgrPtr, kRaceMgr);
    rt::poke32(kRaceMgr + 0x38, kRaceInfo);
    rt::poke16(kRaceInfo + 0x20, uint16_t(cameras));
    rt::poke32(kKartCtrlPtr, kKartCtrl);
    unsigned i = 0;
    for (float a : aspects) rt::poke32(kKartCtrl + 0x200 + 4 * i, cam(i)), pokef(cam(i) + 0x168, a), ++i;
}

void test_game_side() {
    pokef(kAspect, kWhole), pokef(kAspectDiv2, kHalf);  // as __sinit_systemData_cpp leaves them
    rt::poke32(kRaceMgrPtr, 0);
    check(enhance::replacements().size() == 3, "three getters are replaced");
    check(getter(0x801D0F50u) == kWhole && getter(0x801D0F58u) == kHalf && getter(0x801D0F60u) == kWhole,
          "before anything is asked for the getters give the game's own values");
    check(close(enhance::shown_aspect(), 4.0f / 3.0f, 1e-6f), "and the picture is 4:3");

    // 16:9 asked for outside a race: taken up at the frame's end, shown 4:3.
    enhance::set_aspect(16.0f / 9.0f, mkdd::Hud::edges);
    check(getter(0x801D0F50u) == kWhole, "a change waits for the end of the frame");
    check(enhance::end_frame() == 1.0f, "outside a race a frame isn't widened");
    const float w169 = (16.0f / 9.0f) / (4.0f / 3.0f);
    check(enhance::widening() == w169, "16:9 is 4/3 as wide");
    check(getter(0x801D0F50u) == kWhole * w169 && getter(0x801D0F60u) == kWhole * w169, "the whole screen's and a quarter's aspect widen");
    check(getter(0x801D0F58u) == kHalf * w169, "half the screen's aspect widens");
    check(close(enhance::shown_aspect(), 4.0f / 3.0f, 1e-6f), "menus stay 4:3 at 16:9");
    check(!enhance::in_race(), "no race without RaceMgr");

    // A race starts: DoPort copies the getters' values; frames are shown wide.
    race(4, {kWhole * w169, kWhole * w169, kHalf * w169, kHalf * w169});
    check(enhance::in_race(), "a race with RaceMgr");
    check(enhance::end_frame() == w169, "a race's frame is widened");
    check(close(enhance::shown_aspect(), 16.0f / 9.0f, 1e-6f), "and shown 16:9");
    check(peekf(cam(0) + 0x168) == kWhole * w169, "cameras are left alone while nothing changes");

    // 21:9 during the race: this frame was drawn at 16:9, the cameras change for the next.
    enhance::set_aspect(64.0f / 27.0f, mkdd::Hud::middle);
    const float w219 = (64.0f / 27.0f) / (4.0f / 3.0f);
    check(enhance::end_frame() == w169, "the frame a change arrives in keeps its own shape");
    check(close(peekf(cam(0) + 0x168), kWhole * w219, 1e-5f) && close(peekf(cam(1) + 0x168), kWhole * w219, 1e-5f),
          "live cameras are rescaled (whole screen)");
    check(close(peekf(cam(2) + 0x168), kHalf * w219, 1e-5f) && close(peekf(cam(3) + 0x168), kHalf * w219, 1e-5f),
          "live cameras are rescaled (halves)");
    check(enhance::end_frame() == w219 && close(enhance::shown_aspect(), 64.0f / 27.0f, 1e-6f), "the next frame is 21:9");

    // Two cameras: the others' slots aren't touched.
    race(2, {kHalf * w219, kHalf * w219});
    pokef(cam(2) + 0x168, 123.0f);
    enhance::set_aspect(32.0f / 9.0f, mkdd::Hud::edges);
    enhance::end_frame();
    const float w329 = (32.0f / 9.0f) / (4.0f / 3.0f);
    check(close(peekf(cam(0) + 0x168), kHalf * w329, 1e-5f) && close(peekf(cam(1) + 0x168), kHalf * w329, 1e-5f), "two cameras rescaled to 32:9");
    check(peekf(cam(2) + 0x168) == 123.0f, "a camera past the count is left alone");

    // Back to 4:3 and the cameras hold the game's own values again.
    enhance::set_aspect(4.0f / 3.0f, mkdd::Hud::edges);
    enhance::end_frame();
    check(close(peekf(cam(0) + 0x168), kHalf, 1e-5f) && enhance::widening() == 1.0f, "4:3 gives the cameras their own aspect back");
    check(getter(0x801D0F58u) == kHalf, "and the getters theirs, exactly");

    // The race ends (KartCtrl's pointer stays, stale): nothing of it is read.
    enhance::set_aspect(16.0f / 9.0f, mkdd::Hud::edges);
    enhance::end_frame();
    rt::poke32(kRaceMgrPtr, 0);
    rt::poke32(kKartCtrlPtr, 0x12345678u);
    enhance::set_aspect(64.0f / 27.0f, mkdd::Hud::edges);
    check(enhance::end_frame() == 1.0f && enhance::widening() == w219, "outside a race a change touches no camera");
    check(close(enhance::shown_aspect(), 4.0f / 3.0f, 1e-6f), "and the picture is 4:3 again");

    // The limits: never narrower than 4:3, never past 4:1.
    enhance::set_aspect(1.0f, mkdd::Hud::edges), enhance::end_frame();
    check(enhance::widening() == 1.0f, "narrower than 4:3 is 4:3");
    enhance::set_aspect(8.0f, mkdd::Hud::edges), enhance::end_frame();
    check(enhance::widening() == 3.0f, "wider than 4:1 is 4:1");
    enhance::set_aspect(4.0f / 3.0f, mkdd::Hud::edges), enhance::end_frame();

    // LAN's draw skip.
    rt::poke32(kDisplayPtr, 0);
    enhance::draw_every_tick();  // (no display yet: nothing to do)
    rt::poke32(kDisplayPtr, kDisplay);
    rt::poke32(kDisplay + 0x38, 2), enhance::draw_every_tick();
    check(rt::peek32(kDisplay + 0x38) == 1, "a draw interval of 2 becomes 1");
    enhance::draw_every_tick();
    check(rt::peek32(kDisplay + 0x38) == 1, "and 1 stays");
}

// ---- The HUD, through the recorder ----

// GX's registers as gpu.cpp keeps them, set up as J2D draws: a viewport and a
// scissor box in the 608x448 frame, an orthographic projection over a box of
// the game's 2D coordinates, coloured quads through position matrix 0 (identity).
struct Gx {
    uint32_t cp[256] = {}, xf[0x1058] = {}, bp[256] = {};
    Gx() { bp[0x53] = 0x595000, bp[0x54] = 0x15; }
    gx::Registers regs() const { return {cp, xf, bp}; }
    void BP(uint32_t reg, uint32_t v) { bp[reg] = v, gx::bp_written(reg, v, true); }
    void XF(uint32_t a, uint32_t v) { xf[a] = v, gx::state_changed(); }
    void XFf(uint32_t a, float v) { XF(a, f2u(v)); }
    void CP(uint32_t reg, uint32_t v) { cp[reg] = v, gx::state_changed(); }

    void setup() {
        BP(0x00, 1u << 4);
        BP(0x59, (342u >> 1) | ((342u >> 1) << 10));
        for (uint32_t row = 0; row < 3; ++row) XFf(row * 4 + row, 1.0f);
        CP(0x30, 30u << 6);
        XF(0x1009, 1), XF(0x100E, 1), XF(0x1010, 1), XF(0x103F, 0);
        BP(0x28, 0);
        BP(0xC0, 10u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));
        BP(0xC1, (5u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19));
        for (uint32_t s = 0; s < 4; ++s) BP(0xF6 + 2 * s, 0 | (1u << 2)), BP(0xF7 + 2 * s, 2 | (3u << 2));
        BP(0xF3, (7u << 16) | (7u << 19));
        BP(0x40, 1u | (3u << 1) | (1u << 4));
        BP(0x41, 1u << 3);
        BP(0x43, 0);
        CP(0x50, (1u << 9) | (1u << 13));  // position and colour, direct
        CP(0x60, 0);
        CP(0x70, 1u | (4u << 1) | (5u << 14));  // xyz f32, RGBA8
        view(0, 0, 608, 448);
        scissor(0, 0, 608, 448);
        ortho(0, 608, 0, 448);
    }
    void view(float x, float y, float w, float h) {
        XFf(0x101A, w / 2), XFf(0x101B, -h / 2), XFf(0x101C, 16777215.0f);
        XFf(0x101D, 342.0f + x + w / 2), XFf(0x101E, 342.0f + y + h / 2), XFf(0x101F, 16777215.0f);
    }
    void scissor(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        BP(0x20, (342u + y) | ((342u + x) << 12));
        BP(0x21, (342u + y + h - 1) | ((342u + x + w - 1) << 12));
    }
    void ortho(float l, float r, float t, float b) {  // C_MTXOrtho, near 0, far 1
        XFf(0x1020, 2 / (r - l)), XFf(0x1021, -(r + l) / (r - l)), XFf(0x1022, 2 / (t - b)), XFf(0x1023, -(t + b) / (t - b));
        XFf(0x1024, -1.0f), XFf(0x1025, -1.0f), XF(0x1026, 1);
    }
    void perspective() { XFf(0x1020, 1.0f), XFf(0x1021, 0.0f), XFf(0x1022, 1.3f), XFf(0x1023, 0.0f), XF(0x1026, 0); }
    // A quad; `own_draw`: as a pane with its own matrix load (else it joins the draw before it).
    void quad(float x0, float y0, float x1, float y1, bool own_draw = true, float z = 0) {
        if (own_draw) gx::state_changed();
        std::vector<uint8_t> data;
        const float v[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        for (const auto& p : v) {
            for (float c : {p[0], p[1], z})
                for (int i = 3; i >= 0; --i) data.push_back(uint8_t(f2u(c) >> (8 * i)));
            data.insert(data.end(), {255, 255, 255, 255});
        }
        gx::record_primitive(regs(), 0x80, data.data(), 4);
    }
    // An EFB copy to a texture, as an effect capturing its work area.
    void copy_texture(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        BP(0x49, x | (y << 10)), BP(0x4A, (w - 1) | ((h - 1) << 10)), BP(0x4B, 0x00400000u >> 5);
        const uint32_t cmd = 3u << 3;
        BP(0x52, cmd);
        gx::record_copy(regs(), cmd);
    }
    // The copy to the XFB that ends the frame; the frame as the renderer would get it.
    std::unique_ptr<gx::Frame> end() {
        BP(0x49, 0), BP(0x4A, 607u | (447u << 10)), BP(0x4B, 0x00500000u >> 5), BP(0x4D, 608 / 16), BP(0x4E, 256);
        const uint32_t cmd = 1u << 14;
        BP(0x52, cmd);
        gx::record_copy(regs(), cmd);
        std::unique_ptr<gx::Frame> f = gx::next_frame(0), more;
        while ((more = gx::next_frame(0))) f = std::move(more);
        return f;
    }
};

// Where draw `i` of a frame lands across the EFB, in pixels (the test's quads go
// through the identity matrix, so the projection's first row and the viewport say).
struct Span {
    float x0 = 1e30f, x1 = -1e30f;
};
Span span_of(const gx::Frame& f, size_t i) {
    Span s;
    const gx::Draw& d = f.draws[i];
    const gx::State st = f.states[d.state];  // (a copy: the frame keeps its states as blocks)
    const float* row = st.projection[0];
    const auto one = [&](const float* pos) {
        const float px = d.viewport[0] + (row[0] * pos[0] + row[3] + 1) * 0.5f * d.viewport[2];
        s.x0 = std::min(s.x0, px), s.x1 = std::max(s.x1, px);
    };
    for (uint32_t k = 0; k < d.index_count; ++k) {
        const uint32_t v = d.direct ? d.first_index + k : f.indices[d.first_index + k];
        one(d.pipeline.small_vertices == gx::kLayoutTiny ? f.tiny[v].pos : d.pipeline.small_vertices ? f.small[v].pos : f.vertices[v].pos);
    }
    return s;
}

float g_test_widening = 1.0f;

// One scene recorded twice, unwidened and widened: draw `i` must have gone from
// its own span to that span squeezed about `anchor` (NaN: left exactly as it was).
struct Expect {
    size_t draw;
    float anchor;
    const char* what;
};
template <class Scene>
void scene(const char* name, hud::Place place, float aspect, Scene&& draw, std::initializer_list<Expect> expects, size_t draws) {
    const float w = aspect / (4.0f / 3.0f);
    hud::set_place(place);
    Gx g;
    g_test_widening = 1.0f;
    g.setup(), draw(g);
    auto plain = g.end();
    g_test_widening = w;
    g.setup(), draw(g);
    auto wide = g.end();
    const std::string at = std::string(name) + " at " + std::to_string(aspect).substr(0, 4) + ": ";
    if (!plain || !wide || plain->draws.size() != draws || wide->draws.size() != draws) {
        check(false, at + "the scene records " + std::to_string(draws) + " draws (got " + std::to_string(wide ? wide->draws.size() : 0) + ")");
        return;
    }
    for (const Expect& e : expects) {
        const Span o = span_of(*plain, e.draw), n = span_of(*wide, e.draw);
        if (std::isnan(e.anchor)) {
            check(n.x0 == o.x0 && n.x1 == o.x1 && wide->draws[e.draw].state == plain->draws[e.draw].state,
                  at + e.what + " is left as drawn (" + std::to_string(n.x0) + ".." + std::to_string(n.x1) + ")");
            continue;
        }
        const float x0 = e.anchor + (o.x0 - e.anchor) / w, x1 = e.anchor + (o.x1 - e.anchor) / w;
        check(close(n.x0, x0) && close(n.x1, x1),
              at + e.what + ": " + std::to_string(o.x0) + ".." + std::to_string(o.x1) + " about " + std::to_string(e.anchor) + " should be " +
                  std::to_string(x0) + ".." + std::to_string(x1) + ", is " + std::to_string(n.x0) + ".." + std::to_string(n.x1));
        // What the squeeze is for: shown `w` times as wide, the part has the width it had at 4:3.
        check(close((n.x1 - n.x0) * w, o.x1 - o.x0, 0.05f), at + e.what + " keeps its 4:3 width on screen");
    }
    gx::recycle_frame(std::move(plain));
    gx::recycle_frame(std::move(wide));
}

constexpr float kLeave = std::numeric_limits<float>::quiet_NaN();

void test_hud() {
    hud::set_widening_source([] { return g_test_widening; });
    hud::set_follows_world(nullptr);
    for (const float aspect : {16.0f / 9.0f, 64.0f / 27.0f, 32.0f / 9.0f}) {
        // One view, the whole screen.
        const auto one_view = [](Gx& g) {
            g.quad(20, 20, 120, 60);      // 0: top left
            g.quad(488, 20, 588, 60);     // 1: top right
            g.quad(254, 200, 354, 240);   // 2: the middle
            g.quad(0, 0, 608, 448);       // 3: a fade
            g.quad(14, 300, 594, 330);    // 4: a bar 95% of the width
            g.quad(150, 100, 200, 120);   // 5, 6: two pieces of a line; the first alone would go left,
            g.quad(202, 100, 262, 120);   //       together their centre (206) is in the middle third
            g.quad(150, 400, 200, 420);   // 7: the same piece on a line of its own
            g.quad(20, 150, 60, 170);     // 8: two quads in one draw (a text line's glyphs)
            g.quad(70, 150, 110, 170, false);
            g.perspective();
            g.quad(-0.2f, -0.2f, 0.2f, 0.2f, true, -1);  // 9: 3D
        };
        scene("one view, edges", hud::Place::edges, aspect, one_view,
              {{0, 0, "a part at the left"}, {1, 608, "a part at the right"}, {2, 304, "a part in the middle third"},
               {3, kLeave, "a full-screen fade"}, {4, kLeave, "a bar across the screen"},
               {5, 206, "the first piece of a line"}, {6, 206, "the second piece of a line"}, {7, 0, "a piece on its own line"},
               {8, 0, "a draw of two quads"}, {9, kLeave, "a 3D draw"}},
              10);
        scene("one view, middle", hud::Place::middle, aspect, one_view,
              {{0, 304, "a part at the left"}, {1, 304, "a part at the right"}, {2, 304, "a part in the middle"},
               {3, kLeave, "a full-screen fade"}, {5, 304, "a piece of a line"}, {8, 304, "a draw of two quads"}, {9, kLeave, "a 3D draw"}},
              10);
        scene("one view, stretched", hud::Place::stretched, aspect, one_view,
              {{0, kLeave, "a part at the left"}, {1, kLeave, "a part at the right"}, {2, kLeave, "a part in the middle"}, {5, kLeave, "a line"}},
              10);

        // Two views, one above the other, each the screen's width (the game's
        // sca3DViewPortDiv2): the 2D box is still the whole screen, cut by a scissor.
        const auto two_views = [](Gx& g) {
            g.scissor(0, 0, 608, 223);
            g.quad(20, 20, 120, 60);     // 0
            g.quad(500, 180, 590, 210);  // 1
            g.scissor(0, 225, 608, 223);
            g.quad(20, 245, 120, 285);   // 2
            g.quad(500, 400, 590, 430);  // 3
            g.quad(0, 225, 608, 448);    // 4: the lower view's fade
        };
        scene("two views, edges", hud::Place::edges, aspect, two_views,
              {{0, 0, "the upper view's left part"}, {1, 608, "the upper view's right part"}, {2, 0, "the lower view's left part"},
               {3, 608, "the lower view's right part"}, {4, kLeave, "a view's fade"}},
              5);
        scene("two views, middle", hud::Place::middle, aspect, two_views, {{0, 304, "the upper view's left part"}, {3, 304, "the lower view's right part"}}, 5);

        // Four views by scissor boxes in the whole screen's 2D box.
        const auto four_scissors = [](Gx& g) {
            g.scissor(0, 0, 303, 223);
            g.quad(10, 10, 60, 40);        // 0: top-left view, its left
            g.quad(240, 10, 290, 40);      // 1: top-left view, its right (by the divider)
            g.quad(126, 100, 176, 130);    // 2: top-left view, its middle
            g.scissor(305, 225, 303, 223);
            g.quad(320, 240, 380, 270);    // 3: bottom-right view, its left (by the divider)
            g.quad(540, 240, 600, 270);    // 4: bottom-right view, its right
            g.quad(305, 225, 608, 448);    // 5: that view's fade
        };
        scene("four views by scissor, edges", hud::Place::edges, aspect, four_scissors,
              {{0, 0, "a quarter's left part"}, {1, 303, "a quarter's right part"}, {2, 151, "a quarter's middle part"},
               {3, 305, "the last quarter's left part"}, {4, 608, "the last quarter's right part"}, {5, kLeave, "a quarter's fade"}},
              6);
        scene("four views by scissor, middle", hud::Place::middle, aspect, four_scissors,
              {{0, 151.5f, "a quarter's left part"}, {1, 151.5f, "a quarter's right part"}, {3, 456.5f, "the last quarter's left part"},
               {4, 456.5f, "the last quarter's right part"}},
              6);

        // Four views by viewports, each with a 2D box of its own (608 wide, mapped to 303 pixels).
        const auto four_viewports = [](Gx& g) {
            g.view(305, 0, 303, 223), g.scissor(305, 0, 303, 223);
            g.quad(20, 20, 120, 60);     // 0: at pixels 315..365
            g.quad(488, 20, 588, 60);    // 1: at pixels 548..598
            g.quad(0, 0, 608, 448);      // 2
        };
        scene("four views by viewport, edges", hud::Place::edges, aspect, four_viewports,
              {{0, 305, "a quarter's left part"}, {1, 608, "a quarter's right part"}, {2, kLeave, "a quarter's fade"}}, 3);
        scene("four views by viewport, middle", hud::Place::middle, aspect, four_viewports,
              {{0, 456.5f, "a quarter's left part"}, {1, 456.5f, "a quarter's right part"}}, 3);

        // An effect's work area: flat draws a texture copy then reads are no HUD.
        scene("a captured area", hud::Place::edges, aspect,
              [](Gx& g) {
                  g.quad(20, 20, 100, 60);  // 0: inside what's copied
                  g.copy_texture(0, 0, 128, 128);
                  g.quad(20, 300, 100, 340);  // 1: drawn after, elsewhere
                  g.quad(20, 20, 100, 60);    // 2: drawn after the copy, in the same place
              },
              {{0, kLeave, "a draw a copy reads back"}, {1, 0, "a part drawn after the copy"}, {2, 0, "a part drawn over the copied area later"}}, 3);

        // A mark that follows a kart keeps its place and loses only the stretch.
        hud::set_follows_world([](const gx::Frame&, const gx::Draw& d) { return d.primitive == 777; });
        scene("a mark over a kart", hud::Place::edges, aspect,
              [](Gx& g) {
                  gx::set_primitive_serial(777);
                  g.quad(40, 100, 80, 120);  // 0: in the left third
                  gx::set_primitive_serial(0);
                  g.quad(40, 300, 80, 320);  // 1: an ordinary part beside it
              },
              {{0, 60, "a mark at the left"}, {1, 0, "a part at the left"}}, 2);
        scene("a mark over a kart, middle", hud::Place::middle, aspect,
              [](Gx& g) {
                  gx::set_primitive_serial(777);
                  g.quad(40, 100, 80, 120);
                  gx::set_primitive_serial(0);
              },
              {{0, 60, "a mark at the left"}}, 1);
        hud::set_follows_world(nullptr);
    }
    // At 4:3 nothing is touched at all.
    scene("4:3", hud::Place::edges, 4.0f / 3.0f, [](Gx& g) { g.quad(20, 20, 120, 60); }, {{0, kLeave, "a part"}}, 1);

    // The rule on its own.
    float a = 0;
    check(hud::anchor_of(hud::Place::edges, 0, 608, 100, 200, false, a) && a == 0, "centre 150 of 608: the left edge");
    check(hud::anchor_of(hud::Place::edges, 0, 608, 180, 240, false, a) && a == 210, "centre 210: its own centre");
    check(hud::anchor_of(hud::Place::edges, 0, 608, 400, 500, false, a) && a == 608, "centre 450: the right edge");
    check(!hud::anchor_of(hud::Place::edges, 0, 608, 30, 578, false, a), "548 of 608 wide (90%) fills the view");
    check(hud::anchor_of(hud::Place::edges, 0, 608, 31, 577, false, a), "546 of 608 doesn't");
    check(!hud::anchor_of(hud::Place::edges, 0, 608, -200, 800, false, a), "wider than the view fills it");
    check(!hud::anchor_of(hud::Place::stretched, 0, 608, 100, 200, false, a), "stretched: nothing is placed");
}

const rt::FunctionEntry kNoFunctions[] = {{0x80004000u, [](rt::Cpu&) {}}};

}  // namespace

int main() {
    rt::init({kNoFunctions, {}});  // guest memory
    gx::set_rendering(true);       // (frames are kept for whoever takes them: here, the test)
    test_game_side();
    test_hud();
    if (g_failures) {
        std::printf("enhance_test: %d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("enhance_test: all checks passed");
    return 0;
}
