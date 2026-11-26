// The renderer end to end, without the game: GX register writes as a game
// makes them go through the real recorder (mkdd/src/recorder.cpp) and
// renderer (renderer.cpp) on a Vulkan device, and the frame copied to the
// external frame buffer is read back and checked pixel by pixel.
//
// Covers what the game's boot screens don't reach: logic ops (with the GPU's
// own and with the blending approximation), line widths and point sizes (and
// their one-pixel fallbacks), Z textures (late and early Z), EFB copies of
// depth, and the RG8/GB8 copy formats. Exit 77 (skipped) without a Vulkan 1.3
// device.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gpu.hpp"
#include "gx.hpp"
#include "hw.hpp"
#include "os.hpp"
#include "recorder.hpp"
#include "renderer.hpp"
#include "video.hpp"
#include "qilin/host/vulkan.hpp"
#include "qilin/rt/system.hpp"

namespace rt = qilin::rt;
namespace host = qilin::host;
namespace gx = mkdd::gx;

// The command decoder (gpu.cpp) is linked in whole, for the scenarios that go
// through it; what it asks of the OS and the hardware layer isn't there. Its
// registers are never mapped here and its GPU thread never started, so these
// only have to exist; the draw-done and token events it queues go nowhere.
namespace mkdd {
uint64_t time_base() { return 0; }
void wake_idle() {}
void raise_interrupt(uint32_t) {}
void add_timer_source(std::function<void()>, std::function<uint64_t()>) {}
void on_wpar_write(void (*)()) {}
std::vector<MmioRange> g_mapped;  // (--mtxtag writes the FIFO's registers through these)
void map_mmio(const MmioRange& range) { g_mapped.push_back(range); }
uint32_t frame_count() { return 0; }
}  // namespace mkdd

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++g_failures;
}

uint32_t f2u(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

constexpr uint32_t kXfb = 0x00500000u, kTexI8 = 0x00300000u, kCopyTex = 0x00400000u, kScratch = 0x00600000u;

// GX's registers, as gpu.cpp keeps them, and the writes it makes.
struct Gx {
    uint32_t cp[256] = {}, xf[0x1058] = {}, bp[256] = {};
    // Where clears copy to. The recorder's copies are global, so a second
    // renderer in the same run uses its own address (a texture one renderer
    // made, the other never did).
    uint32_t scratch = kScratch;
    // What GXInit leaves: the copy filter's default weights, 21/22/21 on the
    // line itself (64: unchanged), none on the lines above and below.
    Gx() { bp[0x53] = 0x595000, bp[0x54] = 0x15; }
    gx::Registers regs() const { return {cp, xf, bp}; }
    void BP(uint32_t reg, uint32_t v) {
        bp[reg] = v;
        gx::bp_written(reg, v, true);
    }
    void XF(uint32_t a, uint32_t v) {
        xf[a] = v;
        gx::state_changed();
    }
    void XFf(uint32_t a, float v) { XF(a, f2u(v)); }
    void CP(uint32_t reg, uint32_t v) {
        cp[reg] = v;
        gx::state_changed();
    }

    // What a 2D game sets up: a 640x528 viewport and scissor, an orthographic
    // projection with z in [-1, 0], one colour channel from the vertices, one
    // TEV stage passing the rasterised colour, depth test LEQUAL with updates.
    void setup() {
        BP(0x00, (1u << 4));                                    // genmode: 1 colour channel, 1 TEV stage, no culling
        BP(0x20, 342u | (342u << 12));                           // scissor top-left (+342)
        BP(0x21, (342u + 527u) | ((342u + 639u) << 12));         // scissor bottom-right
        BP(0x59, (342u >> 1) | ((342u >> 1) << 10));             // scissor offset
        XFf(0x101A, 320.0f), XFf(0x101B, -264.0f), XFf(0x101C, 16777215.0f);  // viewport: half sizes, z range
        XFf(0x101D, 342.0f + 320.0f), XFf(0x101E, 342.0f + 264.0f), XFf(0x101F, 16777215.0f);  // origin, far
        // Ortho over (0,0)-(640,528), near 0 and far 1 (GX's C_MTXOrtho).
        XFf(0x1020, 2.0f / 640.0f), XFf(0x1021, -1.0f), XFf(0x1022, 2.0f / -528.0f), XFf(0x1023, 1.0f);
        XFf(0x1024, -1.0f), XFf(0x1025, -1.0f), XF(0x1026, 1);
        for (uint32_t row = 0; row < 3; ++row) XFf(row * 4 + row, 1.0f);  // position matrix 0: identity
        for (uint32_t row = 0; row < 3; ++row) XFf((30 + row) * 4 + row, 1.0f);  // texture matrix 30: identity
        CP(0x30, 30u << 6);                                      // matrix indices: position 0, texture 0 -> 30
        XF(0x1009, 1), XF(0x100E, 1), XF(0x1010, 1);             // 1 colour channel, material from the vertex
        XF(0x103F, 0);                                           // no texgens
        BP(0x28, 0);                                             // stage 0: no texture, colour channel 0
        color_from(10, 5);                                       // rasterised colour and alpha
        for (uint32_t s = 0; s < 4; ++s) BP(0xF6 + 2 * s, 0 | (1u << 2)), BP(0xF7 + 2 * s, 2 | (3u << 2));  // identity swaps
        BP(0xF3, (7u << 16) | (7u << 19));                       // alpha test: always
        BP(0x40, 1u | (3u << 1) | (1u << 4));                    // Z: on, LEQUAL, updates
        BP(0x41, 1u << 3);                                       // colour updates, no blending
        BP(0x43, 0);                                             // RGB8_Z24, Z after texturing
        // Vertex format 0: position xyz f32, colour RGBA8, both direct.
        CP(0x50, (1u << 9) | (1u << 13));
        CP(0x60, 0);
        CP(0x70, 1u | (4u << 1) | (5u << 14));
        clear();
    }
    // TEV stage 0's colour and alpha outputs: input d (colour 10 rasc, 8 texc; alpha 5 rasa, 4 texa).
    void color_from(uint32_t c, uint32_t a) {
        BP(0xC0, c | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));
        BP(0xC1, (a << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19));
    }
    // Texture map 0 sampled by stage 0 from texgen 0 (texture coordinate 0).
    void texture(uint32_t address, uint32_t w, uint32_t h, uint32_t format) {
        BP(0x80, 0), BP(0x84, 0);                                // clamp, nearest, no mips
        BP(0x88, (w - 1) | ((h - 1) << 10) | (format << 20));
        BP(0x94, address >> 5);
        BP(0x30, w - 1), BP(0x31, h - 1);                        // texture coordinate scale
        XF(0x103F, 1);
        XF(0x1040, 5u << 7);                                     // texgen 0: from texture coordinate 0, ST
        BP(0x28, 1u << 6);                                       // stage 0: texture map 0, coordinate 0, colour 0
        CP(0x50, (1u << 9) | (1u << 13));
        CP(0x60, 1u);                                            // texture coordinate 0 direct
        CP(0x70, 1u | (4u << 1) | (5u << 14) | (1u << 21) | (4u << 22));  // + tc0: s,t f32
    }
    void no_texture() {
        XF(0x103F, 0);
        BP(0x28, 0);
        CP(0x60, 0);
        CP(0x70, 1u | (4u << 1) | (5u << 14));
    }
    // A clear of the whole EFB (black, far), by a copy as games make them.
    void clear() {
        BP(0x4F, 0), BP(0x50, 0), BP(0x51, 0xFFFFFF);
        BP(0x49, 0), BP(0x4A, 639u | (527u << 10));
        BP(0x4B, scratch >> 5);
        const uint32_t cmd = (1u << 11) | (3u << 3);             // clear, RGBA8 texture copy
        BP(0x52, cmd);
        gx::record_copy(regs(), cmd);
    }
};

struct V {
    float x, y, z;
    uint8_t r, g, b, a;
    float s = 0, t = 0;
};
void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
}
void draw(Gx& g, uint8_t opcode, std::initializer_list<V> vs, bool tex = false) {
    std::vector<uint8_t> data;
    for (const V& v : vs) {
        put_be32(data, f2u(v.x)), put_be32(data, f2u(v.y)), put_be32(data, f2u(v.z));
        data.insert(data.end(), {v.r, v.g, v.b, v.a});
        if (tex) put_be32(data, f2u(v.s)), put_be32(data, f2u(v.t));
    }
    gx::record_primitive(g.regs(), opcode, data.data(), uint32_t(vs.size()));
}
void quad(Gx& g, float x0, float y0, float x1, float y1, float z, uint8_t r, uint8_t gr, uint8_t b, bool tex = false) {
    draw(g, 0x80, {{x0, y0, z, r, gr, b, 255, 0, 0}, {x1, y0, z, r, gr, b, 255, 1, 0}, {x1, y1, z, r, gr, b, 255, 1, 1},
                   {x0, y1, z, r, gr, b, 255, 0, 1}},
         tex);
}

