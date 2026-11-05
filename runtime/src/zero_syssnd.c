// zero_syssnd.c - cockpit system sounds (bug #88): the cockpit "lives" -- switch clicks, landing gear, air brake, and
// the game's own warning / lock tones placed at the instrument panel.
//
// What the game already plays (dynamic trace ACZ_DEBUG_SYSSND_TRACE + static RE, 2026-10-23; NOTES has the numbers):
// - The sound manager queues requests (0x115AD0 play SE (mgr, &bank, &id), 0x115A58 play with a handle, 0x115508 play
//   3D, 0x1157B0 / 0x115880 / 0x115968 ...); the library starts them later on a channel through 0x2DFDC8 (a0 = sound
//   descriptor: bank table 0x424D10 + 12 x bank, +8 header, sounds at +0x10 + 0x50 x id). Channel ch = SPU2 voice ch.
// - Weapon switch: yes, a 2D beep: (1,6) to the special weapon, (1,7) back to missiles, (1,5) none left (player update
//   0x232184 around 0x232F70: action 2, player +0x1AA8 = selected weapon, +0x1ACB = 2 / 1 / 4 for one field).
// - Lock: yes, the HUD (a0 of 0x24EE20, HUD manager object): lock slots bytes +0xCC.. (count +0xD8; bit 0 seeking,
//   bit 1 locked): seeking beeps (1, hud+0x35E/0x35F) with handle hud+0x2CC (request ra 0x24EFC4), the locked tone
//   (1, hud+0x342 / +0x344) with handle hud+0x2D0 (ra 0x24EF20), special weapons (0x24F030: ra 0x24F0FC).
// - Warnings: yes (player update 0x230DD4 from 0x231420): an enemy missile homing on the player (0x20F780(*(player
//   +0x1A20), &dist, group, index): 16 missile slots at +0xE0 + 0x50 k, in use mask +0x5F0, target group/index
//   +0x3E/+0x3F, homing bit +0x43 & 1, position +0x00) -> MISSILE beeps (1,4), handle player+0x1A70, faster as it
//   closes (table 0x3B0AC0); else the radar-lock WARNING beeps (1,3), handle player+0x1A74 (unit +0x200 bit 8, or
//   +0x1AD8 with unit +0x1F8 & 0x10). PULL UP (player +0x1600 & 0x400000, 0x23780C): the alert (1,254/255/256 by
//   view) through 0x115AD0 (ra 0x237B30), then a repeating tone (1,245/246/247) with handle player+0x1A80 (ra 0x237C24).
// - Target change (Triangle, player +0x1AB6 counts the held frames), big map (Square held): no sound.
// - Landing gear: no state, no sound. Flaps: the aircraft model has speed-driven flap channels (player +0x804, 0 down
//   .. 50 up, with +0x828 / +0x82C: down below ~186 kt, up by ~250 kt) but no sound. Air brake (flight model +0x408,
//   the wheel brake on the ground): no sound of its own (the engine layers follow the throttle). Engine spool-up: the
//   engine layers already follow the throttle on the take-off roll. Canopy: no event.
// - Missile launch (1,98 3D), kills (1,275/276 2D), view-specific variants (HUD / cockpit / chase: N, N+1, N+2).
//
// What this adds (ACZ_SND_SYSTEMS = level, 0 off; items in ACZ_SND_SYSTEMS_ITEMS; ACZ_SND_WARN_PLACE):
// - switch: mechanical clicks of the pilot's controls where the game has an event: the weapon switch on the throttle
//   (left hand), the target button on the stick (press and release), the big map button on the panel (press and
//   release; action 0x10 = f12 of the HUD manager update), the master arm toggle once per flight when the pilot gets
//   control.
// - gear: retraction after take-off (and extension for a landing back over the take-off runway): door / uplock
//   clunks, the hydraulic pump whine, the lock thumps (nose, then the mains a few tens of ms apart), the doors; the
//   airflow rumble with the gear down; the wheels on the take-off runway (a roll rising with speed and the slab joints
//   knocking under the nose wheel, then the mains).
// - flaps: the flap motor while the game's flap channels move, a soft thud at the stops, a little more airflow with
//   them down.
// - brake: the air brake: a short actuator whine and a soft stop thump, and the airflow rumble (with the dynamic
//   pressure) while it is out.
// - ACZ_SND_SYSTEMS_MIX=2 (default; 1 = the first mix): fewer layers on the take-off roll and at lift-off (the runway
//   and the hydraulics are felt more than heard under a helmet): the wheels' roll 8 dB lower and fading under the
//   engine above ~90 kt, the flaps' airflow only in the air (6 dB lower), the gear's airflow 6 dB lower with a gentler
//   wobble, the pumps and motors 2-6 dB lower. The continuous layers duck under a loud game and the radio through
//   the immersion bus (ps2_gforce_audio.c, ACZ_IMM_DUCK; without it: 4 dB under the radio as before).
// - (ACZ_SND_WARN_PLACE) the game's own warning and lock tones are placed at the instrument panel (right of centre, below the eye)
//   through the spatial mixer (head-tracked, binaural), the missile alert leaning toward the incoming missile's bearing
//   by ACZ_SND_WARN_BEARING (a subtle cue); level, timing and meaning untouched. Needs the binaural path (VR).
// Every added sound is synthesised here (no sample files), placed at its spot in the cockpit relative to the head
// (aircraft axes x the listener's head axes) with the spatial mixer's head model (Woodworth interaural delay +
// Brown-Duda head shadow, as ps2_spatial.c / the debris layer), outside parts (airflow) muffled in the cockpit view
// as the game's outside sounds (ACZ_AUDIO_MUFFLE). Mixed after the immersion layer (ps2_audio.c ps2_audio_sys_mix).
#include "ps2_runtime.h"
#include "ps2_menv.h"
#include "ps2_hook.h"
#include "ps2_spatial.h"
#include "zero_instr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#endif

u64 ps2_kernel_vblank_count(void);
int zero_vr_is_zero(void);

#define ZS_SNDSTART 0x002DFDC8u     /* sound library: start a sound (a0 descriptor, a1 library handle) */
#define ZS_REQ2D    0x00115A58u     /* sound manager: play with a handle (mgr, &bank, &id, handle) */
#define ZS_REQSE    0x00115AD0u     /* sound manager: play a sound effect (mgr, &bank, &id) */
#define ZS_BANKS    0x00424D10u     /* sound banks: 64 x 12 bytes: +0 id (-1 none), +8 header (count u16 +8, sounds +0x10, 0x50 each) */
#define ZS_CHAN     0x00423420u     /* channel records, 0x6C bytes: +1 active, +0x26 3D flag */
#define ZS_LSN      0x00422C18u     /* sound listener: position, velocity, front, up */
#define ZS_HUDMGR   0x0024D9D8u     /* HUD manager update, a0 = player + 0x1310 (once per flight frame) */
#define ZS_UPM      5.12f           /* game units per metre */
#define ZS_RATE     48000.0f

static float zs_fw(u32 a) { u32 w = ps2_r32(a); float f; memcpy(&f, &w, 4); return f; }
static int zs_ptr(u32 a) { return a >= 0x100000u && a < 0x2000000u; }
static u64 zs_ticks(void) {
#ifdef _WIN32
    LARGE_INTEGER t; QueryPerformanceCounter(&t); return (u64)t.QuadPart;
#else
    return 0;
#endif
}
static double zs_tick_us(void) {
#ifdef _WIN32
    static double k; if (k == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); k = 1e6 / (double)f.QuadPart; } return k;
#else
    return 0.0;
#endif
}

/* ---- switches (re-read about once a second: the ENHANCEMENTS page sets them live) ---- */
enum { ZI_SWITCH = 1, ZI_GEAR = 2, ZI_BRAKE = 4, ZI_FLAPS = 8, ZI_ALL = 15 };
static float zs_level = 1.0f;          /* ACZ_SND_SYSTEMS: 0 off, 1 the designed level, up to 2 */
static int zs_items = ZI_ALL;          /* ACZ_SND_SYSTEMS_ITEMS: switch,gear,brake,flaps (or all) */
static int zs_warn_place = 1;          /* ACZ_SND_WARN_PLACE: the game's warning / lock tones from the panel */
static float zs_bearing = 0.3f;        /* ACZ_SND_WARN_BEARING: missile alert leaning toward the missile, 0..1 */
static int zs_mixv = 2;                /* ACZ_SND_SYSTEMS_MIX: 2 the take-off mix of 2026-10-23 night, 1 the first */
static void zs_read_env(void) {                 /* (also every second, zs_update: ps2_menv moves only at a mission edge) */
    const char *e = ps2_menv("ACZ_SND_SYSTEMS");
    float l = e && *e ? (float)atof(e) : 1.0f;
    l = l < 0.0f ? 0.0f : l > 2.0f ? 2.0f : l;
    ps2_menv_note("ACZ_SND_SYSTEMS", zs_level, l);
    zs_level = l;
    e = getenv("ACZ_SND_SYSTEMS_ITEMS");
    if (!e || !*e || !strcmp(e, "all")) zs_items = ZI_ALL;
    else {
        int m = 0;
        if (strstr(e, "switch")) m |= ZI_SWITCH;
        if (strstr(e, "gear")) m |= ZI_GEAR;
        if (strstr(e, "brake")) m |= ZI_BRAKE;
        if (strstr(e, "flaps")) m |= ZI_FLAPS;
        zs_items = m;
    }
    e = ps2_menv("ACZ_SND_WARN_PLACE");
    ps2_menv_note("ACZ_SND_WARN_PLACE", zs_warn_place, !(e && *e == '0'));
    zs_warn_place = !(e && *e == '0');
    e = getenv("ACZ_SND_WARN_BEARING");
    float b = e && *e ? (float)atof(e) : 0.3f;
    zs_bearing = b < 0.0f ? 0.0f : b > 1.0f ? 1.0f : b;
    e = getenv("ACZ_SND_SYSTEMS_MIX");
    zs_mixv = e && *e == '1' ? 1 : 2;
}

/* ---- cockpit anchors: metres from the pilot's eye in the aircraft's axes (x right, y up, z forward) ---- */
enum { A_NOSE, A_MAIN_L, A_MAIN_R, A_GEAR_AIR, A_BRAKE, A_BRAKE_AIR, A_THROTTLE, A_STICK, A_PANEL, A_ARM, A_RWR, A_FLAP_L, A_FLAP_R,
       ZS_NA };
static const float zs_anchor[ZS_NA][3] = {
    {  0.00f, -1.20f,  1.30f },     /* nose gear bay, under and ahead of the seat */
    { -1.40f, -1.40f, -3.50f },     /* left main gear */
    {  1.40f, -1.40f, -3.50f },     /* right main gear */
    {  0.00f, -2.00f, -0.50f },     /* gear airflow (outside, below) */
    {  0.00f,  0.00f, -3.00f },     /* air brake actuator (behind the cockpit) */
    {  0.00f,  0.60f, -4.00f },     /* air brake airflow (outside, behind / above) */
    { -0.38f, -0.42f,  0.20f },     /* throttle (left hand): weapon switch */
    {  0.02f, -0.55f,  0.38f },     /* stick (right hand): target button */
    {  0.18f, -0.38f,  0.62f },     /* panel: map / display buttons */
    { -0.22f, -0.40f,  0.55f },     /* master arm switch (left of the panel) */
    {  0.20f, -0.30f,  0.68f },     /* warning panel: where the game's warning / lock tones are placed */
    { -1.70f, -0.70f, -2.60f },     /* left flap actuator (wing root, behind) */
    {  1.70f, -0.70f, -2.60f },     /* right flap actuator */
};
static const u8 zs_outside[ZS_NA] = { 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0 };

/* ---- EE -> mixer: one published state per game frame (seqlock) and an event ring ---- */
enum { EV_NONE, EV_CLICK_WEAPON, EV_CLICK_TGT_DN, EV_CLICK_TGT_UP, EV_CLICK_MAP, EV_ARM, EV_GEAR_UP, EV_GEAR_DOWN,
       EV_GEAR_LOCKED, EV_BRAKE_MOVE, EV_BRAKE_STOP, EV_FLAP_MOVE, EV_FLAP_STOP, EV_N };
