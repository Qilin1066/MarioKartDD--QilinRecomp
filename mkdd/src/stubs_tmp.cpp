// TEMPORARY, to be deleted when the branches are merged: stand-ins for
// libraries whose replacements are being written elsewhere (PAD and SI, CARD
// and EXI, AR and ARQ: the devices work; the DSP and AI hardware: the audio
// work), so that start-up can be brought up before they exist. Each says what
// an empty console would: nothing plugged in, no memory card.
#include <cstring>
#include <vector>

#include "boot.hpp"
#include "os.hpp"

namespace mkdd {
namespace rt = qilin::rt;
using rt::Cpu;

namespace {

void nothing(Cpu&) {}
void returns_true(Cpu& c) { c.r[3] = 1; }

// PAD: no controller in any port. PADRead(PADStatus[4]) -> the motor mask;
// a PADStatus is 12 bytes with its error at +0xA (-1: no controller).
void PADRead(Cpu& c) {
    for (uint32_t i = 0; i < 4; ++i) {
        std::memset(rt::guest_ptr(c.r[3] + 12 * i), 0, 12);
        rt::poke8(c.r[3] + 12 * i + 0xA, 0xFF);
    }
    c.r[3] = 0;
}
void SIProbe(Cpu& c) { c.r[3] = 0x08; }  // SI_ERROR_NO_RESPONSE: nothing there

// AR and ARQ: the 16 MB of audio RAM as a buffer of the port's own, the
// allocator's stack pointer, and transfers done at once with their callback
// as an interrupt. (The DSP's accelerator reads the same memory: the real
// replacement has to share it with the audio work.)
constexpr uint32_t kAramSize = 0x01000000u, kAramBase = 0x4000u;
std::vector<uint8_t> g_aram(kAramSize);
uint32_t g_aram_top = kAramBase, g_aram_lengths = 0;
void ARInit(Cpu& c) {  // (u32* block lengths, entries) -> the base address
    g_aram_lengths = c.r[3];
    g_aram_top = kAramBase;
    c.r[3] = kAramBase;
}
void ARAlloc(Cpu& c) {  // (length) -> its address in ARAM
    const uint32_t at = g_aram_top;
    if (uint64_t{at} + c.r[3] > kAramSize) rt::fatal(&c, "ARAlloc(%u): ARAM is full", c.r[3]);
    g_aram_top += c.r[3];
    rt::poke32(g_aram_lengths, c.r[3]);
    g_aram_lengths += 4;
    c.r[3] = at;
}
void ARGetSize(Cpu& c) { c.r[3] = kAramSize; }  // (800B15E4 reads __AR_Size, r13-25388: not ARGetBaseAddress, as its size alone suggests)
// ARQPostRequest(request, owner, type (0 to ARAM, 1 from it), priority, source, dest, length, callback(request)).
void ARQPostRequest(Cpu& c) {
    const uint32_t request = c.r[3], type = c.r[5], source = c.r[7], dest = c.r[8], length = c.r[9], callback = c.r[10];
    const uint32_t aram = type == 0 ? dest : source, ram = type == 0 ? source : dest;
    if (type > 1 || uint64_t{aram} + length > kAramSize || ram < 0x80000000u || uint64_t{ram} + length > 0x81800000u)
        rt::fatal(&c, "ARQPostRequest: type %u, %u bytes from %08X to %08X", type, length, source, dest);
    if (type == 0) std::memcpy(g_aram.data() + aram, rt::guest_ptr(ram), length);
    else std::memcpy(rt::guest_ptr(ram), g_aram.data() + aram, length);
    if (callback) post_interrupt([request, callback](Cpu& cc) { guest_callback(cc, callback, request, 0); });
}

const rt::Provided kStubs[] = {
    {0x800C4B00u, "EXIInit (temporary: nothing)", nothing},  // OSInit calls it
    {0x800EEFF4u, "SIInit (temporary: nothing)", nothing},   // OSInit calls it
    {0x800ECB24u, "PADClamp (temporary: no controller to clamp)", nothing},
    {0x800ECC38u, "PADClampCircle (temporary: no controller to clamp)", nothing},
    {0x800ED5BCu, "PADReset (temporary)", returns_true},
    {0x800ED6CCu, "PADRecalibrate (temporary)", returns_true},
    {0x800ED7E0u, "PADInit (temporary)", returns_true},
    {0x800ED930u, "PADRead (temporary: no controller)", PADRead},
    {0x800EDC30u, "PADControlMotor (temporary: nothing)", nothing},
    {0x800EDCE8u, "PADSetSpec (temporary: nothing)", nothing},
    {0x800EE4A0u, "PADSetAnalogMode (temporary: nothing)", nothing},
    {0x800EFF3Cu, "SIProbe (temporary: no response)", SIProbe},
    {0x800B14B8u, "ARAlloc (temporary)", ARAlloc},
    {0x800B1520u, "ARInit (temporary)", ARInit},
    {0x800B15E4u, "ARGetSize (temporary)", ARGetSize},
    {0x800B3028u, "ARQInit (temporary: nothing)", nothing},
    {0x800B3098u, "ARQPostRequest (temporary: copied at once)", ARQPostRequest},
};

}  // namespace

std::span<const rt::Provided> temporary_stubs() { return kStubs; }

}  // namespace mkdd
