/* PS2_TERRAIN_ZCENSUS (perf5_terrain, diagnostic; the variants mesh_zcen*.frag and gs_zcen*.frag, compiled with
   -DZCENSUS and taken only by the native terrain's draws while the census is on): counts the terrain passes' fragments
   by what their blend can still contribute, atomically into the reserved mesh blocks ZC_BLOCK0 and ZC_BLOCK0 + 1
   (ps2_video.c zcen_*: the frame's arena stops below them while the census runs). Per pass of the block (0 the atlas,
   1-2 the detail textures, 3 the world-XZ layer), 8 copies by gl_FragCoord.x & 7 (less contention), categories:
   0 fragments run (helper invocations do not count: their atomics have no effect), 1 reached the output (not discarded
   by DATE or the alpha test), and of those 2 the interpolated vertex colour rgb <= 0, 3 the texel rgb <= 0, 4 the
   output rgb <= 0 (the overlay passes' blend Cd - Cs x FIX leaves the target exactly as it is), 5 the output rgb > 0
   but under half a level in every channel (unchanged only through the blend's rounding: not exact), 6 the texel rgb
   <= 0 while the colour is not (what a fragment discard after the texture fetch could still save). The block's pass
   is uint 81 and its program uint 84 of the 120 (ps2_vk_mesh_block: checked at start-up) */
#define ZC_BLOCK0 15864u
layout(std430, set = 2, binding = 0) buffer ZcBlocks { uint zc_w[]; };
uint zc_pass = 0u;
bool zc_terr = false;
void zc_add(uint cat) {
    if (!zc_terr) return;
    atomicAdd(zc_w[ZC_BLOCK0 * 120u + ((uint(gl_FragCoord.x) & 7u) * 4u + zc_pass) * 7u + cat], 1u);
}
void zc_begin(uint blk) {
    zc_pass = zc_w[blk * 120u + 81u] & 3u;
    zc_terr = zc_w[blk * 120u + 84u] == 1u;
    zc_add(0u);
}
/* col: the interpolated vertex colour, tex: the texel (x < -0.5: untextured), o: the output colour before the clamp */
void zc_end(vec3 col, vec3 tex, vec3 o) {
    zc_add(1u);
    bool cz = col.r <= 0.0 && col.g <= 0.0 && col.b <= 0.0;
    bool tz = tex.r > -0.5 && tex.r <= 0.0 && tex.g <= 0.0 && tex.b <= 0.0;
    bool oz = o.r <= 0.0 && o.g <= 0.0 && o.b <= 0.0;
    if (cz) zc_add(2u);
    if (tz) zc_add(3u);
    if (oz) zc_add(4u);
    else if (max(o.r, max(o.g, o.b)) < 0.5 / 255.0) zc_add(5u);
    if (tz && !cz) zc_add(6u);
}
