#!/usr/bin/env python3
"""Cross-check qilin's Gekko decoder against GNU objdump, word by word.
(The GameCube's Gekko has the Wii's Broadway instruction set, paired singles
included, so objdump's `broadway` set is the reference for both.)

Two inputs:
  --dol main.dol   every word of the game's text sections (needs your own copy)
  --sweep          a structured sweep of the encoding space (no game files; runs in CI):
                   random words for every primary and extended opcode, then every
                   single-bit neighbour of each word either decoder accepts, so the
                   edges of each instruction's reserved bits are tested too.

objdump runs as `-M broadway,raw`: the Broadway instruction set, no simplified
mnemonics. Use binutils-powerpc-linux-gnu (Debian/Ubuntu) or devkitPPC's
powerpc-eabi-objdump; set OBJDUMP to choose one.

Where there is no PowerPC objdump, `--dol main.dol --assembler "zig"` (or
"python -m ziglang", or a Clang) checks the other way round with LLVM's
assembler, which is as independent of qilin as binutils is: every distinct word
qilin decodes is written out as qilin's text, assembled, and must come back as
the same word. That checks the mnemonic and every operand field of each valid
word; it can't check that an invalid word is invalid, and LLVM doesn't know
Gekko's paired singles, which are counted and left out.

The comparison is strict: mnemonic and every operand must match, and a word
must be valid for both or neither. Disagreements that are reviewed and
explained are counted by class (KNOWN below) instead of failing; anything else
fails the check. Exit status 0 only if nothing unexplained remains.
"""
import argparse
import collections
import os
import random
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

BASE = 0x80004000

# Reviewed differences between qilin and binutils 2.42's `broadway` set.
# Each rule is as narrow as the case it explains, so anything else still
# fails. In all but the first, binutils accepts an encoding that the IBM
# 750CL user's manual reserves or calls an invalid form, which no retail
# compiler emits: qilin rejects it, so the recompiler fails loudly on it.
def known_difference(word, ours, theirs):
    p = word >> 26
    xo10 = (word >> 1) & 0x3FF
    xo6 = (word >> 1) & 0x3F
    xo5 = (word >> 1) & 0x1F
    rd, ra = (word >> 21) & 31, (word >> 16) & 31
    ours_valid = not ours.startswith(".long")
    theirs_valid = not theirs.startswith(".long")
    if ours_valid and not theirs_valid:
        # binutils' broadway/750cl sets leave mftb out, but the 750CL manual
        # lists it and the SDK's OSGetTime reads the time base with it.
        if p == 31 and xo10 == 371:
            return "mftb: in the 750CL; missing from binutils' broadway set"
        # An invalid form by the manual, so binutils rejects it; the 750 runs it
        # and MetroTRK's context restore (`lmw r0,0(r3)`) is in the game's text.
        # qilin decodes it and the lifter refuses it (decoder.hpp, lmw_loads_base).
        if p == 46 and ra >= rd:
            return "lmw with rA among the registers it loads (an invalid form the 750 runs; MetroTRK uses it)"
        return None
    if ours_valid and theirs_valid:
        if p == 31 and xo10 == 19 and theirs == ours + ",-1":
            return "mfcr: objdump's raw mode prints the absent FXM field as -1"
        return None
    if ours_valid or not theirs_valid:
        return None
    if p == 31 and xo10 in (19, 144) and (word >> 20) & 1:
        return "mfcr/mtcrf with bit 11 set (later ISAs' mfocrf/mtocrf; reserved on the 750CL)"
    if p == 31 and xo10 in (978, 1010):
        return "tlbld/tlbli: the 603e's software TLB reload; not on the 750CL"
    if p == 31 and xo10 == 370:
        return "tlbia: optional; the 750CL doesn't implement it"
    if p == 31 and xo10 == 146 and (word >> 16) & 1:
        return "mtmsr with bit 15 set (later ISAs' L field; reserved on the 750CL)"
    if p == 31 and xo10 == 306 and (word >> 21) & 1:
        return "tlbie with bit 10 set (later ISAs' L field; reserved on the 750CL)"
    if p == 31 and xo10 == 598 and (word >> 21) & 3:
        return "sync with bits 9-10 set (later ISAs' L field, lwsync; reserved on the 750CL)"
    if (p in (10, 11) or (p == 31 and xo10 in (0, 32))) and (word >> 21) & 1:
        return "cmp/cmpi/cmpl/cmpli with L=1: a 64-bit compare, invalid on a 32-bit CPU"
    if (p in (10, 11) or (p == 31 and xo10 in (0, 32))) and (word >> 22) & 1:
        return "cmp/cmpi/cmpl/cmpli with bit 9 set (reserved)"
    if p == 19 and xo10 in (16, 528) and (word >> 11) & 3:
        return "bclr/bcctr with bits 19-20 set (later ISAs' BH hint; reserved on the 750CL)"
    if p == 19 and xo10 == 528 and not rd & 4:
        return "bcctr with a BO that decrements CTR (an invalid form)"
    if p == 31 and xo10 == 86 and (word >> 21) & 3:
        return "dcbf with bits 9-10 set (later ISAs' L field; reserved on the 750CL)"
    if p == 17 and word & 2 and theirs.startswith("sc"):
        return "sc with reserved bits set (binutils ignores most of them; prints later ISAs' LEV field)"
    if p == 31 and xo10 in (246, 278) and rd != 0:
        return "dcbt/dcbtst with bits 6-10 set (later ISAs' TH hint; reserved on the 750CL)"
    if p == 31 and xo10 == 20 and word & 1:
        return "lwarx with bit 31 set (later ISAs' EH hint; reserved on the 750CL)"
    if p == 63 and xo10 == 711 and ((word >> 25) & 1 or (word >> 16) & 1):
        return "mtfsf with bit 6 or 15 set (later ISAs' L and W fields; reserved on the 750CL)"
    if p in (59, 63) and xo5 == 22:
        return "fsqrt/fsqrts: optional instructions the 750CL doesn't implement"
    if ((p == 59 and xo5 == 24) or (p == 63 and xo5 == 26)) and ra == 1:
        return "fres/frsqrte with bit 15 set (reserved on the 750CL)"
    if p == 31 and xo10 == 597:
        regs = ((((word >> 11) & 31) or 32) + 3) // 4
        if (ra - rd) % 32 < regs:
            return "lswi with rA among the registers it loads (an invalid form)"
    if (p in (57, 61) or (p == 4 and xo6 in (38, 39))) and ra == 0:
        return "paired-single update form with rA=0 (an invalid form)"
    if p == 4 and xo6 in (6, 7, 38, 39) and word & 1:
        return "paired-single indexed load/store with bit 31 set (reserved)"
    return None


