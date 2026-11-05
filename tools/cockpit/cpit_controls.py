"""cpit_controls.py FILE.cpit ... -- which pilot's controls move and which gloves a cockpit file carries (touch_ux
2026-10-29, bug #238: the census of cockpits/; the runtime logs the same as "cockpit controls:" per model).

Reads only the header, skips the textures and reads the animation table (ac2cpit.py's "ACZCPIT3" layout): the
controls are the rotate / translate animations of CPIT_P_ELEV / AIL (stick), RUD (pedals), THR (throttle); the gloves
are the hand slots (prop 0x1000 | hand << 4 | pose; offset = the control: 1 stick, 2 throttle, 0 none). Prints one
row per file (markdown) with the stick's throw at full deflection (rotate) or travel (translate).
"""
import os, struct, sys

P_ELEV, P_AIL, P_RUD, P_THR = 18, 19, 20, 21


def angle(fac, off, tab, v):
    if tab:
        if v <= tab[0][0]: return tab[0][1]
        for i in range(1, len(tab)):
            if v <= tab[i][0]:
                x0, y0 = tab[i - 1]; x1, y1 = tab[i]
                return y0 + (y1 - y0) * (v - x0) / (x1 - x0) if x1 > x0 else y1
        return tab[-1][1]
    return v * fac + off


def census(path):
    with open(path, 'rb') as f:
        if f.read(8) != b'ACZCPIT3': return None
        ntex, nmesh, nanim = struct.unpack('<III', f.read(12))
        for _ in range(ntex):
            w, h = struct.unpack('<II', f.read(8))
            f.seek(w * h * 4, 1)
        anims = []
        for _ in range(nanim):
            prop, cx, cy, cz, ax, ay, az, fac, off, nt = struct.unpack('<I3f3fffI', f.read(40))
            tab = [struct.unpack('<ff', f.read(8)) for _ in range(nt)]
            anims.append((prop, (cx, cy, cz), (ax, ay, az), fac, off, tab))
    r = {'stick': [], 'thr': [], 'rud': [], 'hands': {}, 'nanim': nanim}
    for prop, c, a, fac, off, tab in anims:
        if prop & 0x1000:
            hand, pose = (prop >> 4) & 1, prop & 15
            r['hands'].setdefault(hand, []).append((pose, int(off), c))
            continue
        typ = 3 if prop & 0x800 else 0 if prop < 0x100 else -1
        if typ < 0: continue
        p = prop & 0xFF
        amt = max(abs(angle(fac, off, tab, 1.0)), abs(angle(fac, off, tab, -1.0)))
        if p in (P_ELEV, P_AIL): r['stick'].append((p, typ, amt))
        elif p == P_THR: r['thr'].append((typ, max(abs(angle(fac, off, tab, 0.0)), abs(angle(fac, off, tab, 1.0)))))
        elif p == P_RUD: r['rud'].append((typ, amt))
    return r


def fmt(r):
    def stick():
        if not r['stick']: return 'still'
        el = [x for x in r['stick'] if x[0] == P_ELEV]
        rot = [x[2] for x in el if x[1] == 0]
        tr = [x[2] for x in el if x[1] == 3]
        s = 'moves'
        if rot: s += ' (%.0f deg)' % max(rot)
        elif tr: s += ' (%.1f cm)' % (100 * max(tr))
        return s
    def hands():
        if not r['hands']: return 'none'
        out = []
        for h in sorted(r['hands']):
            poses = r['hands'][h]
            ctl = {o for p, o, c in poses if p == 0}
            out.append('%s %d poses%s' % ('R' if h else 'L', len(poses),
                                           ' on the %s' % ('stick' if 1 in ctl else 'throttle') if ctl else ' (free only)'))
        return ', '.join(out)
    return stick(), 'moves' if r['thr'] else 'still', 'move' if r['rud'] else 'still', hands()


if __name__ == '__main__':
    print('| file | stick | throttle | pedals | gloves |')
    print('|---|---|---|---|---|')
    for p in sys.argv[1:]:
        r = census(p)
        if r is None:
            print('| %s | (not a cockpit file) | | | |' % os.path.basename(p)); continue
        print('| %s | %s | %s | %s | %s |' % ((os.path.basename(p),) + fmt(r)))
