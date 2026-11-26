"""Shared helpers for the analysis tools: the DOL, the disc's linker map, qilin's output.

The map (files/debugInfoS.MAP) is the debug build's: names, sizes, objects and
link order, not this DOL's addresses. port_symbols.py carries the names over.
"""
import json
import os
import re
import struct
import subprocess
import tempfile


class Dol:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        self.sections = []  # (address, size, file offset, is_text, index)
        for i in range(18):
            off, addr, size = (struct.unpack_from(">I", d, base + 4 * i)[0] for base in (0, 0x48, 0x90))
            if size:
                self.sections.append((addr, size, off, i < 7, i))
        self.sections.sort()
        self.bss, self.bss_size, self.entry = struct.unpack_from(">III", d, 0xD8)

    def offset(self, a):
        for addr, size, off, _, _ in self.sections:
            if addr <= a < addr + size:
                return off + a - addr
        return None

    def u32(self, a):
        o = self.offset(a)
        return struct.unpack_from(">I", self.data, o)[0] if o is not None and o + 4 <= len(self.data) else None

    def bytes(self, a, n):
        o = self.offset(a)
        return self.data[o:o + n] if o is not None else None

    def cstr(self, a, limit=256):
        o = self.offset(a)
        if o is None:
            return None
        end = self.data.find(b"\0", o, o + limit)
        return self.data[o:end] if end >= 0 else None

    def is_text(self, a):
        return any(t and addr <= a < addr + size for addr, size, _, t, _ in self.sections)

    def data_words(self):
        for addr, size, off, text, _ in self.sections:
            if not text:
                for o in range(0, size - 3, 4):
                    yield addr + o, struct.unpack_from(">I", self.data, off + o)[0]


MAP_LINE = re.compile(r"^\s+([0-9a-f]{8}) ([0-9a-f]{6}) ([0-9a-f]{8})\s+(\d+) (\S+) \t(.*)$")
TREE_LINE = re.compile(r"^\s*(\d+)\] (\S+) \((\w+),(\w+)\) found in (.*)$")


class Symbol:
    __slots__ = ("addr", "size", "name", "lib", "obj", "section", "binding", "index")

    def __init__(self, addr, size, name, lib, obj, section):
        self.addr, self.size, self.name, self.lib, self.obj, self.section = addr, size, name, lib, obj, section
        self.binding = None
        self.index = -1

    def __repr__(self):
        return f"<{self.name} {self.addr:08X}+{self.size:X} {self.lib} {self.obj}>"


def where(text):
    parts = text.strip().split(" ")
    return (parts[0], parts[1]) if len(parts) > 1 and parts[1] else ("", parts[0])


class LinkMap:
    """The CodeWarrior map: the link tree (who first referenced what), the section layouts, the memory map."""

    def __init__(self, path):
        self.sections = {}   # name -> [Symbol] in address order, section-header lines excluded
        self.objects = {}    # section -> [(addr, size, lib, obj)]
        self.memory = {}     # section -> (address, size)
        self.linker = {}     # linker-generated symbol -> address
        self.tree = {}       # (name, lib, obj) -> [(name, lib, obj)] children, in the map's order
        self.kind = {}       # (name, lib, obj) -> (type, binding)
        cur = None
        stack = []
        mode = "tree"
        for raw in open(path, encoding="latin-1"):
            line = raw.rstrip("\r\n")
            m = re.match(r"^(\S+) section layout", line)
            if m:
                mode = "layout"
                cur = m.group(1)
                self.sections.setdefault(cur, [])  # extabindex appears twice (the second is empty)
                self.objects.setdefault(cur, [])
                continue
            if line.startswith("Memory map"):
                mode = "memory"
                continue
            if line.startswith("Linker generated symbols"):
                mode = "linker"
                continue
            if mode == "tree":
                m = TREE_LINE.match(line)
                if not m:
                    continue
                depth, name, typ, binding = int(m.group(1)), m.group(2), m.group(3), m.group(4)
                key = (name,) + where(m.group(5))
                self.kind[key] = (typ, binding)
                del stack[depth - 1:]
                if stack:
                    self.tree.setdefault(stack[-1], []).append(key)
                stack.append(key)
            elif mode == "layout":
                m = MAP_LINE.match(line)
                if not m:
                    continue
                _, size, addr, align, name, w = m.groups()
                lib, obj = where(w)
                if name == cur or (name.startswith(".") and align == "1"):
                    self.objects[cur].append((int(addr, 16), int(size, 16), lib, obj))
                else:
                    self.sections[cur].append(Symbol(int(addr, 16), int(size, 16), name, lib, obj, cur))
            elif mode == "memory":
                p = line.split()
                if len(p) == 4 and re.fullmatch(r"[0-9a-f]{8}", p[1]):
                    self.memory[p[0]] = (int(p[1], 16), int(p[2], 16))
            elif mode == "linker":
                p = line.split()
                if len(p) == 2:
                    self.linker[p[0]] = int(p[1], 16)
        for syms in self.sections.values():
            for s in syms:
                k = self.kind.get((s.name, s.lib, s.obj))
                s.binding = k[1] if k else None

    def functions(self):
        """Functions of .init and .text in link order (zero-sized entry labels left out)."""
        out = [s for sec in (".init", ".text") for s in self.sections.get(sec, []) if s.size]
        for i, s in enumerate(out):
            s.index = i
        return out


