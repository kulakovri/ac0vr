"""preview.py OUT.png SCALE IN.png[@x,y,w,h][:tint] [IN2.png...]: side-by-side previews of RGBA textures, composited
over a dark checkerboard, each cropped (in its own pixels) and scaled to a common height with nearest neighbour.
SCALE applies to the first image; later ones are scaled so that their crop covers the same height (a 4x replacement
crop of 4x the size lines up with the 1x original). :tint multiplies the colours (hex RRGGBB, e.g. :40FF40 for the
green HUD) and stretches alpha to full, the way the HUD shows a coverage-only atlas; :raw shows alpha as grey."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import read_png, write_png  # noqa: E402


def load(spec):
    tint = None
    tail = spec.rsplit(':', 1)[-1]
    if ':' in spec and (tail == 'raw' or (len(tail) == 6 and all(c in '0123456789abcdefABCDEF' for c in tail))):
        spec, tint = spec.rsplit(':', 1)
    crop = None
    if '@' in spec:
        spec, c = spec.split('@')
        crop = [int(v) for v in c.split(',')]
    w, h, px = read_png(spec)
    if crop is None:
        crop = [0, 0, w, h]
    x0, y0, cw, ch = crop
    out = bytearray(cw * ch * 4)
    amax = max(px[3::4]) or 255
    for y in range(ch):
        for x in range(cw):
            s = ((y0 + y) * w + x0 + x) * 4
            out[(y * cw + x) * 4:(y * cw + x) * 4 + 4] = px[s:s + 4]
    if tint == 'raw':
        for i in range(cw * ch):
            a = out[4 * i + 3]
            out[4 * i:4 * i + 4] = bytes((a, a, a, 255))
    elif tint:
        tr, tg, tb = int(tint[0:2], 16), int(tint[2:4], 16), int(tint[4:6], 16)
        for i in range(cw * ch):
            out[4 * i] = min(255, out[4 * i] * tr // 105)
            out[4 * i + 1] = min(255, out[4 * i + 1] * tg // 105)
            out[4 * i + 2] = min(255, out[4 * i + 2] * tb // 105)
            out[4 * i + 3] = min(255, out[4 * i + 3] * 255 // amax)
    return cw, ch, out


def main():
    outp, scale = sys.argv[1], int(sys.argv[2])
    imgs = [load(s) for s in sys.argv[3:]]
    H = imgs[0][1] * scale
    tiles = []
    for cw, ch, px in imgs:
        s = max(1, H // ch)
        tiles.append((cw, ch, px, s))
    W = sum(cw * s for cw, ch, px, s in tiles) + 8 * (len(tiles) - 1)
    canvas = bytearray(W * H * 4)
    for i in range(W * H):
        x, y = i % W, i // W
        g = 48 if ((x >> 4) ^ (y >> 4)) & 1 else 24
        canvas[4 * i:4 * i + 4] = bytes((g, g, g, 255))
    ox = 0
    for cw, ch, px, s in tiles:
        for y in range(min(H, ch * s)):
            for x in range(cw * s):
                si = ((y // s) * cw + x // s) * 4
                a = px[si + 3]
                di = (y * W + ox + x) * 4
                for c in range(3):
                    canvas[di + c] = (px[si + c] * a + canvas[di + c] * (255 - a)) // 255
        ox += cw * s + 8
    write_png(outp, W, H, canvas, level=6)
    print(outp, W, H)


if __name__ == '__main__':
    main()
