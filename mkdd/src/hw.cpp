// The hardware registers: who answers which range (hw.hpp).
#include "hw.hpp"

#include <algorithm>
#include <vector>

#include "audio.hpp"
#include "gpu.hpp"
#include "os.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {
std::vector<MmioRange> g_ranges;  // sorted by begin; a handful, so a linear search

const MmioRange* find(uint32_t a) {
    for (const MmioRange& r : g_ranges)
        if (a >= r.begin && a < r.end) return &r;
    return nullptr;
}
uint32_t read(Cpu& c, uint32_t a, unsigned size) {
    if (const MmioRange* r = find(a)) return r->read(c, a, size);
    rt::fatal(&c, "load of %u bytes from hardware register %08X: no device there", size, a);
}
void write(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    if (const MmioRange* r = find(a)) return r->write(c, a, v, size);
    rt::fatal(&c, "store of %u bytes (%08X) to hardware register %08X: no device there", size, v, a);
}
}  // namespace

void map_mmio(const MmioRange& range) {
    for (const MmioRange& r : g_ranges)
        if (range.begin < r.end && r.begin < range.end)
            rt::fatal(nullptr, "hardware ranges %s and %s overlap", r.name, range.name);
    g_ranges.push_back(range);
    std::sort(g_ranges.begin(), g_ranges.end(), [](const MmioRange& x, const MmioRange& y) { return x.begin < y.begin; });
}

// The memory interface (0xCC004000). Its protection is the OS's (replaced);
// what lifted code reads are two 16-bit counters at +0x4E and +0x50, which
// GX's abort and shutdown sample until they stop moving (__GXAbort): the GPU
// here has always finished what it was sent, so they stand still.
static uint32_t mi_read(Cpu& c, uint32_t a, unsigned size) {
    if (size == 2 && (a == 0xCC00404Eu || a == 0xCC004050u)) return 0;
    rt::fatal(&c, "load of %u bytes from the memory interface's register %08X", size, a);
}
static void mi_write(Cpu& c, uint32_t a, uint32_t v, unsigned size) {
    rt::fatal(&c, "store of %u bytes (%08X) to the memory interface's register %08X", size, v, a);
}

void init_hardware() {
    map_mmio({0xCC004000u, 0xCC004080u, "the memory interface", mi_read, mi_write});
    init_gpu();
    init_audio();
    rt::set_mmio(read, write);
    rt::set_idle(poll_loop);
    rt::set_poll(poll_point);  // (core: safe points on every lifted loop's back edge)
    start_poll_clock();
}

}  // namespace mkdd
