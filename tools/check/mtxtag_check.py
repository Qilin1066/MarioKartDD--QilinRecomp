#!/usr/bin/env python3
"""Check that the native matrix loaders write the bytes the game's own do.

    tools/check/mtxtag_check.py --dol <disc>/sys/main.dol --test <build>/mkdd_render_test

mkdd/src/mtxtag.cpp replaces J3DFifoLoadPosMtxImm (80067A64) and
GXLoadPosMtxImm (800C20D8) so that every position matrix carries a tag for
the pictures drawn between frames. The only thing the game can see of that
is the bytes in the FIFO, which must be the originals'. This runs the two
functions' own code from the DOL (a few instructions each: an interpreter
for just those is below, paired-single loads and stores among them, which
general PowerPC emulators lack) on a set of matrices and slots, asks the
test program for the bytes the replacements write for the same inputs
(`mkdd_render_test --mtxtag`), and compares. It needs the DOL, so it runs
where the game is built, not in CI.
"""
import argparse
import os
import struct
import subprocess
import sys

FUNCTIONS = [("J3DFifoLoadPosMtxImm", 0x80067A64), ("GXLoadPosMtxImm", 0x800C20D8)]
SLOTS = [0, 3, 27, 30]
MATRIX_AT = 0x80600000
PIPE = 0xCC008000


def matrix_words(k):
    """The test's k-th matrix: twelve words, each a finite float's bits (render_test.cpp has the same)."""
    return [(0x3F800000 + 0x00010203 * (k * 12 + i + 1)) & 0xFFFFFFFF for i in range(12)]


class Dol:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        offs = struct.unpack_from(">18I", self.data, 0x00)
        addrs = struct.unpack_from(">18I", self.data, 0x48)
        sizes = struct.unpack_from(">18I", self.data, 0x90)
        self.sections = [(a, o, s) for a, o, s in zip(addrs, offs, sizes) if s]

    def word(self, address):
        for a, o, s in self.sections:
            if a <= address < a + s:
                return struct.unpack_from(">I", self.data, o + address - a)[0]
        raise SystemExit(f"{address:08X} isn't in the DOL")


def run(dol, entry, r3, r4):
    """Run the leaf at `entry` until its blr; the bytes it writes to the pipe."""
    r = [0] * 32
    r[3], r[4] = r3, r4
    f = [(0, 0)] * 32  # each paired-single register: two words, as loaded
    mem = {}
    for k in range(len(SLOTS)):
        for i, w in enumerate(matrix_words(k)):
            mem[MATRIX_AT + 64 * k + 4 * i] = w
    out = bytearray()

    def store(address, value, size):
        if address & ~31 != PIPE:
            raise SystemExit(f"a store to {address:08X}: not the pipe")
        out.extend(value.to_bytes(4, "big")[4 - size:])

    def ea(insn):
        ra, d = (insn >> 16) & 31, insn & 0xFFFF
        return ((r[ra] if ra else 0) + (d - 0x10000 if d & 0x8000 else d)) & 0xFFFFFFFF

    pc = entry
    for _ in range(200):
        insn = dol.word(pc)
        op, rt, ra = insn >> 26, (insn >> 21) & 31, (insn >> 16) & 31
        simm = (insn & 0xFFFF) - (0x10000 if insn & 0x8000 else 0)
        if insn == 0x4E800020:  # blr
            return bytes(out)
        if op == 14:  # addi
            r[rt] = ((r[ra] if ra else 0) + simm) & 0xFFFFFFFF
        elif op == 15:  # addis
            r[rt] = ((r[ra] if ra else 0) + (simm << 16)) & 0xFFFFFFFF
        elif op == 25:  # oris (rs is in the rt field)
            r[ra] = r[rt] | ((insn & 0xFFFF) << 16)
        elif op == 21:  # rlwinm
            sh, mb, me = (insn >> 11) & 31, (insn >> 6) & 31, (insn >> 1) & 31
            v = ((r[rt] << sh) | (r[rt] >> (32 - sh))) & 0xFFFFFFFF if sh else r[rt]
            bits = [1 if (mb <= b <= me if mb <= me else (b >= mb or b <= me)) else 0 for b in range(32)]
            mask = int("".join(map(str, bits)), 2)
            r[ra] = v & mask
        elif op == 32:  # lwz
            r[rt] = mem[ea(insn)]
        elif op == 36:  # stw
            store(ea(insn), r[rt], 4)
        elif op == 38:  # stb
            store(ea(insn), r[rt] & 0xFF, 1)
        elif op == 44:  # sth
            store(ea(insn), r[rt] & 0xFFFF, 2)
        elif op in (56, 60):  # psq_l, psq_st: a pair of floats through GQR0 (no scaling), 12-bit offset
            if (insn >> 15) & 1 or (insn >> 12) & 7:
                raise SystemExit(f"{pc:08X}: a paired-single access that isn't a pair through GQR0")
            d = insn & 0xFFF
            a = ((r[ra] if ra else 0) + (d - 0x1000 if d & 0x800 else d)) & 0xFFFFFFFF
            if op == 56:
                f[rt] = (mem[a], mem[a + 4])
            else:
                store(a, f[rt][0], 4)
                store(a, f[rt][1], 4)
        else:
            raise SystemExit(f"{pc:08X}: instruction {insn:08X} isn't one this check knows; has the function changed?")
        pc += 4
    raise SystemExit(f"{entry:08X}: no return within 200 instructions")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--test", required=True, help="the mkdd_render_test executable")
    args = ap.parse_args()
    dol = Dol(args.dol)
    want = bytearray()
    for _, entry in FUNCTIONS:
        for k, slot in enumerate(SLOTS):
            want += run(dol, entry, MATRIX_AT + 64 * k, slot)
    got = None
    for line in subprocess.run([os.path.abspath(args.test), "--mtxtag"], capture_output=True, text=True).stdout.splitlines():
        if line.startswith("mtxtag: "):
            got = bytes.fromhex(line.split()[1])
    if got is None:
        print("the test program printed no 'mtxtag:' line", file=sys.stderr)
        return 2
    if got[:len(want)] != bytes(want):
        at = next(i for i in range(len(want)) if i >= len(got) or got[i] != want[i])
        name = FUNCTIONS[at // (53 * len(SLOTS))][0]
        print(f"DIFFERENT at byte {at} ({name}, call {at // 53 % len(SLOTS)}): the game writes "
              f"{bytes(want[at:at + 8]).hex()}, the replacement {got[at:at + 8].hex()}", file=sys.stderr)
        return 1
    print(f"mtxtag: {len(want)} bytes from {len(FUNCTIONS) * len(SLOTS)} calls, the same from the game's code and the replacements")
    return 0


if __name__ == "__main__":
    sys.exit(main())
