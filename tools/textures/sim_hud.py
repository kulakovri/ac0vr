"""sim_hud.py ORIG.png REPL.png OUT.png u0,v0,w,h [--zoom 5.2] [--up 4] [--tint 40FF40] [--offset 0.37]

CPU model of runtime/shaders/gs.frag for one point-sampled sprite showing the texel rectangle u0,v0,w,h of a texture:
left, the game's texture through the original path (texel coordinate snapped to the native pixel centre, nearest
texel); right, the replacement image through the rep_scale path (the same texel per native pixel, inside it the
fragment's own position, "sharp bilinear" within that texel's block of the larger image). --zoom = render pixels
per texel (4x internal resolution x 1.3 VR HUD scale = 5.2), --up = the internal resolution factor (native pixel =
up render pixels), --offset = sub-pixel position of the sprite. The
texture's alpha is shown as coverage of the tint colour over a dark background, the way the HUD looks.
This checks the sampling arithmetic, not the GPU: confirm with per-eye dumps from a run.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import read_png, write_png  # noqa: E402


def main():
    a = sys.argv
    ow, oh, opx = read_png(a[1])
    rw, rh, rpx = read_png(a[2])
    S = rw // ow
    u0, v0, w, h = [int(x) for x in a[4].split(',')]
    zoom = float(a[a.index('--zoom') + 1]) if '--zoom' in a else 5.2
    up = int(a[a.index('--up') + 1]) if '--up' in a else 4
    tint = a[a.index('--tint') + 1] if '--tint' in a else '40FF40'
    off = float(a[a.index('--offset') + 1]) if '--offset' in a else 0.37
    tr, tg, tb = int(tint[0:2], 16), int(tint[2:4], 16), int(tint[4:6], 16)
    amax = max(opx[3::4]) or 255
    W, H = int(w * zoom) + 2 * up, int(h * zoom) + 2 * up

    def tc_at(x, y):           # render-pixel centre -> texel coordinate (continuous)
        return u0 + (x + 0.5 - up - off) / zoom, v0 + (y + 0.5 - up - off) / zoom

    def orig(x, y):
        nx, ny = math.floor(x / up), math.floor(y / up)
        u, v = tc_at(nx * up + up / 2 - 0.5, ny * up + up / 2 - 0.5)
        if not (u0 <= u < u0 + w and v0 <= v < v0 + h):
            return None
        i, j = int(math.floor(u + 1 / 32)), int(math.floor(v + 1 / 32))
        return opx[(j * ow + i) * 4 + 3] / amax

    def fetch(i, j):
        i, j = min(max(i, 0), rw - 1), min(max(j, 0), rh - 1)
        return rpx[(j * rw + i) * 4 + 3] / amax

    def rep(x, y):
        nx, ny = math.floor(x / up), math.floor(y / up)
        us, vs = tc_at(nx * up + up / 2 - 0.5, ny * up + up / 2 - 0.5)     # snapped, as the game's point filter
        if not (u0 <= us < u0 + w and v0 <= vs < v0 + h):
            return None
        u, v = tc_at(x, y)
        fw = S / zoom
        out = []
        for t, ts in ((u, us), (v, vs)):
            p0 = math.floor(ts + 1 / 32)
            c = (p0 + min(1.0, max(0.0, t - p0))) * S - 0.5
            lo = math.floor(c)
            c = lo + min(1.0, max(0.0, (c - lo - 0.5) / max(fw, 1 / 64) + 0.5))
            c = min(max(c, p0 * S), p0 * S + S - 1)
            lo = math.floor(c)
            out.append((lo, c - lo))
        (lx, fx), (ly, fy) = out
        lx, ly = int(lx), int(ly)
        top = fetch(lx, ly) * (1 - fx) + fetch(lx + 1, ly) * fx
        bot = fetch(lx, ly + 1) * (1 - fx) + fetch(lx + 1, ly + 1) * fx
        return top * (1 - fy) + bot * fy

    img = bytearray((2 * W + 8) * H * 4)
    TW = 2 * W + 8
    for y in range(H):
        for x in range(TW):
            g = 22
            col = (g, g, g)
            if x < W:
                c = orig(x, y)
            elif x >= W + 8:
                c = rep(x - W - 8, y)
            else:
                c, col = None, (90, 90, 90)
            if c is not None:
                col = tuple(int(g + (t - g) * c) for t in (tr, tg, tb))
            img[(y * TW + x) * 4:(y * TW + x) * 4 + 4] = bytes(col + (255,))
    write_png(a[3], TW, H, img, level=6)
    print(a[3], TW, H, 'scale %dx, %.2f render px per texel' % (S, zoom))


if __name__ == '__main__':
    main()