// The Vulkan side: render each frame the recorder hands over, read the XFB back.
struct Target {
    host::Gpu& gpu;
    mkdd::Renderer renderer;
    Target(host::Gpu& g) : gpu(g), renderer(g, 1) {}

    std::vector<uint8_t> finish(Gx& g, bool clear_after = true) {
        g.BP(0x49, 0), g.BP(0x4A, 639u | (527u << 10));
        g.BP(0x4B, kXfb >> 5), g.BP(0x4D, 640 / 16), g.BP(0x4E, 256);
        const uint32_t cmd = 1u << 14;
        g.BP(0x52, cmd);
        gx::record_copy(g.regs(), cmd);  // ends the frame
        auto px = shown();
        if (clear_after) g.clear();  // the next scenario starts black and far
        return px;
    }
    size_t last_draws = 0;  // the draws of the frame shown() drew last
    // The frames recorded are drawn, and the frame buffer read back.
    // `hold`: the renderer keeps the frame, for pictures between it and the one before.
    std::vector<uint8_t> shown(bool hold = false) {
        while (auto f = gx::next_frame(0)) {
            last_draws = f->draws.size();
            if (hold) {
                renderer.render_held(std::move(f));
            } else {
                renderer.render(*f);
                gx::recycle_frame(std::move(f));
            }
        }
        return read(renderer.xfb(kXfb));
    }
    // The picture between the two frames held, at `phase`; empty if the renderer gives none.
    std::vector<uint8_t> between(float phase) {
        const host::Image* x = renderer.render_between(phase);
        return x ? read(x) : std::vector<uint8_t>{};
    }
    std::vector<uint8_t> read(const host::Image* x) {
        vkDeviceWaitIdle(gpu.device);
        if (!x) rt::fatal(nullptr, "render_test: no XFB image");
        const VkImage image = x->image;
        const uint32_t w = x->width, h = x->height;
        host::Buffer buf = host::Buffer::host_visible(gpu, VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = gpu.family;
        VkCommandPool pool;
        vkCreateCommandPool(gpu.device, &pci, nullptr, &pool);
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmdbuf;
        vkAllocateCommandBuffers(gpu.device, &cai, &cmdbuf);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmdbuf, &bi);
        VkBufferImageCopy r{};
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        r.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmdbuf, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &r);
        vkEndCommandBuffer(cmdbuf);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmdbuf;
        vkQueueSubmit(gpu.queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(gpu.queue);
        std::vector<uint8_t> px(static_cast<const uint8_t*>(buf.mapped), static_cast<const uint8_t*>(buf.mapped) + size_t(w) * h * 4);
        vkDestroyCommandPool(gpu.device, pool, nullptr);
        buf.destroy(gpu);
        return px;
    }
};

const uint8_t* at(const std::vector<uint8_t>& px, int x, int y) { return &px[size_t(y * 640 + x) * 4]; }
std::string rgb(const uint8_t* p) {
    char s[32];
    std::snprintf(s, sizeof s, "%u,%u,%u", p[0], p[1], p[2]);
    return s;
}
bool near(const uint8_t* p, int r, int g, int b, int tolerance = 2) {
    return std::abs(p[0] - r) <= tolerance && std::abs(p[1] - g) <= tolerance && std::abs(p[2] - b) <= tolerance;
}
// How many pixels in a window are lit.
int lit(const std::vector<uint8_t>& px, int x0, int y0, int x1, int y1) {
    int n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) n += at(px, x, y)[0] > 128;
    return n;
}

bool vulkan_13_device() {
    if (volkInitialize() != VK_SUCCESS || volkGetInstanceVersion() < VK_API_VERSION_1_3) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;
    volkLoadInstance(inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    bool ok = false;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        ok |= p.apiVersion >= VK_API_VERSION_1_3;
    }
    vkDestroyInstance(inst, nullptr);
    return ok;
}

// ---- the scenarios

void sanity(Target& t, Gx& g) {
    quad(g, 0, 0, 640, 528, -0.5f, 200, 40, 90);
    const auto px = t.finish(g);
    check(near(at(px, 320, 264), 200, 40, 90), "a quad's colour reaches the frame buffer (" + rgb(at(px, 320, 264)) + ")");
}

// The copy filter: deflicker weights 16/32/16 (above, the line, below)
// blend the line where white meets black, as the hardware's integer maths:
// the last white line (16*255 + 32*255) / 64 = 191, the first black one
// 16*255 / 64 = 63; lines further away are unchanged.
void copy_filter(Target& t, Gx& g) {
    quad(g, 0, 0, 640, 264, -0.5f, 255, 255, 255);
    g.BP(0x53, 8u | (8u << 6) | (10u << 12) | (12u << 18));
    g.BP(0x54, 10u | (8u << 6) | (8u << 12));
    const auto px = t.finish(g);
    g.BP(0x53, 0x595000), g.BP(0x54, 0x15);  // GXInit's again
    const int a = at(px, 320, 262)[1], b = at(px, 320, 263)[1], c = at(px, 320, 264)[1], d = at(px, 320, 265)[1];
    check(a == 255 && std::abs(b - 191) <= 1 && std::abs(c - 63) <= 1 && d == 0,
          "the copy filter blends the lines around an edge: " + std::to_string(a) + " " + std::to_string(b) + " " +
              std::to_string(c) + " " + std::to_string(d) + " (255 191 63 0)");
}

// dst (255,0,255) op src (255,255,0), per channel on 0/255.
uint8_t logic(unsigned op, uint8_t s, uint8_t d) {
    const unsigned S = s ? 1 : 0, D = d ? 1 : 0;
    const unsigned nS = 1 - S, nD = 1 - D;
    const unsigned table[16] = {0, S & D, S & nD, S, nS & D, D, S ^ D, S | D, 1 - (S | D), 1 - (S ^ D), nD, S | nD, nS, nS | D, 1 - (S & D), 1};
    return table[op] ? 255 : 0;
}
void logic_ops(Target& t, Gx& g, bool exact_only, const char* how) {
    static const char* names[16] = {"clear", "and", "and-reverse", "copy", "and-inverted", "no-op", "xor", "or", "nor",
                                    "equivalent", "invert", "or-reverse", "copy-inverted", "or-inverted", "nand", "set"};
    for (unsigned op = 0; op < 16; ++op) {
        // The approximation is exact for these (Dolphin's list), and for xor, and and or on 0/255.
        const bool exact = op == 0 || op == 3 || op == 5 || op == 10 || op == 12 || op == 15 || op == 1 || op == 6 || op == 7;
        if (exact_only && !exact) continue;
        quad(g, 0, 0, 640, 528, -0.5f, 255, 0, 255);
        g.BP(0x41, (1u << 1) | (1u << 3) | (op << 12));  // logic op on, colour updates
        quad(g, 0, 0, 640, 528, -0.5f, 255, 255, 0);
        g.BP(0x41, 1u << 3);
        const auto px = t.finish(g);
        const uint8_t* p = at(px, 320, 264);
        const int r = logic(op, 255, 255), gg = logic(op, 255, 0), b = logic(op, 0, 255);
        check(near(p, r, gg, b), std::string("logic op ") + names[op] + " (" + how + "): " + rgb(p));
    }
}

void lines_and_points(Target& t, Gx& g, bool wide, const char* how) {
    g.BP(0x22, 36u | (60u << 8));                        // lines 6 pixels wide, points 10 pixels
    draw(g, 0xA8, {{100.0f, 200.0f, -0.5f, 255, 255, 255, 255}, {540.0f, 200.0f, -0.5f, 255, 255, 255, 255}});
    draw(g, 0xB8, {{320.0f, 400.0f, -0.5f, 255, 255, 255, 255}});
    g.BP(0x22, 6u | (6u << 8));
    const auto px = t.finish(g);
    const int line = lit(px, 320, 180, 321, 220);   // the line's thickness, down one column
    const int point = lit(px, 300, 380, 340, 420);  // the point's area
    if (wide) {
        check(std::abs(line - 6) <= 1, "a line 6 pixels wide is " + std::to_string(line) + " (" + how + ")");
        check(std::abs(point - 100) <= 21, "a point 10 pixels across covers " + std::to_string(point) + " pixels (" + how + ")");
    } else {
        check(line == 1 && point == 1, std::string("without wide lines and large points: 1 pixel each (") + how + "), got " +
                                           std::to_string(line) + " and " + std::to_string(point));
    }
}

