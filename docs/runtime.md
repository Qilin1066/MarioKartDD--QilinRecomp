# The runtime

Last updated 2026-11-26. How the lifted game runs: the guest machine, booting, and the native replacements for the OS, DVD and VI libraries. Controllers, audio RAM and the expansion ports are in `input.md`, memory cards in `saves.md`, the disc's formats in `disc.md`, sound in `audio.md`, what the game asks of the GPU in `graphics.md`, the library plan and the names in `code-analysis.md`.

## Building and running

The game is built only from the player's own `main.dol`; nothing derived from it enters the repository.

```sh
cmake -S . -B build-game -DQILIN_GAME_DOL="<extracted disc>/sys/main.dol"
cmake --build build-game --target mkdd
build-game/mkdd "<the disc>"      # an image (.iso, .gcm, .ciso) or an extracted folder (sys/, files/)
                                  # --offscreen: draw without a window; --headless: don't draw
```

At build time `qilin lift <main.dol> -o build-game/generated --plan data/GM4E01/plan.tsv --files 32` writes the C++, which is compiled into `mkdd` with the runtime and the replacements in `mkdd/src/`. `-DQILIN_GAME_PLAN=<file>` builds with another plan (for trying a boundary before changing the one in the repository).

The supported build is Mario Kart: Double Dash!! (USA), disc `GM4E01` revision 0. `boot.cpp` checks the disc's ID and revision and main.dol's SHA-256 (`recomp/src/dol.cpp`), and stops on anything else.

## The library plan

`data/GM4E01/plan.tsv` is the plan of `code-analysis.md` as address ranges: lifted, replaced or dropped. For the libraries this document covers:

| Range | Plan | Why |
|---|---|---|
| `80003100`–`80003458` | replaced | `__start` and what it calls: done natively by `boot.cpp`. |
| `800A9600`–`800A9714` | replaced | PPCArch: the CPU's special registers. |
| `800AA6B8`–`800AEC3C` | replaced | DVD: the drive is the disc library and a timing model. |
| `800AEC3C`–`800B0AA0` | replaced | VI: the retrace is a timed interrupt; the picture goes to the renderer. |
| `800E58D8`–`800EC7B8` | replaced | OS, but for the three below. |
| `800E6B9C`–`800E6DD0` | lifted | OSAlloc and OSArena: bookkeeping that decides the game's memory layout. |
| `800E6DD0`–`800E6F8C` | lifted | `__OSInitAudioSystem`: it runs the SDK's stub program on the DSP, which is emulated (`audio.md`). |
| `800E8078`–`800E80F8` | lifted | `OSReport`: it formats through MSL's `vprintf`. |
| `800EC7B8`–`800EC84C` | lifted | `__init_user`, `__init_cpp`, `_ExitProcess`: the EABI's constructors. |
| `801116DC`–`80111854` | replaced | MetroTRK's console procedures: MSL's `__write_console` calls `__TRK_write_console` after the UART, so that one function of the dropped debugger stub is reached. It does nothing. |

A replaced function runs only if `mkdd/src` registers a replacement for its address; a call to one without stops the game and names it. `tools/analysis/check_replacements.py` lists those that lifted code calls directly.

The game loads no REL modules: `OSLink.c` contributes only `__OSModuleInit` to this DOL, and nothing calls `OSLink`.

## The guest machine (`runtime/`)

- **Memory:** the 4 GB guest address space is reserved in one block; MEM1 (`0x80000000`, 24 MB) and the locked cache (`0xE0000000`, 16 KB) are backed by memory. There is no MEM2. Lifted code reaches cached MEM1 directly and everything from `0xC0000000` up through the hooks: the uncached mirror maps to the same memory, hardware registers (`0xCC000000`–`0xCC008FFF`) go to the game's handlers (`rt::set_mmio`), anything else stops the runtime with its address.
- **Dispatcher:** an indirect call searches the function table, sorted by address. A target that starts no function is fatal.
- **Registry:** replacements are registered by address (`rt::provide`). Registering an address the plan lifts or drops, or one address twice, is fatal at start-up.
- **Fatal errors** print the reason and the guest's call stack, walked through the back chain. The exit status is 70.
- **Tracing:** with `MKDD_TRACE=1`, every call into a replacement is logged with `r3`, `r4` and the caller.

## Booting (`mkdd/src/boot.cpp`, `build.hpp`)

1. **The disc.** One `qilin::disc::Disc` is opened from the path given (an image or an extracted folder); its main.dol is copied into MEM1.
2. **Low memory**, as a retail NTSC console's boot ROM (the IPL) and the disc's apploader leave it. Where each value comes from:

   | Address | Value | From |
   |---|---|---|
   | `80000000` | the disc header's first 0x20 bytes (ID, magic `C2339F3D`) | the IPL reads them there |
   | `80000020`, `24` | `0D15EA5E`, 1 | the apploader (its routine at `81200728`) |
   | `80000028`, `F0` | `01800000`: physical and simulated memory | hardware; BI2's second word (the apploader, `812008A8`) |
   | `8000002C` | `00000003`: a retail console, HW2 | hardware (`OSGetConsoleType`, `800E5A00`, returns the word) |
   | `80000030` | 0: no arena low, so OSInit takes the linker's | the apploader |
   | `80000034`, `38` | `817FECE0`: arena high and the file table's address, `(81800000 − FSTMaxLength) & ~31` | the apploader (`81200A34`); FSTMaxLength is the header's word at 0x42C |
   | `8000003C` | the header's FSTMaxLength (`1313`) | the apploader |
   | `800000CC` | 0: NTSC | hardware (Dolphin) |
   | `800000D0` | `01000000`: 16 MB of ARAM | hardware (Dolphin) |
   | `800000E8`, `EC` | 0, `81800000`: no debug monitor | BI2's first word (the apploader, `8120087C`) |
   | `800000F4` | `817FCCE0`: BI2, 0x2000 bytes straight below the file table | the apploader (`81200A4C`) |
   | `800000F8`, `FC` | 162,000,000 and 486,000,000: the bus and CPU clocks | hardware (Dolphin) |
   | `800030D8` | 0: system time's offset from the time base | `__OSGetSystemTime` (`800EC35C`) adds it |
   | `800030E4` | 0: controller 4's buttons at boot | `__check_pad3` (`80003100`) resets on a combination |

   The apploader's addresses are in its own code (the disc's apploader, loaded at `81200000`), read with `qilin disasm`. BI2 lies inside the arena (arena high is the file table); `__start` and OSInit read it before anything is allocated. The time base is the bus clock over four: 40.5 MHz.
