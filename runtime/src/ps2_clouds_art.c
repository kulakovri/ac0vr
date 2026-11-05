/* ps2_clouds_art.c -- S5 (art) of the cloud replacement (CLOUDS_3D_RESEARCH_2026-10-20.md section 8): the game's own
   cloud, sky and fog colours, captured every game frame from its GS draws, for the colour fields of contract C1
   (lit, shadow, fog, sky_top, sky_hor, fog_near/far, sun) and the art ramps of config_zero/cloud_art.json.

   Zero's cloud shading (the impostor build of the cloud field 0x16A6C0, one build per frame for all clouds on screen;
   draw dumps PS2_DUMP_DRAWS, mission 1):
   - Each of the 4 impostor targets (128x128, slots 3/8/9/10) gets ~64 puff sprites from the puff target (textured,
     blended, render-target texture), then one untextured, unblended quad over the whole target with FBMSK FF000000
     (RGB only; the alpha keeps the cloud shape): the BASE colour.
   - Then up to two overlays: the target's 2:1 alpha copy (slot 7, RGB 128) drawn back ~16 % larger and offset,
     blended (Cs - Cd) As + Cd, RGB only, vertex colour = an overlay colour, vertex alpha = its strength (/128):
     a LIGHT overlay [208,211,213] and a DARK one [38,51,72, the sky's zenith colour], offset in opposite directions.
   - BASE = mix(DARK, LIGHT, t) exactly (per channel): t 0.98 (sun in view: loop/climb), 0.90-0.93, 0.38 and 0.23
     (level flight, sun behind). With t high the dark overlay is strong (a 0.75-0.85) and the light one absent; with
     t low the light overlay is strong (a 0.57-0.78) and the dark one ~0.06. So the game's clouds are a two-tone
     ramp between LIGHT and DARK whose position follows the view against the sun, with an offset rim of the other
     tone. The screen sprites then sample it with vertex RGB 128 (x1.0) and the distance fade in their vertex alpha.
   Published: C1 lit = LIGHT, C1 shadow = DARK (the palette, held per mission), plus the frame's t, overlay
   strengths and offsets, the camera's forward and cos(forward, sun), so the ramp t(cos) can be fitted
   (cloud_art.json) and evaluated per view direction by our renderer instead of per frame (the game's per-frame
   choice would recolour every cloud whenever the head turns).

   Other sources:
   - sky_top / sky_hor: the sky dome (0x161318): untextured Gouraud triangles into the frame, depth ALWAYS, z write,
     no blend, drawn ring by ring from the top down; vertex alpha 127 above the horizon, 0 below. The zenith is the
     centre of the first ring's fan (seen when looking up) [36,51,74]; without it the topmost visible ring is the
     estimate (sky_top_exact 0). The horizon is the alpha-127 vertex of the triangles that straddle the alpha change
     [115,126,145]; below it the ground-side ring starts [106,118,136] (sky_below).
   - fog: the GS FOGCOL register of the frame's fogged world draws (GS path and native meshes), the most frequent
     value; fog_near/far from the native mesh fog coefficients: F = clamp(fog0 + w fog1, 0, 255) (mesh.vert, w = view
     depth in game units, 255 = no fog): fog starts at w = (fog0 - 255) / -fog1, is full at fog0 / -fog1
     [221.7 / -0.0043: -1290 .. 8590 m; a negative near = already 13 % fog at the eye]. 6 game units a metre.
   - haze (logged): the horizon haze ring (0x1615E8): untextured, depth GREATER, blend (Cs - Cd) FIX + Cd.
   - sunq (logged): the tex-395 quad (textured STQ, blend Cs Ad + Cd, abcd 0211, destination alpha test): a quad at
     4096 units in the sun's direction (S1: colour from the colour file +0x30), not a cloud deck; its screen centre is
     a second sun cross-check.
   - sun: the sun disc right after the dome (textured, blend Cs FIX + Cd, abcd 0221, small): its screen centre through
     the game frame's camera (ps2_xr_camera_of: x = cx + fx X/-Z, y = cy - fy Y/-Z, world = R view, y up) gives the
     world direction (C1 sun_dir_disc); its vertex colour the sun colour.
   - in-cloud colour (renderer thread): the immersion cloud measure's five 8x8 patches of the left eye after the
     frame's last cloud sprite (cloud_measure_collect), weighted by how much the clouds changed them, from frames
     inside a cloud only (raw density >= 0.3; before, distant cloud edges at night gave 28,20,14, below the sky).
     Without such a frame C1 gets the palette point mix(DARK, LIGHT, incloud_t).
   Colours are the game's 0..255 values / 255, the space of the frame they are composited into (slot 0 holds the
   PS2's display values; no gamma decode). Sources out of view hold their last value.

   Calls from ps2_video.c (three one-line call-outs): ps2_clouds_art_draw (ps2_vk_draw, every GS draw, GS thread),
   ps2_clouds_art_mesh (ps2_vk_draw_mesh_topo, GS thread), ps2_clouds_art_patches (cloud_measure_collect, renderer).
   Switches: ACZ_CLOUD_ART=0 off; ACZ_DEBUG_CLOUD_ART=N log every N game frames; ACZ_CLOUD_ART_CSV=path one CSV row per
   game frame (research). */
#include "ps2_vk.h"
#include "ps2_clouds.h"
#include "ps2_clouds_art.h"
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern u32 ps2_gs_frame_count;
int ps2_xr_camera_of(u32 seq, float eye[3], float rot[9], float f[2], float c[2]);
void ps2_gs_native_frame(float *xoff, float *yoff, float *zmax);

#define ART_RT 64
#define ART_IMP 8
#define ART_OV 4
#define ART_FOGS 8

typedef struct {
    u32 rt;
    int fill, nov;
    float base[3], tw;
    struct { float rgb[3], a, dx, dy, scale; } ov[ART_OV];
} art_imp;

typedef struct {
    u32 frame, seq;
    u16 puffs[ART_RT];                 /* puff sprites drawn into each target this frame */
    int nimp;
    art_imp imp[ART_IMP];
    /* dome */
    int ndome, ring_ok, fan_n, hor_n, below_n, since_dome;
    float fan_x, fan_y, zen[3], ring[3], hor[3], below[3], t0[3][2], t0c[3][3];
    /* haze ring, the tex-395 sun-direction quad */
    int haze_n, sunq_n;
    float haze[3], haze_fix, sunq[3], sunq_a, sunq_x, sunq_y;
    /* screen cloud sprites */
    int spr_n;
    float spr_rgb[3], spr_w, spr_cover, spr_near_m;
    struct { int seen; u32 tfx, tcc, fge, fogcol, ate, atst, aref, afail, fix, abcd, texrt, tex_point, fb_mask;
             float rmin, rmax, amin, amax; } spr0;      /* the first cloud sprite's GS state (debug) */
    /* fog */
    int nfog;
    struct { u32 col; u32 n; } fog[ART_FOGS];
    int nfc;
    struct { float f0, f1; u32 n; } fc[ART_FOGS];
    /* sun */
    int sun_ok;
    float sun_x, sun_y, sun_rgb[3], sun_fix;
    int tracked;                       /* a head-tracked (tagged) frame: its camera record is the one drawn */
} art_acc;

static art_acc A;
static int art_on = -1, art_dbg, art_inited;
static FILE *art_csv;

/* published state (seqlock) */
static volatile u32 pub_lock;
static ps2_cloud_art pub;
/* held values */
static ps2_cloud_art hold;
static u32 zen_seen_frame;
static int pal_light_ok, pal_dark_ok;
static float frame_off[2];             /* the GS XYOFFSET of the world draws (last seen) */

/* in-cloud colour from the renderer thread */
static volatile u32 ic_lock;
static float ic_rgb[3], ic_density;
static u32 ic_n;

