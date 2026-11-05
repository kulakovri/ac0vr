"""j35j_paint.py DRAKENC.png DRAKEN_INTERNAL.ac OUT.png -- finish the two unpainted parts of the J35J cockpit texture
(FlightGear SaabJ35F, Model/drakenc.png, GPL-2.0; the result is a derived work under the same licence). Run by
build_j35j.sh (J35J_PAINT=1, the default).

The model's author painted the cockpit over an ambient-occlusion bake but left two texture islands as the raw bake:
plain grey (texel 180 / 130 with the soft occlusion shading) -- the ejection seat (Seat: bucket, back rest; its pan
cushion only a flat brown, 107 88 51) and the cockpit tub around it (draken_cockpit_unlit: foot wells, knee panels,
the floor beside and behind the seat). In the VR cockpit, with no pilot body, that is what the head sees looking down:
"a plain box, a flat brown floor with white walls" (bug #50; the upstream Resources/TODO lists "More detailed seat"
and "Improve textures eg. screws, markings"). This paints them in the texture's own style, procedurally, on the
model's real geometry (every texel of the islands is traced back to its face and 3D position):
  - seat bucket: grey-green paint, screws along the top edges of its walls, paint worn to the metal on the exposed
    edges, grime in the inner corners; the author's "VARNING KRUT / ROR EJ MEKANISMEN" seat placard (from the head
    box) repeated on the inner walls of the pan
  - cushions (the pan where the author painted it brown, the back rest): olive drab canvas (FS 34087-like, as on
    Swedish seat cushions), stitched channels and border seam, and the unbuckled harness in khaki webbing with steel
    fittings: lap straps and the crotch strap lying on the pan, shoulder straps down the back rest
  - tub: the cockpit's own paint (56 67 51, the texel of the painted panels), rivet rows along its corners, worn
    edges, scuffed floor at the heel rests; the author's dark hand-painted areas stay (tinted to the paint)
The occlusion of the bake is kept as the shading (texel / bake level). Texels the author painted (placards, red
handles, the green head box, black) are not touched; neither is anything outside the two islands. Deterministic (no
randomness but hashes); pure Python (zlib) like ac2cpit.py, ~12 s.
"""
import math, struct, sys, zlib

# ---------------------------------------------------------------- PNG

def png_read(path):
    d = open(path, 'rb').read()
    if d[:8] != b'\x89PNG\r\n\x1a\n': raise SystemExit('not a PNG: ' + path)
    pos, idat = 8, []
    w = h = depth = ctype = inter = 0
    while pos < len(d):
        n, t = struct.unpack('>I4s', d[pos:pos + 8]); body = d[pos + 8:pos + 8 + n]; pos += 12 + n
        if t == b'IHDR': w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT': idat.append(body)
        elif t == b'IEND': break
    if depth != 8 or ctype not in (2, 6) or inter: raise SystemExit('expected an 8-bit RGB(A) PNG: ' + path)
    ch = 4 if ctype == 6 else 3
    raw = zlib.decompress(b''.join(idat))
    stride = w * ch
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        if f == 1:
            for i in range(ch, stride): line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif f == 3:
            for i in range(stride): line[i] = (line[i] + ((line[i - ch] if i >= ch else 0) + prev[i]) // 2) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0; b = prev[i]; c = prev[i - ch] if i >= ch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        o = y * w * 4
        if ch == 4: out[o:o + w * 4] = line
        else:
            for c_ in range(3): out[o + c_:o + w * 4:4] = line[c_::3]
            out[o + 3:o + w * 4:4] = bytes((255,)) * w
        prev = line
    return w, h, out


def png_write(path, w, h, px):
    rows = b''.join(b'\0' + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, body):
        return struct.pack('>I', len(body)) + t + body + struct.pack('>I', zlib.crc32(t + body) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))

# ---------------------------------------------------------------- AC3D (the objects' faces in the FlightGear frame)

AC_TO_FG = ((1, 0, 0), (0, 0, -1), (0, 1, 0))           # AC3D (x, y up, z) -> FG (x aft, y right, z up)


