#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* Cloud volume around the aircraft (ps2_video.c veil_pass): inside a cloud, soft vapour puffs in a world-fixed lattice
   (cells of par.x metres around the eye, at most one puff a cell) drawn in true stereo behind the 3D cockpit, so they
   stream past with the aircraft's real motion and parallax; plus a near haze over the whole view (instance 0). The
   puffs live in the cockpit frame (metres; x right, y up, z back), seen through the head pose and projection of the
   frame, as the cockpit model (cockpit.vert). Their number follows the measured cloud density (fog.a), clustered by a
   coarse world noise, so a thin cloud gives scattered wisps and a thick one a moving whiteout. */
layout(location = 0) out vec2 v_uv;      /* puff: -1..1 over the quad */
layout(location = 1) out vec4 v_p;       /* x opacity, y texture seed, z sun glow, w 1 = the full-view haze */

layout(push_constant) uniform VP {
    vec4  rot[3];     /* rows of the head rotation (cockpit frame -> head) */
    vec4  head;       /* head position in the cockpit frame (m); w half the eye separation (m) */
    vec4  proj;       /* fx, fy, cx, cy in target pixels */
    vec4  misc;       /* x 2 / target width, y 2 / target height, z eye sign (two-pass stereo), w time (s) */
    vec4  wld[3];     /* rows of the world -> cockpit frame rotation; w: the eye's offset in its lattice cell (m) */
    ivec4 cell;       /* the eye's lattice cell (world), w half extent of the lattice (cells) */
    vec4  fog;        /* rgb the cloud colour (frame colour space), a density 0..1 */
    vec4  sun;        /* xyz direction to the sun (cockpit frame; 0 none), w exit brightening 0..1 */
    vec4  par;        /* x cell size (m), y puff opacity, z haze opacity, w puff size scale */
} vp;

uvec3 pcg3(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec4 hash4(ivec3 c) {
    uvec3 h = pcg3(uvec3(c));
    uvec3 g = pcg3(h.zxy ^ 0x9E3779B9u);
    return vec4(h, g.x) * (1.0 / 4294967296.0);
}
float hash1(ivec3 c) { return float(pcg3(uvec3(c) + 7919u).x) * (1.0 / 4294967296.0); }
/* coarse value noise over the lattice (clusters of puffs ~4 cells across) */
float cluster(ivec3 c) {
    ivec3 b = c >> 2;                                   /* floor division by 4 (arithmetic shift) */
    vec3 f = vec3(c - (b << 2)) * 0.25 + 0.125;
    f = f * f * (3.0 - 2.0 * f);
    float a = mix(hash1(b), hash1(b + ivec3(1, 0, 0)), f.x), e = mix(hash1(b + ivec3(0, 1, 0)), hash1(b + ivec3(1, 1, 0)), f.x);
    float g = mix(hash1(b + ivec3(0, 0, 1)), hash1(b + ivec3(1, 0, 1)), f.x), h = mix(hash1(b + ivec3(0, 1, 1)), hash1(b + ivec3(1, 1, 1)), f.x);
    return mix(mix(a, e, f.y), mix(g, h, f.y), f.z);
}

void main() {
    int k = gl_VertexIndex;
    float eye = vp.misc.z;
#ifdef MV
    eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
    v_uv = vec2(0.0);
    v_p = vec4(0.0);
    if (gl_InstanceIndex == 0) {                        /* the haze: one triangle over the whole target */
        vec2 q = vec2((k << 1) & 2, k & 2);
        gl_Position = k < 3 ? vec4(q * 2.0 - 1.0, 0.5, 1.0) : vec4(0.0);
        v_p.w = 1.0;
        return;
    }
    int n = 2 * vp.cell.w + 1, i = gl_InstanceIndex - 1;
    ivec3 l = ivec3(i % n, (i / n) % n, i / (n * n)) - vp.cell.w;
    ivec3 cw = vp.cell.xyz + l;
    vec4 h = hash4(cw);
    float dens = vp.fog.a;
    /* presence: a soft threshold against the cell's hash, so puffs fade in and out as the density changes */
    float p = dens * (0.2 + 0.95 * cluster(cw));
    float pres = smoothstep(h.x - 0.12, h.x + 0.12, p);
    vec4 g = hash4(cw + ivec3(5023, 1931, 7177));
    float cs = vp.par.x;
    vec3 r = (vec3(l) + 0.15 + 0.7 * h.yzw) * cs - vec3(vp.wld[0].w, vp.wld[1].w, vp.wld[2].w);
    vec3 P = vec3(dot(vp.wld[0].xyz, r), dot(vp.wld[1].xyz, r), dot(vp.wld[2].xyz, r));     /* cockpit frame */
    vec3 V = P - vp.head.xyz;
    float d = length(V);
    float R = float(vp.cell.w) * cs;
    /* near puffs fade out (they would cross the view in a few frames), far ones fade before the lattice ends */
    float a = pres * vp.par.y * smoothstep(6.0, 24.0, d) * (1.0 - smoothstep(0.6 * R, 0.94 * R, d));
    if (a < 0.003 || k >= 6) { gl_Position = vec4(0.0); return; }
    vec3 dir = V / d;
    float s = cs * (0.42 + 0.38 * g.x) * vp.par.w;
    s = min(s, 0.45 * d);
    /* billboard facing the eye, kept upright in the cockpit frame (head roll does not turn it) */
    vec3 rt = cross(dir, vec3(0.0, 1.0, 0.0));
    rt = dot(rt, rt) > 1e-4 ? normalize(rt) : vec3(1.0, 0.0, 0.0);
    vec3 up = cross(rt, dir);
    const vec2 Q[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    vec2 q = Q[k];
    vec3 X = P + (q.x * rt + q.y * up) * s;
    vec3 v = X - vp.head.xyz;
    vec3 c = vec3(dot(vp.rot[0].xyz, v), dot(vp.rot[1].xyz, v), dot(vp.rot[2].xyz, v));
    c.x -= eye * vp.head.w;
    float w = -c.z;
    float Xc = (vp.proj.z * vp.misc.x - 1.0) * w + vp.proj.x * vp.misc.x * c.x;
    float Yc = (vp.proj.w * vp.misc.y - 1.0) * w - vp.proj.y * vp.misc.y * c.y;
    gl_Position = vec4(Xc, Yc, w - 0.25, w);            /* clipped 0.25 m in front of the eye (z >= 0) */
    /* a random turn of the texture per puff (upright in the cockpit frame, not with the head) */
    float an = g.y * 6.2831853;
    v_uv = mat2(cos(an), sin(an), -sin(an), cos(an)) * q;
    float sg = vp.sun.xyz == vec3(0.0) ? 0.0 : pow(max(dot(dir, vp.sun.xyz), 0.0), 6.0);
    v_p = vec4(a, g.z * 17.0 + g.w, sg, 0.0);
}
