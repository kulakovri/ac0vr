/* city_look.glsl -- the city buildings' detailed look (zero_props.c stage 3, ACZ_TERRAIN_CITY_LOOK, push-constant bit
   524288; bug #261, the user in the headset 2026-10-31 M6: "flying close the houses become plastic textured objects").
   Included by mesh.frag; every value world-fixed (the same in both eyes and in the in-between frames), every detail
   faded into its mean where it would be under ~3-4 pixels (no shimmer far out: the city then looks as before).

   Walls (kinds 1, 2): the wall's own light per pixel as before (0.50 + 0.40 x its facing to the sun), but
   - the foot: the vertex colour's linear 0.70 -> 1 over the whole wall is taken out and the darkening put where a
     street's shade and the ground's occlusion are, the lowest few metres (contact with the ground);
   - low sun (the mission's sun height): the sun reaches the lower floors less (the street canyon's shade);
   - windows laid out on the wall (whole bays between corner piers, floors up to the cornice), each glass a mix of the
     dark room and the sky's reflection (the game's fog colour) by the view angle, varied per window, with a light
     frame, a sill and the lintel's shadow; a cornice band under a flat roof, the eaves' shadow under a pitched one;
   - a faint weathering of the plaster (two world-fixed noises) and a per-building hue of the calm facade texel.
   Roofs (kinds 3 flat, 4 pitched): the photo's few texels magnified 10-40x drew smooth glossy blobs; near, the roof
   takes the photo's mean colour over the roof (its centre, at the mip level of the roof's size) with a little of the
   texel detail, a parapet and its inner shadow (flat), courses of tiles, ridge and eaves (pitched), and a faint
   surface noise. */

