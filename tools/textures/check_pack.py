"""check_pack.py SETDIR DUMPDIR [DUMPDIR ...] [--worst N] [--sheet OUT.png]: fidelity of a replacement set against
the decodes it stands for (PS2_TEX_DUMP files of the same name): each replacement is box-filtered back to the
original size and compared texel by texel (RGB weighted by alpha, and alpha). A faithful upscale only adds detail
inside each texel, so its block averages stay those of the original: large differences mean a changed colour or
shape. Prints one line per texture (mean / 99th-percentile / max difference, in 0..255 units) and the totals;
--sheet writes the N worst as original | replacement pairs (nearest, over a checkerboard) for a look."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import read_png, write_png  # noqa: E402


def compare(o, r):
    (w, h, op), (W, H, rp) = o, r
    s = W // w
    n = s * s
    errs = []
    for y in range(h):
        for x in range(w):
            acc = [0, 0, 0, 0]
            for j in range(s):
                row = ((y * s + j) * W + x * s) * 4
                for i in range(s):
                    k = row + i * 4
                    a = rp[k + 3]
                    acc[0] += rp[k] * a
                    acc[1] += rp[k + 1] * a
                    acc[2] += rp[k + 2] * a
                    acc[3] += a
            k = (y * w + x) * 4
            oa = op[k + 3]
            ra = acc[3] / n
            e = abs(ra - oa)
            # premultiplied colour difference, so that colour under zero alpha does not count
            for c in range(3):
                e = max(e, abs(acc[c] / n / 255.0 - op[k + c] * oa / 255.0))
            errs.append(e)
    errs.sort()
    return sum(errs) / len(errs), errs[int(len(errs) * 0.99)], errs[-1]


def main():
    a = sys.argv
    setdir = a[1]
    dumps = [d for d in a[2:] if not d.startswith('--') and os.path.isdir(d)]
    worst = int(a[a.index('--worst') + 1]) if '--worst' in a else 8
    sheet = a[a.index('--sheet') + 1] if '--sheet' in a else None
    rows = []
    for fn in sorted(os.listdir(setdir)):
        if not fn.lower().endswith('.png'):
            continue
        base = '_'.join(fn.split('_')[:3]).split('.')[0] + '.png'
        src = next((os.path.join(d, base) for d in dumps if os.path.exists(os.path.join(d, base))), None)
        if not src:
            continue
        o, r = read_png(src), read_png(os.path.join(setdir, fn))
        if r[0] % o[0] or r[0] // o[0] != r[1] // o[1]:
            continue
        m, p99, mx = compare(o, r)
        rows.append((p99, m, mx, fn, src))
        print('%-36s mean %5.1f  p99 %5.1f  max %5.1f' % (fn, m, p99, mx))
    if not rows:
        print('nothing to compare')
        return
    rows.sort(reverse=True)
    print('%d replacements: mean of means %.2f, worst p99 %.1f (%s)' % (len(rows), sum(r[1] for r in rows) / len(rows),
                                                                     rows[0][0], rows[0][3]))
    if sheet:
        tiles = []
        for p99, m, mx, fn, src in rows[:worst]:
            (w, h, op), (W, H, rp) = read_png(src), read_png(os.path.join(setdir, fn))
            z = max(1, 128 // max(w, h))
            s = W // w
            tw, th = w * z, h * z
            img = bytearray(tw * 2 * th * 4 + 0)
            for y in range(th):
                for x in range(tw * 2):
                    if x < tw:
                        k = ((y // z) * w + x // z) * 4
                        px = op[k:k + 4]
                    else:
                        xx = (x - tw) * s // z
                        k = ((y * s // z) * W + xx) * 4
                        px = rp[k:k + 4]
                    g = 0x30 if ((x // 8) ^ (y // 8)) & 1 else 0x48
                    al = px[3]
                    d = (y * tw * 2 + x) * 4
                    for c in range(3):
                        img[d + c] = (px[c] * al + g * (255 - al)) // 255
                    img[d + 3] = 255
            tiles.append((tw * 2, th, img))
        W = max(t[0] for t in tiles)
        H = sum(t[1] + 4 for t in tiles)
        out = bytearray(W * H * 4)
        y0 = 0
        for tw, th, img in tiles:
            for y in range(th):
                out[((y0 + y) * W) * 4:((y0 + y) * W + tw) * 4] = img[y * tw * 4:(y + 1) * tw * 4]
            y0 += th + 4
        write_png(sheet, W, H, out)
        print(sheet)


if __name__ == '__main__':
    main()
