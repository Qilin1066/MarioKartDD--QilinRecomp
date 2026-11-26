#!/usr/bin/env python3
"""Questions docs/enhancements.md asks of the game's code, answered from a listing.

Inputs (none of them belong in the repository):
  --dol      the game's main.dol (for its data sections)
  --insns    `qilin insns main.dol -o listing.txt` (one "address word mnemonic operands" per line)
  --symbols  "ADDRESS SIZE NAME ..." per line (work/symbols.txt), optional

Commands:
  fn NAME|ADDR ...         a function's listing, calls named, small-data operands resolved
  callers NAME|ADDR ...    every bl to it and the function it's in
  refs ADDR[-END] ...      instructions that address the range: r2/r13-relative, or through an
                           addis/addi pair followed within the function (displacements included)
  floats VALUE[~TOL] ...   float and double constants in the data sections near VALUE, with refs
  words ADDR N             N data words as hex, float and integer
  field OFFSET BASE        loads and stores at OFFSET(rX) where rX was loaded from the small-data
                           word BASE (a singleton's pointer) at most 12 instructions before

The register tracking is linear and per function (a register keeps what addis/addi/ori gave it
until it's written again or a call clobbers r0, r3-r12): enough for compiler-made address pairs,
not a data-flow analysis. A reference it reports is real; it can miss ones made through pointers.
"""
import argparse
import bisect
import re
import struct
import sys

SDA = 0x803D1420   # r13, set by __init_registers (80003324..80003338)
SDA2 = 0x803D45A0  # r2

LINE = re.compile(r'^([0-9a-f]{8}) ([0-9a-f]{8}) (\S+)\s*(.*)$')
MEM = re.compile(r'^(?:[rf]?\d+),\s*(-?\d+)\(r?(\d+)\)$')


class Dol:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        self.sections = []  # (address, size, file offset, is_text)
        for i in range(18):
            off, addr, size = (struct.unpack_from('>I', self.data, base + i * 4)[0] for base in (0, 0x48, 0x90))
            if size:
                self.sections.append((addr, size, off, i < 7))

    def read(self, addr, n=4):
        for a, size, off, _ in self.sections:
            if a <= addr and addr + n <= a + size:
                return self.data[off + addr - a:off + addr - a + n]
        return None  # BSS: zero until the game writes it

    def data_words(self):
        for a, size, off, text in self.sections:
            if not text:
                for i in range(0, size - 3, 4):
                    yield a + i, self.data[off + i:off + i + 4]


