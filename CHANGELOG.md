# Changelog

All notable changes to this project are recorded here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

The project is in development: the game does not run yet. It was started from PunchOutWii--QilinRecomp v0.22.0 and is being converted to the GameCube and to Mario Kart: Double Dash!! (USA, `GM4E01`, revision 0).

### Added

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
- The render resolution's "auto" is taken from the window's size when the game starts, not from the screen's.
- The test that opens a window (`qilin_padtest`) runs only when asked (`QILIN_WINDOW_TESTS=1`, or `QILIN_REQUIRE_GRAPHICS=1` as CI sets under a virtual display), so a plain `ctest` opens nothing.

### Removed

- The Wii Remote: its sideways mapping, its pointer (mouse and right stick) and its speaker; the Wii's console settings (language list, widescreen); the migration of settings and data folders from older versions of the Wii project.
