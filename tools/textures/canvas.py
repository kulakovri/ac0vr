"""canvas.py: anti-aliased coverage masks and straight-alpha RGBA compositing in pure Python, for drawing small
texture art (button glyphs) at any scale.

Masks are flat lists of floats 0..1 (w*h). Coordinates are in pixels, y down, pixel centres at +0.5.
"""
import math


def polys(w, h, contours, ss=8):
    """non-zero winding fill of polygons; ss scanlines per pixel, exact horizontal coverage"""
    m = [0.0] * (w * h)
    edges = []
    for c in contours:
        for i in range(len(c)):
            (x0, y0), (x1, y1) = c[i], c[(i + 1) % len(c)]
            if y0 != y1:
                edges.append((x0, y0, x1, y1, 1 if y1 > y0 else -1))
    for sy in range(h * ss):
        y = (sy + 0.5) / ss
        xs = []
        for x0, y0, x1, y1, d in edges:
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append((x0 + (y - y0) * (x1 - x0) / (y1 - y0), d))
        if not xs:
            continue
        xs.sort()
        wind = 0
        row = (sy // ss) * w
        for k in range(len(xs) - 1):
            wind += xs[k][1]
            if wind == 0:
                continue
            a, b = max(0.0, xs[k][0]), min(float(w), xs[k + 1][0])
            if b <= a:
                continue
            ia, ib = int(a), int(b)
            if ia == ib:
                m[row + ia] += (b - a) / ss
                continue
            m[row + ia] += (ia + 1 - a) / ss
            for i in range(ia + 1, min(ib, w)):
                m[row + i] += 1.0 / ss
            if ib < w:
                m[row + ib] += (b - ib) / ss
    return [min(1.0, v) for v in m]


def shape(w, h, inside, ss=8):
    """coverage of an implicit shape: inside(x, y) -> bool, ss x ss samples per pixel"""
    m = [0.0] * (w * h)
    inv = 1.0 / (ss * ss)
    for py in range(h):
        for px in range(w):
            n = 0
            for j in range(ss):
                y = py + (j + 0.5) / ss
                for i in range(ss):
                    if inside(px + (i + 0.5) / ss, y):
                        n += 1
            m[py * w + px] = n * inv
    return m


def circle(w, h, cx, cy, r, ss=8):
    r2 = r * r
    return shape(w, h, lambda x, y: (x - cx) ** 2 + (y - cy) ** 2 <= r2, ss)


def rrect(w, h, x0, y0, x1, y1, r, ss=8):
    """rounded rectangle [x0, x1] x [y0, y1] with corner radius r (r = half height: a pill)"""
    def inside(x, y):
        if x < x0 or x > x1 or y < y0 or y > y1:
            return False
        cx = min(max(x, x0 + r), x1 - r)
        cy = min(max(y, y0 + r), y1 - r)
        return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
    return shape(w, h, inside, ss)


def sub(a, b):
    return [max(0.0, x - y) for x, y in zip(a, b)]


def mul(a, b):
    return [x * y for x, y in zip(a, b)]


def union(a, b):
    return [min(1.0, x + y - x * y) for x, y in zip(a, b)]


def scale(a, k):
    return [x * k for x in a]


def radial(w, h, cx, cy, r0, r1, a0, a1):
    """alpha a0 inside r0 falling linearly to a1 at r1 (soft halos)"""
    out = []
    for py in range(h):
        for px in range(w):
            d = math.hypot(px + 0.5 - cx, py + 0.5 - cy)
            if d <= r0:
                out.append(a0)
            elif d >= r1:
                out.append(a1)
            else:
                out.append(a0 + (a1 - a0) * (d - r0) / (r1 - r0))
    return out


class Image:
    """straight-alpha RGBA in floats 0..1"""
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = [[0.0, 0.0, 0.0, 0.0] for _ in range(w * h)]

    def paint(self, mask, rgb, alpha=1.0):
        """source-over of a solid colour (0..255 ints) through a coverage mask"""
        sr, sg, sb = rgb[0] / 255.0, rgb[1] / 255.0, rgb[2] / 255.0
        for i, m in enumerate(mask):
            sa = m * alpha
            if sa <= 0.0:
                continue
            p = self.px[i]
            da = p[3]
            oa = sa + da * (1.0 - sa)
            if oa <= 0.0:
                continue
            p[0] = (sr * sa + p[0] * da * (1.0 - sa)) / oa
            p[1] = (sg * sa + p[1] * da * (1.0 - sa)) / oa
            p[2] = (sb * sa + p[2] * da * (1.0 - sa)) / oa
            p[3] = oa

    def rgba8(self):
        out = bytearray(self.w * self.h * 4)
        for i, (r, g, b, a) in enumerate(self.px):
            out[4 * i] = int(round(min(1.0, r) * 255))
            out[4 * i + 1] = int(round(min(1.0, g) * 255))
            out[4 * i + 2] = int(round(min(1.0, b) * 255))
            out[4 * i + 3] = int(round(min(1.0, a) * 255))
        return out
