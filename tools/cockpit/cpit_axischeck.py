"""cpit_axischeck.py A.cpit [B.cpit] [--state K=V,...] [--eps M] [--list N] [--senses] [--bake OUT.cpit] -- offline
check of a cockpit file's rotate
animations as the runtime poses them (ck_axes 2026-10-31, bug #262; ps2_video.c cpit_anim_update / cpit_rot):

  * zero axes: a rotation about a zero axis scales the part by cos(angle) about its centre (cpit_rot with a = 0, as
    SimGear's set_rotation does) -- an axis ac2cpit did not read (a line object without --axis-objects); counted
    constant / live, and the parts posed non-rigidly at the test state (their 3x3 not a rotation);
  * the parts that stand more than EPS metres (0.10) from their rest place at the test state ("flying");
  * with B: the parts whose posed vertices differ by more than EPS between A and B at the test state and at the rest
    state (meshes matched by order when the two files list the same names and triangle counts, else by name);
  * --senses: per live animation the sense: a dial's turn with a rising value as seen from the eye (CW / CCW), a
    control part's travel from value 0 to 1 (thr 0 -> 1) in the eye frame (push: forward = -Z, right roll: +X, power:
    -Z);
  * --bake OUT.cpit: a copy of the last file with every part posed at the test state and no animations, for
    cpit_view.c (which draws the rest pose): the dials and controls as the runtime shows them in that state.

Test state (FlightGear conventions, the runtime's CPIT_P_* values): 350 kt, Mach 0.53, 2650 ft, +1500 fpm, 3 G (min 1,
max 3.5), AoA 8, pitch 10, roll 35 (right bank), heading 70, RPM 90 %, EGT 1100 F, fuel flow 300 gph, oil 40 psi, clock
12:00:00, EGT 593 C, 1950 pph; controls elevator 0.6 (push), aileron 0.6 (right), rudder 0.5, throttle 0.8. --state
overrides by name (speed mach alt vsi g gmin gmax aoa pitch roll hdg rpm egt ff oilp clock egtc ffpph elev ail rud thr).
The rest state: every value 0 (G 1), as the runtime before the game's values arrive.
Eye frame metres (X right, Y up, Z back), the eye at the origin. Needs numpy (PYV: any Python with it).
Only reads the given files."""
import math, re, struct, sys
import numpy as np

NAMES = ['speed', 'mach', 'alt', 'vsi', 'g', 'gmin', 'gmax', 'aoa', 'pitch', 'roll', 'hdg', 'rpm', 'egt', 'ff', 'oilp',
         'clock', 'egtc', 'ffpph', 'elev', 'ail', 'rud', 'thr']
TEST = [350, 0.53, 2650, 1500, 3.0, 1.0, 3.5, 8, 10, 35, 70, 90, 1100, 300, 40, 43200, 593, 1950, 0.6, 0.6, 0.5, 0.8]
REST = [0] * 22; REST[4] = REST[5] = REST[6] = 1.0
P_ELEV, P_AIL, P_RUD, P_THR = 18, 19, 20, 21


def read(path):
    b = open(path, 'rb').read()
    if b[:8] != b'ACZCPIT3': raise ValueError('not a cockpit file: ' + path)
    ntex, nmesh, nanim = struct.unpack_from('<III', b, 8)
    p = 20
    for _ in range(ntex):
        w, h = struct.unpack_from('<II', b, p); p += 8 + w * h * 4
    anims = []
    for _ in range(nanim):
        prop, cx, cy, cz, ax, ay, az, fac, off, nt = struct.unpack_from('<I3f3fffI', b, p); p += 40
        tab = [struct.unpack_from('<ff', b, p + 8 * k) for k in range(nt)]; p += 8 * nt
        typ = 4 if prop & 0x1000 else 3 if prop & 0x800 else 2 if prop & 0x200 else 1 if prop & 0x100 else 0
        anims.append(dict(prop=prop & 0xFF, type=typ, c=np.array((cx, cy, cz)), a=np.array((ax, ay, az)), fac=fac, off=off,
                          tab=tab))
    meshes = []
    for _ in range(nmesh):
        name = b[p:p + 32].split(b'\0')[0].decode('latin-1')
        flags, = struct.unpack_from('<I', b, p + 52)
        an = struct.unpack_from('<4I', b, p + 56)
        ntri, = struct.unpack_from('<I', b, p + 80)
        v = np.frombuffer(b, dtype='<f4', count=ntri * 27, offset=p + 84).reshape(-1, 9)[:, :3].astype(np.float64)
        meshes.append(dict(name=name, flags=flags, anim=[x for x in an if x < nanim], v=v, ntri=ntri))
        p += 84 + ntri * 108
    return meshes, anims


