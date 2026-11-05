// ps2_hotas.c - joysticks, throttles and pedals as the pilot's controls (ctl_hotas, 2026-11-04;
// RELEASE_RESEARCH_2026-11-03.md section 10, docs/research_2026-11-03/hotas_research.md; volunteers: docs/HOTAS.md).
//
// The owner: "I don't have one, but we can add it and someone else tests it." So: every real HOTAS here is untested; the
// shipped profiles (profiles/joystick/*.ini) are marked unverified until a volunteer's device log confirms them.
//
// Devices. Every SDL joystick that is not a gamepad is opened (a flight stick or throttle by SDL's VID/PID lists even if
// a mapping makes it a gamepad: ps2_hotas_claims; the community gamecontrollerdb is never loaded), up to 8, a slot each
// in order of appearance. One log line per device for the volunteers ("joy: <name> GUID ..."); "joy sweep:" lines report
// what each axis and button did, so a volunteer's log says which axis is which. ACZ_HOTAS=0: no joystick is opened.
//
// Profiles. A profile is its own layout, keyed by SDL's GUID, never by slot or path: the settings file's
// [joystick:<GUID>] section if there is one (the player's own; a second device with the same GUID: <GUID>:2), else the
// shipped file that matches by VID:PID, then by a part of the name. A device with no profile does nothing until bound
// (F4 > Controller > Joysticks). A profile with no binding for an action leaves the pad's binding in force: everything
// merges in poll_input per action by max, as Touch does, so a pad beside the stick keeps working.
//   axisN = invert:0 dz:0.05 curve:1.0 slider:0 neutral:0.5
//   bind.pitch = axis1          bind.cross = button0 button4        bind.dpad = hat0        bind.l2 = hat0:left
//
// Mapping (the order for an axis: the profile's invert, dead zone and curve; then ACZ_HOTAS_STICK's direct mapping; then
// ps2_pad_stick's deadzone, axis_scale and invert; then ACZ_PAD_STICK_REMAP):
// - pitch, roll: one bipolar axis each, to the left stick. The game ignores the inner half of each stick axis (bytes
//   128 +- 64), and axis_scale (1.33) saturates it at 75 %, so a long stick would do nothing for the first 37 % and be
//   full at 75 %. ACZ_HOTAS_STICK=direct (default) maps the axis onto what the game answers to, from the first movement
//   to full deflection (as Touch's HANDS ON STICK does); =game goes through as a pad would.
// - yaw: a bipolar axis (twist, rocker, pedals) to L2 / R2 pressure by its sign; or l2 / r2 as two buttons or a hat pair.
//   autopilot: a button pressing L2 + R2 together.
// - throttle (CONTROLS > THROTTLE LEVER, ACZ_HOTAS_THROTTLE): the game has no throttle setting, only L1 / R1 pressure:
//   thr = (1.5 R1 - L1) x 1.176 / 255, afterburner at thr 1. CENTRED (default): a lever around a neutral position (per
//   axis in the profile, 50 % by default, SET LEVER NEUTRAL takes it from the lever) with a dead band (its dz, default
//   8 % each way): above it R1 pressure, below it L1 pressure, linear, with the axis's curve. SPRING: the same with
//   neutral at the axis's centre (a rocker, a paddle). SPLIT: accelerate and brake as their own half axes (accel / brake
//   with an axis are read only in this mode; as buttons always). ABSOLUTE (env only, an experiment): the lever is a
//   target power and L1 / R1 pressure steer the flight model's power onto it. A lever axis without a known position
//   (SDL_GetJoystickAxisInitialState false) is neutral until it moves. One lever: a device of type throttle wins over a
//   stick's own slider, else the lowest slot. A lever (or SPLIT's axes) counts only in flight: resting anywhere, it must not hold
//   L1 or R1 down in a menu or a pause.
// - highg: a button pressing L1 + R1 together (a lever cannot); highg_rule = 1 in a profile: full brake + the stick
//   pulled past half is High-G too (a modern game's "type B"), off by default.
// - hats: bind.dpad = hat0 is the D-pad (wingman commands, menus); the rest are buttons of the game's actions.
//
// FFB (ACZ_FFB, default 0 = off): the merged rumble levels as a sine effect (SDL_Haptic, updated when the level changes),
// or SDL_RumbleJoystick on a stick that reports rumble and no haptic. Non-FFB HOTAS report neither.
//
// Tests without hardware: PS2_PAD_JOY (below) attaches SDL virtual joysticks with real VID/PIDs, so SDL's type detection
// and the profile match run on them; ACZ_DEBUG_HOTAS=N logs the lever, the pressures and the game's power every N fields.
#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_hotas.h"
#include "ps2_settings.h"
#include "ps2_menv.h"
#include "zero_instr.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 ps2_kernel_vblank_count(void);
extern int (*ps2_vk_instr_get)(zero_instr *out);     /* ps2_video.c; set by zero_vr.c */
extern volatile float ps2_imm_in[8];                 /* ps2_video.c: [3] = the field of the last flight frame (zero_vr.c) */
extern void ps2_enh_hotas_rows(int on);              /* ps2_settings.c: the CONTROLS rows show only with a joystick */

#define HJ_SLOTS 8
#define HJ_AX 16                                     /* axes with tuning and a sweep report (DirectInput has 8) */
#define HJ_BTN_MAX 256
#define HJ_CAP_TRAVEL 29491                          /* 45 % of the range: a centred stick pushed to its stop moves 50 % */

enum { HA_PITCH = PS2_ACT_COUNT, HA_ROLL, HA_YAW, HA_THROTTLE, HA_LOOK_X, HA_LOOK_Y, HA_ACCEL, HA_BRAKE, HA_HIGHG,
       HA_AUTOPILOT, HA_DPAD, HA_COUNT };
enum { HK_BUTTON, HK_AXIS, HK_HAT };
static const struct { const char *key, *label, *prompt; unsigned char kind; } ha[HA_COUNT] = {
    [PS2_ACT_SELECT]   = { "select", "Map (Select)", NULL, HK_BUTTON },
    [PS2_ACT_L3]       = { "l3", "L3", NULL, HK_BUTTON },
    [PS2_ACT_R3]       = { "r3", "View (R3)", NULL, HK_BUTTON },
    [PS2_ACT_START]    = { "start", "Pause (Start)", NULL, HK_BUTTON },
    [PS2_ACT_UP]       = { "up", "D-pad up", NULL, HK_BUTTON },
    [PS2_ACT_RIGHT]    = { "right", "D-pad right", NULL, HK_BUTTON },
    [PS2_ACT_DOWN]     = { "down", "D-pad down", NULL, HK_BUTTON },
    [PS2_ACT_LEFT]     = { "left", "D-pad left", NULL, HK_BUTTON },
    [PS2_ACT_L2]       = { "l2", "Yaw left (L2)", NULL, HK_BUTTON },
    [PS2_ACT_R2]       = { "r2", "Yaw right (R2)", NULL, HK_BUTTON },
    [PS2_ACT_L1]       = { "l1", "L1", NULL, HK_BUTTON },     /* (parsed as brake) */
    [PS2_ACT_R1]       = { "r1", "R1", NULL, HK_BUTTON },     /* (parsed as accel) */
    [PS2_ACT_TRIANGLE] = { "triangle", "Target (Triangle)", NULL, HK_BUTTON },
    [PS2_ACT_CIRCLE]   = { "circle", "Missile (Circle)", NULL, HK_BUTTON },
    [PS2_ACT_CROSS]    = { "cross", "Gun (Cross)", NULL, HK_BUTTON },
    [PS2_ACT_SQUARE]   = { "square", "Weapon switch (Square)", NULL, HK_BUTTON },
    [PS2_ACT_LS_UP]    = { "lstick_up", "Stick forward", NULL, HK_BUTTON },
    [PS2_ACT_LS_DOWN]  = { "lstick_down", "Stick back", NULL, HK_BUTTON },
    [PS2_ACT_LS_LEFT]  = { "lstick_left", "Stick left", NULL, HK_BUTTON },
    [PS2_ACT_LS_RIGHT] = { "lstick_right", "Stick right", NULL, HK_BUTTON },
    [PS2_ACT_RS_UP]    = { "rstick_up", "Look up", NULL, HK_BUTTON },
    [PS2_ACT_RS_DOWN]  = { "rstick_down", "Look down", NULL, HK_BUTTON },
    [PS2_ACT_RS_LEFT]  = { "rstick_left", "Look left", NULL, HK_BUTTON },
    [PS2_ACT_RS_RIGHT] = { "rstick_right", "Look right", NULL, HK_BUTTON },
    [HA_PITCH]     = { "pitch", "Pitch", "Pull the stick fully back.", HK_AXIS },
    [HA_ROLL]      = { "roll", "Roll", "Push the stick fully right.", HK_AXIS },
    [HA_YAW]       = { "yaw", "Yaw", "Twist, rock or press yaw fully right.", HK_AXIS },
    [HA_THROTTLE]  = { "throttle", "Throttle lever", "Push the lever fully forward (from the back or the middle).", HK_AXIS },
    [HA_LOOK_X]    = { "look_x", "Look left / right", "Push that axis fully right.", HK_AXIS },
    [HA_LOOK_Y]    = { "look_y", "Look up / down", "Push that axis fully down.", HK_AXIS },
    [HA_ACCEL]     = { "accel", "Accelerate", NULL, HK_BUTTON },
    [HA_BRAKE]     = { "brake", "Brake", NULL, HK_BUTTON },
    [HA_HIGHG]     = { "highg", "High-G turn", NULL, HK_BUTTON },
    [HA_AUTOPILOT] = { "autopilot", "Autopilot", NULL, HK_BUTTON },
    [HA_DPAD]      = { "dpad", "D-pad (a whole hat)", "Press the hat in any direction.", HK_HAT },
};
/* the window's table */
static const struct { int act; const char *group; } hrow[] = {
    { HA_PITCH, "Flight" }, { HA_ROLL, NULL }, { HA_YAW, NULL }, { PS2_ACT_L2, NULL }, { PS2_ACT_R2, NULL },
    { HA_THROTTLE, NULL }, { HA_ACCEL, NULL }, { HA_BRAKE, NULL }, { HA_HIGHG, NULL }, { HA_AUTOPILOT, NULL },
    { PS2_ACT_CROSS, "Weapons" }, { PS2_ACT_CIRCLE, NULL }, { PS2_ACT_SQUARE, NULL }, { PS2_ACT_TRIANGLE, NULL },
    { PS2_ACT_SELECT, "Views and menus" }, { PS2_ACT_START, NULL }, { PS2_ACT_R3, NULL }, { PS2_ACT_L3, NULL },
    { HA_LOOK_X, NULL }, { HA_LOOK_Y, NULL },
    { HA_DPAD, "Wingman commands" }, { PS2_ACT_UP, NULL }, { PS2_ACT_RIGHT, NULL }, { PS2_ACT_DOWN, NULL }, { PS2_ACT_LEFT, NULL },
};
#define HROWS ((int)(sizeof hrow / sizeof hrow[0]))