static const char *const zs_ev_name[EV_N] = { "none", "weapon-switch", "target-press", "target-release", "map",
                                               "master-arm", "gear-up", "gear-down", "gear-locked", "brake-move", "brake-stop",
                                               "flaps-move", "flaps-stop" };
typedef struct {
    u64 field;              /* game field of the update */
    float kt;               /* airspeed */
    float gear;             /* landing gear 0 up .. 1 down (the airflow follows it) */
    float brake;            /* air brake 0..1 */
    float ground;           /* 1 rolling on the runway (wheels), 0 airborne */
    float flaps;            /* flaps 0 up .. 1 down (the game's channel) */
    float mu;               /* muffling of outside parts (cockpit view x ACZ_AUDIO_MUFFLE) */
    float g_inside, g_mech, g_air;   /* view gains: clicks (first person only), mechanisms, airflow */
    float dir[ZS_NA][3];    /* per anchor: unit direction in head axes (x right, y up, z front) */
    int items, mixv; float level;
} zs_state;
static zs_state zs_pub;
static u32 zs_seq;
static struct { u32 kind; u64 field; float a; } zs_ev[32];
static u32 zs_ev_seq;
static void zs_publish(const zs_state *s) {
    u32 q = __atomic_load_n(&zs_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&zs_seq, q + 1u, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(&zs_pub, s, sizeof zs_pub);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&zs_seq, q + 2u, __ATOMIC_RELEASE);
}
static int zs_dbg;                      /* ACZ_DEBUG_SYSSND=N: state every N fields and every event */
static void zs_event(u32 kind, float a) {
    u32 n = __atomic_load_n(&zs_ev_seq, __ATOMIC_RELAXED);
    zs_ev[n % 32u].kind = kind; zs_ev[n % 32u].field = ps2_kernel_vblank_count(); zs_ev[n % 32u].a = a;
    __atomic_store_n(&zs_ev_seq, n + 1u, __ATOMIC_RELEASE);
    if (zs_dbg) ps2_log("syssnd: event %s (%.2f) field %llu", zs_ev_name[kind < EV_N ? kind : 0], a,
                        (unsigned long long)ps2_kernel_vblank_count());
}

/* ================= EE side ================= */
static int zs_trace;                    /* ACZ_DEBUG_SYSSND_TRACE=1: log the game's sound requests and starts */
static u32 zs_player;
static FILE *zs_dump;
static int zs_dump_every = 15;
static u64 zs_ee_ticks, zs_ee_n, zs_ee_max;
static u64 zs_nev[EV_N];

/* ---- the game's warning and lock tones: classified by the request's caller, then by sound (bank, id) ---- */
enum { ZC_NONE, ZC_WARN_RADAR, ZC_WARN_MISSILE, ZC_LOCK_SEEK, ZC_LOCK_ON, ZC_LOCK_SP, ZC_WARN_PULLUP, ZC_N };
static const char *const zs_cls_name[ZC_N] = { "-", "radar-warning", "missile-alert", "lock-seeking", "locked", "sp-lock", "pull-up" };
static struct { u32 key; u8 cls; } zs_cls[32];
static int zs_ncls;
static u8 zs_chan_cls[PS2_SPAT_VOICES];
static u32 zs_chan_desc[PS2_SPAT_VOICES];
static u64 zs_placed_fields[ZC_N];
static int zs_cls_of(u32 key) {
    for (int k = 0; k < zs_ncls; k++) if (zs_cls[k].key == key) return zs_cls[k].cls;
    return ZC_NONE;
}
static int zs_req2d_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 ra = ctx->r[31].uw[0], a1 = ctx->r[5].uw[0], a2 = ctx->r[6].uw[0];
    int cls = ra == 0x00231568u ? ZC_WARN_RADAR : ra == 0x0023161Cu ? ZC_WARN_MISSILE : ra == 0x0024EFC4u ? ZC_LOCK_SEEK
            : ra == 0x0024EF20u ? ZC_LOCK_ON : ra == 0x0024F0FCu ? ZC_LOCK_SP
            : ra == 0x00237B30u || ra == 0x00237C24u ? ZC_WARN_PULLUP : ZC_NONE;
    if (cls == ZC_NONE || !zs_ptr(a1) || !zs_ptr(a2)) return 0;
    u32 key = (u32)ps2_r16(a1) << 16 | ps2_r16(a2);
    if (zs_cls_of(key) != ZC_NONE || zs_ncls >= 32) return 0;
    zs_cls[zs_ncls].key = key; zs_cls[zs_ncls].cls = (u8)cls; zs_ncls++;
    ps2_log("syssnd: the game's %s tone is sound (%u,%u)", zs_cls_name[cls], key >> 16, key & 0xFFFFu);
    return 0;
}
static void zs_bank_of(u32 desc, int *bank, int *idx) {
    *bank = -1; *idx = -1;
    for (int b = 0; b < 64; b++) {
        u32 e = ZS_BANKS + 12u * (u32)b;
        if (ps2_r32(e) == 0xFFFFFFFFu) continue;
        u32 h = ps2_r32(e + 8u);
        if (!zs_ptr(h)) continue;
        u32 n = ps2_r16(h + 8u);
        if (desc >= h + 0x10u && desc < h + 0x10u + 0x50u * n && (desc - h - 0x10u) % 0x50u == 0u) {
            *bank = b; *idx = (int)((desc - h - 0x10u) / 0x50u);
            return;
        }
    }
}
static int zs_text(u32 w) { return w >= 0x100000u && w < 0x3FEF10u && !(w & 3u); }
static int zs_after_call(u32 ra) {
    if (!zs_text(ra) || ra < 0x100008u) return 0;
    u32 i = ps2_r32(ra - 8u);
    return (i >> 26) == 3u || ((i >> 26) == 0u && (i & 0x3Fu) == 9u);
}
static u32 zs_last_handle, zs_last_desc;
static int zs_last_cls;
static int zs_start_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 desc = ctx->r[4].uw[0];
    zs_last_handle = ctx->r[5].uw[0]; zs_last_desc = desc; zs_last_cls = ZC_NONE;
    int b = -1, i = -1;
    if (zs_ncls || zs_trace) zs_bank_of(desc, &b, &i);
    if (b >= 0) zs_last_cls = zs_cls_of((u32)b << 16 | (u32)i);
    if (!zs_trace) return 0;
    u32 sp = ctx->r[29].uw[0];
    char chain[200]; int n = 0, k = 0;
    chain[0] = 0;
    for (u32 o = 0; o < 0x800u && k < 9; o += 4u) {
        u32 w = ps2_r32(sp + o);
        if (zs_after_call(w)) { n += snprintf(chain + n, sizeof chain - (size_t)n, " %06X", w); k++; }
    }
    zero_instr in; int iv = zero_instr_get(&in);
    ps2_log("syssnd-trace: start field %llu bank %d idx %d desc %08X vol %d ra %06X chain%s | kt %.0f alt %.0f brake %.2f thr %.2f",
            (unsigned long long)ps2_kernel_vblank_count(), b, i, desc, (int)(s16)ctx->r[7].uw[0], ctx->r[31].uw[0], chain,
            iv ? in.speed_kt : -1.0f, iv ? in.alt_ft : -1.0f, iv ? in.airbrake : -1.0f, iv ? in.throttle : -1.0f);
    return 0;
}
static int zs_start_leave(ps2_ctx *ctx, void *user) {
    (void)user;
    u32 h = zs_last_handle;
    int ch = zs_ptr(h) && ctx->r[2].uw[0] == 0u ? (int)ps2_r32(h) : -1;
    if (ch >= 0 && ch < PS2_SPAT_VOICES) { zs_chan_cls[ch] = (u8)zs_last_cls; zs_chan_desc[ch] = zs_last_desc; }
    if (zs_trace) ps2_log("syssnd-trace:   -> v0 %08X channel %d%s%s", ctx->r[2].uw[0], ch, zs_last_cls ? " class " : "",
                          zs_last_cls ? zs_cls_name[zs_last_cls] : "");
    return 0;
}

/* trace of the sound manager's requests (first calls per entry and caller), ACZ_DEBUG_SYSSND_TRACE=1 */
static const u32 zs_mgr[] = { 0x00114F90,0x00114FF8,0x00115020,0x001150B8,0x00115198,0x001151C0,0x001152A8,0x001152E8,0x00115328,0x00115380,0x001153C0,0x001153F0,0x00115418,0x00115490,0x00115508,0x001156D0,0x001157B0,0x00115880,0x00115968,0x00115A58,0x00115AD0,0x00115B38,0x00115BB8,0x00115C78,0x00115CD8,0x00115D18,0x00115D88,0x00115DB0,0x00115E40,0x00115E90,0x00115F00,0x00115F30,0x00116000,0x001160D8,0x00116138,0x001161C8,0x00116218,0x001162B8,0x00116308,0x00116368,0x00116420,0x001164C0,0x00116578,0x001165D8,0x00116680,0x001166C0,0x00116730,0x001167D0,0x00116830,0x00116880,0x001168F0,0x00116930,0x001169A0,0x001169F8,0x00116A40,0x00116B10,0x00116C08,0x00116C30 };
#define ZS_NMGR (sizeof zs_mgr / sizeof zs_mgr[0])
static struct { u32 fn, ra, n; } zs_req[512];
static int zs_nreq;
static int zs_req_enter(ps2_ctx *ctx, void *user) {
    u32 fn = (u32)(uintptr_t)user, ra = ctx->r[31].uw[0];
    int k;
    for (k = 0; k < zs_nreq; k++) if (zs_req[k].fn == fn && zs_req[k].ra == ra) break;
    if (k == zs_nreq) { if (zs_nreq >= 512) return 0; zs_req[k].fn = fn; zs_req[k].ra = ra; zs_req[k].n = 0; zs_nreq++; }
    if (++zs_req[k].n > 12u) return 0;
    u32 a1 = ctx->r[5].uw[0], a2 = ctx->r[6].uw[0];
    int h1 = zs_ptr(a1) ? (int)ps2_r16(a1) : -1, h2 = zs_ptr(a2) ? (int)ps2_r16(a2) : -1;
    ps2_log("syssnd-trace: req field %llu fn %06X ra %06X a1 %08X(%d) a2 %08X(%d) a3 %08X", (unsigned long long)ps2_kernel_vblank_count(),
            fn, ra, a1, h1, a2, h2, ctx->r[7].uw[0]);
    return 0;
}

/* ---- per-field state log (ACZ_DEBUG_SYSSND_STATE=1): the player's weapon / target / view bytes when they change ---- */
static int zs_state_on;
static void zs_state_log(u32 p, u64 f) {
    static u8 last[0xA0 + 16]; static int have;
    u8 cur[0xA0 + 16];
    u32 ts = p + 0x1880u;
    for (u32 k = 0; k < 0xA0u; k++) cur[k] = ps2_r8(ts + k);
    static const u32 po[16] = { 0x1AA4, 0x1AA5, 0x1AA6, 0x1AA8, 0x1AA9, 0x1AAA, 0x1AAB, 0x1ACB, 0x1AB3, 0x1AB6, 0x1A68, 0x1A69,
                                0x1A6A, 0x1A6B, 0x1AC8, 0x1AC9 };
    for (int k = 0; k < 16; k++) cur[0xA0 + k] = ps2_r8(p + po[k]);
    if (have && !memcmp(cur, last, sizeof cur)) return;
    char ln[1400]; int n = snprintf(ln, sizeof ln, "syssnd-state: field %llu", (unsigned long long)f);
    for (u32 k = 0; k < sizeof cur && n < (int)sizeof ln - 24; k++)
        if (!have || cur[k] != last[k]) {
            if (k < 0xA0u) n += snprintf(ln + n, sizeof ln - (size_t)n, " TS+%02X:%02X>%02X", k, last[k], cur[k]);
            else n += snprintf(ln + n, sizeof ln - (size_t)n, " P+%04X:%02X>%02X", po[k - 0xA0u], last[k], cur[k]);
        }
    if (have) ps2_log("%s", ln);
    memcpy(last, cur, sizeof cur); have = 1;
}

