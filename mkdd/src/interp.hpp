// Pictures between two of the game's frames (frame rates above its own).
//
// The game steps its logic and draws once per retrace, 59.94 times a second,
// and has no time step to scale (docs/enhancements.md, A), so more pictures a
// second can only come from here: the renderer keeps two consecutive recorded
// frames and draws the newer one's draw list again with its position matrices
// moved part of the way back towards the older one's. The guest takes no part
// and can't see it.
//
// Which matrix of the older frame belongs to a matrix of the newer one is told
// by the tags the two native loaders give (mtxtag.cpp; gx.hpp's Load): the
// n-th load by one tag into one slot in each frame is the same thing, as long
// as both frames have as many. Each loaded matrix is split into the camera's
// view (given with the tag) and the model's own; the two are blended apart,
// rotations as rotations (a wheel turns half a revolution a tick), and put
// together again. What has no partner keeps its place in the world under the
// blended camera; a camera that cuts, a model that jumps, flat (orthographic)
// drawing and anything untagged are drawn as the newer frame has them.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "gx.hpp"
#include "qilin/rt/system.hpp"
#include "settings.hpp"

namespace mkdd::mtxtag {
// The two matrix loaders, natively (mtxtag.cpp): for rt::provide, once the plan
// marks 80067A64-80067AE8 and 800C20D8-800C2128 replaced. Asking for them is
// what says they are in (installed()).
std::span<const qilin::rt::Provided> replacements();
// And the wrappers of Race2D::drawPlayerMark (8014573C), drawAlarm (80146CF0) and drawBomb
// (80147B1C), which the plan must wrap: for rt::provide too.
std::span<const qilin::rt::Provided> wrappers();
bool installed();
// For hud::set_follows_world: a flat draw that follows something in the 3D
// scene, as far as the tags tell (the lens flare; see mtxtag.cpp for what they can't).
bool follows_world(const gx::Frame& frame, const gx::Draw& draw);
}  // namespace mkdd::mtxtag

namespace mkdd::interp {

// ---- pacing (main.cpp; enhance.hpp's frame-rate pair forwards here)

// Pictures between frames can be offered: the tagged matrix loaders are in the
// build (mtxtag::replacements() was given to the runtime).
bool supported();
// The frame-rate setting, with the display's refresh in Hz (0: not known).
// original and sixty: every frame is shown as it comes, nothing is held back
// and nothing blended. display and unlimited: the newest frame is held for
// one logic tick while pictures blended from the one before towards it are
// shown, one a refresh (display) or as fast as they draw (unlimited).
void set_frame_rate(FrameRate rate, double display_hz);
// display or unlimited is chosen (and supported): the main loop asks the
// renderer for pictures between frames (Renderer::render_between).
bool between_frames();
// unlimited: present without waiting for the retrace.
bool unlimited();
// The phase of the picture to present at `now_ns` (the time it will reach the
// screen, as near as the caller knows: the retrace clock), the newest frame
// having come at `arrived_ns`: above 0, the frame before, up to 1, the newest
// frame itself, reached one tick after it came. Where the display's rate is a
// whole multiple n of the game's, phases are the n even steps k/n.
double phase(uint64_t arrived_ns, uint64_t now_ns);

// ---- blending (renderer.cpp)

class Blender {
public:
    // Pair `cur`'s matrix loads and views with `prev`'s (once a frame, as it
    // arrives). False: nothing of this frame can be blended (the picture
    // between is the newer frame's).
    bool pair(const gx::Frame& prev, const gx::Frame& cur);
    // `cur`'s states for a picture at `phase` (0 to 1) into `out` (room for
    // cur.states.size()): position and normal matrices, lights and
    // projections moved towards the frame before; at 1, cur's own, bit for bit.
    void states(const gx::Frame& cur, float phase, gx::State* out);
    // What the last pair() found (the tests, the stats line).
    struct Counts {
        uint32_t loads = 0, paired = 0, alone = 0, jumped = 0, views = 0, cuts = 0;
        uint32_t textures = 0, texture_snaps = 0;  // texture matrices blended, and left as the newer frame has them (a wrap, a jump)
    };
    const Counts& counts() const { return counts_; }

private:
    void pair_textures(const gx::Frame& prev, const gx::Frame& cur);
    struct Key {
        uint64_t key;  // tag and id
        uint32_t slot, view, index;
    };
    struct Pose {  // a matrix as rotation (a quaternion), scale and translation
        float q[4], s[3], t[3];
        bool rigid;  // false: not a rotation times a scale; `m` is blended entry by entry
        float m[12];
    };
    struct Prepared {
        uint8_t how;  // 0 as loaded, 1 paired (from/to), 2 alone (to under the blended view)
        Pose from, to;
        float mv0[12];  // paired: the older frame's matrix as it was loaded (phase 0 is exactly it)
        uint32_t prev;  // paired: the older frame's load
    };
    // A state's texture matrices (XF rows 30-59, GX_TEXMTX0-9) blended from those of the older
    // frame's matching material state under its first paired position load. Counts and image
    // bindings must agree: an animated texture moves without borrowing another material's transform.
    struct TexBlend {
        uint32_t state, groups;  // groups: bit g, the matrix at row 30 + 3g
        float from[30][4];
    };
    std::vector<TexBlend> tex_;
    struct TextureState { uint32_t load, state; };
    std::vector<TextureState> prev_tex_states_, cur_tex_states_;
    std::vector<uint8_t> matched_loads_;
    struct ViewPair {
        bool blend;
        Pose from, to;      // the camera: rotation and eye position
        float inverse[12];  // of the newer view
        float projection[2][6];
        bool has_projection;
    };
    std::vector<Key> prev_keys_, cur_keys_;
    std::vector<Prepared> prepared_;  // one for each of cur's loads
    std::vector<ViewPair> views_;
    std::vector<float> last_step_;    // each view's eye movement in the pair before (below 0: not known)
    std::vector<float> blended_;      // scratch: twelve floats a load, then a view
    Counts counts_;
};

// The maths, for the tests: `out` = `a` towards `b` by t, as rotation, scale
// and translation apart (three rows of four).
void blend_matrix(const float a[12], const float b[12], float t, float out[12]);

}  // namespace mkdd::interp


