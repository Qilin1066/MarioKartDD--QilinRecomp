#!/usr/bin/env python3
"""Carry the function names of the disc's linker map over to the retail main.dol.

The disc ships `files/debugInfoS.MAP`, the map of the *debug* build: every
function's name, size, object file and library, in the debug link order. The
retail DOL was built from the same sources, but:

  * `Dolphin.a` (SDK, MSL, runtime, MetroTRK) is the same archive: its
    functions have the same sizes in both, bar a handful.
  * JSystem and the game were compiled for release: most sizes differ, inline
    (weak) functions that the debug build emitted are mostly gone, and the
    debugging tools (JHostIO, JAH*/JAW* viewers) are stripped.
  * The link order differs: the retail build links the game's libraries
    alphabetically (Bando, Inagaki, Kameda, Kaneshige, Kawano, Osako, Sato,
    Shiraiwa, Yamamoto) and JSystem's sub-libraries in another order, and a
    few objects moved inside their library. Functions keep their order inside
    an object.

So the port works per object:

  1. Anchors: runs of three functions whose sizes are equal in both builds and
     occur once in each. They say where an object is in the retail text. An
     object with fewer than two agreeing anchors counts only if its place does
     not fall inside another object's.
  2. Object order: anchored objects sorted by retail position; an object with
     no anchor stays with its neighbour in the debug order (the side on which
     it aligns better, where the two neighbours are apart in the retail DOL).
  3. Alignment: between consecutive anchors, a Needleman-Wunsch alignment of
     the debug functions (in retail object order) with the retail functions
     (`qilin cfg`). A pair scores by size (equal sizes most; `Dolphin.a` only
     ever by equal size), `__sinit_*` functions pair only with functions the
     .ctors table points at, skipping a weak debug function is nearly free
     (the release build inlined it), skipping anything else costs.
  4. Call graph: the map's link tree says who first referenced each symbol.
     If a function's referrer is matched, the function should be among the
     retail referrer's call targets (or the addresses it builds); the
     alignment is repeated with that as a bonus, and as a penalty elsewhere.
     Unanchored objects left with under a third of their functions matched
     and no confirmation from the call graph are judged absent (the stripped
     debugging tools) and taken out, and the alignment is repeated.
  5. Weak functions the release build emitted in another object: leftover
     retail functions are compared with every object's unmatched weak
     functions, and named only on equal sizes or the call graph's word.
  6. Confidence per name (the last column):
       A  equal size, and the neighbours in the object or the call graph agree
       B  equal size alone, or unequal size confirmed by the call graph
       C  placed by order between matched neighbours; or equal size with a
          doubt (the call graph contradicts, or a skipped debug function of
          the same size is next to it and nothing says which survived)
       D  a guess: nothing but order and a rough size
     A and B are reliable enough to act on; confirm C and D by reading code.

With --pal, the PAL retail symbol list of the doldecomp/mkdd decompilation
(third_party/mkdd, CC0) is used as an independent check: a name present in
both must sit at a constant distance from its PAL address within a
translation unit. The disagreements are counted per confidence class, and
with --pal-fix a name that disagrees is demoted to D. With --pal-slices, the
PAL build's order of translation units places an unanchored object where the
debug order cannot (step 2).

  port_symbols.py --map <disc>/files/debugInfoS.MAP --dol <disc>/sys/main.dol --qilin <qilin>
                  [-o symbols.txt] [--pal third_party/mkdd/Release_eu/symbols.yml]
                  [--pal-slices third_party/mkdd/Release_eu/dol_slices.yml] [--pal-fix]

The output is derived from the game: keep it out of git.
"""
import argparse
import bisect
import collections
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcdol  # noqa: E402

K = 3                 # anchor: this many consecutive equal sizes
ANCHOR_BYTES = 0x100  # ... adding up to at least this
SKIP_RETAIL = 3.0
SDK = "Dolphin.a"


