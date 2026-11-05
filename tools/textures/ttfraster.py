"""ttfraster.py: a small pure-Python TrueType outline reader and anti-aliased rasterizer (no PIL on this machine).

Font(path).outline(ch) -> (contours, advance) in font units (contours: lists of (x, y) points, curves flattened)
Coverage maps are built by Canvas (canvas.py) with the non-zero winding rule, supersampled.
Only simple glyphs and composite glyphs made of simple ones (offset only) are handled; enough for Latin letters.
"""
import struct


class Font:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        n = struct.unpack('>H', d[4:6])[0]
        self.tab = {}
        for i in range(n):
            tag, _, off, ln = struct.unpack('>4sIII', d[12 + 16 * i:28 + 16 * i])
            self.tab[tag.decode('latin1')] = (off, ln)
        head = self.tab['head'][0]
        self.upem = struct.unpack('>H', d[head + 18:head + 20])[0]
        self.loca_long = struct.unpack('>h', d[head + 50:head + 52])[0] == 1
        maxp = self.tab['maxp'][0]
        self.nglyphs = struct.unpack('>H', d[maxp + 4:maxp + 6])[0]
        hhea = self.tab['hhea'][0]
        self.ascent, self.descent = struct.unpack('>hh', d[hhea + 4:hhea + 8])
        self.nhmetrics = struct.unpack('>H', d[hhea + 34:hhea + 36])[0]
        self.cmap = self._cmap()

    def _cmap(self):
        d = self.d
        base = self.tab['cmap'][0]
        n = struct.unpack('>H', d[base + 2:base + 4])[0]
        for i in range(n):
            pid, eid, off = struct.unpack('>HHI', d[base + 4 + 8 * i:base + 12 + 8 * i])
            if pid == 3 and eid in (1, 10) and struct.unpack('>H', d[base + off:base + off + 2])[0] == 4:
                return self._fmt4(base + off)
        raise ValueError('no format-4 unicode cmap')

    def _fmt4(self, o):
        d = self.d
        segx2 = struct.unpack('>H', d[o + 6:o + 8])[0]
        seg = segx2 // 2
        ends = struct.unpack('>%dH' % seg, d[o + 14:o + 14 + segx2])
        p = o + 16 + segx2
        starts = struct.unpack('>%dH' % seg, d[p:p + segx2])
        deltas = struct.unpack('>%dh' % seg, d[p + segx2:p + 2 * segx2])
        roff_pos = p + 2 * segx2
        roffs = struct.unpack('>%dH' % seg, d[roff_pos:roff_pos + segx2])
        m = {}
        for s in range(seg):
            for c in range(starts[s], min(ends[s], 0x24F) + 1):
                if roffs[s] == 0:
                    g = (c + deltas[s]) & 0xFFFF
                else:
                    a = roff_pos + 2 * s + roffs[s] + 2 * (c - starts[s])
                    g = struct.unpack('>H', d[a:a + 2])[0]
                    if g:
                        g = (g + deltas[s]) & 0xFFFF
                m[c] = g
        return m

    def _loca(self, g):
        o = self.tab['loca'][0]
        if self.loca_long:
            a, b = struct.unpack('>II', self.d[o + 4 * g:o + 4 * g + 8])
        else:
            a, b = struct.unpack('>HH', self.d[o + 2 * g:o + 2 * g + 4])
            a, b = a * 2, b * 2
        return a, b

    def advance(self, g):
        o = self.tab['hmtx'][0]
        k = min(g, self.nhmetrics - 1)
        return struct.unpack('>H', self.d[o + 4 * k:o + 4 * k + 2])[0]

    def glyph_contours(self, g, dx=0, dy=0):
        a, b = self._loca(g)
        if a == b:
            return []
        d = self.d
        o = self.tab['glyf'][0] + a
        nc = struct.unpack('>h', d[o:o + 2])[0]
        if nc < 0:   # composite: components with x/y offsets only
            res = []
            p = o + 10
            while True:
                flags, gi = struct.unpack('>HH', d[p:p + 4])
                p += 4
                if flags & 1:
                    ax, ay = struct.unpack('>hh', d[p:p + 4])
                    p += 4
                else:
                    ax, ay = struct.unpack('>bb', d[p:p + 2])
                    p += 2
                if flags & 8:
                    p += 2
                elif flags & 0x40:
                    p += 4
                elif flags & 0x80:
                    p += 8
                res += self.glyph_contours(gi, dx + ax, dy + ay)
                if not flags & 0x20:
                    return res
        ends = struct.unpack('>%dH' % nc, d[o + 10:o + 10 + 2 * nc])
        npts = ends[-1] + 1
        p = o + 10 + 2 * nc
        ilen = struct.unpack('>H', d[p:p + 2])[0]
        p += 2 + ilen
        flags = []
        while len(flags) < npts:
            f = d[p]
            p += 1
            flags.append(f)
            if f & 8:
                r = d[p]
                p += 1
                flags += [f] * r
        flags = flags[:npts]
        xs, ys = [], []
        for coords, short, same in ((xs, 2, 16), (ys, 4, 32)):
            v = 0
            for f in flags:
                if f & short:
                    dv = d[p]
                    p += 1
                    v += dv if f & same else -dv
                elif not f & same:
                    v += struct.unpack('>h', d[p:p + 2])[0]
                    p += 2
                coords.append(v)
        contours = []
        s = 0
        for e in ends:
            pts = [(xs[i] + dx, ys[i] + dy, flags[i] & 1) for i in range(s, e + 1)]
            s = e + 1
            contours.append(self._flatten(pts))
        return contours

    @staticmethod
    def _flatten(pts, steps=8):
        """quadratic B-splines -> polyline (implied on-curve midpoints between off-curve points)"""
        n = len(pts)
        if not n:
            return []
        start = next((i for i in range(n) if pts[i][2]), None)
        if start is None:   # all off-curve: start at a midpoint
            a, b = pts[0], pts[1 % n]
            pts = [((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, 1)] + pts[1:] + [pts[0]]
            n = len(pts)
            start = 0
        seq = pts[start:] + pts[:start] + [pts[start]]
        out = [(seq[0][0], seq[0][1])]
        i = 1
        cur = seq[0]
        while i < len(seq):
            p = seq[i]
            if p[2]:
                out.append((p[0], p[1]))
                cur = p
                i += 1
                continue
            nxt = seq[i + 1] if i + 1 < len(seq) else seq[0]
            end = nxt if nxt[2] else ((p[0] + nxt[0]) / 2, (p[1] + nxt[1]) / 2, 1)
            for k in range(1, steps + 1):
                t = k / steps
                x = (1 - t) ** 2 * cur[0] + 2 * (1 - t) * t * p[0] + t * t * end[0]
                y = (1 - t) ** 2 * cur[1] + 2 * (1 - t) * t * p[1] + t * t * end[1]
                out.append((x, y))
            cur = end
            i += 1 if not nxt[2] else 2
        return out

    def text_contours(self, text, size_px, x0, baseline, tracking=0.0):
        """contours of a string in pixels: em = size_px, pen starts at x0, y grows downwards"""
        sc = size_px / self.upem
        res = []
        pen = x0
        for ch in text:
            g = self.cmap.get(ord(ch), 0)
            for c in self.glyph_contours(g):
                res.append([(pen + x * sc, baseline - y * sc) for x, y in c])
            pen += self.advance(g) * sc + tracking
        return res, pen - x0 - tracking

    def text_box(self, text, size_px):
        """ink bounding box (x0, y0, x1, y1) of a string drawn at pen 0, baseline 0"""
        cs, _ = self.text_contours(text, size_px, 0.0, 0.0)
        xs = [x for c in cs for x, y in c]
        ys = [y for c in cs for x, y in c]
        return min(xs), min(ys), max(xs), max(ys)
