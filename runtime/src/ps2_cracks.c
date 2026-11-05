// ps2_cracks.c - canopy cracks from hits (the user's wish 2026-10-23; agent fx_cracks; CRACKS_RESEARCH_2026-10-23.md
// sections 4-5). Shared by ac5 and gsreplay (gsreplay never gets a hit event: nothing to draw there).
//
// What it does (renderer thread unless noted):
// - Glass: at the cockpit model's load ps2_video.c passes every glass mesh's triangles; the canopy and windscreen
//   meshes (not the HUD's combiner, the gauge glasses or an inner copy: the fx_glass rule) keep them, with their open
//   border (edges of one triangle: where the glass meets the frame) and a material from the cockpit's file and the
//   mesh's name (CRACKS_RESEARCH 1.1 / 4.2: stretched acrylic by default; the F-16's acrylic-over-polycarbonate
//   laminate; the F-22's polycarbonate; glass-laminate windscreens on the F-14, A-10 and the MiG-21's front panel).
// - Hits (zero_cracks.c, EE thread -> a ring): energy E = damage share x kind factor (missile 1, gun 0.8, special 1,
//   other 0.5) x ACZ_VR_CANOPY_CRACKS. The source direction is cast from the eye (the cockpit frame's origin) onto the
//   glass: an impact there. A missile's proximity burst gives one larger impact plus a few small fragment impacts
//   around it (Poisson, ~6 E x the glass's share of a 30 deg cone); a gun hit one impact, now and then a second small
//   one; hits of one burst (same attacker within 0.3 s) grow the same impact. A direction that misses the glass
//   (below the rail, the floor) with E > 0.15 starts a stress crack at the nearest frame edge instead (real unloaded
//   canopies crack at their fasteners). At most ACZ_VR_CANOPY_CRACK_MAX (8) main impacts: more hits grow the nearest.
// - Out of sight (ACZ_VR_CANOPY_CRACK_SHOCK, lb_f15 2026-10-26): a heavy hit (E > 0.15) whose crack would land behind
//   the headrest (more than ACZ_VR_CANOPY_CRACK_SHOCK_Z, 0.30 m, behind the eye: the canopy's tail over the spine, which
//   the seat hides from a pilot looking ahead) or nowhere at all also puts a short stress crack at the canopy's bow
//   (its forward frame) on the burst's side at eye height (the border point nearest az 55 +- 5 deg, el +5 deg from the
//   eye): the flexing canopy cracks at its fasteners, and there it is seen against the sky at the edge of a forward
//   look (lower, at the sill, it lay against the ground and was hard to see); a missile also chips the side glass aft
//   of the shoulder now and then (Poisson 2 E, at most 2). A
//   frame crack that would only sit behind the headrest is replaced by it; an impact star there stays (it is where
//   the fragments struck). The F-15C's missiles from dead astern (playtest 2026-10-26: three hits, 38 % of its HP
//   each) left nothing a pilot looking ahead could see. The frame point's test also accepts the glass the eye meets
//   first within 3 cm (the F-15C's canopy has two shells, CanopyGlass and the lower CanopyGlassInside: a border point
//   of the outer one failed behind the inner one), and a frame crack runs into the glass square to the border (it ran
//   towards the border triangle's centroid, i.e. along the edge under the frame for the sliver triangles at a canopy's
//   sill; that way stays the fallback: crk_frame_start).
// - The pattern, in the impact's tangent plane (u along the glass towards the tail, v across; cockpit.frag projects
//   the same way, so it is the same in both eyes and needs no UVs): N radial arms, N = N_m (E / 0.3)^1/4 (after the
//   star-crack law N ~ v^1/2 with E ~ v^2), length L_m E^1/2, each a random walk (steps 3-8 mm, kinks of a few
//   degrees) pulled radial near the hit and, beyond three core radii, weakly towards fore-aft (the hoop stress of a
//   pressurised, sucked canopy); branches at 25-40 deg more often far out (mirror -> mist -> hackle); concentric
//   chords between neighbouring arms at 3-6 glass thicknesses above a threshold energy; a crushed whitened core and
//   a halo of micro-cracks; holes for polycarbonate. Arms stop at the frame (3 mm from the border) and at earlier
//   cracks (a T-junction: the forensic rule that later cracks end on earlier ones). The whole star forms in under a
//   millisecond; it is revealed outward over ACZ_VR_CANOPY_CRACK_REVEAL (40 ms) so it reads as cracking.
// - Gunsight rule (ACZ_VR_CANOPY_CRACK_CLEAR, 8 deg): no core / hole within that cone of the HUD combiner seen from
//   the eye (the impact is moved to the cone's edge); arms crossing it are drawn at half contrast. The milky area
//   (cores and halos) is capped at 4 % of the glass.
// - Creep (ACZ_VR_CANOPY_CRACK_CREEP, acrylic only): load = cabin differential (0 below 8,000 ft, 5 psi from 23,000 ft)
//   + 0.7 dynamic pressure + 2 kPa a G above 3 G + 8 kPa x buffet; speed 1 mm/s x (load / 15 kPa - 1)^2 (0 in a 350 kt
//   cruise at 10,000 ft; a hard turn at altitude, ~30 kPa: 1 mm/s), at most 3 mm/s, in pops of 3-15 mm with a faint
//   tick, at most 35 % of the star's first length (a real pursuit at altitude, M10, ran at 21-47 kPa).
// - Sound (ACZ_IMM_HIT_FX sound): synthesised, binaural from the impact: acrylic = a 2-4 ms snap and a 1.5-4 kHz ring
//   with a short crackle; glass laminate a duller crunch; polycarbonate a thud and a tick; creep pops faint ticks.
//   Rumble (rumble): one 40 ms jolt on the small motor at a crack.
// - Debris chips (bug #139, the user's wish 2026-10-27 "flying past debris ... touching and scratching the glass; take
//   the physics into account"; ACZ_VR_CANOPY_CHIPS): every canopy hit of zero_vr.c's debris model (wish #47: the bits of
//   a kill's explosion cloud and the wreck's bits met on the aircraft's path -- a Poisson draw from their density, the
//   canopy's area and the relative path each frame -- and the chunks whose own drag-and-gravity path passes within
//   1.2 m; event ring ps2_imm_deb_ev kinds 3 tick / 4 clank, with the impact's direction from the eye, the relative
//   velocity and a strength) leaves a mark where the piece meets the glass, sized by its kinetic energy (strength x
//   (relative speed / 200 m/s)^2; a chunk ~10x a bit): a pit (0.5-1.2 mm crushed core for a bit, 1.5-3.5 mm for a
//   chunk) with a halo, a few micro-cracks (a bit now and then 1-2 of 2-6 mm; a chunk a small star of 3-5 arms of
//   6-20 mm), and the scratch the piece leaves skidding on: from the pit along the relative airflow over the glass
//   (aft, the impact plane's u axis, +-8 deg), 1.5-6 cm for a bit, 4-12 cm for a chunk (x the relative speed / 200
//   m/s), a groove 50-80 / 100-160 um wide (drawn >= 1.3 px with its energy kept: faint), broken where the piece
//   bounced. They are drawn by the crack layer (the same lines, glints near the sun, thickness band left out: a groove
//   in the surface, not a crack through it), at most ACZ_VR_CANOPY_CHIP_MAX (6) at once (a new one replaces the oldest
//   chip; the hits' cracks always take precedence), no sound or rumble of their own (zero_vr.c's tick / clank and the
//   debris rumble are the sound), for the mission.
//   Grit (bug #189, ACZ_IMM_SMOKE_PASS: the fine grit in a kill's smoke ball, event kind 5): a micro-pit (0.15-0.4 mm
//   crushed core, E = strength x 0.2 x (v / 200 m/s)^2, no micro-cracks) and a fine short scratch skidding aft, 0.8-3.2
//   cm (x v / 200 m/s), a 40-90 um groove (a grain's contact width): a thin silver line against the sky, near the sun
//   a glint of +10-30 levels on its 1.3 px line (sim, 4x); the same slots as the chips.
// - Persistence: for the mission; cleared with the canopy water at a mission start, RETRY, a jump or the aircraft's restore
//   after a crash (a checkpoint respawn: zero_cracks.c, bug #133) (ps2_imm_reset)
//   and when the cockpit model changes.
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include "ps2_cpuprof.h"        /* CPU zones (PS2_CPU_PROF, diagnostic) */
#include "ps2_cracks.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

u64 ps2_kernel_vblank_count(void);
int ps2_spatial_hrtf_on(void);
extern volatile float ps2_imm_in[8];      /* [2] airspeed kt, [3] field stamp, [7] altitude m (zero_vr.c) */
extern volatile float ps2_gf_vis[8];      /* [2] buffet 0..1, [3] stamp, [4] first person, [5] G (zero_vr.c gf_update) */
extern volatile u32 ps2_imm_reset;        /* bumps at a mission start / RETRY / restart / respawn (zero_vr.c; a
                                             jump of the aircraft too only with ACZ_IMM_RESET_KEEP=0, bug #268) */

#define CRK_PI 3.14159265f
#define CRK_MESH 32
#define CRK_TIPS 96
#define CRK_EV_RING 16u

/* ---------------------------------------------------------------- switches */
static struct {
    int init; float strength; int max_main; float clear_deg; float reveal_s; int creep; char mat[256];
    int fx_sound, fx_rumble; int dbg, show, ab;
    int shock; float shock_z;
    float chips; int chip_max;                /* bug #139: ACZ_VR_CANOPY_CHIPS, ACZ_VR_CANOPY_CHIP_MAX */
    int boot_on;                              /* the start's strength > 0: the glass is prepared at a cockpit's load */
} cs;
static void crk_read(void) {                  /* the switches of the NEXT MISSION tier (ps2_menv.h) */
    const char *e = ps2_menv("ACZ_VR_CANOPY_CRACKS");
    cs.strength = e && *e ? (float)atof(e) : 1.0f;
    if (e && (!strcmp(e, "off"))) cs.strength = 0.0f;
    if (!(cs.strength >= 0.0f)) cs.strength = 0.0f;
    if (cs.strength > 3.0f) cs.strength = 3.0f;
    e = ps2_menv("ACZ_VR_CANOPY_CRACK_CREEP"); cs.creep = !(e && *e == '0');
    e = ps2_menv("ACZ_IMM_HIT_FX");
    if (!e || !*e || !strcmp(e, "1") || !strcmp(e, "on")) cs.fx_sound = cs.fx_rumble = 1;
    else { cs.fx_sound = strstr(e, "sound") != NULL; cs.fx_rumble = strstr(e, "rumble") != NULL; }
}
static void crk_cfg(void) {
    if (cs.init) return;
    cs.init = 1;
    const char *e = ps2_menv_boot("ACZ_VR_CANOPY_CRACKS");
    {   float b = e && *e ? (float)atof(e) : 1.0f; if (e && !strcmp(e, "off")) b = 0.0f; cs.boot_on = b > 0.0f; }
    crk_read();
    e = getenv("ACZ_VR_CANOPY_CRACK_MAX"); cs.max_main = e ? atoi(e) : 8;
    if (cs.max_main < 1) cs.max_main = 1;
    if (cs.max_main > 12) cs.max_main = 12;
    e = getenv("ACZ_VR_CANOPY_CRACK_CLEAR"); cs.clear_deg = e ? (float)atof(e) : 8.0f;
    if (!(cs.clear_deg >= 0.0f)) cs.clear_deg = 0.0f;
    if (cs.clear_deg > 30.0f) cs.clear_deg = 30.0f;
    e = getenv("ACZ_VR_CANOPY_CRACK_REVEAL"); cs.reveal_s = (e ? (float)atof(e) : 40.0f) * 0.001f;
    if (!(cs.reveal_s >= 0.0f)) cs.reveal_s = 0.0f;
    if (cs.reveal_s > 2.0f) cs.reveal_s = 2.0f;
    e = getenv("ACZ_VR_CANOPY_CRACK_MATERIAL"); snprintf(cs.mat, sizeof cs.mat, "%s", e && *e ? e : "auto");
    e = getenv("ACZ_DEBUG_CANOPY_CRACKS"); cs.dbg = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_CANOPY_CRACKS_SHOW"); cs.show = e ? atoi(e) : 0;
    e = getenv("ACZ_VR_CANOPY_CRACK_AB"); cs.ab = e ? atoi(e) : 0;
    if (cs.ab < 0) cs.ab = 0;
    e = getenv("ACZ_VR_CANOPY_CRACK_SHOCK"); cs.shock = !(e && *e == '0');
    e = getenv("ACZ_VR_CANOPY_CRACK_SHOCK_Z"); cs.shock_z = e && *e ? (float)atof(e) : 0.30f;
    if (!(cs.shock_z > 0.05f)) cs.shock_z = 0.05f;
    e = getenv("ACZ_VR_CANOPY_CHIPS"); cs.chips = e && *e ? (float)atof(e) : 1.0f;
    if (!(cs.chips >= 0.0f)) cs.chips = 0.0f;
    if (cs.chips > 3.0f) cs.chips = 3.0f;
    e = getenv("ACZ_VR_CANOPY_CHIP_MAX"); cs.chip_max = e && *e ? atoi(e) : 6;
    if (cs.chip_max < 1) cs.chip_max = 1;
    if (cs.chip_max > 8) cs.chip_max = 8;
    ps2_log("cracks: canopy cracks %s (strength %.2f, at most %d impacts, gunsight cone %.0f deg, reveal %.0f ms, creep %s, "
            "material %s, hit effects%s%s; out-of-sight hits %s; debris chips %.2f, at most %d)", cs.strength > 0.0f ? "on" : "off",
            cs.strength, cs.max_main, cs.clear_deg, cs.reveal_s * 1000.0f, cs.creep ? "on" : "off", cs.mat,
            cs.fx_sound ? " sound" : "", cs.fx_rumble ? " rumble" : "", cs.shock ? "crack the canopy's bow on their side" : "as before",
            cs.chips, cs.chip_max);
}
int ps2_crk_on(void) { crk_cfg(); return cs.strength > 0.0f; }
int ps2_crk_boot_on(void) { crk_cfg(); return cs.boot_on; }
void ps2_crk_menv_reload(void) {
    if (!cs.init) return;
    const float a = cs.strength; const int b = cs.creep, d = cs.fx_sound + 2 * cs.fx_rumble;
    crk_read();
    ps2_menv_note("ACZ_VR_CANOPY_CRACKS", a, cs.strength);
    ps2_menv_note("ACZ_VR_CANOPY_CRACK_CREEP", b, cs.creep);
    ps2_menv_note("ACZ_IMM_HIT_FX (mask)", d, cs.fx_sound + 2 * cs.fx_rumble);
}
/* ACZ_IMM_DEBRIS_LOOK (fx_debris 2026-10-31, bug #260: the user flew through three fresh kills in the headset -- M1 84 m
   ahead through the middle 0.45 s later, M10 15 m from it 2 s later with the burning wreck passing the canopy -- and
   saw "no dust, no debris ... no effect", "debris doesn't scratch the glass"; the sim's marks of such a pass: soot
   flecks of 5-9 mm gone in 2.8 s, 0.5-0.7 mm pits with 50-80 um scratches (0.1 alpha at the headset's pixel), a soot
   film of 0.04-0.11 alpha). Level 0..2 (1; 0 = as before) of what such a pass leaves where it can be seen: the bits'
   hit rate x (1 + 2 L) (at most 6 a kill), a bit's chip a small stone chip (pit x (1 + 1.2 L), 1-3 micro-cracks
   always, the scratch x (1 + 1.5 L) wide), grit pits x (1 + L) with scratches x (1 + 1.2 L) wide, the soot flecks
   x 1.5 across, darker and x (1 + 1.5 L) longer on the glass, and the loose dust of a smoke pass (2 L x the soot
   deposited, blown off in ~8 s) over the stuck soot. Shared by zero_vr.c (rates), here (chips), ps2_video.c (flecks,
   dust) and cockpit.frag (the flecks' size) */
float ps2_imm_debris_look(void) {
    static float k = -1.0f;
    if (k < 0.0f) {
        const char *e = getenv("ACZ_IMM_DEBRIS_LOOK");
        k = e && *e ? (float)atof(e) : 1.0f;
        if (e && !strcmp(e, "off")) k = 0.0f;
        if (!(k >= 0.0f)) k = 0.0f;
        if (k > 2.0f) k = 2.0f;
    }
    return k;
}
static double crk_now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}
int ps2_crk_ab_off(void) {
    static int last = -1;
    crk_cfg();
    if (!cs.ab) return 0;
    int ph = (int)((long long)(crk_now() / (double)cs.ab) & 1);
    if (ph != last) { last = ph; ps2_log("cracks A/B: %s", ph ? "cracks off" : "cracks on"); }
    return ph;
}

