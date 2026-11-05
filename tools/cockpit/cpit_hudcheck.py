"""cpit_hudcheck.py -- (ck_align 2026-10-28, bug #154) offline audit of a cockpit model (.cpit, format 3) against the runtime's HUD rules.

For each file: the glass meshes the runtime takes as the HUD's combiner (ACZ_VR_HUD_GLASS_MESH rule: name, glass,
centred within 15 deg of straight ahead, nearer than 1.5 m), their angular extent from the design eye and the
runtime's 5-ray boresight test (ps2_video.c cpit load: hg_hits), the boresight's clearance (the largest circle about
the boresight inside the glass, and its clearance up / down / left / right), plus geometry around the design eye:
what the boresight ray hits, the first solid part below the boresight (over-the-nose view) and above it (canopy bow),
the seat under the eye and the headrest behind it, and the candidate glass parts ahead (unmarked combiners).

Usage: python -I cpit_hudcheck.py FILE.cpit [...] [--names LIST] [--json OUT] [--dz M] [--dy M] [--hide LIST]
  --names  the combiner name list (default: the runtime's ACZ_VR_HUD_GLASS_MESH default)
  --dy/--dz  evaluate from an eye moved by that much (metres; + = up / back) -- what a rebuilt eye would see
  --hide   comma list of mesh names left out (ACZ_VR_COCKPIT_HIDE-like)
  --all    keep the parts behind a select that is false whatever the runtime's values (by default they are left out,
           as the runtime's ACZ_VR_COCKPIT_CONST_HIDE does at load)
Reads only the given files (skips the textures). Needs numpy (any Python with them).
"""
import json, math, re, struct, sys
import numpy as np

DEF_NAMES = ("HUD_glass,revi.verre*,HUDglass1,HUD2,gunsight_glass,ILSGlass,glass.001,CPT-hud-screen,viseur,frm-24,"
             "HUDImage1,HUDImage2,hudglass,GlassHud,DrawCall63")      # = ps2_video.c hg_init's default
HDR = 84
CPIT_P_N = 22                                  # ps2_video.c: the properties the runtime drives (CPIT_P_SPEED..THR)
VTX = 9


def name_match(name, lst):
    n = name.lower()
    for p in lst.split(','):
        p = p.strip().lower()
        if not p:
            continue
        if p.endswith('*'):
            if n.startswith(p[:-1]):
                return True
        elif n == p:
            return True
    return False


def load(path):
    f = open(path, 'rb')
    d = f.read(20)
    if d[:8] != b'ACZCPIT3':
        raise ValueError('not a format-3 cpit: ' + path)
    ntex, nmesh, nanim = struct.unpack('<3I', d[8:20])
    for _ in range(ntex):
        w, h = struct.unpack('<2I', f.read(8))
        f.seek(w * h * 4, 1)
    sel = []                                   # per animation: None, or shown (1/0) with every property at 0
    for _ in range(nanim):
        a = f.read(40)
        nt = struct.unpack('<I', a[36:40])[0]
        tab = f.read(nt * 8)
        prop, = struct.unpack('<I', a[:4]); c = struct.unpack('<3f', a[4:16])
        if (prop & 0x1000) or (prop & 0x800) or not (prop & 0x200):
            sel.append(None); continue
        anyr, allr, cst = 0, 1, True           # (ps2_video.c cpit_const_hidden: each term on x = 0)
        for t in range(nt):
            cf, val = struct.unpack('<2f', tab[t * 8:t * 8 + 8])
            code = int(cf); op = code & 0xFF; x = 0.0
            if (code >> 8) & 0xFF < CPIT_P_N:      # a property the game drives: decided per frame
                cst = False; break
            r = (x < val if op == 1 else x <= val if op == 2 else x > val if op == 3 else x >= val if op == 4
                 else x == val if op == 5 else x != val if op == 6 else x != 0.0)
            if (code >> 16) & 1: r = not r
            anyr |= r; allr &= r
        if not cst:
            sel.append(None); continue
        on = anyr if c[0] != 0.0 else allr
        if c[1] != 0.0: on = not on
        sel.append(int(bool(on)))
    meshes = []
    for i in range(nmesh):
        h = f.read(HDR)
        if len(h) < HDR:
            break
        name = h[:32].split(b'\0')[0].decode('latin-1')
        tex, = struct.unpack('<I', h[32:36])
        rgba = struct.unpack('<4f', h[36:52])
        flags, = struct.unpack('<I', h[52:56])
        anim = struct.unpack('<4I', h[56:72])
        ntri, = struct.unpack('<I', h[80:84])
        v = np.frombuffer(f.read(ntri * 3 * VTX * 4), dtype='<f4').reshape(ntri * 3, VTX) if ntri else np.zeros((0, VTX), 'f4')
        chain = list(anim[:anim.index(0xFFFFFFFF)] if 0xFFFFFFFF in anim else anim)
        chid = not (flags >> 8) & 255 and any(k < len(sel) and sel[k] == 0 for k in chain)
        meshes.append(dict(i=i, name=name, tex=tex, rgba=rgba, flags=flags, anim=anim, ntri=ntri, chid=chid,
                           pos=v[:, :3].astype(np.float64).copy()))
    f.close()
    for m in meshes:
        p = m['pos']
        if len(p):
            lo, hi = p.min(0), p.max(0)
            c = 0.5 * (lo + hi)
            r = float(np.sqrt(((p - c) ** 2).sum(1)).max()) + 0.001
        else:
            lo = hi = c = np.zeros(3); r = 0.0
        m['lo'], m['hi'], m['c'], m['r'] = lo, hi, c, r
    return meshes


