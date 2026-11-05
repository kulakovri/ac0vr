#version 450
/* The rear-view mirrors' terrain (ps2_video.c mlv_terr_*, ACZ_VR_COCKPIT_MIRRORS_TERRAIN; agent mir_live 2026-10-29,
   step M3 of docs/research/2026-10-24/mirrors-lookback.md): the static grid of the loaded map (zero_terrain.c,
   ps2_terrain_mirror_grid: world positions in game units, the game's own tile colours x the relief light) drawn into the
   live map before its aircraft, by the game frame's rear camera (as mesh.vert maps the aircraft: pixel = c + f X / D +
   0.5, the game's depth z = (zA + zB / D) x zscale / zmax, kept above 0 so the map's coverage sees it), with the game's
   fog (C1's near / far distances and colour) by the distance, and within ~4 deg of the horizon the stage-1 mirrors'
   haze band towards the sky's horizon colour (ACZ_VR_COCKPIT_MIRRORS_HAZE: the fully fogged far ground meets the sky
   without a line, as the game's haze ring makes it in the frame). */
layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec4 in_col;
layout(location = 0) out vec3 v_col;
layout(location = 1) out float v_fog;
layout(location = 2) out float v_band;
layout(location = 3) out vec2 v_cell;     /* (phase 4) the tile grid's cell coordinates (x, z) */
layout(location = 4) out float v_shade;   /* the textures' shade: the vertex colour x the relief x the brightness */
layout(push_constant) uniform PC {
    vec4 rx, ry, rz;      /* rows of the world -> rear view turn; w: the rear camera's eye (world, game units) */
    vec4 proj;            /* cx, cy, fx, fy (map pixels) */
    vec4 size;            /* 2 / W, 2 / H, zA zscale / zmax, zB zscale / zmax */
    vec4 fog;             /* the fog colour (rgb), the brightness of the tile colours (w) */
    vec4 fogd;            /* the fog's far end (game units), 1 / (far - near), the haze band's strength (0 / 1), the
                             tiles in the texture array (0: the tiles' mean colours, ACZ_VR_COCKPIT_MIRRORS_TERRAIN_TEX) */
    vec4 hor;             /* the sky's horizon colour (C1), - */
} pc;
void main() {
    vec3 d = in_pos - vec3(pc.rx.w, pc.ry.w, pc.rz.w);
    vec3 v = vec3(dot(pc.rx.xyz, d), dot(pc.ry.xyz, d), dot(pc.rz.xyz, d));
    float D = -v.z;
    float X = (pc.proj.x * D + pc.proj.z * v.x + 0.5 * D) * pc.size.x - D;
    float Y = (pc.proj.y * D - pc.proj.w * v.y + 0.5 * D) * pc.size.y - D;
    float Z = max(pc.size.z * D + pc.size.w, 1e-6 * abs(D));
    gl_Position = vec4(X, Y, Z, D);
    v_col = in_col.rgb * pc.fog.w;
    v_cell = (in_pos.xz + 262144.0) / 2048.0;
    v_shade = in_col.a * 2.0 * pc.fog.w;
    float L = length(v);
    v_fog = clamp((pc.fogd.x - L) * pc.fogd.y, 0.0, 1.0);
    float s = dot(v, vec3(pc.rx.y, pc.ry.y, pc.rz.y)) / max(L, 1e-3);   /* the sine of the ray's elevation (world up) */
    v_band = pc.fogd.z * exp(-max(-s, 0.0) * 14.0);
}