typedef struct { int invert, slider, set; float dz, curve, neutral; } hax;   /* dz < 0: not set (lever 0.08, else 0.05) */
enum { HT_ANY, HT_STICK, HT_THROTTLE, HT_PEDALS };
typedef struct {
    char name[64], src[80], status[32];
    int kind, highg_rule;
    hax ax[HJ_AX];
    int bind[HA_COUNT][PS2_BIND_SLOTS];              /* local codes: no slot bits */
} hprof;
typedef struct {
    int on, logged, polls, unknown;                  /* unknown: axes without an initial state yet */
    SDL_JoystickID id;
    SDL_Joystick *j;
    char guid[40], secid[48], name[64];
    Uint16 vid, pid, ver, crc;
    SDL_JoystickType type;
    int naxes, nbtn, nhat;
    hprof p;
    struct { short act; short k; int code; } bl[HA_COUNT * PS2_BIND_SLOTS];
    int nbl;
    unsigned char known[HJ_AX], aseen[HJ_AX];
    Sint16 amin[HJ_AX], amax[HJ_AX];
    Uint32 bseen[HJ_BTN_MAX / 32];
    unsigned hseen;
    int sweep_dirty;
    SDL_Haptic *hap;
    SDL_HapticEffectID eff;
    SDL_HapticEffect effd;
    int rumble_cap;
    float ffb_last;
    u64 ffb_ns;
    u32 ffb_n;
    float ffb_peak;
} hdev;
static hdev hd[HJ_SLOTS];
static int hj_n, hj_ready, hj_off = -1;

/* ---- the settings file's [joystick:<id>] sections, as text ---- */
#define HSEC_MAX 16
#define HSEC_LINES 96
#define HSEC_LEN 140
typedef struct { char id[48]; int n; char line[HSEC_LINES][HSEC_LEN]; } hsec;
static hsec *hs[HSEC_MAX];
static int hs_n;

void ps2_hotas_ini_reset(void) {
    for (int i = 0; i < hs_n; i++) free(hs[i]);
    hs_n = 0;
}
static hsec *sec_get(const char *id, int create) {
    for (int i = 0; i < hs_n; i++) if (!strcmp(hs[i]->id, id)) return hs[i];
    if (!create || hs_n >= HSEC_MAX) return NULL;
    hsec *s = (hsec *)calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->id, sizeof s->id, "%s", id);
    hs[hs_n++] = s;
    return s;
}
void ps2_hotas_ini_kv(const char *id, const char *key, const char *val) {
    hsec *s = sec_get(id, 1);
    if (s && s->n < HSEC_LINES) snprintf(s->line[s->n++], HSEC_LEN, "%s = %s", key, val);
}
void ps2_hotas_ini_write(FILE *f) {
    for (int i = 0; i < hs_n; i++) {
        fprintf(f, "\n[joystick:%s]\n", hs[i]->id);
        for (int k = 0; k < hs[i]->n; k++) fprintf(f, "%s\n", hs[i]->line[k]);
    }
}

/* ---- bindings as text ---- */
static const char *const hat_dir_name[4] = { "up", "right", "down", "left" };
static int act_of_key(const char *k) {
    if (!strcmp(k, "l1")) return HA_BRAKE;
    if (!strcmp(k, "r1")) return HA_ACCEL;
    for (int a = 0; a < HA_COUNT; a++) if (ha[a].key && !strcmp(ha[a].key, k)) return a;
    return -1;
}
static int spec_parse(const char *s, int kind) {     /* one token -> a local code, 0 if it is none */
    char *e;
    if (!strncmp(s, "axis", 4) && s[4] >= '0' && s[4] <= '9') {
        long a = strtol(s + 4, &e, 10);
        if (a < 0 || a > 63 || kind == HK_HAT) return 0;
        return PS2_JOYBIND_AXIS | (int)a << 1 | (*e == '-' ? 0 : 1);
    }
    if (!strncmp(s, "button", 6) && s[6] >= '0' && s[6] <= '9') {
        long b = strtol(s + 6, &e, 10);
        if (b < 0 || b >= HJ_BTN_MAX || kind != HK_BUTTON) return 0;
        return PS2_JOYBIND_BUTTON | (int)b;
    }
    if (!strncmp(s, "hat", 3) && s[3] >= '0' && s[3] <= '9') {
        long h = strtol(s + 3, &e, 10);
        if (h < 0 || h > 7 || kind == HK_AXIS) return 0;
        if (kind == HK_HAT) return PS2_JOYBIND_HAT | (int)h << 2;
        if (*e != ':') return 0;
        for (int d = 0; d < 4; d++) if (!strcmp(e + 1, hat_dir_name[d])) return PS2_JOYBIND_HAT | (int)h << 2 | d;
    }
    return 0;
}
static void code_text(int code, int kind, char *out, size_t n) {
    if (code & PS2_JOYBIND_AXIS) {
        int a = (code >> 1) & 63, dir = code & 1;
        snprintf(out, n, "axis%d%s", a, kind == HK_AXIS ? (dir ? "" : "-") : (dir ? "+" : "-"));
    } else if (code & PS2_JOYBIND_BUTTON) snprintf(out, n, "button%d", code & 0xFF);
    else if (kind == HK_HAT) snprintf(out, n, "hat%d", (code >> 2) & 7);
    else snprintf(out, n, "hat%d:%s", (code >> 2) & 7, hat_dir_name[code & 3]);
}

