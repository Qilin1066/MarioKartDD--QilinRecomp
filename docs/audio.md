# Sound

Mario Kart: Double Dash!! plays its sound with Nintendo's JAudio (JSystem's `JAS*`/`JAI*`), not with the SDK's AX. JAudio brings its own DSP microcode, its own DSP task manager (`osdsp.c`, `osdsp_task.c`: its own `__DSPHandler`, `DSPAddTask`, `DSPAddPriorTask`) and its own mail protocol. All of JAudio, the SDK's `dsp` and `ai` libraries and `__OSInitAudioSystem` are lifted game code and run unchanged. Under them are three things of ours:

- **the DSP's and the audio interface's registers and ARAM** (`mkdd/src/dsp.cpp`, `audio.hpp`);
- **the DSP itself** (`mkdd/src/dsp_core.hpp`, `dsp_core.cpp`): registers, memories, arithmetic, the hardware registers at `0xFFxx`;
- **the game's microcode, statically recompiled** to C++ at build time from the player's DOL by `qilin-dsp` (`recomp/src/dsp/dsplift.cpp`), running on that DSP. Nothing of the microcode is in the repository, and nothing interprets it at run time.

This file records what the microcode is and does (found with `tools/analysis/dspdis.py`), why it is recompiled rather than imitated, the hardware under it, and how it is tested. Addresses are those of GM4E01 rev 0.

## The microcode

`DspBoot` (`dsptask.c`, `0x800A1C40`) fills a DSP task (`audio_task`, `0x80364E00`) and adds it with `DSPAddPriorTask`: instruction image `jdsp` at `0x80343680`, `0x1D20` bytes (3,728 words) loaded at word 0, started at `0x0000`, resumed at `0x0010`; a data-memory save area of `0x2000` bytes (`AUDIO_YIELD_BUFFER`); priority `0xF0`; init callback `DspHandShake`. The image's hash (xor each byte in, rotate left 3: the one emulators use) is `2FCDF1EC`. It is of the family Dolphin calls the "Zelda" microcode. A linear disassembly is 2,673 instructions, of which 2,443 are reached; the rest is tables and dead code.

    python tools/analysis/dspdis.py <disc>/sys/main.dol --dol 80343680 1D20 --stats -o <scratch>/jdsp.asm

### Layout of the code

| words | what |
|---|---|
| `0000`-`0011` | vectors: reset to `0012`; the external interrupt (`000E`) to `05B8`; resume (`0010`) to `004E`; the accelerator's and the others are `RTI` |
| `0012`-`0066` | boot (clear data RAM, send the greeting), command dispatch, common "command done" tail (`0043`) |
| `0067`-`0101` | helpers: copy mail words, block copies, mixing with a constant gain |
| `0102`-`0242` | per sub-frame: clear the buses, the four effect lines, the wait for channels to be released (`0239`) |
| `0243`-`041C` | command 2: the frame (sub-frame loop, channel loop, final mix and output) |
| `041D`-`04BF` | commands 6-9: copies between main memory and ARAM through the DSP |
| `0523`-`057F` | DMA and accelerator (raw port) helpers |
| `05B8`-`05EF`, `0692`-`06C4` | the external interrupt's handler: takes the CPU's mails |
| `05F0`-`065D` | end of frame: `DCD10005`, the CPU's answer, task switch through the ROM |
| `065E`-`0691` | mail waits and sends |
| `06C5`-`06F8` | the main loop: pop commands from the ring |
| `06F9`-`087B`, `098F`-`0B2D` | sample sources: the two ADPCMs, PCM8, PCM16, streams from main memory |
| `087C`-`095E` | the built-in oscillators |
| `0B2E`-`0BEB`, `0C84`-`0CA8` | per-channel filters: FIR, biquad, one-pole |
| `0CA9`-`0D85` | mixing a channel into the buses with volume ramps; the "auto mixer" |
| `0D86`-`0E0D` | the resampler |
| `0E1B`-`0E8E` | constants and the surround bus's mix |

