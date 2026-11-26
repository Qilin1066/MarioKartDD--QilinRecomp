// Textures from guest memory (texture.cpp): GX's formats decoded to RGBA8,
// cached by what they were decoded from, and the textures EFB copies make.
#pragma once

#include <cstdint>

#include "gx.hpp"

namespace mkdd::gx {

// BP 0x65: copy `bytes` of palette from guest memory into TMEM.
void load_tlut(uint32_t source, uint32_t tmem_address, uint32_t bytes);

// The texture texture map `map` names in the BP registers `bp`: its id,
// decoded into `frame` if it's new or its memory changed. Width and height
// out. -1 if the map has no address.
int32_t texture_for(const uint32_t* bp, unsigned map, Frame& frame, uint32_t& width, uint32_t& height);

// An EFB copy to a texture at `address`: the id it becomes. Later reads of
// a texture of that size at that address get the copy, until the game
// writes that memory.
int32_t texture_from_copy(uint32_t address, uint32_t width, uint32_t height, Frame& frame);

// Once per frame: forget what hasn't been used for a while.
void textures_end_frame(Frame& frame);
// BP 0x66: the GPU's texture cache was invalidated (texture data may have changed).
void textures_invalidated();

// ---- copies the CPU reads (Copy::to_memory), answered on the game's thread
//
// The game copies 2x2 pixels of depth and reads them on the CPU in its next pass's
// calculation, where the console's GPU has long made the copy (ZCaptureNode::getZValue: is a
// player's mark or the sun hidden?). The renderer submits the work up to such a copy as soon
// as it has recorded it; the game's read waits for that (mtxtag.cpp's wrapper). Copies are
// told apart by their destination (physical, MEM1).
//
// The recorder: a frame handed to the renderer, with its copies for the CPU on their way.
void cpu_copies_queued(const Frame& frame);
// The renderer: the copy to `dest` (`rgba`: 4 bytes a pixel, rows of 4 pixels; good until
// cpu_copies_reached passes `ticket`; null: not read back, memory is left) is ready once the
// GPU passes `ticket`.
void cpu_copy_submitted(uint32_t dest, uint64_t ticket, const uint8_t* rgba, uint32_t width, uint32_t height);
// The renderer: the GPU has passed `ticket`: those copies go to guest memory now.
void cpu_copies_reached(uint64_t ticket);
// How a waiting thread waits for the GPU to pass a ticket (the renderer's timeline
// semaphore; true if it did within the time); null: no renderer.
using TicketWait = bool (*)(void* context, uint64_t ticket, uint64_t timeout_ns);
void set_ticket_wait(TicketWait wait, void* context);
// The game's thread: every copy to `dest` handed to the renderer is in guest memory on
// return (false: not within a quarter of a second, logged; or nothing renders).
bool await_cpu_copy(uint32_t dest);
// Waits that found a copy not yet in memory, and their time in all (for the log).
void cpu_copy_waits(uint64_t& waits, uint64_t& ns);

// GX's texture formats and their sizes in memory.
uint32_t texture_bytes(unsigned format, uint32_t width, uint32_t height);
// Decode one level (tests): RGBA8 out, width * height * 4 bytes.
void decode_texture(unsigned format, const uint8_t* src, uint32_t width, uint32_t height, const uint8_t* tlut,
                    unsigned tlut_format, uint8_t* out);

}  // namespace mkdd::gx
