// Sound: the DSP's and the audio interface's registers and ARAM (dsp.cpp),
// under the game's own DSP microcode, lifted (dsp_core.hpp; docs/audio.md).
// The game's JAudio, AI and DSP libraries are lifted and drive this hardware
// unchanged.
#pragma once

#include <cstddef>
#include <cstdint>

#include "qilin/rt/system.hpp"

namespace mkdd {

// Map the DSP (0xCC005000) and AI (0xCC006C00) registers; init_hardware calls it.
void init_audio();

// The OS's interrupt mask changed: set the DSP control register's enables
// for interrupts 5, 6 and 7 (AI DMA, ARAM, DSP) to match, as the OS does.
void dsp_interrupt_enables(uint32_t os_mask);

// Where the audio DMA's samples go: interleaved left, right, host byte
// order, at `rate` Hz. Called on a guest thread. Without a sink, sound is
// computed and discarded (headless).
using AudioSink = void (*)(const int16_t* left_right, size_t frames, unsigned rate, void* user);
void set_audio_sink(AudioSink sink, void* user);

// command_lists: times the CPU interrupted the DSP (a command or a release of channels).
struct AudioStats {
    uint64_t dma_periods = 0, frames = 0, command_lists = 0, voices = 0, mails_in = 0, mails_out = 0, dsp_runs = 0;
    uint32_t ucode_crc = 0;
};
const AudioStats& audio_stats();

// ARAM: the 16 MB of audio memory behind the DSP, as bytes at their ARAM
// addresses. The DSP's accelerator reads sample data from it; the AR library
// (or its replacement) fills it. Zeroed at start; valid for the program's life.
inline constexpr size_t kAramSize = 16u << 20;
uint8_t* aram_base();

namespace dsp {
// Guest memory at a physical address (the segment bits are ignored).
uint8_t* physical(qilin::rt::Cpu* c, uint32_t address, uint32_t size);
}  // namespace dsp

}  // namespace mkdd
