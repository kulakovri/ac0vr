#version 450
#ifdef MV
#extension GL_EXT_multiview : require
#endif
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec3 in_stq;
layout(location = 3) in float in_fog;
layout(location = 4) in uint in_round_uv;
#ifdef MV
layout(location = 5) in float in_xr_dx;   /* right-eye x shift; view 0 (left) uses -in_xr_dx */
#endif
layout(location = 6) in float in_xr_iw;   /* 1 / view depth of a world vertex; < 0: never reprojected */

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_stq;
layout(location = 2) out float v_fog;
layout(location = 3) flat out uint v_round_uv;

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
    uint  mv_flags;        /* multiview: 1 set-0 texture is layered, 2 set-1 texture is layered, 4 shift eyes;
                              256 reproject world vertices with rp */
    vec4  rp[4];           /* reprojection to another camera: rows of R (xyz) + cx, cy, fx; t (xyz) + fy */
    vec4  rp2;             /* x: fraction of a game frame (objects with their own motion, mesh.vert); y: with fogr, the
                              GS Z (0..1) at an infinite depth */
    vec4  fogr;            /* radial fog (ACZ_VR_FOG_RADIAL, ps2_video.c fogr_*), set only for the horizon haze ring's
                              layers: the game frame's camera cx, cy (target pixels), fx, fy */
} pc;
/* A world vertex at screen (x, y) with inverse view depth iw, seen from another camera: view direction at unit depth
   d = ((x - cx) / fx, -(y - cy) / fy, -1); new view point (scaled by iw) p = R d + t iw. Returns the new screen
   position and the depth ratio w / w' (for perspective texture coordinates). */
vec3 reproject(float x, float y, float iw) {
    vec3 d = vec3((x - pc.rp[0].w) / pc.rp[2].w, -(y - pc.rp[1].w) / pc.rp[3].w, -1.0);
    vec3 p = vec3(dot(pc.rp[0].xyz, d), dot(pc.rp[1].xyz, d), dot(pc.rp[2].xyz, d)) + pc.rp[3].xyz * iw;
    float pz = -p.z;
    if (pz < 1e-4) return vec3(x, y, 1.0);
    return vec3(pc.rp[0].w + pc.rp[2].w * p.x / pz, pc.rp[1].w - pc.rp[3].w * p.y / pz, 1.0 / pz);
}


void main() {
    v_color = in_color / 128.0;
    v_stq   = in_stq;
    v_fog   = in_fog;
    v_round_uv = in_round_uv;
    gl_PointSize = pc.point_size;
    float x = in_pos.x, y = in_pos.y;
    float iw = in_xr_iw;
#ifdef MV
    /* ACZ_REC_CINE (ps2_video.c cine_*): the recording's cinematic camera sees every vertex of a world, HUD or sun draw
       from where it is -- also those the in-between frames leave alone (xr_iw < 0: near draws, the HUD): their inverse
       view depth from the eye shift, xr_dx = -mh iw (rp2.w = 1 / mh; the HUD and overlays have none: at infinity) */
    if ((pc.mv_flags & 131072u) != 0u && iw < 0.0) iw = max(-in_xr_dx * pc.rp2.w, 0.0);
#endif
    if ((pc.mv_flags & 256u) != 0u && iw >= 0.0) {
        vec3 r = reproject(x, y, iw);
        x = r.x; y = r.y;
        if (((pc.flags.x >> 2) & 1u) == 0u) v_stq = in_stq * r.z;   /* STQ = (s q, t q, q): q is 1 / w */
    }
#ifdef MV
    if ((pc.mv_flags & 4u) != 0u) x += gl_ViewIndex == 0 ? -in_xr_dx : in_xr_dx;
#endif
    /* the haze ring's layers sit at the GS Z of a view depth h and cover (depth GREATER) what lies beyond; in VR the
       view depth of a point is its distance times the cosine of its angle off the head's view axis, so the haze
       moved with head turns. With pc.fogr the layer's depth at this vertex is the Z of the view depth h / rho
       (rho = distance / view depth of the vertex's direction, from its game-frame position): Z = A + rho (Z_h - A) */
    float z = in_pos.z;
    if (pc.fogr.z > 0.0) {
        float rho = length(vec3((in_pos.x - pc.fogr.x) / pc.fogr.z, (in_pos.y - pc.fogr.y) / pc.fogr.w, 1.0));
        z = clamp(pc.rp2.y + rho * (in_pos.z - pc.rp2.y), 0.0, 0.99999);
    }
    gl_Position = vec4(x * pc.inv_size.x - 1.0,
                       y * pc.inv_size.y - 1.0,
                       z, 1.0);
#ifdef MV
    /* ACZ_REC_CINE_MONO (ps2_video.c cine_*): the recording's one view is layer 0; view 1's primitives collapse off
       screen (no fragments) */
    if ((pc.mv_flags & 262144u) != 0u && gl_ViewIndex == 1) gl_Position = vec4(-4.0, -4.0, 0.5, 1.0);
#endif
}