static int art_enabled(void) {
    if (art_on < 0) {
        const char *e = getenv("ACZ_CLOUD_ART");
        art_on = !(e && *e == '0');
        e = getenv("ACZ_DEBUG_CLOUD_ART");
        art_dbg = e ? atoi(e) : 0;
        e = getenv("ACZ_CLOUD_ART_CSV");
        if (art_on && e && *e) {
            art_csv = fopen(e, "w");
            if (art_csv)
                fprintf(art_csv, "frame,seq,valid,nimp,base_r,base_g,base_b,t,light_r,light_g,light_b,a_light,"
                                 "dx_light,dy_light,dark_r,dark_g,dark_b,a_dark,dx_dark,dy_dark,ov_scale,"
                                 "fwd_x,fwd_y,fwd_z,cos_sun,sunv_x,sunv_y,sunv_z,fog_r,fog_g,fog_b,fog_near_m,fog_far_m,"
                                 "top_r,top_g,top_b,top_exact,hor_r,hor_g,hor_b,below_r,below_g,below_b,"
                                 "haze_r,haze_g,haze_b,haze_fix,sunq_r,sunq_g,sunq_b,sunq_a,"
                                 "spr_n,spr_r,spr_g,spr_b,spr_cover,spr_near_m,sun_seen,sun_x,sun_y,sun_z,"
                                 "sun_r,sun_g,sun_b,sun_px,sun_py,ic_r,ic_g,ic_b,ic_density\n");
        }
    }
    return art_on;
}

static float lum3(const float c[3]) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }
static void set3(float d[3], float r, float g, float b) { d[0] = r; d[1] = g; d[2] = b; }
static void cpy3(float d[3], const float s[3]) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }

static void finish_frame(void);
static void c1_write(const ps2_cloud_art *o);

static void begin_frame(u32 frame, u32 seq) {
    if (art_inited) finish_frame();
    memset(&A, 0, sizeof A);
    A.frame = frame;
    A.seq = seq;
    A.spr_near_m = 1e9f;
    art_inited = 1;
}

static void note_fog(u32 col) {
    col &= 0xFFFFFFu;
    for (int i = 0; i < A.nfog; i++) if (A.fog[i].col == col) { A.fog[i].n++; return; }
    if (A.nfog < ART_FOGS) { A.fog[A.nfog].col = col; A.fog[A.nfog].n = 1; A.nfog++; }
}

static int imp_index(u32 rt, int create) {
    for (int i = 0; i < A.nimp; i++) if (A.imp[i].rt == rt) return i;
    if (!create || A.nimp >= ART_IMP) return -1;
    memset(&A.imp[A.nimp], 0, sizeof A.imp[0]);
    A.imp[A.nimp].rt = rt;
    return A.nimp++;
}

/* ps2_vk_draw: every GS draw (GS thread), with its stereo class (0 off-screen, 1 world...) and the frame's seq */
void ps2_clouds_art_draw(const ps2_vk_state *st, const ps2_vk_vertex *v, int n, int cls, u32 seq) {
    if (!art_enabled() || n < 1 || !st) return;
    if (!art_inited || A.frame != ps2_gs_frame_count) begin_frame(ps2_gs_frame_count, seq);
    if (seq) A.seq = seq;
    if (cls >= 1 && st->rt == 0u) {
        A.tracked = 1;
        if (st->zte && (st->ztst == 2u || st->ztst == 3u)) {   /* world draws: the frame's XYOFFSET (as the renderer) */
            float xo, yo, zm;
            ps2_gs_native_frame(&xo, &yo, &zm);
            if (xo > 0.0f && yo > 0.0f) { frame_off[0] = xo; frame_off[1] = yo; }
        }
    }
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (int i = 1; i < n; i++) {
        if (v[i].x < x0) x0 = v[i].x;
        if (v[i].x > x1) x1 = v[i].x;
        if (v[i].y < y0) y0 = v[i].y;
        if (v[i].y > y1) y1 = v[i].y;
    }
    if (st->rt != 0u) {
        if (st->rt >= ART_RT) return;
        /* puff sprites: small textured blended sprites from a render target, pixel UVs */
        if (st->tme && st->abe && st->tex_rt && st->fst && !st->tex_self && x1 - x0 < 48.0f && y1 - y0 < 48.0f) {
            if (A.puffs[st->rt] < 65535u) A.puffs[st->rt]++;
            return;
        }
        if (A.puffs[st->rt] < 8u) return;
        float tw = (float)st->fb_w;
        /* the base colour: untextured, unblended, alpha masked, over the whole target */
        if (!st->tme && !st->abe && (st->fb_mask & 0xFF000000u) == 0xFF000000u && (st->fb_mask & 0xFFFFFFu) == 0u
            && x0 <= 1.0f && x1 >= tw - 1.0f) {
            int k = imp_index(st->rt, 1);
            if (k >= 0) { set3(A.imp[k].base, v[0].r, v[0].g, v[0].b); A.imp[k].fill = 1; A.imp[k].tw = tw; A.imp[k].nov = 0; }
            return;
        }
        /* an overlay: the alpha copy drawn back enlarged and offset, blended, RGB only (one sprite, two triangles) */
        if (st->tme && st->abe && st->tex_rt && (st->fb_mask & 0xFF000000u) == 0xFF000000u
            && (x0 < 0.0f || x1 > tw || y0 < 0.0f || y1 > tw) && st->alpha_a == 0u && st->alpha_b == 1u
            && st->alpha_c == 0u && st->alpha_d == 1u) {
            int k = imp_index(st->rt, 0);
            if (k < 0 || !A.imp[k].fill || tw <= 0.0f) return;
            art_imp *im = &A.imp[k];
            float dx = (0.5f * (x0 + x1) - 0.5f * tw) / tw, dy = (0.5f * (y0 + y1) - 0.5f * tw) / tw;
            for (int j = 0; j < im->nov; j++)                      /* the sprite's second triangle */
                if (fabsf(im->ov[j].dx - dx) < 0.02f && fabsf(im->ov[j].dy - dy) < 0.02f
                    && im->ov[j].rgb[0] == v[0].r && im->ov[j].a == v[0].a / 128.0f) return;
            if (im->nov >= ART_OV) return;
            set3(im->ov[im->nov].rgb, v[0].r, v[0].g, v[0].b);
            im->ov[im->nov].a = v[0].a / 128.0f;
            im->ov[im->nov].dx = dx; im->ov[im->nov].dy = dy;
            im->ov[im->nov].scale = (x1 - x0) / tw;
            im->nov++;
        }
        return;
    }
    /* frame (slot 0) */
    if (st->fge) note_fog(st->fogcol);
    if (!st->tme && st->zte && st->ztst == 1u && st->zwrite && !st->abe && n >= 3) {
        /* the sky dome */
        int hi = 0, lo = 0;
        for (int i = 0; i < n; i++) { if (v[i].a >= 64.0f) hi++; else lo++; }
        if (!A.ring_ok)
            for (int i = 0; i < n; i++) if (v[i].a >= 64.0f) { set3(A.ring, v[i].r, v[i].g, v[i].b); A.ring_ok = 1; break; }
        /* the zenith fan: the first ring is a fan, so its consecutive triangles all share the zenith vertex (strip
           triangles share a vertex with at most two neighbours) */
        if (A.ndome == 0) {
            for (int i = 0; i < 3 && i < n; i++) { A.t0[i][0] = v[i].x; A.t0[i][1] = v[i].y; set3(A.t0c[i], v[i].r, v[i].g, v[i].b); }
        } else if (A.ndome == 1) {
            A.fan_n = 0;
            for (int j = 0; j < 3 && !A.fan_n; j++)
                for (int i = 0; i < n; i++)
                    if (fabsf(v[i].x - A.t0[j][0]) < 0.25f && fabsf(v[i].y - A.t0[j][1]) < 0.25f) {
                        A.fan_x = A.t0[j][0]; A.fan_y = A.t0[j][1]; cpy3(A.zen, A.t0c[j]); A.fan_n = 2; break;
                    }
        } else if (A.fan_n == A.ndome) {
            for (int i = 0; i < n; i++)
                if (fabsf(v[i].x - A.fan_x) < 0.25f && fabsf(v[i].y - A.fan_y) < 0.25f) { A.fan_n++; break; }
        }
        if (hi && lo) {
            for (int i = 0; i < n; i++) {
                if (v[i].a >= 64.0f) { A.hor[0] += v[i].r; A.hor[1] += v[i].g; A.hor[2] += v[i].b; A.hor_n++; }
                else { A.below[0] += v[i].r; A.below[1] += v[i].g; A.below[2] += v[i].b; A.below_n++; }
            }
        }
        A.ndome++;
        A.since_dome = 0;
        return;
    }
    A.since_dome++;
    if (A.ndome && A.since_dome <= 4 && !A.sun_ok && st->tme && st->abe && st->alpha_a == 0u && st->alpha_b == 2u
        && st->alpha_c == 2u && st->alpha_d == 1u && x1 - x0 < 96.0f && y1 - y0 < 96.0f) {
        /* the sun disc */
        A.sun_ok = 1;
        A.sun_x = 0.5f * (x0 + x1); A.sun_y = 0.5f * (y0 + y1);
        set3(A.sun_rgb, v[0].r, v[0].g, v[0].b);
        A.sun_fix = (float)st->alpha_fix / 128.0f;
        return;
    }
    if (!st->tme && st->abe && st->zte && st->ztst == 3u && !st->zwrite && st->alpha_a == 0u && st->alpha_b == 1u
        && st->alpha_c == 2u && st->alpha_d == 1u) {
        /* the horizon haze ring */
        for (int i = 0; i < n; i++) { A.haze[0] += v[i].r; A.haze[1] += v[i].g; A.haze[2] += v[i].b; }
        A.haze_n += n;
        A.haze_fix = (float)st->alpha_fix / 128.0f;
        return;
    }
    if (st->tme && !st->fst && st->abe && st->alpha_a == 0u && st->alpha_b == 2u && st->alpha_c == 1u && st->alpha_d == 1u
        && st->date) {
        /* the tex-395 sun-direction quad (not a cloud deck) */
        for (int i = 0; i < n; i++) {
            A.sunq[0] += v[i].r; A.sunq[1] += v[i].g; A.sunq[2] += v[i].b; A.sunq_a += v[i].a;
            A.sunq_x += v[i].x; A.sunq_y += v[i].y;
        }
        A.sunq_n += n;
        return;
    }
    if (st->tex_rt && !st->tex_self && st->tme && st->fst && st->abe && st->zte && st->ztst >= 2u && !st->zwrite) {
        /* the screen cloud sprites: vertex colour (128 = x1) and coverage */
        float al = 0.0f, iw = 0.0f;
        if (!A.spr0.seen) {
            A.spr0.seen = 1;
            A.spr0.tfx = st->tfx; A.spr0.tcc = (u32)st->tcc; A.spr0.fge = (u32)st->fge; A.spr0.fogcol = st->fogcol;
            A.spr0.ate = (u32)st->ate; A.spr0.atst = st->atst; A.spr0.aref = st->aref; A.spr0.afail = st->afail;
            A.spr0.fix = st->alpha_fix; A.spr0.texrt = st->tex_rt; A.spr0.tex_point = (u32)st->tex_point;
            A.spr0.fb_mask = st->fb_mask;
            A.spr0.abcd = st->alpha_a << 12 | st->alpha_b << 8 | st->alpha_c << 4 | st->alpha_d;
            A.spr0.rmin = A.spr0.amin = 1e9f; A.spr0.rmax = A.spr0.amax = -1e9f;
        }
        for (int i = 0; i < n; i++) {
            if (v[i].r < A.spr0.rmin) A.spr0.rmin = v[i].r;
            if (v[i].r > A.spr0.rmax) A.spr0.rmax = v[i].r;
            if (v[i].a < A.spr0.amin) A.spr0.amin = v[i].a;
            if (v[i].a > A.spr0.amax) A.spr0.amax = v[i].a;
        }
        for (int i = 0; i < n; i++) {
            float w = v[i].a / 128.0f;
            A.spr_rgb[0] += v[i].r * w; A.spr_rgb[1] += v[i].g * w; A.spr_rgb[2] += v[i].b * w; A.spr_w += w;
            al += v[i].a;
            if (v[i].xr_iw > iw) iw = v[i].xr_iw;
        }
        al /= (float)n * 128.0f;
        float W = (float)(st->scissor[1] - st->scissor[0] + 1), H = (float)(st->scissor[3] - st->scissor[2] + 1);
        if (W > 0.0f && H > 0.0f && n >= 3) A.spr_cover += 0.5f * al * (x1 - x0) * (y1 - y0) / (W * H);   /* triangles */
        if (iw > 0.0f && 1.0f / iw / 6.0f < A.spr_near_m) A.spr_near_m = 1.0f / iw / 6.0f;
        A.spr_n++;
    }
}

