# Mario Kart: Double Dash!! (GameCube): roadmap and design targets

**Guiding principle: efficiency and reliability come first.** Every design choice is judged on whether it makes the game cheaper to run on low-end hardware and whether it fails loudly and predictably instead of silently misbehaving.

The requirements and scope are the same as GBTGhoulies--QilinRecomp's (Grabbed by the Ghoulies), applied to the GameCube the way PunchOutWii--QilinRecomp applies them to the Wii, which this project was started from. Where a part is still the Wii project's, its own note says so; this document is the target.

## Targets

- **Game:** *Mario Kart: Double Dash!!* (Nintendo, 2003), USA, disc `GM4E01`, revision 0, recognised by the SHA-256 of its `main.dol` (`e96b8578451b9157e2b68fe5e918ebb572940c3ea54d6c8c7d45c24382bf12ae`). Other regions and revisions are other binaries and out of scope for now. The disc carries English only.
- **Platforms:** 64-bit Windows, Linux and macOS (Intel and Apple Silicon), on x86-64 or ARM64, with Vulkan. See *Hardware scope*.
- **Language:** C++23 throughout (recompiler, generated code and runtime). Minimum compilers: GCC 14, Clang 19, Xcode 16, Visual Studio 2022 17.10. No compiler extensions without a fallback.
- **Graphics API:** Vulkan 1.3, and only Vulkan: the 1.3 core API, and of its optional features and limits only what Mesa's v3dv driver gives a Raspberry Pi 4, so one render path works everywhere (MoltenVK on macOS). See *Vulkan baseline*.
- **Static recompilation only:** the game's PowerPC code is translated to C++ ahead of time. No interpreter, JIT or emulator at run time.
- **Dependencies:** as few as possible, fetched by CMake at pinned versions: SDL3, the Vulkan headers, volk, Dear ImGui, and the source files of SMAA and AMD FSR 1 that the picture's passes are built from (`host/shaders/post/third_party.txt`).
- **No game data in the repository**, nor anything derived from it. Address constants and range tables for the one supported build are not game data.
- **Reference:** Dolphin running the original game, for behaviour, picture and sound. The disc's own linker map (of its debug build) and the doldecomp/mkdd decompilation (CC0) give names and structure (`prior-art.md`).
- **Players:** one to four, each with a GameCube controller; two can share a kart.
- **Enhancement example target:** 3440x1440 (21:9) at 144 Hz.

## Hardware scope

**Supported: 64-bit hosts with a Vulkan 1.3 driver.** In practice:

- Windows and Linux on x86-64 or ARM64 with a Vulkan-capable GPU and driver;
- macOS on Intel and Apple Silicon Macs through MoltenVK (Vulkan on Metal), from macOS 10.15;
- ARM boards with Vulkan 1.3 drivers, such as the Raspberry Pi 4 and newer (Mesa v3dv, Mesa 24.3 or later).

**Out of scope:** anything without Vulkan 1.3, 32-bit hosts, phones and tablets. Because of this there is one renderer and one memory model.

**Low-end reference:** a Raspberry Pi 4-class board. It is what efficiency is measured against and tested on, not a floor.

### Vulkan baseline: 1.3

1.3 makes core what would otherwise be extensions to probe for, and the renderer and the host layer are built on them: dynamic rendering (no render pass or framebuffer objects), synchronization2, extended dynamic state, timeline semaphores. Optional features and device limits follow v3dv. What is required is checked when the device is created and a missing piece stops the program with its name; there are no fallback render paths. Two optional things are used where a device has them, and the settings menu says when it hasn't: compute shaders on the drawing queue (FSR 1's two passes) and `VK_KHR_present_wait` (low latency). Vulkan 1.4 is out of scope for the baseline.

## Settings, not platform branches

All enhancements live behind one settings file (`mkdd.ini`) with presets (original, low-end, desktop) and the settings menu over the game, the same three pages as the sister projects: Controls, Display, Game. A setting that can't work on the machine, or yet, is shown disabled with the reason, never silently ignored.