class Port:
    def __init__(self, link_map, dol, cfg, log=print, hints=None):
        self.log = log
        self.hints = hints or {}  # object name without extension -> PAL text address
        self.map = link_map
        self.dol = dol
        self.D = link_map.functions()
        self.R = gcdol.cfg_functions(cfg)
        self.rs = [e - s for s, e, _ in self.R]
        self.rl = [math.log2(x) for x in self.rs]
        self.rindex = {s: j for j, (s, _, _) in enumerate(self.R)}
        self.rcalls = [set(self.rindex[c] for c in calls if c in self.rindex) for _, _, calls in self.R]
        # The link tree records references, not calls: add the functions whose address a
        # function builds (lis, then addi or ori on the same register: callbacks).
        for j, (start, end, _) in enumerate(self.R):
            high = {}
            for a in range(start, end, 4):
                w = dol.u32(a) or 0
                op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
                if op == 15 and ra == 0:
                    high[rd] = imm << 16
                elif op == 14 and ra in high:
                    t = self.rindex.get((high[ra] + imm - (0x10000 if imm & 0x8000 else 0)) & 0xFFFFFFFF)
                    if t is not None:
                        self.rcalls[j].add(t)
                elif op == 24 and rd in high:
                    t = self.rindex.get(high[rd] | imm)
                    if t is not None:
                        self.rcalls[j].add(t)
        self.ctors = self.find_ctors()
        self.rct = [s in self.ctors for s, _, _ in self.R]
        self.objects = []  # [lib, obj, first, end) in debug order
        for i, d in enumerate(self.D):
            if not self.objects or self.objects[-1][:2] != [d.lib, d.obj] or d.section != self.D[i - 1].section:
                self.objects.append([d.lib, d.obj, i, i])
            self.objects[-1][3] = i + 1
        self.match = {}
        self.fixed = {}
        self.order = []
        # the link tree, by function index
        key_index = {}
        for i, d in enumerate(self.D):
            key_index.setdefault((d.name, d.lib, d.obj), i)
        self.parent = {}
        self.children = collections.defaultdict(list)
        for p, kids in link_map.tree.items():
            pi = key_index.get(p)
            if pi is None:
                continue
            for k in kids:
                ki = key_index.get(k)
                if ki is not None and ki != pi:
                    self.parent[ki] = pi
                    self.children[pi].append(ki)

    @staticmethod
    def stem(obj):
        return obj.rsplit(".", 1)[0]

    def find_ctors(self):
        """The .ctors table: the first data section after the text whose words all point at function starts."""
        text_end = max(a + s for a, s, _, t, _ in self.dol.sections if t)
        for addr, size, _, text, _ in self.dol.sections:
            if text or addr < text_end:
                continue
            words = [self.dol.u32(addr + o) for o in range(0, size, 4)]
            while words and words[-1] == 0:
                words.pop()
            if words and all(w in self.rindex for w in words):
                return set(words)
            break
        raise SystemExit("no .ctors table after the text")

    # ---- 1. anchors -------------------------------------------------------------------------

    def anchors(self):
        retail = collections.defaultdict(list)
        for j in range(len(self.rs) - K + 1):
            retail[tuple(self.rs[j:j + K])].append(j)
        debug = collections.Counter()
        for _, _, a, b in self.objects:
            for i in range(a, b - K + 1):
                debug[tuple(x.size for x in self.D[i:i + K])] += 1
        placed = []
        for n, (_, _, a, b) in enumerate(self.objects):
            found = []
            for i in range(a, b - K + 1):
                t = tuple(x.size for x in self.D[i:i + K])
                # (weak functions are emitted wherever they are first used: they place nothing)
                if (sum(t) >= ANCHOR_BYTES and debug[t] == 1 and len(retail.get(t, ())) == 1
                        and sum(x.binding != "weak" for x in self.D[i:i + K]) >= 2):
                    found.append((i, retail[t][0]))
            placed.append(self.chain(found))
        def drop_islands(least):
            # Libraries moved whole, so a library's objects are one run in the retail order.
            # An object away from its library's main run is a coincidence (two objects with
            # the same run of sizes swap places otherwise).
            while True:
                have = sorted((c[0][1], n) for n, c in enumerate(placed) if len(c) >= least)
                runs = []
                for _, n in have:
                    key = (self.objects[n][0], self.D[self.objects[n][2]].section)
                    if runs and runs[-1][0] == key:
                        runs[-1][1].append(n)
                    else:
                        runs.append((key, [n]))
                main = {}
                for key, members in runs:
                    weight = sum(len(placed[n]) for n in members)
                    if weight > main.get(key, (0, None))[0]:
                        main[key] = (weight, members)
                island = [n for key, members in runs if members is not main[key][1] for n in members]
                if not island:
                    return
                for n in island:
                    placed[n] = []

        drop_islands(2)
        # A lone anchor may be chance. It counts if a nearby object of its library (in the debug
        # order) is anchored a matching distance away in the retail DOL; otherwise only between
        # two well-anchored objects that leave room for it: not inside one's extent, not between
        # two that are consecutive in the debug order, and next to its own library.
        supported = set()
        have = [n for n, c in enumerate(placed) if c]
        for x, n in enumerate(have):
            for m in have[x + 1:x + 4]:
                if self.objects[m][0] != self.objects[n][0]:
                    break
                dd = placed[m][0][0] - placed[n][-1][0]
                dr = placed[m][0][1] - placed[n][-1][1]
                if 0 < dr <= dd + 2:
                    supported.update((n, m))
        strong = sorted((c[0][1], c[-1][1] + K - 1, n) for n, c in enumerate(placed)
                        if len(c) >= 2 or (c and n in supported))
        starts = [s[0] for s in strong]
        for n, c in enumerate(placed):
            if len(c) != 1 or n in supported:
                continue
            at = bisect.bisect_right(starts, c[0][1])
            before = strong[at - 1] if at else None
            after = strong[at] if at < len(strong) else None
            libs = {self.objects[x[2]][0] for x in (before, after) if x}
            if ((before and c[0][1] <= before[1]) or (before and after and after[2] == before[2] + 1)
                    or self.objects[n][0] not in libs):
                placed[n] = []
        drop_islands(1)
        # strong objects must not interleave either: the weaker of two overlapping ones loses
        strong = sorted((c[0][1], c[-1][1], n) for n, c in enumerate(placed) if c)
        for (lo1, hi1, n1), (lo2, hi2, n2) in zip(strong, strong[1:]):
            if lo2 <= hi1 and placed[n1] and placed[n2]:
                loser = n1 if len(placed[n1]) < len(placed[n2]) else n2
                placed[loser] = []
        return placed

    @staticmethod
    def chain(found):
        """The longest run of anchors whose retail positions rise with the debug ones, each step no
        longer than the debug step plus a little (functions vanish, they are not added)."""
        if not found:
            return []
        best = [1] * len(found)
        prev = [-1] * len(found)
        for i in range(len(found)):
            for j in range(i):
                dd, dr = found[i][0] - found[j][0], found[i][1] - found[j][1]
                fits = dr == dd if dd < K else K <= dr <= dd + 2  # overlapping anchors must agree
                if fits and best[j] + 1 > best[i]:
                    best[i], prev[i] = best[j] + 1, j
        i = max(range(len(found)), key=lambda x: best[x])
        out = []
        while i >= 0:
            out.append(found[i])
            i = prev[i]
        return out[::-1]

    # ---- 2. object order --------------------------------------------------------------------

    def place_objects(self, starts=None):
        """starts: {object: retail address of its first function} from an earlier alignment."""
        placed = self.placed if starts else self.anchors()
        self.placed = placed
        starts = starts or {}
        # Pinned objects: the anchored ones by retail position, and (with --pal-slices) objects
        # with no anchor that the PAL build's translation-unit list places after one of them.
        key = {n: (c[0][1], 0, 0) for n, c in enumerate(placed) if c}
        base = collections.Counter(self.stem(o[1]) for o in self.objects)
        hint = {n: self.hints[self.stem(o[1])] for n, o in enumerate(self.objects)
                if base[self.stem(o[1])] == 1 and self.stem(o[1]) in self.hints}
        # An unanchored object's place: its PAL address, moved by the distance of the nearest
        # anchored object that both lists have (found by a first alignment without the list).
        # The two builds differ by a few instructions here and there, so this is good for the
        # order of the objects, not for an address. It must land beside its own library.
        known = sorted((hint[n], n) for n in key if n in hint and n in starts)
        addresses = [r[0] for r in self.R]
        guess = {}
        for n, at in hint.items():
            if n in key or not known or self.objects[n][0] == SDK:  # (the SDK kept its order)
                continue
            k = bisect.bisect_left(known, (at, -1))
            near = min((x for x in (known[k - 1] if k else None, known[k] if k < len(known) else None) if x),
                       key=lambda x: abs(x[0] - at))
            guess[n] = (bisect.bisect_right(addresses, at + starts[near[1]] - near[0]) - 1, 1, 0)
        # Used only where the debug order cannot say: in a run of unanchored objects whose
        # anchored neighbours (in the debug order) are apart in the retail DOL.
        have = sorted((v, n) for n, v in key.items())
        after_of = {a[1]: b[1] for a, b in zip(have, have[1:])}
        anchored_ids = sorted(key)
        for n in list(guess):
            k = bisect.bisect_left(anchored_ids, n)
            before = anchored_ids[k - 1] if k else None
            after = anchored_ids[k] if k < len(anchored_ids) else None
            if before is None or after is None or after_of.get(before) == after:
                del guess[n]
        both = sorted([(v, n) for n, v in key.items()] + [(v, n) for n, v in guess.items()])
        for x, (v, n) in enumerate(both):
            if n in guess:
                sides = [self.objects[both[y][1]][0] for y in (x - 1, x + 1) if 0 <= y < len(both)]
                if self.objects[n][0] in sides:
                    key[n] = v
        pinned = sorted((v, n) for n, v in key.items())
        retail_next = {a[1]: b[1] for a, b in zip(pinned, pinned[1:])}
        retail_prev = {b[1]: a[1] for a, b in zip(pinned, pinned[1:])}
        follows = collections.defaultdict(list)   # pinned object -> loose ones after it
        precedes = collections.defaultdict(list)  # pinned object -> loose ones before it
        runs = []
        n = 0
        last = None
        while n < len(self.objects):
            if n in key:
                last = n
                n += 1
                continue
            m = n
            while m < len(self.objects) and m not in key:
                m += 1
            runs.append((last, list(range(n, m)), m if m < len(self.objects) else None))
            n = m

        def sequence(objs):
            return [i for o in objs for i in range(self.objects[o][2], self.objects[o][3])]

        def tail(o):  # debug functions of an anchored object after its last anchor
            return list(range(placed[o][-1][0] + K, self.objects[o][3]))

        def head(o):
            return list(range(self.objects[o][2], placed[o][0][0]))

        for before, run, after in runs:
            if before is None:
                precedes[after] = run
            elif after is None or retail_next.get(before) == after:
                follows[before] = run
            else:
                # the neighbours are apart in the retail DOL: split the run where both ends align
                # best, each part staying with its own library
                nb, pa = retail_next.get(before), retail_prev.get(after)
                lb, la = self.objects[before][0], self.objects[after][0]
                if lb == la:
                    splits = range(len(run) + 1)
                else:  # what is not of the later library stays behind the earlier one
                    splits = [max([x + 1 for x, o in enumerate(run) if self.objects[o][0] != la], default=0)]
                best = None
                names = [self.objects[o][1] for o in [before] + run + [after]]
                for s in splits:
                    # files of one module share a prefix (JKRAram, JKRAramHeap): prefer not to cut there
                    score = -3.0 * len(os.path.commonprefix([names[s], names[s + 1]]))
                    if nb is not None and placed[before] and placed[nb]:
                        score += self.align(tail(before) + sequence(run[:s]) + head(nb),
                                            placed[before][-1][1] + K, placed[nb][0][1], score_only=True)
                    if pa is not None and placed[pa] and placed[after]:
                        score += self.align(tail(pa) + sequence(run[s:]) + head(after),
                                            placed[pa][-1][1] + K, placed[after][0][1], score_only=True)
                    if best is None or score > best[0]:
                        best = (score, s)
                s = best[1]
                follows[before] = run[:s]
                precedes[after] = run[s:]
        anchored = pinned
        order = []
        for _, n in anchored:
            order += precedes[n] + [n] + follows[n]
        self.object_order = order
        self.order = sequence([o for o in order if o not in self.absent])
        self.fixed = {}
        for (_, _, a, b), c in zip(self.objects, placed):
            for i, j in c:
                for k in range(K):
                    # The end of an anchor is not certain if the debug function beyond it has the
                    # same size: either of the two may be the one that survived. The alignment
                    # decides (the call graph may know).
                    if k == K - 1 and i + K < b and self.D[i + K].size == self.D[i + k].size:
                        continue
                    if k == 0 and i > a and self.D[i - 1].size == self.D[i].size:
                        continue
                    self.fixed[i + k] = j + k
        moved = sum(1 for a, b in zip(order, order[1:]) if b != a + 1)
        self.log(f"objects: {len(self.objects)}, anchored {len(anchored)}, {moved} breaks in the debug order")

    # ---- 3. alignment -----------------------------------------------------------------------

    def align(self, dlist, r0, r1, bonus=None, score_only=False):
        """Best alignment of the debug functions dlist with the retail functions r0..r1."""
        D, rs, rl, rct = self.D, self.rs, self.rl, self.rct
        n = max(0, r1 - r0)
        H = [-SKIP_RETAIL * j for j in range(n + 1)]
        dirs = []
        for i in dlist:
            d = D[i]
            sd = d.size
            ld = math.log2(sd)
            sdk = d.lib == SDK
            equal = 6.0 if sdk else min(5.0, 1.0 + 0.5 * math.log2(sd / 4))
            skip = 2.0 if sdk else {"weak": 0.2, "local": 1.0}.get(d.binding, 2.0)
            sinit = d.name.startswith("__sinit_")
            bo = bonus.get(i) if bonus else None
            Hn = [H[0] - skip]
            dr = bytearray(n + 1)
            left = Hn[0]
            for j in range(1, n + 1):
                rj = r0 + j - 1
                if rs[rj] == sd:
                    m = equal
                elif sdk:
                    m = -8.0
                else:
                    m = 0.5 - 3.0 * abs(rl[rj] - ld)
                    if m < -4.0:
                        m = -4.0
                if rct[rj]:
                    m += 8.0 if sinit else -10.0
                elif sinit:
                    m -= 10.0
                if bo:
                    m += bo.get(rj, bo.get(-1, 0.0))
                best = H[j - 1] + m
                c = 1
                u = H[j] - skip
                if u > best:
                    best, c = u, 0
                lft = left - SKIP_RETAIL
                if lft > best:
                    best, c = lft, 2
                Hn.append(best)
                dr[j] = c
                left = best
            H = Hn
            dirs.append(dr)
        if score_only:
            return H[n]
        out = {}
        i, j = len(dlist) - 1, n
        while i >= 0:
            c = dirs[i][j]
            if c == 2:
                j -= 1
            elif c == 1:
                out[dlist[i]] = r0 + j - 1
                i -= 1
                j -= 1
            else:
                i -= 1
        return out

    def align_all(self, bonus=None):
        match = {}
        seg = []
        r0 = 0
        for i in self.order:
            if i in self.fixed:
                j = self.fixed[i]
                if seg or j > r0:
                    match.update(self.align(seg, r0, j, bonus))
                match[i] = j
                seg = []
                r0 = j + 1
            else:
                seg.append(i)
        if seg:
            match.update(self.align(seg, r0, len(self.R), bonus))
        if len(set(match.values())) != len(match):
            raise SystemExit("two names for one function: the anchors are inconsistent")
        self.match = match
        return match

    # ---- 4. call graph ----------------------------------------------------------------------

    def call_bonus(self):
        bonus = collections.defaultdict(dict)
        callers = collections.defaultdict(set)
        for j, calls in enumerate(self.rcalls):
            for c in calls:
                callers[c].add(j)
        for i in range(len(self.D)):
            p = self.parent.get(i)
            b = bonus[i]
            if p in self.match:  # the referrer's retail calls are where this one should be
                for rj in self.rcalls[self.match[p]]:
                    b[rj] = b.get(rj, 0.0) + 3.0
                # ... and nowhere else, unless the release build could have inlined the call
                # (same object, or a weak function). Key -1: every other retail function.
                if self.D[i].binding != "weak" and self.D[p].obj != self.D[i].obj and self.rcalls[self.match[p]]:
                    b[-1] = -3.0
            for k in self.children.get(i, ()):  # whoever calls a child's match may be this one
                if k in self.match:
                    for rj in callers.get(self.match[k], ()):
                        b[rj] = min(6.0, b.get(rj, 0.0) + 2.0)
        return bonus

    def prune(self):
        """Take out objects that are probably not in the retail DOL at all.

        The debugging tools were stripped (hundreds of functions in a row), and among so many
        candidates a few always fit the leftover retail functions by size. An unanchored
        object of four functions or more stays only if a third of its non-weak functions
        are matched or the call graph confirms one of them. Returns the number taken out; the caller aligns again."""
        gone = 0
        for n, (_, _, a, b) in enumerate(self.objects):
            if self.placed[n] or n in self.absent:
                continue
            matched = [i for i in range(a, b) if i in self.match]
            if not matched:
                continue
            solid = [i for i in range(a, b) if self.D[i].binding != "weak"] or list(range(a, b))
            if (self.objects[n][0] == SDK or b - a < 4
                    or 3 * sum(1 for i in solid if i in self.match) >= len(solid)
                    or any(self.support(i)[0] for i in matched)):
                continue
            self.absent.add(n)
            gone += 1
        if gone:
            self.order = [i for o in self.object_order if o not in self.absent
                          for i in range(self.objects[o][2], self.objects[o][3])]
        return gone

    def support(self, i):
        """(agreeing, contradicting) link-tree edges of a matched function."""
        j = self.match[i]
        yes = no = 0
        p = self.parent.get(i)
        if p in self.match:
            if j in self.rcalls[self.match[p]]:
                yes += 1
            elif self.D[i].binding != "weak" and self.D[p].obj != self.D[i].obj:
                no += 1  # (inside one object the release build may have inlined the call)
        for k in self.children.get(i, ()):
            if k in self.match and self.match[k] in self.rcalls[j]:
                yes += 1
        return yes, no

    # ---- 5. confidence ----------------------------------------------------------------------

    def confidence(self):
        pos = {i: n for n, i in enumerate(self.order)}
        taken = {j: i for i, j in self.match.items()}
        out = {}
        for i, j in self.match.items():
            d = self.D[i]
            equal = d.size == self.rs[j]
            yes, no = self.support(i)
            if i in self.adopted:
                out[i] = "B" if (equal and d.size >= 0x20) or (yes and not no) else "C"
                continue
            # sandwiched: the retail neighbours are matched to this one's neighbours in the object
            n = pos[i]
            near = 0
            for step in (-1, 1):
                o = taken.get(j + step)
                if o is not None and self.D[o].obj == d.obj and 0 < (pos[o] - n) * step <= 3:
                    near += 1 + (self.D[o].size == self.rs[j + step])
            # A skipped debug function of the same size next to this one could be the one that
            # survived instead (ARGetBaseAddress and ARGetSize, 8 bytes each, one stripped).
            twin = False
            for step in (-1, 1):
                k = n + step
                while 0 <= k < len(self.order) and self.order[k] not in self.match:
                    twin |= self.D[self.order[k]].size == d.size and self.D[self.order[k]].obj == d.obj
                    k += step
            if no or (twin and not yes):
                c = "C" if equal else "D"
            elif d.name.startswith("__sinit_") and self.rct[j]:
                c = "A" if near >= 2 or equal else "B"
            elif equal:
                big = d.size >= 0x20
                if d.lib == SDK:
                    c = "A" if near >= 2 or yes else "B"
                elif big:
                    c = "A" if near >= 2 or (yes and not no) else "B"
                elif yes and not no and near >= 2:
                    c = "A"
                else:  # a run of tiny functions of one size can slip by one unnoticed
                    c = "B" if (yes and not no) or near == 4 else "C"
            else:
                ratio = self.rs[j] / d.size
                if yes >= 1 and not no and 0.3 <= ratio <= 1.6:
                    c = "B"
                elif near >= 2 and 0.3 <= ratio <= 1.6 and not no:
                    c = "C"
                else:
                    c = "D"
            out[i] = c
        return out

    # ---- 4b. weak functions emitted elsewhere ----------------------------------------------

    def adopt(self):
        """Name retail functions the alignment left over with weak debug functions of other objects.

        A weak (inline, template) function is kept once, in the first object that emitted it,
        and that object need not be the same in the two builds. A run of leftover retail
        functions is compared with every object's unmatched weak functions, in order; a pair
        needs equal sizes or the call graph's word. The object that explains the run best
        (clearly better than the next) gives the names. They are listed under the object
        they are in here, not the debug build's."""
        self.adopted = {}
        for _ in range(4):
            bonus = self.call_bonus()
            taken = set(self.match.values())
            runs = []
            for j in range(len(self.R)):
                if j in taken:
                    continue
                if runs and runs[-1][1] == j:
                    runs[-1][1] = j + 1
                else:
                    runs.append([j, j + 1])
            free = []
            for n, (_, _, a, b) in enumerate(self.objects):
                # (an object judged absent may still have left a function or two: all of its count)
                lst = [i for i in range(a, b)
                       if i not in self.match and (self.D[i].binding == "weak" or n in self.absent)]
                if lst:
                    free.append(lst)
            changed = False
            owner = {j: i for i, j in self.match.items()}
            for r0, r1 in runs:
                # not in the SDK's part of the text (no weak functions there), nor in .init
                sides = [self.D[owner[j]].lib for j in (r0 - 1, r1) if j in owner]
                if not sides or all(lib == SDK for lib in sides):
                    continue
                # a game library's inline functions are not in JSystem's objects, nor in another's
                here = set(sides)
                scored = sorted(((self.align_weak(lst, r0, r1, bonus), n) for n, lst in enumerate(free)
                                 if lst and (self.D[lst[0]].lib in here or (self.D[lst[0]].lib == "JSystemV.a"
                                                                   and SDK not in here))),
                                key=lambda x: -x[0][0])
                if not scored or scored[0][0][0] < 4.0:
                    continue
                if len(scored) > 1 and scored[0][0][0] < scored[1][0][0] + 2.0:
                    continue
                pairs = scored[0][0][1]
                for i, j in pairs.items():
                    self.match[i] = j
                    self.adopted[i] = self.D[i].binding == "weak"
                free[scored[0][1]] = [i for i in free[scored[0][1]] if i not in pairs]
                changed = True
            if not changed:
                break

    def align_weak(self, dlist, r0, r1, bonus):
        """(score, {debug: retail}) of the best in-order pairing; only evidenced pairs count."""
        n = r1 - r0
        H = [0.0] * (n + 1)
        dirs = []
        for i in dlist:
            sd = self.D[i].size
            bo = bonus.get(i, {})
            Hn = [0.0]
            dr = bytearray(n + 1)
            for j in range(1, n + 1):
                rj = r0 + j - 1
                called = bo.get(rj, 0.0)
                if self.rs[rj] == sd:
                    m = (2.0 + 0.5 * math.log2(sd / 4) if sd >= 0x20 else 0.1) + called
                elif called >= 3.0 and 0.4 <= self.rs[rj] / sd <= 1.5:
                    m = 1.0
                else:
                    m = -100.0
                if self.rct[rj]:
                    m = -100.0
                best, c = H[j - 1] + m, 1
                if H[j] > best:
                    best, c = H[j], 0
                if Hn[j - 1] > best:
                    best, c = Hn[j - 1], 2
                Hn.append(best)
                dr[j] = c
            H = Hn
            dirs.append(dr)
        out = {}
        i, j = len(dlist) - 1, n
        while i >= 0 and j > 0:
            c = dirs[i][j]
            if c == 2:
                j -= 1
            elif c == 1:
                out[dlist[i]] = r0 + j - 1
                i -= 1
                j -= 1
            else:
                i -= 1
        return H[n], out

    def location(self, i):
        """(library, object) a named function is in, in the retail DOL."""
        if not self.adopted.get(i):
            return self.D[i].lib, self.D[i].obj
        if not hasattr(self, "_homes"):
            self._homes = sorted((j, k) for k, j in self.match.items() if not self.adopted.get(k))
        at = bisect.bisect_left(self._homes, (self.match[i], -1))
        k = self._homes[at - 1][1] if at else self._homes[0][1]
        return self.D[k].lib, self.D[k].obj

    def run(self):
        self.absent = set()
        self.place_objects()
        self.align_all()
        if self.hints:
            first = {}
            index = {}
            for n, (_, _, a, b) in enumerate(self.objects):
                for i in range(a, b):
                    index[i] = n
            for i, j in self.match.items():
                n = index[i]
                if self.placed[n] and self.D[i].binding != "weak":
                    first[n] = min(first.get(n, 1 << 32), self.R[j][0])
            self.place_objects(first)
            self.align_all()
        for n in range(8):
            self.align_all(self.call_bonus())
            if n and not self.prune():
                break
        else:
            self.align_all(self.call_bonus())
        self.adopt()
        return self.confidence()