/* ps2_vk_draw_mesh_topo: native mesh draws (GS thread): fog colour and coefficients */
void ps2_clouds_art_mesh(const ps2_vk_state *st, const ps2_vk_mesh_block *b) {
    if (!art_enabled() || !st || !b) return;
    if (!art_inited || A.frame != ps2_gs_frame_count) begin_frame(ps2_gs_frame_count, 0);
    if (st->rt != 0u) return;
    if (st->fge) note_fog(st->fogcol);
    if (b->fog1 < 0.0f && b->fog0 > 0.0f) {
        u32 wgt = b->prog == 1u ? 4u : 1u;                  /* prefer the terrain program's */
        for (int i = 0; i < A.nfc; i++)
            if (A.fc[i].f0 == b->fog0 && A.fc[i].f1 == b->fog1) { A.fc[i].n += wgt; return; }
        if (A.nfc < ART_FOGS) { A.fc[A.nfc].f0 = b->fog0; A.fc[A.nfc].f1 = b->fog1; A.fc[A.nfc].n = wgt; A.nfc++; }
    }
}

/* cloud_measure_collect (renderer thread): the five patches (RGBA8, npix each) before and after the clouds */
void ps2_clouds_art_patches(const u8 *before, const u8 *after, int npatch, int npix, float density) {
    if (!art_enabled() || !before || !after || npatch <= 0 || npix <= 0) return;
    float s[3] = { 0, 0, 0 }, ws = 0.0f;
    for (int k = 0; k < npatch; k++) {
        const u8 *pb = before + (size_t)k * (size_t)npix * 4u, *pa = after + (size_t)k * (size_t)npix * 4u;
        float m[3] = { 0, 0, 0 }, ch = 0.0f;
        for (int j = 0; j < npix; j++)
            for (int c = 0; c < 3; c++) { m[c] += pa[j * 4 + c]; ch += fabsf((float)pa[j * 4 + c] - (float)pb[j * 4 + c]); }
        ch /= (float)npix * 3.0f;
        float w = ch > 4.0f && density >= 0.3f ? ch : 0.0f;   /* inside a cloud (the measure's density), and only
                                                                   the patches the clouds changed */
        for (int c = 0; c < 3; c++) s[c] += w * m[c] / (float)npix;
        ws += w;
    }
    __atomic_add_fetch(&ic_lock, 1u, __ATOMIC_ACQ_REL);
    if (ws > 0.0f) {
        float a = ic_n ? 0.15f : 1.0f;
        for (int c = 0; c < 3; c++) ic_rgb[c] += a * (s[c] / ws - ic_rgb[c]);
        ic_n++;
    }
    ic_density = density;
    __atomic_add_fetch(&ic_lock, 1u, __ATOMIC_ACQ_REL);
}

