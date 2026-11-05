"""upscale_hud.py IN.png OUT.png [--scale 4] [--method xbrz|sharp|xbrz-bin|nearest|bilinear|esrgan] [--backproject N]
[--keep-rgb]

Upscale a texture (RGBA in the runtime's decode convention, e.g. from acz_disc.py extract or PS2_TEX_DUMP) for a
replacement pack, keeping the game's look:
- Coverage atlases (the HUD fonts and symbols: every visible texel has the same colour, the CLUT is an alpha ramp)
  are scaled as a grey coverage image; the output gets exactly the original colour everywhere and alpha = coverage
  times the original's top alpha, so colour and brightness stay those of the game.
- Grey text with a dark shadow (every visible texel grey, R = G = B; the menus' proportional font) with --method
  sharp: the grey level and the alpha are sharpened as two separate coverage maps.
- --backproject N (default 2) nudges the result so that each s x s block averages back to the original texel:
  stroke weights stay the game's while the edges stay smooth. For RGBA upscales (xBRZ, AI) alpha and the
  premultiplied colour are back-projected, so xBRZ's rounder edges stay inside each texel and no texel changes its
  colour or coverage (0 = off).
- Other images are scaled as RGBA (xBRZ argb mode); --palette snaps every pixel to the nearest original colour
  (instead of the back-projection).
Methods: xbrz (ext_tools/xbrz_build/xbrz_cli.exe, see build_xbrz.sh), esrgan (realesr-animevideov3 through
ext_tools/realesrgan, on the coverage image), nearest, bilinear (references).
"""
import os
import subprocess
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import read_png, write_png  # noqa: E402


def find_ext_tools():
    """the nearest parent of this tree that has an ext_tools folder, else the one next to the repository"""
    here = os.path.dirname(os.path.abspath(__file__))
    d = here
    while True:
        if os.path.isdir(os.path.join(d, 'ext_tools')):
            return os.path.join(d, 'ext_tools').replace(os.sep, '/')
        if os.path.dirname(d) == d:
            return os.path.normpath(os.path.join(here, '..', '..', '..', 'ext_tools')).replace(os.sep, '/')
        d = os.path.dirname(d)


EXT = find_ext_tools()
XBRZ = EXT + '/xbrz_build/xbrz_cli.exe'
ESRGAN = EXT + '/realesrgan/realesrgan-ncnn-vulkan.exe'


def coverage_colour(w, h, px):
    """the single RGB of every visible texel, or None"""
    col = None
    for i in range(w * h):
        if px[4 * i + 3]:
            c = (px[4 * i], px[4 * i + 1], px[4 * i + 2])
            if col is None:
                col = c
            elif max(abs(a - b) for a, b in zip(c, col)) > 2:
                return None
    return col


def run_xbrz(w, h, rgba, s, mode):
    with tempfile.TemporaryDirectory() as td:
        a, b = os.path.join(td, 'in.raw'), os.path.join(td, 'out.raw')
        with open(a, 'wb') as f:
            f.write(struct.pack('<II', w, h) + bytes(rgba))
        subprocess.run([XBRZ, str(s), a, b, mode], check=True)
        d = open(b, 'rb').read()
    return bytearray(d[8:])


