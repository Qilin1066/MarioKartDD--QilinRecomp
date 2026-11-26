// Addresses in the supported build: Mario Kart: Double Dash!! (USA), GameCube,
// disc GM4E01 revision 0, main.dol SHA-256 e96b8578... (recomp/src/dol.cpp).
// Everything that names a place in the game's code or data is here, found by
// reading the code (docs/runtime.md says how each one was found).
#pragma once

#include <cstdint>

namespace mkdd::build {

inline constexpr char kDiscId[] = "GM4E01";

// __start (80003154) and what it calls.
inline constexpr uint32_t kStackTop = 0x803E22F8u;     // _stack_addr, set by __init_registers (800032B0)
inline constexpr uint32_t kStackEnd = 0x803D22F4u;     // _stack_end: the end of the BSS (__OSThreadInit, 800EB098)
inline constexpr uint32_t kSdaBase2 = 0x803D45A0u;     // r2, _SDA2_BASE_
inline constexpr uint32_t kSdaBase = 0x803D1420u;      // r13, _SDA_BASE_
inline constexpr uint32_t kRomCopyInfo = 0x80005544u;  // __rom_copy_info: {rom, address, size}, size 0 ends (__init_data, 80003340)
inline constexpr uint32_t kBssInitInfo = 0x800055D4u;  // __bss_init_info: {address, size}, size 0 ends
inline constexpr uint32_t kDBInit = 0x800F0714u;       // replaced (os.cpp)
inline constexpr uint32_t kOSInit = 0x800E5B8Cu;       // replaced (os.cpp)
inline constexpr uint32_t kInitUser = 0x800EC7B8u;     // __init_user: the static constructors (lifted)
inline constexpr uint32_t kMain = 0x801CC6A8u;         // main(argc, argv)
inline constexpr uint32_t kExit = 0x800FFE28u;         // exit(status): destructors, then _ExitProcess and PPCHalt (lifted)

// What OSInit (800E5B8C) calls in libraries with replacements of their own.
inline constexpr uint32_t kEXIInit = 0x800C4B00u;
inline constexpr uint32_t kSIInit = 0x800EEFF4u;
inline constexpr uint32_t kDVDInit = 0x800ABE14u;
inline constexpr uint32_t kInitAudioSystem = 0x800E6DD0u;  // __OSInitAudioSystem: lifted, over the DSP's registers (dsp.cpp)

// OS globals (small data, r13-relative), from the code that reads them.
inline constexpr uint32_t kArenaLo = kSdaBase - 32080;          // __OSArenaLo (OSGetArenaLo, 800E6DB8: lifted)
inline constexpr uint32_t kArenaHi = kSdaBase - 24736;          // __OSArenaHi (OSGetArenaHi, 800E6DB0: lifted)
inline constexpr uint32_t kBootInfo = kSdaBase - 24824;         // BootInfo: 0x80000000 (OSInit)
inline constexpr uint32_t kBi2DebugFlag = kSdaBase - 24820;     // BI2DebugFlag: a pointer to BI2's debug flag (OSInit)
inline constexpr uint32_t kStartTime = kSdaBase - 24768;        // __OSStartTime, 64 bits (OSInit)
inline constexpr uint32_t kPadSpec = kSdaBase - 24576;          // __PADSpec, from BI2 +0x24 (OSInit)
inline constexpr uint32_t kFpscrEnableBits = kSdaBase - 32072;  // __OSFpscrEnableBits (OSCreateThread, 800EB880)
inline constexpr uint32_t kErrorTable = 0x803755B0u;            // __OSErrorTable[17] (OSCreateThread reads [16], 800EB848)

// The arena starts after the debugger's stack, as the linker placed it, when
// low memory gives no arena and no debugger is attached (OSInit, 800E5C9C).
inline constexpr uint32_t kArenaLoValue = 0x803E4300u;

// OSThread.c's own data (__OSThreadInit, 800EB010).
inline constexpr uint32_t kDefaultThread = 0x80375A90u;  // DefaultThread
inline constexpr uint32_t kOSExitThread = 0x800EB968u;   // a new thread's return address (OSCreateThread, 800EB80C)

// OSRtc.c's copy of the console's SRAM and its bookkeeping (__OSInitSram, 800EA570):
// 64 bytes, then offset +0x40, enabled +0x44, locked +0x48, sync +0x4C.
inline constexpr uint32_t kSramControl = 0x80375620u;

}  // namespace mkdd::build
