# Seed plausible function starts inside the .text regions that the recompiler's partition leaves uncovered.
#
# Functions reached only through pointers the other scanners cannot see (computed in registers, stored into
# .bss at run time, split lui/addiu pairs) end up uncovered and are reported at run time as "indirect branch
# to X, which no recompiled function claims". Seeding every plausible start inside the uncovered gaps
# removes that whole class at once. A wrong seed only splits a function, which the partitioner turns into
# musttail tail calls, so it costs speed, not correctness. Candidates whose straight-line code hits an
# invalid encoding before a return are rejected, so data embedded in .text stays out.
#
# Usage: PYTHONPATH=tools python -I config_zero/scan_gaps.py game/SLUS_213.46 config_zero/ida_like_zero.json \
#            config_zero/seeds_zero.json config_zero/seeds_zero.json
import json, sys
sys.path.insert(0, "tools")
from ps2recomp.elf import ElfFile
from ps2recomp.analysis import Program
from ps2recomp import r5900

elf_path, ida_db, seeds_in, seeds_out = sys.argv[1:5]
elf = ElfFile(elf_path)
seeds = json.load(open(seeds_in))
quiet = lambda *a, **k: None


def build(extra):
    prog = Program(elf, ida_db, log=quiet)
    prog.seed()
    for key in ("xref", "ptr"):
        for a in seeds.get(key, ()):
            if prog.in_text(a):
                prog.entries.add(a)
    for a in extra:
        prog.entries.add(a)
    prog.partition()
    return prog


def unconditional_end(ins):
    if ins.cat == r5900.CAT_JUMPR and ins.name == "JR":
        return True
    if ins.cat == r5900.CAT_JUMP and ins.name == "J":
        return True
    return ins.name == "BEQ" and ins.rs == 0 and ins.rt == 0


def plausible(prog, a, gap_end):
    """Straight-line walk from a: accept when a return/jump appears before an invalid word or the gap end."""
    x = a
    while x < gap_end:
        ins = prog.insns[x]
        if ins.cat == r5900.CAT_INVALID:
            return False
        if unconditional_end(ins):
            ds = prog.insns.get(x + 4)
            return ds is not None and ds.cat not in (r5900.CAT_INVALID, r5900.CAT_JUMP, r5900.CAT_JUMPR,
                                                     r5900.CAT_BRANCH, r5900.CAT_BRANCH_LIKELY)
        x += 4
    return False


def opens_frame(ins):
    return ins.name in ("ADDIU", "DADDIU") and ins.rs == 29 and ins.rt == 29 and ins.simm < 0


extra = set()
# Global pass: anywhere in .text, a frame opener right after an unconditional block end (+ nop padding) is a
# function start, even when some other function already owns it (e.g. reached by a fallthrough).
probe = Program(elf, ida_db, log=quiet)
for lo, hi in probe.text:
    for x in range(lo + 8, hi, 4):
        if not opens_frame(probe.insns[x]):
            continue
        b = x - 4
        while b > lo and probe.insns[b].is_nop:
            b -= 4
        if unconditional_end(probe.insns[b - 4]) and plausible(probe, x, hi):
            extra.add(x)
print("global frame-opener starts: %d" % len(extra))
for rnd in range(1, 16):
    prog = build(extra)
    covered = set(prog.owner)
    total = sum(hi - lo for lo, hi in prog.text) // 4
    new = set()
    for lo, hi in prog.text:
        a = lo
        while a < hi:
            if a in covered:
                a += 4
                continue
            g0 = a
            while a < hi and a not in covered:
                a += 4
            g1 = a
            # first real instruction of the gap
            s = g0
            while s < g1 and prog.insns[s].is_nop:
                s += 4
            if s < g1 and plausible(prog, s, g1):
                new.add(s)
            # later starts inside the gap: a frame opener right after an unconditional block end (+ padding)
            for x in range(s + 4, g1, 4):
                if not opens_frame(prog.insns[x]):
                    continue
                b = x - 4
                while b > g0 and prog.insns[b].is_nop:
                    b -= 4
                if b - 4 >= g0 and unconditional_end(prog.insns[b - 4]) and plausible(prog, x, g1):
                    new.add(x)
    new -= extra
    print("round %d: coverage %.2f%% (%d functions), %d new gap seeds"
          % (rnd, 100.0 * len(covered) / total, len(prog.functions), len(new)))
    if not new:
        break
    extra |= new

seeds["xref"] = sorted(set(seeds.get("xref", [])) | extra)
seeds["//gaps"] = "added %d starts inside uncovered .text gaps (config_zero/scan_gaps.py)" % len(extra)
json.dump(seeds, open(seeds_out, "w"), indent=1)
print("gap seeds: %d (total xref seeds %d)" % (len(extra), len(seeds["xref"])))
for probe in (0x17B880, 0x143E50):
    print("  %08X %s" % (probe, "seeded" if probe in seeds["xref"] else "not seeded"))