/* ---- test autopilot (ACZ_DEBUG_SYSSND_CHASE=from_field): pursue the current target with the left stick ---- */
static struct { int on; u64 from; u8 lx, ly, act; } zs_ch;
static int zs_chase_steer(u8 *lx, u8 *ly) { if (!zs_ch.act) return 0; *lx = zs_ch.lx; *ly = zs_ch.ly; return 1; }
static u32 zs_unit(u32 team, u8 g, u8 i) {
    if (!zs_ptr(team) || g == 0xFFu) return 0u;
    u32 slots = ps2_r32(team); if (!zs_ptr(slots)) return 0u;
    u32 grp = ps2_r32(slots + 4u * g); if (!zs_ptr(grp)) return 0u;
    u32 u = ps2_r32(grp + 0x104u) + (u32)i * 0x4A0u;
    return zs_ptr(u) ? u : 0u;
}
static void zs_axes(const zero_instr *in, float rt[3], float up[3], float fw[3]) {
    /* the flight model's attitude, the game's euler convention R = Ry(-heading) Rx(pitch) Rz(roll) (as zero_audio3d) */
    float y = -in->heading_deg * 0.01745329f, p = in->pitch_deg * 0.01745329f, r = in->roll_deg * 0.01745329f;
    float cy = cosf(y), sy = sinf(y), cp = cosf(p), sp = sinf(p), cr = cosf(r), sr = sinf(r);
    rt[0] = cy * cr + sy * sp * sr; rt[1] = cp * sr; rt[2] = -sy * cr + cy * sp * sr;
    up[0] = -sr * cy + cr * sp * sy; up[1] = cp * cr; up[2] = sr * sy + cr * sp * cy;
    fw[0] = -sy * cp; fw[1] = sp; fw[2] = -cy * cp;
}
static void zs_chase(u32 p, u64 f) {
    zs_ch.act = 0;
    if (!zs_ch.on || f < zs_ch.from) return;
    u32 ts = p + 0x1880u;
    u32 u = zs_unit(ps2_r32(ts + 0x84u), ps2_r8(ts + 0x98u), ps2_r8(ts + 0x99u));
    u32 me = zs_unit(ps2_r32(ts + 0x84u), ps2_r8(ts + 0x96u), ps2_r8(ts + 0x97u));
    if (!u || !me) return;
    float d[3];
    for (u32 k = 0; k < 3; k++) d[k] = zs_fw(u + 0x260u + 4u * k) - zs_fw(me + 0x260u + 4u * k);
    zero_instr in; if (!zero_instr_get(&in) || !in.valid) return;
    float rt[3], up[3], fw[3];
    zs_axes(&in, rt, up, fw);
    float bx = d[0] * rt[0] + d[1] * rt[1] + d[2] * rt[2], by = d[0] * up[0] + d[1] * up[1] + d[2] * up[2];
    float bz = d[0] * fw[0] + d[1] * fw[1] + d[2] * fw[2];
    float off = atan2f(sqrtf(bx * bx + by * by), bz);
    float rollc = atan2f(bx, by > -1e-3f ? by : -by);
    float sx = rollc * 1.2f, sy2 = by > 0.0f ? off * 2.0f : 0.15f;
    if (off < 0.05f) { sx = 0.0f; sy2 = 0.0f; }
    if (in.alt_ft < 1500.0f) { sy2 = 0.8f; sx = -in.roll_deg * 0.02f; }
    sx = sx > 1.0f ? 1.0f : sx < -1.0f ? -1.0f : sx; sy2 = sy2 > 1.0f ? 1.0f : sy2 < -1.0f ? -1.0f : sy2;
    int ix = fabsf(sx) < 0.04f ? 0 : (int)copysignf(60.0f + 60.0f * fabsf(sx), sx);
    int iy = fabsf(sy2) < 0.04f ? 0 : (int)copysignf(60.0f + 60.0f * fabsf(sy2), sy2);
    zs_ch.lx = (u8)(128 + ix); zs_ch.ly = (u8)(128 + iy); zs_ch.act = 1;
    if (f % 60u == 0u)
        ps2_log("syssnd-chase: field %llu target %u/%u (own %u/%u) dist %.0f m off %.1f deg -> LX %u LY %u", (unsigned long long)f,
                ps2_r8(ts + 0x98u), ps2_r8(ts + 0x99u), ps2_r8(ts + 0x96u), ps2_r8(ts + 0x97u),
                sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / ZS_UPM, off * 57.3f, zs_ch.lx, zs_ch.ly);
}

/* ---- the threat: the nearest enemy missile homing on the player (the game's own test, 0x20F780) ---- */
static int zs_threat_any;              /* ACZ_DEBUG_SYSSND_THREAT=1 (test): any homing missile counts (the player's own) */
static int zs_threat(u32 p, const float *eye, float out[3], float *dist_m) {
    u32 mm = ps2_r32(p + 0x1A20u);
    if (!zs_ptr(mm)) return 0;
    u8 g = ps2_r8(p + 0x1AB1u), ix = ps2_r8(p + 0x1AB2u);
    u32 mask = ps2_r32(mm + 0x5F0u);
    float best = 1e30f; int found = 0;
    for (u32 k = 0; k < 16u; k++) {
        if (!(mask >> k & 1u)) continue;
        u32 s = mm + 0xE0u + 0x50u * k;
        if (!zs_threat_any && (ps2_r8(s + 0x3Eu) != g || ps2_r8(s + 0x3Fu) != ix)) continue;
        if (!(ps2_r8(s + 0x43u) & 1u)) continue;
        float q[3] = { zs_fw(s), zs_fw(s + 4u), zs_fw(s + 8u) }, d[3];
        if (!(fabsf(q[0]) < 1e7f && fabsf(q[1]) < 1e7f && fabsf(q[2]) < 1e7f)) continue;
        for (int j = 0; j < 3; j++) d[j] = q[j] - eye[j];
        float m = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
        if (m < best) { best = m; memcpy(out, d, sizeof d); found = 1; }
    }
    if (found) *dist_m = sqrtf(best) / ZS_UPM;
    return found;
}

/* ---- per-field update (HUD manager hook: the player's flight frame) ---- */
static struct {
    u32 p; u64 last_f, arm_f, arm_done_f;
    int arm_pending; u8 acb, ab6, map; float brake, brake_v, brake_rest; int brake_still;
    float gear, gear_t; int gear_moving, gear_init; u64 gear_f0;
    float flaps, fch; int flap_dir, flap_moving, flap_still;
    float lp[3]; int have_lp; float rwy_m; int rwy; float rwy_pos[3]; float ground;
    float dir_head[ZS_NA][3];
    int fp;
    float threat[3]; float threat_m; int threat_on;
    u64 env_f;
} zs;
static float zs_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void zs_norm(float *a) { float n = sqrtf(zs_dot(a, a)); if (n > 1e-6f) { a[0] /= n; a[1] /= n; a[2] /= n; } else { a[0] = a[1] = 0.0f; a[2] = 1.0f; } }

/* ACZ_SND_SYSTEMS_TEST="field:event[,field:event...]" (test): fire events at fields (weapon, tdn, tup, map, arm, gearup,
   geardown, brake=0.8@f2 [brake held to f2]) -- listening clips of every sound in any mission */
static struct { u64 f, f2; u32 kind; float a; } zs_tst[32];
static int zs_ntst;
static float zs_test_brake(u64 f) {
    for (int k = 0; k < zs_ntst; k++) if (zs_tst[k].kind == EV_BRAKE_MOVE && f >= zs_tst[k].f && f < zs_tst[k].f2) return zs_tst[k].a;
    return -1.0f;
}

