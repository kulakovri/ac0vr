#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* A big burst as a volume (ps2_video.c bst_pass; ACZ_VR_FX_BURST, bug #266): no vertex buffer. Vertices 0..3 = a quad
   facing the frame's centre eye that covers the sphere's outline, at the sphere's front (the frame's depth test hides
   it behind nearer terrain, aircraft and our clouds' stamped depth; no depth write); vertices 4..6 (the eye in or near
   the sphere) = one triangle over the whole target at the nearest depth. The fragment shader marches each eye's own
   ray through the sphere (true stereo), so the camera axes, the projection and the eye shifts are those of trails.vert. */
layout(location = 0) out vec3 v_pos;     /* (unused by the march: the fragment shader builds the ray from its pixel) */

layout(push_constant) uniform BP {
    vec4 cx, cy, cz;    /* the camera's axes in world (x right, y up, z back) */
    vec4 proj;          /* fx, fy, cx, cy in target pixels */
    vec4 misc;          /* 2 / target width, 2 / target height, depth A, depth B */
    vec4 eyes;          /* x left eye shift, y right eye shift (game units along cx; two-pass: x = this pass's), z time (s),
                           w game units per metre */
    vec4 ctr;           /* xyz the burst's centre from the centre eye (game units, world axes), w its radius (units) */
    vec4 st;            /* x age / life (0..1), y strength, z seed, w the quad's half size at its plane (units) */
    vec4 heat;          /* x core heat, y opacity scale, z smoke share, w the eye's distance to the centre / radius */
    vec4 tint;          /* rgb the game's halo colour (its burst cards), w the halo's share */
    vec4 fogc;          /* rgb fog colour, w the fog's amount at the burst */
    vec4 veil;          /* (burst.frag) */
    vec4 box;
} bp;

void main() {
    int i = gl_VertexIndex;
    float sh = bp.eyes.x;
#ifdef MV
    sh = gl_ViewIndex == 0 ? bp.eyes.x : bp.eyes.y;
#endif
    if (i >= 4) {                                   /* the full-target triangle, nearest depth (larger = nearer) */
        vec2 q = vec2(i == 5 ? 3.0 : -1.0, i == 6 ? 3.0 : -1.0);
        gl_Position = vec4(q, 0.99999, 1.0);
        v_pos = vec3(0.0);
        return;
    }
    vec3 c = bp.ctr.xyz;
    float d = max(length(c), 1e-3);
    vec3 f = c / d;                                  /* towards the centre */
    vec3 u = normalize(abs(f.y) < 0.95 ? cross(f, vec3(0.0, 1.0, 0.0)) : cross(f, vec3(1.0, 0.0, 0.0)));
    vec3 v = cross(u, f);
    float R = bp.ctr.w;
    vec3 pc = f * max(d - R, 1.0);                   /* the sphere's front */
    vec2 k = vec2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0);
    vec3 p = pc + (u * k.x + v * k.y) * bp.st.w;
    vec3 cv = vec3(dot(p, bp.cx.xyz), dot(p, bp.cy.xyz), dot(p, bp.cz.xyz));
    cv.x -= sh;
    float w = -cv.z;
    float Xc = (bp.proj.z * bp.misc.x - 1.0) * w + bp.proj.x * bp.misc.x * cv.x;
    float Yc = (bp.proj.w * bp.misc.y - 1.0) * w - bp.proj.y * bp.misc.y * cv.y;
    float Zc = bp.misc.z + bp.misc.w * w;            /* z / w = A / depth + B */
    gl_Position = vec4(Xc, Yc, Zc, w);
    v_pos = p;
}
