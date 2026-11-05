"""acz_disc.py: textures straight from the Ace Combat Zero disc image, decoded the way the runtime's GS decode does
(runtime/src/ps2_gs.c current_texture_inner), with the content hash the texture replacement packs are named by.

  python -I acz_disc.py ISO list MEMBER                 GIM / TIM2 pictures in a DATA.PAC member (recursively)
  python -I acz_disc.py ISO extract MEMBER PATH OUTDIR  one picture as OUTDIR/HHHHHHHH_PSM_WxH.png (RGBA, decode
                                                         convention: alpha 255 = GS 0x80) + _idx.png (indices)
  PATH: sub-file indices joined by dots, e.g. 0 or 0.1.0; a TIM2 inside a SWAD menu package is SWADSUB:SECTION:PIC,
  e.g. 0:8:0 (member sub-file 0 = the SWAD, section 8, picture 0).

The predicted hash assumes the game uploads the picture unchanged (indices as stored, the file's palette as the CLUT,
T8 palettes in the CSM1 order) and that the decode covers exactly the picture (TEX0 size = picture size). Pictures
that do not fill a power-of-two TEX0 size, or whose CLUT the game changes at run time, get other hashes: check with
PS2_TEX_HASHLOG=1 in a run.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pngio import write_png  # noqa: E402

M32 = 0xFFFFFFFF
PSM_CT32, PSM_T8, PSM_T4 = 0, 19, 20


def iso_files(iso):
    """{path: (lba, size)} of an ISO9660 image"""
    def sector(lba, n=1):
        iso.seek(lba * 2048)
        return iso.read(2048 * n)
    out = {}

    def walk(lba, size, path):
        data = sector(lba, (size + 2047) // 2048)
        pos = 0
        while pos < len(data):
            ln = data[pos]
            if ln == 0:
                pos = (pos // 2048 + 1) * 2048
                continue
            rec = data[pos:pos + ln]
            pos += ln
            elba, esz = struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]
            name = rec[33:33 + rec[32]]
            if name in (b'\x00', b'\x01'):
                continue
            nm = name.decode('latin1').split(';')[0]
            if rec[25] & 2:
                walk(elba, esz, path + nm + '/')
            else:
                out[path + nm] = (elba, esz)
    root = sector(16)[156:156 + 34]
    walk(struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0], '/')
    return out


class Pac:
    """DATA.PAC members: DATA.TBL = count + (offset, size) pairs"""
    def __init__(self, iso_path):
        self.f = open(iso_path, 'rb')
        files = iso_files(self.f)
        tbl_lba, tbl_size = files['/BIN/DATA.TBL']
        self.pac_lba = files['/BIN/DATA.PAC'][0]
        self.f.seek(tbl_lba * 2048)
        self.tbl = self.f.read(tbl_size)

    def member(self, i):
        off, size = struct.unpack('<II', self.tbl[8 + 8 * i:16 + 8 * i])
        self.f.seek(self.pac_lba * 2048 + off)
        return self.f.read(size)


def subfiles(blob):
    """count + offsets container -> [(index, bytes)], or [] when blob is not one"""
    if len(blob) < 8:
        return []
    cnt = struct.unpack('<I', blob[:4])[0]
    if not 0 < cnt < 300 or 4 + 4 * cnt > len(blob):
        return []
    offs = struct.unpack('<%dI' % cnt, blob[4:4 + 4 * cnt])
    if not any(offs) or not all(o == 0 or 4 + 4 * cnt <= o < len(blob) for o in offs):
        return []
    nz = [o for o in offs if o]
    res = []
    for k, o in enumerate(offs):
        if o:
            res.append((k, blob[o:min([x for x in nz if x > o] + [len(blob)])]))
    return res


def csm1(i):
    return (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)


class Picture:
    """psm (GS), w, h, idx (list or None), pal (logical order, raw 32-bit CLUT words) or px (raw CT32 words)"""
    def __init__(self, psm, w, h, idx=None, pal=None, px=None):
        self.psm, self.w, self.h, self.idx, self.pal, self.px = psm, w, h, idx, pal, px

    def decode_pal(self):
        """the runtime's pal[]: alpha 0x80 and above -> 255, else doubled"""
        out = []
        for c in self.pal:
            a = c >> 24
            a = 255 if a >= 128 else a * 2
            out.append((c & 0xFFFFFF) | (a << 24))
        return out

    def rgba(self):
        out = bytearray(self.w * self.h * 4)
        if self.idx is not None:
            pal = self.decode_pal()
            for i, k in enumerate(self.idx):
                out[4 * i:4 * i + 4] = struct.pack('<I', pal[k])
        else:
            for i, c in enumerate(self.px):   # CT32: texel_rgba keeps the alpha byte as is (ps2_gs.c texel_rgba)
                out[4 * i:4 * i + 4] = struct.pack('<I', c)
        return out

    def runtime_hash(self):
        """ps2_gs.c current_texture_inner: FNV over the decoded palette, then the texels row by row in runs of the
        PSM's block width, four at a time (rotate 13, xor, * 0x9E3779B1), the rest one at a time (FNV)"""
        h = 2166136261
        if self.idx is not None:
            pal = self.decode_pal()
            n = 16 if self.psm == PSM_T4 else 256
            for e in range(n):
                h = ((h ^ pal[e]) * 16777619) & M32
            bw = 32 if self.psm == PSM_T4 else 16
            for y in range(self.h):
                row = self.idx[y * self.w:(y + 1) * self.w]
                x = 0
                while x < self.w:
                    run = min(bw - (x % bw), self.w - x)
                    k = 0
                    while k + 4 <= run:
                        r = row[x + k:x + k + 4]
                        h = ((((h << 13) | (h >> 19)) & M32) ^ (r[0] | r[1] << 8 | r[2] << 16 | r[3] << 24))
                        h = (h * 0x9E3779B1) & M32
                        k += 4
                    while k < run:
                        h = ((h ^ row[x + k]) * 16777619) & M32
                        k += 1
                    x += run
        else:
            for c in self.px:
                h = ((h ^ c) * 16777619) & M32
        return h

    def name(self):
        return '%08X_%d_%dx%d' % (self.runtime_hash(), self.psm, self.w, self.h)


