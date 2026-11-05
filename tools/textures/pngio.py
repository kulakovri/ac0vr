"""pngio.py: minimal PNG reading and writing in pure Python (no PIL on this machine).

read_png(path) -> (w, h, rgba bytearray)   8-bit RGBA/RGB/grey/grey+alpha/palette, non-interlaced
write_png(path, w, h, rgba)                 8-bit RGBA
"""
import struct
import zlib


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('%s: not a PNG' % path)
    p = 8
    idat = b''
    plte = None
    trns = None
    while p < len(d):
        n, tp = struct.unpack('>I4s', d[p:p + 8])
        body = d[p + 8:p + 8 + n]
        p += 12 + n
        if tp == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif tp == b'PLTE':
            plte = body
        elif tp == b'tRNS':
            trns = body
        elif tp == b'IDAT':
            idat += body
        elif tp == b'IEND':
            break
    if depth != 8 or interlace:
        raise ValueError('%s: only 8-bit non-interlaced PNGs (depth %d, interlace %d)' % (path, depth, interlace))
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    q = 0
    for y in range(h):
        f = raw[q]
        line = bytearray(raw[q + 1:q + 1 + stride])
        q += 1 + stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                line[i] = (line[i] + (((line[i - ch] if i >= ch else 0) + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                c = prev[i - ch] if i >= ch else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 255
        prev = line
        o = y * w * 4
        for x in range(w):
            if ctype == 6:
                out[o:o + 4] = line[x * 4:x * 4 + 4]
            elif ctype == 2:
                out[o:o + 3] = line[x * 3:x * 3 + 3]
                out[o + 3] = 255
            elif ctype == 0:
                out[o] = out[o + 1] = out[o + 2] = line[x]
                out[o + 3] = 255
            elif ctype == 4:
                out[o] = out[o + 1] = out[o + 2] = line[x * 2]
                out[o + 3] = line[x * 2 + 1]
            else:
                k = line[x]
                out[o:o + 3] = plte[k * 3:k * 3 + 3]
                out[o + 3] = trns[k] if trns and k < len(trns) else 255
            o += 4
    return w, h, out


def write_png(path, w, h, rgba, level=9):
    rows = []
    for y in range(h):
        rows.append(b'\x00' + bytes(rgba[y * w * 4:(y + 1) * w * 4]))

    def chunk(tp, b):
        return struct.pack('>I', len(b)) + tp + b + struct.pack('>I', zlib.crc32(tp + b) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(b''.join(rows), level)) + chunk(b'IEND', b''))