float cl_hash(vec3 p) {
    uvec3 q = uvec3(ivec3(floor(p)));
    uint n = q.x * 1597334673u ^ q.y * 3812015801u ^ q.z * 2798796415u;
    n = (n ^ (n >> 15)) * 2246822519u;
    n ^= n >> 13;
    return float(n >> 8) * (1.0 / 16777216.0);
}
float cl_noise(vec2 p, float s) {          /* value noise, seed s */
    vec2 i = floor(p), f = p - i;
    f = f * f * (3.0 - 2.0 * f);
    float a = cl_hash(vec3(i, s)), b = cl_hash(vec3(i + vec2(1.0, 0.0), s));
    float c = cl_hash(vec3(i + vec2(0.0, 1.0), s)), d = cl_hash(vec3(i + vec2(1.0, 1.0), s));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
/* a detail of period 1 in coordinate q: 1 where it is wide on screen, 0 where under ~3 pixels */
float cl_vis(vec2 q) { vec2 fw = fwidth(q); return 1.0 - smoothstep(0.18, 0.35, max(fw.x, fw.y)); }
/* an antialiased box [a, b] in x with filter width w (linear ramps of 2 w: as soft as smoothstep's at 1-2 pixels) */
float cl_box(float x, float a, float b, float w) {
    float iw = 0.5 / w;
    return clamp((x - a) * iw + 0.5, 0.0, 1.0) - clamp((x - b) * iw + 0.5, 0.0, 1.0);
}

/* rgb: the fragment's colour before the facade light (vertex colour x texel); t: the texel; uv: its coordinates;
   tmod / tadd: the texture function's factor and term (rgb = t x tmod + tadd); fog: the game's fog colour */
vec3 city_look(vec3 rgb, vec4 t, vec2 uv, vec3 tmod, vec3 tadd, vec3 fog, float lodb) {
    /* kind and seed (kind + seed / 256 interpolated: rounded, so no pixel flips to a neighbouring seed) */
    int ks = int(floor(v_fac.w * 256.0 + 0.5));
    int kind = ks >> 8;
    float sd = float(ks & 255) * (1.0 / 256.0);         /* the building's seed */
    float h = v_fac.z;                                  /* metres above the base (the highest ground under it) */
    /* (the building's numbers rounded to their steps: interpolation must not make them differ between pixels) */
    float top = floor(v_fac2.x * 16.0 + 0.5) * (1.0 / 16.0), bot = floor(v_fac2.y * 16.0 + 0.5) * (1.0 / 16.0);
    float hl = floor(v_fac2.z * 2.0 + 0.5) * 0.5, hw = floor(v_fac2.w * 2.0 + 0.5) * 0.5;
    bool pitched = bot >= 64.0;                         /* (the foot's code carries the pitched-roof flag) */
    if (pitched) bot -= 64.0;
    vec2 sunh = v_wrel.xy;
    float sy = clamp(v_wrel.z, 0.0, 1.0);
    float tanel = sy / max(sqrt(1.0 - sy * sy), 0.05);

    if (kind >= 3) {
        /* ---- roofs ---- */
        vec2 l = v_fac.xy;                                /* along the length / width, m */
        vec2 lx = dFdx(l), ly = dFdy(l);
        float fwl = max(length(abs(lx) + abs(ly)), 1e-4);   /* metres per pixel */
        /* near (a 6.25 m texel over ~3 pixels and more) the mean over the roof, far the photo exactly as before */
        float near = 1.0 - smoothstep(1.2, 2.4, fwl);
        if (near <= 0.0) return t.rgb * tmod + tadd;
        /* the roof's centre in the texture: uv is affine in l over the roof plane */
        mat2 A = mat2(lx, ly), B = mat2(dFdx(uv), dFdy(uv));
        float det = determinant(A);
        vec2 uvc = uv;
        if (abs(det) > 1e-12) uvc = uv - (B * inverse(A)) * l;
        float lod = clamp(log2(max(2.0 * min(hl, hw), 1.0) / 6.25) - 0.5, 0.0, 2.0);
        vec4 tc = SAMPLE_TL(uvc, lod + lodb);
        /* a roof material per building (gravel, bitumen, old tiles), faint, on the photo's own colour */
        float rm = fract(sd * 9.1 + 0.37);
        float tl = dot(tc.rgb, vec3(0.30, 0.55, 0.15));
        vec3 rc = rm < 0.25 ? mix(tc.rgb, vec3(tl * 1.18), 0.45) : rm < 0.40 ? tc.rgb * 0.82
                : rm < 0.55 ? tc.rgb * vec3(1.08, 0.97, 0.90) : tc.rgb;
        vec3 base = mix(t.rgb, mix(t.rgb, rc, 0.70), near) * tmod + tadd;
        /* the surface (0.67 m grain), where a grain spans ~3 pixels and more */
        float vs = 1.0 - smoothstep(0.18, 0.35, fwl * 1.5);
        float f = 1.0;
        if (vs > 0.0) f += vs * 0.18 * (cl_noise(l * 1.5, 5.0 + sd * 97.0) - 0.5);
        float ve = 1.0 - smoothstep(0.25, 0.6, fwl);    /* edges over ~2 pixels and more */
        float we = fwl * 0.75 + 1e-4;
        if (kind == 3) {
            float e = min(hl - abs(l.x), hw - abs(l.y));   /* to the roof's edge, m */
            float cope = 1.0 - smoothstep(0.30 - we, 0.30 + we, e);
            float inner = cl_box(e, 0.30, 1.10, we) * (1.0 - smoothstep(0.30, 1.10, e));
            f *= 1.0 + ve * (0.14 * cope - 0.22 * inner);
        } else {
            float across = hw - abs(l.y);                 /* up the slope from the eave (horizontal m) */
            float s = across / 0.30;                      /* courses of tiles */
            float ws = fwl / 0.30 + 1e-4;
            float vt = 1.0 - smoothstep(0.18, 0.35, ws);
            float fs = fract(s);
            float course = smoothstep(0.0, 0.16 + ws, fs) * (1.0 - 0.35 * smoothstep(0.55, 1.0, fs));
            f *= 1.0 - vt * 0.20 * (1.0 - course);
            f *= 1.0 - ve * (0.14 * (1.0 - smoothstep(0.0, 0.25 + we, abs(l.y)))      /* ridge cap */
                             + 0.22 * (1.0 - smoothstep(0.0, 0.25 + we, across)));   /* gutter */
        }
        return base * f;
    }

    /* ---- walls and gables ---- */
    vec3 fp = vec3(v_fac.x, v_wabs.y, v_fac.y);
    vec3 fpx = dFdx(fp), fpy = dFdy(fp);
    vec2 fnh = cross(fpx, fpy).xz;
    float nl = length(fnh);
    fnh = nl > 1e-12 ? fnh / nl : vec2(0.0);
    if (dot(fnh, v_fac.xy) < 0.0) fnh = -fnh;
    /* the vertex colour's foot (0.70 at the wall's foot -> 1 at its top) out */
    float K = mix(0.70, 1.0, clamp((h + bot) / max(top + bot, 0.1), 0.0, 1.0));
    vec3 base = rgb / K * 0.90;                         /* (x 0.90: the wall's mean as before, from afar the same city) */
    /* a calm hue per building */
    float hk = fract(sd * 3.7 + 0.13);
    vec3 hue = hk < 0.30 ? vec3(1.0) : hk < 0.48 ? vec3(1.05, 1.01, 0.93) : hk < 0.64 ? vec3(0.95, 0.98, 1.03)
             : hk < 0.80 ? vec3(1.06, 0.99, 0.88) : hk < 0.92 ? vec3(1.04, 0.97, 0.95) : vec3(1.12, 1.10, 1.06);
    base *= hue;
    /* the wall's coordinates: along it (m) from its centre, its half length; metres per pixel along and up */
    vec2 tdir = vec2(-fnh.y, fnh.x);
    float ta = dot(v_fac.xy, tdir) * (1.0 / 5.12), dn = dot(v_fac.xy, fnh) * (1.0 / 5.12);
    float ha = abs(dn - hl) < abs(dn - hw) ? hw : hl;
    float fta = (abs(dot(fpx.xz, tdir)) + abs(dot(fpy.xz, tdir))) * (1.0 / 5.12);
    float fth = (abs(fpx.y) + abs(fpy.y)) * (1.0 / 5.12) + 1e-4;
    /* light: the facing to the sun; a low sun reaches the lower floors less (the street's shade) */
    float sunf = max(dot(fnh, sunh), 0.0);
    float hs = clamp((2.5 + 6.0 * fract(sd * 5.3 + 0.41)) * clamp(0.30 / max(tanel, 0.05), 0.0, 1.0), 0.0, 0.6 * top);
    float sunvis = mix(0.45, 1.0, smoothstep(hs - 2.0, hs + 2.0, h));
    float light = 0.50 + 0.40 * sunf * sunvis;
    /* the ground: the street's shade and the occlusion in the lowest metres */
    float hp = max(h, 0.0);
    float ao = (1.0 - 0.36 * exp(-hp / 1.5)) * (1.0 - 0.08 * exp(-hp / 6.0));
    /* the cornice under a flat roof, the eaves' shadow under a pitched one */
    float vh = 1.0 - smoothstep(0.25, 0.6, fth);
    float cor = 1.0;
    if (h < top + 0.05 && vh > 0.0) {
        if (pitched) cor = 1.0 - vh * 0.30 * cl_box(h, top - 0.55, top + 0.1, fth);
        else cor = 1.0 + vh * (0.12 * cl_box(h, top - 0.40, top + 0.1, fth) - 0.16 * cl_box(h, top - 0.85, top - 0.40, fth));
    }
    /* plaster: a faint world-fixed noise (2 m blotches) where a blotch spans ~3 pixels and more */
    float wv = 1.0 - smoothstep(0.18, 0.35, 0.5 * max(fta, fth));
    float wf = 1.0;
    if (wv > 0.0) wf += wv * 0.12 * (cl_noise(vec2(ta, hp * 0.6) * 0.5, 3.0 + sd * 61.0) - 0.5);
    vec3 lit = base * light * ao;
    vec3 wall = lit * wf * cor;
    if (kind != 1) return wall;

    /* windows: whole bays between 0.7 m corner piers (world-fixed bays where the wall's length is unknown) and floors
       up to the cornice */
    float fh = 3.0 + 0.4 * fract(sd * 7.0);
    float nf = max(floor((top - 0.6) / fh), 1.0);
    float bw = 3.0 + 1.2 * sd;
    float len = 2.0 * ha - 1.4;
    float nb = ha > 0.4 ? max(floor(len / bw), 1.0) : 1e6;
    float bwr = ha > 0.4 ? max(len, 0.5) / nb : bw;
    vec2 q = vec2(ha > 0.4 ? (ta + ha - 0.7) / bwr : ta / bw, h / fh);
    vec2 fw = max(vec2(fta / bwr, fth / fh), vec2(1e-4));
    float vwin = 1.0 - smoothstep(0.18, 0.35, max(fw.x, fw.y));
    float inside = cl_box(q.x, 0.0, nb, fw.x) * cl_box(q.y, 0.0, nf, fw.y);
    float ww = 0.40 + 0.14 * fract(sd * 13.0);
    float x0 = 0.5 - 0.5 * ww, x1 = 0.5 + 0.5 * ww, y0 = 0.30, y1 = 0.80;
    /* far: the pattern's mean (the window area's share of the glass's mean) */
    float wa = ww * (y1 - y0) * clamp(nf / max(top / fh, 1.0), 0.6, 1.0);
    vec3 glass_mean = mix(lit * 0.30, fog * 0.9, 0.25);
    vec3 far_c = mix(wall, glass_mean, wa * 0.85 * inside);
    if (vwin <= 0.0) return far_c;

    vec2 cell = floor(q), fq = q - cell;
    float wallid = ha == hl ? 1.0 : 2.0;               /* (the long or the short walls: stable per pixel) */
    float r1 = cl_hash(vec3(cell.x + 17.0 * wallid, cell.y, sd * 256.0));
    float r2 = fract(r1 * 7.31 + 0.27);
    float win = cl_box(fq.x, x0, x1, fw.x) * cl_box(fq.y, y0, y1, fw.y) * inside;
    float frm = cl_box(fq.x, x0 - 0.05, x1 + 0.05, fw.x) * cl_box(fq.y, y0 - 0.04, y1 + 0.03, fw.y) * inside;
    float sill = cl_box(fq.x, x0 - 0.07, x1 + 0.07, fw.x) * cl_box(fq.y, y0 - 0.08, y0 - 0.04, fw.y) * inside;
    float lint = cl_box(fq.y, y1 - 0.12, y1, fw.y);
    /* weathering: a faint darker trace under each sill */
    float streak = cl_box(fq.x, x0 + 0.06, x1 - 0.06, fw.x) * inside
                 * (1.0 - smoothstep(0.0, 0.30, (y0 - 0.08) - fq.y)) * step(fq.y, y0 - 0.08);
    /* the glass: the dark room and the sky's reflection by the view angle (Schlick, n 1.5), varied per window */
    float vlen = length(v_view);
    float cosv = vlen > 1.0 ? abs(dot(v_view, vec3(fnh.x, 0.0, fnh.y))) / vlen : 0.5;
    float m5 = 1.0 - cosv, m2 = m5 * m5;
    float fres = 0.04 + 0.96 * m2 * m2 * m5;
    float refl = clamp((0.10 + 0.90 * fres) * (0.55 + 0.9 * r2), 0.0, 0.90);
    vec3 room = lit * (r1 < 0.16 ? 0.62 : 0.20 + 0.18 * r1);    /* some with light curtains */
    vec3 glass = mix(room, fog * (0.80 + 0.25 * r2) * (0.75 + 0.25 * ao), refl);
    glass *= 1.0 - 0.35 * lint * (0.5 + 0.5 * sunf);    /* the lintel's shadow in the recess */
    vec3 frame = lit * 1.16 * wf;
    vec3 near_c = mix(mix(wall * (1.0 - 0.07 * streak), wall * 1.14, sill), mix(frame, glass, win / max(frm, 1e-4)), frm);
    return mix(far_c, near_c, vwin);
}