static void finish_frame(void) {
    ps2_cloud_art o = hold;
    o.frame = A.frame;
    o.seq = A.seq;
    o.valid &= ~(u32)(PS2_CLOUD_ART_SEEN_LIT | PS2_CLOUD_ART_SEEN_SKY | PS2_CLOUD_ART_SEEN_SUN | PS2_CLOUD_ART_SEEN_FOG);
    /* the camera of this frame: forward (world) */
    float eye[3], R[9], f[2], cc[2];
    int cam = A.tracked && A.seq && ps2_xr_camera_of(A.seq, eye, R, f, cc) && f[0] > 0.0f && f[1] > 0.0f;
    if (cam) {
        /* the record's centre is in GS primitive coordinates (2048, 2048); the draws are in frame pixels */
        if (frame_off[0] > 0.0f) { cc[0] -= frame_off[0]; cc[1] -= frame_off[1]; }
        else { cc[0] -= 2048.0f - 256.0f; cc[1] -= 2048.0f - 224.0f; }
    }
    if (cam) set3(o.view_fwd, -R[2], -R[5], -R[8]);
    /* sun */
    if (A.sun_ok) {
        o.valid |= PS2_CLOUD_ART_SEEN_SUN;
        o.sun_px[0] = A.sun_x; o.sun_px[1] = A.sun_y;
        for (int c = 0; c < 3; c++) o.sun_rgb[c] = A.sun_rgb[c] / 128.0f;
        o.sun_fix = A.sun_fix;
        if (cam) {
            float d[3] = { (A.sun_x - cc[0]) / f[0], (cc[1] - A.sun_y) / f[1], -1.0f };
            float w[3];
            for (int i = 0; i < 3; i++) w[i] = R[3 * i] * d[0] + R[3 * i + 1] * d[1] + R[3 * i + 2] * d[2];
            float l = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            if (l > 1e-6f) {
                /* the sun moves very little within a mission: smooth the per-frame rounding of its screen box */
                float a = (o.valid & PS2_CLOUD_ART_SUN) ? 0.2f : 1.0f;
                for (int i = 0; i < 3; i++) o.sun_dir_disc[i] += a * (w[i] / l - o.sun_dir_disc[i]);
                float m = sqrtf(o.sun_dir_disc[0] * o.sun_dir_disc[0] + o.sun_dir_disc[1] * o.sun_dir_disc[1] + o.sun_dir_disc[2] * o.sun_dir_disc[2]);
                if (m > 1e-6f) for (int i = 0; i < 3; i++) o.sun_dir_disc[i] /= m;
                o.valid |= PS2_CLOUD_ART_SUN;
            }
        }
    }
    o.cos_view_sun = (cam && (o.valid & PS2_CLOUD_ART_SUN))
        ? o.view_fwd[0] * o.sun_dir_disc[0] + o.view_fwd[1] * o.sun_dir_disc[1] + o.view_fwd[2] * o.sun_dir_disc[2] : -2.0f;
    if (cam && (o.valid & PS2_CLOUD_ART_SUN))          /* the sun in this frame's view axes (x right, y up, z back) */
        for (int i = 0; i < 3; i++) o.sun_view[i] = R[i] * o.sun_dir_disc[0] + R[3 + i] * o.sun_dir_disc[1] + R[6 + i] * o.sun_dir_disc[2];
    else o.sun_view[0] = o.sun_view[1] = o.sun_view[2] = 0.0f;
    /* clouds: the first impostor with a base colour (all four share the frame's shading) */
    const art_imp *im = NULL;
    o.nimp = 0;
    for (int i = 0; i < A.nimp; i++) if (A.imp[i].fill) { if (!im) im = &A.imp[i]; o.nimp++; }
    if (im) {
        o.valid |= PS2_CLOUD_ART_SEEN_LIT;
        for (int c = 0; c < 3; c++) o.base[c] = im->base[c] / 255.0f;
        float lb = lum3(im->base);
        int li = -1, di = -1;
        for (int j = 0; j < im->nov; j++) {
            if (lum3(im->ov[j].rgb) > lb) { if (li < 0 || im->ov[j].a > im->ov[li].a) li = j; }
            else if (di < 0 || im->ov[j].a > im->ov[di].a) di = j;
        }
        o.a_light = o.a_dark = 0.0f;
        o.off_light[0] = o.off_light[1] = o.off_dark[0] = o.off_dark[1] = 0.0f;
        if (li >= 0) {
            for (int c = 0; c < 3; c++) o.lit[c] = im->ov[li].rgb[c] / 255.0f;
            o.a_light = im->ov[li].a; o.off_light[0] = im->ov[li].dx; o.off_light[1] = im->ov[li].dy;
            o.ov_scale = im->ov[li].scale;
            pal_light_ok = 1;
        }
        if (di >= 0) {
            for (int c = 0; c < 3; c++) o.shadow[c] = im->ov[di].rgb[c] / 255.0f;
            o.a_dark = im->ov[di].a; o.off_dark[0] = im->ov[di].dx; o.off_dark[1] = im->ov[di].dy;
            o.ov_scale = im->ov[di].scale;
            pal_dark_ok = 1;
        }
        if (!pal_light_ok) cpy3(o.lit, o.base);             /* until a light overlay was seen: the base */
        if (!pal_dark_ok) cpy3(o.shadow, o.base);
        o.valid |= PS2_CLOUD_ART_LIT | PS2_CLOUD_ART_SHADOW;
        if (pal_light_ok && pal_dark_ok) {
            float num = 0.0f, den = 0.0f;
            for (int c = 0; c < 3; c++) {
                float d = o.lit[c] - o.shadow[c];
                num += (o.base[c] - o.shadow[c]) * d; den += d * d;
            }
            o.game_t = den > 1e-6f ? num / den : 0.0f;
            o.valid |= PS2_CLOUD_ART_T;
        } else o.valid &= ~(u32)PS2_CLOUD_ART_T;
        /* what the screen shows at most / least (full mask alpha) */
        for (int c = 0; c < 3; c++) {
            float b = o.base[c], hi = b + (o.lit[c] - b) * o.a_light, lo = b + (o.shadow[c] - b) * o.a_dark;
            o.eff_light[c] = hi > b ? hi : b;
            o.eff_dark[c] = lo < b ? lo : b;
        }
    }
    /* sky */
    if (A.ndome) {
        o.valid |= PS2_CLOUD_ART_SEEN_SKY;
        if (A.fan_n >= 6) {
            for (int c = 0; c < 3; c++) o.sky_top[c] = A.zen[c] / 255.0f;
            o.sky_top_exact = 1;
            zen_seen_frame = A.frame;
            o.valid |= PS2_CLOUD_ART_SKY_TOP;
        } else if (A.ring_ok && (!(o.valid & PS2_CLOUD_ART_SKY_TOP) || !o.sky_top_exact
                                 || A.frame - zen_seen_frame > 600u)) {
            /* the topmost visible ring: smoothed (it moves with the view pitch) */
            float a = (o.valid & PS2_CLOUD_ART_SKY_TOP) && !o.sky_top_exact ? 0.1f : 1.0f;
            for (int c = 0; c < 3; c++) o.sky_top[c] += a * (A.ring[c] / 255.0f - o.sky_top[c]);
            o.sky_top_exact = 0;
            o.valid |= PS2_CLOUD_ART_SKY_TOP;
        }
        if (A.hor_n) {
            for (int c = 0; c < 3; c++) o.sky_hor[c] = A.hor[c] / (float)A.hor_n / 255.0f;
            o.valid |= PS2_CLOUD_ART_SKY_HOR;
        }
        if (A.below_n) for (int c = 0; c < 3; c++) o.sky_below[c] = A.below[c] / (float)A.below_n / 255.0f;
    }
    o.ndome = A.ndome;
    if (A.haze_n) { for (int c = 0; c < 3; c++) o.haze[c] = A.haze[c] / (float)A.haze_n / 255.0f; o.haze_fix = A.haze_fix; }
    if (A.sunq_n) {
        for (int c = 0; c < 3; c++) o.sunq[c] = A.sunq[c] / (float)A.sunq_n / 255.0f;
        o.sunq_a = A.sunq_a / (float)A.sunq_n / 128.0f;
        o.sunq_px[0] = A.sunq_x / (float)A.sunq_n; o.sunq_px[1] = A.sunq_y / (float)A.sunq_n;
    }
    o.sunq_n = A.sunq_n;
    /* fog */
    if (A.nfog) {
        int b = 0;
        for (int i = 1; i < A.nfog; i++) if (A.fog[i].n > A.fog[b].n) b = i;
        u32 col = A.fog[b].col;
        set3(o.fog, (float)(col & 0xFF) / 255.0f, (float)((col >> 8) & 0xFF) / 255.0f, (float)((col >> 16) & 0xFF) / 255.0f);
        o.valid |= PS2_CLOUD_ART_FOG | PS2_CLOUD_ART_SEEN_FOG;
    }
    if (A.nfc) {
        int b = 0;
        for (int i = 1; i < A.nfc; i++) if (A.fc[i].n > A.fc[b].n) b = i;
        o.fog0 = A.fc[b].f0; o.fog1 = A.fc[b].f1;
        o.fog_near_m = (o.fog0 - 255.0f) / -o.fog1 / 6.0f;
        o.fog_far_m = o.fog0 / -o.fog1 / 6.0f;
        o.valid |= PS2_CLOUD_ART_FOG_RANGE;
    }
    /* screen sprites */
    o.spr_n = A.spr_n;
    o.spr_cover = A.spr_cover;
    o.spr_near_m = A.spr_n ? A.spr_near_m : 0.0f;
    if (A.spr_w > 0.0f) for (int c = 0; c < 3; c++) o.spr_rgb[c] = A.spr_rgb[c] / A.spr_w / 128.0f;
    /* in-cloud colour (renderer thread) */
    {
        u32 s0, s1;
        float rgb[3], dn; u32 n;
        int tries = 0;
        do {
            s0 = __atomic_load_n(&ic_lock, __ATOMIC_ACQUIRE);
            cpy3(rgb, ic_rgb); dn = ic_density; n = ic_n;
            s1 = __atomic_load_n(&ic_lock, __ATOMIC_ACQUIRE);
        } while ((s0 != s1 || (s0 & 1u)) && ++tries < 8);
        if (n) {
            for (int c = 0; c < 3; c++) o.incloud[c] = rgb[c] / 255.0f;
            o.valid |= PS2_CLOUD_ART_INCLOUD;
        }
        o.incloud_density = dn;
    }
    hold = o;
    __atomic_add_fetch(&pub_lock, 1u, __ATOMIC_ACQ_REL);
    pub = o;
    __atomic_add_fetch(&pub_lock, 1u, __ATOMIC_ACQ_REL);
    c1_write(&o);

    if (art_dbg > 0 && (A.frame % (u32)art_dbg) == 0u) {
#define C255(x) (int)lrintf((x) * 255.0f)
        ps2_log("cloud-art: f %u seq %u valid %04X | base %d,%d,%d t %.3f | light %d,%d,%d a %.2f off %+.3f,%+.3f | "
                "dark %d,%d,%d a %.2f off %+.3f,%+.3f | scale %.2f (%d imp) | fwd %+.2f,%+.2f,%+.2f cos(sun) %+.3f",
                o.frame, o.seq, o.valid, C255(o.base[0]), C255(o.base[1]), C255(o.base[2]), (double)o.game_t,
                C255(o.lit[0]), C255(o.lit[1]), C255(o.lit[2]), (double)o.a_light, (double)o.off_light[0],
                (double)o.off_light[1], C255(o.shadow[0]), C255(o.shadow[1]), C255(o.shadow[2]), (double)o.a_dark,
                (double)o.off_dark[0], (double)o.off_dark[1], (double)o.ov_scale, o.nimp, (double)o.view_fwd[0],
                (double)o.view_fwd[1], (double)o.view_fwd[2], (double)o.cos_view_sun);
        ps2_log("cloud-art: f %u fog %d,%d,%d %.0f..%.0f m (%.4g %.4g) | sky top %d,%d,%d %s hor %d,%d,%d below %d,%d,%d "
                "(%d dome) | haze %d,%d,%d fix %.2f | sunquad %d,%d,%d a %.2f px %.0f,%.0f (%d) | sprites %d vtx %.2f,%.2f,%.2f cover %.2f "
                "near %.0f m | sun %s %+.3f %+.3f %+.3f rgb %.2f,%.2f,%.2f px %.0f,%.0f | incloud %d,%d,%d d %.2f",
                o.frame, C255(o.fog[0]), C255(o.fog[1]), C255(o.fog[2]), (double)o.fog_near_m, (double)o.fog_far_m,
                (double)o.fog0, (double)o.fog1, C255(o.sky_top[0]), C255(o.sky_top[1]), C255(o.sky_top[2]),
                o.sky_top_exact ? "zenith" : "ring", C255(o.sky_hor[0]), C255(o.sky_hor[1]), C255(o.sky_hor[2]),
                C255(o.sky_below[0]), C255(o.sky_below[1]), C255(o.sky_below[2]), A.ndome, C255(o.haze[0]),
                C255(o.haze[1]), C255(o.haze[2]), (double)o.haze_fix, C255(o.sunq[0]), C255(o.sunq[1]), C255(o.sunq[2]),
                (double)o.sunq_a, (double)o.sunq_px[0], (double)o.sunq_px[1], o.sunq_n, o.spr_n, (double)o.spr_rgb[0], (double)o.spr_rgb[1], (double)o.spr_rgb[2],
                (double)o.spr_cover, (double)o.spr_near_m,
                A.sun_ok ? ((o.valid & PS2_CLOUD_ART_SUN) ? "dir" : "seen") : "-", (double)o.sun_dir_disc[0],
                (double)o.sun_dir_disc[1], (double)o.sun_dir_disc[2], (double)o.sun_rgb[0], (double)o.sun_rgb[1],
                (double)o.sun_rgb[2], (double)o.sun_px[0], (double)o.sun_px[1], C255(o.incloud[0]), C255(o.incloud[1]),
                C255(o.incloud[2]), (double)o.incloud_density);
        if (A.spr0.seen)
            ps2_log("cloud-art: f %u sprite state: tfx %u tcc %u fge %u fogcol %06X ate %u atst %u aref %u afail %u "
                    "abcd %04X fix %u texrt %u point %u fbmsk %08X | vertex r %.0f..%.0f a %.0f..%.0f", o.frame,
                    A.spr0.tfx, A.spr0.tcc, A.spr0.fge, A.spr0.fogcol, A.spr0.ate, A.spr0.atst, A.spr0.aref, A.spr0.afail,
                    A.spr0.abcd, A.spr0.fix, A.spr0.texrt, A.spr0.tex_point, A.spr0.fb_mask, (double)A.spr0.rmin,
                    (double)A.spr0.rmax, (double)A.spr0.amin, (double)A.spr0.amax);
        int differ = 0;
        for (int i = 1; i < A.nimp; i++)
            if (A.imp[i].fill && im && (A.imp[i].base[0] != im->base[0] || A.imp[i].nov != im->nov)) differ = 1;
        if (differ) {
            char b[320]; int p = 0;
            for (int i = 0; i < A.nimp && p < 260; i++)
                p += snprintf(b + p, sizeof b - (size_t)p, " rt%u base %.0f,%.0f,%.0f ov %d", A.imp[i].rt,
                              (double)A.imp[i].base[0], (double)A.imp[i].base[1], (double)A.imp[i].base[2], A.imp[i].nov);
            ps2_log("cloud-art: f %u impostors differ:%s", o.frame, b);
        }
#undef C255
    }
    if (art_csv) {
        fprintf(art_csv, "%u,%u,%u,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.4f,%.4f,"
                         "%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.1f,%.1f,%.4f,%.4f,%.4f,%d,%.4f,%.4f,%.4f,"
                         "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.3f,%.4f,%.4f,%.4f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.1f,%d,"
                         "%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%.1f,%.1f,%.4f,%.4f,%.4f,%.3f\n",
                o.frame, o.seq, o.valid, o.nimp, o.base[0], o.base[1], o.base[2], o.game_t, o.lit[0], o.lit[1],
                o.lit[2], o.a_light, o.off_light[0], o.off_light[1], o.shadow[0], o.shadow[1], o.shadow[2], o.a_dark,
                o.off_dark[0], o.off_dark[1], o.ov_scale, o.view_fwd[0], o.view_fwd[1], o.view_fwd[2], o.cos_view_sun,
                o.sun_view[0], o.sun_view[1], o.sun_view[2], o.fog[0], o.fog[1], o.fog[2], o.fog_near_m, o.fog_far_m, o.sky_top[0], o.sky_top[1], o.sky_top[2],
                o.sky_top_exact, o.sky_hor[0], o.sky_hor[1], o.sky_hor[2], o.sky_below[0], o.sky_below[1],
                o.sky_below[2], o.haze[0], o.haze[1], o.haze[2], o.haze_fix, o.sunq[0], o.sunq[1], o.sunq[2],
                o.sunq_a, o.spr_n, o.spr_rgb[0], o.spr_rgb[1], o.spr_rgb[2], o.spr_cover, o.spr_near_m, A.sun_ok,
                o.sun_dir_disc[0], o.sun_dir_disc[1], o.sun_dir_disc[2], o.sun_rgb[0], o.sun_rgb[1], o.sun_rgb[2], o.sun_px[0],
                o.sun_px[1], o.incloud[0], o.incloud[1], o.incloud[2], o.incloud_density);
        if ((o.frame & 63u) == 0u) fflush(art_csv);
    }
}

