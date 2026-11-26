# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

The project is in development: the game does not run yet. It was started from PunchOutWii--QilinRecomp v0.22.0 and is being converted to the GameCube and to Mario Kart: Double Dash!! (USA, `GM4E01`, revision 0).

### Added

- The recompiler for the GameCube and this DOL: the Gekko build of the decoder and lifter, CodeWarrior's switch shapes of this compiler version, data inside the text sections, the GameCube's memory map; every function of the game lifted or accounted for by the plan, and the game's own functions in the differential test (`docs/recompiler-core.md`).
- Names for the retail DOL's functions and data, ported from the disc's own linker map of its debug build (`tools/analysis/port_symbols.py`, `port_data.py`), and the library plan made from them (`tools/analysis/make_plan.py`, `data/GM4E01/plan.tsv`, `docs/code-analysis.md`).
- The game's DSP microcode statically recompiled: `qilin-dsp` lifts it from the player's DOL to C++ at build time, and it runs on a model of the DSP (`docs/audio.md`). The Windows download ships `qilin-dsp` and builds that file with the rest.
- The renderer for the GameCube and this game: its memory and FIFO, primitives of one state joined into one draw, depth copies as the game makes them, Z before texturing with the alpha test (`docs/graphics.md`).
- An analysis of the game's frame timing and picture shape from its code, with the design for higher frame rates and wider pictures (`docs/enhancements.md`).
- The render resolution follows the window while the game runs (and a change in the menu applies when it closes): the renderer's frame buffer is made again at the new scale between two frames.
- A value in `mkdd.ini` may be written in double quotes, which keeps a `#` in it (a display or sound device called "Speakers #2"); the program writes such values that way.
- The disc is read where it lies: `.iso`, `.gcm` and `.ciso` images and extracted folders, through the `qilin-disc` tool and its library (`docs/disc.md`). The launchers (`Play Double Dash.bat`, `mkdd.sh`) and the Windows download take an image or a folder, check it is the supported game, and take `main.dol` from it.
- Four controller ports, each a GameCube controller (`mkdd/src/input.hpp`): any SDL3 controller or the keyboard, with every control (the sticks' directions and the analogue triggers included) set per player by pressing it; controllers assigned to ports automatically or by name, remembered, with their places kept while unplugged; a dead zone for the sticks, a threshold for the triggers' click, rumble per player. `MKDD_INPUT` to `MKDD_INPUT4` script a port for runs without a player.
- GameCube controllers, memory cards (as `.gci` files, with a history of previous saves) and audio RAM in place of the Wii's remotes and NAND (`docs/input.md`, `docs/saves.md`).
- The settings file `mkdd.ini` and the settings menu (F1 or Esc; Back + Start) for this game: `[display]`, `[sound]`, `[controls]`, `[player1]` to `[player4]` and `[console]` (the GameCube's own: sound mode, progressive scan).
- SMAA 1x anti-aliasing and AMD FSR 1 upscaling (EASU and RCAS, with a sharpness setting) before the picture is shown, as in GBTGhoulies--QilinRecomp.
- Sound output as stereo or as 5.1 decoded from the game's Dolby Surround signal, a choice of playback device, and the volume in its own `[sound]` section.
- F9 captures the next frames for a bug report: the pictures as PNG, the log and the settings in one tar under `captures/` in the data folder.
- A low-latency switch (at most one frame waiting for the display, where the graphics driver can wait for a frame to reach the screen).
- Settings and menu entries for a picture wider than 4:3 (aspect ratio, HUD placement) and for frame rates above the game's own. They do nothing yet and the menu says so: both need the game's code patched (`mkdd/src/enhance.hpp`, `docs/roadmap-and-design-targets.md`).
- `-DQILIN_SHADER_COMPILER=ON` builds a pinned glslang for `tools/build_shaders.py`, so regenerating the committed SPIR-V needs nothing installed.

### Changed

- The picture is the GameCube's 640x480 at 4:3 (the Wii project assumed 16:9); the default window is 960x720.
- Frame pacing is for the console's 59.94 Hz: the retraces follow the display where its refresh is a whole multiple of that to within half a percent (59.94, 60, 119.88, 120, 180, 240 Hz, up to eight times), and otherwise run at 59.94 Hz by the clock.
- The render resolution's "auto" goes by the window's size, not the screen's.
- The Windows download builds the game as `work\mkdd.exe` and copies it to `Double Dash.exe` (Ninja splits a target's name at a space).
- The test that opens a window (`qilin_padtest`) runs only when asked (`QILIN_WINDOW_TESTS=1`, or `QILIN_REQUIRE_GRAPHICS=1` as CI sets under a virtual display), so a plain `ctest` opens nothing.

### Fixed

- CI compiled an analysis script that no longer exists.

### Removed

- The Wii Remote: its sideways mapping, its pointer (mouse and right stick) and its speaker; the Wii's console settings (language list, widescreen); the migration of settings and data folders from older versions of the Wii project.