/* ---- profiles ---- */
static void prof_default(hprof *p) {
    memset(p, 0, sizeof *p);
    for (int a = 0; a < HJ_AX; a++) { p->ax[a].dz = -1.0f; p->ax[a].curve = 1.0f; p->ax[a].neutral = 0.5f; }
}
static void axis_parse(hax *x, const char *v) {
    while (*v) {
        char k[16]; size_t n = 0;
        while (*v == ' ' || *v == '\t') v++;
        while (*v && *v != ':' && *v != ' ' && n + 1 < sizeof k) k[n++] = *v++;
        k[n] = 0;
        if (*v != ':') { while (*v && *v != ' ') v++; continue; }
        char *e;
        float f = strtof(v + 1, &e);
        v = e;
        if (!strcmp(k, "invert")) x->invert = f != 0.0f;
        else if (!strcmp(k, "slider")) x->slider = f != 0.0f;
        else if (!strcmp(k, "dz")) x->dz = f < 0.0f ? 0.0f : f > 0.9f ? 0.9f : f;
        else if (!strcmp(k, "curve")) x->curve = f < 0.2f ? 0.2f : f > 5.0f ? 5.0f : f;
        else if (!strcmp(k, "neutral")) x->neutral = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
        x->set = 1;
    }
}
static void prof_kv(hprof *p, const char *key, const char *val) {
    if (!strcmp(key, "name")) snprintf(p->name, sizeof p->name, "%s", val);
    else if (!strcmp(key, "status")) snprintf(p->status, sizeof p->status, "%s", val);
    else if (!strcmp(key, "highg_rule")) p->highg_rule = atoi(val) != 0;
    else if (!strcmp(key, "type")) p->kind = !strcmp(val, "stick") ? HT_STICK : !strcmp(val, "throttle") ? HT_THROTTLE
                                             : !strcmp(val, "pedals") ? HT_PEDALS : HT_ANY;
    else if (!strncmp(key, "axis", 4)) {
        int a = atoi(key + 4);
        if (a >= 0 && a < HJ_AX) axis_parse(&p->ax[a], val);
    } else if (!strncmp(key, "bind.", 5)) {
        int act = act_of_key(key + 5), slot = 0;
        if (act < 0) { ps2_log("joy: profile %s: unknown action '%s'", p->src, key + 5); return; }
        char buf[64], *save = NULL;
        snprintf(buf, sizeof buf, "%s", val);
        for (char *t = SDL_strtok_r(buf, " \t", &save); t && slot < PS2_BIND_SLOTS; t = SDL_strtok_r(NULL, " \t", &save)) {
            int c = spec_parse(t, ha[act].kind);
            if (c) p->bind[act][slot++] = c;
            else ps2_log("joy: profile %s: bind.%s = %s: not usable for this action", p->src, key + 5, t);
        }
    }
}
static char *trim_(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}
static const char *profile_dir(void) {
    static char dir[512]; static int done;
    if (done) return dir[0] ? dir : NULL;
    done = 1;
    const char *e = getenv("ACZ_HOTAS_PROFILES");
    SDL_PathInfo pi;
    if (e && *e) { snprintf(dir, sizeof dir, "%s", e); return dir; }
    if (SDL_GetPathInfo("profiles/joystick", &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) {
        snprintf(dir, sizeof dir, "profiles/joystick");
        return dir;
    }
    const char *b = SDL_GetBasePath();
    if (b) {
        snprintf(dir, sizeof dir, "%s../../profiles/joystick", b);
        if (SDL_GetPathInfo(dir, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY) return dir;
    }
    dir[0] = 0;
    return NULL;
}
/* a shipped file's match keys; returns 2 VID:PID, 1 name, 0 no; every key = value goes to p when p is given */
static int file_read(const char *path, Uint16 vid, Uint16 pid, const char *name, hprof *p, char *hints, size_t nh, char *hintt, size_t nt) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256], mt[128] = "", mn[64] = "", ty[16] = "";
    int r = 0;
    while (fgets(line, sizeof line, f)) {
        char *c = strpbrk(line, ";#");
        if (c) *c = 0;
        char *s = trim_(line), *eq = strchr(s, '=');
        if (!*s || *s == '[' || !eq) continue;
        *eq = 0;
        char *k = trim_(s), *v = trim_(eq + 1);
        if (!strcmp(k, "match")) snprintf(mt, sizeof mt, "%s", v);
        else if (!strcmp(k, "match_name")) snprintf(mn, sizeof mn, "%s", v);
        else if (!strcmp(k, "type")) snprintf(ty, sizeof ty, "%s", v);
        if (p) prof_kv(p, k, v);
    }
    fclose(f);
    char m[128], *save = NULL;
    snprintf(m, sizeof m, "%s", mt);
    for (char *t = SDL_strtok_r(m, ", ", &save); t; t = SDL_strtok_r(NULL, ", ", &save)) {
        unsigned a = 0, b = 0;
        if (sscanf(t, "%x:%x", &a, &b) != 2) continue;
        if (a == vid && b == pid) r = 2;
        if (hints && !strcmp(ty, "stick")) {
            size_t l = strlen(hints);
            if (l + 16 < nh) snprintf(hints + l, nh - l, "%s0x%04x/0x%04x", l ? "," : "", a, b);
        }
        if (hintt && !strcmp(ty, "throttle")) {
            size_t l = strlen(hintt);
            if (l + 16 < nt) snprintf(hintt + l, nt - l, "%s0x%04x/0x%04x", l ? "," : "", a, b);
        }
    }
    if (!r && name && mn[0] && SDL_strcasestr(name, mn)) r = 1;
    return r;
}
static void prof_rebuild(hdev *d) {
    d->nbl = 0;
    for (int a = 0; a < HA_COUNT; a++)
        for (int k = 0; k < PS2_BIND_SLOTS; k++)
            if (d->p.bind[a][k]) { d->bl[d->nbl].act = (short)a; d->bl[d->nbl].k = (short)k; d->bl[d->nbl++].code = d->p.bind[a][k]; }
}
static void prof_resolve(hdev *d) {
    prof_default(&d->p);
    int nth = 1;
    for (int i = 0; i < HJ_SLOTS; i++) if (&hd[i] != d && hd[i].on && !strcmp(hd[i].guid, d->guid)) nth++;
    if (nth > 1) snprintf(d->secid, sizeof d->secid, "%s:%d", d->guid, nth);
    else snprintf(d->secid, sizeof d->secid, "%s", d->guid);
    hsec *s = sec_get(d->secid, 0);
    if (!s) s = sec_get(d->guid, 0);
    if (s) {
        snprintf(d->p.src, sizeof d->p.src, "settings file [joystick:%s]", s->id);
        for (int i = 0; i < s->n; i++) {
            char line[HSEC_LEN];
            snprintf(line, sizeof line, "%s", s->line[i]);
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            prof_kv(&d->p, trim_(line), trim_(eq + 1));
        }
    } else {
        const char *dir = profile_dir();
        int cnt = 0, best = 0;
        char bestf[256] = "";
        char **fl = dir ? SDL_GlobDirectory(dir, "*.ini", 0, &cnt) : NULL;
        for (int i = 0; fl && i < cnt; i++) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, fl[i]);
            int r = file_read(path, d->vid, d->pid, d->name, NULL, NULL, 0, NULL, 0);
            if (r > best) { best = r; snprintf(bestf, sizeof bestf, "%s", fl[i]); }
        }
        SDL_free(fl);
        if (best) {
            char path[512];
            snprintf(path, sizeof path, "%s/%s", dir, bestf);
            snprintf(d->p.src, sizeof d->p.src, "%s (by %s)", bestf, best == 2 ? "VID:PID" : "name");
            file_read(path, d->vid, d->pid, d->name, &d->p, NULL, 0, NULL, 0);
        }
    }
    prof_rebuild(d);
}
/* the player changed something: the device's section now holds its whole profile */
static void prof_sync(hdev *d) {
    hsec *s = sec_get(d->secid, 1);
    char v[96], t[32];
    if (!s) return;
    s->n = 0;
    ps2_hotas_ini_kv(s->id, "name", d->p.name[0] ? d->p.name : d->name);
    if (d->p.kind) ps2_hotas_ini_kv(s->id, "type", d->p.kind == HT_STICK ? "stick" : d->p.kind == HT_THROTTLE ? "throttle" : "pedals");
    if (d->p.status[0]) ps2_hotas_ini_kv(s->id, "status", d->p.status);
    if (d->p.highg_rule) ps2_hotas_ini_kv(s->id, "highg_rule", "1");
    for (int a = 0; a < HJ_AX; a++) {
        const hax *x = &d->p.ax[a];
        if (!x->set) continue;
        snprintf(t, sizeof t, "axis%d", a);
        if (x->dz >= 0.0f) snprintf(v, sizeof v, "invert:%d dz:%.3f curve:%.2f slider:%d neutral:%.3f", x->invert, (double)x->dz, (double)x->curve, x->slider, (double)x->neutral);
        else snprintf(v, sizeof v, "invert:%d curve:%.2f slider:%d neutral:%.3f", x->invert, (double)x->curve, x->slider, (double)x->neutral);
        ps2_hotas_ini_kv(s->id, t, v);
    }
    for (int a = 0; a < HA_COUNT; a++) {
        if (!d->p.bind[a][0] && !d->p.bind[a][1]) continue;
        char k[32], c0[24] = "", c1[24] = "";
        snprintf(k, sizeof k, "bind.%s", ha[a].key);
        if (d->p.bind[a][0]) code_text(d->p.bind[a][0], ha[a].kind, c0, sizeof c0);
        if (d->p.bind[a][1]) code_text(d->p.bind[a][1], ha[a].kind, c1, sizeof c1);
        snprintf(v, sizeof v, "%s%s%s", c0, c0[0] && c1[0] ? " " : "", c1);
        ps2_hotas_ini_kv(s->id, k, v);
    }
    snprintf(d->p.src, sizeof d->p.src, "settings file [joystick:%s]", s->id);
}

/* ---- the switches ---- */
enum { M_CENTRED, M_SPRING, M_SPLIT, M_ABSOLUTE };
static int hj_mode;
static const char *const mode_name[4] = { "CENTRED", "SPRING", "SPLIT", "ABSOLUTE" };
static int hj_stick_game, hj_remap, hj_dbg;
static float hj_game_dz = 64.0f, hj_ab_gate, hj_ffb;
static int hj_lever = -1;                            /* the slot whose lever counts */
static void mode_set(const char *v) {
    int m = !v || !*v || !SDL_strcasecmp(v, "centred") || !SDL_strcasecmp(v, "centered") ? M_CENTRED
          : !SDL_strcasecmp(v, "spring") ? M_SPRING : !SDL_strcasecmp(v, "split") ? M_SPLIT
          : !SDL_strcasecmp(v, "absolute") ? M_ABSOLUTE : -1;
    if (m < 0) { ps2_log("joy: ACZ_HOTAS_THROTTLE=%s not understood (centred, spring, split, absolute)", v); return; }
    if (m != hj_mode) ps2_log("joy: throttle lever mode %s", mode_name[m]);
    hj_mode = m;
}
void ps2_hotas_set_throttle(const char *mode) { mode_set(mode); }

static int hj_off_now(void) {
    if (hj_off < 0) { const char *e = getenv("ACZ_HOTAS"); hj_off = e && *e == '0'; }
    return hj_off;
}

/* ---- PS2_PAD_JOY="dev:<vid>:<pid>:<name>:<axes>:<buttons>:<hats>[:rumble]; field:devN:axisK:value; ..." (test hook):
   SDL virtual joysticks with real VID/PIDs, attached at start (dev@F1-F2: at field F1, detached at F2) in the order they
   are declared, driven by entries separated by ';':
     F:devN:axisK:V (raw -32768..32767) | F1-F2:devN:axisK:A..B (a ramp) | F:devN:buttonK:0/1 | F:devN:hatK:up/right/down/
     left/center or a mask | F:capture:<action>[:slot] (the F4 popup's capture, no window) | F:f4:<action> (the window's
     popup) | F:neutral (SET LEVER NEUTRAL) | F:mode:<centred|spring|split|absolute> | noreal (real joysticks stay out).
   ":rumble" gives the device SDL's rumble capability and counts what it receives (ACZ_FFB's routing). "@file": the script is
   in that file. */
#define JV_MAX 24
#define JE_MAX 4096
enum { JE_AXIS, JE_RAMP, JE_BTN, JE_HAT, JE_CAP, JE_F4, JE_NEUTRAL, JE_MODE };
static struct {
    int decl, on, gone, naxes, nbtn, nhat, rumble;
    Uint16 vid, pid;
    char name[64];
    u64 at, off;
    SDL_JoystickID id;
    SDL_Joystick *j;
    volatile u32 rumbles;
    volatile Uint16 lo, hi;
} jv[JV_MAX];
static struct { u64 f0, f1; short dev, idx, kind, done; int a, b; char txt[16]; } je[JE_MAX];
static int jv_n, je_n, jv_noreal;