## Wider pictures and higher frame rates (open design)

Both are settings and menu entries now (`aspect`, `hud`, `frame_rate`), routed to two hooks that do nothing (`mkdd/src/enhance.hpp`). Neither is implemented. `enhancements.md` has the analysis of the game's code for both, with addresses, and the design that follows from it; where it and the outline below differ, it is the later and the better informed.

**The game as it is** (from that analysis). It draws a 4:3 picture and has no widescreen mode. Its main loop takes one fixed step of the logic per retrace in every mode, whatever the number of players: 59.94 a second. The only switch is a draw skip (a picture every second step) that LAN play turns on for its fullest games. Nothing in the game is driven by a time step.

**Aspect ratio: Hor+.** The vertical field of view stays and the picture gains at the sides.

1. *Projection.* Find where the game builds its perspective projections (each camera, each split-screen view) and widen them to the picture's shape. Split screens divide the picture: each view's shape follows from its share of it.
2. *Culling.* Whatever the game culls against the 4:3 view (frustum tests on course sections, karts, items, effects; any screen-space rejection) must use the wider one, or things pop in at the sides. This is the part most likely to need function-by-function work, and it decides how wide the picture can go.
3. *The HUD and menus.* Drawn for 4:3 in screen space (the game's 2D layer). Three placements, as in GBTGhoulies: at the screen's edges (each element anchored to its nearest edge or corner), in the middle 4:3, or stretched. Menus and full-screen pictures (title, course previews, the award ceremony) are 4:3 art: they stay in the middle with bars unless a scene can widen.
4. *Effects in screen space* (fades, speed lines, the item roulette, lens flare, copies of the frame buffer used as textures) need checking one by one.
5. *The renderer* then draws an EFB wider than 640 (its height unchanged), and the window shows that shape. `Display::present_image` already takes the shape as a parameter.

**Frame rate above the game's own.** Two ways were considered:

- *Run the game's frame more often with a scaled time step*, as GBTGhoulies does. Ruled out by the analysis: the game has no time step to scale; everything counts steps.
- *Keep the logic at its own rate and interpolate what is drawn* (the way chosen). The renderer keeps the transforms (positions and joint matrices, cameras) of the last two logic frames and draws frames in between at the display's rate. It never changes the game's behaviour, so times, ghosts and physics stay exactly the console's, at the cost of up to one logic frame of added latency, and of care for what must not be interpolated (camera cuts, respawns, particles being born, texture animation). It needs draws identified across frames, which the GX command stream doesn't give by itself: the hook would be in the game's own scene code (JSystem's model and joint updates), not in the renderer alone.

It also suits a racing game: time trials and ghosts are compared between players. "Unlimited" and "the display's refresh rate" are pacing choices on top of it; "60" draws every step (LAN's skip off). Until one is in, the game runs at its own rate and the display shows each frame once, twice or more (`host/include/qilin/host/pacing.hpp`).

## Roadmap

The table in the README is the status. In order:

1. **Analysis of this DOL:** the GameCube's Gekko in the decoder and lifter, function discovery, names from the disc's map, the library plan (`data/GM4E01/plan.tsv`).
2. **Runtime:** OS, disc, video, threads, controllers, memory cards, audio RAM and the DSP for the GameCube's SDK; start-up to the first frame.
3. **Renderer:** GX on Vulkan with this game's use of it; the first picture shown.
4. **Playable at original spec:** one to four players, sound, saves; compared with Dolphin; tested on the low-end reference.
5. **The host's enhancements with the game running:** render resolution, anisotropic filtering, SMAA, FSR 1, the CRT look, pacing on 120, 144 and variable-refresh displays: each is implemented and tested offscreen now, and wants looking at with the game.
6. **Wider pictures**, then **higher frame rates**, as above.
7. **Extras:** texture replacement packs and the like.
