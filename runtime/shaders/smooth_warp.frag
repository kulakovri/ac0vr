#version 450
#extension GL_EXT_multiview : require
/* Layered in-between frames (ps2_video.c lw_*, ACZ_VR_SMOOTH_MODE=layered; perf4_smooth 2026-11-03): one full-screen
   pass per in-between frame writes slot 0's colour and depth from the last game frame's world layer (lw.c: slot 0
   right before the native cockpit was drawn), moved by the same camera motion the re-render gives a GS world vertex
   (gs.vert reproject(): direction at unit depth d, p = R d + t iw), per eye and per pixel from its depth.
   A source pixel s of eye view v with frame depth z: iw = 1 / view depth = dp.x z + dp.y (the frame's Z mapping, as
   xr_depth.frag; z <= 0 the sky, z >= sp.y the saturated 2D / screen passes: at infinity, iw = 0); the eye's shift
   sh = sg mh iw (view 0 +, view 1 -: gs.vert's -xr_dx / +xr_dx) is taken off before the reprojection of the centre
   view and put back after it (the game frame's shift, as gs.vert keeps xr_dx). iw above dp.w (the near rule,
   PS2_XR_REPROJ_NEAR_COCKPIT): not moved. The pass needs the inverse -- for a target pixel p the source s with
   fwd(s) = p --: a fixed-point search s(k+1) = p - (fwd(s(k)) - s(k)) from s(0) = p, sp.z steps; of its last two
   steps the one that lands within half a pixel of p, else the farther one (where a near edge moved away the game frame
   never saw what it uncovers: the background next to it is stretched over the gap). Colour sampled there (sp.w: 0
   nearest, 1 bilinear, 2 Catmull-Rom), the source's own depth written (the next draws test against it as against the
   game frame's). Everything is clamped to the copied rectangle (area, target pixels).
   Moving objects (M, lw_movers: native objects matched to themselves one game frame back whose own motion differs from
   the world's): a pixel inside a mover's reach also searches with that object's own motion (q' = N q + c on
   q = (x w, y w, w), the re-render's extrapolation of a matched object, xr_host_apply); a converged source inside the
   object's game-frame box and depth range is a candidate, the nearest candidate wins. The static solution that lands on
   a mover's own pixels is its ghost (the object moved as if it were world): it is replaced by the background beside it
   (steps from the ghost away from where the object went).
   CPC = 1 (ACZ_VR_SMOOTH_LAYER_COCKPIT, perf5_reproj): the game frame's resolved native cockpit (ck: premultiplied, the
   background clear) over the warped world, in this pass: each cockpit pixel moved by the eye's move since the game
   frame's model pass (Movers.cpc[0].xyz, view metres; the head's orientation and the projection are the frame's) at
   its own depth (ckz: the frame depth after the model, linear: 1 - 0.0002 per metre) -- the same fixed-point search;
   pixels without model depth (the glass, the background, an edge's partly covered pixels) and the combiner's mark
   stay. Colour: ck + (1 - ck.a) world, as the composite blends it (RGB; the world's alpha kept); depth: the moved
   model depth, the mark where it was, else the world's. */
layout(set = 0, binding = 0) uniform sampler2DArray src;    /* the world layer's colour (linear, clamped) */
layout(set = 1, binding = 0) uniform sampler2DArray srcz;   /* its depth (point) */
layout(set = 3, binding = 0) uniform sampler2DArray ck;     /* CPC: the cockpit layer (premultiplied; linear, clamped) */
layout(set = 4, binding = 0) uniform sampler2DArray ckz;    /* CPC: the frame depth after the model (point) */
layout(constant_id = 0) const int CPC = 0;

layout(push_constant) uniform PC {
    vec4 rp[4];    /* rows of R (xyz) + cx, cy, fx; t (xyz) + fy: GS units, as gs.vert's pc.rp */
    vec4 dp;       /* x, y: iw = x z + y; z: mh (eye shift = mh iw, GS px); w: iw above which a pixel is held */
    vec4 sp;       /* x: target px per GS px; y: z at / above which a pixel is at infinity; z: steps; w: filter */
    vec4 area;     /* the copied rectangle: x0, y0, x1, y1 (target px) */
} pc;