def mv(m, v): return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3)]
def mm(a, b): return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def ac_objects(path, names):
    """{name: [(fg points, uv points (texture pixels of a 1x1 texture: u, 1 - v))]} of the named objects"""
    lines = open(path, 'r', encoding='latin-1').read().split('\n')
    res = {}
    i = 0
    ident = [[1, 0, 0], [0, 1, 0], [0, 0, 1]]

    def obj(R, T):
        nonlocal i
        while i < len(lines) and not lines[i].strip(): i += 1
        i += 1                                             # OBJECT line
        name, loc, rot, verts, surfs, kids = '', [0, 0, 0], ident, [], [], 0
        while i < len(lines):
            t = lines[i].split()
            if not t: i += 1; continue
            k = t[0]
            if k == 'name': name = lines[i].strip()[5:].strip().strip('"'); i += 1
            elif k == 'data':
                n = int(t[1]); i += 1; got = 0
                while i < len(lines) and got < n: got += len(lines[i].rstrip('\r')) + 1; i += 1
            elif k == 'loc': loc = [float(x) for x in t[1:4]]; i += 1
            elif k == 'rot': v = [float(x) for x in t[1:10]]; rot = [v[0:3], v[3:6], v[6:9]]; i += 1
            elif k == 'numvert':
                n = int(t[1]); i += 1
                for _ in range(n): verts.append([float(x) for x in lines[i].split()[:3]]); i += 1
            elif k == 'numsurf':
                n = int(t[1]); i += 1
                for _ in range(n):
                    flags = int(lines[i].split()[1], 0); i += 1
                    while not lines[i].strip().startswith('refs'): i += 1
                    nr = int(lines[i].split()[1]); i += 1
                    refs = []
                    for _ in range(nr):
                        a = lines[i].split(); refs.append((int(a[0]), float(a[1]), float(a[2]))); i += 1
                    surfs.append((flags, refs))
            elif k == 'kids': kids = int(t[1]); i += 1; break
            else: i += 1
        R2 = mm(R, rot); T2 = [T[j] + mv(R, loc)[j] for j in range(3)]
        if name in names:
            wv = [mv(AC_TO_FG, [a + b for a, b in zip(mv(R2, v), T2)]) for v in verts]
            res[name] = [([wv[r[0]] for r in refs], [(r[1], 1.0 - r[2]) for r in refs])
                         for flags, refs in surfs if (flags & 15) == 0 and len(refs) >= 3]
        for _ in range(kids):
            while i < len(lines) and not lines[i].strip(): i += 1
            if i >= len(lines) or not lines[i].startswith('OBJECT'): break
            obj(R2, T2)

    while i < len(lines):
        if lines[i].startswith('OBJECT'): obj(ident, [0, 0, 0])
        else: i += 1
    return res

# ---------------------------------------------------------------- small vector helpers and noise

def sub(a, b): return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
def add(a, b): return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]
def mul(a, s): return [a[0] * s, a[1] * s, a[2] * s]
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def norm(a):
    ln = math.sqrt(dot(a, a)) or 1.0
    return [a[0] / ln, a[1] / ln, a[2] / ln]


def newell(pts):
    n = [0.0, 0.0, 0.0]
    for k in range(len(pts)):
        a, b = pts[k], pts[(k + 1) % len(pts)]
        n[0] += (a[1] - b[1]) * (a[2] + b[2]); n[1] += (a[2] - b[2]) * (a[0] + b[0]); n[2] += (a[0] - b[0]) * (a[1] + b[1])
    return norm(n)


def poly_area3(pts):
    s = [0.0, 0.0, 0.0]
    for k in range(1, len(pts) - 1): s = add(s, cross(sub(pts[k], pts[0]), sub(pts[k + 1], pts[0])))
    return 0.5 * math.sqrt(dot(s, s))


