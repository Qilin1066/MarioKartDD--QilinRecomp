# Runtime analysis: hardware access, privileged state, exceptions (roadmap step 1)

Last updated 2026-11-25. What the runtime has to provide beyond the replaced libraries' APIs, found in the code before any of it runs. Function names are the ported ones (`code-analysis.md`); the library plan is `data/GM4E01/plan.tsv`.

## Hardware access

`tools/analysis/hardware_access.py` traces the base register of every load, store and cache-block instruction back through its function (`lis`, `addi`, `ori`, register copies, and loads of a pointer variable whose initial value is in the DOL) and classifies the addresses it can resolve:

```sh
python3 tools/analysis/hardware_access.py --qilin build/qilin --dol path/to/main.dol [--symbols symbols.txt] [--list]
```

| Class | Addresses |
|---|---|
| wgpipe | `0xCC008000`: the GX write-gather pipe (the command FIFO) |
| cp, pe, vi, pi, mi | `0xCC000000`, `0xCC001000`, `0xCC002000`, `0xCC003000`, `0xCC004000`: command processor, pixel engine, video, processor interface, memory interface |
| dsp | `0xCC005000`: DSP mailboxes and control, ARAM, the audio DMA |
| dvd, si, exi, ai | `0xCC006000`, `0xCC006400`, `0xCC006800`, `0xCC006C00` |
| mem-unc | `0xC0000000`-`0xC17FFFFF`: main memory through its uncached mirror |
| locked | `0xE0000000`-`0xE0003FFF`: the locked cache |
| lowmem | `0x80000000`-`0x800030FF`: the OS's globals and the boot information |

Accesses found, by library (cells: accesses, then functions):

