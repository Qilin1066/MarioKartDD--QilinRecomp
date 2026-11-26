# Code analysis: names, libraries and the lifted/replaced boundary

Last updated 2026-11-26 (data symbols placed and used as evidence; every function of the US `main.dol` assigned to an object file of the disc's linker map, 98.5% of them named; the boundary decided per object).

## Where the names come from

The disc carries `files/debugInfoS.MAP`, the linker map of the **debug** build: 15,705 functions with name, size, object file and library, in the debug link order. It is not this DOL's map. What the two builds share and where they differ, as found:

- **`Dolphin.a` is the same archive in both.** 1,553 of its 1,587 functions are in the retail DOL with identical sizes, in the same order.
- **JSystem and the game were compiled for release.** Most sizes differ; the debug text is 3,781,288 bytes, the retail text 2,960,984. Inline and template functions (weak symbols: 3,702 in the map) that the debug build emitted are mostly gone.
- **The link order differs.** The debug build links `JSystemV.a`, `Dolphin.a`, Bando, Yamamoto, Kaneshige, Kameda, Kawano, Osako, Sato, Shiraiwa, Inagaki. The retail build links JSystem, `Dolphin.a`, then the game's libraries in alphabetical order (Bando, Inagaki, Kameda, Kaneshige, Kawano, Osako, Sato, Shiraiwa, Yamamoto), JSystem's parts in another order (J3DU, JKernel, JSupport, JGadget, JUtility, JMath, J2DGraph, J3D, JFramework, JParticle, JAudio; in the debug build J3DU, JParticle, JFramework, J3D, J2DGraph, JMath, JHostIO, JUtility, JGadget, JSupport, JKernel, JAudio), and a few objects moved inside their library (Yamamoto's `kartBodyStrat.cpp` sits after `kartvec.cpp`). Functions keep their order inside an object. A size alignment of the two lists as they stand therefore names `Dolphin.a` and little else correctly.
- **The debugging tools are stripped:** JHostIO (JHI*, JOR*), Inagaki's audio viewers (JAH*, JAW*, `GameAudioCustomMenu`, most of `GameAudioHostIO`), `hio.c`. 26 objects (1,094 functions) are judged absent, and others kept one or two functions.
- **A weak function is kept once, in the first object that emits it, and that object need not be the same in both builds.** The retail `J3DUMotion.cpp`, the first user of J3D's matrix-calculator templates, is followed by 0x2D00 bytes of them (`800057B4`-`800084B4`, 67 functions, unnamed) where the debug build has them in `J3DJoint.cpp` and other J3D objects.

`tools/analysis/port_symbols.py` ports the names in six steps (its header has the detail):

1. **Anchors**: runs of three functions whose sizes are equal in both builds and occur once in each, at least two of them not weak. An object's anchors say where it is. Lone anchors count only where they cannot be chance; a library's objects must form one run (two objects with the same run of sizes swap places otherwise: `JKRDvdFile.cpp` and Kameda's `PrintMemoryCard.cpp` did).
2. **Object order**: anchored objects by retail position; an object without anchors stays with its neighbour in the debug order, and where the two neighbours are apart in the retail DOL, on the side where it aligns better.
3. **Alignment** of the debug functions (in retail object order) with `qilin cfg`'s functions between anchors: equal sizes score most (`Dolphin.a` pairs only by equal size), `__sinit_*` functions pair only with functions the `.ctors` table points at, skipping a weak function is nearly free, skipping another costs.
4. **Call graph**: the map's link tree records who first referenced each symbol. A function whose referrer is matched should be among the referrer's call targets or the addresses it builds; the alignment is repeated with that as bonus and penalty. Unanchored objects that end with under a third of their functions matched and no call-graph confirmation are taken out (among hundreds of stripped functions a few always fit by size), and the alignment is repeated.
5. **Weak functions elsewhere**: leftover retail functions are compared with every object's unmatched weak functions, accepted only on equal sizes or the call graph's word (83 found).
6. **Data**: the map's data symbols are placed in the retail DOL (below), and the alignment is repeated with them as evidence: a function should reference the data it was the first to reference in the debug build, and a retail function that reads a variable few others read is probably the function that owns it.

```sh
python3 tools/analysis/port_symbols.py --map <disc>/files/debugInfoS.MAP --dol <disc>/sys/main.dol --qilin build/qilin \
    --pal third_party/mkdd/Release_eu/symbols.yml --pal-slices third_party/mkdd/Release_eu/dol_slices.yml --pal-fix -o symbols.txt --data-output symbols-data.txt
```

The output (`ADDRESS SIZE NAME LIBRARY OBJECT CONFIDENCE`) is derived from the game and stays out of git.

### Confidence

| | Meaning | Names |
|---|---|---|
| A | equal size, and the neighbours in the object, the call graph or the data it reads agree | 8,178 |
| B | equal size alone, or unequal size confirmed by the call graph or the data | 3,328 |
| C | placed by order between matched neighbours; or equal size with a doubt (a contradiction in the call graph, or a skipped function of the same size next to it) | 1,392 |
| D | a guess: order and a rough size only; or contradicted by the PAL list | 370 |

A and B are meant to be acted on, C and D to be confirmed by reading the code first. A name of any class is still worth reading the code for before a replacement is written against it.

### Coverage

13,268 of the 13,471 retail functions are named (98.5%). The 203 without a name are mostly weak functions that changed size (67 of them at the start of J3DU), and the debugger stub's.

| Library | Debug functions | Named in retail | A | B | C | D |
|---|---|---|---|---|---|---|
| `Dolphin.a` (SDK, MSL, runtime, TRK) | 1,587 | 1,553 | 1,550 | 3 | 0 | 0 |
| `JSystemV.a` | 4,024 | 3,767 | 1,329 | 1,547 | 778 | 113 |
| of which JAudio | | 777 | 314 | 300 | 103 | 60 |
| of which JKernel | | 312 | 110 | 138 | 42 | 22 |
| of which JUtility | | 189 | 85 | 90 | 10 | 4 |
| `SatoS.a` | 2,385 | 2,199 | 1,544 | 441 | 144 | 70 |
| `ShiraiwaS.a` | 2,118 | 1,918 | 1,266 | 376 | 249 | 27 |
| `InagakiS.a` | 1,767 | 343 | 107 | 136 | 67 | 33 |
| `KaneshigeS.a` | 1,513 | 1,310 | 824 | 339 | 110 | 37 |
| `KamedaS.a` | 776 | 724 | 407 | 271 | 14 | 32 |
| `YamamotoS.a` | 632 | 597 | 535 | 55 | 2 | 5 |
| `OsakoS.a` | 537 | 515 | 347 | 110 | 8 | 50 |
| `KawanoS.a` | 364 | 340 | 267 | 50 | 20 | 3 |
| `BandoS.a`, `kartLocale.o` | 2 | 2 | 2 | 0 | 0 | 0 |

The 34 `Dolphin.a` functions without a retail counterpart are dead-stripped ones (`ARGetBaseAddress`, `hio.c`, parts of MetroTRK's `targimpl.c`) and assembly the function finder does not split.

### How it was checked

- **Against people who read the code.** The doldecomp/mkdd decompilation (CC0, `third_party/mkdd`) has a hand-made symbol list for the **PAL** retail build: 4,417 names with addresses in the text. The two retail builds are near-identical (all of JSystem is at the same addresses in both), so a name in both lists must keep its distance to the PAL address among its neighbours. Of 4,309 comparable names 48 disagree: A 16 of 3,065 (0.5%), B 19 of 889 (2.1%), C 9 of 196 (4.6%), D 4 of 159. Most of the 48 read are places where the two builds differ inside a translation unit (`LogoApp.cpp`, `system.cpp`, `SystemFile.cpp`: region-specific code), so the distance changes legitimately; `--pal-fix` demotes all 48 to D regardless. The check is independent of the port: the list is not an input to the names (`--pal-slices`, the PAL translation-unit order, is used only to place objects without anchors beside the right neighbour, and the numbers above are the same without it).
- **Object extents.** The same decompilation lists the PAL text range of 318 translation units. For 288 of the 317 found the port's extent of the object (first to last named function) has exactly the PAL size; the other 29 are same-named objects (`main.c`), genuine PAL differences (Osako's applications), and objects whose last functions are unnamed weak ones (`JUTFont.cpp` is 0xD4 short).
- **By reading code.** Every start-up address (`dol-analysis.md`) was read. The hardware-access trace (`runtime-analysis.md`) is a blind test of the names: of the 217 lifted functions it finds writing the GX FIFO or reading low memory, every one has a name that says so (`drawK2D__10K2DPicture`, `J3DFifoLoadPosMtxImm`, `GXBegin`, `AIStartDMA`, `__DSPHandler`). The devices agent read `800B15E4` and found it to be `ARGetSize` where an earlier version said `ARGetBaseAddress` with confidence A: two 8-byte functions in a row, one stripped. Since then an anchor does not fix its last function when the next debug function has the same size, the call graph decides between such twins, and without its word the name is C.
- **What is not checked:** runs of tiny functions (4 to 16 bytes: `blr`, getters) of equal size inside one object can still be shifted by one where no caller distinguishes them; they are B or C. Names in the audio-tool remnants of Inagaki's library are the least certain in the list.

### Data symbols

`tools/analysis/port_data.py` (run by `port_symbols.py --data-output`) places the map's data symbols. Data is laid out like the text, object by object, a symbol's size the same in both builds, but symbols the release build does not need are gone (assert strings above all), so within an object a symbol's distance to its debug address can only stay or fall. What fixes that distance is a reference from a named function: the link tree says which function first referenced each symbol or each object's section base (CodeWarrior addresses an object's `.data`, `.rodata` and `.bss` from one base), and the same function in the retail DOL references a handful of data addresses (r13- and r2-relative, `lis`-built, and the displacements used on them). Per object the longest staircase of non-rising distances through those candidates is taken.

| Section | Named symbols in the map | Placed | A | B | C |
|---|---|---|---|---|---|
| `.rodata` | 401 | 246 | 162 | 1 | 83 |
| `.data` | 1,529 | 1,159 | 720 | 39 | 400 |
| `.bss` | 429 | 258 | 147 | 4 | 107 |
| `.sdata` | 1,295 | 927 | 610 | 11 | 306 |
| `.sbss` | 1,562 | 1,417 | 1,042 | 12 | 363 |
| `.sdata2` | 83 | 79 | 67 | 1 | 11 |

A: on or between references that agree (two or more in the object); B: chosen between two distances because only one address is referenced (a vtable must hold function pointers); C: a single reference or the nearest one only. Compiler-made names (`@123`: literals, strings) are placed but not written. Checked the same way as the functions, against the PAL list's data names: of 370 comparable A names 26 differ, all but two by 4 to 40 bytes, which the two builds' own data differences may explain as well as an error here; single references were right only half the time in `.sbss`, hence C. Three placements were confirmed by code read independently: `scAspect__10SystemData` at `803CBE08` (the enhancements work found the same address from the getters), `__AR_Size` at `803CB0F4` (`ARGetSize` reads it), `__OSArenaLo`/`__OSArenaHi` at `803C96D0`/`803CB380` (OSArena's accessors).

What the data added to the function names is modest: 26 more A names, the twins settled where a variable tells them apart. **Naming virtual functions from vtables was tried and dropped:** the link tree does not list a vtable's functions in slot order, and pairing the unnamed ones by elimination was wrong for 6 of the 29 names the PAL list could check.

## The libraries in the retail DOL

`tools/analysis/make_plan.py` writes `data/GM4E01/plan.tsv` from the ported symbols and a decision per object file (its tables): 70 ranges that tile the code of `.init` (`80003100`-`80005544`) and `.text` (`800056C0`-`802D8518`) exactly. `qilin lift --plan` accepts it: 12,756 functions lifted, 510 replaced, 205 dropped, every instruction outside replaced and dropped code lifted.

Words are 4-byte instructions in the library's functions; *fp* counts floating-point instructions, *ps* paired singles, *priv* privileged ones (`mfmsr`, `mtmsr`, `rfi`, segment registers, and SPRs other than LR, CTR, XER).

| Library | Functions | Words | fp | ps | priv | Plan |
|---|---|---|---|---|---|---|
| .init: OS start-up | 8 | 214 | 0 | 0 | 2 | replace |
| .init: `memset`, `__fill_mem`, `memcpy` | 3 | 78 | 0 | 0 | 0 | lift |
| .init: debugger stub and exception vectors | 29 | 397 | 0 | 0 | 245 | drop |
| game: kartLocale | 1 | 61 | 0 | 0 | 0 | lift |
| JSystem/J3DU | 129 | 4,963 | 656 | 22 | 0 | lift |
| JSystem/JKernel | 322 | 13,223 | 25 | 0 | 4 | lift |
| JSystem/JSupport | 31 | 822 | 0 | 0 | 0 | lift |
| JSystem/JGadget | 6 | 123 | 0 | 0 | 0 | lift |
| JSystem/JUtility (with its force-feedback classes) | 236 | 14,712 | 1,067 | 38 | 0 | lift |
| JSystem/JMath | 9 | 419 | 222 | 48 | 0 | lift |
| JSystem/J2DGraph | 897 | 39,555 | 4,010 | 360 | 0 | lift |
| JSystem/J3D | 1,239 | 45,791 | 1,912 | 411 | 4 | lift |
| JSystem/JFramework | 20 | 1,641 | 89 | 4 | 0 | lift |
| JSystem/JParticle | 201 | 13,785 | 3,527 | 1,070 | 0 | lift |
| JSystem/JAudio (JAInterface, JASystem, its DSP driver, JAUtility) | 787 | 32,711 | 2,599 | 215 | 8 | lift |
| dolphin/base | 22 | 69 | 6 | 0 | 16 | replace |
| dolphin/mtx | 36 | 1,001 | 389 | 455 | 0 | lift |
| dolphin/dvd | 90 | 4,449 | 0 | 0 | 0 | replace |
| dolphin/vi | 24 | 1,945 | 0 | 0 | 0 | replace |
| dolphin/ai | 19 | 569 | 0 | 0 | 0 | lift |
| dolphin/ar | 7 | 1,717 | 0 | 0 | 0 | replace |
| dolphin/arq | 5 | 231 | 0 | 0 | 0 | lift |
| dolphin/dsp | 11 | 384 | 8 | 0 | 0 | lift |
| dolphin/card | 93 | 6,978 | 0 | 0 | 0 | replace |
| dolphin/gx | 169 | 8,769 | 678 | 68 | 0 | lift |
| debugger links (amcstubs, odenotstub, OdemuExi2) | 23 | 686 | 0 | 0 | 0 | drop |
| dolphin/exi | 27 | 2,028 | 0 | 0 | 0 | replace (`EXIGetType`, 70 words, lifted) |
| dolphin/gd | 7 | 735 | 0 | 0 | 0 | lift |
| dolphin/ip, with md5 and base64 | 336 | 28,533 | 8 | 0 | 0 | lift |
| dolphin/eth | 23 | 3,458 | 0 | 0 | 0 | replace |
| dolphin/os | 151 | 6,923 | 153 | 128 | 146 | replace |
| dolphin/os: OSAlloc, OSArena, `OSReport`, `__init_user`/`__init_cpp`/`_ExitProcess` | 13 | 210 | 8 | 0 | 0 | lift |
| dolphin/pad: PADClamp, `__PADDisableRecalibration` | 5 | 349 | 41 | 0 | 0 | lift |
| dolphin/pad | 19 | 1,616 | 0 | 0 | 0 | replace |
| dolphin/si | 37 | 2,053 | 0 | 0 | 0 | replace |
| dolphin/db | 5 | 59 | 8 | 0 | 2 | replace |
| dolphin/thp | 22 | 7,846 | 126 | 3,528 | 10 | lift |
| Logitech steering-wheel force feedback | 36 | 2,190 | 39 | 0 | 0 | lift |
| dolphin/upnp | 49 | 4,788 | 0 | 0 | 0 | lift |
| EABI runtime | 43 | 894 | 47 | 0 | 0 | lift |
| EABI MSL (C library) | 138 | 12,749 | 1,438 | 32 | 0 | lift |
| MetroTRK | 153 | 5,828 | 14 | 4 | 222 | drop (`__TRK_write_console`, `__read_console`, 94 words, replaced) |
| game: Bando | 1 | 28 | 0 | 0 | 0 | lift |
| game: Inagaki (audio) | 350 | 17,339 | 2,121 | 351 | 0 | lift |
| game: Kameda (2D screens, scenes) | 744 | 87,397 | 12,648 | 391 | 8 | lift |
| game: Kaneshige (race, course, movies) | 1,312 | 62,651 | 8,647 | 3,698 | 0 | lift |
| game: Kawano (drivers, animation) | 340 | 22,861 | 2,487 | 161 | 0 | lift |
| game: Osako (system, applications, network) | 512 | 28,374 | 684 | 20 | 4 | lift |
| game: aspect getters (`System::getPerspAspect`, `Div2`, `Div4`) | 3 | 6 | 3 | 0 | 0 | replace |
| game: Sato (items, objects, effects) | 2,208 | 101,193 | 14,964 | 6,916 | 0 | lift |
| game: Shiraiwa (map objects, wipes) | 1,920 | 79,779 | 15,997 | 6,688 | 0 | lift |
| game: Yamamoto (kart physics, camera) | 597 | 65,451 | 18,361 | 4,275 | 0 | lift |

Totals, in functions: **702,275 words lifted (94.8%)**, 31,609 replaced (4.3%), 6,911 dropped (0.9%).

## Why each library is lifted or replaced

The rule is the base's: a library that drives the hardware or the processor's privileged state at the level of the operating system is **replaced** at its API; software whose exact behaviour matters more than native speed, reaching the hardware only through a replaced API, is **lifted**. The exceptions are the ones the base made, for the same reason: **GX, AI and DSP are lifted** and the hardware they drive is emulated, because the game and JSystem write the GX FIFO themselves (1,849 stores from 136 lifted functions, 55 of them outside GX, `runtime-analysis.md`) and JAudio brings its own DSP microcode and task driver, so there is no API to replace them at.

Replaced:

- **`.init` start-up** (`__start`, `__init_registers`, `__init_data`, `__init_hardware`, `__flush_cache`): the runtime starts the game.
- **dolphin/os**: `OS.c`, alarms, cache, context, error handlers, interrupts, messages, mutexes, memory protection, reset, RTC, threads, time, `OSAudioSystem.c` (it drives the DSP's registers during boot), `OSFont.c`, `OSLink.c`, `OSSync.c`. 146 privileged instructions. Message queues and mutexes are thread bookkeeping and go with the scheduler.
- **dolphin/base** (`PPCArch.c`: MSR, HID, L2CR, WPAR accessors; 16 privileged instructions) and **dolphin/db** (debugger hooks: no debugger).
- **dolphin/dvd**, **vi**, **pad**, **si** (with the steering-wheel transfer layer, `SISteering*.c`), **exi**, **card**: each drives its own registers.
- **dolphin/ar**: ARAM is reached through the DSP's registers (35 accesses in 4 functions). JKernel and JAudio use it through `ARAlloc`, `ARStartDMA`, `ARRegisterDMACallback`, `ARInit`, `ARGetSize`.
- **dolphin/eth** (`eth.c`, `ethsec.c`): the Broadband Adapter's driver over EXI. The game's LAN mode calls eight of its functions (`ETHInit`, `ETHSendAsync` …); replaced so that the adapter's absence, or later a native transport, is answered at this API.
- **The game's three aspect getters** (`801D0F50`, `801D0F58`, `801D0F60`: each returns one float of `SystemData`, `scAspect` or `scAspectDiv2`; read): replaced so that the enhancements can answer with the window's shape (`enhancements.md`).
- **MetroTRK's `__TRK_write_console` and `__read_console`**: MSL's `FILE` procedures for the console call or point at them (`stdin`'s read procedure at `80348CE4`, `80348D34`, `80348D84`); with no debugger they report an I/O error.

Lifted, where a reader might expect otherwise:

- **dolphin/arq**: read (`800B2E58`-`800B31F4`): a two-priority request queue that calls `ARStartDMA` and is called back by AR's interrupt routine; the trace finds no hardware access in it.
- **dolphin/os: OSAlloc, OSArena** (heap and arena bounds: no hardware, no privileged instruction), **`OSReport`** (formats through MSL's `vprintf`), **`__init_user`, `__init_cpp`, `_ExitProcess`** (the static constructors), and **`__OSInitAudioSystem`** (the OS replacement's choice: the boot's hand-shake with the DSP runs as written against the emulated DSP).
- **dolphin/pad: `PADClamp`** (stick maths) and **`__PADDisableRecalibration`** (a flag byte at `0x800030E3` between interrupt calls), **dolphin/exi: `EXIGetType`** (a table over `EXIGetID`): pure code, at the devices replacement's request.
- **dolphin/ip, upnp, md5, base64**: the TCP/IP stack is software over `eth`, alarms and threads. Its only low-memory access is the bus clock at `0x800000F8` (timer arithmetic).
- **The Logitech force-feedback library** (`allsrc.c`: `LG*`, `VDevice_*`, `Effect_*`, a simulation thread): software over `SISteering` and OS threads.
- **dolphin/thp, mtx, gd**, **MSL** and the **EABI runtime**, **all of JSystem**, **the game**.

Dropped: **MetroTRK** with its `.init` vectors, **amcstubs/odenotstub** and **OdemuExi2**. No lifted function calls or points at any of them (`check_replacements.py --dol`), apart from the two console procedures above.

Against the devices agent's bindings: everything it binds natively is in a replaced range. The plan replaces all of EXI (bar `EXIGetType`) and all of `Pad.c` (bar `__PADDisableRecalibration`) where that agent expected the unbound rest to stay lifted and unreached. The difference shows only if such a function is reached: replaced, the game stops and names it; lifted, it would run into the EXI or SI registers.

## Checks of the boundary against the code

- **Replaced functions that lifted code calls**: 144 by direct call (the three aspect getters among them), one more through data (`tools/analysis/check_replacements.py cfg.json --symbols symbols.txt --dol main.dol`): the to-do list of the replacements.
- **Privileged instructions in lifted code**: 38, all moves to or from the paired-single quantisation registers (GQR2-GQR6, SPRs 914-918), which are user state in practice: `OSInitFastCast` inlined into `JKRTask::run`, `J3DSys::drawInit`, `JASTaskThread::run`, `JASAudioThread::run`, `SceneOption` and `SceneRecord`'s constructors, `System::init` and `THPInit`, and `__THPDecompressYUV` saving and setting GQR5 and GQR6. The lifter has to treat them as registers of the context.
- **Unknown words in lifted code**: the case bodies of four switches the function finder misses (`dol-analysis.md`): a recompiler matter, not a boundary one.
- **Hardware access in lifted code**: `runtime-analysis.md`.

## Tools

| | |
|---|---|
| `tools/analysis/port_symbols.py` | the names (above) |
| `tools/analysis/port_data.py` | the data symbols, and the data each function reads as evidence |
| `tools/analysis/make_plan.py` | `data/GM4E01/plan.tsv`; without `-o` it checks the committed plan against the symbols |
| `tools/analysis/check_replacements.py` | replaced or dropped functions that lifted code calls or points at |
| `tools/analysis/audit_natives.py` | the native replacement tables against the plan and the names: wrong addresses, entries in lifted ranges, and which called functions have no native yet |
| `tools/analysis/boot_path.py` | a static trace from the constructors, `System::init` and `System::run`: the replaced functions and hardware the boot reaches, in order |
| `tools/analysis/hardware_access.py` | hardware and low-memory access per library |
| `tools/analysis/gcdol.py` | shared: the DOL, the map, the plan, `qilin`'s output |
