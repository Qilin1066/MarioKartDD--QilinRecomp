#!/usr/bin/env python3
"""Where Mario Kart: Double Dash!!'s code touches the hardware directly, library by library.

For every load and store (and `dcbz`, `dcbz_l`), the base register's value is
traced back on the straight line through the function: `lis`, `addi`, `ori`,
register copies, and loads of a pointer variable (small data via r2/r13, or
an absolute `lis`/`lwz`) whose initial value is in the DOL. If the address
is known, it's classified (GameCube memory map):

  cp       0xCC000000: command processor      pe    0xCC001000: pixel engine
  vi       0xCC002000: video interface        pi    0xCC003000: processor interface (interrupts, FIFO)
  mi       0xCC004000: memory interface       dsp   0xCC005000: DSP mailboxes, ARAM, audio DMA
  dvd      0xCC006000: disc interface         si    0xCC006400: serial interface (controllers)
  exi      0xCC006800: EXI (cards, RTC, BBA)  ai    0xCC006C00: audio streaming interface
  wgpipe   0xCC008000: the GX write-gather pipe (the command FIFO)
  mem-unc  0xC0000000-0xC17FFFFF: main memory through its uncached mirror
  locked   0xE0000000-0xE0003FFF: the locked cache (a scratchpad in L1)
  lowmem   0x80000000-0x800030FF: the OS globals and boot information in low memory

The trace doesn't follow branches back into loops or values passed in from a
caller, so it finds addresses a function builds itself. A pointer the game
gets at run time (OSPhysicalToUncached, a FIFO base from GXInit) isn't seen
here: the runtime catches those, as every mirror and register access traps
during bring-up (docs/roadmap-and-design-targets.md).

Usage:
  hardware_access.py --qilin build/qilin --dol main.dol [--plan data/GM4E01/plan.tsv]
                     [--symbols work/symbols.txt] [--cfg cfg.json] [--list]
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcdol  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLASSES = [
    ("wgpipe", 0xCC008000, 0xCC008040),
    ("cp", 0xCC000000, 0xCC001000),
    ("pe", 0xCC001000, 0xCC002000),
    ("vi", 0xCC002000, 0xCC003000),
    ("pi", 0xCC003000, 0xCC004000),
    ("mi", 0xCC004000, 0xCC005000),
    ("dsp", 0xCC005000, 0xCC006000),
    ("dvd", 0xCC006000, 0xCC006400),
    ("si", 0xCC006400, 0xCC006800),
    ("exi", 0xCC006800, 0xCC006C00),
    ("ai", 0xCC006C00, 0xCC007000),
    ("mem-unc", 0xC0000000, 0xC1800000),
    ("locked", 0xE0000000, 0xE0004000),
    ("lowmem", 0x80000000, 0x80003100),
]
REGISTERS = ("wgpipe", "cp", "pe", "vi", "pi", "mi", "dsp", "dvd", "si", "exi", "ai")
MEMOP = re.compile(r"^(l|st)(b|h|w|f|mw|sw|psq_)\w*$|^(dcbz|dcbz_l|dcbf|dcbst|dcbi|icbi)$")
DISP = re.compile(r"^(\w+) (\w+),(-?\d+)\((r\d+)\)$")      # stw r4,-32768(r3)
INDEXED = re.compile(r"^(\w+) (\w+),(r\d+),(r\d+)$")        # stwx r4,r3,r5 / dcbz r3,r4 (via rA0)
PSQ = re.compile(r"^(psq_\w+) (\w+),(-?\d+)\((r\d+)\),\d+,\d+$")
VOLATILE = {0} | set(range(3, 13))


def classify(a):
    for name, lo, hi in CLASSES:
        if lo <= a < hi:
            return name
    return None


def sx(v):
    return v - 0x10000 if v & 0x8000 else v


def reg(t):
    return int(t[1:]) if t.startswith("r") and t[1:].isdigit() else None


NO_DEST = ("cmp", "cmpi", "cmpl", "cmpli", "tw", "twi", "mtspr", "mtcrf", "mtmsr", "mtsr", "mtsrin", "mtfsf",
           "mtfsfi", "mtfsb0", "mtfsb1", "sync", "isync", "eieio", "rfi", "sc", "fcmpo", "fcmpu", "ps_cmpo0",
           "ps_cmpo1", "ps_cmpu0", "ps_cmpu1", "mcrf", "mcrfs", "mcrxr", "tlbie", "tlbsync")


def operands(text):
    return text.split(" ", 1)[1].split(",") if " " in text else []


def gpr_writes(m, text):
    """GPRs an instruction may write, from its text (objdump prints the destination first)."""
    ops = operands(text)
    if m.startswith(("b", "dcb", "icb")) or m in NO_DEST or not ops:
        return set()
    out = set()
    if m.startswith("st"):  # stores write only the base of the update forms
        if m.endswith(("u", "ux")) and len(ops) >= 2:
            base = ops[1].split("(")[1].rstrip(")") if "(" in ops[1] else ops[1]
            if reg(base) is not None:
                out.add(reg(base))
        return out
    if m == "lmw":
        return set(range(reg(ops[0]), 32))
    if m in ("lswi", "lswx"):
        return set(range(32))
    if m.startswith("l") and m.endswith(("u", "ux")) and len(ops) >= 2:
        base = ops[1].split("(")[1].rstrip(")") if "(" in ops[1] else ops[1]
        if reg(base) is not None:
            out.add(reg(base))
    if reg(ops[0]) is not None:
        out.add(reg(ops[0]))
    return out


class Tracer:
    def __init__(self, insns, dol, r2, r13):
        self.insns = insns
        self.dol = dol
        self.r2, self.r13 = r2, r13

    def value(self, r, before, start, depth=0):
        """The constant in GPR r just before `before`, walking back on the straight line to `start`."""
        if depth > 8 or r is None or r == 0:
            return None
        p = before
        while p > start:
            p -= 4
            w, m, text = self.insns.get(p, (0, "", ""))
            ops = operands(text)
            if m == "rfi" or m == "b" or (m in ("bclr", "bcctr") and ops and ops[0] == "20"):
                return None  # code above can't fall through to here
            if m in ("bl", "bclrl", "bcctrl", "bla") or (m == "bcl"):
                if r in VOLATILE:
                    return None  # a call may change it
                continue
            if r not in gpr_writes(m, text):
                continue
            if m == "addis":
                hi = (int(ops[2]) & 0xFFFF) << 16
                if ops[1] == "0":
                    return hi
                base = self.value(reg(ops[1]), p, start, depth + 1)
                return None if base is None else (base + hi) & 0xFFFFFFFF
            if m == "addi":
                lo = int(ops[2])
                if ops[1] == "0":
                    return lo & 0xFFFFFFFF
                rb = reg(ops[1])
                if rb in (2, 13):
                    return ((self.r2 if rb == 2 else self.r13) + lo) & 0xFFFFFFFF
                base = self.value(rb, p, start, depth + 1)
                return None if base is None else (base + lo) & 0xFFFFFFFF
            if m == "ori":
                base = self.value(reg(ops[1]), p, start, depth + 1)
                return None if base is None else base | (int(ops[2]) & 0xFFFF)
            if m == "or" and len(ops) == 3 and ops[1] == ops[2]:  # mr
                return self.value(reg(ops[1]), p, start, depth + 1)
            if m == "lwz" and "(" in ops[1]:  # a pointer variable with an initial value
                disp, base_reg = ops[1].rstrip(")").split("(")
                rb = reg(base_reg)
                if rb in (2, 13):
                    addr = ((self.r2 if rb == 2 else self.r13) + int(disp)) & 0xFFFFFFFF
                else:
                    base = self.value(rb, p, start, depth + 1)
                    if base is None:
                        return None
                    addr = (base + int(disp)) & 0xFFFFFFFF
                v = self.dol.u32(addr)
                return v if v and classify(v) else None
            return None
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--qilin", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--plan", default=os.path.join(ROOT, "data", "GM4E01", "plan.tsv"))
    ap.add_argument("--symbols", help="symbols.txt (port_symbols.py): name the functions")
    ap.add_argument("--cfg", help="an existing `qilin cfg --json` of the DOL")
    ap.add_argument("--list", action="store_true", help="list every access found in lifted code")
    args = ap.parse_args()

    dol = gcdol.Dol(args.dol)
    insns = gcdol.parse_insns(gcdol.run_qilin(args.qilin, args.dol, "insns"))
    funcs = gcdol.cfg_functions(gcdol.run_qilin(args.qilin, args.dol, "cfg", args.cfg))
    plan = gcdol.Plan(args.plan)
    names = {a: n for a, _, n, _, _, _ in gcdol.read_symbols(args.symbols)} if args.symbols else {}
    _, r2, r13 = gcdol.small_data_bases(dol)
    tracer = Tracer(insns, dol, r2, r13)

    per_lib = collections.defaultdict(collections.Counter)
    funcs_lib = collections.defaultdict(lambda: collections.defaultdict(set))
    kinds = {}
    order = []
    found = []
    for s, e, _ in funcs:
        kind, lib = plan.decide(s)
        if lib not in kinds:
            kinds[lib] = kind
            order.append(lib)
        for a in range(s, e, 4):
            w, m, text = insns.get(a, (0, "", ""))
            if not MEMOP.match(m):
                continue
            base = disp = None
            md = DISP.match(text) or PSQ.match(text)
            if md:
                base, disp = reg(md.group(4)), int(md.group(3))
            else:
                mx = INDEXED.match(text)
                if mx:  # rA (0 means 0) + rB: known only when one side is 0
                    ra_, rb_ = reg(mx.group(3)), reg(mx.group(4))
                    if ra_ == 0 or ra_ is None:
                        base, disp = rb_, 0
                    else:
                        vb = tracer.value(rb_, a, s)
                        base, disp = (ra_, vb) if vb is not None and vb < 0x10000 else (None, None)
                elif m in ("dcbz", "dcbz_l", "dcbf", "dcbst", "dcbi", "icbi"):
                    parts = text.split()[1].split(",") if " " in text else []
                    if len(parts) == 2:
                        base, disp = reg(parts[1]), 0
            if base is None or base == 1:
                continue
            v = tracer.value(base, a, s)
            if v is None:
                continue
            addr = (v + disp) & 0xFFFFFFFF
            cls = classify(addr)
            if not cls:
                continue
            per_lib[lib][cls] += 1
            funcs_lib[lib][cls].add(s)
            found.append((a, s, lib, cls, addr, text))

    used = [c[0] for c in CLASSES if any(per_lib[lib][c[0]] for lib in order)]
    print("| Library | Plan | " + " | ".join(used) + " |")
    print("|---|---|" + "---|" * len(used))
    for lib in order:
        if not per_lib[lib]:
            continue
        cells = [f"{per_lib[lib][c]} ({len(funcs_lib[lib][c])})" if per_lib[lib][c] else "" for c in used]
        print(f"| {lib} | {kinds[lib]} | " + " | ".join(cells) + " |")
    print()
    print("Cells: accesses (functions).")
    lifted = [f for f in found if kinds[f[2]] == "lift"]
    print(f"Accesses in lifted code: {len(lifted)} in {len({f[1] for f in lifted})} functions")
    by_func = collections.defaultdict(list)
    for f in lifted:
        by_func[f[1]].append(f)
    for s in sorted(by_func):
        hits = by_func[s]
        what = collections.Counter(h[3] for h in hits)
        span = sorted({h[4] for h in hits})
        print(f"  {s:08X} {names.get(s, '?')} [{hits[0][2]}]: "
              + ", ".join(f"{c} x{n}" for c, n in what.items())
              + f" ({span[0]:08X}" + (f"..{span[-1]:08X})" if len(span) > 1 else ")"))
        if args.list:
            for a, _, _, cls, addr, text in hits:
                print(f"      {a:08X} {cls} {addr:08X}: {text}")
    # Data words holding a hardware register address: pointer variables such as the SDK's
    # __VIRegs. (Mirror addresses aren't counted: floats like -2.0 fall in those ranges.)
    ptrs = [(a, v) for a, v in dol.data_words() if classify(v) in REGISTERS]
    print(f"Data words holding a hardware register address: {len(ptrs)}")
    for a, v in ptrs:
        print(f"  {a:08X} = {v:08X} ({classify(v)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