// Triangle strips and lists are drawn as Vulkan strips and lists, without
// indices: a strip's alternate triangles must keep GX's winding (with culling
// on, all of the strip shows, or none), and a list's triangles all show.
void strips_and_lists(Target& t, Gx& g) {
    const uint32_t genmode = g.bp[0x00];
    int shown[3] = {};
    for (unsigned cull = 0; cull < 3; ++cull) {  // none, back, front
        g.BP(0x00, (genmode & ~(3u << 14)) | (cull << 14));
        draw(g, 0x98, {{100, 100, -0.5f, 255, 255, 255, 255}, {100, 300, -0.5f, 255, 255, 255, 255},
                       {200, 100, -0.5f, 255, 255, 255, 255}, {200, 300, -0.5f, 255, 255, 255, 255},
                       {300, 100, -0.5f, 255, 255, 255, 255}, {300, 300, -0.5f, 255, 255, 255, 255}});
        const auto px = t.finish(g);
        shown[cull] = lit(px, 100, 100, 300, 300);
    }
    g.BP(0x00, genmode);
    const int all = 200 * 200;
    check(std::abs(shown[0] - all) <= 400, "a triangle strip covers its band: " + std::to_string(shown[0]) + " of " + std::to_string(all));
    const bool one_way = (std::abs(shown[1] - all) <= 400 && shown[2] == 0) || (std::abs(shown[2] - all) <= 400 && shown[1] == 0);
    check(one_way, "culling keeps or drops a whole strip (its triangles wind alike): back " + std::to_string(shown[1]) + ", front " +
                       std::to_string(shown[2]));
    draw(g, 0x90, {{100, 100, -0.5f, 255, 255, 255, 255}, {100, 300, -0.5f, 255, 255, 255, 255}, {300, 100, -0.5f, 255, 255, 255, 255},
                   {300, 100, -0.5f, 255, 255, 255, 255}, {100, 300, -0.5f, 255, 255, 255, 255}, {300, 300, -0.5f, 255, 255, 255, 255}});
    const auto px = t.finish(g);
    const int list = lit(px, 100, 100, 300, 300);
    check(std::abs(list - all) <= 400, "a triangle list covers its square: " + std::to_string(list) + " of " + std::to_string(all));
}

// A texture of one I8 value, 8x4 (one tile), in guest memory.
void i8_texture(uint32_t phys, uint8_t value) { std::memset(rt::guest_ptr(0x80000000u | phys), value, 32); }

void z_textures(Target& t, Gx& g) {
    for (int early = 0; early < 2; ++early) {
        i8_texture(kTexI8, 0x10);
        g.texture(kTexI8, 8, 4, 1);                       // I8: alpha 0x10
        g.color_from(10, 5);                              // draw the vertex colour; the texture is sampled all the same
        g.BP(0x43, early ? (1u << 6) : 0u);
        g.BP(0xF4, 0x100000), g.BP(0xF5, 0u | (2u << 2));  // Z texture: u8, replace: z = 0x100010, near
        quad(g, 0, 0, 640, 528, -0.5f, 0, 255, 0, true);   // green, at mid depth by its vertices
        g.BP(0xF5, 0), g.BP(0x43, 0);
        g.no_texture();
        quad(g, 0, 0, 640, 528, -0.5f, 255, 0, 0);         // red at mid depth, LEQUAL: hidden only if the green wrote 0x100010
        const auto px = t.finish(g);
        const uint8_t* p = at(px, 320, 264);
        if (!early) check(near(p, 0, 255, 0), "a Z texture (replace, Z after texturing) writes depth: " + rgb(p));
        else check(near(p, 255, 0, 0), "a Z texture with the Z test before texturing leaves depth alone: " + rgb(p));
    }
}

void depth_copy(Target& t, Gx& g) {
    // Depth 0.3 of the way: 0x4CCCCC, so Z8 (the high byte) is 0x4C and Z8M (the
    // middle byte) 0xCC. Z8M is what shows the bytes are split: reading the
    // depth image as a colour gets the high byte about right by itself.
    const uint32_t z = uint32_t(0.3 * 16777215.0);
    struct Case { uint32_t fmt4; const char* name; int want; };
    const Case cases[] = {{1, "Z8 (the high byte)", int(z >> 16)}, {3, "Z8M (the middle byte)", int((z >> 8) & 255)}};
    for (const Case& k : cases) {
        quad(g, 0, 0, 640, 528, -0.3f, 0, 0, 0);
        g.BP(0x43, 3);                                    // Z24: the copy reads depth
        g.BP(0x49, 0), g.BP(0x4A, 63u | (63u << 10));
        g.BP(0x4B, kCopyTex >> 5), g.BP(0x4D, 64 / 32);
        const uint32_t cmd = k.fmt4 << 3;                 // texture copy: R8 (8) or G8 (9)
        g.BP(0x52, cmd);
        gx::record_copy(g.regs(), cmd);
        g.BP(0x43, 0);
        g.texture(kCopyTex, 64, 64, 1);                   // sampled as I8
        g.color_from(8, 4);                               // texture colour
        quad(g, 0, 0, 640, 528, -0.1f, 255, 255, 255, true);
        g.no_texture();
        g.color_from(10, 5);
        const auto px = t.finish(g);
        const uint8_t* p = at(px, 320, 264);
        char want[8];
        std::snprintf(want, sizeof want, "0x%02X", k.want);
        check(near(p, k.want, k.want, k.want, 2), std::string("an EFB copy of depth as ") + k.name + " is " + want + ": " + rgb(p));
    }
}

void rg8_gb8(Target& t, Gx& g) {
    for (int gb = 0; gb < 2; ++gb) {
        quad(g, 0, 0, 640, 528, -0.5f, 0x40, 0x80, 0xC0);
        g.BP(0x49, 0), g.BP(0x4A, 63u | (63u << 10));
        g.BP(0x4B, kCopyTex >> 5), g.BP(0x4D, 64 / 16);
        const uint32_t fmt4 = gb ? 9u : 7u;               // RG8 = 11, GB8 = 12: fmt/2 + (fmt & 1) * 8
        const uint32_t cmd = fmt4 << 3;
        g.BP(0x52, cmd);
        gx::record_copy(g.regs(), cmd);
        g.texture(kCopyTex, 64, 64, 3);                   // sampled as IA8
        g.color_from(8, 4);
        quad(g, 0, 0, 640, 528, -0.25f, 255, 255, 255, true);
        g.no_texture();
        g.color_from(10, 5);
        const auto px = t.finish(g);
        const uint8_t* p = at(px, 320, 264);
        const int want = gb ? 0x80 : 0x40;
        check(near(p, want, want, want), std::string(gb ? "GB8" : "RG8") + " copies sample with intensity from " +
                                             (gb ? "green" : "red") + ": " + rgb(p));
    }
}