def angle(an, v):
    t = an['tab']
    if t:
        if v <= t[0][0]: return t[0][1]
        for k in range(1, len(t)):
            if v <= t[k][0]:
                x0, y0 = t[k - 1]; x1, y1 = t[k]
                return y0 + (y1 - y0) * (v - x0) / (x1 - x0) if x1 > x0 else y1
        return t[-1][1]
    return v * an['fac'] + an['off']


def unit(a):
    l = float(np.linalg.norm(a))
    return a / l if l > 1e-6 else a * 0.0           # (the runtime's loader: a zero axis stays zero)


def mat(an, vals):
    """the runtime's 3x4 of one animation (types 0 and 3; 1, 2, 4 are no geometry here: identity)"""
    M = np.eye(4)
    v = vals[an['prop']] if an['prop'] < len(vals) else 0.0
    if an['type'] == 3:
        M[:3, 3] = unit(an['a']) * angle(an, v)
    elif an['type'] == 0:
        t = math.radians(angle(an, v)); cs, sn = math.cos(t), math.sin(t); k = 1 - cs
        x, y, z = unit(an['a'])
        R = np.array([[cs + x * x * k, x * y * k - z * sn, x * z * k + y * sn],
                      [y * x * k + z * sn, cs + y * y * k, y * z * k - x * sn],
                      [z * x * k - y * sn, z * y * k + x * sn, cs + z * z * k]])
        M[:3, :3] = R; M[:3, 3] = an['c'] - R @ an['c']
    return M


def pose(m, anims, vals):
    M = np.eye(4)
    for x in m['anim']: M = M @ mat(anims[x], vals)   # the first animation is the outermost
    return M


def posed(m, M): return m['v'] @ M[:3, :3].T + M[:3, 3]


def rigid(M): return np.abs(M[:3, :3].T @ M[:3, :3] - np.eye(3)).max() < 1e-3


def is_rot(an): return an['type'] == 0


def zero(an): return float(np.linalg.norm(an['a'])) < 1e-6


def live(an): return an['prop'] < len(NAMES) and (an['fac'] != 0.0 or len(an['tab']) > 1)


def summary(tag, meshes, anims, vals, eps, nlist):
    rot = [an for an in anims if is_rot(an)]
    zl = [an for an in rot if zero(an) and live(an)]; zc = [an for an in rot if zero(an) and not live(an)]
    zc_nz = [an for an in zc if abs(angle(an, 0.0)) > 1e-6]
    print('%s: %d meshes, %d animations, %d rotate; zero axis: %d live, %d constant (%d at a non-zero angle)' % (
        tag, len(meshes), len(anims), len(rot), len(zl), len(zc), len(zc_nz)))
    nonrig, fly = [], []
    for i, m in enumerate(meshes):
        if not m['anim'] or m['flags'] & 16 or not len(m['v']): continue
        M = pose(m, anims, vals)
        d = float(np.linalg.norm(posed(m, M) - m['v'], axis=1).max())
        if not rigid(M): nonrig.append((d, m['name'], i))
        if d > eps: fly.append((d, m['name'], i))
    for lab, lst in (('posed non-rigidly (a zero axis at a non-zero angle)', nonrig),
                     ('more than %.2f m from their rest place' % eps, fly)):
        lst.sort(reverse=True)
        print('  %d parts %s%s' % (len(lst), lab, ': ' if lst else ''))
        for d, n, i in lst[:nlist]: print('    %-32s #%-5d %.3f m' % (n[:32], i, d))
        if len(lst) > nlist: print('    ... %d more' % (len(lst) - nlist))
    return nonrig, fly


def match(A, B):
    if [(m['name'], m['ntri']) for m in A] == [(m['name'], m['ntri']) for m in B]: return list(zip(A, B))
    byname = {}
    for m in B: byname.setdefault((m['name'], m['ntri']), []).append(m)
    out = []
    for m in A:
        l = byname.get((m['name'], m['ntri']))
        if l: out.append((m, l.pop(0)))
    return out


