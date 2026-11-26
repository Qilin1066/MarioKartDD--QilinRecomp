#!/usr/bin/env python3
"""Disassemble GameCube DSP microcode.

  dspdis.py <file> [--dol ADDRESS SIZE | --words] [--base WORD] [--stats] [-o out.txt]

<file> is a raw big-endian image of 16-bit words, or with --dol a DOL from
which SIZE bytes at virtual ADDRESS are taken (the microcode a game hands to
DSPAddTask is data in its DOL; for Mario Kart: Double Dash!! (GM4E01) it is
0x1D20 bytes at 0x80343680). Game data: write the listing outside the repository.

Each line: word address, the instruction's words, the mnemonic. Targets of
jumps and calls get labels; --stats adds what a lifter wants to know: how
often each instruction occurs, the hardware registers touched, and every
use of the DSP's ROMs (calls and jumps to 0x8000-0x8FFF, addresses
0x1000-0x17FF loaded as immediates).

The encodings are those of the GameCube DSP as publicly documented (the
"GameCube DSP User's Manual" kept with Dolphin); the table below is the
decoder's only knowledge, as bit patterns with a letter per operand field.
"""
import argparse
import struct
import sys

REG = ["ar0", "ar1", "ar2", "ar3", "ix0", "ix1", "ix2", "ix3", "wr0", "wr1", "wr2", "wr3",
       "st0", "st1", "st2", "st3", "ac0.h", "ac1.h", "config", "sr", "prod.l", "prod.m1",
       "prod.h", "prod.m2", "ax0.l", "ax1.l", "ax0.h", "ax1.h", "ac0.l", "ac1.l", "ac0.m", "ac1.m"]
CC = ["ge", "l", "g", "le", "nz", "z", "nc", "c", "x8", "x9", "xa", "xb", "lnz", "lz", "o", ""]
IFX = {0xFFC9: "DSCR", 0xFFCB: "DSBL", 0xFFCD: "DSPA", 0xFFCE: "DSMAH", 0xFFCF: "DSMAL",
       0xFFD1: "FORMAT", 0xFFD2: "ACUNK1", 0xFFD3: "ACDRAW", 0xFFD4: "ACSAH", 0xFFD5: "ACSAL",
       0xFFD6: "ACEAH", 0xFFD7: "ACEAL", 0xFFD8: "ACCAH", 0xFFD9: "ACCAL", 0xFFDA: "PRED_SCALE",
       0xFFDB: "YN1", 0xFFDC: "YN2", 0xFFDD: "ACDSAMP", 0xFFDE: "GAIN", 0xFFDF: "ACIN",
       0xFFEF: "AMDM", 0xFFFB: "DIRQ", 0xFFFC: "DMBH", 0xFFFD: "DMBL", 0xFFFE: "CMBH", 0xFFFF: "CMBL"}
for _i in range(16):
    IFX[0xFFA0 + _i] = "COEF_A%d_%d" % (1 + (_i & 1), _i >> 1)