def run_qilin(qilin, dol_path, what, cache=None):
    """`qilin cfg --json` (parsed) or `qilin insns` (text). cache: a file to keep it in."""
    path = cache
    tmp = None
    if not path or not os.path.exists(path):
        if not path:
            fd, tmp = tempfile.mkstemp(suffix=".json" if what == "cfg" else ".txt")
            os.close(fd)
            path = tmp
        args = [qilin, what, dol_path, "--json" if what == "cfg" else "-o", path]
        subprocess.run(args, check=True, stdout=subprocess.DEVNULL)
    try:
        if what == "cfg":
            return json.load(open(path))
        return open(path, encoding="utf-8").read()
    finally:
        if tmp:
            os.unlink(tmp)


def cfg_functions(cfg):
    """[(start, end, [call targets in order])] sorted by address."""
    return sorted((int(f["start"], 16), int(f["end"], 16), [int(c, 16) for c in f.get("calls", [])])
                  for f in cfg["functions"])


def parse_insns(text):
    """address -> (word, mnemonic, text) from `qilin insns`."""
    out = {}
    for line in text.splitlines():
        p = line.split(" ", 2)
        if len(p) == 3:
            out[int(p[0], 16)] = (int(p[1], 16), p[2].split(" ", 1)[0], p[2])
    return out


def small_data_bases(dol):
    """(r1, r2, r13) set by __init_registers: the lis/ori pairs before its blr, found from the entry."""
    a = dol.entry
    w = dol.u32(a)
    if w >> 26 != 18 or not w & 1:
        raise SystemExit("__start does not begin with bl __init_registers")
    off = w & 0x03FFFFFC
    a = (a + (off - 0x4000000 if off & 0x2000000 else off)) & 0xFFFFFFFF
    regs = {}
    while dol.u32(a) != 0x4E800020:
        w = dol.u32(a)
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        if op == 15 and ra == 0:
            regs[rd] = imm << 16
        elif op == 24 and rd in regs:  # ori rA,rS,imm: rS is in the rd field
            regs[ra] = regs[rd] | imm
        a += 4
    return regs[1], regs[2], regs[13]


class Plan:
    def __init__(self, path):
        self.rows = []
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            start, end, kind, library = line.rstrip("\n").split("\t")
            self.rows.append((int(start, 16), int(end, 16), kind, library))

    def decide(self, address):
        """(kind, library) of the narrowest range holding the address, as qilin lift does."""
        best = None
        for start, end, kind, library in self.rows:
            if start <= address < end and (best is None or end - start < best[1] - best[0]):
                best = (start, end, kind, library)
        return (best[2], best[3]) if best else ("lift", "")


def read_symbols(path):
    """work/symbols.txt -> [(address, size, name, library, object, confidence)]."""
    out = []
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        p = line.split()
        out.append((int(p[0], 16), int(p[1], 16), p[2], p[3], p[4], p[5] if len(p) > 5 else "A"))
    return out