/* any thread: the last finished game frame's colours; 0 when nothing was captured yet */
int ps2_clouds_art_get(ps2_cloud_art *out) {
    if (!art_enabled() || !out) return 0;
    u32 s0, s1;
    int tries = 0;
    do {
        s0 = __atomic_load_n(&pub_lock, __ATOMIC_ACQUIRE);
        *out = pub;
        s1 = __atomic_load_n(&pub_lock, __ATOMIC_ACQUIRE);
    } while ((s0 != s1 || (s0 & 1u)) && ++tries < 16);
    return out->frame != 0u && out->valid != 0u;
}

/* ---- config_zero/cloud_art.json: per-kind art ramps (S2/S3 shading parameters) ----
   A minimal JSON reader for this file's shape (objects, arrays, numbers, strings). Kind selection: "missions"
   (mission number -> kind) first, then "env_type" (raw env set +0x190 value -> kind), else "default"; a kind inherits
   "default" and overrides keys. Curves are given as [[cos, value], ...] points and resampled to PS2_CLOUD_RAMP_N
   evenly spaced samples over cos(view, sun) = -1..+1. ACZ_CLOUD_ART_JSON=path overrides the file. */
typedef struct jv {
    int type;                       /* 0 null, 1 number, 2 string, 3 array, 4 object */
    double num;
    char str[48];
    int first, next;                /* children (array items / object members), sibling */
    char key[48];
} jv;
#define JV_MAX 1024
static jv jv_pool[JV_MAX];
static int jv_n;
static const char *jp;