# Pattern (x: any), mnemonic, operand template. In templates {X} is field X:
# r plain register, a accumulator, m acX.m, h axX.h, l axX.l, x axX, 8 reg 0x18+X,
# c reg 0x1C+X, i/A immediates, M a short memory address, cc a condition.
MAIN = [
    ("0000000000000000", "nop", ""),
    ("00000000000001dd", "dar", "$ar{d}"),
    ("00000000000010dd", "iar", "$ar{d}"),
    ("00000000000011dd", "subarn", "$ar{d}"),
    ("000000000001ssdd", "addarn", "$ar{d}, $ix{s}"),
    ("0000000000100001", "halt", ""),
    ("00000000010rrrrr", "loop", "{r:r}"),
    ("00000000011rrrrr aaaaaaaaaaaaaaaa", "bloop", "{r:r}, {a:A}"),
    ("00000000100ddddd iiiiiiiiiiiiiiii", "lri", "{d:r}, #{i:i}"),
    ("00000000110ddddd mmmmmmmmmmmmmmmm", "lr", "{d:r}, @{m:D}"),
    ("00000000111sssss mmmmmmmmmmmmmmmm", "sr", "@{m:D}, {s:r}"),
    ("000000100111cccc", "if{c:cc}", ""),
    ("000000101001cccc aaaaaaaaaaaaaaaa", "j{c:jcc}", "{a:A}"),
    ("000000101011cccc aaaaaaaaaaaaaaaa", "call{c:cc}", "{a:A}"),
    ("000000101101cccc", "ret{c:cc}", ""),
    ("000000101111cccc", "rti{c:cc}", ""),
    ("0000001d00000000 iiiiiiiiiiiiiiii", "addi", "$ac{d}, #{i:i}"),
    ("0000001d00100000 iiiiiiiiiiiiiiii", "xori", "$ac{d}.m, #{i:i}"),
    ("0000001d01000000 iiiiiiiiiiiiiiii", "andi", "$ac{d}.m, #{i:i}"),
    ("0000001d01100000 iiiiiiiiiiiiiiii", "ori", "$ac{d}.m, #{i:i}"),
    ("0000001d10000000 iiiiiiiiiiiiiiii", "cmpi", "$ac{d}, #{i:i}"),
    ("0000001d10100000 iiiiiiiiiiiiiiii", "andf", "$ac{d}.m, #{i:i}"),
    ("0000001d11000000 iiiiiiiiiiiiiiii", "andcf", "$ac{d}.m, #{i:i}"),
    ("0000001d000100ss", "ilrr", "$ac{d}.m, @$ar{s}"),
    ("0000001d000101ss", "ilrrd", "$ac{d}.m, @$ar{s}"),
    ("0000001d000110ss", "ilrri", "$ac{d}.m, @$ar{s}"),
    ("0000001d000111ss", "ilrrn", "$ac{d}.m, @$ar{s}"),
    ("0000001011001010", "lsrn", ""),
    ("0000001011001011", "asrn", ""),
    ("0000010diiiiiiii", "addis", "$ac{d}, #{i:s8}"),
    ("0000011diiiiiiii", "cmpis", "$ac{d}, #{i:s8}"),
    ("00001dddiiiiiiii", "lris", "{d:8}, #{i:s8}"),
    ("00010000iiiiiiii", "loopi", "#{i:i}"),
    ("00010001iiiiiiii aaaaaaaaaaaaaaaa", "bloopi", "#{i:i}, {a:A}"),
    ("0001001000000iii", "sbclr", "#{i:i}"),
    ("0001001100000iii", "sbset", "#{i:i}"),
    ("0001010r00iiiiii", "lsl", "$ac{r}, #{i:d}"),
    ("0001010r01iiiiii", "lsr", "$ac{r}, #{i:n6}"),
    ("0001010r10iiiiii", "asl", "$ac{r}, #{i:d}"),
    ("0001010r11iiiiii", "asr", "$ac{r}, #{i:n6}"),
    ("00010110mmmmmmmm iiiiiiiiiiiiiiii", "si", "@{m:M}, #{i:i}"),
    ("00010111rrr0cccc", "jr{c:cc}", "{r:r}"),
    ("00010111rrr1cccc", "callr{c:cc}", "{r:r}"),
    ("000110000ssddddd", "lrr", "{d:r}, @$ar{s}"),
    ("000110001ssddddd", "lrrd", "{d:r}, @$ar{s}"),
    ("000110010ssddddd", "lrri", "{d:r}, @$ar{s}"),
    ("000110011ssddddd", "lrrn", "{d:r}, @$ar{s}"),
    ("000110100ddsssss", "srr", "@$ar{d}, {s:r}"),
    ("000110101ddsssss", "srrd", "@$ar{d}, {s:r}"),
    ("000110110ddsssss", "srri", "@$ar{d}, {s:r}"),
    ("000110111ddsssss", "srrn", "@$ar{d}, {s:r}"),
    ("000111dddddsssss", "mrr", "{d:r}, {s:r}"),
    ("00100dddmmmmmmmm", "lrs", "{d:8}, @{m:M}"),
    ("0010100smmmmmmmm", "srsh", "@{m:M}, $ac{s}.h"),
    ("001011ssmmmmmmmm", "srs", "@{m:M}, {s:c}"),
    # 0x3000-0x3FFF: the extension is 7 bits.
    ("001100sd0xxxxxxx", "xorr", "$ac{d}.m, $ax{s}.h"),
    ("001101sd0xxxxxxx", "andr", "$ac{d}.m, $ax{s}.h"),
    ("001110sd0xxxxxxx", "orr", "$ac{d}.m, $ax{s}.h"),
    ("0011110d0xxxxxxx", "andc", "$ac{d}.m, $ac{d:o}.m"),
    ("0011111d0xxxxxxx", "orc", "$ac{d}.m, $ac{d:o}.m"),
    ("0011000d1xxxxxxx", "xorc", "$ac{d}.m, $ac{d:o}.m"),
    ("0011001d1xxxxxxx", "not", "$ac{d}.m"),
    ("001101sd1xxxxxxx", "lsrnrx", "$ac{d}, $ax{s}.h"),
    ("001110sd1xxxxxxx", "asrnrx", "$ac{d}, $ax{s}.h"),
    ("0011110d1xxxxxxx", "lsrnr", "$ac{d}"),
    ("0011111d1xxxxxxx", "asrnr", "$ac{d}"),
    ("01000ssdxxxxxxxx", "addr", "$ac{d}, {s:8}"),
    ("010010sdxxxxxxxx", "addax", "$ac{d}, $ax{s}"),
    ("0100110dxxxxxxxx", "add", "$ac{d}, $ac{d:o}"),
    ("0100111dxxxxxxxx", "addp", "$ac{d}"),
    ("01010ssdxxxxxxxx", "subr", "$ac{d}, {s:8}"),
    ("010110sdxxxxxxxx", "subax", "$ac{d}, $ax{s}"),
    ("0101110dxxxxxxxx", "sub", "$ac{d}, $ac{d:o}"),
    ("0101111dxxxxxxxx", "subp", "$ac{d}"),
    ("01100ssdxxxxxxxx", "movr", "$ac{d}, {s:8}"),
    ("011010sdxxxxxxxx", "movax", "$ac{d}, $ax{s}"),
    ("0110110dxxxxxxxx", "mov", "$ac{d}, $ac{d:o}"),
    ("0110111dxxxxxxxx", "movp", "$ac{d}"),
    ("011100sdxxxxxxxx", "addaxl", "$ac{d}, $ax{s}.l"),
    ("0111010dxxxxxxxx", "incm", "$acs{d}"),
    ("0111011dxxxxxxxx", "inc", "$ac{d}"),
    ("0111100dxxxxxxxx", "decm", "$acs{d}"),
    ("0111101dxxxxxxxx", "dec", "$ac{d}"),
    ("0111110dxxxxxxxx", "neg", "$ac{d}"),
    ("0111111dxxxxxxxx", "movnp", "$ac{d}"),
    ("1000r000xxxxxxxx", "nx", ""),
    ("1000r001xxxxxxxx", "clr", "$ac{r}"),
    ("10000010xxxxxxxx", "cmp", ""),
    ("10000011xxxxxxxx", "mulaxh", ""),
    ("10000100xxxxxxxx", "clrp", ""),
    ("10000101xxxxxxxx", "tstprod", ""),
    ("1000011rxxxxxxxx", "tstaxh", "$ax{r}.h"),
    ("10001010xxxxxxxx", "m2", ""),
    ("10001011xxxxxxxx", "m0", ""),
    ("10001100xxxxxxxx", "clr15", ""),
    ("10001101xxxxxxxx", "set15", ""),
    ("10001110xxxxxxxx", "set16", ""),
    ("10001111xxxxxxxx", "set40", ""),
    ("1001s000xxxxxxxx", "mul", "$ax{s}.l, $ax{s}.h"),
    ("1001r001xxxxxxxx", "asr16", "$ac{r}"),
    ("1001s01rxxxxxxxx", "mulmvz", "$ax{s}.l, $ax{s}.h, $ac{r}"),
    ("1001s10rxxxxxxxx", "mulac", "$ax{s}.l, $ax{s}.h, $ac{r}"),
    ("1001s11rxxxxxxxx", "mulmv", "$ax{s}.l, $ax{s}.h, $ac{r}"),
    ("101st000xxxxxxxx", "mulx", "$ax0.{s:lh}, $ax1.{t:lh}"),
    ("1010d001xxxxxxxx", "abs", "$ac{d}"),
    ("1011r001xxxxxxxx", "tst", "$ac{r}"),
    ("101st01rxxxxxxxx", "mulxmvz", "$ax0.{s:lh}, $ax1.{t:lh}, $ac{r}"),
    ("101st10rxxxxxxxx", "mulxac", "$ax0.{s:lh}, $ax1.{t:lh}, $ac{r}"),
    ("101st11rxxxxxxxx", "mulxmv", "$ax0.{s:lh}, $ax1.{t:lh}, $ac{r}"),
    ("110st000xxxxxxxx", "mulc", "$ac{s}.m, $ax{t}.h"),
    ("110rs001xxxxxxxx", "cmpaxh", "$ac{s}, $ax{r}.h"),
    ("110st01rxxxxxxxx", "mulcmvz", "$ac{s}.m, $ax{t}.h, $ac{r}"),
    ("110st10rxxxxxxxx", "mulcac", "$ac{s}.m, $ax{t}.h, $ac{r}"),
    ("110st11rxxxxxxxx", "mulcmv", "$ac{s}.m, $ax{t}.h, $ac{r}"),
    ("111000stxxxxxxxx", "maddx", "$ax0.{s:lh}, $ax1.{t:lh}"),
    ("111001stxxxxxxxx", "msubx", "$ax0.{s:lh}, $ax1.{t:lh}"),
    ("111010stxxxxxxxx", "maddc", "$ac{s}.m, $ax{t}.h"),
    ("111011stxxxxxxxx", "msubc", "$ac{s}.m, $ax{t}.h"),
    ("1111000rxxxxxxxx", "lsl16", "$ac{r}"),
    ("1111001sxxxxxxxx", "madd", "$ax{s}.l, $ax{s}.h"),
    ("1111010rxxxxxxxx", "lsr16", "$ac{r}"),
    ("1111011sxxxxxxxx", "msub", "$ax{s}.l, $ax{s}.h"),
    ("111110sdxxxxxxxx", "addpaxz", "$ac{d}, $ax{s}"),
    ("1111110rxxxxxxxx", "clrl", "$ac{r}.l"),
    ("1111111dxxxxxxxx", "movpz", "$ac{d}"),
]
EXT = [
    ("000000xx", "", ""),
    ("000001rr", "dr", "$ar{r}"),
    ("000010rr", "ir", "$ar{r}"),
    ("000011rr", "nr", "$ar{r}"),
    ("0001ddss", "mv", "{d:8}, {s:c}"),
    ("001ss0dd", "s", "@$ar{d}, {s:c}"),
    ("001ss1dd", "sn", "@$ar{d}, {s:c}"),
    ("01ddd0ss", "l", "{d:8}, @$ar{s}"),
    ("01ddd1ss", "ln", "{d:8}, @$ar{s}"),
    ("10dd000s", "ls", "{d:8}, $ac{s}.m"),
    ("10dd010s", "lsn", "{d:8}, $ac{s}.m"),
    ("10dd100s", "lsm", "{d:8}, $ac{s}.m"),
    ("10dd110s", "lsnm", "{d:8}, $ac{s}.m"),
    ("10dd001s", "sl", "$ac{s}.m, {d:8}"),
    ("10dd011s", "sln", "$ac{s}.m, {d:8}"),
    ("10dd101s", "slm", "$ac{s}.m, {d:8}"),
    ("10dd111s", "slnm", "$ac{s}.m, {d:8}"),
    ("11sr0011", "ldax", "$ax{r}, @$ar{s}"),
    ("11sr0111", "ldaxn", "$ax{r}, @$ar{s}"),
    ("11sr1011", "ldaxm", "$ax{r}, @$ar{s}"),
    ("11sr1111", "ldaxnm", "$ax{r}, @$ar{s}"),
    ("11dr00ss", "ld", "$ax0.{d:lh}, $ax1.{r:lh}, @$ar{s}"),
    ("11dr01ss", "ldn", "$ax0.{d:lh}, $ax1.{r:lh}, @$ar{s}"),
    ("11dr10ss", "ldm", "$ax0.{d:lh}, $ax1.{r:lh}, @$ar{s}"),
    ("11dr11ss", "ldnm", "$ax0.{d:lh}, $ax1.{r:lh}, @$ar{s}"),
]


