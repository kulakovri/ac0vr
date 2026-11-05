#version 450
/* G presence (ps2_video.c gforce_pass, wish #77): grey-out = the periphery losing colour, contrast and light first
   (tunnel vision), a neutral grey closing in from clear_angle over the transition width, a little over the whole view when
   deep; red-out (sustained negative G) = a faint red cast, stronger outward. Premultiplied over the frame (the
   destination keeps 1 - alpha), colour only. A +-0.5/255 dither keeps the smooth ramp free of bands over the sky. */
layout(push_constant) uniform P {
    vec4 proj;   /* fx, fy, cx, cy (target pixels, unscaled) */
    vec4 misc;   /* x 2 / target width, y 2 / target height, z inner radius (tangent), w outer radius (tangent) */
    vec4 par;    /* x clear angle (rad), y transition width (rad), z edge alpha, w alpha over the whole view */
    vec4 red;    /* x red-out edge alpha, y red-out alpha over the whole view, z dither seed, w the head fade: black
                    over the whole view (ps2_video.c hvf_*, bug #156: the eye inside the cockpit model's parts) */
} pc;
layout(location = 0) in vec2 v_t;
layout(location = 0) out vec4 o;
void main() {
    float th = atan(length(v_t));                            /* the angle from the eye's forward axis */
    float m = smoothstep(pc.par.x, pc.par.x + pc.par.y, th);
    float ag = clamp(pc.par.z * m + pc.par.w, 0.0, 0.95);
    float edge = smoothstep(0.35, 0.95, th);
    float ar = clamp(pc.red.x * (0.35 + 0.65 * edge) + pc.red.y, 0.0, 0.5);
    float a = 1.0 - (1.0 - ag) * (1.0 - ar);
    vec3 c = vec3(0.20, 0.20, 0.205) * ag + vec3(0.30, 0.025, 0.02) * ar * (1.0 - ag);
    float d = fract(sin(dot(gl_FragCoord.xy + pc.red.z, vec2(12.9898, 78.233))) * 43758.5453) - 0.5;
    float hk = clamp(pc.red.w, 0.0, 1.0);
    o = vec4(max(c + d / 255.0 * step(0.002, a), vec3(0.0)) * (1.0 - hk), 1.0 - (1.0 - a) * (1.0 - hk));
}
