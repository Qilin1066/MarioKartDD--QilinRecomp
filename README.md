# MarioKartDD--QilinRecomp

A static recompilation of **Mario Kart: Double Dash!!** (Nintendo, GameCube, 2003) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs are to run it through MoltenVK (macOS 10.15 or later).

> **No game data is included, and none ever will be.** You need your own copy of the game: the USA release, disc `GM4E01`, revision 0. You point the game at it the first time you play.

## Status

**In development: the game does not run yet.** The project was started from the author's Punch-Out!! (Wii) recompilation and is being converted to the GameCube and to this game part by part. What is listed as done below is tested without the game (see [Building](#building)); nothing here is a claim about playing it.

| Part | State |
|---|---|
| Disc reader | ✅ `.iso`, `.gcm`, `.ciso` images and extracted folders, read in place (`docs/disc.md`) |
| Recompiler (decoder, function discovery, lifter) | ✅ for this DOL: 13,472 functions found, 12,754 lifted to C++ with no stubs, the rest replaced or dropped by the plan (`docs/recompiler-core.md`, `docs/dol-analysis.md`) |
| Names and the library plan (`data/GM4E01/plan.tsv`) | ✅ names ported from the disc's own map of its debug build; what is lifted, replaced or dropped, library by library (`docs/code-analysis.md`) |
| DSP microcode | ✅ the game's own sound-processor program, recompiled to C++ from the player's DOL at build time (`qilin-dsp`, `docs/audio.md`) |
| Controllers, memory cards, audio RAM | 🔨 written and tested against the SDK's interfaces, not against the running game (`docs/input.md`, `docs/saves.md`) |
| Renderer (GX on Vulkan) | 🔨 adapted to the GameCube and to what this game draws, tested on recorded commands, not with the game (`docs/graphics.md`) |
| Sound | 🔨 the DSP and its microcode tested apart from the game (`docs/audio.md`) |
| Runtime: OS, disc, video, threads, start-up | 🔨 in development (`docs/runtime.md` still describes parts of the Wii project it came from) |
| Window, settings menu, picture, sound output | 🔨 written; tested offscreen and by unit tests only (below) |
| Wider pictures, higher frame rates | 🔨 written from an analysis of the game's code (`docs/enhancements.md`, `docs/graphics.md`), tested apart from the game; not seen with it |
| The game, boot to credits | ❌ not yet |

## What it is meant to be

The same requirements and scope as the author's other recompilations (Grabbed by the Ghoulies, Punch-Out!!, Beat the Beat): the game's own program translated ahead of time into C++ (no interpreter, no JIT, no emulator underneath), the console's libraries that touch hardware replaced natively, one Vulkan 1.3 renderer for every platform, and a settings menu over the game. [`docs/roadmap-and-design-targets.md`](docs/roadmap-and-design-targets.md) has the targets and the plan.

## Playing

None of this plays the game yet; it is how it will be started, and the launchers already do what they say up to the point where the game itself would run.

