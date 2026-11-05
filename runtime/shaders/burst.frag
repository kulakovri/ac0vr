#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* A big burst as a volume (ps2_video.c bst_pass; ACZ_VR_FX_BURST, bug #266: M18, Pixy's MPBM burst "a very harsh
   gradient ... it does not look volumetric, it still looks 2D ... no beautiful explosion gradient").
   Each eye marches its own ray through the burst's sphere (centre and radius from the game's own burst, its age from
   the game's timing), so the volume has true parallax in stereo and the eye can be inside it. The medium: a thick
   shell whose outline is broken by turbulence (3D value-noise fBm moving outwards with the blast), emitting by its
   temperature through a black-body ramp -- white-hot core, yellow, orange, deep red -- and absorbing more as it cools
   into smoke; the heat falls with the radius and with age. Front-to-back integration with a per-pixel jitter of the
   first step (no banding), a soft limb (no hard edge), premultiplied over (the target's alpha is left alone). The
   game's halo colour (its burst cards) tints the outermost, thinnest part, so the MPBM keeps its pink rim. */
layout(location = 0) in vec3 v_pos;
layout(location = 0) out vec4 o_col;

layout(push_constant) uniform BP {
    vec4 cx, cy, cz;
    vec4 proj;          /* fx, fy, cx, cy (target pixels) */
    vec4 misc;
    vec4 eyes;          /* x left eye shift, y right eye shift (units along cx), z time (s), w units per metre */
    vec4 ctr;           /* xyz centre from the centre eye (units, world), w radius (units) */
    vec4 st;            /* x age / life, y strength, z seed, w quad half size */
    vec4 heat;          /* x core heat, y the game's glow strength (sqrt of its alpha: 1 full, 0 gone), z smoke share, w the
                           eye's distance to the centre / radius */
    vec4 tint;          /* rgb the game's halo colour, w its share */
    vec4 fogc;          /* rgb fog colour, w the fog's amount at the burst */
    vec4 veil;          /* x 1: set 0 is the effects' cloud veil (ps2_video.c fxs_*: r the scene's view depth in m, 0 =
                           nothing; g, b the view depths where our cloud starts and has reached its transmittance a), y z
                           the fade's transmittance range (ACZ_VR_CLOUD_FX_SOFT) */
    vec4 box;           /* the veil's valid box (target pixels x0, x1, y0, y1) */
} bp;
#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray veilt;
#else
layout(set = 0, binding = 0) uniform sampler2D veilt;
#endif

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float vnoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i), n100 = hash13(i + vec3(1, 0, 0)), n010 = hash13(i + vec3(0, 1, 0)),
          n110 = hash13(i + vec3(1, 1, 0)), n001 = hash13(i + vec3(0, 0, 1)), n101 = hash13(i + vec3(1, 0, 1)),
          n011 = hash13(i + vec3(0, 1, 1)), n111 = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}
float fbm(vec3 p) {                    /* (two octaves; the billows' detail comes from the third noise below) */
    return 0.667 * vnoise(p) + 0.333 * vnoise(p * 2.03 + 17.1);
}
/* black-body-like ramp, 0 cold smoke .. 1 white-hot: piecewise linear between the stops (no flat bands at the stops,
   as a smoothstep per segment would make) */
vec3 heat_col(float t) {
    const vec3 c0 = vec3(0.075, 0.066, 0.062);  /* smoke */
    const vec3 c1 = vec3(0.30, 0.055, 0.025);   /* dark red */
    const vec3 c2 = vec3(0.78, 0.17, 0.035);    /* deep red-orange */
    const vec3 c3 = vec3(1.00, 0.46, 0.10);     /* orange */
    const vec3 c4 = vec3(1.00, 0.76, 0.36);     /* yellow-orange */
    const vec3 c5 = vec3(1.00, 0.93, 0.74);     /* yellow-white */
    const vec3 c6 = vec3(1.00, 1.00, 0.98);     /* white-hot */
    t = clamp(t, 0.0, 1.0) * 6.0;
    if (t < 1.0) return mix(c0, c1, t);
    if (t < 2.0) return mix(c1, c2, t - 1.0);
    if (t < 3.0) return mix(c2, c3, t - 2.0);
    if (t < 4.0) return mix(c3, c4, t - 3.0);
    if (t < 5.0) return mix(c4, c5, t - 4.0);
    return mix(c5, c6, t - 5.0);
}

