# MarioKartDD--QilinRecomp

A static recompilation of **Mario Kart: Double Dash!!** (Nintendo, GameCube, 2003) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs are to run it through MoltenVK (macOS 10.15 or later).

> **No game data is included, and none ever will be.** You need your own copy of the game: the USA release, disc `GM4E01`, revision 0. You point the game at it the first time you play.

## Status

The game plays, as far as scripted runs have taken it: from the disc through the opening movie, the title and every menu into races, a whole cup, time trials with ghosts, split screen and battle, with sound and memory cards. **None of it has yet been seen in a real window, heard, or played with a real controller.** Every check so far is a run without a window (`--headless`) or drawing without one (`--offscreen`), with input played by a script (`docs/input.md`, "Runs without a player"), read from logs, frame dumps and sound dumps on the developer's Windows PC. Expect faults that only someone playing will find.

| Part | State |
|---|---|
| Disc reader | ✅ `.iso`, `.gcm`, `.ciso` images and extracted folders, read in place (`docs/disc.md`) |
| Recompiler (decoder, function discovery, lifter) | ✅ every one of the DOL's 13,472 functions accounted for: 12,755 lifted to C++, 511 replaced, 206 dropped, 0 stubs; differential tests against a reference interpreter: 0 differences (`docs/recompiler-core.md`) |
| Names and the library plan (`data/GM4E01/plan.tsv`) | ✅ names ported from the disc's own map of its debug build; what is lifted, replaced or dropped, library by library (`docs/code-analysis.md`) |
| Runtime: OS, threads, disc, video | ✅ boots and runs every scene tried; a 15-minute unattended run without a stop and without memory growth (`docs/runtime.md`) |
| Memory cards | ✅ slots A and B as `.gci` files, with a history of previous saves; a card pulled out before or during a write behaves as on a GameCube (`docs/saves.md`) |
| Controllers | 🔨 four ports, SDL3 controllers and the keyboard; only scripted input so far (`docs/input.md`) |
| Renderer (GX on Vulkan) | 🔨 frame dumps from eight courses in one-, two- and four-view layouts reviewed, no confirmed fault; not yet compared with Dolphin or seen in a window (`docs/graphics.md`) |
| Sound | 🔨 the game's own DSP microcode statically recompiled; checked by numbers (no clipping, agrees with reference decodes), not yet listened to (`docs/audio.md`) |
| Wider pictures | 🔨 any aspect ratio, Hor+, with the HUD at the edges, in the middle or stretched; tested offscreen (`docs/enhancements.md`) |
| Frame rates above 59.94 | 🔨 pictures drawn between the game's frames; tested offscreen (`docs/graphics.md`) |
| Window, settings menu, sound output | 🔨 tested offscreen and by unit tests only |
| The game, boot to credits | 🔨 not yet played to the end |

**What the scripted runs did.** Booted from an image and from an extracted folder: the progressive-scan question, the opening movie, the title and every menu. A whole 50cc Mushroom Cup Grand Prix, with the award ceremony and the cup saved to the card. Time trials, with the record and the ghost saved; the saved ghost loaded and raced against, its replay matching the run that recorded it to a tenth of a unit for the whole race (the recompiled game is deterministic). Two players on a split screen, two in one kart, a four-player Versus race, a two-player Balloon Battle. Memory cards in slots A and B, the save history, and a card pulled out before and during a write.

**Wider pictures and higher frame rates.** The picture can be any shape: the game's cameras are widened (Hor+), the frame gets whole extra columns at full resolution rather than being stretched, and in the race surveyed nothing is still culled against the old 4:3 edges. Menus and movies stay 4:3 between bars. The game has no time step: its physics, timers and animation all move one fixed step a frame, 59.94 frames a second, and ghosts are recordings of those steps, so it cannot simply be run faster. Frame rates above 59.94 are instead pictures drawn between two of its frames with the positions blended, which costs one frame (17 ms) of delay in those modes; the game itself is unchanged. In every mode the runtime ends each frame as soon as the game has drawn it and reads the input just before the retrace, which cuts the delay from a press to the picture by about 16 to 27 ms (measured offscreen; `docs/runtime.md`, "Response time").

**Efficiency**, on a Ryzen 7 5800X: the game's threads work about 5.4 ms a frame in a one-view race; turning the game's GPU commands into Vulkan takes about 4.4 ms a frame in a four-player race. Nothing has been measured on a Raspberry Pi yet.

**Not done yet:**