def ray_hits(tris, o, dirs):
    """Moller-Trumbore: tris (T,3,3), rays from o along dirs (N,3): nearest t per ray (inf) and triangle index"""
    v0, e1, e2 = tris[:, 0], tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]
    best_t = np.full(len(dirs), np.inf); best_i = np.full(len(dirs), -1)
    for k, dv in enumerate(dirs):
        pv = np.cross(dv, e2)
        det = (e1 * pv).sum(1)
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1.0 / np.where(ok, det, 1.0), 0.0)
        tv = o - v0
        u = (tv * pv).sum(1) * inv
        qv = np.cross(tv, e1)
        vv = (qv * dv).sum(1) * inv
        t = (e2 * qv).sum(1) * inv
        hit = ok & (u >= 0) & (vv >= 0) & (u + vv <= 1) & (t > 1e-4)
        if hit.any():
            tt = np.where(hit, t, np.inf)
            j = int(tt.argmin())
            best_t[k] = tt[j]; best_i[k] = j
    return best_t, best_i


def dir_of(az, el):
    a, e = math.radians(az), math.radians(el)
    return np.array([math.sin(a) * math.cos(e), math.sin(e), -math.cos(a) * math.cos(e)])


def analyse(path, names=DEF_NAMES, dy=0.0, dz=0.0, hide='', keep_all=False):
    meshes = load(path)
    nch = sum(1 for m in meshes if m['chid'] and m['ntri'])
    if not keep_all:                           # as ACZ_VR_COCKPIT_CONST_HIDE: never drawn, left out at load
        meshes = [m for m in meshes if not m['chid']]
    eye = np.array([0.0, dy, dz])
    hide_l = hide
    res = dict(file=path.replace('\\', '/').split('/')[-1], eye_shift=[0.0, dy, dz], nmesh=len(meshes), const_hidden=nch)
    # the runtime's combiner rule (from the shifted eye: positions relative to it)
    ap, hits, ext = [], 0, [1e9, -1e9, 1e9, -1e9]
    rx = [0.0, -0.0349, 0.0349, 0.0, 0.0]; ry = [0.0, 0.0, 0.0, -0.0349, 0.0349]
    ap_tris = []
    for m in meshes:
        if not (m['flags'] & 1) or not m['ntri'] or (hide_l and name_match(m['name'], hide_l)):
            continue
        c = m['c'] - eye
        bl = float(np.linalg.norm(c))
        named = name_match(m['name'], names)
        ahead = 0.05 < bl < 1.5 and -c[2] > 0.966 * bl
        if not (named and ahead):
            continue
        ap.append(m['name'])
        p = (m['pos'] - eye).reshape(-1, 3, 3)
        for t in p:
            if (t[:, 2] > -0.01).any():
                # extents count the vertices up to the first one behind (as the runtime)
                for q in t:
                    if q[2] > -0.01:
                        break
                    az = math.degrees(math.atan(q[0] / -q[2])); el = math.degrees(math.atan(q[1] / -q[2]))
                    ext = [min(ext[0], az), max(ext[1], az), min(ext[2], el), max(ext[3], el)]
                continue
            a = np.stack([t[:, 0] / -t[:, 2], t[:, 1] / -t[:, 2]], 1)
            for q in a:
                az, el = math.degrees(math.atan(q[0])), math.degrees(math.atan(q[1]))
                ext = [min(ext[0], az), max(ext[1], az), min(ext[2], el), max(ext[3], el)]
            ap_tris.append(a)
            for r in range(5):
                s = [(a[(j + 1) % 3][0] - a[j][0]) * (ry[r] - a[j][1]) - (a[(j + 1) % 3][1] - a[j][1]) * (rx[r] - a[j][0])
                     for j in range(3)]
                if (s[0] >= 0 and s[1] >= 0 and s[2] >= 0) or (s[0] <= 0 and s[1] <= 0 and s[2] <= 0):
                    hits |= 1 << r
    res['combiner'] = ap
    res['ext'] = [round(x, 1) for x in ext] if ap else None
    res['hits'] = hits
    res['taken'] = hits == 31
    res['verdict'] = ('inside' if hits == 31 else 'inside <2deg' if hits & 1 else 'outside') if ap else 'none'
    # clearance of the boresight inside the union of the projected combiner triangles (degrees)
    if ap_tris:
        T = np.array(ap_tris)                                 # (n,3,2) tangents
        def inside(pts):                                       # pts (P,2) -> bool (P,)
            out = np.zeros(len(pts), bool)
            for s in range(0, len(T), 2048):
                t = T[s:s + 2048]
                x0, y0 = t[:, 0, 0][None], t[:, 0, 1][None]
                x1, y1 = t[:, 1, 0][None], t[:, 1, 1][None]
                x2, y2 = t[:, 2, 0][None], t[:, 2, 1][None]
                px, py = pts[:, 0:1], pts[:, 1:2]
                s0 = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)
                s1 = (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1)
                s2 = (x0 - x2) * (py - y2) - (y0 - y2) * (px - x2)
                h = ((s0 >= 0) & (s1 >= 0) & (s2 >= 0)) | ((s0 <= 0) & (s1 <= 0) & (s2 <= 0))
                out |= h.any(1)
            return out
        rr = np.arange(0.0, 40.0, 0.05)
        clear = {}
        mins = 99.0
        for th in range(0, 360, 3):
            ang = np.radians(rr)
            pts = np.stack([np.tan(ang) * math.cos(math.radians(th)), np.tan(ang) * math.sin(math.radians(th))], 1)
            ins = inside(pts)
            if not ins[0]:
                first = 0.0
            else:
                o = np.nonzero(~ins)[0]
                first = float(rr[o[0]]) if len(o) else 40.0
            mins = min(mins, first)
            clear[th] = first
        res['margin'] = round(mins, 2)
        res['clear_up'] = clear[90]; res['clear_down'] = clear[270]
        res['clear_left'] = clear[180]; res['clear_right'] = clear[0]
    # geometry around the eye: solid parts, glass parts
    solid = [m for m in meshes if m['ntri'] and not (m['flags'] & 1) and not (m['flags'] & 16)
             and not (hide_l and name_match(m['name'], hide_l))]
    glass = [m for m in meshes if m['ntri'] and (m['flags'] & 1) and not (hide_l and name_match(m['name'], hide_l))]
    def tri_set(ms):
        if not ms:
            return np.zeros((0, 3, 3)), np.zeros(0, int)
        t = np.concatenate([m['pos'].reshape(-1, 3, 3) for m in ms])
        owner = np.concatenate([np.full(m['ntri'], k) for k, m in enumerate(ms)])
        return t, owner
    ST, SO = tri_set(solid)
    GT, GO = tri_set(glass)
    def first_solid(dirs):
        t, i = ray_hits(ST, eye, dirs)
        return t, [solid[SO[j]]['name'] if j >= 0 else None for j in i]
    # boresight: every part along it (solid and glass)
    bt, bn = first_solid([dir_of(0, 0)])
    gt, gi = ray_hits(GT, eye, [dir_of(0, 0)])
    res['bore_solid'] = (bn[0], round(float(bt[0]), 3) if np.isfinite(bt[0]) else None)
    res['bore_glass'] = (glass[GO[gi[0]]]['name'] if gi[0] >= 0 else None, round(float(gt[0]), 3) if np.isfinite(gt[0]) else None)
    # over the nose: from 0 down in 0.1 deg steps, the first elevation whose ray meets a solid part within 6 m
    els = np.arange(0.0, -40.0, -0.1)
    t, n = first_solid([dir_of(0, e) for e in els])
    k = next((j for j in range(len(els)) if np.isfinite(t[j]) and t[j] < 6.0), None)
    res['nose'] = (round(float(els[k]), 1), n[k], round(float(t[k]), 3)) if k is not None else None
    # the lowest elevation still seeing the world over the nose (the ray reaching beyond 30 m: past the aircraft)
    k2 = next((j for j in range(len(els)) if np.isfinite(t[j]) and t[j] < 30.0 and n[j] is not None), None)
    # above: the first solid part from 0 up (canopy bow / frame) within 2 m
    elu = np.arange(0.0, 70.0, 0.2)
    t, n = first_solid([dir_of(0, e) for e in elu])
    k = next((j for j in range(len(elu)) if np.isfinite(t[j]) and t[j] < 2.0), None)
    res['bow'] = (round(float(elu[k]), 1), n[k], round(float(t[k]), 3)) if k is not None else None
    # the seat under the eye (straight down and 10 deg forward), the headrest behind
    t, n = first_solid([np.array([0, -1.0, 0]), dir_of(0, -80)])
    res['down'] = [(n[j], round(float(t[j]), 3) if np.isfinite(t[j]) else None) for j in range(2)]
    t, n = first_solid([np.array([0, 0, 1.0]), np.array([0, -0.2588, 0.9659])])
    res['back'] = [(n[j], round(float(t[j]), 3) if np.isfinite(t[j]) else None) for j in range(2)]
    # candidate combiners: glass centred within 25 deg of ahead, nearer than 1.5 m, radius under 0.25 m
    cand = []
    for m in glass:
        c = m['c'] - eye; bl = float(np.linalg.norm(c))
        if 0.05 < bl < 1.5 and -c[2] > math.cos(math.radians(25)) * bl and m['r'] <= 0.25:
            p = m['pos'] - eye
            fr = p[:, 2] < -0.01
            az = np.degrees(np.arctan(p[fr, 0] / -p[fr, 2])); el = np.degrees(np.arctan(p[fr, 1] / -p[fr, 2]))
            cand.append(dict(name=m['name'], c=[round(float(x), 3) for x in c], r=round(m['r'], 3), ntri=m['ntri'],
                             az=[round(float(az.min()), 1), round(float(az.max()), 1)] if fr.any() else None,
                             el=[round(float(el.min()), 1), round(float(el.max()), 1)] if fr.any() else None,
                             rgba=[round(x, 2) for x in m['rgba']], named=name_match(m['name'], names)))
    res['cand'] = cand
    # parts named like a HUD / sight
    hudish = []
    for m in meshes:
        nl = m['name'].lower()
        if re.search(r'hud|sight|revi|viseur|^ils|combin|collim|pricel|kols', nl) and m['ntri']:
            c = m['c'] - eye
            hudish.append(dict(name=m['name'], glass=bool(m['flags'] & 1), c=[round(float(x), 3) for x in c],
                               r=round(m['r'], 3), ntri=m['ntri']))
    res['hudish'] = hudish
    return res