static void j_ws(void) { while (*jp == ' ' || *jp == '\t' || *jp == '\n' || *jp == '\r') jp++; }
static int j_str(char *out, size_t cap) {
    if (*jp != '"') return -1;
    jp++;
    size_t k = 0;
    while (*jp && *jp != '"') {
        if (*jp == '\\' && jp[1]) jp++;
        if (k + 1 < cap) out[k++] = *jp;
        jp++;
    }
    out[k] = 0;
    if (*jp != '"') return -1;
    jp++;
    return 0;
}
static int j_val(void) {
    j_ws();
    if (jv_n >= JV_MAX) return -1;
    int me = jv_n++;
    jv *v = &jv_pool[me];
    memset(v, 0, sizeof *v);
    v->first = v->next = -1;
    if (*jp == '{' || *jp == '[') {
        int obj = *jp == '{';
        v->type = obj ? 4 : 3;
        jp++;
        int last = -1;
        for (;;) {
            j_ws();
            if (*jp == (obj ? '}' : ']')) { jp++; break; }
            char key[48] = "";
            if (obj) {
                if (j_str(key, sizeof key)) return -1;
                j_ws();
                if (*jp != ':') return -1;
                jp++;
            }
            int c = j_val();
            if (c < 0) return -1;
            strcpy(jv_pool[c].key, key);
            if (last < 0) jv_pool[me].first = c; else jv_pool[last].next = c;
            last = c;
            j_ws();
            if (*jp == ',') { jp++; continue; }
            if (*jp == (obj ? '}' : ']')) { jp++; break; }
            return -1;
        }
        return me;
    }
    if (*jp == '"') { v->type = 2; return j_str(v->str, sizeof v->str) ? -1 : me; }
    if (!strncmp(jp, "true", 4)) { v->type = 1; v->num = 1; jp += 4; return me; }
    if (!strncmp(jp, "false", 5)) { v->type = 1; v->num = 0; jp += 5; return me; }
    if (!strncmp(jp, "null", 4)) { v->type = 0; jp += 4; return me; }
    char *end;
    v->num = strtod(jp, &end);
    if (end == jp) return -1;
    v->type = 1;
    jp = end;
    return me;
}
static int j_get(int obj, const char *key) {
    if (obj < 0 || jv_pool[obj].type != 4) return -1;
    for (int c = jv_pool[obj].first; c >= 0; c = jv_pool[c].next) if (!strcmp(jv_pool[c].key, key)) return c;
    return -1;
}

static int ramp_loaded = -1, ramp_root = -1;
static void ramp_load(void) {
    if (ramp_loaded >= 0) return;
    ramp_loaded = 0;
    const char *path = getenv("ACZ_CLOUD_ART_JSON");
    if (!path || !*path) path = "config_zero/cloud_art.json";
    FILE *fp = fopen(path, "rb");
    if (!fp) { ps2_log("cloud-art: %s not found; built-in ramp", path); return; }
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof buf - 1, fp);
    fclose(fp);
    buf[n] = 0;
    jp = buf; jv_n = 0;
    ramp_root = j_val();
    if (ramp_root < 0 || jv_pool[ramp_root].type != 4) {
        ps2_log("cloud-art: %s: parse error near byte %d; built-in ramp", path, (int)(jp - buf));
        ramp_root = -1;
        return;
    }
    ramp_loaded = 1;
    ps2_log("cloud-art: art ramps from %s (%d values)", path, jv_n);
}

/* resample [[x, y], ...] (x ascending) to n samples over x = -1..+1 */
static void ramp_curve(int arr, float *out, int n) {
    if (arr < 0 || jv_pool[arr].type != 3) return;
    float xs[16], ys[16];
    int m = 0;
    for (int c = jv_pool[arr].first; c >= 0 && m < 16; c = jv_pool[c].next) {
        int a = jv_pool[c].first;
        if (a < 0 || jv_pool[a].next < 0) continue;
        xs[m] = (float)jv_pool[a].num; ys[m] = (float)jv_pool[jv_pool[a].next].num; m++;
    }
    if (!m) return;
    for (int i = 0; i < n; i++) {
        float x = -1.0f + 2.0f * (float)i / (float)(n - 1), y = ys[0];
        if (x >= xs[m - 1]) y = ys[m - 1];
        else for (int k = 0; k + 1 < m; k++)
            if (x >= xs[k] && x <= xs[k + 1]) {
                float u = xs[k + 1] > xs[k] ? (x - xs[k]) / (xs[k + 1] - xs[k]) : 0.0f;
                y = ys[k] + u * (ys[k + 1] - ys[k]);
                break;
            }
        out[i] = y;
    }
}

static void ramp_apply(int obj, ps2_cloud_ramp *r) {
    if (obj < 0) return;
    static const struct { const char *k; size_t off; } F[] = {
#define RF(name) { #name, offsetof(ps2_cloud_ramp, name) }
        RF(rim_light), RF(rim_dark), RF(rim_scale), RF(cloud_frac), RF(incloud_t),
        RF(ambient_top), RF(ambient_base), RF(fog_scale), RF(fog_max), RF(opacity_max), RF(edge_softness),
#undef RF
        { "base_period_m", offsetof(ps2_cloud_ramp, shape) }, { "detail_period_m", offsetof(ps2_cloud_ramp, shape) + 4 },
        { "erosion", offsetof(ps2_cloud_ramp, shape) + 8 }, { "billow", offsetof(ps2_cloud_ramp, shape) + 12 },
        { "base_darkening", offsetof(ps2_cloud_ramp, shade) }, { "thin_rim", offsetof(ps2_cloud_ramp, shade) + 4 },
        { "dark_core", offsetof(ps2_cloud_ramp, shade) + 8 }, { "dark_copy", offsetof(ps2_cloud_ramp, shade) + 12 },
    };
    for (size_t i = 0; i < sizeof F / sizeof F[0]; i++) {
        int c = j_get(obj, F[i].k);
        if (c >= 0 && jv_pool[c].type == 1) *(float *)((char *)r + F[i].off) = (float)jv_pool[c].num;
    }
    {   int c = j_get(obj, "near_art");                 /* S3's four near-only values, as given */
        if (c >= 0 && jv_pool[c].type == 3) {
            int i = 0;
            for (int k = jv_pool[c].first; k >= 0 && i < 4; k = jv_pool[k].next) r->near_art[i++] = (float)jv_pool[k].num;
        }
    }
    static const char *pk[2] = { "near_fade_m", "far_fade_m" };
    for (int q = 0; q < 2; q++) {
        int c = j_get(obj, pk[q]);
        float *dst = q ? r->far_fade_m : r->near_fade_m;
        if (c >= 0 && jv_pool[c].type == 3 && jv_pool[c].first >= 0 && jv_pool[jv_pool[c].first].next >= 0) {
            dst[0] = (float)jv_pool[jv_pool[c].first].num;
            dst[1] = (float)jv_pool[jv_pool[jv_pool[c].first].next].num;
        }
    }
    /* the colour curves: [[c, value], ...] points, or {"a": .., "b": .., "min": .., "max": ..} = clamp(a + b c) */
    static const char *ck[3] = { "t_curve", "a_light_curve", "a_dark_curve" };
    float *cd[3] = { r->t_curve, r->a_light_curve, r->a_dark_curve };
    for (int q = 0; q < 3; q++) {
        int c = j_get(obj, ck[q]);
        if (c < 0) continue;
        if (jv_pool[c].type == 3) ramp_curve(c, cd[q], PS2_CLOUD_RAMP_N);
        else if (jv_pool[c].type == 4) {
            int ka = j_get(c, "a"), kb = j_get(c, "b"), kn = j_get(c, "min"), kx = j_get(c, "max");
            float A0 = ka >= 0 ? (float)jv_pool[ka].num : 0.0f, B0 = kb >= 0 ? (float)jv_pool[kb].num : 0.0f;
            float lo = kn >= 0 ? (float)jv_pool[kn].num : 0.0f, hi = kx >= 0 ? (float)jv_pool[kx].num : 1.0f;
            for (int i = 0; i < PS2_CLOUD_RAMP_N; i++) {
                float x = -1.0f + 2.0f * (float)i / (float)(PS2_CLOUD_RAMP_N - 1), y = A0 + B0 * x;
                cd[q][i] = y < lo ? lo : (y > hi ? hi : y);
            }
        }
    }
}

