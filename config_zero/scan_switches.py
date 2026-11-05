# Detect GCC/MIPS jump tables (switch statements) in a PS2 ELF and write them in the --ida-db JSON shape that
# ps2recomp understands ({"functions": [], "switches": [{"ea", "targets"}], "names": {}}). AC5 got this from IDA.
#
# Pattern (with variations in register use and order):
#     sltiu  $at, $idx, N            ; bound (optional)
#     sll    $t, $idx, 2
#     lui    $b, %hi(table)
#     [addiu $b, $b, %lo(table)]
#     addu   $b, $b, $t
#     lw     $r, %lo(table)($b)       ; or 0($b) when addiu supplied %lo
#     jr     $r
# Usage: PYTHONPATH=tools python -I config_zero/scan_switches.py game/SLUS_213.46 config_zero/ida_like_zero.json
import json, sys
sys.path.insert(0, "tools")
from ps2recomp.elf import ElfFile

elf_path, out_path = sys.argv[1], sys.argv[2]
e = ElfFile(elf_path)
text = [s for s in e.sections if s.name == ".text"][0]
tlo, thi = text.addr, text.addr + text.size
data_secs = [s for s in e.sections if s.name in (".rodata", ".data", ".sdata") and s.data]

def rd(a):
    for s in data_secs:
        if s.addr <= a and a + 4 <= s.addr + s.size:
            o = a - s.addr
            return int.from_bytes(s.data[o:o + 4], "little")
    return None

def w(a): return e.word(a)
def sx(i): return i - 0x10000 if i & 0x8000 else i

def dec(ins):
    return ins >> 26, (ins >> 21) & 31, (ins >> 16) & 31, (ins >> 11) & 31, ins & 0xFFFF, ins & 0x3F

switches = []
for jr in range(tlo, thi, 4):
    op, rs, rt, rd_, imm, fn = dec(w(jr))
    if not (op == 0 and fn == 0x08 and rs != 31):          # jr rs (not $ra)
        continue
    r = rs
    # walk back for "lw r, off(base)"
    base = off = None; lw_at = None
    for a in range(jr - 4, max(tlo, jr - 4 * 16), -4):
        o, s_, t_, d_, im, f = dec(w(a))
        if o == 0x23 and t_ == r:                           # lw r, im(s_)
            base, off, lw_at = s_, sx(im), a; break
        if (o == 0 and d_ == r and f in (0x21, 0x2D, 0x25)) or (o in (0x09, 0x19) and t_ == r):
            break                                           # r redefined by something else
    if base is None:
        continue
    # find "addu base, x, y" and the lui (+addiu) that forms the table address
    hi = lo_add = None
    regs_from = {base}
    table = None
    for a in range(lw_at - 4, max(tlo, lw_at - 4 * 16), -4):
        o, s_, t_, d_, im, f = dec(w(a))
        if o == 0 and f in (0x21, 0x2D) and d_ in regs_from:      # addu/daddu d, s, t
            regs_from |= {s_, t_}
            continue
        if o in (0x09, 0x19) and t_ in regs_from and s_ == t_:      # addiu b, b, lo
            lo_add = sx(im)
            continue
        if o == 0x0F and t_ in regs_from:                           # lui b, hi
            hi = im << 16
            table = (hi + (lo_add or 0) + off) & 0xFFFFFFFF
            break
    if table is None:
        continue
    # bound from a preceding sltiu (optional)
    n = None
    for a in range(lw_at - 4, max(tlo, lw_at - 4 * 24), -4):
        o, s_, t_, d_, im, f = dec(w(a))
        if o == 0x0B:                                               # sltiu
            n = im; break
    targets = []
    limit = n if n and n < 2048 else 1024
    for i in range(limit):
        v = rd(table + 4 * i)
        if v is None or not (tlo <= v < thi) or v & 3:
            break
        targets.append(v)
    if n and len(targets) != n:
        if len(targets) < 2: continue
    if len(targets) < 2:
        continue
    switches.append({"ea": jr, "targets": sorted(set(targets)), "table": table, "ncases": len(targets)})

out = {"meta": {"source": "config_zero/scan_switches.py"}, "segments": [], "functions": [], "switches": switches, "names": {}}
json.dump(out, open(out_path, "w"), indent=1)
print("jump tables: %d (targets %d)" % (len(switches), sum(len(s["targets"]) for s in switches)))
for s in switches:
    if s["ea"] - 0x2FE000 < 0x1000: print("  near printf core: jr %08X table %08X cases %d" % (s["ea"], s["table"], s["ncases"]))