static int jv_of(SDL_JoystickID id) {
    for (int n = 0; n < jv_n; n++) if (jv[n].on && jv[n].id == id) return n;
    return -1;
}
static bool SDLCALL jv_rumble_cb(void *ud, Uint16 lo, Uint16 hi) {
    int n = (int)(intptr_t)ud;
    jv[n].rumbles++; jv[n].lo = lo; jv[n].hi = hi;
    return true;
}
static void hj_add(SDL_JoystickID id, const char *why);
static void hj_remove(SDL_JoystickID id);
static void jv_attach(int n) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_UNKNOWN;              /* (SDL's VID/PID lists and our hints decide) */
    d.vendor_id = jv[n].vid;
    d.product_id = jv[n].pid;
    d.naxes = (Uint16)jv[n].naxes;
    d.nbuttons = (Uint16)jv[n].nbtn;
    d.nhats = (Uint16)jv[n].nhat;
    d.name = jv[n].name;
    d.userdata = (void *)(intptr_t)n;
    if (jv[n].rumble) d.Rumble = jv_rumble_cb;
    jv[n].id = SDL_AttachVirtualJoystick(&d);
    jv[n].j = jv[n].id ? SDL_OpenJoystick(jv[n].id) : NULL;
    jv[n].on = jv[n].j != NULL;
    jv[n].gone = !jv[n].on;                          /* (no second try) */
    ps2_log("joyt dev%d: %s %04x:%04x, %d axes %d buttons %d hats attached at field %llu: %s", n, jv[n].name, jv[n].vid, jv[n].pid,
            jv[n].naxes, jv[n].nbtn, jv[n].nhat, (unsigned long long)ps2_kernel_vblank_count(), jv[n].on ? "ok" : SDL_GetError());
    if (jv[n].on) hj_add(jv[n].id, "plugged");
}
static void jv_detach(int n) {
    if (!jv[n].on) return;
    ps2_log("joyt dev%d: detached at field %llu", n, (unsigned long long)ps2_kernel_vblank_count());
    SDL_JoystickID id = jv[n].id;
    hj_remove(id);
    SDL_CloseJoystick(jv[n].j);
    SDL_DetachVirtualJoystick(id);
    jv[n].on = 0; jv[n].gone = 1; jv[n].j = NULL;
}
static void jv_parse(void) {
    const char *e = getenv("PS2_PAD_JOY");
    if (!e || !*e) return;
    char *buf = NULL, *save = NULL;
    if (*e == '@') {                                 /* @file: long scripts (the environment cuts a value at about 8 KB) */
        buf = (char *)SDL_LoadFile(e + 1, NULL);
        if (!buf) { ps2_log("joyt: PS2_PAD_JOY: cannot read %s", e + 1); return; }
    } else buf = SDL_strdup(e);
    for (char *s = SDL_strtok_r(buf, ";", &save); s; s = SDL_strtok_r(NULL, ";", &save)) {
        s = trim_(s);
        if (!*s) continue;
        if (!strcmp(s, "noreal")) { jv_noreal = 1; continue; }
        if (!strncmp(s, "dev", 3) && (s[3] == ':' || s[3] == '@' || s[3] == '-')) {
            char *p = s + 3;
            if (jv_n >= JV_MAX) continue;
            u64 at = 0, off = 0;
            if (*p == '@') at = strtoull(p + 1, &p, 0);
            if (*p == '-') off = strtoull(p + 1, &p, 0);
            unsigned vid = 0, pid = 0; char name[64] = ""; int na = 0, nb = 0, nh = 0;
            if (*p != ':' || sscanf(p + 1, "%x:%x:%63[^:]:%d:%d:%d", &vid, &pid, name, &na, &nb, &nh) != 6) {
                ps2_log("joyt: PS2_PAD_JOY: bad device '%s'", s);
                continue;
            }
            jv[jv_n].decl = 1; jv[jv_n].vid = (Uint16)vid; jv[jv_n].pid = (Uint16)pid;
            snprintf(jv[jv_n].name, sizeof jv[jv_n].name, "%s", name);
            jv[jv_n].naxes = na; jv[jv_n].nbtn = nb; jv[jv_n].nhat = nh; jv[jv_n].at = at; jv[jv_n].off = off;
            jv[jv_n].rumble = strstr(p, ":rumble") != NULL;
            jv_n++;
            continue;
        }
        if (je_n >= JE_MAX) continue;
        char *p;
        u64 f0 = strtoull(s, &p, 0), f1 = 0;
        if (*p == '-') f1 = strtoull(p + 1, &p, 0);
        if (*p != ':') { ps2_log("joyt: PS2_PAD_JOY: bad entry '%s'", s); continue; }
        p++;
        je[je_n].f0 = f0; je[je_n].f1 = f1; je[je_n].done = 0; je[je_n].dev = -1;
        if (!strncmp(p, "dev", 3)) {
            int dv = atoi(p + 3);
            char *q = strchr(p, ':');
            if (!q || dv < 0 || dv >= JV_MAX) { ps2_log("joyt: PS2_PAD_JOY: bad entry '%s'", s); continue; }
            q++;
            je[je_n].dev = (short)dv;
            if (!strncmp(q, "axis", 4)) {
                je[je_n].idx = (short)atoi(q + 4);
                q = strchr(q, ':');
                if (!q) continue;
                char *r;
                je[je_n].a = (int)strtol(q + 1, &r, 0);
                je[je_n].kind = JE_AXIS;
                if (r[0] == '.' && r[1] == '.') { je[je_n].b = (int)strtol(r + 2, NULL, 0); je[je_n].kind = JE_RAMP; }
            } else if (!strncmp(q, "button", 6)) {
                je[je_n].idx = (short)atoi(q + 6);
                q = strchr(q, ':');
                if (!q) continue;
                je[je_n].a = atoi(q + 1); je[je_n].kind = JE_BTN;
            } else if (!strncmp(q, "hat", 3)) {
                je[je_n].idx = (short)atoi(q + 3);
                q = strchr(q, ':');
                if (!q) continue;
                q++;
                je[je_n].kind = JE_HAT;
                je[je_n].a = !strcmp(q, "up") ? SDL_HAT_UP : !strcmp(q, "right") ? SDL_HAT_RIGHT : !strcmp(q, "down") ? SDL_HAT_DOWN
                           : !strcmp(q, "left") ? SDL_HAT_LEFT : !strcmp(q, "center") ? SDL_HAT_CENTERED : (unsigned)atoi(q);
            } else { ps2_log("joyt: PS2_PAD_JOY: bad entry '%s'", s); continue; }
        } else if (!strncmp(p, "capture:", 8) || !strncmp(p, "f4:", 3)) {
            je[je_n].kind = p[0] == 'c' ? JE_CAP : JE_F4;
            char *q = strchr(p, ':') + 1, *r = strchr(q, ':');
            if (r) { *r = 0; je[je_n].b = atoi(r + 1); } else je[je_n].b = 0;
            je[je_n].a = act_of_key(q);
            if (je[je_n].a < 0) { ps2_log("joyt: PS2_PAD_JOY: unknown action '%s'", q); continue; }
        } else if (!strcmp(p, "neutral")) je[je_n].kind = JE_NEUTRAL;
        else if (!strncmp(p, "mode:", 5)) { je[je_n].kind = JE_MODE; snprintf(je[je_n].txt, sizeof je[je_n].txt, "%s", p + 5); }
        else { ps2_log("joyt: PS2_PAD_JOY: bad entry '%s'", s); continue; }
        je_n++;
    }
    SDL_free(buf);
    ps2_log("joyt: PS2_PAD_JOY: %d test devices, %d scripted changes%s", jv_n, je_n, jv_noreal ? "; real joysticks stay out" : "");
}
static int row_of_act(int act) {
    for (int r = 0; r < HROWS; r++) if (hrow[r].act == act) return r;
    return 0;
}
static void jv_service(u64 now) {
    for (int n = 0; n < jv_n; n++) {
        if (!jv[n].on && !jv[n].gone && now >= jv[n].at) jv_attach(n);
        if (jv[n].on && jv[n].off && now >= jv[n].off) jv_detach(n);
    }
    for (int i = 0; i < je_n; i++) {
        if (je[i].done || je[i].f0 > now) continue;
        int n = je[i].dev;
        if (n >= 0 && (n >= jv_n || !jv[n].on)) continue;
        switch (je[i].kind) {
        case JE_AXIS: SDL_SetJoystickVirtualAxis(jv[n].j, je[i].idx, (Sint16)je[i].a); je[i].done = 1; break;
        case JE_RAMP: {
            double t = je[i].f1 > je[i].f0 ? (double)(now - je[i].f0) / (double)(je[i].f1 - je[i].f0) : 1.0;
            if (t >= 1.0) { t = 1.0; je[i].done = 1; }
            SDL_SetJoystickVirtualAxis(jv[n].j, je[i].idx, (Sint16)lround(je[i].a + (je[i].b - je[i].a) * t));
            break; }
        case JE_BTN: SDL_SetJoystickVirtualButton(jv[n].j, je[i].idx, je[i].a != 0); je[i].done = 1; break;
        case JE_HAT: SDL_SetJoystickVirtualHat(jv[n].j, je[i].idx, (Uint8)je[i].a); je[i].done = 1; break;
        case JE_CAP: ps2_hotas_capture_begin(row_of_act(je[i].a), je[i].b); je[i].done = 1; break;
        case JE_F4: ps2_ui_hotas_test(row_of_act(je[i].a), je[i].b); je[i].done = 1; break;
        case JE_NEUTRAL: ps2_hotas_neutral_now(); je[i].done = 1; break;
        case JE_MODE: mode_set(je[i].txt); je[i].done = 1; break;
        }
    }
}