/* per mover j (GS px; 6 vec4): r0..r2 the rows of N + source box x0, y0, x1; r3 c + source box y1; r4 the box of target
   pixels it may reach; r5 its view depth range (w lo, hi) and the hole step (GS px) */
/* cpc[0]: the eye's move (view m) + 1 when the cockpit layer is on; cpc[1]: the cockpit's fx, fy, cx, cy (target px) */
layout(set = 2, binding = 0) uniform Movers { vec4 hdr; vec4 mv[48]; vec4 cpc[2]; } M;

layout(location = 0) out vec4 o;

float iw_of(float z) {
    if (z <= 0.0 || z >= pc.sp.y) return 0.0;
    return max(pc.dp.x * z + pc.dp.y, 0.0);
}

/* where the world point at source pixel s (target px) with inverse depth iw is seen in the in-between frame */
vec2 fwd(vec2 s, float iw, float sg) {
    if (iw > pc.dp.w) return s;
    float k = pc.sp.x;
    vec2 g = s / k;
    float sh = sg * pc.dp.z * iw;
    vec3 d = vec3((g.x - sh - pc.rp[0].w) / pc.rp[2].w, -(g.y - pc.rp[1].w) / pc.rp[3].w, -1.0);
    vec3 p = vec3(dot(pc.rp[0].xyz, d), dot(pc.rp[1].xyz, d), dot(pc.rp[2].xyz, d)) + pc.rp[3].xyz * iw;
    float pz = -p.z;
    if (pz < 1e-4) return s;
    return vec2(pc.rp[0].w + pc.rp[2].w * p.x / pz + sh, pc.rp[1].w - pc.rp[3].w * p.y / pz) * k;
}

/* the in-between position of source pixel s (target px) on mover j */
vec2 fwdm(int j, vec2 s, float iw, float sg) {
    if (iw <= 0.0) return s;
    float k = pc.sp.x, w = 1.0 / iw;
    vec2 g = s / k;
    float sh = sg * pc.dp.z * iw;
    vec3 q = vec3((g.x - sh) * w, g.y * w, w);
    vec4 r0 = M.mv[6 * j], r1 = M.mv[6 * j + 1], r2 = M.mv[6 * j + 2], r3 = M.mv[6 * j + 3];
    vec3 q2 = vec3(dot(r0.xyz, q), dot(r1.xyz, q), dot(r2.xyz, q)) + r3.xyz;
    if (q2.z < 1e-3) return s;
    return vec2(q2.x / q2.z + sh, q2.y / q2.z) * k;
}
/* source pixel s with inverse depth iw is one of mover j's own pixels in the game frame */
bool on_mover(int j, vec2 s, float iw, float sg) {
    if (iw <= 0.0) return false;
    float w = 1.0 / iw;
    vec2 g = s / pc.sp.x;
    g.x -= sg * pc.dp.z * iw;
    vec4 r5 = M.mv[6 * j + 5];
    return g.x >= M.mv[6 * j].w && g.y >= M.mv[6 * j + 1].w && g.x <= M.mv[6 * j + 2].w && g.y <= M.mv[6 * j + 3].w
        && w >= r5.x && w <= r5.y;
}

float zat(vec2 c) { return texelFetch(srcz, ivec3(ivec2(c), int(gl_ViewIndex)), 0).r; }

vec4 tap(sampler2DArray t, vec2 c, vec2 lo, vec2 hi, vec2 inv) {
    return textureLod(t, vec3(clamp(c, lo, hi) * inv, float(gl_ViewIndex)), 0.0);
}

