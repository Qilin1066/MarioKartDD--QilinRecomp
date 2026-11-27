# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html). A minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix; 1.0.0 will be the first build that plays the whole game.

## [Unreleased]

## [0.1.2] - 2026-11-27

### Fixed

- Recorded frames and their frame buffers retain their own aspect ratio. A queued race picture no longer borrows the shape of a newer menu or movie; captures use the recorded shape too.
- A real-copy confirmation inherits the earlier picture's shape and does not advance camera settings a second time. Interpolation skips pairs recorded at different aspect ratios.
- Texture interpolation matches material-state sequences under each paired model load, with matching counts and image/palette bindings. A second material no longer scrolls towards the first material's texture transform; ambiguous or reordered materials keep their current transform.
- HUD elements in vertically split views are grouped only within the same clipped view. Oversized elements crossing the dividing line no longer pull another player's HUD away from its anchor.
- HUD callbacks are published atomically and sampled once per frame. Repeated settings updates no longer race the recording thread or change the callback between its null check and call.

### Tests

- Added material-count/order, split-view boundary, queued aspect ratio, recycled-frame, incompatible-projection and real-copy confirmation regressions.
- Added a concurrent callback-publication test to the ThreadSanitizer job. The old callbacks reproduce a data race; the corrected code passes the same test.

## [0.1.1] - 2026-11-26

### Fixed

- Widescreen HUD placement preserves each render state's interpolation metadata. Repositioning the HUD no longer silently disables pictures between frames.
- Split-screen matrix pairing counts matching objects separately for each camera. An object appearing or disappearing in one player's view no longer disrupts the other players' interpolation.
- Returning to original frame rate, switching interpolation modes or changing anisotropic filtering clears held frames and replay descriptors after their GPU work finishes. Resuming interpolation starts with consecutive frames rather than blending towards an old race picture or using destroyed samplers.
- The current camera widening is published atomically for the renderer, removing a data race while changing aspect ratio.

### Tests

- Added GPU-free integration checks for widescreen HUD placement with interpolation, and independent visibility changes in two- and four-player frames.
- Added rendered regression checks for switching frame-rate modes, resuming interpolation and changing filtering with a replay in flight.

## [0.1.0] - 2026-11-26

The first release. The project was started from PunchOutWii--QilinRecomp v0.22.0 and converted to the GameCube and to Mario Kart: Double Dash!! (USA, `GM4E01`, revision 0). The game boots and plays in headless and offscreen runs with scripted input: the menus, a whole 50cc Mushroom Cup, time trials with ghosts, split screen, co-op, Versus and Battle, with sound and memory cards. None of it has yet been seen in a real window, heard, or played with a real controller. Built and tested on Windows x64 with MSVC; Linux, macOS and ARM64 are compile-checked with Clang only, and CI has not run yet.

### Added