/* ---- the registry ---- */
static const char *jtype_name(SDL_JoystickType t) {
    switch (t) {
    case SDL_JOYSTICK_TYPE_GAMEPAD: return "gamepad";
    case SDL_JOYSTICK_TYPE_WHEEL: return "wheel";
    case SDL_JOYSTICK_TYPE_ARCADE_STICK: return "arcade-stick";
    case SDL_JOYSTICK_TYPE_FLIGHT_STICK: return "flight-stick";
    case SDL_JOYSTICK_TYPE_DANCE_PAD: return "dance-pad";
    case SDL_JOYSTICK_TYPE_GUITAR: return "guitar";
    case SDL_JOYSTICK_TYPE_DRUM_KIT: return "drum-kit";
    case SDL_JOYSTICK_TYPE_ARCADE_PAD: return "arcade-pad";
    case SDL_JOYSTICK_TYPE_THROTTLE: return "throttle";
    default: return "unknown";
    }
}
static int hj_find(SDL_JoystickID id) {
    for (int i = 0; i < HJ_SLOTS; i++) if (hd[i].on && hd[i].id == id) return i;
    return -1;
}
int ps2_hotas_claims(SDL_JoystickID id) {
    if (hj_off_now()) return 0;
    SDL_JoystickType t = SDL_GetJoystickTypeForID(id);
    return t == SDL_JOYSTICK_TYPE_FLIGHT_STICK || t == SDL_JOYSTICK_TYPE_THROTTLE;
}
static void lever_pick(void) {
    int best = -1, bestk = -1;
    for (int i = 0; i < HJ_SLOTS; i++) {
        if (!hd[i].on || (!hd[i].p.bind[HA_THROTTLE][0] && !hd[i].p.bind[HA_THROTTLE][1])) continue;
        int k = hd[i].p.kind == HT_THROTTLE || hd[i].type == SDL_JOYSTICK_TYPE_THROTTLE;
        if (k > bestk) { best = i; bestk = k; }
    }
    if (best != hj_lever) {
        hj_lever = best;
        if (best >= 0) ps2_log("joy: the throttle lever is %s's", hd[best].name);
    }
}
static void ffb_open(hdev *d) {
    d->eff = -1;
    if (hj_ffb <= 0.0f) return;
    if (SDL_IsJoystickHaptic(d->j)) {
        d->hap = SDL_OpenHapticFromJoystick(d->j);
        if (d->hap) {
            Uint32 fe = SDL_GetHapticFeatures(d->hap);
            memset(&d->effd, 0, sizeof d->effd);
            if (fe & SDL_HAPTIC_SINE) {
                d->effd.type = SDL_HAPTIC_SINE;
                d->effd.periodic.direction.type = SDL_HAPTIC_CARTESIAN;
                d->effd.periodic.direction.dir[0] = 1;
                d->effd.periodic.period = 33;
                d->effd.periodic.length = SDL_HAPTIC_INFINITY;
            } else if (fe & SDL_HAPTIC_CONSTANT) {
                d->effd.type = SDL_HAPTIC_CONSTANT;
                d->effd.constant.direction.type = SDL_HAPTIC_CARTESIAN;
                d->effd.constant.direction.dir[0] = 1;
                d->effd.constant.length = SDL_HAPTIC_INFINITY;
            }
            d->eff = d->effd.type ? SDL_CreateHapticEffect(d->hap, &d->effd) : -1;
            if (d->eff >= 0) SDL_RunHapticEffect(d->hap, d->eff, SDL_HAPTIC_INFINITY);
            ps2_log("joy: FFB on %s: %s, ACZ_FFB %.2f: %s", d->name, d->effd.type == SDL_HAPTIC_SINE ? "sine effect"
                    : d->effd.type == SDL_HAPTIC_CONSTANT ? "constant force" : "no usable effect", (double)hj_ffb,
                    d->eff >= 0 ? "running" : SDL_GetError());
            return;
        }
        ps2_log("joy: FFB on %s: could not be opened: %s", d->name, SDL_GetError());
    }
    d->rumble_cap = SDL_GetBooleanProperty(SDL_GetJoystickProperties(d->j), SDL_PROP_JOYSTICK_CAP_RUMBLE_BOOLEAN, false);
    ps2_log("joy: FFB on %s: %s", d->name, d->rumble_cap ? "no haptic device, SDL rumble" : "none (no haptic device, no rumble)");
}
static void ffb_close(hdev *d) {                     /* (the haptic device goes before the joystick) */
    if (d->hap) {
        if (d->eff >= 0) SDL_DestroyHapticEffect(d->hap, d->eff);
        SDL_CloseHaptic(d->hap);
        d->hap = NULL;
    }
    if (d->rumble_cap) SDL_RumbleJoystick(d->j, 0, 0, 0);
}
static void hj_add(SDL_JoystickID id, const char *why) {
    if (hj_off_now() || hj_find(id) >= 0) return;
    SDL_JoystickType t = SDL_GetJoystickTypeForID(id);
    if (!(t == SDL_JOYSTICK_TYPE_FLIGHT_STICK || t == SDL_JOYSTICK_TYPE_THROTTLE || !SDL_IsGamepad(id))) return;
    if (jv_noreal && jv_of(id) < 0) return;
    int s = 0;
    while (s < HJ_SLOTS && hd[s].on) s++;
    if (s == HJ_SLOTS) { ps2_log("joy: more than %d joysticks: %s stays out", HJ_SLOTS, SDL_GetJoystickNameForID(id)); return; }
    SDL_Joystick *j = SDL_OpenJoystick(id);
    if (!j) { ps2_log("joy: joystick %u could not be opened: %s", (unsigned)id, SDL_GetError()); return; }
    hdev *d = &hd[s];
    memset(d, 0, sizeof *d);
    d->id = id; d->j = j; d->type = t;
    const char *nm = SDL_GetJoystickName(j);
    snprintf(d->name, sizeof d->name, "%s", nm && *nm ? nm : "Joystick");
    SDL_GUIDToString(SDL_GetJoystickGUID(j), d->guid, sizeof d->guid);
    SDL_GetJoystickGUIDInfo(SDL_GetJoystickGUID(j), &d->vid, &d->pid, &d->ver, &d->crc);
    d->naxes = SDL_GetNumJoystickAxes(j); d->nbtn = SDL_GetNumJoystickButtons(j); d->nhat = SDL_GetNumJoystickHats(j);
    d->on = 1;
    hj_n++;
    d->unknown = d->naxes > HJ_AX ? HJ_AX : d->naxes;
    prof_resolve(d);
    ffb_open(d);
    (void)why;
    lever_pick();
    if (hj_n == 1) ps2_enh_hotas_rows(1);
}
static void sweep_log(hdev *d) {
    char b[900]; int l = 0;
    d->sweep_dirty = 0;
    l += snprintf(b + l, sizeof b - (size_t)l, "joy sweep: %s GUID %s: axes", d->name, d->guid);
    for (int a = 0; a < HJ_AX && a < d->naxes && l < 700; a++)
        if (d->aseen[a]) l += snprintf(b + l, sizeof b - (size_t)l, " %d:%d..%d", a, d->amin[a], d->amax[a]);
    l += snprintf(b + l, sizeof b - (size_t)l, "; buttons");
    for (int k = 0; k < HJ_BTN_MAX && l < 860; k++)
        if (d->bseen[k / 32] >> (k % 32) & 1u) l += snprintf(b + l, sizeof b - (size_t)l, " %d", k);
    l += snprintf(b + l, sizeof b - (size_t)l, "; hats");
    for (int k = 0; k < 32 && l < 890; k++)
        if (d->hseen >> k & 1u) l += snprintf(b + l, sizeof b - (size_t)l, " %d", k);
    ps2_log("%s", b);
}
static void hj_remove(SDL_JoystickID id) {
    int s = hj_find(id);
    if (s < 0) return;
    hdev *d = &hd[s];
    if (d->sweep_dirty) sweep_log(d);
    ps2_log("joy: %s removed (slot %d)", d->name, s);
    ffb_close(d);
    SDL_CloseJoystick(d->j);
    d->on = 0;
    hj_n--;
    lever_pick();
    if (hj_n == 0) ps2_enh_hotas_rows(0);
}
int ps2_hotas_count(void) { return hj_n; }

void ps2_hotas_pre_init(void) {
    if (hj_off_now()) return;
    const char *dir = profile_dir();
    int cnt = 0;
    char hs_stick[1024] = "", hs_thr[1024] = "";
    char **fl = dir ? SDL_GlobDirectory(dir, "*.ini", 0, &cnt) : NULL;
    for (int i = 0; fl && i < cnt; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", dir, fl[i]);
        file_read(path, 0, 0, NULL, NULL, hs_stick, sizeof hs_stick, hs_thr, sizeof hs_thr);
    }
    SDL_free(fl);
    if (hs_stick[0]) SDL_SetHint(SDL_HINT_JOYSTICK_FLIGHTSTICK_DEVICES, hs_stick);
    if (hs_thr[0]) SDL_SetHint(SDL_HINT_JOYSTICK_THROTTLE_DEVICES, hs_thr);
    ps2_log("joy: %d shipped profiles in %s", cnt, dir ? dir : "(no profiles folder)");
}
void ps2_hotas_menv_reload(void) {               /* the NEXT MISSION tier (ps2_menv.h): the renderer's first frame after the edge */
    const int a = hj_remap;
    const char *e = ps2_menv("ACZ_PAD_STICK_REMAP"); hj_remap = e && *e == '1';
    ps2_menv_note("ACZ_PAD_STICK_REMAP (joystick)", a, hj_remap);
}
void ps2_hotas_init(void) {
    const char *e;
    ps2_enh_hotas_rows(0);
    if (hj_off_now()) { ps2_log("joy: ACZ_HOTAS=0: joysticks are not opened"); return; }
    e = getenv("ACZ_HOTAS_STICK"); hj_stick_game = e && !SDL_strcasecmp(e, "game");
    e = ps2_menv("ACZ_PAD_STICK_REMAP"); hj_remap = e && *e == '1';
    e = getenv("ACZ_TOUCH_GAME_DZ"); if (e && *e) hj_game_dz = (float)atof(e);
    e = getenv("ACZ_HOTAS_AB_GATE"); if (e && *e) hj_ab_gate = (float)atof(e);
    e = getenv("ACZ_DEBUG_HOTAS"); if (e && *e) { hj_dbg = atoi(e); if (hj_dbg < 1) hj_dbg = 0; }
    e = getenv("ACZ_FFB"); if (e && *e) hj_ffb = (float)atof(e);
    if (hj_ffb > 1.0f) hj_ffb = 1.0f;
    if (hj_ffb > 0.0f && !SDL_InitSubSystem(SDL_INIT_HAPTIC)) ps2_log("joy: ACZ_FFB: the haptic subsystem: %s", SDL_GetError());
    mode_set(getenv("ACZ_HOTAS_THROTTLE"));
    jv_parse();
    for (int n = 0; n < jv_n; n++) if (jv[n].at == 0) jv_attach(n);
    int cnt = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&cnt);
    for (int i = 0; ids && i < cnt; i++) hj_add(ids[i], NULL);
    SDL_free(ids);
    hj_ready = 1;
    if (!hj_n) ps2_log("joy: no joystick connected (hot-plug works)");
}
void ps2_hotas_shutdown(void) {
    for (int i = 0; i < HJ_SLOTS; i++) {
        if (!hd[i].on) continue;
        if (hd[i].sweep_dirty) sweep_log(&hd[i]);
        ffb_close(&hd[i]);
    }
}

/* ---- reading the axes ---- */
static const hax hax_none = { 0, 0, 0, -1.0f, 1.0f, 0.5f };
static inline const hax *ax_of(const hdev *d, int a) { return a < HJ_AX ? &d->p.ax[a] : &hax_none; }
static inline int ax_known(const hdev *d, int a) { return a >= HJ_AX || d->known[a]; }
static inline float ax_x(const hdev *d, int a) {     /* -1..1 after the inversion; 0 while its position is unknown */
    if (a >= d->naxes || !ax_known(d, a)) return 0.0f;
    int r = SDL_GetJoystickAxis(d->j, a);
    float v = (float)r / (r < 0 ? 32768.0f : 32767.0f);
    return ax_of(d, a)->invert ? -v : v;
}
static inline float shaped(const hax *x, float m) {  /* a magnitude 0..1: the dead zone, the curve */
    float dz = x->dz >= 0.0f ? x->dz : 0.05f;
    if (m <= dz) return 0.0f;
    m = (m - dz) / (1.0f - dz);
    if (m > 1.0f) m = 1.0f;
    return x->curve != 1.0f ? powf(m, x->curve) : m;
}
static inline float axis_signed(const hdev *d, int code) {
    int a = (code >> 1) & 63;
    float v = ax_x(d, a), m = shaped(ax_of(d, a), fabsf(v));
    if (!(code & 1)) v = -v;
    return v < 0.0f ? -m : m;
}
static inline float code_value(const hdev *d, int code) {   /* a button-type action: 0..1 */
    if (code & PS2_JOYBIND_AXIS) {
        int a = (code >> 1) & 63;
        const hax *x = ax_of(d, a);
        float v = ax_x(d, a);
        if (x->slider) return ax_known(d, a) && a < d->naxes ? shaped(x, (v + 1.0f) * 0.5f) : 0.0f;
        if ((code & 1) ? v <= 0.0f : v >= 0.0f) return 0.0f;
        return shaped(x, fabsf(v));
    }
    if (code & PS2_JOYBIND_BUTTON) {
        int b = code & 0xFF;
        return b < d->nbtn && SDL_GetJoystickButton(d->j, b) ? 1.0f : 0.0f;
    }
    int h = (code >> 2) & 7;
    static const Uint8 mask[4] = { SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT };
    return h < d->nhat && (SDL_GetJoystickHat(d->j, h) & mask[code & 3]) ? 1.0f : 0.0f;
}
/* a stick axis's magnitude -> the value that ps2_pad_stick and the game turn into it: from the first movement to full */
static inline float stick_out(float m) {
    if (m <= 0.0f || hj_stick_game) return m;
    float s = ps2_cfg.axis_scale > 0.01f ? ps2_cfg.axis_scale : 1.0f;
    if (hj_remap) return m / s;                      /* (ACZ_PAD_STICK_REMAP does the game's dead zone) */
    float dc = hj_game_dz + (127.0f - hj_game_dz) * m;
    return (2.0f * dc + 1.0f) / (255.0f * s);
}
#define PUT_MAX(a, x) do { float x_ = (x); if (x_ > v[a]) v[a] = x_; } while (0)