def main():
    a = sys.argv[1:]
    names, out, dy, dz, hide, files, keep_all = DEF_NAMES, None, 0.0, 0.0, '', [], False
    i = 0
    while i < len(a):
        if a[i] == '--names': names = a[i + 1]; i += 2
        elif a[i] == '--json': out = a[i + 1]; i += 2
        elif a[i] == '--dy': dy = float(a[i + 1]); i += 2
        elif a[i] == '--dz': dz = float(a[i + 1]); i += 2
        elif a[i] == '--hide': hide = a[i + 1]; i += 2
        elif a[i] == '--all': keep_all = True; i += 1
        else: files.append(a[i]); i += 1
    allr = []
    for f in files:
        r = analyse(f, names, dy, dz, hide, keep_all)
        allr.append(r)
        print('== %s (%d meshes, %d behind a constant-false select %s)%s' % (r['file'], r['nmesh'], r['const_hidden'],
              'kept' if keep_all else 'left out', '  eye shift dy %+.3f dz %+.3f' % (dy, dz) if dy or dz else ''))
        if r['combiner']:
            print('  combiner %s: az %.1f..%.1f el %.1f..%.1f deg; boresight %s; margin %.2f deg (up %.2f down %.2f '
                  'left %.2f right %.2f)' % (','.join(r['combiner']), *r['ext'], r['verdict'], r['margin'],
                                              r['clear_up'], r['clear_down'], r['clear_left'], r['clear_right']))
        else:
            print('  combiner: none marked')
        print('  boresight: solid %s, glass %s' % (r['bore_solid'], r['bore_glass']))
        print('  over the nose (first solid below 0 within 6 m): %s' % (r['nose'],))
        print('  above (first solid from 0 up within 2 m): %s' % (r['bow'],))
        print('  down: %s   back: %s' % (r['down'], r['back']))
        for c in r['cand']:
            print('  glass ahead: %-24s c %s r %.3f ntri %d az %s el %s rgba %s%s' % (c['name'], c['c'], c['r'], c['ntri'],
                  c['az'], c['el'], c['rgba'], ' (named)' if c['named'] else ''))
        for c in r['hudish']:
            print('  hud-named: %-24s %s c %s r %.3f ntri %d' % (c['name'], 'glass' if c['glass'] else 'solid', c['c'],
                                                                 c['r'], c['ntri']))
    if out:
        json.dump(allr, open(out, 'w'), indent=1, default=str)


if __name__ == '__main__':
    main()
