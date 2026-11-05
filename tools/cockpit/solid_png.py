#!/usr/bin/env python3
"""solid_png.py OUT.png R,G,B[,A] [SIZE] -- a one-colour RGBA PNG of our own (SIZE x SIZE, default 8; standard library only):
a replacement for a source texture that may not ship, placed first in ac2cpit's --tex-dir (build_f22a.sh F22A_ATLAS=0)."""
import struct, sys, zlib


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    c = [int(x) for x in sys.argv[2].split(',')]
    if len(c) == 3:
        c.append(255)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 8
    row = b'\0' + bytes(c) * n
    raw = row * n

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    with open(sys.argv[1], 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', n, n, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


if __name__ == '__main__':
    main()
