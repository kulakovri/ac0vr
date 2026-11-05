#version 450
/* Native VR cockpit (ps2_video.c cpit_shadow_pass): the model's depth seen from the game's sun, an orthographic map
   over the front cockpit (rows u, v: map x, y in -1..1; row d: depth 0 nearest the sun .. 1). */
layout(location = 0) in vec3 in_pos;

layout(push_constant) uniform SP {
    vec4 u, v, d;      /* xyz direction / extent, w offset */
    vec4 model[3];     /* rows of the part's animation transform */
} sp;

void main() {
    vec3 pa = vec3(dot(sp.model[0].xyz, in_pos) + sp.model[0].w, dot(sp.model[1].xyz, in_pos) + sp.model[1].w,
                   dot(sp.model[2].xyz, in_pos) + sp.model[2].w);
    gl_Position = vec4(dot(sp.u.xyz, pa) + sp.u.w, dot(sp.v.xyz, pa) + sp.v.w, dot(sp.d.xyz, pa) + sp.d.w, 1.0);
}
