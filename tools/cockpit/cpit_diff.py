#!/usr/bin/env python3
"""cpit_diff.py -- the offline parity check of a cockpit file against another: per view of two cpit_view output folders
(the same model in two formats) the pixel differences, one TSV line per view and a summary per model. With --png DIR: the
image pair and an amplified diff (x8) of the worst view, and the worst 256 x 256 px crop 2x enlarged (night_assets
2026-11-04; RELEASE_RESEARCH_2026-11-03.md R1). Reads binary PPM (P6) only; numpy and Pillow from the venv
(a venv of your own), python -I.

usage: cpit_diff.py OLD_DIR NEW_DIR [--png OUT_DIR --name MODEL] [--keep-ppm]

columns: view, mean absolute difference (0-255, per channel), RMS, the share of pixels whose largest channel difference
is over 4 / 8 / 16 / 32 of 255, the largest difference. PPMs are deleted unless --keep-ppm.
"""
import glob, os, sys
import numpy as np
from PIL import Image


def read_ppm(path):
    with open(path, 'rb') as f:
        d = f.read()
    # P6 <w> <h> <max> <data>; comments are not used by cpit_view
    parts = d.split(maxsplit=4)
    if parts[0] != b'P6':
        raise ValueError(path)
    w, h = int(parts[1]), int(parts[2])
    pos = 0
    for tok in parts[:4]:
        pos = d.index(tok, pos) + len(tok)
    data = np.frombuffer(d, dtype=np.uint8, count=w * h * 3, offset=pos + 1)
    return data.reshape(h, w, 3)


def main():
    a = sys.argv[1:]
    png = None
    name = ''
    keep = '--keep-ppm' in a
    if keep:
        a.remove('--keep-ppm')
    if '--png' in a:
        k = a.index('--png')
        png = a[k + 1]
        del a[k:k + 2]
    if '--name' in a:
        k = a.index('--name')
        name = a[k + 1]
        del a[k:k + 2]
    old, new = a[0], a[1]
    views = sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(old, '*.ppm')) if not p.endswith('_d.ppm'))
    print('view\tmean\trms\t>4\t>8\t>16\t>32\tmax')
    worst = None
    tot = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0]
    for v in views:
        A = read_ppm(os.path.join(old, v + '.ppm'))
        B = read_ppm(os.path.join(new, v + '.ppm'))
        D = np.abs(A.astype(np.int16) - B.astype(np.int16)).astype(np.uint8)
        m = D.max(axis=2)
        r = [float(D.mean()), float(np.sqrt((D.astype(np.float64) ** 2).mean())), float((m > 4).mean()), float((m > 8).mean()),
             float((m > 16).mean()), float((m > 32).mean()), int(m.max())]
        print('%s\t%.3f\t%.3f\t%.4f%%\t%.4f%%\t%.4f%%\t%.4f%%\t%d' % (v, r[0], r[1], 100 * r[2], 100 * r[3], 100 * r[4], 100 * r[5], r[6]))
        for i in range(6):
            tot[i] += r[i]
        tot[6] = max(tot[6], r[6])
        if worst is None or r[3] > worst[0]:
            worst = (r[3], v, A, B, D, m)
    n = max(1, len(views))
    print('all %d views\t%.3f\t%.3f\t%.4f%%\t%.4f%%\t%.4f%%\t%.4f%%\t%d' % (len(views), tot[0] / n, tot[1] / n, 100 * tot[2] / n, 100 * tot[3] / n,
                                                                            100 * tot[4] / n, 100 * tot[5] / n, tot[6]))
    if png and worst is not None:
        os.makedirs(png, exist_ok=True)
        _, v, A, B, D, m = worst
        h, w = m.shape
        Image.fromarray(A).save(os.path.join(png, '%s_%s_old.png' % (name, v)))
        Image.fromarray(B).save(os.path.join(png, '%s_%s_new.png' % (name, v)))
        Image.fromarray(np.clip(D.astype(np.int32) * 8, 0, 255).astype(np.uint8)).save(os.path.join(png, '%s_%s_diffx8.png' % (name, v)))
        # the 256 x 256 window with the most error: old, new, diff x 8 side by side at 2x
        s = 256
        ii = np.pad((m.astype(np.float64)).cumsum(0).cumsum(1), ((1, 0), (1, 0)))
        if h > s and w > s:
            sums = ii[s:, s:] - ii[:-s, s:] - ii[s:, :-s] + ii[:-s, :-s]
            y, x = np.unravel_index(np.argmax(sums), sums.shape)
        else:
            y, x = 0, 0
        tri = np.concatenate([A[y:y + s, x:x + s], B[y:y + s, x:x + s], np.clip(D[y:y + s, x:x + s].astype(np.int32) * 8, 0, 255).astype(np.uint8)], axis=1)
        Image.fromarray(tri).resize((tri.shape[1] * 2, tri.shape[0] * 2), Image.NEAREST).save(os.path.join(png, '%s_%s_crop.png' % (name, v)))
    if not keep:
        for d_ in (old, new):
            for p in glob.glob(os.path.join(d_, '*.ppm')):
                os.remove(p)


if __name__ == '__main__':
    main()
