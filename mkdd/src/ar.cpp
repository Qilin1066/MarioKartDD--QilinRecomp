// The audio RAM library's replacements (dolphin/ar): the 16 MB of ARAM behind
// the DSP, which the game uses as a second store for files (JKRAram, lifted)
// and for its sound data.
//
// ARQ, the queue of transfers above this, is plain code and stays lifted. It
// starts one transfer at a time with ARStartDMA and is told of each one's end
// through the callback it registers; the console raises the ARAM interrupt
// for that and the library's handler calls the callback, so here the
// callback runs as an interrupt when the transfer's time has passed.
//
// A transfer copies between main memory and ARAM at 32 bytes in 246 cycles
// of the 486 MHz processor, about 63 MB a second: the rate Dolphin's authors
// measured on a console. The copy itself is made when the transfer starts,
// since nothing may touch either end before it is reported done.
#include <cstring>

#include "boot.hpp"
#include "later.hpp"
#include "os.hpp"

#ifdef MKDD_DEVICES_STANDALONE
// Without the audio layer (this file's test; every build until that layer is
// merged): a block of this file's own, under the names it will have there.
namespace mkdd {
inline constexpr uint32_t kAramSize = 16u << 20;
uint8_t* aram_base() {
    static uint8_t* block = new uint8_t[kAramSize]();
    return block;
}
}  // namespace mkdd
#else
#include "audio.hpp"  // uint8_t* aram_base() and kAramSize: the block is the audio layer's (the DSP's accelerator reads it)
#endif

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

constexpr uint32_t kUserBase = 0x4000;  // the first 16 KiB are the audio library's own
constexpr uint32_t kMainSize = 24u << 20;

uint32_t g_callback = 0;
bool g_initialised = false, g_in_flight = false;
uint32_t g_stack_pointer = kUserBase, g_free_blocks = 0, g_block_lengths = 0;

// (lengths[], count) -> the first address the game may use. `lengths` is the
// caller's array, where ARAlloc notes each block's size.
void ARInit(Cpu& c) {
    if (!g_initialised) {
        g_initialised = true;
        g_callback = 0;
        g_block_lengths = c.r[3];
        g_free_blocks = c.r[4];
    }
    c.r[3] = kUserBase;
}
void ARAlloc(Cpu& c) {  // (length) -> the block's address in ARAM
    const uint32_t length = c.r[3], at = g_stack_pointer;
    if (!g_initialised || g_free_blocks == 0)
        rt::fatal(&c, "ARAlloc(%u) with no block left to note it in (ARInit gave %s)", length, g_initialised ? "fewer" : "none: not called");
    if (uint64_t{at} + length > kAramSize) rt::fatal(&c, "ARAlloc(%u) at %08X: past the end of ARAM", length, at);
    g_stack_pointer += length;
    rt::poke32(g_block_lengths, length);
    g_block_lengths += 4;
    --g_free_blocks;
    c.r[3] = at;
}
void ARGetSize(Cpu& c) { c.r[3] = kAramSize; }
void ARRegisterDMACallback(Cpu& c) {  // (callback) -> the one before
    const uint32_t was = g_callback;
    g_callback = c.r[3];
    c.r[3] = was;
}

// (direction, main memory address, ARAM address, length): 0 to ARAM, 1 from
// it. The hardware's registers drop the low five bits of each.
void ARStartDMA(Cpu& c) {
    const uint32_t to_main = c.r[3], main = c.r[4] & 0x03FFFFE0u, aram = c.r[5] & 0x03FFFFE0u, length = c.r[6] & ~0x1Fu;
    if (to_main > 1) rt::fatal(&c, "ARStartDMA: direction %u", to_main);
    if (g_in_flight) rt::fatal(&c, "ARStartDMA(%u, %08X, %08X, %u) while a transfer is in flight", to_main, c.r[4], c.r[5], c.r[6]);
    if (uint64_t{main} + length > kMainSize || uint64_t{aram} + length > kAramSize)
        rt::fatal(&c, "ARStartDMA(%u, %08X, %08X, %u): outside main memory or ARAM", to_main, c.r[4], c.r[5], c.r[6]);
    uint8_t* m = rt::guest_ptr(0x80000000u | main);
    uint8_t* a = aram_base() + aram;
    if (to_main) std::memcpy(m, a, length);
    else std::memcpy(a, m, length);
    g_in_flight = true;
    if (devices_log()) {
        static uint64_t count = 0, bytes = 0;
        ++count, bytes += length;
        if ((count & (count - 1)) == 0 || count % 1024 == 0)  // 1, 2, 4 ... then every 1024th
            rt::log("ar: transfer %llu (%s ARAM %08X, main %08X, %u bytes); %llu KiB so far", static_cast<unsigned long long>(count),
                    to_main ? "from" : "to", aram, main, length, static_cast<unsigned long long>(bytes >> 10));
    }
    run_after(uint64_t{length / 32} * 246 * kTimerHz / 486000000u, [](Cpu& cc) {
        g_in_flight = false;
        if (g_callback) guest_callback(cc, g_callback, 0, 0);
    });
}

const rt::Provided kReplacements[] = {
    {0x800B1384u, "ARRegisterDMACallback", ARRegisterDMACallback},
    {0x800B13C8u, "ARStartDMA", ARStartDMA},
    {0x800B14B8u, "ARAlloc", ARAlloc},
    {0x800B1520u, "ARInit", ARInit},
    {0x800B15E4u, "ARGetSize", ARGetSize},
};

}  // namespace

std::span<const rt::Provided> ar_replacements() { return kReplacements; }

}  // namespace mkdd
