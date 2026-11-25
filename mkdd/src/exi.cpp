// The expansion interface (dolphin/exi): the bus behind the memory card
// slots, the two serial ports under the console and the clock chip.
//
// Nothing here drives it: the memory cards are replaced above it (card.cpp),
// the clock and the saved settings above it too (the OS's, at OSRtc), and the
// debug serial port with the OS's console output. What is left is asking a
// port what it holds. The game asks serial port 1 for the Broadband Adapter
// (BBAMgr, through EXIGetType, which stays lifted over EXIGetID) and offers
// LAN play if it answers; no adapter is fitted, as on most consoles, and the
// game goes on as it does there.
//
// Every other EXI call stays lifted and unreached. One that were reached
// would touch the bus's registers, which no device answers: the runtime stops
// there and names the register.
#include "boot.hpp"
#include "card.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

// EXIInit installs the bus's interrupt handlers and looks for the debug
// serial adapter: neither exists here.
void EXIInit(Cpu&) {}

// (channel, device, id*) -> BOOL. A memory card slot is channel 0 or 1,
// device 0: a card answers with its size in megabits, an empty slot fails.
// Anything else is a bare connector. Its data line reads as zeros here, which
// names no device; what the console's own line reads with nothing fitted was
// not measured, and all ones would name none either.
void EXIGetID(Cpu& c) {
    const uint32_t channel = c.r[3], device = c.r[4], id = c.r[5];
    if (channel > 2 || device > 2) rt::fatal(&c, "EXIGetID(%u, %u): there is no such port", channel, device);
    if (channel == 0 && device == 1) rt::fatal(&c, "EXIGetID asks for the clock chip, which is answered above EXI (OSRtc)");
    if (channel < 2 && device == 0) {
        if (!card_present(channel)) {
            c.r[3] = 0;
            return;
        }
        rt::poke32(id, kCardMegabits);
    } else {
        rt::poke32(id, 0);
    }
    c.r[3] = 1;
}

const rt::Provided kReplacements[] = {
    {0x800C4B00u, "EXIInit", EXIInit},
    {0x800C4EE4u, "EXIGetID", EXIGetID},
};

}  // namespace

std::span<const rt::Provided> exi_replacements() { return kReplacements; }

}  // namespace mkdd