- Looking at it, listening to it and playing it with a controller on a real display; comparing pictures and sound with Dolphin.
- The rest of the game: the other cups and classes, the All-Cup Tour, what is unlocked, the staff roll.
- LAN play (no Broadband Adapter is reported, so the game never starts its network code) and the steering wheel.
- The console's reset button (nothing sets it yet); the disc drive's errors and its cover.
- The pipeline cache starts empty, so a first run may pause while it builds its shaders.
- Split screen and courses other than the one surveyed have not been checked for culling at wide shapes; matching pictures between frames to their neighbours in a played race (items, Lakitu, cannons) is checked only on the demo races.
- Measuring on a Raspberry Pi 4.

**Platforms.** The game itself has been built and run only on Windows x64 with MSVC. CI (`.github/workflows/ci.yml`) builds everything except the game's own code (which needs the disc) and runs the tests on Linux x64 (GCC 14 and Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64, and Windows x64, with warnings as errors, and every test again on Linux under AddressSanitizer, LeakSanitizer and UndefinedBehaviorSanitizer (the threading tests under ThreadSanitizer too); the Windows download is cross-built and packaged by `.github/workflows/player-kit.yml`. The game has not been run on Linux, macOS or ARM64.

## What it is meant to be

The same requirements and scope as the author's other recompilations (Grabbed by the Ghoulies, Punch-Out!!, Beat the Beat): the game's own program translated ahead of time into C++ (no interpreter, no JIT, no emulator underneath), the console's libraries that touch hardware replaced natively, one Vulkan 1.3 renderer for every platform, and a settings menu over the game. [`docs/roadmap-and-design-targets.md`](docs/roadmap-and-design-targets.md) has the targets and the plan.

## Playing

The game's code is built on your own computer from your own disc, and never distributed. These are the ways to start it; of them, the source build on Windows (`Play Double Dash.bat`, or by hand) is the one that has been used. The Windows download and the Linux and macOS commands are written but not yet tried.

Your disc can be an image as it is, `.iso`, `.gcm` or `.ciso` (GameCube discs aren't encrypted, so nothing has to be extracted first), or a folder extracted from one (the folder holding `sys` and `files`). Compressed formats aren't read: convert an `.rvz` or `.gcz` to ISO with Dolphin (right-click the game, Convert File); an NKit image has to be converted back to the original with NKit. The launcher says which it is and what to do.

### Windows download

Each tagged release is to have `MarioKartDD--QilinRecomp-<version>-windows-x64.zip` attached, built by `.github/workflows/player-kit.yml` (which has not run yet). Unpack it and double-click **`Play Double Dash.bat`**: the first time, it asks for your disc and builds the game from it with the bundled compiler (LLVM-MinGW); after that it starts the game at once. `-ChooseDisc` picks another disc.

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

The console's reset button has no key by default (a stray press loses the race being played); give it one in the settings menu's Controls page, "Reset (console button)".

### Settings

The menu has three pages, as in the author's other recompilations.

- **Controls:** which controller plays which port, rumble for each, every control's inputs (keys, a pad's buttons, sticks and triggers, the mouse), the console's reset button, the sticks' dead zone, where the triggers click, rumble strength.
- **Display:** full screen and on which display; the picture's size (whole multiples, fit, fill); render resolution (following the window, or 1x to 8x of 640x480); SMAA anti-aliasing; AMD FSR 1 upscaling with its sharpness; a plain or CRT look; vsync and low latency; presets; anisotropic filtering; true colour; the graphics device; frame times.
- **Game:** volume, sound output (stereo, or 5.1 decoded from the game's Dolby Surround) and device; the console's own settings (sound, progressive scan, whether a component cable is plugged in); previous saves.

Two groups of options go beyond the console: a picture wider than 4:3 (aspect ratio, HUD placement; `docs/enhancements.md`) and frame rates above the game's own 59.94, which are pictures drawn between its frames (`docs/graphics.md`, `docs/runtime.md`). Both work in offscreen runs and have not yet been seen on a real display.

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

Releases are tagged `vX.Y.Z`, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. 1.0.0 will be the first build that plays the whole game.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0). The dependencies CMake fetches keep their own: SDL3 (zlib), the Vulkan headers (Apache-2.0 or MIT), volk (MIT), Dear ImGui (MIT), SMAA (MIT; Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, Diego Gutierrez) and AMD FidelityFX FSR 1 (MIT). The last two are also in the repository in compiled form, as SPIR-V (`host/src/post_shaders.inc`). The Windows download carries each licence under `LICENSES/`.

Mario Kart: Double Dash!! itself, its code and data, belong to Nintendo and are not covered by anything here: you need your own copy, and nothing built from it may be shared.
