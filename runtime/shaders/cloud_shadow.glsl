/* cloud_shadow.glsl - the cloud shadow map lookup (ACZ_VR_CLOUD_SHADOW; built by ps2_clouds_shadow.c with
   cloud_shadow.comp). Self-contained: cloud_common.glsl includes it with its own binding and CloudFrame copy, and any
   other shader (terrain, fog) can include it alone, bind the map and pass the three parameter vectors that
   ps2_clouds_shadow_hook() returns on the C side (see ps2_clouds.h).

   The map is a sun-aligned deep opacity map of the cloud layer over the 20 km weather window, toroidal like the weather
   map (uv = cloud-frame x/z / 20 km, sampled LINEAR + REPEAT), so it is world-fixed and identical for both eyes. Each
   texel is one light ray: the line toward the light (the sun; at night the side the game lights its clouds from), its
   elevation clamped to at least 8 degrees, through the point where it crosses the band's base plane at the texel's x/z.
   Per texel (RGBA16F), from the light down along that ray:
     R  altitude (m) of the first cloud the ray meets (the top of the shadow caster); <= the band base: no cloud
     G  optical depth from the light down to D1 below R
     B  optical depth down to D2 below R
     A  optical depth down to the band base: the whole column (what the ground and anything below the layer gets)
   Between those points the optical depth is linear in altitude (deep opacity maps, Yuksel & Keyser 2008: the layers
   start at the first occluder, so a cloud's own top gets the fine steps wherever it sits in a thick band).
   Parameters:
     s0  xy: the map uv of the frame's centre eye (cloud-frame x/z / map size, fractional), z: 1 / map size (m),
         w: optical depth scale (the strength; 0 = no map: the lookup returns weight 0)
     s1  xy: the light ray's horizontal run per metre of rise (light.xz / light.y), z: band base (m of altitude),
         w: altitude of the centre eye (m)
     s2  x: D1 (m), y: D2 (m), z: start of the edge fade (m: the larger of |x| and |z| of the ray's base point from the
         eye), w: 1 / the fade width (m; negative = the clouds' debug view, ACZ_VR_CLOUD_SHADOW_VIEW); the map is only
         valid inside: beyond, the caller falls back (weight 0)
   p_m: METRES, y up, relative to the centre eye (cloud space; any camera-relative world vector in metres). */
#ifndef CLOUD_SHADOW_GLSL
#define CLOUD_SHADOW_GLSL

/* optical depth (unscaled) at altitude y of one texel t whose band base is base and offsets d1 < d2 */
float cl_shadow_eval(vec4 t, float y, float base, float d1, float d2) {
    float s = t.r - y;                                     /* depth below the first occluder along the ray (m) */
    if (s <= 0.0) return 0.0;
    if (s <= d1) return t.g * (s / d1);
    if (s <= d2) return mix(t.g, t.b, (s - d1) / (d2 - d1));
    return mix(t.b, t.a, clamp((s - d2) / max(t.r - d2 - base, 1.0), 0.0, 1.0));
}
/* scaled optical depth toward the light at p_m and the map weight (1 inside, 0 beyond the edge or without a map) */
float cl_shadow_od_map(sampler2D m, vec4 s0, vec4 s1, vec4 s2, vec3 p_m, out float weight) {
    float alt = s1.w + p_m.y;
    vec2 q = p_m.xz - s1.xy * (alt - s1.z);               /* where the light ray through p_m crosses the base plane */
    weight = s0.w > 0.0 ? 1.0 - clamp((max(abs(q.x), abs(q.y)) - s2.z) * abs(s2.w), 0.0, 1.0) : 0.0;
    if (weight <= 0.0) return 0.0;
    return cl_shadow_eval(textureLod(m, s0.xy + q * s0.z, 0.0), alt, s1.z, s2.x, s2.y) * s0.w;
}
/* transmittance for an optical depth, softened for multiple scattering (Nubis: the brighter of Beer's law and a
   0.7 x exp(-od / 4) tail), so the inside of a thick cloud keeps a gradient instead of going black */
float cl_shadow_t(float od) { return max(exp(-od), 0.7 * exp(-0.25 * od)); }
/* THE HOOK for terrain and fog: the sun's transmittance through the cloud layer at p_m, 1 = full sun (also beyond the
   map's edge and without a map) */
float cl_sun_shadow_map(sampler2D m, vec4 s0, vec4 s1, vec4 s2, vec3 p_m) {
    float w, od = cl_shadow_od_map(m, s0, s1, s2, p_m, w);
    return mix(1.0, cl_shadow_t(od), w);
}

#endif