static float zs_mapv;                  /* the big map's button (action 0x10) as the HUD manager gets it (f12) */
static void zs_update(u32 p, u64 f) {
    u64 t0 = zs_ticks();
    if (f - zs.env_f >= 60u || !zs.env_f) { zs_read_env(); zs.env_f = f; }
    zero_instr in;
    if (!zero_instr_get(&in) || !in.valid) return;
    int newflight = p != zs.p || !zs.last_f || f - zs.last_f > 600u;
    u32 fm = p + 0xEF0u;
    float pos[3] = { zs_fw(fm + 0x3B0u), zs_fw(fm + 0x3B4u), zs_fw(fm + 0x3B8u) };
    if (zs.have_lp && f - zs.last_f <= 8u) {
        float dx = pos[0] - zs.lp[0], dy = pos[1] - zs.lp[1], dz = pos[2] - zs.lp[2];
        if (dx * dx + dy * dy + dz * dz > (300.0f * ZS_UPM) * (300.0f * ZS_UPM)) newflight = 1;   /* RETRY, stage jump */
    }
    memcpy(zs.lp, pos, sizeof zs.lp); zs.have_lp = 1;
    int hold = (ps2_r32(p + 0x1A68u) & 0x2000u) != 0u;           /* a mission event holds the camera */
    u8 view = ps2_r8(p + 0x1AA4u);                                /* 0 HUD, 2 cockpit, 1 chase */
    if (newflight) {
        /* a cut in the same mission right after a take-off (the aircraft put into the flight scene) keeps the gear
           down so it still retracts */
        int keep_gear = p == zs.p && zs.gear > 0.5f && zs.rwy && in.speed_kt > 100.0f && f - zs.last_f <= 8u;
        zs.p = p; zs.arm_pending = 1; zs.arm_f = 0; zs.acb = ps2_r8(p + 0x1ACBu); zs.ab6 = ps2_r8(p + 0x1AB6u);
        zs.brake = in.airbrake; zs.brake_v = 0.0f; zs.gear_moving = 0;
        zs.gear = zs.gear_t = 0.0f; zs.rwy = 0; zs.gear_init = 1;     /* decided at the first frame with control */
        if (keep_gear) { zs.gear = 1.0f; zs.gear_t = 0.0f; zs.gear_init = 0; zs.arm_pending = 0; }  /* raise it now */
        if (zs_dbg) ps2_log("syssnd: new flight at field %llu (player %08X, %s, gear %s)", (unsigned long long)f, p,
                            hold ? "event" : "control", zs.gear > 0.5f ? "down" : "up");
    }
    /* master arm: once per flight, 1.2 s after the pilot gets control */
    if (zs.arm_pending && !hold) {
        if (!zs.arm_f) zs.arm_f = f + 72u;
        else if (f >= zs.arm_f) {
            zs.arm_pending = 0;
            if (f - zs.arm_done_f > 3600u || !zs.arm_done_f) { zs_event(EV_ARM, 1.0f); zs.arm_done_f = f; zs_nev[EV_ARM]++; }
        }
    }
    /* weapon switch: player +0x1ACB pulses 1 / 2 / 4 for one frame on the button (the game's beep plays with it) */
    u8 acb = ps2_r8(p + 0x1ACBu);
    if (acb && !zs.acb && !hold) { zs_event(EV_CLICK_WEAPON, acb == 4u ? 0.7f : 1.0f); zs_nev[EV_CLICK_WEAPON]++; }
    zs.acb = acb;
    /* target button: player +0x1AB6 counts the frames it is held (press: 0 -> 1, release: back to 0) */
    u8 ab6 = ps2_r8(p + 0x1AB6u);
    if (ab6 && !zs.ab6 && !hold) { zs_event(EV_CLICK_TGT_DN, 1.0f); zs_nev[EV_CLICK_TGT_DN]++; }
    if (!ab6 && zs.ab6 && !hold) { zs_event(EV_CLICK_TGT_UP, 1.0f); zs_nev[EV_CLICK_TGT_UP]++; }
    zs.ab6 = ab6;
    /* big map button: action 0x10 (Square held / View) reaches the HUD manager update 0x24D9D8 as f12 (0x233984 and
       0x2344D8: 0x103A10 digital or 0x103A60 analog); the game plays nothing for it: a panel push button, press and
       release */
    {   float mv = zs_mapv;
        if (!(mv >= 0.0f && mv <= 1.5f)) mv = 0.0f;
        if (mv > 0.5f && !zs.map && !hold) { zs_event(EV_CLICK_MAP, 1.0f); zs_nev[EV_CLICK_MAP]++; }
        if (mv < 0.3f && zs.map && !hold) zs_event(EV_CLICK_MAP, 0.5f);
        zs.map = mv > 0.5f ? 1u : mv < 0.3f ? 0u : zs.map; }
    /* air brake: the flight model's +0x408 (0..1, from L1: proportional with an analog trigger). A stroke starts when
       it leaves its resting value by 0.08 (the actuator runs), ends after 3 still frames; a full stroke (>= 0.3) ending
       fully out or in thumps. On the ground the same value is the wheel brake (1 while parked): no air brake there. */
    float br = in.airbrake;
    {   float tb = zs_test_brake(f); if (tb >= 0.0f) br = tb; }
    if (!(br >= 0.0f && br <= 1.0f)) br = 0.0f;
    if (newflight || zs.ground > 0.0f) { zs.brake_rest = br; zs.brake_v = 0.0f; zs.brake_still = 0; }
    else if (!zs.brake_v) {
        if (fabsf(br - zs.brake_rest) > 0.08f && !hold) {
            zs.brake_v = br > zs.brake_rest ? 1.0f : -1.0f; zs.brake_still = 0;
            zs_event(EV_BRAKE_MOVE, zs.brake_v); zs_nev[EV_BRAKE_MOVE]++;
        }
    } else {
        zs.brake_still = fabsf(br - zs.brake) <= 0.004f ? zs.brake_still + 1 : 0;
        if (zs.brake_still >= 3) {
            float stroke = fabsf(br - zs.brake_rest);
            float a = stroke >= 0.3f && br > 0.95f ? 1.0f : stroke >= 0.3f && br < 0.05f ? -1.0f : 0.0f;
            zs_event(EV_BRAKE_STOP, a); zs_nev[EV_BRAKE_STOP]++;
            zs.brake_rest = br; zs.brake_v = 0.0f;
        }
    }
    zs.brake = br;
    /* test events */
    for (int k = 0; k < zs_ntst; k++)
        if (zs_tst[k].f == f && zs_tst[k].kind != EV_BRAKE_MOVE) {
            if (zs_tst[k].kind == EV_GEAR_UP || zs_tst[k].kind == EV_GEAR_DOWN) {
                zs.gear_t = zs_tst[k].kind == EV_GEAR_DOWN ? 1.0f : 0.0f;
            } else zs_event(zs_tst[k].kind, zs_tst[k].a);
        }
    /* landing gear: the game has no gear state or sound (the trace, and player / unit record diffs over a real take-off
       show none; the speed-driven channel below is the flaps). Ours follows the flight: a flight whose pilot gets
       control on the ground (FM+0x236 bit 0; M2's take-off) has the gear down on its runway (height, place); once
       climbing through 15 m above it at > 150 kt the pilot raises the gear (4 s); back over that runway (within 6 km,
       below 120 m above it, slower than 170 kt, descending) it comes down again (3.5 s). Air starts have the gear up and
       never extend it. With the gear down on the ground the wheels roll (zs.ground). */
    {
        float alt_m = in.alt_ft / 3.2808f;
        int on_ground = (ps2_r8(fm + 0x236u) & 1u) != 0u;     /* the flight model's ground contact (FM+0x236 bit 0) */
        if (zs.gear_init && !hold) {                 /* the first frame with control decides: on the ground = a runway */
            zs.gear_init = 0;
            zs.rwy = on_ground; zs.rwy_m = alt_m; memcpy(zs.rwy_pos, pos, sizeof pos);
            zs.gear = zs.gear_t = zs.rwy ? 1.0f : 0.0f; zs.gear_moving = 0;
        }
        if (zs.rwy && on_ground) { zs.rwy_m = alt_m; memcpy(zs.rwy_pos, pos, sizeof pos); }
        float dx = (pos[0] - zs.rwy_pos[0]) / ZS_UPM, dz = (pos[2] - zs.rwy_pos[2]) / ZS_UPM;
        int near_rwy = zs.rwy && dx * dx + dz * dz < 6000.0f * 6000.0f;
        if (!zs.gear_init) {
            if (zs.gear_t > 0.5f && in.speed_kt > 150.0f && alt_m > zs.rwy_m + 15.0f && in.vsi_fpm > 200.0f) zs.gear_t = 0.0f;
            if (zs.gear_t < 0.5f && near_rwy && in.speed_kt < 170.0f && in.speed_kt > 40.0f && alt_m < zs.rwy_m + 120.0f
                && in.vsi_fpm < 0.0f) zs.gear_t = 1.0f;
        }
        zs.ground = on_ground && zs.gear > 0.9f && !hold ? 1.0f : 0.0f;          /* the wheels roll */
        if (zs.gear_t != (zs.gear > 0.5f ? 1.0f : 0.0f) && !zs.gear_moving) {
            zs.gear_moving = 1; zs.gear_f0 = f;
            zs_event(zs.gear_t > 0.5f ? EV_GEAR_DOWN : EV_GEAR_UP, 1.0f);
            zs_nev[zs.gear_t > 0.5f ? EV_GEAR_DOWN : EV_GEAR_UP]++;
        }
        if (zs.gear_moving) {
            /* a fighter's cycle: up ~4 s, down ~3.5 s (gravity helps); the airflow follows the doors and the legs */
            float dur = zs.gear_t > 0.5f ? 3.5f : 4.0f, u = (float)(f - zs.gear_f0) / 60.0f / dur;
            if (u >= 1.0f) { zs.gear = zs.gear_t; zs.gear_moving = 0; zs_event(EV_GEAR_LOCKED, zs.gear_t); zs_nev[EV_GEAR_LOCKED]++; }
            else zs.gear = zs.gear_t > 0.5f ? u : 1.0f - u;
        }
    }
    /* flaps: the aircraft model's own flap channels (player +0x804 0 down .. 50 up, with +0x828 / +0x82C; the model's
       inboard trailing-edge surfaces move with them: ACZ_DEBUG_SYSSND_FLAPFORCE image diff on the F-5E, the same channel
       on the Su-27) follow the airspeed: fully down below ~186 kt, up at ~250 kt (M2 take-off: 188 -> 250 kt in 2.8 s
       after lift-off; in flight they come down again when slowing through that band). The motor runs while the
       channel moves; at an end it stops with a soft thud; halted half-way (the speed held in the band) the motor just
       stops. The airflow under the wings grows a little with them down (zs_state.flaps). */
    {
        float ch = zs_fw(p + 0x804u);
        if (ch >= -1.0f && ch <= 101.0f) {
            float fl = 1.0f - (ch < 0.0f ? 0.0f : ch > 50.0f ? 1.0f : ch / 50.0f);
            if (newflight || hold) { zs.fch = ch; zs.flap_dir = 0; zs.flap_moving = 0; zs.flap_still = 0; }   /* (set in the intro) */
            else {
                float d = ch - zs.fch;
                int dir = d > 0.005f ? -1 : d < -0.005f ? 1 : 0;              /* -1 retracting, +1 extending */
                int at_end = ch <= 1.0f || ch >= 49.0f;
                if (dir && !zs.flap_moving && !at_end) {
                    zs.flap_dir = dir; zs.flap_moving = 1; zs.flap_still = 0;
                    zs_event(EV_FLAP_MOVE, (float)dir); zs_nev[EV_FLAP_MOVE]++;
                } else if (dir && zs.flap_moving) zs.flap_dir = dir;
                if (zs.flap_moving) {
                    if ((zs.flap_dir < 0 && ch >= 49.99f) || (zs.flap_dir > 0 && ch <= 0.01f)) {
                        zs_event(EV_FLAP_STOP, 1.0f); zs.flap_moving = 0; zs.flap_dir = 0;     /* at the stop: a thud */
                    } else if (!dir && ++zs.flap_still >= 20) {
                        zs_event(EV_FLAP_STOP, 0.0f); zs.flap_moving = 0; zs.flap_dir = 0;     /* halted: no thud */
                    } else if (dir) zs.flap_still = 0;
                }
            }
            zs.fch = ch; zs.flaps = fl;
        }
    }
    /* geometry: anchors in head axes. Listener (head-turned camera) front / up from the sound listener; the aircraft
       axes from the flight model's attitude */
    {
        float F[3], U[3], R[3], E[3];
        for (int j = 0; j < 3; j++) { E[j] = zs_fw(ZS_LSN + 4u * (u32)j); F[j] = zs_fw(ZS_LSN + 0x18u + 4u * (u32)j); U[j] = zs_fw(ZS_LSN + 0x24u + 4u * (u32)j); }
        R[0] = F[1] * U[2] - F[2] * U[1]; R[1] = F[2] * U[0] - F[0] * U[2]; R[2] = F[0] * U[1] - F[1] * U[0];
        zs_norm(R); zs_norm(F); zs_norm(U);
        float rt[3], up[3], fw[3];
        zs_axes(&in, rt, up, fw);
        int chase = view == 1u;
        for (int a = 0; a < ZS_NA; a++) {
            float w[3];
            for (int j = 0; j < 3; j++) w[j] = zs_anchor[a][0] * rt[j] + zs_anchor[a][1] * up[j] + zs_anchor[a][2] * fw[j];
            if (chase) for (int j = 0; j < 3; j++) w[j] = pos[j] + w[j] * ZS_UPM - E[j];   /* from the chase camera */
            float h[3] = { zs_dot(w, R), zs_dot(w, U), zs_dot(w, F) };
            zs_norm(h);
            memcpy(zs.dir_head[a], h, sizeof h);
        }
        zs.fp = !chase;
        /* the incoming missile (for the alert's bearing cue) */
        float d[3], dm = 0.0f;
        zs.threat_on = zs_threat(p, E, d, &dm);
        if (zs.threat_on) { float h[3] = { zs_dot(d, R), zs_dot(d, U), zs_dot(d, F) }; zs_norm(h); memcpy(zs.threat, h, sizeof h); zs.threat_m = dm; }
    }
    /* publish */
    static zs_state s;
    memset(&s, 0, sizeof s);
    s.field = hold ? 0u : f;                                   /* held by an event: the mixer fades as in a pause */
    s.kt = in.speed_kt; s.gear = zs.gear; s.brake = zs.brake; s.ground = zs.ground; s.flaps = zs.flaps;
    {   extern volatile int ps2_vk_cockpit_view;
        extern float ps2_spatial_muffle(void);
        float mu = ps2_vk_cockpit_view ? ps2_spatial_muffle() : 0.0f;
        s.mu = mu < 0.0f ? 0.0f : mu > 1.0f ? 1.0f : mu; }
    s.g_inside = zs.fp ? 1.0f : 0.0f;
    s.g_mech = zs.fp ? 1.0f : 0.3f;
    s.g_air = zs.fp ? 1.0f : 0.6f;
    memcpy(s.dir, zs.dir_head, sizeof s.dir);
    s.items = zs_items; s.level = zs_level; s.mixv = zs_mixv;
    zs_publish(&s);
    zs.last_f = f;
    u64 dt = zs_ticks() - t0;
    zs_ee_ticks += dt; zs_ee_n++; if (dt > zs_ee_max) zs_ee_max = dt;
    if (zs_dbg > 0 && f % 1800u == 0u) {             /* CPU every 30 s (a timed-out test run never reaches the exit report) */
        void zero_syssnd_report(void);
        zero_syssnd_report();
    }
    if (zs_dbg > 0 && f % (u64)zs_dbg == 0u)
        ps2_log("syssnd: field %llu view %u%s kt %.0f alt %.0f ft gear %.2f%s brake %.2f mu %.2f level %.2f items %X threat %s%.0f m | "
                "rwr dir %.2f %.2f %.2f, throttle dir %.2f %.2f %.2f", (unsigned long long)f, view, hold ? " (event)" : "",
                in.speed_kt, in.alt_ft, zs.gear, zs.gear_moving ? " moving" : "", zs.brake, s.mu, zs_level, zs_items,
                zs.threat_on ? "" : "none ", zs.threat_on ? zs.threat_m : 0.0f, zs.dir_head[A_RWR][0], zs.dir_head[A_RWR][1],
                zs.dir_head[A_RWR][2], zs.dir_head[A_THROTTLE][0], zs.dir_head[A_THROTTLE][1], zs.dir_head[A_THROTTLE][2]);
}

