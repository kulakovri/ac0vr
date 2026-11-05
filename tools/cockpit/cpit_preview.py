"""cpit_preview.py IN.cpit OUT.png [W H] [yaw_deg pitch_deg] -- software-rendered preview of a cockpit file from the
pilot's eye (textured, depth-buffered, nearest sampling). Pure Python; slow but dependency-free."""
import math, struct, sys, zlib

path, outp = sys.argv[1], sys.argv[2]
W = int(sys.argv[3]) if len(sys.argv) > 3 else 480
H = int(sys.argv[4]) if len(sys.argv) > 4 else 360
yaw = math.radians(float(sys.argv[5])) if len(sys.argv) > 5 else 0.0
pitch = math.radians(float(sys.argv[6])) if len(sys.argv) > 6 else -15.0 * math.pi / 180
d = open(path, 'rb').read()
assert d[:8] == b'ACZCPIT1'
ntex, nmesh = struct.unpack_from('<II', d, 8); p = 16
tex = []
for _ in range(ntex):
    w, h = struct.unpack_from('<II', d, p); p += 8
    tex.append((w, h, d[p:p + w * h * 4])); p += w * h * 4
img = bytearray(b'\x30\x40\x58' * (W * H)); zb = [1e9] * (W * H)
f = 0.5 * W / math.tan(math.radians(45))               # 90 degree horizontal FOV
cy_, sy_, cp_, sp_ = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)


def view(x, y, z):                                       # world (eye frame) -> camera rotated by yaw, pitch
    x, z = cy_ * x - sy_ * z, sy_ * x + cy_ * z
    y, z = cp_ * y - sp_ * z, sp_ * y + cp_ * z
    return x, y, z


for _ in range(nmesh):
    name = d[p:p + 32]; p += 32
    ti, r, g, b, a, flags, ntri = struct.unpack_from('<I4fII', d, p); p += 28
    for t in range(ntri):
        vs = [struct.unpack_from('<8f', d, p + 32 * k) for k in range(3)]; p += 96
        pts = []
        for v in vs:
            x, y, z = view(v[0], v[1], v[2])
            if z > -0.02: break
            pts.append((W * 0.5 + f * x / -z, H * 0.5 - f * y / -z, -z, v[6], v[7]))
        if len(pts) < 3: continue
        (x0, y0, z0, u0, v0), (x1, y1, z1, u1, v1), (x2, y2, z2, u2, v2) = pts
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9: continue
        bx0, bx1 = max(0, int(min(x0, x1, x2))), min(W - 1, int(max(x0, x1, x2)) + 1)
        by0, by1 = max(0, int(min(y0, y1, y2))), min(H - 1, int(max(y0, y1, y2)) + 1)
        shade = 0.55 + 0.45 * abs(vs[0][4])                  # light from above
        for py in range(by0, by1 + 1):
            for px in range(bx0, bx1 + 1):
                qx, qy = px + 0.5, py + 0.5
                l1 = ((x2 - qx) * (y0 - qy) - (x0 - qx) * (y2 - qy)) / area
                l2 = ((x0 - qx) * (y1 - qy) - (x1 - qx) * (y0 - qy)) / area
                l0 = 1.0 - l1 - l2
                if l0 < 0 or l1 < 0 or l2 < 0: continue
                zz = l0 * z0 + l1 * z1 + l2 * z2
                k = py * W + px
                if zz >= zb[k]: continue
                cr, cg, cb = r, g, b
                if ti != 0xFFFFFFFF:
                    iz = l0 / z0 + l1 / z1 + l2 / z2
                    uu = (l0 * u0 / z0 + l1 * u1 / z1 + l2 * u2 / z2) / iz
                    vv = (l0 * v0 / z0 + l1 * v1 / z1 + l2 * v2 / z2) / iz
                    tw, th, tp = tex[ti]
                    sx = int((uu % 1.0) * tw) % tw; sy = int((vv % 1.0) * th) % th
                    o = (sy * tw + sx) * 4
                    if tp[o + 3] < 128: continue
                    cr, cg, cb = cr * tp[o] / 255, cg * tp[o + 1] / 255, cb * tp[o + 2] / 255
                zb[k] = zz
                img[3 * k:3 * k + 3] = bytes((min(255, int(cr * 255 * shade)), min(255, int(cg * 255 * shade)),
                                             min(255, int(cb * 255 * shade))))
rows = b''.join(b'\x00' + bytes(img[3 * W * y:3 * W * (y + 1)]) for y in range(H))
ck = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(outp, 'wb').write(b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                       + ck(b'IDAT', zlib.compress(rows, 6)) + ck(b'IEND', b''))
print(outp)