def pal_symbols(path):
    """name -> [PAL addresses] of the decomp's manual PAL list (text only)."""
    out = collections.defaultdict(list)
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s+0x(80[0-2][0-9a-fA-F]{5}):\s*(\S+)", line)
        if m:
            out[m.group(2)].append(int(m.group(1), 16))
    return out


def pal_slices(path):
    """Object name (no directory, no extension) -> PAL text start, from the decomp's dol_slices.yml.
    Names that occur twice are left out."""
    found = collections.defaultdict(list)
    unit = None
    for line in open(path, encoding="utf-8"):
        m = re.match(r"^#?([^\s#].*):\s*$", line)
        if m:
            unit = os.path.basename(m.group(1)).rsplit(".", 1)[0]
            continue
        m = re.match(r"\s+\.text:\s*\[0x([0-9a-fA-F]+),", line)
        if m and unit:
            found[unit].append(int(m.group(1), 16))
    return {k: v[0] for k, v in found.items() if len(v) == 1}


def pal_check(port, conf, pal):
    """For names in both lists: is (US address - PAL address) the same as the neighbours'?
    Returns {debug index: True/False}."""
    names = collections.Counter(d.name for d in port.D)
    pts = []
    for i, j in port.match.items():
        n = port.D[i].name
        if names[n] == 1 and len(pal.get(n, ())) == 1:
            pts.append((pal[n][0], port.R[j][0] - pal[n][0], i))
    pts.sort()
    verdict = {}
    for k, (_, delta, i) in enumerate(pts):
        window = [p[1] for p in pts[max(0, k - 6):k + 7]]
        verdict[i] = delta == collections.Counter(window).most_common(1)[0][0]
    return verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--map", required=True, help="<disc>/files/debugInfoS.MAP")
    ap.add_argument("--dol", required=True)
    ap.add_argument("--qilin", help="the qilin executable (or give --cfg)")
    ap.add_argument("--cfg", help="an existing `qilin cfg --json` of the DOL")
    ap.add_argument("-o", "--output", help="symbols.txt to write (atomically)")
    ap.add_argument("--pal", help="third_party/mkdd/Release_eu/symbols.yml: check against the PAL names")
    ap.add_argument("--pal-slices", help="third_party/mkdd/Release_eu/dol_slices.yml: the PAL build's object "
                    "order places objects that no anchor does")
    ap.add_argument("--pal-fix", action="store_true", help="demote names the PAL list contradicts to D")
    ap.add_argument("--min", default="D", choices="ABCD", help="leave out names below this confidence")
    args = ap.parse_args()
    if not args.cfg and not args.qilin:
        ap.error("give --qilin or --cfg")

    dol = gcdol.Dol(args.dol)
    cfg = gcdol.run_qilin(args.qilin, args.dol, "cfg", args.cfg)
    port = Port(gcdol.LinkMap(args.map), dol, cfg, hints=pal_slices(args.pal_slices) if args.pal_slices else None)
    conf = port.run()

    if args.pal:
        verdict = pal_check(port, conf, pal_symbols(args.pal))
        tally = collections.Counter((conf[i], ok) for i, ok in verdict.items())
        print("PAL check (names in both, constant distance to the PAL address among neighbours):")
        for c in "ABCD":
            good, bad = tally[(c, True)], tally[(c, False)]
            if good + bad:
                print(f"  {c}: {good + bad:5d} comparable, {bad:4d} disagree ({100.0 * bad / (good + bad):.1f}%)")
        if args.pal_fix:
            for i, ok in verdict.items():
                if not ok:
                    conf[i] = "D"

    total = collections.Counter(d.lib or "(game)" for d in port.D)
    got = collections.defaultdict(collections.Counter)
    for i, c in conf.items():
        got[port.D[i].lib or "(game)"][c] += 1
    print(f"{'library':14s} {'debug':>6s} {'named':>6s}      A      B      C      D")
    for lib, n in total.most_common():
        g = got[lib]
        print(f"{lib:14s} {n:6d} {sum(g.values()):6d} " + " ".join(f"{g[c]:6d}" for c in "ABCD"))
    named = len(conf)
    print(f"objects judged absent from the retail DOL: {len(port.absent)} "
          f"({sum(port.objects[n][3] - port.objects[n][2] for n in port.absent)} functions)")
    print(f"weak functions found in another object than the debug build's: {len(port.adopted)}")
    print(f"retail functions: {len(port.R)}, named {named} ({100.0 * named / len(port.R):.1f}%)")

    if args.output:
        rows = []
        for i, j in sorted(port.match.items(), key=lambda x: x[1]):
            if conf[i] > args.min:
                continue
            d = port.D[i]
            s, e, _ = port.R[j]
            lib, obj = port.location(i)
            rows.append(f"{s:08X} {e - s:06X} {d.name} {lib or '-'} {obj} {conf[i]}\n")
        tmp = args.output + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            f.write("# GM4E01 rev 0 main.dol: function names ported from the debug build's map by tools/analysis/port_symbols.py\n")
            f.write("# ADDRESS SIZE NAME LIBRARY OBJECT CONFIDENCE (hex address and size of the retail DOL; - for no library)\n")
            f.write("# Confidence: A equal size and neighbours or call graph agree; B equal size alone, or call graph alone;\n")
            f.write("# C placed by order between matched neighbours; D a guess. Confirm C and D by reading the code.\n")
            f.writelines(rows)
        os.replace(tmp, args.output)
        print(f"wrote {len(rows)} names to {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
