#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
layout(location = 1) in vec3 v_stq;
layout(location = 0) out vec4 out_color;

#ifdef MV
layout(set = 0, binding = 0) uniform sampler2DArray tex;
#else
layout(set = 0, binding = 0) uniform sampler2D tex;
#endif

layout(push_constant) uniform PC {
    vec2  inv_size;
    vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
    uvec4 clamp_uv;
    vec2  img_size;
    float scale;
    float point_size;
    float tex_scale;
    float pad0;
    uint  mesh_block;
    uint  mv_flags;        /* multiview: 1 set-0 texture is layered, 2 set-1 texture is layered, 4 shift eyes */
} pc;

void main() {
    bool fst = ((pc.flags.x >> 2) & 1u) != 0u;
    vec2 tc = fst ? v_stq.xy : (v_stq.xy / max(v_stq.z, 1e-9)) * pc.tex_size;
    tc = clamp(tc, vec2(0.0), max(pc.tex_size - 1.0, vec2(0.0)));
#ifdef MV
    float d = texture(tex, vec3((tc + 0.5) / pc.img_size, float(((pc.mv_flags & 1u) != 0u) ? gl_ViewIndex : 0))).r;
#else
    float d = texture(tex, (tc + 0.5) / pc.img_size).r;
#endif
    gl_FragDepth = d;
    out_color = vec4(d, 0.0, 0.0, 1.0);
}
