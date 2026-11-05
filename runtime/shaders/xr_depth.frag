#version 450
/* The frame's depth for the headset (ps2_video.c xrd_*, PS2_XR_DEPTH; XR_KHR_composition_layer_depth): one full-screen
   triangle (present.vert) into an eye's depth swapchain image, which covers the same pixels as the eye's colour copy.
   Each pixel of the frame's own depth buffer (larger = nearer, GEQUAL) is decoded into its distance in metres along
   the view axis, then written in the one mapping the layer declares (reversed: XrCompositionLayerDepthInfoKHR nearZ =
   far, farZ = near, minDepth 0, maxDepth 1):
     d >= world.w           saturated (Z = max: full-screen passes, 2D overlays such as the sun's flare written at
                            the front; the cockpit would be nearer than 5 cm): at infinity
     d >= cpit.x            the native 3D cockpit (cpit_draw: 1 - 0.0002 per metre, > 0.998)
     |d - HG_MARK| <= band  the HUD combiner's mark (hudglass): the glass is transparent and the symbols seen through
                            it are collimated -- at infinity, like the world behind it
     d <= 0                 nothing drawn (sky; the clear value)
     else                   the world through the game's projection: Z / zmax = A / depth + B (the trails' mapping),
                            depth = A / (d - B) game units
   out = f (n - D) / ((n - f) D) for D metres (n = nearZ, f = farZ: 0 at n, 1 at f), clamped to [0, 1]; D is the
   apparent distance (game units / (units per metre x ACZ_VR_STEREO_SCALE), cockpit metres / stereo scale), the one the
   stereo images show. Debug (mode 1): out = the raw frame depth (identity copy). */
#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray src;
#else
layout(set = 0, binding = 0) uniform sampler2D src;
#endif

layout(push_constant) uniform PC {
    ivec4 off;      /* source pixel of the image's (0, 0); layer; mode (0 convert, 1 raw) */
    vec4 world;     /* A / units (m), B, 1 / stereo scale, saturation threshold */
    vec4 cpit;      /* cockpit threshold, metres per depth unit (1 / 0.0002), HUD mark low, high */
    vec4 outp;      /* f (m), 1 / n (1 / m), 1 / (1 - f / n), unused */
} pc;

void main() {
    ivec2 p = ivec2(gl_FragCoord.xy) + pc.off.xy;
#ifdef MV
    float d = texelFetch(src, ivec3(p, pc.off.z), 0).r;
#else
    float d = texelFetch(src, p, 0).r;
#endif
    if (pc.off.w == 1) { gl_FragDepth = clamp(d, 0.0, 1.0); return; }
    float D = -1.0;                                   /* metres; < 0: at infinity */
    if (d >= pc.world.w) D = -1.0;
    else if (d >= pc.cpit.x) D = max(1.0 - d, 0.0) * pc.cpit.y;
    else if (d >= pc.cpit.z && d <= pc.cpit.w) D = -1.0;
    else if (d > 0.0) {
        float q = d - pc.world.y;
        if (q > 0.0) D = pc.world.x / q;
    }
    float z = 0.0;
    if (D >= 0.0) {
        D *= pc.world.z;
        z = D <= pc.outp.x ? 1.0 : pc.outp.x * (1.0 - D * pc.outp.y) * pc.outp.z / D;
    }
    gl_FragDepth = clamp(z, 0.0, 1.0);
}