Your disc can be an image as it is, `.iso`, `.gcm` or `.ciso` (GameCube discs aren't encrypted, so nothing has to be extracted first), or a folder extracted from one (the folder holding `sys` and `files`). Compressed formats aren't read: convert an `.rvz` or `.gcz` to ISO with Dolphin (right-click the game, Convert File); an NKit image has to be converted back to the original with NKit. The launcher says which it is and what to do.

### Windows download

Each tagged release will have `MarioKartDD--QilinRecomp-<version>-windows-x64.zip` attached (`.github/workflows/player-kit.yml`). Unpack it and double-click **`Play Double Dash.bat`**: the first time, it asks for your disc and builds the game from it with the bundled compiler (LLVM-MinGW); after that it starts the game at once. `-ChooseDisc` picks another disc.

### From the source on Windows

With Visual Studio 2022 (17.10 or newer, with "Desktop development with C++") and Git installed, in the project's folder:

- **`Play Double Dash.bat`** starts the game. The first time it builds it (10 to 40 minutes), asking for your disc.
- **`Update Double Dash.bat`** pulls the latest code, rebuilds what changed and starts the game.

Both run `tools/build-mkdd.ps1`.

### Linux and macOS

```sh
./mkdd.sh --disc path/to/your/disc.iso    # the first time: builds, then starts
./mkdd.sh                                 # after that
./mkdd.sh update                          # git pull, rebuild what changed, start
```

### By hand

```sh
cmake -S . -B build-game -DCMAKE_BUILD_TYPE=Release
cmake --build build-game --target qilin_disc_tool
build-game/qilin-disc info path/to/your/disc.iso            # GM4E01, revision 0, and its main.dol's SHA-256
build-game/qilin-disc dol  path/to/your/disc.iso build-game/main.dol
cmake -S . -B build-game -DQILIN_GAME_DOL=$PWD/build-game/main.dol
cmake --build build-game --parallel --target mkdd           # lifts the game's code and its DSP microcode, then compiles
build-game/mkdd path/to/your/disc.iso
```

### Controls

Four controller ports, each a GameCube controller. Controllers take the ports in the order each first has a button pressed (or as set in the menu), and keep them while unplugged; the keyboard plays the first port. Everything can be changed in the settings menu by pressing the new input, for each player.

| GameCube controller | Controller (an Xbox pad's names) | Keyboard (player 1) |
|---|---|---|
| A | A (the bottom button) | X |
| B | X (the left button) | Z |
| X | B (the right button) | C |
| Y | Y (the top button) | S |
| Z | RB or LB | D |
| L, R (analogue, with their click) | LT, RT | A, Space |
| Start | Start | Enter |
| Control stick | Left stick | Arrow keys |
| C-stick | Right stick | I, K, J, L |
| D-pad | D-pad | T, G, F, H |

The pad's buttons go by where they are, not by their letters: the GameCube's B is beside its large A, where an Xbox pad has X.

| Key | What |
|---|---|
| F1 or Esc; Back + Start on a controller | The settings menu (the game pauses) |
| F3 | Frame times, top left |
| F9 | Capture the next frames for a bug report (`captures/` in the data folder) |
| F11 or Alt+Enter | Full screen |

### Settings

The menu has three pages, as in the author's other recompilations.

- **Controls:** which controller plays which port, rumble for each, every control's inputs (keys, a pad's buttons, sticks and triggers, the mouse), the sticks' dead zone, where the triggers click, rumble strength.
- **Display:** full screen and on which display; the picture's size (whole multiples, fit, fill); render resolution (following the window, or 1x to 8x of 640x480); SMAA anti-aliasing; AMD FSR 1 upscaling with its sharpness; a plain or CRT look; vsync and low latency; presets; anisotropic filtering; true colour; the graphics device; frame times.
- **Game:** volume, sound output (stereo, or 5.1 decoded from the game's Dolby Surround) and device; the console's own settings (sound, progressive scan); previous saves.

Two groups of options change the game itself and have not been seen working with it yet: a picture wider than 4:3 (aspect ratio, HUD placement; `docs/enhancements.md`) and frame rates above the game's own 60, which are pictures drawn between its frames (`docs/graphics.md`, `docs/runtime.md`).

The settings (`mkdd.ini`, editable by hand), the saves, captures, the pipeline cache and the log live in `qilin/mkdd` in your per-user application data (Windows: `%LOCALAPPDATA%\qilin\mkdd`). Send a capture (F9), or `mkdd.log`, with any bug report.

## Building

The whole project (recompiler, runtime and generated code) is C++23. You need CMake 3.25 or newer and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

CMake stops with a message if the compiler is older or the target is 32-bit. It fetches the rest at pinned versions and builds it in: SDL3, the Vulkan headers, volk, Dear ImGui, and the few source files of SMAA and AMD FSR 1 that the picture's passes are built from. Running needs a Vulkan 1.3 driver.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Without game data the build links every runtime source into `mkdd_runtime_check`, a build check rather than a playable program; the tests need no game files. One test opens a real window (controllers through SDL) and so runs only when asked: `QILIN_WINDOW_TESTS=1`. The picture's passes (scaling, the CRT look, SMAA, FSR 1, captures) are tested by drawing offscreen and reading the pixels back, where there is a Vulkan 1.3 device. `-DQILIN_SANITIZE=address,undefined` builds with the sanitizers (GCC, Clang).

The shaders are committed as SPIR-V, so no shader compiler is needed. After changing one, run `tools/build_shaders.py`; it uses a `glslangValidator` on the PATH, or the one `-DQILIN_SHADER_COMPILER=ON` builds (target `qilin_shader_compiler`).

## The tools

Each reads the player's own disc or its `main.dol`; none of their output belongs in the repository.

```sh
build/qilin-disc info|list|extract|dol|verify <image or folder> ...   # the disc (docs/disc.md)
build/qilin info     path/to/main.dol [--json info.json]   # SHA-256, supported build, entry point, sections
build/qilin insns    path/to/main.dol [-o listing.txt]     # every word of the code, decoded
build/qilin cfg      path/to/main.dol [--json cfg.json]    # function discovery and control flow
build/qilin coverage path/to/main.dol [--list]             # every word: code, padding, data or unknown
build/qilin suspects path/to/main.dol                      # functions with problems, for review
build/qilin lift     path/to/main.dol -o generated --plan data/GM4E01/plan.tsv   # the game as C++
build/qilin-dsp lift path/to/main.dol -o dsp_ucode.cpp     # the DSP microcode in it as C++ (docs/audio.md)
```

`tools/analysis/port_symbols.py` ports the names in the disc's map (of the debug build) to the retail DOL, and `tools/analysis/make_plan.py` writes or checks `data/GM4E01/plan.tsv` from them (`docs/code-analysis.md`); `hardware_access.py` lists the code's hardware accesses by library (`docs/runtime-analysis.md`); `tools/check/verify_decoder.py` compares the decoder with GNU objdump.

## Layout

| Path | What |
|---|---|
| `recomp/` | The recompiler: DOL loader, decoder, function discovery, lifter, the `qilin` tool; the DSP microcode's lifter (`qilin-dsp`); their tests |
| `disc/` | The disc reader and the `qilin-disc` tool |
| `runtime/` | The guest CPU state, the guest address space, system and error reporting for lifted code |
| `host/` | The platform layer: window, Vulkan presentation and the picture's passes, controllers, sound output, captures, paths |
| `mkdd/` | The game's runtime: OS, disc, video, controllers, memory cards, the GX renderer, sound, settings and the menu |
| `data/GM4E01/` | `plan.tsv`: what is lifted, replaced or dropped, library by library |
| `tools/` | Checks, analysis scripts, the shader build, the Windows download's packaging, launcher tests |
| `docs/` | Design targets and roadmap, naming, prior art, and a note for each part |

## Versions and changes

Changes are recorded in [`CHANGELOG.md`](CHANGELOG.md). The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. 1.0.0 will be the first build that plays the whole game.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0). The dependencies CMake fetches keep their own: SDL3 (zlib), the Vulkan headers (Apache-2.0 or MIT), volk (MIT), Dear ImGui (MIT), SMAA (MIT; Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez) and AMD FidelityFX FSR 1 (MIT). The last two are also in the repository in compiled form, as SPIR-V (`host/src/post_shaders.inc`). The Windows download carries each licence under `LICENSES/`.

Mario Kart: Double Dash!! itself, its code and data, belong to Nintendo and are not covered by anything here: you need your own copy, and nothing built from it may be shared.