| Library | Plan | wgpipe | vi | pi | mi | dsp | dvd | si | exi | ai | mem-unc | locked | lowmem |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| .init: OS start-up (__start, __init_registers, __init_data) | replace |  |  |  |  |  |  |  |  |  |  |  | 8 (2) |
| JSystem/JKernel | lift |  |  |  |  |  |  |  |  |  |  |  | 1 (1) |
| JSystem/JUtility | lift | 24 (1) |  |  |  |  |  |  |  |  |  |  | 4 (4) |
| JSystem/J2DGraph | lift | 348 (16) |  |  |  |  |  |  |  |  |  |  |  |
| JSystem/J3D | lift | 167 (14) |  |  |  |  |  |  |  |  |  |  |  |
| JSystem/JFramework | lift | 16 (1) |  |  |  |  |  |  |  |  |  |  | 6 (3) |
| JSystem/JParticle | lift | 15 (2) |  |  |  |  |  |  |  |  |  |  |  |
| JSystem/JAudio (JASystem) | lift |  |  |  |  |  |  |  |  |  |  |  | 1 (1) |
| JSystem/JAudio (JASystem: DSP driver) | lift |  |  |  |  | 2 (1) |  |  |  |  |  |  |  |
| dolphin/dvd | replace |  |  | 2 (1) |  |  | 78 (22) |  |  |  |  |  | 28 (17) |
| dolphin/vi | replace |  | 54 (8) |  |  |  |  |  |  |  |  |  | 3 (3) |
| dolphin/ai (its registers and DMA are emulated) | lift |  |  |  |  | 12 (4) |  |  |  | 46 (12) |  |  | 1 (1) |
| dolphin/ar (ARAM) | replace |  |  |  |  | 35 (4) |  |  |  |  | 1 (1) |  |  |
| dolphin/dsp (task manager; the DSP's mailboxes are emulated) | lift |  |  |  |  | 12 (6) |  |  |  |  |  |  |  |
| dolphin/card | replace |  | 1 (1) |  |  |  |  |  |  |  |  |  | 10 (8) |
| dolphin/gx (its FIFO and registers are emulated) | lift | 847 (81) |  | 2 (2) |  |  |  |  |  |  |  |  | 1 (1) |
| OdemuExi2 (debugger link over EXI2) | drop |  |  | 1 (1) |  |  |  |  | 19 (7) |  |  |  |  |
| dolphin/exi | replace |  |  |  |  |  |  |  | 7 (1) |  |  |  | 4 (3) |
| dolphin/ip (TCP/IP stack, over eth) | lift |  |  |  |  |  |  |  |  |  |  |  | 51 (33) |
| dolphin/eth (Broadband Adapter driver) | replace |  |  |  |  |  |  | 2 (1) |  |  |  |  | 4 (2) |
| dolphin/os | replace |  | 3 (3) | 11 (7) | 12 (6) | 26 (5) | 2 (2) |  | 9 (2) | 3 (2) |  | 2 (2) | 90 (37) |
| dolphin/pad | replace |  |  |  |  |  |  |  |  |  |  |  | 7 (3) |
| dolphin/pad: __PADDisableRecalibration (a flag in low memory) | lift |  |  |  |  |  |  |  |  |  |  |  | 5 (1) |
| dolphin/si | replace |  | 1 (1) |  |  |  |  | 34 (11) |  |  |  |  | 4 (2) |
| dolphin/db (debugger hooks) | replace |  |  |  |  |  |  |  |  |  |  |  | 1 (1) |
| Logitech steering-wheel force feedback (over SISteering) | lift |  |  |  |  |  |  |  |  |  |  |  | 2 (2) |
| dolphin/upnp (over ip) | lift |  |  |  |  |  |  |  |  |  |  |  | 9 (7) |
| MetroTRK (debugger stub) | drop |  |  |  |  |  |  |  |  |  |  |  | 1 (1) |
| game: Kameda (2D screens, scenes) | lift | 20 (1) |  |  |  |  |  |  |  |  |  |  | 2 (2) |
| game: Kaneshige (race, course, movies) | lift | 116 (5) |  |  |  |  |  |  |  |  |  |  |  |
| game: Osako (system, applications, network) | lift | 32 (1) |  |  |  |  |  |  |  |  |  |  | 2 (2) |
| game: Sato (items, objects, effects) | lift | 46 (4) |  |  |  |  |  |  |  |  |  |  |  |
| game: Shiraiwa (map objects, wipes) | lift | 218 (10) |  |  |  |  |  |  |  |  |  |  |  |

**The runtime's to-do list from this** (2,008 accesses in 217 lifted functions; `--symbols` lists each function):

- **The GX FIFO, from everywhere.** 1,849 stores to `0xCC008000` in 136 lifted functions: 81 in GX itself, and 55 in JSystem and the game, which write vertices and commands through GX's inlined macros: J2D's drawing (`J2DPicture::draw`, `J2DWindow::drawFrameTexture` …, 16 functions), J3D's shape drawing and matrix loads (`J3DShape::draw`, `J3DFifoLoadPosMtxImm` …, 14), `JUTResFont::drawChar_scale`, `JFWDisplay::clearEfb`, JParticle's point and line drawers, and 21 game functions (screen effects, wipes, `THPGXYuv2RgbDraw`, `K2DPicture::drawK2D`). The command-stream decoder at the pipe is required, as in the base.
- **PI's FIFO reset, from GX.** `__GXAbort` and `GXAbortFrame` write `0xCC003018`. The trace finds no access to GX's other register blocks (CP at `0xCC000000`, PE at `0xCC001000`) although the SDK's GX has them: it reaches them through pointers the trace cannot resolve (not confirmed by reading `GXInit`). Expect CP and PE register reads and writes from lifted GX at run time.
- **The DSP's registers, from three lifted libraries.** dolphin/dsp (`DSPCheckMailToDSP`, `DSPCheckMailFromDSP`, `DSPReadMailFromDSP`, `DSPSendMailToDSP`, `DSPAssertInt`, `DSPInit`: mailboxes at `0xCC005000`-`0xCC005006`, control at `0xCC00500A`), dolphin/ai's DMA half (`AIInitDMA`, `AIStartDMA`, `AIStopDMA`: `0xCC005030`-`0xCC005036`; `__AIDHandler` acknowledges at `0xCC00500A`), and JAudio's own DSP interrupt handler (`__DSPHandler`, `800A2000`, in `osdsp_task.c`: `0xCC00500A`). JAudio brings its own DSP task driver (`dspproc.c`, `dsptask.c`, `osdsp.c`, `osdsp_task.c`): the DSP emulation has to deliver its interrupt to that lifted handler and speak the mailbox protocol of JAudio's microcode.
- **AI's streaming registers**, from lifted dolphin/ai: `0xCC006C00`-`0xCC006C0C` in 12 functions.
- **Low memory, read by lifted code:**
  - `0x800000F8`, the bus clock, in 54 functions: tick arithmetic inlined into the network stack, UPnP, JUtility, JFramework, the Logitech library, AI, GX and a few game functions. It must hold the console's value.
  - `0x80000028`, the memory size, in `JKRHeap::initArena`.
  - `0x800030C8` in `JUTException::showMapInfo_subroutine`, `0x800030DC` in the IP stack's `DoListen`, `0x800030E3` in `__PADDisableRecalibration` (a flag byte, read and written).
- **Nothing else**: no lifted function touches VI, MI, the disc interface, SI or EXI registers, the uncached mirror or (by a constant address) the locked cache. Data holds no pointer to a hardware register.
- **The locked cache, through the API.** The THP player calls `LCEnable` (from `THPPlayerInit`), and the decoder's row functions call `LCStoreData` and `LCQueueWait` (replaced OS functions); their work area in the locked cache is addressed through pointers the trace cannot follow. The runtime needs the locked-cache range usable once `LCEnable` is called.
- **What the trace can't see:** an address passed in from a caller or computed at run time. During bring-up every access to a mirror or a hardware register traps with its address, so anything this misses shows up the first time it runs.

## Privileged state in lifted code

38 instructions, all on the paired-single quantisation registers (SPRs 914-918, GQR2-GQR6): `mtspr` to GQR2-GQR5 in `JKRTask::run` (`8001701C`), `J3DSys::drawInit` (`8004E224`), `JASTaskThread::run` (`80091AF0`), `JASAudioThread::run` (`80092BB8`), `SceneOption`'s and `SceneRecord`'s constructors (`8012E684`, `80131044`), `System::init` (`801D0504`) and `THPInit` (`800F7D20`), the SDK's fast-cast set-up inlined; `__THPDecompressYUV` (`800F1868`) saves GQR5 and GQR6, sets them and restores them. They are per-thread state. No lifted function reads or writes the MSR, a BAT, a segment register or HID.

## Replaced functions the game calls

`tools/analysis/check_replacements.py cfg.json --symbols symbols.txt --dol main.dol` lists them with their callers: 144 by direct call (141 of the SDK and the debugger stub, and the game's three aspect getters), and `__read_console` through MSL's `FILE` tables. No dropped function is called or pointed at by lifted code.

## C++ exceptions

The game can't throw. `extabindex` has five entries, all in the runtime's array helpers (`__destroy_new_array`, `__destroy_arr`, `__construct_array`, `__partial_array_destructor`'s destructor, `__construct_new_array`, `800FF0F8`-`800FF4A4`), and of `Gecko_ExceptionPPC` only `__register_fragment` and `__unregister_fragment` are linked: no `__throw`, no unwinder. JSystem and the game were built without exception tables.

**For the runtime:** `extab` and `extabindex` are dead data, and lifted code needs no unwinding support. `__init_cpp_exceptions` (the first `.ctors` entry, `800FFC3C`) stays lifted and registers its fragment as on the console.

## Not analysed yet

Frame timing (`JFWDisplay` sleeps on alarms in `threadSleep` and `JFWDrawDoneAlarm` and waits for retraces through VI's replaced API; whether the game steps by frames or by time is not read), the interrupt sources lifted handlers expect (`__OSSetInterruptHandler` and `__OSUnmaskInterrupts` are called from lifted code), are still to do. Data symbols are placed now (`code-analysis.md`).
