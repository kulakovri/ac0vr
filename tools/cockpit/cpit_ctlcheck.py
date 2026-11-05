"""cpit_ctlcheck.py FILE.cpit [--hide REGEX] [--bake-hands OUT.cpit] -- offline check of a cockpit file's moving
controls (ck_ctl 2026-10-30, bug #241: the stick, throttle and pedals grafted into a cockpit).

The meshes animated by the controls (rotate / translate animations of CPIT_P_ELEV / AIL = the stick, THR = the throttle,
RUD = the pedals; ac2cpit.py --controls) are posed at their extremes -- the stick at every corner and centre of pitch x
roll in -1 / 0 / 1, the throttle at 0 (IDLE), 0.5, 0.95 (MIL), 1 (MAX), the rudder at -1 / 0 / 1 -- and tested against
the still cockpit (meshes without animations; glass and gloves left out): per pose the triangle pairs that cross (an edge
of one through the other) per still mesh, with the box of the control's crossing triangles. A control resting on its
console or floor crosses that surface at its foot; anything else (the seat, the panel, a canopy sill) is a collision.
--bake-hands OUT.cpit writes a copy whose gloves stand in their grip poses on the controls at rest (the slot's origin
and rotation applied, flag 16 cleared; the other poses moved 100 m away), so that cpit_view.c shows them.
Eye frame metres (X right, Y up, Z back). Needs numpy (PYV: any Python with it). Only reads FILE.cpit.
"""
import math, re, struct, sys
import numpy as np

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
        anims.append(dict(prop=prop, c=(cx, cy, cz), a=(ax, ay, az), fac=fac, off=off, tab=tab))
    meshes = []
    for _ in range(nmesh):
        name = b[p:p + 32].split(b'\0')[0].decode('latin-1')
        flags, = struct.unpack_from('<I', b, p + 52)
        an = struct.unpack_from('<4I', b, p + 56)
        ntri, = struct.unpack_from('<I', b, p + 80)
        vp = p + 84
        v = np.frombuffer(b, dtype='<f4', count=ntri * 27, offset=vp).reshape(ntri, 3, 9)
        meshes.append(dict(name=name, flags=flags, anim=an, v=v, at=p, vp=vp))
        p = vp + ntri * 108
    return b, meshes, anims


def angle(an, v):
    tab = an['tab']
    if tab:
        if v <= tab[0][0]: return tab[0][1]
        for k in range(1, len(tab)):
            if v <= tab[k][0]:
                x0, y0 = tab[k - 1]; x1, y1 = tab[k]
                return y0 + (y1 - y0) * (v - x0) / (x1 - x0) if x1 > x0 else y1
        return tab[-1][1]
    return v * an['fac'] + an['off']


def rot(axis, t):
    n = np.array(axis, dtype=float); n /= np.linalg.norm(n) + 1e-12
    K = np.array([[0, -n[2], n[1]], [n[2], 0, -n[0]], [-n[1], n[0], 0]])
    return np.eye(3) + math.sin(t) * K + (1 - math.cos(t)) * K @ K


def xform(an, vals):
    p = an['prop']; v = vals.get(p & 0xFF, 0.0)
    M = np.eye(4)
    if p & 0x800:                                         # translate (--controls)
        ax = np.array(an['a'], dtype=float); M[:3, 3] = ax / (np.linalg.norm(ax) + 1e-12) * angle(an, v)
    elif p < 0x100:                                       # rotate
        R = rot(an['a'], math.radians(angle(an, v))); c = np.array(an['c'])
        M[:3, :3] = R; M[:3, 3] = c - R @ c
    return M


def seg_tri(P0, P1, T):
    d = P1 - P0
    A = T[:, 0]; e1 = T[:, 1] - A; e2 = T[:, 2] - A
    pv = np.cross(d[:, None, :], e2[None]); det = (e1[None] * pv).sum(-1)
    ok = np.abs(det) > 1e-14; inv = np.where(ok, 1 / np.where(ok, det, 1), 0)
    tv = P0[:, None, :] - A[None]; u = (tv * pv).sum(-1) * inv
    qv = np.cross(tv, e1[None]); w = (qv * d[:, None, :]).sum(-1) * inv; t = (e2[None] * qv).sum(-1) * inv
    return ok & (u >= 0) & (w >= 0) & (u + w <= 1) & (t >= 0) & (t <= 1)


def crossing(TA, TB):
    hit = np.zeros((len(TA), len(TB)), bool)
    for i0, i1 in ((0, 1), (1, 2), (2, 0)):
        hit |= seg_tri(TA[:, i0], TA[:, i1], TB)
        hit |= seg_tri(TB[:, i0], TB[:, i1], TA).T
    return hit


