"""make_glyphs.py ROOT [--scale N] [--iso ISO] [--ps-scale N] [--hud-atlas FILE] [--sheet OUT.png] [--touch]: draw
the Xbox (and with --touch the Quest Touch) button glyph sets (our own art) for the menu help bars, as texture
replacements named after the PlayStation glyph textures they stand for.

  ROOT/xbox/          A, B, X, Y, View, LB, RB  (active while an Xbox-type pad is the active pad)
  ROOT/xbox_modern/   LT, RT for the game's L1 / R1 images (modern layout: throttle on the triggers)
  ROOT/ps_modern/     (--iso) the game's L1 / R1 images with the 1 redrawn as a 2 (PlayStation pad, modern layout),
                      upscaled to --ps-scale (default 2, like the menu set) with xBRZ back-projected to the 1x edit
  ROOT/xbox/          (--hud-atlas FILE, the hud set's atlas) the HUD atlas with the font's square and triangle
                      symbols (in-flight text: the replay's "square DISPLAY") redrawn as circled X and Y
  --touch: ROOT/touch/ (active while the Quest Touch controllers are the active controller, ctl_priority 2026-11-03):
                      A, B, X, Y as Touch's dark buttons with white letters, View as the left thumbstick's press (its
                      View in AS A PAD), L1 / R1 as L / R GRIP (classic layout: the grips are the bumpers),
                      the replay bar (GRIP) and the HUD atlas (the circled X and Y: Touch has those letters too);
           ROOT/touch_modern/ L1 / R1 as L / R TRIG (modern: throttle on the triggers), the replay bar (TRIG); the
                      words small beside the side's letter in the pill of the game's L1 / R1 box.

The game's glyphs are 32x32 PSMT4 TIM2 pictures, one copy per menu (DATA.PAC menu SWADs); their runtime content
hashes are computed from the disc (acz_disc.py) when --iso is given, else the table below (computed the same way)
is used. --scale draws at N x 32 px (1 = the original size; the runtime samples larger replacements in texel space).
Art: filled circles in the Xbox colours with the letter (Segoe UI Bold outlines, rasterized here), the View icon on
a dark button, LB/RB/LT/RT as grey pills like the game's L1/R1 boxes; sizes and positions follow the originals
(face symbols occupy x 9..21, y 8..22; the L1/R1 boxes x 5..26, y 9..22).
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import canvas as C  # noqa: E402
from pngio import write_png  # noqa: E402
from ttfraster import Font  # noqa: E402

# PlayStation glyph textures: label -> (DATA.PAC member, picture path, runtime name)
PS_GLYPHS = {
    'cross':        (1241, '0:8:0',   'A688AB64_20_32x32'),
    'circle':       (1246, '0:14:0',  '18588EB2_20_32x32'),
    'triangle':     (1241, '0:9:0',   'ABCE6B7A_20_32x32'),
    'square':       (1241, '1:11:0',  'EADCA67B_20_32x32'),
    'square_small': (1252, '0:18:0',  '3D20F85B_20_32x32'),
    'select':       (1252, '0:19:0',  '97E0F01E_20_32x32'),
    'L1':           (1256, '0:185:0', 'A5BEF74C_20_32x32'),
    'R1':           (1256, '0:184:0', 'B69CE355_20_32x32'),
}

FONT = 'C:/Windows/Fonts/segoeuib.ttf'
FACE = {   # Xbox face buttons: fill colour, letter colour (decode convention: the menus draw them unmodulated)
    'A': ((0x4C, 0xAE, 0x3E), (0xF6, 0xF6, 0xF6)),
    'B': ((0xDE, 0x46, 0x42), (0xF6, 0xF6, 0xF6)),
    'X': ((0x3E, 0x78, 0xD8), (0xF6, 0xF6, 0xF6)),
    'Y': ((0xEA, 0xBC, 0x2E), (0x30, 0x2A, 0x1C)),
}
HALO = (0x3F, 0x3D, 0x2D)          # the soft dark disc behind the game's own symbols
PILL_EDGE = (0x26, 0x26, 0x27)     # the game's L1/R1 box: dark edge, grey fill, light grey text
PILL_FILL = (0x47, 0x45, 0x44)
PILL_TEXT = (0xA0, 0xA0, 0xA0)


def text_mask(font, s, text, cap_px, cx, cy, ss):
    """a string centred on (cx, cy) whose capital height is cap_px (all in 32-px units, scaled by s)"""
    x0, y0, x1, y1 = font.text_box('H', 100.0)
    size = cap_px / ((y1 - y0) / 100.0)
    bx0, by0, bx1, by1 = font.text_box(text, size * s)
    tw, th = bx1 - bx0, by1 - by0
    cs, _ = font.text_contours(text, size * s, cx * s - tw / 2 - bx0, cy * s - th / 2 - by0)
    return C.polys(32 * s, 32 * s, cs, ss)


def face(font, s, letter, ss):
    W = 32 * s
    img = C.Image(W, W)
    cx = cy = 15.5
    img.paint(C.circle(W, W, cx * s, cy * s, 7.4 * s, ss), HALO, 0.55)
    fill, ink = FACE[letter]
    img.paint(C.circle(W, W, cx * s, cy * s, 6.6 * s, ss), fill)
    img.paint(text_mask(font, s, letter, 7.4, cx, cy + 0.15, ss), ink)
    return img


def view(font, s, ss):
    """the View button: two overlapping rounded rectangles on a dark button"""
    W = 32 * s
    img = C.Image(W, W)
    cx = cy = 15.5
    img.paint(C.circle(W, W, cx * s, cy * s, 7.4 * s, ss), HALO, 0.55)
    img.paint(C.circle(W, W, cx * s, cy * s, 6.6 * s, ss), (0x3A, 0x3A, 0x3C))
    ink = (0xE6, 0xE6, 0xE6)
    back_o = C.rrect(W, W, 11.2 * s, 11.6 * s, 17.4 * s, 16.6 * s, 0.9 * s, ss)
    back_i = C.rrect(W, W, 12.3 * s, 12.7 * s, 16.3 * s, 15.5 * s, 0.3 * s, ss)
    front = C.rrect(W, W, 13.8 * s, 14.4 * s, 20.0 * s, 19.4 * s, 0.9 * s, ss)
    gap = C.rrect(W, W, 12.8 * s, 13.4 * s, 21.0 * s, 20.4 * s, 1.2 * s, ss)
    img.paint(C.sub(C.sub(back_o, back_i), gap), ink)
    img.paint(front, ink)
    return img


def menu_icon(font, s, ss):
    """the Menu button (three lines): drawn for completeness, the menus have no Start glyph to replace"""
    W = 32 * s
    img = C.Image(W, W)
    cx = cy = 15.5
    img.paint(C.circle(W, W, cx * s, cy * s, 7.4 * s, ss), HALO, 0.55)
    img.paint(C.circle(W, W, cx * s, cy * s, 6.6 * s, ss), (0x3A, 0x3A, 0x3C))
    for y in (12.6, 15.5, 18.4):
        img.paint(C.rrect(W, W, 11.9 * s, (y - 0.75) * s, 19.1 * s, (y + 0.75) * s, 0.5 * s, ss), (0xE6, 0xE6, 0xE6))
    return img


def pill(font, s, text, ss):
    W = 32 * s
    img = C.Image(W, W)
    x0, y0, x1, y1 = 5.0, 9.0, 27.0, 23.0
    r = (y1 - y0) / 2
    img.paint(C.rrect(W, W, x0 * s, y0 * s, x1 * s, y1 * s, r * s, ss), PILL_EDGE)
    img.paint(C.rrect(W, W, (x0 + 1) * s, (y0 + 1) * s, (x1 - 1) * s, (y1 - 1) * s, (r - 1) * s, ss), PILL_FILL)
    img.paint(text_mask(font, s, text, 7.6, (x0 + x1) / 2, (y0 + y1) / 2, ss), PILL_TEXT)
    return img


# a pixel "2" in the style of the game's "1" (1-px strokes, soft edges), coverage 0..1 on 4 x 8 texels
TWO = [
    (0.55, 1.0, 1.0, 0.55),
    (1.0, 0.25, 0.25, 1.0),
    (0.0, 0.0, 0.3, 1.0),
    (0.0, 0.0, 0.75, 0.7),
    (0.0, 0.55, 0.9, 0.15),
    (0.45, 0.9, 0.25, 0.0),
    (1.0, 0.45, 0.0, 0.0),
    (1.0, 1.0, 1.0, 1.0),
]


def ps_trigger(pic):
    """the game's own L1 / R1 image with the 1 replaced by a 2 (PlayStation pad, modern layout: the game's L1/R1
    are on the L2/R2 triggers). 1x, in the original's colours."""
    px = pic.rgba()
    bg = px[(14 * 32 + 8) * 4:(14 * 32 + 8) * 4 + 4]          # the box interior
    ink = (0x98, 0x98, 0x98)
    for y in range(11, 21):
        for x in range(16, 22):
            px[(y * 32 + x) * 4:(y * 32 + x) * 4 + 4] = bg
    for j, row in enumerate(TWO):
        for i, c in enumerate(row):
            k = ((12 + j) * 32 + 17 + i) * 4
            for ch in range(3):
                px[k + ch] = int(round(bg[ch] + (ink[ch] - bg[ch]) * c))
    return px


