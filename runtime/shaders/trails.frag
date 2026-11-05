#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* Contrails and wing vapour (ps2_video.c trl_pass): soft, premultiplied, lit like the game's own cloud impostors --
   base = mix(DARK, LIGHT, t), t = 0.578 + 0.429 cos(view, sun) (the game's palette curve: bright looking towards the
   sun, the dark rim away from it) -- dimmed under our clouds' shadow (the cloud shadow map's transmittance, per
   vertex), and fogged with the game's fog colour and distances. The noise lives on the strip (along = metres of trail
   since it started, across = -1..1): it stays with the air and is the same in both eyes.
   The cloud wake (ACZ_VR_CLOUD_WAKE; kinds 4 and 5, ps2_video.c trl_pass): the cloud dragged out of a cloud (4: a soft
   lumpy veil, lumps about its own width, eaten from the thin parts outward as it evaporates: the threshold the noise must
   clear rises with age / life) and the vortex-core threads (5: a thin beaded core that frays into beads late in life);
   v_b.z = age / life, v_b.w = the width (m). */
layout(location = 0) in vec4 v_a;        /* opacity, along, across, kind */
layout(location = 1) in vec4 v_b;        /* seed, T, ex.y, ex.z */
layout(location = 2) in vec4 v_dir;      /* view vector (game units), ex.w */
layout(location = 0) out vec4 o_col;

layout(push_constant) uniform TP {
    vec4 cx, cy, cz;
    vec4 proj;
    vec4 misc;
    vec4 eyes;          /* z time (s), w game units per metre */
    vec4 sun;           /* xyz towards the sun, w contrail strength */
    vec4 lit;           /* rgb lit, w vapour strength */
    vec4 dark;          /* rgb dark, w haze strength */
    vec4 fogc;          /* rgb fog colour, w 1 = fog on */
    vec4 fogd;          /* near, far (m) */
} tp;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash12(i), b = hash12(i + vec2(1.0, 0.0)), c = hash12(i + vec2(0.0, 1.0)), d = hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
float fbm(vec2 p) {
    return 0.55 * vnoise(p) + 0.30 * vnoise(p * 2.03 + 7.1) + 0.15 * vnoise(p * 4.01 + 3.7);
}

void main() {
    float a = v_a.x, along = v_a.y, across = v_a.z, kind = v_a.w, seed = v_b.x, T = clamp(v_b.y, 0.0, 1.0);
    float dens;
    if (kind > 3.5) {
        float x = abs(across), lf = clamp(v_b.z, 0.0, 1.0), wmm = max(v_b.w, 0.5);
        float t = tp.eyes.z;
        if (kind < 4.5) {
            vec2 q = vec2(along / max(0.7 * wmm, 4.0), across * 1.2);
            float n = fbm(q + vec2(seed, seed * 0.37) + vec2(t * 0.02, 0.0));
            float n2 = vnoise(q * 2.7 + vec2(seed * 2.3, t * 0.05));
            n = 0.75 * n + 0.25 * n2;
            /* ragged edges: the half width breathes with a slower noise along the trail (lumps of the rolled-up oval) */
            float ew = 0.62 + 0.5 * vnoise(vec2(along / max(1.6 * wmm, 8.0) + seed * 3.1, 2.5));
            float prof = 1.0 - smoothstep(0.05, 1.0, x / ew);
            prof *= prof * (3.0 - 2.0 * prof);
            float er = mix(0.34, 0.70, lf);                                   /* evaporation eats the thin parts first */
            dens = prof * smoothstep(er - 0.2, er + 0.16, n);
        } else {
            float prof = exp(-3.5 * x * x) * (1.0 - smoothstep(0.75, 1.0, x));
            float n = fbm(vec2(along * 0.11 + seed, across * 0.8 + seed * 0.5));
            float er = mix(0.15, 0.62, lf * lf);                              /* late in life: beads */
            dens = prof * smoothstep(er - 0.15, er + 0.2, n);
        }
    } else if (kind < 0.5) {
        /* a contrail: a soft core across, broken up a little along (the vortex pair's puffs, the spreading) */
        float x = abs(across);
        float prof = exp(-2.6 * x * x) * (1.0 - smoothstep(0.72, 1.0, x));
        float n = fbm(vec2(along * 0.045 + seed, across * 1.4 + seed * 0.37));
        float n2 = vnoise(vec2(along * 0.012 + seed * 1.7, 0.5));
        dens = prof * mix(0.62, 1.0, n) * mix(0.8, 1.0, n2);
    } else {
        /* a vapour puff: the silhouette of a flat ellipsoid over the wing (the quad spans it: a unit disc), churning */
        vec2 q = vec2(along, across);
        float r = length(q * vec2(1.0, 1.0));
        float prof = 1.0 - smoothstep(0.25, 1.0, r);
        float t = tp.eyes.z;
        float n = fbm(q * vec2(1.3, 2.2) + vec2(seed * 3.1 - t * 1.7, seed));
        dens = pow(prof, 1.5) * mix(0.40, 1.0, n);
    }
    float al = clamp(a * dens, 0.0, 1.0);
    if (al < 0.002) discard;
    float d = length(v_dir.xyz);
    vec3 vd = v_dir.xyz / max(d, 1e-3);
    vec3 col;
    {
        float t = 0.578;
        if (dot(tp.sun.xyz, tp.sun.xyz) > 0.25) t = clamp(0.578 + 0.429 * dot(vd, normalize(tp.sun.xyz)), 0.0, 1.0);
        /* in a cloud's shadow the direct light goes: towards the dark colour (the ambient), the forward glow first */
        t *= 0.35 + 0.65 * T;
        col = mix(tp.dark.rgb, tp.lit.rgb, t);
    }
    if (tp.fogc.w > 0.5) {
        float dm = d / max(tp.eyes.w, 1e-3);
        float f = clamp((dm - tp.fogd.x) / max(tp.fogd.y - tp.fogd.x, 1.0), 0.0, 1.0);
        col = mix(col, tp.fogc.rgb, f);
        al *= 1.0 - 0.85 * f * f;
    }
    o_col = vec4(col * al, al);
}