def chamfer(x0, y0, x1, y1, inside, W):
    """two-pass chamfer distance (texels) of the texels of a rectangle for which inside(texel index) holds to the
    nearest one for which it does not; row-major list over the rectangle"""
    dw, hh = x1 - x0, y1 - y0
    D = [1e9 if inside((y + y0) * W + x + x0) else 0.0 for y in range(hh) for x in range(dw)]
    for y in range(hh):
        for x in range(dw):
            i = y * dw + x
            if D[i] == 0: continue
            v = D[i]
            if x > 0: v = min(v, D[i - 1] + 1)
            if y > 0:
                v = min(v, D[i - dw] + 1)
                if x > 0: v = min(v, D[i - dw - 1] + 1.414)
                if x < dw - 1: v = min(v, D[i - dw + 1] + 1.414)
            D[i] = v
    for y in range(hh - 1, -1, -1):
        for x in range(dw - 1, -1, -1):
            i = y * dw + x
            if D[i] == 0: continue
            v = D[i]
            if x < dw - 1: v = min(v, D[i + 1] + 1)
            if y < hh - 1:
                v = min(v, D[i + dw] + 1)
                if x < dw - 1: v = min(v, D[i + dw + 1] + 1.414)
                if x > 0: v = min(v, D[i + dw - 1] + 1.414)
            D[i] = v
    return D


def seg_dist(p, a, b):
    ab = sub(b, a); t = dot(sub(p, a), ab) / (dot(ab, ab) or 1e-12)
    t = min(max(t, 0.0), 1.0)
    d = sub(p, add(a, mul(ab, t)))
    return math.sqrt(dot(d, d))


