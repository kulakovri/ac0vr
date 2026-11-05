"""ac_flatten.py IN.ac OUT.ac REGEX [--offset X,Y,Z] [--mat-scale R=F]... -- copy the poly objects of an AC3D file whose names match REGEX
into a flat AC3D file: each object's vertices in world coordinates (the loc / rot of all its parents applied), moved by
--offset (FlightGear frame metres: x aft, y right, z up), materials and texture names kept. Used to borrow one part of
another model in another frame (a canopy glass of one FlightGear model over another's cockpit,
which has none above the pilot): ac2cpit.py --extra OUT.ac REGEX. Only reads IN.ac (ck_standin 2026-10-28).
--mat-scale R=F: the objects whose names match R get copies of their materials with the colour (rgb, amb) times F (a
light grey part painted darker; repeatable, first match wins; ck_polish 2026-10-28: the throttle grips in
build_fa18c.sh). F may also be R,G,B (a colour factor per channel: a part tinted towards another cockpit's paint).
Options for grafting controls into another cockpit (ck_ctl 2026-10-30; files built
without them do not change):
--within X0,X1,Y0,Y1,Z0,Z1  only objects whose vertex centroid lies in this box (FlightGear frame metres of IN.ac, before
               --offset): one of two same-named copies (the F-15's front and rear cockpit controls)
--stretch R@Z,DZ[,S]  the objects matching R: vertices below FlightGear z = Z (IN.ac's frame) go to Z + (z - Z) * S + DZ
               (S default 1: moved by DZ, a stick's shaft lengthened at its foot so that it reaches a deeper floor; S < 1
               squashes the part below Z towards it: a centre stick's shaft cut down to a side stick's; repeatable)
--crop-tex S   the textures of the kept objects are cut down to the texel boxes those objects use (one box per object,
               4 texels of margin, overlapping boxes merged), scaled by S (1, 0.5, 0.25), packed into one PNG per texture
               next to OUT.ac (<OUT stem>_<texture stem>.png) and the texture coordinates moved to it: a few parts of a 4096 px atlas
               without carrying the whole atlas (the F-15's interior). Textures whose coordinates wrap stay as they are.
               Decoded with ac2cpit.py's PNG / SGI / DDS readers."""
import os, re, struct, sys, zlib


def mul(a, b):                                          # 3x3 row-major
    return [sum(a[i * 3 + k] * b[k * 3 + j] for k in range(3)) for i in range(3) for j in range(3)]


def app(m, v):
    return [m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
            m[6] * v[0] + m[7] * v[1] + m[8] * v[2]]


