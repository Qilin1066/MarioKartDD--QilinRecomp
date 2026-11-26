// The DSP's hardware registers, its DMA and the accelerator's raw port: the
// slow paths behind dsp_core.hpp.
//
// 0xFFxx, as the microcode uses them (GameCube DSP User's Manual; the raw
// port's behaviour is Dolphin's Accelerator.cpp as fact):
//   C9 DSCR  bit 0 to the CPU, bit 1 instruction memory, bit 2 busy (never: a DMA is done at once)
//   CB DSBL  length in bytes; writing it does the DMA    CD DSPA  word address in the DSP
//   CE/CF    main memory address
//   D1 FORMAT (bits 0-1 the raw port's unit: 0 nibble, 1 byte, 2 word)
//   D3 ACDRAW a unit of ARAM at the current address, which then steps; written, it stores a word
//   D4/D5 start, D6/D7 end, D8/D9 current address (in units; bit 31 is set for writing)
//   FB DIRQ  bit 0: interrupt the CPU
//   FC/FD    mail to the CPU (FD sets "unread")    FE/FF  mail from the CPU (reading FF takes it)
#include "dsp_core.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mkdd::dsp {

uint32_t ector_hash(const uint8_t* p, size_t n) {
    uint32_t h = 0;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h = (h << 3) | (h >> 29);
    }
    return h;
}

void Core::die(const char* what, uint32_t a, uint32_t b) const {
    char text[256];
    std::snprintf(text, sizeof text, what, a, b);
    if (fatal) fatal(user, text);
    std::fprintf(stderr, "dsp: %s\n", text);
    std::abort();
}
void Core::bad_pc() const {
    die("the DSP reached %04X, which the lifter did not take for code (microcode %08X)", pc, program ? program->hash : 0);
}

void Core::boot(const Program* p, uint16_t start) {
    r = Regs{};
    r.sr = kIE | kEIE;
    r.config = 0x04;
    std::memset(stack, 0, sizeof stack);
    std::memset(sp, 0, sizeof sp);
    std::memset(dram, 0, sizeof dram);
    std::memset(ifx, 0, sizeof ifx);
    acc_start = acc_end = acc_current = 0;
    program = p;
    pc = start;
    ext_int = false;
    progress = true;
    idle_seen = false;
    idle_edge = 0xFFFF;
}

void Core::run() {
    waiting = false;
    for (;;) {
        if (halted || !program) return;
        if (pc >= 0x8000) {
            if (!rom) die("the microcode jumped into the DSP's ROM at %04X", pc);
            if (!rom(user, *this, pc)) return;
            continue;
        }
        if (int_due(r.sr)) {
            r.sr = take_int(r.sr, pc);
            pc = 0x000E;
        }
        program->run(*this);
        if (halted) return;
        if (pc < 0x8000) {
            waiting = true;
            return;
        }
    }
}

bool Core::idle_slow() {
    if (idle_seen && r == idle_regs && !std::memcmp(sp, idle_sp, sizeof sp) && !std::memcmp(stack, idle_stack, sizeof stack)) return true;
    idle_regs = r;
    std::memcpy(idle_sp, sp, sizeof sp);
    std::memcpy(idle_stack, stack, sizeof stack);
    idle_seen = true;
    return false;
}

uint16_t Core::rd_slow(uint16_t a) {
    if (a >= 0xFF00) return hw_read(a);
    if (a < 0x1800) die("the microcode reads the DSP's coefficient ROM at %04X (at %04X): the ROM is not here", a, pc);
    die("the microcode reads data memory at %04X, where there is none", a);
}
void Core::wr_slow(uint16_t a, uint16_t v) {
    if (a >= 0xFF00) return hw_write(a, v);
    die("the microcode writes %04X to data memory at %04X, where there is no RAM", v, a);
}

uint16_t Core::hw_read(uint16_t a) {
    switch (a & 0xFF) {
        case 0xFC: return static_cast<uint16_t>(dsp_mail >> 16);
        case 0xFD: return static_cast<uint16_t>(dsp_mail);
        case 0xFE: return static_cast<uint16_t>(cpu_mail >> 16);
        case 0xFF:
            cpu_mail &= 0x7FFFFFFFu;
            progress = true;
            return static_cast<uint16_t>(cpu_mail);
        case 0xC9: return ifx[0xC9] & ~uint16_t(4);
        case 0xD3: return accelerator_read();
        case 0xD4: return static_cast<uint16_t>(acc_start >> 16);
        case 0xD5: return static_cast<uint16_t>(acc_start);
        case 0xD6: return static_cast<uint16_t>(acc_end >> 16);
        case 0xD7: return static_cast<uint16_t>(acc_end);
        case 0xD8: return static_cast<uint16_t>(acc_current >> 16);
        case 0xD9: return static_cast<uint16_t>(acc_current);
        case 0xDD: die("the microcode reads the accelerator's decoded samples (ACDSAMP) at %04X: not handled", pc);
        case 0xCB: case 0xCD: case 0xCE: case 0xCF: case 0xD1: case 0xD2: case 0xDA: case 0xDB: case 0xDC: case 0xDE:
        case 0xDF: case 0xEF:
            return ifx[a & 0xFF];
        default:
            if ((a & 0xF0) == 0xA0) return ifx[a & 0xFF];
            die("the microcode reads hardware register %04X (at %04X): not known", a, pc);
    }
}

