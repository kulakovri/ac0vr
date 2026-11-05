"""tornado_gpl_parts.py OUT.ac [--mirrors] [--lever] -- the parts of build_tornado_gpl.sh's Tornado cockpit that no free source has, made here
(ours, no input files): the three rear-view mirrors on the canopy's front bow (flat glass in a housing; the runtime takes
meshes named mirror_* by its name rule, ps2_video.c mir_check) and the wing sweep lever on the left console.

Coordinates below are in the eye frame (metres, X right, Y up, Z back, origin at the design eye), written into the AC
file as FlightGear model coordinates: AC x = Z, AC y = Y, AC z = -X (build_tornado_gpl.sh adds the design eye with
--add-model). The bow's numbers come from the GPL exterior (TornadoGR4.ac: its rim at the centre line is at Y 0.10..0.16,
Z -0.44..-0.33; at X 0.27 at Y 0.02..0.06, Z -0.39..-0.30).
"""
import math, sys


def norm(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / n for c in v]


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def add(a, b, s=1.0):
    return [a[i] + b[i] * s for i in range(3)]


class Model:
    def __init__(self):
        self.objs = []

    def obj(self, name, mat, verts, faces, crease=30.0):
        self.objs.append((name, mat, verts, faces, crease))

    def box(self, name, mat, c, ux, uy, uz, sx, sy, sz, crease=30.0):
        """a box: centre c, half sizes along the unit vectors ux, uy, uz"""
        v = []
        for k in (-1, 1):
            for j in (-1, 1):
                for i in (-1, 1):
                    v.append(add(add(add(c, ux, i * sx), uy, j * sy), uz, k * sz))
        f = [(0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)]
        self.obj(name, mat, v, f, crease)

    def tube(self, name, mat, a, b, r, n=8, r2=None):
        """a capped cylinder from a to b"""
        d = norm([b[i] - a[i] for i in range(3)])
        u = norm(cross(d, [0.0, 0.0, 1.0] if abs(d[2]) < 0.9 else [1.0, 0.0, 0.0]))
        w = cross(d, u)
        v, f = [], []
        for i in range(n):
            t = 2 * math.pi * i / n; c, s = math.cos(t), math.sin(t)
            v.append([a[k] + (u[k] * c + w[k] * s) * r for k in range(3)])
            v.append([b[k] + (u[k] * c + w[k] * s) * (r2 or r) for k in range(3)])
        for i in range(n):
            j = (i + 1) % n
            f.append((2 * i, 2 * j, 2 * j + 1, 2 * i + 1))
        f.append(tuple(2 * i for i in range(n - 1, -1, -1)))
        f.append(tuple(2 * i + 1 for i in range(n)))
        self.obj(name, mat, v, f, 50.0)

    def write(self, path, mats):
        with open(path, 'w', newline='\n') as o:
            o.write('AC3Db\n')
            for name, rgb, spec, shi in mats:
                o.write('MATERIAL "%s" rgb %.3f %.3f %.3f amb %.3f %.3f %.3f emis 0 0 0 spec %.3f %.3f %.3f shi %d trans 0\n'
                        % (name, *rgb, *rgb, spec, spec, spec, shi))
            o.write('OBJECT world\nkids %d\n' % len(self.objs))
            for name, mat, v, f, crease in self.objs:
                o.write('OBJECT poly\nname "%s"\ncrease %.1f\nnumvert %d\n' % (name, crease, len(v)))
                for p in v:
                    o.write('%.5f %.5f %.5f\n' % (p[2], p[1], -p[0]))     # eye frame -> AC
                o.write('numsurf %d\n' % len(f))
                for q in f:
                    o.write('SURF 0x20\nmat %d\nrefs %d\n' % (mat, len(q)))
                    for i in q: o.write('%d 0 0\n' % i)
                o.write('kids 0\n')


def mirror(m, tag, c, along, w, h, aim, housing=0.022, lip=0.006):
    """flat glass (name mirror_<tag>) facing the eye so that it shows the direction aim; a dark housing behind it"""
    d = norm(c)
    nrm = norm([aim[i] - d[i] for i in range(3)])            # the reflection of the eye's ray through c gives aim
    up = norm(cross(nrm, along)); ax = norm(cross(up, nrm))
    if ax[0] < 0: ax = [-q for q in ax]; up = [-q for q in up]
    g = add(c, nrm, 0.0012)                                  # the glass stands just out of the housing's face

    def outline(o, ww, hh, r):
        """a rounded rectangle about o: 4 corners of 5 points"""
        pts = []
        for sx, sy, a0 in ((1, 1, 0), (-1, 1, 90), (-1, -1, 180), (1, -1, 270)):
            for k in range(5):
                t = math.radians(a0 + k * 22.5)
                pts.append(add(add(o, ax, sx * (ww - r) + r * math.cos(t)), up, sy * (hh - r) + r * math.sin(t)))
        return pts
    r = min(w, h) * 0.45
    m.obj('mirror_' + tag, 1, outline(g, w, h, r), [tuple(range(20))], 10.0)
    # the housing: a rounded plate behind the glass, tapering to the bow
    a = outline(c, w + lip, h + lip, r + lip)
    b = outline(add(c, nrm, -2 * housing), w * 0.85, h * 0.8, r)
    faces = [tuple(range(20))] + [(i, (i + 1) % 20, 20 + (i + 1) % 20, 20 + i) for i in range(20)]
    m.obj('arch_pad_' + tag, 0, a + b, faces, 40.0)


def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'):
        sys.exit(__doc__)
    m = Model()
    if '--mirrors' in a:
        back = norm([0.0, 0.05, 1.0])
        # the centre mirror under the bow's middle; the side ones on its shoulders, looking 22 degrees outboard of the tail
        mirror(m, 'C', [0.0, 0.112, -0.335], [1.0, 0.0, 0.0], 0.105, 0.028, back)
        for tag, s in (('L', -1.0), ('R', 1.0)):
            aim = norm([s * math.sin(math.radians(22)), 0.05, math.cos(math.radians(22))])
            mirror(m, tag, [s * 0.262, 0.030, -0.312], [s * 0.73, -0.60, 0.33], 0.075, 0.026, aim)
    if '--lever' in a:
        # the wing sweep lever: a grey gated plate on the left console, outboard of the throttles, with a black lever
        # at the gate's forward end (25 deg: the wings as the exterior shows them)
        px, py, pz = -0.515, -0.525, -0.165
        x, y, z = [1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]
        m.box('sweep_plate', 3, [px, py, pz], x, y, z, 0.030, 0.006, 0.105)
        m.box('sweep_slot', 2, [px, py + 0.0065, pz], x, y, z, 0.006, 0.0008, 0.090)
        base = [px, py + 0.006, pz - 0.075]; top = [px, py + 0.085, pz - 0.095]
        m.tube('sweep_lever', 2, base, top, 0.0055)
        m.box('sweep_grip', 4, add(top, y, 0.016), x, y, z, 0.013, 0.021, 0.016)
    m.write(a[0], [('housing', (0.05, 0.05, 0.055), 0.2, 20), ('mirror', (0.70, 0.72, 0.75), 0.6, 96),
                   ('lever', (0.05, 0.05, 0.05), 0.5, 60), ('plate', (0.20, 0.21, 0.22), 0.2, 20),
                   ('grip', (0.62, 0.50, 0.08), 0.3, 30)])
    print('tornado_gpl_parts: %d objects -> %s' % (len(m.objs), a[0]))


if __name__ == '__main__':
    main()