/* ---------------------------------------------------------------- hit ring (EE -> renderer) */
static crk_hit hit_ring[CRK_EV_RING];
static volatile u32 hit_seq;
void ps2_crk_hit_push(const crk_hit *h) {
    u32 s = hit_seq;
    hit_ring[s % CRK_EV_RING] = *h;
    __atomic_store_n(&hit_seq, s + 1u, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------- materials */
enum { MAT_ACRYLIC = 0, MAT_PCLAM = 1, MAT_PC = 2, MAT_GLASS = 3 };
typedef struct {
    const char *name;
    float arms, arms_lo, arms_hi;   /* N_m and its range */
    float len;                      /* L_m (m) at E = 1 (L = L_m E^1/2) */
    float run_p;                    /* chance an arm runs on (x2.2: towards the frame) */
    float step_lo, step_hi;         /* walk step (m) */
    float kink;                     /* kink a step (deg, sd) */
    float branch_p;                 /* branching chance a step at the arm's end (rises from 0 at the hit) */
    int rings;                      /* concentric chords (most) */
    float ring_t;                   /* ring radius / glass thickness */
    float ring_dr;                  /* further rings' spacing (m) */
    float ring_p;                   /* a sector gets a chord */
    float ring_e;                   /* energy threshold for the first ring */
    float core;                     /* crushed core radius (m) at E 0.5 */
    float halo;                     /* halo radius / core */
    float core_a;                   /* core opacity */
    float hole_e;                   /* energy for a hole (> 2: never) */
    float hole;                     /* hole radius (m) */
    float width;                    /* a crack's drawn width near the hit (m) */
    int creep;
    float hoop;                     /* pull towards fore-aft far out */
} crk_mat;
static const crk_mat mats[4] = {
    /* stretched acrylic (most canopies and many windscreens): a star of a few long arms, chords, a milky core */
    { "acrylic", 6.0f, 3.0f, 12.0f, 0.35f, 0.25f, 0.004f, 0.008f, 3.5f, 0.05f, 2, 4.0f, 0.035f, 0.65f, 0.10f,
      0.012f, 2.4f, 0.50f, 1.6f, 0.006f, 4.5e-4f, 1, 0.25f },
    /* acrylic over polycarbonate (F-16): the thin outer ply stars, short; a white ring; holes only for big hits */
    { "acrylic-pc laminate", 5.0f, 3.0f, 8.0f, 0.07f, 0.0f, 0.003f, 0.005f, 7.0f, 0.02f, 1, 6.0f, 0.02f, 0.6f, 0.15f,
      0.006f, 3.0f, 0.45f, 0.6f, 0.005f, 3.0e-4f, 0, 0.1f },
    /* polycarbonate (F-22): a hole smaller than the round, petals, a stress-whitened ring, almost no cracks */
    { "polycarbonate", 2.0f, 0.0f, 3.0f, 0.03f, 0.0f, 0.003f, 0.004f, 5.0f, 0.0f, 0, 4.0f, 0.02f, 0.0f, 9.0f,
      0.005f, 4.0f, 0.40f, 0.08f, 0.005f, 2.5e-4f, 0, 0.0f },
    /* laminated glass (F-14 / A-10 windscreens, the MiG-21's front panel): a dense spider web held in one piece */
    { "glass laminate", 10.0f, 6.0f, 16.0f, 0.16f, 0.1f, 0.003f, 0.005f, 6.0f, 0.10f, 4, 2.0f, 0.016f, 0.85f, 0.03f,
      0.008f, 2.2f, 0.55f, 9.0f, 0.004f, 3.0e-4f, 0, 0.1f },
};

/* ---------------------------------------------------------------- glass */
typedef struct {
    u32 mesh; char name[33]; int keep, mat; float thick;
    u32 ntri; float *tri, *nrm, *area;
    u32 nb; float *bord;            /* border edges: a(3), b(3), the triangle's centroid(3) */
    float total;                    /* area (m^2) */
    float cen[3];                   /* area-weighted centre (a frame crack's inward side) */
} crk_gmesh;
struct crk_glass {
    char path[260], base[300];
    int nm; crk_gmesh m[CRK_MESH];
    float area;
    float bmin[3], bmax[3];          /* the kept glass's box (m, + 2 cm) */
    u32 serial;
};
static u32 glass_serial;

crk_glass *ps2_crk_glass_begin(const char *path) {
    crk_cfg();
    if (!cs.boot_on) return NULL;
    crk_glass *g = (crk_glass *)calloc(1, sizeof *g);
    if (!g) return NULL;
    snprintf(g->path, sizeof g->path, "%s", path ? path : "");
    const char *b = g->path, *s;
    for (s = g->path; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    snprintf(g->base, sizeof g->base, "%s", b);
    for (char *c = g->base; *c; c++) { if (*c == '.') { *c = 0; break; } if (*c >= 'A' && *c <= 'Z') *c += 32; }
    g->serial = ++glass_serial;
    return g;
}
void ps2_crk_glass_mesh(crk_glass *g, u32 mesh, const char *name, const void *verts, u32 stride, u32 ntri) {
    if (!g || g->nm >= CRK_MESH || !ntri) return;
    crk_gmesh *m = &g->m[g->nm];
    m->tri = (float *)malloc((size_t)ntri * 9u * sizeof(float));
    m->nrm = (float *)malloc((size_t)ntri * 3u * sizeof(float));
    m->area = (float *)malloc((size_t)ntri * sizeof(float));
    if (!m->tri || !m->nrm || !m->area) { free(m->tri); free(m->nrm); free(m->area); memset(m, 0, sizeof *m); return; }
    m->mesh = mesh; m->ntri = ntri; m->keep = 1;
    snprintf(m->name, sizeof m->name, "%s", name ? name : "");
    const u8 *v = (const u8 *)verts;
    for (u32 t = 0; t < ntri; t++) {
        float *T = m->tri + t * 9u;
        for (int k = 0; k < 3; k++) memcpy(T + 3 * k, v + ((size_t)t * 3u + (size_t)k) * stride, 12);
        float e1[3] = { T[3] - T[0], T[4] - T[1], T[5] - T[2] }, e2[3] = { T[6] - T[0], T[7] - T[1], T[8] - T[2] };
        float c[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
        float l = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
        m->area[t] = 0.5f * l;
        m->total += 0.5f * l;
        for (int k = 0; k < 3; k++) m->nrm[t * 3u + (u32)k] = l > 1e-12f ? c[k] / l : 0.0f;
        for (int k = 0; k < 3; k++) m->cen[k] += 0.5f * l * (T[k] + T[3 + k] + T[6 + k]) * (1.0f / 3.0f);
    }
    for (int k = 0; k < 3; k++) m->cen[k] = m->total > 0.0f ? m->cen[k] / m->total : 0.0f;
    g->nm++;
}
void ps2_crk_glass_keep(crk_glass *g, u32 mesh, int keep) {
    if (!g) return;
    for (int i = 0; i < g->nm; i++) if (g->m[i].mesh == mesh) g->m[i].keep = keep;
}
static int crk_ci_has(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (const char *h = hay; *h; h++) {
        size_t k = 0;
        while (k < n && h[k] && ((h[k] | 32) == (needle[k] | 32))) k++;
        if (k == n) return 1;
    }
    return 0;
}
static int crk_mat_word(const char *w, size_t n) {
    if ((n == 7 && !strncmp(w, "acrylic", 7))) return MAT_ACRYLIC;
    if ((n == 5 && !strncmp(w, "pclam", 5)) || (n == 10 && !strncmp(w, "acrylic-pc", 10))) return MAT_PCLAM;
    if (n == 2 && !strncmp(w, "pc", 2)) return MAT_PC;
    if (n == 5 && !strncmp(w, "glass", 5)) return MAT_GLASS;
    return -1;
}
/* the material of a glass mesh: the override (ACZ_VR_CANOPY_CRACK_MATERIAL: a material for all, or a list of
   [cockpit:]name=material, case-insensitive substrings of the cockpit file and the mesh name), else the table */
static int crk_mesh_mat(const crk_glass *g, const crk_gmesh *m, int *wind) {
    const char *n = m->name;
    *wind = crk_ci_has(n, "wind") || crk_ci_has(n, "screen") || crk_ci_has(n, "front") || crk_ci_has(n, "vitres")
            || (crk_ci_has(g->base, "a10") && crk_ci_has(n, "fcanopy"));
    if (strcmp(cs.mat, "auto")) {
        int all = crk_mat_word(cs.mat, strlen(cs.mat));
        if (all >= 0) return all;
        const char *p = cs.mat;
        while (*p) {
            const char *e = strchr(p, ','), *end = e ? e : p + strlen(p), *eq = memchr(p, '=', (size_t)(end - p));
            if (eq) {
                char key[96], cp[96]; size_t kl = (size_t)(eq - p); if (kl > 95) kl = 95;
                memcpy(key, p, kl); key[kl] = 0;
                char *colon = strchr(key, ':');
                int ok = 1;
                if (colon) { *colon = 0; snprintf(cp, sizeof cp, "%s", key); ok = crk_ci_has(g->base, cp); memmove(key, colon + 1, strlen(colon + 1) + 1); }
                int mt = crk_mat_word(eq + 1, (size_t)(end - eq - 1));
                if (ok && mt >= 0 && (!*key || crk_ci_has(n, key))) return mt;
            }
            if (!e) break;
            p = e + 1;
        }
    }
    const char *b = g->base;
    if (crk_ci_has(b, "f16")) return MAT_PCLAM;              /* one piece: acrylic 3 mm over polycarbonate 13 mm */
    if (crk_ci_has(b, "f22")) return MAT_PC;                 /* 19 mm polycarbonate */
    if (*wind && (crk_ci_has(b, "f14") || crk_ci_has(b, "a10") || crk_ci_has(b, "mig21"))) return MAT_GLASS;
    return MAT_ACRYLIC;
}
void ps2_crk_glass_end(crk_glass *g) {
    if (!g) return;
    char ln[1024]; int n = 0;
    for (int i = 0; i < g->nm; i++) {
        crk_gmesh *m = &g->m[i];
        if (m->total < 0.03f) m->keep = 0;             /* (a few cm^2: a HUD edge, a lamp's cover) */
        if (!m->keep) { free(m->tri); free(m->nrm); free(m->area); m->tri = m->nrm = m->area = NULL; m->ntri = 0; continue; }
        int wind = 0;
        m->mat = crk_mesh_mat(g, m, &wind);
        /* the thickness sets the rings' radius: acrylic canopies ~6.4 mm, windscreens ~18 mm (F-5E, F-15, F/A-18);
           laminates by their outer ply */
        m->thick = m->mat == MAT_ACRYLIC ? (wind ? 0.018f : 0.0064f) : m->mat == MAT_GLASS ? 0.008f : 0.004f;
        g->area += m->total;
        /* the border: edges of one triangle (positions joined at 0.1 mm) */
        u32 nv = m->ntri * 3u, hn = 16u, en = 16u;
        while (hn < 2u * nv) hn <<= 1;
        while (en < 6u * m->ntri) en <<= 1;
        u32 *slot = (u32 *)malloc((size_t)hn * 4u), *id = (u32 *)malloc((size_t)nv * 4u);
        s32 (*key)[3] = (s32 (*)[3])malloc((size_t)nv * sizeof *key);
        u64 *ek = (u64 *)malloc((size_t)en * 8u); u32 *ec = (u32 *)calloc(en, 4u), *et = (u32 *)malloc((size_t)en * 4u);
        u32 nu = 0;
        if (slot && id && key && ek && ec && et) {
            memset(slot, 0xFF, (size_t)hn * 4u);
            for (u32 k = 0; k < nv; k++) {
                const float *q = m->tri + k * 3u;
                s32 kq[3] = { (s32)lrintf(q[0] * 1e4f), (s32)lrintf(q[1] * 1e4f), (s32)lrintf(q[2] * 1e4f) };
                u32 h = ((u32)kq[0] * 73856093u) ^ ((u32)kq[1] * 19349663u) ^ ((u32)kq[2] * 83492791u);
                for (u32 s = h & (hn - 1u);; s = (s + 1u) & (hn - 1u)) {
                    if (slot[s] == 0xFFFFFFFFu) { slot[s] = nu; memcpy(key[nu], kq, sizeof kq); id[k] = nu++; break; }
                    if (!memcmp(key[slot[s]], kq, sizeof kq)) { id[k] = slot[s]; break; }
                }
            }
            for (u32 t = 0; t < m->ntri; t++)
                for (int j = 0; j < 3; j++) {
                    u32 a = id[t * 3u + (u32)j], b = id[t * 3u + (u32)((j + 1) % 3)];
                    if (a == b) continue;
                    u64 k2 = a < b ? ((u64)a << 32 | b) : ((u64)b << 32 | a);
                    u32 h = (u32)(k2 * 0x9E3779B97F4A7C15ull >> 32);
                    for (u32 s = h & (en - 1u);; s = (s + 1u) & (en - 1u)) {
                        if (!ec[s]) { ek[s] = k2; ec[s] = 1; et[s] = t * 3u + (u32)j; break; }
                        if (ek[s] == k2) { ec[s]++; break; }
                    }
                }
            u32 nb = 0;
            for (u32 s = 0; s < en; s++) if (ec[s] == 1u) nb++;
            m->bord = (float *)malloc((size_t)(nb ? nb : 1u) * 9u * sizeof(float));
            if (m->bord) {
                for (u32 s = 0; s < en; s++) {
                    if (ec[s] != 1u) continue;
                    u32 t = et[s] / 3u, j = et[s] % 3u;
                    const float *T = m->tri + t * 9u;
                    float *B = m->bord + m->nb * 9u;
                    memcpy(B, T + 3u * j, 12); memcpy(B + 3, T + 3u * ((j + 1u) % 3u), 12);
                    for (int c = 0; c < 3; c++) B[6 + c] = (T[c] + T[3 + c] + T[6 + c]) * (1.0f / 3.0f);
                    m->nb++;
                }
            }
        }
        free(slot); free(id); free(key); free(ek); free(ec); free(et);
        if (!m->nb) {                                  /* (a closed glass: a combiner box, a lamp -- not a canopy) */
            m->keep = 0; g->area -= m->total;
            free(m->tri); free(m->nrm); free(m->area); free(m->bord); m->tri = m->nrm = m->area = m->bord = NULL; m->ntri = 0;
            continue;
        }
        n += snprintf(ln + n, sizeof ln - (size_t)n, "%s\"%s\" %u tri %.2f m2 %u border edges %s%s", n ? ", " : "", m->name,
                      m->ntri, m->total, m->nb, mats[m->mat].name, wind ? " (windscreen)" : "");
        if (n > (int)sizeof ln - 120) break;
    }
    for (int j = 0; j < 3; j++) { g->bmin[j] = 1e9f; g->bmax[j] = -1e9f; }
    for (int i = 0; i < g->nm; i++) {
        const crk_gmesh *m = &g->m[i];
        if (!m->keep || !m->tri) continue;
        for (u32 k = 0; k < m->ntri * 3u; k++)
            for (int j = 0; j < 3; j++) {
                float v = m->tri[k * 3u + (u32)j];
                if (v < g->bmin[j]) g->bmin[j] = v;
                if (v > g->bmax[j]) g->bmax[j] = v;
            }
    }
    for (int j = 0; j < 3; j++) { g->bmin[j] -= 0.02f; g->bmax[j] += 0.02f; }
    ps2_log("cracks: %s: glass %s", g->base, n ? ln : "none");
}
static pthread_mutex_t crk_mx = PTHREAD_MUTEX_INITIALIZER;
static const crk_glass *cur_glass;
static crk_glass *want_glass;                    /* (the renderer's model's glass, for the worker) */
static void crk_clear(const char *why);
void ps2_crk_glass_free(crk_glass *g) {
    if (!g) return;
    pthread_mutex_lock(&crk_mx);                       /* (the worker may be using it) */
    if (g == cur_glass) { crk_clear("the cockpit model was freed"); cur_glass = NULL; }
    if (g == want_glass) want_glass = NULL;
    pthread_mutex_unlock(&crk_mx);
    for (int i = 0; i < g->nm; i++) { free(g->m[i].tri); free(g->m[i].nrm); free(g->m[i].area); free(g->m[i].bord); }
    free(g);
}

/* ---------------------------------------------------------------- small vector helpers */
static float v3dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v3cross(const float *a, const float *b, float *o) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static float v3norm(float *a) {
    float l = sqrtf(v3dot(a, a));
    if (l > 1e-12f) { a[0] /= l; a[1] /= l; a[2] /= l; }
    return l;
}
static u32 crk_rng(u32 *s) { u32 x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x ? x : 0x9E3779B9u; return *s; }
static float crk_rand(u32 *s) { return (float)(crk_rng(s) >> 8) * (1.0f / 16777216.0f); }
static float crk_gauss(u32 *s) {
    float a = crk_rand(s), b = crk_rand(s);
    return sqrtf(-2.0f * logf(a + 1e-7f)) * cosf(2.0f * CRK_PI * b);
}
static float crk_wrap(float a) { while (a > CRK_PI) a -= 2.0f * CRK_PI; while (a < -CRK_PI) a += 2.0f * CRK_PI; return a; }

/* ray from the eye (origin) along d: the nearest kept glass triangle; t, mesh, triangle */
static int crk_ray(const crk_glass *g, const float *d, float *tout, int *mi, u32 *ti) {
    float best = 1e30f; int found = 0;
    for (int i = 0; i < g->nm; i++) {
        const crk_gmesh *m = &g->m[i];
        if (!m->keep || !m->ntri) continue;
        for (u32 t = 0; t < m->ntri; t++) {
            const float *T = m->tri + t * 9u;
            float e1[3] = { T[3] - T[0], T[4] - T[1], T[5] - T[2] }, e2[3] = { T[6] - T[0], T[7] - T[1], T[8] - T[2] }, p[3];
            v3cross(d, e2, p);
            float det = v3dot(e1, p);
            if (fabsf(det) < 1e-12f) continue;
            float inv = 1.0f / det, s[3] = { -T[0], -T[1], -T[2] };
            float u = v3dot(s, p) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            float q[3]; v3cross(s, e1, q);
            float v = v3dot(d, q) * inv;
            if (v < 0.0f || u + v > 1.0f) continue;
            float t2 = v3dot(e2, q) * inv;
            if (t2 > 0.02f && t2 < best) { best = t2; *mi = i; *ti = t; found = 1; }
        }
    }
    *tout = best;
    return found;
}

/* ---------------------------------------------------------------- an impact's plane: the glass projected */
typedef struct {
    float R, cs; int gn;
    u32 nt; float *t2, *t3;              /* projected triangles (2D a, b, c) and their 3D corners */
    u32 *toff, *tidx;                    /* grid -> triangles (CSR) */
    u32 nbe; float *b2;                  /* border edges (2D) */
    u32 *boff, *bidx;
} crk_proj;
typedef struct {
    float a[2], b[2];                    /* in the impact's plane (m) */
    float a3[3], b3[3];                  /* on the glass (cockpit frame) */
    float t0, t1, w; u32 fl;             /* reveal times (s from the epoch), true width (m), kind | half << 2 | tilt << 3 */
    float s0, s1;                        /* path length from the hit */
    int arm;                             /* the tip that walked it (-1: a ring) */
} crk_seg;
enum { SEG_ARM = 0, SEG_BRANCH = 1, SEG_RING = 2, SEG_FRAME = 3 };
typedef struct {
    float p[2], th, s, left, w; u32 rng; int depth, state, kind;   /* state 0 can grow, 1 frame, 2 crack, 3 done */
    float len0;
} crk_tip;
typedef struct {
    int used, main, mesh, mat, frame;
    float c[3], n[3], t1[3], t2[3], dir[3];
    float E, L0, creep_used, creep_max, creep_acc, pop;
    float rc, rh, core_a, hole, thick;
    double birth, last_hit; u32 src; int burst;
    u32 seed, rng;
    crk_seg *seg; u32 nseg, cap;
    crk_tip tip[CRK_TIPS]; int ntip;
    crk_proj pj;
    float rb, sag;                       /* bounding radius and the glass's sag from the plane over it */
    int dirty;
    int chip;                            /* a debris chip (bug #139): the oldest goes when a slot is needed */
} crk_star;
static crk_star stars[CRK_MAX_STARS];
static u32 cur_serial;
static double epoch;
static u32 gpu_ver = 1;
static u32 nhits_total;

static void proj_free(crk_proj *p) { free(p->t2); free(p->t3); free(p->toff); free(p->tidx); free(p->b2); free(p->boff); free(p->bidx); memset(p, 0, sizeof *p); }
static void star_free(crk_star *s) { proj_free(&s->pj); free(s->seg); memset(s, 0, sizeof *s); }
static void crk_clear(const char *why) {
    int n = 0;
    for (u32 i = 0; i < CRK_MAX_STARS; i++) if (stars[i].used) { n++; star_free(&stars[i]); }
    gpu_ver++;
    if (n || cs.dbg) ps2_log("cracks: cleared %d impact%s (%s)", n, n == 1 ? "" : "s", why);
}
static void crk_to_plane(const crk_star *s, const float *x, float *uv) {
    float q[3] = { x[0] - s->c[0], x[1] - s->c[1], x[2] - s->c[2] };
    uv[0] = v3dot(q, s->t1); uv[1] = v3dot(q, s->t2);
}
/* build the projection of the impact's mesh within R of its centre (triangles facing within ~78 deg) */
static int proj_build(crk_star *s, const crk_gmesh *m, float R) {
    crk_proj *p = &s->pj;
    proj_free(p);
    p->R = R; p->cs = R / 30.0f; if (p->cs < 0.008f) p->cs = 0.008f;
    p->gn = (int)ceilf(2.0f * R / p->cs);
    if (p->gn > 96) { p->gn = 96; p->cs = 2.0f * R / 96.0f; }
    u32 nc = (u32)(p->gn * p->gn);
    p->t2 = (float *)malloc((size_t)m->ntri * 6u * 4u); p->t3 = (float *)malloc((size_t)m->ntri * 9u * 4u);
    p->toff = (u32 *)calloc(nc + 1u, 4u); p->boff = (u32 *)calloc(nc + 1u, 4u);
    p->b2 = (float *)malloc((size_t)(m->nb ? m->nb : 1u) * 4u * 4u);
    if (!p->t2 || !p->t3 || !p->toff || !p->boff || !p->b2) return 0;
    for (u32 t = 0; t < m->ntri; t++) {
        const float *T = m->tri + t * 9u;
        if (v3dot(m->nrm + t * 3u, s->n) * (v3dot(m->nrm + t * 3u, s->n) >= 0.0f ? 1.0f : -1.0f) < 0.2f) continue;
        float uv[6], lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f }, hmin = 1e9f;
        for (int k = 0; k < 3; k++) {
            crk_to_plane(s, T + 3 * k, uv + 2 * k);
            for (int j = 0; j < 2; j++) { lo[j] = fminf(lo[j], uv[2 * k + j]); hi[j] = fmaxf(hi[j], uv[2 * k + j]); }
            float q[3] = { T[3 * k] - s->c[0], T[3 * k + 1] - s->c[1], T[3 * k + 2] - s->c[2] };
            hmin = fminf(hmin, fabsf(v3dot(q, s->n)));
        }
        /* (its box overlaps the square; not the far side of the canopy) */
        if (lo[0] > R + 0.05f || lo[1] > R + 0.05f || hi[0] < -R - 0.05f || hi[1] < -R - 0.05f || hmin > 0.6f) continue;
        memcpy(p->t2 + p->nt * 6u, uv, sizeof uv); memcpy(p->t3 + p->nt * 9u, T, 36);
        p->nt++;
    }
    for (u32 e = 0; e < m->nb; e++) {
        const float *B = m->bord + e * 9u;
        float a[2], b[2];
        crk_to_plane(s, B, a); crk_to_plane(s, B + 3, b);
        float q[3] = { B[0] - s->c[0], B[1] - s->c[1], B[2] - s->c[2] };
        if (fminf(a[0], b[0]) > R + 0.05f || fminf(a[1], b[1]) > R + 0.05f || fmaxf(a[0], b[0]) < -R - 0.05f
            || fmaxf(a[1], b[1]) < -R - 0.05f || fabsf(v3dot(q, s->n)) > 0.6f) continue;
        float *o = p->b2 + p->nbe * 4u; o[0] = a[0]; o[1] = a[1]; o[2] = b[0]; o[3] = b[1];
        p->nbe++;
    }
    /* CSR grids: count, prefix, fill (each item into every cell its box covers) */
    for (int pass = 0; pass < 2; pass++) {
        for (int kind = 0; kind < 2; kind++) {
            u32 *off = kind ? p->boff : p->toff, n = kind ? p->nbe : p->nt;
            u32 *fill = NULL;
            if (pass) {
                u32 tot = off[nc];
                u32 *idx = (u32 *)malloc((size_t)(tot ? tot : 1u) * 4u);
                if (!idx) return 0;
                if (kind) p->bidx = idx; else p->tidx = idx;
                fill = (u32 *)malloc((size_t)nc * 4u);
                if (!fill) return 0;
                memcpy(fill, off, (size_t)nc * 4u);
            }
            for (u32 i = 0; i < n; i++) {
                float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
                const float *v = kind ? p->b2 + i * 4u : p->t2 + i * 6u;
                for (int k = 0; k < (kind ? 2 : 3); k++)
                    for (int j = 0; j < 2; j++) { lo[j] = fminf(lo[j], v[2 * k + j]); hi[j] = fmaxf(hi[j], v[2 * k + j]); }
                int x0 = (int)floorf((lo[0] + R) / p->cs), x1 = (int)floorf((hi[0] + R) / p->cs);
                int y0 = (int)floorf((lo[1] + R) / p->cs), y1 = (int)floorf((hi[1] + R) / p->cs);
                x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 >= p->gn ? p->gn - 1 : x1; y1 = y1 >= p->gn ? p->gn - 1 : y1;
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) {
                        u32 c = (u32)(y * p->gn + x);
                        if (!pass) off[c + 1u]++;
                        else (kind ? p->bidx : p->tidx)[fill[c]++] = i;
                    }
            }
            if (!pass) for (u32 c = 0; c < nc; c++) off[c + 1u] += off[c];
            free(fill);
        }
    }
    return 1;
}
/* the glass under (u, v): 1 and the point on it (3D), else 0 */
static int proj_on(const crk_proj *p, const float *uv, float *x3) {
    int x = (int)floorf((uv[0] + p->R) / p->cs), y = (int)floorf((uv[1] + p->R) / p->cs);
    if (x < 0 || y < 0 || x >= p->gn || y >= p->gn) return 0;
    u32 c = (u32)(y * p->gn + x);
    for (u32 k = p->toff[c]; k < p->toff[c + 1u]; k++) {
        const float *T = p->t2 + p->tidx[k] * 6u;
        float d1[2] = { T[2] - T[0], T[3] - T[1] }, d2[2] = { T[4] - T[0], T[5] - T[1] }, q[2] = { uv[0] - T[0], uv[1] - T[1] };
        float det = d1[0] * d2[1] - d1[1] * d2[0];
        if (fabsf(det) < 1e-14f) continue;
        float a = (q[0] * d2[1] - q[1] * d2[0]) / det, b = (d1[0] * q[1] - d1[1] * q[0]) / det;
        if (a < -1e-5f || b < -1e-5f || a + b > 1.0f + 1e-5f) continue;
        if (x3) {
            const float *X = p->t3 + p->tidx[k] * 9u;
            for (int j = 0; j < 3; j++) x3[j] = X[j] + a * (X[3 + j] - X[j]) + b * (X[6 + j] - X[j]);
        }
        return 1;
    }
    return 0;
}
static float seg_dist2(const float *p, const float *a, const float *b) {
    float ab[2] = { b[0] - a[0], b[1] - a[1] }, ap[2] = { p[0] - a[0], p[1] - a[1] };
    float l = ab[0] * ab[0] + ab[1] * ab[1], t = l > 0.0f ? (ap[0] * ab[0] + ap[1] * ab[1]) / l : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float dx = ap[0] - t * ab[0], dy = ap[1] - t * ab[1];
    return dx * dx + dy * dy;
}
/* distance to the glass's border near (u, v) (searched within one cell: at most cs) */
static float proj_edge(const crk_proj *p, const float *uv) {
    int x = (int)floorf((uv[0] + p->R) / p->cs), y = (int)floorf((uv[1] + p->R) / p->cs);
    float best = p->cs * p->cs;
    for (int yy = y - 1; yy <= y + 1; yy++)
        for (int xx = x - 1; xx <= x + 1; xx++) {
            if (xx < 0 || yy < 0 || xx >= p->gn || yy >= p->gn) continue;
            u32 c = (u32)(yy * p->gn + xx);
            for (u32 k = p->boff[c]; k < p->boff[c + 1u]; k++) {
                const float *B = p->b2 + p->bidx[k] * 4u;
                float d2 = seg_dist2(uv, B, B + 2);
                if (d2 < best) best = d2;
            }
        }
    return sqrtf(best);
}
/* the obstacles for crossing tests: every segment of the impact and the earlier ones' on the same glass, in this plane */
typedef struct { float a[2], b[2]; } crk_ob;
static struct { crk_ob *ob; u32 n, cap; u32 *head, *next, *item; u32 ni, capi; int gn; float cs, R; } obs;
static void obs_add(const float *a, const float *b) {
    if (obs.n >= obs.cap) {
        u32 nc = obs.cap ? obs.cap * 2u : 1024u;
        crk_ob *o = (crk_ob *)realloc(obs.ob, (size_t)nc * sizeof *o);
        if (!o) return;
        obs.ob = o; obs.cap = nc;
    }
    u32 id = obs.n++;
    memcpy(obs.ob[id].a, a, 8); memcpy(obs.ob[id].b, b, 8);
    int x0 = (int)floorf((fminf(a[0], b[0]) + obs.R) / obs.cs), x1 = (int)floorf((fmaxf(a[0], b[0]) + obs.R) / obs.cs);
    int y0 = (int)floorf((fminf(a[1], b[1]) + obs.R) / obs.cs), y1 = (int)floorf((fmaxf(a[1], b[1]) + obs.R) / obs.cs);
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 >= obs.gn ? obs.gn - 1 : x1; y1 = y1 >= obs.gn ? obs.gn - 1 : y1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            if (obs.ni >= obs.capi) {
                u32 nc = obs.capi ? obs.capi * 2u : 4096u;
                u32 *nx = (u32 *)realloc(obs.next, (size_t)nc * 4u), *it;
                if (!nx) return;
                obs.next = nx;
                it = (u32 *)realloc(obs.item, (size_t)nc * 4u);
                if (!it) return;
                obs.item = it; obs.capi = nc;
            }
            u32 c = (u32)(y * obs.gn + x);
            obs.item[obs.ni] = id; obs.next[obs.ni] = obs.head[c]; obs.head[c] = obs.ni++;
        }
}
static void obs_begin(const crk_star *s) {
    u32 nc;
    obs.gn = s->pj.gn; obs.cs = s->pj.cs; obs.R = s->pj.R; obs.n = 0; obs.ni = 0;
    nc = (u32)(obs.gn * obs.gn);
    free(obs.head);
    obs.head = (u32 *)malloc((size_t)nc * 4u);
    if (obs.head) memset(obs.head, 0xFF, (size_t)nc * 4u);
    for (u32 i = 0; i < CRK_MAX_STARS; i++) {
        const crk_star *o = &stars[i];
        if (!o->used || o->mesh != s->mesh) continue;
        for (u32 k = 0; k < o->nseg; k++) {
            float a[2], b[2];
            if (o == s) { memcpy(a, o->seg[k].a, 8); memcpy(b, o->seg[k].b, 8); }
            else {
                float qa[3] = { o->seg[k].a3[0] - s->c[0], o->seg[k].a3[1] - s->c[1], o->seg[k].a3[2] - s->c[2] };
                if (fabsf(v3dot(qa, s->n)) > 0.3f) continue;
                crk_to_plane(s, o->seg[k].a3, a); crk_to_plane(s, o->seg[k].b3, b);
                if (fminf(a[0], b[0]) > s->pj.R || fminf(a[1], b[1]) > s->pj.R || fmaxf(a[0], b[0]) < -s->pj.R
                    || fmaxf(a[1], b[1]) < -s->pj.R) continue;
            }
            obs_add(a, b);
        }
    }
}
/* the first crossing of p -> q with an obstacle (not one that starts or ends at p): its parameter along p -> q */
static float obs_cross(const float *p, const float *q) {
    if (!obs.head) return 2.0f;
    float best = 2.0f;
    int x0 = (int)floorf((fminf(p[0], q[0]) + obs.R) / obs.cs), x1 = (int)floorf((fmaxf(p[0], q[0]) + obs.R) / obs.cs);
    int y0 = (int)floorf((fminf(p[1], q[1]) + obs.R) / obs.cs), y1 = (int)floorf((fmaxf(p[1], q[1]) + obs.R) / obs.cs);
    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 >= obs.gn ? obs.gn - 1 : x1; y1 = y1 >= obs.gn ? obs.gn - 1 : y1;
    float r[2] = { q[0] - p[0], q[1] - p[1] };
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            for (u32 k = obs.head[(u32)(y * obs.gn + x)]; k != 0xFFFFFFFFu; k = obs.next[k]) {
                const crk_ob *o = &obs.ob[obs.item[k]];
                float sx = o->b[0] - o->a[0], sy = o->b[1] - o->a[1];
                if ((fabsf(o->a[0] - p[0]) < 2e-5f && fabsf(o->a[1] - p[1]) < 2e-5f)
                    || (fabsf(o->b[0] - p[0]) < 2e-5f && fabsf(o->b[1] - p[1]) < 2e-5f)) continue;
                float den = r[0] * sy - r[1] * sx;
                if (fabsf(den) < 1e-14f) continue;
                float qx = o->a[0] - p[0], qy = o->a[1] - p[1];
                float t = (qx * sy - qy * sx) / den, u = (qx * r[1] - qy * r[0]) / den;
                if (t > 1e-4f && t <= 1.0f && u >= 0.0f && u <= 1.0f && t < best) best = t;
            }
    return best;
}

/* ---------------------------------------------------------------- the generator */
static double gen_birth;                 /* the reveal's origin (s from the epoch) */
static float gen_vrev;                   /* reveal speed (m/s; 0 = instant) */
static float gen_bore[3], gen_clear;     /* gunsight cone (cos of its half-angle; 2 = none) */
static int seg_push(crk_star *s, const float *a, const float *b, float s0, float s1, float w, int kind, float tilt, double t_extra,
                    int arm) {
    if (s->nseg >= s->cap) {
        u32 nc = s->cap ? s->cap * 2u : 256u;
        crk_seg *n = (crk_seg *)realloc(s->seg, (size_t)nc * sizeof *n);
        if (!n) return 0;
        s->seg = n; s->cap = nc;
    }
    crk_seg *g = &s->seg[s->nseg];
    memcpy(g->a, a, 8); memcpy(g->b, b, 8);
    if (!proj_on(&s->pj, a, g->a3)) for (int j = 0; j < 3; j++) g->a3[j] = s->c[j] + a[0] * s->t1[j] + a[1] * s->t2[j];
    if (!proj_on(&s->pj, b, g->b3)) for (int j = 0; j < 3; j++) g->b3[j] = s->c[j] + b[0] * s->t1[j] + b[1] * s->t2[j];
    double base = gen_birth + t_extra;
    g->t0 = (float)(base + (gen_vrev > 0.0f ? s0 / gen_vrev : 0.0f) - epoch);
    g->t1 = (float)(base + (gen_vrev > 0.0f ? s1 / gen_vrev : 0.0f) - epoch);
    if (g->t1 < g->t0 + 1e-4f) g->t1 = g->t0 + 1e-4f;
    g->w = w; g->s0 = s0; g->s1 = s1; g->arm = arm;
    int tq = (int)(tilt * (15.0f / 0.7f) + 0.5f); if (tq < 0) tq = 0; if (tq > 15) tq = 15;
    int half = 0;
    if (gen_clear < 1.5f) {
        float m[3] = { 0.5f * (g->a3[0] + g->b3[0]), 0.5f * (g->a3[1] + g->b3[1]), 0.5f * (g->a3[2] + g->b3[2]) };
        v3norm(m);
        half = v3dot(m, gen_bore) > gen_clear;
    }
    g->fl = (u32)kind | (u32)half << 2 | (u32)tq << 3;
    s->nseg++;
    obs_add(a, b);
    s->dirty = 1;
    return 1;
}
/* walk tip k by up to `budget` metres (its own random state); returns the length walked */
static float tip_walk(crk_star *s, int k, float budget, double t_extra, int creep) {
    crk_tip *tp = &s->tip[k];
    const crk_mat *M = &mats[s->mat];
    float walked = 0.0f;
    while (budget > 1e-4f && tp->state == 0 && s->nseg < CRK_MAX_SEGS) {
        float step = M->step_lo + (M->step_hi - M->step_lo) * crk_rand(&tp->rng);
        if (step > budget) step = budget;
        float r = sqrtf(tp->p[0] * tp->p[0] + tp->p[1] * tp->p[1]);
        /* the direction it is pulled to: radial near the hit; beyond 3 core radii a little fore-aft (u) */
        float rad = r > 1e-5f ? atan2f(tp->p[1], tp->p[0]) : tp->th;
        float tgt = rad, kappa = 0.3f;
        if (tp->kind == SEG_FRAME) { tgt = tp->th; kappa = 0.05f; }
        else if (r > 3.0f * fmaxf(s->rc, 0.004f) && M->hoop > 0.0f) {
            float fa = cosf(rad) >= 0.0f ? 0.0f : CRK_PI;
            float wv = M->hoop * fminf(1.0f, (r - 3.0f * s->rc) / 0.1f);
            float x = cosf(rad) + wv * cosf(fa), y = sinf(rad) + wv * sinf(fa);
            tgt = atan2f(y, x); kappa = 0.15f;
        }
        if (tp->depth > 0) kappa *= 0.5f;                  /* branches keep their own heading longer */
        else if (tp->kind == SEG_ARM && r < 3.0f * fmaxf(s->rc, 0.004f)) kappa = 0.6f;   /* near the hit: radial */
        /* kinks: the mirror zone near the hit runs straight, the hackle zone further out wanders */
        float kz = 0.3f + 0.7f * fminf(1.0f, tp->s / (6.0f * fmaxf(s->rc, 0.004f)));
        tp->th += kappa * crk_wrap(tgt - tp->th) + crk_gauss(&tp->rng) * M->kink * kz * (CRK_PI / 180.0f);
        float q[2] = { tp->p[0] + step * cosf(tp->th), tp->p[1] + step * sinf(tp->th) };
        /* the frame: the glass must go on under it, 3 mm clear of the border */
        int ok = fmaxf(fabsf(q[0]), fabsf(q[1])) < s->pj.R && proj_on(&s->pj, q, NULL) && proj_edge(&s->pj, q) >= 0.003f;
        if (!ok) {
            float lo = 0.0f, hi = 1.0f;
            for (int it = 0; it < 5; it++) {
                float m = 0.5f * (lo + hi), qq[2] = { tp->p[0] + m * step * cosf(tp->th), tp->p[1] + m * step * sinf(tp->th) };
                if (proj_on(&s->pj, qq, NULL) && proj_edge(&s->pj, qq) >= 0.003f) lo = m; else hi = m;
            }
            q[0] = tp->p[0] + lo * step * cosf(tp->th); q[1] = tp->p[1] + lo * step * sinf(tp->th);
            step *= lo;
            tp->state = 1;
            if (step < 5e-4f) break;
        }
        /* an earlier crack: end on it */
        float tc = obs_cross(tp->p, q);
        if (tc <= 1.0f) {
            q[0] = tp->p[0] + tc * (q[0] - tp->p[0]); q[1] = tp->p[1] + tc * (q[1] - tp->p[1]);
            step *= tc;
            tp->state = 2;
        }
        float frac = tp->len0 > 0.0f ? (tp->s - 0.0f) / (tp->len0 + 1e-6f) : 1.0f;
        float w = tp->w * (creep ? 0.75f : 1.0f) * fmaxf(0.55f, 1.25f - 0.7f * fminf(frac, 1.0f));
        float tilt = 0.6f * expf(-tp->s / (3.0f * fmaxf(s->rc, 0.004f))) + 0.12f * crk_rand(&tp->rng);
        seg_push(s, tp->p, q, tp->s, tp->s + step, w, tp->kind, tilt, t_extra, k);
        tp->p[0] = q[0]; tp->p[1] = q[1]; tp->s += step; walked += step; budget -= step; tp->left -= step;
        if (tp->state) break;
        /* branching: more often far out; 25-40 deg; 0.3-0.6 of what is left; two levels at most */
        float pb = M->branch_p * fminf(1.0f, frac) * (creep ? 0.3f : 1.0f);
        if (tp->depth < 2 && s->ntip < CRK_TIPS && tp->left > 0.01f && crk_rand(&tp->rng) < pb) {
            crk_tip *nb = &s->tip[s->ntip++];
            *nb = *tp;
            nb->rng = crk_rng(&tp->rng) ^ 0xA5A5A5A5u;
            nb->depth = tp->depth + 1; nb->kind = SEG_BRANCH;
            nb->th = tp->th + (crk_rand(&tp->rng) < 0.5f ? -1.0f : 1.0f) * (25.0f + 15.0f * crk_rand(&tp->rng)) * (CRK_PI / 180.0f);
            nb->left = tp->left * (0.3f + 0.3f * crk_rand(&tp->rng));
            nb->len0 = nb->s + nb->left; nb->w = tp->w * 0.8f;
            if (!creep) tip_walk(s, s->ntip - 1, nb->left, t_extra, 0);
        }
    }
    return walked;
}
/* concentric chords at radius r between neighbouring primary arms (they end on the arms) */
static void ring_add(crk_star *s, float r, float p_sec, double t_extra) {
    const crk_mat *M = &mats[s->mat];
    /* each primary arm's first point at radius >= r (its segments in walk order) */
    float pt[CRK_TIPS][2], ang[CRK_TIPS], sat[CRK_TIPS];
    int n = 0;
    for (int k = 0; k < s->ntip && n < CRK_TIPS; k++) {
        if (s->tip[k].depth != 0 || s->tip[k].kind != SEG_ARM) continue;
        for (u32 j = 0; j < s->nseg; j++) {
            const crk_seg *g = &s->seg[j];
            if (g->arm != k) continue;
            if (g->b[0] * g->b[0] + g->b[1] * g->b[1] >= r * r) {
                pt[n][0] = g->b[0]; pt[n][1] = g->b[1]; ang[n] = atan2f(g->b[1], g->b[0]); sat[n] = g->s1; n++;
                break;
            }
        }
    }
    if (cs.dbg >= 2) ps2_log("cracks:   ring at %.1f mm: %d arms reach it", r * 1000.0f, n);
    if (n < 2) return;
    for (int i = 0; i < n; i++)                        /* by angle */
        for (int j = i + 1; j < n; j++)
            if (ang[j] < ang[i]) {
                float t0 = ang[i]; ang[i] = ang[j]; ang[j] = t0;
                t0 = sat[i]; sat[i] = sat[j]; sat[j] = t0;
                float x = pt[i][0], y = pt[i][1]; pt[i][0] = pt[j][0]; pt[i][1] = pt[j][1]; pt[j][0] = x; pt[j][1] = y;
            }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        if (n == 2 && i == 1) break;
        float gap = crk_wrap(ang[j] - ang[i]); if (gap < 0.0f) gap += 2.0f * CRK_PI;
        if (gap > 1.9f || crk_rand(&s->rng) > p_sec) continue;
        /* two segments, bowed slightly (the chords of real stars are nearly straight); they end on the arms, or on a
           crack in between */
        float m[2] = { 0.5f * (pt[i][0] + pt[j][0]), 0.5f * (pt[i][1] + pt[j][1]) };
        float ml = sqrtf(m[0] * m[0] + m[1] * m[1]) + 1e-6f;
        float cl = sqrtf((pt[j][0] - pt[i][0]) * (pt[j][0] - pt[i][0]) + (pt[j][1] - pt[i][1]) * (pt[j][1] - pt[i][1]));
        float bow = (crk_rand(&s->rng) - 0.35f) * 0.12f * cl;
        m[0] += m[0] / ml * bow; m[1] += m[1] / ml * bow;
        float s0 = fmaxf(sat[i], sat[j]);
        float a[2] = { pt[i][0], pt[i][1] }, b[2];
        for (int h = 0; h < 2; h++) {
            const float *e = h ? pt[j] : m;
            b[0] = e[0]; b[1] = e[1];
            float tc = obs_cross(a, b);
            int stop = 0;
            if (tc <= 1.0f && !(h == 1 && tc > 0.97f)) { b[0] = a[0] + tc * (b[0] - a[0]); b[1] = a[1] + tc * (b[1] - a[1]); stop = 1; }
            if (!proj_on(&s->pj, b, NULL)) break;
            seg_push(s, a, b, s0 + 0.5f * cl * (float)h, s0 + 0.5f * cl * (float)(h + 1), M->width * 0.75f, SEG_RING, 0.25f,
                     t_extra, -1);
            if (stop) break;
            a[0] = b[0]; a[1] = b[1];
        }
    }
}
/* bounds, sag and the dirty flag after a change */
static void star_bounds(crk_star *s) {
    float rb = fmaxf(s->rh, s->hole * 1.3f) + 0.004f, sag = 0.004f;
    for (u32 k = 0; k < s->nseg; k++) {
        const crk_seg *g = &s->seg[k];
        for (int e = 0; e < 2; e++) {
            const float *x = e ? g->b3 : g->a3;
            float q[3] = { x[0] - s->c[0], x[1] - s->c[1], x[2] - s->c[2] };
            float d = sqrtf(v3dot(q, q)), h = fabsf(v3dot(q, s->n));
            if (d + 0.004f > rb) rb = d + 0.004f;
            if (h > sag) sag = h;
        }
    }
    s->rb = rb; s->sag = sag + 0.006f;
    s->dirty = 1;
    gpu_ver++;
}

