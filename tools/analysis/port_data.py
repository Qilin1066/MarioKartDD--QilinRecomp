"""Place the debug map's data symbols in the retail DOL, and use them to settle function names.

Data is laid out like the text: object by object in the link order, a symbol's
size the same in both builds, but symbols the release build does not need are
gone, so an object's later symbols sit lower than in the debug build. Within
an object a symbol's distance to its debug address (its "shift") can only
stay or fall, give or take alignment padding.

What fixes a shift is a reference from a named function. The map's link tree
says which function first referenced each symbol, or each object's section
start (the `...data.0`, `...rodata.0`, `...bss.0` entries: CodeWarrior
addresses an object's data from one base). The same function in the retail
DOL references a handful of data addresses (r13- and r2-relative, and
`lis`-built ones with the displacements used on them); the symbol is at one
of them. Per object, the longest staircase of non-rising shifts through these
candidates is taken. A symbol between two references with the same shift is
placed with certainty; between two different shifts it is placed only if one
of the two addresses is referenced and the other is not (a vtable also has to
hold function pointers); before the first or after the last reference it
takes the nearest shift, with less confidence.

Confidence: A on or between references that agree on the shift (two or more in
the object); B chosen between two different shifts because only one of the two
addresses is referenced (for a vtable: holds function pointers); C on a single
reference, or by the nearest reference only. Compiler-made names (@123) are
placed but not written.

Checked against the PAL list of the decompilation (a name in both keeps its
distance to the PAL address among its neighbours): of 370 comparable A names
26 differ, all but two by 4 to 40 bytes, which the two builds' own data
differences may explain; single references were right only half the time in
.sbss, hence C.

port_symbols.py calls this twice: to give the alignment the data each function
reads (a getter is named by the variable it returns), and on the final names
for the output. Naming virtual functions from the vtables found this way was
tried and dropped: the link tree does not list a vtable's functions in slot
order, and pairing by elimination was wrong for 6 of 29 names the PAL list
could check.
"""
import bisect
import collections

SECTIONS = (".rodata", ".data", ".bss", ".sdata", ".sbss", ".sdata2", ".sbss2")
PSEUDO = {"...rodata.0": ".rodata", "...data.0": ".data", "...bss.0": ".bss"}
SLACK = 28  # alignment padding may push a symbol up a little


def retail_sections(dol):
    """{name: (start, end)} from __init_data's tables, which follow the code of .init."""
    lo = min(a for a, _, _, t, _ in dol.sections if t)
    hi = lo + next(s for a, s, _, t, _ in dol.sections if a == lo)
    table = next(a for a in range(lo, hi, 4) if dol.u32(a) == lo and dol.u32(a + 4) == lo)
    names = [".init", "extab", "extabindex", ".text", ".ctors", ".dtors", ".BINARY", ".rodata", ".data",
             ".sdata", ".sdata2"]
    out = {}
    a = table
    for name in names:
        start, size = dol.u32(a + 4), dol.u32(a + 8)
        out[name] = (start, start + size)
        a += 12
    if dol.u32(a + 8) != 0:
        raise SystemExit("unexpected _rom_copy_info: not eleven sections")
    a += 12
    for name in (".bss", ".sbss", ".sbss2"):
        start, size = dol.u32(a), dol.u32(a + 4)
        out[name] = (start, start + size)
        a += 8
    return out


def data_references(port, r2, r13):
    """Per retail function: the data addresses it references."""
    dol = port.dol
    text = [(a, a + s) for a, s, _, t, _ in dol.sections if t]
    out = []
    for start, end, _ in port.R:
        refs = set()
        high = {}   # register -> lis value
        full = {}   # register -> built address
        for a in range(start, end, 4):
            w = dol.u32(a) or 0
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15 and ra == 0:
                high[rd] = imm << 16
                full.pop(rd, None)
                continue
            if op == 14 or 32 <= op <= 55:  # addi, loads and stores
                v = None
                if ra in (2, 13):
                    v = ((r2 if ra == 2 else r13) + simm) & 0xFFFFFFFF
                elif ra in full:
                    v = (full[ra] + simm) & 0xFFFFFFFF
                elif ra in high and ra != 0:
                    v = (high[ra] + simm) & 0xFFFFFFFF
                if v is not None:
                    refs.add(v)
                    if op == 14:  # the register now holds the address
                        high.pop(rd, None)
                        full[rd] = v
                        continue
            elif op == 24 and rd in high:  # ori rA,rS,imm
                v = high[rd] | imm
                refs.add(v)
                full[ra] = v
                continue
            elif op == 18 and w & 1:  # a call: the volatile registers are gone
                for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                    high.pop(r, None)
                    full.pop(r, None)
                continue
            # anything else that writes a general register ends what we knew of it
            if op in (7, 8, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43):
                written = rd
            elif op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
                written = ra
            elif op == 31:
                written = ra if (w >> 1) & 0x3FF in (28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824,
                                                     26, 922, 954) else rd
            elif op == 46:  # lmw
                for r in range(rd, 32):
                    high.pop(r, None)
                    full.pop(r, None)
                continue
            else:
                continue
            high.pop(written, None)
            full.pop(written, None)
        out.append({v for v in refs if not any(lo <= v < hi for lo, hi in text)})
    return out