// ---- through the command decoder (gpu.cpp): commands as bytes, as the game's
// GX and J3D write them to the pipe.
struct Stream {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint32_t v) { b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v)); }
    void u32(uint32_t v) { put_be32(b, v); }
    void bp(uint32_t reg, uint32_t v) { u8(0x61), u32((reg << 24) | (v & 0xFFFFFFu)); }
    void cp(uint32_t reg, uint32_t v) { u8(0x08), u8(uint8_t(reg)), u32(v); }
    void xf(uint32_t address, std::initializer_list<uint32_t> words) {
        u8(0x10), u32((uint32_t(words.size() - 1) << 16) | address);
        for (uint32_t w : words) u32(w);
    }
    void xff(uint32_t address, std::initializer_list<float> words) {
        u8(0x10), u32((uint32_t(words.size() - 1) << 16) | address);
        for (float w : words) u32(f2u(w));
    }
    void call(uint32_t physical, uint32_t bytes) { u8(0x40), u32(physical), u32(bytes); }
    // A position matrix load with its tag and view, as mtxtag.cpp's loaders make them.
    static inline uint64_t sent = 0;  // bytes given to the decoder so far, by every Stream
    void tagged(uint32_t address, const float mv[12], uint32_t tag, const float view[12]) {
        mkdd::tag_matrix_at(sent + b.size(), tag, view);
        u8(0x10), u32((11u << 16) | address);
        for (int i = 0; i < 12; ++i) u32(f2u(mv[i]));
    }
    // The viewport and scissor of a w x h rectangle at (x, y), with an orthographic projection over it in pixels.
    void port(float x, float y, float w, float h) {
        xff(0x101A, {w / 2, -h / 2, 16777215.0f, 342.0f + x + w / 2, 342.0f + y + h / 2, 16777215.0f});
        xff(0x1020, {2.0f / w, -1.0f, 2.0f / -h, 1.0f, -1.0f, -1.0f});
        xf(0x1026, {1});
        bp(0x20, (342u + uint32_t(y)) | ((342u + uint32_t(x)) << 12));
        bp(0x21, (342u + uint32_t(y + h) - 1) | ((342u + uint32_t(x + w) - 1) << 12));
        bp(0x59, (342u >> 1) | ((342u >> 1) << 10));
    }
    // Gx::setup's state, as commands.
    void setup() {
        bp(0x00, 1u << 4);
        port(0, 0, 640, 528);
        xff(0x0000, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});            // position matrix 0: identity
        xff(30 * 4, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0});            // texture matrix 30: identity
        cp(0x30, 30u << 6), cp(0x40, 0);
        xf(0x1009, {1}), xf(0x100E, {1}), xf(0x1010, {1}), xf(0x103F, {0});
        bp(0x28, 0);
        bp(0xC0, 10u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));
        bp(0xC1, (5u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19));
        for (uint32_t s = 0; s < 4; ++s) bp(0xF6 + 2 * s, 0 | (1u << 2)), bp(0xF7 + 2 * s, 2 | (3u << 2));
        bp(0xF3, (7u << 16) | (7u << 19));
        bp(0x40, 1u | (3u << 1) | (1u << 4));
        bp(0x41, 1u << 3);
        bp(0x43, 0);
        bp(0x53, 0x595000), bp(0x54, 0x15);
        bp(0x22, 6u | (6u << 8));
    }
    // Texture map 0 on stage 0 from texture coordinate 0, colour = texture x rasterised.
    void texture(uint32_t address, uint32_t w, uint32_t h, uint32_t format) {
        bp(0x80, 0), bp(0x84, 0);
        bp(0x88, (w - 1) | ((h - 1) << 10) | (format << 20));
        bp(0x94, address >> 5);
        bp(0x30, w - 1), bp(0x31, h - 1);
        xf(0x103F, {1}), xf(0x1040, {5u << 7});
        bp(0x28, 1u << 6);
        bp(0xC0, 15u | (8u << 4) | (10u << 8) | (15u << 12) | (1u << 19));   // a 0, b texc, c rasc, d 0
    }
    // The copy of the w x h EFB to the frame buffer, clearing: the frame's end.
    void end_frame(uint32_t w = 640, uint32_t h = 528) {
        bp(0x4F, 0), bp(0x50, 0), bp(0x51, 0xFFFFFF);
        bp(0x49, 0), bp(0x4A, (w - 1) | ((h - 1) << 10));
        bp(0x4B, kXfb >> 5), bp(0x4D, w / 16), bp(0x4E, 256);
        bp(0x52, (1u << 14) | (1u << 11));
    }
    void send() {
        mkdd::decode_commands(b.data(), b.size());
        sent += b.size();
        b.clear();
    }
};

// A frame shaped like one of this game's races, from what its models hold (the
// disc's course and kart models: every attribute by a 16-bit index, positions
// as floats, normals and texture coordinates as 16-bit fractions, triangle
// strips of five vertices on average, a display list a shape): per player a
// course of 20 shapes and 25,000 vertices and 48 kart and object shapes of
// 300, each shape with its own material and matrices.
struct RaceFrame {
    static constexpr uint32_t kLists = 0x00800000u, kPos = 0x00A00000u, kNrm = 0x00A40000u, kCol = 0x00A80000u, kUv = 0x00AC0000u,
                              kTex = 0x00B00000u;
    static constexpr uint32_t kElements = 4096, kTextures = 16;
    struct List {
        uint32_t at, bytes;
    };
    std::vector<List> course, karts;
    uint32_t next = kLists;
    uint8_t* mem(uint32_t physical) { return rt::guest_ptr(0x80000000u | physical); }
    void be32(uint32_t physical, uint32_t v) {
        uint8_t* p = mem(physical);
        p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    }
    // A display list of `strips` strips of `each` vertices, three 16-bit indices a vertex.
    List list(uint32_t strips, uint32_t each, uint32_t seed) {
        const uint32_t at = next;
        uint8_t* p = mem(at);
        uint32_t n = 0, x = seed * 2654435761u + 1;
        for (uint32_t s = 0; s < strips; ++s) {
            p[n++] = 0x98, p[n++] = uint8_t(each >> 8), p[n++] = uint8_t(each);
            x = x * 1664525u + 1013904223u;
            const uint32_t base = (x >> 8) % (kElements - 2 * each);
            for (uint32_t v = 0; v < each; ++v)
                for (int a = 0; a < 3; ++a) p[n++] = uint8_t((base + v) >> 8), p[n++] = uint8_t(base + v);
        }
        while (n % 32) p[n++] = 0;
        next += n;
        return {at, n};
    }
    explicit RaceFrame(uint32_t each = 5) {
        // The arrays: positions in a grid of small triangles over the picture, so strips cover a few pixels each.
        for (uint32_t i = 0; i < kElements; ++i) {
            const float x = float((i * 37u) % 600u) + ((i & 1) ? 6.0f : 0.0f), y = float((i * 91u) % 440u) + ((i & 2) ? 6.0f : 0.0f);
            be32(kPos + 12 * i, f2u(x)), be32(kPos + 12 * i + 4, f2u(y)), be32(kPos + 12 * i + 8, f2u(-0.5f - float(i % 100) / 400.0f));
            uint8_t* nr = mem(kNrm + 6 * i);
            nr[0] = 0, nr[1] = 0, nr[2] = 0, nr[3] = 0, nr[4] = 0x40, nr[5] = 0;
            be32(kCol + 4 * i, 0x80C0FFFFu ^ (i * 0x01010100u));
            uint8_t* uv = mem(kUv + 4 * i);
            uv[0] = uint8_t(i >> 4), uv[1] = uint8_t(i << 4), uv[2] = uint8_t(i >> 6), uv[3] = uint8_t(i << 2);
        }
        for (uint32_t t = 0; t < kTextures; ++t)
            for (uint32_t i = 0; i < 64 * 64; ++i) mem(kTex + t * 4096)[i] = uint8_t(i * (t + 3) + (i >> 6) * 5);
        for (uint32_t s = 0; s < 20; ++s) course.push_back(list(1250 / each, each, s));
        for (uint32_t s = 0; s < 48; ++s) karts.push_back(list(300 / each, each, 100 + s));
    }
    // One player's view into `rect` (x, y, w, h).
    void view(Stream& st, const float rect[4], uint32_t frame) {
        st.port(rect[0], rect[1], rect[2], rect[3]);
        // A perspective projection that lands the arrays' pixel coordinates where the orthographic
        // one did, for points ten units ahead (flat drawing isn't blended between frames).
        st.xff(0x1020, {20.0f / rect[2], 1.0f, -20.0f / rect[3], -1.0f, -1.0f / 99.0f, -100.0f / 99.0f});
        st.xf(0x1026, {0});
        const float camera[12] = {1, 0, 0, rect[0] + 0.25f * float(frame % 4), 0, 1, 0, rect[1], 0, 0, 1, 0};
        st.cp(0xA0, kPos), st.cp(0xB0, 12), st.cp(0xA1, kNrm), st.cp(0xB1, 6), st.cp(0xA2, kCol), st.cp(0xB2, 4);
        st.cp(0xA4, kUv), st.cp(0xB4, 4);
        const float sx = rect[2] / 608.0f, sy = rect[3] / 448.0f;
        uint32_t material = 0;
        const auto shape = [&](const List& l, bool lit) {
            // The material: its texture, a konstant and a TEV colour; then the shape's matrices and format.
            st.texture(kTex + (material % kTextures) * 4096, 64, 64, 1);
            st.bp(0xE0, (material * 7) & 0xFF), st.bp(0xE1, (material * 13) & 0xFF);
            st.bp(0xF3, (7u << 16) | (7u << 19) | (material & 0xFF));
            const float mv[12] = {sx, 0, 0, float(material % 8 + frame % 4), 0, sy, 0, float(material % 5), 0, 0, 1, -10};
            st.tagged(0x0000, mv, 0x80300000u + 64 * material, camera);
            st.xff(0x0400, {1, 0, 0, 0, 1, 0, 0, 0, 1});
            st.cp(0x50, lit ? (3u << 9) | (3u << 11) : (3u << 9) | (3u << 13));
            st.cp(0x60, 3u);
            st.cp(0x70, 1u | (4u << 1) | (3u << 10) | (1u << 13) | (5u << 14) | (1u << 21) | (3u << 22) | (8u << 25));
            st.call(l.at, l.bytes);
            ++material;
        };
        for (const List& l : course) shape(l, false);
        for (const List& l : karts) shape(l, true);
    }
    void frame(Stream& st, unsigned players, uint32_t n) {
        static constexpr float kRects[3][4][4] = {{{0, 0, 608, 448}},
                                                  {{0, 0, 608, 224}, {0, 224, 608, 224}},
                                                  {{0, 0, 304, 224}, {304, 0, 304, 224}, {0, 224, 304, 224}, {304, 224, 304, 224}}};
        const unsigned layout = players == 1 ? 0 : players == 2 ? 1 : 2;
        st.bp(0x66, 0);  // GXInvalidateTexAll, as the game's frames begin
        for (unsigned p = 0; p < players; ++p) view(st, kRects[layout][p], n);
        st.end_frame(608, 448);
    }
};

