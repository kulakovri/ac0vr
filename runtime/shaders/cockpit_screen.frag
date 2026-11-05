#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* A display screen of the native VR cockpit (ps2_video.c mfd_*, tools/cockpit/ac2cpit.py --screen) showing a block of
   the game's own HUD (the radar, the weapon block), drawn at the game's pixels into a render target: unlit (the
   display shines), opaque, the screen's texture coordinates (0..1 over the screen, v down) mapped onto the part of the
   target that holds the block. Each pixel averages the target's texels under it (no mip chain on a render target:
   the block, 4x upscaled, is seen smaller than its texels). Same vertex stage and push constants as cockpit.frag. */
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec3 v_n;
layout(location = 2) in vec3 v_p;
layout(location = 3) in float v_ao;
layout(location = 0) out vec4 out_color;

#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray img;     /* both layers hold the same image */
#define SAMPLE(uv) texture(img, vec3(uv, 0.0))
#define IMG_SIZE vec2(textureSize(img, 0).xy)
#else
layout(set = 0, binding = 0) uniform sampler2D img;
#define SAMPLE(uv) texture(img, uv)
#define IMG_SIZE vec2(textureSize(img, 0))
#endif

layout(push_constant) uniform CP {
    vec4 rot[3];
    vec4 head;
    vec4 proj;
    vec4 misc;
    vec4 rect;        /* (cockpit.frag: color) the part of the image on the screen: u0, v0, width, height (0..1) */
    vec4 par;         /* (light) x brightness, y black level, z 1 = test pattern */
    vec4 model[3];
    vec4 clip;        /* (su) the block's own area u0, v0, u1, v1: dark outside (letterbox, other blocks) */
    vec4 sv;          /* a round scope's face (screen 4, ps2_video.c mfd_scope_clip): centre u, v, radius (screen
                         heights; 0 = no clip), width / height: dark outside that circle */
    vec4 sd;
    vec4 uvoff;
    vec4 wx;
} cp;

vec3 tap(vec2 t) {
    if (t.x < cp.clip.x || t.y < cp.clip.y || t.x > cp.clip.z || t.y > cp.clip.w) return vec3(0.0);
    return SAMPLE(t).rgb;
}

void main() {
    vec2 uv = clamp(v_uv, 0.0, 1.0);
    vec3 col;
    if (cp.par.z > 0.5) {
        /* orientation test: red top left, green top right, blue bottom left, grey grid of 8 */
        vec2 g = abs(fract(uv * 8.0) - 0.5);
        col = vec3(0.15) + vec3(0.35) * step(0.44, max(g.x, g.y));
        if (uv.x < 0.3 && uv.y < 0.3) col = vec3(1.0, 0.1, 0.1);
        else if (uv.x > 0.7 && uv.y < 0.3) col = vec3(0.1, 1.0, 0.1);
        else if (uv.x < 0.3 && uv.y > 0.7) col = vec3(0.1, 0.2, 1.0);
    } else {
        vec2 t = cp.rect.xy + uv * cp.rect.zw;
        vec2 sz = IMG_SIZE;
        /* the pixel's footprint in texels (axis-aligned box), covered by up to 4x4 bilinear taps of 2x2 texels each */
        vec2 dx = dFdx(t) * sz, dy = dFdy(t) * sz;
        vec2 foot = max(abs(dx) + abs(dy), vec2(1.0));
        ivec2 nt = ivec2(clamp(ceil(foot * 0.5), vec2(1.0), vec2(4.0)));
        vec3 sum = vec3(0.0);
        for (int j = 0; j < nt.y; j++)
            for (int i = 0; i < nt.x; i++) {
                vec2 o = ((vec2(i, j) + 0.5) / vec2(nt) - 0.5) * foot / sz;
                sum += tap(t + o);
            }
        col = sum / float(nt.x * nt.y) * cp.par.x;
    }
    if (cp.sv.z > 0.0) {      /* a round CRT image inside the bezel's opening (ACZ_VR_MFD_SCOPE_CLIP), edge anti-aliased */
        float d = length((v_uv - cp.sv.xy) * vec2(cp.sv.w, 1.0)), w = max(fwidth(d), 1e-4);
        col *= 1.0 - smoothstep(cp.sv.z - w, cp.sv.z + w, d);
    }
    out_color = vec4(max(col, vec3(cp.par.y)), 1.0);
}
