#version 450
/* Cloud volume around the aircraft (cloudveil.vert, ps2_video.c veil_pass): premultiplied over the world, behind the 3D
   cockpit (blend ONE, ONE_MINUS_SRC_ALPHA; RGB only, the frame's destination alpha stays). */
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_p;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler3D noise;   /* 64^3 tileable fbm, 0..1 */

layout(push_constant) uniform VP {
    vec4  rot[3];
    vec4  head;
    vec4  proj;
    vec4  misc;
    vec4  wld[3];
    ivec4 cell;
    vec4  fog;
    vec4  sun;        /* w exit brightening 0..1 */
    vec4  par;        /* z haze opacity */
} vp;

void main() {
    if (v_p.w > 0.5) {
        /* the near haze: the world behind the canopy greyed towards the cloud's colour; after leaving a cloud into
           brighter light, a brief overexposure (additive, alpha 0) */
        float hz = vp.par.z;
        vec3 fl = vec3(1.0, 0.98, 0.94) * 0.16 * vp.sun.w;
        out_color = vec4(vp.fog.rgb * hz + fl * (1.0 - hz), hz);
        return;
    }
    float r = length(v_uv);
    if (r >= 1.0) discard;
    /* a soft blob broken up by the noise into wisps; the texture drifts slowly (the vapour churns) */
    vec3 tc = vec3(v_uv * 0.23 + v_p.y, fract(v_p.y * 0.37) + vp.misc.w * 0.004);
    float n = texture(noise, tc).r * 0.7 + texture(noise, tc * 2.7 + 0.31).r * 0.3;
    float body = 1.0 - smoothstep(0.1, 1.0, r);
    float shape = clamp(body * (0.25 + 1.5 * n) - 0.18, 0.0, 1.0);
    float a = shape * v_p.x;
    if (a < 0.002) discard;
    /* denser cores darker (they shade themselves), thin edges brighter (they scatter), the side towards the sun
       brighter still: the contrast that lets the vapour be seen streaming past against the haze */
    vec3 col = vp.fog.rgb * (1.1 - 0.34 * shape) + vec3(1.0, 0.97, 0.9) * v_p.z * 0.25;
    out_color = vec4(col * a, a);
}
