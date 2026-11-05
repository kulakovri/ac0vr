"""make_hud_pack.py DUMPDIR HASHLOG ROOT [--method sharp|xbrz|esrgan] [--scale 4]: the in-flight HUD textures, taken
from a mission run with PS2_TEX_DUMP=DUMPDIR PS2_TEX_HASHLOG=1 (log HASHLOG), upscaled into ROOT/hud/.

The game builds the HUD's CLUT at run time (an alpha ramp different from the one in the disc's GIM files), so the
pack is made from the textures as the runtime decodes them, not from disc data. Taken:
  - the HUD atlas (font, digits, symbols, reticles, markers; 256x256 PSMT4, VRAM slot "2276" in earlier dumps)
  - the own-aircraft damage silhouette and the weapon icon (small PSMT8 pictures)
  - the radio-subtitle glyph cache: 32x32 PSMT4 textures, one character each, in the VRAM range the hash log shows
    for them (every character the dump run displayed; a longer run covers more of the alphabet)
The atlas is a coverage image (one colour, alpha = coverage), the radio glyphs grey text with a dark shadow (grey
level and alpha sharpened as two coverage maps), the two icons pictures (xBRZ); upscale_hud.py keeps colour and alpha
range exactly ("sharp": bilinear coverage through a contrast curve, back-projected to the original texel averages;
the xBRZ output is back-projected too, so every texel keeps its colour and coverage).
xBRZ and realesr-animevideov3 were compared on the atlas and change the pixel font (xBRZ rounds it into lens shapes,
the AI model thickens the strokes); they stay available as --method.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from pngio import read_png  # noqa: E402
from upscale_hud import coverage_colour, grey_ramp  # noqa: E402
HUD_FIXED = {
    '3F861D67_20_256x256': 'hud_atlas',
    'E71758F5_19_48x48': 'damage_silhouette',
    '83D19D4A_19_32x32': 'weapon_icon',
}
GLYPH_TBP = (2359296, 2383872)          # radio glyph cache in VRAM (mission 1 log: tbp 2359552..2379520)


def main():
    dump, log, root = sys.argv[1], sys.argv[2], sys.argv[3]
    method = sys.argv[sys.argv.index('--method') + 1] if '--method' in sys.argv else 'sharp'
    scale = sys.argv[sys.argv.index('--scale') + 1] if '--scale' in sys.argv else '4'
    want = dict(HUD_FIXED)
    for line in open(log, errors='replace'):
        m = re.search(r'texhash: ([0-9A-F]{8}_20_32x32) tbp (\d+)', line)
        if m and GLYPH_TBP[0] <= int(m.group(2)) < GLYPH_TBP[1]:
            want.setdefault(m.group(1), 'radio_glyph')
    out = os.path.join(root, 'hud')
    os.makedirs(out, exist_ok=True)
    n = 0
    for name, label in sorted(want.items()):
        src = os.path.join(dump, name + '.png')
        if not os.path.exists(src):
            print('not in the dump: %s (%s)' % (name, label))
            continue
        w, h, px = read_png(src)
        # coverage atlases and grey text with a shadow (the radio glyphs): --method; pictures: xBRZ (flat pixel art),
        # back-projected to the original texels
        m = method if coverage_colour(w, h, px) is not None or grey_ramp(w, h, px) else 'xbrz'
        subprocess.run([sys.executable, '-I', os.path.join(HERE, 'upscale_hud.py'), src,
                        os.path.join(out, '%s_%s.png' % (name, label)), '--method', m, '--scale', scale],
                       check=True, stdout=subprocess.DEVNULL)
        n += 1
    print('%d HUD textures -> %s' % (n, out))


if __name__ == '__main__':
    main()
