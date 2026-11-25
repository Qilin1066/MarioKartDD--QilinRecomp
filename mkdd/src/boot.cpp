// Booting the game: main.dol into MEM1, the boot information the console's
// boot ROM and the disc's apploader leave in low memory, then what __start
// does, natively (the plan replaces the start-up code).
#include "qilin/rt/report.hpp"
#include "boot.hpp"

#include <cstdio>
#include <vector>

#include "build.hpp"
#include "dol.hpp"
#include "os.hpp"
#include "threads.hpp"
#include "qilin/rt/system.hpp"

namespace mkdd {
namespace rt = qilin::rt;

namespace {

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::vector<uint8_t> out;
    if (FILE* f = std::fopen(p.string().c_str(), "rb")) {
        uint8_t buf[65536];
        for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
        std::fclose(f);
    } else {
        rt::fatal(nullptr, "cannot read %s", p.string().c_str());
    }
    return out;
}

uint32_t be32(std::span<const uint8_t> d, size_t at) {
    return uint32_t{d[at]} << 24 | uint32_t{d[at + 1]} << 16 | uint32_t{d[at + 2]} << 8 | d[at + 3];
}

constexpr uint32_t kMemSize = 0x01800000u, kMemEnd = 0x80000000u + kMemSize;

// Low memory as a retail NTSC GameCube's boot ROM (the IPL) and the disc's
// apploader leave it.
//  - The apploader's part is from its own code (sys/apploader.img, the routine
//    at 81200778): the debug monitor's size and address and the simulated
//    memory size from BI2's first words; the file table at the top of memory,
//    32-byte aligned, with BI2's 0x2000 bytes straight below it; the boot
//    magic, arena and file-table words (the routine at 81200728). Arena high
//    is the file table's address, so BI2 lies inside the arena: OSInit and
//    __start read what they need from it before anything is allocated.
//  - The IPL's part (memory size, console type, clocks, video mode, ARAM
//    size) is the hardware's, as Dolphin documents it (facts only).
void write_boot_info(std::span<const uint8_t> header, std::span<const uint8_t> bi2, std::span<const uint8_t> fst) {
    std::memcpy(rt::guest_ptr(0x80000000u), header.data(), 0x20);  // the disc ID, to its magic (C2339F3D at +0x1C)
    rt::poke32(0x80000028u, kMemSize);     // physical memory
    rt::poke32(0x8000002Cu, 0x00000003u);  // console type: retail, HW2 (OSGetConsoleType, 800E5A00, returns it)
    rt::poke32(0x800000CCu, 0);            // video mode: NTSC
    rt::poke32(0x800000D0u, 0x01000000u);  // ARAM: 16 MB
    rt::poke32(0x800000F8u, 162000000u);   // bus clock
    rt::poke32(0x800000FCu, 486000000u);   // CPU clock

    const uint32_t fst_max = be32(header, 0x42C);  // the disc header's FSTMaxLength
    if (fst.size() > fst_max || fst_max > 0x00400000u)
        rt::fatal(nullptr, "the disc's file table (%zu bytes) doesn't fit its header's limit (%u)", fst.size(), fst_max);
    rt::poke32(0x800000E8u, be32(bi2, 0));                     // debug monitor size (0)
    rt::poke32(0x800000ECu, (kMemEnd - be32(bi2, 0)) & ~31u);  // debug monitor address
    const uint32_t simulated = be32(bi2, 4) ? be32(bi2, 4) : kMemSize;
    if (simulated != kMemSize)
        rt::fatal(nullptr, "BI2 asks for %08X bytes of simulated memory: only a retail console's %08X is modelled", simulated, kMemSize);
    rt::poke32(0x800000F0u, simulated);
    const uint32_t fst_at = (kMemEnd - fst_max) & ~31u, bi2_at = fst_at - 0x2000u;
    std::memcpy(rt::guest_ptr(fst_at), fst.data(), fst.size());
    std::memcpy(rt::guest_ptr(bi2_at), bi2.data(), 0x2000);
    rt::poke32(0x800000F4u, bi2_at);
    rt::poke32(0x80000020u, 0x0D15EA5Eu);  // booted from a disc by the apploader
    rt::poke32(0x80000024u, 1);
    rt::poke32(0x80000030u, 0);        // arena low: none, so OSInit uses the linker's
    rt::poke32(0x80000034u, fst_at);   // arena high
    rt::poke32(0x80000038u, fst_at);   // the file table
    rt::poke32(0x8000003Cu, fst_max);
    rt::poke32(0x800030D8u, 0);  // system time's offset from the time base (64 bits)
    rt::poke32(0x800030DCu, 0);
    rt::poke16(0x800030E4u, 0);  // controller 4's buttons at boot (__check_pad3): none held
}

}  // namespace