def find_objdump():
    for name in [os.environ.get("OBJDUMP"), "powerpc-linux-gnu-objdump", "powerpc-eabi-objdump",
                 "powerpc64-linux-gnu-objdump"]:
        if name and shutil.which(name):
            return shutil.which(name)
    sys.exit("verify_decoder: no PowerPC objdump found (apt install binutils-powerpc-linux-gnu, "
             "or set OBJDUMP to devkitPPC's powerpc-eabi-objdump)")


def normalise(text):
    text = text.strip()
    parts = text.split(None, 1)
    if not parts:
        return ""
    return parts[0] if len(parts) == 1 else parts[0] + " " + parts[1].replace(" ", "")


def run_objdump(objdump, words, base):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(b"".join(struct.pack(">I", w) for w in words))
        path = f.name
    try:
        out = subprocess.run([objdump, "-D", "-z", "-b", "binary", "-m", "powerpc:750", "-EB",
                              "-M", "broadway,raw", f"--adjust-vma={base:#x}", path],
                             check=True, capture_output=True, text=True).stdout
    finally:
        os.unlink(path)
    result = {}
    for line in out.splitlines():
        cols = line.split("\t")
        if len(cols) < 3 or not cols[0].strip().endswith(":"):
            continue
        result[int(cols[0].strip()[:-1], 16)] = normalise(cols[2])
    return [result[base + 4 * i] for i in range(len(words))]


def run_qilin(qilin, words, base):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(b"".join(struct.pack(">I", w) for w in words))
        path = f.name
    try:
        out = subprocess.run([qilin, "disasm", path, "--base", hex(base)], check=True, capture_output=True,
                             text=True).stdout
    finally:
        os.unlink(path)
    lines = out.splitlines()
    assert len(lines) == len(words)
    return [normalise(line.split(" ", 2)[2]) for line in lines]


def compare(words, ours, theirs, base, report):
    unexplained = []
    for i, w in enumerate(words):
        if ours[i] == theirs[i]:
            report["agree_valid" if not ours[i].startswith(".long") else "agree_invalid"] += 1
            continue
        why = known_difference(w, ours[i], theirs[i])
        if why:
            report["known:" + why] += 1
        else:
            unexplained.append((base + 4 * i, w, ours[i], theirs[i]))
    return unexplained


# ---- the round trip through LLVM's assembler (--assembler)
REGISTER = re.compile(r"\b(?:cr|r|f)(\d+)\b")
BRANCHES = {"b", "bl", "bc", "bcl"}  # relative: qilin prints the target, the assembler wants the distance


