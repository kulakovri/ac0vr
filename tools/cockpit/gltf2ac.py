"""gltf2ac.py -- convert a glTF 2.0 (.gltf / .glb) or Wavefront OBJ (+ .mtl) model into what ac2cpit.py reads: an
AC3D text model (.ac) plus a small FlightGear-style PropertyList XML (the "shim") that loads it, with the textures as
PNG next to them. Used for models delivered as glTF or OBJ (the CC0 gloves of build_hands.sh).


Usage: python -I gltf2ac.py IN OUT_DIR NAME [options]      (IN: .gltf, .glb, .obj or DirectX text .x)
  writes OUT_DIR/NAME.ac, OUT_DIR/NAME.xml and OUT_DIR/<texture>.png; then
  python -I ac2cpit.py OUT_DIR/NAME.xml EYE_X EYE_Y EYE_Z OUT.cpit ... (eye in the converted FlightGear frame)
  --axes A,B,C   source axes giving FlightGear's x (aft), y (right), z (up), each a signed source axis; default
                 '-z,-x,y' (glTF: +Y up, the model's front at +Z, +X its left side). OBJ files have no convention:
                 give it (ModelConverterX's: z up, y forward: '-y,x,z'); .x files (left-handed, x right, y up, z forward
                 as Flight Simulator models): default '-z,x,y'
  --scale S      metres per source unit (Unreal: 0.01)
  --offset X,Y,Z added after --axes / --scale, FlightGear frame metres (e.g. to put the model's datum at 0)
  --skip REGEX   drop nodes (and their subtrees) whose names match
  --only-tex R / --skip-tex R   keep only / drop meshes whose source texture file name matches (untextured: '')
  --mat-skip R   drop the meshes whose material name matches
  --mat-color R=R,G,B[,A]   base colour (alpha) of the materials whose names match (a material the export lost; .x
                 too: a Flight Simulator glass colour)
  --mat-files D   exports whose glTF materials carry no textures; D/**/<material>.mat names the
                 diffuse texture (exported next to it as PNG): those materials take it, with a white base colour
  --drop-box X0,X1,Y0,Y1,Z0,Z1   drop triangles lying wholly inside this box (FlightGear frame metres, after --axes,
                 --scale, --offset): cuts the cockpit opening out of an exterior model (several allowed)
  --only REGEX   keep only mesh nodes whose names (or an ancestor's) match
  --crease DEG   crease angle written for every object (default 179: the source's own vertex sharing decides the
                 smoothing -- vertices are merged only where position and normal agree; 30 for sources without
                 normals)
  --xml-extra F  splice the elements of PropertyList file F (animations for the model's objects) into the shim
  --skin MODE    skinned meshes: 'rest' (default; posed by the joints' rest transforms, one object per primitive) or
                 'rigid' (each triangle goes to the joint with the largest weight on it: one object per joint, named
                 after the joint, so FlightGear-style animations can move it; the joints' rest positions and axes go to
                 OUT_DIR/NAME.joints.txt)
  --tex-dir D    texture search folder for images not found next to the input (several allowed)
  --node-names   name objects after their node even when the mesh has its own name (default: node name, else mesh)
  --tex-white    textured materials take a white base colour (Flight Simulator MDL materials through
                 ModelConverterX carry black or grey diffuse colours that FS does not apply to textured parts)
  --frame-turn F=A:DEG[;A:DEG...]   (.x) frame F's own rotation is turned first by DEG degrees about the parent's
                 source axis A (x, y or z), about the frame's origin: a Flight Simulator part's bind pose is its
                 animation's keyframe 0 (a stick full forward / left), turned back to rest here (repeatable)
  --frame-move F=DX,DY,DZ   (.x) frame F's origin moved by that much (source units, parent frame; repeatable)
  --name OLD=NEW rename the written object OLD (exact name) to NEW (repeatable): parts the runtime finds by name (the
                 HUD's combiner glass: ACZ_VR_HUD_GLASS_MESH; rear-view mirrors: ACZ_VR_COCKPIT_MIRRORS) or that the
                 build options name. .x and (since ck_rafale 2026-10-28) glTF input; a glTF mesh of several primitives
                 writes NAME_p0, NAME_p1, ...
  --split-uv NEW=OBJ:U0,V0,U1,V1   (.x) the triangles of object OBJ whose texture coordinates (AC3D's: v up) all lie
                 in that box go into a separate object NEW (same material): a display face painted on a panel
                 texture becomes its own part for ac2cpit --screen (repeatable)
  --split-pos NEW=OBJ:A<V | NEW=OBJ:A>V   (.x) the same by position: the triangles of object OBJ whose vertices all
                 have source coordinate A (x, y or z: the .x file's own, in the part's frame) below / above V go into
                 NEW: two displays that share one part and one texture box (the F-117A's MFD faces frm-46; ck_f117
                 2026-10-28; repeatable, after --split-uv; files built without it do not change)
  OBJ input (ck_look 2026-10-30; before, an OBJ took none of --name / --split-uv / --split-pos: a build's
  --name of the HUD's combiner had no effect): --name OLD=NEW as above; --split-uv NEW=OBJ:U0,V0,U1,V1[@X0,X1,Y0,Y1,Z0,Z1]
                 where OBJ is a regular expression on the object name (searched) and the optional box (FlightGear frame
                 metres after --axes / --scale / --offset, as --drop-box) also asks the triangle's centre to lie in it
                 (a display page an atlas uses twice, front and rear cockpit); then
  --split-box NEW=OBJ@X0,X1,Y0,Y1,Z0,Z1   (OBJ input) the triangles of the objects matching OBJ whose centre lies in
                 the box go into NEW (repeatable, after --split-uv)
  --split-pieces OBJ   (OBJ input) every connected piece (triangles sharing a position) of the objects matching OBJ
                 becomes its own object <name>_pc<k> (k in the order of their first triangle): a set of small cover
                 glasses drawn as one call (display and dial glass) is glass the runtime then sizes one by one
                 (ACZ_VR_CANOPY_OUTER_R: inner glass, no outside weather)
  --cut-behind OBJ   (OBJ input) the triangles of every other object whose centre lies beyond the surface of the
                 objects matching OBJ, seen from the origin (the eye of an eye-space export), are left out: the walls of
                 a capsule that a display dome covers with its picture of the outside. Files built
                 without these do not change
Textures: PNG copied, BMP / TGA / DDS decoded here; JPEG through System.Drawing (powershell.exe one-off: Python has no
JPEG decoder). Texture coordinates: glTF / OBJ top-left / bottom-left origins mapped to AC3D's (bottom-left).
Materials: base colour (factor), alpha (BLEND: factor alpha; OPAQUE / MASK: 1, the texture's alpha is cut at 0.5 by
the runtime), specular (glTF: 0.25 * (1 - roughness); OBJ Ks), shininess; two-sided -> AC3D surface flag 0x20.
Only reads the given files; no code from the downloaded data is executed.
"""
import base64, json, math, os, re, struct, subprocess, sys, zlib

# ---------------------------------------------------------------- small image codecs (to PNG)


