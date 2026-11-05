#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* The native cockpit model's depth in the frame (ps2_video.c cpit_draw mode 2, after the multisampled pass) for the
   parts whose texture has see-through texels: only cockpit.frag's alpha cut (texels below light.w are not drawn), no
   colour. Without it the vertex-only depth pass wrote those texels too, and the HUD symbols and target boxes drawn
   only where the model is not in front (hudocc) vanished behind a clear canopy (the Su-27's canopy.glass and
   object.001: 72% of the forward view, bug #43). ACZ_VR_COCKPIT_DEPTH_CUT=0: as before. */
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray tex;
#define SAMPLE(uv) texture(tex, vec3(uv, 0.0))
#else
layout(set = 0, binding = 0) uniform sampler2D tex;
#define SAMPLE(uv) texture(tex, uv)
#endif

layout(push_constant) uniform CP {   /* the first members of cockpit.vert / cockpit.frag's block */
    vec4 rot[3];
    vec4 head;
    vec4 proj;
    vec4 misc;
    vec4 color;       /* material colour */
    vec4 light;       /* z textured; w alpha cut */
} cp;

void main() {
    vec4 c = cp.color;
    if (cp.light.z != 0.0) c *= SAMPLE(v_uv);
    if (c.a < cp.light.w) discard;
    out_color = vec4(0.0);
}
