#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* Heat haze behind the nozzles (ps2_video.c trl_pass; wish #86): the frame behind the plume (a snapshot of the target
   made right before this pass, over the plumes' box) seen through a small refraction: the image is displaced by an
   angle (pixels = angle x the projection's focal length, the same in both eyes) that comes from a noise living on the
   plume's own surface (metres from the nozzle along the exhaust, the position across it), flowing aft with the
   exhaust and churning. So the shimmer is world-anchored and identical in both eyes; it is strongest just behind the
   nozzle and fades along the plume and towards its edges. Depth-tested against the frame (whatever is in front of the
   plume -- the own tail, another aircraft -- is not touched); the 3D cockpit comes later and is never displaced. */
layout(location = 0) in vec4 v_a;        /* strength, along (0 at the nozzle .. 1 at the plume's end), across, kind */
layout(location = 1) in vec4 v_b;        /* seed, T, plume length (m), nozzle radius (m) */
layout(location = 2) in vec4 v_dir;
layout(location = 0) out vec4 o_col;

#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray snap;
#define FETCH(p) texelFetch(snap, ivec3(p, gl_ViewIndex), 0)
#define SNAPSIZE() textureSize(snap, 0).xy
#else
layout(set = 0, binding = 0) uniform sampler2D snap;
#define FETCH(p) texelFetch(snap, p, 0)
#define SNAPSIZE() textureSize(snap, 0)
#endif

layout(push_constant) uniform TP {
    vec4 cx, cy, cz;
    vec4 proj;
    vec4 misc;
    vec4 eyes;          /* z time (s) */
    vec4 sun;
    vec4 lit;
    vec4 dark;          /* w displacement at full strength (target pixels) */
    vec4 fogc;
    vec4 fogd;
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
vec4 bilinear(vec2 p) {
    ivec2 sz = SNAPSIZE();
    vec2 q = p - 0.5, f = fract(q);
    ivec2 i0 = clamp(ivec2(floor(q)), ivec2(0), sz - 1), i1 = clamp(i0 + 1, ivec2(0), sz - 1);
    return mix(mix(FETCH(i0), FETCH(ivec2(i1.x, i0.y)), f.x), mix(FETCH(ivec2(i0.x, i1.y)), FETCH(i1), f.x), f.y);
}

void main() {
    float k = v_a.x, along = v_a.y, across = v_a.z, seed = v_b.x, len_m = v_b.z;
    float t = tp.eyes.z;
    float m;
    vec2 q;
    if (v_a.w > 2.5) {
        /* the plume end-on: a disc about it (radius v_b.w m); the exhaust comes towards / goes away from the eye, so the
           pattern boils in place instead of flowing across */
        float r = length(vec2(along, across));
        m = (1.0 - smoothstep(0.45, 1.0, r)) * k;
        q = vec2(along, across) * v_b.w * 1.4 + seed + vec2(t * 2.2, -t * 1.7);
    } else {
        m = (1.0 - smoothstep(0.40, 1.0, abs(across))) * smoothstep(0.0, 0.05, along) * (1.0 - smoothstep(0.30, 1.0, along));
        m *= k;
        float s = along * len_m;                     /* metres aft of the nozzle */
        /* two octaves of value noise on the plume, flowing aft (the visible pattern moves at ~15 m/s, churns at 3 Hz) */
        q = vec2(s * 1.4 - t * 15.0, across * 1.6) + seed;
    }
    if (m < 0.004) discard;
    vec2 g = vec2(vnoise(q + vec2(0.0, t * 3.0)), vnoise(q * 1.07 + vec2(13.7, -t * 2.6))) - 0.5;
    g += 0.5 * (vec2(vnoise(q * 2.3 + 5.1), vnoise(q * 2.1 + 9.4)) - 0.5);
    vec2 off = g * (2.0 * tp.dark.w * m);
    vec4 c = bilinear(gl_FragCoord.xy + off);
    float a = clamp(m * 1.5, 0.0, 1.0);              /* (fully the displaced image in the core; edges blend) */
    if (tp.sun.w < 0.0) c.rgb = mix(c.rgb, vec3(1.0, 0.1, 0.1), 0.5);   /* ACZ_DEBUG_TRAILS_TINT: the plumes in red */
    o_col = vec4(c.rgb * a, a);
}
