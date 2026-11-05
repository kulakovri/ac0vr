"""hands_bake.py -- gloved pilot hands for the native VR cockpit (wish #76, ACZ_VR_HANDS): the Godot XR Tools hand
models (CC0 1.0, DigitalN8m4r3 / Miodrag Sejic, made with MakeHuman CE; github.com/GodotVR/godot-xr-tools,
addons/godot-xr-tools/hands) posed offline into rigid meshes, written as one AC3D file for tools/cockpit/ac2cpit.py
--hands.

Usage: python -I hands_bake.py SRC_DIR OUT.ac [--preview OUT.png] [--glove R,G,B] [--low] [--stick-mm D]
  SRC_DIR   a folder with the downloaded files (nothing else is read; no code from it runs):
              model/hand_l.gltf, model/hand_r.gltf (or model/Hand_low_L.gltf / _R with --low)
              anim/left/<pose>.res, anim/right/<pose>.res (Godot 4 binary Animation resources, one key per bone)
              tex/cleaning_glove_baseColor.png (the full glove's albedo: its wrinkles and seams are kept, its yellow
              is replaced by --glove)
  OUT.ac    objects hand_<L|R>_<pose> and hand_<L|R>_<pose>_cuff, the textures <OUT>_glove.png and <OUT>_cuff.png next
            to it
  --glove   the glove colour (sRGB 0-255; default 92,86,70: a worn sage-tan Nomex flight glove)
  --sleeve  the flight suit's sleeve colour of the cuffs (default 78,84,64: sage green); a cuff is a short tube from the
            glove's wrist opening up the forearm (7.5 cm), fading to dark towards its end, so no bare cut shows
  --stick-mm the handle diameter the grip pose closes round (default 36): "Grip Shaft" fits a ~55 mm shaft, "Grip"
            the Touch controller; the grip is the per-bone blend between them that fits D
  --preview a contact sheet of the poses (flat shaded, three views) for checking

Poses (Godot animation -> object): "Grip Shaft" blended towards "Grip" (--stick-mm) -> grip (on the stick / throttle
grip), "Default pose" -> open (a free hand, relaxed), "Sign_Point" -> point (index out: the finger is off the trigger),
"Grip" -> fist (squeezing).

Frame: every object is in its hand's OpenXR GRIP frame (metres; the frame tc_touch's controller poses use, and the
cockpit's attach frames): origin on the axis of the tube the closed fingers form, at the middle finger; -Z along that
axis from the little finger to the index / thumb; +X normal to the palm, to the hand's right when held upright (right
hand: into the palm, left hand: out of it); +Y = Z x X, from the grip towards the wrist. The tube is fitted
to the "Grip" pose's finger joints; all poses share that frame relative to the wrist (the wrist does not move between
poses), except "grip", fitted to its own tube so the fingers close round the cockpit's handle.
Vertices stay in that frame (no AC3D -> FlightGear axis swap: ac2cpit.py --hands reads them as eye-frame metres).
"""
import base64, json, math, os, struct, sys, zlib

POSES = (('grip', 'Grip Shaft'), ('open', 'Default pose'), ('point', 'Sign_Point'), ('fist', 'Grip'))
FIT_POSE = 'Grip'                  # the Touch controller grip: the frame of the free poses


# ---------------------------------------------------------------- Godot 4 binary resources (.res, "RSRC")

class _Rd:
    def __init__(self, d): self.d, self.p = d, 0
    def take(self, fmt):
        v = struct.unpack_from('<' + fmt, self.d, self.p); self.p += struct.calcsize('<' + fmt)
        return v if len(v) > 1 else v[0]
    def ustr(self):
        n = self.take('I') & 0x7FFFFFFF
        s = self.d[self.p:self.p + n]; self.p += n
        return s.rstrip(b'\0').decode('utf-8', 'replace')


def _name(r, strs):
    v = r.take('I')
    if v & 0x80000000:
        n = v & 0x7FFFFFFF; s = r.d[r.p:r.p + n]; r.p += n
        return s.rstrip(b'\0').decode()
    return strs[v]