static int zs_hud_enter(ps2_ctx *ctx, void *user) {
    (void)user;
    static u64 last;
    u64 f = ps2_kernel_vblank_count();
    if (f == last) return 0;
    last = f;
    u32 p = ctx->r[4].uw[0] - 0x1310u;
    if (!zs_ptr(p)) return 0;
    zs_player = p;
    zs_mapv = ctx->f[12].f;
    {   /* ACZ_DEBUG_SYSSND_FLAPFORCE=f0,f1 (test): hold the model's flap channels at their low-speed values (0, 50, 100 at
           player +0x804 / +0x828 / +0x82C) between those fields: what the aircraft model shows with them */
        static int gf = -1; static unsigned long long g0, g1;
        if (gf < 0) { const char *e = getenv("ACZ_DEBUG_SYSSND_FLAPFORCE"); gf = e && sscanf(e, "%llu,%llu", &g0, &g1) == 2; }
        if (gf && f >= g0 && f < g1) {
            const float v[3] = { 0.0f, 50.0f, 100.0f }; const u32 o[3] = { 0x804u, 0x828u, 0x82Cu };
            for (int k = 0; k < 3; k++) { u32 w; memcpy(&w, &v[k], 4); ps2_w32(p + o[k], w); }
        }
    }
    if (zs_state_on) zs_state_log(p, f);
    zs_chase(p, f);
    zs_update(p, f);
    if (zs_dump && f % (u64)zs_dump_every == 0u) {
        u32 t = ps2_r32(p + 0x1A5Cu), u = 0u;
        if (zs_ptr(t)) {
            u32 g = ps2_r32(t);
            u32 e = zs_ptr(g) ? ps2_r32(g + 4u * ps2_r8(p + 0x1AB1u)) : 0u;
            u = zs_ptr(e) ? ps2_r32(e + 0x104u) + (u32)ps2_r8(p + 0x1AB2u) * 0x4A0u : 0u;
        }
        u32 hdr[4] = { 0x5A53594Eu, (u32)f, p, u };
        fwrite(hdr, 4, 4, zs_dump);
        u8 buf[0x2000 + 0x4A0];
        for (u32 k = 0; k < 0x2000u; k++) buf[k] = ps2_r8(p + k);
        for (u32 k = 0; k < 0x4A0u; k++) buf[0x2000u + k] = zs_ptr(u) ? ps2_r8(u + k) : 0u;
        fwrite(buf, 1, sizeof buf, zs_dump);
        fflush(zs_dump);
    }
    return 0;
}

/* ---- the game's warning / lock tones at the panel: called by zero_audio3d.c before it publishes its frame ---- */
static void zs_slerp(const float *a, const float *b, float t, float *o) {
    float c = zs_dot(a, b); c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
    float th = acosf(c);
    if (th < 1e-3f) { memcpy(o, a, 3 * sizeof *o); return; }
    if (th > 3.1f) {                                       /* straight behind: lean over the top */
        float m[3] = { 0.0f, 1.0f, 0.0f };
        zs_slerp(a, m, t * 2.0f > 1.0f ? 1.0f : t * 2.0f, o);
        if (t > 0.5f) { float o2[3]; memcpy(o2, o, sizeof o2); zs_slerp(o2, b, (t - 0.5f) * 2.0f, o); }
        return;
    }
    float s = sinf(th), wa = sinf((1.0f - t) * th) / s, wb = sinf(t * th) / s;
    for (int j = 0; j < 3; j++) o[j] = wa * a[j] + wb * b[j];
    zs_norm(o);
}
void zero_syssnd_place(ps2_spat_frame *fr) {
    if (!zs.p || !zs_warn_place || !zs.fp) return;
    u64 f = ps2_kernel_vblank_count();
    if (f - zs.last_f > 8u) return;
    for (u32 ch = 0; ch < PS2_SPAT_VOICES; ch++) {
        int cls = zs_chan_cls[ch];
        if (!cls) continue;
        u32 c = ZS_CHAN + 0x6Cu * ch;
        if (!ps2_r8(c + 1u) || ps2_r8(c + 0x26u) || fr->v[ch].is3d) { if (!ps2_r8(c + 1u)) zs_chan_cls[ch] = ZC_NONE; continue; }
        ps2_spat_src *s = &fr->v[ch];
        float d[3]; memcpy(d, zs.dir_head[A_RWR], sizeof d);
        if (cls == ZC_WARN_MISSILE && zs.threat_on && zs_bearing > 0.0f) zs_slerp(zs.dir_head[A_RWR], zs.threat, zs_bearing, d);
        memcpy(s->dir, d, sizeof d);
        s->dist_m = 0.7f; s->dopp = 1.0f; s->level = 0.0f; s->is3d = 1; s->own = 1;
        zs_placed_fields[cls]++;
    }
}

/* ================= mixer side (SDL audio thread) ================= */
#define ZS_RING 64u
#define ZS_HS_K (2.0f * ZS_RATE / (2.0f * 343.0f / 0.0875f))      /* head-shadow filter constant (12.24) */
#define ZS_ITD  (0.0875f / 343.0f * ZS_RATE)                       /* head radius / c in samples (12.24) */
typedef struct {
    float ring[ZS_RING]; u32 wp;
    float tau[2], al[2], g[2], dtau[2], dal[2], dg[2];
    float hx[2], hy[2], mlp[2];
    float acc; int live;        /* this sample's input; samples left until the delay line and filters are quiet */
    int fresh;
} zs_bin;
static float zs_alpha(float c) {            /* as ps2_spatial.c sp_alpha: c = cos(angle from the ear's axis) */
    float th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
    float a = (1.05f + 0.95f * cosf(th * (180.0f / 150.0f))) * (1.0f / 0.7564f);
    return a < 0.08f ? 0.08f : a > 2.0f ? 2.0f : a;
}
static float zs_itd(float c) {              /* as sp_itd: Woodworth, samples */
    float th = acosf(c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c);
    return 2.0f + ZS_ITD * (th < 1.5707963f ? 1.0f - c : 1.0f + th - 1.5707963f);
}
static void zs_bin_block(zs_bin *b, const float *dir, int hrtf) {
    float x = dir[0], tau[2], al[2], g[2];
    if (hrtf) { tau[0] = zs_itd(-x); tau[1] = zs_itd(x); al[0] = zs_alpha(-x); al[1] = zs_alpha(x); g[0] = g[1] = 1.0f; }
    else {                                   /* constant-power pan on the azimuth, centre = 1 */
        float p = 0.5f + 0.5f * (x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x);
        tau[0] = tau[1] = 2.0f; al[0] = al[1] = 1.0f;
        g[0] = cosf(p * 1.5707963f) * 1.4142136f; g[1] = sinf(p * 1.5707963f) * 1.4142136f;
    }
    for (int e = 0; e < 2; e++) {
        if (b->fresh) { b->tau[e] = tau[e]; b->al[e] = al[e]; b->g[e] = g[e]; b->dtau[e] = b->dal[e] = b->dg[e] = 0.0f; }
        else { b->dtau[e] = (tau[e] - b->tau[e]) / 32.0f; b->dal[e] = (al[e] - b->al[e]) / 32.0f; b->dg[e] = (g[e] - b->g[e]) / 32.0f; }
    }
    b->fresh = 0;
}
static void zs_bin_run(zs_bin *b, float s, float *l, float *r) {
    b->ring[b->wp & (ZS_RING - 1u)] = s;
    for (int e = 0; e < 2; e++) {
        float d = b->tau[e];
        int n = (int)d; float fr = d - (float)n;
        float p0 = b->ring[(b->wp - (u32)n) & (ZS_RING - 1u)], p1 = b->ring[(b->wp - (u32)n - 1u) & (ZS_RING - 1u)];
        float y = p0 + fr * (p1 - p0);
        float h = ((y + b->hx[e]) + b->al[e] * ZS_HS_K * (y - b->hx[e])) * (1.0f / (1.0f + ZS_HS_K))
                  - ((1.0f - ZS_HS_K) / (1.0f + ZS_HS_K)) * b->hy[e];
        b->hx[e] = y; b->hy[e] = h;
        float o = h * b->g[e];
        if (e == 0) *l += o; else *r += o;
        b->tau[e] += b->dtau[e]; b->al[e] += b->dal[e]; b->g[e] += b->dg[e];
    }
    b->wp++;
}

/* voices */
typedef struct { int on, anchor; float t, amp, c[5], r2[5], y1[5], y2[5], exc[5], nz; u32 rng; int second; float t2, amp2; } zs_click;
typedef struct { int on, anchor; float t, amp, c[4], r2[4], y1[4], y2[4], nz; u32 rng; } zs_thump;
typedef struct { float env, target, ph[1], f0, f, lp1, lp2, hp; u32 rng; float t, rip; } zs_whine;
typedef struct { float g, lp1, lp2, amlp, bp1; u32 rng; } zs_air;
static struct {
    int init; u32 rd;
    zs_bin bin[ZS_NA];
    zs_click click[4];
    zs_thump thump[12];
    float joint_ph; float joint_due[4]; int joint_n; float roll_lp1, roll_lp2, roll_g; u32 roll_rng;
    zs_whine gwh, bwh, fwh;          /* gear pump, air brake actuator, flap motor */
    zs_air gair, bair;
    struct { float t; int kind; } gseq;  /* gear sequence: seconds since the selector, 1 up / 2 down / 0 idle */
    u64 samples, ticks, max_ticks, chunks, active;
    float duck;                      /* under radio speech (continuous layers) */
    float bed, am_k;                 /* the continuous layers' gain (the immersion bus, else duck); airflow wobble depth */
    FILE *wav; u32 wav_bytes;
} zm;
static u32 zm_rand(u32 *r) { *r ^= *r << 13; *r ^= *r >> 17; *r ^= *r << 5; return *r; }
static float zm_frand(u32 *r) { return (float)(zm_rand(r) >> 8) * (1.0f / 16777216.0f); }
static float zm_noise(u32 *r) { return zm_frand(r) * 2.0f - 1.0f; }

