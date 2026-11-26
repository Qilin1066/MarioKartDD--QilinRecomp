// What the guest's side of the GPU (gpu.cpp, recorder.cpp, texture.cpp) hands
// the renderer (renderer.cpp): a frame's worth of draws and EFB copies, in
// order, with everything they read from guest memory already read.
//
// Drawing happens on the main thread, which owns Vulkan; the guest's side
// records while guest memory is as the game left it at each command.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace mkdd::gx {

// The memory the GPU can be pointed at: the GameCube's 24 MB of MEM1, from
// physical address 0 (there is no MEM2). GX hands the GPU physical addresses
// (display lists, vertex arrays, textures, palettes, copy destinations, the
// FIFO); callers mask a virtual one down first, as the hardware ignores the
// top bits.
constexpr uint32_t kMem1Size = 0x01800000u;
constexpr bool in_memory(uint32_t physical, uint32_t bytes) { return physical <= kMem1Size && bytes <= kMem1Size - physical; }
static_assert(in_memory(0, kMem1Size) && in_memory(kMem1Size - 32, 32) && !in_memory(kMem1Size - 31, 32) && !in_memory(0x10000000u, 4) &&
              !in_memory(0x100, 0xFFFFFF00u));

// A BP register that holds an address (a texture's, a palette's source, a copy's
// destination) holds it over 32 in its low 21 bits: the GameCube's 26-bit physical
// address. GX stores (pointer & 0x3FFFFFFF) >> 5 in 24, and the GPU has no use for the
// top three: this game loads a palette with one of them set (BP 0x64 = 831280 for the
// palette at 80625000, from JUTTexture::load at a Grand Prix's start), which the console
// reads as the address below it.
constexpr uint32_t bp_address(uint32_t value) { return (value & 0x1FFFFFu) << 5; }
static_assert(bp_address(0x831280u) == 0x00625000u && bp_address(0x0BFFFFu) == 0x017FFFE0u);

// One vertex, every attribute GX can send, decoded to floats (the formats
// and fixed-point scales are the vertex attribute table's). What the vertex
// didn't have is filled as Dolphin's vertex loader fills it.
struct Vertex {
    float pos[3];
    float normal[3], tangent[3], binormal[3];
    uint8_t color0[4], color1[4];  // RGBA
    float uv[8][2];
    uint32_t matrices[3];  // position matrix row, then texture matrix rows 0-7, a byte each
};
static_assert(sizeof(Vertex) == 132);

// The compact layout for the common draw (no tangents or binormals, at most
// four texture coordinates): 58% of the bytes to write, upload and fetch.
// The renderer feeds the attributes it lacks from zeros, which is what the
// shader would see anyway (renderer.cpp). Four, not two: the game's models
// mostly have three (the main menu: 88,000 of its 89,000 vertices a frame),
// which the full layout's 132 bytes took before.
struct SmallVertex {
    float pos[3];
    float normal[3];
    uint8_t color0[4], color1[4];
    float uv[4][2];
    uint32_t matrices[3];
};
static_assert(sizeof(SmallVertex) == 76);
constexpr unsigned kSmallUvs = 4;  // texture coordinates SmallVertex has

// Which attributes the vertex format had (the shaders' lighting and texgen
// rules depend on it).
enum Has : uint32_t {
    kHasPosMatrix = 1u << 0,
    kHasNormal = 1u << 1,
    kHasTangent = 1u << 2,
    kHasBinormal = 1u << 3,
    kHasColor0 = 1u << 4,
    kHasColor1 = 1u << 5,
    kHasUv0 = 1u << 6,         // << i for UV i
    kHasTexMatrix0 = 1u << 14,  // << i for texture matrix index i
};

// The GX state a draw reads, laid out as the shaders read it (std430, all
// 16-byte vectors). Built from the XF and BP registers when they change.
struct State {
    float posmtx[64][4];     // XF 0x000: position and texture matrices, by row
    float nrmmtx[32][4];     // XF 0x400: normal matrices, a row of 3 each
    float postmtx[64][4];    // XF 0x500: post-transform matrices
    struct Light {
        float color[4];     // 0-255
        float cosatt[4], distatt[4], pos[4], dir[4];
    } lights[8];
    float projection[4][4];  // rows
    float pixel_center[4];   // xy: half-pixel correction; z, w: depth range terms
    int32_t materials[4][4];  // ambient 0, 1, material 0, 1 (RGBA 0-255)
    uint32_t xf[8][4];       // see the shader: channel controls, texgens, post matrices
    uint32_t bp[64][4];      // the BP registers, all 256
    int32_t texdims[8][4];   // per texture map: width, height; per texgen: s and t scale
    int32_t tev_regs[4][4];  // PREV, C0, C1, C2 as the TEV starts
    int32_t konst[4][4];     // the konstant colours
    int32_t indmtx[6][4];    // indirect matrices: two rows each, scale in .w
    int32_t misc[4][4];      // [0]: alpha refs, fog; [1]: fog colour; [2]: z texture
    float fog[2][4];         // a, b magnitude, b shift; range: center, ... (see shader)
    float fog_range[3][4];
};

