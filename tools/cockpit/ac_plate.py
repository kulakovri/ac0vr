"""ac_plate.py -- reshape a flat plate object of an AC3D file that faces along the file's z axis (a HUD combiner glass
facing the pilot; its width along x): its height is re-spread so that its lowest and highest vertices come to the
given heights, along the plate's own slope in the y-z plane (a least-squares fit z = a + b y over its vertices), so
its tilt, width and thickness stay as they are and its rounded corners stretch with it. Used for a combiner plate (bug
#154: a combiner frm-24 rose only to the design eye's level; the real NSU-2 sight head's glass stands on
the sight head body, its visible part about 0.85 as high as it is wide).

Usage: python -I ac_plate.py IN.ac OUT.ac OBJECT Y0 Y1
  OBJECT  the object's exact name (a 'poly' object)
  Y0, Y1  the new lowest / highest vertex height (the file's own up axis y = FlightGear z, metres)
Prints the plate's slope from the vertical and the old / new extents. Reads and writes only the given files.
"""
import math, sys


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    src, dst, name, y0, y1 = sys.argv[1], sys.argv[2], sys.argv[3], float(sys.argv[4]), float(sys.argv[5])
    lines = open(src, encoding='latin-1', newline='').read().split('\n')
    i, found = 0, 0
    while i < len(lines):
        if lines[i].startswith('name "') and lines[i].split('"')[1] == name:
            j = i + 1
            while j < len(lines) and not lines[j].startswith('numvert') and not lines[j].startswith('OBJECT'):
                j += 1
            if j >= len(lines) or not lines[j].startswith('numvert'):
                i += 1
                continue
            n = int(lines[j].split()[1])
            v = [[float(x) for x in lines[j + 1 + k].split()[:3]] for k in range(n)]
            my = sum(p[1] for p in v) / n
            mz = sum(p[2] for p in v) / n
            syy = sum((p[1] - my) ** 2 for p in v)
            syz = sum((p[1] - my) * (p[2] - mz) for p in v)
            b = syz / syy if syy > 0 else 0.0                  # z = a + b y: the plate's slope
            ymin, ymax = min(p[1] for p in v), max(p[1] for p in v)
            k_ = (y1 - y0) / (ymax - ymin)
            out = []
            for p in v:
                y = y0 + (p[1] - ymin) * k_
                out.append((p[0], y, p[2] + b * (y - p[1])))      # moved along the slope (y, b y)
            for k in range(n):
                lines[j + 1 + k] = '%.6g %.6g %.6g' % out[k]
            print('ac_plate: %s "%s": %d vertices, slope %.1f deg from the vertical; height %.4f..%.4f -> %.4f..%.4f m, '
                  'along the slope %.4f -> %.4f m' % (src, name, n, math.degrees(math.atan(abs(b))), ymin, ymax, y0, y1,
                                                       (ymax - ymin) * math.hypot(1, b), (y1 - y0) * math.hypot(1, b)))
            found += 1
            i = j + 1 + n
            continue
        i += 1
    if not found:
        sys.exit('ac_plate: no poly object "%s" in %s' % (name, src))
    open(dst, 'w', encoding='latin-1', newline='').write('\n'.join(lines))


if __name__ == '__main__':
    main()