void Core::hw_write(uint16_t a, uint16_t v) {
    progress = true;
    const unsigned n = a & 0xFF;
    switch (n) {
        case 0xFB:
            if ((v & 1) && irq) irq(user);
            return;
        case 0xFC: dsp_mail = (dsp_mail & 0xFFFF) | (uint32_t(v & 0x7FFF) << 16); return;
        case 0xFD: dsp_mail = (dsp_mail & 0x7FFF0000u) | v | 0x80000000u; return;
        case 0xCB:
            ifx[n] = v;
            dma();
            return;
        case 0xD3: accelerator_write(v); return;
        case 0xD4: acc_start = (acc_start & 0xFFFF) | (uint32_t(v) << 16); return;
        case 0xD5: acc_start = (acc_start & 0xFFFF0000u) | v; return;
        case 0xD6: acc_end = (acc_end & 0xFFFF) | (uint32_t(v) << 16); return;
        case 0xD7: acc_end = (acc_end & 0xFFFF0000u) | v; return;
        case 0xD8: acc_current = (acc_current & 0xFFFF) | (uint32_t(v) << 16); return;
        case 0xD9: acc_current = (acc_current & 0xFFFF0000u) | v; return;
        case 0xC9: case 0xCD: case 0xCE: case 0xCF: case 0xD1: case 0xD2: case 0xDA: case 0xDB: case 0xDC: case 0xDE:
        case 0xDF: case 0xEF:
            ifx[n] = v;
            return;
        default:
            if ((n & 0xF0) == 0xA0) {
                ifx[n] = v;
                return;
            }
            die("the microcode writes %04X to hardware register %04X: not known", v, a);
    }
}

void Core::dma() {
    const uint32_t address = (uint32_t(ifx[0xCE]) << 16) | ifx[0xCF];
    const unsigned control = ifx[0xC9] & 3, words = ifx[0xCB] / 2, at = ifx[0xCD];
    if (control & 2) die("the microcode starts a DMA on its instruction memory (DSCR %04X) at %04X: not handled", ifx[0xC9], pc);
    if (at + words > 0x1000) die("a DSP DMA of %u words at %04X leaves the data RAM", words, at);
    if (!words) return;
    if (!memory) die("a DSP DMA with no main memory to reach");
    uint8_t* p = memory(user, address, words * 2);
    dma_bytes += words * 2;
    if (control & 1) {
        for (unsigned i = 0; i < words; ++i) {
            const uint16_t w = dram[at + i];
            p[2 * i] = static_cast<uint8_t>(w >> 8);
            p[2 * i + 1] = static_cast<uint8_t>(w);
        }
    } else {
        for (unsigned i = 0; i < words; ++i) dram[at + i] = static_cast<uint16_t>((p[2 * i] << 8) | p[2 * i + 1]);
    }
}

uint16_t Core::accelerator_read() {
    progress = true;
    ++aram_reads;
    const uint32_t a = acc_current, mask = static_cast<uint32_t>(kAramSize - 1);
    uint16_t v = 0;
    switch (ifx[0xD1] & 3) {
        case 0:
            v = aram[(a >> 1) & mask];
            v = (a & 1) ? (v & 0xF) : (v >> 4);
            break;
        case 1: v = aram[a & mask]; break;
        case 2: v = static_cast<uint16_t>((aram[(a * 2) & mask] << 8) | aram[(a * 2 + 1) & mask]); break;
        default: die("the accelerator's raw port is read with format %04X: not known", ifx[0xD1]);
    }
    if (++acc_current >= acc_end)
        die("the accelerator's raw port reached its end address %08X (at %04X): not handled", acc_end, pc);
    return v;
}

void Core::accelerator_write(uint16_t v) {
    ++aram_writes;
    if ((ifx[0xD1] & 3) != 2) die("the accelerator's raw port is written with format %04X: only words are known", ifx[0xD1]);
    const uint32_t a = acc_current, mask = static_cast<uint32_t>(kAramSize - 1);
    aram[(a * 2) & mask] = static_cast<uint8_t>(v >> 8);
    aram[(a * 2 + 1) & mask] = static_cast<uint8_t>(v);
    if (++acc_current >= acc_end)
        die("the accelerator's raw port reached its end address %08X (at %04X): not handled", acc_end, pc);
}

}  // namespace mkdd::dsp
