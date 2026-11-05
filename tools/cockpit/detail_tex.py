"""detail_tex.py IN.png OUT.png SIZE [--grain A] [--speck A] -- a sharper copy of a small "flood" texture (a soft
tileable noise a modeller spreads over a whole cockpit tub): IN smoothly enlarged to SIZE x SIZE (its blotches kept as
the large-scale mottling, wrapping at the edges so a tiled copy has no seams), plus a fine matte-paint grain (value noise
of 2-16 texels, +-A of the mean, default 0.035) and sparse light / dark specks (A, default 0.02); the mean colour kept.
Used by build_f22a.sh (F22A_FLOOD=1, ck_seats 2026-10-28): FlightGear's F-22 maps its 128 px grey-blue-flood.png once
over the whole tub, consoles and housings (16 m2, 0.5 texel per cm: blotches 10-20 cm wide, nothing finer), so the
surfaces round the seat and the knees look like a smudge in VR. Deterministic. Needs numpy and PIL (any Python with them)."""
import sys
import numpy as np
from PIL import Image


def wrap_noise(n, cells, rng):
    """tileable smooth value noise: a cells x cells lattice, bicubic, wrapping"""
    g = rng.normal(0, 1, (cells, cells)).astype(np.float32)
    big = np.tile(g, (3, 3))
    im = Image.fromarray(big, mode='F').resize((3 * n, 3 * n), Image.BICUBIC)
    return np.asarray(im, dtype=np.float32)[n:2 * n, n:2 * n]


def main():
    a = sys.argv[1:]
    src, dst, n = a[0], a[1], int(a[2])
    opt = lambda k, d: float(a[a.index(k) + 1]) if k in a else d
    im = Image.open(src).convert('RGB')
    s = np.asarray(im, dtype=np.float32) / 255.0
    h, w = s.shape[:2]
    big = np.tile(s, (3, 3, 1))                                     # wrap: enlarge the 3 x 3 tiling, keep the middle
    up = np.stack([np.asarray(Image.fromarray(big[..., c], mode='F').resize((3 * n, 3 * n), Image.BICUBIC),
                              dtype=np.float32)[n:2 * n, n:2 * n] for c in range(3)], 2)
    rng = np.random.default_rng(22)
    g = opt('--grain', 0.035)
    grain = 1.0 + g * (0.5 * wrap_noise(n, max(2, n // 16), rng) + 0.35 * wrap_noise(n, max(2, n // 4), rng)
                       + 0.25 * wrap_noise(n, max(2, n // 2), rng))
    sp = opt('--speck', 0.02)
    r = rng.random((n, n)).astype(np.float32)
    speck = 1.0 + sp * 4 * ((r > 0.997).astype(np.float32) - (r < 0.003).astype(np.float32))
    out = up * (grain * speck)[..., None]
    out *= (s.reshape(-1, 3).mean(0) / out.reshape(-1, 3).mean(0))[None, None, :]     # the mean colour kept
    Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB').save(dst)
    print('detail_tex: %s (%d x %d from %d x %d), mean %s -> %s' % (dst, n, n, w, h, (s.reshape(-1, 3).mean(0) * 255).round(1),
                                                              (out.reshape(-1, 3).mean(0) * 255).round(1)))


if __name__ == '__main__':
    main()
