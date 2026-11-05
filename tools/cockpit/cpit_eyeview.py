"""cpit_eyeview.py -- (ck_align 2026-10-28) offline first-person view of a cockpit model (.cpit) from its design eye (or a shifted eye):
painter's algorithm, flat shading (no textures: textured parts grey), glass as a faint tint with its outline, the
runtime's combiner meshes (ACZ_VR_HUD_GLASS_MESH rule) outlined green, the boresight cross (red) and a 5-deg grid.

Usage: python -I cpit_eyeview.py FILE.cpit OUT.png [--eye DY,DZ] [--fov H,V] [--size W] [--names LIST] [--hide LIST]
                              [--title TEXT]
Needs numpy and PIL (any Python with them). Reads only the given file.
"""
import math, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, __import__('os').path.dirname(__file__))
import cpit_hudcheck as hc


def main():
    a = sys.argv[1:]
    f, out = a[0], a[1]
    dy, dz, fh, fv, W, names, hide, title = 0.0, 0.0, 70.0, 56.0, 900, hc.DEF_NAMES, '', ''
    i = 2
    while i < len(a):
        if a[i] == '--eye': dy, dz = (float(v) for v in a[i + 1].split(',')); i += 2
        elif a[i] == '--fov': fh, fv = (float(v) for v in a[i + 1].split(',')); i += 2
        elif a[i] == '--size': W = int(a[i + 1]); i += 2
        elif a[i] == '--names': names = a[i + 1]; i += 2
        elif a[i] == '--hide': hide = a[i + 1]; i += 2
        elif a[i] == '--title': title = a[i + 1]; i += 2
        else: i += 1
    th, tv = math.tan(math.radians(fh / 2)), math.tan(math.radians(fv / 2))
    H = int(W * tv / th)
    meshes = hc.load(f)
    eye = np.array([0.0, dy, dz])
    T, C, G, A = [], [], [], []
    for m in meshes:
        if not m['ntri'] or (m['flags'] & 16) or (hide and hc.name_match(m['name'], hide)):
            continue
        t = m['pos'].reshape(-1, 3, 3) - eye
        keep = (t[:, :, 2] < -0.02).all(1)
        if not keep.any():
            continue
        t = t[keep]
        tx, ty = t[:, :, 0] / -t[:, :, 2], t[:, :, 1] / -t[:, :, 2]
        vis = (tx.max(1) > -th) & (tx.min(1) < th) & (ty.max(1) > -tv) & (ty.min(1) < tv)
        if not vis.any():
            continue
        t = t[vis]
        T.append(t)
        glass = bool(m['flags'] & 1)
        c = np.array(m['rgba'][:3]) if m['tex'] == 0xFFFFFFFF else np.array([0.62, 0.62, 0.62])
        C.append(np.tile(c, (len(t), 1)))
        G.append(np.full(len(t), glass))
        c_ = m['c'] - eye; bl = float(np.linalg.norm(c_))
        ap = glass and hc.name_match(m['name'], names) and 0.05 < bl < 1.5 and -c_[2] > 0.966 * bl
        A.append(np.full(len(t), ap))
    T = np.concatenate(T); C = np.concatenate(C); G = np.concatenate(G); A = np.concatenate(A)
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
    n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-12
    L = np.array([0.3, 0.8, 0.5]); L /= np.linalg.norm(L)
    sh = 0.35 + 0.65 * np.abs(n @ L)
    depth = np.linalg.norm(T.mean(1), axis=1)
    order = np.argsort(-depth)
    def P(q):
        return ((q[0] / -q[2] / th * 0.5 + 0.5) * W, (0.5 - q[1] / -q[2] / tv * 0.5) * H)
    img = Image.new('RGB', (W, H), (150, 185, 225))
    d = ImageDraw.Draw(img, 'RGBA')
    for k in order:
        t = T[k]
        pts = [P(q) for q in t]
        if G[k]:
            if A[k]:
                d.polygon(pts, fill=(60, 255, 120, 40), outline=(0, 140, 40, 255))
            else:
                d.polygon(pts, fill=(200, 230, 255, 25))
            continue
        c = tuple(int(255 * min(1.0, C[k][j] * sh[k])) for j in range(3))
        d.polygon(pts, fill=c)
    # grid every 5 deg, boresight
    for g in range(-30, 31, 5):
        x = (math.tan(math.radians(g)) / th * 0.5 + 0.5) * W
        y = (0.5 - math.tan(math.radians(g)) / tv * 0.5) * H
        d.line([(x, 0), (x, H)], fill=(255, 255, 255, 50))
        d.line([(0, y), (W, y)], fill=(255, 255, 255, 50))
    cx, cy = W / 2, H / 2
    d.line([(cx - 14, cy), (cx + 14, cy)], fill=(255, 0, 0, 255), width=2)
    d.line([(cx, cy - 14), (cx, cy + 14)], fill=(255, 0, 0, 255), width=2)
    d.ellipse([cx - 2.5 * W / (2 * th) * math.tan(math.radians(1)), cy - 2.5 * W / (2 * th) * math.tan(math.radians(1)),
               cx + 2.5 * W / (2 * th) * math.tan(math.radians(1)), cy + 2.5 * W / (2 * th) * math.tan(math.radians(1))],
              outline=(255, 0, 0, 200))
    d.rectangle((0, 0, W, 16), fill=(0, 0, 0, 160))
    d.text((4, 2), title or '%s  eye %+.3f up %+.3f back  fov %gx%g  grid 5 deg' % (f.replace('\\', '/').split('/')[-1], dy, dz, fh, fv),
           fill=(255, 255, 0, 255))
    img.save(out)


if __name__ == '__main__':
    main()