// What a pipeline is made from: everything Vulkan 1.3 can't change dynamically.
struct PipelineKey {
    uint8_t topology = 0;     // 0 triangles, 1 lines, 2 points
    uint8_t blend = 0;        // enable
    uint8_t src = 0, dst = 0, src_alpha = 0, dst_alpha = 0;  // Vulkan blend factors
    uint8_t op = 0, op_alpha = 0;                            // Vulkan blend ops
    uint8_t write_mask = 0xF;  // RGBA
    uint8_t logic = 0;        // logic op + 1, or 0 (approximated where the GPU has none)
    // 1: the shader writes depth (Z textures with the Z test after texturing).
    // 2: the depth test and write come before the shader (BP 0x43 bit 6, GXSetZCompLoc
    //    before texturing) and its alpha test can fail: a pixel the test drops has
    //    written its depth all the same, as on the console.
    uint8_t depth_out = 0;
    uint8_t small_vertices = 0;  // the draw's vertices are SmallVertex (Frame::small)
    // GX's clipping off (XF 0x1005 bit 0, GXSetClipMode: JPA's particles): nothing is cut at the near
    // and far planes, the depth is clamped instead, where the GPU can (renderer.cpp).
    uint8_t no_clip = 0;
    bool operator==(const PipelineKey&) const = default;
};

// A draw's TEV "shape": the BP registers that decide what the pixel shader
// does, as opposed to the colours it uses. The renderer compiles a pipeline
// specialised to each shape (gx.frag's specialisation constants, in this
// order), so a weak GPU runs straight-line code instead of interpreting the
// registers per pixel. Masked to the bits the shader reads, so draws that
// differ only elsewhere share one.
constexpr unsigned kShapeWords = 70;
using ShapeWords = std::array<uint32_t, kShapeWords>;
// genmode (stage counts), 0x10-0x1F (indirect), 0x25-0x2F (indirect scales
// and refs, orders), 0xC0-0xDF (combiners), 0xF5 (Z texture mode),
// 0xF6-0xFD (konst and swap selects), 0xF3 bits 16-23 (alpha test functions).
inline ShapeWords shape_of(const uint32_t* bp) {
    ShapeWords w{};
    unsigned n = 0;
    w[n++] = bp[0x00] & 0x73C00u;  // TEV stages (10-13), indirect stages (16-18)
    for (unsigned i = 0x10; i < 0x20; ++i) w[n++] = bp[i];
    for (unsigned i = 0x25; i < 0x30; ++i) w[n++] = bp[i];
    for (unsigned i = 0xC0; i < 0xE0; ++i) w[n++] = bp[i];
    w[n++] = bp[0xF5] & 0xFu;
    for (unsigned i = 0xF6; i < 0xFE; ++i) w[n++] = bp[i];
    w[n++] = (bp[0xF3] >> 16) & 0xFFu;
    return w;
}
// The registry of shapes (recorder.cpp): the id for a draw's BP registers,
// and, from any thread, the words of an id handed out.
uint32_t shape_id(const uint32_t* bp);
// A shape remembered from an earlier run, given its id before the game starts
// (the renderer's start-up only: the recorder's table isn't locked for it).
uint32_t intern_shape(const ShapeWords& w);
ShapeWords shape_words(uint32_t id);

struct Sampler {
    uint8_t wrap_s, wrap_t;  // 0 clamp, 1 repeat, 2 mirror
    uint8_t mag_linear, min_linear, mip;  // mip: 0 none, 1 point, 2 linear
    int8_t lod_bias;   // in 1/32
    uint8_t min_lod, max_lod;  // in 1/16
    uint8_t max_aniso;  // the hardware's own anisotropic filtering: 0 none, 1 2x, 2 4x (TEXMODE0 bits 19-20)
    bool operator==(const Sampler&) const = default;
};