- The recompiler for the GameCube and this DOL: the Gekko build of the decoder and lifter, CodeWarrior's switch shapes of this compiler version, data inside the text sections, the GameCube's memory map. All 13,472 functions are accounted for by the library plan: 12,755 lifted, 511 replaced, 206 dropped, 0 stubs. The differential tests, the game's own functions included, find 0 differences (`docs/recompiler-core.md`).
- Faster lifted floating point: single mode across calls and with doubles left in the CPU state (3,100 functions), cheaper helpers, the FMA instruction chosen at run time, and only the registers a callee may touch synchronised around a direct call. The game's threads work about 5.4 ms a frame in a one-view race on a Ryzen 7 5800X.
- Safe points on the back edges of lifted loops, so interrupts (the sound's among them) reach long loops such as loads and the movie decoder; and a fourth kind in the plan, `wrap`, for native code around a lifted function.
- Names for the retail DOL's functions and data, ported from the disc's own linker map of its debug build (`tools/analysis/port_symbols.py`, `port_data.py`), and the library plan made from them (`tools/analysis/make_plan.py`, `data/GM4E01/plan.tsv`, `docs/code-analysis.md`).
- The runtime for the GameCube: boot as `GM4E01`, the Dolphin SDK's OS, threads, DVD with drive timing, NTSC video, the game's SRAM kept in the settings (`docs/runtime.md`). Development switches for runs without a player: `MKDD_THREADS`, `MKDD_DVD_LOG`, `MKDD_WATCH`, `MKDD_DEVICES_LOG`, and a pilot that plays the first port by the game's state and drives a kart along a path (`MKDD_PILOT_STEPS`, `MKDD_PILOT`; `docs/input.md`).
- The disc is read where it lies: `.iso`, `.gcm` and `.ciso` images and extracted folders, through the `qilin-disc` tool and its library (`docs/disc.md`). The launchers (`Play Double Dash.bat`, `mkdd.sh`) and the Windows download take an image or a folder, check it is the supported game, and take `main.dol` from it.
- GameCube controllers, memory cards in slots A and B (as `.gci` files, written safely, with a history of previous saves) and audio RAM in place of the Wii's remotes and NAND (`docs/input.md`, `docs/saves.md`).
- Four controller ports (`mkdd/src/input.hpp`): any SDL3 controller or the keyboard, with every control set per player by pressing it; controllers assigned to ports automatically or by name, remembered, with their places kept while unplugged; a dead zone for the sticks, a threshold for the triggers' click, rumble per player. `MKDD_INPUT` to `MKDD_INPUT4` script a port.
- The game's DSP microcode statically recompiled: `qilin-dsp` lifts it from the player's DOL to C++ at build time, and it runs on a model of the DSP (`docs/audio.md`). The Windows download ships `qilin-dsp` and builds that file with the rest.
- The renderer for the GameCube and this game: its memory and FIFO, primitives of one state joined into one draw, depth copies as the game makes them, Z before texturing with the alpha test, `GXSetClipMode` off as depth clamping; recording a third faster on race frames (about 4.4 ms a frame in a four-player race) (`docs/graphics.md`).
- Wider pictures (`aspect`, `hud`): any shape, Hor+, the game's cameras widened, whole extra columns of the frame at full resolution, the HUD at the screen's edges, in the middle 4:3 or stretched, with marks that follow karts kept in place. Races widen; menus and movies stay 4:3. Tested offscreen (`docs/enhancements.md`).
- Frame rates above the game's own: with `frame_rate = display` or `unlimited` the renderer draws pictures between the game's 59.94 frames, blending positions matched by the matrices' loaders, and the main loop presents them at the display's rate. The game has no time step, so it always computes 59.94 frames a second; these modes add one frame of delay. F3 shows the logic rate, the pictures shown and how many matrices paired. Tested offscreen.
- A shorter response time in every mode, about 16 to 27 ms measured offscreen: the frame ends as soon as the game has drawn it (`JFWDisplay::endRender` wrapped) and the input is read just before the retrace (`docs/runtime.md`, "Response time"; `MKDD_LATENCY` measures it).
- An analysis of the game's frame timing and picture shape from its code (`docs/enhancements.md`).
- The settings file `mkdd.ini` and the settings menu (F1 or Esc; Back + Start) for this game: `[display]`, `[sound]`, `[controls]`, `[player1]` to `[player4]` and `[console]` (the GameCube's own: sound mode, progressive scan). A value may be written in double quotes, which keeps a `#` in it.
- SMAA 1x anti-aliasing and AMD FSR 1 upscaling (EASU and RCAS, with a sharpness setting) before the picture is shown, as in GBTGhoulies--QilinRecomp.
- Sound output as stereo or as 5.1 decoded from the game's Dolby Surround signal, a choice of playback device, and the volume in its own `[sound]` section.
- F9 captures the next frames for a bug report: the pictures as PNG, the log and the settings in one tar under `captures/` in the data folder.
- A low-latency switch (at most one frame waiting for the display, where the graphics driver can wait for a frame to reach the screen).
- The render resolution follows the window while the game runs.
- `-DQILIN_SHADER_COMPILER=ON` builds a pinned glslang for `tools/build_shaders.py`, so regenerating the committed SPIR-V needs nothing installed.

### Changed

- The picture is the GameCube's 640x480 at 4:3 (the Wii project assumed 16:9); the default window is 960x720.
- Frame pacing is for the console's 59.94 Hz: the retraces follow the display where its refresh is a whole multiple of that to within half a percent (59.94, 60, 119.88, 120, 180, 240 Hz, up to eight times), and otherwise run at 59.94 Hz by the clock.
- The render resolution's "auto" goes by the window's size, not the screen's.
- The Windows download builds the game as `work\mkdd.exe` and copies it to `Double Dash.exe` (Ninja splits a target's name at a space).
- The test that opens a window (`qilin_padtest`) runs only when asked (`QILIN_WINDOW_TESTS=1`, or `QILIN_REQUIRE_GRAPHICS=1` as CI sets under a virtual display), so a plain `ctest` opens nothing.
- The game's lifted files are compiled in parallel under MSVC, at most six compilers a target.

### Fixed

- The opening movie stayed black: `LCEnable` now sets HID2's locked-cache bit, which the movie decoder checks, and `VIGetNextField` alternates under progressive scan too.
- The streamed music was noise: the DSP's `LSRN` and `ASRN` shift right.
- A Grand Prix stopped at its start: the GPU's address registers hold 21 bits, so a palette load with a bit set above them is no longer taken as outside memory.
- A busy memory card still answers a probe; a card pulled out in the middle of a change ends the call with NOCARD and leaves its files as they were, instead of writing them and answering READY.
- A DVD cancel's callbacks run with interrupts off, and `OSResetSystem` stops the scheduler first, as the SDK's do.
- CI compiled an analysis script that no longer exists.

### Removed

- The Wii Remote: its sideways mapping, its pointer (mouse and right stick) and its speaker; the Wii's console settings (language list, widescreen); the migration of settings and data folders from older versions of the Wii project.