static float hj_dbg_u = -1.0f, hj_dbg_acc, hj_dbg_brk, hj_dbg_thr, hj_dbg_target;
/* ABSOLUTE (an experiment, ACZ_HOTAS_THROTTLE=absolute): the lever is a target for the flight model's power, 0..1, and L1 / R1
   pressure are steered onto it. The game's power follows thr within 4 fields and exactly: power = 0.4 + 0.66 thr, clamped to 0..1
   (sim, the same at 14 pressures), so the thr for a target is known; a slow trim takes any other aircraft's difference. (A fast term on
   the error oscillates: gain 2 on thr swung the power 0..1 at every lever position.) The top 3 % of the travel is the afterburner.
   Never the default: the PS2 game has no throttle setting, only pressure. */
static int hj_flying;
static void lever_absolute(float u, float *acc, float *brk) {
    static float trim;
    static Uint64 last;
    zero_instr in;
    *acc = *brk = 0.0f;
    if (!ps2_vk_instr_get || !ps2_vk_instr_get(&in) || !in.valid) return;
    Uint64 now = SDL_GetTicksNS();
    float dt = last ? (float)(now - last) * 1e-9f : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    float target = u > 0.97f ? 1.0f : u;
    if (hj_flying) trim += (target - in.throttle) * 0.5f * dt;
    trim = trim < -0.25f ? -0.25f : trim > 0.25f ? 0.25f : trim;
    float thr = (target - 0.4f) / 0.66f + trim;
    if (u <= 0.97f && thr > 0.97f) thr = 0.97f;      /* (below the gate: no afterburner, whatever the trim) */
    if (thr < -1.0f) thr = -1.0f;
    if (u > 0.97f) thr = 1.0f;
    hj_dbg_thr = thr; hj_dbg_target = target;
    if (thr >= 0.999f) *acc = 1.0f;
    else if (thr > 0.0f) *acc = thr * 144.6f / 255.0f;
    else if (thr < 0.0f) *brk = -thr * 216.8f / 255.0f;
}
static void lever_out(const hdev *d, int code, float *acc, float *brk) {
    int a = (code >> 1) & 63;
    const hax *x = ax_of(d, a);
    float n = hj_mode == M_SPRING ? 0.5f : x->neutral;
    float db = x->dz >= 0.0f ? x->dz : hj_mode == M_SPRING ? 0.05f : 0.08f;
    float u = n;
    if (ax_known(d, a) && a < d->naxes) {
        float t = ax_x(d, a);
        if (!(code & 1)) t = -t;
        u = (t + 1.0f) * 0.5f;
    }
    hj_dbg_u = u;
    if (hj_mode == M_ABSOLUTE) { lever_absolute(u, acc, brk); return; }
    float A = n + db < 1.0f ? (u - n - db) / (1.0f - n - db) : 0.0f, B = n - db > 0.0f ? (n - db - u) / (n - db) : 0.0f;
    A = A < 0.0f ? 0.0f : A > 1.0f ? 1.0f : A;
    B = B < 0.0f ? 0.0f : B > 1.0f ? 1.0f : B;
    if (x->curve != 1.0f) { if (A > 0.0f) A = powf(A, x->curve); if (B > 0.0f) B = powf(B, x->curve); }
    if (hj_ab_gate > 0.0f && A > 0.0f) A = A >= hj_ab_gate ? 1.0f : A / hj_ab_gate * 0.54f;
    *acc = A; *brk = B;
}

static float hj_val[PS2_ACT_COUNT];
static u64 hj_cost_ns, hj_cost_max, hj_cost_n;
static float axis_u(const hdev *d, int code) {       /* (the debug line, SPLIT: the half axis's position 0..1) */
    float t = ax_x(d, (code >> 1) & 63);
    return ((code & 1) ? t + 1.0f : 1.0f - t) * 0.5f;
}
void ps2_hotas_merge(float *value) {
    if (!hj_n) return;
    u64 t0 = SDL_GetPerformanceCounter();
    float v[PS2_ACT_COUNT], pull = 0.0f, lever_brk = 0.0f;
    int hg = 0, ap = 0, rule = 0;
    /* a lever rests anywhere: it is the throttle only in flight; in a menu or a pause it must not hold L1 or R1 down */
    int flying = ps2_imm_in[3] > 0.0f && (float)ps2_kernel_vblank_count() - ps2_imm_in[3] <= 8.0f;
    hj_flying = flying;
    memset(v, 0, sizeof v);
    hj_dbg_acc = hj_dbg_brk = 0.0f;
    for (int s = 0; s < HJ_SLOTS; s++) {
        const hdev *d = &hd[s];
        if (!d->on) continue;
        rule |= d->p.highg_rule;
        for (int i = 0; i < d->nbl; i++) {
            int act = d->bl[i].act, code = d->bl[i].code;
            float sv;
            switch (act) {
            case HA_PITCH:
                sv = axis_signed(d, code);
                if (sv > pull) pull = sv;
                if (sv > 0.0f) PUT_MAX(PS2_ACT_LS_DOWN, stick_out(sv)); else if (sv < 0.0f) PUT_MAX(PS2_ACT_LS_UP, stick_out(-sv));
                break;
            case HA_ROLL:
                sv = axis_signed(d, code);
                if (sv > 0.0f) PUT_MAX(PS2_ACT_LS_RIGHT, stick_out(sv)); else if (sv < 0.0f) PUT_MAX(PS2_ACT_LS_LEFT, stick_out(-sv));
                break;
            case HA_YAW:
                sv = axis_signed(d, code);
                if (sv > 0.0f) PUT_MAX(PS2_ACT_R2, sv); else if (sv < 0.0f) PUT_MAX(PS2_ACT_L2, -sv);
                break;
            case HA_LOOK_X:
                sv = axis_signed(d, code);
                if (sv > 0.0f) PUT_MAX(PS2_ACT_RS_RIGHT, sv); else if (sv < 0.0f) PUT_MAX(PS2_ACT_RS_LEFT, -sv);
                break;
            case HA_LOOK_Y:
                sv = axis_signed(d, code);
                if (sv > 0.0f) PUT_MAX(PS2_ACT_RS_DOWN, sv); else if (sv < 0.0f) PUT_MAX(PS2_ACT_RS_UP, -sv);
                break;
            case HA_THROTTLE:
                if (s == hj_lever && hj_mode != M_SPLIT) {
                    float acc, brk;
                    lever_out(d, code, &acc, &brk);
                    if (!flying) acc = brk = 0.0f;
                    PUT_MAX(PS2_ACT_R1, acc); PUT_MAX(PS2_ACT_L1, brk);
                    if (acc > hj_dbg_acc) hj_dbg_acc = acc;
                    if (brk > hj_dbg_brk) hj_dbg_brk = brk;
                    if (brk > lever_brk) lever_brk = brk;
                }
                break;
            case HA_ACCEL:
                if (!(code & PS2_JOYBIND_AXIS) || (hj_mode == M_SPLIT && flying)) {
                    sv = code_value(d, code);
                    PUT_MAX(PS2_ACT_R1, sv);
                    if (hj_dbg && (code & PS2_JOYBIND_AXIS) && sv > hj_dbg_acc) { hj_dbg_acc = sv; hj_dbg_u = axis_u(d, code); }
                }
                break;
            case HA_BRAKE:
                if (!(code & PS2_JOYBIND_AXIS) || (hj_mode == M_SPLIT && flying)) {
                    sv = code_value(d, code);
                    PUT_MAX(PS2_ACT_L1, sv);
                    if (sv > lever_brk) lever_brk = sv;
                    if (hj_dbg && (code & PS2_JOYBIND_AXIS) && sv > hj_dbg_brk) { hj_dbg_brk = sv; hj_dbg_u = axis_u(d, code); }
                }
                break;
            case HA_HIGHG: if (code_value(d, code) > 0.5f) hg = 1; break;
            case HA_AUTOPILOT: if (code_value(d, code) > 0.5f) ap = 1; break;
            case HA_DPAD: {
                int h = (code >> 2) & 7;
                Uint8 m = h < d->nhat ? SDL_GetJoystickHat(d->j, h) : 0;
                if (m & SDL_HAT_UP) v[PS2_ACT_UP] = 1.0f;
                if (m & SDL_HAT_RIGHT) v[PS2_ACT_RIGHT] = 1.0f;
                if (m & SDL_HAT_DOWN) v[PS2_ACT_DOWN] = 1.0f;
                if (m & SDL_HAT_LEFT) v[PS2_ACT_LEFT] = 1.0f;
                break; }
            default: if (act < PS2_ACT_COUNT) PUT_MAX(act, code_value(d, code)); break;
            }
        }
    }
    if (rule && lever_brk >= 0.95f && pull >= 0.5f) hg = 1;   /* a modern game's "type B": a profile's opt-in */
    if (hg) v[PS2_ACT_L1] = v[PS2_ACT_R1] = 1.0f;
    if (ap) v[PS2_ACT_L2] = v[PS2_ACT_R2] = 1.0f;
    for (int a = 0; a < PS2_ACT_COUNT; a++) {
        hj_val[a] = v[a];
        if (v[a] > value[a]) value[a] = v[a];
    }
    u64 dt = SDL_GetPerformanceCounter() - t0;
    hj_cost_ns += dt; hj_cost_n++;
    if (dt > hj_cost_max) hj_cost_max = dt;
}
int ps2_hotas_gamepad_button(int b) {
    if (!hj_n) return 0;
    int act = b == SDL_GAMEPAD_BUTTON_BACK ? PS2_ACT_SELECT : b == SDL_GAMEPAD_BUTTON_START ? PS2_ACT_START
            : b == SDL_GAMEPAD_BUTTON_SOUTH ? PS2_ACT_CROSS : b == SDL_GAMEPAD_BUTTON_EAST ? PS2_ACT_CIRCLE
            : b == SDL_GAMEPAD_BUTTON_WEST ? PS2_ACT_SQUARE : b == SDL_GAMEPAD_BUTTON_NORTH ? PS2_ACT_TRIANGLE : -1;
    return act >= 0 && hj_val[act] > 0.5f;
}