def smooth(e0, e1, x):
    t = min(max((x - e0) / (e1 - e0), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def hsh(ix, iy, seed):
    h = (ix * 374761393 + iy * 668265263 + seed * 1442695041) & 0xffffffff
    h = ((h ^ (h >> 13)) * 1274126177) & 0xffffffff
    return ((h ^ (h >> 16)) & 0xffff) / 65535.0


def vnoise(x, y, seed):
    ix, iy = math.floor(x), math.floor(y)
    fx, fy = x - ix, y - iy
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b = hsh(ix, iy, seed), hsh(ix + 1, iy, seed)
    c, d = hsh(ix, iy + 1, seed), hsh(ix + 1, iy + 1, seed)
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy


def fbm(x, y, seed, octaves=3):
    s, amp, tot = 0.0, 1.0, 0.0
    for o in range(octaves):
        s += amp * vnoise(x, y, seed + 17 * o); tot += amp
        x, y = x * 2.03 + 11.7, y * 2.03 + 5.3; amp *= 0.5
    return s / tot


def plane2(p, n):
    """2D coordinates of a surface point for noise: the plane the face is closest to; |y| (the cockpit is mirrored
    left / right and several faces share their texels with their mirror image)"""
    ax = [abs(n[0]), abs(n[1]), abs(n[2])]
    if ax[2] >= ax[0] and ax[2] >= ax[1]: return p[0], abs(p[1])
    if ax[1] >= ax[0]: return p[0], p[2]
    return abs(p[1]), p[2]

# ---------------------------------------------------------------- what is painted

ISLAND_SEAT = ((0, 820, 345, 1275), (250, 1370, 680, 2000))      # texels (x0, y0, x1, y1) of a 2048 texture
ISLAND_TUB = ((1100, 1540, 2048, 2048),)
PROTECT = ((400, 1395, 562, 1460),)                              # the head box's placards and the grey fitting above
PLACARD = (410, 1420, 475, 1453)                                 # "VARNING KRUT" plate on the head box, 1.86 mm/texel

SEAT_METAL = (68, 72, 65)            # bucket: a grey-green, greyer than the tub so the seat reads as its own part
CANVAS = (98, 95, 63)                # olive drab cushions
WEBBING = (118, 114, 90)             # harness straps
STEEL = (150, 150, 142)
BARE = (132, 134, 126)               # paint worn through to the aluminium
TUB = (56, 67, 51)                   # the cockpit paint of the panels (drakenc.png)
FLOOR = (52, 61, 48)


def in_rects(x, y, rects):
    return any(x0 <= x < x1 and y0 <= y < y1 for x0, y0, x1, y1 in rects)


def main():
    src, acp, outp = sys.argv[1:4]
    W, H, px = png_read(src)
    if (W, H) != (2048, 2048): raise SystemExit('%s: expected 2048x2048, got %dx%d' % (src, W, H))
    orig = bytes(px)
    objs = ac_objects(acp, {'Seat', 'draken_cockpit_unlit'})
    for k in ('Seat', 'draken_cockpit_unlit'):
        if k not in objs: raise SystemExit('%s: no object %s' % (acp, k))

    # faces of the two islands, with their folds (edges where the surface turns by more than 30 degrees)
    faces = []                                           # dict per face
    for oname, rects, kind in (('Seat', ISLAND_SEAT, 'seat'), ('draken_cockpit_unlit', ISLAND_TUB, 'tub'),
                               ('Seat', ISLAND_TUB, 'tub')):         # (the seat's rear face is mapped on the tub)
        allf = objs[oname]
        info = []
        for pts, uvs in allf:
            n = newell(pts)
            c = [sum(p[j] for p in pts) / len(pts) for j in range(3)]
            info.append((n, c))
        edges = {}
        key = lambda p: (round(p[0], 4), round(p[1], 4), round(p[2], 4))
        for fi, (pts, uvs) in enumerate(allf):
            for k in range(len(pts)):
                a, b = key(pts[k]), key(pts[(k + 1) % len(pts)])
                edges.setdefault((min(a, b), max(a, b)), []).append(fi)
        for fi, (pts, uvs) in enumerate(allf):
            uc = (sum(u for u, v in uvs) / len(uvs) * W, sum(v for u, v in uvs) / len(uvs) * H)
            if not in_rects(uc[0], uc[1], rects): continue
            n, c = info[fi]
            folds = []                                   # (a, b, convex, angle in degrees)
            for k in range(len(pts)):
                a, b = pts[k], pts[(k + 1) % len(pts)]
                ka, kb = key(a), key(b)
                others = [g for g in edges[(min(ka, kb), max(ka, kb))] if g != fi]
                if math.sqrt(dot(sub(b, a), sub(b, a))) < 0.03: continue
                if not others:
                    folds.append((a, b, True, 90.0))           # an open edge: worn like an outer one
                    continue
                g = others[0]
                ng, cg = info[g]
                ang = math.degrees(math.acos(min(abs(dot(n, ng)), 1.0)))    # (|.|: some faces are wound the other way)
                if ang < 30: continue
                folds.append((a, b, dot(sub(cg, c), n) < 0, ang))
            faces.append(dict(kind=kind, obj=oname, idx=fi, pts=pts, uvs=uvs, n=n, c=c, folds=folds))

    # screws / rivets along the real corners (not the folds of a curved surface's few faces): seat walls along their
    # top edges, the tub where floor and walls meet
    for f in faces:
        sc = []
        pitch, inset = (0.055, 0.011) if f['kind'] == 'seat' else (0.040, 0.010)
        for a, b, convex, ang in f['folds']:
            ab = sub(b, a); ln = math.sqrt(dot(ab, ab))
            if f['kind'] == 'seat' and not (convex and ang > 60 and abs(f['n'][1]) > 0.8 and ln >= 0.09): continue
            if f['kind'] == 'tub' and not (ang > 50 and ln >= 0.12): continue
            d = norm(ab)
            inw = norm(cross(f['n'], d))
            if dot(inw, sub(f['c'], a)) < 0: inw = mul(inw, -1)
            cnt = int(ln / pitch)
            off = (ln - (cnt - 1) * pitch) / 2 if cnt > 1 else ln / 2
            for k in range(max(cnt, 1)):
                sc.append(add(add(a, mul(d, off + k * pitch)), mul(inw, inset)))
        f['screws'] = sc

    # raster: which face (and where on it) each texel of the islands shows; texels up to 1.5 texel outside a face
    # (its bilinear border) belong to the nearest face
    cover = {}                                           # texel index -> (score, face, 3D point)
    for f in faces:
        P, U = f['pts'], [(u * W, v * H) for u, v in f['uvs']]
        for k in range(1, len(P) - 1):
            tri = ((P[0], U[0]), (P[k], U[k]), (P[k + 1], U[k + 1]))
            (p0, (x0, y0)), (p1, (x1, y1)), (p2, (x2, y2)) = tri
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-6: continue
            e = [math.hypot(x2 - x1, y2 - y1), math.hypot(x0 - x2, y0 - y2), math.hypot(x1 - x0, y1 - y0)]
            bx0, bx1 = max(int(min(x0, x1, x2)) - 2, 0), min(int(max(x0, x1, x2)) + 3, W)
            by0, by1 = max(int(min(y0, y1, y2)) - 2, 0), min(int(max(y0, y1, y2)) + 3, H)
            for ty in range(by0, by1):
                py = ty + 0.5
                for tx in range(bx0, bx1):
                    qx = tx + 0.5
                    w0 = ((x1 - qx) * (y2 - py) - (x2 - qx) * (y1 - py)) / area
                    w1 = ((x2 - qx) * (y0 - py) - (x0 - qx) * (y2 - py)) / area
                    w2 = 1.0 - w0 - w1
                    # signed distance (texels) to the triangle's edges; > 0 inside
                    sd = min(w0 * abs(area) / (e[0] or 1), w1 * abs(area) / (e[1] or 1), w2 * abs(area) / (e[2] or 1))
                    if sd < -1.5: continue
                    i = ty * W + tx
                    old = cover.get(i)
                    if old is not None and old[0] >= sd: continue
                    cw = [min(max(w, -0.5), 1.5) for w in (w0, w1, w2)]
                    cover[i] = (sd, f, [cw[0] * p0[j] + cw[1] * p1[j] + cw[2] * p2[j] for j in range(3)])

    # classes of the original texels: 'g' unpainted grey bake, 'b' the author's brown cushion, 'd' his dark
    # hand-painted areas (tub), else painted (kept)
    def klass(i, kind):
        x, y = i % W, i // W
        if in_rects(x, y, PROTECT): return None
        r, g, b = orig[4 * i], orig[4 * i + 1], orig[4 * i + 2]
        mx, mn = max(r, g, b), min(r, g, b)
        if kind == 'tub':
            if mx - mn < 14: return 'g' if mx >= 72 else 'd'
            return None
        # seat: grey, the brown, and the anti-aliased blends between them (r >= g >= b; not the red handles, the
        # green head box or a placard)
        if mx < 80 or r < g - 3 or g < b - 3: return None
        tb = (r - b) / mx
        if tb > 0.68 or r - g > 45: return None
        return 'b' if tb > 0.24 else 'g'

    # cushions: the author's brown, and the back rest (the face of the Seat between its side wings)
    def backrest(f):
        return f['obj'] == 'Seat' and f['n'][0] < -0.8 and abs(f['n'][1]) < 0.2 and f['c'][2] > 0.3 and \
            max(abs(p[1]) for p in f['pts']) < 0.235

    def pan(f):                                          # the seat pan's cushion face
        return f['obj'] == 'Seat' and f['n'][2] > 0.9 and -3.95 < f['c'][0] < -3.40 and abs(f['c'][1]) < 0.1

    canvas = bytearray(W * H)
    region = {}                                          # texel -> (kind, class) for every texel to paint
    for rects, kind in ((ISLAND_SEAT, 'seat'), (ISLAND_TUB, 'tub')):
        for x0, y0, x1, y1 in rects:
            for y in range(y0, y1):
                for x in range(x0, x1):
                    i = y * W + x
                    k = klass(i, kind)
                    if k is None: continue
                    cv = cover.get(i)
                    if kind == 'seat' and (k == 'b' or (cv and backrest(cv[1]))): canvas[i] = 1
                    region[i] = (kind, k)
    # an upstream texture that moved or painted these islands: stop rather than paint over the wrong texels
    for kind, least in (('seat', 140000), ('tub', 130000)):          # (half of the 280146 / 261621 there are)
        cnt = sum(1 for kd, k in region.values() if kd == kind and k in 'gb')
        if cnt < least:
            raise SystemExit('%s: only %d unpainted %s texels where the texture of 2026-10 has ~%d: the islands moved or '
                             'were painted upstream; check ISLAND_* in %s or build with J35J_PAINT=0'
                             % (src, cnt, kind, least * 2, sys.argv[0]))
    # distance (texels) of canvas texels to the nearest non-canvas one: the cushions' rounded borders and seams
    xs0, ys0, xs1, ys1 = 0, 820, 680, 2000
    dw = xs1 - xs0
    D = chamfer(xs0, ys0, xs1, ys1, lambda i: canvas[i] == 1, W)
    # the author's dark texels outside every face are left unless next to a face (the black between the islands);
    # grey and brown gutters are painted plain (bilinear filtering and mip-maps mix them into the faces' edges)
    near = {}
    for rects in (ISLAND_SEAT, ISLAND_TUB):
        for x0, y0, x1, y1 in rects:
            Dg = chamfer(x0, y0, x1, y1, lambda i: i not in cover, W)
            for y in range(y0, y1):
                for x in range(x0, x1):
                    if Dg[(y - y0) * (x1 - x0) + x - x0] <= 6: near[y * W + x] = 1

    # texel size (m) per face
    for f in faces:
        U = [(u * W, v * H) for u, v in f['uvs']]
        a3 = poly_area3(f['pts'])
        a2 = 0.5 * abs(sum((U[k][0] - U[0][0]) * (U[k + 1][1] - U[0][1]) - (U[k + 1][0] - U[0][0]) * (U[k][1] - U[0][1])
                           for k in range(1, len(U) - 1)))
        f['m_px'] = math.sqrt(a3 / a2) if a2 > 0 else 0.003

    # the harness: straps as 3D segments on the cushions (centre line a -> b, width), fittings at their ends
    straps = []
    for s in (1, -1):
        straps.append(((-3.505, s * 0.222, 0.0), (-3.735, s * 0.055, 0.0), 0.044))      # lap straps on the pan
        straps.append(((-3.168, s * 0.105, 0.0), (-3.40, s * 0.112, 0.0), 0.042))      # shoulder straps (back rest)
    straps.append(((-3.955, 0.0, 0.0), (-3.785, 0.0, 0.0), 0.040))                    # crotch strap, from the cut-out
    fittings = []
    for s in (1, -1):
        fittings.append(((-3.752, s * 0.048), 0.030, 0.026))                           # lap buckle halves
        fittings.append(((-3.392, s * 0.112), 0.028, 0.024))                           # shoulder strap ends
    fittings.append(((-3.778, 0.0), 0.030, 0.028))                                     # crotch strap's fitting

    # placard: the head box's seat warning plate repeated on the inner walls of the pan (8.5 x 4.2 cm)
    plac = []
    for y in range(PLACARD[1], PLACARD[3]):
        plac.append([orig[4 * (y * W + x):4 * (y * W + x) + 3] for x in range(PLACARD[0], PLACARD[2])])
    pw, ph = PLACARD[2] - PLACARD[0], PLACARD[3] - PLACARD[1]

    def placard_at(f, p):
        """colour of the placard at point p of an inner wall of the pan, or None"""
        if f['kind'] != 'seat' or abs(f['n'][1]) < 0.9 or not (-3.95 < f['c'][0] < -3.40): return None
        if abs(f['c'][1]) > 0.236: return None       # outer walls
        side = 1 if f['c'][1] > 0 else -1
        # viewer at the seat's middle looking at the wall: right = view x up
        view = [0, side, 0]; up = [0, 0, 1]
        right = cross(view, up)
        cx, cz = -3.60, 0.122
        lx = dot(sub(p, [cx, p[1], cz]), right) / 0.085 + 0.5
        lz = 0.5 - (p[2] - cz) / 0.042
        if not (0 <= lx < 1 and 0 <= lz < 1): return None
        fx, fy = lx * pw - 0.5, lz * ph - 0.5
        ix, iy = int(math.floor(fx)), int(math.floor(fy))
        tx, ty = fx - ix, fy - iy
        acc = [0.0, 0.0, 0.0]
        for (jx, jy, wgt) in ((ix, iy, (1 - tx) * (1 - ty)), (ix + 1, iy, tx * (1 - ty)), (ix, iy + 1, (1 - tx) * ty),
                              (ix + 1, iy + 1, tx * ty)):
            jx, jy = min(max(jx, 0), pw - 1), min(max(jy, 0), ph - 1)
            c = plac[jy][jx]
            for j in range(3): acc[j] += wgt * c[j]
        edge = min(lx, 1 - lx, lz * 0.5, (1 - lz) * 0.5)
        return acc, smooth(0.0, 0.03, edge)

    def harness(f, p, col):
        """the unbuckled harness lying on the pan and down the back rest (straps with their shadows, steel fittings)"""
        for a, b, wd in straps:
            if (a[0] > -3.45) != backrest(f): continue
            if backrest(f):                         # back rest straps: x/z along the face; use z for the run
                a3 = [a[0], a[1], 0.777]; b3 = [b[0], b[1], 0.10]
                q = [p[0], p[1], p[2]]
                ab = sub(b3, a3); t = dot(sub(q, a3), ab) / dot(ab, ab)
                if not (0 <= t <= 1): continue
                across = abs(p[1] - (a3[1] + (b3[1] - a3[1]) * t))
                along = t * math.sqrt(dot(ab, ab))
            else:
                ab = [b[0] - a[0], b[1] - a[1]]; ln = math.hypot(*ab)
                t = ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1]) / (ln * ln)
                if not (0 <= t <= 1.02): continue
                across = abs((p[0] - a[0]) * ab[1] - (p[1] - a[1]) * ab[0]) / ln
                along = t * ln
            if across < wd * 0.5 + 0.006:
                if across > wd * 0.5:               # its shadow on the canvas
                    col = [c * (0.70 + 0.30 * smooth(wd * 0.5, wd * 0.5 + 0.006, across)) for c in col]
                    continue
                w = list(WEBBING)
                wm = 0.93 + 0.07 * fbm(along * 140, across * 40, 11, 2)
                wm *= 0.96 if int(across * 1000 / 4.0) % 2 else 1.0                       # woven ribs
                wm *= 0.72 + 0.28 * smooth(0.0, 0.004, wd * 0.5 - across)                  # folded edges
                col = [w[j] * wm for j in range(3)]
        for (fx_, fy_), lx_, ly_ in fittings:
            if (fx_ > -3.45) != backrest(f): continue
            if backrest(f):
                zc = 0.13
                ax_, ay_ = abs(p[2] - zc) / (ly_ * 0.5), abs(p[1] - fy_) / (lx_ * 0.5)
            else:
                ax_, ay_ = abs(p[0] - fx_) / (lx_ * 0.5), abs(p[1] - fy_) / (ly_ * 0.5)
            if ax_ < 1.25 and ay_ < 1.25:
                if ax_ < 1 and ay_ < 1:
                    sm = 0.92 + 0.12 * (1 - ay_) - 0.10 * ax_
                    if 0.25 < ay_ < 0.55 and ax_ < 0.6: sm *= 0.45                       # the slot
                    col = [c * sm for c in STEEL]
                else:
                    col = [c * 0.72 for c in col]  # shadow
        return col

    out = bytearray(px)
    painted = 0
    for i, (kind, k) in region.items():
        r0, g0, b0 = orig[4 * i], orig[4 * i + 1], orig[4 * i + 2]
        lum = 0.3 * r0 + 0.59 * g0 + 0.11 * b0
        cv = cover.get(i)
        if cv is None and k == 'd' and i not in near: continue   # black between the islands
        if k == 'd':                                    # the author's dark areas in the tub: tinted to the paint
            t = 0.45
            col = [r0 * (1 - t) + t * lum * TUB[0] / 63.0, g0 * (1 - t) + t * lum * TUB[1] / 63.0,
                   b0 * (1 - t) + t * lum * TUB[2] / 63.0]
            out[4 * i:4 * i + 3] = bytes(max(0, min(255, int(round(c)))) for c in col)
            painted += 1
            continue
        if kind == 'seat':                              # bake level: grey 182, brown 102 (pads) / 90 (pan), blends
            tb = min(max((r0 - b0) / max(r0, g0, b0, 1) / 0.48, 0.0), 1.0)
            ao = lum / (182.0 * (1 - tb) + (102.0 if i // W < 1300 else 90.0) * tb)
        else: ao = lum / 132.0
        ao = min(max(ao, 0.22), 1.06)
        is_canvas = canvas[i] == 1
        if cv is None:                                  # gutter between the faces: plain colour with the bake
            base = CANVAS if is_canvas else (SEAT_METAL if kind == 'seat' else TUB)
            col = [c * ao for c in base]
            out[4 * i:4 * i + 3] = bytes(max(0, min(255, int(round(c)))) for c in col)
            painted += 1
            continue
        _, f, p = cv
        n = f['n']
        u2, v2 = plane2(p, n)
        if is_canvas:
            dx = D[((i // W) - ys0) * dw + (i % W) - xs0] * f['m_px']        # metres to the cushion's border
            col = list(CANVAS)
            m = 0.90 + 0.13 * fbm(u2 * 9, v2 * 9, 3) + 0.05 * (hsh(i % W, i // W, 7) - 0.5)
            # puffed: darker towards the border and the quilt seams
            if backrest(f):
                seams = [abs(p[2] - z) for z in (0.20, 0.31, 0.42, 0.53, 0.64)]    # rolls across the back rest
            else:
                seams = [abs(abs(p[1]) - 0.075)]                    # three channels along the pan
            ds = min(seams)
            thin = f['m_px'] < 0.0012                    # the small front wall pads (0.4 mm texels): no seams
            m *= (0.82 + 0.18 * smooth(0.0, 0.028, dx)) * (1.0 if thin else 0.90 + 0.10 * smooth(0.0, 0.022, ds))
            if not thin:
                line = max(1.0 - smooth(0.0, 0.0022, abs(ds)), 1.0 - smooth(0.0, 0.0022, abs(dx - 0.018)))
                m *= 1.0 - 0.25 * line
            col = [c * m for c in col]
        else:
            base = SEAT_METAL if kind == 'seat' else (FLOOR if n[2] > 0.6 else TUB)
            m = 0.94 + 0.10 * fbm(u2 * 7, v2 * 7, 5) + 0.035 * (hsh(i % W, i // W, 9) - 0.5)
            col = [c * m for c in base]
            # folds: worn paint on the outer edges, grime in the inner corners
            for a, b, convex, ang in f['folds']:
                if ang < 45: continue
                d = seg_dist(p, a, b)
                if convex and d < 0.010:                 # chips: clustered (a coarse mask) and only right on the edge
                    wear = fbm(u2 * 220, v2 * 220, 21, 2) * smooth(0.25, 0.75, fbm(u2 * 18, v2 * 18, 23, 2))
                    wear *= 1.0 - d / 0.010
                    if wear > 0.45:
                        t = min((wear - 0.45) * 3.0, 1.0) * 0.70
                        col = [col[j] * (1 - t) + BARE[j] * t for j in range(3)]
                elif not convex and d < 0.020:
                    col = [c * (0.90 + 0.10 * smooth(0.0, 0.020, d)) for c in col]
            if kind == 'tub' and n[2] > 0.6:            # heel rests: scuffed
                w = math.exp(-(((p[0] + 4.17) / 0.14) ** 2 + ((abs(p[1]) - 0.19) / 0.09) ** 2))
                if w > 0.02:
                    sc = fbm(p[0] * 30, abs(p[1]) * 220, 31, 3)
                    if sc > 0.52:
                        t = min((sc - 0.52) * 3.0, 1.0) * w * 0.7
                        col = [col[j] * (1 - t) + BARE[j] * 0.78 * t for j in range(3)]
                    else:
                        col = [c * (1 - 0.22 * w * (0.52 - sc) * 2) for c in col]
            # screws / rivets
            rs = 0.0042 if kind == 'seat' else 0.0030
            for s in f['screws']:
                dd = sub(p, s); d = math.sqrt(dot(dd, dd))
                if d < rs * 1.6:
                    if d < rs:
                        sh = 1.30 - 0.45 * (d / rs) ** 2
                        col = [c * sh for c in (base[0] * 1.2, base[1] * 1.15, base[2] * 1.2)]
                    else:
                        col = [c * (0.62 + 0.38 * smooth(rs, rs * 1.6, d)) for c in col]
            pl = placard_at(f, p)
            if pl is not None:
                pc, a_ = pl
                col = [col[j] * (1 - a_) + pc[j] * a_ for j in range(3)]
        if backrest(f) or pan(f): col = harness(f, p, col)
        col = [c * ao for c in col]
        out[4 * i:4 * i + 3] = bytes(max(0, min(255, int(round(c)))) for c in col)
        painted += 1
    png_write(outp, W, H, out)
    print('%s: %d texels painted (%d faces: %d seat, %d tub)' % (outp, painted, len(faces),
                                                                 sum(1 for f in faces if f['kind'] == 'seat'),
                                                                 sum(1 for f in faces if f['kind'] == 'tub')))


main()
