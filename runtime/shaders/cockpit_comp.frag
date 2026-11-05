#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* Native VR cockpit (ps2_video.c cpit_composite): the multisampled cockpit, resolved (premultiplied: background 0,
   edges partly covered), over the frame. pc.org: the resolved image's origin in the target (0 0; the recording's
   cinematic view at its own density, ACZ_REC_CINE_SCALE: a window-sized image at the window clip's corner) */
#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray src;
#else
layout(set = 0, binding = 0) uniform sampler2D src;
#endif
layout(push_constant) uniform PC { ivec2 org; } pc;
layout(location = 0) out vec4 out_color;

void main() {
#ifdef MV
    vec4 c = texelFetch(src, ivec3(ivec2(gl_FragCoord.xy) - pc.org, gl_ViewIndex), 0);
#else
    vec4 c = texelFetch(src, ivec2(gl_FragCoord.xy) - pc.org, 0);
#endif
    if (c.a <= 0.0) discard;
    out_color = c;
}