/* ---- capture (F4 > Controller > Joysticks; PS2_PAD_JOY's "capture") ---- */
static struct {
    int active, armed, act, slot;
    Uint64 t0;
    Sint16 base[HJ_SLOTS][HJ_AX];
    Uint8 hat[HJ_SLOTS][8];
} cap;
void ps2_hotas_capture_begin(int row, int slot) {
    if (row < 0 || row >= HROWS) return;
    memset(&cap, 0, sizeof cap);
    cap.active = 1; cap.act = hrow[row].act; cap.slot = slot ? 1 : 0; cap.t0 = SDL_GetTicks();
    ps2_log("joy: capture of %s (%s) started", ha[cap.act].label, cap.slot ? "secondary" : "primary");
}
int ps2_hotas_capture_active(void) { return cap.active; }
int ps2_hotas_capture_armed(void) { return cap.active && cap.armed; }
const char *ps2_hotas_capture_label(void) { return cap.active ? ha[cap.act].label : ""; }
const char *ps2_hotas_capture_prompt(void) {
    if (!cap.active) return "";
    if (ha[cap.act].kind == HK_HAT) return ha[cap.act].prompt;
    if (ha[cap.act].kind == HK_AXIS) return ha[cap.act].prompt;
    return "Press a button, push a hat, or move an axis through most of its travel.";
}
void ps2_hotas_capture_cancel(void) {
    if (cap.active) ps2_log("joy: capture cancelled");
    cap.active = 0;
}
static void bind_set(int act, int slot, int dev, int code) {
    for (int s = 0; s < HJ_SLOTS; s++) {
        if (!hd[s].on) continue;
        int ch = 0;
        if (hd[s].p.bind[act][slot]) { hd[s].p.bind[act][slot] = 0; ch = 1; }
        if (s == dev && (code & (PS2_JOYBIND_BUTTON | PS2_JOYBIND_HAT)))     /* (one button, one action; levers may share) */
            for (int a = 0; a < HA_COUNT; a++)
                for (int k = 0; k < PS2_BIND_SLOTS; k++)
                    if (hd[s].p.bind[a][k] == code) { hd[s].p.bind[a][k] = 0; ch = 1; }
        if (s == dev && code) { hd[s].p.bind[act][slot] = code; ch = 1; }
        if (ch) { prof_rebuild(&hd[s]); prof_sync(&hd[s]); }
    }
    lever_pick();
    ps2_settings_save();
}
void ps2_hotas_capture_clear(void) {
    if (!cap.active) return;
    bind_set(cap.act, cap.slot, -1, 0);
    ps2_log("joy: capture: %s (%s) cleared", ha[cap.act].label, cap.slot ? "secondary" : "primary");
    cap.active = 0;
}
void ps2_hotas_unbind(int row, int slot) {
    if (row >= 0 && row < HROWS) bind_set(hrow[row].act, slot ? 1 : 0, -1, 0);
}
static void cap_bound(int s, int code) {
    char t[32];
    code_text(code, ha[cap.act].kind, t, sizeof t);
    int inv = code & PS2_JOYBIND_AXIS ? hd[s].p.ax[((code >> 1) & 63) < HJ_AX ? (code >> 1) & 63 : 0].invert : 0;
    ps2_log("joy: capture: %s (%s) <- %s: %s%s", ha[cap.act].label, cap.slot ? "secondary" : "primary", hd[s].name, t,
            code & PS2_JOYBIND_AXIS && inv ? " (inverted)" : "");
    bind_set(cap.act, cap.slot, s, code);
    cap.active = 0;
}
static void cap_arm(void) {
    if (SDL_GetTicks() - cap.t0 < 150) return;
    for (int s = 0; s < HJ_SLOTS; s++) {
        hdev *d = &hd[s];
        if (!d->on) continue;
        for (int b = 0; b < d->nbtn; b++) if (SDL_GetJoystickButton(d->j, b)) return;
        for (int a = 0; a < d->naxes && a < HJ_AX; a++) cap.base[s][a] = SDL_GetJoystickAxis(d->j, a);
        for (int h = 0; h < d->nhat && h < 8; h++) cap.hat[s][h] = SDL_GetJoystickHat(d->j, h);
    }
    cap.armed = 1;
    ps2_log("joy: capture armed: %s", ps2_hotas_capture_prompt());
}
static void cap_event(const SDL_Event *e) {
    int kind = ha[cap.act].kind, s;
    switch (e->type) {
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        s = hj_find(e->jaxis.which);
        if (s < 0 || e->jaxis.axis >= HJ_AX || kind == HK_HAT) return;
        {
            int a = e->jaxis.axis, delta = (int)e->jaxis.value - (int)cap.base[s][a];
            if (abs(delta) < HJ_CAP_TRAVEL) return;
            hax *x = &hd[s].p.ax[a];
            if (kind == HK_AXIS) { x->invert = delta < 0; x->set = 1; cap_bound(s, PS2_JOYBIND_AXIS | a << 1 | 1); }
            else cap_bound(s, PS2_JOYBIND_AXIS | a << 1 | ((delta > 0) ^ x->invert));
        }
        break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        s = hj_find(e->jbutton.which);
        if (s >= 0 && kind == HK_BUTTON) cap_bound(s, PS2_JOYBIND_BUTTON | e->jbutton.button);
        break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        s = hj_find(e->jhat.which);
        if (s < 0 || kind == HK_AXIS || e->jhat.value == SDL_HAT_CENTERED || (e->jhat.hat < 8 && e->jhat.value == cap.hat[s][e->jhat.hat])) return;
        if (kind == HK_HAT) cap_bound(s, PS2_JOYBIND_HAT | e->jhat.hat << 2);
        else cap_bound(s, PS2_JOYBIND_HAT | e->jhat.hat << 2 | (e->jhat.value & SDL_HAT_UP ? 0 : e->jhat.value & SDL_HAT_RIGHT ? 1
                       : e->jhat.value & SDL_HAT_DOWN ? 2 : 3));
        break;
    default: break;
    }
}
void ps2_hotas_event(const SDL_Event *e) {
    int s;
    switch (e->type) {
    case SDL_EVENT_JOYSTICK_ADDED: if (hj_ready) hj_add(e->jdevice.which, "plugged"); return;
    case SDL_EVENT_JOYSTICK_REMOVED: hj_remove(e->jdevice.which); return;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        s = hj_find(e->jaxis.which);
        if (s >= 0 && e->jaxis.axis < HJ_AX) {
            hdev *d = &hd[s]; int a = e->jaxis.axis; Sint16 v = e->jaxis.value;
            if (!d->aseen[a]) { d->aseen[a] = 1; d->amin[a] = d->amax[a] = v; d->sweep_dirty = 1; }
            else if (v < d->amin[a]) { d->amin[a] = v; d->sweep_dirty = 1; }
            else if (v > d->amax[a]) { d->amax[a] = v; d->sweep_dirty = 1; }
        }
        break;
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        s = hj_find(e->jbutton.which);
        if (s >= 0) { hd[s].bseen[e->jbutton.button / 32] |= 1u << (e->jbutton.button % 32); hd[s].sweep_dirty = 1; }
        break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        s = hj_find(e->jhat.which);
        if (s >= 0 && e->jhat.hat < 32) { hd[s].hseen |= 1u << e->jhat.hat; hd[s].sweep_dirty = 1; }
        break;
    default: return;
    }
    if (cap.active && cap.armed) cap_event(e);
}

/* ---- SET LEVER NEUTRAL ---- */
static char neutral_note[120] = "No throttle lever is connected.";
const char *ps2_hotas_neutral_note(void) { return neutral_note; }
void ps2_hotas_neutral_now(void) {
    int s = hj_lever;
    if (s < 0) { snprintf(neutral_note, sizeof neutral_note, "No throttle lever is connected."); ps2_log("joy: SET LEVER NEUTRAL: %s", neutral_note); return; }
    hdev *d = &hd[s];
    int code = d->p.bind[HA_THROTTLE][0] ? d->p.bind[HA_THROTTLE][0] : d->p.bind[HA_THROTTLE][1], a = (code >> 1) & 63;
    float t = ax_x(d, a);
    if (!(code & 1)) t = -t;
    float u = (t + 1.0f) * 0.5f;
    if (a < HJ_AX) { d->p.ax[a].neutral = u; d->p.ax[a].set = 1; }
    prof_sync(d);
    ps2_settings_save();
    snprintf(neutral_note, sizeof neutral_note, "The lever's neutral is now at %.0f%% of its travel (saved).", (double)u * 100.0);
    ps2_log("joy: SET LEVER NEUTRAL: %s axis %d neutral %.3f (saved in [joystick:%s])", d->name, a, (double)u, d->secid);
}