/* the ramp for a mission (1..18, 0 unknown) and the stage's raw env set cloud type (-1 unknown); with the sun below the
   horizon (sun_y < 0) the kind named by "sun_below" is applied on top (the game's night ramp differs) */
int ps2_clouds_art_ramp(int mission, int env_type, ps2_cloud_ramp *r) { return ps2_clouds_art_ramp_sun(mission, env_type, 1.0f, r); }
int ps2_clouds_art_ramp_sun(int mission, int env_type, float sun_y, ps2_cloud_ramp *r) {
    if (!r) return 0;
    memset(r, 0, sizeof *r);
    /* built-in defaults: the game's own model, measured in mission 1 (cloud_art.json overrides) */
    for (int i = 0; i < PS2_CLOUD_RAMP_N; i++) {
        float x = -1.0f + 2.0f * (float)i / (float)(PS2_CLOUD_RAMP_N - 1);
        float t = 0.578f + 0.429f * x, al = 0.291f - 0.598f * x, ad = 0.322f + 0.562f * x;
        r->t_curve[i] = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        r->a_light_curve[i] = al < 0.0f ? 0.0f : (al > 1.0f ? 1.0f : al);
        r->a_dark_curve[i] = ad < 0.0f ? 0.0f : (ad > 1.0f ? 1.0f : ad);
    }
    r->rim_light = 0.14f; r->rim_dark = 0.167f; r->rim_scale = 1.167f; r->cloud_frac = 0.6f;
    r->ambient_top = 0.0f; r->ambient_base = 0.0f; r->incloud_t = 0.52f;
    r->shade[0] = 0.15f; r->shade[1] = 0.5f; r->shade[2] = 0.25f; r->shade[3] = 1.0f;   /* B's cl_shade defaults */
    r->fog_scale = 1.0f; r->fog_max = 1.0f; r->opacity_max = 0.97f; r->edge_softness = 0.35f;
    r->near_fade_m[0] = 350.0f; r->near_fade_m[1] = 1000.0f; r->far_fade_m[0] = 3000.0f; r->far_fade_m[1] = 6000.0f;
    r->near_art[0] = 0.8f; r->near_art[1] = 0.5f; r->near_art[2] = 1.0f; r->near_art[3] = 0.6f;   /* S3's defaults */
    strcpy(r->kind, "default");
    if (!art_enabled()) return 1;
    ramp_load();
    if (ramp_root < 0) return 1;
    ramp_apply(j_get(ramp_root, "default"), r);
    const char *kind = NULL;
    char key[16];
    if (mission > 0) {
        snprintf(key, sizeof key, "%d", mission);
        int c = j_get(j_get(ramp_root, "missions"), key);
        if (c >= 0 && jv_pool[c].type == 2) kind = jv_pool[c].str;
    }
    if (!kind && env_type >= 0) {
        snprintf(key, sizeof key, "%d", env_type);
        int c = j_get(j_get(ramp_root, "env_type"), key);
        if (c >= 0 && jv_pool[c].type == 2) kind = jv_pool[c].str;
    }
    if (kind) {
        int k = j_get(j_get(ramp_root, "kinds"), kind);
        if (k >= 0) { ramp_apply(k, r); snprintf(r->kind, sizeof r->kind, "%s", kind); }
    }
    int sb = j_get(ramp_root, "sun_below");
    if (sun_y < -0.02f && sb >= 0 && jv_pool[sb].type == 2 && (!kind || strcmp(kind, jv_pool[sb].str))) {
        int k = j_get(j_get(ramp_root, "kinds"), jv_pool[sb].str);
        if (k >= 0) { ramp_apply(k, r); snprintf(r->kind, sizeof r->kind, "%s", jv_pool[sb].str); }
    }
    return 1;
}

/* ---- C1 (ps2_clouds.h): the colour fields, written into the staging copy once per game frame (GS thread); S1
   publishes it. lit / shadow = the mission's LIGHT / DARK palette; shadow_a = the dark rim's strength at its
   strongest (a_dark at c = +1); shape = the kind's cloud_art.json shape (0 = B's defaults). The colour model's
   coefficients (ps2_clouds_art_ramp_pack) are in ps2_clouds_art_ramp_packed until C1 has a field for them.
   Sources never seen in this mission fall back: fog colour -> sky_hor, sky_top -> shadow, fog range -> the game's
   mission-1 values. If no other module publishes C1 for 30 game frames (S1 absent), S5 publishes itself.
   ACZ_CLOUD_ART_C1=0: capture and log only, no C1 writes. ---- */
#define ART_SAVE 0x003B3810u                      /* campaign slots (as S1: slot A +0x50, B +0x5FE4, selector +0xC04C) */
static int art_mission(void) {
    u32 s = ps2_r8(ART_SAVE + 0xC04Cu) ? ART_SAVE + 0x5FE4u : ART_SAVE + 0x50u;
    int idx = (int)ps2_r32(s + 0x5F48u);
    return idx >= 0 && idx < 32 ? idx + 1 : 0;
}

/* the colour model as clamp(a + b c): pk[0] = t a, b, a_light a, b; pk[1] = a_dark a, b, s_light, s_dark (rim offsets
   along the sun as fractions of a cloud's diameter); pk[2] = far fade start / end (m), opacity_max, edge_softness */
void ps2_clouds_art_ramp_pack(const ps2_cloud_ramp *r, float pk[3][4]) {
    const float *cv[3] = { r->t_curve, r->a_light_curve, r->a_dark_curve };
    float ab[3][2];
    for (int q = 0; q < 3; q++) {                 /* least squares over the unclamped samples */
        double sx = 0, sy = 0, sxx = 0, sxy = 0; int n = 0;
        for (int i = 0; i < PS2_CLOUD_RAMP_N; i++) {
            float y = cv[q][i], x = -1.0f + 2.0f * (float)i / (float)(PS2_CLOUD_RAMP_N - 1);
            if (y <= 0.0f || y >= 1.0f) continue;
            sx += x; sy += y; sxx += (double)x * x; sxy += (double)x * y; n++;
        }
        double d = n * sxx - sx * sx;
        ab[q][1] = n >= 2 && d > 1e-9 ? (float)((n * sxy - sx * sy) / d) : 0.0f;
        ab[q][0] = n ? (float)((sy - ab[q][1] * sx) / n) : cv[q][PS2_CLOUD_RAMP_N / 2];
    }
    float frac = r->cloud_frac > 0.05f ? r->cloud_frac : 0.6f;
    pk[0][0] = ab[0][0]; pk[0][1] = ab[0][1]; pk[0][2] = ab[1][0]; pk[0][3] = ab[1][1];
    pk[1][0] = ab[2][0]; pk[1][1] = ab[2][1]; pk[1][2] = r->rim_light / frac; pk[1][3] = r->rim_dark / frac;
    pk[2][0] = r->far_fade_m[0]; pk[2][1] = r->far_fade_m[1]; pk[2][2] = r->opacity_max; pk[2][3] = r->edge_softness;
}

static ps2_cloud_ramp cur_ramp;
static int cur_ramp_key = -9999;
static float art_k_cur[4] = { 0.578f, 0.429f, 0.291f, 0.322f };
volatile float ps2_clouds_art_ramp_packed[3][4];

/* the current ramp (any thread; a torn copy only mixes two ramps of the same mission) */
int ps2_clouds_art_current(ps2_cloud_ramp *out) {
    if (!out || cur_ramp_key == -9999) return 0;
    *out = cur_ramp;
    return 1;
}

