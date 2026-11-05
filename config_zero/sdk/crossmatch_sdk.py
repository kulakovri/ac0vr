#!/usr/bin/env python3
"""Transfer Ace Combat 5's PS2 SDK symbol table onto Ace Combat Zero.

We do not have Sony's EE libraries, so tools/identify_sdk.py cannot be run
against SLUS_213.46.  AC5 (SLUS_208.51) is already labelled, though
(config/sdk_symbols.json + config/manual_symbols.json), and both games link
Sony's libraries -- AC5 links SDK 2.8.0, Zero links 3.0.0 / 3.0.2 (see the
"PsIIlibxxxx NNNN" version strings in both ELFs).  Most library functions
are identical modulo relocations between those releases; some changed.

Pipeline (each AC5 symbol is mapped to at most one Zero address):

  exact      AC5 body == Zero body after masking relocatable fields:
             jal/j targets, the lui/addiu/ori/load/store halves of absolute
             addresses (tracked through a small register-taint pass),
             gp-relative offsets and branches that leave the function.
             Short functions with several identical copies in Zero are
             disambiguated by the displacement of matched neighbours.
  near       same length, at most max(2, n/10) words differ.
  fuzzy      different body; difflib ratio of masked instruction streams
             (branch offsets masked too), 3-gram pre-filtered, unique best.
  callgraph  a matched pair's aligned call sites / function-pointer
             references map callee to callee; accepted when the callee's
             body is similar too.
  vu0sig     libvu0 was rebuilt more compactly in SDK 3.0; compare only the
             COP2 (VU0 macro) instruction sequence.
  layout     SDK objects are linked contiguously: an unmatched symbol between
             two matched neighbours is looked for among the function starts
             between their Zero counterparts, in order.

Every match is then checked: each SDK callee / function pointer of the AC5
function must map to the corresponding target in Zero (calls that now go
through a new SDK 3.0 wrapper which itself calls the expected function are
counted separately), the match's displacement is compared with its
neighbours', and config/rpc_sids.json (SIF RPC server ids / function numbers
used by AC5's SDK functions) is checked against the Zero counterparts.

Usage (from the repo root, Git Bash):

  PYTHONPATH=tools python -I config_zero/sdk/crossmatch_sdk.py

Outputs (next to this script by default):
  sdk_symbols.json       Zero's table, same format as config/sdk_symbols.json
  manual_symbols.json    the AC5 manual_symbols.json entries, for Zero
  sdk_seeds.json         --ida-seeds file: SDK functions nothing calls directly
  overrides_draft.json   config/overrides.json rewritten for Zero
  coverage.md            per-override and per-symbol results
  crossmatch_report.json all details (notes, evidence, candidates)
"""

import argparse
import bisect
import difflib
import json
import os
import re
import sys
import time
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from ps2recomp.elf import ElfFile                      # noqa: E402
from ps2recomp.analysis import Program                 # noqa: E402

# ----------------------------------------------------------------------------
OP_J, OP_JAL, OP_LUI = 0x02, 0x03, 0x0F
BRANCH_OPS = {0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}
IMM_ALU_ADDR = {0x08, 0x09, 0x0D, 0x18, 0x19}          # addi addiu ori daddi(u)
IMM_ALU_OTHER = {0x0A, 0x0B, 0x0C, 0x0E}               # slti sltiu andi xori
GPR_LOADS = {0x1A, 0x1B, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
             0x27, 0x37}
OTHER_LOADS = {0x31, 0x36}                             # lwc1 lqc2
STORES = {0x1F, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x39, 0x3E, 0x3F}
MEMOPS = GPR_LOADS | OTHER_LOADS | STORES | {0x2F, 0x33}
CALLER_SAVED = set(range(1, 16)) | {24, 25, 31}
GP = 28
JR_RA = 0x03E00008
ERET = 0x42000018

STRONG = ("exact", "near")


def is_branch(w):
    op = w >> 26
    if op in BRANCH_OPS:
        return True
    if op == 0x01:
        return ((w >> 16) & 0x1F) in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)
    if op in (0x10, 0x11, 0x12) and ((w >> 21) & 0x1F) == 0x08:
        return True
    return False


def ends_flow(w):
    """Unconditional end of a straight-line run (needs a delay slot):
    jr ra, j, and b (beq $0,$0), which analysis.py treats as conditional."""
    return w == JR_RA or (w >> 26) == OP_J or (w & 0xFFFF0000) == 0x10000000


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def norm2(w):
    """Context-free coarse token: opcode + registers, no immediates."""
    op = w >> 26
    if op in (OP_J, OP_JAL):
        return w & 0xFC000000
    if op == 0 or op == 0x1C or op in (0x10, 0x11, 0x12):
        return w & 0xFFFF0000 if is_branch(w) else w
    return w & 0xFFFF0000


def cop2sig(words):
    """VU0 macro-mode skeleton: COP2 ops, lqc2/sqc2 with the GPR masked."""
    out = []
    for w in words:
        op = w >> 26
        if op == 0x12:
            rs = (w >> 21) & 0x1F
            if rs in (0x01, 0x02, 0x05, 0x06):          # qmfc2 cfc2 qmtc2 ctc2
                w &= ~(0x1F << 16)
            out.append(w)
        elif op in (0x36, 0x3E):
            out.append(w & ~(0x1F << 21))
    return out