### Mails and interrupts

CPU to DSP mails land in the CPU mailbox; the DSP sees bit 15 of the high half set until it reads the low half. DSP to CPU mails are `DCD1xxxx` (with the DSP interrupt at the CPU, `DIRQ`) and `F355xxxx` (without); the microcode waits for each to be read before going on.

**Boot.** The ROM's loader greets with `8071FEED`; the SDK's `__DSP_boot_task` answers `80F3A001` + image address, `80F3C002` + place, `80F3A002` + length, `80F3B002` + 0, `80F3D001` + start. The microcode clears its data RAM, zeroes `0x200` bytes at ARAM 0, sends `DCD10000` (interrupt: `__DSPHandler` marks the task running and calls `DspHandShake`) and `F3551111` (which `DspHandShake` reads), enables the external interrupt and idles in its main loop.

**Commands** (`DSPSendCommands2`, `0x800A1D00`): the CPU sends a mail holding the word count *n*, raises the DSP's external interrupt (`DSPAssertInt`, control bit 1), then sends *n* words, each when the last has been taken. The handler at `05B8` copies count and words into a ring at `0280` (48 words, wrap register `0x2F`) and counts the command (`0352`); the main loop pops it. The first word is `0x80 | command` in the top byte, a parameter byte, a parameter word.

| cmd | sender | words | what |
|---|---|---|---|
| 1 | `DsetupTable(n, CH_BUF, DSPRES_FILTER, DSPADPCM_FILTER, FX_BUF)` | 5 | channel count (low word), the channel blocks' address, then two tables the DSP DMAs in at once: `0x280` words to `0000` (resampling filter, 64 phases of 4 taps, and the oscillators' wave tables), `0x20` words to `0300` (16 ADPCM coefficient pairs); the effect lines' address |
| 2 | `DsyncFrame(subframes, left, right)` | 3 | one frame: parameter byte the number of sub-frames, low word the master level (`DSP_MIXERLEVEL`; `0x1000` is 1), then where in main memory the left and right output go |
| 3, 0, 10-12, 14, 15 | | | nothing |
| 4, 5 | | 3 | DMA main memory to data RAM, and back (debugging) |
| 6-9 | | 3 | copies between main memory and ARAM through the accelerator's raw port |
| 13 | | 2 | stores two parameters at `0374` (unused by the rest) |

Each finished command is answered `DCD10004` (interrupt) then `F355` + the command's first high word (`F3558100` for `DsetupTable`), which `DspFinishWork` matches to the callback queued by `DspStartWork`.

**A mail with count 0** takes the other path in the handler: the next mail is `(group << 16) | mask` (`DSPReleaseHalt2`, from `DSP_CreateMap2`): the 16 channels of that group may now be processed (`034E` = 16 × (group + 1)), those with their mask bit set (bit 15 first) are; the masks live at `04FC`-`04FF`.

**A frame** (command 2) is *subframes* sub-frames of 80 samples (2.5 ms at 32 kHz). Per sub-frame: clear the buses; run the four effect lines; then for each channel wait until its group is released (`0239`), DMA its block (`0x180` bytes) into data RAM at `0400`, process it unless inactive or finished, DMA the block back; then send `DCD10004` + `F355FF`*nn* (*nn* the sub-frame's number from 1: JAudio's request callback updates the channels for the next and releases them again), mix the buses to the output with the master level, and DMA 80 left and 80 right samples to the two addresses, which advance by 160 bytes. After the last sub-frame it sends `DCD10005` (interrupt) and waits for one mail whose low word selects: 0 halt, 1 switch task (ten more mails, then the ROM), 2 reset to the ROM, 3 go on. JAudio's `__DSPHandler` (`0x800A2000`) answers `CDD10003` unless another DSP task waits (`CDD10001`).

An external interrupt with no mail waiting queues a "yield" (`DCD10002`, then `HALT`); JAudio never does that.

### The channel block