# Quest Touch: its buttons are dark with white letters (Touch Plus); a light rim keeps them apart from the dark halo
TOUCH_BTN = (0x2C, 0x2E, 0x31)
TOUCH_RIM = (0x9C, 0x9F, 0xA4)
TOUCH_INK = (0xF2, 0xF2, 0xF2)
TOUCH_CAP = (0x5E, 0x61, 0x66)


def face_touch(font, s, letter, ss):
    W = 32 * s
    img = C.Image(W, W)
    cx = cy = 15.5
    img.paint(C.circle(W, W, cx * s, cy * s, 7.4 * s, ss), HALO, 0.55)
    img.paint(C.circle(W, W, cx * s, cy * s, 6.6 * s, ss), TOUCH_RIM)
    img.paint(C.circle(W, W, cx * s, cy * s, 5.9 * s, ss), TOUCH_BTN)
    img.paint(text_mask(font, s, letter, 6.8, cx, cy + 0.15, ss), TOUCH_INK)
    return img


def stick_press(font, s, ss):
    """the left thumbstick pressed (Touch's View in AS A PAD): the stick's cap from above in its well, a dot"""
    W = 32 * s
    img = C.Image(W, W)
    cx = cy = 15.5
    img.paint(C.circle(W, W, cx * s, cy * s, 7.4 * s, ss), HALO, 0.55)
    img.paint(C.circle(W, W, cx * s, cy * s, 6.6 * s, ss), TOUCH_RIM)
    img.paint(C.circle(W, W, cx * s, cy * s, 5.9 * s, ss), TOUCH_BTN)
    img.paint(C.circle(W, W, cx * s, cy * s, 4.4 * s, ss), (0xB8, 0xBB, 0xC0))
    img.paint(C.circle(W, W, cx * s, cy * s, 3.7 * s, ss), TOUCH_CAP)
    img.paint(C.circle(W, W, cx * s, cy * s, 1.2 * s, ss), TOUCH_INK)
    return img