def _variant(r, strs):
    t = r.take('I')
    if t == 1: return None
    if t in (2, 3): return r.take('i')
    if t == 40: return r.take('q')
    if t == 4: return r.take('f')
    if t == 41: return r.take('d')
    if t in (5, 44): return r.ustr()
    if t == 12: return r.take('3f')
    if t == 14: return r.take('4f')
    if t == 22:                                          # NodePath: names and subnames as string-table indices
        nn = r.take('H'); sn = r.take('H') & 0x7FFF
        names = [_name(r, strs) for _ in range(nn)]
        subs = [_name(r, strs) for _ in range(sn)]
        return '/'.join(names) + (':' + ':'.join(subs) if subs else '')
    if t in (32, 33, 49):
        n = r.take('I'); f = {32: 'i', 33: 'f', 49: 'd'}[t]
        return list(r.take('%d%s' % (n, f))) if n > 1 else ([r.take(f)] if n else [])
    if t == 30: return [_variant(r, strs) for _ in range(r.take('I') & 0x7FFFFFFF)]
    if t == 26: return {_variant(r, strs): _variant(r, strs) for _ in range(r.take('I') & 0x7FFFFFFF)}
    raise ValueError('Godot variant type %d not handled' % t)


def godot_pose(path):
    """bone name -> local rotation quaternion (x, y, z, w) of an Animation resource's first keys (rotation_3d tracks)"""
    r = _Rd(open(path, 'rb').read())
    if r.d[:4] != b'RSRC': raise ValueError('%s: not a Godot binary resource' % path)
    r.p = 4
    r.take('5I'); r.ustr(); r.take('Q')
    flags = r.take('I')
    if flags & 2: r.take('Q')
    if flags & 4: r.ustr()
    r.take('11I')
    strs = [r.ustr() for _ in range(r.take('I'))]
    for _ in range(r.take('I')):
        r.ustr(); r.ustr()
        if flags & 2: r.take('Q')
    ints = [(r.ustr(), r.take('Q')) for _ in range(r.take('I'))]
    pose = {}
    for _, off in ints:
        r.p = off
        if r.ustr() != 'Animation': continue
        props = {}
        for _ in range(r.take('I')):
            nm = strs[r.take('I')]
            props[nm] = _variant(r, strs)
        i = 0
        while 'tracks/%d/type' % i in props:
            ty, pa, keys = props['tracks/%d/type' % i], props['tracks/%d/path' % i], props['tracks/%d/keys' % i]
            if ty == 'rotation_3d' and ':' in pa and len(keys) >= 6:
                pose[pa.split(':')[-1]] = tuple(keys[2:6])   # time, transition, x, y, z, w
            i += 1
    return pose


# ---------------------------------------------------------------- glTF (embedded buffers), skinning

def gltf_load(path):
    g = json.load(open(path))
    bufs = []
    for b in g['buffers']:
        u = b['uri']
        if not u.startswith('data:'): raise ValueError('%s: external buffers not supported' % path)
        bufs.append(base64.b64decode(u.split(',', 1)[1]))

    def acc(i):
        a = g['accessors'][i]; bv = g['bufferViews'][a['bufferView']]
        n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
        f = {5126: 'f', 5121: 'B', 5123: 'H', 5125: 'I'}[a['componentType']]
        sz = struct.calcsize(f) * n
        stride = bv.get('byteStride', sz)
        base = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        d = bufs[bv['buffer']]
        return [struct.unpack_from('<%d%s' % (n, f), d, base + k * stride) for k in range(a['count'])]
    mesh_node = next(n for n in g['nodes'] if 'mesh' in n)
    prim = g['meshes'][mesh_node['mesh']]['primitives'][0]
    at = prim['attributes']
    m = {'pos': acc(at['POSITION']), 'nrm': acc(at['NORMAL']), 'uv': acc(at['TEXCOORD_0']),
         'jnt': acc(at['JOINTS_0']), 'wgt': acc(at['WEIGHTS_0']), 'idx': [i[0] for i in acc(prim['indices'])]}
    skin = g['skins'][mesh_node.get('skin', 0)]
    m['joints'] = skin['joints']
    m['ibm'] = [mat_from_gl(x) for x in acc(skin['inverseBindMatrices'])]
    m['nodes'] = g['nodes']
    m['parent'] = {c: i for i, n in enumerate(g['nodes']) for c in n.get('children', [])}
    return m


def mat_from_gl(c):                                      # glTF column-major 4x4 -> row-major 3x4
    return [[c[0], c[4], c[8], c[12]], [c[1], c[5], c[9], c[13]], [c[2], c[6], c[10], c[14]]]