3. **`__start` (`80003154`), natively**, in its order: `r1` (`_stack_addr`, `803E22F8`), `r2` (`803D45A0`) and `r13` (`803D1420`) as `__init_registers` (`800032B0`) sets them; the outermost frame; `__init_data`'s copy and zero tables (`80005544`, `800055D4`, read from guest memory); `DBInit` and `OSInit`; `__init_user` (`800EC7B8`); `main(0, NULL)` (`801CC6A8`); `exit` (`800FFE28`). BI2's debug flag and argument offset are checked to be 0, since the debugger's and the arguments' parts of `__start` are left out on that account.

## Replacements

Replacements take arguments in `r3`–`r10` and return in `r3` (with `r4` for 64-bit results), as the EABI passes them. Every address bound was taken from the retail code and checked against `work/symbols.txt`'s names; the structure offsets below were read from this SDK's own functions (named in the sources), not assumed from the Wii's.

### OS (`os.cpp`)

- **`OSInit`** (`800E5B8C`) runs once, in the SDK's order less what only hardware needs (performance counters, exception vectors, caches, memory protection): the boot-info and BI2 pointers, `__PADSpec`, the arena (low `803E4300`, the linker's `__ArenaLo`, since low memory gives none and no debugger is attached; high from `80000034`), the interrupt table cleared and everything masked, `EXIInit` and `SIInit` (other libraries' replacements, called at their addresses), SRAM, `__OSInitAudioSystem` (lifted), the arena zeroed as after a cold boot, interrupts on, `DVDInit`. The drive's inquiry isn't run: its result, the device code in `800030E6`, is stored directly (`8000`: a retail drive).
- **Interrupts:** `OSDisableInterrupts`, `OSEnableInterrupts`, `OSRestoreInterrupts`, and `PPCMfmsr`/`PPCMtmsr` for code that changes MSR itself, keep one flag, as MSR[EE] does; each thread keeps its own across switches.
- **Interrupt handlers:** the table is the OS's own at `80003040`, the mask its two words at `800000C4`/`C8` (`__OSSetInterruptHandler`, `__OSGetInterruptHandler`, `__OSMaskInterrupts`, `__OSUnmaskInterrupts`). The numbers, from `__OSDispatchInterrupt` (`800E8C54`): 0–4 memory protection, 5 audio DMA, 6 ARAM, 7 DSP, 8 audio streaming, 9–16 EXI, 17 CP, 18 PE token, 19 PE finish, 20 SI, 21 DVD, 22 reset switch, 23 PI error, 24 VI, 25 debugger, 26 high-speed port. `raise_interrupt(n)` runs a handler as an interrupt, with the number and the current context as the OS passes them; a masked one is held and arrives when the OS unmasks it, as the hardware's line stays up.
- **Safe points:** interrupts are queued and delivered whenever the guest enables interrupts and inside every replacement that waits or polls. Each is entered with the guest's registers written back, so a delivery is the same as an interrupt arriving at that instruction. Handlers and callbacks run on a fresh frame below the interrupted thread's stack, with interrupts off and every register restored afterwards. If one wakes a thread of higher priority, the switch happens right after it.
- **Contexts:** thread switching is native, so `OSSetCurrentContext`, `OSGetCurrentContext` and `OSClearContext` keep only the words in low memory that lifted interrupt handlers (GX, AI, DSP, JAudio) set around their callbacks. `OSFillFPUContext` and `OSGetStackPointer` serve the game's crash screen.
- **Caches:** every flush and invalidate is a no-op, because the host is coherent; `DCZeroRange` zeroes memory. The locked cache's DMA (`LCStoreBlocks`, `LCStoreData`) is a copy done at once. `LCEnable` and `LCDisable` set and clear HID2's locked-cache bit, which the movie decoder reads.
- **Time:** the time base counts at 40.5 MHz by the host's steady clock, from the PC's local date and time counted from 1 January 2000, as the console's does from its clock chip (`OSGetTime`, `OSGetTick`, `__OSGetSystemTime`). `OSTicksToCalendarTime` is exact for any date.
- **SRAM** (`OSRtc.c`): the console's settings live where the SDK keeps its copy (`80375620`), filled from the port's settings instead of the clock chip's memory over EXI: language, the stereo flag, the progressive flag, with the checksum the SDK computes. `OSGetSoundMode`, `OSSetSoundMode`, `OSGetProgressiveMode`, `OSSetProgressiveMode`, the wireless-pad and Game Boy Player words, and the lock and unlock calls work on it. What the game changes is kept until the program ends; it isn't written back to the settings file yet.
- **Reset** (`OSReset.c`): `OSResetSystem` calls the registered reset functions as the OS does (each with FALSE until all agree, then with TRUE). A restart or hot reset starts the program again with the same command line (`restart.cpp`); a hot reset to the console's menu closes it. The shutdown form isn't modelled and stops the runtime. `OSGetResetSwitchState` reads `g_reset_button` (`os.hpp`), which nothing sets yet.
- **Errors:** `OSPanic` is fatal. `OSSetErrorHandler` keeps the handlers in the OS's table, never called: a fault in the guest stops the runtime with its own report. A handler for floating-point exceptions is logged; none are raised.
- **Console output:** the C library's console writes go to the debug serial port (`InitializeUART`, `WriteUARTN`), which a retail console doesn't have; here they become the log's `game: ...` lines.
- **The IPL's font** (`OSFont.c`): only `OSGetFontEncode` is in the DOL (it answers ANSI); the game doesn't load the ROM font.

### Threads (`threads.cpp`)

- **How they run:** each guest thread runs on a host thread of its own (16 MB host stack) with its own registers, and exactly one runs at a time. The scheduler hands over where the OS would, with the OS's rules: priorities 0–31, first come first served within one, suspend counts, threads created suspended, priority lent through mutexes.
- **Yield is a safe point**, and so is the idle loop: with every thread blocked, the blocked thread sleeps to the next timer deadline and delivers interrupts. If nothing can ever wake a thread, the runtime reports a deadlock.
- **Guest-visible state:** an `OSThread`'s state, suspend count, priority and exit value; the current thread at `800000E4` and its context at `800000D4`; the list of active threads from `800000DC`; a mutex's owner and count; a message queue's ring; an alarm's handler, fire time and period.
- **Covered** (all that is in this DOL): create, resume, suspend, yield, exit, cancel, detach, `OSIsThreadTerminated`, sleep and wake on a thread queue, `OSDisableScheduler`/`OSEnableScheduler` (a count; enabling doesn't itself switch, as on the OS); mutexes; message queues (send, jam, receive); alarms, one-shot and periodic. This SDK build has no `OSJoinThread`, conditions or `OSSleepTicks`.
- **Tested without the game** (`mkdd/tests/threads_test.cpp`): the real scheduler and interrupt layer run small guest programs written in C++, and the order of events is checked against the OS's rules: preemption on resume; blocking receive and the send that preempts; queue order, a full queue and jam; a mutex, recursion included; priority inheritance and its withdrawal on suspend and cancel; yield within a priority; suspend counts; exit values, detaching and the active list; the scheduler switched off; an alarm firing while every thread waits; interrupts held by the CPU's flag and by the OS's mask; SRAM's sound mode, lock and checksum; calendar dates; `OSResetSystem` closing and restarting the program.

### DVD (`dvd.cpp`)

- **The disc:** every read is by disc offset through `qilin::disc::Disc` (`disc.md`), image or folder alike. The file table is also in guest memory where the apploader puts it, though no lifted code reads it.
- **Lookups** resolve as the SDK does: from the root, `.` and `..`, without case. Long names are allowed, as OSInit sets the SDK's flag for them.
- **Structures**, from this SDK's code: a command block's state at +0x0C (0 done, 1 busy, 2 waiting, 10 cancelled), offset, length and buffer at +0x10, +0x14, +0x18, bytes transferred at +0x20; a file's start in bytes at +0x30, its length at +0x34. The bounds `DVDReadPrio` and `DVDReadAsyncPrio` panic on stop the runtime here.
- **The drive** runs one command at a time, the rest waiting by priority, each finishing as an interrupt after the time a drive takes: 2 MiB/s at the start of the disc to 3.125 at the end (Dolphin's figures, taken as linear in the offset), plus a seek of 20–120 ms when the head moves. The data reaches guest memory when the read completes. A synchronous read sleeps its thread until then, so others run. `MKDD_DVD_SPEED=<n>` divides the times (0: none), for tests.
- **Simplified:** a long read completes in one piece (the SDK splits it into 512 KB transfers and advances the count after each); there are no drive errors, no cover, no audio streaming.
- **Covered:** `DVDInit`, `DVDConvertPathToEntrynum`, `DVDFastOpen`, `DVDOpen`, `DVDClose`, `DVDReadPrio`, `DVDReadAsyncPrio`, `DVDCancel`, `DVDGetCommandBlockStatus`, `DVDGetDriveStatus`, `DVDCheckDisk`, `DVDGetCurrentDiskID`: every DVD function lifted code calls directly.

### VI (`vi.cpp`, `video.cpp`)

- **The retrace** is a timed interrupt from the host clock at the NTSC rate, 60000/1001 Hz, until the main loop matches it to the display (`set_retrace_rate`; 0/0 returns to the console's own). It counts, runs the pre-retrace callback, applies what `VIFlush` passed on (frame buffer, black, size, scan mode), hands the picture to the main thread (`video.cpp`), runs the post-retrace callback and wakes the threads in `VIWaitForRetrace`. More than a frame late, the schedule restarts from now rather than posting the missed retraces in a burst.
- **Settings take effect at a retrace after `VIFlush`**, as the hardware's registers do: `VISetNextFrameBuffer`, `VISetBlack`, `VIConfigure`. `VIGetNextFrameBuffer` and `VIGetCurrentFrameBuffer` read the two sides of that.
- **Modes:** `VIGetTvFormat` says NTSC; `VIConfigure` accepts NTSC modes (interlaced, double-strike, progressive) and stops on another TV format. `VIGetDTVStatus` reports a component cable when the progressive setting is on, which is what makes the game offer progressive scan. `VIGetNextField` alternates with the retraces, under progressive scan too (the hardware's line counter still runs through two "fields", each a whole picture; the movie player counts on it).
- **Not in:** `VIGetCurrentLine` (nothing calls it).

## Hardware registers (`hw.cpp`)

Each device maps its own ranges of `0xCC000000`–`0xCC008FFF`: the GPU's CP (`CC000000`), PE (`CC001000`), PI (`CC003000`) and write-gather pipe (`CC008000`) in `gpu.cpp`; the DSP (`CC005000`) and AI (`CC006C00`) in `dsp.cpp`; the memory interface (`CC004000`) in `hw.cpp`, where only the two counters GX's abort samples (+0x4E, +0x50) read, as 0. An access outside every range, or to another MI register, is fatal and names the address. VI's, SI's, EXI's and the DVD interface's registers have no device: their libraries are replaced above them.

## Poll loops

Code that waits for an interrupt by spinning would never see it, because interrupts arrive at safe points. The lifter finds such loops (`recompiler-core.md`) and their back edges call the runtime's idle hook (`poll_loop`), which delivers what's due or sleeps until the next timed event, a millisecond at most.

## The host's side, as inherited

The sections from here to "Frame pacing, sound, logs" are the base project's (a Wii game's port), kept because the design they describe is this port's too; where they speak of the Wii Remote, MEM2, 16:9 or AX they describe the base. What differs for this game is in `graphics.md`, `input.md`, `audio.md` and `enhancements.md`.

## The host platform (`host/`, v0.9.0)

- **Threads:** the main thread owns the window: it handles its events and shows the newest frame the guest has handed over. The guest runs on threads of its own; its main thread starts on a host thread with a 16 MB stack, like every other guest thread. Closing the window ends the program (it will become the console's power button).
- **Window:** an SDL3 window, resizable and high-DPI.
- **Input:** the first SDL gamepad (any, by an Xbox pad's button names) and the keyboard. Arrows are the D-pad, X and Z are A and B, C or Space is Y, Enter is Start, Backspace is View, H is Guide (Escape and F1 open the settings menu). The left stick counts as the D-pad past half way. Rumble goes to the pad.
- **Vulkan:** `Gpu` makes a Vulkan 1.3 device, preferring a discrete GPU. It requires dynamic rendering and synchronization2 (both in v3dv's set) and names whatever a device lacks. `Display` presents through a FIFO swapchain, rebuilt when the window changes. Headless, it draws into an image of its own, which `qilin_vktest` reads back to check the letterboxing (skipped where there's no Vulkan 1.3 device; it passes on Mesa's llvmpipe).
- **Frames:** at each retrace VI hands over the frame buffer the game set (`video.cpp`), and the main thread shows it at 16:9, letterboxed. For now that's the XFB in guest memory, YUV 4:2:2, converted on the CPU. The renderer's EFB copies will take its place.

## The GPU's front end (`gpu.cpp`, v0.8.0)

- **Registers:** the command processor, pixel engine and processor-interface FIFO registers GX touches, and the write-gather pipe (any address in its 32-byte block at `0xCC008000`).
- **Decoding:** the command processor decodes as the stream arrives. There's no FIFO in guest memory to overflow, so it reads as idle, its read pointer is its write pointer, and the PI's write pointer moves through the CPU FIFO's ring as on hardware.
- **Commands:** the decoder knows every command (NOP, CP, XF, BP and indexed loads, display-list calls, primitives). It keeps the vertex descriptors and attribute tables it needs to size vertex data, by the hardware's rules as Dolphin documents them. An unknown byte stops the runtime with the bytes around it, because it means the decoder has lost its place.
- **The write-gather pipe** collects 32 bytes before the decoder sees them, as on hardware (a write can straddle two bursts).
- **Interrupts:** draw-done (BP `0x45`) and token commands (`0x47`, `0x48`) raise the pixel engine's finish and token interrupts, which reach GX's own handlers; the token register reads back the last token.
- **GPU latency:** the game relies on the GPU being behind. It writes draw-done, installs the callback for it, then waits. So these interrupts come 50 µs after the command is decoded, as timed events. Dolphin likewise schedules them at least 500 CPU cycles out.
- **Counting:** every five seconds it logs the draws, vertices, display lists, frame-buffer copies and command bytes it has seen. Drawing them is the renderer's job.

## The renderer (`recorder.cpp`, `texture.cpp`, `renderer.cpp`, v0.13.0)

Two threads share the work. The game's thread records what the command stream asks for; the main thread owns Vulkan and draws it.
- **Recording** (`recorder.cpp`): each primitive's vertices are decoded by the current vertex format into one canonical vertex (position, normal, binormal and tangent, two colours, eight texture coordinates, matrix indices, and bits saying which are present). Quads, strips and fans become index lists. The XF, BP and TEV state the shaders read is mirrored in a `gx::State` (std430). A new one is snapshotted only when a register it depends on changes. Viewport and scissor follow the hardware's rules as Dolphin documents them.
- **Textures** (`texture.cpp`): TMEM is modelled for palettes and preloads. Every format (I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR) is decoded on the CPU, with mip levels, into a cache keyed by address, format, size, levels and palette, and checked by a hash of the data. A texture that an EFB copy wrote is used straight from the GPU while guest memory still holds what the copy left there. Entries unused for 600 frames go.
  - Checking for changes hashes a texture's data (four independent lanes, about 11 GB/s on a desktop core). It does so once between invalidations of the GPU's texture cache (BP `0x66`) and frame ends, however many draws use the texture. On the Wii, the GPU keeps what it fetched until the game invalidates, so until then the data it would use is the data hashed.
- **Frames:** a copy to the XFB ends a frame. The main thread takes it, at most two frames behind (otherwise the game waits).
- **Drawing** (`renderer.cpp`): the EFB is RGBA8 plus a 32-bit depth buffer at a whole-number scale of 640×528 (`MKDD_SCALE`). Draws use dynamic rendering. Pipelines are keyed by blending and primitive type; cull mode and depth test are dynamic state. GX's reversed depth range goes into the viewport's min and max depth.
- **Shaders** (`mkdd/shaders/`, built by `tools/build_shaders.py`):
  - The vertex shader does XF's transform, lighting (the channel controls, attenuation, spot and specular) and texture-coordinate generation.
  - One fragment ubershader reads the TEV configuration from the state and does all 16 stages: indirect textures, the colour and alpha combiners with the hardware's 8-bit rounding and compares, konst and swap tables, the alpha test, dither and fog.
  - **Specialised pipelines (v0.15.0):** a weak GPU pays dearly for interpreting the TEV registers per pixel, so each draw's *shape* (gx.hpp `shape_of`: 70 words of stage counts, indirect commands, orders, combiners, konst and swap selects, the Z-texture mode and the alpha-test functions; colours stay dynamic) also gets a pipeline of its own. The same shader source is built once per stage count (`kGxFragStages1`…`16`, stages unrolled) and the shape goes in as specialisation constants, so the driver folds every decision away and compiles only the stages the draw has.
    - Worker threads compile them (one per core beyond two, up to four); until one is ready its draws use the ubershader, so a new shape never stalls a frame.
    - **No compiling while playing (unreleased):** the ubershader's own pipelines (one per blend, write mask, logic op, primitive type and vertex layout) can't wait, since a draw needs one, and a desktop driver takes a large fraction of a second for each the first time. So every pipeline the game uses, of both kinds, is remembered in `pipelines.keys` in the user folder, and they are all compiled as the game starts: the ubershader's before the first frame, the specialised ones by the workers. Those seen while making the build are built in (`mkdd/src/pipeline_seed.inc`, from `tools/make_pipeline_seed.py`), so a first run has them too. A pipeline new to every run so far is still compiled on the drawing thread, and the log says how long it took.
    - The Vulkan pipeline cache (`pipelines.bin`) and new keys are written by a thread of their own, at most every 20 s and at exit, never by the drawing thread; later runs get their pipelines from it in milliseconds (drivers that keep nothing there, like lavapipe, still have Mesa's own disk cache).
    - Draws that write depth (Z textures with late Z) keep the ubershader variant.
    - `MKDD_UBERSHADER=1` draws everything with the ubershader; `MKDD_SPECIALIZE_SYNC=1` compiles each specialised pipeline when first needed (the tests); `MKDD_SPECIALIZE_FROM=<seconds>` switches specialisation on at that game time, to compare the two on one scene.
    - `mkdd_render_test` runs on both paths (`_ubershader`, `_specialized`) with the same expectations; a planted shape bug fails 32 of its checks.
  - A copy shader handles the EFB copy formats, halving and gamma.
  - They're compiled to SPIR-V when the shaders change, not at build time, into `mkdd/src/shaders.inc`, which is committed. `--check` fails if the header is stale.
- **GPU images are reused:** a texture or copy target that's done with becomes spare once its frame's fence has passed. The next image of the same size and use takes it, and spares unused for two seconds are freed. On the boot screens, no image is created after start-up. A frame's releases are handled after it's drawn: one frame can make and release the same texture (two copies to one address), and handling them first leaked it.
- **Copies:** a copy to a texture stays a GPU image, found again by its address. A copy to the XFB becomes the image VI presents (`Display::present_image`). When there isn't one, VI converts the XFB in guest memory on the CPU, as before v0.13.0.
- **Logic ops** (v0.13.9) use Vulkan's where the GPU has them, since GX numbers its sixteen as Vulkan does. Where it doesn't (Apple's GPUs through MoltenVK), they're approximated with blending, following Dolphin's table, with the shader forcing or inverting its output for four of them. That's exact for clear, copy, no-op, invert, copy-inverted and set, and for and, or and xor on 0/255 values; close for the rest. It's logged once when it happens.
- **Line widths and point sizes** (BP 0x22, in sixths of a pixel) use dynamic line width and `gl_PointSize`, scaled with the EFB, within the device's range. Without `wideLines` or `largePoints` they're one pixel, logged once. The texture-coordinate offsets GX applies across lines and points aren't done.
- **Z textures** (BP 0xF4/0xF5): the last texture sampled, unswizzled, weighted by its type (u8, u16, u24), plus the bias, replacing or adding to the depth, as Dolphin computes it.
  - Fog uses the result whenever it's on.
  - It reaches the depth buffer when Z is compared after texturing (BP 0x43 bit 6 clear). Those draws use a second build of the TEV shader that writes depth, because writing depth turns off the GPU's early depth test and every other draw keeps it.
- **EFB copies of depth** (pixel format Z24 at the copy): the 24-bit depth is split into bytes, (z >> 16, z >> 8, z, 255), and these go through the colour copy's format conversion. So Z8 is the high byte, Z8M the middle and Z8L the low, as in Dolphin. The depth image is read with a nearest sampler, and the half-scale filter averages the bytes.
- **Copy formats RG8 and GB8** sample (as IA8) with intensity from red and from green respectively. Before v0.13.9 the two channels were the wrong way round.
- **Still approximate:** constant alpha together with blending that reads source alpha needs dual-source blending, which the Pi 4's driver doesn't have. There, the constant isn't written.
- **Tested without the game** (`mkdd_render_test`, in `ctest`, skipped without a Vulkan 1.3 device). GX register writes go through the real recorder and renderer, and the frame copied to the XFB is read back and checked. It covers:
  - all sixteen logic ops, with the GPU's own and approximated;
  - line and point sizes, and their fallbacks;
  - Z textures with late and early Z;
  - Z8 and Z8M depth copies;
  - RG8 and GB8.

  Planted bugs fail it: the old RG8 order, Z textures not writing depth, and depth copies reading colour.
- **The stats line** (every five seconds) adds, per frame:
  - the game's thread recording, of which checking and decoding textures;
  - its waits for the main thread;
  - the main thread's drawing, of which waiting for the GPU;
  - the textures held on the GPU, the spares, and the images made.

  It shows which side holds the game back on a given machine.
- **Testing without a screen:**
  - `--offscreen` draws with no window, for example on llvmpipe.
  - `MKDD_DUMP=dir` writes every 60th frame shown as a PPM at 1280×720 and logs the time of each; `MKDD_DUMP_EVERY=n` changes the 60.
  - `MKDD_INPUT="Y@16,Start@20.5"` presses pad buttons at those seconds of game time, for a fifth of a second each. Game time is the frames the game has handed VI, over 60: the game waits for the GPU, so on a slow one this keeps a script in step with the screen where the clock would drift.
  - `MKDD_POINTER="x,y"` holds the pointer at a point (−1 to 1, y up); `"x,y;x,y@40;x,y@52"` moves it at those times.
  - The save lives in the per-user folder's `nand/`; copying it aside and back makes a scripted run repeatable.
  - `MKDD_GPU_THREAD=0` decodes the command stream on the guest's thread instead of the GPU thread (v0.17.0; gpu.cpp explains the 16 KB queue).
  - `MKDD_FULL_VERTICES=1` stores every vertex in the full 132-byte layout instead of the compact one where it fits (v0.16.0; 76 bytes and up to four texture coordinates since the recording work of v0.22.0).
  - Draws of triangle lists and strips, lines, line strips and points have `Draw::direct` set (1 list, 2 strip) and no indices: `first_index` and `index_count` are then the first vertex and the count, and the renderer sets the topology (dynamic, within the pipeline's class) and calls vkCmdDraw.
  - Draw states are built incrementally (`build_state` parts: `kPartPos`, `kPartNormal`, `kPartPost`, `kPartLights`, `kPartXfRegs`, `kPartBp`): gpu.cpp reports XF writes by address (`gx::xf_changed`), BP writes as changed or not, and CP writes not at all (vertex formats are read by each primitive, not kept in the state).
  - `MKDD_CHECK_DECODE=1` also checks each incrementally built state against one built whole. It decodes every primitive both ways, an attribute at a time across its vertices (the way used) and a vertex at a time (the reference), and stops at the first byte that differs.
  - `mkdd --bench-record <file.dff> [times]` decodes and records a frame captured with `MKDD_DFF` that many times (default 200), without a window or the game running, and prints the median, fastest and slowest times and the draws and vertices: the recorder's cost, on its own. With `MKDD_PROFILE` (run it long enough for a 5 s write) it shows where that goes.
  - `MKDD_PROFILE=<file>` (Linux): a SIGPROF sampler, 1 kHz of CPU time, needing no perf or ptrace; `tools/analysis/profile_report.py <exe> <file>` names the hot functions.
  - `MKDD_SAVE_DIR=dir` keeps the settings and save there instead of the per-user folder.
- **Comparing a frame with Dolphin:**
  - `MKDD_DFF=<file>@<seconds>` writes the first whole frame after that much game time as a Dolphin FIFO log (`.dff`, version 6): the registers as it starts, its commands, RAM as it starts, and each piece of memory the GPU reads that changed since, where it read it (as Dolphin's own recorder does). It also writes our picture of that frame (`<file>.ppm`) and `<file>.prims`, every primitive's place in the commands. Dolphin's FIFO player renders the log (`dolphin-emu-nogui -e <file>` with frame dumping on) as a reference.
  - `tools/analysis/dff_cut.py <file> <primitive> <out.dff> [writes]` cuts the log after one primitive (then copies the EFB to the screen), to see Dolphin's picture part way through the frame; `writes` (`E0=8FF0FF,xf100E=0000060B`) changes BP or XF registers just before it, to test what a register does.
  - `mkdd --render-dff <file.dff> <out.ppm> [loops]` draws such a log with our renderer and no game running, and writes the picture at the console's own size (640x480 for the game's frames). It starts as Dolphin's FIFO player starts (an empty EFB, then one clearing copy of the whole EFB under the log's registers), so the two renderers draw the same commands from the same state and their pictures compare pixel for pixel. `loops` (1) replays the frame that many times, each loop starting from what the one before left, as the game's frames do; Dolphin's first frame is the first loop. `MKDD_TRUE_COLOR=1` draws in true colour, `MKDD_PROBE` works here too.
  - `tools/check/picture_check.py` does all of this for a set of scenes (`tools/check/picture_scenes.json`: a new save's notice, the title screen, the main menu; with `--slow`, the practice fight's VS screen and the fight itself): each is captured headless from a fresh save (or a copy of a given one) with scripted input, drawn by both renderers, in the console's 18-bit colour and in true colour, and compared as drawn, with no resampling. A scene fails when more than its limit of pixels is over 8 of 255 off in any channel (0.1%), or over 48 off (0.01%). Today (2026-11-21) no pixel of any scene is more than 8 off in either mode; 95-99.96% are equal, the rest one or two steps apart where a copy is filtered (the half-scale box and bilinear sampling round differently from one GPU to the next). It sets Dolphin up itself, in a user folder kept for it: the Vulkan backend, frame dumps as drawn at 1x, a frame for each copy to the screen ("Immediately Present XFB": the player's made-up display mode otherwise dumps frames of odd sizes), the console's copy filter on (Dolphin's default leaves it out), and "Force 24-Bit Color" to match. It needs the disc and a Dolphin build, so it's run by hand before a release, not by CI.
  - What the comparison found (2026-11-21), each now as Dolphin has it: copies round as the hardware does (whole bytes through the copy filter, the intensity weights in integers, narrower formats keeping their top bits, the half-scale box adding four pixels and dropping the remainder); the EFB's picture is reinterpreted when the game changes its pixel format between RGB8 and RGBA6 (the practice fight does, twice a frame: the hologram is drawn with alpha, the ring without); true colour as a setting. Two faults were the capture's, not the picture's: a log kept only one of a TEV register and the konstant sharing its address (it now begins with writes for both), and one capture in a few gave the player no FIFO (the PI's registers were read while the game was building a display list, which points them elsewhere: the "frame Dolphin draws black" noted here before).
  - One difference is the player's and stays: it clears the EFB once, before its first frame. The game's last copy of a practice-fight frame doesn't clear the depth, so the next frame's hologram is drawn behind the depth the frame before left where Little Mac stood, and doesn't show through him; the player's first frame has it showing through. Replayed twice (`--render-dff ... 2`), ours shows the game's own.
  - `MKDD_PROBE=x,y` (EFB pixels at 1x) logs, for the captured frame, every draw and copy that changes that pixel's colour or depth: before and after, with the draw's primitive number and its state (blending, depth, TEV stages, konstants, lighting channels and lights, textures); `MKDD_PROBE_DIR=dir` saves the frame's texture copies. `MKDD_PROBE_SOLO=<draw>` draws that one unblended, so the probe shows what its pixel shader gives; `MKDD_PROBE_SOLO_MODE=z` (and `c`) also sets its depth test to always (and culling off).

## Settings (`settings.cpp`, v0.13.2; rebuilt in v0.18.0)

`mkdd.ini` sits in the game's data folder, next to the saves. The first run writes it with every key and comments; the settings menu (`menu.cpp`) changes it and saves it back through a temporary file. Its layout follows the other Qilin recomps (BeatTheBeat's `beatthebeat.ini`):
- `[display]`: `preset` (original, low-end, desktop: sets `internal_scale`, `anisotropy`, `vsync` and `true_color`; keys after it override it), `fullscreen`, `monitor`, `scaling` (integer, fit, stretch), `smooth`, `look` (plain, crt), `scanlines` and `mask` (0-100), `internal_scale` (1-8; `MKDD_SCALE` overrides it), `anisotropy` (1, 2, 4, 8, 16), `true_color` (on: every pixel keeps its 24 bits, as Dolphin's "Force 24-Bit Color"; off: the console's 18-bit colour with its dither wherever the game draws with alpha, and its copies narrowed as the console narrows them), `vsync`, `gpu`.
- `[controls]`: `rumble`, `second_player` (off, second controller, first controller), and each of the remote's buttons, in the player's terms (`up down left right one two a plus minus home`), with a comma-separated list of inputs:
  - `pad:<Xbox name>` (Up Down Left Right A B X Y Start Back Guide LB RB LT RT L3 R3);
  - `key:<SDL key name>` (not Escape, F1 or F11, which are the program's);
  - `mouse`, its left button.

  An empty list leaves the button unused. Controls the file doesn't mention keep their defaults.
- `[console]`: what `SCGetLanguage`, `SCGetAspectRatio` and `SCGetSoundMode` answer (`sc.cpp`): `language` (1-5, the PAL disc's), `widescreen`, `sound` (mono, stereo). The window's picture follows the aspect ratio.

v0.17.0's `settings.ini` (`[video] scale` and `[controls]`) is read once, written out as `mkdd.ini` and renamed `settings.ini.old`; a `key:Escape` in it becomes `key:H`.

The main thread turns the pad, the keys and the mouse into the remote's buttons after each poll; WPAD turns those into its own bits. Mistakes stop the program at start-up, naming the line or the key, rather than leaving a button dead without saying why: an unknown name, section, button, key, preset or look; a number out of range; a control set twice; a reserved key.

`mkdd_settings_test` checks that every setting reads back as written, that edits and presets are read, that the old file carries over, and that mistakes are reported.

## The settings menu and the window's picture (`menu.cpp`, `host/src/display.cpp`, v0.18.0)

- **Pause:** opening the menu asks for a pause (`request_pause`); the guest's running thread parks at the start of the next retrace interrupt (`park_if_requested` in `vi.cpp`). Only one guest thread runs at a time, so the game stands still. The time base leaves out the time parked, so OSGetTime, alarms, the retrace and the audio go on from where they were. The main thread shows the last picture again (`present_again`) with the menu over it.
- **Drawing:** each present draws into the window image with dynamic rendering: black, the picture through its look in the rectangle the scaling gives, then the overlay (Dear ImGui's Vulkan backend). Whole multiples are of the console's picture, 853×480 at 16:9. The looks are `host/shaders/plain.frag` (the sampler is bilinear or nearest) and `crt.frag` (after the APB recomp's: Gaussian beams on the console's raster lines, wider when brighter, so average brightness is kept; gaps that fade where a line gets under two window pixels; an aperture grille in window pixels). `qilin_vktest` checks fill, whole multiples, and the CRT's beams against values worked out by hand.
- **Input while open:** the display's event hook gives every SDL event to the menu first; while it's up the game gets none (and no buttons). F11 and Alt+Enter are the display's, open or not.
- **Binding by pressing:** *Add* takes the next key (by its SDL scancode name, as `key_code` reads it), or the next pad button or click not already held when it began (read from the display's buttons). Meanwhile ImGui's mouse and gamepad navigation are off, until the press is let go.

## Frame pacing, sound, logs (`vi.cpp`, `os.cpp`, `priority.cpp`, `host/src/display.cpp`, `host/src/audio.cpp`, `runtime/src/report.cpp`)

- **Retraces** come from the host clock, one at a time (`poll_retrace`). The period is 60 Hz until the main loop matches it to the display (`set_retrace_rate`, checked every second): the display's refresh, or a whole fraction of it, when that is within 0.3 Hz of 60; otherwise, and without vsync, a free 60 Hz. More than a frame late, the schedule restarts from now rather than posting the missed retraces in a burst.
- **Idle waits** (`idle_sleep`) to a timer's deadline sleep on the condition variable until 1.5 ms (Windows, with `timeBeginPeriod(1)`) or 0.25 ms (elsewhere) before it, then yield until it; a wake (`wake_idle`) ends either part. A wait that is only an upper bound (the millisecond cap in `poll_loop`) just sleeps.
- **Priorities** (`priority.cpp`): game threads, the GPU command thread and the main loop high; shader compilers and the cache saver background.
- **Presenting**: with vsync and `VK_KHR_present_wait`, a frame waits for the previous one to reach the screen before acquiring an image (bounded at 50 ms), keeping one frame queued at most. `QILIN_NO_PRESENT_WAIT=1` disables it.
- **Vblank steering** (`main.cpp`, `steer_to_vblank`; Windows): with the retraces locked to the display, the time from each present to the next vblank (DwmGetCompositionTimingInfo) is held at max(3 ms, 1 ms + 4σ) by a PI loop on the retrace clock's speed (`set_retrace_speed`, ±0.2%; Kp 1/s, Ki 0.25/s²), as in Beat the Beat.
- **Sound** (`host/src/audio.cpp`): the sound waits in a queue of our own (`AudioQueue`), which SDL's device thread empties as it plays (the stream's callback asks for what it needs, so nothing waits inside SDL and the queue's length is exact). It is steered to its target with SDL's frequency ratio (±0.5%, `audio_rate_ratio`): 40 ms, or twice the device's piece and 10 ms where the device takes more than 15 ms at a time. Past twice the target the oldest sound is dropped down to the target, and a queue run dry plays nothing until the target's worth is back. Before (to 2026-11-21) the queue was SDL's, dropped only over 200 ms and "dry" only when SDL reported exactly nothing queued: after either side stopped for a moment (the sound device under a fight's load, on the owner's PC) the queue stayed that much longer, 110-200 ms in fights, heard as sound lagging the picture, and at 0.5% it took minutes to play off. The log's sound line has the target, the times it ran dry, and how much was dropped. `MKDD_AUDIO=1` gives a headless run its sound (with `SDL_AUDIO_DRIVER=dummy` in tests); `qilin_frame_times_test` checks the queue's rules.
- **Where the time goes**: the renderer writes GPU timestamps at the start and end of each frame's command buffer (`RecorderStats::gpu_ns`); `Display::take_present_stats()` is the time spent waiting to present. `report_timing` in main.cpp puts them, the recording and drawing times and the sound queue under F3's graph each second and in the log each 5 s.
- **Logs and crashes** (`runtime/include/qilin/rt/report.hpp`): rt::log and rt::fatal also write to `mkdd.log` in the user folder (`mkdd.old.log` the run before). The crash handler (signals; Windows' unhandled-exception filter; std::terminate) reports with the registers and back chain of the guest thread it happened on (`set_thread_cpu`, set by start_game and each guest thread's host thread). A watchdog thread in main.cpp logs a game that draws nothing for 10 s unpaused, or a main loop stopped for 5 s.
- **Dynamic blend state** (`Gpu::dynamic_blend`, `dynamic_logic`): with VK_EXT_extended_dynamic_state3 the renderer keys pipelines on `base_key` (topology, depth output, vertex layout; the logic op only where it's static) and sets blending per draw (`set_dynamic_blend`, again after each copy, whose pipelines don't have it dynamic); the full keys are still what `pipelines.keys` remembers.
- **Measuring**: `Display::frame_times()` keeps the intervals between new frames presented (`host/include/qilin/host/frame_times.hpp`, 1200 frames); F3 or `performance_overlay` shows them, and the log has them and the retraces' lateness every 5 s.

## Where the game stands

Checked on 2026-11-26, on Windows, with no controller and an empty memory-card folder:

- **Unattended, drawn without a window** (`--offscreen`, frames dumped), 170 s from the extracted disc: the progressive-scan question with its countdown, the Lakitu loading scene, the opening movie, the title screen with "PRESS START", and the attract mode's demo races, in the game's own cycle, without a stop. With the progressive setting off there is no question and the movie follows the loading scene directly.
- **From a disc image** (`.ciso`), headless, 60 s: the same start, into the movie.
- **Tests:** all 27 of `ctest -E qilin_padtest` pass on the merged tree.

Two faults stood between the loading scene and the movie, both found with `MKDD_THREADS` (below):

- `LCEnable` did nothing, and the movie decoder (`THPVideoDecode`, `800F0848`) refuses to decode unless HID2's locked-cache bit is set. The player's decode thread suspended itself with the error, and the loading scene faded to black for good while still being drawn.
- `VIGetNextField` was constant under progressive scan. The player takes a frame of its 59.94-a-second movie only when the next field is the one it expects, and flips that after each, so it showed one frame and waited for ever.

Known and open:

- **The movie plays slowly.** Its decode thread uses nearly all of the wall time while it plays (17.4 s of CPU in about 20 s of movie; `MKDD_THREADS` shows each thread's time), the game falls to about 155–190 frames in every 300 retraces, and the sound is skipped ahead. The decoder's inverse DCT is paired-single code, lifted with the exact single-precision and quantised load/store helpers; making that fast is the recompiler's work (`recompiler-core.md`). Measured with an MSVC build while other builds were running on the same PC.
- **Nothing past the title with input** was tried here.
- **SRAM changes kept** between runs (`OSSetSoundMode`, `OSSetProgressiveMode`), and the **reset switch** (nothing sets `g_reset_button`).
- **`OSResetSystem`'s shutdown form**, `VIGetCurrentLine`, drive errors and the cover, the DVD's 512 KB steps: not modelled; the first two stop the runtime if reached. The eight ETH functions have no native code: they are reached only with a Broadband Adapter, which no port reports.

## Finding what a game that stands still waits for

- `MKDD_THREADS=<seconds>` logs, once at that much game time (frames handed to VI over 60), every guest thread: state, priority, what it sleeps on, the time it has had the CPU, and its call stack as return addresses. `MKDD_PEEK=<address>:<words>` (hex) adds that much guest memory.
- `MKDD_DVD_LOG=1` logs each file as the game opens it, with the time.
- `MKDD_TRACE=1` logs every call into a replacement.

## Next

1. The movie decoder's speed (the lifter's paired-single and quantised code).
2. Play with a controller: the menus, a race; write whatever replaced function that reaches.
3. SRAM changes back into the settings file, and a key or pad button for the reset switch.
