# main.dol: analysis (roadmap step 1)

Last updated 2026-11-25 (the US `main.dol` run through every tool: hash, sections, start-up constants, functions and coverage; what's linked in, from the linker map on the disc).

## Getting main.dol

The tools read `main.dol` (`sys/main.dol` of the extracted disc) and the linker map the disc carries (`files/debugInfoS.MAP`). Nothing from the game goes into git (`.gitignore` covers it).

```sh
build/qilin info    path/to/main.dol            # SHA-256, entry point, sections, BSS (JSON)
build/qilin profile path/to/main.dol            # instruction profile of the text sections
build/qilin insns   path/to/main.dol -o main.insns.txt
build/qilin cfg     path/to/main.dol --json cfg.json   # functions, jump tables, summary
build/qilin coverage path/to/main.dol --list           # every word: code, padding, dead or unknown
build/qilin suspects path/to/main.dol                  # and the rejected starts
python3 tools/analysis/port_symbols.py --map path/to/debugInfoS.MAP --dol path/to/main.dol --qilin build/qilin -o symbols.txt
```

## The supported build

| | |
|---|---|
| Disc | `GM4E01`, **revision 0**: *Mario Kart: Double Dash!!*, USA (`sys/boot.bin` starts `GM4E01`) |
| `main.dol` SHA-256 | `e96b8578451b9157e2b68fe5e918ebb572940c3ea54d6c8c7d45c24382bf12ae` |
| `main.dol` size | 3,572,640 bytes |
| Entry point | `0x80003154` (`__start`) |

## Sections

From `qilin info`. The names are those of the map's memory map, in the same order, and `__init_data`'s own tables agree: its ROM copy table lists these eleven sections with their exact sizes, its BSS table the three BSS ranges.

| Slot | Address | Size (bytes, exact) | Section |
|---|---|---|---|
| text0 | `0x80003100` | `0x24F4` | `.init`: start-up code, `memset`/`memcpy`, the debugger stub's exception vectors, `__init_data`'s tables |
| data0 | `0x80005600` | `0x48` | `extab` (C++ exception actions) |
| data1 | `0x80005660` | `0x5C` | `extabindex` (function → exception table; `_eti_init_info` at `0x8000569C`) |
| text1 | `0x800056C0` | `0x2D2E58` | `.text` |
| data2 | `0x802D8520` | `0x5C4` | `.ctors`: 368 static initialisers and a terminating zero |
| data3 | `0x802D8B00` | `0x10` | `.dtors` |
| data4 | `0x802D8B20` | `0x4A27A` | `.BINARY`: an archive linked into the DOL (`_pSystemArc`), and the build-time data at `0x80322D80` (`_pBuildtimeDat`) |
| data5 | `0x80322DA0` | `0x1A031` | `.rodata` |
| data6 | `0x8033CDE0` | `0x271B8` | `.data` |
| (bss) | `0x80363FA0` | `0x65470` | `.bss` |
| data7 | `0x803C9420` | `0x1720` | `.sdata` |
| (bss) | `0x803CAB40` | `0x1A4C` | `.sbss` |
| data8 | `0x803CC5A0` | `0x5CC8` | `.sdata2` |
| (bss) | `0x803D2280` | `0x74` | `.sbss2` |

The DOL header's single BSS range (`0x80363FA0` + 451,412) spans `.bss` to the end of `.sbss2`, over `.sdata` and `.sdata2`; `__init_data` clears only the three real ranges.

## Start-up constants

Read from the code (`qilin insns`); `mkdd/src/build.hpp` carries them.

| | | Where |
|---|---|---|
| `__start` | `0x80003154` | calls `__init_registers`, `__init_hardware`, `__init_data`, then `DBInit` (`800F0714`), `OSInit` (`800E5B8C`), the debugger checks, `__init_user` (`800EC7B8`), `main` (`801CC6A8`, with `argc`/`argv` in r14/r15 from the BI2 at `0x800000F4`), and jumps to `exit` (`800FFE28`) |
| `_stack_addr` (r1) | `0x803E22F8` | `__init_registers` (`800032B0`), `lis`/`ori` at `80003324`. The stack is the 64 KiB after `.sbss2` (`_stack_end` `0x803D22F4`), its top aligned to 8 |
| `_SDA2_BASE_` (r2) | `0x803D45A0` | at `8000332C`: `.sdata2` + `0x8000` |
| `_SDA_BASE_` (r13) | `0x803D1420` | at `80003334`: `.sdata` + `0x8000` |
| `_rom_copy_info` | `0x80005544` | read by `__init_data` (`80003340`) at `80003358`: eleven {rom, address, size} entries and a zero one; every rom address equals its address, so nothing is copied |
| `_bss_init_info` | `0x800055D4` | read at `800033AC`: {address, size} for `.bss`, `.sbss`, `.sbss2`, then zero |
| `__ArenaLo` | `0x803E42F8` | by the debug map's layout (the debugger's 8 KiB stack follows the main stack); not read from this DOL's code: `OSInit` sets the real arena |

