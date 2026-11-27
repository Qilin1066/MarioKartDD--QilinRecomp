// Exercise the real audio registers and DMA clock with synthetic stereo data.
#include "audio.hpp"
#include "device_harness.hpp"
#include "hw.hpp"
#include "priority.hpp"

namespace {
std::vector<mkdd::MmioRange> ranges;
unsigned interrupts = 0;
std::vector<int16_t> played;
}
namespace mkdd {
void map_mmio(const MmioRange& range) { ranges.push_back(range); }
uint32_t interrupt_mask() { return 0; }
void wake_idle() {}
bool set_this_thread_priority(Priority) { return true; }
bool call_interrupt_handler(qilin::rt::Cpu& c, uint32_t n) {
    harness::check(n == 5, "audio DMA uses interrupt 5");
    ++interrupts;
    ranges[0].write(c, 0xCC00500A, 0x18, 2); // acknowledge AID, retain its enable
    return true;
}
}
int main() {
    using namespace harness;
    namespace rt = qilin::rt;
    rt::init({{}, {}});
    auto c = rt::new_cpu();
    mkdd::init_audio();
    mkdd::set_audio_sink([](const int16_t* lr, size_t frames, unsigned rate, void*) {
        check(rate == 32000 && frames == 160, "DMA delivers a 5 ms stereo buffer at 32 kHz");
        played.insert(played.end(), lr, lr + 2 * frames);
    }, nullptr);
    const auto write = [&](uint32_t offset, uint32_t v) { ranges[0].write(c, 0xCC005000 + offset, v, 2); };
    constexpr uint32_t a = 0x00100000, b = 0x00101000;
    for (uint32_t i = 0; i < 160; ++i) {
        rt::poke16(0x80000000 + a + i * 4, 200);
        rt::poke16(0x80000000 + a + i * 4 + 2, 100);
        rt::poke16(0x80000000 + b + i * 4, 400);
        rt::poke16(0x80000000 + b + i * 4 + 2, 300);
    }
    write(0x0A, 0x10);
    write(0x30, a >> 16); write(0x32, a & 0xFFFF);
    write(0x36, 0x8000 | 20);
    deliver(c);
    check(interrupts == 1, "starting DMA requests the next buffer immediately");
    write(0x30, b >> 16); write(0x32, b & 0xFFFF);
    advance(c, ms(5));
    check(played.size() == 320 && played[0] == 100 && played[1] == 200, "DMA preserves left/right order");
    check(interrupts == 2, "a completed buffer requests fresh sound");
    write(0x30, a >> 16); write(0x32, a & 0xFFFF);
    // A shader compilation or disc stall keeps the guest from polling for 150 ms.
    // Poll once on return: request fresh sound now, without replaying old buffers.
    now += ms(150);
    deliver(c);
    check(interrupts == 3, "a long stall immediately requests fresh sound");
    check(next_deadline() == now + ms(5), "the next DMA period starts at the recovery time");
    const auto recovered = played.size();
    check(recovered == 320, "recovery discards the stale buffer instead of repeating it");
    advance(c, ms(5));
    check(interrupts == 4 && played.size() == recovered + 320 && played[recovered] == 100 && played[recovered + 1] == 200,
          "the reloaded buffer plays normally after recovery");
    write(0x36, 0);
    advance(c, ms(20));
    check(interrupts == 4, "stopping DMA stops its clock");
    return failures ? 1 : 0;
}
