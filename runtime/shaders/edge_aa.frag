#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
/* Edge anti-aliasing of the world image before the HUD and the cockpit (ps2_video.c edge_aa_pass, PS2_EDGE_AA=1):
   FXAA-style. Pixels whose 4-neighbour luma range is small keep their colour (discard); on an edge the colour is
   re-sampled across it, by how far the pixel lies from the ends of the edge (searched along it) and by the local
   sub-pixel contrast. The source is a copy of the target (its DATE snapshot), one layer per eye in multiview.
   Writes RGB only (the pipeline's mask): the game's destination alpha stays. */
#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray src;
#define TEX(p) textureLod(src, vec3(p, float(gl_ViewIndex)), 0.0)
#define TEXOFF(p, o) textureLodOffset(src, vec3(p, float(gl_ViewIndex)), 0.0, o)
#else
layout(set = 0, binding = 0) uniform sampler2D src;
#define TEX(p) textureLod(src, p, 0.0)
#define TEXOFF(p, o) textureLodOffset(src, p, 0.0, o)
#endif
layout(push_constant) uniform PC {
    vec4 par;               /* x: edge threshold (relative), y: minimum threshold, z: sub-pixel amount, w: unused */
} pc;
layout(location = 0) out vec4 out_color;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main() {
    vec2 size = vec2(textureSize(src, 0).xy);
    vec2 inv = 1.0 / size;
    vec2 uv = gl_FragCoord.xy * inv;
    vec4 cM = TEX(uv);
    float lM = luma(cM.rgb);
    float lN = luma(TEXOFF(uv, ivec2(0, -1)).rgb), lS = luma(TEXOFF(uv, ivec2(0, 1)).rgb);
    float lW = luma(TEXOFF(uv, ivec2(-1, 0)).rgb), lE = luma(TEXOFF(uv, ivec2(1, 0)).rgb);
    float lMin = min(lM, min(min(lN, lS), min(lW, lE)));
    float lMax = max(lM, max(max(lN, lS), max(lW, lE)));
    float range = lMax - lMin;
    if (range < max(pc.par.y, lMax * pc.par.x)) discard;
    float lNW = luma(TEXOFF(uv, ivec2(-1, -1)).rgb), lNE = luma(TEXOFF(uv, ivec2(1, -1)).rgb);
    float lSW = luma(TEXOFF(uv, ivec2(-1, 1)).rgb), lSE = luma(TEXOFF(uv, ivec2(1, 1)).rgb);
    /* sub-pixel aliasing: contrast of the pixel against its neighbourhood */
    float lAvg = (2.0 * (lN + lS + lW + lE) + lNW + lNE + lSW + lSE) / 12.0;
    float sub = clamp(abs(lAvg - lM) / range, 0.0, 1.0);
    sub = smoothstep(0.0, 1.0, sub);
    sub = sub * sub * pc.par.z;
    /* edge orientation: a horizontal edge changes along y (second differences down the columns) */
    float eH = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    float eV = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    bool horz = eH >= eV;
    float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
    float g1 = l1 - lM, g2 = l2 - lM;
    bool steep1 = abs(g1) >= abs(g2);
    float gScaled = 0.25 * max(abs(g1), abs(g2));
    float stepLen = horz ? inv.y : inv.x;
    float lLocal;
    if (steep1) { stepLen = -stepLen; lLocal = 0.5 * (l1 + lM); } else lLocal = 0.5 * (l2 + lM);
    vec2 cuv = uv;
    if (horz) cuv.y += 0.5 * stepLen; else cuv.x += 0.5 * stepLen;
    /* search along the edge for its ends */
    vec2 off = horz ? vec2(inv.x, 0.0) : vec2(0.0, inv.y);
    vec2 uv1 = cuv - off, uv2 = cuv + off;
    float e1 = luma(TEX(uv1).rgb) - lLocal, e2 = luma(TEX(uv2).rgb) - lLocal;
    bool r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
    const float Q[10] = float[](1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (int i = 0; i < 10 && !(r1 && r2); i++) {
        if (!r1) { uv1 -= off * Q[i]; e1 = luma(TEX(uv1).rgb) - lLocal; r1 = abs(e1) >= gScaled; }
        if (!r2) { uv2 += off * Q[i]; e2 = luma(TEX(uv2).rgb) - lLocal; r2 = abs(e2) >= gScaled; }
    }
    float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y;
    float d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
    bool near1 = d1 < d2;
    float pixOff = 0.5 - min(d1, d2) / (d1 + d2);
    bool mSmaller = lM < lLocal;
    bool good = ((near1 ? e1 : e2) < 0.0) != mSmaller;
    float f = max(good ? pixOff : 0.0, sub);
    vec2 fuv = uv;
    if (horz) fuv.y += f * stepLen; else fuv.x += f * stepLen;
    out_color = vec4(TEX(fuv).rgb, cM.a);
}