`JASDsp::TChannel`, `0x180` bytes; 64 of them. In the DSP it sits at `0400` (word *w* = byte offset / 2). Names where doldecomp/mkdd's `JASDsp.h` (CC0) has them, otherwise from the microcode's use:

| bytes | DSP | what |
|---|---|---|
| `00` | `0400` | `mIsActive` |
| `02` | `0401` | `mIsFinished`: set by the DSP when the sound ends; the CPU clears both (`replyFinishRequest`) |
| `04` | `0402` | `mPitch`: source samples per output sample, 4.12 (`0x1000` is 1; at most `0x7FFF`) |
| `08` | `0404` | set by `playStart`: reset the decoder's state; cleared by the DSP |
| `0A` | `0405` | set by the DSP when the sound looped or ended this sub-frame |
| `0C` | `0406` | `mPauseFlag`: hold the last sample |
| `0E` | `0407` | `mMixerInitDelayMax`: samples of delay to carry between sub-frames |
| `10`-`3F` | `0408`-`041F` | six mixers of four words: the bus's address in the DSP (0: none; `SEND_TABLE`), target volume, volume now (ramped over the sub-frame), delay |
| `50`-`59` | `0428`-`042C` | the "auto mixer": pan, effect and "dolby" bytes, volume now and target, on flag: the DSP derives the mixers' volumes itself from the sine table |
| `60` | `0430` | resampler: fraction of the position |
| `64` | `0432` | decoded samples left over |
| `66` | `0433` | the last output sample |
| `68` | `0434` | start offset in samples |
| `70`, `74` | `0438`, `043A` | current ARAM address; samples remaining |
| `78` | `043C` | resampler: the last four samples |
| `80`-`A7` | `0440`-`0453` | FIR history |
| `A8` | `0454` | biquad history |
| `B0`-`CF` | `0458`-`0467` | sixteen decoded samples (the last two are the ADPCM history) |
| `100` | `0480` | the source: bytes per ADPCM frame (9 or 5), 8 PCM8, 16 PCM16, 32/33 a stream from main memory; anything else an oscillator number (`setOscInfo`) |
| `102` | `0481` | loops |
| `104`, `106` | `0482`, `0483` | ADPCM history at the loop start |
| `108` | `0484` | `mFilterMode`: bits 0-4 FIR taps, bit 5 biquad |
| `10A` | `0485` | `mForcedStop`: fade out and finish |
| `110`, `114` | `0488`, `048A` | loop start; loop end or length, in samples |
| `118` | `048C` | the wave's ARAM address in bytes |
| `11C` | `048E` | length in samples |
| `120` | `0490` | `mFir8FilterParam[8]` |
| `148` | `04A4` | `mIirFilterParam[8]`: biquad; `[4]` (`04A8`) the one-pole "dist" filter |