def touch_pill(font, s, side, word, ss):
    """the game's L1 / R1 box (a pill) with L or R and the Touch control's name beside it, smaller: GRIP (classic: the
    grips are the bumpers) or TRIG (modern: the triggers). A silhouette of the controller was tried first: at this size
    it read as a letter ("L7", "LT")."""
    W = 32 * s
    img = C.Image(W, W)
    x0, y0, x1, y1 = 5.0, 9.0, 27.0, 23.0
    r = (y1 - y0) / 2
    img.paint(C.rrect(W, W, x0 * s, y0 * s, x1 * s, y1 * s, r * s, ss), PILL_EDGE)
    img.paint(C.rrect(W, W, (x0 + 1) * s, (y0 + 1) * s, (x1 - 1) * s, (y1 - 1) * s, (r - 1) * s, ss), PILL_FILL)
    img.paint(text_mask(font, s, side, 6.6, 9.0, 16.0, ss), PILL_TEXT)
    img.paint(text_mask(font, s, word, 4.0, 18.5, 16.0, ss), PILL_TEXT)
    return img


def replay_bar(font, pic, shoulder, ss=8, touch=False):
    """the in-flight replay help bar (512x64 T8: "x OK | R1 FAST-FORWARD | square DISPLAY OFF") with Xbox glyphs
    drawn into its glyph boxes: A for cross, X for square, RB/RT for R1 (touch: Touch's dark buttons, GRIP / TRIG).
    1x, the rest of the picture untouched."""
    W, H = pic.w, pic.h
    px = pic.rgba()
    img = C.Image(W, H)
    for i in range(W * H):
        img.px[i] = [px[4 * i] / 255.0, px[4 * i + 1] / 255.0, px[4 * i + 2] / 255.0, px[4 * i + 3] / 255.0]
    boxes = {'cross': (26, 27, 38, 39, (0x39, 0x39, 0x39)), 'square': (242, 27, 253, 39, (0x2D, 0x2D, 0x2E)),
             'R1': (81, 27, 100, 38, (0x3B, 0x3B, 0x3B))}
    for x0, y0, x1, y1, dark in boxes.values():
        img.paint(C.rrect(W, H, x0, y0, x1 + 1, y1 + 1, 0.0, 2), dark)
    for key, letter in (('cross', 'A'), ('square', 'X')):
        x0, y0, x1, y1, _ = boxes[key]
        cx, cy = (x0 + x1 + 1) / 2, (y0 + y1 + 1) / 2
        fill, ink = (TOUCH_BTN, TOUCH_INK) if touch else FACE[letter]
        if touch:
            img.paint(C.circle(W, H, cx, cy, 6.2, ss), TOUCH_RIM)
        img.paint(C.circle(W, H, cx, cy, 5.7, ss), fill)
        bx0, by0, bx1, by1 = font.text_box('H', 100.0)
        size = 6.2 / ((by1 - by0) / 100.0)
        tx0, ty0, tx1, ty1 = font.text_box(letter, size)
        cs, _ = font.text_contours(letter, size, cx - (tx1 - tx0) / 2 - tx0, cy + 0.1 - (ty1 - ty0) / 2 - ty0)
        img.paint(C.polys(W, H, cs, ss), ink)
    x0, y0, x1, y1, _ = boxes['R1']
    cx, cy = (x0 + x1 + 1) / 2, (y0 + y1 + 1) / 2
    bx0, by0, bx1, by1 = font.text_box('H', 100.0)
    size = (5.4 if len(shoulder) > 2 else 7.0) / ((by1 - by0) / 100.0)
    tx0, ty0, tx1, ty1 = font.text_box(shoulder, size)
    cs, _ = font.text_contours(shoulder, size, cx - (tx1 - tx0) / 2 - tx0, cy - (ty1 - ty0) / 2 - ty0)
    img.paint(C.polys(W, H, cs, ss), (0x9C, 0x9C, 0x9C))
    return img.rgba8()