/* sound and rumble events (renderer -> audio thread / gamepad) */
typedef struct { float st, x; int mat, kind; u32 snaps; } crk_snd;     /* kind 0 crack, 1 creep tick */
static crk_snd snd_ring[16];
static volatile u32 snd_seq;
static volatile float rum_amp; static volatile u32 rum_seq;
static float view_head[9]; static int view_head_ok;
static void crk_sound(const float *dir_c, float st, int mat, int kind, u32 snaps) {
    crk_cfg();
    if (!cs.fx_sound) return;
    if (ps2_gf_vis[3] > 0.0f && ps2_gf_vis[4] < 0.5f) return;     /* (the chase view, a cutscene: not in the cockpit) */
    float x = 0.0f;
    if (view_head_ok) x = view_head[0] * dir_c[0] + view_head[1] * dir_c[1] + view_head[2] * dir_c[2];
    else x = dir_c[0];
    u32 s = snd_seq;
    crk_snd *e = &snd_ring[s % 16u];
    e->st = st; e->x = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x; e->mat = mat; e->kind = kind; e->snaps = snaps;
    __atomic_store_n(&snd_seq, s + 1u, __ATOMIC_RELEASE);
}

/* a new impact at glass point x (cockpit frame) on mesh mi (triangle ti); E its energy; frame = a stress crack from
   the border along `inward` (in the plane) */
