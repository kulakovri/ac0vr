"""eject_handle.py OUT_DIR NAME LOOP [LOOP ...] -- ejection-seat handles as yellow-and-black striped loops (a round tube
bent into an inverted U: two legs and a rounded grip bar) for a seat model that lacks them, as a FlightGear model:
OUT_DIR/NAME.ac + NAME.xml (ac2cpit.py --add-model NAME.xml@<the seat's own offsets>) and OUT_DIR/eject_stripes.png.

  LOOP  X,Y,Z@AX,AY,AZ@UX,UY,UZ@W,H,R
        in the seat model's FlightGear frame (metres; x aft, y right, z up): the base midpoint (X,Y,Z) between the two
        legs, the direction from leg to leg (AX,AY,AZ), the direction the loop rises (UX,UY,UZ: made perpendicular to
        the first), the width W between the legs' centres, the height H of the grip bar's centre above the base and the
        tube radius R. The legs start 2 cm below the base (inside the seat), the bar's corners are rounded (radius
        0.45 min(W/2, H))
The tube is a 12-sided ring every <= 6 mm along its centre line; texture u runs along it (one yellow and one black band
per 36 mm) and v around it, on a stripe texture at 45 degrees: the bands wind round the tube like the paint of real
handles. The texture is generated here (deterministic: the ACES II / K-36 yellow, a near-black, a little wear along the
band edges), 64 x 64. ck_polish 2026-10-28 (bugs #175 / #176): the K-36DM's pull loop between the knees (build_su27.sh,
build_mig29a.sh), the ACES II side handles of the F-15 (build_f15c.sh). Pure Python, writes only OUT_DIR.
"""
import math, os, struct, sys, zlib

PERIOD = 0.036       # metres of tube per yellow + black band pair
SIDES = 12
YELLOW = (226, 178, 24)
BLACK = (24, 24, 22)


def png_write(path, w, h, rgb):
    raw = b''.join(b'\0' + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def stripes(path):
    """64 x 64 RGB: bands at 45 degrees (u + v), half yellow, half black, softened over a texel, a faint wear grain"""
    n = 64
    px = bytearray(n * n * 3)
    for y in range(n):
        for x in range(n):
            t = ((x + y) % n) / n                        # position across one band pair, 0..1
            d = min(abs(t - 0.0), abs(t - 0.5), abs(t - 1.0)) * n     # texels from a band edge
            k = 1.0 if t < 0.5 else 0.0                  # yellow first
            if d < 1.0: k = 0.5 + (k - 0.5) * d           # a soft edge
            h = ((x * 73856093) ^ (y * 19349663)) & 1023
            g = 1.0 - 0.06 * (h / 1023.0)                # grain
            for c in range(3):
                px[(y * n + x) * 3 + c] = int(max(0, min(255, (YELLOW[c] * k + BLACK[c] * (1 - k)) * g)) + 0.5)
    png_write(path, n, n, px)


def norm(v):
    l = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / l for c in v]


def cross(a, b): return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def loop_path(P, A, U, W, H):
    """centre line points (FlightGear frame) of the inverted U"""
    rc = 0.45 * min(W / 2, H)
    pts = []

    def at(a, u): return [P[j] + A[j] * a + U[j] * u for j in range(3)]

    def seg(a0, u0, a1, u1):
        n = max(1, int(math.ceil(math.hypot(a1 - a0, u1 - u0) / 0.006)))
        for k in range(n): pts.append(at(a0 + (a1 - a0) * k / n, u0 + (u1 - u0) * k / n))

    def arc(ca, cu, t0, t1):
        n = max(4, int(math.ceil(abs(t1 - t0) * rc / 0.006)))
        for k in range(n):
            t = t0 + (t1 - t0) * k / n
            pts.append(at(ca + rc * math.cos(t), cu + rc * math.sin(t)))
    seg(-W / 2, -0.02, -W / 2, H - rc)
    arc(-W / 2 + rc, H - rc, math.pi, math.pi / 2)
    seg(-W / 2 + rc, H, W / 2 - rc, H)
    arc(W / 2 - rc, H - rc, math.pi / 2, 0.0)
    seg(W / 2, H - rc, W / 2, -0.02)
    pts.append(at(W / 2, -0.02))
    return pts