double median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// ---- scenarios through the decoder: what this game's code and models do (docs/graphics.md)

// Direct vertices as Gx's draw(): position xyz f32, colour RGBA8, and with `tex` s,t f32.
void stream_format(Stream& st, bool tex) {
    st.cp(0x50, (1u << 9) | (1u << 13)), st.cp(0x60, tex ? 1u : 0u);
    st.cp(0x70, 1u | (4u << 1) | (5u << 14) | (1u << 21) | (4u << 22));
}
void stream_quad(Stream& st, float x0, float y0, float x1, float y1, float z, uint32_t rgba, bool tex = false, float s1 = 1, float t1 = 1) {
    stream_format(st, tex);
    st.u8(0x80), st.u16(4);
    const float xy[4][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}, uv[4][2] = {{0, 0}, {s1, 0}, {s1, t1}, {0, t1}};
    for (int i = 0; i < 4; ++i) {
        st.u32(f2u(xy[i][0])), st.u32(f2u(xy[i][1])), st.u32(f2u(z)), st.u32(rgba);
        if (tex) st.u32(f2u(uv[i][0])), st.u32(f2u(uv[i][1]));
    }
}
// Every scenario starts from setup()'s state, whatever the one before left.
void stream_begin(Stream& st) {
    gx::state_changed();
    st.setup();
    st.xf(0x103F, {0});
    st.bp(0xF4, 0), st.bp(0xF5, 0), st.bp(0x42, 0);
}
uint8_t* guest(uint32_t physical) { return rt::guest_ptr(0x80000000u | physical); }

// A model's display list as J3D's are: strips, a fan and quads of 16-bit
// indices, one after another with no state between them. They become one
// draw, and with culling on show whole or not at all (a joined strip's
// triangles wind as GX's).
void joined_primitives(Target& t, Stream& st) {
    constexpr uint32_t kList = 0x00900000u, kPos = 0x00910000u, kCol = 0x00920000u;
    // A grid of points from (20, 100), 50 apart across and 100 down: column c, row r is element 3 * c + r.
    for (uint32_t c = 0; c < 12; ++c)
        for (uint32_t r = 0; r < 3; ++r) {
            const float p[3] = {20.0f + 50.0f * float(c), 100.0f + 100.0f * float(r), -0.5f};
            for (int k = 0; k < 3; ++k) {
                const uint32_t v = f2u(p[k]);
                uint8_t* m = guest(kPos + 12 * (3 * c + r) + 4 * k);
                m[0] = uint8_t(v >> 24), m[1] = uint8_t(v >> 16), m[2] = uint8_t(v >> 8), m[3] = uint8_t(v);
            }
        }
    std::memset(guest(kCol), 255, 4);
    std::vector<uint8_t> dl;
    const auto prim = [&](uint8_t op, std::initializer_list<uint32_t> elements) {
        dl.push_back(op), dl.push_back(0), dl.push_back(uint8_t(elements.size()));
        for (uint32_t e : elements) dl.insert(dl.end(), {uint8_t(e >> 8), uint8_t(e), 0, 0});  // position, colour 0
    };
    // Rows 0-1, columns 0-4 as two strips; columns 4-6 as a fan; columns 6-8 as two quads; then
    // rows 1-2, columns 0-2 as a triangle list. All wound alike (clockwise on screen).
    prim(0x98, {0, 1, 3, 4, 6, 7});
    prim(0x98, {6, 7, 9, 10, 12, 13});
    prim(0xA0, {12, 13, 16, 19, 18, 15});
    prim(0x80, {18, 19, 22, 21, 21, 22, 25, 24});
    prim(0x90, {1, 2, 4, 4, 2, 5, 4, 5, 7, 7, 5, 8});
    while (dl.size() % 32) dl.push_back(0);
    std::memcpy(guest(kList), dl.data(), dl.size());
    int shown[3] = {};
    size_t draws = 0;
    for (unsigned cull = 0; cull < 3; ++cull) {
        stream_begin(st);
        st.bp(0x00, (1u << 4) | (cull << 14));
        st.cp(0x50, (3u << 9) | (3u << 13)), st.cp(0x60, 0), st.cp(0x70, 1u | (4u << 1) | (5u << 14));
        st.cp(0xA0, kPos), st.cp(0xB0, 12), st.cp(0xA2, kCol), st.cp(0xB2, 4);
        st.call(kList, uint32_t(dl.size()));
        st.end_frame();
        st.send();
        const auto px = t.shown();
        draws = t.last_draws;
        shown[cull] = lit(px, 20, 100, 420, 200) + lit(px, 20, 200, 120, 300);
    }
    const int all = 400 * 100 + 100 * 100;
    check(draws == 1, "a display list's primitives with one state are one draw: " + std::to_string(draws));
    check(std::abs(shown[0] - all) <= 600, "strips, a fan, quads and a list from 16-bit indices cover their area: " + std::to_string(shown[0]) +
                                               " of " + std::to_string(all));
    const bool one_way = (std::abs(shown[1] - all) <= 600 && shown[2] == 0) || (std::abs(shown[2] - all) <= 600 && shown[1] == 0);
    check(one_way, "culling keeps or drops all of the joined primitives: back " + std::to_string(shown[1]) + ", front " +
                       std::to_string(shown[2]));
}

// JFWDisplay::clearEfb, each frame of this game: a quad with a 4x4 Z24X8 texture of 0x00FFFFFF as a
// Z texture (replace), the Z test after texturing and always passing, a constant destination alpha.
// It must reset the depth to far whatever its own vertices' depth is.
void clear_by_z_texture(Target& t, Stream& st) {
    constexpr uint32_t kZ = 0x00930000u;
    for (uint32_t i = 0; i < 64; ++i) guest(kZ)[i] = (i % 4) ? 0xFF : 0x00;
    stream_begin(st);
    st.bp(0x43, 1), st.bp(0x41, (1u << 3) | (1u << 4));           // RGBA6_Z24, colour and alpha updates
    stream_quad(st, 0, 0, 640, 528, -0.01f, 0xFF0000FFu);         // red, nearly at the near plane
    st.texture(kZ, 4, 4, 6);
    st.bp(0xC0, 10u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));  // the rasterised colour all the same
    st.bp(0xF4, 0), st.bp(0xF5, 2u | (2u << 2));                  // Z texture: 24 bits, replace
    st.bp(0x40, 1u | (7u << 1) | (1u << 4));                      // Z: always, updates
    st.bp(0x42, 0x40u | (1u << 8));                               // destination alpha 0x40
    stream_quad(st, 0, 0, 640, 528, -0.01f, 0x000000FFu, true);   // the clear: black
    st.bp(0xF5, 0), st.bp(0x42, 0), st.bp(0x40, 1u | (3u << 1) | (1u << 4));
    st.xf(0x103F, {0}), st.bp(0x28, 0);
    stream_quad(st, 0, 0, 320, 528, -0.9f, 0x00FF00FFu);          // green, far: shows only if the depth was reset
    // The EFB's alpha, as this game's shadows and ghosts read it: an A8 copy at half scale, drawn as a texture.
    st.bp(0x49, 0), st.bp(0x4A, 63u | (63u << 10)), st.bp(0x4B, kCopyTex >> 5), st.bp(0x4D, 32 / 32);
    st.bp(0x49, 400u | (100u << 10));
    st.bp(0x52, (14u << 3) | (1u << 9));
    st.texture(kCopyTex, 32, 32, 1);
    st.bp(0xC0, 8u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));
    stream_quad(st, 400, 300, 500, 400, -0.95f, 0xFFFFFFFFu, true);
    st.end_frame();
    st.send();
    const auto px = t.shown();
    check(near(at(px, 100, 264), 0, 255, 0), "the game's EFB clear (a Z24X8 Z texture, replace, Z always) resets the depth: " + rgb(at(px, 100, 264)));
    check(near(at(px, 350, 100), 0, 0, 0), "... and writes its colour: " + rgb(at(px, 350, 100)));
    // 0x40 in six bits is 16, which reads back as 0x41.
    check(near(at(px, 450, 350), 0x41, 0x41, 0x41, 3), "a constant destination alpha reaches an A8 copy at half scale (0x41): " + rgb(at(px, 450, 350)));
}

