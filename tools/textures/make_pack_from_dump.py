"""make_pack_from_dump.py DUMPDIR [DUMPDIR ...] OUTSET [--scale 2] [--method auto|xbrz|sharp|esrgan] [--min 16]
[--only LIST] [--skip LIST] [--pictures skip|xbrz|esrgan] [--force] [--list]

Build a replacement set from PS2_TEX_DUMP folders (HHHHHHHH_PSM_WxH.png, one per decoded texture, real runtime
hashes): every texture at least --min texels on both sides is upscaled into OUTSET/ under the same name. Several dump
folders (one per menu-tour run) can be given; a texture already in OUTSET is kept unless --force (so a later tour
only adds what it saw new). --list only prints each texture's class.
auto classes:
  blank     one colour everywhere (cleared VRAM, empty glyph slots): skipped, a larger copy adds nothing
  coverage  one colour, alpha = coverage (text, fonts, line art) -> "sharp" (exact colour and alpha range kept,
            back-projected)
  text      greys with alpha (font glyphs with a dark shadow: a grey ramp from transparent black to the white core;
            glyph cells, or larger font atlases with flat-art edges) -> "sharp" on the grey level and the alpha
            separately
  flat      flat UI art: at most 32 colours whose neighbouring texels mostly change in hard steps (frames, icons,
            glyph images, logos) -> xBRZ, back-projected so every texel keeps its average colour and coverage
  picture   everything else: photographic backgrounds, clouds, gradients, dithered panels (even as 16-colour PSMT4,
            where a colour count alone does not tell them from icons) -> --pictures skip (default: left as they
            are, an upscaler only changes their look), xbrz or esrgan (realesr-animevideov3, GPU)
--only / --skip take comma-separated hash prefixes.
--log LOG[:FIELD][,LOG2[:FIELD2]...] with --tbp A[-B][,...]: take only textures the runs' PS2_TEX_HASHLOG logs show
decoded at one of those VRAM addresses (byte offsets as logged) up to that field (a tour that ends in flight: the
field the mission starts, so terrain, aircraft and HUD textures stay out of a menu set). Menus: --tbp
3145728,2359296-2434048 (the SWAD menu images, the font glyph cache).
--exclude SETDIR[,SETDIR...]: leave out textures another set already replaces (e.g. textures/hud).
Run it on dumps made while going through the menus, e.g. (from the tree, game runs only through run_test.sh):
  LIGHT=1 SCALE=1 MISSION= PAD="..." run_test.sh TREE menus PS2_XR=0 PS2_TEX_DUMP=out/texdump_menus PS2_TEX_HASHLOG=1
  make_pack_from_dump.py out/texdump_menus textures/menu --log zero_cp_menus.log --tbp 3145728,2359296-2434048
      --exclude textures/hud
then check_pack.py textures/menu out/texdump_menus (every replacement must average back to its original).
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pngio import read_png  # noqa: E402
from upscale_hud import coverage_colour, grey_ramp  # noqa: E402

HARD_STEP = 48          # a neighbouring texel pair differing by this much (any channel, alpha included) is an edge


def edge_stats(w, h, px):
    """over neighbouring texel pairs that are not both transparent and differ: the share of hard steps and the mean
    step. Flat art changes colour in few, large steps; photographs, clouds and gradients in many small ones."""
    diff = hard = total = 0
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            i = row + x * 4
            for j in ((i + 4) if x + 1 < w else -1, (i + w * 4) if y + 1 < h else -1):
                if j < 0 or (px[i + 3] == 0 and px[j + 3] == 0):
                    continue
                d = max(abs(px[i] - px[j]), abs(px[i + 1] - px[j + 1]), abs(px[i + 2] - px[j + 2]),
                        abs(px[i + 3] - px[j + 3]))
                if d:
                    diff += 1
                    total += d
                    if d >= HARD_STEP:
                        hard += 1
    return (hard / diff if diff else 0.0), (total / diff if diff else 0.0)


def classify(w, h, px):
    colours = set(bytes(px[i:i + 4]) for i in range(0, len(px), 4))
    if len(colours) <= 1:
        return 'blank', ''
    if coverage_colour(w, h, px) is not None:
        return 'coverage', ''
    hard, step = edge_stats(w, h, px)
    info = '%d colours, hard edges %.2f, mean step %.0f' % (len(colours), hard, step)
    flat = len(colours) <= 32 and hard >= 0.2 and step >= 40
    if grey_ramp(w, h, px) and (flat or (w <= 64 and h <= 64 and hard >= 0.2)):
        return 'text', info
    if flat:
        return 'flat', info
    return 'picture', info


def logged(logs, tbps):
    """names (HHHHHHHH_P_WxH) decoded at one of the tbp ranges up to each log's field limit"""
    keep = set()
    for spec in logs.split(','):
        path, lim = spec, None
        m = re.match(r'^(.*):(\d+)$', spec)
        if m and not re.match(r'^[A-Za-z]$', m.group(1)):
            path, lim = m.group(1), int(m.group(2))
        for line in open(path, errors='replace'):
            m = re.search(r'texhash: ([0-9A-F]{8}_\d+_\d+x\d+) tbp (\d+) .*?field (\d+)', line)
            if not m or (lim is not None and int(m.group(3)) > lim):
                continue
            t = int(m.group(2))
            if any(lo <= t <= hi for lo, hi in tbps):
                keep.add(m.group(1))
    return keep