vec4 colour(sampler2DArray t, vec2 s, vec2 lo, vec2 hi) {
    int f = int(pc.sp.w);
    if (f == 0) return texelFetch(t, ivec3(ivec2(s), int(gl_ViewIndex)), 0);
    vec2 inv = 1.0 / vec2(textureSize(t, 0).xy);
    if (f == 1) return tap(t, s, lo, hi, inv);
    /* Catmull-Rom in 9 bilinear taps (the middle two texels of each axis share one tap) */
    vec2 tc = s - 0.5, t1 = floor(tc), fr = tc - t1;
    vec2 w0 = fr * (-0.5 + fr * (1.0 - 0.5 * fr));
    vec2 w1 = 1.0 + fr * fr * (-2.5 + 1.5 * fr);
    vec2 w2 = fr * (0.5 + fr * (2.0 - 1.5 * fr));
    vec2 w3 = fr * fr * (-0.5 + 0.5 * fr);
    vec2 w12 = w1 + w2;
    vec2 p0 = t1 - 0.5, p3 = t1 + 2.5, p12 = t1 + 0.5 + w2 / w12;
    vec4 r = tap(t, vec2(p0.x, p0.y), lo, hi, inv) * (w0.x * w0.y) + tap(t, vec2(p12.x, p0.y), lo, hi, inv) * (w12.x * w0.y)
           + tap(t, vec2(p3.x, p0.y), lo, hi, inv) * (w3.x * w0.y)
           + tap(t, vec2(p0.x, p12.y), lo, hi, inv) * (w0.x * w12.y) + tap(t, vec2(p12.x, p12.y), lo, hi, inv) * (w12.x * w12.y)
           + tap(t, vec2(p3.x, p12.y), lo, hi, inv) * (w3.x * w12.y)
           + tap(t, vec2(p0.x, p3.y), lo, hi, inv) * (w0.x * w3.y) + tap(t, vec2(p12.x, p3.y), lo, hi, inv) * (w12.x * w3.y)
           + tap(t, vec2(p3.x, p3.y), lo, hi, inv) * (w3.x * w3.y);
    return clamp(r, 0.0, 1.0);
}

/* CPC: the frame depth after the model at c (target px) */
float ckzat(vec2 c) { return texelFetch(ckz, ivec3(ivec2(c), int(gl_ViewIndex)), 0).r; }
/* CPC: where the cockpit point at source pixel s with frame depth z is seen from the moved eye; pixels without model
   depth (z below the model's linear range 1 - 0.0002 / m, or a saturated 2D value) stay */
vec2 ckfwd(vec2 s, float z) {
    if (!(z >= 0.998 && z < pc.sp.y)) return s;
    vec4 kp = M.cpc[1];
    float w = (1.0 - z) / 0.0002;
    vec3 cv = vec3((s.x - kp.z) * w / kp.x, -(s.y - kp.w) * w / kp.y, -w) - M.cpc[0].xyz;
    float w1 = -cv.z;
    if (w1 < 1e-3) return s;
    return vec2(kp.z + kp.x * cv.x / w1, kp.w - kp.y * cv.y / w1);
}