def gim(blob):
    """Namco 'GIM\\0' texture (in-flight HUD and mission data)"""
    psm = blob[0x13]
    w, h = struct.unpack('<HH', blob[0x1C:0x20])
    px = blob[0x20:]
    if psm == 0x14:
        n = w * h // 2
        pal = [struct.unpack('<I', px[n + 4 * i:n + 4 * i + 4])[0] for i in range(16)]
        idx = []
        for b in px[:n]:
            idx += [b & 15, b >> 4]
        return Picture(PSM_T4, w, h, idx=idx, pal=pal)
    if psm == 0x13:   # the palette is stored in logical order (de-swizzling it garbles the replay bar's text)
        n = w * h
        pal = [struct.unpack('<I', px[n + 4 * i:n + 4 * i + 4])[0] for i in range(256)]
        return Picture(PSM_T8, w, h, idx=list(px[:n]), pal=pal)
    if psm == 0x00:
        return Picture(PSM_CT32, w, h, px=list(struct.unpack('<%dI' % (w * h), px[:4 * w * h])))
    return None


def tim2(blob):
    """TIM2 pictures (menu SWAD sections), 32-bit CLUTs only"""
    n = struct.unpack('<H', blob[6:8])[0]
    p = 16
    res = []
    for _ in range(n):
        tot, clsz, imsz, hsz, ncol = struct.unpack('<IIIHH', blob[p:p + 16])
        pf, mips, ctype, itype, w, h = struct.unpack('<BBBBHH', blob[p + 16:p + 24])
        img = blob[p + hsz:p + hsz + imsz]
        clut = blob[p + hsz + imsz:p + hsz + imsz + clsz]
        pic = None
        if (ctype & 0x3F) == 3 and itype in (4, 5):
            pal = [struct.unpack('<I', clut[4 * i:4 * i + 4])[0] for i in range(ncol)]
            if itype == 4:
                idx = []
                for b in img[:w * h // 2]:
                    idx += [b & 15, b >> 4]
                pic = Picture(PSM_T4, w, h, idx=idx, pal=(pal + [0] * 16)[:16])
            else:
                if not (ctype & 0x80):
                    pal = [pal[csm1(i)] for i in range(256)]
                pic = Picture(PSM_T8, w, h, idx=list(img[:w * h]), pal=(pal + [0] * 256)[:256])
        elif itype == 3:
            pic = Picture(PSM_CT32, w, h, px=list(struct.unpack('<%dI' % (w * h), img[:4 * w * h])))
        res.append(pic)
        p += tot
    return res


def walk(blob, path, out):
    """every picture under blob: [(path, Picture)]"""
    if blob[:4] == b'GIM\x00':
        out.append((path, gim(blob)))
        return
    if blob[:4] == b'SWAD':
        n = struct.unpack('<I', blob[4:8])[0]
        for i in range(n):
            o, sz = struct.unpack('<II', blob[8 + 8 * i:16 + 8 * i])
            if blob[o:o + 4] == b'TIM2':
                for k, pic in enumerate(tim2(blob[o:o + sz])):
                    out.append(('%s:%d:%d' % (path, i, k), pic))
        return
    for k, sub in subfiles(blob):
        walk(sub, '%s.%d' % (path, k) if path else '%d' % k, out)


def find(pac, member, path):
    out = []
    walk(pac.member(member), '', out)
    for p, pic in out:
        if p == path:
            return pic
    raise KeyError('m%d %s not found (%s)' % (member, path, ', '.join(p for p, _ in out)))


def save(pic, outdir, label=''):
    os.makedirs(outdir, exist_ok=True)
    nm = os.path.join(outdir, pic.name() + (('_' + label) if label else '') + '.png')
    write_png(nm, pic.w, pic.h, pic.rgba())
    if pic.idx is not None:
        g = bytearray(pic.w * pic.h * 4)
        mul = 17 if pic.psm == PSM_T4 else 1
        for i, k in enumerate(pic.idx):
            g[4 * i:4 * i + 4] = bytes((k * mul, k * mul, k * mul, 255))
        write_png(nm[:-4] + '_idx.png', pic.w, pic.h, g)
    return nm


def main():
    pac = Pac(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'list':
        out = []
        walk(pac.member(int(sys.argv[3])), '', out)
        for p, pic in out:
            if pic is None:
                print('%-12s (format not decoded)' % p)
                continue
            extra = ''
            if pic.idx is not None:
                used = sorted(set(pic.idx))
                extra = ' %d colours used' % len(used)
            print('%-12s psm %2d %4dx%-4d %s%s' % (p, pic.psm, pic.w, pic.h, pic.name(), extra))
    elif cmd == 'extract':
        pic = find(pac, int(sys.argv[3]), sys.argv[4])
        print(save(pic, sys.argv[5], sys.argv[6] if len(sys.argv) > 6 else ''))
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