static crk_star *star_new(const crk_glass *g, int mi, u32 ti, const float *x, const float *dir, float E, int main_imp,
                          int frame, const float *inward, u32 seed) {
    const crk_gmesh *m = &g->m[mi];
    crk_star *s = NULL;
    for (u32 i = 0; i < CRK_MAX_STARS && !s; i++) if (!stars[i].used) s = &stars[i];
    if (!s) {                                          /* (bug #139: a hit's crack before a debris chip) */
        for (u32 i = 0; i < CRK_MAX_STARS; i++)
            if (stars[i].used && stars[i].chip && (!s || stars[i].birth < s->birth)) s = &stars[i];
        if (s) star_free(s);
    }
    if (!s) return NULL;
    memset(s, 0, sizeof *s);
    s->used = 1; s->main = main_imp; s->mesh = mi; s->mat = m->mat; s->frame = frame; s->E = E; s->thick = m->thick;
    memcpy(s->c, x, 12); memcpy(s->dir, dir, 12);
    memcpy(s->n, m->nrm + ti * 3u, 12);
    if (v3dot(s->n, x) > 0.0f) for (int j = 0; j < 3; j++) s->n[j] = -s->n[j];     /* towards the eye */
    float z[3] = { 0.0f, 0.0f, 1.0f };
    float d = v3dot(z, s->n);
    for (int j = 0; j < 3; j++) s->t1[j] = z[j] - d * s->n[j];
    if (v3norm(s->t1) < 0.2f) { float xx[3] = { 1.0f, 0.0f, 0.0f }; d = v3dot(xx, s->n); for (int j = 0; j < 3; j++) s->t1[j] = xx[j] - d * s->n[j]; v3norm(s->t1); }
    v3cross(s->n, s->t1, s->t2);
    s->seed = seed ? seed : 0x1234567u; s->rng = s->seed;
    const crk_mat *M = &mats[s->mat];
    float Ec = E > 1.5f ? 1.5f : E;
    s->birth = gen_birth; s->last_hit = gen_birth;
    /* size: arms ~ (E / 0.3)^1/4, length ~ E^1/2, core ~ 0.6 + 0.8 E */
    float L = M->len * sqrtf(Ec) * (frame ? 0.8f : 1.0f);
    if (!main_imp) L = 0.02f + 0.04f * crk_rand(&s->rng);
    s->rc = frame ? 0.0f : M->core * (0.6f + 0.8f * Ec) * (main_imp ? 1.0f : 0.4f);
    s->rh = s->rc * M->halo;
    s->core_a = M->core_a;
    s->hole = !frame && Ec >= M->hole_e ? M->hole * (0.8f + 0.6f * fminf(Ec, 1.0f)) * (main_imp ? 1.0f : 0.6f) : 0.0f;
    float R = fminf(0.6f, 2.6f * L + s->rh + 0.04f);
    if (!proj_build(s, m, R)) { star_free(s); return NULL; }
    obs_begin(s);
    int N;
    if (frame) N = 1 + (crk_rand(&s->rng) < 0.5f);
    else if (main_imp) {
        float nf = M->arms * powf(Ec / 0.3f, 0.25f) * (0.85f + 0.3f * crk_rand(&s->rng));
        N = (int)(nf + 0.5f);
        if (N < (int)M->arms_lo) N = (int)M->arms_lo;
        if (N > (int)M->arms_hi) N = (int)M->arms_hi;
    } else N = 2 + (int)(crk_rand(&s->rng) * 3.0f);
    float th0 = crk_rand(&s->rng) * 2.0f * CRK_PI;
    float in_th = inward ? atan2f(v3dot(inward, s->t2), v3dot(inward, s->t1)) : 0.0f;   /* (3D: into the plane) */
    float Lsum = 0.0f;
    for (int a = 0; a < N && s->ntip < CRK_TIPS; a++) {
        crk_tip *tp = &s->tip[s->ntip++];
        memset(tp, 0, sizeof *tp);
        tp->rng = s->seed * 2654435761u + (u32)a * 40503u + 17u; crk_rng(&tp->rng);
        tp->th = frame ? in_th + (a ? 1.0f : -1.0f) * (0.15f + 0.35f * crk_rand(&tp->rng)) * (N > 1 ? 1.0f : 0.0f)
                       : th0 + (float)a * 2.0f * CRK_PI / (float)N + (crk_rand(&tp->rng) - 0.5f) * 0.7f * 2.0f * CRK_PI / (float)N;
        float st = frame ? 0.0f : fmaxf(s->rc * 0.5f, 0.0015f);
        tp->p[0] = st * cosf(tp->th); tp->p[1] = st * sinf(tp->th); tp->s = st;
        float fl = 0.55f + 0.9f * crk_rand(&tp->rng);
        if (!frame && main_imp && crk_rand(&tp->rng) < M->run_p) fl *= 2.2f;
        tp->left = L * fl; tp->len0 = st + tp->left;
        tp->w = M->width * (main_imp ? 1.0f : 0.7f);
        tp->kind = frame ? SEG_FRAME : SEG_ARM;
        Lsum += tp->left;
    }
    int nprim = s->ntip;
    for (int a = 0; a < nprim; a++) tip_walk(s, a, s->tip[a].left, 0.0, 0);
    /* rings: the first at 3-6 thicknesses, independent of the speed, above a threshold energy; more for glass */
    if (!frame && main_imp && M->rings > 0 && Ec >= M->ring_e) {
        float r0 = M->ring_t * s->thick * (1.0f + 0.5f * crk_rand(&s->rng));
        if (r0 < s->rh + 0.004f) r0 = s->rh + 0.004f;
        int nr = 1 + (int)((Ec - M->ring_e) / (s->mat == MAT_GLASS ? 0.08f : 0.25f));   /* (glass: a spider web) */
        if (nr > M->rings) nr = M->rings;
        for (int i = 0; i < nr; i++) ring_add(s, r0 + (float)i * M->ring_dr * (0.8f + 0.4f * crk_rand(&s->rng)), M->ring_p, 0.0);
    }
    s->L0 = Lsum;
    s->creep_max = M->creep && main_imp && !frame ? 0.35f * Lsum : 0.0f;
    s->pop = 0.003f + 0.012f * crk_rand(&s->rng);
    star_bounds(s);
    return s;
}
/* a further hit on an impact: its arms run on, perhaps a ring further out */
static void star_grow(crk_star *s, float dE) {
    const crk_mat *M = &mats[s->mat];
    float E0 = s->E > 1.5f ? 1.5f : s->E, E1 = s->E + dE > 1.5f ? 1.5f : s->E + dE;
    float dL = M->len * (sqrtf(E1) - sqrtf(E0));
    s->E += dE;
    if (dL <= 0.002f) return;
    obs_begin(s);
    double tx = gen_birth - s->birth;
    (void)tx;
    int n = s->ntip;
    double saved = gen_birth;
    for (int k = 0; k < n; k++) {
        crk_tip *tp = &s->tip[k];
        if (tp->state == 0 && tp->depth == 0) {
            float add = dL * (0.6f + 0.8f * crk_rand(&tp->rng));
            tp->left += add; tp->len0 += add;
            /* the reveal continues from the tip: its path length so far is already shown */
            gen_birth = saved - (gen_vrev > 0.0f ? (double)(tp->s / gen_vrev) : 0.0);
            tip_walk(s, k, tp->left, 0.0, 0);
            gen_birth = saved;
        }
    }
    if (M->rings > 0 && E1 >= M->ring_e && E0 < M->ring_e + 0.25f && crk_rand(&s->rng) < 0.7f) {
        float r = M->ring_t * s->thick * 1.6f + M->ring_dr * (1.0f + crk_rand(&s->rng));
        gen_birth = saved - (gen_vrev > 0.0f ? (double)(r / gen_vrev) : 0.0);
        ring_add(s, r, M->ring_p * 0.8f, 0.0);
        gen_birth = saved;
    }
    s->creep_max += M->creep && s->main ? 0.35f * dL * (float)n : 0.0f;
    star_bounds(s);
}

