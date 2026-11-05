"""ac_pick.py IN.ac OUT.ac [options] -- copy chosen objects of an AC3D model into a new flat one, for grafting single
parts of one FlightGear cockpit into another (build_tornado_gpl.sh: a seat out of a two-seat interior, the side consoles
out of a whole cockpit.ac). Only reads IN.ac and writes OUT.ac; no code from the model is run.

  --name R        keep poly objects whose name, or a group above them, matches regex R (repeatable: any of them)
  --drop R        ... and leave out those whose name matches R (repeatable), after --name / --box
  --box X0,X1,Y0,Y1,Z0,Z1  keep only objects whose bounding box centre lies inside (the file's own axes: FlightGear's x
                  aft, AC y = FlightGear z up, AC z = minus FlightGear y); repeatable: inside any of them
  --prefix P      prepend P to every kept object's name (a name another part of the cockpit hides or animates)
  --tex-from DIR  texture paths written relative to OUT.ac's folder, pointing at DIR/<the file's texture path>
                  (default: IN.ac's own folder, so the graft keeps finding its textures wherever OUT.ac lives)
  --no-tex R      objects whose texture file name matches R lose it (keep the material colour)
  --drop-tex R    objects whose texture file name matches R are left out (a lamp's lit-up copy: FlightGear selects it)
  --list          print the kept objects (name, texture, vertices, surfaces, box) on stdout

The output holds the file's materials, one world object and the kept poly objects with their accumulated translation
and rotation folded into the vertices (groups are flattened); surfaces, normals' crease angles and texture repeats stay
as they were. Faces keep their indices. Exit 1 when nothing was kept.
"""
import os, re, sys


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def app(m, p):
    return [sum(m[i][k] * p[k] for k in range(3)) for i in range(3)]


def parse(lines):
    """the object tree: {name, type, rot, loc, head (lines to keep), verts, tail (numsurf... lines), kids}"""
    i = next(j for j, l in enumerate(lines) if l.startswith('OBJECT'))

    def obj(i):
        o = dict(type=lines[i].split()[1], name='', rot=None, loc=[0.0, 0.0, 0.0], head=[], verts=[], tail=[], kids=[], tex='')
        i += 1
        while i < len(lines):
            s = lines[i]; w = s.split()
            if not w:
                i += 1; continue
            k = w[0]
            if k == 'name':
                o['name'] = s[5:].strip().strip('"')
            elif k == 'rot':
                v = [float(x) for x in w[1:10]]; o['rot'] = [v[0:3], v[3:6], v[6:9]]
            elif k == 'loc':
                o['loc'] = [float(x) for x in w[1:4]]
            elif k == 'data':
                n = int(w[1]); o['head'].append(s); i += 1; got = 0
                while got < n and i < len(lines):
                    o['head'].append(lines[i]); got += len(lines[i]) + 1; i += 1
                continue
            elif k == 'texture':
                o['tex'] = s.split(None, 1)[1].strip().strip('"')
            elif k == 'numvert':
                n = int(w[1])
                o['verts'] = [[float(x) for x in lines[i + 1 + j].split()[:3]] for j in range(n)]
                i += n
            elif k == 'numsurf':
                n = int(w[1]); o['tail'].append(s); i += 1; got = 0
                while got < n and i < len(lines):
                    o['tail'].append(lines[i])
                    if lines[i].startswith('refs'):
                        r = int(lines[i].split()[1])
                        o['tail'].extend(lines[i + 1:i + 1 + r]); i += r; got += 1
                    i += 1
                continue
            elif k == 'kids':
                n = int(w[1]); i += 1
                for _ in range(n):
                    c, i = obj(i); o['kids'].append(c)
                return o, i
            else:
                o['head'].append(s)
            i += 1
        return o, i
    return obj(i)[0]


def main():
    a = sys.argv[1:]
    if len(a) < 2:
        sys.exit(__doc__)
    src, dst = a[0], a[1]; a = a[2:]
    names, drops, boxes, prefix, tex_from, no_tex, drop_tex, show = [], [], [], '', None, None, None, False
    while a:
        k = a.pop(0)
        if k == '--name': names.append(re.compile(a.pop(0)))
        elif k == '--drop': drops.append(re.compile(a.pop(0)))
        elif k == '--box': boxes.append([float(x) for x in a.pop(0).split(',')])
        elif k == '--prefix': prefix = a.pop(0)
        elif k == '--tex-from': tex_from = a.pop(0)
        elif k == '--no-tex': no_tex = re.compile(a.pop(0))
        elif k == '--drop-tex': drop_tex = re.compile(a.pop(0))
        elif k == '--list': show = True
        else: sys.exit('ac_pick: unknown option ' + k)
    lines = open(src, 'r', encoding='latin-1', newline='').read().replace('\r\n', '\n').split('\n')
    mats = [l for l in lines if l.startswith('MATERIAL')]
    root = parse(lines)
    tdir = os.path.relpath(tex_from or os.path.dirname(os.path.abspath(src)), os.path.dirname(os.path.abspath(dst)))
    kept = []

    def walk(o, m, t, up):
        m2 = mul(m, o['rot']) if o['rot'] else m
        t2 = [t[i] + app(m, o['loc'])[i] for i in range(3)]
        path = up + [o['name']]
        if o['type'] == 'poly' and o['verts'] and o['tail']:
            v = [[app(m2, p)[i] + t2[i] for i in range(3)] for p in o['verts']]
            lo = [min(p[i] for p in v) for i in range(3)]; hi = [max(p[i] for p in v) for i in range(3)]
            c = [(lo[i] + hi[i]) / 2 for i in range(3)]
            ok = (not names) or any(r.search(n) for r in names for n in path)
            ok = ok and ((not boxes) or any(all(b[2 * i] <= c[i] <= b[2 * i + 1] for i in range(3)) for b in boxes))
            ok = ok and not any(r.search(o['name']) for r in drops)
            ok = ok and not (drop_tex and o['tex'] and drop_tex.search(os.path.basename(o['tex'])))
            if ok: kept.append((o, v, lo, hi))
        for c in o['kids']:
            walk(c, m2, t2, path)

    identity = [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]
    walk(root, identity, [0.0, 0.0, 0.0], [])
    if not kept:
        sys.exit('ac_pick: nothing kept from ' + src)
    with open(dst, 'w', encoding='latin-1', newline='\n') as f:
        f.write('AC3Db\n')
        for m in mats: f.write(m + '\n')
        f.write('OBJECT world\nkids %d\n' % len(kept))
        for o, v, lo, hi in kept:
            f.write('OBJECT poly\nname "%s%s"\n' % (prefix, o['name']))
            for s in o['head']: f.write(s + '\n')
            tex = o['tex']
            if tex and not (no_tex and no_tex.search(os.path.basename(tex))):
                f.write('texture "%s"\n' % os.path.join(tdir, tex).replace(os.sep, '/'))
            f.write('numvert %d\n' % len(v))
            for p in v: f.write('%.6f %.6f %.6f\n' % tuple(p))
            for s in o['tail']: f.write(s + '\n')
            f.write('kids 0\n')
            if show:
                print('%-34s %-28s v%5d %s' % (o['name'], tex[:28], len(v), ' '.join('%.3f..%.3f' % (lo[i], hi[i]) for i in range(3))))
    print('ac_pick: %s -> %s: %d objects, %d vertices' % (os.path.basename(src), dst, len(kept), sum(len(k[1]) for k in kept)))


if __name__ == '__main__':
    main()