class Data:
    def __init__(self, port, r2, r13):
        self.port = port
        self.sections = retail_sections(port.dol)
        if not hasattr(port, "rdata"):
            port.rdata = data_references(port, r2, r13)
        self.referenced = set().union(*port.rdata)
        for _, w in port.dol.data_words():
            self.referenced.add(w)
        # the link tree upwards: the function that first referenced each data symbol
        self.up = {}
        for parent, kids in port.map.tree.items():
            for k in kids:
                self.up.setdefault(k, parent)
        self.findex = {}
        for i, d in enumerate(port.D):
            self.findex.setdefault((d.name, d.lib, d.obj), i)

    def referrer(self, key):
        """The function a data symbol hangs under in the tree (through section pseudo-entries only)."""
        kind = self.port.map.kind
        p = self.up.get(key)
        while p is not None and kind.get(p, ("", ""))[0] in ("notype", "section"):
            p = self.up.get(p)
        if p is not None and kind.get(p, ("", ""))[0] == "func":
            return self.findex.get(p)
        return None

    def place(self, conf):
        """[(retail address, size, Symbol, confidence)] and {function index: [retail addresses of its data]}."""
        port = self.port
        placed = []
        kids = collections.defaultdict(list)
        good = {i for i, c in conf.items() if c in "AB" and i in port.match}
        for section in SECTIONS:
            if section not in port.map.sections or section not in self.sections:
                continue
            lo, hi = self.sections[section]
            symbols = port.map.sections[section]
            starts = [s.addr for s in symbols]
            for base, size, lib, obj in port.map.objects.get(section, []):
                a, b = bisect.bisect_left(starts, base), bisect.bisect_left(starts, base + size)
                items = [(base, None)] + [(s.addr, s) for s in symbols[a:b] if (s.lib, s.obj) == (lib, obj)]
                cands = []
                for address, sym in items:
                    if sym is None:
                        name = next((n for n, sec in PSEUDO.items() if sec == section), None)
                        key = (name, lib, obj) if name else None
                    else:
                        key = (sym.name, lib, obj)
                    f = self.referrer(key) if key else None
                    if f in good:
                        deltas = sorted({r - address for r in port.rdata[port.match[f]] if lo <= r < hi})
                        cands.append((f, deltas))
                    else:
                        cands.append((None, []))
                shift = self.staircase(cands)
                anchored = [k for k, s in enumerate(shift) if s is not None]
                if not anchored:
                    continue
                counts = collections.Counter(shift[k] for k in anchored)
                for k, (address, sym) in enumerate(items):
                    if sym is None:
                        continue
                    at = bisect.bisect_left(anchored, k)
                    before = anchored[at - 1] if at else None
                    after = anchored[at] if at < len(anchored) else None
                    if shift[k] is not None:
                        s, c = shift[k], "A" if counts[shift[k]] >= 2 else "C"
                    elif before is not None and after is not None and shift[before] == shift[after]:
                        s, c = shift[before], "A" if counts[shift[before]] >= 2 else "C"
                    elif before is not None and after is not None:
                        ok = [x for x in (shift[before], shift[after]) if self.plausible(sym, address + x, lo, hi)]
                        if len(ok) != 1:
                            continue
                        s, c = ok[0], "B"
                    else:
                        s, c = shift[before if before is not None else after], "C"
                    r = address + s
                    if not lo <= r < hi or (sym.name.startswith("__vt__") and not self.vtable(r, sym.size)):
                        continue
                    placed.append((r, sym.size, sym, c))
                    f = cands[k][0]
                    if f is not None and c in "AB" and shift[k] is not None:
                        kids[f].append(r)
        return placed, kids

    @staticmethod
    def staircase(cands):
        """One shift per item (or None): the longest chain of candidates whose shifts do not rise
        by more than the padding slack."""
        nodes = [(k, d) for k, (_, deltas) in enumerate(cands) for d in deltas[:24]]
        if not nodes:
            return [None] * len(cands)
        votes = collections.Counter(d for _, d in nodes)
        best = [0.0] * len(nodes)
        prev = [-1] * len(nodes)
        for x, (k, d) in enumerate(nodes):
            best[x] = 1.0 + 0.01 * votes[d]
            for y in range(x):
                k2, d2 = nodes[y]
                if k2 < k and d <= d2 + SLACK and best[y] + 1.0 + 0.01 * votes[d] + (0.5 if d == d2 else 0.0) > best[x]:
                    best[x] = best[y] + 1.0 + 0.01 * votes[d] + (0.5 if d == d2 else 0.0)
                    prev[x] = y
        x = max(range(len(nodes)), key=lambda n: best[n])
        out = [None] * len(cands)
        while x >= 0:
            out[nodes[x][0]] = nodes[x][1]
            x = prev[x]
        return out

    def plausible(self, sym, address, lo, hi):
        if not lo <= address < hi:
            return False
        if sym.name.startswith("__vt__"):
            return self.vtable(address, sym.size)
        return address in self.referenced

    def vtable(self, address, size):
        """Two header words (RTTI pointer or zero, zero), then function pointers or zero."""
        port = self.port
        words = [port.dol.u32(address + o) for o in range(0, size - size % 4, 4)]
        if len(words) < 3 or None in words:
            return False
        return any(words[2:]) and all(w == 0 or w in port.rindex for w in words[2:])