/* ---------------------------------------------------------------- hits -> impacts */
static float crk_ang(const float *a, const float *b) { float d = v3dot(a, b); return acosf(d > 1.0f ? 1.0f : d < -1.0f ? -1.0f : d); }
/* turn d away from the gunsight cone (to its edge + 1 deg) */
static void crk_clear_cone(float *d) {
    if (gen_clear > 1.5f) return;
    float c = v3dot(d, gen_bore);
    if (c <= gen_clear) return;
    float perp[3] = { d[0] - c * gen_bore[0], d[1] - c * gen_bore[1], d[2] - c * gen_bore[2] };
    if (v3norm(perp) < 1e-4f) { perp[0] = 0.0f; perp[1] = 1.0f; perp[2] = 0.0f; }
    float a = acosf(gen_clear) + 0.0175f;
    for (int j = 0; j < 3; j++) d[j] = cosf(a) * gen_bore[j] + sinf(a) * perp[j];
}
/* a frame crack's start on border edge B of mesh i: 6 mm into the glass, then the triangle under it seen from the eye.
   sq = 0: towards the border triangle's centroid (the research's rule, as before); sq = 1 (ACZ_VR_CANOPY_CRACK_SHOCK):
   square to the edge on the side of the glass's centre, and the inward direction = the surface's normal x the edge --
   towards the centroid ran along the edge, under the frame, for the sliver triangles canopies have at their frame (the
   F-15C's sill strips are 0-13 deg off the edge). With the switch the glass the eye meets first also counts when it
   lies within 3 cm of the start (the F-15C's canopy has two shells, CanopyGlass and the lower CanopyGlassInside: a
   border point of the outer one failed behind the inner one) */
static int crk_frame_start(const crk_glass *g, int i, const float *B, int sq, int *mi, u32 *ti, float *x, float *inw) {
    const crk_gmesh *m = &g->m[i];
    float mp[3] = { 0.5f * (B[0] + B[3]), 0.5f * (B[1] + B[4]), 0.5f * (B[2] + B[5]) };
    float in[3] = { B[6] - mp[0], B[7] - mp[1], B[8] - mp[2] }, ed[3] = { B[3] - B[0], B[4] - B[1], B[5] - B[2] };
    float tc[3] = { m->cen[0] - mp[0], m->cen[1] - mp[1], m->cen[2] - mp[2] };
    float il = v3norm(in);
    if (sq) {
        v3norm(ed);
        float k = v3dot(tc, ed);
        for (int j = 0; j < 3; j++) in[j] = tc[j] - k * ed[j];
        if (v3norm(in) < 1e-4f) return 0;
        il = 1.0f;
    }
    for (int j = 0; j < 3; j++) x[j] = mp[j] + in[j] * fminf(0.006f, 0.5f * il);
    float dd[3] = { x[0], x[1], x[2] }, t; int m2; u32 t2;
    float xl = v3norm(dd);
    if (!crk_ray(g, dd, &t, &m2, &t2)) return 0;
    if (m2 != i && (!cs.shock || fabsf(t - xl) > 0.03f)) return 0;
    if (sq) {                 /* inward = the surface normal x the edge, on the centre's side; at a corner (border edges
                                 of either direction within 4 cm) the mean of theirs: the crack leaves the corner on the
                                 bisector instead of hugging one side of the frame */
        float N[3], pp[3], sum[3] = { 0.0f, 0.0f, 0.0f }; memcpy(N, g->m[m2].nrm + t2 * 3u, 12);
        v3cross(N, ed, pp);
        if (v3norm(pp) < 1e-3f) return 0;
        if (v3dot(pp, tc) < 0.0f) for (int j = 0; j < 3; j++) pp[j] = -pp[j];
        for (u32 e = 0; e < m->nb; e++) {
            const float *C = m->bord + e * 9u;
            float ab[3] = { C[3] - C[0], C[4] - C[1], C[5] - C[2] }, ap[3] = { mp[0] - C[0], mp[1] - C[1], mp[2] - C[2] };
            float l2 = v3dot(ab, ab), u = l2 > 0.0f ? v3dot(ap, ab) / l2 : 0.0f;
            u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
            float q[3] = { ap[0] - u * ab[0], ap[1] - u * ab[1], ap[2] - u * ab[2] };
            if (v3dot(q, q) > 0.04f * 0.04f || l2 < 1e-10f) continue;
            float ek[3] = { ab[0], ab[1], ab[2] }, pk[3];
            v3norm(ek); v3cross(N, ek, pk);
            if (v3norm(pk) < 1e-3f) continue;
            if (v3dot(pk, tc) < 0.0f) for (int j = 0; j < 3; j++) pk[j] = -pk[j];   /* (on the centre's side) */
            for (int j = 0; j < 3; j++) sum[j] += pk[j];
        }
        if (v3norm(sum) > 1e-3f && v3dot(sum, pp) > 0.3f) memcpy(pp, sum, 12);
        memcpy(inw, pp, 12);
    } else memcpy(inw, in, 12);
    *mi = m2; *ti = t2;
    for (int j = 0; j < 3; j++) x[j] = dd[j] * t;
    return 1;
}
/* the nearest border point of the kept glass to direction d (angle < 60 deg): a frame stress crack starts there */
static int crk_frame_point(const crk_glass *g, const float *d, int *mi, u32 *ti, float *x, float *inw) {
    float best = cosf(60.0f * CRK_PI / 180.0f); int found = -1; const float *Bb = NULL;
    for (int i = 0; i < g->nm; i++) {
        const crk_gmesh *m = &g->m[i];
        if (!m->keep || !m->ntri || !m->bord) continue;
        for (u32 e = 0; e < m->nb; e++) {
            const float *B = m->bord + e * 9u;
            float mp[3] = { 0.5f * (B[0] + B[3]), 0.5f * (B[1] + B[4]), 0.5f * (B[2] + B[5]) }, md[3];
            memcpy(md, mp, 12); v3norm(md);
            float c = v3dot(md, d);
            if (c > best) { best = c; found = i; Bb = B; }
        }
    }
    if (found < 0) return 0;
    if (cs.shock && crk_frame_start(g, found, Bb, 1, mi, ti, x, inw)) return 1;
    return crk_frame_start(g, found, Bb, 0, mi, ti, x, inw);
}
static float crk_glass_share(const crk_glass *g, const float *d, u32 *rng) {
    /* the glass's share of a 30 deg cone about d (16 rays) */
    float up[3] = { 0.0f, 1.0f, 0.0f }, a[3], b[3];
    if (fabsf(d[1]) > 0.9f) { up[0] = 1.0f; up[1] = 0.0f; }
    v3cross(d, up, a); v3norm(a); v3cross(d, a, b);
    int hit = 0;
    for (int k = 0; k < 16; k++) {
        float r = 0.52f * sqrtf(crk_rand(rng)), ph = 2.0f * CRK_PI * crk_rand(rng);
        float q[3];
        for (int j = 0; j < 3; j++) q[j] = d[j] + r * (cosf(ph) * a[j] + sinf(ph) * b[j]);
        v3norm(q);
        float t; int mi; u32 ti;
        hit += crk_ray(g, q, &t, &mi, &ti);
    }
    return (float)hit / 16.0f;
}
static int crk_main_count(void) { int n = 0; for (u32 i = 0; i < CRK_MAX_STARS; i++) n += stars[i].used && stars[i].main; return n; }
static float crk_milky(void) {
    float a = 0.0f;
    for (u32 i = 0; i < CRK_MAX_STARS; i++) if (stars[i].used) a += CRK_PI * stars[i].rh * stars[i].rh;
    return a;
}
/* ---------------------------------------------------------------- debris chips (bug #139) */
static u32 chips_made;
/* a chip at glass point x (mesh mi, triangle ti) seen along dir from the eye: Ej = its energy share (a bit ~0.25-1, a
   chunk ~3-10, grit ~0.05-0.2), big = 1 a chunk, 2 grit from a kill's smoke (bug #189), delay = s until the piece
   arrives */