def senses(tag, meshes, anims):
    """per live rotate / translate animation used by a mesh: the sense of its motion with a rising value"""
    print('%s: senses of the live animations (dial: CW / CCW as seen from the eye for a rising value; control: the '
          'part\'s centre travel from value 0 to 1, eye frame cm)' % tag)
    seen = {}
    for m in meshes:
        for x in m['anim']: seen.setdefault(x, []).append(m)
    rows = []
    for x, ms in sorted(seen.items()):
        an = anims[x]
        if an['type'] not in (0, 3) or not live(an): continue
        nm = ','.join(sorted({m['name'] for m in ms}))[:44]
        if an['prop'] in (P_ELEV, P_AIL, P_RUD, P_THR):
            v0, v1 = [0.0] * 22, [0.0] * 22; v0[4] = v1[4] = 1.0; v1[an['prop']] = 1.0
            cen = np.concatenate([m['v'] for m in ms]); c0 = cen.mean(0)
            d = (posed(dict(v=c0[None]), mat(an, v1)) - posed(dict(v=c0[None]), mat(an, v0)))[0] * 100
            rows.append('  %-5s anim %-5d %-44s travel %+6.1f %+6.1f %+6.1f cm%s' % (
                NAMES[an['prop']], x, nm, d[0], d[1], d[2], '  (ZERO AXIS)' if zero(an) else ''))
        elif an['type'] == 0:
            if zero(an): rows.append('  %-5s anim %-5d %-44s ZERO AXIS' % (NAMES[an['prop']], x, nm)); continue
            # rising value: the angle's slope near the test value (table or factor)
            v = TEST[an['prop']]; dv = max(abs(v) * 0.01, 0.01)
            slope = angle(an, v + dv) - angle(an, v - dv)
            if abs(slope) < 1e-9:
                vs = [angle(an, t) for t, _ in an['tab']] if an['tab'] else [0.0]
                slope = vs[-1] - vs[0]
            c = an['c']; look = c / (np.linalg.norm(c) + 1e-9)
            s = float(np.dot(unit(an['a']), look)) * slope
            rows.append('  %-5s anim %-5d %-44s %s (axis . view %+.2f, %+.2f deg per unit)' % (
                NAMES[an['prop']], x, nm, 'CW ' if s > 0 else 'CCW', float(np.dot(unit(an['a']), look)), slope / (2 * dv)
                if abs(angle(an, v + dv) - angle(an, v - dv)) > 1e-9 else slope))
    for r in rows: print(r)


def bake(src, meshes, anims, vals, out):
    """a copy of src with every part posed at vals (positions and normals) and no animations left: cpit_view.c then
    draws the instruments and controls where the runtime shows them in that state"""
    b = open(src, 'rb').read()
    d = bytearray(b)
    ntex, nmesh, nanim = struct.unpack_from('<III', b, 8)
    p = 20
    for _ in range(ntex):
        w, h = struct.unpack_from('<II', b, p); p += 8 + w * h * 4
    for _ in range(nanim):
        nt, = struct.unpack_from('<I', b, p + 36); p += 40 + 8 * nt
    for m in meshes:
        ntri, = struct.unpack_from('<I', b, p + 80)
        if m['anim'] and not m['flags'] & 16:
            M = pose(m, anims, vals)
            V = np.frombuffer(b, dtype='<f4', count=ntri * 27, offset=p + 84).reshape(-1, 9).astype(np.float64)
            V[:, :3] = V[:, :3] @ M[:3, :3].T + M[:3, 3]
            n = V[:, 3:6] @ M[:3, :3].T
            V[:, 3:6] = n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
            d[p + 84:p + 84 + ntri * 108] = V.astype('<f4').tobytes()
            struct.pack_into('<4I', d, p + 56, *([0xFFFFFFFF] * 4))
        p += 84 + ntri * 108
    open(out, 'wb').write(d)
    print('posed copy written: %s' % out)


def main():
    a = sys.argv[1:]
    eps, nlist, vals = 0.10, 25, list(TEST)
    bake_out = None
    if '--bake' in a: i = a.index('--bake'); bake_out = a[i + 1]; del a[i:i + 2]
    if '--eps' in a: i = a.index('--eps'); eps = float(a[i + 1]); del a[i:i + 2]
    if '--list' in a: i = a.index('--list'); nlist = int(a[i + 1]); del a[i:i + 2]
    if '--state' in a:
        i = a.index('--state')
        for kv in a[i + 1].split(','):
            k, v = kv.split('='); vals[NAMES.index(k)] = float(v)
        del a[i:i + 2]
    sense = '--senses' in a
    if sense: a.remove('--senses')
    if not a: sys.exit(__doc__)
    files = [read(f) for f in a[:2]]
    for f, (ms, an) in zip(a, files):
        summary(f, ms, an, vals, eps, nlist)
    if len(files) == 2:
        (MA, AA), (MB, AB) = files
        pairs = match(MA, MB)
        print('A -> B: %d of %d / %d meshes matched' % (len(pairs), len(MA), len(MB)))
        for lab, vv in (('test', vals), ('rest', REST)):
            diff = []
            for ma, mb in pairs:
                if not (ma['anim'] or mb['anim']) or ma['flags'] & 16: continue
                d = float(np.linalg.norm(posed(ma, pose(ma, AA, vv)) - posed(mb, pose(mb, AB, vv)), axis=1).max())
                if d > eps: diff.append((d, ma['name']))
            diff.sort(reverse=True)
            print('  %s state: %d parts differ by more than %.2f m%s' % (lab, len(diff), eps, ':' if diff else ''))
            for d, n in diff[:nlist]: print('    %-32s %.3f m' % (n[:32], d))
            if len(diff) > nlist: print('    ... %d more' % (len(diff) - nlist))
    if sense:
        for f, (ms, an) in zip(a, files): senses(f, ms, an)
    if bake_out:
        bake(a[-1], files[-1][0], files[-1][1], vals, bake_out)


if __name__ == '__main__':
    main()
