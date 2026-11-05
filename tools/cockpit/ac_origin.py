"""ac_origin.py FILE.ac REGEX [EYE_X,EYE_Y,EYE_Z] -- world origin (FlightGear frame: x aft, y right, z up) of every
AC3D object whose name matches REGEX (loc / rot of the parents applied as ac2cpit.py does), and with an eye the
offset from it (eye frame: X right, Y up, Z back). For a Flight Simulator model converted by gltf2ac.py the origin of
an animated part is its animation frame's: the pivot of a stick or throttle for the --controls animations written by
a build script. Read-only."""
import re, sys

L = open(sys.argv[1], encoding='latin-1').read().split('\n')
rx = re.compile(sys.argv[2])
eye = [float(v) for v in sys.argv[3].split(',')] if len(sys.argv) > 3 else None


def mul(a, b): return [sum(a[i * 3 + k] * b[k * 3 + j] for k in range(3)) for i in range(3) for j in range(3)]
def app(m, v): return [sum(m[i * 3 + k] * v[k] for k in range(3)) for i in range(3)]


def obj(i, M, T):
    name, loc, rot, kids = '', [0, 0, 0], [1, 0, 0, 0, 1, 0, 0, 0, 1], 0
    i += 1
    while i < len(L):
        l = L[i]; t = l.split(' ', 1)[0]
        if t == 'name': name = l[5:].strip().strip('"')
        elif t == 'loc': loc = [float(x) for x in l.split()[1:4]]
        elif t == 'rot': rot = [float(x) for x in l.split()[1:10]]
        elif t == 'numvert': i += int(l.split()[1])
        elif t == 'numsurf':
            n = int(l.split()[1]); i += 1
            for _ in range(n):
                while not L[i].startswith('refs'): i += 1
                i += 1 + int(L[i].split()[1])
            continue
        elif t == 'kids':
            kids = int(l.split()[1]); i += 1; break
        i += 1
    Mw = mul(M, rot); Tw = [a + b for a, b in zip(app(M, loc), T)]
    if rx.search(name):
        fg = [Tw[0], -Tw[2], Tw[1]]
        s = '%-30s FG %.4f %.4f %.4f' % (name, fg[0], fg[1], fg[2])
        if eye: s += '   eye frame X %+.3f Y %+.3f Z %+.3f' % (fg[1] - eye[1], fg[2] - eye[2], fg[0] - eye[0])
        print(s)
    for _ in range(kids):
        while i < len(L) and not L[i].startswith('OBJECT'): i += 1
        if i >= len(L): break
        i = obj(i, Mw, Tw)
    return i


i = 0
while not L[i].startswith('OBJECT'): i += 1
obj(i, [1, 0, 0, 0, 1, 0, 0, 0, 1], [0.0, 0.0, 0.0])