// Depth copies with the game's deflicker weights set, as they are all frame
// (half the line above, half the line): Z16 at half scale, sampled as IA8
// (the heat shimmer), and 2x2 pixels as Z24X8 that the CPU reads (the
// players' marks, the lens flare). The top of the picture is at depth
// 0x3FFFFF, the rest at 0x4CCCCC; just below the edge a copy must hold the
// lower depth's own bytes, not a blend.
void depth_copies(Target& t, Stream& st) {
    constexpr uint32_t kZTile = 0x00940000u;
    std::memset(guest(kZTile), 0xFF, 64);
    stream_begin(st);
    st.bp(0x53, 32u | (32u << 12)), st.bp(0x54, 0);
    stream_quad(st, 0, 0, 640, 200, -0.25f, 0x000000FFu);
    stream_quad(st, 0, 200, 640, 528, -0.3f, 0x000000FFu);
    st.bp(0x43, 3);                                               // as GXCopyTex does for a Z format
    st.bp(0x49, 0u | (200u << 10)), st.bp(0x4A, 63u | (63u << 10)), st.bp(0x4B, kCopyTex >> 5), st.bp(0x4D, 32 / 16);
    st.bp(0x52, (6u << 3) | (1u << 9));                           // Z16, half scale
    st.bp(0x49, 100u | (200u << 10)), st.bp(0x4A, 1u | (1u << 10)), st.bp(0x4B, kZTile >> 5), st.bp(0x4D, 1);
    st.bp(0x52, 12u << 3);                                        // Z24X8, 2x2
    st.bp(0x43, 0);
    st.texture(kCopyTex, 32, 32, 3);
    st.bp(0xC0, 8u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));  // the texture's colour
    stream_quad(st, 0, 0, 320, 528, -0.1f, 0xFFFFFFFFu, true, 1.0f, 1.0f / 32.0f);  // its first line
    st.bp(0xC0, 9u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));  // the texture's alpha
    stream_quad(st, 320, 0, 640, 528, -0.1f, 0xFFFFFFFFu, true, 1.0f, 1.0f / 32.0f);
    st.end_frame();
    st.send();
    const auto px = t.shown();
    t.renderer.finish_copies();
    const uint8_t* i = at(px, 160, 264), *a = at(px, 480, 264);
    check(near(i, 0xCC, 0xCC, 0xCC, 1), "a Z16 copy's intensity is the depth's middle byte, unfiltered (204): " + rgb(i));
    check(near(a, 0x4C, 0x4C, 0x4C, 1), "... and its alpha the high byte (76): " + rgb(a));
    const uint8_t* m = guest(kZTile);
    char got[40];
    std::snprintf(got, sizeof got, "%02X %02X %02X", m[1], m[32], m[33]);
    check(m[1] == 0x4C && m[32] == 0xCC && std::abs(int(m[33]) - 0xCC) <= 1 && m[3] == 0x4C && m[9] == 0x4C && m[4] == 0xFF,
          std::string("a 2x2 Z24X8 copy reaches guest memory for the CPU (4C CC CC): ") + got);
}

// Four players: each view's viewport and scissor take a quarter of the 608x448
// picture, and a quad larger than the view stays inside it.
void split_screen(Target& t, Stream& st) {
    stream_begin(st);
    static constexpr uint32_t kColors[4] = {0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFF00FFu};
    for (unsigned p = 0; p < 4; ++p) {
        st.port(float(p % 2) * 304.0f, float(p / 2) * 224.0f, 304, 224);
        stream_quad(st, -50, -50, 400, 300, -0.5f, kColors[p]);
    }
    st.end_frame();
    st.send();
    const auto px = t.shown();
    const bool ok = near(at(px, 303, 223), 255, 0, 0) && near(at(px, 304, 223), 0, 255, 0) && near(at(px, 303, 224), 0, 0, 255) &&
                    near(at(px, 304, 224), 255, 255, 0) && near(at(px, 0, 0), 255, 0, 0) && near(at(px, 607, 447), 255, 255, 0) &&
                    near(at(px, 608, 100), 0, 0, 0) && near(at(px, 100, 448), 0, 0, 0);
    check(ok, "four views meet at the middle of 608x448 and stay inside it: " + rgb(at(px, 303, 223)) + " | " + rgb(at(px, 304, 223)) +
                  " | " + rgb(at(px, 303, 224)) + " | " + rgb(at(px, 304, 224)));
}

// J3D loads a shape's matrices by index from its own arrays (commands 0x20 and
// 0x28, which the SDK's API never writes here): a position matrix that moves
// the quad 200 to the right.
void indexed_matrices(Target& t, Stream& st) {
    constexpr uint32_t kMatrices = 0x00950000u;
    const float m[12] = {1, 0, 0, 200, 0, 1, 0, 0, 0, 0, 1, 0};
    for (int i = 0; i < 12; ++i) {
        const uint32_t v = f2u(m[i]);
        uint8_t* p = guest(kMatrices + 48 + 4 * uint32_t(i));     // the array's element 1
        p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
    }
    stream_begin(st);
    st.cp(0xAC, kMatrices), st.cp(0xBC, 48);
    st.u8(0x20), st.u16(1), st.u16(0xB000u | 12u);                // element 1, twelve words to XF 0x00C: matrix 1
    st.cp(0x30, 3u | (30u << 6));                                 // position matrix at row 3
    stream_quad(st, 0, 100, 100, 200, -0.5f, 0xFFFFFFFFu);
    st.cp(0x30, 30u << 6);
    st.end_frame();
    st.send();
    const auto px = t.shown();
    check(lit(px, 200, 100, 300, 200) == 10000 && lit(px, 0, 100, 200, 200) == 0,
          "a position matrix loaded by index from an array moves the quad: " + std::to_string(lit(px, 200, 100, 300, 200)) + " lit of 10000");
}

// GXSetZCompLoc: with Z compared before texturing a pixel the alpha test drops
// has written its depth all the same (24 of this game's alpha-tested materials
// are set so); compared after, it hasn't. A green quad that fails its alpha
// test, nearer than the red one drawn next.
void z_before_alpha_test(Target& t, Stream& st) {
    int red[2] = {};
    for (int before = 0; before < 2; ++before) {
        stream_begin(st);
        st.bp(0x43, before ? 1u << 6 : 0u);
        st.bp(0xF3, (0u << 16) | (7u << 19));                      // alpha test: never and always
        stream_quad(st, 0, 0, 640, 528, -0.2f, 0x00FF00FFu);
        st.bp(0xF3, (7u << 16) | (7u << 19));
        stream_quad(st, 0, 0, 640, 528, -0.5f, 0xFF0000FFu);
        st.end_frame();
        st.send();
        const auto px = t.shown();
        red[before] = at(px, 320, 264)[0];
        if (at(px, 320, 264)[1] != 0) red[before] = -1;           // the green was drawn: the alpha test didn't drop it
    }
    check(red[0] == 255 && red[1] == 0, "a pixel dropped by the alpha test writes depth only with Z before texturing: red behind it " +
                                            std::to_string(red[0]) + " after, " + std::to_string(red[1]) + " before");
}

// A texture's LOD bias (a fifth of this game's course textures have one): drawn one texel a
// pixel, a bias of +1 takes the second level (black here; the first is white).
void lod_bias(Target& t, Stream& st) {
    constexpr uint32_t kMip = 0x00960000u;
    std::memset(guest(kMip), 0xFF, 256), std::memset(guest(kMip + 256), 0x00, 64);  // I8 16x16, then 8x8
    int got[2] = {};
    for (int biased = 0; biased < 2; ++biased) {
        stream_begin(st);
        st.texture(kMip, 16, 16, 1);
        st.bp(0x80, 1u | (1u << 2) | (1u << 5) | (biased ? 32u << 9 : 0u));  // repeat; the nearest level, nearest texel
        st.bp(0x84, 16u << 8);                                            // levels 0 to 1
        st.bp(0xC0, 8u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));
        stream_quad(st, 0, 0, 320, 320, -0.5f, 0xFFFFFFFFu, true, 20.0f, 20.0f);
        st.end_frame();
        st.send();
        got[biased] = at(t.shown(), 160, 160)[0];
    }
    check(got[0] == 255 && got[1] == 0, "a LOD bias of +1 samples the next mip level: " + std::to_string(got[0]) + " without, " +
                                            std::to_string(got[1]) + " with");
}