void main() {
    float sg = gl_ViewIndex == 0 ? 1.0 : -1.0;
    vec2 p = gl_FragCoord.xy, lo = pc.area.xy + 0.5, hi = pc.area.zw - 0.5;
    int n = int(pc.sp.z) & 15, seed2 = int(pc.sp.z) >> 4;
    vec2 q = p, q0 = p;
    for (int k = 0; k < n; k++) {
        vec2 c = clamp(q, lo, hi);
        vec2 f = fwd(c, iw_of(zat(c)), sg) - c;
        q0 = q;
        q = p - f;
    }
    vec2 a = clamp(q, lo, hi), b = clamp(q0, lo, hi);
    float za = zat(a), zb = zat(b), ia = iw_of(za), ib = iw_of(zb);
    float ra = length(fwd(a, ia, sg) - p), rb = length(fwd(b, ib, sg) - p);
    bool pa = ra <= 0.5 || (rb > 0.5 && ia <= ib);
    vec2 s = pa ? a : b;
    float zs = pa ? za : zb, is = pa ? ia : ib;
    if (seed2 > 0) {
        /* a near edge moving over the background (forward flight: the outer edges of near things) has two sources and
           the search from p finds the background's: a second search from the nearest depth met back along the
           translation's flow at p (seed2 samples, 0.5 .. 5 GS px), kept when it lands within half a pixel and is
           nearer */
        vec2 ft = fwd(p, 1e-3, sg) - fwd(p, 0.0, sg);
        float fl = length(ft);
        if (fl > 1e-6) {
            vec2 dir = ft / fl;
            float zn = zs; vec2 cn = s;
            for (int k = 1; k <= seed2; k++) {
                float dk = pc.sp.x * 0.5 * pow(10.0, float(k - 1) / max(float(seed2 - 1), 1.0));
                vec2 c = clamp(p - dir * dk, lo, hi);
                float zc = zat(c);
                if (zc > zn) { zn = zc; cn = c; }
            }
            if (zn > zs) {
                vec2 q2 = p - (fwd(cn, iw_of(zn), sg) - cn);
                for (int k = 0; k < 2; k++) {
                    vec2 c = clamp(q2, lo, hi);
                    q2 = p - (fwd(c, iw_of(zat(c)), sg) - c);
                }
                vec2 c = clamp(q2, lo, hi);
                float zc = zat(c), ic = iw_of(zc);
                if (zc > zs && length(fwd(c, ic, sg) - p) <= 0.5) { s = c; zs = zc; is = ic; }
            }
        }
    }
    int nm = int(M.hdr.x);
    if (nm > 0) {
        vec2 g = p / pc.sp.x;
        int gh = -1;                                   /* the static solution is mover gh's ghost */
        vec2 sm = s; float zm = -1.0;
        for (int j = 0; j < nm; j++) {
            vec4 tb = M.mv[6 * j + 4];
            if (g.x < tb.x || g.y < tb.y || g.x > tb.z || g.y > tb.w) continue;
            if (gh < 0 && on_mover(j, s, is, sg)) gh = j;
            vec2 qm = p;
            for (int k = 0; k < n; k++) {
                vec2 c = clamp(qm, lo, hi);
                qm = p - (fwdm(j, c, iw_of(zat(c)), sg) - c);
            }
            vec2 c = clamp(qm, lo, hi);
            float zc = zat(c), ic = iw_of(zc);
            if (zc > zm && on_mover(j, c, ic, sg) && length(fwdm(j, c, ic, sg) - p) <= 0.75) { sm = c; zm = zc; }
        }
        if (zm >= 0.0 && (gh >= 0 || zm >= zs)) { s = sm; zs = zm; }
        else if (gh >= 0) {                            /* the ghost: the background beside it */
            vec2 st = M.mv[6 * gh + 5].zw * pc.sp.x;
            for (int k = 1; k <= 4; k++) {
                vec2 c = clamp(s - st * float(k), lo, hi);
                float zc = zat(c);
                if (!on_mover(gh, c, iw_of(zc), sg)) { s = c; zs = zc; break; }
            }
        }
    }
    o = colour(src, s, lo, hi);
    gl_FragDepth = zs;
    if (CPC != 0 && M.cpc[0].w > 0.5) {
        /* the cockpit layer: source sc with ckfwd(sc) = p (the search as above, from p) */
        vec2 qc = p, qc0 = p;
        for (int k = 0; k < 3; k++) {
            vec2 c = clamp(qc, lo, hi);
            qc0 = qc;
            qc = p - (ckfwd(c, ckzat(c)) - c);
        }
        vec2 ca = clamp(qc, lo, hi), cb = clamp(qc0, lo, hi);
        float za = ckzat(ca), zb = ckzat(cb);
        vec2 fa = ckfwd(ca, za), fb = ckfwd(cb, zb);
        bool ta = length(fa - p) <= 0.5 || (length(fb - p) > 0.5 && za <= zb);   /* (else the farther: what it uncovers) */
        vec2 sc = ta ? ca : cb;
        float zc = ta ? za : zb;
        /* a move under 0.35 px keeps the pixel as drawn (the panel's fine text stays sharp; resampling it every other
           frame softens it at 60 Hz), a larger one resamples */
        vec4 kc = length(sc - p) < 0.35 ? texelFetch(ck, ivec3(ivec2(p), int(gl_ViewIndex)), 0) : colour(ck, sc, lo, hi);
        o.rgb = kc.rgb + (1.0 - kc.a) * o.rgb;
        float zp = ckzat(clamp(p, lo, hi));
        if (zc >= 0.998 && zc < pc.sp.y) {                      /* a model pixel: its depth from the moved eye */
            float w = (1.0 - zc) / 0.0002, w1 = w + M.cpc[0].z;
            gl_FragDepth = max(zs, 1.0 - 0.0002 * w1);
        } else if (zp > 0.99 && zp < 0.998) gl_FragDepth = max(zs, zp);   /* the combiner's mark stays */
    }
}