def compile_table(table):
    out = []
    for pattern, name, operands in table:
        words = pattern.split()
        first = words[0]
        mask = value = 0
        fields = {}
        bits = "".join(words)
        total = len(bits)
        for i, ch in enumerate(bits):
            pos = total - 1 - i
            if ch in "01":
                if i < len(first):
                    mask |= 1 << (len(first) - 1 - i)
                    value |= int(ch) << (len(first) - 1 - i)
            elif ch != "x":
                lo, n = fields.get(ch, (pos, 0))
                fields[ch] = (pos, n + 1)
        out.append((mask, value, len(words), name, operands, fields))
    return out


MAIN_T = compile_table(MAIN)
EXT_T = compile_table(EXT)


def lookup(table, word):
    for entry in table:
        if (word & entry[0]) == entry[1]:
            return entry
    return None


class Insn:
    __slots__ = ("addr", "size", "words", "name", "text", "target", "ext", "fields", "ext_fields", "mem", "imm")


def fmt(template, fields, raw, insn):
    out = []
    i = 0
    while i < len(template):
        if template[i] != "{":
            out.append(template[i])
            i += 1
            continue
        j = template.index("}", i)
        key, _, kind = template[i + 1:j].partition(":")
        lo, n = fields[key]
        v = (raw >> lo) & ((1 << n) - 1)
        if kind == "":
            out.append(str(v))
        elif kind == "r":
            out.append("$" + REG[v])
        elif kind == "8":
            out.append("$" + REG[0x18 + v])
        elif kind == "c":
            out.append("$" + REG[0x1C + v])
        elif kind == "o":
            out.append(str(1 - v))
        elif kind == "lh":
            out.append("lh"[v])
        elif kind == "cc":
            out.append(CC[v])
        elif kind == "jcc":
            out.append(CC[v] if v != 15 else "mp")
        elif kind == "i":
            insn.imm = v
            out.append("0x%04x" % v)
        elif kind == "s8":
            insn.imm = v - 256 if v & 0x80 else v
            out.append("%d" % insn.imm)
        elif kind == "d":
            out.append(str(v))
        elif kind == "n6":  # a right shift is encoded as the negative count
            out.append(str(64 - v if v else 0))
        elif kind == "A":
            insn.target = v
            out.append("0x%04x" % v)
        elif kind == "D":
            insn.mem = v
            out.append(IFX.get(v, "0x%04x" % v))
        elif kind == "M":  # 8 bits; the high byte is 0xFF (SI) or the config register (LRS, SRS)
            insn.mem = v
            out.append("0x%02x" % v)
        else:
            raise ValueError(kind)
        i = j + 1
    return "".join(out)