def png_write(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def crop_textures(objs, src_dir, dst, scale, margin=4):
    """--crop-tex (ck_ctl 2026-10-30): per texture the texel boxes the objects use, merged, scaled, shelf-packed into one
    PNG next to dst; the objects' refs lines get the new coordinates (AC3D: v up, rows of the image top first)"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import ac2cpit
    bytex = {}
    for o in objs:
        if not o['tex']: continue
        bytex.setdefault(o['tex'].split(None, 1)[1].strip().strip('"'), []).append(o)
    for tname, olist in bytex.items():
        uvs = {id(o): [(float(l.split()[1]), float(l.split()[2])) for s in o['surfs'] for l in s
                       if len(l.split()) == 3 and not l.startswith(('mat', 'refs', 'SURF'))] for o in olist}
        allu = [p for v in uvs.values() for p in v]
        if not allu or min(min(u, v) for u, v in allu) < -1e-3 or max(max(u, v) for u, v in allu) > 1 + 1e-3:
            print('crop-tex: %s kept whole (texture coordinates wrap)' % tname); continue
        path = os.path.join(src_dir, tname)
        W, H, px = ac2cpit.load_texture(path)
        boxes = []
        for o in olist:
            us = [u for u, v in uvs[id(o)]]; vs = [v for u, v in uvs[id(o)]]
            if not us: continue
            boxes.append([max(0, int(min(us) * W) - margin), max(0, int((1 - max(vs)) * H) - margin),
                          min(W, int(max(us) * W + 1) + margin), min(H, int((1 - min(vs)) * H + 1) + margin)])
        merged = True
        while merged:                                       # merge overlapping boxes
            merged = False
            for p in range(len(boxes)):
                for q in range(p + 1, len(boxes)):
                    a_, b_ = boxes[p], boxes[q]
                    if a_[0] < b_[2] and b_[0] < a_[2] and a_[1] < b_[3] and b_[1] < a_[3]:
                        boxes[p] = [min(a_[0], b_[0]), min(a_[1], b_[1]), max(a_[2], b_[2]), max(a_[3], b_[3])]
                        del boxes[q]; merged = True; break
                if merged: break
        k = 0                                               # halvings: scale 1, 0.5, 0.25 ...
        while k < 4 and scale <= 0.5 ** (k + 1) + 1e-6: k += 1
        f = 2 ** k
        for b in boxes:                                     # align to the halving so texel boxes stay exact
            b[0] -= b[0] % f; b[1] -= b[1] % f; b[2] = min(W, b[2] + (-b[2]) % f); b[3] = min(H, b[3] + (-b[3]) % f)
        sizes = [((b[2] - b[0]) // f, (b[3] - b[1]) // f) for b in boxes]
        area = sum(w * h for w, h in sizes)
        order = sorted(range(len(boxes)), key=lambda j: -sizes[j][1])
        best = None
        TW = 16
        while TW < max(w for w, h in sizes): TW *= 2
        for _ in range(6):                                  # shelf packing; the width giving the smallest texture
            pl, x, y, rowh = {}, 0, 0, 0
            for j in order:
                w, h = sizes[j]
                if x + w > TW: x, y, rowh = 0, y + rowh, 0
                pl[j] = (x, y); x += w; rowh = max(rowh, h)
            TH = 16
            while TH < y + rowh: TH *= 2
            if best is None or TW * TH < best[0] * best[1] or (TW * TH == best[0] * best[1] and abs(TW - TH) < abs(best[0] - best[1])):
                best = (TW, TH, pl)
            TW *= 2
        TW, TH, place = best
        out = bytearray(TW * TH * 4)
        for j, b in enumerate(boxes):
            bw, bh = b[2] - b[0], b[3] - b[1]
            sub = b''.join(px[((b[1] + r) * W + b[0]) * 4:((b[1] + r) * W + b[2]) * 4] for r in range(bh))
            for _ in range(k): bw, bh, sub = ac2cpit.half_rgba(bw, bh, sub)
            ox, oy = place[j]
            for r in range(bh):
                out[((oy + r) * TW + ox) * 4:((oy + r) * TW + ox + bw) * 4] = sub[r * bw * 4:(r + 1) * bw * 4]
        stem = '%s_%s.png' % (os.path.splitext(os.path.basename(dst))[0], os.path.splitext(os.path.basename(tname))[0])
        png_write(os.path.join(os.path.dirname(dst) or '.', stem), TW, TH, bytes(out))
        print('crop-tex: %s %dx%d -> %s %dx%d (%d boxes, scale %g)' % (tname, W, H, stem, TW, TH, len(boxes), 1.0 / f))
        for o in olist:
            o['tex'] = 'texture "%s"' % stem
            ns = []
            for s in o['surfs']:
                s2 = []
                for l in s:
                    t = l.split()
                    if len(t) == 3 and not l.startswith(('mat', 'refs', 'SURF')):
                        u, v = float(t[1]), float(t[2]); tx, ty = u * W, (1 - v) * H
                        j = next(j_ for j_, b in enumerate(boxes) if b[0] - 1 <= tx <= b[2] + 1 and b[1] - 1 <= ty <= b[3] + 1)
                        b = boxes[j]; ox, oy = place[j]
                        l = '%s %.6f %.6f' % (t[0], (ox + (tx - b[0]) / f) / TW, 1 - (oy + (ty - b[1]) / f) / TH)
                    s2.append(l)
                ns.append(s2)
            o['surfs'] = ns


def main():
    a = sys.argv[1:]
    off = [0.0, 0.0, 0.0]
    mscale = []
    while '--mat-scale' in a:
        k = a.index('--mat-scale'); r_, _, f_ = a[k + 1].rpartition('=')
        fv = tuple(float(x) for x in f_.split(','))           # F or R,G,B (ck_ctl 2026-10-30)
        mscale.append((re.compile(r_), fv[0] if len(fv) == 1 else fv))
        del a[k:k + 2]
    if '--offset' in a:
        k = a.index('--offset'); off = [float(x) for x in a[k + 1].split(',')]; del a[k:k + 2]
    within, stretch, crop = None, [], None                 # ck_ctl 2026-10-30: grafting controls (see the header)
    if '--within' in a:
        k = a.index('--within'); within = [float(x) for x in a[k + 1].split(',')]; del a[k:k + 2]
    while '--stretch' in a:
        k = a.index('--stretch'); r_, _, v_ = a[k + 1].rpartition('@')
        stretch.append((re.compile(r_), [float(x) for x in v_.split(',')])); del a[k:k + 2]
    if '--crop-tex' in a:
        k = a.index('--crop-tex'); crop = float(a[k + 1]); del a[k:k + 2]
    src, dst, rx = a[0], a[1], re.compile(a[2])
    off_ac = [off[0], off[2], -off[1]]                   # FlightGear -> AC3D (x, y up, z = -right)
    L = open(src, 'r', encoding='latin-1').read().split('\n')
    mats = [l for l in L if l.startswith('MATERIAL')]
    out, objs = [], []
    newmat = {}                                          # (material, factor) -> index of its scaled copy

    def scaled(k, f):
        if (k, f) not in newmat:
            t = mats[k].split()
            for key in ('rgb', 'amb'):
                if key in t:
                    j = t.index(key)
                    for c in range(1, 4): t[j + c] = '%.4f' % (float(t[j + c]) * (f[c - 1] if isinstance(f, tuple) else f))
            if t[1].endswith('"'):
                t[1] = t[1][:-1] + ('_x%g_%g_%g"' % f if isinstance(f, tuple) else '_x%g"' % f)
            newmat[k, f] = len(mats) + len(newmat); scaled.lines.append(' '.join(t))
        return newmat[k, f]
    scaled.lines = []
    i = 0

    def obj(i, M, T):
        """object at line i; parent world rotation M (AC3D: v' = M v, row-major as AC3D's rot) and translation T"""
        name, tex, loc, rot, verts, surfs, kids, extra = '', None, [0, 0, 0], [1, 0, 0, 0, 1, 0, 0, 0, 1], [], [], 0, []
        i += 1
        while i < len(L):
            l = L[i]; t = l.split(' ', 1)[0]
            if t == 'name': name = l[5:].strip().strip('"')
            elif t == 'texture': tex = l
            elif t in ('texrep', 'texoff', 'crease'): extra.append(l)
            elif t == 'loc': loc = [float(x) for x in l.split()[1:4]]
            elif t == 'rot': rot = [float(x) for x in l.split()[1:10]]
            elif t == 'data':
                n = int(l.split()[1]); got = 0; i += 1
                while got < n and i < len(L): got += len(L[i]) + (1 if got else 0); i += 1
                continue
            elif t == 'url': pass
            elif t == 'numvert':
                n = int(l.split()[1]); verts = [[float(x) for x in L[i + 1 + k].split()[:3]] for k in range(n)]; i += n
            elif t == 'numsurf':
                n = int(l.split()[1]); i += 1
                for _ in range(n):
                    s = []
                    while not L[i].startswith('refs'): s.append(L[i]); i += 1
                    nr = int(L[i].split()[1]); s.append(L[i]); s += L[i + 1:i + 1 + nr]; i += 1 + nr
                    surfs.append(s)
                continue
            elif t == 'kids':
                kids = int(l.split()[1]); i += 1; break
            i += 1
        # AC3D: world = parent (M, T) applied to (rot * v + loc), rot row-major as written (as ac2cpit.py reads it)
        Mw = mul(M, rot); Tw = [a_ + b_ for a_, b_ in zip(app(M, loc), T)]
        if verts and surfs and rx.search(name):
            W = [[x + y for x, y in zip(app(Mw, v), Tw)] for v in verts]       # world, AC3D axes (y up, z = -right)
            if within:                                   # ck_ctl: the centroid in the FlightGear frame (x, -z, y)
                c = [sum(w_[k] for w_ in W) / len(W) for k in range(3)]
                fg = (c[0], -c[2], c[1])
                if not all(within[2 * k] <= fg[k] <= within[2 * k + 1] for k in range(3)):
                    for _ in range(kids): i = obj(i, Mw, Tw)
                    return i
            for r_, sv_ in stretch:                      # ck_ctl: a shaft lengthened / squashed (FlightGear z = AC y)
                z_, dz_, s_ = (sv_ + [1.0])[:3]
                if r_.search(name): W = [[w_[0], z_ + (w_[1] - z_) * s_ + dz_ if w_[1] < z_ else w_[1], w_[2]] for w_ in W]
            f = next((f_ for r_, f_ in mscale if r_.search(name)), None)
            if f is not None:
                surfs = [['mat %d' % scaled(int(l.split()[1]), f) if l.startswith('mat ') else l for l in s] for s in surfs]
            objs.append(dict(name=name, tex=tex, extra=extra, W=W, surfs=surfs))
            print('%s: %d vertices, %d surfaces' % (name, len(verts), len(surfs)))
        for _ in range(kids):
            i = obj(i, Mw, Tw)
        return i
    while i < len(L) and not L[i].startswith('OBJECT'): i += 1
    obj(i, [1, 0, 0, 0, 1, 0, 0, 0, 1], [0.0, 0.0, 0.0])
    if crop:
        crop_textures(objs, os.path.dirname(src), dst, crop)
    for o in objs:
        out.append('OBJECT poly'); out.append('name "%s"' % o['name'])
        if o['tex']: out.append(o['tex'])
        out.extend(o['extra'])
        out.append('numvert %d' % len(o['W']))
        for w in o['W']:
            out.append('%.6f %.6f %.6f' % tuple(x + y for x, y in zip(w, off_ac)))
        out.append('numsurf %d' % len(o['surfs']))
        for s in o['surfs']:
            out.extend(s)
        out.append('kids 0')
    n = sum(1 for l in out if l == 'OBJECT poly')
    open(dst, 'w', encoding='latin-1').write('\n'.join(['AC3Db'] + mats + scaled.lines + ['OBJECT world', 'kids %d' % n] + out) + '\n')


main()