class Symbols:
    def __init__(self, path):
        self.list = []
        if path:
            for line in open(path, encoding='utf-8', errors='replace'):
                p = line.split()
                if len(p) >= 3 and not line.startswith('#'):
                    try:
                        self.list.append((int(p[0], 16), int(p[1], 16), p[2]))
                    except ValueError:
                        pass
        self.list.sort()
        self.starts = [s[0] for s in self.list]
        self.by_name = {}
        for s in self.list:
            self.by_name.setdefault(s[2], s)

    def at(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and addr < self.list[i][0] + max(self.list[i][1], 4):
            return self.list[i]
        return None

    def name(self, addr):
        s = self.at(addr)
        if not s:
            return '%08X' % addr
        return s[2] if addr == s[0] else '%s+0x%X' % (s[2], addr - s[0])

    def resolve(self, text):
        if text in self.by_name:
            return self.by_name[text][0]
        return int(text, 16)


def listing(path):
    out = []
    for line in open(path, encoding='ascii', errors='replace'):
        m = LINE.match(line.rstrip())
        if m:
            out.append((int(m.group(1), 16), m.group(3), m.group(4)))
    return out


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def effective(insns, syms):
    """Yields (index, address the instruction forms or accesses, kind) by linear tracking."""
    reg = {}
    for i, (addr, op, args) in enumerate(insns):
        s = syms.at(addr)
        if s and s[0] == addr:
            reg = {}
        a = [x.strip() for x in args.split(',')] if args else []
        if op in ('bl', 'bctrl', 'bcctrl', 'blrl', 'bclrl'):
            for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                reg.pop(r, None)
            continue
        m = MEM.match(args)
        if m:  # load or store with a displacement
            d, base = int(m.group(1)), int(m.group(2))
            ea = None
            if base == 13:
                ea = SDA + d
            elif base == 2:
                ea = SDA2 + d
            elif base in reg:
                ea = (reg[base] + d) & 0xFFFFFFFF
            if ea is not None:
                yield i, ea, op
            if op in ('lwz', 'lhz', 'lha', 'lbz', 'lmw', 'lwzu', 'lhzu', 'lbzu'):
                t = a[0].lstrip('r')
                if t.isdigit():
                    reg.pop(int(t), None)
            continue
        try:
            if op == 'addis' and len(a) == 3:
                t, b, imm = int(a[0].lstrip('r')), int(a[1].lstrip('r')), int(a[2], 0)
                if b == 0:
                    reg[t] = (imm << 16) & 0xFFFFFFFF
                elif b in reg:
                    reg[t] = (reg[b] + (imm << 16)) & 0xFFFFFFFF
                else:
                    reg.pop(t, None)
            elif op == 'addi' and len(a) == 3:
                t, b, imm = int(a[0].lstrip('r')), int(a[1].lstrip('r')), int(a[2], 0)
                if b == 13:
                    reg[t] = SDA + imm
                elif b == 2:
                    reg[t] = SDA2 + imm
                elif b == 0:
                    reg[t] = imm & 0xFFFFFFFF
                elif b in reg:
                    reg[t] = (reg[b] + imm) & 0xFFFFFFFF
                else:
                    reg.pop(t, None)
                if t in reg and reg[t] >= 0x80000000:
                    yield i, reg[t], 'addr'
            elif op == 'ori' and len(a) == 3:
                t, b, imm = int(a[0].lstrip('r')), int(a[1].lstrip('r')), int(a[2], 0)
                if b in reg:
                    reg[t] = reg[b] | imm
                    if reg[t] >= 0x80000000:
                        yield i, reg[t], 'addr'
                else:
                    reg.pop(t, None)
            elif op == 'or' and len(a) == 3 and a[1] == a[2]:  # mr
                t, b = int(a[0].lstrip('r')), int(a[1].lstrip('r'))
                if b in reg:
                    reg[t] = reg[b]
                else:
                    reg.pop(t, None)
            elif a and re.fullmatch(r'r\d+', a[0]) and not op.startswith(('st', 'cmp', 'b', 'mt')):
                reg.pop(int(a[0][1:]), None)
        except ValueError:
            pass


def show(insns, syms, i, note=''):
    addr, op, args = insns[i]
    print('%08X  %-7s %-28s %s%s' % (addr, op, args, syms.name(addr), note))


def cmd_fn(dol, insns, syms, targets):
    starts = [x[0] for x in insns]
    notes = {i: (ea, kind) for i, ea, kind in effective(insns, syms)}
    for t in targets:
        if '-' in t and t not in syms.by_name:
            lo, hi = (int(x, 16) for x in t.split('-'))
        else:
            lo = syms.resolve(t)
            s = syms.at(lo)
            hi = s[0] + s[1] if s else lo + 0x100
        for i in range(bisect.bisect_left(starts, lo), bisect.bisect_left(starts, hi)):
            addr, op, args = insns[i]
            s = syms.at(addr)
            if s and s[0] == addr:
                print('; %s' % s[2])
            note = ''
            if op == 'bl':
                note = '  ; ' + syms.name(int(args, 0))
            elif i in notes:
                ea, kind = notes[i]
                note = '  ; %08X' % ea
                raw = dol.read(ea, 8 if kind == 'lfd' else 4)
                if raw and kind == 'lfs':
                    note += ' = %.9g' % struct.unpack('>f', raw)[0]
                elif raw and kind == 'lfd':
                    note += ' = %.17g' % struct.unpack('>d', raw)[0]
            print('%08X  %-7s %s%s' % (addr, op, args, note))


def cmd_callers(dol, insns, syms, targets):
    for t in targets:
        want = syms.resolve(t)
        print('; calls to %s (%08X)' % (syms.name(want), want))
        n = 0
        for i, (addr, op, args) in enumerate(insns):
            if op in ('bl', 'b') and args.startswith('0x') and int(args, 16) == want:
                if op == 'b' and syms.at(addr) and syms.at(addr)[0] <= want < syms.at(addr)[0] + syms.at(addr)[1]:
                    continue
                show(insns, syms, i)
                n += 1
        print('; %d' % n)


def cmd_refs(dol, insns, syms, targets):
    ranges = []
    for t in targets:
        lo, _, hi = t.partition('-')
        lo = int(lo, 16)
        ranges.append((lo, int(hi, 16) if hi else lo + 4))
    for i, ea, kind in effective(insns, syms):
        for lo, hi in ranges:
            if lo <= ea < hi:
                show(insns, syms, i, '  ; %s %08X' % (kind, ea))


def cmd_floats(dol, insns, syms, targets):
    wanted = []
    for t in targets:
        v, _, tol = t.partition('~')
        v = float(eval(v, {'__builtins__': {}}))  # so 608/448 can be written as such
        wanted.append((v, float(tol) if tol else abs(v) * 2e-7))
    found = {}
    for addr, raw in dol.data_words():
        f = struct.unpack('>f', raw)[0]
        for v, tol in wanted:
            if abs(f - v) <= tol:
                found[addr] = ('float', f)
        if addr % 8 == 0:
            raw8 = dol.read(addr, 8)
            if raw8:
                d = struct.unpack('>d', raw8)[0]
                for v, tol in wanted:
                    if abs(d - v) <= tol and d != 0:
                        found[addr] = ('double', d)
    refs = {}
    for i, ea, kind in effective(insns, syms):
        if ea in found:
            refs.setdefault(ea, []).append(i)
    for addr in sorted(found):
        kind, v = found[addr]
        print('; %08X %s %.9g: %d reference(s)' % (addr, kind, v, len(refs.get(addr, []))))
        for i in refs.get(addr, []):
            show(insns, syms, i)


def cmd_words(dol, insns, syms, targets):
    addr, n = int(targets[0], 16), int(targets[1])
    for a in range(addr, addr + 4 * n, 4):
        raw = dol.read(a)
        if raw is None:
            print('%08X  (not in the file: BSS)' % a)
        else:
            print('%08X  %s  %-14.8g %d' % (a, raw.hex(), struct.unpack('>f', raw)[0], struct.unpack('>i', raw)[0]))


def cmd_field(dol, insns, syms, targets):
    offset, base = int(targets[0], 0), int(targets[1], 16)
    holders = {}  # register -> index it was loaded at
    for i, (addr, op, args) in enumerate(insns):
        m = MEM.match(args)
        if not m:
            continue
        d, b = int(m.group(1)), int(m.group(2))
        t = args.split(',')[0].strip()
        ea = SDA + d if b == 13 else SDA2 + d if b == 2 else None
        if op == 'lwz' and ea == base:
            holders[int(t[1:])] = i
            continue
        if b in holders and i - holders[b] <= 12 and d == offset:
            show(insns, syms, i)
        if op.startswith('l') and t[0] == 'r' and int(t[1:]) in holders:
            del holders[int(t[1:])]


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--dol', required=True)
    p.add_argument('--insns', required=True)
    p.add_argument('--symbols')
    p.add_argument('command', choices=['fn', 'callers', 'refs', 'floats', 'words', 'field'])
    p.add_argument('targets', nargs='+')
    a = p.parse_args()
    dol, syms = Dol(a.dol), Symbols(a.symbols)
    insns = listing(a.insns) if a.command != 'words' else []
    globals()['cmd_' + a.command](dol, insns, syms, a.targets)


if __name__ == '__main__':
    sys.exit(main())
