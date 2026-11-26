# Recompiler core: status

Last updated 2026-11-25: the core adapted from the Wii game it was written for (PunchOutWii--QilinRecomp v0.22.0, itself from RhythmHeavenFever--QilinRecomp) to Mario Kart: Double Dash!! on the GameCube: the supported build, a GameCube guest memory map, one more switch shape, a *data* class for the tables that sit in `.init`, a differential test over the game's own functions, and lifted code that compiles without a warning. Every number below is for `GM4E01` revision 0 unless it says otherwise. The design sections are the base's and still describe the code.

## Language and toolchain
Everything is **C++23**: the recompiler, the runtime and the generated code. The C++23 features in use so far are `std::expected` (the DOL loader), `std::print`/`std::format` (the tools), `std::ranges` algorithms and `std::string::contains`.

Minimum compilers are GCC 14, Clang 19, Xcode 16 and Visual Studio 2022 17.10. CMake checks these at configure time, and refuses 32-bit targets. Clang 18 is excluded because it can't use libstdc++'s `std::expected`.

Runtime code (what players run: the DOL loader and SHA-256 so far) avoids `std::format`, because the macOS runtime is built for 10.15 and Apple's libc++ only has it from 13.3. The developer tools are built for macOS 13.3.

CI builds with warnings as errors (`-Wall -Wextra -Wpedantic -Wshadow`, MSVC `/W4`) and runs the tests on six configurations:
- Linux x64 with GCC 14 (plus the decoder sweep, below)
- Linux x64 with Clang 19
- Linux ARM64 with GCC 14
- macOS ARM64 and macOS x64 (Intel): the runtime built for macOS 10.15, the tools for 13.3
- Windows x64 with MSVC

## Done

### DOL loader (`recomp/src/dol.*`, `sha256.*`)
It parses the header's 7 text and 11 data slots, BSS and entry point, and builds the flat guest image from the lowest section to the end of the highest section or BSS. Sections inside the BSS range (the small-data sections CodeWarrior's linker puts there) keep their bytes; the rest of BSS is zero. `Dol::load`/`parse` return `std::expected<Dol, std::string>`: a malformed file is an error message, never a crash. It rejects:
- sections whose bytes lie outside the file;
- sections or BSS outside MEM1 (`0x80000000`–`0x81800000`);
- sections that overlap in memory;
- code that isn't word-aligned;
- an entry point that isn't an instruction in a text section.

The SHA-256 of the whole file identifies the build; `known_build()` lists the one this port supports: `GM4E01` revision 0 (USA), SHA-256 `e96b8578451b9157e2b68fe5e918ebb572940c3ea54d6c8c7d45c24382bf12ae`, 3,572,640 bytes, entry `0x80003154`, `.init` at `80003100`–`80005600` and `.text` at `800056C0`–`802D8520` (742,616 words of text, 164 distinct instructions). The SHA-256 is our own (FIPS 180-4), tested against the standard vectors and the padding edges.

### Decoder (`recomp/src/decoder.*`)
Our own, table-driven. The GameCube's Gekko (a 750CXe) runs the same instruction set as the Wii's Broadway (a 750CL), paired singles included, so nothing here changed for the GameCube but one invalid form (below). One row per base instruction (**222**: the 750CL's integer, floating-point, load/store, branch, condition-register, system, cache and synchronisation instructions, and the paired singles), with its opcode, extended opcode, operand fields in objdump's order, and flags (record/overflow/link/absolute bits, branch, load, store, float, paired, privileged, cache, sync) for the analysis passes and the lifter. The decode tables are built once: one slot per primary opcode, and for the five primary opcodes with extended opcodes (4, 19, 31, 59, 63) a slot per value of the 10-bit extended-opcode field (bits 21–30), filled from the rows (10-bit, 9-bit XO, 6-bit and 5-bit A-form extended opcodes). Bit numbers in these docs are IBM's, from 0 at the most significant. Building the tables fails if two rows claim the same encoding.

**Every bit an instruction doesn't define must be zero**, so a word decodes as invalid when:
- a reserved bit is set (including the fields later Power ISA versions added: branch hints, `L` fields, `TH`, `EH`, …);
- it's an invalid form: a load with update whose base is 0 or its target; a store, floating-point or paired-single update with base 0; `lswi` with the base among the registers loaded; `lswx` with `rD` equal to `rA` or `rB`; a `bc`/`bclr`/`bcctr` with a non-zero "z" bit in BO; `bcctr` that decrements CTR; a compare with `L=1` (64-bit);
- it isn't a 750CL instruction (`fsqrt`, `fsqrts`, `tlbia`, the 603e's `tlbld`/`tlbli`, 64-bit and AltiVec instructions …).

**One invalid form decodes:** `lmw` with its base among the registers it loads (`lmw r0, 0(r3)`). The manual calls it invalid, binutils rejects it, but the 750 runs it and MetroTRK's two entry routines restore a whole context that way, so the word is in this game's text. It decodes as `lmw` (`ppc::lmw_loads_base` tells), so those routines are functions rather than code that runs into an invalid word; the lifter refuses the form (what the base register ends up holding isn't verified on hardware), which makes them stubs, and they are debugger code the plan drops.

**Cross-check against GNU binutils 2.42** (`tools/check/verify_decoder.py`, `objdump -M broadway,raw`): the text of every word, mnemonic and every operand, must match, and a word must be valid for both or neither.

Where there is no PowerPC objdump (the PC this port was made on), `verify_decoder.py --dol main.dol --assembler "python -m ziglang"` checks the other way round with LLVM's assembler, as independent of qilin as binutils is: every distinct word qilin decodes is written as qilin's text, assembled for `powerpc` `-mcpu=750`, and must come back as the same word. That checks the mnemonic and every operand field of each valid word. It can't check that an invalid word is invalid (the sweep with objdump does), and zig's own `objdump` has no disassembler.

| Set | Words | Result |
|---|---|---|
| Sweep of the encoding space, 8 seeds | ~38 million | **0 unexplained disagreements**; 4.15 million valid words per seed identical |
| Every word of the base's Wii `main.dol` (before this port) | 512,944 | **0 unexplained disagreements**; 505,952 valid and identical |
| Every word of this game's text sections, through LLVM's assembler | 742,616 | **no disagreements**: 740,888 valid words, 134,265 distinct; 129,044 of them assembled back to themselves from qilin's text; 5,221 left out (paired singles and `dcbz_l`, which LLVM doesn't have); `mftb` comes back as `mfspr` (8 words, operands the same) |

The sweep takes random words for every primary opcode and every extended-opcode value (each register field left zero half the time, so valid forms of instructions with reserved fields come up), then every single-bit neighbour of every word either side accepts, which tests the edge of each instruction's reserved bits. CI runs one seed (about 4.8 million words, 30 s).

The reviewed disagreements, all counted by class and none failing the check:
- **binutils leaves out `mftb`** in its `broadway` and `750cl` sets; the 750CL has it, and the SDK's `OSGetTime` uses it.
- **binutils accepts what the 750CL reserves:** later ISAs' fields (`bclr`/`bcctr` BH, `dcbt`/`dcbtst` TH, `lwarx` EH, `dcbf`/`sync`/`tlbie`/`mtmsr` L, `mtfsf` L and W, `sc` LEV, `mfocrf`/`mtocrf`), compare `L=1`, the reserved bit 9 of compares, bit 15 of `fres`/`frsqrte`, bit 31 of the paired-single indexed forms.
- **binutils accepts instructions the 750CL doesn't have:** `fsqrt`, `fsqrts`, `tlbia`, `tlbld`, `tlbli`.
- **binutils doesn't check some invalid forms:** paired-single updates with `rA=0`, `lswi` with `rA` among the registers loaded, `bcctr` that decrements CTR.
- **Formatting only:** objdump's raw mode prints `mfcr`'s absent field as `-1`. Elsewhere our text follows objdump's (`addi r3,0,0`, `bclr 20,lt,0`, `lwarx r3,r4,r5,0`, `psq_l f1,8(r0),0,0` although `rA=0` means 0 there).

