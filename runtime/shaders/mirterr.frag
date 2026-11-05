#version 450
/* The rear-view mirrors' terrain (mirterr.vert): the tile colour into the game's fog, opaque. (phase 4,
   ACZ_VR_COCKPIT_MIRRORS_TERRAIN_TEX) The colour is the game's own atlas tile at the point -- the cell's tile id from
   the tile grid, the tile at 32 x 32 with its mips (BC1; texel column along x, row along z, no flips in the game's
   grid) -- times the shade, where the tiles' mean colours drew 400 m blotches */
layout(location = 0) in vec3 v_col;
layout(location = 1) in float v_fog;
layout(location = 2) in float v_band;
layout(location = 3) in vec2 v_cell;
layout(location = 4) in float v_shade;
layout(set = 0, binding = 0) uniform sampler2DArray tiles;
layout(set = 0, binding = 1) uniform usampler2D tgrid;
layout(location = 0) out vec4 out_color;
layout(push_constant) uniform PC {
    vec4 rx, ry, rz;
    vec4 proj;
    vec4 size;
    vec4 fog;
    vec4 fogd;
    vec4 hor;
} pc;
void main() {
    vec3 col = v_col;
    if (pc.fogd.w > 0.5) {
        ivec2 ci = clamp(ivec2(floor(v_cell)), ivec2(0), ivec2(255));
        uint t = texelFetch(tgrid, ci, 0).r & 0x3FFu;
        if (float(t) < pc.fogd.w)
            col = textureGrad(tiles, vec3(v_cell - floor(v_cell), float(t)), dFdx(v_cell), dFdy(v_cell)).rgb * v_shade;
    }
    out_color = vec4(mix(mix(pc.fog.rgb, col, v_fog), pc.hor.rgb, v_band), 1.0);
}