// ---- for pictures between two frames (interp.hpp, docs/graphics.md)
//
// Position matrices are loaded into ten slots (XF rows 0, 3, ... 27: GX_PNMTX0-9).
// Every load of one in a frame is recorded with who loaded it (its tag: the J3D
// shape packet, or GXLoadPosMtxImm's caller with bit 1 set, or for an indexed
// load the matrix's own address with bit 0 set; 0: unknown) and the camera's
// view matrix as it was then, so the same load can be found in the frame before
// and the two blended.
constexpr unsigned kMatrixSlots = 10;
constexpr uint32_t kNoLoad = 0xFFFFFFFFu;
constexpr uint16_t kNoView = 0xFFFFu;
// A load's context: the game function it was made under, where a native wrapper says so.
constexpr uint8_t kContextNone = 0, kContextFollowsWorld = 1;  // 1: a HUD mark that follows something in the 3D scene
struct Load {
    uint32_t tag;
    // Which of the things one caller draws this is, where the loader can tell (mtxtag.cpp: a
    // particle's or a model's own address); 0 where it can't. Loads pair by tag, id and slot.
    uint32_t id;
    uint8_t context;  // what was being drawn when it was loaded: kContext...
    uint16_t slot, view;  // view: index into the frame's views (kNoView: not known)
    float mv[12];         // the matrix loaded (model and view), three rows of four
};
// A camera's view matrix (j3dSys's when a tagged matrix was loaded), and the
// perspective projection first drawn with under it (XF 0x1020-0x1025).
struct View {
    float m[12];
    float projection[6];
    bool has_projection;
};
// Beside each State: the load each matrix slot holds (index into the frame's
// loads; kNoLoad: loaded in an earlier frame or never) and the view current when
// the state was made.
struct StateInfo {
    uint32_t load[kMatrixSlots];
    uint16_t view;
};

struct Draw {
    uint32_t state;        // index into the frame's states
    uint32_t has;          // Has bits
    uint32_t first_index, index_count;  // indexed draws; direct ones: the first vertex, and how many
    uint8_t direct = 0;    // 0 indexed; 1 the vertices in order, as a list; 2 as a strip (no indices to write or upload)
    PipelineKey pipeline;
    uint32_t shape;        // shape id (shape_words)
    uint8_t cull;          // Vulkan cull mode
    uint8_t depth_test, depth_write, depth_func;  // Vulkan compare op
    float viewport[6];     // x, y, width, height, min, max depth (EFB pixels)
    int32_t scissor[4];    // x, y, width, height (EFB pixels)
    uint8_t line_width = 6, point_size = 6;  // lines and points: in sixths of an EFB pixel (BP 0x22)
    int32_t textures[8];   // texture ids (-1: none)
    Sampler samplers[8];
    uint32_t primitive = 0;  // the decoder's serial number of the primitive it came from (MKDD_DFF's .prims)
    uint32_t tag = 0;        // who loaded the position matrix it draws with (Load::tag; of a joined draw, its first primitive's)
    uint8_t context = 0;     // ... and under what (Load::context)
};
// Who loaded a draw's position matrix: a J3D shape packet's address, the
// address GXLoadPosMtxImm was called from with bit 1 set (0x800C....2), an
// indexed load's source address with bit 0 set, or 0 when not known (no tagged
// loader in the build, or a matrix left from an earlier frame).
inline uint32_t draw_tag(const Draw& d) { return d.tag; }
// What its position matrix was loaded under (kContextNone, kContextFollowsWorld).
inline uint8_t draw_context(const Draw& d) { return d.context; }

// An EFB copy: to a texture in guest memory, or to an external frame buffer.
struct Copy {
    int32_t src[4];        // x, y, width, height in the EFB
    uint32_t dest;         // guest physical address
    uint32_t dest_width, dest_height;
    uint32_t format;       // the copy's texture format (texture copies), see renderer.cpp
    bool to_xfb, half_scale, from_depth;
    bool to_memory;        // also written to guest memory at `dest`, once the GPU has drawn it (4x4 at most, RGBA8's layout)
    bool efb_alpha;        // the EFB's pixel format has alpha (RGBA6_Z24); else a copy's alpha is 255
    int32_t texture;       // the id the copy becomes (texture copies)
    bool clear, clear_color, clear_alpha, clear_depth;
    uint8_t clear_rgba[4];
    uint32_t clear_z;      // 24-bit
    float gamma;
    // The copy filter (BP 0x53, 0x54): 6-bit weights for the line above, the
    // line itself and the line below (seven taps, summed into three as the
    // hardware's 2-3-2), 64 = unchanged; and whether the lines above and below
    // stop at the copy's own rectangle (else they come from the EFB around it).
    uint8_t filter[3];
    bool clamp_top, clamp_bottom;
};