static crk_star *chip_new(const crk_glass *g, int mi, u32 ti, const float *x, const float *dir, float Ej, int big, float vr,
                          u32 seed, double delay) {
    int grit = big == 2;
    if (grit) big = 0;
    int nchip = 0; crk_star *old = NULL, *s = NULL;
    for (u32 i = 0; i < CRK_MAX_STARS; i++)
        if (stars[i].used && stars[i].chip) { nchip++; if (!old || stars[i].birth < old->birth) old = &stars[i]; }
    if (nchip >= cs.chip_max && old) star_free(old);
    for (u32 i = 0; i < CRK_MAX_STARS && !s; i++) if (!stars[i].used) s = &stars[i];
    if (!s) return NULL;                               /* (all slots held by the hits' cracks) */
    const crk_gmesh *m = &g->m[mi];
    double saved = gen_birth;
    gen_birth = crk_now() + delay;
    memset(s, 0, sizeof *s);
    s->used = 1; s->chip = grit ? 2 : 1; s->main = 0; s->mesh = mi; s->mat = m->mat; s->frame = 0; s->E = Ej;
    s->thick = 0.0003f;                                /* (a groove in the surface: no band through the glass) */
    memcpy(s->c, x, 12); memcpy(s->dir, dir, 12);
    memcpy(s->n, m->nrm + ti * 3u, 12);
    if (v3dot(s->n, x) > 0.0f) for (int j = 0; j < 3; j++) s->n[j] = -s->n[j];
    float z[3] = { 0.0f, 0.0f, 1.0f }, d = v3dot(z, s->n);
    for (int j = 0; j < 3; j++) s->t1[j] = z[j] - d * s->n[j];         /* u: aft along the glass (the airflow) */
    if (v3norm(s->t1) < 0.2f) { float xx[3] = { 1.0f, 0.0f, 0.0f }; d = v3dot(xx, s->n); for (int j = 0; j < 3; j++) s->t1[j] = xx[j] - d * s->n[j]; v3norm(s->t1); }
    v3cross(s->n, s->t1, s->t2);
    s->seed = seed ? seed : 0x2468ACEu; s->rng = s->seed;
    s->birth = gen_birth; s->last_hit = gen_birth; s->src = 0xFFFFu; s->burst = 1;
    float e = Ej > 10.0f ? 10.0f : Ej, ec = cbrtf(e);  /* (crater size ~ energy^1/3) */
    float vk = vr < 0.3f ? 0.3f : vr > 2.0f ? 2.0f : vr;
    /* ACZ_IMM_DEBRIS_LOOK (bug #260): a bit's chip and a grain's pit bigger, their scratches wider (see above) */
    float lk = ps2_imm_debris_look(), kp = big ? 1.0f : grit ? 1.0f + lk : 1.0f + 1.2f * lk;
    s->rc = (big ? 0.0015f : grit ? 0.0004f : 0.0005f) * (0.8f + 0.4f * crk_rand(&s->rng)) * ec * kp;
    if (s->rc > (big ? 0.0035f : grit ? 0.0004f : 0.0012f) * kp) s->rc = (big ? 0.0035f : grit ? 0.0004f : 0.0012f) * kp;
    if (grit && s->rc < 0.00015f * kp) s->rc = 0.00015f * kp;
    s->rh = s->rc * (big ? 2.4f : 1.7f);
    s->core_a = mats[s->mat].core_a * (big ? 0.85f : grit ? 0.45f : 0.6f * (1.0f + 0.3f * fminf(lk, 1.0f)));
    float Ls = (big ? 0.04f + 0.08f * crk_rand(&s->rng) : grit ? 0.008f + 0.024f * crk_rand(&s->rng)
                : 0.015f + 0.045f * crk_rand(&s->rng)) * vk;
    int na = big ? 3 + (int)(crk_rand(&s->rng) * 3.0f) : grit ? 0
             : lk > 0.0f ? 1 + (int)(crk_rand(&s->rng) * 2.99f)
             : (crk_rand(&s->rng) < 0.3f * fminf(e, 1.0f) ? 1 + (crk_rand(&s->rng) < 0.4f) : 0);
    float La = big ? 0.006f + 0.014f * crk_rand(&s->rng) : (0.002f + 0.004f * crk_rand(&s->rng)) * (1.0f + 0.5f * lk);
    float R = fminf(0.3f, Ls + La + s->rh + 0.03f);
    if (!proj_build(s, m, R)) { star_free(s); gen_birth = saved; return NULL; }
    obs_begin(s);
    /* micro-cracks: short arms from the pit */
    float th0 = crk_rand(&s->rng) * 2.0f * CRK_PI;
    for (int a = 0; a < na && s->ntip < CRK_TIPS; a++) {
        crk_tip *tp = &s->tip[s->ntip++];
        memset(tp, 0, sizeof *tp);
        tp->rng = s->seed * 2654435761u + (u32)a * 40503u + 29u; crk_rng(&tp->rng);
        tp->th = th0 + (float)a * 2.0f * CRK_PI / (float)na + (crk_rand(&tp->rng) - 0.5f) * 1.2f;
        float st = fmaxf(s->rc * 0.8f, 0.0006f);
        tp->p[0] = st * cosf(tp->th); tp->p[1] = st * sinf(tp->th); tp->s = st;
        tp->left = La * (0.5f + crk_rand(&tp->rng)); tp->len0 = st + tp->left;
        tp->w = mats[s->mat].width * (big ? 0.5f : 0.3f);
        tp->kind = SEG_BRANCH; tp->depth = 2;           /* (no further branching) */
    }
    for (int a = 0; a < s->ntip; a++) tip_walk(s, a, s->tip[a].left, 0.0, 0);
    /* the scratch: from the pit's lee side aft along the flow, a smooth groove with the bounces left out */
    {
        float th = (crk_rand(&s->rng) - 0.5f) * (16.0f * CRK_PI / 180.0f), p[2] = { s->rc * cosf(th), s->rc * sinf(th) };
        float w = (big ? 1.0e-4f + 0.6e-4f * crk_rand(&s->rng) : grit ? 0.4e-4f + 0.5e-4f * crk_rand(&s->rng)
                   : 0.5e-4f + 0.3e-4f * crk_rand(&s->rng)) * (big ? 1.0f : grit ? 1.0f + 1.2f * lk : 1.0f + 1.5f * lk);
        float sl = s->rc, left = Ls;
        int on = 1;
        while (left > 5e-4f && s->nseg < CRK_MAX_SEGS) {
            float step = 0.004f + 0.006f * crk_rand(&s->rng);
            if (step > left) step = left;
            th += crk_gauss(&s->rng) * (1.2f * CRK_PI / 180.0f);
            float q[2] = { p[0] + step * cosf(th), p[1] + step * sinf(th) };
            if (!(fmaxf(fabsf(q[0]), fabsf(q[1])) < s->pj.R && proj_on(&s->pj, q, NULL) && proj_edge(&s->pj, q) >= 0.003f)) break;
            if (on) seg_push(s, p, q, sl, sl + step, w * (0.6f + 0.4f * left / Ls), SEG_ARM, 0.45f, 0.0, -1);
            /* a bounce: off for 2-8 mm now and then, more often towards the end */
            on = on ? crk_rand(&s->rng) > 0.12f + 0.25f * (1.0f - left / Ls) : crk_rand(&s->rng) < 0.6f;
            p[0] = q[0]; p[1] = q[1]; sl += step; left -= step;
            w *= 0.97f;
        }
    }
    s->L0 = Ls; s->creep_max = 0.0f; s->pop = 0.0f;
    star_bounds(s);
    gen_birth = saved;
    chips_made++;
    return s;
}
/* the debris model's canopy hits (zero_vr.c imm_debris: ps2_imm_deb_ev kinds 3 tick / 4 clank / 5 grit) -> chips */
static void crk_debris(const crk_glass *g) {
    extern volatile float ps2_imm_deb_ev[32][12];
    extern volatile u32 ps2_imm_deb_seq;
    extern int ps2_imm_debris_mask(void);
    static u32 rd; static int init;
    u32 n = __atomic_load_n(&ps2_imm_deb_seq, __ATOMIC_ACQUIRE);
    if (!init) { rd = n; init = 1; }
    if (n - rd > 32u) rd = n - 32u;
    int m = ps2_imm_debris_mask();
    for (; rd != n; rd++) {
        volatile float *e = ps2_imm_deb_ev[rd % 32u];
        int kind = (int)e[0];
        if (kind != 3 && kind != 4 && kind != 5) continue;
        if (!g || cs.chips <= 0.0f || m <= 0 || !(m & 2)) continue;
        float d[3] = { e[9], e[10], e[11] }, vr = sqrtf(e[5] * e[5] + e[6] * e[6] + e[7] * e[7]) / 200.0f;
        if (v3norm(d) < 0.5f) continue;                /* (not on the canopy in view) */
        float st = e[8] > 0.0f && e[8] <= 1.0f ? e[8] : 0.7f, delay = e[1] > 0.0f && e[1] < 1.0f ? e[1] : 0.0f;
        int big = kind == 4 ? 1 : kind == 5 ? 2 : 0;
        float Ej = cs.chips * st * (big == 1 ? 6.0f : big == 2 ? 0.2f : 0.6f) * fminf(vr * vr, 4.0f);
        float t; int mi; u32 ti;
        if (!crk_ray(g, d, &t, &mi, &ti)) continue;
        float x[3] = { d[0] * t, d[1] * t, d[2] * t };
        u32 seed = (rd * 2654435761u) ^ (++nhits_total * 0x85EBCA6Bu) ^ 0x7F4A7C15u;
        double t0 = crk_now();
        crk_star *s = chip_new(g, mi, ti, x, d, Ej, big, vr, seed, delay);
        if (s && cs.dbg)
            ps2_log("cracks: debris chip (%s, %.0f m/s, strength %.2f -> E %.2f) on %s at %.3f %.3f %.3f: pit %.1f mm, %d "
                    "micro-cracks, scratch %.0f mm in %u segments (%.2f ms)", big == 1 ? "chunk" : big == 2 ? "grit" : "bit",
                    vr * 200.0f, st, Ej,
                    g->m[mi].name, x[0], x[1], x[2], s->rc * 1000.0f, s->ntip, s->L0 * 1000.0f, s->nseg,
                    (crk_now() - t0) * 1000.0);
    }
}
static void crk_hit_apply(const crk_glass *g, const crk_hit *h) {
    static const float kk[4] = { 0.8f, 1.0f, 1.0f, 0.5f };
    int kind = h->kind >= 0 && h->kind <= 3 ? h->kind : CRK_AREA;
    float share = h->share > 1.5f ? 1.5f : h->share;
    float E = share * kk[kind] * cs.strength;
    u32 seed = (h->field * 2654435761u) ^ (++nhits_total * 0x85EBCA6Bu) ^ (h->src * 0xC2B2AE35u);
    u32 rng = seed | 1u;
    float d[3] = { h->dir[0], h->dir[1], h->dir[2] };
    int known = v3norm(d) > 0.5f;
    if (!known) {                                      /* no direction: somewhere in the upper front half */
        float az = (crk_rand(&rng) - 0.5f) * 2.4f, el = 0.1f + 0.6f * crk_rand(&rng);
        d[0] = sinf(az) * cosf(el); d[1] = sinf(el); d[2] = -cosf(az) * cosf(el);
    }
    if (E < 0.004f) {
        if (cs.dbg) ps2_log("cracks: hit field %u kind %d share %.3f: E %.4f too small", h->field, kind, h->share, E);
        return;
    }
    gen_birth = crk_now();
    /* a burst (same attacker within 0.3 s): the 2nd gun hit a second impact a few cm from the first, then they grow */
    int burst2 = 0;
    if (h->src != 0xFFFFu) {
        crk_star *b = NULL;
        for (u32 i = 0; i < CRK_MAX_STARS; i++)
            if (stars[i].used && stars[i].main && stars[i].src == h->src && gen_birth - stars[i].last_hit < 0.3) b = &stars[i];
        if (b) {
            b->last_hit = gen_birth;
            if (kind == CRK_GUN && b->burst < 2 && crk_main_count() < cs.max_main) {
                b->burst = 2; burst2 = 1;
                float up[3] = { 0.0f, 1.0f, 0.0f }, a[3], c[3];
                memcpy(d, b->dir, sizeof d);
                v3cross(d, up, a); v3norm(a); v3cross(d, a, c);
                float r = 0.05f + 0.1f * crk_rand(&rng), ph = 2.0f * CRK_PI * crk_rand(&rng);
                for (int j = 0; j < 3; j++) d[j] += r * (cosf(ph) * a[j] + sinf(ph) * c[j]);
                v3norm(d);
            } else {
                star_grow(b, E * 0.6f);
                if (cs.dbg) ps2_log("cracks: hit field %u kind %d E %.3f: the burst's impact grows (E %.3f)", h->field, kind, E, b->E);
                crk_sound(b->dir, fminf(1.0f, 0.3f + E), b->mat, 0, 2);
                rum_amp = fminf(1.0f, 0.3f + E); __atomic_add_fetch(&rum_seq, 1u, __ATOMIC_RELEASE);
                return;
            }
        }
    }
    /* the cap: more hits grow the nearest main impact */
    if (crk_main_count() >= cs.max_main) {
        crk_star *b = NULL; float ba = 9.0f;
        for (u32 i = 0; i < CRK_MAX_STARS; i++)
            if (stars[i].used && stars[i].main) { float a = crk_ang(stars[i].dir, d); if (a < ba) { ba = a; b = &stars[i]; } }
        if (b) {
            star_grow(b, E * 0.5f);
            if (cs.dbg) ps2_log("cracks: hit field %u kind %d E %.3f: %d impacts already: the nearest grows (%.0f deg away)",
                                h->field, kind, E, cs.max_main, ba * 57.29578f);
            crk_sound(b->dir, fminf(1.0f, 0.3f + E), b->mat, 0, 2);
            rum_amp = fminf(1.0f, 0.3f + E); __atomic_add_fetch(&rum_seq, 1u, __ATOMIC_RELEASE);
            return;
        }
    }
    float Em = kind == CRK_MISSILE || kind == CRK_SPECIAL ? 0.75f * E : E;
    crk_clear_cone(d);
    float t; int mi; u32 ti;
    crk_star *s = NULL;
    double t0 = crk_now();
    float milky = crk_milky(), cap = 0.04f * g->area;
    if (crk_ray(g, d, &t, &mi, &ti)) {
        float x[3] = { d[0] * t, d[1] * t, d[2] * t };
        s = star_new(g, mi, ti, x, d, Em, 1, 0, NULL, seed);
    } else if (E > 0.15f) {                            /* missed the glass: a stress crack from the nearest frame edge */
        float x[3], inw[3];
        if (crk_frame_point(g, d, &mi, &ti, x, inw)) {
            s = star_new(g, mi, ti, x, d, Em, 1, 1, inw, seed);
        }
    }
    /* out of sight (ACZ_VR_CANOPY_CRACK_SHOCK): nothing, or only glass behind the headrest -- a stress crack at the
       canopy's bow on the burst's side instead of a frame crack back there (an impact star there stays) */
    int shock = 0; crk_star *sk = NULL;
    if (cs.shock && E > 0.15f && (!s || s->c[2] > cs.shock_z) && crk_main_count() < cs.max_main) {
        float side = d[0] > 0.05f ? 1.0f : d[0] < -0.05f ? -1.0f : (crk_rand(&rng) < 0.5f ? -1.0f : 1.0f);
        const char *why = !s ? "no glass that way" : s->frame ? "a frame crack behind the headrest, left out"
                                                              : "an impact behind the headrest";
        if (s && s->frame) { star_free(s); s = NULL; }
        float az = (55.0f + 10.0f * (crk_rand(&rng) - 0.5f)) * CRK_PI / 180.0f, el = 5.0f * CRK_PI / 180.0f;
        float q[3] = { side * sinf(az) * cosf(el), sinf(el), -cosf(az) * cosf(el) }, x[3], inw[3];
        int m3; u32 t3;
        shock = 1;
        inw[0] = inw[1] = inw[2] = 0.0f;
        if (crk_frame_point(g, q, &m3, &t3, x, inw)) {
            sk = star_new(g, m3, t3, x, q, Em, 1, 1, inw, seed ^ 0x5BD1E995u);   /* (the research's frame crack energy) */
            if (sk) { sk->src = h->src; sk->burst = 1; sk->last_hit = gen_birth; }
        }
        if (cs.dbg) {
            float walked = 0.0f;
            for (u32 k = 0; sk && k < sk->nseg; k++) walked += sk->seg[k].s1 - sk->seg[k].s0;
            ps2_log("cracks:   out of sight (%s): a stress crack at the %s bow%s at %.3f %.3f %.3f (%s) inward %+.2f %+.2f %+.2f, "
                    "arms %d, planned %.2f m, walked %.2f m in %u segments", why, side > 0.0f ? "right" : "left",
                    sk ? "" : " -- no border point there", sk ? sk->c[0] : 0.0f, sk ? sk->c[1] : 0.0f, sk ? sk->c[2] : 0.0f,
                    sk ? g->m[sk->mesh].name : "-", inw[0], inw[1], inw[2], sk ? sk->ntip : 0, sk ? sk->L0 : 0.0f, walked,
                    sk ? sk->nseg : 0u);
        }
    }
    if (s) {
        s->src = h->src; s->burst = burst2 ? 2 : 1; s->last_hit = gen_birth;
        if (milky + CRK_PI * s->rh * s->rh > cap) {    /* the milky area capped (4 % of the glass) */
            float room = cap - milky;
            float k = room > 0.0f ? sqrtf(room / (CRK_PI * s->rh * s->rh + 1e-9f)) : 0.0f;
            s->rc *= k; s->rh *= k; s->hole *= k;
            star_bounds(s);
        }
    }
    /* fragments of a proximity burst: a few small impacts about the main direction; a gun's second round now and then */
    int nf = 0;
    if (kind == CRK_MISSILE || kind == CRK_SPECIAL) {
        float lam = 6.0f * E * crk_glass_share(g, d, &rng);
        float L = expf(-lam), p = 1.0f;
        do { nf++; p *= crk_rand(&rng); } while (p > L && nf < 8);
        nf--;
        if (nf > 5) nf = 5;
    } else if (kind == CRK_GUN && !burst2 && E > 0.05f && crk_rand(&rng) < 0.35f) nf = 1;   /* (a second round) */
    int made = 0;
    if (shock && (kind == CRK_MISSILE || kind == CRK_SPECIAL)) {   /* (out of sight: chips on the side glass, aft) */
        float lam = 2.0f * E, L = expf(-lam), p = 1.0f; int nc = 0;
        do { nc++; p *= crk_rand(&rng); } while (p > L && nc < 4);
        nc--;
        if (nc > 2) nc = 2;
        float side = sk ? (sk->c[0] > 0.0f ? 1.0f : -1.0f) : (d[0] >= 0.0f ? 1.0f : -1.0f);
        for (int f = 0; f < nc; f++) {
            float az = (105.0f + 20.0f * crk_rand(&rng)) * CRK_PI / 180.0f, el = (25.0f * crk_rand(&rng)) * CRK_PI / 180.0f;
            float q[3] = { side * sinf(az) * cosf(el), sinf(el), -cosf(az) * cosf(el) };
            if (!crk_ray(g, q, &t, &mi, &ti) || t * q[2] > cs.shock_z + 0.4f) continue;
            float x[3] = { q[0] * t, q[1] * t, q[2] * t };
            crk_star *fs = star_new(g, mi, ti, x, q, 0.04f, 0, 0, NULL, crk_rng(&rng));
            if (fs) { fs->src = h->src; fs->burst = 2; fs->last_hit = gen_birth; made++; }
            if (fs && cs.dbg)
                ps2_log("cracks:   out of sight: a chip on %s at %.3f %.3f %.3f, %d arms, length %.3f m", g->m[mi].name,
                        x[0], x[1], x[2], fs->ntip, fs->L0);
        }
    }
    for (int f = 0; f < nf; f++) {
        float up[3] = { 0.0f, 1.0f, 0.0f }, a[3], b[3], q[3];
        v3cross(d, up, a); v3norm(a); v3cross(d, a, b);
        float r = (kind == CRK_GUN ? 0.12f : 0.45f) * sqrtf(crk_rand(&rng)), ph = 2.0f * CRK_PI * crk_rand(&rng);
        for (int j = 0; j < 3; j++) q[j] = d[j] + r * (cosf(ph) * a[j] + sinf(ph) * b[j]);
        v3norm(q);
        crk_clear_cone(q);
        if (!crk_ray(g, q, &t, &mi, &ti)) continue;
        float x[3] = { q[0] * t, q[1] * t, q[2] * t };
        crk_star *fs = star_new(g, mi, ti, x, q, kind == CRK_GUN ? E * 0.4f : 0.04f, kind == CRK_GUN, 0, NULL, crk_rng(&rng));
        if (fs) { fs->src = h->src; fs->burst = 2; fs->last_hit = gen_birth; made++; }
        if (fs && cs.dbg)
            ps2_log("cracks:   fragment impact on %s at %.3f %.3f %.3f, %d arms, length %.3f m", g->m[mi].name, x[0], x[1], x[2],
                    fs->ntip, fs->L0);
    }
    double ms = (crk_now() - t0) * 1000.0;
    u32 nseg = 0; for (u32 i = 0; i < CRK_MAX_STARS; i++) nseg += stars[i].used ? stars[i].nseg : 0u;
    ps2_log("cracks: hit field %u kind %d share %.3f HP left %.2f%s dir %+.2f %+.2f %+.2f -> E %.3f: %s%s%s%s%s%s, %d "
            "fragment impact%s; %d impacts, %u segments (%.2f ms)", h->field, kind, h->share, h->hp_left, h->test ? " (test)" : "",
            d[0], d[1], d[2], E, s ? (s->frame ? "a frame crack on " : "an impact on ") : shock ? "" : "no glass that way",
            s ? g->m[s->mesh].name : "", s && s->c[2] > cs.shock_z ? " (behind the headrest)" : "",
            shock ? (s ? "; out of sight: " : "out of sight: ") : "",
            shock ? (sk ? "a stress crack at the bow of " : "no bow crack") : "", sk ? g->m[sk->mesh].name : "",
            made, made == 1 ? "" : "s", crk_main_count(), nseg, ms);
    if (s && cs.dbg)
        ps2_log("cracks:   impact at %.3f %.3f %.3f (%s) arms %d tips %d segments %u, core %.1f mm halo %.1f mm hole %.1f mm, "
                "first length %.2f m, bound %.2f m sag %.3f m", s->c[0], s->c[1], s->c[2], mats[s->mat].name, s->ntip,
                s->ntip, s->nseg, s->rc * 1000.0f, s->rh * 1000.0f, s->hole * 1000.0f, s->L0, s->rb, s->sag);
    if (s || sk || made) {
        float st = fminf(1.0f, 0.25f + 1.2f * E);
        crk_sound(s ? s->dir : sk ? sk->dir : d, st, s ? s->mat : sk ? sk->mat : MAT_ACRYLIC, 0, (u32)made);
        rum_amp = st; __atomic_add_fetch(&rum_seq, 1u, __ATOMIC_RELEASE);
    }
}