def run_esrgan(w, h, rgba, s):
    """realesr-animevideov3 on an opaque image (GPU; keep runs short: the GPU is shared)"""
    with tempfile.TemporaryDirectory() as td:
        a, b = os.path.join(td, 'in.png'), os.path.join(td, 'out.png')
        write_png(a, w, h, rgba)
        subprocess.run([ESRGAN, '-i', a, '-o', b, '-n', 'realesr-animevideov3', '-s', str(s), '-f', 'png'],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        W, H, out = read_png(b)
    assert (W, H) == (w * s, h * s)
    return out


def nearest(w, h, rgba, s):
    out = bytearray(w * h * s * s * 4)
    W = w * s
    for y in range(h * s):
        for x in range(W):
            si = ((y // s) * w + x // s) * 4
            out[(y * W + x) * 4:(y * W + x) * 4 + 4] = rgba[si:si + 4]
    return out


def bilinear_grey(w, h, g, s):
    """texel-centre bilinear upscale of a float grid, edge clamped"""
    W, H = w * s, h * s
    out = [0.0] * (W * H)
    for y in range(H):
        fy = (y + 0.5) / s - 0.5
        y0 = int(fy // 1)
        ty = fy - y0
        ya, yb = min(max(y0, 0), h - 1), min(max(y0 + 1, 0), h - 1)
        for x in range(W):
            fx = (x + 0.5) / s - 0.5
            x0 = int(fx // 1)
            tx = fx - x0
            xa, xb = min(max(x0, 0), w - 1), min(max(x0 + 1, 0), w - 1)
            top = g[ya * w + xa] * (1 - tx) + g[ya * w + xb] * tx
            bot = g[yb * w + xa] * (1 - tx) + g[yb * w + xb] * tx
            out[y * W + x] = top * (1 - ty) + bot * ty
    return out


def backproject(w, h, cov, hi, s, iters):
    """hi += upsample(cov - boxdown(hi)), clamped to 0..1, so each s x s block averages back to the original"""
    W = w * s
    for _ in range(iters):
        err = [0.0] * (w * h)
        for y in range(h):
            for x in range(w):
                t = 0.0
                for j in range(s):
                    row = (y * s + j) * W + x * s
                    for i in range(s):
                        t += hi[row + i]
                err[y * w + x] = cov[y * w + x] - t / (s * s)
        # distribute the error inside each block in proportion to the room left (keeps zero areas zero and
        # full areas full, so no halo or grey veil appears around the strokes)
        for y in range(h):
            for x in range(w):
                e = err[y * w + x]
                if abs(e) < 1e-4:
                    continue
                idx = [(y * s + j) * W + x * s + i for j in range(s) for i in range(s)]
                room = [(1.0 - hi[k]) if e > 0 else hi[k] for k in idx]
                tot = sum(room)
                if tot <= 1e-6:
                    continue
                amount = e * s * s
                for k, r in zip(idx, room):
                    hi[k] = min(1.0, max(0.0, hi[k] + amount * r / tot))
    return hi


def grey_ramp(w, h, px):
    """True when every visible texel is a grey (R = G = B): text with an anti-aliased dark shadow or outline, whose
    palette is a ramp from transparent black through the shadow to the white core"""
    seen = False
    for i in range(w * h):
        if px[4 * i + 3]:
            r, g, b = px[4 * i], px[4 * i + 1], px[4 * i + 2]
            if max(r, g, b) - min(r, g, b) > 2:
                return False
            seen = True
    return seen


def upscale_map(a, w, h, cov, s, method):
    """one coverage map (floats 0..1) to s x its size with the chosen method; back-projected to its block averages"""
    bp = int(a[a.index('--backproject') + 1]) if '--backproject' in a else 2
    grey = bytearray(w * h * 4)
    for i, c in enumerate(cov):
        v = int(round(c * 255))
        grey[4 * i:4 * i + 4] = bytes((v, v, v, 255))
    if method == 'xbrz':
        hi8 = run_xbrz(w, h, grey, s, 'rgb')
        hi = [hi8[4 * i] / 255.0 for i in range(w * h * s * s)]
    elif method == 'xbrz-bin':
        # xBRZ is made for flat pixel art: give it the glyphs' shapes as two levels (coverage >= --level),
        # let it round the diagonals and corners, then the back-projection puts the anti-aliasing back
        lv = float(a[a.index('--level') + 1]) if '--level' in a else 0.4
        for i, c in enumerate(cov):
            v = 255 if c >= lv else 0
            grey[4 * i:4 * i + 4] = bytes((v, v, v, 255))
        hi8 = run_xbrz(w, h, grey, s, 'rgb')
        hi = [hi8[4 * i] / 255.0 for i in range(w * h * s * s)]
    elif method == 'esrgan':
        hi8 = run_esrgan(w, h, grey, s)
        hi = [hi8[4 * i] / 255.0 for i in range(w * h * s * s)]
    elif method == 'bilinear':
        hi = bilinear_grey(w, h, cov, s)
    elif method == 'sharp':
        # bilinear coverage through a contrast curve around 0.5 (like rendering a distance field): crisp
        # anti-aliased edges at the shapes' own outlines; the back-projection then restores faint strokes
        gain = float(a[a.index('--gain') + 1]) if '--gain' in a else 3.0
        hi = [min(1.0, max(0.0, (v - 0.5) * gain + 0.5)) if v > 0.0 else 0.0
              for v in bilinear_grey(w, h, cov, s)]
    else:
        hi = [cov[(y // s) * w + x // s] for y in range(h * s) for x in range(w * s)]
    if bp and method != 'nearest':
        hi = backproject(w, h, cov, hi, s, bp)
    return hi, bp


def backproject_rgba(w, h, px, out, s, iters):
    """an RGBA upscale (xBRZ, AI model) nudged so that each s x s block averages back to the original texel: alpha
    first, then the colour premultiplied by alpha (both with backproject's room-proportional spreading), so the
    upscaler's sharper edges stay inside each texel while every texel keeps its colour and coverage"""
    W, n = w * s, w * h * s * s
    alpha = [out[4 * i + 3] / 255.0 for i in range(n)]
    alpha = backproject(w, h, [px[4 * i + 3] / 255.0 for i in range(w * h)], alpha, s, iters)
    res = bytearray(n * 4)
    for i in range(n):
        res[4 * i + 3] = int(round(alpha[i] * 255))
    for c in range(3):
        # premultiplied channel as a fraction of the pixel's own alpha room: hi = c / 255 * alpha, bounded by alpha
        cur = [out[4 * i + c] / 255.0 * alpha[i] for i in range(n)]
        want = [px[4 * i + c] / 255.0 * px[4 * i + 3] / 255.0 for i in range(w * h)]
        for _ in range(iters):
            for y in range(h):
                for x in range(w):
                    idx = [(y * s + j) * W + x * s + i for j in range(s) for i in range(s)]
                    e = want[y * w + x] - sum(cur[k] for k in idx) / (s * s)
                    if abs(e) < 1e-4:
                        continue
                    room = [(alpha[k] - cur[k]) if e > 0 else cur[k] for k in idx]
                    tot = sum(room)
                    if tot <= 1e-6:
                        continue
                    for k, r in zip(idx, room):
                        cur[k] = min(alpha[k], max(0.0, cur[k] + e * s * s * r / tot))
        for i in range(n):
            res[4 * i + c] = int(round(min(255.0, cur[i] / alpha[i] * 255.0))) if alpha[i] > 0 else out[4 * i + c]
    return res


def main():
    a = sys.argv
    src, dst = a[1], a[2]
    s = int(a[a.index('--scale') + 1]) if '--scale' in a else 4
    method = a[a.index('--method') + 1] if '--method' in a else 'xbrz'
    w, h, px = read_png(src)
    col = coverage_colour(w, h, px)
    if col is not None:
        amax = max(px[3::4])
        cov = [px[4 * i + 3] / amax for i in range(w * h)]
        hi, bp = upscale_map(a, w, h, cov, s, method)
        out = bytearray(w * h * s * s * 4)
        for i, c in enumerate(hi):
            out[4 * i:4 * i + 4] = bytes((col[0], col[1], col[2], int(round(min(1.0, max(0.0, c)) * amax))))
        write_png(dst, w * s, h * s, out)
        print('%s: coverage atlas, colour %02X%02X%02X, top alpha %d, %s x%d, backproject %d -> %s'
              % (os.path.basename(src), col[0], col[1], col[2], amax, method, s, bp, dst))
        return
    if method == 'sharp' and grey_ramp(w, h, px):
        # grey text with a shadow: the grey level (the white core) and the alpha (core and shadow) are two nested
        # coverage maps; each is sharpened on its own, so core, shadow and their colours stay the game's
        lmax, amax = max(px[0::4]) or 1, max(px[3::4])
        lum, bp = upscale_map(a, w, h, [px[4 * i] / lmax for i in range(w * h)], s, method)
        alp, _ = upscale_map(a, w, h, [px[4 * i + 3] / amax for i in range(w * h)], s, method)
        out = bytearray(w * h * s * s * 4)
        for i in range(w * h * s * s):
            al = int(round(min(1.0, max(0.0, alp[i])) * amax))
            v = int(round(min(1.0, max(0.0, lum[i])) * lmax)) if al else 0
            out[4 * i:4 * i + 4] = bytes((v, v, v, al))
        write_png(dst, w * s, h * s, out)
        print('%s: grey text, top grey %d, top alpha %d, %s x%d, backproject %d -> %s'
              % (os.path.basename(src), lmax, amax, method, s, bp, dst))
        return
    if method == 'xbrz':
        out = run_xbrz(w, h, px, s, 'argb')
    elif method == 'esrgan':
        out = run_esrgan(w, h, px, s)
    else:
        out = nearest(w, h, px, s)
    bp = int(a[a.index('--backproject') + 1]) if '--backproject' in a else 2
    if '--palette' in a:
        bp = 0
        pal = sorted(set(bytes(px[4 * i:4 * i + 4]) for i in range(w * h)))
        cache = {}
        for i in range(w * h * s * s):
            c = bytes(out[4 * i:4 * i + 4])
            if c not in cache:
                cache[c] = min(pal, key=lambda p: sum((p[k] - c[k]) ** 2 for k in range(4)))
            out[4 * i:4 * i + 4] = cache[c]
    elif bp and method != 'nearest':
        out = backproject_rgba(w, h, px, out, s, bp)
    write_png(dst, w * s, h * s, out)
    print('%s: RGBA, %s x%d%s -> %s' % (os.path.basename(src), method, s,
                                        ', palette' if '--palette' in a else ', backproject %d' % bp if bp else '',
                                        dst))


if __name__ == '__main__':
    main()