// A texture decoded from guest memory, mip levels one after another.
struct Texture {
    int32_t id;
    uint32_t width, height, levels;
    std::vector<uint8_t> rgba;  // RGBA8
};

// kConvert: the EFB's pixel format changed between RGB8 and RGBA6 (BP 0x43), and the
// console's EFB keeps its bits: the same 24 a pixel are then read as the other format
// (8,8,8 as 6,6,6,6 or back). A game that draws in one format on top of what it drew in
// the other, without clearing in between, sees that. index: 0 RGB8 to RGBA6, 1 back.
struct Packet {
    enum Kind : uint8_t { kDraw, kCopy, kConvert } kind;
    uint32_t index;  // into draws or copies
};

// A vector's allocator that leaves new elements as they are instead of
// zeroing them: a fight's frame holds 85,000 vertices (6.5 MB), each wholly
// written by the decoder straight after resize() made room for it.
template <class T>
struct Unfilled : std::allocator<T> {
    template <class U>
    struct rebind {
        using other = Unfilled<U>;
    };
    Unfilled() = default;
    template <class U>
    Unfilled(const Unfilled<U>&) {}
    template <class U>
    void construct(U* p) { ::new (static_cast<void*>(p)) U; }
    template <class U, class A, class... More>
    void construct(U* p, A&& a, More&&... more) { ::new (static_cast<void*>(p)) U(std::forward<A>(a), std::forward<More>(more)...); }
};

// A frame's states, kept as blocks of 256 bytes: a state shares each block whose bytes are the same as
// the state before's (a draw mostly changes a matrix or a colour of the 5 KB; a four-view race frame has
// 843 states). Read a state whole with resolve (or operator[], a copy), or all of them in order into the
// renderer's buffer with resolve_all: the State layout the shaders read is unchanged.
class States {
public:
    static constexpr size_t kBlock = 256, kBlocks = (sizeof(State) + kBlock - 1) / kBlock;
    size_t size() const { return index_.size() / kBlocks; }
    bool empty() const { return index_.empty(); }
    size_t blocks() const { return blocks_.size(); }  // made this frame (each kBlock bytes)
    void clear() { blocks_.clear(), index_.clear(); }
    void resolve(size_t i, State& out) const {
        auto* to = reinterpret_cast<uint8_t*>(&out);
        const uint32_t* at = index_.data() + i * kBlocks;
        for (size_t b = 0; b + 1 < kBlocks; ++b) std::memcpy(to + b * kBlock, blocks_[at[b]].bytes, kBlock);
        std::memcpy(to + (kBlocks - 1) * kBlock, blocks_[at[kBlocks - 1]].bytes, sizeof(State) - (kBlocks - 1) * kBlock);
    }
    State operator[](size_t i) const {
        State s;
        resolve(i, s);
        return s;
    }
    void resolve_all(State* out) const {
        for (size_t i = 0, n = size(); i < n; ++i) resolve(i, out[i]);
    }
    // Part of a state: `bytes` from `offset` (offsetof(State, ...)) into `out` (interp.cpp's texture matrices).
    void read(size_t i, size_t offset, size_t bytes, void* out) const {
        const uint32_t* at = index_.data() + i * kBlocks;
        auto* to = static_cast<uint8_t*>(out);
        while (bytes) {
            const size_t b = offset / kBlock, in = offset % kBlock, n = std::min(bytes, kBlock - in);
            std::memcpy(to, blocks_[at[b]].bytes + in, n);
            to += n, offset += n, bytes -= n;
        }
    }
    // A new state, last. `changed`: a bit for each block that may differ from the state before's (the
    // others are taken as the same without looking); each of those is compared, and shared if it is.
    void push(const State& s, uint32_t changed = (1u << kBlocks) - 1) {
        const auto* from = reinterpret_cast<const uint8_t*>(&s);
        const size_t n = index_.size();
        const bool first = n == 0;
        index_.resize(n + kBlocks);
        uint32_t* at = index_.data() + n;
        const uint32_t* before = first ? nullptr : at - kBlocks;
        for (size_t b = 0; b < kBlocks; ++b) {
            const size_t bytes = b + 1 < kBlocks ? kBlock : sizeof(State) - b * kBlock;
            if (!first) {
                if (!((changed >> b) & 1) || std::memcmp(blocks_[before[b]].bytes, from + b * kBlock, bytes) == 0) {
                    at[b] = before[b];
                    continue;
                }
            }
            at[b] = uint32_t(blocks_.size());
            Block& k = blocks_.emplace_back();
            std::memcpy(k.bytes, from + b * kBlock, bytes);
        }
    }

private:
    struct Block {
        alignas(16) uint8_t bytes[kBlock];
    };
    std::vector<Block, Unfilled<Block>> blocks_;  // (each written as it is made)
    std::vector<uint32_t> index_;  // kBlocks a state
};
static_assert(States::kBlocks <= 32, "a block mask is 32 bits");