# the HUD font's PlayStation symbols (in-flight text such as the replay's "square DISPLAY"): character cells of the
# HUD atlas 3F861D67_20_256x256 (coverage, one grey, tinted by the game), inclusive texel boxes at 1x
HUD_ATLAS = '3F861D67_20_256x256'
HUD_CELLS = {'square': ((187, 222, 195, 231), 'X'), 'triangle': ((176, 222, 185, 231), 'Y')}


def hud_atlas_xbox(font, px, w, h):
    """the HUD set's atlas (any multiple of 256) with the font's square and triangle cells redrawn as circled X and Y
    (the Xbox face buttons in the same positions), in the atlas's own grey and stroke weight (1 texel)"""
    s = w // 256
    col = None
    for i in range(w * h):
        if px[4 * i + 3]:
            col = px[4 * i:4 * i + 3]
            break
    out = bytearray(px)
    for (x0, y0, x1, y1), letter in HUD_CELLS.values():
        cw, ch = (x1 - x0 + 1) * s, (y1 - y0 + 1) * s
        cx, cy, r = cw / 2.0, ch / 2.0, min(cw, ch) / 2.0
        ring = C.sub(C.circle(cw, ch, cx, cy, r, 4), C.circle(cw, ch, cx, cy, r - s, 4))
        bx0, by0, bx1, by1 = font.text_box('H', 100.0)
        size = 0.52 * ch / ((by1 - by0) / 100.0)
        tx0, ty0, tx1, ty1 = font.text_box(letter, size)
        cs, _ = font.text_contours(letter, size, cx - (tx1 - tx0) / 2 - tx0, cy - (ty1 - ty0) / 2 - ty0)
        mask = C.union(ring, C.polys(cw, ch, cs, 4))
        for y in range(ch):
            for x in range(cw):
                k = ((y0 * s + y) * w + x0 * s + x) * 4
                out[k:k + 3] = col
                out[k + 3] = int(round(min(1.0, mask[y * cw + x]) * 255))
    return out


