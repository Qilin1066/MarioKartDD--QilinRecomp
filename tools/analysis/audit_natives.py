#!/usr/bin/env python3
"""Check the native replacement tables against the plan and the ported symbols.

Every `{0x8xxxxxxx, "name", fn}` entry in the sources is checked: the address
must be the start of a function, in a replaced range of the plan (rt::provide
is fatal otherwise), and the function there should be the one the entry names
(symbols.txt). Then every replaced function that lifted code calls, or that
lifted code or data points at, is listed as bound (with its file) or missing.

  audit_natives.py cfg.json --symbols symbols.txt --dol main.dol --sources mkdd/src [--sources other/mkdd/src]
                   [--notes notes.tsv] [-o missing-natives.txt]

With several --sources the later directory's file wins where both have a file
of the same name (a work in progress over the merged tree). notes.tsv
(`NAME<tab>what the function does`) adds a line to each missing function.
"""
import argparse
import bisect
import json
import os
import pathlib
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcdol  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
ENTRY = re.compile(r"\{\s*0x(8[0-9A-Fa-f]{7})u?\s*,\s*\"([^\"]+)\"")


def base_name(mangled):
    """`alloc__7JKRHeapFUli` -> `alloc`; C names unchanged."""
    name = mangled.split(" (")[0].split("::")[-1]  # table entries: "System::getPerspAspect", "x (no debugger)"
    m = re.match(r"^(.+?)__[0-9A-Z]", name)
    return m.group(1) if m and not name.startswith("__") else name


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cfg")
    ap.add_argument("--symbols", required=True)
    ap.add_argument("--dol", required=True)
    ap.add_argument("--plan", default=str(ROOT / "data/GM4E01/plan.tsv"))
    ap.add_argument("--sources", action="append", required=True)
    ap.add_argument("--notes")
    ap.add_argument("-o", "--output")
    args = ap.parse_args()

    plan = gcdol.Plan(args.plan)
    symbols = {a: (n, lib, obj, c) for a, _, n, lib, obj, c in gcdol.read_symbols(args.symbols)}
    functions = gcdol.cfg_functions(json.load(open(args.cfg)))
    starts = [f[0] for f in functions]
    notes = {}
    if args.notes:
        for line in open(args.notes, encoding="utf-8"):
            if "\t" in line and not line.startswith("#"):
                k, v = line.rstrip("\n").split("\t", 1)
                notes[k] = v

    bound = {}
    problems = []
    for a, entry, fname, tree in gcdol.native_tables(args.sources):
        where = f"{tree}/{fname}"
        kind, lib = plan.decide(a)
        sym = symbols.get(a)
        if a in bound:
            problems.append(f"{a:08X} {entry} ({where}): also bound in {bound[a][1]} as {bound[a][0]}")
        bound[a] = (entry, where)
        if a not in starts:
            i = bisect.bisect_right(starts, a) - 1
            inside = symbols.get(starts[i], ("?",))[0] if i >= 0 and a < functions[i][1] else "no function"
            problems.append(f"{a:08X} {entry} ({where}): not a function start (inside {inside})")
        elif kind != "replace":
            problems.append(f"{a:08X} {entry} ({where}): in a {kind} range ({lib}): rt::provide is fatal")
        elif sym is None:
            problems.append(f"{a:08X} {entry} ({where}): no name in symbols.txt to compare")
        elif base_name(sym[0]) != base_name(entry) and sym[0] != entry:
            problems.append(f"{a:08X} {entry} ({where}): symbols.txt says {sym[0]} (confidence {sym[3]})")

    # replaced functions lifted code calls or points at
    needed = {}
    for start, _, calls in functions:
        if plan.decide(start)[0] != "lift":
            continue
        for c in calls:
            if plan.decide(c)[0] == "replace":
                needed.setdefault(c, set()).add(start)
    dol = gcdol.Dol(args.dol)
    start_set = set(starts)
    pointed = {}
    internal = ("dolphin/os", "dolphin/vi", "dolphin/dvd", "dolphin/card", "dolphin/exi", "dolphin/si", "dolphin/pad",
                "dolphin/ar", "dolphin/eth", "dolphin/db", "dolphin/base", ".init")
    for a, w in dol.data_words():
        if w in start_set and plan.decide(w)[0] == "replace" and not plan.decide(w)[1].startswith(internal):
            pointed.setdefault(w, []).append(a)
    for start, end, _ in functions:
        if plan.decide(start)[0] != "lift":
            continue
        high = {}
        for a in range(start, end, 4):
            w = dol.u32(a) or 0
            op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 15 and ra == 0:
                high[rd] = imm << 16
            elif op == 14 and ra in high:
                v = (high[ra] + imm - (0x10000 if imm & 0x8000 else 0)) & 0xFFFFFFFF
                if v in start_set and plan.decide(v)[0] == "replace":
                    pointed.setdefault(v, []).append(a)

    def name(a):
        return symbols.get(a, ("?",))[0]

    out = []
    out.append(f"# Native replacements: {len(bound)} table entries in {', '.join(args.sources)}")
    out.append(f"# Table entries with a problem: {len(problems)}")
    out += ["#   " + p for p in problems]
    have = [a for a in needed if a in bound]
    miss = sorted(a for a in needed if a not in bound)
    out.append(f"# Replaced functions lifted code calls: {len(needed)}; bound {len(have)}, missing {len(miss)}")
    ptr_miss = sorted(a for a in pointed if a not in bound and a not in needed)
    out.append(f"# Replaced functions only pointed at (data or lis/addi in lifted code): "
               f"{len([a for a in pointed if a not in needed])}; missing {len(ptr_miss)}")
    out.append("#")
    out.append("# MISSING (address name [library]; what it does; callers)")
    for a in miss + ptr_miss:
        callers = sorted(needed.get(a, ()))
        out.append(f"{a:08X} {name(a)} [{plan.decide(a)[1]}]")
        if name(a) in notes:
            out.append(f"    does: {notes[name(a)]}")
        if callers:
            out.append("    called from: " + ", ".join(f"{c:08X} {name(c)}" for c in callers[:8])
                       + (f" and {len(callers) - 8} more" if len(callers) > 8 else ""))
        if a in pointed:
            out.append("    pointed at from: " + " ".join(f"{p:08X}" for p in pointed[a][:6]))
    out.append("#")
    out.append("# BOUND (address name: file)")
    for a in sorted(have + [a for a in pointed if a in bound and a not in needed]):
        out.append(f"{a:08X} {name(a)}: {bound[a][1]}" + ("" if base_name(name(a)) == base_name(bound[a][0]) else f" as {bound[a][0]}"))
    out.append("#")
    out.append("# BOUND BUT NOT CALLED by lifted code directly (internal, by pointer, or unused)")
    for a in sorted(a for a in bound if a not in needed and a not in pointed):
        out.append(f"{a:08X} {bound[a][0]}: {bound[a][1]}")
    text = "\n".join(out) + "\n"
    if args.output:
        tmp = args.output + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        os.replace(tmp, args.output)
        print("\n".join(out[:4 + len(problems)]))
    else:
        sys.stdout.write(text)
    return 1 if problems or miss or ptr_miss else 0


if __name__ == "__main__":
    sys.exit(main())