/* ---------------------------------------------------------------- creep */
static void crk_creep(double dt) {
    static float last_log;
    float stamp = ps2_imm_in[3];
    int fly = stamp > 0.0f && (float)ps2_kernel_vblank_count() - stamp <= 8.0f;
    if (!fly || !cs.creep) return;
    float kt = ps2_imm_in[2], alt_m = ps2_imm_in[7];
    float ft = alt_m * 3.2808f;
    float dp = ft <= 8000.0f ? 0.0f : ft >= 23000.0f ? 34.5f : 34.5f * (ft - 8000.0f) / 15000.0f;        /* kPa */
    float rho = 1.225f * expf(-(alt_m > 0.0f ? alt_m : 0.0f) / 8500.0f), V = kt * 0.5144f;
    float q = 0.5f * rho * V * V * 0.001f;                                                                    /* kPa */
    float G = ps2_gf_vis[5], buf = ps2_gf_vis[2];
    if (!(G > -15.0f && G < 25.0f)) G = 1.0f;
    if (!(buf >= 0.0f && buf <= 2.0f)) buf = 0.0f;
    float load = dp + 0.7f * q + 2.0f * fmaxf(0.0f, fabsf(G) - 3.0f) + 8.0f * buf;
    float x = load / 15.0f - 1.0f;
    float v = x > 0.0f ? fminf(0.003f, 0.001f * x * x) : 0.0f;                                                /* m/s */
    last_log += (float)dt;
    if (cs.dbg && last_log > 2.0f) {
        last_log = 0.0f;
        ps2_log("cracks: creep load %.1f kPa (cabin %.1f, q %.1f, G %.1f, buffet %.2f) -> %.1f mm/s", load, dp, q, G, buf, v * 1000.0f);
    }
    if (v <= 0.0f) return;
    for (u32 i = 0; i < CRK_MAX_STARS; i++) {
        crk_star *s = &stars[i];
        if (!s->used || s->creep_max <= 0.0f || s->creep_used >= s->creep_max) continue;
        s->creep_acc += v * (float)dt;
        if (s->creep_acc < s->pop) continue;
        /* a pop: one arm still free runs on by the pop's length */
        int cand[CRK_TIPS], n = 0;
        for (int k = 0; k < s->ntip; k++) if (s->tip[k].state == 0) cand[n++] = k;
        float len = s->pop;
        s->creep_acc -= s->pop;
        s->pop = 0.003f + 0.012f * crk_rand(&s->rng);
        if (!n) { s->creep_max = 0.0f; continue; }
        int k = cand[(int)(crk_rand(&s->rng) * (float)n) % n];
        obs_begin(s);
        gen_birth = crk_now();
        double saved = gen_birth;
        gen_birth = saved - (gen_vrev > 0.0f ? (double)(s->tip[k].s / gen_vrev) : 0.0);
        s->tip[k].left += len;
        float w = tip_walk(s, k, len, 0.0, 1);
        gen_birth = saved;
        s->creep_used += w > 0.0f ? w : len;
        star_bounds(s);
        crk_sound(s->dir, 0.08f + 0.1f * fminf(1.0f, len / 0.015f), s->mat, 1, 0);
        if (cs.dbg) ps2_log("cracks: creep pop on impact %u: %.1f mm (%.0f of %.0f mm used) at load %.1f kPa", i, w * 1000.0f,
                            s->creep_used * 1000.0f, s->creep_max * 1000.0f, load);
    }
}

/* ---------------------------------------------------------------- the occupancy grid
   Over the glass's box in cells of ~4 cm (at most CRK_OCC_CELLS): per cell the impacts (bit = their slot) with a crack
   segment, core, halo or hole within 6 mm of the cell. cockpit.frag looks the fragment's cell up first: glass far from
   every crack costs that one load. Built on the worker after each change. */
static u16 occ[CRK_OCC_CELLS];
static float occ_o[3], occ_cs; static int occ_n[3]; static u32 occ_ver;
static void occ_mark(const float *lo, const float *hi, u16 bit) {
    int a[3], b[3];
    for (int j = 0; j < 3; j++) {
        a[j] = (int)floorf((lo[j] - occ_o[j]) / occ_cs); b[j] = (int)floorf((hi[j] - occ_o[j]) / occ_cs);
        if (a[j] < 0) a[j] = 0;
        if (b[j] >= occ_n[j]) b[j] = occ_n[j] - 1;
        if (a[j] > b[j]) return;
    }
    for (int z = a[2]; z <= b[2]; z++)
        for (int y = a[1]; y <= b[1]; y++)
            for (int x = a[0]; x <= b[0]; x++) occ[x + occ_n[0] * (y + occ_n[1] * z)] |= bit;
}
static void occ_build(const crk_glass *g) {
    memset(occ, 0, sizeof occ);
    occ_n[0] = occ_n[1] = occ_n[2] = 0;
    if (!g || !(g->bmax[0] > g->bmin[0])) return;
    float ext[3], vol = 1.0f;
    for (int j = 0; j < 3; j++) { ext[j] = g->bmax[j] - g->bmin[j]; vol *= ext[j]; }
    occ_cs = cbrtf(vol / (float)(CRK_OCC_CELLS - 4096u));
    if (occ_cs < 0.02f) occ_cs = 0.02f;
    for (;;) {
        u32 tot = 1u;
        for (int j = 0; j < 3; j++) { occ_n[j] = (int)ceilf(ext[j] / occ_cs); if (occ_n[j] < 1) occ_n[j] = 1; tot *= (u32)occ_n[j]; }
        if (tot <= CRK_OCC_CELLS) break;
        occ_cs *= 1.05f;
    }
    memcpy(occ_o, g->bmin, sizeof occ_o);
    const float m = 0.006f;                          /* (lines, and the thickness band: at most 2.8 mm either side) */
    for (u32 i = 0; i < CRK_MAX_STARS; i++) {
        const crk_star *s = &stars[i];
        if (!s->used) continue;
        u16 bit = (u16)(1u << i);
        float r = fmaxf(fmaxf(s->rh, s->hole * 1.5f), 0.004f) + m;
        float lo[3] = { s->c[0] - r, s->c[1] - r, s->c[2] - r }, hi[3] = { s->c[0] + r, s->c[1] + r, s->c[2] + r };
        occ_mark(lo, hi, bit);
        for (u32 k = 0; k < s->nseg; k++) {
            const crk_seg *sg = &s->seg[k];
            float d[3] = { sg->b3[0] - sg->a3[0], sg->b3[1] - sg->a3[1], sg->b3[2] - sg->a3[2] };
            float len = sqrtf(v3dot(d, d));
            int ns = 1 + (int)(len / (0.5f * occ_cs));
            for (int t = 0; t <= ns; t++) {
                float f = (float)t / (float)ns, q[3];
                for (int j = 0; j < 3; j++) q[j] = sg->a3[j] + f * d[j];
                float l2[3] = { q[0] - m, q[1] - m, q[2] - m }, h2[3] = { q[0] + m, q[1] + m, q[2] + m };
                occ_mark(l2, h2, bit);
            }
        }
    }
}

/* ---------------------------------------------------------------- per frame */
int ps2_crk_count(void) { int n = 0; for (u32 i = 0; i < CRK_MAX_STARS; i++) n += stars[i].used; return n; }
int ps2_crk_mesh_slot(const crk_glass *g, u32 mesh) {
    if (!g || g != cur_glass) return -1;
    for (int i = 0; i < g->nm; i++) {
        if (g->m[i].mesh != mesh || !g->m[i].keep) continue;
        for (u32 k = 0; k < CRK_MAX_STARS; k++) if (stars[k].used && stars[k].mesh == i) return i;
        return -1;
    }
    return -1;
}
/* The generator runs on its own thread (a hit's impacts take 0.2-2 ms: kept off the renderer); the renderer passes the
   glass and the view (ps2_crk_update) and packs the result for the GPU when the worker is not busy */
static crk_view want_view; static int want_view_ok;
static volatile int crk_n;
static volatile int crk_nhit;                 /* (the impacts that are not debris chips: ps2_crk_hits) */
static volatile int crk_ngrit;                /* (the chips that are grit pits from a kill's smoke: ps2_crk_grit) */
static volatile float crk_hole_area;          /* (m^2: the holes' whistle) */
static void crk_step(void) {
    static u32 seen_hit, seen_reset; static int reset_init; static double last_t;
    crk_glass *g = want_glass;
    double now = crk_now();
    if (epoch == 0.0) epoch = now;
    double dt = last_t > 0.0 ? now - last_t : 0.0;
    last_t = now;
    if (dt > 0.2) dt = 0.2;
    if (want_view_ok) {
        memcpy(view_head, want_view.head, sizeof view_head); view_head_ok = want_view.head_ok;
        memcpy(gen_bore, want_view.bore, 12);
        if (v3norm(gen_bore) < 0.5f) { gen_bore[0] = 0.0f; gen_bore[1] = -0.05f; gen_bore[2] = -1.0f; v3norm(gen_bore); }
    }
    gen_clear = cs.clear_deg > 0.0f ? cosf(cs.clear_deg * CRK_PI / 180.0f) : 2.0f;
    u32 r = ps2_imm_reset;
    if (!reset_init) { seen_reset = r; reset_init = 1; }
    if (r != seen_reset) {
        seen_reset = r;
        static int rs = -1;
        if (rs < 0) { const char *e = getenv("ACZ_IMM_RESET"); rs = !(e && *e == '0'); }
        if (rs) crk_clear("mission start / RETRY / jump / respawn");
    }
    if (g != cur_glass || (g && g->serial != cur_serial)) {
        if (ps2_crk_count()) crk_clear("the cockpit model changed");
        cur_glass = g; cur_serial = g ? g->serial : 0u;
    }
    u32 hs = __atomic_load_n(&hit_seq, __ATOMIC_ACQUIRE);
    if (hs - seen_hit > CRK_EV_RING) seen_hit = hs - CRK_EV_RING;
    gen_vrev = cs.reveal_s > 0.0f ? 0.25f / cs.reveal_s : 0.0f;     /* (a 25 cm arm in the reveal time) */
    while (seen_hit != hs) {
        crk_hit h = hit_ring[seen_hit % CRK_EV_RING];
        seen_hit++;
        if (!g) { ps2_log("cracks: hit field %u kind %d share %.3f: no 3D cockpit glass loaded", h.field, h.kind, h.share); continue; }
        crk_hit_apply(g, &h);
    }
    if (g) crk_creep(dt);
    crk_debris(g);                                     /* (bug #139: the debris model's canopy hits -> chips) */
    if (gpu_ver != occ_ver) {                          /* (the impacts changed: the occupancy with them) */
        double t0 = crk_now();
        occ_build(cur_glass);
        occ_ver = gpu_ver;
        if (cs.dbg) ps2_log("cracks: occupancy %d x %d x %d cells of %.1f cm (%.2f ms)", occ_n[0], occ_n[1], occ_n[2],
                            occ_cs * 100.0f, (crk_now() - t0) * 1000.0);
    }
    crk_n = ps2_crk_count();
    {   int h = 0, gr = 0;
        for (u32 i = 0; i < CRK_MAX_STARS; i++) { h += stars[i].used && !stars[i].chip; gr += stars[i].used && stars[i].chip == 2; }
        crk_nhit = h; crk_ngrit = gr;
    }
    float ha = 0.0f;
    for (u32 i = 0; i < CRK_MAX_STARS; i++) if (stars[i].used) ha += CRK_PI * stars[i].hole * stars[i].hole;
    crk_hole_area = ha;
}
static void *crk_worker(void *arg) {
    (void)arg;
    extern void ps2_host_thread_start(const char *who);
    extern void ps2_host_thread_replace(void);
    ps2_host_thread_start("bg");                      /* (placement: a background worker for PS2_THREAD_CORES=h) */
    for (;;) {
        struct timespec ts = { 0, 4000000 };          /* every 4 ms: a hit cracks the glass within a frame or two */
        nanosleep(&ts, NULL);
        ps2_host_thread_replace();
        pthread_mutex_lock(&crk_mx);
        u64 cz0 = ps2_cz_t();                         /* (PS2_CPU_PROF) */
        crk_step();
        ps2_cz_end(CZ_CRK_WORKER, cz0);
        pthread_mutex_unlock(&crk_mx);
    }
    return NULL;
}
int ps2_crk_hits(void) { return crk_nhit; }
int ps2_crk_grit(void) { return crk_ngrit; }
int ps2_crk_update(crk_glass *g, const crk_view *v) {
    static int started;
    crk_cfg();
    if (cs.strength <= 0.0f) return 0;
    if (!started) {
        started = 1;
        pthread_t t;
        if (pthread_create(&t, NULL, crk_worker, NULL) == 0) pthread_detach(t);
        else ps2_log("cracks: no worker thread");
    }
    if (!pthread_mutex_trylock(&crk_mx)) {            /* (busy: the next frame passes them) */
        want_glass = g;
        if (v) { want_view = *v; want_view_ok = 1; }
        pthread_mutex_unlock(&crk_mx);
    }
    return crk_n;
}

/* ---------------------------------------------------------------- GPU packing */
uint32_t ps2_crk_gpu_version(void) { return gpu_ver; }
static float gpu_sun[4], gpu_sunrgb[4];
void ps2_crk_set_sun(const float *S, const float *rgb) {
    if (S) { memcpy(gpu_sun, S, 12); }
    if (rgb) { memcpy(gpu_sunrgb, rgb, 12); }
}
/* layout (vec4): [0] now, impacts, strength, debug view; [1] sun xyz, 1 = valid; [2] sun rgb, -; [3] spare;
   [4 + 8 i ..] impact i: c.xyz rb | t1.xyz mesh | t2.xyz material | n.xyz sag | u0 v0 cell gn | first segment (vec4
   index), segments, cell table offset (uint part), flags | core, halo, core alpha, hole | birth, -, -, -;
   [4 + 8 CRK_MAX_STARS ..] segments: a.xy b.xy | t0 t1 width flags. uint part: per impact gn x gn cells (offset << 8 |
   count), then the segment index lists (indices relative to the impact's first segment) */
int ps2_crk_gpu_write(float *f4, uint32_t *u, int full) {
    double now = crk_now();
    int ns = 0;
    float *h = f4;
    if (full && pthread_mutex_trylock(&crk_mx)) full = 0;  /* (the worker is busy: the old data stays a frame longer) */
    if (full) {
        u32 sbase = CRK_HDR + CRK_MAX_STARS * 8u, nsg = 0, uo = CRK_OCC_CELLS / 2u;
        u32 nocc = (u32)(occ_n[0] * occ_n[1] * occ_n[2]);
        memcpy(u, occ, (size_t)((nocc + 1u) / 2u) * 4u);                 /* (two u16 a uint, little-endian) */
        for (u32 i = 0; i < CRK_MAX_STARS; i++) {
            crk_star *s = &stars[i];
            if (!s->used) { f4[(CRK_HDR + i * 8u) * 4u + 3u] = 0.0f; continue; }
            ns = (int)i;
            u32 take = s->nseg;
            if (nsg + take > CRK_MAX_SEGS) take = CRK_MAX_SEGS - nsg;
            /* the grid over the impact's segments and core */
            float lo[2] = { -fmaxf(s->rh, s->hole * 1.3f), -fmaxf(s->rh, s->hole * 1.3f) }, hi[2] = { -lo[0], -lo[1] };
            for (u32 k = 0; k < take; k++) {
                const crk_seg *g = &s->seg[k];
                for (int j = 0; j < 2; j++) {
                    lo[j] = fminf(lo[j], fminf(g->a[j], g->b[j])); hi[j] = fmaxf(hi[j], fmaxf(g->a[j], g->b[j]));
                }
            }
            lo[0] -= 0.004f; lo[1] -= 0.004f; hi[0] += 0.004f; hi[1] += 0.004f;
            float ext = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
            float cell = fmaxf(ext / 40.0f, 0.006f);          /* (finer cells: fewer segments a fragment tests) */
            int gn = (int)ceilf(ext / cell); if (gn < 1) gn = 1; if (gn > 40) gn = 40;
            u32 nc = (u32)(gn * gn);
            if (uo + nc + take * 4u > CRK_MAX_U) { take = 0; }
            u32 coff = uo; uo += nc;
            /* count, then fill (a segment into every cell its box + 3 mm covers; 48 a cell at most) */
            u32 cnt[1600]; memset(cnt, 0, sizeof cnt);
            for (int pass = 0; pass < 2; pass++) {
                u32 fillp[1600];
                if (pass) {
                    u32 acc = uo;
                    for (u32 c = 0; c < nc; c++) {
                        u32 n = cnt[c] > 48u ? 48u : cnt[c];
                        if (acc + n > CRK_MAX_U) n = 0;
                        u[coff + c] = acc << 8 | n;
                        fillp[c] = acc; acc += n;
                        cnt[c] = n;
                    }
                    uo = acc;
                }
                u32 used[1600]; memset(used, 0, sizeof used);
                for (u32 k = 0; k < take; k++) {
                    const crk_seg *g = &s->seg[k];
                    int x0 = (int)floorf((fminf(g->a[0], g->b[0]) - 0.003f - lo[0]) / cell), x1 = (int)floorf((fmaxf(g->a[0], g->b[0]) + 0.003f - lo[0]) / cell);
                    int y0 = (int)floorf((fminf(g->a[1], g->b[1]) - 0.003f - lo[1]) / cell), y1 = (int)floorf((fmaxf(g->a[1], g->b[1]) + 0.003f - lo[1]) / cell);
                    x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 >= gn ? gn - 1 : x1; y1 = y1 >= gn ? gn - 1 : y1;
                    for (int y = y0; y <= y1; y++)
                        for (int x = x0; x <= x1; x++) {
                            u32 c = (u32)(y * gn + x);
                            if (!pass) cnt[c]++;
                            else if (used[c] < cnt[c]) u[fillp[c] + used[c]++] = k;
                        }
                }
            }
            float *S = f4 + (CRK_HDR + i * 8u) * 4u;
            S[0] = s->c[0]; S[1] = s->c[1]; S[2] = s->c[2]; S[3] = s->rb;
            S[4] = s->t1[0]; S[5] = s->t1[1]; S[6] = s->t1[2]; S[7] = (float)s->mesh;
            S[8] = s->t2[0]; S[9] = s->t2[1]; S[10] = s->t2[2]; S[11] = (float)s->mat;
            S[12] = s->n[0]; S[13] = s->n[1]; S[14] = s->n[2]; S[15] = s->sag;
            S[16] = lo[0]; S[17] = lo[1]; S[18] = cell; S[19] = (float)gn;
            S[20] = (float)(sbase + nsg * 2u); S[21] = (float)take; S[22] = (float)coff; S[23] = (float)(s->frame | s->main << 1);
            S[24] = s->rc; S[25] = s->rh; S[26] = s->core_a; S[27] = s->hole;
            S[28] = (float)(s->birth - epoch); S[29] = (float)i; S[30] = s->thick; S[31] = 0.0f;
            for (u32 k = 0; k < take; k++) {
                const crk_seg *g = &s->seg[k];
                float *G = f4 + (sbase + (nsg + k) * 2u) * 4u;
                G[0] = g->a[0]; G[1] = g->a[1]; G[2] = g->b[0]; G[3] = g->b[1];
                G[4] = g->t0; G[5] = g->t1; G[6] = g->w; G[7] = (float)g->fl;
            }
            if (cs.dbg >= 2)
                ps2_log("cracks: gpu impact %d (#%u%s): c %.3f %.3f %.3f rb %.3f mesh %d sag %.3f grid %.3f %.3f cell %.4f gn %d "
                        "segments %u at %u, cells at %u", ns, i, s->main ? " main" : "", s->c[0], s->c[1], s->c[2], s->rb,
                        s->mesh, s->sag, lo[0], lo[1], cell, gn, take, sbase + nsg * 2u, coff);
            nsg += take;
            ns = (int)i + 1;
            s->dirty = 0;
        }
        h[1] = (float)ns;
        h[16] = occ_o[0]; h[17] = occ_o[1]; h[18] = occ_o[2]; h[19] = occ_cs;
        h[20] = (float)occ_n[0]; h[21] = (float)occ_n[1]; h[22] = (float)occ_n[2]; h[23] = (float)nocc;
        pthread_mutex_unlock(&crk_mx);
        if (cs.dbg) {
            double us = (crk_now() - now) * 1e6;
            ps2_log("cracks: GPU data rewritten: %d impact slots, %u segments, %.0f us (renderer)", ns, (unsigned)nsg, us);
        }
    }
    h[0] = (float)(now - (epoch > 0.0 ? epoch : now)); h[2] = cs.strength; h[3] = (float)cs.show;
    h[4] = gpu_sun[0]; h[5] = gpu_sun[1]; h[6] = gpu_sun[2]; h[7] = gpu_sun[0] != 0.0f || gpu_sun[1] != 0.0f || gpu_sun[2] != 0.0f;
    h[8] = gpu_sunrgb[0]; h[9] = gpu_sunrgb[1]; h[10] = gpu_sunrgb[2]; h[11] = 0.0f;
    h[12] = h[13] = h[14] = h[15] = 0.0f;
    return full;
}

