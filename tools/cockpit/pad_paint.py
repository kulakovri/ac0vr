"""pad_paint.py IN.png OUT.png X0,Y0,X1,Y1 [--ref N] [--tol T] [--channels K] -- repaint one flat-coloured patch of
a texture as a padded cushion. Example (bug #122): a seat's headrest is a
single flat dark-red trapezoid in the cockpit texture (f5a_02_t.png, ~20 x 18 texels in the 1024 px original), so in
VR the pad behind the head is an unshaded slab.

  X0,Y0,X1,Y1  the box holding the patch, in texels of a texture N texels wide (--ref, default: the image's own
               width), so the same numbers work on the original and on an upscaled copy
  --tol T      texels within T (0-255, default 48) of the colour at the box centre form the patch (the flat colour);
               the rest of the box (the border, the bracket) is left alone
  --channels K vertical stitched channels across the pad (default 0: a plain pad)
The patch keeps its colour on average (within a few per cent); it gets: a rolled edge (darker towards the patch
boundary, over ~1/8 of its size), light from above (the top of the pad a little lighter), a stitched welt a few
texels inside the edge, optional channels, and a fine grain. Deterministic. Needs numpy and PIL (any Python with them). Writes OUT.png with IN's size and mode (RGB / RGBA)."""
import sys
import numpy as np
from PIL import Image


def main():
    a = sys.argv[1:]
    src, dst, box = a[0], a[1], [float(v) for v in a[2].split(',')]
    opt = lambda k, d: type(d)(a[a.index(k) + 1]) if k in a else d
    im = Image.open(src)
    mode = im.mode if im.mode in ('RGB', 'RGBA') else 'RGBA'
    px = np.asarray(im.convert(mode)).astype(np.float32)
    H, W = px.shape[:2]
    s = W / opt('--ref', float(W))
    x0, y0, x1, y1 = int(box[0] * s), int(box[1] * s), int(np.ceil(box[2] * s)), int(np.ceil(box[3] * s))
    sub = px[y0:y1, x0:x1, :3]
    h, w = sub.shape[:2]
    ref = sub[h // 2, w // 2]
    mask = np.abs(sub - ref).max(2) <= opt('--tol', 48.0)
    # the connected region around the centre (flood fill, 4-neighbours)
    reg = np.zeros_like(mask); st = [(h // 2, w // 2)]
    while st:
        y, x = st.pop()
        if 0 <= y < h and 0 <= x < w and mask[y, x] and not reg[y, x]:
            reg[y, x] = True
            st += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
    ys, xs = np.nonzero(reg)
    # distance to the region's edge (texels): brute force over the edge texels (small patches)
    rp = np.pad(reg, 1)                                            # (the box's sides are edges too)
    edge = reg & ~(rp[:-2, 1:-1] & rp[2:, 1:-1] & rp[1:-1, :-2] & rp[1:-1, 2:])
    ey, ex = np.nonzero(edge)
    dist = np.zeros((h, w), np.float32)
    for k in range(0, len(ys), 4096):
        yy, xx = ys[k:k + 4096, None], xs[k:k + 4096, None]
        dist[ys[k:k + 4096], xs[k:k + 4096]] = np.sqrt(((yy - ey[None]) ** 2 + (xx - ex[None]) ** 2).min(1))
    size = max(4.0, min(ys.max() - ys.min(), xs.max() - xs.min()))
    roll = size / 6.0
    t = np.clip(dist / roll, 0, 1)
    shade = 0.50 + 0.50 * np.sin(t * np.pi / 2) ** 0.8            # the rolled edge
    yn = (np.arange(h, dtype=np.float32)[:, None] - ys.min()) / max(1, ys.max() - ys.min())
    shade *= 1.08 - 0.16 * np.clip(yn, 0, 1)                       # lit from above
    cy, cx = ys.mean(), xs.mean()                                  # the pillow: a soft sheen over the middle
    ry, rx = max(1.0, (ys.max() - ys.min()) / 2), max(1.0, (xs.max() - xs.min()) / 2)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    shade *= 1.0 + 0.14 * np.clip(1.0 - ((yy - cy) / ry) ** 2 - ((xx - cx) / rx) ** 2, 0, 1)
    welt = size / 14.0
    shade *= 1.0 - 0.22 * np.exp(-((dist - welt) / max(0.6, size / 90.0)) ** 2)   # the stitched welt
    k = opt('--channels', 0)
    if k > 0:
        xn = (np.arange(w, dtype=np.float32)[None, :] - xs.min()) / max(1, xs.max() - xs.min())
        f = (xn * k) % 1.0
        d = np.minimum(f, 1 - f) * (xs.max() - xs.min()) / k
        shade *= (0.86 + 0.14 * np.clip(d / max(1.0, size / 40.0), 0, 1)) * np.ones_like(shade)
    rng = np.random.default_rng(122)
    shade *= 1.0 + 0.025 * rng.normal(0, 1, (h, w)).astype(np.float32)
    shade /= shade[reg].mean()                                     # the patch keeps its mean colour
    new = sub * shade[..., None]
    sub[reg] = np.clip(new[reg], 0, 255)
    px[y0:y1, x0:x1, :3] = sub
    Image.fromarray((px + 0.5).astype(np.uint8), mode).save(dst)
    print('pad_paint: %s: patch %d texels (%dx%d box at %d,%d), colour %s' % (dst, reg.sum(), w, h, x0, y0, ref.round()))


if __name__ == '__main__':
    main()