/* a click: an impulse into up to four resonant modes (switch body, plastic, spring), plus an optional knock */
static void zm_click_start(int anchor, const float *f, const float *tau_ms, const float *a, float amp, float th_f, float th_tau_ms,
                           float th_amp, float second_ms, float second_amp) {
    int k = 0;
    for (int i = 0; i < 4; i++) { if (!zm.click[i].on) { k = i; break; } if (zm.click[i].t > zm.click[k].t) k = i; }
    zs_click *c = &zm.click[k];
    memset(c, 0, sizeof *c);
    c->on = 1; c->anchor = anchor; c->amp = amp; c->rng = 0x9E3779B9u ^ (u32)(zm.samples * 2654435761u); if (!c->rng) c->rng = 1u;
    const float dt = 1.0f / ZS_RATE;
    for (int m = 0; m < 4; m++) {
        if (f[m] <= 0.0f) continue;
        float rr = expf(-dt / (tau_ms[m] * 0.001f));
        c->c[m] = 2.0f * rr * cosf(6.2831853f * f[m] * dt); c->r2[m] = rr * rr;
        c->exc[m] = a[m] * amp * (0.9f + 0.2f * zm_frand(&c->rng)) * sinf(6.2831853f * f[m] * dt);
        c->y1[m] = c->exc[m];
    }
    if (th_amp > 0.0f) {                   /* the knock through the panel: a fifth, low mode */
        float rr = expf(-dt / (th_tau_ms * 0.001f));
        c->c[4] = 2.0f * rr * cosf(6.2831853f * th_f * dt); c->r2[4] = rr * rr;
        c->exc[4] = c->y1[4] = th_amp * amp / 0.11f * sinf(6.2831853f * th_f * dt);
    }
    c->nz = amp * 0.35f;                    /* the contact's broadband tick */
    c->second = second_ms > 0.0f; c->t2 = second_ms * 0.001f; c->amp2 = second_amp;
}
static void zm_click_kind(int kind, float a) {
    static const float wf[4] = { 3200.0f, 1250.0f, 6100.0f, 0.0f }, wt[4] = { 5.0f, 9.0f, 2.0f, 0.0f }, wa[4] = { 1.0f, 0.5f, 0.4f, 0.0f };
    static const float tf[4] = { 3700.0f, 1500.0f, 6800.0f, 0.0f }, tt[4] = { 4.0f, 8.0f, 2.0f, 0.0f }, ta[4] = { 1.0f, 0.45f, 0.35f, 0.0f };
    static const float mf[4] = { 2400.0f, 900.0f, 5200.0f, 0.0f }, mt[4] = { 6.0f, 12.0f, 3.0f, 0.0f }, ma[4] = { 1.0f, 0.6f, 0.3f, 0.0f };
    static const float af[4] = { 1700.0f, 3900.0f, 650.0f, 7400.0f }, at[4] = { 15.0f, 6.0f, 25.0f, 2.0f }, aa[4] = { 1.0f, 0.6f, 0.45f, 0.25f };
    switch (kind) {
    case EV_CLICK_WEAPON: zm_click_start(A_THROTTLE, wf, wt, wa, 0.036f * a, 0.0f, 0.0f, 0.0f, 85.0f, 0.5f); break;
    case EV_CLICK_TGT_DN: zm_click_start(A_STICK, tf, tt, ta, 0.040f * a, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f); break;
    case EV_CLICK_TGT_UP: zm_click_start(A_STICK, tf, tt, ta, 0.022f * a, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f); break;
    case EV_CLICK_MAP:    zm_click_start(A_PANEL, mf, mt, ma, 0.034f * a, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f); break;   /* a = 0.5: release */
    case EV_ARM:          zm_click_start(A_ARM, af, at, aa, 0.045f * a, 160.0f, 15.0f, 0.04f, 0.0f, 0.0f); break;
    default: break;
    }
}
/* a thump: a low knock through the structure (sine burst), the clank of metal modes and a contact tick */
static void zm_thump_start(int anchor, float amp, float f, float tau_ms, float m0, float m1, float m2, float clank) {
    int k = 0;
    for (int i = 0; i < 12; i++) { if (!zm.thump[i].on) { k = i; break; } if (zm.thump[i].t > zm.thump[k].t) k = i; }
    zs_thump *h = &zm.thump[k];
    memset(h, 0, sizeof *h);
    amp *= 0.46f;                          /* (the table's levels are the peaks of the knock before the clank) */
    h->on = 1; h->anchor = anchor; h->amp = amp;
    h->rng = 0x2545F491u ^ (u32)(zm.samples * 40503u); if (!h->rng) h->rng = 1u;
    /* modes: the structure's knock (f, tau) and three metal modes of the clank; each a two-pole resonator started by
       an impulse (y1 = a sin(w) rings with peak a) */
    const float dt = 1.0f / ZS_RATE, mf[4] = { f, m0, m1, m2 }, md[4] = { tau_ms * 0.001f, 0.05f, 0.03f, 0.02f },
                ma[4] = { 1.0f, 0.45f * clank, 0.25f * clank, 0.12f * clank };
    for (int m = 0; m < 4; m++) {
        if (mf[m] <= 0.0f) continue;
        float rr = expf(-dt / md[m]);
        h->c[m] = 2.0f * rr * cosf(6.2831853f * mf[m] * dt); h->r2[m] = rr * rr;
        h->y1[m] = ma[m] * amp * sinf(6.2831853f * mf[m] * dt);
    }
    h->nz = amp * 0.25f;
}
/* the gear's two phases: kind 1 / 2 = retraction / extension starting (seconds from the selector: doors and uplocks
   release, the pump runs), kind 3 / 4 = the legs reaching up / down (seconds from that moment: the locks, nose first,
   then the mains a little apart; after a retraction the doors close) */
static void zm_gear_seq(float t0, float t1, int kind) {
    static const struct { float t; int what; } st_up[] = { { 0.00f, 0 }, { 0.03f, 1 } }, st_dn[] = { { 0.00f, 7 }, { 0.05f, 8 } },
        lk_up[] = { { 0.00f, 2 }, { 0.15f, 3 }, { 0.23f, 4 }, { 0.65f, 5 }, { 0.80f, 6 } }, lk_dn[] = { { 0.00f, 9 }, { 0.12f, 10 }, { 0.18f, 11 } };
    const struct { float t; int what; } *tab = kind == 1 ? (const void *)st_up : kind == 2 ? (const void *)st_dn : kind == 3 ? (const void *)lk_up : (const void *)lk_dn;
    int n = kind == 1 || kind == 2 ? 2 : kind == 3 ? 5 : 3;
    for (int i = 0; i < n; i++) {
        float t = tab[i].t; int w = tab[i].what;
        if (t < t0 || t >= t1) continue;
        switch (w) {
        case 0: zm_thump_start(A_NOSE, 0.07f, 75.0f, 50.0f, 480.0f, 1150.0f, 2600.0f, 0.8f); break;      /* nose door / uplock */
        case 1: zm_thump_start(A_MAIN_L, 0.06f, 68.0f, 50.0f, 420.0f, 1080.0f, 2450.0f, 0.7f);
                zm_thump_start(A_MAIN_R, 0.06f, 70.0f, 50.0f, 440.0f, 1120.0f, 2500.0f, 0.7f); break;
        case 2: zm_thump_start(A_NOSE, 0.10f, 60.0f, 70.0f, 700.0f, 1650.0f, 2900.0f, 0.9f); break;      /* nose up-lock */
        case 3: zm_thump_start(A_MAIN_L, 0.13f, 52.0f, 90.0f, 640.0f, 1550.0f, 2750.0f, 1.0f); break;   /* mains up-lock */
        case 4: zm_thump_start(A_MAIN_R, 0.13f, 54.0f, 90.0f, 660.0f, 1580.0f, 2800.0f, 1.0f); break;
        case 5: zm_thump_start(A_MAIN_L, 0.05f, 85.0f, 40.0f, 520.0f, 1300.0f, 0.0f, 0.6f);
                zm_thump_start(A_MAIN_R, 0.05f, 88.0f, 40.0f, 530.0f, 1320.0f, 0.0f, 0.6f); break;        /* doors close */
        case 6: zm_thump_start(A_NOSE, 0.04f, 90.0f, 35.0f, 560.0f, 1400.0f, 0.0f, 0.6f); break;
        case 7: zm_thump_start(A_NOSE, 0.06f, 78.0f, 45.0f, 500.0f, 1200.0f, 2600.0f, 0.7f); break;      /* doors open, uplocks */
        case 8: zm_thump_start(A_MAIN_L, 0.07f, 66.0f, 55.0f, 430.0f, 1100.0f, 2500.0f, 0.8f);
                zm_thump_start(A_MAIN_R, 0.07f, 64.0f, 55.0f, 450.0f, 1130.0f, 2520.0f, 0.8f); break;
        case 9: zm_thump_start(A_NOSE, 0.11f, 58.0f, 80.0f, 680.0f, 1600.0f, 2850.0f, 1.0f); break;      /* down and locked */
        case 10: zm_thump_start(A_MAIN_L, 0.15f, 50.0f, 100.0f, 620.0f, 1500.0f, 2700.0f, 1.0f); break;
        case 11: zm_thump_start(A_MAIN_R, 0.15f, 51.0f, 100.0f, 630.0f, 1520.0f, 2720.0f, 1.0f); break;
        }
    }
}

static void zm_take_events(int play, int items) {
    u32 n = __atomic_load_n(&zs_ev_seq, __ATOMIC_ACQUIRE);
    if (n - zm.rd > 32u) zm.rd = n - 32u;
    for (; zm.rd != n; zm.rd++) {
        u32 kind = zs_ev[zm.rd % 32u].kind; float a = zs_ev[zm.rd % 32u].a;
        if (!play) continue;
        switch (kind) {
        case EV_CLICK_WEAPON: case EV_CLICK_TGT_DN: case EV_CLICK_TGT_UP: case EV_CLICK_MAP: case EV_ARM:
            if (items & ZI_SWITCH) zm_click_kind((int)kind, a);
            break;
        case EV_GEAR_UP: case EV_GEAR_DOWN:
            if (items & ZI_GEAR) {      /* a 1: from an end (doors, uplocks); a 0.5: resuming from a halt (the pump only) */
                if (a > 0.75f) { zm.gseq.t = 0.0f; zm.gseq.kind = kind == EV_GEAR_DOWN ? 2 : 1; }
                zm.gwh.target = 1.0f; zm.gwh.t = 0.0f; zm.gwh.f0 = kind == EV_GEAR_DOWN ? 380.0f : 410.0f; }
            break;
        case EV_GEAR_LOCKED:        /* a = 1 down and locked, 0 up and locked: the locks; 0.5 halted: only the pump stops */
            if ((items & ZI_GEAR) && (a < 0.25f || a > 0.75f)) { zm.gseq.t = 0.0f; zm.gseq.kind = a > 0.5f ? 4 : 3; }
            zm.gwh.target = 0.0f;
            break;
        case EV_FLAP_MOVE:          /* a: -1 retracting, +1 extending */
            if (items & ZI_FLAPS) { zm.fwh.target = 1.0f; zm.fwh.t = 0.0f; zm.fwh.f0 = a < 0.0f ? 300.0f : 280.0f; }
            break;
        case EV_FLAP_STOP:          /* a 1: at the stop (a thud each side), 0: halted half-way */
            zm.fwh.target = 0.0f;
            if ((items & ZI_FLAPS) && a > 0.5f) {
                zm_thump_start(A_FLAP_L, 0.050f, 110.0f, 35.0f, 720.0f, 1750.0f, 0.0f, 0.4f);
                zm_thump_start(A_FLAP_R, 0.045f, 115.0f, 35.0f, 740.0f, 1780.0f, 0.0f, 0.4f);
            }
            break;
        case EV_BRAKE_MOVE:
            if (items & ZI_BRAKE) { zm.bwh.target = 1.0f; zm.bwh.t = 0.0f; zm.bwh.f0 = a > 0.0f ? 560.0f : 500.0f; }
            break;
        case EV_BRAKE_STOP:
            zm.bwh.target = 0.0f;
            if ((items & ZI_BRAKE) && a > 0.5f) zm_thump_start(A_BRAKE, 0.045f, 95.0f, 40.0f, 760.0f, 1900.0f, 0.0f, 0.5f);
            else if ((items & ZI_BRAKE) && a < -0.5f) zm_thump_start(A_BRAKE, 0.035f, 100.0f, 35.0f, 800.0f, 2000.0f, 0.0f, 0.4f);
            break;
        default: break;
        }
    }
}
/* one sample of a hydraulic whine: the pump's tone (harmonics), its flow ripple, the hiss of the fluid, through the
   structure (low-passed) */
static float zm_whine(zs_whine *w, float load) {
    const float dt = 1.0f / ZS_RATE;
    float k = w->target > w->env ? dt / 0.15f : dt / 0.30f;
    w->env += (w->target - w->env) * k * 3.0f;
    if (w->env < 1e-4f && w->target == 0.0f) { w->env = 0.0f; return 0.0f; }
    if (!w->rng) w->rng = 0x7F4A7C15u;
    w->t += dt;
    float spin = w->t < 0.4f ? 0.88f + 0.3f * w->t : 1.0f;              /* the pump spins up */
    w->f = w->f0 * spin * (1.0f - 0.035f * load);
    w->rip += 6.5f * dt; if (w->rip >= 1.0f) w->rip -= 1.0f;
    float am = 1.0f + 0.15f * sinf(6.2831853f * w->rip);
    w->ph[0] += w->f * dt; if (w->ph[0] >= 1.0f) w->ph[0] -= 1.0f;
    float s1 = sinf(6.2831853f * w->ph[0]), c1 = cosf(6.2831853f * w->ph[0]);
    float s2 = 2.0f * s1 * c1, c2 = 2.0f * c1 * c1 - 1.0f, s3 = s2 * c1 + c2 * s1, s4 = 2.0f * s2 * c2;
    float s = s1 + 0.5f * s2 + 0.25f * s3 + 0.12f * s4;             /* the pump's tone and its harmonics */
    float nz = zm_noise(&w->rng);
    w->hp += (nz - w->hp) * 0.35f;                                     /* hiss: noise minus its low part */
    s = s * am * 0.6f + (nz - w->hp) * 0.25f;
    w->lp1 += (s - w->lp1) * 0.21f; w->lp2 += (w->lp1 - w->lp2) * 0.21f;  /* ~1.8 kHz, twice: through the airframe */
    return w->lp2 * w->env;
}
/* airflow: low-passed noise whose corner and level follow the dynamic pressure, with a slow buffet modulation */
static float zm_air(zs_air *a, float target, float kt) {
    const float dt = 1.0f / ZS_RATE;
    a->g += (target - a->g) * dt / 0.25f;
    if (a->g < 1e-5f && target == 0.0f) { a->g = 0.0f; return 0.0f; }
    if (!a->rng) a->rng = 0x6C8E9CF5u;
    float k = a->bp1;                                                   /* corner coefficient, set per chunk */
    float nz = zm_noise(&a->rng);
    a->lp1 += (nz - a->lp1) * k; a->lp2 += (a->lp1 - a->lp2) * k;
    a->amlp += (zm_noise(&a->rng) - a->amlp) * (6.2831853f * 7.0f * dt);   /* ~7 Hz buffet */
    float am = 1.0f + zm.am_k * a->amlp;
    return a->lp2 * 2.2f * a->g * am;
}