**Unit tests** (`recomp/tests/tests.cpp`, no binutils or game files), for the decoder: 63 encodings pinned to objdump's exact text; 17 words that must not decode, each with its reason; for each of the 222 instructions, a canonical word that must decode to it and, for every bit it doesn't define, the same word with that bit set, which must not decode; the `lmw` form above (decodes, and is outside the lifter's slice); field accessors (sign extension, branch targets, the paired-single fields) and mnemonic suffixes.

### Function discovery + CFG (`recomp/src/cfg.*`)
CodeWarrior makes this simpler than on x86: every instruction is one aligned word, switch tables and constants live in data sections, so text holds code and padding, and each function is one contiguous range. It runs in about 2 s on the game.

1. **Strong seeds:** the entry point and every `bl` target from a linear sweep of the text. A `bl` to the next instruction is the get-PC idiom (`bl $+4; mflr`), not a call.
2. **Recursive descent**, each function bounded by the next known start. Calls continue; `blr`, `bctr`, `b`, `rfi` and an unconditional trap end a path; conditional returns and branches fork it. Execution that runs past the bound into the next function is recorded (`falls_into`): CodeWarrior's register save/restore routines are one routine with an entry point per register.
3. **Tail calls:** a `b` (or `bc`) past the bound to a *clean boundary* (a valid word that starts its section or follows code that can't fall through, or padding) becomes a start. So does one into another function's span when the branching function is strongly seeded: that span was explored before the start was known, and CodeWarrior never branches between functions except to an entry. `lis`/`addi` (or `ori`) pairs in code that build an address at a clean boundary are starts too (callbacks, thread entries).
4. **Jump tables**, CodeWarrior's pattern (`cmplwi`, `bgt`, `lis`, `slwi`, `addi`, `lwzx`, `mtctr`, `bctr`, in any order). The search walks back from the `bctr` on the straight line, follows the one branch into a block above an early return, and finds the table base in a data-pool register (`lis r18` early in the function, `addi r3, r18, n` at the switch: one writer, and for a volatile register no call after it; calls preserve r14–r31 under the EABI). **A table is proven** when a `cmplwi` on the index register, followed by `bgt` (or `bge`, or a conditional return) on that CR field, bounds it, nothing branches into the code between the check and the `bctr`, and every entry lands inside the function on a valid instruction; otherwise it's read while entries are plausible and reported. Every table in the game is proven (**302**). The Wii game the core was written for added four shapes, each proven rather than guessed, and this game a fifth:
   - **The check after the shift.** The scheduler may put `cmplwi` below `slwi`, and reuse the index register for the table's address once it's been shifted. The check may sit on either side of the shift, as long as the index doesn't change between the two.
   - **A table base set at a loop's head.** The base register is written more than once in the function (`li r31, 0` early on, `lis r31` before the loop), so "one writer" fails. It's still constant at the switch if the last write before it heads a region that can only be entered through that write: the region grows to cover every branch back into it, a branch into it from above the head fails, and so does any other write to the register inside it.
   - **A strength-reduced index.** In a loop, CodeWarrior may index the table with a register that steps by 4 and check the bound on one that steps by 1 (`lwzx r3, r3, r19` with `cmplwi r24, 6`). Proven when the last writes of both before the switch zero them together (nothing can branch in between), that zeroing heads a region holding the switch that can only be entered through it, and inside the region the only writes to either are one step of each, again together. Then `r19 == 4 * r24` wherever the switch runs. The registers may be volatile if the region makes no calls.
   - The two region rules also require that **no word in data, other than the switch's own table, points into the region**, since another switch or a function pointer could enter it that way.
   - **The table's address in a register set before a loop that is entered at its foot** (this game: `GXSetVtxDescv`, `GXSetVtxAttrFmtv`, `GDSetVtxDescv`, `DHCPProcessOptions`). The SDK's attribute-list loops are `lis r4; addi r4, r4, table; b foot; top: ... lwzx r5, r4, r5; mtctr; bctr; ...; foot: ... bne top`: the code above the switch on the straight line is the loop's top, which only the branch at the foot reaches, so no definition of the table's register is found by walking back. The same region rule applies to the whole address: the last write of the register before the switch (`lis`/`addi` on itself, or `addi r31, r3, lo` after `lis r3`) heads a region that grows to cover every branch back into it, nothing enters it from above the head, nothing in it writes the register again, and, for a volatile register (`r3`–`r12`), nothing in it calls. A pointer in data to the head itself is fine (the function's own address, when the `lis` is its first instruction); one to anywhere after it fails the proof. The four switches were 4 unresolved `bctr`s and 508 unknown words before.

   **Soundness fix:** walking back for a register's definition, the search used to step straight up through a loop head, so it could return a `lis` even when the loop body rewrote the register. Now, when the walk crosses any word a branch lands on, the definition it finds must be the only one that reaches the use (the same region check). No table changed; the case that showed it is in the tests.

5. **Weak seeds, in rounds until nothing changes:** pointers into text stored in data (vtables, `.ctors`, `extabindex`, callback tables), except words inside a jump table: every table a linear scan of the code can prove is excluded first, so a function reached only through a vtable doesn't have its case labels taken for functions. Nor is any case label of a table the scan proves, wherever a pointer to it turns up: in the Wii game, a word of binary data in `.rodata` happens to equal a case label of the 416-case switch, and seeding it cut the switch's function short before its table could be proven (286 false functions followed). Then the first word of each run of unreached code that starts at a clean boundary and isn't inside a function's span (gaps). A weak seed must not fall inside another function's span.
6. **CodeWarrior's leftovers.** Checking our boundaries against the Rhythm Heaven Fever decomp's (in RhythmHeavenFever--QilinRecomp) found three habits, now handled. (a) After a function's final tail call (`b` or `bctr`), or a trap, CodeWarrior keeps the function's own `blr`; after a final call through `bctr` (the OS's `Run`), its whole epilogue (`addi r1, r1, n` … `blr`). That code is unreachable and belongs to the function: gap seeding steps over it and coverage counts it dead, unless something branches to it or points at it, in which case it's a function of its own (a lone `blr` reached through a vtable is an empty method). (b) A stack frame is only ever set up at a function's entry, so a branch to `stwu r1, -n(r1)` (possibly after `mflr r0`) at a clean boundary is a tail call to a new function, even inside the branching function's range: the Bluetooth stack's state machine reaches each state handler that way.
7. **Validation:** a weakly seeded function that reaches an invalid word or runs off its section is dropped and never re-seeded; a strongly seeded one is a suspect. Branches to targets that are neither inside the function nor a clean boundary are suspects too.

**Coverage** (`qilin coverage`, exit status 3 while any word is unknown) accounts for every word of the text: **code** (reached), **padding** (zero or `nop`, not reached), **dead** (not reached, inside a function, after code that can't fall through, and no branch in reached code nor any pointer in data targets any word of it), **data** (not reached, nothing branches or points to it, and it starts at an address that reached code forms with `lis`/`addi`: a table the code reads, which was tried as a function start and didn't hold, or doesn't decode at all) or **unknown**.

| Section | Words | Code | Padding | Dead | Data | Unknown |
|---|---|---|---|---|---|---|
| `.init` `80003100`–`80005600` | 2,368 | 689 | 1,628 | 0 | 51 | **0** |
| `.text` `800056C0`–`802D8520` | 740,248 | 740,049 (99.973%) | 84 | 115 | 0 | **0** |

**13,472 functions** (1 entry, 7,162 called, 7 tail-called, 592 through `lis`/`addi`, 5,676 through pointers in data, 34 gaps), 740,738 reachable instructions, 5,191 indirect calls, 260 tail calls, 2 indirect tail calls (`__ptmf_scall`, `__ptmf_scall4`), 14 functions that fall into the next (the `_savefpr`/`_restfpr` chains; this CodeWarrior saves GPRs with `stmw`, so there is no `_savegpr` chain), **0 suspects**. The unmodified base reported 13,471 functions, 298 tables, 2 suspects and 9 unknown spans on this DOL; what each was:
- `800035E4`–`80003614` (12 words): the string "Metrowerks Target Resident Kernel for PowerPC" that heads MetroTRK's interrupt vector table in `.init`; `__TRK_copy_vectors` addresses it. **Data.**
- `80005544`–`800055C8` (33 words) and `800055D4`–`800055EC` (6 words): the linker's `_rom_copy_info` (ten address, address, size triples: the DOL's sections) and `_bss_init_info` (three address, size pairs), which `__init_data` walks. **Data**; both start with a word that decodes, so both were tried as functions and are the 2 rejected starts.
- `800BC398`–`800BC58C`, `800BCCFC`–`800BCEEC`, `800C5B64`–`800C5D54`, `800D4A6C`–`800D4C64` (508 words): the case blocks of the four switches above. **Code** now.
- `80110DC0`–`80110E58` and `80110EB0`–`80110EEC` (53 words), with the suspect `80110E58` ("reaches an invalid word at 80110EAC") and the rejected start `80110DC0`: MetroTRK's two entry routines, which use the `lmw` form above. **Code** now (two functions, both stubs in dropped code).
- The suspect `800E6208` ("branch to 00000060"): `__OSDBJump`, the one instruction `bla 0x60` that the OS copies into an exception vector to reach `__OSDBIntegrator`'s copy in low memory. A `bla` to an address outside the image isn't a suspect now: the CFG records it (`Function::absolute`) and the lifter gives it to the dispatcher (`hooks->call(c, 0x60)`), which stops the runtime naming the address if it ever runs.
- `800EAFA4` is a one-`nop` function that falls into `__OSInitSystemCall`: the label that ends the system-call vector's template, whose address the OS takes. Harmless, and in replaced code.

**Tests:** a synthetic program with a function found each way (entry, call, tail call, `lis`/`addi`, vtable, gap), a switch with its case after the default, a register-save chain, the get-PC idiom, an indirect tail call, a tail call into a frame set-up, the leftover `blr` and epilogue, padding and dead code, checked function by function and word by word; and a data pointer to code that runs into an invalid word, which validation must reject. For the Mario Kart: Double Dash!! rules: a stray data word equal to a case label, the check after the shift (and a variant where the index changes in between, which must stay unproven), a table base set at a loop's head (and a variant whose loop rewrites it), and a strength-reduced index (and a variant stepping by 8, which must not resolve). For this game: the loop entered at its foot (and variants where a case rewrites the register, where the loop calls with the address in a volatile register, and where a callee-saved one is rewritten on the way in: all must stay unresolved), data that code addresses (a string, and a table whose first word decodes) against the same words with nothing addressing them (unknown), and a `bla` into low memory (no suspect; lifted to a dispatcher call).

### Lifter, slice 1: integer code (`recomp/src/lift.*`, v0.4.0)
One guest function becomes one C++ function, `void fn_<address>(Cpu& c)`, over the runtime's `Cpu` (`runtime/include/qilin/rt/cpu.hpp`).

- **Register model.** The GPRs, CR fields, LR, CTR and XER's SO, OV and CA that a function uses are host locals, loaded at entry. They're written back to the `Cpu` before anything outside the function can see them (a call, a return, a trap, an indirect jump), and loaded again after a call. CR fields are separate locals, so the C++ compiler drops any compare result nothing reads: that's the CR elimination the design asks for, done by the host compiler for now. A dataflow pass that also removes the write-backs nobody reads comes later, with measurements.
- **Control flow.** Branches inside the function are `goto`s to labels at their targets; proven jump tables are a `switch` on CTR whose default traps; a branch out of the function is a tail call (`fn_X(c); return;`); `bl` is a call; `bctrl`, `blrl` and an unresolved `bctr` go through the runtime's dispatcher (`hooks->call`), with the low two bits of the target cleared as the hardware does; falling into the next function calls it. The get-PC idiom only sets LR.
- **Memory.** Loads and stores go through `read8/16/32` and `write8/16/32`: cached MEM1 is host memory at `mem + address` (and so is the locked cache), byte-swapped (one `bswap`/`rev`); anything from `0xC0000000` to `0xDFFFFFFF` (the uncached mirror, the hardware registers, the write-gather pipe) takes the out-of-line branch ("The guest machine", below), which is where the GX FIFO decoder will sit.
- **The slice:** integer arithmetic (with carries, `mulhw`, `divw` …), logic, rotates and shifts, compares, all the CR operations, `mfcr`/`mtcrf`/`mcrxr`, integer loads and stores (update, indexed, byte-reversed, `lmw`/`stmw`, `lwarx`/`stwcx.`), `dcbz` and the cache and ordering hints (no-ops on a coherent host), LR, CTR, XER and the GQRs, traps, and every branch. The game uses no overflow-enable (`o`) forms and no string loads or stores, so those stay out and fail loudly if met. `divw`'s undefined cases (by zero, INT_MIN / −1) give all ones for a negative dividend and zero otherwise, as Dolphin's interpreter does.
- **Anything outside the lifter** (in slice 1 floating point and paired singles; now only privileged and system instructions, FPSCR access and the locked cache) makes the function a stub that traps with the reason and address when called, so the whole program always links and nothing is skipped silently.

### Lifter, slice 2: floating point (v0.5.0)
Every FPR is two host locals when a function uses it: `f<n>`, the double the FP instructions work on (ps0), and `p<n>`, the second paired-single half (ps1), which Broadway writes too wherever a single-precision result is produced. Values live in host `double`s and move bit for bit; the arithmetic goes through `rt::fp` (`runtime/include/qilin/rt/cpu.hpp`), which reproduces what Broadway does, as Dolphin's interpreter records it from hardware tests:

- **The game's FPSCR, fixed:** `OSInit` calls `PPCSetFpNonIEEEMode`, so the whole game runs with NI set: a double result that comes out subnormal is a signed zero, and so is a single-precision result whose value before rounding is below the smallest normal single. Rounding is to nearest and every exception disabled. FPSCR's flags aren't kept: only the replaced OS reads FPSCR (`mffs` and `mtfsf` appear nowhere else), and no lifted code uses a record form (`fadd.`); both stay outside the slice and fail loudly.
- **NaNs:** a NaN result is the first NaN operand made quiet, in the order a, b, c (b before any negation), else the default NaN `0x7FF8000000000000`, not the host's (x86's is negative). The check costs one predictable branch on the result.
- **Single precision:** the double result rounded to single, and written to both halves. In `fmuls` and the single multiply-adds, frC is first rounded to 25 significant bits. The single multiply-adds round only once: a double `fma`, and where that lands exactly halfway between two singles, the `fma`'s own rounding error (found with an error-free transformation) decides the direction.
- **`fmadd` family:** `std::fma`, one rounding; `fnmadd`/`fnmsub` negate unless the result is a NaN. The host compiler is told never to fuse anything itself (`-ffp-contract=off`).
- **`fres` and `frsqrte`:** Broadway's estimate tables (32 base-and-decrement entries each), not the host's reciprocal.
- **`fctiw`/`fctiwz`:** the integer in the low word with `0xFFF80000` above it (and the next bit set for a negative value that converts to zero), saturating to `0x7FFFFFFF`/`0x80000000`, NaN to `0x80000000`.
- **`lfs`/`stfs`:** the manual's bitwise conversions, not the host's: no rounding, signalling NaNs kept, single subnormals handled as the hardware does. `lfs` fills both halves; `lfd` only ps0.

The rules and the tables come from Dolphin's interpreter (`Interpreter_FPUtils.h`, `Common/FloatUtils.cpp`; GPL-2.0-or-later). They're measurements of the hardware, and the code here is written from them, not copied (`prior-art.md` §8).

**Known answers** (`recomp/tests/tests.cpp`) pin the primitives: the 57 `frsqrte` results from Dolphin's hardware-derived unit tests, the `fmadds` case found in Mario Strikers Charged (hardware gives `0xBF55BF17`, a double fma rounded again gives `0xBF55BF18`, and both are checked), the bitwise single conversions, the 25-bit rounding, `fctiw`'s saturation and its negative-zero bit, NaN selection and the default NaN, and NI flushing. **The differential test** now also has every FP instruction in the slice (45, 24 encodings each), from random FPR states mixing infinities, both kinds of NaN with either sign, subnormals, the single-precision boundaries, values singles hold exactly and raw bit patterns, comparing both halves of every FPR bit for bit. The reference interpreter wires each instruction independently from the manual's operand fields (which register, which half, flushing, update forms) and shares only the `rt::fp` primitives the known answers pin down: 4,173 cases, 267,072 runs, 0 differences; planted wiring bugs (`fmuls` reading frB for frC, `lfd` filling both halves) fail at once.

### Lifter, slice 3: paired singles (v0.6.0)
Every paired-single instruction the game uses: each half is computed from the old register values, then both are written, so a destination that is also a source reads correctly.

- **Arithmetic** (`ps_add`, `ps_sub`, `ps_mul`, `ps_div`, the four multiply-adds, `ps_muls0/1`, `ps_madds0/1`): per half the same `rt::fp` rules as the single-precision scalar instructions (NaN selection, frC at 25 bits, one rounding, NI flushing). The `s0`/`s1` forms use one half of frC for both results.
- **`ps_sum0/1`:** one half is `a0 + b1` rounded to single, the other frC's half rounded to single.
- **Moves** (`ps_merge00/01/10/11`, `ps_mr`, `ps_neg`, `ps_abs`, `ps_nabs`): raw bit copies, sign bits only, no rounding. **`ps_sel`:** per half, `a >= -0.0 ? c : b`.
- **Estimates:** `ps_res` is `fres`'s table per half (not rounded again); `ps_rsqrte` is `frsqrte`'s rounded to single.
- **`ps_cmpu0/1`, `ps_cmpo0/1`:** compare one half into a CR field; FPSCR isn't kept, so the ordered and unordered forms agree.
- **`psq_l`/`psq_st` (all four forms each):** through `psq_load`/`psq_store` in the runtime, which read the GQR at run time (the game sets GQRs in code, so they can't be folded in at lift time). Types: float, u8, u16, s8, s16; the 6-bit scale is signed. Loads convert the integer to single and multiply by 2^-scale; float loads are `lfs`'s bitwise conversion; `W=1` loads one element and sets ps1 to 1.0. Stores multiply by 2^scale in single precision, clamp to the type's range and truncate toward zero; float stores flush single subnormals to signed zeros. An invalid type (1–3) traps with the address.

As before, the rules come from Dolphin's interpreter (`Interpreter_Paired.cpp`, `Interpreter_LoadStorePaired.cpp`). One case it leaves to the host: quantising a NaN to an integer type. Here that stores 0, which isn't verified on hardware. Left out: `dcbz_l` (the locked cache, used only by the replaced OS) and paired-single record forms (the game has none); both fail loudly.

**Known answers** for the conversions (types, signed scales, clamping, truncation, NaN, the float store's flushing), and **the differential test** now has all 44 paired-single instructions too, from random GQRs (a valid type in both halves 15 times in 16; invalid types must trap in both): 5,061 cases, 323,904 runs, 0 differences.

`qilin lift main.dol -o generated/ [--plan plan.tsv] [--files n]` writes `functions.hpp`, `lifted_NNN.cpp`, `table.cpp` (every function by address, for the dispatcher), `replaced.cpp` and `unlifted.txt`. On this game:

| | Functions | Lifted | Replaced | Dropped | Stubs | Instructions lifted |
|---|---|---|---|---|---|---|
| With `data/GM4E01/plan.tsv` | 13,472 | **12,755** | 511 | 206 | **0** | 702,205 of 702,205 outside replaced and dropped code |
| With no plan | 13,472 | 13,382 (99.3%) | | | 90 | 739,005 of 740,738 (99.8%) |

The 90 functions the lifter refuses all use what a recompiled program has no use for, and every one is in code the plan replaces or drops: `PPCArch.c` (20: MSR, HID0/HID2, L2CR, WPAR, DEC, the performance counters, FPSCR), `OSCache.c` (10: HID0, `dcbi`, `sc`, the locked cache's `dcbz_l` and DMA registers), `OSContext.c` (7) and `OSInterrupt.c` (3) (`mfmsr`/`mtmsr`), `OSMemory.c` (3: the BATs, `rfi`), `OSTime.c` (2: `mftb`), `OS.c` (4: `__OSFPRInit`, `__OSDBIntegrator`, the exception vector template and default handler), `OSReset.c`'s `Reset`, the system-call vector, `db.c`'s `__DBExceptionDestination`, `__init_hardware`, and MetroTRK (24 pieces of its vector table in `.init`, 12 functions in `.text`). Lifted code's only special-purpose registers are LR, CTR, XER and the GQRs (JSystem sets GQR2–GQR6 in 9 functions), all of which the lifter has. The base also wrote one file too many when the functions divided evenly among `--files` (13,472 over 32 does): fixed.

**The output compiles without a warning.** MSVC 19.44 (`/W4 /permissive-`, Release): all 32 files and `table.cpp` of the no-plan lift, every function body there is, and later of the plan's lift, as part of `qilin_game_difftest`: 0 warnings. Clang (`python -m ziglang c++ -std=c++23 -O1 -Wall -Wextra -Wpedantic -Wshadow -Werror -ffp-contract=off -fno-fast-math -c`): all 34 files of the lift with the plan (32 lifted files, `table.cpp`, `replaced.cpp`), and five of the no-plan lift including the two largest: no warning. Two things in the base's output warned under Clang and were changed in the lifter: a function that touches no state (a lone `blr`) now leaves its parameter unnamed, and single mode declares a "written" flag only where some way out reads it (and joins its entry checks as `int`s, since every one of them must run). MSVC compiles the 32 files one after another unless the target is given `/MP` (well over half an hour at idle priority here; Clang takes 15 to 100 s a file).

**Differential test** (`qilin_difftest`, in CI on every configuration, no game files). At build time `qilin_difftest_gen` makes 24 random valid encodings of each of the slice's non-branch instructions (193 with floating point and paired singles) (the decoder decides validity; memory operands are steered into a test window), and `bc`, `bclr` and `bcctr` programs over every BO the decoder accepts and a spread of BI, and lifts them: 5,061 cases. Each runs 64 times from random states (with the edge values 0, ±1, INT_MIN, INT_MAX … mixed in), lifted and on a reference interpreter (`recomp/tests/difftest/ref.cpp`) written separately from the manual's definitions: packed CR and XER, carries in 64 bits, memory byte by byte. GPRs, both halves of all 32 FPRs (bit for bit), CR, XER, LR, CTR, GQRs, the reservation, traps, indirect calls and the whole memory window must agree: 323,904 runs, 0 differences, in about 12 s. Planted bugs (`subf`'s operands swapped, `rlwimi`'s mask wrong, the CTR test inverted) each fail on the first trial.

**Differential test over the game's own code** (`qilin_game_difftest`, `recomp/tests/difftest/game.cpp`; needs `main.dol`, so not in CI: configure with `-DQILIN_GAME_DOL=...`, build the target, run it on the DOL). The test above checks each instruction alone; this one checks what the lifter makes of whole functions: the register model across calls, branches, jump tables, tail calls, falling into the next function, single mode and the write-backs on every way out.
- **How:** `main.dol` is lifted with the plan and linked into the test. For each lifted function and trial the reference interpreter runs first, on the DOL's image with everything around it made to look like a running game's memory: a heap of 64-byte objects whose first word is mostly one of the image's own vtables (found by their shape: type info, a zero, then function addresses), the rest pointers to other objects, small numbers, zeros and floats; BSS and the stack likewise; and the first megabyte of the address space backed too, so that a null or small "pointer" reads something on both sides instead of ending the run. Arguments follow each function's own first use of a register: one it loads through gets an object (or a global of the image), one it indexes with a small multiple of 4, one it compares with a small constant a number near it; `r2` and `r13` are the real small-data bases. The interpreter follows calls as the lifted code makes them (a `bl` must come back to the instruction after it; `bctrl`, `blrl` and an unresolved `bctr` must reach a function's start; a switch outside its table traps). Code the plan replaces or drops returns at once on both sides (the lifted thunk calls `hooks->replaced`, which does nothing here). A run counts when it returns or traps having stayed inside the test's memory within 20,000 instructions. Then the lifted function runs from the same state, and all 32 GPRs, both halves of all 32 FPRs, CR, XER, LR, CTR, the GQRs, the reservation, the trap and its address, and every byte of memory must agree. Runs that go astray are skipped and counted by reason.
- **Result:** 40 trials a function: **510,200 runs, 273,127 compared, in 9,861 of the 12,755 lifted functions, 277,916 of their 702,205 instructions executed (39.6%), 0 differences**. A longer run (120 trials, 50,000 instructions a run, on a slightly earlier mix of inputs) compared 815,308 runs in 9,960 functions and reached 42.4%, also with 0 differences. The first version of the test (random registers over a soup of pointers, replaced code ending the run) reached 28.7% at 40 trials. What still stops runs: wild accesses (172,039 of the 40-trial runs: code words, hardware registers at `0xCC...` and large numbers used as addresses) and calls through a word that isn't a function (58,425).
- **What it found:** no difference on either side, in the lifter or the reference, on this game. (The instruction-level test did lose `lmw` when the decoder began accepting the invalid form, because its generator judged each instruction by its all-zero encoding; that is fixed, and it is back to 5,061 cases, 323,904 runs, 0 differences.)
- **Teeth:** with `subf` wrong for one operand value in the reference the test fails at once; with `stw r29` storing a wrong bit, 315 functions differ (measured on the first version).

**Floating point against exact arithmetic** (`qilin_fp_check`, v0.13.8, in CI wherever Python 3 is). The differential test checks the floating-point wiring: operand fields, which half of an FPR is written, flushing, update forms. For the arithmetic it uses `rt::fp` on both sides. `recomp/tests/fp/fp_check.py` checks `rt::fp` itself.
- **How:** `qilin_fp_eval` applies the helpers exactly as the lifter composes them for each instruction. The script computes every result with Python's `Fraction`: exact values, rounded to nearest-even at the target precision, with gradual underflow, then each instruction's rule. Those rules are NaN priority a, b, c; the default NaN; the NI flush; `round25` (25 significant bits, ties away); the single multiply-add rounded once; `fctiw`'s saturation; `stfs`'s truncation; and quantisation's scale, clamp and truncation. No result is computed with the host's floating-point arithmetic.
- **`fres`/`frsqrte`:** checked against the manual's special cases and its 1/4096 accuracy. `frsqrte` also gets Dolphin's 57 hardware-measured results (`recomp/tests/fp/dolphin_frsqrte.json`).
- **Cases:** 40,000 random ones per run. They include constructed ones: single multiply-adds whose double result sits exactly halfway between two singles (a third of which would be wrong if rounded twice), and halfway `b`s plus products too small for any double.
- **Teeth:** planted bugs are caught. Without the single-rounding fix 2 operations fail, moving the NI threshold fails 5, breaking `round25` fails 4, and swapping the NaN order fails 11.
- **What it found:** in v0.13.8, a bug in `madd_single`. When the product is below the smallest subnormal, the fma's rounding error underflows to zero and the tie went the wrong way, so `fmadds` gave infinity instead of `FLT_MAX` for `b` halfway between them. Now fixed, and kept as a regression case.

### Lifter: single-precision leaf functions (v0.22.0)

The Wii game's hottest code in a fight was a handful of leaf loops that skin vertices with single-precision and paired-single arithmetic (`fn_801C264C`, `fn_801C21A0`). Lifted as the rest is, every FPR half is a `double`, and each single operation is a double operation, a rounding to single and a widening back, with the helpers' checks. A function qualifies for *single mode* when it is a leaf and every floating-point instruction in it is one whose result is a single (the `s` forms, the paired singles, single loads and stores, `psq_l`/`psq_st`, moves, compares). Then:
- Its FPR halves are `float` locals. Halves it reads before writing (found by a liveness pass over the function's blocks) are checked on entry: if any holds a double that isn't exactly a single, the function runs its ordinary twin instead (`fn_X_d`, emitted beside it), so nothing is assumed about callers.
- Arithmetic is on floats, by helpers (`fp::add_s`, `mul_s`, `madd_s` and so on in `cpu.hpp`) whose common path is the host's own single operation; anything but an ordinary result (zero, denormal, infinity, NaN) goes to the double helpers, which are the reference. `a*c+b` is exact in double for single operands except for its own rounding, and rounding that to single is a second rounding: `madd_by_double` proves the sum exact (an error-free transformation) wherever the double lands on a single's halfway point, and otherwise takes the slow path; with FMA (`QILIN_FUSED_FLOAT`) it is one fused operation.
- On leaving, only the halves the function wrote are written back (a must/may-written dataflow; a `bool` per half written on some paths only).

It changes no result: the unit test runs three million random and edge-case operands through every single helper against the double ones (exact and rounded ties among them), and every other trial of the differential test starts from single-valued registers so the single versions are the ones compared with the interpreter. In that game's practice fight on a 2-core test machine, frames drawn per 300 retraces went from 275-289 to 293-297.

### Single mode across calls, and the movie decoder (2026-11-26)

The opening movie showed what the leaf rule above leaves out. THP's row decoder (`__THPDecompressiMCURowNxN`, `800F4E50`: the inverse DCT of every block, all paired singles) calls the Huffman decoders and saves `f27`–`f31` with `stfd`/`lfd`, so it ran in double mode: a 608x448 frame, 3.09 million guest instructions, took 12 ms, and the movie plays at 59.94 frames a second. Single mode now reaches such functions:
- **A call** gets the halves the function has written written back, as a way out does; after it every half is the `Cpu`'s again. The halves read as singles later are fetched again there, and if one of them isn't a single-precision value the function's double version takes over from that very point: it takes a second argument, the resume point, and switches on it to a label after the call (all the state is in the `Cpu` then). Conditional calls keep a function out of single mode; compilers don't emit them.
- **`stfd`** (and, since round 4, `stfs` and `psq_st` of a half the run hasn't written on every path) stores the register as it stands: the float written back as a double if this run wrote it, else the `Cpu`'s own double, single or not (a prologue saving the caller's register). **`lfd`** loads straight into the `Cpu`, and the half is the `Cpu`'s from there (an epilogue's restore).
- **Doubles in the `Cpu`** (round 4, below): a value that isn't a single (what `lfd` loads, `frsqrte`'s estimate, `fctiw`'s integer, double arithmetic, a move of one) stays in the `Cpu`'s register and is read from there with the double helpers; the rest of the function stays in floats.
- **Left in double mode on purpose:** a function that rounds with `frsp` a value it didn't make (a double argument, or what a call returned): it would leave single mode every time it ran. Also one where a register holds a float on one path and a double in the `Cpu` on another, where both meet; and one where a paired-single instruction reads such a double.
- The halves written on some paths only are one bit mask (`written`), not a flag each.

Single-mode functions in this game: 522 as leaves, **2,073** now (453 with resume points).

**The helpers** (`cpu.hpp`), measured per operation on a quiet Ryzen 5800X:

| | MSVC before | after | Clang for MinGW before | after |
|---|---|---|---|---|
| `single(add)` | 2.3 ns | 1.0 | 1.0 | 1.1 |
| `single(mul(round25))` | 3.8 | 2.5 | 1.5 | 1.4 |
| `flush(madd)` | 2.3 | 1.3 | 17.3 | 1.8 |
| `single(madd_single)` | 5.6 | 5.5 | 19.3 | 4.8 |

- `is_nan`, `flush`, `single` and single mode's `ordinary` are compares now, not bit tests (same answers; MSVC takes a double's bits through memory), and the arithmetic helpers are forced inline.
- `fused()` asks the CPU once and uses the FMA instruction where it is there, on an x86-64 build for every CPU: MinGW's library `fma` is software (the 17 ns), and that is the compiler a Windows player's build uses. A build with FMA enabled, and ARM64, use the instruction directly as before.
- Single mode's quantised `psq_l`/`psq_st` stay in floats (a movie's DCT coefficients are loaded as `s16` pairs), and `pow2f` is the exponent field, not `ldexp`.

**Measured on the decoder itself** (`qilin_game_difftest main.dol --thp movie.thp`: `THPInit`, then `THPVideoDecode` on the first frames of the disc's movie, on the reference interpreter and lifted, every register and every byte compared, the locked cache included; then the lifted decode alone, timed):

| | Before | After |
|---|---|---|
| MSVC 19.44 `/O2` | 12.07 ms a frame (1,890 ns an 8x8 block) | **6.63 ms** (1,038 ns) |
| Clang (zig, `x86_64-windows-gnu`, `-O2`) | 10.0 ms (1,565 ns) | **6.0 ms** (940 ns), the lifter's part alone |

0 differences on the frames, in the instruction test and in the test over the game's functions. For scale: a Gekko needs about 6 ms for those instructions, so the lifted decoder now costs what the console's did, on one core of a far faster machine: the row function is still 70% of it at about 1.5 ns a guest instruction, and is where a second round would look.

### Safe points in loops (2026-11-26)

Interrupts reach lifted code only where it lets them: a replaced call, a poll loop. A loop that calls no replacement (a file being decompressed at a load, a movie frame decoded) held the sound's DMA interrupt back 33 to 44 ms, and frames of sound were lost. Every backward branch in lifted code is now a safe point *when one is due*: it tests `rt::g_poll_due`, an atomic flag (`if (poll_due()) [[unlikely]] goto P_<pc>;`), and the block it jumps to, at the function's end and out of the loop's way, writes the stack pointer back and calls `hooks->poll`, then goes round. The registers stay in the function's locals: an interrupt's handler saves and restores the `Cpu`, and a thread switch comes back to the same one, so nothing needs reading back; the handler's frame goes below `r1`, which is why that alone is written.
- The game's runtime sets the flag from a thread of its own (`start_poll_clock`, os.cpp) when a timer is due, not on a fixed tick: the guest's thread publishes the next timer deadline wherever it works one out (`next_timer_deadline` after each delivery and safe point; `note_deadline` when an alarm, a delayed device completion, the audio DMA or a disc read is set), and the clock waits on a high-resolution timer for it, woken early by a sooner one. It raises the flag at the deadline, again a millisecond after the guest takes it while it stays undelivered (interrupts off), and once 4 ms after the last publication in case a timer was set without one. Its hook (`poll_point`) clears the flag, delivers what's due under the same rules as every other safe point (nothing while the guest has interrupts off or is inside a handler) and publishes the next deadline. `MKDD_POLL=ms` keeps the first version's flag every millisecond, deadline or not (to measure against), and `MKDD_POLL=0` turns the clock off. ([runtime.md](runtime.md) has the measurements.)
- **Cost:** with the call inside the loop the movie decoder lost 6% (7.03 ms against 6.63: a call in a loop keeps the compiler from holding the loop's values in registers it must save around it); with the call's block at the function's end, nothing measurable (6.63 ms), and 1% with the hook taken on every pass (6.71 ms).
- The differential tests run with the flag set (`--poll 1`), the hook doing nothing: 0 differences.
- **In the game** (headless, 95 s: the opening movie, the title, the load and the first demo race; `MKDD_POLL=0` turns the clock off to compare): the longest stretch with the timers unpolled was 49.9 ms without safe points (the load before the race) and 8.6 ms with them, which is the idle sleep between retraces and no longer a loop; the game's threads worked 10.2 to 10.6 ms a retrace through the movie and 8.4 to 9.1 ms in the race either way.

### Wrapped functions (`plan.tsv`'s `wrap`, 2026-11-26)

A fourth kind beside lift, replace and drop, for native code that wants to run *around* a function of the game rather than instead of it. A `wrap` range is one function. Its address is the runtime's, exactly as a replaced one's is: `fn_<address>` is the thunk into `hooks->replaced`, in the dispatch table and the replaced table, so direct and indirect calls reach the native wrapper registered with `rt::provide`. The lifted original is emitted beside it as `fn_<address>_wrapped`, outside the dispatch table, and listed in `kWrapped` (replaced.cpp); the wrapper gets it with `rt::GuestFn rt::wrapped(address)` (fatal if the plan doesn't wrap that address) and calls it with the `Cpu` it was given. `qilin lift` refuses a wrap whose function is branched to or fallen into by another (it must only ever be called), whose range doesn't start at the function, or whose body doesn't lift. `tools/analysis/make_plan.py` takes the kind in its table of single functions; `rt::Program` has a third member, `wrapped`.

### `qilin` CLI (`recomp/src/main.cpp`)
`info` (JSON), `insns` (every word of the text sections: address, word, text), `disasm` (a raw file of big-endian words, for the sweep), `profile` (instruction mix of the text sections, with paired-single, floating-point and privileged counts), `cfg` (summary, `--json` for every function), `coverage` (exit status 3 while any word is unknown), `suspects` (with the rejected starts' addresses), `polls` (the poll loops, below) and `lift`. Exit status is non-zero on any error.

## The guest machine: a GameCube
`runtime/` is what lifted code compiles against (`cpu.hpp`) and the memory, dispatcher and fatal-error layer under it (`system.hpp`). For this game it is a GameCube:
- **MEM1**, 24 MB at `0x80000000`, and the **locked cache**, 16 KB at `0xE0000000`, are backed host memory at `mem + address`: a load or store there is one range check (`a - 0xC0000000 < 0x20000000`, never true for them), a `mov` and a byte swap. There is **no MEM2**: nothing is committed at `0x90000000`, so an access there faults and the crash report names it.
- `0xC0000000`–`0xDFFFFFFF` takes the out-of-line branch: the uncached mirror of MEM1 (`0xC0000000`–`0xC17FFFFF`) is the same memory; a store to the write-gather pipe (`0xCC008000`–`0xCC00801F`) goes straight to `rt::g_pipe_write` when the game's runtime has set one; everything else goes to the hooks, which give `0xCC000000`–`0xCC008FFF` (the GameCube's hardware registers) to the handlers set with `rt::set_mmio` and stop the runtime on anything else.
- No signature in `runtime/include` changed from the base. What changed is behaviour: `rt::init` commits MEM1 and the locked cache only; `rt::guest_ptr(0x90000000...)` is no longer memory; the hardware range ends at `0xCC008FFF` (the Wii's `0xCD000000` registers are fatal).
- Lifted code calls the hooks as before: `read`/`write` (the slow range), `call` (`bctrl`, `blrl`, an unresolved `bctr`; new: a `bla` into low memory, with the address), `trap`, `replaced`, `idle`.
- FPSCR is fixed, as in the base, and that holds here: this game's `OSInit` calls `PPCSetFpNonIEEEMode` too (at `800E5C0C`). `OSSetErrorHandler` and `__OSUnhandledException` read and write FPSCR through `PPCMffpscr`/`PPCMtfpscr` (the exception enables); they are replaced OS code, and lifted code never sees an FP exception.

## Poll loops in this game
`qilin polls main.dol` lists them (function, branch back, loop head): **119 loops in 26 functions**. In code the plan lifts, 44 in 11 functions, whose back edges call `hooks->idle`:

| Function | Library | Loops |
|---|---|---|
| `80019584` `JUTConsoleManager::drawDirect` | JUtility | 1 |
| `8001C724` `JUTException::printContext`, `8001CCBC` `JUTException::createFB` | JUtility | 1 each |
| `80092D28` `JASAudioThread::DSPCallback` | JAudio | 1 |
| `800A1AE0` `DsetupTable`, `800A1C00` `DspHandShake` | JAudio's DSP driver | 1 each |
| `800A1D00` `DSPSendCommands2` | JAudio's DSP driver | 3 |
| `800A2000` `__DSPHandler` | JAudio's DSP driver | 5 |
| `800B11A0` `__AI_SRC_INIT` | dolphin/ai | 2 |
| `800B3394` `__DSP_exec_task` | dolphin/dsp | 17 |
| `800B3534` `__DSP_boot_task` | dolphin/dsp | 11 |

The other 75 are in replaced or dropped code: `__ARChecksize` (35), the EXI2 debugger link (16), `dolphin/os` (15: `__OSInitAudioSystem` 8, `__OSStopAudioSystem` 5, `OSResetSystem`, `SelectThread`'s idle loop), `EXIInit` (3), `ETHInit` (3), the memory card's `InitCallback` (2) and `SIInit` (1). The names are the symbols port's (`work/symbols.txt`, from the disc's map).

## Portability of the whole tree (2026-11-26)
Every hand-written source of the merged tree (76 files: `runtime/`, `recomp/` with its tests and `dsp/`, `disc/`, `host/`, `mkdd/` with its tests, `tools/player-kit`) compiled one by one with zig's Clang for `x86_64-linux-gnu`, `aarch64-linux-gnu` and `aarch64-macos`, `-std=c++23 -Wall -Wextra -Wpedantic -Wshadow -Werror` (and `-Wno-missing-field-initializers` on the Vulkan files, as CMake has it): all clean but one, `mkdd/src/menu.cpp` for macOS, which is zig's macOS headers and not the code (`#include <cmath>` then `<float.h>` leaves `FLT_MIN` undefined there, in two lines of test; Dear ImGui's header needs it). The generated code compiles for all three: the lifted files (the two largest, the first, `table.cpp`, `replaced.cpp`) with the full warning set, and `dsp_ucode.cpp`, `dsp_synth.cpp` and the instruction difftest's cases with `-w`, as CMake compiles them (the DSP lifter's output has unused labels; a `tw rA,rA` case compares a register with itself). zig's libc++ doesn't model Apple's availability rules, so the macOS 10.15 rule (no `std::format` in runtime code) isn't checked by this.

## Efficiency, round 4 (2026-11-26)

**How it is measured.** The runtime prints, every 300 retraces, the work of the game's threads per retrace, and the
part of it spent waiting for the GPU thread; the figure here is work less that wait. Two scenes, headless: the
attract mode's title (retraces 2100-3600 of a run from boot, the same 89,700 draws every time: the steadiest
figure), its first demo race (windows 5700-6900; it varies more, as the demo isn't the same race every run), and a
four-player Versus race reached with the devices role's `p4-steps.txt` (ports 2-4 pressing A, from retrace 4200).
Builds are compared **side by side**, both running at once, so whatever else the PC is doing weighs on both: six
agents share it, and a build in the background moved a single run's figure by half. The scripts are in the work
folder (`core/bench.sh`, `bench4.sh`, `pair.sh`); `core/mbench` times single lifted functions (copied out of the
generated code) under both compilers. All numbers: Ryzen 7 5800X, the MSVC build, unless they say otherwise.

**Where the time went** (sampled from outside, the demo race, before this round): lifted SDK and JSystem 53% of
the game thread's busy time, lifted game code 34%, runtime helpers 10%, native code 2%. By library: J3D 19%,
`dolphin/mtx` 19% (`PSMTXConcat` alone 9.7%, `PSMTXCopy` 2.5%, `PSMTXInverse` 2%), the game's own libraries 31%,
GX 5%. The write-gather pipe's handling on the game thread (`pipe_write`, `send_burst`, the store's slow branch) is
about 3-5%; the dispatcher for indirect calls 1.8%; replaced-function lookup 0.6%.

| Change | Title | Demo race | 4-view race |
|---|---|---|---|
| Registers around a direct call cut down to the callee's (before: `--full-sync`) | 10.63 -> 10.33 ms | 8.08 -> 7.13 | 6.77 -> 6.70 |
| Cheaper floating-point helpers (below) | 9.71 -> 7.27 ms | 6.69 -> 5.39 | 6.50 -> 5.70 |
| Tried and left out: lifted code reading the dispatcher's cache itself, a cache for replaced-function lookups | 7.31 -> 7.42 | 5.37 -> 5.14 | 5.86 -> 5.83 |
| Doubles in the `Cpu` in single mode (below; after merging main, two pairs) | 7.71 -> 7.68, 7.61 -> 7.58 | 5.52 -> 5.42, 5.51 -> 5.18 | 5.92 -> 5.96 |
| Stores of a half not ours take the register as it stands (below; two pairs) | 7.68 -> 7.69, 7.60 -> 7.63 | 5.47 -> 5.61, 5.51 -> 5.32 | 5.77 -> 5.62 |

(The pairs ran at different times, so a row's "after" needn't equal the next row's "before". The third row is within
the noise either way, so the code stayed as it was. The demo race is the least steady figure: it isn't quite the
same race every run.) The movie decoder (`qilin_game_difftest --thp`, above) went from
6.63 to 4.73 ms a frame with MSVC and from 6.0 to 4.01 ms with Clang for MinGW (zig, `x86_64-windows-gnu -O2`, the
player kit's compiler), 0 differences; that Clang build of the game difftest (10 trials a function, 68,375 runs
compared) found 0 differences too.

**The helpers.** A float's double, and `lfs`'s conversion, are the host's conversion, which is exact for every
value (a denormal comes out normalised, as the manual has it); only a NaN takes the bitwise path, since the host
would quiet a signalling one. Single mode's check for an ordinary result is one integer compare on its common path.
`round25`, `store_single`, `madd_single` and `madd` are forced inline, with their rare paths (a subnormal, a tie)
out of line; MSVC had left them as calls. GCC and Clang on x86-64 built for every CPU take the FMA instruction,
where the CPU has it, through an asm statement (a function with a target attribute can't be inlined, and MinGW's
`fma` is software), as MSVC does through its intrinsic. Checked: `load_single`, `to_d` and `ordinary` against the
old definitions on all 2^32 floats, `round25` on 400 million doubles: 0 differences; the unit tests (also built with
zig's Clang, for the asm path), the instruction difftest, `qilin_fp_check` and the game difftest pass. One lifted
function alone (`mbench`), before and after, MSVC / Clang for MinGW: `PSMTXConcat` 136 / 126 ns -> 77 / 67,
`PSMTXCopy` 33 / 35 -> 21 / 21, `PSMTXMultVec` 63 / 69 -> 35 / 33, `PSVECCrossProduct` 44 / 47 -> 20 / 21.

**Four views.** Headless, the four-player race's game thread waits about 9.5 ms a retrace for the GPU thread, which
records 14.7 ms of commands a frame: there the GPU thread is the bottleneck, not the lifted code.

**On a Raspberry Pi 4** (not measurable here): build Release with GCC 14 or Clang 19 on the Pi itself (ARM64 has
FMA, so `QILIN_FUSED_FLOAT` applies and none of the run-time choice above), with the CPU governor at `performance`
and nothing else running, and run the same two scenes headless (`mkdd --headless <disc>`, a fresh save folder;
`MKDD_PILOT_STEPS` and `MKDD_INPUT2..4` as in `bench4.sh` for four views). Read the same lines from the log: the
title's windows 2100-3600 and the race's from 5700, work less the GPU wait. Three runs at least, as the figures move;
`MKDD_PROFILE=<file>` (Linux's SIGPROF sampler, `profile.cpp`) gives the split by function there.

**Doubles in the `Cpu`.** About 8% of the race's busy time was the game's own vector code in double mode. It
uses `frsqrte` (an inlined square root: `checkPosition`, `MakeSusMatrix`, `DoTireAction`, `NeckCtrl::update`,
`PSVECMag`), double constants loaded with `lfd` (`searchBase`, `J3DGetKeyFrameInterpolation`) or `fctiwz`
(`DriverModel::update`, `getTransform`). Single mode now leaves such a value where double mode would have it: in
the `Cpu`'s register. `analyse_single` follows, forwards to a fixed point, which ps0 halves hold such a double on
every path and on some (`SingleInfo::cpu`). `lfd`, `frsqrte`, `fctiw(z)` and the double arithmetic make one, a move
of one (`fmr`, the sign changes, `fsel` choosing between two) is one, a single result ends one, and a call leaves it
where it is. An instruction that reads one, or makes one, is lifted with the double helpers exactly as double mode
lifts it (`mixed_insn`): a float operand is read as its double, and a single result goes to a float (the double's
float, exact). A double never needs fetching, checking or writing back. Every one of those functions is in single
mode now: **3,100** single-mode functions (2,073 before; 672 with resume points). A function stays in double mode if
a register is a float on one path and such a double on another where they meet, or if a paired-single instruction
reads such a double. `PSVECMag` alone (`mbench`): MSVC 28.9 -> 26.9 ns, Clang for MinGW 46.4 -> 32.2 ns.
Checked:
- the unit tests: an estimate, a constant, and a register that is a float on one path and a double on the other;
- the instruction difftest;
- the game difftest: 40 trials, 272,741 runs compared, 0 differences; and 20 trials after the fix below, 0 differences;
- every file of the lift compiled with zig's Clang and `-Wall -Wextra -Wpedantic -Wshadow -Werror`: no warning.

That warning check caught a latent fault, now fixed. An `lfd` with nothing to fetch after it still noted the
"written" flags of a fallback it didn't emit, which left a flag set and never read.

**Stores take a half as the register stands** (for `J3DJoint::recursiveCalc`, 2.2% of the race's busy time). Its
prologue saves `f26`-`f31` with `stfd` and `psq_st`, and after its calls it stores values it loaded before them.
Single mode used to read every half `psq_st` and `stfs` store as a float. That cost 12 checks on entry, with a double
version behind them, and 6 checks after each of its four calls. Now a store of a half that this run hasn't written
on every path stores the register as it stands, as `stfd` always has: our float where we wrote it (a "written" flag
says so where only some paths did), else the `Cpu`'s own value, through double mode's store. That is the same value,
because a half we haven't written is still the `Cpu`'s, so nothing checks it on entry or fetches it again after a
call. `recursiveCalc` has no check and no double version now. Over the whole lift:
- `to_f_exact` checks went from 8,021 to 2,341;
- double versions from 1,194 to 771 (672 to 575 with resume points);
- `mkdd.exe` shrank from 65.0 MB to 58.5 MB.

The scenes' figures moved within their noise (at best 1-3% in the four-view race). I kept the change for the code it
removes. Checked:
- the unit tests: a new case, an argument stored with no check;
- the instruction difftest;
- the game difftest: 40 trials, 271,333 runs compared, 0 differences;
- the whole lift under zig's Clang with the full warning set: no warning, after a fix so that a half taken from the
  `Cpu` declares no float.

**Deferred checks, tried and left out.** I tried checking a run's results once instead of each one. A run is a
stretch of float arithmetic with its loads, and integer work that reads no float and no CR field, ending at a
float's store, a call, a branch or a label. Each result was noted branch-free (`bad |= unusual(r)`). If any wasn't
ordinary, the run's floats went back to their values at its start and the floating point was done again with the
checking helpers: the loads' values were kept, so memory is read once, and the integer work isn't repeated. That is
exact (a helper's fast result is its result whenever it's ordinary), and the game's code shows it would apply
widely: `PSMTXConcat`'s body is one run. But it saved little. `PSMTXConcat` went from 77 to 74 ns with MSVC, and
from 68 to 69 ns with Clang. The time is in computing each check (a move to an integer register and two compares),
not in branching on it, and a deferred check still computes every one. A cheaper exact test of a whole run would
need the host's floating-point status flags; their underflow rule doesn't match Broadway's "below the smallest
normal before rounding", and ARM's differ again. The patch is in `work\core\deferred_checks_tried.patch`.

**What is left** (the demo race): `PSMTXConcat` is still the hottest function (6.8% of busy time), then the pipe's
handling (about 5%, `gpu.cpp`, the GPU role's). In single-mode code the ordinary-result checks are still half the
time of a function like `PSMTXConcat` (with them removed, which would be wrong, it takes 46 ns instead of 77), and
writing the FPR halves back on the way out is another 10-25%.

## Efficiency, round 5 (2026-11-26)

**Why the title costs more than a race.** The attract mode's title (retraces 2100-3600) plays a movie behind the
logo, and the game thread decodes it: sampled from outside (core's build, 18 s of the title), 80% of the game
thread's busy time was THP (`__THPDecompressiMCURowNxN` 65%, the Huffman decoders 14%), and another 17% was
`psq_store_quantized_s`, the out-of-line path of the decoder's `psq_st` of pixels as `u8` (GQR6: type 4, scale
-3; 96 pairs a decoded row), with MSVC leaving `pow2f`, `signed_scale`, `compare_signed`, `std::rotl` and even
`std::atomic::load` (`poll_due`) as calls. The race has no movie. So the title's cost is the movie decoder, and the
measure for it is `qilin_game_difftest --thp` (above), not a run.

**What changed** (all exact; each checked as below):
- **Registers go back only if changed** (`lift.cpp`, `find_dirty`). A local is clean where it was loaded from the
  `Cpu` (on entry, after a call reads the callee's part back) until the function assigns it; a forward dataflow over
  the function's paths (union) gives the possibly dirty set before each instruction, and a write-back before a call
  or a way out stores only those. Which locals an instruction assigns is read from its own emitted line (`r3 = `,
  `cr0 = `, `ctr -= `, `f1 = `), plus the out-parameters the text doesn't show (`add_carry`'s and `sraw`'s CA,
  double mode's `psq_load` halves), so it can't disagree with the code. GPRs, CR fields, LR, CTR, SO/OV/CA and double
  mode's FPR halves; a register that is only read (`r2`, `r13`, a callee's argument) is never stored again. Stores of
  GPRs to the `Cpu` in the lifted code went from 566,246 to 283,258, and the constructors that call the most
  (`Race2D`, `Result2D`, `SceneMenu`: 1.9 MB of x64 between them and their twins) shrank by 40-45%.
- **Guest memory through a local base** (`Mem`, `cpu.hpp`). A guest store is a byte store through `c.mem + a`, which
  for all the compiler knows changes the `Cpu`, so every access after a store loaded `c.mem` again (501 times in the
  THP row function). The lifted function now takes `const Mem m{c.mem, c};` on entry; the helpers take `Mem`
  (`Cpu&` overloads remain for native code).
- **One access for eight bytes**: `lfd`, `stfd` and a `psq_l`/`psq_st` float pair are one 8-byte load or store and
  one swap where no byte is in the slow range (`is_slow_n`); else the two 4-byte accesses as before, out of line. The
  guest reservation is 4 GB and 64 KB (`memory.cpp`), so an access running off the top of the space still faults.
  `lmw`/`stmw` likewise: one check for all the words, plain loads and stores, and the slow way out of line through the
  `Cpu`'s registers (4,166 `stmw` and 4,157 `lmw` in the lift).
- **`psq_st` as `u8`** inline where the GQR isn't GQR0 (`i` is a constant at each site, so GQR0's sites, 5,548 of
  them, get no extra code), both bytes as one store; `psq_l` of `s16` likewise only away from GQR0.
- **The slow branch of a load or store** is one function per size (`read_slow<N>`, `write_slow<N>`): each of the
  sites passes the address (and value) only, and the masking of an 8- or 16-bit value moved inside.
- **Inlining MSVC refused**: the CR helpers (`compare_signed`, `record`, ...), the carries and shifts, `pow2f`,
  `signed_scale`, `store_single_ftz` and the bit casts are forced inline; `poll_due` reads the flag with
  `__iso_volatile_load8` on MSVC (what its `<atomic>` does for a relaxed load); rotates are emitted as shifts where
  the mask allows (`rotated`, `lift.cpp`) and as `rotl32` otherwise, no `std::rotl`.
- **Rare paths that clobber nothing** (`QILIN_RARE`): Clang's `preserve_all` on the floating-point helpers' rare
  paths (`add_s_rare` and the rest, `round25_subnormal`, `madd_single_tie`, `load_single_nan`). With the usual
  convention each of the hundreds of checks in a single-mode function is a call that clobbers every vector register
  (all of them on x86-64 SysV; all but d8-d15's low halves on ARM64), so floats were kept on the stack around them.
  Clang 17 and later on x86-64 and ARM64, not Apple's Clang (unchecked); GCC and MSVC have no such convention.
- **Twins cold and small** (`QILIN_TWIN`): the double versions are `[[gnu::cold]]` (GCC builds them for size and puts
  them out of the hot code's way) and, under Clang, `minsize`. Neither profile (the race, the title) has a single
  sample in a twin, and they were a quarter of the lifted code (12.9 MB of 51 MB in `mkdd.exe`).
- **ARM64 forms**: `ordinary()` tests the exponent field (`ubfx`, small immediates) instead of the x86 form's two
  32-bit constants, which ARM64 rebuilt with `mov`/`movk` at every check (780 `movk` in the THP row function); it sends
  exponent 1 (below 2^-125) the careful way too. `is_slow_n` tests the last byte only (two small immediates instead of
  a constant pair); an access from the very end of the slow range into the locked cache then faults on unbacked memory
  where the hooks would have stopped the runtime.
- **`to_f_exact`** (single mode's entry checks and fetches after calls, 2,341 sites) is the host's conversion and a
  compare where the value is within a single's range; NaNs, infinities and anything larger go the manual's bitwise
  way, out of line.

**Checked:**
- exhaustively: ARM64's `ordinary` against the x86 form on all 2^32 floats (it never accepts what the x86 form
  rejects; it rejects 16,777,214 more, exponent 1); both `is_slow_n` forms for 8 bytes against the definition on all
  2^32 addresses (x86 exact; ARM64 differs only at `DFFFFFF9`-`DFFFFFFF`); `to_f_exact` against the bitwise form on
  17.2 billion doubles (every single's double, its neighbours either side, 2^32 random ones): 0 differences;
- the unit tests (`qilin_tests`, the sync texts' expectations updated: r3 and r4 aren't stored around the leaf call
  any more), the instruction difftest (5,061 cases, 323,904 runs, 0 failing), `qilin_fp_check`;
- the game difftest, 40 trials, Clang for MinGW (`work/cpu2/clangbuild.py`): 271,786 runs compared in 9,833
  functions, 0 functions differ (after the first three changes, after the dirty registers, and on the final state);
  the THP frames: 0 differences;
- zig's Clang for `x86_64-linux-gnu` and `aarch64-linux-gnu`, `-Wall -Wextra -Wpedantic -Wshadow -Werror`: the changed
  sources and lifted files (`lifted_023.cpp`, the THP functions); MSVC `/W4 /permissive-` on `lifted_012.cpp`: no warning.

**Measured** (Ryzen 7 5800X; Clang is zig's for `x86_64-windows-gnu -O2`, the player kit's compiler):

| | Before | After |
|---|---|---|
| THP decode, Clang (`--thp play1.thp --frames 3 --reps 30`) | 3.99 ms a frame | **3.38 ms** (-15%) |
| `qilin_game_difftest.exe`, Clang (nearly all lifted code) | 58.8 MB | **44.5 MB** (-24%) |
| THP row function (`800F4E50`), x86-64 / ARM64 (Linux targets) | 367 / 215 KB | **303 / 169 KB** |
| `PSMTXConcat`, x86-64 / ARM64 | 22.9 / 9.9 KB | **13.6 / 6.1 KB** |
| `Race2D` constructor and its twin, x86-64 / ARM64 | 490 / 346 KB | **287 / 181 KB** |
| `lifted_023.cpp` (468 functions), x86-64 / ARM64 | 2,295 / 1,611 KB | **1,575 / 1,045 KB** (-31% / -35%) |

## Cheap wins noted, not taken
- **A function's FPR halves written back on the way out only where a caller can read them: not taken, because it
  can't be made provably safe here.** It would save 10-25% of a small single-mode leaf (`PSMTXCopy` 21 -> 16 ns,
  `PSMTXMultVec` 37 -> 32 in `mbench`, with every write-back removed). Why not:
  - *Callers aren't all known.* 5,676 functions are reached through pointers in data and 592 through addresses built
    in code; any function the dispatcher can reach (every one is in its table) may be called through a pointer the
    analysis can't see: one computed at run time, or held by native code (`mkdd/src/build.hpp`'s addresses, the
    wrappers, callbacks the runtime calls). For those every half has to go back.
  - *Liveness after a return is interprocedural and transitive.* A double-mode caller reloads, after a call, every
    FPR of the callee's set that it uses and writes all of them back at its own exit, so a stale half would reach the
    caller's caller as a register value. A half is dead only if it is dead in every caller, and in their callers
    wherever it passes through unchanged. That needs a fixed point over the whole call graph with summaries of
    what passes through each function, and any function with an unknown caller (above) stops it.
  - *The EABI says f0-f13 are dead after a call (but f1 and the paired f1-f4 returns).* Taking that as given would
    be an assumption about every routine in the game, the SDK's hand-written assembly included. A single routine
    that breaks it would give wrong results with no fatal error, so the rule against unproven assumptions keeps it out.
  - *The game difftest couldn't check it.* It compares every register after each function. With halves left out on
    purpose it would have to skip them, so it couldn't catch an analysis that got one wrong.
- `ps1` of an FPR is carried through functions that only use scalar singles; a per-function "ps1 never read" would drop half the FP write-backs there.