def q2m(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def trs(t, q, s):
    R = q2m(q)
    return [[R[i][0] * s[0], R[i][1] * s[1], R[i][2] * s[2], t[i]] for i in range(3)]


def mul(a, b):                                           # 3x4 affine product
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0.0) for j in range(4)] for i in range(3)]


def apply(m, v): return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] for i in range(3)]
def rot(m, v): return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3)]
def sub(a, b): return [a[i] - b[i] for i in range(3)]
def add(a, b): return [a[i] + b[i] for i in range(3)]
def scl(a, s): return [x * s for x in a]
def dot(a, b): return sum(a[i] * b[i] for i in range(3))
def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
def norm(a):
    l = math.sqrt(dot(a, a)) or 1.0
    return [x / l for x in a]


def globals_of(m, pose):
    """global 3x4 of every node with the pose's local rotations (bones keep their rest translation and scale)"""
    out = {}

    def g(i):
        if i in out: return out[i]
        n = m['nodes'][i]
        q = pose.get(n.get('name'), n.get('rotation', (0, 0, 0, 1)))
        L = trs(n.get('translation', (0, 0, 0)), q, n.get('scale', (1, 1, 1)))
        out[i] = mul(g(m['parent'][i]), L) if i in m['parent'] else L
        return out[i]
    for i in range(len(m['nodes'])): g(i)
    return out


def skinned(m, pose):
    G = globals_of(m, pose)
    S = [mul(G[j], m['ibm'][k]) for k, j in enumerate(m['joints'])]
    P, N = [], []
    for v, n, jj, ww in zip(m['pos'], m['nrm'], m['jnt'], m['wgt']):
        p = [0.0, 0.0, 0.0]; q = [0.0, 0.0, 0.0]
        for j, w in zip(jj, ww):
            if w <= 0.0: continue
            p = add(p, scl(apply(S[j], v), w)); q = add(q, scl(rot(S[j], n), w))
        P.append(p); N.append(norm(q))
    joints = {m['nodes'][j]['name'].rsplit('_', 1)[0]: G[j] for j in m['joints']}
    return P, N, joints


def skinned_joints(m, pose):
    G = globals_of(m, pose)
    return None, None, {m['nodes'][j]['name'].rsplit('_', 1)[0]: G[j] for j in m['joints']}


def blend(a, b, t):
    """per-bone slerp of two poses (bones missing in one keep the other's rotation)"""
    out = dict(a)
    for k, qb in b.items():
        qa = a.get(k, qb)
        d = sum(x * y for x, y in zip(qa, qb))
        if d < 0.0: qb, d = tuple(-x for x in qb), -d
        if d > 0.9995: q = [x + (y - x) * t for x, y in zip(qa, qb)]
        else:
            th = math.acos(d); s0 = math.sin((1 - t) * th) / math.sin(th); s1 = math.sin(t * th) / math.sin(th)
            q = [s0 * x + s1 * y for x, y in zip(qa, qb)]
        l = math.sqrt(sum(x * x for x in q)); out[k] = tuple(x / l for x in q)
    return out


def grip_frame(joints, right):
    """the OpenXR grip frame (3x4: columns X, Y, Z, origin in the model frame) of a closed pose: the tube axis through
    the centre of the circle the four fingers' joints lie on (least squares in the plane across the knuckle line)"""
    pt = lambda n: [joints[n][i][3] for i in range(3)]
    fing = ('Index', 'Middle', 'Ring', 'Little')
    ax = norm(sub(pt('Index_Proximal'), pt('Little_Proximal')))          # little -> index: -Z
    pts = [pt(f + '_' + s) for f in fing for s in ('Proximal', 'Intermediate', 'Distal', 'Tip')]
    # a 2D basis across the axis
    u = norm(cross(ax, [0.0, 0.0, 1.0] if abs(ax[2]) < 0.9 else [1.0, 0.0, 0.0])); v = cross(ax, u)
    c0 = scl(add(pt('Middle_Proximal'), pt('Ring_Proximal')), 0.5)
    xy = [(dot(sub(p, c0), u), dot(sub(p, c0), v)) for p in pts]
    # algebraic circle fit: x^2 + y^2 + D x + E y + F = 0
    A = [[x, y, 1.0] for x, y in xy]; b = [-(x * x + y * y) for x, y in xy]
    AtA = [[sum(A[k][i] * A[k][j] for k in range(len(A))) for j in range(3)] for i in range(3)]
    Atb = [sum(A[k][i] * b[k] for k in range(len(A))) for i in range(3)]
    D, E, F = solve3(AtA, Atb)
    cx, cy = -D / 2.0, -E / 2.0
    rad = math.sqrt(max(cx * cx + cy * cy - F, 0.0))
    centre = add(c0, add(scl(u, cx), scl(v, cy)))
    # origin: on the axis at the middle finger's knuckle
    mid = pt('Middle_Proximal')
    org = add(centre, scl(ax, dot(sub(mid, centre), ax)))
    Z = scl(ax, -1.0)
    # +X: the palm's normal (OpenXR: into the palm of the right hand, away from the palm of the left -- the user's right
    # for both when held upright) = metacarpals (wrist -> middle knuckle) x knuckle line (little -> index), the same
    # formula for both hands (mirrored models), made perpendicular to Z; Y = Z x X (towards the wrist)
    f = norm(sub(pt('Middle_Proximal'), pt('Wrist')))
    X = cross(f, ax)
    X = norm(sub(X, scl(Z, dot(X, Z))))
    Y = cross(Z, X)
    return [[X[i], Y[i], Z[i], org[i]] for i in range(3)], rad


