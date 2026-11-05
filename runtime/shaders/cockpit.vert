#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
#ifdef CPIT_VRS
/* (cockpit_vrs.vert, ACZ_VR_COCKPIT_VRS: parts of the cockpit pass shaded coarser off the eye's axis, ps2_video.c cvrs_*)
   the squared tangents of the zones' radii: 1x1 inside r1, 2x1 / 1x2 to r2 (coarse along the radius), 2x2 beyond */
#extension GL_EXT_fragment_shading_rate : require
layout(constant_id = 0) const float CVRS_T1 = 0.3333;   /* tan^2 30 deg */
layout(constant_id = 1) const float CVRS_T2 = 0.7041;   /* tan^2 40 deg */
#endif
/* Native VR cockpit (ps2_video.c cpit_*): a model in the pilot's eye frame (metres; x right, y up, z back), seen
   through the head pose of the game frame and the frame's own projection, in true stereo. */
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_nrm;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in float in_ao;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec3 v_n;       /* cockpit frame */
layout(location = 2) out vec3 v_p;       /* cockpit frame (m) */
layout(location = 3) out float v_ao;
invariant gl_Position;                   /* the depth pre-pass and the colour pass (EQUAL) must agree exactly */

layout(push_constant) uniform CP {
    vec4 rot[3];      /* rows of the inverse head rotation (cockpit frame -> head); w: light block (-1 none), specular
                         strength, shininess */
    vec4 head;        /* head position in the cockpit frame (m); w = half the eye separation (m) */
    vec4 proj;        /* fx, fy, cx, cy in target pixels */
    vec4 misc;        /* inverse target size x, y; depth slope per metre (< 0: -near); eye sign (two-pass stereo) */
    vec4 color;       /* material colour */
    vec4 light;       /* x brightness, y ambient (fixed light); z textured; w alpha cut (< 0: glass) */
    vec4 model[3];    /* rows of the part's animation transform (rotation xyz, translation w) */
    vec4 su, sv, sd;  /* sun shadow map: rows u, v (map -1..1), d (depth 0..1); su.xyz = 0: none */
    vec4 uvoff;       /* texture offset (drum counters) */
} cp;

void main() {
    vec3 pa = vec3(dot(cp.model[0].xyz, in_pos) + cp.model[0].w, dot(cp.model[1].xyz, in_pos) + cp.model[1].w,
                   dot(cp.model[2].xyz, in_pos) + cp.model[2].w);              /* needles, attitude ball, ... */
    vec3 na = vec3(dot(cp.model[0].xyz, in_nrm), dot(cp.model[1].xyz, in_nrm), dot(cp.model[2].xyz, in_nrm));
    vec3 v = pa - cp.head.xyz;
    vec3 c = vec3(dot(cp.rot[0].xyz, v), dot(cp.rot[1].xyz, v), dot(cp.rot[2].xyz, v));
    float eye = cp.misc.w;
#ifdef MV
    eye = gl_ViewIndex == 0 ? -1.0 : 1.0;
#endif
    c.x -= eye * cp.head.w;                      /* each eye sits half the separation to its side */
    float w = -c.z;
    /* screen x = cx + fx X / w, y = cy - fy Y / w (target pixels), then the GS shader's mapping to clip space */
    float X = (cp.proj.z * cp.misc.x - 1.0) * w + cp.proj.x * cp.misc.x * c.x;
    float Y = (cp.proj.w * cp.misc.y - 1.0) * w - cp.proj.y * cp.misc.y * c.y;
    /* depth, larger is nearer (GEQUAL): misc.z > 0 just below 1, in front of the world (the frame's own buffer);
       misc.z < 0 -misc.z / w, reversed hyperbolic (a buffer of the model alone: fine at the panel) */
    gl_Position = vec4(X, Y, cp.misc.z < 0.0 ? -cp.misc.z : (1.0 - cp.misc.z * w) * w, w);
#ifdef MV
    /* (ACZ_REC_CINE_MONO, ps2_video.c cine_*: misc.w 2 = the recording's one view, layer 0; view 1 collapses off screen) */
    if (cp.misc.w > 1.5 && gl_ViewIndex == 1) gl_Position = vec4(-4.0, -4.0, 0.5, 1.0);
#endif
#ifdef CPIT_VRS
    /* this eye's angle off its axis at the provoking vertex (the projection's centre is the eye's axis: c.xy = 0) */
    {   float q = c.x * c.x + c.y * c.y, zz = c.z * c.z;
        int r = 0;
        if (w > 0.0 && q >= CVRS_T1 * zz)
            r = q >= CVRS_T2 * zz ? (gl_ShadingRateFlag2HorizontalPixelsEXT | gl_ShadingRateFlag2VerticalPixelsEXT)
              : abs(c.x) >= abs(c.y) ? gl_ShadingRateFlag2HorizontalPixelsEXT : gl_ShadingRateFlag2VerticalPixelsEXT;
        gl_PrimitiveShadingRateEXT = r;
    }
#endif
    v_uv = in_uv + cp.uvoff.xy;
    v_n = na;
    v_p = pa;
    v_ao = in_ao;
}
