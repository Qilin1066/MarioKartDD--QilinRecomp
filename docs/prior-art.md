# Prior art

What this project builds on or consults, and under which rule. Two rules hold throughout: no third-party code or text is copied into the repository (dependencies are fetched by CMake at pinned versions), and no game data, or anything derived from it, is committed.

## 1. Sibling projects (same author, GPL-3.0)

- **PunchOutWii--QilinRecomp** (Punch-Out!!, Wii). This project was started from its v0.22.0: the recompiler (DOL loader, PowerPC decoder, function discovery, lifter with its differential test), the guest runtime, the GX renderer on Vulkan, the host layer and the settings menu. The Wii and the GameCube share the CPU family, GX and most of the SDK's shape, which is why; what differs (no IOS, no Wii Remote, memory cards for NAND, the DSP's microcode, the disc) was replaced part by part for 0.1.0.
- **GBTGhoulies--QilinRecomp** (Grabbed by the Ghoulies, Xbox). The reference for scope and for the settings menu's options. Its present-pass shader wrappers for SMAA and FSR 1 (`recomp/runtime/gfx/smaa`, `gfx/fsr`), their Vulkan passes and its menu's wording were reused here (`host/src/post.cpp`, `host/shaders/post/`, `mkdd/src/menu.cpp`).
- **BeatTheBeat--QilinRecomp** (Wii): vblank steering in the main loop came from it by way of the Punch-Out project.

## 2. This game

- **The disc's own linker map**, `files/debugInfoS.MAP`: the map of the game's debug build, not of the retail DOL. Same link order and the same SDK library, so SDK functions line up by size; game and JSystem code differ. It names every function with its object file and library. It is game data: it is read from the player's disc by the analysis, and no list ported from it is committed.
- **doldecomp/mkdd** (CC0): a decompilation in progress. Used for names and structure layouts only, with a comment where code relies on it; none of its code is copied.

## 3. Dolphin

The reference for hardware facts (register layouts, formats, timing) and, running the original game, the ground truth for behaviour, picture and sound. Its code is not copied. Its disc tools are the player's way to convert a compressed image (`.rvz`, `.gcz`) to one this project reads (`disc.md`).

## 4. GNU binutils

The decoder is checked word by word against binutils' PowerPC disassembler (`tools/check/verify_decoder.py`).

## 5. Libraries built in (fetched, pinned)

| Library | Licence | For |
|---|---|---|
| SDL3 | zlib | window, events, controllers, sound output |
| Vulkan-Headers, volk | Apache-2.0 or MIT; MIT | Vulkan, loaded at run time (no SDK needed to build or run) |
| Dear ImGui | MIT | the settings menu |
| SMAA (Jimenez, Echevarria, Masia, Navarro, Gutierrez) | MIT | anti-aliasing: `SMAA.hlsl` and its area and search tables |
| AMD FidelityFX FSR 1 | MIT | upscaling: `ffx_a.h`, `ffx_fsr1.h` (the shaders, and their constants computed on the CPU) |
| glslang (only with `-DQILIN_SHADER_COMPILER=ON`) | BSD-3-Clause and others | regenerating the committed SPIR-V |

SMAA's and FSR's sources are fetched file by file, each checked against a SHA-256 (`host/shaders/post/third_party.txt`). Their shaders are committed only in compiled form (`host/src/post_shaders.inc`), built by `tools/build_shaders.py` from our own wrappers (`host/shaders/post/`) over the fetched sources.