// ---- pictures between frames (interp.hpp)

// A perspective projection for the scenes below: x and y of a point ten units ahead reach the
// picture's edge at 5; near 1, far 100.
void perspective(Stream& st) {
    st.xff(0x1020, {2.0f, 0.0f, 2.0f, 0.0f, -1.0f / 99.0f, -100.0f / 99.0f});
    st.xf(0x1026, {0});
}
struct Mat {
    float m[12];
};
Mat camera_at(float x) { return {{1, 0, 0, -x, 0, 1, 0, 0, 0, 0, 1, 0}}; }
// A model turned `degrees` about the axis towards the camera, at (x, y) ten units ahead.
Mat model_at(float x, float y, float degrees = 0) {
    const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    return {{c, -s, 0, x, s, c, 0, y, 0, 0, 1, -10}};
}
// A white rectangle (half sizes w, h) drawn with the model matrix under the view, loaded with `tag`.
void object(Stream& st, const Mat& view, const Mat& model, uint32_t tag, float w = 0.25f, float h = 0.25f) {
    Mat mv = model;
    for (int r = 0; r < 3; ++r) mv.m[4 * r + 3] += view.m[4 * r + 3];  // the cameras here only move
    st.tagged(0, mv.m, tag, view.m);
    stream_quad(st, -w, h, w, -h, 0, 0xFFFFFFFFu);
}
// The middle of what is lit in a window, and its extent.
struct Blob {
    double x = 0, y = 0;
    int n = 0, w = 0, h = 0;
};
Blob blob(const std::vector<uint8_t>& px, int x0, int y0, int x1, int y1) {
    Blob b;
    int lx = 1 << 20, hx = -1, ly = 1 << 20, hy = -1;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (at(px, x, y)[0] > 128) b.x += x + 0.5, b.y += y + 0.5, ++b.n, lx = std::min(lx, x), hx = std::max(hx, x), ly = std::min(ly, y), hy = std::max(hy, y);
    if (b.n) b.x /= b.n, b.y /= b.n, b.w = hx - lx + 1, b.h = hy - ly + 1;
    return b;
}
std::string num(double v) {
    char s[32];
    std::snprintf(s, sizeof s, "%.1f", v);
    return s;
}

// Two frames, the camera moving 0.4 to the right between them: an object that
// moves, one that turns 170 degrees, one that jumps, one only in the newer
// frame and one only in the older. Screen x of world x under a camera at e:
// 320 + 64 (x - e).
void between_frames(Target& t, Stream& st) {
    const auto frame = [&](int n) {
        stream_begin(st);
        perspective(st);
        const Mat cam = camera_at(n ? 0.4f : 0.0f);
        object(st, cam, model_at(n ? -1.0f : -2.0f, 3.0f), 0x80100000u);              // moves one unit
        object(st, cam, model_at(2.0f, 3.0f, n ? 170.0f : 0.0f), 0x80100040u, 0.5f, 0.05f);  // a bar, turning
        object(st, cam, model_at(n ? 3.0f : -3.0f, -1.5f), 0x80100080u);              // jumps six units
        if (n) object(st, cam, model_at(0.0f, -3.5f), 0x801000C0u);                   // new in the second frame
        else object(st, cam, model_at(3.0f, -3.5f), 0x80100100u);                     // gone from the second
        st.end_frame();
        st.send();
        return t.shown(true);
    };
    const auto a = frame(0), b = frame(1);
    const auto& pc = t.renderer.pairing();
    check(pc.paired == 2 && pc.jumped == 1 && pc.alone == 2 && pc.cuts == 0,
          "matrices pair by tag: " + std::to_string(pc.paired) + " paired, " + std::to_string(pc.jumped) + " jumped, " +
              std::to_string(pc.alone) + " alone (2, 1, 2)");
    const auto one = t.between(1.0f), zero = t.between(0.0f), half = t.between(0.5f);
    check(!one.empty() && one == b, "the picture at phase 1 is the frame's own, pixel for pixel");
    // At 0.5 the camera is at 0.2. The mover is at -1.5: x = 320 + 64 * -1.7 = 211.2 (the frames: 192 and 230.4).
    const Blob mover = blob(half, 100, 20, 300, 130);
    check(std::abs(mover.x - 211.2) < 1.0 && mover.n > 800, "half way, a moving object is half way: x " + num(mover.x) + " (211.2)");
    // The bar is 64 pixels long: at 85 degrees it stands (blending the matrices' entries would leave a speck).
    const Blob bar = blob(half, 380, 20, 520, 150);
    check(bar.h > 45 && bar.w < 20 && std::abs(bar.x - (320 + 64 * 1.8)) < 1.5, "half way through a half turn, a bar stands on end: " +
                                                                                   std::to_string(bar.w) + " x " + std::to_string(bar.h) + " at x " + num(bar.x));
    // The jumper is where the newer frame has it (3), seen from 0.2: 499.2. Nothing at the place half way.
    const Blob jumper = blob(half, 420, 300, 600, 400), midway = blob(half, 250, 300, 400, 400);
    check(std::abs(jumper.x - 499.2) < 1.0 && midway.n == 0, "an object that jumps isn't shown half way: x " + num(jumper.x) + " (499.2), " +
                                                                 std::to_string(midway.n) + " pixels in between");
    // The new object stays put in the world (0) under the blended camera: 307.2. The one that left is gone.
    const Blob fresh = blob(half, 200, 420, 420, 528), gone = blob(half, 440, 420, 640, 528);
    check(std::abs(fresh.x - 307.2) < 1.0 && gone.n == 0, "an object new in the frame stands still in the world, one gone from it isn't drawn: x " +
                                                              num(fresh.x) + " (307.2), " + std::to_string(gone.n) + " pixels of the other");
    // Phase 0 with everything paired is the frame before (the same draws, its matrices).
    const auto pair = [&](int n) {
        stream_begin(st);
        perspective(st);
        const Mat cam = camera_at(n ? 0.4f : 0.0f);
        object(st, cam, model_at(n ? -1.0f : -2.0f, 3.0f), 0x80100000u);
        object(st, cam, model_at(2.0f, -2.0f, n ? 50.0f : 0.0f), 0x80100040u, 0.5f, 0.05f);
        st.end_frame();
        st.send();
        return t.shown(true);
    };
    const auto a2 = pair(0);
    pair(1);
    check(t.between(0.0f) == a2, "the picture at phase 0 is the frame before, pixel for pixel");
    (void)a, (void)zero;
    // A cut: the camera 30 units away in a tick. No picture between; the caller shows the frame.
    stream_begin(st);
    perspective(st);
    object(st, camera_at(30.4f), model_at(30.0f, 0.0f), 0x80100000u);
    st.end_frame();
    st.send();
    t.shown(true);
    check(t.between(0.5f).empty() && t.renderer.pairing().cuts == 1, "a camera cut gives no picture between");
}

// Four views of one model (one tag, drawn four times): each view's camera is
// blended by itself, and the one that cuts shows the newer frame's view.
void between_four_views(Target& t, Stream& st) {
    static constexpr float kMove[4] = {0.4f, 0.8f, 3.0f, -0.4f};  // each camera's step; the third's is a cut
    const auto frame = [&](int n) {
        stream_begin(st);
        for (unsigned v = 0; v < 4; ++v) {
            st.port(float(v % 2) * 304.0f, float(v / 2) * 224.0f, 304, 224);
            perspective(st);
            object(st, camera_at(0.001f * float(v) + (n ? kMove[v] : 0.0f)), model_at(0.0f, 0.0f), 0x80200000u);  // (no two cameras alike)
        }
        st.end_frame();
        st.send();
        return t.shown(true);
    };
    frame(0), frame(1), frame(0);                    // (each camera has made a step before the one tested)
    frame(1);
    const auto half = t.between(0.5f);
    bool ok = !half.empty();
    std::string got;
    for (unsigned v = 0; ok && v < 4; ++v) {
        const int x0 = int(v % 2) * 304, y0 = int(v / 2) * 224;
        const Blob b = blob(half, x0, y0, x0 + 304, y0 + 224);
        // A view is 304 wide: 30.4 pixels a unit. Half the step, or all of it for the cut.
        const double want = x0 + 152 - 30.4 * (v == 2 ? kMove[v] : kMove[v] / 2);
        ok = ok && b.n > 100 && std::abs(b.x - want) < 1.0;
        got += num(b.x - x0) + " ";
    }
    check(ok && t.renderer.pairing().cuts == 1, "four views blend each by its own camera, and the one that cuts doesn't: x " + got +
                                                    "(145.9 139.8 60.8 158.1)");
}

