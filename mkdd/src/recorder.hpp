// The guest's side of drawing (recorder.cpp): gpu.cpp's decoder calls these
// as commands arrive, and frames go to the renderer.
#pragma once

#include <atomic>

#include <cstdint>

namespace mkdd::gx {

// The command processor's, XF's and BP's registers as gpu.cpp keeps them.
struct Registers {
    const uint32_t* cp;  // 256
    const uint32_t* xf;  // 0x1058
    const uint32_t* bp;  // 256
};

// A primitive: opcode 0x80-0xBF (type in bits 3-5, vertex format in 0-2),
// `count` vertices of raw data at `data`.
void record_primitive(const Registers& r, uint8_t opcode, const uint8_t* data, uint32_t count);
// State changed (all of it, as far as the recorder knows, the CP's registers
// too): the next draw snapshots it again.
void state_changed();
// A CP register changed (vertex descriptors, attribute tables, array bases
// and strides, matrix indices): the next primitive reads its format again.
void cp_changed();
// XF memory or registers [address, address + count) changed: the next draw
// snapshots what that affects.
void xf_changed(uint32_t address, uint32_t count);
// A position matrix was loaded: twelve words at XF `address` (now in `words`),
// by `tag` (gx.hpp's Load; 0 unknown) under the view matrix `view` (twelve
// floats; null: the one last given, as for an indexed load, which comes from
// the model whose shapes were just tagged). Besides xf_changed.
void matrix_loaded(uint32_t address, const uint32_t* words, uint32_t tag, const float* view, uint32_t id = 0, uint8_t context = 0);
// A BP register was written (the TEV colour registers latch by which half is written).
// `changed`: the value differs from before (else only commands it triggers matter).
void bp_written(uint32_t reg, uint32_t value, bool changed = true);
// BP 0x52: an EFB copy (to a texture, or to the external frame buffer, which
// ends the frame).
void record_copy(const Registers& r, uint32_t command);
// BP 0x43 changed the EFB's pixel format (its low three bits, `from` to `to`).
void record_pixel_format(uint32_t from, uint32_t to);
// BP 0x65: a palette load.
void record_tlut_load(const Registers& r, uint32_t command);

// Counters for the five-second stats line, since boot. The main thread
// writes rendered, render_ns, gpu_wait_ns, gpu_textures, gpu_spares and
// images_made; the guest's thread the rest.
struct RecorderStats {
    std::atomic<uint64_t> frames{0};
    std::atomic<uint64_t> record_ns{0};    // decoding commands and recording them (wait_ns not included)
    std::atomic<uint64_t> wait_ns{0};      // ... waiting for the main thread to take a frame
    std::atomic<uint64_t> texture_lookups{0}, hash_ns{0};  // textures bound, and checking them for changes
    std::atomic<uint64_t> hashed_bytes{0};                 // ... the bytes read to check them
    std::atomic<uint64_t> textures_decoded{0}, texture_bytes{0}, decode_ns{0};  // decoding new ones (bytes of guest data)
    std::atomic<uint64_t> rendered{0}, render_ns{0};  // the main thread drawing frames
    std::atomic<uint64_t> gpu_wait_ns{0};             // ... of which waiting for the GPU to finish an older frame
    std::atomic<uint64_t> gpu_ns{0}, gpu_timed{0};    // the GPU's own time on frames (timestamps), and how many were timed
    std::atomic<uint64_t> gpu_textures{0}, gpu_spares{0};  // images the renderer holds now: in use, and kept to reuse
    std::atomic<uint64_t> images_made{0};             // images created since boot (the rest are reused)
};
RecorderStats& recorder_stats();
// The game's thread is in JFWDisplay::endRender (vi.cpp): the frame ends at the draw done
// it issues, with the copy to the frame buffer the next pass will make (MKDD_EARLY_END=0: not).
void request_frame_end();

// The TEV colour registers and konstants as BP writes (sixteen: register in the
// top byte, value below), for a frame capture's start: BP memory keeps only
// whichever of the two was written last at each address.
void tev_register_writes(const Registers& r, uint32_t out[16]);

// Nanoseconds on a steady clock, for the counters.
uint64_t now_ns();

}  // namespace mkdd::gx