void main() {
    float sh = bp.eyes.x;
#ifdef MV
    sh = gl_ViewIndex == 0 ? bp.eyes.x : bp.eyes.y;
#endif
    vec3 dv = vec3((gl_FragCoord.x - bp.proj.z) / bp.proj.x, -(gl_FragCoord.y - bp.proj.w) / bp.proj.y, -1.0);
    vec3 D = normalize(dv.x * bp.cx.xyz + dv.y * bp.cy.xyz + dv.z * bp.cz.xyz);
    float R = bp.ctr.w;
    vec3 oc = (sh * bp.cx.xyz - bp.ctr.xyz) / R;        /* the eye in the sphere's units */
    float b = dot(oc, D), c = dot(oc, oc) - 1.0, disc = b * b - c;
    if (disc <= 0.0) discard;
    float s = sqrt(disc);
    float t0 = max(-b - s, 0.0), t1 = -b + s;
    if (t1 <= t0) discard;
    /* the effects' cloud veil where bound: the march ends at the scene (terrain, aircraft inside the burst) and each
       sample fades by our cloud's transmittance in front of it, as the game's effects do (gs.frag fxv) */
    vec4 vt = vec4(0.0, 0.0, 0.0, 1.0);
    bool hasv = false;
    if (bp.veil.x > 0.5 && gl_FragCoord.x >= bp.box.x && gl_FragCoord.x < bp.box.y && gl_FragCoord.y >= bp.box.z
        && gl_FragCoord.y < bp.box.w) {
#ifdef MV
        vt = texelFetch(veilt, ivec3(ivec2(gl_FragCoord.xy), gl_ViewIndex), 0);
#else
        vt = texelFetch(veilt, ivec2(gl_FragCoord.xy), 0);
#endif
        hasv = true;
    }
    float fz = max(dot(D, -bp.cz.xyz), 1e-4) * R / bp.eyes.w;   /* view depth (m) per unit of t */
    if (hasv && vt.r > 0.0) t1 = min(t1, vt.r / fz);
    if (t1 <= t0) discard;
    bool fog_cloud = hasv && vt.a < 0.999;
    float age = clamp(bp.st.x, 0.0, 1.0), seed = bp.st.z;
    float core = bp.heat.x, dis = 1.0 - clamp(bp.heat.y, 0.0, 1.0), smoke = bp.heat.z;
    /* the turbulence: billows a fifth to a third of the radius across, carried outwards with the blast (the noise's
       coordinates shrink as the sphere grows, so the lumps ride the expansion) and slowly rolling */
    const int N = 16;
    float dt = (t1 - t0) / float(N);
    float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));   /* (interleaved gradient) */
    float t = t0 + dt * jit;
    vec3 acc = vec3(0.0);
    float tr = 1.0;
    float sig = 6.0;                                    /* extinction at full density, per radius */
    vec3 sd = vec3(seed, seed * 0.71, seed * 1.37);
    float roll = bp.eyes.z * 0.05;
    for (int k = 0; k < N; k++, t += dt) {
        vec3 p = oc + D * t;
        float r = length(p);
        if (r > 1.0) continue;
        vec3 u = p / max(r, 1e-3);
        vec3 q = p * 2.4 + sd - u * (0.8 * age) + vec3(0.0, roll, 0.0);
        float n = fbm(q);
        float n2 = vnoise(q * 3.3 + 11.7);
        float bl = 0.78 * n + 0.22 * n2;                /* (0..1, mean ~0.5) */
        /* the outline: lumps pushed out to the sphere, soft over the last stretch (no hard edge) */
        float edge = 1.0 - 0.34 * (1.0 - bl);
        float dens = 1.0 - smoothstep(edge - 0.24, edge, r);
        /* cauliflower billows: the noise thresholded softly, fuller towards the middle */
        dens *= smoothstep(0.18, 0.62, bl + 0.32 * (1.0 - r));
        /* dissipating as the game's glow fades: the thin parts go first, the burst breaks into lumps and wisps */
        dens *= smoothstep(0.55 * dis, 0.55 * dis + 0.30, bl);
        /* the shell carries the blast outwards: the middle thins as it ages */
        dens *= mix(1.0, 0.30 + 0.70 * smoothstep(0.0, 0.7, r), smoothstep(0.15, 0.75, age));
        if (dens <= 0.002) continue;
        /* the temperature: hottest at the middle and in the billows' cores, cooling outwards and with age */
        float T = core * (1.18 - 0.92 * r * r) + 0.50 * (bl - 0.5);
        T = clamp(T, 0.0, 1.0);
        vec3 col = heat_col(T);
        float em = 0.45 + 3.0 * T * T;                  /* hotter glows brighter (white-hot saturates) */
        /* cool smoke is lit from inside by the fire behind it: a dim orange glow in it while the core is hot */
        col = max(col, heat_col(0.42) * (0.22 * core * (1.0 - T)));
        /* the thin outer rim towards the game's sprites' colour (the MPBM's peach-pink fringe) */
        float rim = smoothstep(0.62, 1.0, r) * (1.0 - smoothstep(0.0, 0.7, dens));
        col = mix(col, bp.tint.rgb, bp.tint.w * rim);
        float ext = sig * dens * (0.32 + 1.2 * smoke * (1.0 - T));   /* (hot gas glows more than it hides) */
        float a = 1.0 - exp(-ext * dt);
        if (fog_cloud) {
            float Tc = mix(1.0, vt.a, clamp((t * fz - vt.g) / max(vt.b - vt.g, 1e-3), 0.0, 1.0));
            a *= smoothstep(bp.veil.y, bp.veil.z, Tc);
        }
        acc += tr * a * col * em;
        tr *= 1.0 - a;
        if (tr < 0.01) break;
    }
    float alpha = (1.0 - tr) * bp.st.y;
    vec3 rgb = acc * bp.st.y;
    rgb = mix(rgb, bp.fogc.rgb * alpha, bp.fogc.w);
    /* dither the 8-bit target (no steps in the smooth parts) */
    float dn = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    rgb += dn / 255.0;
    o_col = vec4(max(rgb, vec3(0.0)), clamp(alpha, 0.0, 1.0));
}