def tube(pts, R, B):
    """rings round the centre line (B: the loop plane's normal); -> vertices, uvs per ring, faces"""
    verts, uvs, s = [], [], 0.0
    vt = max(1, round(2 * math.pi * R / PERIOD))       # whole band pairs round the tube: no seam
    for i, p in enumerate(pts):
        if i: s += math.sqrt(sum((p[j] - pts[i - 1][j]) ** 2 for j in range(3)))
        T = norm([pts[min(i + 1, len(pts) - 1)][j] - pts[max(i - 1, 0)][j] for j in range(3)])
        N = norm(cross(T, B))
        for k in range(SIDES + 1):                       # the seam repeated for the texture
            a = 2 * math.pi * k / SIDES
            verts.append([p[j] + R * (math.cos(a) * N[j] + math.sin(a) * B[j]) for j in range(3)])
            uvs.append((s / PERIOD, k / SIDES * vt))
    faces = []
    for i in range(len(pts) - 1):
        for k in range(SIDES):
            a, b = i * (SIDES + 1) + k, i * (SIDES + 1) + k + 1
            c, d = a + SIDES + 1, b + SIDES + 1
            faces.append((a, c, d, b))
    return verts, uvs, faces


def main():
    out, name = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    stripes(os.path.join(out, 'eject_stripes.png'))
    objs = []
    for li, spec in enumerate(sys.argv[3:]):
        p_, a_, u_, d_ = spec.split('@')
        P = [float(v) for v in p_.split(',')]; A = norm([float(v) for v in a_.split(',')])
        U0 = [float(v) for v in u_.split(',')]
        U = norm([U0[j] - sum(U0[k] * A[k] for k in range(3)) * A[j] for j in range(3)])
        W, H, R = [float(v) for v in d_.split(',')]
        B = norm(cross(A, U))
        verts, uvs, faces = tube(loop_path(P, A, U, W, H), R, B)
        L = ['OBJECT poly', 'name "%s_%d"' % (name, li + 1), 'texture "eject_stripes.png"', 'crease 75',
             'numvert %d' % len(verts)]
        L += ['%.6f %.6f %.6f' % (v[0], v[2], -v[1]) for v in verts]       # FlightGear -> AC3D (x, z up, -y)
        L.append('numsurf %d' % len(faces))
        for f in faces:
            L += ['SURF 0x10', 'mat 0', 'refs 4'] + ['%d %.6f %.6f' % (q, uvs[q][0], uvs[q][1]) for q in f]
        L.append('kids 0')
        objs.append(L)
        print('%s_%d: %d rings, %d faces, base %.3f %.3f %.3f, W %.3f H %.3f R %.4f' % (
            name, li + 1, len(verts) // (SIDES + 1), len(faces), P[0], P[1], P[2], W, H, R))
    txt = ['AC3Db', 'MATERIAL "eject_handle" rgb 1 1 1 amb 1 1 1 emis 0 0 0 spec 0.25 0.25 0.25 shi 24 trans 0',
           'OBJECT world', 'kids %d' % len(objs)]
    for L in objs: txt += L
    open(os.path.join(out, name + '.ac'), 'w', newline='\n').write('\n'.join(txt) + '\n')
    open(os.path.join(out, name + '.xml'), 'w', newline='\n').write(
        '<?xml version="1.0"?>\n<!-- written by tools/cockpit/eject_handle.py -->\n<PropertyList>\n <path>%s.ac</path>\n'
        '</PropertyList>\n' % name)


main()