def main():
    a = sys.argv
    opts = ('--scale', '--method', '--min', '--only', '--skip', '--pictures', '--log', '--tbp', '--exclude')
    pos = []
    k = 1
    while k < len(a):
        if a[k] in opts:
            k += 2
            continue
        if not a[k].startswith('--'):
            pos.append(a[k])
        k += 1
    srcs, out = pos[:-1], pos[-1]
    scale = a[a.index('--scale') + 1] if '--scale' in a else '2'
    method = a[a.index('--method') + 1] if '--method' in a else 'auto'
    mn = int(a[a.index('--min') + 1]) if '--min' in a else 16
    only = a[a.index('--only') + 1].upper().split(',') if '--only' in a else None
    skip = a[a.index('--skip') + 1].upper().split(',') if '--skip' in a else []
    pictures = a[a.index('--pictures') + 1] if '--pictures' in a else 'skip'
    force, listing = '--force' in a, '--list' in a
    keep = None
    if '--log' in a:
        tbps = []
        for r in (a[a.index('--tbp') + 1] if '--tbp' in a else '0-4294967295').split(','):
            lo, _, hi = r.partition('-')
            tbps.append((int(lo), int(hi or lo)))
        keep = logged(a[a.index('--log') + 1], tbps)
    other = set()
    if '--exclude' in a:
        for d in a[a.index('--exclude') + 1].split(','):
            for f in os.listdir(d):
                m = re.match(r'^([0-9A-Fa-f]{8}_\d+_\d+x\d+)[_.]', f)
                if m:
                    other.add(m.group(1).upper().replace('X', 'x'))
    if not listing:
        os.makedirs(out, exist_ok=True)
    n = kept = 0
    seen = set()
    counts = {}
    for src in srcs:
        for fn in sorted(os.listdir(src)):
            if not fn.lower().endswith('.png') or len(fn) < 9 or fn[8] != '_' or fn in seen:
                continue
            seen.add(fn)
            hx = fn[:8].upper()
            if only and not any(hx.startswith(p) for p in only):
                continue
            if any(hx.startswith(p) for p in skip if p):
                continue
            if (keep is not None and fn[:-4] not in keep) or fn[:-4] in other:
                continue
            path = os.path.join(src, fn)
            w, h, px = read_png(path)
            if w < mn or h < mn:
                continue
            cls, info = classify(w, h, px)
            counts[cls] = counts.get(cls, 0) + 1
            m = method
            if m == 'auto':
                m = {'blank': None, 'coverage': 'sharp', 'text': 'sharp', 'flat': 'xbrz'}.get(
                    cls, None if pictures == 'skip' else pictures)
            if listing:
                print('%-28s %-9s %-6s %s' % (fn, cls, m or '-', info))
                continue
            if m is None:
                continue
            dst = os.path.join(out, fn)
            if os.path.exists(dst) and not force:
                kept += 1
                continue
            subprocess.run([sys.executable, '-I', os.path.join(HERE, 'upscale_hud.py'), path, dst,
                            '--method', m, '--scale', scale], check=True)
            n += 1
    print('classes: %s' % ', '.join('%s %d' % kv for kv in sorted(counts.items())))
    if not listing:
        print('%d textures -> %s (%d already there, kept)' % (n, out, kept))


if __name__ == '__main__':
    main()