def png_write(path, w, h, rgba):
    raw = b''.join(b'\0' + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def bmp_rgba(d):
    """BMP: 8-bit palettized, 24- and 32-bit uncompressed -> (w, h, RGBA rows top first)"""
    if d[:2] != b'BM': raise ValueError('not a BMP')
    off, = struct.unpack_from('<I', d, 10)
    hs, w, h, planes, bpp, comp = struct.unpack_from('<IiiHHI', d, 14)
    if comp not in (0, 3): raise ValueError('compressed BMP (%d)' % comp)
    flip = h > 0; h = abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    pal = d[14 + hs:off] if bpp <= 8 else b''
    out = bytearray(w * h * 4)
    for y in range(h):
        row = d[off + (h - 1 - y if flip else y) * stride:][:stride]
        o = y * w * 4
        for x in range(w):
            if bpp == 8:
                k = row[x]; b, g, r = pal[4 * k:4 * k + 3]; a = 255
            elif bpp == 24: b, g, r = row[3 * x:3 * x + 3]; a = 255
            elif bpp == 32: b, g, r, a = row[4 * x:4 * x + 4]
            else: raise ValueError('BMP %d bpp' % bpp)
            out[o + 4 * x:o + 4 * x + 4] = bytes((r, g, b, a))
    if bpp == 32 and not any(out[3::4]):                  # an unused alpha byte
        out[3::4] = bytes((255,)) * (w * h)
    return w, h, bytes(out)


def tga_rgba(d):
    idl, cmt, it, _, _, _, _, _, w, h, bpp, desc = struct.unpack_from('<BBBHHBHHHHBB', d, 0)
    if it not in (2, 10) or bpp not in (24, 32): raise ValueError('TGA type %d / %d bpp not supported' % (it, bpp))
    p = 18 + idl; n = bpp // 8; px = bytearray()
    if it == 2: px = bytearray(d[p:p + w * h * n])
    else:
        while len(px) < w * h * n:
            c = d[p]; p += 1; k = (c & 127) + 1
            if c & 128: px += d[p:p + n] * k; p += n
            else: px += d[p:p + n * k]; p += n * k
    out = bytearray(w * h * 4)
    top = bool(desc & 0x20)
    for y in range(h):
        sy = y if top else h - 1 - y
        for x in range(w):
            q = (sy * w + x) * n
            b, g, r = px[q:q + 3]; a = px[q + 3] if n == 4 else 255
            out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes((r, g, b, a))
    return w, h, bytes(out)


def dds_rgba(d):
    """DDS DXT1/3/5 or uncompressed 32-bit, top level, rows as stored (top first, as DirectX / glTF use them)"""
    if d[:4] != b'DDS ': raise ValueError('not a DDS')
    h, w = struct.unpack_from('<II', d, 12)
    pf_flags, = struct.unpack_from('<I', d, 80); four = d[84:88]; pos = 128
    out = bytearray(w * h * 4)
    if not pf_flags & 4:
        bits, rm, gm, bm, am = struct.unpack_from('<IIIII', d, 88)
        if bits != 32: raise ValueError('DDS %d-bit' % bits)
        sh = [(m & -m).bit_length() - 1 for m in (rm, gm, bm, am)]
        for i, v in enumerate(struct.unpack_from('<%dI' % (w * h), d, pos)):
            out[i * 4:i * 4 + 4] = bytes(((v >> sh[0]) & 255, (v >> sh[1]) & 255, (v >> sh[2]) & 255,
                                          ((v >> sh[3]) & 255) if am else 255))
        return w, h, bytes(out)
    if four not in (b'DXT1', b'DXT3', b'DXT5'): raise ValueError('DDS %r' % four)
    bsz = 8 if four == b'DXT1' else 16

    def c565(c):
        r, g, b = (c >> 11) & 31, (c >> 5) & 63, c & 31
        return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))
    for by in range((h + 3) // 4):
        for bx in range((w + 3) // 4):
            blk = d[pos:pos + bsz]; pos += bsz
            c0, c1, idx = struct.unpack_from('<HHI', blk, bsz - 8)
            p0, p1 = c565(c0), c565(c1)
            if c0 > c1 or four != b'DXT1':
                pal = [p0 + (255,), p1 + (255,), tuple((2 * a + b) // 3 for a, b in zip(p0, p1)) + (255,),
                       tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)) + (255,)]
            else:
                pal = [p0 + (255,), p1 + (255,), tuple((a + b) // 2 for a, b in zip(p0, p1)) + (255,), (0, 0, 0, 0)]
            alpha = None
            if four == b'DXT3':
                a64, = struct.unpack_from('<Q', blk, 0); alpha = [((a64 >> (4 * k)) & 15) * 17 for k in range(16)]
            elif four == b'DXT5':
                a0, a1 = blk[0], blk[1]; ai, = struct.unpack_from('<Q', blk[2:8] + b'\0\0', 0)
                ap = ([a0, a1] + [((7 - k) * a0 + k * a1) // 7 for k in range(1, 7)] if a0 > a1 else
                      [a0, a1] + [((5 - k) * a0 + k * a1) // 5 for k in range(1, 5)] + [0, 255])
                alpha = [ap[(ai >> (3 * k)) & 7] for k in range(16)]
            for k in range(16):
                x, y = bx * 4 + (k & 3), by * 4 + (k >> 2)
                if x < w and y < h:
                    c = pal[(idx >> (2 * k)) & 3]
                    out[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(c[:3] + ((alpha[k],) if alpha else (c[3],)))
    return w, h, bytes(out)


def jpeg_to_png(src, dst):
    """JPEG -> PNG with System.Drawing (powershell.exe one-off: no JPEG decoder in Python's standard library)"""
    def win(p): return os.path.abspath(p).replace('/', '\\')
    cmd = ("Add-Type -AssemblyName System.Drawing; $i = [System.Drawing.Image]::FromFile('%s'); "
           "$i.Save('%s', [System.Drawing.Imaging.ImageFormat]::Png); $i.Dispose()" % (win(src), win(dst)))
    subprocess.run(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', cmd], check=True)


def save_texture(data, name_hint, out_dir, done):
    """image bytes -> OUT_DIR/<stem>.png (once per name); returns the file name or None"""
    stem = re.sub(r'[^A-Za-z0-9_.-]+', '_', os.path.splitext(os.path.basename(name_hint))[0]) or 'tex'
    key = (stem, len(data), zlib.crc32(data))
    if key in done: return done[key]
    fn = stem + '.png'
    k = 1
    while fn in done.values(): fn = '%s_%d.png' % (stem, k); k += 1
    dst = os.path.join(out_dir, fn)
    try:
        if data[:8] == b'\x89PNG\r\n\x1a\n':
            open(dst, 'wb').write(data)
        elif data[:2] == b'\xff\xd8':
            tmp = dst + '.jpg'; open(tmp, 'wb').write(data)
            jpeg_to_png(tmp, dst); os.remove(tmp)
        else:
            if data[:2] == b'BM': w, h, px = bmp_rgba(data)
            elif data[:4] == b'DDS ': w, h, px = dds_rgba(data)
            elif name_hint.lower().endswith('.tga'): w, h, px = tga_rgba(data)
            else: raise ValueError('unknown image format')
            png_write(dst, w, h, px)
    except Exception as e:
        print('texture %s skipped: %s' % (name_hint, e))
        fn = None
    done[key] = fn
    return fn

# ---------------------------------------------------------------- maths (3x4 affine, rows)


def qmat(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def node_local(n):
    if 'matrix' in n:
        m = n['matrix']
        return [[m[0], m[4], m[8], m[12]], [m[1], m[5], m[9], m[13]], [m[2], m[6], m[10], m[14]]]
    R = qmat(n.get('rotation', [0, 0, 0, 1])); s = n.get('scale', [1, 1, 1]); t = n.get('translation', [0, 0, 0])
    return [[R[i][0] * s[0], R[i][1] * s[1], R[i][2] * s[2], t[i]] for i in range(3)]


def amul(A, B):
    return [[sum(A[i][k] * B[k][j] for k in range(3)) + (A[i][3] if j == 3 else 0.0) for j in range(4)] for i in range(3)]


def apt(M, v): return [M[i][0] * v[0] + M[i][1] * v[1] + M[i][2] * v[2] + M[i][3] for i in range(3)]


def adir(M, v): return [M[i][0] * v[0] + M[i][1] * v[1] + M[i][2] * v[2] for i in range(3)]


def ainv(M):
    a = [r[:3] for r in M]
    det = (a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
           a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]))
    inv = [[(a[(j + 1) % 3][(i + 1) % 3] * a[(j + 2) % 3][(i + 2) % 3] -
             a[(j + 1) % 3][(i + 2) % 3] * a[(j + 2) % 3][(i + 1) % 3]) / det for j in range(3)] for i in range(3)]
    t = [-sum(inv[i][k] * M[k][3] for k in range(3)) for i in range(3)]
    return [inv[i] + [t[i]] for i in range(3)]


I34 = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]]


def axes_matrix(spec):
    """'-z,-x,y' -> rows of FG = A * source"""
    rows = []
    for t in spec.split(','):
        t = t.strip().lower(); sgn = -1.0 if t.startswith('-') else 1.0
        r = [0.0, 0.0, 0.0]; r['xyz'.index(t.lstrip('+-'))] = sgn; rows.append(r)
    return rows

# ---------------------------------------------------------------- readers -> meshes in source space
# a mesh: dict(name, pos [[x,y,z]], nrm [[...]] or None, uv [[u,v]] (AC3D convention, v up) or None,
#              tris [(a,b,c)], mat dict(rgba, spec, shi, tex, two, emis))


UMATS = {}           # --mat-files: material name -> texture file (<material>.mat with Diffuse=<texture>)
MAT_COLOR = []       # --mat-color REGEX=R,G,B[,A]: base colour (and alpha) of the materials whose names match


def mat_files(root):
    """an export folder -> {material name: diffuse texture file}"""
    files = {}
    for d, _, fs in os.walk(root):
        for f in fs: files.setdefault(f.lower(), os.path.abspath(os.path.join(d, f)))
    out = {}
    for low, path in files.items():
        if not low.endswith('.mat'): continue
        for ln in open(path, 'r', encoding='latin-1'):
            k, _, v = ln.strip().partition('=')
            if k.lower() == 'diffuse':
                for ext in ('.png', '.tga', '.dds'):
                    if (v.lower() + ext) in files: out[os.path.splitext(os.path.basename(path))[0]] = files[v.lower() + ext]; break
    return out


class Gltf:
    CT = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}
    NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}

    def __init__(self, path, tex_dirs):
        self.path = path; self.base = os.path.dirname(path); self.tex_dirs = tex_dirs
        d = open(path, 'rb').read()
        glb = None
        if d[:4] == b'glTF':
            p = 12; js = None
            while p < len(d):
                n, t = struct.unpack_from('<I4s', d, p)
                if t == b'JSON': js = d[p + 8:p + 8 + n]
                elif t == b'BIN\0': glb = d[p + 8:p + 8 + n]
                p += 8 + n
            self.g = json.loads(js)
        else: self.g = json.loads(d.decode('utf-8'))
        self.bufs = []
        for b in self.g.get('buffers', []):
            u = b.get('uri')
            if u is None: self.bufs.append(glb)
            elif u.startswith('data:'): self.bufs.append(base64.b64decode(u.split(',', 1)[1]))
            else: self.bufs.append(open(os.path.join(self.base, self.unq(u)), 'rb').read())

    @staticmethod
    def unq(u):
        return re.sub(r'%([0-9A-Fa-f]{2})', lambda m: chr(int(m.group(1), 16)), u)

    def acc(self, i):
        a = self.g['accessors'][i]
        n, c = a['count'], self.NC[a['type']]; f = self.CT[a['componentType']]
        if 'sparse' in a: raise ValueError('sparse accessors not supported')
        if 'bufferView' not in a: return [tuple([0] * c)] * n
        bv = self.g['bufferViews'][a['bufferView']]
        sz = struct.calcsize(f); stride = bv.get('byteStride') or sz * c
        off = bv.get('byteOffset', 0) + a.get('byteOffset', 0); b = self.bufs[bv['buffer']]
        fmt = '<%d%s' % (c, f)
        vals = [struct.unpack_from(fmt, b, off + k * stride) for k in range(n)]
        if a.get('normalized') and f != 'f':
            mx = {'b': 127.0, 'B': 255.0, 'h': 32767.0, 'H': 65535.0, 'I': 4294967295.0}[f]
            vals = [tuple(max(x / mx, -1.0) for x in v) for v in vals]
        return vals

    def image_bytes(self, ii):
        im = self.g['images'][ii]
        if 'bufferView' in im:
            bv = self.g['bufferViews'][im['bufferView']]
            b = self.bufs[bv['buffer']]; o = bv.get('byteOffset', 0)
            return b[o:o + bv['byteLength']], im.get('name') or 'image%d' % ii
        u = im.get('uri', '')
        if u.startswith('data:'): return base64.b64decode(u.split(',', 1)[1]), im.get('name') or 'image%d' % ii
        u = self.unq(u)
        for d in [self.base] + self.tex_dirs:
            p = os.path.join(d, u)
            if os.path.exists(p): return open(p, 'rb').read(), u
            p = os.path.join(d, os.path.basename(u))
            if os.path.exists(p): return open(p, 'rb').read(), u
        print('image not found:', u)
        return None, u

    def material(self, mi):
        if mi is None: return dict(rgba=(0.8, 0.8, 0.8, 1.0), spec=0.0, shi=16.0, tex=None, two=False, emis=(0, 0, 0))
        m = self.g['materials'][mi]
        pbr = m.get('pbrMetallicRoughness', {})
        bc = list(pbr.get('baseColorFactor', [1, 1, 1, 1]))
        mode = m.get('alphaMode', 'OPAQUE')
        a = bc[3] if mode == 'BLEND' else 1.0
        tex = None
        bt = pbr.get('baseColorTexture')
        sg = m.get('extensions', {}).get('KHR_materials_pbrSpecularGlossiness')
        if sg:                                             # older exporters
            bc = list(sg.get('diffuseFactor', bc)); bt = sg.get('diffuseTexture', bt)
            a = bc[3] if mode == 'BLEND' else 1.0
        if bt is not None:
            t = self.g['textures'][bt['index']]
            src = t.get('source')
            if src is None:
                ex = t.get('extensions', {})
                for k in ('MSFT_texture_dds', 'KHR_texture_basisu', 'EXT_texture_webp'):
                    if k in ex: src = ex[k].get('source'); break
            if src is not None: tex = ('gltf', src, bt.get('texCoord', 0), bt.get('extensions', {}).get('KHR_texture_transform'))
        rough = pbr.get('roughnessFactor', 1.0)
        nm = m.get('name', 'mat%d' % mi)
        if tex is None and nm in UMATS:                  # the texture is named in <material>.mat
            tex = ('file', UMATS[nm]); bc = [1.0, 1.0, 1.0, bc[3]]
        for rx_, col in MAT_COLOR:                        # --mat-color
            if rx_.search(nm): bc = list(col) + ([bc[3]] if len(col) == 3 else []); a = bc[3] if len(col) > 3 else a
        return dict(rgba=(bc[0], bc[1], bc[2], a), spec=0.25 * (1.0 - rough), shi=max(2.0, 128.0 * (1.0 - rough) ** 2),
                    tex=tex, two=bool(m.get('doubleSided')), emis=tuple(m.get('emissiveFactor', [0, 0, 0])),
                    name=nm, mask=mode == 'MASK')

    def prim_mesh(self, pr):
        at = pr['attributes']
        pos = [list(v) for v in self.acc(at['POSITION'])]
        nrm = [list(v) for v in self.acc(at['NORMAL'])] if 'NORMAL' in at else None
        mat = self.material(pr.get('material'))
        tc = mat['tex'][2] if mat['tex'] and mat['tex'][0] == 'gltf' else 0
        uvk = 'TEXCOORD_%d' % tc
        uv = None
        if uvk in at:
            tt = mat['tex'][3] if mat['tex'] and mat['tex'][0] == 'gltf' else None
            uv = []
            for u, v in self.acc(at[uvk]):
                if tt:                                     # KHR_texture_transform: scale, rotate, offset
                    s = tt.get('scale', [1, 1]); r = tt.get('rotation', 0.0); o = tt.get('offset', [0, 0])
                    u, v = u * s[0], v * s[1]
                    u, v = math.cos(r) * u + math.sin(r) * v, -math.sin(r) * u + math.cos(r) * v
                    u, v = u + o[0], v + o[1]
                uv.append([u, 1.0 - v])                     # glTF: v down from the top; AC3D: v up
        mode = pr.get('mode', 4)
        idx = [i[0] for i in self.acc(pr['indices'])] if 'indices' in pr else list(range(len(pos)))
        if mode == 4: tris = [tuple(idx[k:k + 3]) for k in range(0, len(idx) - 2, 3)]
        elif mode == 5: tris = [tuple(idx[k:k + 3]) if k % 2 == 0 else (idx[k + 1], idx[k], idx[k + 2]) for k in range(len(idx) - 2)]
        elif mode == 6: tris = [(idx[0], idx[k], idx[k + 1]) for k in range(1, len(idx) - 1)]
        else: tris = []
        joints = weights = None
        if 'JOINTS_0' in at and 'WEIGHTS_0' in at:
            joints = self.acc(at['JOINTS_0']); weights = self.acc(at['WEIGHTS_0'])
        return dict(pos=pos, nrm=nrm, uv=uv, tris=tris, mat=mat, joints=joints, weights=weights)


def read_obj(path, tex_dirs):
    """OBJ + MTL -> list of (name, mesh) in source space (no hierarchy: the file is already in one frame)"""
    base = os.path.dirname(path)
    V, VT, VN = [], [], []
    mats, cur_mat, cur_name = {}, None, 'object'
    groups = {}                                            # (name, mat) -> list of faces of (v, vt, vn)
    order = []

    def load_mtl(fn):
        p = os.path.join(base, fn)
        if not os.path.exists(p): print('mtl not found:', fn); return
        m = None
        for ln in open(p, 'r', encoding='latin-1'):
            t = ln.split()
            if not t: continue
            k = t[0]
            if k == 'newmtl': m = mats.setdefault(ln.strip()[7:].strip(), dict(Kd=(0.8, 0.8, 0.8), d=1.0, Ks=(0, 0, 0), Ns=16.0, map=None))
            elif m is None: continue
            elif k == 'Kd': m['Kd'] = tuple(float(x) for x in t[1:4])
            elif k == 'Ks': m['Ks'] = tuple(float(x) for x in t[1:4])
            elif k == 'Ns': m['Ns'] = float(t[1])
            elif k == 'd': m['d'] = float(t[1])
            elif k == 'Tr': m['d'] = 1.0 - float(t[1])
            elif k == 'map_Kd': m['map'] = ln.strip().split(None, 1)[1].split()[-1]
    for ln in open(path, 'r', encoding='latin-1'):
        t = ln.split()
        if not t: continue
        k = t[0]
        if k == 'v': V.append([float(x) for x in t[1:4]])
        elif k == 'vt': VT.append([float(t[1]), float(t[2]) if len(t) > 2 else 0.0])
        elif k == 'vn': VN.append([float(x) for x in t[1:4]])
        elif k in ('o', 'g'): cur_name = ln.strip()[2:].strip() or cur_name
        elif k == 'usemtl': cur_mat = ln.strip()[7:].strip()
        elif k == 'mtllib': load_mtl(ln.strip()[7:].strip())
        elif k == 'f':
            refs = []
            for c in t[1:]:
                s = c.split('/')
                vi = int(s[0]); vi = vi - 1 if vi > 0 else len(V) + vi
                ti = int(s[1]) if len(s) > 1 and s[1] else None
                ni = int(s[2]) if len(s) > 2 and s[2] else None
                if ti is not None: ti = ti - 1 if ti > 0 else len(VT) + ti
                if ni is not None: ni = ni - 1 if ni > 0 else len(VN) + ni
                refs.append((vi, ti, ni))
            key = (cur_name, cur_mat)
            if key not in groups: groups[key] = []; order.append(key)
            for j in range(1, len(refs) - 1): groups[key].append((refs[0], refs[j], refs[j + 1]))
    out = []
    for key in order:
        name, mn = key
        m = mats.get(mn, dict(Kd=(0.8, 0.8, 0.8), d=1.0, Ks=(0, 0, 0), Ns=16.0, map=None))
        pos, nrm, uv, tris, vid = [], [], [], [], {}
        has_n = all(r[2] is not None for f in groups[key] for r in f)
        has_t = all(r[1] is not None for f in groups[key] for r in f)
        for f in groups[key]:
            tri = []
            for r in f:
                if r not in vid:
                    vid[r] = len(pos); pos.append(V[r[0]])
                    nrm.append(VN[r[2]] if has_n else None); uv.append(VT[r[1]] if has_t else None)
                tri.append(vid[r])
            tris.append(tuple(tri))
        mat = dict(rgba=tuple(m['Kd']) + (m['d'],), spec=sum(m['Ks']) / 3.0, shi=m['Ns'], two=False, emis=(0, 0, 0),
                   tex=('file', m['map']) if m['map'] else None, name=mn or 'default', mask=False)
        out.append((name, dict(pos=pos, nrm=nrm if has_n else None, uv=uv if has_t else None, tris=tris, mat=mat)))
    return out

def read_x(path):
    """DirectX text .x (flight-simulator exporters write models this way, the virtual cockpit as <name>_interior_
    <lod>.x): frames with their FrameTransformMatrix, meshes with normals, texture coordinates and materials ->
    a node tree [(name, local 3x4, [meshes], [children])]; source frame left-handed (x right, y up, z forward)"""
    txt = open(path, 'r', encoding='latin-1').read()
    if not txt.startswith('xof ') or txt[8:12] != 'txt ': raise ValueError('not a text .x file')
    txt = re.sub(r'(//|#)[^\n]*', '', txt[16:])
    toks = re.findall(r'"[^"]*"|<[^>]*>|[{}]|[^\s;,{}"<]+', txt)
    pos = 0

    def block():
        """at an identifier: (type, name, data tokens, sub-blocks)"""
        nonlocal pos
        typ = toks[pos]; pos += 1
        name = ''
        if toks[pos] != '{': name = toks[pos]; pos += 1
        assert toks[pos] == '{', 'x: { expected after %s' % typ
        pos += 1
        data, subs = [], []
        while toks[pos] != '}':
            t = toks[pos]
            if t == '{':                                   # a reference { name }
                data.append(('ref', toks[pos + 1])); pos += 3; continue
            if t.startswith('<'): pos += 1; continue
            if re.match(r'[A-Za-z_]', t) and (toks[pos + 1] == '{' or (toks[pos + 2] == '{' and toks[pos + 1] not in '{}')):
                subs.append(block()); continue
            data.append(t); pos += 1
        pos += 1
        return typ, name, data, subs
    top = []
    while pos < len(toks):
        if toks[pos] == 'template':                        # skip template declarations
            depth = 0
            while True:
                t = toks[pos]; pos += 1
                if t == '{': depth += 1
                elif t == '}':
                    depth -= 1
                    if depth == 0: break
            continue
        top.append(block())
    mats_by_name = {}

    def material(b):
        _, nm, d, subs = b
        f = [float(x) for x in d[:11]]
        tex = None
        for s in subs:
            if s[0] in ('TextureFilename', 'TextureFileName', 'DiffuseTextureFileName') and s[2]:
                tex = s[2][0].strip('"'); break
        m = dict(rgba=tuple(f[0:4]), spec=sum(f[5:8]) / 3.0 if f[4] > 0 else 0.0, shi=max(f[4], 2.0), tex=('file', tex) if tex else None,
                 two=False, emis=tuple(f[8:11]), name=nm or 'mat', mask=False)
        for s in subs:
            if s[0] == 'DoubleSidedMaterial' and s[2] and s[2][0].upper() in ('1', 'TRUE'): m['two'] = True
        for rx_, col in MAT_COLOR:                        # --mat-color (.x too since ck_standin 2026-10-28)
            if rx_.search(m['name']):
                m['rgba'] = tuple(col) if len(col) > 3 else tuple(col) + (m['rgba'][3],); break
        if nm: mats_by_name[nm] = m
        return m
    for b in top:
        if b[0] == 'Material': material(b)

    def mesh(b):
        _, nm, d, subs = b
        p = 0
        nv = int(d[p]); p += 1
        V = [[float(d[p + 3 * k]), float(d[p + 3 * k + 1]), float(d[p + 3 * k + 2])] for k in range(nv)]; p += 3 * nv
        nf = int(d[p]); p += 1
        F = []
        for _ in range(nf):
            c = int(d[p]); F.append([int(x) for x in d[p + 1:p + 1 + c]]); p += 1 + c
        N = NF = UV = None; FM = [0] * nf; ML = []
        for s in subs:
            sd = s[2]
            if s[0] == 'MeshNormals':
                q = 0; nn = int(sd[q]); q += 1
                N = [[float(sd[q + 3 * k]), float(sd[q + 3 * k + 1]), float(sd[q + 3 * k + 2])] for k in range(nn)]; q += 3 * nn
                nnf = int(sd[q]); q += 1; NF = []
                for _ in range(nnf):
                    c = int(sd[q]); NF.append([int(x) for x in sd[q + 1:q + 1 + c]]); q += 1 + c
            elif s[0] == 'MeshTextureCoords':
                nt = int(sd[0]); UV = [[float(sd[1 + 2 * k]), 1.0 - float(sd[2 + 2 * k])] for k in range(nt)]
            elif s[0] == 'MeshMaterialList':
                nmat = int(sd[0]); nfi = int(sd[1])
                FM = [int(x) for x in sd[2:2 + nfi]]
                FM += [FM[-1] if FM else 0] * (nf - len(FM))
                refs = [x[1] for x in sd[2 + nfi:] if isinstance(x, tuple)]
                for r in refs: ML.append(mats_by_name.get(r))
                for ss in s[3]:
                    if ss[0] == 'Material': ML.append(material(ss))
        out = []
        for mi in sorted(set(FM)):
            m = ML[mi] if mi < len(ML) and ML[mi] else dict(rgba=(0.8, 0.8, 0.8, 1), spec=0, shi=16, tex=None, two=False, emis=(0, 0, 0), name='default', mask=False)
            pos_, nrm, uv, tris, vid = [], [], [], [], {}
            for fi, f in enumerate(F):
                if FM[fi] != mi: continue
                corners = []
                for k, vi in enumerate(f):
                    ni = NF[fi][k] if NF and fi < len(NF) and k < len(NF[fi]) else None
                    key = (vi, ni)
                    if key not in vid:
                        vid[key] = len(pos_); pos_.append(V[vi])
                        nrm.append(N[ni] if ni is not None else None); uv.append(UV[vi] if UV else None)
                    corners.append(vid[key])
                # (clockwise in the left-handed source = counter-clockwise after the mirroring --axes)
                for k in range(1, len(corners) - 1): tris.append((corners[0], corners[k], corners[k + 1]))
            out.append(dict(pos=pos_, nrm=nrm if N else None, uv=uv if UV else None, tris=tris, mat=m, name=nm))
        return out

    def frame(b):
        _, nm, d, subs = b
        M = [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]]
        meshes, kids = [], []
        for s in subs:
            if s[0] == 'FrameTransformMatrix':
                f = [float(x) for x in s[2][:16]]          # row vectors: v' = v M, translation in the last row
                M = [[f[0], f[4], f[8], f[12]], [f[1], f[5], f[9], f[13]], [f[2], f[6], f[10], f[14]]]
            elif s[0] == 'Mesh': meshes.extend(mesh(s))
            elif s[0] == 'Frame': kids.append(frame(s))
        return (nm or 'frame', M, meshes, kids)
    roots = []
    for b in top:
        if b[0] == 'Frame': roots.append(frame(b))
        elif b[0] == 'Mesh': roots.append(('mesh', [[1.0, 0, 0, 0], [0, 1.0, 0, 0], [0, 0, 1.0, 0]], mesh(b), []))
    return roots

# ---------------------------------------------------------------- AC3D writer


def fmt(x):
    s = '%.6g' % x
    return '0' if s in ('-0', '0') else s


class AcWriter:
    def __init__(self):
        self.mats = []; self.mat_ids = {}; self.lines = []
        self.tri_filter = None                             # (mesh, triangle) -> keep? (--drop-box)

    def mat(self, m, name):
        r, g, b, a = m['rgba']; e = m.get('emis', (0, 0, 0)); sp = m.get('spec', 0.0)
        key = (round(r, 5), round(g, 5), round(b, 5), round(a, 5), round(sp, 5), round(m.get('shi', 16.0), 3),
               tuple(round(x, 5) for x in e))
        if key not in self.mat_ids:
            self.mat_ids[key] = len(self.mats)
            self.mats.append('MATERIAL "%s" rgb %s %s %s amb %s %s %s emis %s %s %s spec %s %s %s shi %s trans %s' % (
                re.sub(r'["\s]+', '_', name)[:40], fmt(r), fmt(g), fmt(b), fmt(r), fmt(g), fmt(b), fmt(e[0]), fmt(e[1]),
                fmt(e[2]), fmt(sp), fmt(sp), fmt(sp), fmt(m.get('shi', 16.0)), fmt(1.0 - a)))
        return self.mat_ids[key]

    def group(self, name, M, nkids):
        L = self.lines
        L.append('OBJECT group'); L.append('name "%s"' % name)
        L.append('loc %s %s %s' % (fmt(M[0][3]), fmt(M[1][3]), fmt(M[2][3])))
        L.append('rot ' + ' '.join(fmt(M[i][j]) for i in range(3) for j in range(3)))
        L.append('kids %d' % nkids)

    def poly(self, name, mesh, mi, tex, crease):
        """mesh in the parent's frame; vertices merged where position and normal agree"""
        L = self.lines
        pos, nrm, uv, tris = mesh['pos'], mesh['nrm'], mesh['uv'], mesh['tris']
        vmap, verts = {}, []
        refs_all = []
        for t in tris:
            if len(set(t)) < 3: continue
            if self.tri_filter and not self.tri_filter(mesh, t): continue
            refs = []
            for k in t:
                p = pos[k]
                key = (round(p[0], 5), round(p[1], 5), round(p[2], 5))
                if nrm is not None and nrm[k] is not None:
                    n = nrm[k]; key += (round(n[0], 2), round(n[1], 2), round(n[2], 2))
                if key not in vmap: vmap[key] = len(verts); verts.append(p)
                refs.append((vmap[key], uv[k] if uv is not None and uv[k] is not None else (0.0, 0.0)))
            refs_all.append(refs)
        if not refs_all: return False
        L.append('OBJECT poly'); L.append('name "%s"' % name)
        if tex: L.append('texture "%s"' % tex)
        L.append('crease %s' % fmt(crease))
        L.append('numvert %d' % len(verts))
        for p in verts: L.append('%s %s %s' % (fmt(p[0]), fmt(p[1]), fmt(p[2])))
        L.append('numsurf %d' % len(refs_all))
        flags = '0x30' if mesh['mat'].get('two') else '0x10'
        for refs in refs_all:
            L.append('SURF %s' % flags); L.append('mat %d' % mi); L.append('refs 3')
            for vi, (u, v) in refs: L.append('%d %s %s' % (vi, fmt(u), fmt(v)))
        L.append('kids 0')
        return True

    def text(self, root_lines):
        return 'AC3Db\n' + '\n'.join(self.mats) + '\n' + '\n'.join(root_lines + self.lines) + '\n'

# ---------------------------------------------------------------- main


def main():
    a = sys.argv[1:]

    def opt(name, default=None, many=False):
        vals = []
        while name in a:
            k = a.index(name); vals.append(a[k + 1]); del a[k:k + 2]
            if not many: break
        return vals if many else (vals[0] if vals else default)

    def flag(name):
        if name in a: a.remove(name); return True
        return False
    axes = opt('--axes'); scale = float(opt('--scale', '1')); offset = [float(x) for x in opt('--offset', '0,0,0').split(',')]
    skip = opt('--skip'); only = opt('--only'); crease_o = opt('--crease'); xml_extra = opt('--xml-extra')
    skin_mode = opt('--skin', 'rest'); tex_dirs = opt('--tex-dir', many=True); node_names = flag('--node-names')
    tex_white = flag('--tex-white')
    um = opt('--mat-files')
    if um: UMATS.update(mat_files(um)); print('material files:', ', '.join('%s=%s' % (k, os.path.basename(v)) for k, v in UMATS.items()))
    for mc in opt('--mat-color', many=True):
        rx_, _, col = mc.rpartition('='); MAT_COLOR.append((re.compile(rx_, re.I), tuple(float(x) for x in col.split(','))))
    mat_skip = opt('--mat-skip'); mat_skip_rx = re.compile(mat_skip, re.I) if mat_skip else None
    only_tex = opt('--only-tex'); skip_tex = opt('--skip-tex')
    boxes = [[float(x) for x in b.split(',')] for b in opt('--drop-box', many=True)]
    frame_turn, frame_move, renames, splits = {}, {}, {}, []
    for s_ in opt('--frame-turn', many=True):
        f_, _, t_ = s_.partition('=')
        frame_turn.setdefault(f_, []).extend((x.split(':')[0].strip().lower(), float(x.split(':')[1])) for x in t_.split(';'))
    for s_ in opt('--frame-move', many=True):
        f_, _, t_ = s_.partition('='); frame_move[f_] = [float(x) for x in t_.split(',')]
    for s_ in opt('--name', many=True):
        o_, _, n_ = s_.partition('='); renames[o_] = n_
    uvboxes = []                                           # (ck_look: the optional @box of an OBJ's --split-uv)
    for s_ in opt('--split-uv', many=True):
        n_, _, r_ = s_.partition('='); o_, _, b_ = r_.rpartition(':'); b_, _, pb_ = b_.partition('@')
        splits.append((n_, o_, [float(x) for x in b_.split(',')]))
        uvboxes.append([float(x) for x in pb_.split(',')] if pb_ else None)
    psplits = []
    for s_ in opt('--split-pos', many=True):
        n_, _, r_ = s_.partition('='); o_, _, c_ = r_.rpartition(':')
        ax_ = 'xyz'.index(c_[0].lower()); lt_ = c_[1] == '<'
        psplits.append((n_, o_, ax_, lt_, float(c_[2:])))
    bsplits = []                                           # ck_look 2026-10-30 (OBJ input): --split-box, --split-pieces
    for s_ in opt('--split-box', many=True):
        n_, _, r_ = s_.partition('='); o_, _, b_ = r_.rpartition('@')
        bsplits.append((n_, re.compile(o_), [float(x) for x in b_.split(',')]))
    pieces_rx = [re.compile(x) for x in opt('--split-pieces', many=True)]
    cut_o = opt('--cut-behind'); cut_rx = re.compile(cut_o) if cut_o else None
    only_tex_rx = re.compile(only_tex, re.I) if only_tex else None
    skip_tex_rx = re.compile(skip_tex, re.I) if skip_tex else None
    src, out_dir, name = a[0], a[1], a[2]
    os.makedirs(out_dir, exist_ok=True)
    skip_rx = re.compile(skip, re.I) if skip else None
    only_rx = re.compile(only, re.I) if only else None
    is_obj = src.lower().endswith('.obj')
    A = axes_matrix(axes or ('-z,x,y' if src.lower().endswith('.x') else '-z,-x,y'))
    B = [[1, 0, 0], [0, 0, 1], [0, -1, 0]]                 # FlightGear -> AC3D (ac2cpit's AC_TO_FG inverted)
    BA = [[sum(B[i][k] * A[k][j] for k in range(3)) * scale for j in range(3)] for i in range(3)]
    bo = [sum(B[i][k] * offset[k] for k in range(3)) for i in range(3)]
    root = [BA[i] + [bo[i]] for i in range(3)]
    w = AcWriter()
    tex_done = {}
    cur_world = [root]                                     # source -> AC3D frame of the meshes being written

    def in_boxes(mesh, t):
        for k in t:
            q = apt(cur_world[0], mesh['pos'][k]); f = (q[0], -q[2], q[1])              # AC3D -> FlightGear
            if not any(b[0] <= f[0] <= b[1] and b[2] <= f[1] <= b[3] and b[4] <= f[2] <= b[5] for b in boxes):
                return True
        return False
    if boxes: w.tri_filter = in_boxes
    nobj = ntri = 0
    joints_txt = []

    def tex_name(mat, reader):
        t = mat.get('tex')
        if not t: return None
        if t[0] == 'gltf':
            data, hint = reader.image_bytes(t[1])
        else:
            data, hint = None, t[1]
            for d in [os.path.dirname(src)] + tex_dirs:
                p = os.path.join(d, t[1])
                if os.path.exists(p): data = open(p, 'rb').read(); break
            if data is None: print('texture not found:', t[1])
        return save_texture(data, hint, out_dir, tex_done) if data else None

    def keep(mat, reader):
        """--only-tex / --skip-tex on the source texture name ('' when untextured); --mat-skip on the material name"""
        if mat_skip_rx and mat_skip_rx.search(mat.get('name', '')): return False
        if not (only_tex_rx or skip_tex_rx): return True
        t = mat.get('tex')
        nm = '' if not t else (t[1] if t[0] == 'file' else (reader.g['images'][t[1]].get('uri') or reader.g['images'][t[1]].get('name') or ''))
        nm = os.path.basename(nm or '')
        if only_tex_rx and not only_tex_rx.search(nm): return False
        return not (skip_tex_rx and skip_tex_rx.search(nm))

    if src.lower().endswith('.x'):
        crease = float(crease_o) if crease_o else 179.0
        names_used = {}

        def uniq(nm):
            nm = re.sub(r'["\r\n]+', '_', nm or 'node')
            k = names_used.get(nm, 0); names_used[nm] = k + 1
            return nm if not k else '%s_%d' % (nm, k)

        def emit_x(node, anc_match, Mp=root):
            nonlocal nobj, ntri
            nm, M, meshes, kids_ = node
            for ax_, deg_ in frame_turn.get(nm, ()):           # --frame-turn: about the parent's axis, at the origin
                c_, s_ = math.cos(math.radians(deg_)), math.sin(math.radians(deg_))
                Q = {'x': [[1, 0, 0], [0, c_, -s_], [0, s_, c_]], 'y': [[c_, 0, s_], [0, 1, 0], [-s_, 0, c_]],
                     'z': [[c_, -s_, 0], [s_, c_, 0], [0, 0, 1]]}[ax_]
                M = [[sum(Q[i][k] * M[k][j] for k in range(3)) for j in range(3)] + [M[i][3]] for i in range(3)]
            if nm in frame_move:
                M = [M[i][:3] + [M[i][3] + frame_move[nm][i]] for i in range(3)]
            Mw = amul(Mp, M); cur_world[0] = Mw
            if skip_rx and skip_rx.search(nm): return 0
            match = anc_match or (only_rx.search(nm) if only_rx else True)
            start = len(w.lines); w.lines.extend([None] * 5)
            kids = 0
            if match:
                for k, m in enumerate(meshes):
                    if not keep(m['mat'], None): continue
                    if tex_white and m['mat'].get('tex'): m['mat'] = dict(m['mat'], rgba=(1.0, 1.0, 1.0, m['mat']['rgba'][3]))
                    mi = w.mat(m['mat'], m['mat'].get('name', 'mat')); tn = tex_name(m['mat'], None)
                    pn = uniq('%s_%s' % (nm, m['mat'].get('name', k)) if len(meshes) > 1 else nm)
                    for new_, obj_, b_ in splits:                # --split-uv: a box of the texture as its own part
                        if obj_ != pn or m['uv'] is None: continue
                        inb = lambda q: q is not None and b_[0] <= q[0] <= b_[2] and b_[1] <= q[1] <= b_[3]
                        tin = [t for t in m['tris'] if all(inb(m['uv'][q]) for q in t)]
                        if tin and w.poly(renames.get(new_, new_), dict(m, tris=tin), mi, tn, crease):
                            kids += 1; nobj += 1; ntri += len(tin)
                            m = dict(m, tris=[t for t in m['tris'] if not all(inb(m['uv'][q]) for q in t)])
                    for new_, obj_, ax_, lt_, v_ in psplits:     # --split-pos: a side of the part as its own part
                        if obj_ != pn: continue
                        inp = lambda q: m['pos'][q][ax_] < v_ if lt_ else m['pos'][q][ax_] > v_
                        tin = [t for t in m['tris'] if all(inp(q) for q in t)]
                        if tin and w.poly(renames.get(new_, new_), dict(m, tris=tin), mi, tn, crease):
                            kids += 1; nobj += 1; ntri += len(tin)
                            m = dict(m, tris=[t for t in m['tris'] if not all(inp(q) for q in t)])
                    if w.poly(renames.get(pn, pn), m, mi, tn, crease): kids += 1; nobj += 1; ntri += len(m['tris'])
            for c in kids_: kids += emit_x(c, match if only_rx else True, Mw)
            if kids == 0:
                del w.lines[start:]; return 0
            w.lines[start:start + 5] = ['OBJECT group', 'name "%s"' % uniq(nm + '_grp'),
                                        'loc %s %s %s' % (fmt(M[0][3]), fmt(M[1][3]), fmt(M[2][3])),
                                        'rot ' + ' '.join(fmt(M[r][c]) for r in range(3) for c in range(3)),
                                        'kids %d' % kids]
            return 1
        top = sum(emit_x(r, False) for r in read_x(src))
        head = ['OBJECT world', 'name "%s"' % name, 'loc %s %s %s' % tuple(fmt(x) for x in bo),
                'rot ' + ' '.join(fmt(BA[i][j]) for i in range(3) for j in range(3)), 'kids %d' % top]
        text = w.text(head)
    elif is_obj:
        meshes = read_obj(src, tex_dirs)
        items = []
        for nm, m in meshes:
            if skip_rx and skip_rx.search(nm): continue
            if only_rx and not only_rx.search(nm): continue
            items.append((nm, m))
        crease = float(crease_o) if crease_o else (179.0 if all(m['nrm'] is not None for _, m in items) else 30.0)
        kids = 0
        used = {}

        def fg_centre(m, t):                               # (ck_look) a triangle's centre, FlightGear frame metres
            q = [apt(root, m['pos'][k]) for k in t]
            c = [(q[0][i] + q[1][i] + q[2][i]) / 3.0 for i in range(3)]
            return (c[0], -c[2], c[1])                     # AC3D -> FlightGear, as in_boxes

        def in_box(c, b): return b[0] <= c[0] <= b[1] and b[2] <= c[1] <= b[3] and b[4] <= c[2] <= b[5]

        def pieces_of(m):                                  # connected pieces (shared positions), first-triangle order
            par = {}

            def find(x):
                while par[x] != x: par[x] = par[par[x]]; x = par[x]
                return x
            keys = [tuple(round(c, 5) for c in p) for p in m['pos']]
            for t in m['tris']:
                ks = [keys[k] for k in t]
                for kk in ks: par.setdefault(kk, kk)
                for kk in ks[1:]:
                    a_, b_ = find(ks[0]), find(kk)
                    if a_ != b_: par[a_] = b_
            order, groups = [], {}
            for t in m['tris']:
                r_ = find(keys[t[0]])
                if r_ not in groups: groups[r_] = []; order.append(r_)
                groups[r_].append(t)
            return [groups[r_] for r_ in order]
        cut_surf = []                                      # --cut-behind: the surface's triangles (source frame)
        for nm, m in items:
            if cut_rx and cut_rx.search(nm): cut_surf += [[m['pos'][k] for k in t] for t in m['tris']]

        def behind_cut(m, t):                              # the triangle's centre lies beyond the surface, from 0
            c = [(m['pos'][t[0]][i] + m['pos'][t[1]][i] + m['pos'][t[2]][i]) / 3.0 for i in range(3)]
            r = math.sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2])
            if r < 1e-9: return False
            d = [x / r for x in c]
            for a_, b_, c_ in cut_surf:                    # Moeller-Trumbore from the origin along d
                e1 = [b_[i] - a_[i] for i in range(3)]; e2 = [c_[i] - a_[i] for i in range(3)]
                p = [d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]]
                det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2]
                if abs(det) < 1e-15: continue
                s = [-a_[0], -a_[1], -a_[2]]
                u_ = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det
                if u_ < 0.0 or u_ > 1.0: continue
                q = [s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]]
                v_ = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det
                if v_ < 0.0 or u_ + v_ > 1.0: continue
                if 0.0 < (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det < r: return True
            return False
        for nm, m in items:
            if cut_surf and not cut_rx.search(nm):
                kept_ = [t for t in m['tris'] if not behind_cut(m, t)]
                print('--cut-behind: %s: %d of %d triangles beyond the surface left out' % (nm, len(m['tris']) - len(kept_), len(m['tris'])))
                m['tris'] = kept_
            if not keep(m['mat'], None): continue
            if tex_white and m['mat'].get('tex'): m['mat'] = dict(m['mat'], rgba=(1.0, 1.0, 1.0, m['mat']['rgba'][3]))
            mi = w.mat(m['mat'], m['mat']['name']); tn = tex_name(m['mat'], None)
            k = used.get(nm, 0); used[nm] = k + 1
            pn = nm if not k else '%s_%d' % (nm, k)
            # ck_look 2026-10-30: --split-uv (OBJ a regular expression, an optional box), --split-box, --split-pieces
            for (new_, obj_, b_), pb_ in zip(splits, uvboxes):
                if not re.search(obj_, pn) or m['uv'] is None: continue
                inb = lambda q: q is not None and b_[0] <= q[0] <= b_[2] and b_[1] <= q[1] <= b_[3]
                sel = lambda t: all(inb(m['uv'][q]) for q in t) and (pb_ is None or in_box(fg_centre(m, t), pb_))
                tin = [t for t in m['tris'] if sel(t)]
                if tin and w.poly(renames.get(new_, new_), dict(m, tris=tin), mi, tn, crease):
                    kids += 1; nobj += 1; ntri += len(tin)
                    m = dict(m, tris=[t for t in m['tris'] if not sel(t)])
            for new_, orx_, b_ in bsplits:
                if not orx_.search(pn): continue
                tin = [t for t in m['tris'] if in_box(fg_centre(m, t), b_)]
                if tin and w.poly(renames.get(new_, new_), dict(m, tris=tin), mi, tn, crease):
                    kids += 1; nobj += 1; ntri += len(tin)
                    m = dict(m, tris=[t for t in m['tris'] if not in_box(fg_centre(m, t), b_)])
            if any(r_.search(pn) for r_ in pieces_rx):
                for j_, tp in enumerate(pieces_of(m)):
                    pc = '%s_pc%d' % (pn, j_)
                    if w.poly(renames.get(pc, pc), dict(m, tris=tp), mi, tn, crease): kids += 1; nobj += 1; ntri += len(tp)
                continue
            if w.poly(renames.get(pn, pn), m, mi, tn, crease):
                kids += 1; nobj += 1; ntri += len(m['tris'])
        head = ['OBJECT world', 'name "%s"' % name, 'loc %s %s %s' % tuple(fmt(x) for x in bo),
                'rot ' + ' '.join(fmt(BA[i][j]) for i in range(3) for j in range(3)), 'kids %d' % kids]
        text = w.text(head)
    else:
        G = Gltf(src, tex_dirs); g = G.g
        crease = float(crease_o) if crease_o else 179.0
        nodes = g.get('nodes', [])
        scene = g['scenes'][g.get('scene', 0)]['nodes'] if g.get('scenes') else [i for i in range(len(nodes))]
        parent = {}
        for i, n in enumerate(nodes):
            for c in n.get('children', []): parent[c] = i
        world = {}

        def wm(i):
            if i not in world:
                world[i] = amul(wm(parent[i]), node_local(nodes[i])) if i in parent else node_local(nodes[i])
            return world[i]
        names_used = {}

        def uniq(nm):
            nm = re.sub(r'["\r\n]+', '_', nm or 'node')
            k = names_used.get(nm, 0); names_used[nm] = k + 1
            return nm if not k else '%s_%d' % (nm, k)
        joint_nodes = {j for s in g.get('skins', []) for j in s.get('joints', [])}

        def emit_node(i, anc_match):
            """returns the number of AC objects written for node i (its own group counts as one)"""
            nonlocal nobj, ntri
            n = nodes[i]; nm = n.get('name') or ('node%d' % i)
            cur_world[0] = amul(root, wm(i))
            if skip_rx and skip_rx.search(nm): return 0
            match = anc_match or (only_rx.search(nm) if only_rx else True)
            start = len(w.lines)
            w.lines.append(None)                           # placeholder for the group header (5 lines)
            w.lines.extend([None] * 4)
            kids = 0
            if 'mesh' in n and match and 'skin' not in n:
                me = g['meshes'][n['mesh']]
                base_nm = nm if (node_names or not me.get('name')) else me.get('name')
                prims = me.get('primitives', [])
                for pi, pr in enumerate(prims):
                    m = G.prim_mesh(pr)
                    if not keep(m['mat'], G): continue
                    if tex_white and m['mat'].get('tex'): m['mat'] = dict(m['mat'], rgba=(1.0, 1.0, 1.0, m['mat']['rgba'][3]))
                    mi = w.mat(m['mat'], m['mat'].get('name', 'mat')); tn = tex_name(m['mat'], G)
                    pn = uniq(base_nm if len(prims) == 1 else '%s_p%d' % (base_nm, pi))
                    if w.poly(renames.get(pn, pn), m, mi, tn, crease):        # --name (glTF too since ck_rafale)
                        kids += 1; nobj += 1; ntri += len(m['tris'])
            for c in n.get('children', []):
                kids += emit_node(c, match if only_rx else True)
                cur_world[0] = amul(root, wm(i))
            if kids == 0:
                del w.lines[start:]; return 0
            M = node_local(n)
            w.lines[start:start + 5] = ['OBJECT group', 'name "%s"' % uniq(nm + '_grp' if 'mesh' in n else nm),
                                        'loc %s %s %s' % (fmt(M[0][3]), fmt(M[1][3]), fmt(M[2][3])),
                                        'rot ' + ' '.join(fmt(M[r][c]) for r in range(3) for c in range(3)),
                                        'kids %d' % kids]
            return 1
        top = 0
        for r in scene: top += emit_node(r, False)
        # skinned meshes: in the scene root frame (glTF ignores the mesh node's transform), posed by the joints'
        # rest transforms; 'rigid' splits them into one object per joint
        skinned = [i for i, n in enumerate(nodes) if 'mesh' in n and 'skin' in n]
        cur_world[0] = root
        for i in skinned:
            n = nodes[i]; nm = n.get('name') or 'skinned%d' % i
            if skip_rx and skip_rx.search(nm): continue
            sk = g['skins'][n['skin']]
            ibm = G.acc(sk['inverseBindMatrices']) if 'inverseBindMatrices' in sk else None
            JM = []
            for k, j in enumerate(sk['joints']):
                Mi = I34 if ibm is None else [[ibm[k][0], ibm[k][4], ibm[k][8], ibm[k][12]],
                                              [ibm[k][1], ibm[k][5], ibm[k][9], ibm[k][13]],
                                              [ibm[k][2], ibm[k][6], ibm[k][10], ibm[k][14]]]
                JM.append(amul(wm(j), Mi))
            me = g['meshes'][n['mesh']]
            for pi, pr in enumerate(me.get('primitives', [])):
                m = G.prim_mesh(pr)
                if m['joints'] is None or not keep(m['mat'], G): continue
                P, N = [], []
                for vi, p in enumerate(m['pos']):
                    acc_p = [0.0, 0.0, 0.0]; acc_n = [0.0, 0.0, 0.0]; ws = 0.0
                    for jk, wt in zip(m['joints'][vi], m['weights'][vi]):
                        if wt <= 0: continue
                        q = apt(JM[jk], p); acc_p = [x + wt * y for x, y in zip(acc_p, q)]; ws += wt
                        if m['nrm']: acc_n = [x + wt * y for x, y in zip(acc_n, adir(JM[jk], m['nrm'][vi]))]
                    P.append([x / ws for x in acc_p] if ws else p)
                    if m['nrm']:
                        ln = math.sqrt(sum(x * x for x in acc_n)) or 1.0; N.append([x / ln for x in acc_n])
                m['pos'] = P; m['nrm'] = N if m['nrm'] else None
                if tex_white and m['mat'].get('tex'): m['mat'] = dict(m['mat'], rgba=(1.0, 1.0, 1.0, m['mat']['rgba'][3]))
                mi = w.mat(m['mat'], m['mat'].get('name', 'mat')); tn = tex_name(m['mat'], G)
                if skin_mode == 'rigid':
                    by_joint = {}
                    for t in m['tris']:
                        sc = {}
                        for vi in t:
                            for jk, wt in zip(m['joints'][vi], m['weights'][vi]): sc[jk] = sc.get(jk, 0.0) + wt
                        jbest = max(sc, key=sc.get)
                        by_joint.setdefault(jbest, []).append(t)
                    for jk, ts in sorted(by_joint.items()):
                        jn = nodes[sk['joints'][jk]].get('name') or 'joint%d' % sk['joints'][jk]
                        sub = dict(m, tris=ts)
                        if w.poly(uniq(jn), sub, mi, tn, crease): top += 1; nobj += 1; ntri += len(ts)
                else:
                    if w.poly(uniq(nm if len(me['primitives']) == 1 else '%s_p%d' % (nm, pi)), m, mi, tn, crease):
                        top += 1; nobj += 1; ntri += len(m['tris'])
            for k, j in enumerate(sk['joints']):
                Wj = wm(j)
                o = apt(root, [Wj[0][3], Wj[1][3], Wj[2][3]])                 # AC3D frame
                fgp = [o[0], -o[2], o[1]]                                     # FlightGear frame
                ax = [adir(root, [Wj[r][c] for r in range(3)]) for c in range(3)]   # the joint's axes, AC3D frame
                fga = [[v[0], -v[2], v[1]] for v in ax]
                joints_txt.append('%-40s pos(FG x aft, y right, z up) %8.4f %8.4f %8.4f  axes x %s y %s z %s' % (
                    nodes[j].get('name') or 'joint%d' % j, fgp[0], fgp[1], fgp[2],
                    ' '.join('%.3f' % v for v in fga[0]), ' '.join('%.3f' % v for v in fga[1]),
                    ' '.join('%.3f' % v for v in fga[2])))
        head = ['OBJECT world', 'name "%s"' % name, 'loc %s %s %s' % tuple(fmt(x) for x in bo),
                'rot ' + ' '.join(fmt(BA[i][j]) for i in range(3) for j in range(3)), 'kids %d' % top]
        text = w.text(head)
    acp = os.path.join(out_dir, name + '.ac')
    open(acp, 'w', encoding='latin-1', errors='replace', newline='\n').write(text)
    extra = ''
    if xml_extra:
        import xml.etree.ElementTree as ET
        ex = ET.parse(xml_extra).getroot()
        extra = ''.join(ET.tostring(e, encoding='unicode') for e in ex)
    xml = ('<?xml version="1.0"?>\n<!-- written by tools/cockpit/gltf2ac.py from %s -->\n<PropertyList>\n <path>%s.ac</path>\n%s'
           '</PropertyList>\n' % (os.path.basename(src).replace('--', '-'), name, extra))
    open(os.path.join(out_dir, name + '.xml'), 'w', encoding='utf-8', newline='\n').write(xml)
    if joints_txt:
        open(os.path.join(out_dir, name + '.joints.txt'), 'w', newline='\n').write('\n'.join(joints_txt) + '\n')
    print('%s: %d objects, %d triangles, %d materials, %d textures' % (acp, nobj, ntri, len(w.mats),
                                                                        len([v for v in tex_done.values() if v])))


if __name__ == '__main__':                 # (prep_gripen.sh imports its image codecs)
    main()