// One frame: everything from one XFB copy to the next.
struct Frame {
    // (The recorder writes every byte of each vertex and index it makes room for.)
    std::vector<Vertex, Unfilled<Vertex>> vertices;
    std::vector<SmallVertex, Unfilled<SmallVertex>> small;  // draws with PipelineKey::small_vertices index these
    std::vector<uint32_t, Unfilled<uint32_t>> indices;
    States states;
    std::vector<StateInfo> state_info;  // one for each state
    std::vector<Load> loads;
    std::vector<View> views;
    std::vector<Draw> draws;
    std::vector<Copy> copies;
    std::vector<Packet> packets;
    std::vector<Texture> textures;  // new this frame
    std::vector<int32_t> released;  // texture ids no longer used
    bool alpha_target = true;       // the EFB has alpha (RGBA6_Z24)
    bool captured = false;          // the frame MKDD_DFF captured (main.cpp saves our picture of it)
    bool foretold = false;          // ended early, its copy to the frame buffer foretold (recorder.hpp's request_frame_end)
    uint64_t serial = 0;            // the recorder's count of frames ended before it (recorder.hpp's real_copy)
    bool confirming = false;        // only the real copy of the frame before, which was foretold
    // Empty again, keeping the vectors' capacity (recycle_frame). A new
    // member must be reset here too.
    void clear() {
        vertices.clear(), small.clear(), indices.clear(), states.clear(), draws.clear(), copies.clear(), packets.clear();
        state_info.clear(), loads.clear(), views.clear();
        textures.clear(), released.clear();
        alpha_target = true;
        captured = false;
        foretold = confirming = false;
        serial = 0;
    }
};
// For a fatal error met while decoding (a texture, array or list outside memory): where the
// BP register that led there was written (gpu.cpp: "the FIFO", or the display list's address
// and the offset in it), and the guest whose commands are being decoded, for its call stack
// (exact with MKDD_GPU_THREAD=0; else where the game's thread has got to since).
extern const char* (*g_bp_source)(uint32_t reg, char* text, size_t room);
extern void* g_decoding_cpu;  // a qilin::rt::Cpu, or null
// MKDD_DFF: the frame the next XFB copy ends is the captured one.
void mark_captured_frame();
// The decoder's serial number of the primitive about to be recorded (tags the Draw).
void set_primitive_serial(uint32_t n);
// MKDD_DFF: while a frame is captured, the GPU reading guest memory (vertex
// arrays, textures, palettes, display lists, indexed loads); the capture saves
// what changed since it last saw those bytes, at that point in the commands,
// as Dolphin's recorder does (a game refills its vertex buffers during a frame).
// (gpu.cpp sets the hook while it records; the recorder's tests run without it.)
extern bool g_dff_use;
extern void (*g_dff_use_hook)(uint32_t physical, uint32_t size);
inline void dff_use_memory(uint32_t physical, uint32_t size) {
    if (g_dff_use_hook) g_dff_use_hook(physical, size);
}

// Hand a recorded frame to the renderer (it may wait while the renderer is
// two frames behind, as the console's CPU waits on a full FIFO).
void submit(std::unique_ptr<Frame> frame);
// The renderer's side: the next frame, or null after `timeout_ms`.
std::unique_ptr<Frame> next_frame(unsigned timeout_ms);
std::unique_ptr<Frame> next_frame_us(uint64_t timeout_us);  // (finer)
// Give a drawn frame back, so the recorder reuses its memory: a fight frame's
// vectors hold 10-20 MB, which allocated fresh each frame cost page faults
// and copies as they grew.
void recycle_frame(std::unique_ptr<Frame> frame);
// The renderer is there to take frames (else they're dropped: --headless).
void set_rendering(bool on);

}  // namespace mkdd::gx