def llvm_text(addr, text):
    """qilin's text for the word at addr, in the assembler's syntax; None for what LLVM has no instruction for."""
    mnemonic, _, operands = text.partition(" ")
    if mnemonic.startswith(("ps_", "psq_")) or mnemonic == "dcbz_l":
        return None
    ops = operands.split(",") if operands else []
    if mnemonic in BRANCHES:
        distance = int(ops[-1], 16) - addr
        if distance >= 1 << 31:
            distance -= 1 << 32
        if distance < -(1 << 31):
            distance += 1 << 32
        ops[-1] = f".{distance:+d}"
    if mnemonic.rstrip(".") in ("fres", "frsqrte") and ops[-1] == "0":
        ops.pop()  # objdump's raw mode prints a later ISA's field there, which the 750 has as zero
    ops = [REGISTER.sub(r"\1", o) for o in ops]
    return f"{mnemonic} {','.join(ops)}"


def assemble(assembler, lines):
    """The words the assembler makes of `lines` (one instruction each), and the lines it refused."""
    refused = {}
    with tempfile.TemporaryDirectory() as tmp:
        src, obj, raw = (os.path.join(tmp, n) for n in ("words.s", "words.o", "words.bin"))
        while True:
            kept = [i for i in range(len(lines)) if i not in refused]
            with open(src, "w") as f:
                f.write(".text\n" + "".join(lines[i] + "\n" for i in kept))
            r = subprocess.run(assembler + ["cc", "-target", "powerpc-linux-gnu", "-mcpu=750", "-c", src, "-o", obj],
                               capture_output=True, text=True)
            if r.returncode == 0:
                break
            bad = {}
            for m in re.finditer(r"words\.s:(\d+):\d+: error: (.*)", r.stderr):
                bad.setdefault(kept[int(m.group(1)) - 2], m.group(2))
            if not bad:
                sys.exit("verify_decoder: the assembler failed:\n" + r.stderr[:2000])
            refused.update(bad)
        subprocess.run(assembler + ["objcopy", "-O", "binary", "--only-section=.text", obj, raw], check=True)
        data = open(raw, "rb").read()
    if len(data) != 4 * len(kept):
        sys.exit(f"verify_decoder: the assembler made {len(data)} bytes of {len(kept)} instructions")
    return {i: struct.unpack_from(">I", data, 4 * k)[0] for k, i in enumerate(kept)}, refused


def roundtrip(assembler, items):
    """items: (address, word, qilin's text). Returns the exit status."""
    distinct, invalid, unknown = {}, 0, collections.Counter()
    for addr, word, text in items:
        if text.startswith(".long"):
            invalid += 1
        elif word not in distinct:
            line = llvm_text(addr, text)
            if line is None:
                unknown[text.split()[0]] += 1
            distinct[word] = (addr, text, line)
    todo = [(w, a, t, l) for w, (a, t, l) in distinct.items() if l is not None]
    words, refused = assemble(assembler, [l for _, _, _, l in todo])
    # LLVM assembles mftb as the mfspr of the same number (what later ISAs made of it): one bit
    # of the extended opcode apart, with every operand field the same.
    mftb = [i for i, (w, _, t, _) in enumerate(todo) if t.startswith("mftb ") and words.get(i) == w ^ 0x40]
    for i in mftb:
        words[i] = todo[i][0]
    if mftb:
        print(f"  known difference ({len(mftb)}): mftb comes back as mfspr (extended opcode 339 for 371), operands the same")
    wrong = [(a, w, t, f"{words[i]:08x}") for i, (w, a, t, _) in enumerate(todo) if i in words and words[i] != w]
    wrong += [(a, w, t, "refused: " + refused[i]) for i, (w, a, t, _) in enumerate(todo) if i in refused]
    print(f"{len(items)} words: {len(items) - invalid} valid for qilin ({len(distinct)} distinct), {invalid} invalid (not checked)")
    print(f"  {len(todo) - len(wrong)} distinct words assembled back to themselves from qilin's text")
    print(f"  {sum(unknown.values())} distinct words left out: LLVM has no paired singles "
          f"({', '.join(sorted(unknown))})" if unknown else "  nothing left out")
    if wrong:
        print(f"{len(wrong)} DISAGREEMENTS (address word | qilin | assembler):")
        classes = collections.defaultdict(list)
        for t in wrong:
            classes[t[2].split()[0]].append(t)
        for name, group in sorted(classes.items(), key=lambda kv: -len(kv[1])):
            print(f"  {len(group):7} {name}")
            for addr, w, x, y in group[:3]:
                print(f"            {addr:08x} {w:08x} | {x} | {y}")
        return 1
    print("no disagreements")
    return 0


