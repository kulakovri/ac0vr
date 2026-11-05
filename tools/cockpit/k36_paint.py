"""k36_paint.py K-36DM.png OUT.png -- repaint the K-36DM ejection seat texture of the FlightGear MiG-29 9.12 (Models/
Submodels/K-36DM/K-36DM.png by Sergey "Mercenary_Mercury" Salov, GPL-3.0; the result is a derived work under the same
licence) at four times its size (512 -> 2048). Used by build_mig29a.sh (MIG29_K36_PAINT=1) and build_su27.sh
(SU27_K36=1), both through ac2cpit --tex-dir.

The original is a 2 KB palette picture: the dark brown of the cushions (57 43 43) with seven 1-texel pure black lines
every 20 texels on its left (x 0-137: the vertical channels of the headrest pad and the back cushion, objects
simple.head.a, K1, simple), the rest of the cushion brown flat, black for the structure and three swatches (dark grey,
light grey, maroon) for the arch, the booms and the arm restraints (ORR). In VR, at 0.2-0.6 m, the black lines on a flat
colour read as the seams of planks ("the headbox looks made of boards", ck_audit 2026-10-28, bug #168).
This paints it in the style of a quilted K-36 cushion, keeping the author's layout and colours (every UV island keeps
its mean colour within a few per cent):
  - channels (x 0-137): each 20-texel channel a padded tube -- lighter along its middle, falling off into a soft
    stitched groove (a dark 2-texel seam blurred over ~6 texels at 2048, with a faint stitch row beside it), slight
    horizontal creases, and a leather / canvas grain
  - the flat cushion brown: the same grain and a few per cent of mottling
  - black structure: a faint grain (no longer a pure 0 black void); the swatches: the same grain
Deterministic (seeded noise). Needs numpy and PIL (any Python with them).
"""
import sys
import numpy as np
from PIL import Image

S = 4


def blur1(a, r, axis):
    """box blur of radius r along one axis (repeated twice: a soft tent)"""
    for _ in range(2):
        c = np.cumsum(np.pad(a, [(r + 1, r) if k == axis else (0, 0) for k in range(a.ndim)], mode='edge'), axis=axis)
        hi = np.take(c, np.arange(2 * r + 1, c.shape[axis]), axis=axis)
        lo = np.take(c, np.arange(0, c.shape[axis] - 2 * r - 1), axis=axis)
        a = (hi - lo) / (2 * r + 1)
    return a


def noise(shape, scale, rng):
    """smooth value noise of feature size `scale` texels (bilinear up-sampled lattice)"""
    h, w = shape
    gh, gw = h // scale + 2, w // scale + 2
    g = rng.normal(0, 1, (gh, gw)).astype(np.float32)
    im = Image.fromarray(g, mode='F').resize((gw * scale, gh * scale), Image.BICUBIC)
    return np.asarray(im, dtype=np.float32)[:h, :w]


def main():
    src = np.asarray(Image.open(sys.argv[1]).convert('RGBA')).astype(np.float32) / 255.0
    H, W = src.shape[:2]
    big = np.asarray(Image.fromarray((src * 255).astype(np.uint8), 'RGBA').resize((W * S, H * S), Image.NEAREST),
                     dtype=np.float32) / 255.0
    h, w = big.shape[:2]
    rng = np.random.default_rng(36)
    rgb = big[..., :3].copy()
    lum = rgb.mean(2)
    xs = (np.arange(w) + 0.5) / S                              # source texel coordinates
    ys = (np.arange(h) + 0.5) / S
    X = np.broadcast_to(xs[None, :], (h, w)); Y = np.broadcast_to(ys[:, None], (h, w))
    brown = (lum > 0.08) & (lum < 0.3) & (big[..., 0] > big[..., 2] + 0.02)  # the cushion colour (57 43 43)
    seam_src = (lum < 0.02) & (X < 137.5)                     # the black channel lines
    chan = X < 137.5
    # channels: positions within each 20-texel channel (seams at x = 16, 36, ..., 136)
    t = ((X - 16.5) % 20.0) / 20.0                             # 0 at a seam centre, 0.5 mid-channel
    d = np.minimum(t, 1.0 - t) * 20.0                          # texels from the nearest seam
    pad = 0.80 + 0.32 * np.clip(np.sin(np.minimum(d / 10.0, 1.0) * np.pi / 2), 0, 1) ** 0.7
    groove = 1.0 - 0.55 * np.exp(-(d * S / 3.2) ** 2)          # the stitched groove: ~2 texels dark, soft
    stitch = 1.0 - 0.10 * ((np.abs(d - 1.6) < 0.35) & ((Y * S).astype(int) % 7 < 4))
    crease = 1.0 + 0.035 * noise((h, w), 48, rng) * np.clip(1.2 - np.abs(noise((h, w), 160, rng)), 0, 1)
    shade = pad * groove * stitch * crease
    m_chan = chan & (brown | seam_src)
    base_col = np.array([57, 43, 43], np.float32) / 255.0
    grain = 1.0 + 0.030 * rng.normal(0, 1, (h, w)).astype(np.float32) + 0.025 * noise((h, w), 6, rng)
    mott = 1.0 + 0.035 * noise((h, w), 96, rng)
    out = rgb.copy()
    ch = base_col[None, None, :] * (shade * grain * mott)[..., None]
    ch *= 1.0 / np.mean(shade[m_chan] * grain[m_chan] * mott[m_chan])          # keep the island's mean colour
    out[m_chan] = ch[m_chan]
    flat = brown & ~chan
    out[flat] = (rgb * (grain * mott)[..., None])[flat]
    dark = lum <= 0.02
    dark &= ~m_chan
    out[dark] = (0.035 + 0.012 * noise((h, w), 8, rng)[..., None] + 0.006 * rng.normal(0, 1, (h, w, 1)))[dark] * np.ones(3)
    rest = ~(m_chan | flat | dark)
    out[rest] = (rgb * (1.0 + 0.025 * noise((h, w), 8, rng) + 0.015 * rng.normal(0, 1, (h, w)))[..., None])[rest]
    o = np.concatenate([np.clip(out, 0, 1), big[..., 3:]], axis=2)
    Image.fromarray((o * 255 + 0.5).astype(np.uint8), 'RGBA').save(sys.argv[2])
    print('k36_paint: %s (%d x %d); channel island mean %s -> %s' % (sys.argv[2], w, h,
          np.round(rgb[m_chan].mean(0) * 255, 1), np.round(out[m_chan].mean(0) * 255, 1)))


if __name__ == '__main__':
    main()
