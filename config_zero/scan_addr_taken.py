# Find code addresses that the game takes with lui/addiu (or lui/ori) pairs and that look like function starts,
# i.e. functions reached only through pointers (thread entries, interrupt/DMA handlers, callbacks).
# They become extra --ida-seeds "xref" entries (IDA provided these for AC5; Zero has no IDA database).
# Usage: PYTHONPATH=tools python -I config_zero/scan_addr_taken.py game/SLUS_213.46 config_zero/sdk/sdk_seeds.json config_zero/seeds_zero.json config_zero/ida_like_zero.json
import json, sys
sys.path.insert(0, "tools")
from ps2recomp.elf import ElfFile

elf_path, base_seeds, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
e = ElfFile(elf_path)
text = [s for s in e.sections if s.name == ".text"][0]
lo, hi = text.addr, text.addr + text.size

def w(a): return e.word(a)

def is_func_start(a):
    if a < lo or a >= hi or a & 3: return False
    ins = w(a)
    op = ins >> 26
    rs, rt = (ins >> 21) & 31, (ins >> 16) & 31
    imm = ins & 0xFFFF
    # addiu/daddiu $sp,$sp,-N
    if op in (0x09, 0x19) and rs == 29 and rt == 29 and imm & 0x8000: return True
    # "jr $ra; <delay>" right before, optionally followed by nop alignment padding -> a new function may
    # start here (0x143E50, a bsearch comparator, sits after one padding nop)
    b = a - 8
    for _ in range(4):
        if b < lo: return False
        if w(b) == 0x03E00008: return True
        if w(b + 4) != 0: return False
        b -= 4
    return False

found = set()
lui = {}
for a in range(lo, hi, 4):
    ins = w(a)
    op = ins >> 26
    rs, rt = (ins >> 21) & 31, (ins >> 16) & 31
    imm = ins & 0xFFFF
    if op == 0x0F:                      # lui rt, imm
        lui[rt] = (imm << 16, a)
        continue
    if op in (0x09, 0x0D, 0x19) and rs in lui:   # addiu / ori / daddiu rt, rs, imm
        base, at = lui[rs]
        if a - at <= 64:
            val = (base + (imm - 0x10000 if (op != 0x0D and imm & 0x8000) else imm)) & 0xFFFFFFFF
            if is_func_start(val): found.add(val)
    if op in (0x02, 0x03) or (op == 0 and (ins & 0x3F) in (0x08, 0x09)):   # j/jal/jr/jalr ends a basic block
        lui.clear()

# Code pointers stored as data: C++ vtables and handler tables in .data/.sdata/.rodata, constructor lists,
# and .eh_frame FDE pc_begin fields (exact function starts). A pointer counts when its target opens a stack
# frame or follows an unconditional block end (jr/j/b plus delay slot, optionally nop padding). Jump-table
# targets are left out: they are case labels inside a function. A wrong guess only splits a function, which
# the partitioner turns into musttail tail calls, so it costs speed, not correctness.
def block_end_before(a):
    b = a - 4
    for _ in range(4):
        if b < lo: return False
        if w(b) != 0: break
        b -= 4
    for c in (b - 4, b):
        x = w(c)
        op = x >> 26
        if x == 0x03E00008: return True                                                   # jr $ra
        if op == 0x02: return True                                                        # j
        if op == 0x04 and ((x >> 21) & 31) == 0 and ((x >> 16) & 31) == 0: return True   # b
        if op == 0 and (x & 0x3F) == 0x08: return True                                    # jr reg
    return False

def opens_frame(a):
    x = w(a)
    return (x >> 26) in (0x09, 0x19) and ((x >> 21) & 31) == 29 and ((x >> 16) & 31) == 29 and bool(x & 0x8000)

switch_targets = set()
if len(sys.argv) > 4:
    for sw in json.load(open(sys.argv[4]))["switches"]:
        switch_targets |= set(sw["targets"])
data_ptrs = set()
for s in e.sections:
    if s.name not in (".data", ".sdata", ".rodata", ".ctors", ".dtors", ".eh_frame") or not s.data:
        continue
    for o in range(0, len(s.data) - 3, 4):
        v = int.from_bytes(s.data[o:o + 4], "little")
        if lo <= v < hi and not v & 3 and v not in switch_targets and (opens_frame(v) or block_end_before(v)):
            data_ptrs.add(v)
found |= data_ptrs

seeds = json.load(open(base_seeds))
seeds.setdefault("xref", [])
have = set(seeds["xref"])
extra = sorted(found - have)
for a in (0x3896D0, 0x3896F4):         # _init / _fini, called through registers from crt0
    if a not in have and a not in extra: extra.append(a)
seeds["xref"] = sorted(have | set(extra))
seeds["//addr_taken"] = "added %d lui/addiu address-taken function starts (config_zero/scan_addr_taken.py)" % len(extra)
json.dump(seeds, open(out_path, "w"), indent=1)
print("address-taken function starts: %d new (total xref seeds %d)" % (len(extra), len(seeds["xref"])))
print("  data-section code pointers: %d" % len(data_ptrs))
for probe in (0x2FEC00, 0x2FE434, 0x11FFA8, 0x356708):
    print("  %08X %s" % (probe, "found" if probe in found else "NOT found"))