// mkdd_render_test --bench [players] [frames] [vertices a strip] [pictures between]: what such a frame costs to decode and record,
// and to draw (the renderer's own time on the CPU, and the GPU's where it has timestamps).
int bench(host::Gpu& gpu, unsigned players, unsigned frames, unsigned strip, unsigned pictures) {
    Target t(gpu);
    RaceFrame race(strip);
    Stream st;
    st.setup();
    st.bp(0x43, 1), st.bp(0x41, (1u << 3) | (1u << 4));  // RGBA6_Z24, colour and alpha updates
    st.send();
    std::vector<double> record_ms, render_ms, between_ms;
    size_t paired = 0;
    size_t draws = 0, vertices = 0, states = 0, bytes = 0;
    auto& stats = gx::recorder_stats();
    uint64_t gpu0 = 0, timed0 = 0;
    for (unsigned n = 0; n < frames + 10; ++n) {
        if (n == 10) gpu0 = stats.gpu_ns.load(), timed0 = stats.gpu_timed.load();  // ten to warm up (pipelines, caches)
        race.frame(st, players, n);
        bytes = st.b.size();
        const uint64_t t0 = gx::now_ns();
        st.send();
        const uint64_t t1 = gx::now_ns();
        double drawn = 0;
        while (auto f = gx::next_frame(0)) {
            draws = f->draws.size(), vertices = f->vertices.size() + f->small.size(), states = f->states.size();
            const uint64_t r0 = gx::now_ns();
            if (pictures) {  // held, and that many pictures between it and the frame before
                t.renderer.render_held(std::move(f));
                drawn += double(gx::now_ns() - r0) / 1e6;
                paired = t.renderer.pairing().paired;
                for (unsigned k = 1; k <= pictures; ++k) {
                    const uint64_t b0 = gx::now_ns();
                    const bool made = t.renderer.render_between(float(k) / float(pictures + 1)) != nullptr;
                    if (n >= 10 && made) between_ms.push_back(double(gx::now_ns() - b0) / 1e6);
                }
            } else {
                t.renderer.render(*f);
                drawn += double(gx::now_ns() - r0) / 1e6;
                gx::recycle_frame(std::move(f));
            }
        }
        if (n >= 10) record_ms.push_back(double(t1 - t0) / 1e6), render_ms.push_back(drawn);
    }
    vkDeviceWaitIdle(gpu.device);
    const uint64_t timed = stats.gpu_timed.load() - timed0;
    std::printf("bench: %u player%s, %u frames on %s: %llu bytes of commands a frame; %llu draws, %llu vertices, %llu states\n"
                "  decoding and recording %.3f ms a frame (median; fastest %.3f), drawing %.3f ms on the CPU (fastest %.3f), %.3f ms on the GPU\n",
                players, players == 1 ? "" : "s", frames, gpu.name.c_str(), static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(draws), static_cast<unsigned long long>(vertices),
                static_cast<unsigned long long>(states), median(record_ms), *std::min_element(record_ms.begin(), record_ms.end()), median(render_ms),
                *std::min_element(render_ms.begin(), render_ms.end()),
                timed ? double(stats.gpu_ns.load() - gpu0) / 1e6 / double(timed) : 0.0);
    if (pictures && !between_ms.empty())
        std::printf("  %u picture%s between frames: %.3f ms each on the CPU (median; fastest %.3f), %.3f ms on the GPU; %llu matrices paired\n",
                    pictures, pictures == 1 ? "" : "s", median(between_ms), *std::min_element(between_ms.begin(), between_ms.end()),
                    t.renderer.between_gpu_ms(), static_cast<unsigned long long>(paired));
    else if (pictures) std::printf("  no pictures between frames were made\n");
    return 0;
}

// mkdd_render_test --mtxtag: the bytes mtxtag.cpp's two loaders write to the FIFO for a fixed
// set of matrices and slots, in hex, for tools/check/mtxtag_check.py to compare with what the
// game's own code writes for the same.
int mtxtag_bytes() {
    constexpr uint32_t kMatrix = 0x80600000u, kFifo = 0x00700000u;
    mkdd::init_gpu();
    rt::Cpu c = rt::new_cpu();
    const auto reg = [&](uint32_t address, uint32_t value) {
        for (const mkdd::MmioRange& r : mkdd::g_mapped)
            if (address >= r.begin && address < r.end) return r.write(c, address, value, 4);
        rt::fatal(nullptr, "render_test: register %08X isn't mapped", address);
    };
    reg(0xCC00300Cu, kFifo), reg(0xCC003010u, kFifo + 0xFFE0u), reg(0xCC003014u, kFifo);  // the CPU's FIFO, not linked to the GPU
    static constexpr uint32_t kSlots[4] = {0, 3, 27, 30};
    for (uint32_t k = 0; k < 4; ++k)
        for (uint32_t i = 0; i < 12; ++i) rt::poke32(kMatrix + 64 * k + 4 * i, 0x3F800000u + 0x00010203u * (k * 12 + i + 1));
    for (const rt::Provided& p : mkdd::mtxtag::replacements())
        for (uint32_t k = 0; k < 4; ++k) {
            c.r[3] = kMatrix + 64 * k, c.r[4] = kSlots[k], c.lr = 0x80001234u;
            p.fn(c);
        }
    for (int i = 0; i < 32; ++i) rt::g_pipe_write(c, 0, 1);  // the last burst out
    std::printf("mtxtag: ");
    for (uint32_t i = 0; i < 2 * 4 * 53; ++i) std::printf("%02x", *rt::guest_ptr(0x80000000u + kFifo + i));
    std::printf("\n");
    return 0;
}

const rt::FunctionEntry kNoFunctions[] = {{0x80004000u, [](rt::Cpu&) {}}};

}  // namespace

int main(int argc, char** argv) {
    if (!vulkan_13_device()) {
        std::puts("render_test: no Vulkan 1.3 device here: skipped");
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;
    }
    rt::init({kNoFunctions, {}});  // guest memory, for textures
    if (argc >= 2 && std::strcmp(argv[1], "--mtxtag") == 0) return mtxtag_bytes();
    host::Gpu gpu = host::Gpu::create({}, false);
    std::printf("device: %s (logic ops %s, wide lines %s, large points %s)\n", gpu.name.c_str(), gpu.logic_op ? "yes" : "no",
                gpu.wide_lines ? "yes" : "no", gpu.large_points ? "yes" : "no");
    gx::set_rendering(true);
    if (argc >= 2 && std::strcmp(argv[1], "--bench") == 0) {
        const int code = bench(gpu, argc >= 3 ? unsigned(std::atoi(argv[2])) : 1, argc >= 4 ? unsigned(std::atoi(argv[3])) : 200,
                               argc >= 5 ? unsigned(std::atoi(argv[4])) : 5, argc >= 6 ? unsigned(std::atoi(argv[5])) : 0);
        gpu.destroy();
        return code;
    }
    {
        Target t(gpu);
        Gx g;
        g.setup();
        sanity(t, g);
        copy_filter(t, g);
        if (gpu.logic_op) logic_ops(t, g, false, "the GPU's");
        lines_and_points(t, g, gpu.wide_lines && gpu.large_points, "the GPU's");
        strips_and_lists(t, g);
        z_textures(t, g);
        depth_copy(t, g);
        rg8_gb8(t, g);
        t.finish(g, false);  // everything this renderer's textures are in, drawn by it
        Stream st;
        joined_primitives(t, st);
        clear_by_z_texture(t, st);
        depth_copies(t, st);
        split_screen(t, st);
        indexed_matrices(t, st);
        lod_bias(t, st);
        z_before_alpha_test(t, st);
        between_frames(t, st);
        between_four_views(t, st);
        stream_begin(st), st.end_frame(), st.send();
        t.shown();
    }
    {
        // As on a GPU without the optional features (Apple's, through MoltenVK).
        host::Gpu without = gpu;
        without.logic_op = without.wide_lines = without.large_points = false;
        without.max_line_width = without.max_point_size = 1.0f;
        Target t(without);
        Gx g;
        g.scratch = 0x00700000u;
        g.setup();
        logic_ops(t, g, true, "approximated");
        lines_and_points(t, g, false, "fallback");
        t.finish(g, false);
    }
    gpu.destroy();
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
