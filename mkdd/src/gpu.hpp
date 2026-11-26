// The GPU's front end (gpu.cpp).
#pragma once

#include <functional>
#include <cstdint>

#include "qilin/rt/system.hpp"

namespace mkdd {

namespace gx {
struct Frame;
}

struct Stats {
    uint64_t fifo_bytes = 0, draws = 0, vertices = 0, display_lists = 0, indexed_loads = 0;
    uint64_t xfb_copies = 0, texture_copies = 0;
};

// Map the GPU's registers (CP, PE, PI, the write-gather pipe); init_hardware calls it.
void init_gpu();
// What the decoder has seen, as of its last batch.
Stats gpu_stats();
// How long the guest has waited for room in the GPU thread's queue, in all.
uint64_t fifo_wait_ns();

// A position matrix load as J3DFifoLoadPosMtxImm and GXLoadPosMtxImm write it (mtxtag.cpp, the
// guest's thread): command 0x10, eleven more words than one, the XF address, and the twelve words
// at guest address `matrix`, to the pipe; and for the recorder, who loads it (gx.hpp's Load::tag)
// and the view matrix now at guest address `view` (twelve floats).
// `id`: gx.hpp's Load::id (0: nothing to tell one of the caller's loads from another).
void load_position_matrix(qilin::rt::Cpu& c, uint32_t matrix, uint32_t xf_address, uint32_t tag, uint32_t view, uint32_t id = 0);
// The same tag for the tests, which write their own bytes: for the XF load that starts
// `stream_offset` bytes into everything given to decode_commands so far.
void tag_matrix_at(uint64_t stream_offset, uint32_t tag, const float view[12], uint32_t id = 0, uint8_t context = 0);
// The context (gx.hpp's kContext...) the matrices loaded from now on carry; returns the one before.
uint8_t set_matrix_context(uint8_t context);
// The guest's thread: everything it has sent to the pipe so far is decoded and recorded on
// return (the GPU thread is waited for; a frame those commands ended is queued). For a read
// of a copy the CPU makes (texture.hpp's await_cpu_copy). Cheap when nothing was sent since.
void sync_decoder();
// Commands given straight to the decoder on the calling thread, as the GPU
// thread gives it the pipe's bursts (the tests and their bench: no registers
// mapped, no GPU thread; a command may be split between calls).
void decode_commands(const uint8_t* bytes, size_t size);
// mkdd --bench-record <file.dff> [times]: decode and record a frame
// captured with MKDD_DFF that many times (default 200) and print how
// long recording it takes (no window, no game run; RAM and registers from the
// log). For measuring the recorder, the CPU's part of drawing. Returns the
// exit code.
int bench_recording(const char* dff_path, unsigned times);
// --render-dff <file.dff> <out.ppm>: replay a captured frame `loops` times through the recorder,
// giving each frame made to `draw` (main.cpp renders them and writes the last).
void replay_dff(const char* dff_path, unsigned loops, const std::function<void(gx::Frame&)>& draw);


}  // namespace mkdd
