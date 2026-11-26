#!/usr/bin/env python3
"""A static trace of the boot: which replaced functions and hardware registers the game
reaches, in roughly the order it reaches them.

The call graph is walked depth-first in call order from a list of stages:

  1. the static constructors (.ctors, in table order), which __init_user runs;
  2. System::init, the first call of main: everything before the first frame;
  3. System::run, the frame loop;
  4. then one stage per class named with --stage (all the class's functions as
     roots: an application or scene the loop enters through a virtual call),
     each on its own, whatever the earlier stages listed.

Edges are direct calls and function addresses a function builds (thread entry
points, callbacks: marked "ptr"). Virtual calls cannot be followed, so after
each stage's walk the vtables its functions reference (constructors do) give
a second wave, marked "virtual": the functions of those vtables and what they
reach. In stages 1 to 3 a function is listed once, in the first stage that
reaches it.

What is printed per stage: every replaced function (bound or MISSING if
--sources is given), every dropped one, and every lifted function that touches
a hardware register or low memory (from hardware_access.py's output), with
the chain of callers that led there.

Confidence: "direct" entries of stages 1 and 2 run unless a branch skips them
(error paths are walked too: OSPanic and friends appear without being reached
on a good boot); "ptr" and "virtual" entries are reachable, not certain, and
their place in the order is a guess.

  boot_path.py cfg.json --symbols symbols.txt --data symbols-data.txt --dol main.dol
               [--hardware hardware.txt] [--sources mkdd/src ...] [--stage LogoApp ...] [-o boot-path.txt]
"""
import argparse
import json
import os
import pathlib
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcdol  # noqa: E402
import port_data  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
ENTRY = re.compile(r"\{\s*0x(8[0-9A-Fa-f]{7})u?\s*,\s*\"")