static void zm_wav_open(void) {
    const char *p = getenv("ACZ_DEBUG_SYSSND_WAV");
    if (!p || !*p) return;
    zm.wav = fopen(p, "wb");
    if (!zm.wav) return;
    u8 h[44] = { 0 };
    fwrite(h, 1, 44, zm.wav);
}
static void zm_wav_close(void) {
    if (!zm.wav) return;
    u8 h[44]; u32 v;
    memcpy(h, "RIFF", 4); v = 36u + zm.wav_bytes; memcpy(h + 4, &v, 4); memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4); u16 s = 1; memcpy(h + 20, &s, 2); s = 2; memcpy(h + 22, &s, 2); v = 48000; memcpy(h + 24, &v, 4);
    v = 48000u * 4u; memcpy(h + 28, &v, 4); s = 4; memcpy(h + 32, &s, 2); s = 16; memcpy(h + 34, &s, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &zm.wav_bytes, 4);
    fseek(zm.wav, 0, SEEK_SET); fwrite(h, 1, 44, zm.wav); fclose(zm.wav); zm.wav = NULL;
}

/* the mixer entry: adds this chunk of the system sounds into buf (interleaved stereo s16) */
static void zs_mix(s16 *buf, int frames) {
    u64 t0 = zs_ticks();
    if (!zm.init) {
        zm.init = 1;
        for (int a = 0; a < ZS_NA; a++) zm.bin[a].fresh = 1;
        zm.gwh.f0 = 410.0f; zm.bwh.f0 = 540.0f; zm.duck = 1.0f; zm.bed = 1.0f; zm.am_k = 2.5f;
        zm_wav_open();
    }
    /* the newest state */
    static zs_state st;
    for (int tries = 0; tries < 4; tries++) {
        u32 a = __atomic_load_n(&zs_seq, __ATOMIC_ACQUIRE);
        if (a & 1u) continue;
        zs_state tmp; memcpy(&tmp, &zs_pub, sizeof tmp);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&zs_seq, __ATOMIC_RELAXED) == a) { if (a) st = tmp; break; }
    }
    u64 now = ps2_kernel_vblank_count();
    int run = st.field && now - st.field <= 8u && st.level > 0.0f;
    zm_take_events(run, st.items);
    float gain = st.level;
    extern int ps2_spatial_hrtf_on(void);
    int hrtf = ps2_spatial_hrtf_on();
    const float dt = 1.0f / ZS_RATE;
    float mu = st.mu, ma = 1.0f - expf(-6.2831853f * 3000.0f * dt), mg = 1.0f - mu * (1.0f - 0.708f);
    float q = st.kt / 200.0f; q = q * q; q = q > 1.4f ? 1.4f : q;            /* dynamic pressure, 1 at 200 kt */
    float qb = st.kt / 300.0f; qb = qb * qb; qb = qb > 1.5f ? 1.5f : qb;
    /* ACZ_SND_SYSTEMS_MIX=2 (default): fewer layers on the take-off roll and at lift-off -- under the helmet the
       runway and the hydraulics are felt more than heard (pilots: "you'd be surprised how silent hydraulics are",
       the gear felt hitting the uplock): the gear's airflow 6 dB lower with a gentler wobble, the flaps' airflow only
       in the air and 6 dB lower, the pumps and motors 2-6 dB lower; the wheels' roll 8 dB lower and fading under the
       engine above ~90 kt (the slab joints' knocks keep the rhythm) */
    int m2 = st.mixv != 1;
    float k_ga = m2 ? 0.14f : 0.275f, k_fa = m2 ? (st.ground > 0.0f ? 0.0f : 0.05f) : 0.10f;
    zm.am_k = m2 ? 1.2f : 2.5f;
    float t_gair = run ? ((st.items & ZI_GEAR) ? k_ga * st.gear * q : 0.0f) + ((st.items & ZI_FLAPS) ? k_fa * st.flaps * q : 0.0f) : 0.0f;
    /* the radio is never covered: while a radio message plays (ps2_nustream.c) the continuous layers (airflow, the
       wheels' roll) sit 4 dB lower; the short clicks and thumps stay. With the immersion bus (ps2_gforce_audio.c,
       ACZ_IMM_DUCK) they also give way to a loud game (its gain includes the radio's 4 dB) */
    {   extern int ps2_nustream_radio_active(void);
        extern int ps2_imm_bus_on(void);
        extern float ps2_imm_duck(int cls);
        float dk = ps2_nustream_radio_active() ? 0.63f : 1.0f;
        zm.duck += (dk - zm.duck) * (frames / (ZS_RATE * 0.25f) > 1.0f ? 1.0f : frames / (ZS_RATE * 0.25f));
        if (zm.duck <= 0.0f) zm.duck = 1.0f;
        zm.bed = ps2_imm_bus_on() ? ps2_imm_duck(0) : zm.duck; }
    t_gair *= st.g_air * zm.bed;
    float t_bair = run && (st.items & ZI_BRAKE) ? 0.150f * st.brake * qb * st.g_air * zm.bed : 0.0f;
    if (!run) { zm.gwh.target = 0.0f; zm.bwh.target = 0.0f; zm.fwh.target = 0.0f; }
    {   float fc = 140.0f + 2.2f * st.kt; fc = fc > 1100.0f ? 1100.0f : fc;
        zm.gair.bp1 = zm.bair.bp1 = 1.0f - expf(-6.2831853f * fc * dt); }
    int busy = zm.gwh.env > 0.0f || zm.gwh.target > 0.0f || zm.bwh.env > 0.0f || zm.bwh.target > 0.0f || zm.gair.g > 0.0f
               || zm.fwh.env > 0.0f || zm.fwh.target > 0.0f
               || zm.bair.g > 0.0f || t_gair > 0.0f || t_bair > 0.0f || zm.gseq.kind;
    for (int i = 0; i < 4; i++) busy |= zm.click[i].on;
    for (int i = 0; i < 12; i++) busy |= zm.thump[i].on;
    busy |= st.ground > 0.0f || zm.roll_g > 0.0f;
    if (!busy && !zm.wav) { zm.samples += (u64)frames; return; }
    if (busy) zm.active += (u64)frames;
    for (int i = 0; i < frames; i++) {
        if (i % 32 == 0) {
            for (int a = 0; a < ZS_NA; a++) zs_bin_block(&zm.bin[a], st.dir[a], hrtf);
            /* the gear's timeline */
            if (zm.gseq.kind) {
                float t1 = zm.gseq.t + 32.0f * dt;
                zm_gear_seq(zm.gseq.t, t1, zm.gseq.kind);
                zm.gseq.t = t1;
                if (zm.gseq.t > 1.0f) zm.gseq.kind = 0;
            }
            if (zm.gwh.target > 0.0f && zm.gwh.t > 20.0f) zm.gwh.target = 0.0f;       /* (a lost lock event) */
            if (zm.fwh.target > 0.0f && zm.fwh.t > 20.0f) zm.fwh.target = 0.0f;
        }
        for (int a = 0; a < ZS_NA; a++) zm.bin[a].acc = 0.0f;
        /* clicks */
        for (int k = 0; k < 4; k++) {
            zs_click *c = &zm.click[k];
            if (!c->on) continue;
            float s = 0.0f, live = 0.0f;
            for (int m = 0; m < 5; m++) {
                if (c->c[m] == 0.0f) continue;
                float y = c->c[m] * c->y1[m] - c->r2[m] * c->y2[m];
                c->y2[m] = c->y1[m]; c->y1[m] = y; s += y; live += fabsf(y);
            }
            if (c->t < 0.0015f) s += c->nz * zm_noise(&c->rng) * (1.0f - c->t / 0.0015f);
            if (c->second && c->t >= c->t2) {        /* the release: the same modes, softer */
                c->second = 0;
                for (int m = 0; m < 5; m++) c->y1[m] += c->amp2 * c->exc[m];
                c->t2 = c->t; c->nz = c->amp * c->amp2 * 0.3f;
            }
            if (c->t2 > 0.0f && c->t - c->t2 < 0.0012f && c->t >= c->t2) s += c->nz * zm_noise(&c->rng);
            zm.bin[c->anchor].acc += s * st.g_inside;
            c->t += dt;
            if (c->t > 0.05f && live < 1e-5f && !c->second) c->on = 0;
            if (c->t > 0.6f) c->on = 0;
        }
        /* thumps */
        for (int k = 0; k < 12; k++) {
            zs_thump *h = &zm.thump[k];
            if (!h->on) continue;
            float s = 0.0f, live = 0.0f;
            for (int m = 0; m < 4; m++) {
                if (h->c[m] == 0.0f) continue;
                float y = h->c[m] * h->y1[m] - h->r2[m] * h->y2[m];
                h->y2[m] = h->y1[m]; h->y1[m] = y; s += y; live += fabsf(y);
            }
            if (h->t < 0.002f) s += h->nz * zm_noise(&h->rng);
            zm.bin[h->anchor].acc += s * st.g_mech;
            h->t += dt;
            if ((h->t > 0.08f && live < 1e-5f) || h->t > 1.0f) h->on = 0;
        }
        /* whines */
        if (zm.gwh.env > 0.0f || zm.gwh.target > 0.0f) {
            float load = zm.gwh.f0 > 400.0f && zm.gwh.t > 0.3f ? 1.0f : 0.3f;   /* lifting the legs loads the pump (retraction) */
            float w = zm_whine(&zm.gwh, load) * (m2 ? 0.021f : 0.042f) * st.g_mech;
            zm.bin[A_NOSE].acc += 0.4f * w; zm.bin[A_MAIN_L].acc += 0.3f * w; zm.bin[A_MAIN_R].acc += 0.3f * w;
        }
        if (zm.fwh.env > 0.0f || zm.fwh.target > 0.0f) {             /* the flap motor, both wing roots */
            float w = zm_whine(&zm.fwh, 0.5f) * (m2 ? 0.014f : 0.022f) * st.g_mech;
            zm.bin[A_FLAP_L].acc += 0.5f * w; zm.bin[A_FLAP_R].acc += 0.5f * w;
        }
        if (zm.bwh.env > 0.0f || zm.bwh.target > 0.0f) {
            zm.bin[A_BRAKE].acc += zm_whine(&zm.bwh, 0.5f) * (m2 ? 0.012f : 0.016f) * st.g_mech;
            if (zm.bwh.t > 0.9f) zm.bwh.target = 0.0f;                 /* an actuator stroke: under a second */
        }
        /* the wheels on the runway: a low roll rising with speed, and the slab joints (every ~6 m) knocking under the nose
           wheel, then under the mains a wheelbase later; the knocks merge into the roll above ~100 kt */
        if (st.ground > 0.0f || zm.roll_g > 0.0f) {
            float v = st.kt * 0.5144f, tr = run && (st.items & ZI_GEAR) ? st.ground * (v > 2.0f ? (v < 40.0f ? v / 40.0f : 1.0f) : 0.0f) : 0.0f;
            if (m2 && tr > 0.0f) {          /* (mix 2) up by 20 m/s, then fading to a quarter between 45 and 80 m/s */
                float u = (v - 45.0f) / 35.0f; u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
                tr = st.ground * (v < 20.0f ? v / 20.0f : 1.0f) * (1.0f - 0.75f * u * u * (3.0f - 2.0f * u));
            }
            zm.roll_g += (tr - zm.roll_g) * (dt / 0.15f);
            if (tr == 0.0f && zm.roll_g < 1e-4f) zm.roll_g = 0.0f;
            if (!zm.roll_rng) zm.roll_rng = 0x1B873593u;
            float nz = zm_noise(&zm.roll_rng);
            float kr = 0.012f + 0.0004f * st.kt;                       /* ~90..500 Hz with speed */
            zm.roll_lp1 += (nz - zm.roll_lp1) * kr; zm.roll_lp2 += (zm.roll_lp1 - zm.roll_lp2) * kr;
            float roll = zm.roll_lp2 * (m2 ? 0.18f : 0.45f) * zm.roll_g * st.g_mech * zm.bed;
            zm.bin[A_NOSE].acc += 0.5f * roll; zm.bin[A_MAIN_L].acc += 0.25f * roll; zm.bin[A_MAIN_R].acc += 0.25f * roll;
            if (i % 32 == 0 && tr > 0.0f && v > 2.0f) {
                zm.joint_ph += v * 32.0f * dt / 6.0f;
                float jg = v < 50.0f ? 1.0f : v > 70.0f ? 0.0f : (70.0f - v) / 20.0f;     /* knocks fade above ~100-135 kt */
                if (zm.joint_ph >= 1.0f) {
                    zm.joint_ph -= 1.0f;
                    if (jg > 0.0f) {
                        float a = (0.020f + 0.0005f * v) * jg * st.g_mech;
                        zm_thump_start(A_NOSE, a, 85.0f, 25.0f, 380.0f, 900.0f, 0.0f, 0.3f);
                        float wb = 5.0f / v;                               /* the mains reach the joint a wheelbase later */
                        if (zm.joint_n < 4) zm.joint_due[zm.joint_n++] = wb;
                    }
                }
                for (int k = 0; k < zm.joint_n; ) {
                    zm.joint_due[k] -= 32.0f * dt;
                    if (zm.joint_due[k] <= 0.0f) {
                        float a = (0.026f + 0.0006f * v) * jg * st.g_mech;
                        zm_thump_start(A_MAIN_L, a, 70.0f, 30.0f, 340.0f, 820.0f, 0.0f, 0.3f);
                        zm_thump_start(A_MAIN_R, a * 0.9f, 72.0f, 30.0f, 350.0f, 840.0f, 0.0f, 0.3f);
                        zm.joint_due[k] = zm.joint_due[--zm.joint_n];
                    } else k++;
                }
            }
        }
        /* airflow */
        if (zm.gair.g > 0.0f || t_gair > 0.0f) zm.bin[A_GEAR_AIR].acc += zm_air(&zm.gair, t_gair, st.kt);
        if (zm.bair.g > 0.0f || t_bair > 0.0f) zm.bin[A_BRAKE_AIR].acc += zm_air(&zm.bair, t_bair, st.kt);
        float l = 0.0f, r = 0.0f;
        for (int a = 0; a < ZS_NA; a++) {
            zs_bin *b = &zm.bin[a];
            float x = b->acc;
            if (x != 0.0f) b->live = 256;
            else if (b->live > 0) b->live--;
            else continue;
            float bl = 0.0f, brr = 0.0f;
            zs_bin_run(b, x, &bl, &brr);
            if (zs_outside[a] && mu > 0.0f) {           /* outside the canopy: muffled in the cockpit view */
                b->mlp[0] += ma * (bl - b->mlp[0]); b->mlp[1] += ma * (brr - b->mlp[1]);
                bl = (bl + mu * (b->mlp[0] - bl)) * mg; brr = (brr + mu * (b->mlp[1] - brr)) * mg;
            }
            l += bl; r += brr;
        }
        int L = (int)lrintf(l * gain * 32767.0f), R = (int)lrintf(r * gain * 32767.0f);
        if (zm.wav) { s16 o[2] = { (s16)(L > 32767 ? 32767 : L < -32768 ? -32768 : L), (s16)(R > 32767 ? 32767 : R < -32768 ? -32768 : R) };
                      fwrite(o, 2, 2, zm.wav); zm.wav_bytes += 4u; }
        L += buf[2 * i]; R += buf[2 * i + 1];
        buf[2 * i] = (s16)(L > 32767 ? 32767 : L < -32768 ? -32768 : L);
        buf[2 * i + 1] = (s16)(R > 32767 ? 32767 : R < -32768 ? -32768 : R);
    }
    zm.samples += (u64)frames; zm.chunks++;
    u64 d = zs_ticks() - t0; zm.ticks += d; if (d > zm.max_ticks) zm.max_ticks = d;
}

