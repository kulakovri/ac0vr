#version 450
/* G presence (ps2_video.c gforce_pass, wish #77): the grey-out / tunnel-vision veil, head-locked. An annulus about the
   eye's forward axis -- the frame camera's centre: each eye looks parallel to the head, so its forward axis projects to
   the same point in both views (no gl_ViewIndex needed with multiview) -- in tangent space, from the angle where the
   veil starts (inner radius, 0 = the whole disc) to beyond the image's corners. 64 segments, 6 vertices each. */
layout(push_constant) uniform P {
    vec4 proj;   /* fx, fy, cx, cy (target pixels, unscaled) */
    vec4 misc;   /* x 2 / target width, y 2 / target height, z inner radius (tangent), w outer radius (tangent) */
    vec4 par;    /* x clear angle (rad), y transition width (rad), z edge alpha, w alpha over the whole view */
    vec4 red;    /* x red-out edge alpha, y red-out alpha over the whole view, z dither seed, w the head fade */
} pc;
layout(location = 0) out vec2 v_t;
const int N = 64;
void main() {
    int seg = gl_VertexIndex / 6, k = gl_VertexIndex - seg * 6;
    int s = (k == 1 || k == 3 || k == 4) ? seg + 1 : seg;          /* (s, in) (s+1, in) (s, out) (s+1, in) (s+1, out) (s, out) */
    bool outer = (k == 2 || k == 4 || k == 5);
    float a = 6.2831853 * float(s) / float(N);
    float r = outer ? pc.misc.w / cos(3.14159265 / float(N)) : pc.misc.z;
    vec2 t = r * vec2(cos(a), sin(a));
    v_t = t;
    vec2 px = pc.proj.zw + t * pc.proj.xy;
    gl_Position = vec4(px * pc.misc.xy - 1.0, 0.0, 1.0);
}