class Image:
    """One executable: decoded .text, image range and partition."""

    def __init__(self, name, path, ida_db=None, ida_seeds=None,
                 augment=False, log=print):
        self.name = name
        self.elf = ElfFile(path)
        t0 = time.time()
        self.prog = Program(self.elf, ida_db, log=lambda *a: None)
        self.prog.seed()
        if ida_seeds:
            with open(ida_seeds) as fp:
                seeds = json.load(fp)
            for key in ("xref", "ptr"):
                for a in seeds.get(key, ()):
                    if self.prog.in_text(a):
                        self.prog.entries.add(a)
        self.prog.partition()
        self.words = self.prog.words
        self.base_functions = set(self.prog.functions)
        self.orphans = []
        if augment:
            self.orphans = self.orphan_starts()
            self.prog.entries |= set(self.orphans)
            self.prog.partition()
        base, img = self.elf.image()
        self.img_lo, self.img_hi = base, base + len(img)
        self.text_lo, self.text_hi = self.prog.lo, self.prog.hi
        self.entries = sorted(self.prog.functions)
        self.ext = {}
        for a, fn in self.prog.functions.items():
            self.ext[a] = max(list(fn.addrs) + list(fn.delay_slots) + [a]) + 4
        n = (self.text_hi - self.text_lo) // 4
        self.n2 = [norm2(self.words.get(self.text_lo + 4 * i, 0))
                   for i in range(n)]
        log("%s: %d functions (%d from the plain partition, %d orphan starts "
            "added), %.1f%% of .text covered (%.1fs)"
            % (name, len(self.prog.functions), len(self.base_functions),
               len(self.orphans), self.prog.stats["coverage"],
               time.time() - t0))

    def orphan_starts(self):
        """Function starts in code nothing calls directly."""
        out = []
        ins = self.prog.insns
        for lo, hi in self.prog.uncovered_ranges():
            a = lo
            while a < hi:
                while a < hi and self.words.get(a) == 0:
                    a += 4
                if a >= hi:
                    break
                if all(ins.get(a + 4 * k) is not None and
                       ins[a + 4 * k].cat != "invalid" for k in range(3)):
                    out.append(a)
                a = self._flow_end(a, lo, hi)
        return sorted(set(out))

    def _flow_end(self, s, lo, hi):
        """First address after the code reachable from s inside [lo, hi),
        with jr ra / j / b / eret ending a path."""
        seen, work, top = set(), [s], s
        while work:
            a = work.pop()
            if a in seen or not lo <= a < hi:
                continue
            seen.add(a)
            w = self.words.get(a, 0)
            top = max(top, a)
            if w == ERET:
                continue
            if ends_flow(w) or (w >> 26) == 0 and (w & 0x3F) == 0x08:
                top = max(top, a + 4)
                if (w >> 26) == OP_J:
                    t = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                    if s < t < hi and t > a:
                        work.append(t)
                elif (w & 0xFFFF0000) == 0x10000000:
                    t = (a + 4 + (s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
                    work.append(t)
                continue
            if is_branch(w):
                top = max(top, a + 4)
                work.append((a + 4 + (s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF)
                work.append(a + 8)
                continue
            work.append(a + 4)
        return top + 4

    def hi_is_addr(self, imm):
        v = imm << 16
        return self.img_lo - 0x10000 <= v <= self.img_hi + 0x10000

    def extent(self, a, fallback=None):
        if a in self.ext:
            return (self.ext[a] - a) // 4
        return fallback

    def calls_from(self, a):
        fn = self.prog.functions.get(a)
        if fn is None:
            return set()
        return set(fn.calls) | set(fn.tail_calls)

    def syscall_stubs(self):
        """addr -> syscall number for 'li v1,N; syscall; jr ra; nop'."""
        if not hasattr(self, "_sys"):
            self._sys = {}
            w = self.words
            for a in range(self.text_lo, self.text_hi - 12, 4):
                x = w.get(a, 0)
                if (x & 0xFFFF0000) == 0x24030000 and \
                        w.get(a + 4) == 0x0C and w.get(a + 8) == JR_RA:
                    self._sys[a] = s16(x & 0xFFFF)
            self._sys_by_num = defaultdict(set)
            for a, n in self._sys.items():
                self._sys_by_num[n].add(a)
        return self._sys

    # ------------------------------------------------------------------
    def tokens(self, start, n, fuzzy=False):
        """Masked tokens, call sites and address references of n words.

        calls[i] = ('JAL'|'J'|'B', target) for instructions that leave the
        function; refs[i] = absolute address formed by a lui/lo pair."""
        toks, calls, refs = [], {}, {}
        taint, hival = set(), {}
        clear_at = -1
        end = start + 4 * n

        def kill(r):
            taint.discard(r)
            hival.pop(r, None)

        for i in range(n):
            a = start + 4 * i
            w = self.words.get(a)
            if w is None:
                toks.append(("?",))
                continue
            if i == clear_at:
                for r in CALLER_SAVED:
                    kill(r)
            op = w >> 26
            rs = (w >> 21) & 0x1F
            rt = (w >> 16) & 0x1F
            rd = (w >> 11) & 0x1F
            tok = w
            if op in (OP_J, OP_JAL):
                tgt = ((a + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2)
                if op == OP_J and start <= tgt < end:
                    tok = ("J", tgt - start) if not fuzzy else ("J",)
                else:
                    tok = w & 0xFC000000
                    calls[i] = ("JAL" if op == OP_JAL else "J", tgt)
                if op == OP_JAL:
                    clear_at = i + 2
            elif is_branch(w):
                tgt = (a + 4 + (s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
                if not (start <= tgt < end):
                    tok = w & 0xFFFF0000
                    calls[i] = ("B", tgt)
                elif fuzzy:
                    tok = w & 0xFFFF0000
            elif op == OP_LUI:
                if self.hi_is_addr(w & 0xFFFF):
                    tok = ("LUI", rt)
                    taint.add(rt)
                    hival[rt] = (w & 0xFFFF) << 16
                else:
                    kill(rt)
            elif op in IMM_ALU_ADDR:
                if rs in taint or rs == GP:
                    tok = w & 0xFFFF0000
                    if rs in hival:
                        lo = (w & 0xFFFF) if op == 0x0D else s16(w & 0xFFFF)
                        refs[i] = (hival[rs] + lo) & 0xFFFFFFFF
                    hival.pop(rt, None)
                    taint.add(rt)
                else:
                    kill(rt)
            elif op in IMM_ALU_OTHER:
                kill(rt)
            elif op in MEMOPS:
                if rs in taint or rs == GP:
                    tok = w & 0xFFFF0000
                    if rs in hival:
                        refs[i] = (hival[rs] + s16(w & 0xFFFF)) & 0xFFFFFFFF
                if op in GPR_LOADS:
                    kill(rt)
            elif op == 0:
                f = w & 0x3F
                if f in (0x21, 0x2D, 0x25, 0x20, 0x2C):
                    if rs in taint or rt in taint:
                        taint.add(rd)
                    else:
                        taint.discard(rd)
                    hival.pop(rd, None)
                elif f == 0x09:
                    clear_at = i + 2
                elif f not in (0x08, 0x0C, 0x0D, 0x0F, 0x18, 0x19, 0x1A,
                               0x1B, 0x11, 0x13):
                    kill(rd)
            elif op in (0x10, 0x11, 0x12):
                if rs in (0, 1, 2):
                    kill(rt)
            elif op == 0x1C:
                kill(rd)
            toks.append(tok)
        return toks, calls, refs


def trim_padding(img, a, n):
    """Drop trailing nop padding that is not a delay slot."""
    while n > 2 and img.words.get(a + 4 * (n - 1)) == 0:
        prev = img.words.get(a + 4 * (n - 2), 0)
        if (prev >> 26) in (OP_J, OP_JAL) or is_branch(prev) or \
                ((prev >> 26) == 0 and (prev & 0x3F) in (0x08, 0x09)):
            break
        n -= 1
    return n


# ----------------------------------------------------------------------------
class Matcher:
    def __init__(self, ac5, zero, symbols, log=print):
        self.A, self.Z = ac5, zero
        self.sym = symbols
        self.log = log
        self.match = {}               # ac5 addr -> dict(z, kind, score, note)
        self.claimed = {}             # zero addr -> ac5 addr
        self.cand_exact = {}
        self.conflicts = []
        self.notes = defaultdict(dict)
        self.alen, self.atok, self.atokf, self.asig = {}, {}, {}, {}
        for a in symbols:
            n = trim_padding(ac5, a, ac5.extent(a, 2))
            self.alen[a] = n
            self.atok[a] = ac5.tokens(a, n)
            self.atokf[a] = ac5.tokens(a, n, fuzzy=True)
            self.asig[a] = cop2sig(ac5.words[a + 4 * i] for i in range(n))
        self._zf_cache = {}
        self._idx = {}
        self.zstarts = sorted(set(zero.entries))

    def note(self, a, text):
        self.notes[a][text] = None

    # ---------------------------------------------------------------- helpers
    def zlen(self, z, fallback):
        n = self.Z.extent(z)
        if n is None:
            return fallback
        return trim_padding(self.Z, z, n)

    def ztokf(self, z, n):
        key = (z, n)
        t = self._zf_cache.get(key)
        if t is None:
            t = self.Z.tokens(z, n, fuzzy=True)
            self._zf_cache[key] = t
        return t

    def ratio(self, a, z, zn=None):
        n = self.alen[a]
        if zn is None:
            zn = self.zlen(z, n)
        sm = difflib.SequenceMatcher(None, self.atokf[a][0],
                                     self.ztokf(z, zn)[0], autojunk=False)
        return sm.ratio(), sm, zn

    def sig_ratio(self, a, z):
        sa = self.asig[a]
        if len(sa) < 3:
            return 0.0
        zn = self.zlen(z, self.alen[a])
        sz = cop2sig(self.Z.words.get(z + 4 * i, 0) for i in range(zn))
        if len(sz) < 3:
            return 0.0
        return difflib.SequenceMatcher(None, sa, sz, autojunk=False).ratio()

    def is_exact_at(self, a, z):
        return self.Z.tokens(z, self.alen[a])[0] == self.atok[a][0]

    def body_score(self, a, z):
        """Best similarity of a against Zero code at z, and its kind."""
        if self.is_exact_at(a, z):
            return 1.0, "exact"
        r = self.ratio(a, z)[0]
        sr = self.sig_ratio(a, z)
        if sr > r and sr >= 0.9:
            return sr, "vu0sig"
        return r, None

    def index(self, k):
        idx = self._idx.get(k)
        if idx is None:
            idx = defaultdict(list)
            n2 = self.Z.n2
            lo = self.Z.text_lo
            for i in range(len(n2) - k + 1):
                idx[tuple(n2[i:i + k])].append(lo + 4 * i)
            self._idx[k] = idx
        return idx

    def accept(self, a, z, kind, score, note=""):
        old = self.claimed.get(z)
        if old is not None and old != a:
            if self.sym[old]["name"] == self.sym[a]["name"]:
                note = (note + "; " if note else "") + \
                    "same Zero code as AC5 %08X" % old
            else:
                self.conflicts.append(
                    "%s (AC5 %08X, %s) would map to Zero %08X, already %s "
                    "(AC5 %08X) -- rejected"
                    % (self.sym[a]["name"], a, kind, z,
                       self.sym[old]["name"], old))
                return False
        self.match[a] = dict(z=z, kind=kind, score=round(score, 4),
                             note=note)
        self.claimed.setdefault(z, a)
        return True

    def drop(self, a, why):
        m = self.match.pop(a)
        if self.claimed.get(m["z"]) == a:
            del self.claimed[m["z"]]
        self.note(a, "dropped %s match at Zero %08X: %s"
                  % (m["kind"], m["z"], why))

    def _pairs(self, ca, cz, ra, rz, positions):
        out = []
        for i, j in positions:
            if i in ca and j in cz and ca[i][0] == cz[j][0]:
                out.append((ca[i][1], cz[j][1], ca[i][0]))
            if i in ra and j in rz:
                out.append((ra[i], rz[j], "REF"))
        return out

    def call_pairs(self, a):
        """(AC5 target, Zero target, kind) for aligned call sites/refs."""
        m = self.match[a]
        z = m["z"]
        n = self.alen[a]
        if m["kind"] in STRONG:
            _, ca, ra = self.atok[a]
            _, cz, rz = self.Z.tokens(z, n)
            return self._pairs(ca, cz, ra, rz, ((i, i) for i in range(n)))
        r, sm, zn = self.ratio(a, z)
        _, ca, ra = self.atokf[a]
        _, cz, rz = self.ztokf(z, zn)
        pos = []
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag == "equal" or (tag == "replace" and i2 - i1 == j2 - j1):
                pos.extend((i1 + d, j1 + d) for d in range(i2 - i1))
        return self._pairs(ca, cz, ra, rz, pos)

    def strong_list(self):
        return sorted((a, m["z"]) for a, m in self.match.items()
                      if m["kind"] in STRONG or m["score"] >= 0.85)

    def neighbour_deltas(self, a, span=0x3000):
        sl = self.strong_list()
        keys = [k for k, _ in sl]
        i = bisect.bisect_left(keys, a)
        out = []
        for j in range(max(0, i - 3), min(len(sl), i + 4)):
            k, z = sl[j]
            if k != a and abs(k - a) <= span:
                out.append((abs(k - a), z - k, k))
        out.sort()
        return out

    # ----------------------------------------------------------------- phases
    def phase_exact(self):
        A, Z = self.A, self.Z
        nexact = nnear = 0
        for a in sorted(self.sym):
            n = self.alen[a]
            if n < 2:
                continue
            k = min(n, 8)
            key = tuple(norm2(A.words[a + 4 * i]) for i in range(k))
            ta = self.atok[a][0]
            exact, near = [], []
            budget = max(2, n // 10)
            for z in self.index(k).get(key, ()):
                tz = Z.tokens(z, n)[0]
                mm = sum(1 for x, y in zip(ta, tz) if x != y)
                if mm == 0:
                    exact.append(z)
                elif mm <= budget and n >= 12:
                    near.append((mm, z))
            self.cand_exact[a] = exact
            if exact:
                if len(exact) == 1 and n >= 4:
                    self.accept(a, exact[0], "exact", 1.0)
                    nexact += 1
            elif near:
                near.sort()
                if len(near) == 1 or near[0][0] < near[1][0]:
                    mm, z = near[0]
                    self.accept(a, z, "near", 1.0 - mm / n,
                                "%d/%d words differ" % (mm, n))
                    nnear += 1
        self.log("exact: %d unique, near: %d, exact with several copies: %d"
                 % (nexact, nnear,
                    sum(1 for a, c in self.cand_exact.items()
                        if len(c) > 1 and a not in self.match)))

    def resolve_ambiguous(self):
        changed = 0
        for a, cands in sorted(self.cand_exact.items()):
            if a in self.match or not cands:
                continue
            if len(cands) == 1 and self.alen[a] >= 4:
                continue
            pick = None
            for dist, delta, k in self.neighbour_deltas(a, 0x4000):
                hits = [z for z in cands if z - a == delta]
                if len(hits) == 1:
                    pick = hits[0]
                    break
            if pick is None:
                continue
            if self.accept(a, pick, "exact", 1.0,
                           ("%d identical copies in Zero, picked by layout"
                            % len(cands)) if len(cands) > 1 else
                           "short body, confirmed by neighbour displacement"):
                changed += 1
        if changed:
            self.log("exact (copies resolved by layout): %d" % changed)
        return changed

    def build_gram_index(self):
        self.zfun = {}
        self.gram_post = defaultdict(set)
        for z in self.zstarts:
            n = self.zlen(z, 2)
            if n < 6:
                continue
            toks = self.ztokf(z, n)[0]
            gs = set(zip(toks, toks[1:], toks[2:]))
            self.zfun[z] = (n, gs)
            for g in gs:
                self.gram_post[g].add(z)

    def phase_fuzzy(self, min_ratio=0.80):
        if not hasattr(self, "zfun"):
            self.build_gram_index()
        got = 0
        for a in sorted(self.sym):
            if a in self.match:
                continue
            n = self.alen[a]
            if n < 8:
                continue
            ta = self.atokf[a][0]
            ga = set(zip(ta, ta[1:], ta[2:]))
            cnt = Counter()
            for g in ga:
                post = self.gram_post.get(g)
                if post and len(post) <= 300:
                    cnt.update(post)
            scored = []
            for z, c in cnt.most_common(40):
                zn, gz = self.zfun[z]
                if not (0.5 * n <= zn <= 2.0 * n):
                    continue
                scored.append((c / max(len(ga), len(gz), 1), z))
            scored.sort(reverse=True)
            res = sorted(((self.ratio(a, z)[0], z) for _, z in scored[:8]),
                         reverse=True)
            if not res or res[0][0] < min_ratio:
                continue
            r, z = res[0]
            second = res[1][0] if len(res) > 1 else 0.0
            if z in self.claimed:
                continue
            if r - second < 0.05:
                self.note(a, "fuzzy tie Zero %08X %.2f vs %.2f"
                          % (z, r, second))
                continue
            if self.accept(a, z, "fuzzy", r,
                           "runner-up %.2f" % second if second else ""):
                got += 1
        if got:
            self.log("fuzzy: %d" % got)
        return got

    def phase_callgraph(self, min_ratio=0.55):
        total = 0
        while True:
            prop = defaultdict(Counter)
            for a in list(self.match):
                for ta, tz, kind in self.call_pairs(a):
                    if ta in self.sym and ta not in self.match:
                        prop[ta][tz] += 1
            got = 0
            for ta, c in sorted(prop.items()):
                mc = c.most_common(2)
                tz, votes = mc[0]
                if len(mc) > 1 and mc[1][1] == votes:
                    self.note(ta, "callgraph vote tie: %s"
                              % ", ".join("%08X x%d" % kv for kv in mc))
                    continue
                if not self.Z.prog.in_text(tz) or tz in self.claimed:
                    continue
                score, kind = self.body_score(ta, tz)
                tiny = self.alen[ta] <= 4
                if kind is None:
                    win = self.window(ta)
                    inwin = bool(win and win[0] < tz < win[1])
                    if score >= min_ratio or (tiny and score >= 0.4):
                        kind = "callgraph"
                    elif votes >= 2 and inwin:
                        kind = "callgraph"
                        self.note(ta, "body differs (ratio %.2f) but %d call "
                                      "sites agree and it sits between the "
                                      "matched neighbours" % (score, votes))
                    else:
                        self.note(ta, "callgraph points at Zero %08X but body "
                                      "ratio only %.2f" % (tz, score))
                        continue
                if self.accept(ta, tz, kind, score,
                               "via %d call site(s)/reference(s)" % votes):
                    got += 1
            total += got
            if not got:
                break
        if total:
            self.log("callgraph: %d" % total)
        return total

    def window(self, a):
        """Zero address window (lo, hi) that a must fall in, from matched
        neighbours (any quality) on both sides, or None."""
        keys = sorted(self.match)
        i = bisect.bisect_left(keys, a)
        if i == 0 or i >= len(keys):
            return None
        aP, aS = keys[i - 1], keys[i]
        zP, zS = self.match[aP]["z"], self.match[aS]["z"]
        if not zP < zS:
            return None
        if aS - aP > 0x6000 or zS - zP > 2 * (aS - aP) + 0x800:
            return None
        return zP, zS, aP, aS

    def phase_vu0(self):
        got = 0
        zsig = {}
        for z in self.zstarts:
            zn = self.zlen(z, 2)
            s = cop2sig(self.Z.words.get(z + 4 * i, 0) for i in range(zn))
            if len(s) >= 3:
                zsig[z] = s
        for a in sorted(self.sym):
            if a in self.match or len(self.asig[a]) < 3:
                continue
            sa = self.asig[a]
            win = self.window(a)
            res = []
            for z, s in zsig.items():
                if z in self.claimed:
                    continue
                if not (0.5 <= len(s) / len(sa) <= 2.0):
                    continue
                r = difflib.SequenceMatcher(None, sa, s,
                                            autojunk=False).ratio()
                if r >= 0.85:
                    inwin = bool(win and win[0] < z < win[1])
                    res.append((r, inwin, z))
            if not res:
                continue
            res.sort(reverse=True)
            best = res[0][0]
            top = [x for x in res if x[0] >= best - 0.05]
            if len(top) > 1:
                inw = [x for x in top if x[1]]
                if len(inw) == 1 and inw[0][0] == best:
                    top = inw
            if best < 0.9 or len(top) != 1:
                if best >= 0.9:
                    self.note(a, "vu0sig: %d candidates at %.2f (%s)"
                              % (len(top), best, " ".join(
                                  "%08X" % x[2] for x in top[:6])))
                continue
            r, inwin, z = top[0]
            if self.accept(a, z, "vu0sig", r,
                           "COP2 sequence" + (", inside neighbour window"
                                              if inwin else "")):
                got += 1
        if got:
            self.log("vu0sig: %d" % got)
        return got

    def phase_layout(self, min_ratio=0.6):
        got = 0
        for a in sorted(self.sym):
            if a in self.match:
                continue
            win = self.window(a)
            if win is None:
                continue
            zP, zS, aP, aS = win
            lo = bisect.bisect_right(self.zstarts, zP)
            hi = bisect.bisect_left(self.zstarts, zS)
            cands = [z for z in self.zstarts[lo:hi] if z not in self.claimed]
            if not cands:
                continue
            scored = []
            for z in cands:
                s, kind = self.body_score(a, z)
                scored.append((s, z, kind))
            scored.sort(reverse=True)
            s, z, kind = scored[0]
            second = scored[1][0] if len(scored) > 1 else 0.0
            between_a = (bisect.bisect_left(self.A.entries, aS) -
                         bisect.bisect_right(self.A.entries, aP))
            between_z = hi - lo
            slot = between_a == 1 and between_z == 1 and len(cands) == 1
            if kind is None and s >= min_ratio and s - second >= 0.05:
                kind = "layout"
                note = "best of %d starts between neighbours" % len(cands)
            elif kind is None and s >= 0.45 and s - second >= 0.25:
                kind = "layout"
                note = "best of %d starts between neighbours, clear margin"                     % len(cands)
            elif kind is None and slot:
                kind = "layout"
                note = ("the only function between its neighbours in both "
                        "games")
            elif kind is not None:
                note = "found between neighbours"
            else:
                self.note(a, "layout: best start between neighbours Zero "
                             "%08X ratio %.2f (runner-up %.2f)"
                          % (z, s, second))
                continue
            if self.accept(a, z, kind, s, note):
                got += 1
        if got:
            self.log("layout: %d" % got)
        return got

    def targets(self, img, a, n, fuzzy):
        _, calls, refs = img.tokens(a, n, fuzzy=fuzzy)
        return set(t for _, t in calls.values()) | set(refs.values())

    def verify(self):
        """Evidence for every match.

        ok    an SDK callee / function pointer of the AC5 body whose Zero
              counterpart is called or referenced anywhere in the Zero body
        wrap  ... reached through a function the Zero body calls (SDK 3.0
              added wrappers, e.g. a retry loop around sceSifCallRpc)
        miss  ... not found in the Zero body (code changed)
        bad   at an aligned call site the Zero body targets a *different*
              matched SDK function"""
        A, Z = self.A, self.Z
        for a, m in self.match.items():
            z = m["z"]
            n = self.alen[a]
            strong = m["kind"] in STRONG
            ta_all = self.targets(A, a, n, not strong)
            zn = n if strong else self.zlen(z, n)
            tz_all = self.targets(Z, z, zn, not strong)
            ok = wrap = miss = 0
            wrap_list, miss_list = [], []
            asys = A.syscall_stubs()
            Z.syscall_stubs()
            for t in sorted(ta_all):
                if t == a:
                    continue
                if t not in self.match:
                    if t in asys:              # unnamed EE syscall stub
                        zs = Z._sys_by_num.get(asys[t], set())
                        if zs & tz_all:
                            ok += 1
                        elif any(zs & Z.calls_from(x) for x in tz_all
                                 if Z.prog.in_text(x)):
                            wrap += 1
                            wrap_list.append("syscall %d" % asys[t])
                        else:
                            miss += 1
                            miss_list.append("syscall %d" % asys[t])
                    continue
                want = self.match[t]["z"]
                if want in tz_all:
                    ok += 1
                elif any(want in Z.calls_from(x) for x in tz_all
                         if Z.prog.in_text(x)):
                    wrap += 1
                    wrap_list.append(self.sym[t]["name"])
                else:
                    miss += 1
                    miss_list.append(self.sym[t]["name"])
            bad_list = []
            for ta, tzz, kind in self.call_pairs(a):
                if ta not in self.match or tzz == self.match[ta]["z"]:
                    continue
                other = self.claimed.get(tzz)
                if other is None or other == ta:
                    continue
                if self.sym[other]["name"] == self.sym[ta]["name"]:
                    continue
                if self.match[ta]["z"] in tz_all:
                    continue
                bad_list.append("%s: AC5 calls %s, Zero calls %s at the same "
                                "place" % (kind, self.sym[ta]["name"],
                                           self.sym[other]["name"]))
            m["cg_ok"], m["cg_wrap"], m["cg_miss"] = ok, wrap, miss
            m["cg_bad"] = len(set(bad_list))
            if bad_list:
                m["cg_conflicts"] = sorted(set(bad_list))
            if wrap_list:
                m["cg_wrapped"] = sorted(set(wrap_list))
            if miss_list:
                m["cg_missing"] = sorted(set(miss_list))
            r, sm, zn2 = self.ratio(a, z)
            m["ratio"] = round(r, 4)
            m["zwords"] = zn2
            m["awords"] = n
            m["zero_entry"] = z in Z.base_functions
            m.pop("outlier", None)
            nd = self.neighbour_deltas(a)
            delta = z - a
            if nd and min(abs(delta - d) for _, d, _ in nd) > 0x2000:
                m["outlier"] = ("displacement %X, neighbours %s"
                                % (delta, " ".join(
                                    "%X" % d for _, d, _ in nd[:3])))

    def prune_outliers(self):
        """Short exact hits far from their neighbours are most likely game
        code that happens to look the same; drop them unless the call graph
        backs them."""
        dropped = 0
        for a in sorted(self.match):
            m = self.match[a]
            if m.get("outlier") and self.alen[a] <= 10 and not m["cg_ok"]:
                self.drop(a, "short body far from its neighbours (%s)"
                          % m["outlier"])
                dropped += 1
        return dropped


# ----------------------------------------------------------------------------
def load_ac5_symbols(cfg):
    sdk = json.load(open(os.path.join(cfg, "sdk_symbols.json")))
    man = json.load(open(os.path.join(cfg, "manual_symbols.json")))
    out = {}
    for k, v in sdk.items():
        if k.startswith("//"):
            continue
        a = int(k, 16)
        lib = v.get("lib", "")
        if "(aligned)" in lib:
            cls = "aligned"
        elif "(anchored)" in lib:
            cls = "anchored"
        elif "verified" in lib:
            cls = "verified"
        elif lib.startswith("sifrpc.o"):
            cls = "manual"
        else:
            cls = "sig"
        out[a] = dict(name=v["name"], lib=lib, words=v.get("words", 0),
                      cls=cls, manual=False)
    for k, v in man.items():
        if k.startswith("//"):
            continue
        a = int(k, 16)
        e = out.setdefault(a, dict(name=v["name"], lib=v.get("src", "manual"),
                                   words=0, cls="manual"))
        e.update(manual=True, src=v.get("src", "manual"), name=v["name"])
    return out


def load_overrides(path):
    txt = open(path).read()
    keys = re.findall(r'^\s*"([^"]+)"\s*:\s*"([^"]+)"', txt, re.M)
    return [(k, h) for k, h in keys if not k.startswith("//")]


def lib_base(lib):
    return lib.split(" ")[0].rstrip(",;")


# AC5 labels that are wrong or dubious in AC5's own table (found while
# building this cross-match); reported in coverage.md.
# Labels shown here are matched (they help the call-graph checks) but not
# written to Zero's table.
AC5_LABEL_ISSUES = {
    0x0032F9C0: "AC5 calls this 'scePrintf', but the body is the syscall "
                "0x42 (SignalSema) stub; libcdvd calls it to release its "
                "semaphores",
    0x003312C0: "AC5 calls this 'sceSifMInitRpc' (aligned), but the body is "
                "a varargs printf: it formats through _sceSnprintf with "
                "deci2Putchar as the sink",
    0x0032F4F8: "AC5 calls this 'sceVu0ClipScreen3' (aligned), but the body "
                "resets VIF0/VU0 (it writes VIF0 MARK/ERR/FBRST, sets FBRST "
                "through ctc2 vi28 and pushes quadwords into the VIF0 FIFO); "
                "it is probably the real sceVpu0Reset",
    0x0032F5F0: "AC5 calls this 'sceVu0ClipAll' (aligned), but the body is "
                "the syscall 7 (_ExecPS2) stub",
    0x0032F650: "AC5 calls this 'sceVpu0Reset' (aligned), but the body is the "
                "syscall 0xD (SetVTLBRefillHandler) stub",
}

# AC5's netcnfif.a / libhttps.a labels sit on 2-5 instruction C++ accessors
# in game code (AC5 has no netcnf or SSL strings at all): false positives of
# the original signature run.  They are not transferred.
AC5_EXCLUDED_LIBS = ("netcnfif.a", "libhttps.a")


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--ac5", default=os.path.join(os.path.dirname(ROOT),
                                                  "ac5game", "SLUS_208.51"))
    ap.add_argument("--ac5-config", default=os.path.join(ROOT, "config"))
    ap.add_argument("--zero", default=os.path.join(ROOT, "game",
                                                   "SLUS_213.46"))
    ap.add_argument("--zero-ida-db")
    ap.add_argument("--zero-ida-seeds")
    ap.add_argument("--overrides",
                    default=os.path.join(ROOT, "config", "overrides.json"))
    ap.add_argument("-o", "--outdir", default=HERE)
    args = ap.parse_args(argv)

    t0 = time.time()
    cfg = args.ac5_config
    A = Image("AC5", args.ac5, os.path.join(cfg, "ida_db.json"),
              os.path.join(cfg, "ida_seeds.json"))
    Z = Image("Zero", args.zero, args.zero_ida_db, args.zero_ida_seeds,
              augment=True)
    all_syms = load_ac5_symbols(cfg)
    syms = {a: s for a, s in all_syms.items()
            if lib_base(s["lib"]) not in AC5_EXCLUDED_LIBS}
    args.excluded = {a: s for a, s in all_syms.items() if a not in syms}
    print("AC5 symbols: %d (%d excluded as AC5 false positives)"
          % (len(syms), len(args.excluded)))

    M = Matcher(A, Z, syms)
    M.phase_exact()
    for rnd in range(8):
        c = M.resolve_ambiguous()
        c += M.phase_callgraph()
        c += M.phase_fuzzy()
        c += M.phase_callgraph()
        c += M.phase_vu0()
        c += M.phase_callgraph()
        c += M.phase_layout()
        if not c:
            M.verify()
            if not M.prune_outliers():
                break
    M.verify()
    rpc_check(os.path.join(cfg, "rpc_sids.json"), Z, M)
    write_outputs(args, A, Z, M, syms, t0)


def rpc_check(path, Z, M):
    """Independent check with config/rpc_sids.json: the SIF RPC server id
    an AC5 function binds, and the RPC function number it calls, must show
    up in the Zero counterpart too."""
    if not os.path.exists(path):
        return
    rs = json.load(open(path))

    def body(a):
        m = M.match[a]
        return m["z"], max(m.get("zwords", 0), m["awords"]) + 8

    def consts(lo, n):
        out, hi = set(), {}
        for i in range(n):
            w = Z.words.get(lo + 4 * i, 0)
            op, r_s, r_t = w >> 26, (w >> 21) & 31, (w >> 16) & 31
            if op == OP_LUI:
                hi[r_t] = (w & 0xFFFF) << 16
                out.add(hi[r_t])
            elif op == 0x0D and r_s in hi:
                out.add(hi[r_s] | (w & 0xFFFF))
            elif op == 0x09 and r_s in hi:
                out.add((hi[r_s] + s16(w & 0xFFFF)) & 0xFFFFFFFF)
        return out

    def a1_imms(lo, n):
        return {w & 0xFFFF for w in (Z.words.get(lo + 4 * i, 0)
                                     for i in range(n))
                if (w & 0xFFFF0000) == 0x24050000}

    for kind, key, parse in (("sid", "sid", lambda v: int(v, 16)),
                             ("fno", "fno", lambda v: int(v, 16))):
        for e in rs.get("bind" if kind == "sid" else "call", ()):
            a = int(e["fn"], 16)
            if e.get(key) is None or a not in M.match:
                continue
            v = parse(e[key])
            z, n = body(a)
            ok = v in (consts(z, n) if kind == "sid" else a1_imms(z, n))
            m = M.match[a]
            m.setdefault("rpc", [])
            m["rpc"].append("%s 0x%X %s" % (kind, v, "ok" if ok else "MISSING"))
    for m in M.match.values():
        if "rpc" in m:
            m["rpc"] = sorted(set(m["rpc"]))


# ----------------------------------------------------------------------------
def quality(m):
    if m["kind"] in STRONG:
        return m["kind"]
    return "%s %.2f" % (m["kind"], m["score"])


def is_weak(m):
    """Not good enough to drive a name-based override."""
    if m.get("cg_bad") or m.get("outlier"):
        return True
    if m["kind"] in STRONG or m["score"] >= 0.75:
        return False
    # re-compiled body: accept when the SDK callees / pointers line up
    agree = m.get("cg_ok", 0) + m.get("cg_wrap", 0)
    return not (agree >= 3 and agree >= 2 * m.get("cg_miss", 0))


def zero_lib(s, m):
    """lib string for the Zero table.  ps2recomp refuses name-based
    overrides/hooks on '(aligned)' entries, so the tag is kept when AC5's own
    name was only aligned, and added when this cross-match is weak."""
    lib = s["lib"]
    if is_weak(m) and "(aligned)" not in lib:
        lib += " (aligned)"
    return "%s; xmatch AC5 0x%08X %s" % (lib, s["addr"], quality(m))


def write_outputs(args, A, Z, M, syms, t0):
    out = args.outdir
    os.makedirs(out, exist_ok=True)
    for a, s in syms.items():
        s["addr"] = a

    sdk_out, man_out = {}, {}
    for a in sorted(M.match, key=lambda k: (M.match[k]["z"], k)):
        if a in AC5_LABEL_ISSUES:
            continue
        m, s = M.match[a], syms[a]
        key = "0x%08X" % m["z"]
        if key in sdk_out:
            continue
        sdk_out[key] = dict(name=s["name"], lib=zero_lib(s, m),
                            words=m["awords"] if m["kind"] in STRONG else 0)
        if s.get("manual"):
            man_out[key] = dict(name=s["name"],
                                src="xmatch AC5 0x%08X %s; AC5: %s"
                                % (a, quality(m), s.get("src", "")))
    sdk_out = dict(sorted(sdk_out.items()))
    man_out = dict(sorted(man_out.items()))
    with open(os.path.join(out, "sdk_symbols.json"), "w") as fp:
        json.dump(sdk_out, fp, indent=1)
        fp.write("\n")
    with open(os.path.join(out, "manual_symbols.json"), "w") as fp:
        json.dump(man_out, fp, indent=1)
        fp.write("\n")

    report = {}
    for a in sorted(syms):
        s = syms[a]
        e = dict(name=s["name"], ac5_lib=s["lib"], ac5_class=s["cls"],
                 ac5_words=M.alen[a])
        if a in M.match:
            m = dict(M.match[a])
            m["z"] = "0x%08X" % m["z"]
            m["weak"] = is_weak(M.match[a])
            e.update(m)
        if M.notes.get(a):
            e["notes"] = list(M.notes[a])
        if len(M.cand_exact.get(a, ())) > 1:
            e["exact_copies"] = ["0x%08X" % z for z in M.cand_exact[a]]
        if a in AC5_LABEL_ISSUES:
            e["ac5_label_issue"] = AC5_LABEL_ISSUES[a]
            e["transferred"] = False
        report["0x%08X" % a] = e
    with open(os.path.join(out, "crossmatch_report.json"), "w") as fp:
        json.dump(dict(conflicts=M.conflicts,
                       zero_orphan_starts=len(Z.orphans),
                       excluded=["0x%08X %s" % (a, s["name"]) for a, s in
                                 sorted(args.excluded.items())],
                       symbols=report), fp, indent=1)
        fp.write("\n")

    seeds = write_seeds(args, Z, M, syms)
    ovr = write_overrides_draft(args, Z, M, syms, sdk_out, seeds)
    write_coverage(args, A, Z, M, syms, sdk_out, ovr, t0, seeds)
    print("wrote %d Zero SDK symbols (%d manual) to %s in %.1fs"
          % (len(sdk_out), len(man_out), out, time.time() - t0))


def resolve_override(key, syms, by_name):
    if key.startswith("0x"):
        return [int(key, 16)]
    return by_name.get(key, [])


def write_seeds(args, Z, M, syms):
    """Entry points for SDK functions nothing in Zero calls directly, in the
    format ps2recomp's --ida-seeds reads ({"xref": [...], "ptr": [...]}).
    Only well-verified matches whose address is not a delay slot."""
    seeds = []
    for a, m in M.match.items():
        if a in AC5_LABEL_ISSUES or m["zero_entry"] or is_weak(m):
            continue
        z = m["z"]
        prev = Z.words.get(z - 4, 0)
        if (prev >> 26) in (OP_J, OP_JAL) or is_branch(prev) or \
                ((prev >> 26) == 0 and (prev & 0x3F) in (0x08, 0x09)):
            M.note(a, "Zero %08X is a delay slot; not seeded" % z)
            continue
        seeds.append(z)
    seeds = sorted(set(seeds))
    with open(os.path.join(args.outdir, "sdk_seeds.json"), "w") as fp:
        json.dump({"//": "SDK functions in SLUS_213.46 that nothing calls "
                         "directly; pass with --ida-seeds so ps2recomp "
                         "recovers them (generated by crossmatch_sdk.py)",
                   "xref": seeds, "ptr": []}, fp, indent=1)
        fp.write("\n")
    return set(seeds)


def write_overrides_draft(args, Z, M, syms, sdk_out, seeds):
    """overrides.json for Zero.  Hex keys where a name would not resolve
    (aligned tag, duplicate name, or AC5 itself used a hex key); entries
    that cannot work yet stay as '//' keys, which ps2recomp skips."""
    ov = load_overrides(args.overrides)
    by_name = defaultdict(list)
    for a, s in syms.items():
        by_name[s["name"]].append(a)
    zname = Counter(v["name"] for v in sdk_out.values())
    lines, status = [], {}
    for key, handler in ov:
        a5 = resolve_override(key, syms, by_name)
        hits = [a for a in a5 if a in M.match]
        if not hits:
            lines.append(("// not in Zero: %s" % key,
                          "%s (AC5 %s)" % (handler, " ".join(
                              "%08X" % a for a in a5) or "-")))
            status[key] = "absent"
            continue
        a = hits[0]
        m = M.match[a]
        z = m["z"]
        lib = sdk_out["0x%08X" % z]["lib"]
        name = syms[a]["name"]
        use_hex = key.startswith("0x") or "(aligned)" in lib or \
            zname[name] != 1
        k = ("0x%08X" % z) if use_hex else name
        if is_weak(m):
            lines.append(("// check before enabling: %s" % k,
                          "%s (%s)" % (handler, quality(m))))
            status[key] = "weak"
        elif not m["zero_entry"] and z in seeds:
            lines.append((k, handler))
            status[key] = "ok+seed"
        elif not m["zero_entry"]:
            lines.append(("// needs a seed at 0x%08X: %s" % (z, k),
                          handler + " (nothing in Zero calls it directly, so "
                          "the config-less partition has no function here)"))
            status[key] = "needs-seed"
        else:
            lines.append((k, handler))
            status[key] = "ok"
    path = os.path.join(args.outdir, "overrides_draft.json")
    with open(path, "w") as fp:
        fp.write("{\n")
        fp.write('"// generated": "by crossmatch_sdk.py from '
                 'config/overrides.json, with Zero (SLUS_213.46) addresses",\n')
        fp.write('"// usage": "needs --symbols config_zero/sdk/sdk_symbols.json '
                 'and --ida-seeds config_zero/sdk/sdk_seeds.json (some '
                 'targets are only recovered through the seeds)",\n')
        for i, (k, v) in enumerate(lines):
            comma = "," if i + 1 < len(lines) else ""
            fp.write("%s: %s%s\n" % (json.dumps(k), json.dumps(v), comma))
        fp.write("}\n")
    json.load(open(path))                     # must stay valid JSON
    return status


def evid(m):
    return "%d/%d/%d/%d" % (m["cg_ok"], m["cg_wrap"], m["cg_miss"],
                            m["cg_bad"])


_CD_ABSENT = ("not linked in Zero: SDK 3.0 libcdvd keeps it in its own object "
              "and Zero never calls it")
ABSENT_WHY = {n: _CD_ABSENT for n in (
    "sceCdRead", "sceCdReadIOPm", "sceCdGetToc", "sceCdSeek",
    "sceCdGetError", "sceCdStatus", "sceCdBreak")}

OVERRIDE_NOTES = {
    "sceCdInitEeCB": "SDK 3.0 body dropped the semaphore setup; it still "
                     "creates and starts the callback thread on _sceCd_cbLoop",
}

LIB_NOTES = {
    "libcdvd.a": "7 functions not linked in Zero (see above)",
    "libmpeg.a": "most AC5 names are (aligned); libmpeg changed a lot in 3.0",
    "libvu0.a": "AC5 names (aligned); matched by VU0 instruction sequence",
    "libkernl.a": "printf family rewritten in 3.0",
}


def write_coverage(args, A, Z, M, syms, sdk_out, ovr, t0, seeds):
    ov = load_overrides(args.overrides)
    by_name = defaultdict(list)
    for a, s in syms.items():
        by_name[s["name"]].append(a)
    transferred = {a for a in M.match if a not in AC5_LABEL_ISSUES}

    L = []
    w = L.append
    w("# Ace Combat Zero (SLUS-213.46): SDK symbol coverage\n")
    w("`config_zero/sdk/crossmatch_sdk.py` generated this file. It "
      "cross-matches the SDK functions labelled for AC5 (SLUS-208.51, in "
      "`config/sdk_symbols.json` and `config/manual_symbols.json`) against "
      "Zero's code. Sony's libraries are not needed. To re-run it, go to the "
      "repo root and run "
      "`PYTHONPATH=tools python -I config_zero/sdk/crossmatch_sdk.py`. It "
      "takes about a minute.\n")
    w("**The SDK versions differ.** AC5 links SDK **2.8.0** (libmc 2.8.1). "
      "Zero links **3.0.0** (libdbc, libpad2 and libmc are 3.0.2). This comes "
      "from the `PsIIlibxxxx NNNN` strings in both executables. Most library "
      "code is identical modulo relocations. libmc, libcdvd, libmpeg, libvu0 "
      "and fileio changed. SDK 3.0's libcdvd is split into more objects, so "
      "the functions Zero never calls are not linked.\n")
    w("Match kinds:\n")
    w("- **exact**: identical after masking relocatable fields (jal/j "
      "targets, the halves of absolute addresses, gp offsets, branches out "
      "of the function).")
    w("- **near**: same length, at most max(2, n/10) words differ.")
    w("- **fuzzy r**: different body; r is the similarity of the masked "
      "instruction streams.")
    w("- **callgraph r**: reached through a matched function's call site or "
      "function pointer.")
    w("- **vu0sig r**: same VU0 macro-instruction sequence. SDK 3.0 libvu0 is "
      "compiled much more compactly, so the full bodies do not compare.")
    w("- **layout r**: the best function start between matched neighbours.\n")
    w("Columns:\n")
    w("- `ratio`: body similarity of the whole function.")
    w("- `ev ok/wrap/miss/bad`: the AC5 body's calls to, and pointers at, "
      "other matched SDK functions. **ok** means the Zero body uses the "
      "counterpart. **wrap** means it reaches the counterpart through a new "
      "SDK 3.0 helper. **miss** means it does not use it (the code changed). "
      "**bad** means that at the same place it calls a *different* known "
      "SDK function.")
    w("- `entry`: whether Zero's config-less partition "
      "(`python -m ps2recomp game/SLUS_213.46`) already has a function "
      "there. ps2recomp refuses an override on an address that is not a "
      "recovered function.\n")
    w("Each entry in `sdk_symbols.json` keeps AC5's `lib` string and appends "
      "`; xmatch AC5 0x... <kind>`. When the cross-match is weak, `(aligned)` "
      "is added. Weak means a score below 0.75 without at least 3 agreeing "
      "SDK references (ok + wrap, and at least twice the misses), a `bad` "
      "reference, or a displacement outlier. EE syscall stubs (`li v1,N; "
      "syscall`) that AC5's table does not name are paired by syscall "
      "number and count as references too. As in "
      "AC5's table, ps2recomp then refuses to use the name for an override "
      "or hook. AC5's own `(aligned)` tags are kept.\n")

    # ---- overrides
    w("## Symbols named in config/overrides.json\n")
    w("| key | AC5 symbol | AC5 addr | Zero addr | match | ratio "
      "| ev ok/wrap/miss/bad | entry | draft | notes |")
    w("|---|---|---|---|---|---|---|---|---|---|")
    cnt = Counter()
    for key, handler in ov:
        a5 = resolve_override(key, syms, by_name)
        if not a5:
            w("| `%s` | (not in AC5 table) | | | | | | | | |" % key)
            cnt["absent"] += 1
            continue
        a = next((x for x in a5 if x in M.match), a5[0])
        s = syms[a]
        m = M.match.get(a)
        st = ovr.get(key, "")
        if m is None:
            w("| `%s` | %s | %08X | **not in Zero** | | | | | %s | %s |"
              % (key, s["name"], a, st, ABSENT_WHY.get(s["name"], "")))
            cnt["absent"] += 1
            continue
        cnt["found"] += 1
        flag = []
        lib = sdk_out.get("0x%08X" % m["z"], {}).get("lib", "")
        if key.startswith("0x"):
            flag.append("AC5 uses a hex key; for Zero use `\"0x%08X\"`"
                        % m["z"])
        elif "(aligned)" in lib:
            flag.append("tagged (aligned): needs the hex key")
        if m.get("cg_bad"):
            flag.append("CONFLICT: " + "; ".join(m.get("cg_conflicts", [])))
        if m.get("cg_wrapped"):
            flag.append("reaches %s through a new wrapper"
                        % ", ".join(m["cg_wrapped"]))
        if m.get("note"):
            flag.append(m["note"])
        if m.get("rpc"):
            flag.append("RPC " + ", ".join(m["rpc"]))
        if s["name"] in OVERRIDE_NOTES:
            flag.append(OVERRIDE_NOTES[s["name"]])
        w("| `%s` | %s | %08X | **%08X** | %s | %.2f | %s | %s | %s | %s |"
          % (key, s["name"], a, m["z"], quality(m), m["ratio"], evid(m),
             "yes" if m["zero_entry"] else "no", st, "; ".join(flag)))
    w("")
    st = Counter(ovr.values())
    w("Of the %d keys, **%d have a counterpart in Zero** and **%d do not "
      "exist in Zero**. `overrides_draft.json` is a Zero version of the list "
      "that ps2recomp can read. It has %d active entries: %d whose function "
      "Zero's config-less partition already recovers, and %d (`ok+seed`) "
      "that are recovered only when `sdk_seeds.json` is passed. Also, %d "
      "entries are commented out as weak, %d as `needs-seed`, and %d because "
      "the function is absent.\n"
      % (len(ov), cnt["found"], cnt["absent"], st["ok"] + st["ok+seed"],
         st["ok"], st["ok+seed"], st["weak"], st["needs-seed"],
         st["absent"]))
    w("`ok+seed`: nothing in Zero calls the function directly (it may be "
      "unused, or reached only through a pointer). Zero's config-less "
      "partition therefore has no function at that address, and without a "
      "seed ps2recomp would stop with \"not a recovered function\". "
      "`sdk_seeds.json` (%d addresses, in the `--ida-seeds` format) adds "
      "every well-verified SDK function that has no recovered entry. If Zero "
      "later gets its own IDA seeds, merge the two `xref` lists, because "
      "ps2recomp accepts only one `--ida-seeds` file. The draft was checked "
      "with:\n" % len(seeds))
    w("```")
    w("PYTHONPATH=tools python -m ps2recomp game/SLUS_213.46 -o <outdir> \\")
    w("    --symbols config_zero/sdk/sdk_symbols.json \\")
    w("    --symbols config_zero/sdk/manual_symbols.json \\")
    w("    --ida-seeds config_zero/sdk/sdk_seeds.json \\")
    w("    --overrides config_zero/sdk/overrides_draft.json")
    w("```\n")
    w("The libcdvd functions marked absent are not in Zero's executable. In "
      "SDK 3.0 they live in separate objects and are linked only when "
      "called. Zero calls only `sceCdInit`, `sceCdMmode` and "
      "`sceCdReadClock` (0x0030D0D8, a function AC5 does not have). This "
      "was confirmed independently: in all of Zero's .text, the only "
      "sceSifCallRpc callers on the libcdvd RPC clients are "
      "sceCdLayerSearchFile, sceCdNcmdDiskReady, sceCdInit, "
      "sceCdDiskReady(_old), sceCdMmode and sceCdReadClock. "
      "`_sceCd_ncmd_prechk` has one caller (sceCdNcmdDiskReady), and "
      "`_sceCd_scmd_prechk` has two (sceCdMmode and sceCdReadClock). Zero "
      "evidently reads its data some other way, for example through IOP-side "
      "file I/O.\n")

    # ---- stats
    kinds = Counter(M.match[a]["kind"] for a in transferred)
    cand = len(syms) - len(AC5_LABEL_ISSUES)
    w("## Overall\n")
    w("- AC5 SDK labels: %d. %d are excluded as AC5 false positives (see "
      "below) and %d are matched but not transferred because AC5's label is "
      "wrong (see below). That leaves %d candidates."
      % (len(syms) + len(args.excluded), len(args.excluded),
         len(AC5_LABEL_ISSUES), cand))
    w("- Located in Zero and written to `sdk_symbols.json`: **%d (%.1f%%)**. "
      "That is %d distinct addresses; %d of them are also in "
      "`manual_symbols.json`."
      % (len(transferred), 100.0 * len(transferred) / cand, len(sdk_out),
         sum(1 for a in transferred if syms[a].get("manual"))))
    for k in ("exact", "near", "fuzzy", "callgraph", "vu0sig", "layout"):
        w("  - %s: %d" % (k, kinds.get(k, 0)))
    weak_new = [a for a in transferred if is_weak(M.match[a])
                and "(aligned)" not in syms[a]["lib"]]
    weak_all = [a for a in transferred
                if "(aligned)" in sdk_out["0x%08X" % M.match[a]["z"]]["lib"]]
    w("  - tagged `(aligned)`: %d. Of these, %d carry AC5's own tag and %d "
      "were added because the cross-match is weak (%s)."
      % (len(weak_all), len(weak_all) - len(weak_new), len(weak_new),
         ", ".join(sorted(syms[a]["name"] for a in weak_new)) or "-"))
    w("- Not located: %d (listed in the table below)."
      % (cand - len(transferred)))
    tot = Counter()
    for a in transferred:
        m = M.match[a]
        for k in ("cg_ok", "cg_wrap", "cg_miss", "cg_bad"):
            tot[k] += m[k]
    rpc = Counter(x.split()[0] + " " + x.split()[-1] for a in transferred
                  for x in M.match[a].get("rpc", ()))
    w("- RPC cross-check against `config/rpc_sids.json` (server ids bound "
      "and RPC function numbers called by AC5's SDK functions): %d server "
      "ids and %d function numbers found in the Zero counterparts, %d "
      "missing." % (rpc["sid ok"], rpc["fno ok"],
                    rpc["sid MISSING"] + rpc["fno MISSING"]))
    w("- Evidence over all matches: %d SDK references agree, %d go through a "
      "new wrapper, %d are no longer used, and %d conflict."
      % (tot["cg_ok"], tot["cg_wrap"], tot["cg_miss"], tot["cg_bad"]))
    noent = sum(1 for a in transferred if not M.match[a]["zero_entry"])
    w("- %d matches have no function in Zero's config-less partition. The "
      "matcher finds them by adding %d orphan starts (code nothing calls "
      "directly) to its own copy of the partition."
      % (noent, len(Z.orphans)))
    w("")
    bylib = defaultdict(lambda: [0, 0])
    for a, s in syms.items():
        if a in AC5_LABEL_ISSUES:
            continue
        b = "sifrpc.o (manual)" if s["lib"].startswith("sifrpc") \
            else lib_base(s["lib"])
        bylib[b][1] += 1
        if a in transferred:
            bylib[b][0] += 1
    w("| library | found / in AC5 | note |")
    w("|---|---|---|")
    for b in sorted(bylib, key=lambda k: -bylib[k][1]):
        w("| %s | %d / %d | %s |" % (b, bylib[b][0], bylib[b][1],
                                     LIB_NOTES.get(b, "")))
    w("")

    # ---- AC5 label problems
    w("## AC5 labels that look wrong (matched, not transferred)\n")
    for a, txt in sorted(AC5_LABEL_ISSUES.items()):
        z = M.match.get(a, {}).get("z")
        w("- AC5 %08X `%s`: %s. Zero counterpart: %s." % (
            a, syms[a]["name"], txt, ("%08X (%s)" % (z, quality(M.match[a])))
            if z else "not found"))
    w("- More generally, about 37 of AC5's libmpeg names, almost all of its "
      "libvu0 names, and its printf-family names are `(aligned)`. They were "
      "assigned by object order, never verified, and the libvu0 tail is "
      "visibly shifted (see above). They are transferred with the tag, but "
      "the names themselves may be wrong.")
    w("")
    w("## AC5 false positives (not transferred)\n")
    w("AC5's %d `netcnfif.a` / `libhttps.a` labels sit on 2-5 instruction "
      "C++ accessors in game code, and AC5 has no netcnf or SSL strings at "
      "all. They are leftovers of the original signature run, so they are "
      "not transferred. Labels: %s.\n"
      % (len(args.excluded), ", ".join(sorted(set(
          s["name"] for s in args.excluded.values())))))

    # ---- suspicious
    w("## Things to look at\n")
    sus = []
    for c in M.conflicts:
        sus.append("conflict: " + c)
    for a in sorted(transferred):
        m = M.match[a]
        if m.get("cg_bad"):
            sus.append("%s (Zero %08X): %s" % (syms[a]["name"], m["z"],
                                               "; ".join(m["cg_conflicts"])))
        if m.get("outlier"):
            sus.append("%s (Zero %08X, %s): displacement outlier: %s"
                       % (syms[a]["name"], m["z"], quality(m), m["outlier"]))
    for a in sorted(syms):
        for n in M.notes.get(a, {}):
            if n.startswith("dropped"):
                sus.append("%s (AC5 %08X): %s" % (syms[a]["name"], a, n))
    for a in sorted(transferred):
        m = M.match[a]
        if any(x.endswith("MISSING") for x in m.get("rpc", ())):
            sus.append("%s (Zero %08X): RPC check failed: %s"
                       % (syms[a]["name"], m["z"], ", ".join(m["rpc"])))
    for a in sorted(transferred):
        m = M.match[a]
        if "(aligned)" in syms[a]["lib"] or m["kind"] in STRONG:
            continue
        mr = re.search(r"runner-up ([0-9.]+)", m.get("note", ""))
        close = mr and m["score"] - float(mr.group(1)) < 0.1
        if m["score"] < 0.6 or close:
            sus.append("%s -> Zero %08X: %s%s; evidence ok/wrap/miss/bad %s"
                       "%s" % (syms[a]["name"], m["z"], quality(m),
                               (" (runner-up %s)" % mr.group(1))
                               if close else " (low body similarity)",
                               evid(m), ("; RPC " + ", ".join(m["rpc"]))
                               if m.get("rpc") else ""))
    for a in sorted(weak_new):
        m = M.match[a]
        sus.append("weak match of a non-aligned AC5 name: %s -> Zero %08X "
                   "(%s, ev %s)" % (syms[a]["name"], m["z"], quality(m),
                                    evid(m)))
    for a in sorted(transferred):
        m = M.match[a]
        if m.get("cg_wrapped"):
            why = []
            if "sceSifCallRpc" in m["cg_wrapped"]:
                why.append("fileio now wraps sceSifCallRpc in a retry loop at "
                           "0x003000C0")
            if "syscall 34" in m["cg_wrapped"]:
                why.append("StartThread is a wrapper function in SDK 3.0, not "
                           "a bare syscall stub")
            sus.append("%s (Zero %08X) now reaches %s through a new SDK 3.0 "
                       "helper%s" % (syms[a]["name"], m["z"],
                                     ", ".join(m["cg_wrapped"]),
                                     (" (" + "; ".join(why) + ")")
                                     if why else ""))
    dz = defaultdict(list)
    for a in transferred:
        dz[M.match[a]["z"]].append(a)
    for z, al in sorted(dz.items()):
        if len(al) > 1:
            sus.append("Zero %08X is the counterpart of %d AC5 addresses (%s: "
                       "%s)" % (z, len(al), ", ".join(
                           "%08X" % a for a in sorted(al)),
                           "/".join(sorted(set(syms[a]["name"]
                                               for a in al)))))
    for x in sus or ["none"]:
        w("- " + x)
    w("")

    # ---- full table
    w("## All AC5 SDK symbols\n")
    w("| AC5 addr | name | AC5 lib, class | words | Zero addr | match "
      "| ratio | ev ok/wrap/miss/bad | entry | notes |")
    w("|---|---|---|---|---|---|---|---|---|---|")
    for a in sorted(syms):
        s = syms[a]
        m = M.match.get(a)
        lib = lib_base(s["lib"]) + ", " + s["cls"]
        if m:
            extra = [m.get("note", "")]
            if m.get("outlier"):
                extra.append(m["outlier"])
            if a in AC5_LABEL_ISSUES:
                extra.insert(0, "NOT TRANSFERRED: AC5 label looks wrong")
            elif is_weak(m) and "(aligned)" not in s["lib"]:
                extra.insert(0, "weak, tagged (aligned)")
            w("| %08X | %s | %s | %d | %08X | %s | %.2f | %s | %s | %s |"
              % (a, s["name"], lib, M.alen[a], m["z"], quality(m),
                 m["ratio"], evid(m), "yes" if m["zero_entry"] else "no",
                 "; ".join(x for x in extra if x)))
        else:
            why = ABSENT_WHY.get(s["name"]) or "; ".join(M.notes.get(a, {}))
            w("| %08X | %s | %s | %d | - | not found | | | | %s |"
              % (a, s["name"], lib, M.alen[a], why))
    w("")
    with open(os.path.join(args.outdir, "coverage.md"), "w") as fp:
        fp.write("\n".join(L))


if __name__ == "__main__":
    main()