/* ================= install / report ================= */
static void zs_close_files(void) {
    if (zs_dump) { fclose(zs_dump); zs_dump = NULL; }
    zm_wav_close();
}
static void zs_test_parse(void) {
    const char *e = getenv("ACZ_SND_SYSTEMS_TEST");
    if (!e) return;
    for (const char *p = e; *p && zs_ntst < 32; ) {
        while (*p == ' ' || *p == ',') p++;
        char *end; unsigned long long f = strtoull(p, &end, 0);
        if (end == p || *end != ':') break;
        p = end + 1;
        const char *w = p; while (*p && *p != ',' && *p != ' ') p++;
        size_t n = (size_t)(p - w);
        u32 k = !strncmp(w, "weapon", n) ? EV_CLICK_WEAPON : !strncmp(w, "tdn", n) ? EV_CLICK_TGT_DN : !strncmp(w, "tup", n) ? EV_CLICK_TGT_UP
              : !strncmp(w, "map", n) ? EV_CLICK_MAP : !strncmp(w, "arm", n) ? EV_ARM : !strncmp(w, "gearup", n) ? EV_GEAR_UP
              : !strncmp(w, "geardown", n) ? EV_GEAR_DOWN : !strncmp(w, "brake", 5) ? EV_BRAKE_MOVE : EV_NONE;
        if (k == EV_NONE) continue;
        zs_tst[zs_ntst].f = f; zs_tst[zs_ntst].kind = k; zs_tst[zs_ntst].a = 1.0f;
        if (k == EV_BRAKE_MOVE) {           /* brake=V@F2 */
            float v = 1.0f; unsigned long long f2 = f + 120u;
            sscanf(w, "brake=%f@%llu", &v, &f2);
            zs_tst[zs_ntst].a = v; zs_tst[zs_ntst].f2 = f2;
        }
        zs_ntst++;
    }
    ps2_log("syssnd: %d test events (ACZ_SND_SYSTEMS_TEST)", zs_ntst);
}

void zero_syssnd_menv_reload(void) { zs_read_env(); }   /* the NEXT MISSION tier (ps2_menv.h): the game thread, at the edge */
void zero_syssnd_install(void) {
    if (!zero_vr_is_zero()) return;
    zs_read_env();
    const char *e = getenv("ACZ_DEBUG_SYSSND"); zs_dbg = e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_SYSSND_TRACE"); zs_trace = e && *e == '1';
    e = getenv("ACZ_DEBUG_SYSSND_STATE"); zs_state_on = e && *e == '1';
    e = getenv("ACZ_DEBUG_SYSSND_THREAT"); zs_threat_any = e && *e == '1';
    e = getenv("ACZ_DEBUG_SYSSND_DUMP");
    if (e && *e) {
        zs_dump = fopen(e, "wb");
        const char *n = getenv("ACZ_DEBUG_SYSSND_DUMP_EVERY");
        if (n) zs_dump_every = atoi(n) > 0 ? atoi(n) : 15;
    }
    e = getenv("ACZ_DEBUG_SYSSND_CHASE");
    if (e && *e) { zs_ch.on = 1; zs_ch.from = strtoull(e, NULL, 0); extern int (*ps2_pad_test_steer)(u8 *, u8 *); ps2_pad_test_steer = zs_chase_steer; }
    zs_test_parse();
    {   /* ACZ_SND_WARN_TEST="bank,id[;bank,id...]" (test): treat those sounds as warning tones (e.g. the weapon switch
           beeps 1,6;1,7) so a scripted press shows the placement */
        const char *t = getenv("ACZ_SND_WARN_TEST");
        for (const char *q = t; q && *q && zs_ncls < 32; ) {
            unsigned b, i; int n = 0;
            if (sscanf(q, "%u,%u%n", &b, &i, &n) < 2) break;
            zs_cls[zs_ncls].key = b << 16 | (i & 0xFFFFu); zs_cls[zs_ncls].cls = ZC_WARN_RADAR; zs_ncls++;
            q += n; while (*q == ';' || *q == ' ') q++;
        }
        if (t) ps2_log("syssnd: %d test warning tones (ACZ_SND_WARN_TEST)", zs_ncls);
    }
    if (ps2_r32(ZS_HUDMGR) >> 16 != 0x27BDu || ps2_r32(ZS_SNDSTART) != 0x27BDFF50u || ps2_r32(ZS_REQ2D) != 0x27BDFFD0u
        || ps2_r32(ZS_REQSE) != 0x27BDFFD0u) {
        ps2_log("syssnd: code mismatch -- cockpit system sounds off");
        return;
    }
    if (zs_trace)
        for (u32 k = 0; k < ZS_NMGR; k++) ps2_hook_before(zs_mgr[k], zs_req_enter, (void *)(uintptr_t)zs_mgr[k], 50, "zero-syssnd");
    ps2_hook_before(ZS_REQ2D, zs_req2d_enter, NULL, 49, "zero-syssnd");
    ps2_hook_before(ZS_REQSE, zs_req2d_enter, NULL, 49, "zero-syssnd");   /* (the PULL UP alert, 1,254..256) */
    ps2_hook_before(ZS_SNDSTART, zs_start_enter, NULL, 50, "zero-syssnd");
    ps2_hook_after(ZS_SNDSTART, zs_start_leave, NULL, 50, "zero-syssnd");
    ps2_hook_before(ZS_HUDMGR, zs_hud_enter, NULL, 50, "zero-syssnd");
    {   extern void (*ps2_audio_sys_mix)(s16 *buf, int frames);
        ps2_audio_sys_mix = zs_mix; }
    atexit(zs_close_files);
    ps2_log("syssnd: cockpit system sounds level %.2f mix %d items %s%s%s%s, the game's warning tones from the panel %s (bearing %.2f)%s",
            zs_level, zs_mixv, zs_items & ZI_SWITCH ? "switch " : "", zs_items & ZI_GEAR ? "gear " : "", zs_items & ZI_FLAPS ? "flaps " : "",
            zs_items & ZI_BRAKE ? "brake" : "",
            zs_warn_place ? "on" : "off", zs_bearing, zs_trace ? " (trace)" : "");
}
void zero_syssnd_report(void) {
    if (!zs_ee_n) return;
    double us = zs_tick_us();
    ps2_log("syssnd: %llu frames, EE %.2f us per frame (max %.1f); events weapon %llu target %llu/%llu map %llu arm %llu gear up %llu "
            "down %llu flaps %llu brake %llu; warning tones placed: radar %llu lock-seek %llu locked %llu missile %llu sp %llu "
            "pull-up %llu fields",
            (unsigned long long)zs_ee_n, (double)zs_ee_ticks * us / (double)zs_ee_n, (double)zs_ee_max * us,
            (unsigned long long)zs_nev[EV_CLICK_WEAPON], (unsigned long long)zs_nev[EV_CLICK_TGT_DN], (unsigned long long)zs_nev[EV_CLICK_TGT_UP],
            (unsigned long long)zs_nev[EV_CLICK_MAP], (unsigned long long)zs_nev[EV_ARM], (unsigned long long)zs_nev[EV_GEAR_UP],
            (unsigned long long)zs_nev[EV_GEAR_DOWN], (unsigned long long)zs_nev[EV_FLAP_MOVE], (unsigned long long)zs_nev[EV_BRAKE_MOVE],
            (unsigned long long)zs_placed_fields[ZC_WARN_RADAR], (unsigned long long)zs_placed_fields[ZC_LOCK_SEEK],
            (unsigned long long)zs_placed_fields[ZC_LOCK_ON], (unsigned long long)zs_placed_fields[ZC_WARN_MISSILE],
            (unsigned long long)zs_placed_fields[ZC_LOCK_SP], (unsigned long long)zs_placed_fields[ZC_WARN_PULLUP]);
    if (zm.samples)
        ps2_log("syssnd: mixer %.3f ms per second of audio (%.1f s mixed, %.1f s with sounds active; longest chunk %.3f ms)",
                (double)zm.ticks * us / 1000.0 / ((double)zm.samples / ZS_RATE), (double)zm.samples / ZS_RATE,
                (double)zm.active / ZS_RATE, (double)zm.max_ticks * us / 1000.0);
}