The debugger checks in `__start`: it clears `0x80000044`, takes the BI2 pointer from `0x800000F4` (or the arena top from `0x80000034`) to find the debugger flag at `0x800030E8`, calls `InitMetroTRK` (`80110DC0`) through LR if a debugger is present, reads pad 3 for the reset combination (`__check_pad3`, `80003100`) and may start the Broadband Adapter debugger link (`InitMetroTRK_BBA`, `80110E58`). None of it applies to a recompilation: the start-up is replaced (`code-analysis.md`).

## What's linked in

The map names every object file and the library it came from, and `port_symbols.py` carries that over to this DOL (`code-analysis.md` has the method, the retail link order and the table).

- **`Dolphin.a`**: the SDK (base, mtx, dvd, vi, ai, ar/arq, dsp, card, gx, exi, gd, os, pad, si, db, thp), its network stack (ip, eth, upnp: the game's LAN mode over the Broadband Adapter), the Logitech steering-wheel force-feedback library, the debugger links (amcstubs, odenotstub, OdemuExi2), MSL, the EABI runtime and MetroTRK, all in one archive.
- **`JSystemV.a`**: Nintendo's JSystem: J3DU, JKernel, JSupport, JGadget, JUtility, JMath, J2DGraph, J3D, JFramework, JParticle, JAudio (JAInterface, JASystem with its own DSP task driver, JAUtility). No function of JHostIO was found in the retail build.
- **The game**, one library per programmer: Bando, Inagaki, Kameda, Kaneshige, Kawano, Osako, Sato, Shiraiwa, Yamamoto, and `kartLocale.o`.

## Instructions

`qilin profile`: 742,616 words in the text sections, 164 distinct instructions. The most common are `addi` (16.0%), `lwz` (11.1%), `b` (8.3%), `stw` (6.7%) and `bc` (6.6%). Paired singles are everywhere in the game's own code (`psq_l` alone is 1.4% of all words; the per-library counts are in `code-analysis.md`): the compiler was allowed to use them for vector maths, so they're not confined to the SDK's matrix library.

## Functions and coverage

`qilin cfg`: **13,471 functions** (1 entry, 7,162 call targets, 6 reached only by tail calls, 591 only through `lis`/`addi` pointers, 5,676 only through pointers in data such as vtables, 35 found in gaps), 740,188 reachable instructions, 298 jump tables, 5,191 indirect calls. 42% of the functions are reached only through data: JSystem and the game are C++ built on virtual calls.

`qilin coverage`, by word:

| Section | Code | Padding | Dead | Unknown |
|---|---|---|---|---|
| `.init` (2,368 words) | 689 | 1,628 | 0 | 51 |
| `.text` (740,248 words) | 739,499 (99.90%) | 84 | 113 | 552 |

The unknown words:

| Where | Words | What |
|---|---|---|
| `0x800035E4` | 12 | The start of the debugger stub's vector table (its name string, by the Wii base's finding; not read here) |
| `0x80005544` | 33 | `_rom_copy_info` |
| `0x800055D4` | 6 | `_bss_init_info` |
| `0x80110DC0`, `0x80110EB0` | 53 | `InitMetroTRK` and the debugger stub's register restore: dropped code |
| `0x800BC398` | 125 | **The cases of a switch in `GXSetVtxDescv` (`800BC36C`)** |
| `0x800BCCFC` | 124 | **The cases of a switch in `GXSetVtxAttrFmtv` (`800BCCAC`)** |
| `0x800C5B64` | 124 | **The cases of a switch in `GDSetVtxDescv` (`800C5AEC`)** |
| `0x800D4A6C` | 126 | **The cases of a switch in `DHCPProcessOptions` (`800D4924`)** |

The last four are code the base tool does not reach. Each function starts with `cmpli`, `bc gt`, `rlwinm`, `lwzx r10,r5,r10`, `mtctr`, `bctr`, the table's address already in r5 from further up (`800BCCE0`-`800BCCF8` in `GXSetVtxAttrFmtv`); `qilin cfg` reports the `bctr` as an indirect tail call (`indirect_tails` in its JSON) instead of a jump table, so the case bodies are not lifted. All four functions are in lifted libraries (GX, GD, ip): the recompiler's jump-table rules need this shape before they can run (`recompiler-core.md`). Who calls the two GX functions was not checked.

**Two suspects** (`qilin suspects`), both in replaced or dropped code: `__OSDBJump` (`800E6208`) is a branch to the debugger's hook in low memory (`0x60`); `InitMetroTRK_BBA` (`80110E58`) runs into the stub's register restore.

Between `800A1A48` and `800A23E0` fourteen short runs of zero words pad JAudio's DSP driver functions (aligned to 32 bytes).