# Random bits within `allowed`, with each 5-bit register field (and the
# record bit) left zero half the time, so that the valid forms of
# instructions with reserved fields come up often.
FIELDS = (0x03E00000, 0x001F0000, 0x0000F800, 0x000007C0, 0x0000003E, 0x00000001)


def fields(rng, allowed):
    w = 0
    for f in FIELDS:
        if rng.random() < 0.5:
            w |= rng.getrandbits(32) & f
    return w & allowed


def sweep_words(rng, per_xo, per_primary):
    words = []
    for p in range(64):
        if p in (4, 19, 31, 59, 63):
            for xo in range(1024):
                for _ in range(per_xo):
                    words.append((p << 26) | (xo << 1) | fields(rng, 0x03FFF801))
        else:
            for _ in range(per_primary):
                words.append((p << 26) | fields(rng, 0x03FFFFFF))
    return words


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qilin", required=True, help="path to the qilin executable")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--dol", help="main.dol: check every word of its text sections")
    src.add_argument("--sweep", action="store_true", help="sweep the encoding space")
    ap.add_argument("--seed", type=int, default=750)
    ap.add_argument("--per-xo", type=int, default=24, help="sweep: random words per extended opcode")
    ap.add_argument("--per-primary", type=int, default=4096, help="sweep: random words per primary opcode")
    ap.add_argument("--show", type=int, default=3, help="examples to print per class of unexplained disagreement")
    ap.add_argument("--assembler", help='with --dol: round-trip through LLVM\'s assembler instead of objdump; the command '
                    'that takes "cc" and "objcopy" (zig, or "python -m ziglang")')
    args = ap.parse_args()
    if args.assembler:
        if not args.dol:
            sys.exit("verify_decoder: --assembler goes with --dol")
        out = subprocess.run([os.path.abspath(args.qilin), "insns", args.dol], check=True, capture_output=True, text=True).stdout
        items = []
        for line in out.splitlines():
            addr_s, word_s, text = line.split(" ", 2)
            items.append((int(addr_s, 16), int(word_s, 16), normalise(text)))
        return roundtrip(shlex.split(args.assembler), items)
    objdump = find_objdump()
    report = collections.Counter()
    unexplained = []

    if args.dol:
        out = subprocess.run([args.qilin, "insns", args.dol], check=True, capture_output=True, text=True).stdout
        by_section = collections.defaultdict(list)  # contiguous runs of words
        prev = None
        for line in out.splitlines():
            addr_s, word_s, text = line.split(" ", 2)
            addr = int(addr_s, 16)
            if prev is None or addr != prev + 4:
                start = addr
            by_section[start].append((int(word_s, 16), normalise(text)))
            prev = addr
        for start, items in by_section.items():
            words = [w for w, _ in items]
            theirs = run_objdump(objdump, words, start)
            unexplained += compare(words, [t for _, t in items], theirs, start, report)
    else:
        rng = random.Random(args.seed)
        words = sweep_words(rng, args.per_xo, args.per_primary)
        ours = run_qilin(args.qilin, words, BASE)
        theirs = run_objdump(objdump, words, BASE)
        unexplained += compare(words, ours, theirs, BASE, report)
        # Every single-bit neighbour of every word either side accepts.
        seeds = sorted({w for w, a, b in zip(words, ours, theirs)
                        if not a.startswith(".long") or not b.startswith(".long")})
        neighbours = sorted({w ^ (1 << bit) for w in seeds for bit in range(32)} - set(words))
        ours = run_qilin(args.qilin, neighbours, BASE)
        theirs = run_objdump(objdump, neighbours, BASE)
        unexplained += compare(neighbours, ours, theirs, BASE, report)
        words += neighbours

    total = sum(report.values()) + len(unexplained)
    print(f"{total} words: {report['agree_valid']} valid and identical, "
          f"{report['agree_invalid']} invalid for both")
    for key, n in sorted(report.items()):
        if key.startswith("known:"):
            print(f"  known difference ({n}): {key[6:]}")
    if unexplained:
        print(f"{len(unexplained)} UNEXPLAINED disagreements (address word | qilin | objdump):")
        classes = collections.defaultdict(list)
        for t in unexplained:
            classes[(t[2].split()[0], t[3].split()[0])].append(t)
        for (a, b), items in sorted(classes.items(), key=lambda kv: -len(kv[1])):
            print(f"  {len(items):7} qilin {a!r:14} objdump {b!r}")
            for addr, w, x, y in items[:args.show]:
                print(f"            {addr:08x} {w:08x} | {x} | {y}")
        return 1
    print("no unexplained disagreements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