static void c1_write(const ps2_cloud_art *o) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ACZ_CLOUD_ART_C1"); on = !(e && *e == '0'); }
    if (!on || !(o->valid & (PS2_CLOUD_ART_LIT | PS2_CLOUD_ART_SKY_TOP | PS2_CLOUD_ART_FOG))) return;
    ps2_cloud_env *e = ps2_cloud_env_edit();
    /* the ramp of this mission and sun state (sun below the horizon: the night variant) */
    float sun_y = (e->valid & CL_ENV_SUN) ? e->sun_dir[1] : ((o->valid & PS2_CLOUD_ART_SUN) ? o->sun_dir_disc[1] : 1.0f);
    int mission = art_mission();
    int key = mission * 2 + (sun_y < -0.02f);
    if (key != cur_ramp_key) {
        ps2_clouds_art_ramp_sun(mission, -1, sun_y, &cur_ramp);
        cur_ramp_key = key;
        float pk[3][4];
        ps2_clouds_art_ramp_pack(&cur_ramp, pk);
        memcpy((void *)ps2_clouds_art_ramp_packed, pk, sizeof pk);
        art_k_cur[0] = pk[0][0]; art_k_cur[1] = pk[0][1]; art_k_cur[2] = pk[0][2]; art_k_cur[3] = pk[1][0];
        ps2_log("cloud-art: mission %d, sun %s: ramp '%s' t %.3f%+.3fc a_light %.3f%+.3fc a_dark %.3f%+.3fc rim %.2f/%.2f "
                "shape %.0f %.0f %.2f %.2f shade %.2f %.2f %.2f %.2f near_art %.2f %.2f %.2f %.2f", mission, sun_y < -0.02f ? "below the horizon" : "up", cur_ramp.kind,
                (double)pk[0][0], (double)pk[0][1], (double)pk[0][2], (double)pk[0][3], (double)pk[1][0],
                (double)pk[1][1], (double)pk[1][2], (double)pk[1][3], (double)cur_ramp.shape[0],
                (double)cur_ramp.shape[1], (double)cur_ramp.shape[2], (double)cur_ramp.shape[3],
                (double)cur_ramp.shade[0], (double)cur_ramp.shade[1], (double)cur_ramp.shade[2], (double)cur_ramp.shade[3],
                (double)cur_ramp.near_art[0], (double)cur_ramp.near_art[1], (double)cur_ramp.near_art[2],
                (double)cur_ramp.near_art[3]);
    }
    if (o->valid & PS2_CLOUD_ART_LIT) {
        float fog[3];
        memcpy(fog, (o->valid & PS2_CLOUD_ART_FOG) ? o->fog : ((o->valid & PS2_CLOUD_ART_SKY_HOR) ? o->sky_hor : o->lit), sizeof fog);
        memcpy(e->lit, o->lit, sizeof e->lit);
        memcpy(e->shadow, o->shadow, sizeof e->shadow);
        memcpy(e->sky_top, (o->valid & PS2_CLOUD_ART_SKY_TOP) ? o->sky_top : o->shadow, sizeof e->sky_top);
        memcpy(e->sky_hor, (o->valid & PS2_CLOUD_ART_SKY_HOR) ? o->sky_hor : fog, sizeof e->sky_hor);
        memcpy(e->fog, fog, sizeof fog);
        if (o->valid & PS2_CLOUD_ART_FOG_RANGE) { e->fog_near_m = o->fog_near_m; e->fog_far_m = o->fog_far_m; }
        else if (!(e->valid & CL_ENV_COLOURS)) { e->fog_near_m = -1290.0f; e->fog_far_m = 8590.0f; }
        e->valid |= CL_ENV_COLOURS;
        e->shadow_a = cur_ramp.a_dark_curve[PS2_CLOUD_RAMP_N - 1];
        e->valid |= CL_ENV_SHADOW_A;
    }
    if (o->valid & PS2_CLOUD_ART_SKY_HOR) {
        memcpy(e->sky_below, o->sky_below[0] + o->sky_below[1] + o->sky_below[2] > 0.0f ? o->sky_below : o->sky_hor,
               sizeof e->sky_below);
        e->valid |= CL_ENV_SKY_BELOW;
    }
    /* in-cloud colour: the measured one while the game's own cloud sprites are drawn; with them dropped (our clouds)
       the patches would measure our own clouds (a loop), and before the first measurement: the palette point the
       game's in-cloud view sits on (mission 1: measured 127,135,146 = mix(DARK, LIGHT, 0.52) to 0.2 levels) */
    {   const ps2_cloud_cfg *cc = ps2_clouds_cfg();
        int ours = cc && cc->drop_game;
        if ((o->valid & PS2_CLOUD_ART_INCLOUD) && !ours) memcpy(e->incloud, o->incloud, sizeof e->incloud);
        else if (o->valid & PS2_CLOUD_ART_LIT)
            for (int c = 0; c < 3; c++) e->incloud[c] = o->shadow[c] + (o->lit[c] - o->shadow[c]) * cur_ramp.incloud_t;
        if ((o->valid & PS2_CLOUD_ART_INCLOUD) || (o->valid & PS2_CLOUD_ART_LIT)) e->valid |= CL_ENV_INCLOUD_COL;
    }
    memcpy(e->shape, cur_ramp.shape, sizeof e->shape);
    if (cur_ramp.shape[0] > 0.0f) e->valid |= CL_ENV_SHAPE; else e->valid &= ~(uint32_t)CL_ENV_SHAPE;
    memcpy(e->shade, cur_ramp.shade, sizeof e->shade);
    e->valid |= CL_ENV_SHADE;
    memcpy(e->near_art, cur_ramp.near_art, sizeof e->near_art);
    if (cur_ramp.near_art[0] != 0.0f || cur_ramp.near_art[1] != 0.0f || cur_ramp.near_art[2] != 0.0f
        || cur_ramp.near_art[3] != 0.0f) e->valid |= CL_ENV_NEAR_ART;
    else e->valid &= ~(uint32_t)CL_ENV_NEAR_ART;
    /* art_k (user[3]): the colour model's per-mission constants t = clamp(k.x + k.y c), aL = clamp(k.z - 0.598 c),
       aD = clamp(k.w + 0.562 c) (the slopes are the same day and night; the night's t is lower) */
    memcpy(e->art_k, art_k_cur, sizeof e->art_k);
    e->valid |= CL_ENV_ART_K;
    ps2_cloud_env_done();
    /* nobody else publishing (S1 not in this build): publish ourselves */
    static uint32_t last_seq; static int idle, said;
    static ps2_cloud_env tmp;
    uint32_t seq = ps2_cloud_env_read(&tmp) ? tmp.seq : 0u;
    if (art_dbg > 0 && (o->frame % (u32)art_dbg) == 0u) {
#define K255(x) (int)lrintf((x) * 255.0f)
        ps2_log("cloud-art: C1 as published: seq %u valid %04X lit %d,%d,%d shadow %d,%d,%d a %.2f fog %d,%d,%d %.0f..%.0f m "
                "sky %d,%d,%d / %d,%d,%d / %d,%d,%d incloud %d,%d,%d sun %+.2f %+.2f %+.2f | shape %.0f %.0f %.2f %.2f "
                "shade %.2f %.2f %.2f %.2f near %.2f %.2f %.2f %.2f art_k %.3f %.3f %.3f %.3f", tmp.seq, tmp.valid,
                K255(tmp.lit[0]), K255(tmp.lit[1]), K255(tmp.lit[2]), K255(tmp.shadow[0]), K255(tmp.shadow[1]),
                K255(tmp.shadow[2]), (double)tmp.shadow_a, K255(tmp.fog[0]), K255(tmp.fog[1]), K255(tmp.fog[2]),
                (double)tmp.fog_near_m, (double)tmp.fog_far_m, K255(tmp.sky_top[0]), K255(tmp.sky_top[1]),
                K255(tmp.sky_top[2]), K255(tmp.sky_hor[0]), K255(tmp.sky_hor[1]), K255(tmp.sky_hor[2]),
                K255(tmp.sky_below[0]), K255(tmp.sky_below[1]), K255(tmp.sky_below[2]), K255(tmp.incloud[0]),
                K255(tmp.incloud[1]), K255(tmp.incloud[2]), (double)tmp.sun_dir[0], (double)tmp.sun_dir[1],
                (double)tmp.sun_dir[2], (double)tmp.shape[0], (double)tmp.shape[1], (double)tmp.shape[2],
                (double)tmp.shape[3], (double)tmp.shade[0], (double)tmp.shade[1], (double)tmp.shade[2], (double)tmp.shade[3],
                (double)tmp.near_art[0], (double)tmp.near_art[1], (double)tmp.near_art[2], (double)tmp.near_art[3],
                (double)tmp.art_k[0], (double)tmp.art_k[1], (double)tmp.art_k[2], (double)tmp.art_k[3]);
#undef K255
    }
    if (seq != last_seq) { last_seq = seq; idle = 0; }
    else if (++idle > 30) {
        ps2_cloud_env_publish(0);
        if (!said++) ps2_log("cloud-art: no module publishes C1 (S1 absent?): S5 publishes it once per game frame");
        last_seq = ps2_cloud_env_read(&tmp) ? tmp.seq : 0u;
        idle = 31;
    }
}