void load_game(const std::filesystem::path& folder) {
    set_game_folder(folder);
    const auto dol_path = folder / "sys" / "main.dol";
    auto dol = qilin::Dol::load(dol_path);
    if (!dol) rt::fatal(nullptr, "%s: %s", dol_path.string().c_str(), dol.error().c_str());
    const qilin::KnownBuild* known = qilin::known_build(dol->sha256);
    if (!known || known->disc_id != build::kDiscId)
        rt::fatal(nullptr, "%s isn't the supported build (Mario Kart: Double Dash!! USA, GM4E01 revision 0): SHA-256 %s",
                  dol_path.string().c_str(), dol->sha256.c_str());
    // The image covers every section and the BSS; bytes outside the sections are zero.
    std::memcpy(rt::guest_ptr(dol->base()), dol->image().data(), dol->image().size());

    const auto header_path = folder / "sys" / "boot.bin";
    const std::vector<uint8_t> header = read_file(header_path);
    if (header.size() < 0x440 || std::memcmp(header.data(), build::kDiscId, 6) != 0 || header[7] != 0)
        rt::fatal(nullptr, "%s isn't the header of Mario Kart: Double Dash!!'s USA disc (GM4E01, revision 0)", header_path.string().c_str());
    const std::vector<uint8_t> bi2 = read_file(folder / "sys" / "bi2.bin");
    if (bi2.size() != 0x2000) rt::fatal(nullptr, "%s should be 8192 bytes", (folder / "sys" / "bi2.bin").string().c_str());
    write_boot_info(header, bi2, read_file(folder / "sys" / "fst.bin"));
}

[[noreturn]] void start_game() {
    namespace b = build;
    rt::Cpu c = rt::new_cpu();
    rt::set_thread_cpu(&c);  // for a crash report's call stack
    init_threads(c);
    // __init_registers: the stack and the two small-data bases.
    c.r[1] = b::kStackTop;
    c.r[2] = b::kSdaBase2;
    c.r[13] = b::kSdaBase;
    // __init_hardware (FP on, paired singles on, FPRs and caches cleared) has
    // nothing left to do: the Cpu starts zeroed, with both on.
    // The outermost frame: a back chain and LR of -1 end the stack.
    c.r[1] -= 8;
    rt::poke32(c.r[1], 0xFFFFFFFFu);
    rt::poke32(c.r[1] + 4, 0xFFFFFFFFu);
    // __init_data: copy each ROM section to its address (none moves in a DOL),
    // zero each BSS range.
    for (uint32_t e = b::kRomCopyInfo; rt::peek32(e + 8) != 0; e += 12) {
        const uint32_t rom = rt::peek32(e), to = rt::peek32(e + 4), size = rt::peek32(e + 8);
        if (rom != to) std::memmove(rt::guest_ptr(to), rt::guest_ptr(rom), size);
    }
    for (uint32_t e = b::kBssInitInfo; rt::peek32(e + 4) != 0; e += 8)
        std::memset(rt::guest_ptr(rt::peek32(e)), 0, rt::peek32(e + 4));
    rt::poke32(0x80000044u, 0);  // the debugger's exception mask
    // BI2's debug flag is 0 on a retail disc (no MetroTRK) and it gives no
    // arguments: checked, since the rest of __start is skipped on that account.
    const uint32_t bi2 = rt::peek32(0x800000F4u);
    if (rt::peek32(bi2 + 0x0C) != 0 || rt::peek32(bi2 + 8) != 0)
        rt::fatal(&c, "BI2 asks for a debugger (%u) or passes arguments (at %u): not a retail disc", rt::peek32(bi2 + 0x0C),
                  rt::peek32(bi2 + 8));
    rt::call(c, b::kDBInit);
    rt::call(c, b::kOSInit);
    // __check_pad3 resets the console when controller 4 holds a button
    // combination at boot (0x800030E4): none is held.
    rt::call(c, b::kInitUser);  // static constructors
    c.r[3] = 0;                 // argc
    c.r[4] = 0;                 // argv
    rt::call(c, b::kMain);
    rt::call(c, b::kExit);  // exit(main's result)
    rt::fatal(&c, "exit returned");
}

}  // namespace mkdd