def decode(words, pc, base=0):
    w = words[pc]
    insn = Insn()
    insn.addr = base + pc
    insn.target = insn.mem = insn.imm = None
    insn.ext = None
    insn.fields = insn.ext_fields = {}
    entry = lookup(MAIN_T, w)
    if entry is None or pc + entry[2] > len(words):
        insn.size, insn.words, insn.name, insn.text = 1, [w], None, ".word 0x%04x" % w
        return insn
    _, _, size, name, operands, fields = entry
    raw = w
    if size == 2:
        raw = (w << 16) | words[pc + 1]
    insn.size = size
    insn.words = words[pc:pc + size]
    insn.fields = {k: (raw >> lo) & ((1 << n) - 1) for k, (lo, n) in fields.items()}
    name = fmt(name, fields, raw, insn)
    text = fmt(operands, fields, raw, insn)
    insn.name = name
    top = w >> 12
    if top >= 3:  # an extension shares the word
        e = w & (0x7F if top == 3 else 0xFF)
        ext = lookup(EXT_T, e)
        if ext is None:
            insn.name, insn.text = None, ".word 0x%04x" % w
            return insn
        if ext[3]:
            insn.ext = ext[3]
            insn.ext_fields = {k: (e >> lo) & ((1 << n) - 1) for k, (lo, n) in ext[5].items()}
            name += "'" + ext[3]
            text += (" : " if text else ": ") + fmt(ext[4], ext[5], e, Insn())
    insn.text = (name + " " + text).rstrip()
    return insn