def bake_hands(b, meshes, anims, out):
    d = bytearray(b)
    for m in meshes:
        if not (m['flags'] & 16) or m['anim'][0] >= len(anims): continue
        an = anims[m['anim'][0]]
        V = m['v'].reshape(-1, 9).astype(np.float32).copy()
        if (an['prop'] & 15) == 0:                        # the grip pose, on its control at rest
            rv = np.array(an['a'], dtype=float); t = np.linalg.norm(rv)
            R = rot(rv, t) if t > 1e-12 else np.eye(3)
            V[:, :3] = (R @ V[:, :3].T).T + np.array(an['c']); V[:, 3:6] = (R @ V[:, 3:6].T).T
        else:
            V[:, :3] += 100.0
        d[m['vp']:m['vp'] + V.nbytes] = V.astype('<f4').tobytes()
        struct.pack_into('<I', d, m['at'] + 52, m['flags'] & ~16)
        struct.pack_into('<4I', d, m['at'] + 56, *([0xFFFFFFFF] * 4))
    open(out, 'wb').write(d)
    print('gloves baked into %s' % out)


def main():
    a = sys.argv[1:]
    hide = bake = None
    if '--hide' in a: i = a.index('--hide'); hide = a[i + 1]; del a[i:i + 2]
    if '--bake-hands' in a: i = a.index('--bake-hands'); bake = a[i + 1]; del a[i:i + 2]
    if not a: sys.exit(__doc__)
    b, meshes, anims = read(a[0])
    if bake: bake_hands(b, meshes, anims, bake)
    groups, still = {}, []
    for m in meshes:
        if m['flags'] & (1 | 16) or (hide and re.search(hide, m['name'])): continue
        idx = [x for x in m['anim'] if x < len(anims)]
        props = {anims[x]['prop'] & 0xFF for x in idx if anims[x]['prop'] < 0x100 or (anims[x]['prop'] & 0x800 and anims[x]['prop'] < 0x1000)}
        g = 'stick' if props & {P_ELEV, P_AIL} else 'throttle' if P_THR in props else 'pedals' if P_RUD in props else None
        if g: groups.setdefault(g, []).append((m, idx))
        elif not idx: still.append(m)
    S = np.concatenate([m['v'][:, :, :3] for m in still]).astype(np.float64)
    SN = np.concatenate([[m['name']] * len(m['v']) for m in still])
    poses = {'stick': [{P_ELEV: e, P_AIL: r} for e in (-1, 0, 1) for r in (-1, 0, 1)],
             'throttle': [{P_THR: t} for t in (0, 0.5, 0.95, 1)], 'pedals': [{P_RUD: r} for r in (-1, 0, 1)]}
    short = {P_ELEV: 'elev', P_AIL: 'ail', P_RUD: 'rud', P_THR: 'thr'}
    for g in ('stick', 'throttle', 'pedals'):
        if g not in groups: print('%s: none' % g); continue
        lst = groups[g]
        print('%s: %d meshes, %d triangles' % (g, len(lst), sum(len(m['v']) for m, _ in lst)))
        for vals in poses[g]:
            T = []
            for m, idx in lst:
                M = np.eye(4)
                for x in idx: M = M @ xform(anims[x], vals)          # outermost first
                V = m['v'][:, :, :3].reshape(-1, 3).astype(np.float64)
                T.append(((M[:3, :3] @ V.T).T + M[:3, 3]).reshape(-1, 3, 3))
            T = np.concatenate(T)
            lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
            near = np.all((S.max(1) >= lo - 0.02) & (S.min(1) <= hi + 0.02), 1)
            SB, SNB = S[near], SN[near]
            hits = {}
            for c0 in range(0, len(T), 400):
                h = crossing(T[c0:c0 + 400], SB)
                for j in np.where(h.any(0))[0]:
                    e = hits.setdefault(SNB[j], [0, np.full(3, 9.0), np.full(3, -9.0)])
                    rows = np.where(h[:, j])[0]; e[0] += len(rows)
                    cc = T[c0 + rows].mean(1); e[1] = np.minimum(e[1], cc.min(0)); e[2] = np.maximum(e[2], cc.max(0))
            print('  %-16s X %+.3f..%+.3f Y %+.3f..%+.3f Z %+.3f..%+.3f  %s' % (
                ' '.join('%s %g' % (short[k], v) for k, v in vals.items()), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2],
                'no crossing' if not hits else 'crossing:'))
            for n, (k, l_, h_) in sorted(hits.items(), key=lambda x: -x[1][0]):
                print('      %-32s %4d  where X %+.3f..%+.3f Y %+.3f..%+.3f Z %+.3f..%+.3f' % (n[:32], k, l_[0], h_[0], l_[1], h_[1], l_[2], h_[2]))


if __name__ == '__main__':
    main()