def solve3(M, r):
    M = [row[:] + [r[i]] for i, row in enumerate(M)]
    for c in range(3):
        p = max(range(c, 3), key=lambda k: abs(M[k][c])); M[c], M[p] = M[p], M[c]
        for k in range(3):
            if k != c:
                f = M[k][c] / M[c][c]
                M[k] = [M[k][j] - f * M[c][j] for j in range(4)]
    return [M[i][3] / M[i][i] for i in range(3)]


def to_frame(F, p):                                       # model point -> frame coordinates
    d = sub(p, [F[i][3] for i in range(3)])
    return [d[0] * F[0][k] + d[1] * F[1][k] + d[2] * F[2][k] for k in range(3)]


def to_frame_dir(F, n): return [n[0] * F[0][k] + n[1] * F[1][k] + n[2] * F[2][k] for k in range(3)]


# ---------------------------------------------------------------- PNG

def png_read(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n'
    p, idat, w = 8, b'', 0
    while p < len(d):
        n, t = struct.unpack('>I4s', d[p:p + 8]); body = d[p + 8:p + 8 + n]; p += 12 + n
        if t == b'IHDR': w, h, bd, ct, _, _, il = struct.unpack('>IIBBBBB', body)
        elif t == b'IDAT': idat += body
    if bd != 8 or il or ct not in (2, 6): raise ValueError('%s: 8-bit RGB(A) non-interlaced PNG expected' % path)
    ch = 4 if ct == 6 else 3
    raw = zlib.decompress(idat); st = w * ch
    out = bytearray(w * h * 3); prev = bytearray(st); q = 0
    for y in range(h):
        f = raw[q]; line = bytearray(raw[q + 1:q + 1 + st]); q += 1 + st
        for i in range(st):
            a = line[i - ch] if i >= ch else 0; b_ = prev[i]; c = prev[i - ch] if i >= ch else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b_) & 255
            elif f == 3: line[i] = (line[i] + ((a + b_) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(b_ - c), abs(a - c), abs(a + b_ - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b_ if pb <= pc else c)) & 255
        for x in range(w): out[(y * w + x) * 3:(y * w + x) * 3 + 3] = line[x * ch:x * ch + 3]
        prev = line
    return w, h, out


def png_write(path, w, h, rgb, ch=3):
    raw = b''.join(b'\0' + bytes(rgb[y * w * ch:(y + 1) * w * ch]) for y in range(h))
    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2 if ch == 3 else 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def glove_texture(src, out, rgb, size=512):
    """the glove's albedo recoloured: luminance relative to its median keeps the wrinkles and seams, the two dark print
    marks of the cleaning glove are filled with the median"""
    w, h, px = png_read(src)
    lum = [0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2] for i in range(0, len(px), 3)]
    med = sorted(lum)[len(lum) // 2]
    k = max(1, w // size)
    ow, oh = w // k, h // k
    o = bytearray(ow * oh * 3)
    lin = [(c / 255.0) ** 2.2 for c in rgb]
    for y in range(oh):
        for x in range(ow):
            s = 0.0
            for dy in range(k):
                for dx in range(k): s += lum[(y * k + dy) * w + x * k + dx]
            r = s / (k * k) / med
            if r < 0.55: r = 1.0                         # the print marks
            r = min(max(r, 0.6), 1.35)
            for c in range(3):
                o[(y * ow + x) * 3 + c] = min(255, int(255.0 * (lin[c] * r) ** (1 / 2.2) + 0.5))
    png_write(out, ow, oh, o)


# ---------------------------------------------------------------- output

def weld(P, N, UV, tri):
    """unique positions (UV seams share a vertex so the converter smooths across them); refs keep their UVs"""
    key, vid, VP = {}, [], []
    for p in P:
        k = tuple(round(c, 6) for c in p)
        if k not in key: key[k] = len(VP); VP.append(p)
        vid.append(key[k])
    return VP, [[(vid[i], UV[i][0], 1.0 - UV[i][1]) for i in t] for t in tri]


def write_ac(path, objs):
    """objs: (name, vertices, polygons of (index, u, v), texture file, material 0 glove / 1 cuff)"""
    with open(path, 'w', newline='\n') as f:
        f.write('AC3Db\nMATERIAL "glove" rgb 1 1 1  amb 0.4 0.4 0.4  emis 0 0 0  spec 0.08 0.08 0.08  shi 12  trans 0\n')
        f.write('MATERIAL "cuff" rgb 1 1 1  amb 0.4 0.4 0.4  emis 0 0 0  spec 0.03 0.03 0.03  shi 8  trans 0\n')
        f.write('OBJECT world\nkids %d\n' % len(objs))
        for name, VP, refs, tex, mat in objs:
            f.write('OBJECT poly\nname "%s"\ntexture "%s"\ncrease 75\nnumvert %d\n' % (name, tex, len(VP)))
            for p in VP: f.write('%.6f %.6f %.6f\n' % tuple(p))
            f.write('numsurf %d\n' % len(refs))
            for r in refs:
                f.write('SURF 0x10\nmat %d\nrefs %d\n' % (mat, len(r)))
                for i, u, v in r: f.write('%d %.6f %.6f\n' % (i, u, v))
            f.write('kids 0\n')


def cuff(VP, F, wrist, length=0.075, segs=20):
    """a sleeve cuff over the glove's wrist opening: the opening's ring (the glove's vertices within 8 mm of its end
    along the forearm), an elliptic tube from 1.5 cm inside it to `length` out, flaring 3 % -> 12 %, closed at its
    end; v runs 0 at the wrist to 1 at the end (the cuff texture fades to dark there)"""
    w = [wrist[i][3] for i in range(3)]
    d = norm(to_frame_dir(F, [-wrist[0][1], -wrist[1][1], -wrist[2][1]]))     # towards the elbow
    wf = to_frame(F, w)
    sv = [dot(sub(v, wf), d) for v in VP]
    smax = max(sv)
    ring = [v for v, s_ in zip(VP, sv) if s_ > smax - 0.008]
    c = scl([sum(v[k] for v in ring) for k in range(3)], 1.0 / len(ring))
    c = add(c, scl(d, smax - dot(sub(c, wf), d)))         # on the opening's plane
    e1 = norm(cross(d, [0.0, 0.0, 1.0] if abs(d[2]) < 0.9 else [1.0, 0.0, 0.0])); e2 = cross(d, e1)
    xy = [(dot(sub(v, c), e1), dot(sub(v, c), e2)) for v in ring]
    sxx = sum(x * x for x, _ in xy); syy = sum(y * y for _, y in xy); sxy = sum(x * y for x, y in xy)
    th = 0.5 * math.atan2(2.0 * sxy, sxx - syy)
    a1 = [math.cos(th) * e1[k] + math.sin(th) * e2[k] for k in range(3)]
    a2 = cross(d, a1)
    ra = max(abs(dot(sub(v, c), a1)) for v in ring); rb = max(abs(dot(sub(v, c), a2)) for v in ring)
    VPc, refs = [], []
    rings = ((-0.015, 1.03), (0.35 * length, 1.07), (length, 1.12))
    for t, (off, k) in enumerate(rings):
        o = add(c, scl(d, off))
        for j in range(segs):
            ang = 2.0 * math.pi * j / segs
            VPc.append(add(o, add(scl(a1, ra * k * math.cos(ang)), scl(a2, rb * k * math.sin(ang)))))
    vt = [0.0, 0.35, 1.0]
    for t in range(len(rings) - 1):
        for j in range(segs):
            j1 = (j + 1) % segs
            u0, u1 = j / segs, (j + 1) / segs
            refs.append([(t * segs + j, u0, 1.0 - vt[t]), (t * segs + j1, u1, 1.0 - vt[t]),
                         ((t + 1) * segs + j1, u1, 1.0 - vt[t + 1]), ((t + 1) * segs + j, u0, 1.0 - vt[t + 1])])
    end = (len(rings) - 1) * segs
    refs.append([(end + j, 0.5, 0.0) for j in range(segs)])                  # the end, dark
    return VPc, refs


def cuff_texture(out, rgb):
    """8 x 64: the sleeve's colour with a faint weave, fading to near black over the last half (v down = to the end)"""
    W_, H_ = 8, 64
    o = bytearray(W_ * H_ * 3)
    for y in range(H_):
        t = (y + 0.5) / H_
        f = 1.0 if t < 0.45 else max(0.0, 1.0 - (t - 0.45) / 0.5) ** 1.5
        for x in range(W_):
            n = 1.0 + 0.04 * (((x * 7 + y * 13) % 5) - 2) / 2.0
            for k in range(3):
                o[(y * W_ + x) * 3 + k] = min(255, int((rgb[k] * f * n) + 10 * (1.0 - f) + 0.5))
    png_write(out, W_, H_, o)


def preview(path, objs, stick_r=0.016):
    """flat-shaded orthographic views of each object placed as on an upright stick in the cockpit (grip X = right, -Z =
    up): from the pilot's seat (looking forward), from the right, from above; one row per object; the stick (a
    cylinder of stick_r about the grip axis) in grey"""
    S, rows = 220, len(objs)
    W, H = S * 3, S * rows
    img = bytearray(W * H * 3)
    zb = [1e9] * (W * H)
    for i in range(0, len(img), 3): img[i:i + 3] = bytes((48, 48, 56))
    L = norm([0.4, 0.7, 0.6])
    ck = lambda p: (p[0], -p[2], p[1])                        # grip frame -> cockpit (x right, y up, z back)
    views = [lambda p: (p[0], p[1], -p[2]), lambda p: (-p[2], p[1], -p[0]), lambda p: (p[0], -p[2], -p[1])]
    cyl = []
    for k in range(16):
        a0, a1 = 2 * math.pi * k / 16, 2 * math.pi * (k + 1) / 16
        p = [(stick_r * math.cos(a), y, stick_r * math.sin(a)) for a in (a0, a1) for y in (-0.08, 0.08)]
        cyl += [(p[0], p[2], p[3], 0), (p[0], p[3], p[1], 0)]
    for row, (name, VP, refs) in enumerate(objs):
        tris = [(ck(VP[r[0][0]]), ck(VP[r[k][0]]), ck(VP[r[k + 1][0]]), 1) for r in refs for k in range(1, len(r) - 1)] + cyl
        for col, vf in enumerate(views):
            for a, b, c, hand in tris:
                n = norm(cross(sub(b, a), sub(c, a)))
                pa, pb, pc = vf(a), vf(b), vf(c)
                nn = vf(n); sh = 0.25 + 0.75 * abs(nn[0] * L[0] + nn[1] * L[1] + nn[2] * L[2])
                tri = [(col * S + S / 2 + q[0] * 900, row * S + S / 2 - q[1] * 900, q[2]) for q in (pa, pb, pc)]
                x0 = max(int(min(t[0] for t in tri)), col * S); x1 = min(int(max(t[0] for t in tri)) + 1, col * S + S)
                y0 = max(int(min(t[1] for t in tri)), row * S); y1 = min(int(max(t[1] for t in tri)) + 1, row * S + S)
                (ax_, ay, az), (bx, by, bz), (cx, cy, cz) = tri
                den = (by - cy) * (ax_ - cx) + (cx - bx) * (ay - cy)
                if abs(den) < 1e-12: continue
                for y in range(y0, y1):
                    for x in range(x0, x1):
                        l1 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / den
                        l2 = ((cy - ay) * (x - cx) + (ax_ - cx) * (y - cy)) / den
                        l3 = 1 - l1 - l2
                        if l1 < 0 or l2 < 0 or l3 < 0: continue
                        z = l1 * az + l2 * bz + l3 * cz
                        k = y * W + x
                        if z < zb[k]:
                            zb[k] = z; g = int(255 * sh)
                            img[k * 3:k * 3 + 3] = bytes((g, int(g * 0.9), int(g * 0.75))) if hand else bytes((g // 2,) * 3)
    png_write(path, W, H, img)


def main():
    a = sys.argv[1:]
    prev_path, glove, low, stick_mm, sleeve = None, (92, 86, 70), False, 36.0, (78, 84, 64)
    if '--sleeve' in a: k = a.index('--sleeve'); sleeve = tuple(int(x) for x in a[k + 1].split(',')); del a[k:k + 2]
    if '--stick-mm' in a: k = a.index('--stick-mm'); stick_mm = float(a[k + 1]); del a[k:k + 2]
    if '--preview' in a: k = a.index('--preview'); prev_path = a[k + 1]; del a[k:k + 2]
    if '--glove' in a: k = a.index('--glove'); glove = tuple(int(x) for x in a[k + 1].split(',')); del a[k:k + 2]
    if '--low' in a: low = True; a.remove('--low')
    src, out = a[0], a[1]
    tex = os.path.splitext(os.path.basename(out))[0] + '_glove.png'
    glove_texture(os.path.join(src, 'tex', 'cleaning_glove_baseColor.png'), os.path.join(os.path.dirname(out) or '.', tex), glove)
    ctex = os.path.splitext(os.path.basename(out))[0] + '_cuff.png'
    cuff_texture(os.path.join(os.path.dirname(out) or '.', ctex), sleeve)
    objs = []
    for side, right in (('L', False), ('R', True)):
        m = gltf_load(os.path.join(src, 'model', ('Hand_low_%s.gltf' if low else 'hand_%s.gltf') % (side if low else side.lower())))
        adir = os.path.join(src, 'anim', 'right' if right else 'left')
        _, _, jfit = skinned(m, godot_pose(os.path.join(adir, FIT_POSE + '.res')))
        Ffree, rfree = grip_frame(jfit, right)
        for obj, anim in POSES:
            pose = godot_pose(os.path.join(adir, anim + '.res'))
            if obj == 'grip':
                # between "Grip Shaft" (a thick shaft) and "Grip" (the controller): the blend whose finger joints lie on
                # the circle of a handle of --stick-mm diameter (joint centres ~9 mm out from the handle's surface)
                tight = godot_pose(os.path.join(adir, FIT_POSE + '.res'))
                want = stick_mm * 0.0005 + 0.009
                lo_t, hi_t = 0.0, 1.0
                for _ in range(20):
                    t = 0.5 * (lo_t + hi_t)
                    _, _, J = skinned_joints(m, blend(pose, tight, t))
                    if grip_frame(J, right)[1] > want: lo_t = t
                    else: hi_t = t
                pose = blend(pose, tight, t)
                print('%s grip: %.2f of the way from "%s" to "%s" for a %.0f mm handle' % (side, t, anim, FIT_POSE, stick_mm))
            P, N, J = skinned(m, pose)
            F, rad = (grip_frame(J, right) if obj == 'grip' else (Ffree, rfree))
            if obj in ('grip', 'fist'): print('%s %s: tube radius %.1f mm' % (side, obj, rad * 1000.0))
            VP = [to_frame(F, p) for p in P]
            tri = [m['idx'][i:i + 3] for i in range(0, len(m['idx']), 3)]
            if not right: tri = [t for t in tri]           # (the left model is mirrored with its winding fixed)
            VP2, refs = weld(VP, N, m['uv'], tri)
            objs.append(('hand_%s_%s' % (side, obj), VP2, refs, tex, 0))
            VPc, refc = cuff(VP2, F, J['Wrist'])
            objs.append(('hand_%s_%s_cuff' % (side, obj), VPc, refc, ctex, 1))
            lo = [min(p[k] for p in VP2) for k in range(3)]; hi = [max(p[k] for p in VP2) for k in range(3)]
            print('hand_%s_%s: %d vertices, %d triangles, X %.3f..%.3f Y %.3f..%.3f Z %.3f..%.3f m'
                  % (side, obj, len(VP2), len(refs), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))
    write_ac(out, objs)
    if prev_path:                                        # (each pose with its cuff)
        rows = []
        for (n_, v_, r_, _, _), (_, vc, rc, _, _) in zip(objs[0::2], objs[1::2]):
            rows.append((n_, v_ + vc, r_ + [[(i + len(v_), u, v) for i, u, v in q] for q in rc]))
        preview(prev_path, rows)


main()
