// The renderer (renderer.cpp): the guest's frames (gx.hpp) drawn with Vulkan
// on the main thread.
#pragma once

#include <cstdint>
#include <memory>

#include "gx.hpp"
#include "qilin/host/vulkan.hpp"

namespace mkdd {

class Renderer {
public:
    // `scale`: the EFB's resolution as a multiple of the console's 640x528.
    Renderer(qilin::host::Gpu& gpu, unsigned scale, unsigned anisotropy = 1);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Draw a frame's work (submitted to the GPU on return).
    void render(gx::Frame& frame);
    // The EFB's scale from the next frame drawn (1 to 8): it is made again at
    // the new size between two frames, with the picture it held (its depth
    // starts at the far plane, as the game's own clear leaves it). Copies made
    // before keep their size until the game makes them again. scale(): the
    // one in use.
    void request_scale(unsigned scale);
    unsigned scale() const;
    // The latest external frame buffer copied to `address` (in
    // TRANSFER_SRC_OPTIMAL layout, sampled usage), or null if there's none.
    const qilin::host::Image* xfb(uint32_t address) const;
    // EFB copies the CPU reads (the small ones: gx::Copy::to_memory) reach
    // guest memory when a later render() finds their frame drawn; this waits
    // for the GPU and writes them now (the tests; the end of a replay).
    void finish_copies();
    // Anisotropic filtering for the game's textures: 1 (the console's), 2, 4,
    // 8 or 16, up to what the GPU allows. Waits for the GPU.
    void set_anisotropy(unsigned level);
    // True colour: an EFB with alpha (six bits a channel on the console, dithered) keeps all
    // eight bits, with no dither. Off, the console's own colour depth. From the next frame.
    void set_true_color(bool on);
    // Write the pipeline cache and the keys met since the last save, now (the
    // program is about to end without its destructors).
    void save_now();
    // Whether draws may use pipelines specialised to their shape (on unless
    // MKDD_UBERSHADER is set); off, everything draws with the ubershader.
    void set_specialization(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mkdd