def disassemble(words, base=0):
    out = []
    pc = 0
    while pc < len(words):
        insn = decode(words, pc, base)
        out.append(insn)
        pc += insn.size
    return out


def read_dol(path, address, size):
    d = open(path, "rb").read()
    offs = struct.unpack(">18I", d[0:72])
    addrs = struct.unpack(">18I", d[72:144])
    sizes = struct.unpack(">18I", d[144:216])
    for a, o, s in zip(addrs, offs, sizes):
        if s and a <= address and address + size <= a + s:
            return d[o + address - a:o + address - a + size]
    sys.exit("dspdis: %08X+%X is not inside one section of %s" % (address, size, path))


def ector_hash(data):
    """The hash emulators name microcode by: xor each byte in, rotate left 3."""
    h = 0
    for b in data:
        h ^= b
        h = ((h << 3) | (h >> 29)) & 0xFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("file")
    ap.add_argument("--dol", nargs=2, metavar=("ADDRESS", "SIZE"))
    ap.add_argument("--base", default="0")
    ap.add_argument("--words", action="store_true", help="the file is text: hexadecimal words")
    ap.add_argument("--stats", action="store_true")
    ap.add_argument("-o", "--output")
    a = ap.parse_args()
    if a.words:  # hexadecimal words as text, ';' starts a comment (the form qilin-dsp --words takes)
        text = [line.split(";")[0] for line in open(a.file, encoding="utf-8")]
        data = b"".join(struct.pack(">H", int(t, 16)) for line in text for t in line.split())
    else:
        data = read_dol(a.file, int(a.dol[0], 16), int(a.dol[1], 16)) if a.dol else open(a.file, "rb").read()
    words = list(struct.unpack(">%dH" % (len(data) // 2), data[:len(data) & ~1]))
    insns = disassemble(words, int(a.base, 16))
    targets = {}
    for i in insns:
        if i.target is not None:
            kind = "sub" if i.name.startswith("call") else "loop" if i.name.startswith("bloop") else "loc"
            if targets.get(i.target) != "sub":
                targets[i.target] = kind
    out = open(a.output, "w", encoding="utf-8") if a.output else sys.stdout
    out.write("; %d words, hash %08X\n" % (len(words), ector_hash(data)))
    for i in insns:
        if i.addr in targets and targets[i.addr] != "loop":
            out.write("%s_%04x:\n" % (targets[i.addr], i.addr))
        note = ""
        if i.mem is not None and i.name in ("si",):
            note = "  ; " + IFX.get(0xFF00 | i.mem, "")
        if i.addr in targets and targets[i.addr] == "loop":
            note += "  ; loop end"
        out.write("  %04x  %-9s  %s%s\n" % (i.addr, " ".join("%04x" % w for w in i.words), i.text, note))
    if a.stats:
        count = {}
        hw = {}
        rom = []
        for i in insns:
            key = i.name or ".word"
            count[key.split("'")[0]] = count.get(key.split("'")[0], 0) + 1
            if i.ext:
                count["'" + i.ext] = count.get("'" + i.ext, 0) + 1
            if i.mem is not None:
                m = i.mem if i.mem > 0xFF else None
                if m is not None and m >= 0xFF00:
                    hw[m] = hw.get(m, 0) + 1
            if i.target is not None and i.target >= 0x8000:
                rom.append("%04x %s" % (i.addr, i.text))
            if i.imm is not None and 0x1000 <= i.imm < 0x1800 and i.name == "lri":
                rom.append("%04x %s (a coefficient ROM address, if used as one)" % (i.addr, i.text))
        out.write("\n; instructions: %d\n" % len(insns))
        for k in sorted(count, key=lambda k: -count[k]):
            out.write(";   %-10s %d\n" % (k, count[k]))
        out.write("; hardware registers by full address:\n")
        for m in sorted(hw):
            out.write(";   %04X %-10s %d\n" % (m, IFX.get(m, "?"), hw[m]))
        out.write("; ROM references: %d\n" % len(rom))
        for r in rom:
            out.write(";   %s\n" % r)


if __name__ == "__main__":
    main()