def names(iso):
    if not iso:
        return {k: v[2] for k, v in PS_GLYPHS.items()}
    import acz_disc
    pac = acz_disc.Pac(iso)
    out = {}
    for k, (m, p, nm) in PS_GLYPHS.items():
        real = acz_disc.find(pac, m, p).name()
        if real != nm:
            print('note: %s is %s on this disc (table: %s)' % (k, real, nm))
        out[k] = real
    return out


def main():
    root = sys.argv[1]
    s = int(sys.argv[sys.argv.index('--scale') + 1]) if '--scale' in sys.argv else 1
    iso = sys.argv[sys.argv.index('--iso') + 1] if '--iso' in sys.argv else None
    sheet = sys.argv[sys.argv.index('--sheet') + 1] if '--sheet' in sys.argv else None
    ps_scale = int(sys.argv[sys.argv.index('--ps-scale') + 1]) if '--ps-scale' in sys.argv else 2
    hud = sys.argv[sys.argv.index('--hud-atlas') + 1] if '--hud-atlas' in sys.argv else None
    touch = '--touch' in sys.argv
    ss = max(2, 16 // s)
    font = Font(FONT)
    nm = names(iso)
    jobs = [
        ('xbox', 'cross', 'A', lambda: face(font, s, 'A', ss)),
        ('xbox', 'circle', 'B', lambda: face(font, s, 'B', ss)),
        ('xbox', 'square', 'X', lambda: face(font, s, 'X', ss)),
        ('xbox', 'square_small', 'X', lambda: face(font, s, 'X', ss)),
        ('xbox', 'triangle', 'Y', lambda: face(font, s, 'Y', ss)),
        ('xbox', 'select', 'View', lambda: view(font, s, ss)),
        ('xbox', 'L1', 'LB', lambda: pill(font, s, 'LB', ss)),
        ('xbox', 'R1', 'RB', lambda: pill(font, s, 'RB', ss)),
        ('xbox_modern', 'L1', 'LT', lambda: pill(font, s, 'LT', ss)),
        ('xbox_modern', 'R1', 'RT', lambda: pill(font, s, 'RT', ss)),
    ]
    if touch:
        jobs += [
            ('touch', 'cross', 'A', lambda: face_touch(font, s, 'A', ss)),
            ('touch', 'circle', 'B', lambda: face_touch(font, s, 'B', ss)),
            ('touch', 'square', 'X', lambda: face_touch(font, s, 'X', ss)),
            ('touch', 'square_small', 'X', lambda: face_touch(font, s, 'X', ss)),
            ('touch', 'triangle', 'Y', lambda: face_touch(font, s, 'Y', ss)),
            ('touch', 'select', 'StickPress', lambda: stick_press(font, s, ss)),
            ('touch', 'L1', 'LGrip', lambda: touch_pill(font, s, 'L', 'GRIP', ss)),
            ('touch', 'R1', 'RGrip', lambda: touch_pill(font, s, 'R', 'GRIP', ss)),
            ('touch_modern', 'L1', 'LTrigger', lambda: touch_pill(font, s, 'L', 'TRIG', ss)),
            ('touch_modern', 'R1', 'RTrigger', lambda: touch_pill(font, s, 'R', 'TRIG', ss)),
        ]
    tiles = []
    for setname, ps, label, draw in jobs:
        d = os.path.join(root, setname)
        os.makedirs(d, exist_ok=True)
        img = draw()
        px = img.rgba8()
        fn = os.path.join(d, '%s_%s.png' % (nm[ps], label))
        write_png(fn, img.w, img.h, px)
        tiles.append(px)
        print(fn)
    if iso:
        import acz_disc
        pac = acz_disc.Pac(iso)
        d = os.path.join(root, 'ps_modern')
        os.makedirs(d, exist_ok=True)
        for ps, label in (('L1', 'L2'), ('R1', 'R2')):
            pic = acz_disc.find(pac, PS_GLYPHS[ps][0], PS_GLYPHS[ps][1])
            fn = os.path.join(d, '%s_%s.png' % (nm[ps], label))
            px = ps_trigger(pic)
            if ps_scale > 1:
                # as sharp as the menu set's own 2x images next to it: xBRZ, back-projected to the 1x edit
                from upscale_hud import run_xbrz, backproject_rgba
                px = backproject_rgba(32, 32, px, run_xbrz(32, 32, px, ps_scale, 'argb'), ps_scale, 2)
            write_png(fn, 32 * ps_scale, 32 * ps_scale, px)
            print(fn)
        bar = acz_disc.find(pac, 1529, '4')           # in-flight replay help bar (every mission package)
        bars = [('xbox', 'RB', False), ('xbox_modern', 'RT', False)]
        if touch:
            bars += [('touch', 'GRIP', True), ('touch_modern', 'TRIG', True)]
        for setname, shoulder, tch in bars:
            fn = os.path.join(root, setname, '%s_replay_bar_%s.png' % (bar.name(), shoulder))
            write_png(fn, bar.w, bar.h, replay_bar(font, bar, shoulder, touch=tch))
            print(fn)
    else:
        print('no --iso: the ps_modern set (L2/R2 drawn over the game\'s own L1/R1 images) is skipped')
    if hud:
        from pngio import read_png
        w, h, px = read_png(hud)
        atlas = hud_atlas_xbox(font, px, w, h)
        for setname in ('xbox', 'touch') if touch else ('xbox',):     # (the circled X and Y: Touch has them too)
            fn = os.path.join(root, setname, '%s_hud_atlas_%s.png' % (HUD_ATLAS, setname))
            write_png(fn, w, h, atlas)
            print(fn)
    if sheet:
        tiles.append(menu_icon(font, s, ss).rgba8())
        W = 32 * s
        n = len(tiles)
        out = bytearray(W * n * W * 4)
        for t, px in enumerate(tiles):
            for y in range(W):
                for x in range(W):
                    si = (y * W + x) * 4
                    di = (y * W * n + t * W + x) * 4
                    g = 0x18 if ((x * 4 // W) ^ (y * 4 // W)) & 1 else 0x28
                    a = px[si + 3]
                    for c in range(3):
                        out[di + c] = (px[si + c] * a + g * (255 - a)) // 255
                    out[di + 3] = 255
        write_png(sheet, W * n, W, out)
        print(sheet)


if __name__ == '__main__':
    main()