class Shim:
    pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cfg")
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--data", required=True, help="symbols-data.txt: the vtables")
    ap.add_argument("--dol", required=True)
    ap.add_argument("--plan", default=str(ROOT / "data/GM4E01/plan.tsv"))
    ap.add_argument("--hardware", help="output of hardware_access.py --symbols")
    ap.add_argument("--sources", action="append", default=[])
    ap.add_argument("--stage", action="append", default=[], help="a class whose functions start a further stage")
    ap.add_argument("-o", "--output")
    args = ap.parse_args()

    dol = gcdol.Dol(args.dol)
    plan = gcdol.Plan(args.plan)
    functions = gcdol.cfg_functions(json.load(open(args.cfg)))
    index = {f[0]: n for n, f in enumerate(functions)}
    names = {a: n for a, _, n, _, _, _ in gcdol.read_symbols(args.symbols)}
    by_name = {}
    for a, n in names.items():
        by_name.setdefault(n, a)

    bound = {a: fname for a, _, fname, _ in gcdol.native_tables(args.sources)}

    hardware = {}
    if args.hardware:
        for line in open(args.hardware, encoding="utf-8"):
            m = re.match(r"^  ([0-9A-F]{8}) \S+ \[.*?\]: (.*)$", line)
            if m:
                hardware[int(m.group(1), 16)] = m.group(2)

    # edges
    shim = Shim()
    shim.dol, shim.R = dol, functions
    _, r2, r13 = gcdol.small_data_bases(dol)
    refs = port_data.data_references(shim, r2, r13)
    vtables = {}
    for line in open(args.data, encoding="utf-8"):
        p = line.split()
        if not line.startswith("#") and len(p) >= 6 and p[2].startswith("__vt__"):
            a, size = int(p[0], 16), int(p[1], 16)
            vtables[a] = (p[2], [w for w in (dol.u32(a + o) for o in range(8, size - size % 4, 4)) if w in index])
    pointers = []
    for start, end, _ in functions:
        high, out = {}, []
        for a in range(start, end, 4):
            w = dol.u32(a) or 0
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:
                high[rd] = imm << 16
            elif op == 14 and ra in high:
                v = (high[ra] + imm - (0x10000 if imm & 0x8000 else 0)) & 0xFFFFFFFF
                if v in index and v not in out:
                    out.append(v)
        pointers.append(out)

    ctors = []
    for a, size, _, text, _ in dol.sections:
        if not text and a > max(s[0] for s in dol.sections if s[3]):
            ctors = [w for w in (dol.u32(a + o) for o in range(0, size, 4)) if w in index]
            break

    seen = {}
    out = []

    def walk(roots, how):
        """Depth-first in call order. Returns the functions visited."""
        visited = []
        stack = [(r, None, how) for r in reversed(roots)]
        while stack:
            f, via, kind = stack.pop()
            if f in seen:
                continue
            seen[f] = (via, kind)
            visited.append(f)
            if plan.decide(f)[0] != "lift":
                continue  # a replaced function's own calls are not the game's
            n = index[f]
            nxt = [(c, f, kind) for c in functions[n][2]]
            nxt += [(c, f, "ptr" if kind == "direct" else kind) for c in pointers[n]]
            stack.extend(reversed(nxt))
        return visited

    def chain(f):
        parts = []
        while f is not None and len(parts) < 7:
            parts.append(names.get(f, f"{f:08X}"))
            f = seen[f][0]
        return " <- ".join(parts[1:])

    def report(title, visited):
        rows = []
        for f in visited:
            kind, lib = plan.decide(f)
            how = seen[f][1]
            if kind == "replace":
                state = "" if not args.sources else (f"bound in {bound[f]}" if f in bound else "MISSING")
                rows.append(f"  {how:7s} {f:08X} {names.get(f, '?')} [{lib}] {state}\n            via {chain(f)}")
            elif kind == "drop":
                rows.append(f"  {how:7s} {f:08X} {names.get(f, '?')} DROPPED [{lib}]\n            via {chain(f)}")
            elif f in hardware:
                rows.append(f"  {how:7s} {f:08X} {names.get(f, '?')} touches {hardware[f]}\n            via {chain(f)}")
        out.append(f"== {title}: {len(visited)} functions reached, {len(rows)} to note")
        out.extend(rows)
        out.append("")

    def stage(title, roots):
        visited = walk(roots, "direct")
        report(title, visited)
        wave = visited
        for n in (1, 2):
            targets = []
            for f in wave:
                for a in sorted(refs[index[f]]):
                    if a in vtables:
                        targets += [t for t in vtables[a][1] if t not in seen]
            if not targets:
                break
            wave = walk(list(dict.fromkeys(targets)), "virtual")
            report(f"{title}: virtual functions of the classes built there (wave {n})", wave)

    out.append("# Boot path, static (tools/analysis/boot_path.py). direct = by direct calls from the stage's roots;")
    out.append("# ptr = through a function address built on the way (thread entries, callbacks); virtual = through")
    out.append("# the vtables of classes constructed on the way. Each function is listed once, where first reached.")
    out.append("")
    stage("1. static constructors (__init_user)", ctors)
    for title, name in (("2. System::init (main's first call: before the first frame)", "init__6SystemFv"),
                        ("3. System::run (the frame loop)", "run__6SystemFv")):
        if name not in by_name:
            raise SystemExit(f"{name} is not in the symbols")
        stage(title, [by_name[name]])
    for n, cls in enumerate(args.stage, 4):
        tag = f"__{len(cls)}{cls}F"
        roots = sorted(a for a, nm in names.items() if tag in nm or nm.endswith(f"__{len(cls)}{cls}"))
        # on its own: these were reached above through some vtable, but this is the class's share
        seen.clear()
        report(f"{n}. class {cls}, by direct calls and built addresses from its own functions "
               "(listed again if listed above)", walk(roots, "direct"))
    text = "\n".join(out) + "\n"
    if args.output:
        tmp = args.output + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, args.output)
        print("\n".join(line for line in out if line.startswith("==")))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
