#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* Contrails, wing vapour and heat haze (ps2_video.c trl_pass; wishes #85-#87): world-space strips built on the CPU
   around the frame's centre eye (positions relative to it, game units, world axes), projected per eye exactly like the
   game's world in this renderer: the view axes of the frame's camera (cl_view_make: the game frame's head-turned camera,
   or the extrapolated one between game frames), each eye shifted along the camera's x by its half separation (as
   ps2_clouds' C2 eyes), the frame's target projection, and the frame's depth mapping (view depth -> the GS depth value:
   z = A / depth + B, larger = nearer) so the frame's depth test hides them behind terrain, aircraft and the stamped cloud
   depth. Both eyes see the same geometry (billboards face the centre eye). */
layout(location = 0) in vec4 a_pa;       /* xyz position (game units, relative to the centre eye), w opacity / strength */
layout(location = 1) in vec4 a_uv;       /* x along, y across (-1..1), z kind (0 trail, 1 puff, 2 haze), w seed */
layout(location = 2) in vec4 a_ex;       /* x sun transmittance (cloud shadow), y z w per kind */

layout(location = 0) out vec4 v_a;       /* opacity, along, across, kind */
layout(location = 1) out vec4 v_b;       /* seed, T, ex.y, ex.z */
layout(location = 2) out vec4 v_dir;     /* xyz view vector from the eye (game units, world), w ex.w */

layout(push_constant) uniform TP {
    vec4 cx, cy, cz;    /* the camera's axes in world (x right, y up, z back) */
    vec4 proj;          /* fx, fy, cx, cy in target pixels */
    vec4 misc;          /* 2 / target width, 2 / target height, depth A, depth B */
    vec4 eyes;          /* x left eye shift, y right eye shift (game units along cx; two-pass: x = this pass's), z time (s),
                           w game units per metre */
    vec4 sun;           /* xyz towards the sun (0 = none), w contrail strength */
    vec4 lit;           /* rgb the lit cloud colour (the game's palette), w vapour strength */
    vec4 dark;          /* rgb the dark one, w haze strength (pixels of displacement at full strength) */
    vec4 fogc;          /* rgb the fog colour, w 1 = fog on */
    vec4 fogd;          /* x near (m), y far (m), z / w target size (pixels) */
} tp;

void main() {
    vec3 p = a_pa.xyz;
    float sh = tp.eyes.x;
#ifdef MV
    sh = gl_ViewIndex == 0 ? tp.eyes.x : tp.eyes.y;
#endif
    vec3 c = vec3(dot(p, tp.cx.xyz), dot(p, tp.cy.xyz), dot(p, tp.cz.xyz));
    c.x -= sh;
    float w = -c.z;
    float Xc = (tp.proj.z * tp.misc.x - 1.0) * w + tp.proj.x * tp.misc.x * c.x;
    float Yc = (tp.proj.w * tp.misc.y - 1.0) * w - tp.proj.y * tp.misc.y * c.y;
    float Zc = tp.misc.z + tp.misc.w * w;           /* z / w = A / depth + B */
    gl_Position = vec4(Xc, Yc, Zc, w);               /* (the clipper cuts what is behind the near limit) */
    v_a = vec4(a_pa.w, a_uv.x, a_uv.y, a_uv.z);
    v_b = vec4(a_uv.w, a_ex.x, a_ex.y, a_ex.z);
    v_dir = vec4(p, a_ex.w);
}