The effect lines (`JASDsp::Fxline`, four of `0x20` bytes, DMA'd to `03A8` every frame): on flags, buffer length in sub-frames, the delay buffer's address **in main memory**, send bus and volume, return bus and volume, eight FIR taps. Each line reads its delayed sub-frame from main memory, optionally filters it, mixes it to its return buses, and writes the bus it listens to back to the buffer. Reverb therefore costs two DMAs of 160 bytes a line a sub-frame and no ARAM.

### Sample sources and the accelerator

The microcode uses the accelerator **only as a raw port into ARAM** (`ACDRAW`, `0xFFD3`, with `FORMAT` 5 for bytes, 6 or 10 for words; start and end at `FFFFFFFF`, so its address never wraps and no accelerator interrupt occurs). It never reads decoded samples (`ACDSAMP`) and does not use the hardware's ADPCM decoder: Nintendo's older ADPCM ("AFC") is decoded in software at `07EB`:

- **4-bit ADPCM** (format byte 9; wave format 0): 9-byte frames of 16 samples; first byte scale (high nibble) and coefficient pair (low nibble, into the table at `0300`); sample = nibble << scale + (c0 × last + c1 × before) >> 11.
- **2-bit ADPCM** (5; wave format 1): 5-byte frames, the same with 2-bit codes two bits higher.
- **PCM8** (8), **PCM16** (16): read through the port, bytes shifted up 8.
- **32, 33**: sample data DMA'd from main memory (addresses at `0436`/`043B`): how JAudio feeds streams (`JASAramStream` plays through ARAM instead).
- **Oscillators** (`087C`): square, saw and table lookups (sine, triangle from the tables that came with command 1), some with noise or frequency modulation from the bus at `0CA0`.

Loops reload the address and the ADPCM history from the block; at the end without a loop the block's finished flag is set.

### Resampling, filters, mixing

- **Resampler** (`0D86`): position accumulates `mPitch` (4.12) per output sample. Below a ratio of 4 each output is a 4-tap FIR over the last four source samples with taps from the table at `0000` chosen by the top six bits of the fraction (`DSPRES_FILTER`, CPU-supplied); from 4 up it picks samples without filtering. The filter is not an identity at ratio 1 (it low-passes slightly).
- **Filters**, in this order: the one-pole filter (`04A8` non-zero), a FIR of up to 24 taps (`0484` bits 0-4), a biquad (bit 5).
- **Mixers**: each of six adds the channel, times a volume ramped linearly across the sub-frame, to a bus of 80 samples in data RAM. Buses: `0D00` left, `0D60` right (the main pair), `0DC0`, `0E20`, `0E80`, `0EE0` (the effect lines'), `0CA0`, `0F40`, `0FA0`, `0B00`, `09A0` (auxiliary: a surround pair that is delayed through a 160-sample ring at `0A60`, phase-inverted into the right channel and mixed down at `0E46` and `03D8`-`03FB`, as Dolby Surround encoding does).
- **Output**: left and right times the master level (unsigned 4.12) with saturation, 16-bit big-endian, 80 samples each a sub-frame, to **two separate buffers** in main memory. JAudio (`JASAiCtrl`) interleaves them into the audio DMA's buffer and adds its own streams.

### What it needs of the DSP's ROMs

Nothing we cannot supply. It never reads the coefficient ROM (`0x1000`-`0x17FF`: its resampling table comes from the CPU) and calls no ROM routine. It **jumps** into the instruction ROM in two places: `0x8000` (reset, answer 2 above) and `0x80B5` (answer 1: the loader's entry for a task switch, with the new task's image address in `ix0:ix1`, length in `ix3`, place in `ix2`, start in `ar0`, and data memory to restore in `ax0`, `ax1.l`, `ax1.h`: the order of `__DSP_exec_task`'s mails). Both are the documented loader, done natively in `dsp.cpp` (`rom_entry`); no ROM dump is used or wanted. A microcode that read ROM data or called ROM code would stop the game with the address.

## Why the microcode is recompiled, not imitated

The base imitates AX at a high level (`ax.cpp`, gone here). For this microcode the choice went the other way:

- It is small (2,443 reached instructions) and self-contained: no ROM code, no ROM data, no accelerator decoding. Everything hard about emulating a DSP at a low level (ROM dumps, the accelerator's formats, cycle timing) is absent; what is left is arithmetic.
- It is intricate where an imitation must be exact to sound right and to keep JAudio's bookkeeping in step: two software ADPCMs with loop history, a polyphase resampler with its own state layout, three filters, ramps, an "auto mixer", effect lines through main memory, a surround matrix, a dozen oscillators, all in the DSP's 40-bit arithmetic with its two extension modes, unsigned multiplies and wrapping address registers. A rewrite by hand is a few thousand lines to get bit-exact; the lifter is 700 and gets the behaviour by construction.
- Cost, measured (`mkdd_dsp_check`, one x86-64 core of the development PC, MSVC Release): see "Cost" below. It is well inside what a Raspberry Pi 4 core can spare, and the DSP's work runs only when the CPU hands it a sub-frame.

So: static recompilation, structured so hot paths can later be replaced by hand-written code and checked against the lifted code (the mixing loop and the ADPCM decoder are where the time goes).

### The lifter (`qilin-dsp`)

`qilin-dsp lift <main.dol> -o dsp_ucode.cpp` finds the microcodes it knows in the DOL (by address and hash; a table in `dsplift.cpp`), follows the code from the vectors and from listed roots (targets of computed jumps: the command table, the oscillator table, the four answers to `DCD10005`), and writes one function per microcode: a `switch` on the DSP's program counter into straight-line C++ with `goto`s. Calls push the DSP's own call stack and jump; returns and computed jumps go back through the `switch`. Block loops use the DSP's loop stacks, tested only after instructions that end a loop. A jump to an address that was not lifted stops with the address. The instruction table (also in `dspdis.py`) is from the publicly documented encodings (the "GameCube DSP User's Manual" kept with Dolphin).

Details that matter (`dsp_core.hpp`):
- Accumulators are 40 bits kept sign-extended in 64; the product is one 40-bit value (no microcode this knows reads its parts; one that did would fail to lift, as would reading the stack registers).
- Flags are computed as documented; logic instructions on `acX.m` set them from the 16 bits; the "sticky overflow" too.
- 40-bit mode (`SET40`): loads to `acX.m` sign-extend and clear, stores from it saturate.
- Multiplies double unless `M0`; with `SET15`, an `ax?.l` operand of the `MULX` family is unsigned.
- `MOVPZ`, `MULxMVZ`, `ADDPAXZ` round the product to even.
- Address registers step with Dolphin's formulas for the wrap registers (verified in the test against plain masking for power-of-two rings).
- Extensions: loads and stores happen with the values from before the instruction; a load into the accumulator the instruction itself writes is refused at lift time (hardware ORs the two; nothing here does it).
- The two-register extended loads (`'LD`, `'LDAX`) read the first word twice when both addresses are in one 1 K page, as hardware does.

### When the DSP runs

On the guest thread, synchronously: whenever the CPU sends a mail, takes one, raises the external interrupt or releases the halt, the lifted function runs until the DSP halts or **provably waits**: at a taken backward jump, if nothing was stored and no device was touched since the same jump was last taken, and every register is the same as then, only the CPU can change what happens next, so the function returns with the program counter at the jump's target. This finds the mail waits, the wait for channels and the main loop without knowing them. The external interrupt is taken on entry and wherever the microcode has just set the enable bit (`SBSET #5`, `RTI`).

So the CPU's busy-waits on the mailbox end on their first poll, a sub-frame is mixed inside the register write that released its last channel group, and everything is deterministic. The base's AX emulation ran on the guest thread in the same way. Moving the DSP to its own host thread is possible (the core has no global state) but then the lifted CPU code's polling loops need a way to sleep; not done.

### Cost

Measured with `mkdd_dsp_check` (one core of the development PC, an x86-64 desktop; MSVC Release). Seven sub-frames a frame; percentages are DSP time against the sound's duration.

| load | share of real time |
|---|---|
| idle (no voice; four effect lines off) | see the report's numbers |
| 64 oscillator voices | see the report's numbers |
| 64 ADPCM voices | see the report's numbers |

A Raspberry Pi 4 core is roughly four to six times slower on this kind of code; a full 64-voice load would then take a large part of one core, a typical race (some 20-30 voices) a fraction of that. If that proves too much on the Pi: the flags are computed after every arithmetic instruction though the microcode tests only zero, sign, overflow and "logic zero", and only after a few of them; a liveness pass in the lifter removes most of that work.

## The hardware under it (`dsp.cpp`)

- **DSP registers** (`0xCC005000`): mailboxes; control (reset, external interrupt, halt, the AI DMA / ARAM / DSP interrupt flags and enables, init); the ARAM DMA registers (`0x5020`-`0x502A`: a copy done at once, then the ARAM interrupt flag; `0x5016` reads "ready"); the audio DMA. The interrupt is a level, as on hardware: while a flag is set with its enable, the OS's handler runs (interrupts 5 AI DMA, 6 ARAM, 7 DSP).
- **The ROM's loader**: `8071FEED` when the DSP first runs after a reset; the `80F3xxxx` parameters; the image is hashed and the lifted program with that hash started. An image that was not lifted into the build is fatal, with its hash. The stub `__OSInitAudioSystem` uploads through ARAM is not run; its answer (`80544348` and the init-code bit) is given, as in the base.
- **ARAM**: 16 MB, `aram_base()` / `kAramSize` in `audio.hpp`. Addresses wrap at 16 MB (no expansion).
- **The audio DMA** is the sound's clock, at the rate the game sets in the AI's control register (32 or 48 kHz): when a buffer has played, it is handed to the sink (`set_audio_sink`), the next is latched and the AI DMA interrupt raised; JAudio answers with the next buffer and the next `DsyncFrame`.
- **AI registers** (`0xCC006C00`): control, volume, the sample counter (counted from the time base); the sample-counter interrupt is fatal if enabled (JAudio does not use disc streaming).

## Tests

- `mkdd_dsp_test` (in `ctest`, no game data): the core's arithmetic against values worked out from the documented rules; and `mkdd/tests/dsp_synth.txt`, a 283-word program written for the test, lifted at build time and run with the test as the CPU: mails, both DMAs, loops, a filter by `MADD` with extended loads, saturating stores, a ring, the accelerator, a computed jump, the interrupt handler, idle detection, `HALT`.
- `mkdd_dsp_check <main.dol>` (built with `QILIN_GAME_DOL`; by hand): boots the game's lifted microcode and plays JAudio's part: handshake, `DsetupTable` with the DOL's tables, frames of `DsyncFrame` and channel releases, voices set up as `TChannel::playStart`/`setWaveInfo` do. It checks every mail against the protocol above, writes the mix to a WAV and reports level, clipping and time.

      mkdd_dsp_check <disc>/sys/main.dol --osc 4 --voices 8 --wav <scratch>/osc.wav
      python tools/analysis/dsp_waves.py <disc>/files/AudioRes/GCKart.baa          # lists waves with the arguments to pass
      mkdd_dsp_check <disc>/sys/main.dol --aw <disc>/files/AudioRes/Waves/NintendoLogoMario_0.aw \
          --offset 0 --bytes 49131 --format 0 --samples 87332 --volume 0x7fff --frames 300 --wav <scratch>/logo.wav
      python tools/analysis/dsp_waves.py <GCKart.baa> --decode NintendoLogoMario_0.aw 0 --waves <Waves> --dol <main.dol> -o <scratch>/ref.wav

  The second WAV is the same wave from `dsp_waves.py`'s own decoder; the two correlate at 0.9999 (the difference is the resampler's filter), the voice ends in the frame its length says, and the ARAM reads equal the wave's bytes.

Not tested yet: `dsp.cpp`'s registers under the lifted JAudio (the game does not reach its sound initialisation yet), task switching through `0x80B5`, the effect lines, streams.

### What to listen for

None of this has been heard: the checks are numerical. When the game first plays sound:

1. The Nintendo logo's voice and the title music: right pitch and speed (wrong: the AI rate or the pitch format), no crackle at 2.5 ms intervals (sub-frame boundaries: volume ramps, resampler history).
2. Looping engine sounds: no click at the loop point (ADPCM loop history).
3. Voices ending: no stuck or cut-off voices (the finished flag's handshake).
4. Reverb in tunnels and menus (effect lines), and that it decays rather than rings or is absent.
5. Stereo: sounds pan left and right correctly (the DMA's sample order is right then left); surround-encoded sounds are not hollow in plain stereo.
6. Music streams (`Stream/*.ast`): continuous, in step with the rest.
7. Loudness against the Dolphin emulator on the same scene; clipping in busy races.
