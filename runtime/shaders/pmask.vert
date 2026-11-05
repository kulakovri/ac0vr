#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* The eye masks of our own passes (ps2_video.c pm_*; ov_passes 2026-10-25): triangles already in clip space, each
   tagged with its eye. a.xy = clip x, y; a.z = depth; a.w = the eye (0 left, 1 right). Multiview: a triangle of the
   other eye goes outside the view volume (clipped whole); two-pass frames draw only the current eye's range. Used as
   the depth pre-fill of the hidden area in the cockpit's multisampled pass (z = 1: the model's GEQUAL test fails
   there) and as the covered area of the cockpit composite, edge AA and the cloud composite (their fragment shaders
   use only gl_FragCoord). */
layout(location = 0) in vec4 a;
void main() {
#ifdef MV
    if (int(a.w + 0.5) != int(gl_ViewIndex)) { gl_Position = vec4(-4.0, -4.0, 0.0, 1.0); return; }
#endif
    gl_Position = vec4(a.xy, a.z, 1.0);
}
