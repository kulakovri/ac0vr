#!/usr/bin/env python3
"""make_tiny_elf.py OUT.elf: write the CI's test program, a PS2 EE ELF of about 200 bytes made by this script.

No game bytes and no toolchain: the R5900 instructions are encoded here by hand. The program covers what the recompiler's
emitter has to handle in the small: a stack frame, calls, a counted loop, 64-bit arithmetic, a 128-bit load and store, an
indirect call and a syscall. It is never run; it exists so that the recompiler produces a real generated set for
the CI to compile the runtime against (ci/README.md).
"""
import struct
import sys

BASE = 0x00100000
ZERO, V0, V1, A0, A1, T0, SP, RA = 0, 2, 3, 4, 5, 8, 29, 31


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def r_type(rs, rt, rd, funct):
    return (rs << 21) | (rt << 16) | (rd << 11) | funct


def addiu(rt, rs, imm): return i_type(0x09, rs, rt, imm)
def lui(rt, imm): return i_type(0x0F, 0, rt, imm)
def ori(rt, rs, imm): return i_type(0x0D, rs, rt, imm)
def lq(rt, base, off): return i_type(0x1E, base, rt, off)
def sq(rt, base, off): return i_type(0x1F, base, rt, off)
def ld(rt, base, off): return i_type(0x37, base, rt, off)
def sd(rt, base, off): return i_type(0x3F, base, rt, off)
def bne(rs, rt, off): return i_type(0x05, rs, rt, off)
def jal(addr): return (0x03 << 26) | ((addr >> 2) & 0x03FFFFFF)
def jr(rs): return r_type(rs, 0, 0, 0x08)
def jalr(rs): return r_type(rs, 0, RA, 0x09)
def daddu(rd, rs, rt): return r_type(rs, rt, rd, 0x2D)
def syscall(): return 0x0C


NOP = 0
ADD64 = BASE + 0x48          # the second function, called directly
TWICE = BASE + 0x50          # the third, called through a register

START = [
    addiu(SP, SP, -32),      # _start: a frame with a 128-bit slot
    sd(RA, SP, 0),
    addiu(A0, ZERO, 3),      # a counted loop around a call
    addiu(A1, ZERO, 4),
    jal(ADD64),              # loop:
    NOP,
    addiu(A0, A0, -1),
    bne(A0, ZERO, -4 & 0xFFFF),
    NOP,
    sq(V0, SP, 16),          # 128-bit store and load
    lq(T0, SP, 16),
    lui(T0, BASE >> 16),     # an indirect call
    ori(T0, T0, TWICE & 0xFFFF),
    jalr(T0),
    NOP,
    addiu(V1, ZERO, 4),      # Exit
    syscall(),
    NOP,
]
assert len(START) * 4 == ADD64 - BASE
ADD64_BODY = [
    jr(RA),                  # add64: v0 = a0 + a1
    daddu(V0, A0, A1),
]
TWICE_BODY = [
    jr(RA),                  # twice: v0 = v0 + v0
    daddu(V0, V0, V0),
]
TEXT = b''.join(struct.pack('<I', w) for w in START + ADD64_BODY + TWICE_BODY)


def build():
    shstr = b'\0.text\0.shstrtab\0'
    ehsize, phsize, shsize = 52, 32, 40
    text_off = 0x1000
    shstr_off = text_off + len(TEXT)
    sh_off = (shstr_off + len(shstr) + 3) & ~3
    e_ident = b'\x7fELF' + bytes([1, 1, 1, 0]) + bytes(8)
    ehdr = e_ident + struct.pack('<HHIIIIIHHHHHH', 2, 8, 1, BASE, ehsize, sh_off, 0x20924001, ehsize, phsize, 1,
                                 shsize, 3, 2)
    phdr = struct.pack('<8I', 1, text_off, BASE, BASE, len(TEXT), len(TEXT), 5, 0x1000)
    out = bytearray(sh_off + 3 * shsize)
    out[0:ehsize] = ehdr
    out[ehsize:ehsize + phsize] = phdr
    out[text_off:text_off + len(TEXT)] = TEXT
    out[shstr_off:shstr_off + len(shstr)] = shstr
    sec = [struct.pack('<10I', *([0] * 10)),
           struct.pack('<10I', 1, 1, 6, BASE, text_off, len(TEXT), 0, 0, 4, 0),
           struct.pack('<10I', 7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0)]
    out[sh_off:sh_off + 3 * shsize] = b''.join(sec)
    return bytes(out)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: make_tiny_elf.py OUT.elf')
    with open(sys.argv[1], 'wb') as fp:
        fp.write(build())