/* ---- every poll ---- */
void ps2_hotas_update(void) {
    u64 now = ps2_kernel_vblank_count();
    if (jv_n || je_n) jv_service(now);
    if (cap.active && !cap.armed) cap_arm();
    if (!hj_n) return;
    for (int s = 0; s < HJ_SLOTS; s++) {
        hdev *d = &hd[s];
        if (!d->on) continue;
        if (d->unknown) {
            Sint16 st;
            int left = 0;
            for (int a = 0; a < d->naxes && a < HJ_AX; a++) {
                if (!d->known[a]) {
                    if (SDL_GetJoystickAxisInitialState(d->j, a, &st) || d->aseen[a]) d->known[a] = 1;
                    else left++;
                }
            }
            d->unknown = left;
        }
        if (!d->logged && ++d->polls >= 2) {         /* (the initial states exist after SDL's first update) */
            char fl[HJ_AX + 2];
            int n = d->naxes < HJ_AX ? d->naxes : HJ_AX;
            for (int a = 0; a < n; a++) fl[a] = d->known[a] ? '1' : '0';
            fl[n] = 0;
            d->logged = 1;
            ps2_log("joy: %s GUID %s vid:pid %04x:%04x ver %04x crc16 %04x type %s axes %d buttons %d hats %d initial-state %s slot %d",
                    d->name, d->guid, d->vid, d->pid, d->ver, d->crc, jtype_name(d->type), d->naxes, d->nbtn, d->nhat, fl, s);
            ps2_log("joy: %s: profile %s%s%s", d->name, d->p.src[0] ? d->p.src : "none: it does nothing until bound (F4 > Controller)",
                    d->p.status[0] ? ", " : "", d->p.status);
        }
    }
    static Uint64 win_ms;
    Uint64 ms = SDL_GetTicks();
    if (!win_ms) win_ms = ms;
    if (ms - win_ms >= 30000) {
        win_ms = ms;
        double pf = 1e6 / (double)SDL_GetPerformanceFrequency();
        char fb[200] = "";
        for (int s = 0, l = 0; s < HJ_SLOTS; s++) {
            hdev *d = &hd[s];
            if (!d->on) continue;
            if (d->sweep_dirty) sweep_log(d);
            if (hj_ffb > 0.0f && (d->ffb_n || d->hap || d->rumble_cap) && l < 150)
                l += snprintf(fb + l, sizeof fb - (size_t)l, "; FFB %s %u updates, peak %.2f", d->name, d->ffb_n, (double)d->ffb_peak);
            d->ffb_n = 0; d->ffb_peak = 0.0f;
        }
        for (int n = 0, l = (int)strlen(fb); n < jv_n && l < 170; n++)   /* (what the test devices received) */
            if (jv[n].on && jv[n].rumble) l += snprintf(fb + l, sizeof fb - (size_t)l, "; test dev%d rumble commands %u", n, jv[n].rumbles);
        ps2_log("joy: last 30 s: %d device%s, merge %.1f us avg, %.1f max over %llu polls%s", hj_n, hj_n == 1 ? "" : "s",
                hj_cost_n ? (double)hj_cost_ns * pf / (double)hj_cost_n : 0.0, (double)hj_cost_max * pf, (unsigned long long)hj_cost_n, fb);
        hj_cost_ns = hj_cost_max = hj_cost_n = 0;
    }
}
void ps2_hotas_debug(const ps2_pad_state *st) {
    static u64 last = ~0ull;
    if (!hj_dbg) return;
    u64 f = ps2_kernel_vblank_count();
    if (f % (u64)hj_dbg || f == last) return;
    last = f;
    zero_instr in;
    int ok = ps2_vk_instr_get && ps2_vk_instr_get(&in) && in.valid;
    ps2_log("hotas: field %llu lever %.3f acc %.3f brk %.3f thr %.3f tgt %.3f | l1 %u r1 %u l2 %u r2 %u lx %u ly %u rx %u ry %u buttons %04x | speed %.0f alt %.0f power %.3f ab %.2f brake %.2f stick %.2f %.2f %.2f",
            (unsigned long long)f, (double)hj_dbg_u, (double)hj_dbg_acc, (double)hj_dbg_brk, (double)hj_dbg_thr, (double)hj_dbg_target,
            st->l1, st->r1, st->l2, st->r2, st->lx, st->ly, st->rx, st->ry, st->buttons,
            ok ? (double)in.speed_kt : -1.0, ok ? (double)in.alt_ft : -1.0, ok ? (double)in.throttle : -1.0, ok ? (double)in.afterburner : -1.0,
            ok ? (double)in.airbrake : -1.0, ok ? (double)in.stick[0] : 0.0, ok ? (double)in.stick[1] : 0.0, ok ? (double)in.stick[2] : 0.0);
}

/* ---- FFB: only with ACZ_FFB set; a level change or a refresh every 100 ms ---- */
void ps2_hotas_ffb(float lo, float hi) {
    if (hj_ffb <= 0.0f || !hj_n) return;
    float lv = (lo > hi ? lo : hi) * hj_ffb;
    u64 now = SDL_GetTicksNS();
    for (int s = 0; s < HJ_SLOTS; s++) {
        hdev *d = &hd[s];
        if (!d->on) continue;
        if (d->hap && d->eff >= 0) {
            if (fabsf(lv - d->ffb_last) < 0.02f) continue;
            if (d->effd.type == SDL_HAPTIC_SINE) {
                d->effd.periodic.magnitude = (Sint16)(lv * 32767.0f);
                d->effd.periodic.period = hi > lo ? 16 : 33;
            } else d->effd.constant.level = (Sint16)(lv * 32767.0f);
            if (SDL_UpdateHapticEffect(d->hap, d->eff, &d->effd)) d->ffb_n++;
            d->ffb_last = lv;
        } else if (d->rumble_cap) {
            if (fabsf(lv - d->ffb_last) < 0.02f && (lv <= 0.0f || now - d->ffb_ns < 100000000ull)) continue;
            SDL_RumbleJoystick(d->j, (Uint16)(lo * hj_ffb * 65535.0f), (Uint16)(hi * hj_ffb * 65535.0f), lv > 0.0f ? 250u : 0u);
            d->ffb_last = lv; d->ffb_ns = now; d->ffb_n++;
        } else continue;
        if (lv > d->ffb_peak) d->ffb_peak = lv;
    }
}

/* ---- the window (F4) ---- */
static hdev *dev_nth(int i) {
    for (int s = 0; s < HJ_SLOTS; s++) if (hd[s].on && i-- == 0) return &hd[s];
    return NULL;
}
int ps2_hotas_info_at(int i, ps2_hotas_info *o) {
    hdev *d = dev_nth(i);
    if (!d) return 0;
    o->name = d->name; o->guid = d->guid; o->type = jtype_name(d->type);
    o->profile = d->p.src[0] ? d->p.src : "none"; o->status = d->p.status;
    o->vid = d->vid; o->pid = d->pid; o->ver = d->ver; o->crc16 = d->crc;
    o->naxes = d->naxes; o->nbuttons = d->nbtn; o->nhats = d->nhat; o->slot = (int)(d - hd);
    return 1;
}
float ps2_hotas_axis_now(int i, int a) {
    hdev *d = dev_nth(i);
    if (!d || a >= d->naxes) return 0.0f;
    int r = SDL_GetJoystickAxis(d->j, a);
    return (float)r / (r < 0 ? 32768.0f : 32767.0f);
}
int ps2_hotas_button_now(int i, int b) { hdev *d = dev_nth(i); return d && b < d->nbtn && SDL_GetJoystickButton(d->j, b); }
int ps2_hotas_hat_now(int i, int h) { hdev *d = dev_nth(i); return d && h < d->nhat ? SDL_GetJoystickHat(d->j, h) : 0; }
void ps2_hotas_axis_get(int i, int a, int *invert, float *dz, float *curve, int *slider, float *neutral) {
    hdev *d = dev_nth(i);
    const hax *x = d ? ax_of(d, a) : &hax_none;
    *invert = x->invert; *dz = x->dz < 0.0f ? 0.05f : x->dz; *curve = x->curve; *slider = x->slider; *neutral = x->neutral;
}
void ps2_hotas_axis_set(int i, int a, int invert, float dz, float curve, int slider, float neutral) {
    hdev *d = dev_nth(i);
    if (!d || a >= HJ_AX) return;
    hax *x = &d->p.ax[a];
    x->invert = invert; x->dz = dz; x->curve = curve; x->slider = slider; x->neutral = neutral; x->set = 1;
    prof_sync(d);
    ps2_settings_touch(PS2_CFG_SAVE);
}
int ps2_hotas_rows(void) { return HROWS; }
const char *ps2_hotas_row_label(int r) { return r >= 0 && r < HROWS ? ha[hrow[r].act].label : "?"; }
const char *ps2_hotas_row_group(int r) { return r >= 0 && r < HROWS ? hrow[r].group : NULL; }
const char *ps2_hotas_binding_name(int r, int k) {
    static char buf[4][96]; static unsigned nx;
    if (r < 0 || r >= HROWS) return "-";
    int act = hrow[r].act;
    for (int s = 0; s < HJ_SLOTS; s++) {
        if (!hd[s].on || !hd[s].p.bind[act][k]) continue;
        int code = hd[s].p.bind[act][k];
        char *o = buf[nx++ % 4], t[24];
        if (code & PS2_JOYBIND_AXIS) {
            int dir = code & 1;
            snprintf(o, 96, "%s: axis %d%s", hd[s].name, (code >> 1) & 63, ha[act].kind == HK_AXIS ? (dir ? "" : " -") : (dir ? " +" : " -"));
        } else if (code & PS2_JOYBIND_BUTTON) snprintf(o, 96, "%s: button %d", hd[s].name, code & 0xFF);
        else { code_text(code, ha[act].kind, t, sizeof t); snprintf(o, 96, "%s: %s", hd[s].name, t); }
        return o;
    }
    return "-";
}
int ps2_hotas_report(char *out, int n) {
    int l = 0;
    for (int s = 0; s < HJ_SLOTS && l < n - 200; s++) {
        hdev *d = &hd[s];
        if (!d->on) continue;
        l += snprintf(out + l, (size_t)(n - l), "joy: %s GUID %s vid:pid %04x:%04x ver %04x crc16 %04x type %s axes %d buttons %d hats %d\n[joystick:%s]\n",
                      d->name, d->guid, d->vid, d->pid, d->ver, d->crc, jtype_name(d->type), d->naxes, d->nbtn, d->nhat, d->secid);
        for (int a = 0; a < HJ_AX && l < n - 200; a++) {
            const hax *x = &d->p.ax[a];
            if (x->set) l += snprintf(out + l, (size_t)(n - l), "axis%d = invert:%d dz:%.3f curve:%.2f slider:%d neutral:%.3f\n", a, x->invert, (double)(x->dz < 0.0f ? 0.05f : x->dz), (double)x->curve, x->slider, (double)x->neutral);
        }
        for (int a = 0; a < HA_COUNT && l < n - 200; a++) {
            char c0[24] = "", c1[24] = "";
            if (!d->p.bind[a][0] && !d->p.bind[a][1]) continue;
            if (d->p.bind[a][0]) code_text(d->p.bind[a][0], ha[a].kind, c0, sizeof c0);
            if (d->p.bind[a][1]) code_text(d->p.bind[a][1], ha[a].kind, c1, sizeof c1);
            l += snprintf(out + l, (size_t)(n - l), "bind.%s = %s %s\n", ha[a].key, c0, c1);
        }
        l += snprintf(out + l, (size_t)(n - l), "\n");
    }
    return l;
}