/* ---------------------------------------------------------------- rumble (the renderer's gamepad) */
void ps2_crk_rumble(uint64_t now_ns, float *lo, float *hi) {
    static u32 seen; static uint64_t until; static float amp;
    *lo = *hi = 0.0f;
    crk_cfg();
    if (!cs.fx_rumble || cs.strength <= 0.0f) return;
    u32 q = __atomic_load_n(&rum_seq, __ATOMIC_ACQUIRE);
    if (q != seen) { seen = q; amp = rum_amp; until = now_ns + 45000000ull; }
    if (now_ns < until) {
        float f = (float)(until - now_ns) * (1.0f / 45000000.0f);
        *hi = amp * (0.55f + 0.45f * f);                /* a sharp jolt on the small motor */
        *lo = amp * 0.3f * f;                           /* and a touch of the large one */
    }
}

/* ---------------------------------------------------------------- sound (the audio thread) */
#define CK_RATE 48000
#define CK_VOICES 8
typedef struct { float b0, b1, b2, a1, a2, z1, z2; } ck_bq;
static float ck_run(ck_bq *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}
static void ck_bp(ck_bq *f, float fc, float q) {
    float w = 2.0f * CRK_PI * fc / CK_RATE, al = sinf(w) / (2.0f * q), a0 = 1.0f + al;
    f->b0 = al / a0; f->b1 = 0.0f; f->b2 = -al / a0; f->a1 = -2.0f * cosf(w) / a0; f->a2 = (1.0f - al) / a0;
    f->z1 = f->z2 = 0.0f;
}
static void ck_lp(ck_bq *f, float fc, float q) {
    float w = 2.0f * CRK_PI * fc / CK_RATE, al = sinf(w) / (2.0f * q), c = cosf(w), a0 = 1.0f + al;
    f->b0 = (1.0f - c) * 0.5f / a0; f->b1 = (1.0f - c) / a0; f->b2 = f->b0; f->a1 = -2.0f * c / a0; f->a2 = (1.0f - al) / a0;
    f->z1 = f->z2 = 0.0f;
}
typedef struct {
    int on; u32 t, len; float amp;
    u32 snap_t[20]; float snap_a[20], snap_d[20]; int nsnap;
    ck_bq res[3]; float res_g[3]; ck_bq lp; float thump;
    float hp_z;
    float ring[64]; u32 wp; float tau[2], al[2], gain[2], hx[2], hy[2];
} ck_voice;
static ck_voice ckv[CK_VOICES];
static u32 ck_rng_s = 0x2545F491u;
static float ck_noise(void) { return (float)(crk_rng(&ck_rng_s) >> 8) * (2.0f / 16777216.0f) - 1.0f; }
static void ck_start(const crk_snd *e) {
    ck_voice *v = NULL;
    for (int i = 0; i < CK_VOICES && !v; i++) if (!ckv[i].on) v = &ckv[i];
    if (!v) { v = &ckv[0]; for (int i = 1; i < CK_VOICES; i++) if (ckv[i].t > v->t) v = &ckv[i]; }
    memset(v, 0, sizeof *v);
    v->on = 1;
    float st = e->st;
    if (e->kind == 1) {                                 /* a creep pop: one faint tick, a short glassy ring */
        v->nsnap = 1; v->snap_t[0] = 0; v->snap_a[0] = 1.0f; v->snap_d[0] = 0.0004f * CK_RATE;
        ck_bp(&v->res[0], 3000.0f + 1500.0f * (float)(ck_rng_s & 255u) / 255.0f, 18.0f); v->res_g[0] = 0.6f;
        v->len = (u32)(0.04f * CK_RATE);
        v->amp = 0.009f * st / 0.15f;
    } else if (e->mat == MAT_GLASS) {                   /* glass laminate: a duller crunch of many small breaks */
        v->nsnap = 10 + (int)(st * 8.0f);
        for (int k = 0; k < v->nsnap && k < 20; k++) {
            ck_rng_s = ck_rng_s * 1664525u + 1013904223u;
            v->snap_t[k] = (u32)((float)k * (0.004f + 0.006f * (float)(ck_rng_s >> 24) / 255.0f) * CK_RATE);
            v->snap_a[k] = k ? 0.25f + 0.35f * (float)((ck_rng_s >> 12) & 15u) / 15.0f : 1.0f;
            v->snap_d[k] = (k ? 0.0006f : 0.0015f) * CK_RATE;
        }
        if (v->nsnap > 20) v->nsnap = 20;
        ck_bp(&v->res[0], 2200.0f, 9.0f); ck_bp(&v->res[1], 3400.0f, 10.0f); ck_bp(&v->res[2], 5100.0f, 12.0f);
        v->res_g[0] = 0.5f; v->res_g[1] = 0.4f; v->res_g[2] = 0.3f;
        ck_lp(&v->lp, 180.0f, 1.2f); v->thump = 0.9f;
        v->len = (u32)(0.16f * CK_RATE);
        v->amp = 0.07f + 0.085f * st;
    } else if (e->mat == MAT_PC) {                      /* polycarbonate: a thud and a tick */
        v->nsnap = 2; v->snap_t[0] = 0; v->snap_a[0] = 1.0f; v->snap_d[0] = 0.003f * CK_RATE;
        v->snap_t[1] = (u32)(0.012f * CK_RATE); v->snap_a[1] = 0.4f; v->snap_d[1] = 0.0005f * CK_RATE;
        ck_bp(&v->res[0], 2000.0f, 6.0f); v->res_g[0] = 0.3f;
        ck_lp(&v->lp, 600.0f, 0.9f); v->thump = 1.6f;
        v->len = (u32)(0.09f * CK_RATE);
        v->amp = 0.07f + 0.085f * st;
    } else {                                            /* acrylic (and its laminate): a snap, a ringing, a short crackle */
        v->nsnap = 1 + 3 + (int)(st * 4.0f) + (int)(e->snaps > 6u ? 6u : e->snaps);
        if (v->nsnap > 20) v->nsnap = 20;
        v->snap_t[0] = 0; v->snap_a[0] = 1.0f; v->snap_d[0] = 0.0009f * CK_RATE;
        for (int k = 1; k < v->nsnap; k++) {
            ck_rng_s = ck_rng_s * 1664525u + 1013904223u;
            v->snap_t[k] = (u32)((0.003f + 0.05f * (float)(ck_rng_s >> 24) / 255.0f) * CK_RATE);
            v->snap_a[k] = 0.12f + 0.3f * (float)((ck_rng_s >> 16) & 255u) / 255.0f;
            v->snap_d[k] = 0.0004f * CK_RATE;
        }
        float f = e->mat == MAT_PCLAM ? 1.3f : 1.0f;
        ck_bp(&v->res[0], 1700.0f * f, 24.0f); ck_bp(&v->res[1], 2650.0f * f, 28.0f); ck_bp(&v->res[2], 3800.0f * f, 30.0f);
        v->res_g[0] = 0.9f; v->res_g[1] = 0.6f; v->res_g[2] = 0.4f;
        ck_lp(&v->lp, 260.0f, 0.9f); v->thump = e->mat == MAT_PCLAM ? 0.8f : 0.35f;
        v->len = (u32)(0.14f * CK_RATE);
        v->amp = 0.08f + 0.10f * st;
    }
    /* binaural placement: Woodworth delay and a one-pole head shadow per ear (as ps2_gforce_audio.c) */
    float x = e->x;
    int hrtf = ps2_spatial_hrtf_on();
    for (int ear = 0; ear < 2; ear++) {
        float c = ear ? x : -x, th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
        if (hrtf) {
            v->tau[ear] = 2.0f + (0.0875f / 343.0f * CK_RATE) * (th < 1.5707963f ? 1.0f - c : 1.0f + th - 1.5707963f);
            float a = (1.05f + 0.95f * cosf(th * (180.0f / 150.0f))) * (1.0f / 0.7564f);
            v->al[ear] = a < 0.08f ? 0.08f : a > 2.0f ? 2.0f : a;
            v->gain[ear] = 1.0f;
        } else {
            float p = 0.5f + 0.5f * x;
            v->tau[ear] = 2.0f; v->al[ear] = 1.0f;
            v->gain[ear] = (ear ? sinf(p * 1.5707963f) : cosf(p * 1.5707963f)) * 1.4142136f;
        }
    }
}
void ps2_crk_mix(int16_t *buf, int frames) {
    static u32 seen; static int dbg = -1; static double e_acc; static u32 n_acc; static float peak;
    crk_cfg();
    if (!cs.fx_sound || cs.strength <= 0.0f) return;
    if (dbg < 0) dbg = cs.dbg;
    u32 q = __atomic_load_n(&snd_seq, __ATOMIC_ACQUIRE);
    if (q - seen > 16u) seen = q - 16u;
    while (seen != q) { ck_start(&snd_ring[seen % 16u]); seen++; }
    /* a hole whistles: an edge tone in the airflow over it, rising with the dynamic pressure, low and steady */
    static float wh_lvl, wh_ph, wh_lfo; static ck_bq wh_bp; static float wh_f;
    float stamp = ps2_imm_in[3];
    int fly = stamp > 0.0f && (float)ps2_kernel_vblank_count() - stamp <= 8.0f;
    float kt = ps2_imm_in[2], alt = ps2_imm_in[7];
    float rho = 1.225f * expf(-(alt > 0.0f ? alt : 0.0f) / 8500.0f), V = kt * 0.5144f, qd = 0.5f * rho * V * V * 0.001f;
    float hk = crk_hole_area / 2e-4f; hk = hk > 1.0f ? 1.0f : hk;
    float wt = fly && hk > 0.0f ? hk * fminf(1.0f, qd / 30.0f) : 0.0f;
    {   extern float ps2_imm_duck(int cls);          /* the immersion bus: a bed, lower under a loud game and the radio */
        wt *= ps2_imm_duck(0); }
    if (wt > 0.0f || wh_lvl > 1e-5f) {
        float f = 900.0f + 1400.0f * sqrtf(fminf(qd, 60.0f) / 30.0f);
        if (fabsf(f - wh_f) > 20.0f) { float z1 = wh_bp.z1, z2 = wh_bp.z2; ck_bp(&wh_bp, f, 14.0f); wh_bp.z1 = z1; wh_bp.z2 = z2; wh_f = f; }
    }
    int any = wt > 0.0f || wh_lvl > 1e-5f;
    for (int i = 0; i < CK_VOICES; i++) any |= ckv[i].on;
    if (!any) return;
    const float K = 2.0f * CK_RATE / (2.0f * 343.0f / 0.0875f);
    for (int n = 0; n < frames; n++) {
        float l = 0.0f, r = 0.0f;
        wh_lvl += (wt - wh_lvl) * 0.0002f;            /* (~100 ms glides) */
        if (wh_lvl > 1e-5f) {
            wh_lfo += 5.3f / CK_RATE; if (wh_lfo > 1.0f) wh_lfo -= 1.0f;
            float w = ck_run(&wh_bp, ck_noise()) * wh_lvl * 0.35f * (0.85f + 0.15f * sinf(2.0f * CRK_PI * wh_lfo));
            l += w; r += w;
            (void)wh_ph;
        }
        for (int i = 0; i < CK_VOICES; i++) {
            ck_voice *v = &ckv[i];
            if (!v->on) continue;
            float ex = 0.0f;
            for (int k = 0; k < v->nsnap; k++) {
                if (v->t < v->snap_t[k]) continue;
                float dt = (float)(v->t - v->snap_t[k]);
                if (dt > 8.0f * v->snap_d[k]) continue;
                ex += v->snap_a[k] * expf(-dt / v->snap_d[k]) * ck_noise();
            }
            /* the snap high-passed (a crack is bright), the plate's modes rung by it, a low thump for heavy panes */
            float hp = ex - v->hp_z; v->hp_z += 0.25f * (ex - v->hp_z);
            float s = hp * 0.8f;
            for (int k = 0; k < 3; k++) if (v->res_g[k] > 0.0f) s += ck_run(&v->res[k], ex) * v->res_g[k] * 6.0f;
            if (v->thump > 0.0f) s += ck_run(&v->lp, ex) * v->thump * 2.0f;
            float env = v->t < v->len ? 1.0f : 0.0f;
            if (v->t > v->len * 3u / 4u) env = (float)(v->len - v->t) / (float)(v->len / 4u + 1u);
            s *= v->amp * (env > 0.0f ? env : 0.0f);
            /* binaural */
            v->ring[v->wp & 63u] = s;
            for (int ear = 0; ear < 2; ear++) {
                float d = v->tau[ear];
                int ni = (int)d; float fr = d - (float)ni;
                float p0 = v->ring[(v->wp - (u32)ni) & 63u], p1 = v->ring[(v->wp - (u32)ni - 1u) & 63u];
                float y = p0 + fr * (p1 - p0);
                float hh = ((y + v->hx[ear]) + v->al[ear] * K * (y - v->hx[ear])) * (1.0f / (1.0f + K)) - ((1.0f - K) / (1.0f + K)) * v->hy[ear];
                v->hx[ear] = y; v->hy[ear] = hh;
                if (ear) r += hh * v->gain[ear]; else l += hh * v->gain[ear];
            }
            v->wp++;
            v->t++;
            if (v->t >= v->len) v->on = 0;
        }
        if (dbg > 0) { e_acc += 0.5 * ((double)l * l + (double)r * r); n_acc++; float a = fmaxf(fabsf(l), fabsf(r)); if (a > peak) peak = a; }
        int L = buf[2 * n] + (int)(l * 32767.0f), R = buf[2 * n + 1] + (int)(r * 32767.0f);
        buf[2 * n] = (int16_t)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        buf[2 * n + 1] = (int16_t)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
    }
    if (dbg > 0) {
        int still = 0;
        for (int i = 0; i < CK_VOICES; i++) still |= ckv[i].on;
        if (!still && n_acc) {
            ps2_log("cracks-sound: a crack sound ended: rms %.1f dBFS peak %.1f dBFS over %.0f ms", 10.0 * log10(e_acc / n_acc + 1e-20),
                    20.0 * log10((double)peak + 1e-10), (double)n_acc * 1000.0 / CK_RATE);
            e_acc = 0.0; n_acc = 0; peak = 0.0f;
        }
    }
}
