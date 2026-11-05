/* ps2_quit.c -- leaving the game from the headset: a clean quit to desktop and the controller prompt for it.

   The user plays in a Quest with a controller and cannot see the desktop; before this the only way out was killing
   the process (and Esc twice on the window, out of reach in the headset). Two ways in, one clean way out:

   - The game's own pause menu (zero_vr.c adds "QUIT GAME" with the game's own YES/NO confirmation) and
   - the controller combo: hold View (Back) + Menu (Start) ~1.5 s whenever the game is not in flight (the pause menu,
     the title screen, the main menu, briefings, the hangar...). A prompt appears in the headset (an OpenXR quad layer,
     ps2_xr.c) and, without a headset, on the window: A = quit to desktop, X = back to the main menu (offered while
     the pause menu is up: the game's own QUIT MISSION, zero_vr.c), B = back to the game (Y too: the game's own menus
     cancel with Triangle/Y, so a habitual Y must never do more than close the prompt). While the prompt is up, and
     afterwards until the buttons that answered it are released, the game sees a neutral pad. In flight the combo
     only shows how to quit (pause first): nothing can end a flight by accident. The menus between missions show
     the combo next to the game's OK / CANCEL bar, in the game's own font (zero_menus.c, ps2_quit_hint_wanted).

   The quit itself (renderer thread, ps2_quit_tick): it waits until the memory card is idle (no card file open and no
   card activity for 1.5 s, at most 30 s), writes any unwritten card change, then closes the window exactly like Esc
   twice: ps2_finish writes the run summary, the renderer ends the OpenXR session (ps2_xr_shutdown: request exit, end,
   destroy) and the process ends with exit code 0 (ps2_quit_exit), so the launcher sees "exit 0" and its window closes.
   A watchdog ends the process (code 0, logged) if the shutdown is still running 20 s after the close.

   Env: ACZ_QUIT_COMBO=0 turns the combo off; ACZ_QUIT_HOLD=1.5 seconds to hold; PS2_EXIT_TERMINATE=0 exits through
   _exit (DLL detach handlers run) even after OpenXR was used.

   The ENHANCEMENTS page (bug #64, the second half of this file): every player-facing improvement as a setting
   (ps2_settings.c ps2_enh_*), laid out like the game's own PAUSE MENU > FLIGHT OPTIONS page -- the breadcrumb in the
   small font, rows in the menu font with the value column, the yellow cursor box, the value list dropping down, the
   help line under a rule -- drawn with the game's own glyphs (its HUD atlas, the 4x replacement in
   textures/hud/3F861D67_20_256x256_hud_atlas.png; a system font if that file is missing) on the game's dark grid
   panel, as the headset quad layer (69 x 54 degrees, the menu font ~2.1 degrees tall: 1.5 m wide at 1.1 m; since bug
   #136 it sits on the cinema screen with the game's own menus, fixed in the world, ps2_xr.c ACZ_ENH_PAGE_PLACE) and
   as text on the window without a headset. Opened from the pause menu (zero_vr.c: an ENHANCEMENTS row; the pause
   menu is hidden while the page is up) or from the View + Menu panel outside the pause menu ({X} Enhancements: the
   main menu and the other menus between missions). Categories -> items -> values: D-pad or left stick up/down moves, left/right changes a value
   at once, A opens (a category, an item's value list, an action), B or Y goes back, Menu closes. Each change is saved
   at once (ps2_settings_save) and applied at once where the item allows; the help line says "In effect now", "Takes
   effect at the next mission" or "Restart the game to apply" (items waiting for a restart are marked *), and leaving
   the page after such a change asks RESTART THE GAME TO APPLY: QUIT GAME NOW / CONTINUE. The game sees a neutral pad
   while the page is up. A list longer than the page (bug #149: GRAPHICS 15 rows, IMMERSION 23, SOUND 13, COCKPIT 11)
   scrolls with the cursor: ACZ_ENH_PAGE_ROWS=10 rows at a time (all that fit above the help line, at the same font
   size), one row of the list kept beyond the cursor while the list goes on, small yellow triangles at the right edge
   of the first / last row when more rows are above / below; up from the first row wraps to the last and down from
   the last to the first, as before; the value column moves right where a category's longest label would run into it
   (COCKPIT, SOUND, CLOUDS, IMMERSION; 0 = the old page: a fixed list of the first 12 rows, values at one column for
   every category). ACZ_ENH_PAGE_W=1.5 the quad's width in metres at 1.1 m; ACZ_DEBUG_ENH=1 writes each new page image
   to out/enh_page_NNN.ppm (the quad layer is not part of the eye dumps). */
#include "ps2_runtime.h"
#include "ps2_quit.h"
#include "ps2_settings.h"
#include "ps2_touch.h"                       /* the TOUCH CONTROLS card: the Touch mapping in use */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#pragma GCC diagnostic pop

int  ps2_video_request_close(const char *why);   /* ps2_video.c: like Esc twice */
int  ps2_xr_runtime_loaded(void);                /* ps2_xr.c: an OpenXR instance was created in this process */
int  ps2_xr_session_running(void);

static int (*volatile game_state_fn)(void);
static int (*volatile title_fn)(int do_it);
void ps2_quit_set_game_state_fn(int (*fn)(void)) { game_state_fn = fn; }
void ps2_quit_set_title_fn(int (*fn)(int do_it)) { title_fn = fn; }
static int game_state(void) { int (*f)(void) = game_state_fn; return f ? f() : PS2_QUIT_GAME_MENU; }
static int title_possible(void) { int (*f)(int) = title_fn; return f ? f(0) : 0; }

/* ---- the quit request ---- */
static volatile int q_requested;
static const char *volatile q_why = "";
void ps2_quit_request(const char *why) {
    if (__atomic_exchange_n(&q_requested, 1, __ATOMIC_ACQ_REL)) return;
    q_why = why ? why : "?";
    ps2_log("quit: requested (%s): waiting for the memory card, then closing the game", q_why);
}
int ps2_quit_requested(void) { return __atomic_load_n(&q_requested, __ATOMIC_ACQUIRE); }

static void *quit_watchdog(void *arg) {
    (void)arg;
    sleep(20);
    ps2_log("quit: the shutdown is still running 20 s after the close; ending the process now (the memory card was "
            "written before the close)");
    ps2_quit_exit(0);
    return NULL;
}

static volatile double q_close_t = -1.0;        /* wall time of the close request (for the exit log line) */
void ps2_quit_exit(int code) {
    if (q_close_t >= 0.0)
        ps2_log("quit: process exit, code %d, %.2f s after the close request", code, ps2_wall_seconds() - q_close_t);
    fflush(NULL);
#ifdef _WIN32
    {   static int term = -1;
        if (term < 0) { const char *e = getenv("PS2_EXIT_TERMINATE"); term = !(e && *e == '0'); }
        if (term && ps2_xr_runtime_loaded()) {
            /* no DLL detach handlers: the OpenXR session and instance are gone already (ps2_xr_shutdown), and the
               Meta runtime's detach handler hung for 60 s ("RuntimeIPC ... std::thread construction FAILED") */
            TerminateProcess(GetCurrentProcess(), (UINT)code);
        }
    }
#endif
    _exit(code);
}

/* ---- prompt state (renderer thread) ---- */
enum { Q_IDLE, Q_HOLD, Q_PROMPT, Q_TITLE, Q_SAVING, Q_CLOSING };
static int q_state;
static double q_t0;                 /* start of the current state */
static int q_wait_release;          /* prompt: ignore buttons until all are released once */
static int q_swallow;               /* after the prompt: the game sees a neutral pad until all are released */
static int q_prev_a, q_prev_b, q_prev_x, q_prev_y, q_prev_both;
static int q_ps;                    /* PlayStation glyphs */
static int q_touch;                 /* Quest Touch glyphs (ctl_priority: Touch is the active controller) */
static double q_hint_until;         /* "pause first" hint */
static int q_hint_kind;             /* 1 = pause first, 2 = main menu not possible now */
static float q_hold_frac;
static int q_title_ok;
static u32 q_gen = 1;               /* overlay image generation */
static char q_text[512];            /* the window text (ps2_quit_overlay_text) */
static int q_combo = -1;
static float q_hold_s = 1.5f;

static void q_set(int st, double now) { q_state = st; q_t0 = now; q_gen++; }

/* the ENHANCEMENTS page (below) */
static volatile int pg_req = -1;    /* an open request (from), any thread */
static volatile int pg_vis;         /* the page is up */
static int pg_native;               /* ACZ_ENH_NATIVE: this visit of the page is drawn by the game (zero_vr.c zn_*) */
static int pg_native_req;
static void pg_snap_publish(void);
static int pg_drawn_by_game(void);  /* no overlay image: the page without the TOUCH CONTROLS card, with ACZ_ENH_NATIVE */
static void pg_open_now(int from, double now);
static int pg_pad(ps2_pad_state *st, int a, int b, int y, double now);
/* the TOUCH CONTROLS card (bug #239; below the ENHANCEMENTS page, as one of its levels) */
static volatile int tc_req = -1;    /* an open request (from), any thread */
static int tc_pending = -1;         /* the request being served: FROM_FLIGHT / FROM_AUTO wait for the game to pause */
static double tc_req_t, tc_pulse_until, tc_resume_t;
static int tc_resume;               /* 1: the card paused a flight (a Start press): it resumes it when it closes; 2 the
                                       close asked for the press, 3 pressing */
static void tc_serve(ps2_pad_state *st, double now);
static char tq_title[64], tq_line[160];   /* ps2_quit_touch_hint */
void ps2_quit_touch_card_open(int from) { __atomic_store_n(&tc_req, from, __ATOMIC_RELEASE); }
int ps2_quit_game_state(void) { return game_state(); }
static int (*volatile menu_pad_fn)(ps2_pad_state *st);
void ps2_quit_set_menu_pad_fn(int (*fn)(ps2_pad_state *st)) { menu_pad_fn = fn; }
static volatile double menu_seen_t = -10.0;
void ps2_quit_note_menu(void) { menu_seen_t = ps2_wall_seconds(); }
int ps2_quit_menu_on_screen(void) { return ps2_wall_seconds() - menu_seen_t < 0.25; }
int ps2_quit_enh_visible(void) { return pg_vis || pg_req >= 0; }
void ps2_quit_enh_open(int from) { __atomic_store_n(&pg_req, from, __ATOMIC_RELEASE); }

/* A requested quit (the pause menu item, the prompt): the memory card wait, then the close. Returns 1 while it runs. */
static int quit_tick(double now) {
    if (ps2_quit_requested() && q_state < Q_SAVING) {
        q_set(Q_SAVING, now);
        q_hint_until = 0.0;
    }
    if (q_state == Q_SAVING) {
        int busy = ps2_mc_busy();
        if (!busy || now - q_t0 > 30.0) {
            if (busy) ps2_log("quit: the memory card is still busy after 30 s; closing anyway (the card file on disk "
                              "keeps its last complete state)");
            ps2_mc_flush();
            ps2_log("quit: closing the game (%s) %.1f s after the request", q_why, now - q_t0);
            q_set(Q_CLOSING, now);
            q_close_t = now;
            if (ps2_video_request_close(q_why) == 0) {
                pthread_t t;
                if (pthread_create(&t, NULL, quit_watchdog, NULL) == 0) pthread_detach(t);
            } else {
                ps2_log("quit: no window to close; ending the run directly");
                ps2_quit_exit(0);
            }
        }
        return 1;
    }
    return q_state == Q_CLOSING;
}
void ps2_quit_tick(void) { quit_tick(ps2_wall_seconds()); }

static int q_enh_ok(void) {                /* ACZ_ENH_MENU=0: no ENHANCEMENTS entry (pause row, the panel's X) */
    static int on = -1;
    if (on < 0) { const char *e = getenv("ACZ_ENH_MENU"); on = !(e && *e == '0'); }
    return on;
}
static void q_env(void) {
    if (q_combo >= 0) return;
    const char *e = getenv("ACZ_QUIT_HOLD");
    if (e && *e) { float v = (float)atof(e); if (v >= 0.2f && v <= 10.0f) q_hold_s = v; }
    e = getenv("ACZ_QUIT_COMBO");
    q_combo = !(e && *e == '0');
}

static void pg_dirty(void);
int ps2_quit_pad(ps2_pad_state *st, int back, int start, int a, int b, int x, int y, int glyphs) {
    double now = ps2_wall_seconds();
    q_env();
    if ((glyphs == 0) != q_ps || (glyphs == 2) != q_touch) {   /* the active controller's kind changed */
        q_ps = glyphs == 0; q_touch = glyphs == 2; q_gen++;
        if (pg_vis) pg_dirty();
    }
    if (quit_tick(now)) return 1;
    {   int from = __atomic_exchange_n(&pg_req, -1, __ATOMIC_ACQ_REL);
        if (from >= 0) pg_open_now(from, now);
    }
    if (q_hint_kind >= 3 && q_hint_until > 0.0 && now > q_hint_until) {   /* (Touch hint) */
        if (q_hint_kind == 4) ps2_log("quit: the Touch line is gone (its time)");
        q_hint_until = 0.0; q_gen++;
    }
    {   int from = __atomic_exchange_n(&tc_req, -1, __ATOMIC_ACQ_REL);      /* the TOUCH CONTROLS card */
        if (from >= 0) { tc_pending = from; tc_req_t = now; tc_pulse_until = 0.0; }
    }
    if (tc_pending >= 0) tc_serve(st, now);
    if (tc_resume >= 2 && !pg_vis && now >= tc_resume_t) {   /* the card paused a flight: Start resumes it after the close */
        if (tc_resume == 2 && game_state() != PS2_QUIT_GAME_PAUSED) {
            ps2_log("quit: TOUCH CONTROLS card closed; the game is not paused any more (no resume press)");
            tc_resume = 0;
        } else if (now >= tc_resume_t + 0.12) tc_resume = 0;          /* (the press was 0.12 s) */
        else tc_resume = 3;
        if (tc_resume == 3) {                            /* Start alone (the closing button may still be down) */
            st->buttons = 1u << PS2_ACT_START;
            st->l1 = st->r1 = st->l2 = st->r2 = 0;
            st->lx = st->ly = st->rx = st->ry = PS2_PAD_ANALOG_NEUTRAL;
            return 0;
        }
    }
    if (pg_vis) {                            /* the page takes the controller; the game sees a neutral pad */
        pg_pad(st, a, b, y, now);
        pg_snap_publish();
        q_prev_a = a; q_prev_b = b; q_prev_x = x; q_prev_y = y; q_prev_both = back && start;
        return 1;
    }
    if (q_state == Q_IDLE && !q_swallow && menu_pad_fn && menu_pad_fn(st)) return 1;   /* (zero_menus.c: the OPTIONS row) */
    if (!q_combo) return 0;
    int block = 0;

    int both = back && start;
    if (back && (st->buttons & (1u << 3))) st->buttons &= (u16)~(1u << 3);   /* Start never reaches the game with View held */
    if (q_swallow) {                         /* the answer to the prompt must not reach the game (Y/B = its cancel) */
        if (!a && !b && !x && !y && !back && !start) q_swallow = 0;
        else block = 1;
    }
    int gs = game_state();
    switch (q_state) {
    case Q_IDLE:
        if (both && !q_prev_both) {
            if (gs == PS2_QUIT_GAME_FLYING) {
                q_hint_kind = 1; q_hint_until = now + 4.0; q_gen++;
                ps2_log("quit: View + Menu in flight: only the hint (pause first)");
            } else {
                q_set(Q_HOLD, now);
                q_hold_frac = 0.0f;
            }
        }
        break;
    case Q_HOLD: {
        if (!both || gs == PS2_QUIT_GAME_FLYING) { q_set(Q_IDLE, now); break; }
        float f = (float)((now - q_t0) / q_hold_s);
        if (f >= 1.0f) {
            q_title_ok = title_possible();
            q_set(Q_PROMPT, now);
            q_wait_release = 1;
            ps2_log("quit: prompt shown (game state %d, main menu %s)", gs, q_title_ok ? "offered" : "not offered");
        } else if ((int)(f * 40.0f) != (int)(q_hold_frac * 40.0f)) {
            q_gen++;
        }
        q_hold_frac = f;
        block = 1;
        break; }
    case Q_PROMPT:
        block = 1;
        if (q_wait_release) {
            if (!a && !b && !x && !y && !back && !start) q_wait_release = 0;
        } else if (a && !q_prev_a) {
            ps2_quit_request("controller prompt");
        } else if (x && !q_prev_x && q_title_ok) {
            int (*f)(int) = title_fn;
            if (f && f(1)) { q_set(Q_TITLE, now); ps2_log("quit: back to the main menu (controller prompt)"); }
            else { q_hint_kind = 2; q_hint_until = now + 3.0; q_set(Q_IDLE, now); }
            q_swallow = 1;
        } else if (x && !q_prev_x && !q_title_ok && q_enh_ok()) {
            q_set(Q_IDLE, now);                       /* outside the pause menu X opens the ENHANCEMENTS page */
            ps2_log("quit: ENHANCEMENTS page (controller prompt)");
            pg_open_now(PS2_ENH_FROM_MENUS, now);
            block = 1;
        } else if ((b && !q_prev_b) || (y && !q_prev_y)) {
            q_set(Q_IDLE, now);
            q_swallow = 1;
            ps2_log("quit: prompt cancelled");
        } else if (now - q_t0 > 30.0) {
            q_set(Q_IDLE, now);
            q_swallow = 1;
            ps2_log("quit: prompt timed out");
        }
        if (q_state == Q_PROMPT && !q_title_ok && title_possible()) { q_title_ok = 1; q_gen++; }
        break;
    case Q_TITLE:
        block = 1;                                    /* hold the pad neutral while the game leaves */
        if (now - q_t0 > 2.5) q_set(Q_IDLE, now);
        break;
    }
    q_prev_a = a; q_prev_b = b; q_prev_x = x; q_prev_y = y; q_prev_both = both;
    if (q_hint_until > 0.0 && now > q_hint_until) { q_hint_until = 0.0; q_gen++; }
    return block;
}

/* The menus between missions show a one-line hint for the combo (zero_menus.c, in the game's own font) whenever the
   combo would open the prompt: combo on, the game not flying, no prompt / quit under way. Any thread (the hint is
   drawn on the EE thread; these are plain flags of the renderer thread). */
int ps2_quit_hint_wanted(int *playstation) {
    if (playstation) *playstation = q_ps;
    q_env();
    if (q_combo == 0 || q_state != Q_IDLE || ps2_quit_requested() || q_hint_until > 0.0 || ps2_quit_enh_visible()) return 0;
    return game_state() != PS2_QUIT_GAME_FLYING;
}

/* ---- the prompt image: a small dark panel with text and controller glyphs, drawn on the CPU ----
   One image size for the prompt and the page (the headset layer's swapchain is made once): the prompt's 1024 x 288
   panel sits in the middle band (PR_Y0) of a transparent 1024 x 768 image on a 0.62 m quad, so it looks as before. */
#define OVL_W 1024
#define OVL_H 768
#define PR_Y0 240.0f
static u8 ovl_px[OVL_W * OVL_H * 4];
static u32 ovl_built_gen;
static int font_state;              /* 0 not loaded, 1 ok, -1 failed */
static stbtt_fontinfo font;
static unsigned char *font_data;

static void font_load(void) {
    static const char *names[] = { "bahnschrift.ttf", "segoeuib.ttf", "arialbd.ttf", "segoeui.ttf", "arial.ttf" };
    char dir[260] = "C:\\Windows";
#ifdef _WIN32
    if (!GetWindowsDirectoryA(dir, sizeof dir)) strcpy(dir, "C:\\Windows");
#endif
    font_state = -1;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[400];
        snprintf(p, sizeof p, "%s\\Fonts\\%s", dir, names[i]);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char *d = n > 0 && n < (64L << 20) ? (unsigned char *)malloc((size_t)n) : NULL;
        if (d && fread(d, 1, (size_t)n, f) == (size_t)n && stbtt_InitFont(&font, d, stbtt_GetFontOffsetForIndex(d, 0))) {
            fclose(f);
            font_data = d;
            font_state = 1;
            ps2_log("quit: prompt font %s", p);
            return;
        }
        free(d);
        fclose(f);
    }
    ps2_log("quit: no system font found; the headset prompt shows no text (the log and the window still do)");
}

/* premultiplied "over" with a straight colour (sRGB 0..1) and a coverage */
static void px_over(int x, int y, float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= OVL_W || y >= OVL_H || a <= 0.0f) return;
    if (a > 1.0f) a = 1.0f;
    u8 *p = ovl_px + ((size_t)y * OVL_W + (size_t)x) * 4;
    float k = 1.0f - a;
    float v[4] = { r * a + p[0] / 255.0f * k, g * a + p[1] / 255.0f * k, b * a + p[2] / 255.0f * k, a + p[3] / 255.0f * k };
    for (int i = 0; i < 4; i++) { float t = v[i] * 255.0f + 0.5f; p[i] = (u8)(t > 255.0f ? 255.0f : t); }
}

typedef float (*sdf_fn)(float x, float y, const float *p);
static float sd_rbox(float x, float y, const float *p) {      /* cx cy hw hh r */
    float qx = fabsf(x - p[0]) - (p[2] - p[4]), qy = fabsf(y - p[1]) - (p[3] - p[4]);
    float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
    float in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - p[4];
}
static float sd_circle(float x, float y, const float *p) { return hypotf(x - p[0], y - p[1]) - p[2]; }
static float sd_tri(float x, float y, const float *p) {       /* a filled triangle ax ay bx by cx cy (either winding) */
    float area = (p[2] - p[0]) * (p[5] - p[1]) - (p[3] - p[1]) * (p[4] - p[0]), d = -1e9f;
    for (int i = 0; i < 3; i++) {                             /* the farthest of the three edges' half-planes */
        float ax = p[2 * i], ay = p[2 * i + 1], ex = p[(2 * i + 2) % 6] - ax, ey = p[(2 * i + 3) % 6] - ay;
        float l = sqrtf(ex * ex + ey * ey), c = l > 0 ? (ex * (y - ay) - ey * (x - ax)) / l : 0.0f;
        float di = area > 0 ? -c : c;
        if (di > d) d = di;
    }
    return d;
}
static float sd_ring(float x, float y, const float *p) { return fabsf(hypotf(x - p[0], y - p[1]) - p[2]) - p[3] * 0.5f; }
static float seg_dist(float x, float y, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    float t = l > 0 ? ((x - ax) * dx + (y - ay) * dy) / l : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return hypotf(x - (ax + t * dx), y - (ay + t * dy));
}
static float sd_segs(float x, float y, const float *p) {      /* n, width, then n segments ax ay bx by */
    int n = (int)p[0];
    float d = 1e9f;
    for (int i = 0; i < n; i++) {
        float s = seg_dist(x, y, p[2 + 4 * i], p[3 + 4 * i], p[4 + 4 * i], p[5 + 4 * i]);
        if (s < d) d = s;
    }
    return d - p[1] * 0.5f;
}
static void shape(float x0, float y0, float x1, float y1, sdf_fn f, const float *p, float r, float g, float b, float a) {
    int ix0 = (int)floorf(x0) - 1, iy0 = (int)floorf(y0) - 1, ix1 = (int)ceilf(x1) + 1, iy1 = (int)ceilf(y1) + 1;
    for (int y = iy0; y <= iy1; y++)
        for (int x = ix0; x <= ix1; x++) {
            float d = f((float)x + 0.5f, (float)y + 0.5f, p);
            float c = 0.5f - d;
            if (c <= 0.0f) continue;
            px_over(x, y, r, g, b, (c > 1.0f ? 1.0f : c) * a);
        }
}

static float text_width(const char *s, float px) {
    if (font_state != 1) return 0.0f;
    float sc = stbtt_ScaleForPixelHeight(&font, px), w = 0.0f;
    for (const char *c = s; *c; c++) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font, (unsigned char)*c, &adv, &lsb);
        w += (float)adv * sc;
        if (c[1]) w += sc * (float)stbtt_GetCodepointKernAdvance(&font, (unsigned char)c[0], (unsigned char)c[1]);
    }
    return w;
}
/* text with its baseline at y, starting at x; returns the end x */
static float text(float x, float y, const char *s, float px, float r, float g, float b, float a) {
    if (font_state != 1) return x;
    float sc = stbtt_ScaleForPixelHeight(&font, px);
    static unsigned char gbuf[160 * 160];
    for (const char *c = s; *c; c++) {
        int cp = (unsigned char)*c, adv, lsb, gx0, gy0, gx1, gy1;
        float fx = x - floorf(x);
        stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
        stbtt_GetCodepointBitmapBoxSubpixel(&font, cp, sc, sc, fx, 0.0f, &gx0, &gy0, &gx1, &gy1);
        int gw = gx1 - gx0, gh = gy1 - gy0;
        if (gw > 0 && gh > 0 && gw <= 160 && gh <= 160) {
            stbtt_MakeCodepointBitmapSubpixel(&font, gbuf, gw, gh, gw, sc, sc, fx, 0.0f, cp);
            int bx = (int)floorf(x) + gx0, by = (int)floorf(y) + gy0;
            for (int j = 0; j < gh; j++)
                for (int i = 0; i < gw; i++) {
                    unsigned char v = gbuf[j * gw + i];
                    if (v) px_over(bx + i, by + j, r, g, b, a * (float)v / 255.0f);
                }
        }
        x += (float)adv * sc;
        if (c[1]) x += sc * (float)stbtt_GetCodepointKernAdvance(&font, cp, (unsigned char)c[1]);
    }
    return x;
}
static void text_c(float cx, float y, const char *s, float px, float r, float g, float b, float a) {
    text(cx - text_width(s, px) * 0.5f, y, s, px, r, g, b, a);
}

/* controller glyphs, centred at (cx, cy) with radius rad. kind: 'A' 'B' 'X' 'Y' (Xbox: coloured letter in a dark
   disc; PlayStation: cross / circle / square / triangle; Quest Touch: a white letter, as on its dark buttons), 'V' View /
   Select / Touch's left thumbstick press (its View in pad mode), 'M' Menu / Start. */
static void glyph(char kind, float cx, float cy, float rad) {
    float disc[3] = { cx, cy, rad };
    shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_circle, disc, 0.13f, 0.14f, 0.16f, 1.0f);
    float ring[4] = { cx, cy, rad - 1.5f, 2.0f };
    shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_ring, ring, 0.55f, 0.58f, 0.62f, 1.0f);
    if (kind == 'V' || kind == 'M') {
        if (kind == 'V') {           /* View: two overlapping windows / Select: a small bar / Touch: a thumbstick */
            if (q_touch) {
                float cap[3] = { cx, cy, rad * 0.52f };
                shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_circle, cap, 0.42f, 0.44f, 0.47f, 1.0f);
                float rim[4] = { cx, cy, rad * 0.52f, 1.6f };
                shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_ring, rim, 0.85f, 0.86f, 0.88f, 1.0f);
                float dot[3] = { cx, cy, rad * 0.13f };
                shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_circle, dot, 0.92f, 0.92f, 0.92f, 1.0f);
            } else if (q_ps) {
                float bx[5] = { cx, cy, rad * 0.45f, rad * 0.16f, rad * 0.12f };
                shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_rbox, bx, 0.9f, 0.9f, 0.9f, 1.0f);
            } else {
                float s = rad * 0.32f, w = 2.6f;
                /* front window (x -1.3..0.5, y -0.4..1.1) and the corner of the one behind it, in units of s */
                static const float k[8][4] = {
                    { -1.3f, -0.4f, 0.5f, -0.4f }, { 0.5f, -0.4f, 0.5f, 1.1f }, { 0.5f, 1.1f, -1.3f, 1.1f },
                    { -1.3f, 1.1f, -1.3f, -0.4f }, { -0.6f, -1.1f, 1.3f, -1.1f }, { 1.3f, -1.1f, 1.3f, 0.4f },
                    { 1.3f, 0.4f, 0.5f, 0.4f }, { -0.6f, -1.1f, -0.6f, -0.4f } };
                float sq[2 + 8 * 4] = { 8, w };
                for (int i = 0; i < 8; i++) {
                    sq[2 + 4 * i] = cx + s * k[i][0]; sq[3 + 4 * i] = cy + s * k[i][1];
                    sq[4 + 4 * i] = cx + s * k[i][2]; sq[5 + 4 * i] = cy + s * k[i][3];
                }
                shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_segs, sq, 0.92f, 0.92f, 0.92f, 1.0f);
            }
        } else if (q_ps) {           /* Start: a small right-pointing triangle */
            float s = rad * 0.40f;
            float tr[2 + 12] = { 3, 3.0f, cx - s * 0.7f, cy - s, cx + s, cy, cx + s, cy, cx - s * 0.7f, cy + s,
                                 cx - s * 0.7f, cy + s, cx - s * 0.7f, cy - s };
            shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_segs, tr, 0.92f, 0.92f, 0.92f, 1.0f);
        } else {                     /* Menu: three lines */
            float s = rad * 0.42f;
            float ln[2 + 3 * 4] = { 3, 3.0f, cx - s, cy - s * 0.65f, cx + s, cy - s * 0.65f,
                                    cx - s, cy, cx + s, cy, cx - s, cy + s * 0.65f, cx + s, cy + s * 0.65f };
            shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_segs, ln, 0.92f, 0.92f, 0.92f, 1.0f);
        }
        return;
    }
    if (q_touch) {                   /* Quest Touch: the letter in white */
        char s[2] = { kind, 0 };
        float px = rad * 1.35f;
        text_c(cx, cy + px * 0.36f, s, px, 0.93f, 0.93f, 0.94f, 1.0f);
    } else if (!q_ps) {
        float r = 0.9f, g = 0.9f, b = 0.9f;
        if (kind == 'A') { r = 0.42f; g = 0.80f; b = 0.25f; }
        else if (kind == 'B') { r = 0.92f; g = 0.30f; b = 0.25f; }
        else if (kind == 'Y') { r = 0.98f; g = 0.80f; b = 0.12f; }
        else if (kind == 'X') { r = 0.25f; g = 0.55f; b = 0.98f; }
        char s[2] = { kind, 0 };
        float px = rad * 1.35f;
        text_c(cx, cy + px * 0.36f, s, px, r, g, b, 1.0f);
    } else if (kind == 'A') {        /* cross */
        float s = rad * 0.42f;
        float ln[2 + 8] = { 2, 3.5f, cx - s, cy - s, cx + s, cy + s, cx - s, cy + s, cx + s, cy - s };
        shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_segs, ln, 0.55f, 0.70f, 0.98f, 1.0f);
    } else if (kind == 'B') {        /* circle */
        float rg[4] = { cx, cy, rad * 0.45f, 3.5f };
        shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_ring, rg, 0.98f, 0.45f, 0.45f, 1.0f);
    } else if (kind == 'X') {        /* square */
        float s = rad * 0.40f;
        float ln[2 + 16] = { 4, 3.5f, cx - s, cy - s, cx + s, cy - s, cx + s, cy - s, cx + s, cy + s,
                             cx + s, cy + s, cx - s, cy + s, cx - s, cy + s, cx - s, cy - s };
        shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_segs, ln, 0.95f, 0.55f, 0.85f, 1.0f);
    } else {                         /* triangle */
        float s = rad * 0.5f;
        float ln[2 + 12] = { 3, 3.5f, cx, cy - s, cx + s * 0.95f, cy + s * 0.6f, cx + s * 0.95f, cy + s * 0.6f,
                             cx - s * 0.95f, cy + s * 0.6f, cx - s * 0.95f, cy + s * 0.6f, cx, cy - s };
        shape(cx - rad, cy - rad, cx + rad, cy + rad, sd_segs, ln, 0.35f, 0.85f, 0.70f, 1.0f);
    }
}


/* A line of text with controller glyphs: "{A}" "{B}" "{X}" "{Y}" "{V}" (View / Select) "{M}" (Menu / Start), centred on
   cx with its baseline at y. Returns the width (draw = 0: measure only). */
static float rich(float cx, float y, const char *s, float px, int draw) {
    float rad = px * 0.56f, gap = px * 0.22f, w = 0.0f, x = 0.0f;
    for (int pass = draw ? 0 : 1; pass < 2; pass++) {
        x = pass ? 0.0f : cx - rich(cx, y, s, px, 0) * 0.5f;
        const char *p = s;
        char seg[256];
        while (*p) {
            if (p[0] == '{' && p[1] && p[2] == '}') {
                if (pass == 0) glyph(p[1], x + gap + rad, y - px * 0.34f, rad);
                x += gap * 2.0f + rad * 2.0f;
                p += 3;
                continue;
            }
            size_t n = 0;
            while (p[n] && !(p[n] == '{' && p[n + 1] && p[n + 2] == '}') && n + 1 < sizeof seg) { seg[n] = p[n]; n++; }
            seg[n] = 0;
            p += n;
            if (pass == 0) x = text(x, y, seg, px, 0.93f, 0.94f, 0.95f, 1.0f);
            else x += text_width(seg, px);
        }
        if (pass == 1) w = x;
    }
    return w;
}

static void panel(void) {                  /* the prompt's 1024 x 288 panel in the image's middle band */
    memset(ovl_px, 0, sizeof ovl_px);
    float outer[5] = { OVL_W * 0.5f, PR_Y0 + 144.0f, OVL_W * 0.5f - 2.0f, 142.0f, 26.0f };
    shape(0, PR_Y0, OVL_W, PR_Y0 + 288.0f, sd_rbox, outer, 0.55f, 0.70f, 0.78f, 0.95f);
    float inner[5] = { OVL_W * 0.5f, PR_Y0 + 144.0f, OVL_W * 0.5f - 5.0f, 139.0f, 23.0f };
    shape(0, PR_Y0, OVL_W, PR_Y0 + 288.0f, sd_rbox, inner, 0.035f, 0.055f, 0.075f, 1.0f);
}

/* What the prompt says now: title and one line (with {X} glyph tokens); returns 0 when nothing is shown, else a kind
   (1 hold with its progress bar, 2 the question, 3 main menu, 4 card wait, 5 closing, 6 a hint). */
static int compose(const char **title, char *line, size_t n) {
    *title = NULL;
    line[0] = 0;
    switch (q_state) {
    case Q_HOLD:
        *title = "QUIT GAME";
        snprintf(line, n, q_touch ? "Keep holding {M}" : "Keep holding {V} + {M}");   /* (Touch: Menu held = both) */
        return 1;
    case Q_PROMPT:
        *title = "QUIT ACE COMBAT ZERO?";
        if (q_title_ok) snprintf(line, n, "{A} Quit to desktop     {X} Main menu     {B} Back to the game");
        else if (q_enh_ok()) snprintf(line, n, "{A} Quit to desktop     {X} Enhancements     {B} Back to the game");
        else snprintf(line, n, "{A} Quit to desktop      {B} Back to the game");
        return 2;
    case Q_TITLE:
        *title = "QUITTING THE MISSION";
        snprintf(line, n, "Back to the main menu...");
        return 3;
    case Q_SAVING:
        *title = "QUITTING";
        snprintf(line, n, "Waiting for the memory card...");
        return 4;
    case Q_CLOSING:
        *title = "QUITTING";
        snprintf(line, n, "Closing the game...");
        return 5;
    }
    if (q_hint_until > 0.0) {
        if (q_hint_kind == 4) { *title = tq_title; return 7; }                              /* Touch's line (bug #314) */
        if (q_hint_kind == 3) { *title = tq_title; snprintf(line, n, "%s", tq_line); }      /* Touch (bug #239) */
        else if (q_hint_kind == 1) {
            *title = "TO QUIT THE GAME";
            snprintf(line, n, q_touch ? "Pause first {M}, then hold {M}" : "Pause first {M}, then hold {V} + {M}");
        }
        else { *title = "NO MAIN MENU FROM HERE"; snprintf(line, n, "Pause the mission first, or quit to desktop"); }
        return 6;
    }
    return 0;
}

const char *ps2_quit_overlay_text(void) {
    const char *t;
    char line[256], *o = q_text;
    if (!compose(&t, line, sizeof line)) return NULL;
    size_t left = sizeof q_text;
    int k = snprintf(o, left, "%s\n", t);
    o += k; left -= (size_t)k;
    for (const char *p = line; *p && left > 12; ) {          /* glyph tokens as words for the window */
        if (p[0] == '{' && p[1] && p[2] == '}') {
            const char *w = "?";
            switch (p[1]) {
            case 'A': w = q_ps ? "[Cross]" : "[A]"; break;
            case 'B': w = q_ps ? "[Circle]" : "[B]"; break;
            case 'Y': w = q_ps ? "[Triangle]" : "[Y]"; break;
            case 'X': w = q_ps ? "[Square]" : "[X]"; break;
            case 'V': w = q_ps ? "[Select]" : q_touch ? "[Left stick press]" : "[View]"; break;
            case 'M': w = q_ps ? "[Start]" : "[Menu]"; break;
            }
            k = snprintf(o, left, "%s", w);
            o += k; left -= (size_t)k;
            p += 3;
        } else { *o++ = *p++; left--; *o = 0; }
    }
    if (q_state == Q_HOLD) snprintf(o, left, " (%d%%)", (int)(q_hold_frac * 100.0f));
    return q_text;
}

static void pg_render(void);
static void pg_render_body(void);
static void hud_line_render(const char *s);
static u32 pg_gen = 1;

/* ACZ_DEBUG_ENH=1 [debug]: each new headset overlay image (the page or the quit prompt) as out/enh_page_NNN.ppm (over
   a mid grey, a stand-in for the game behind it) and its alpha out/enh_page_NNN_a.pgm -- the quad layer is not part
   of the eye dumps (tools: a private script puts it into one) */
static void ovl_debug_dump(void) {
    static int dbg = -1, n;
    if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_ENH"); dbg = e && *e == '1'; }
    if (!dbg || n >= 400) return;
    char nm[64];
    snprintf(nm, sizeof nm, "out/enh_page_%03d.ppm", ++n);
    FILE *fo = fopen(nm, "wb");
    if (!fo) return;
    fprintf(fo, "P6\n%d %d\n255\n", OVL_W, OVL_H);
    for (size_t i = 0; i < (size_t)OVL_W * OVL_H; i++) {
        const u8 *p = ovl_px + 4 * i;
        for (int c = 0; c < 3; c++) fputc(p[c] + (96 * (255 - p[3]) + 127) / 255, fo);
    }
    fclose(fo);
    ps2_log("quit: overlay image %s (%s)", nm, pg_vis ? "ENHANCEMENTS page" : "prompt");
    snprintf(nm, sizeof nm, "out/enh_page_%03d_a.pgm", n);
    if ((fo = fopen(nm, "wb"))) {
        fprintf(fo, "P5\n%d %d\n255\n", OVL_W, OVL_H);
        for (size_t i = 0; i < (size_t)OVL_W * OVL_H; i++) fputc(ovl_px[4 * i + 3], fo);
        fclose(fo);
    }
}

int ps2_quit_overlay_image(const u8 **rgba, u32 *w, u32 *h, u32 *gen) {
    const char *title;
    char line[256];
    if (pg_vis && !pg_drawn_by_game()) {              /* the ENHANCEMENTS page */
        u32 g = 0x80000000u | pg_gen;
        if (ovl_built_gen != g) {
            if (!font_state) font_load();
            pg_render();
            ovl_built_gen = g;
            ovl_debug_dump();
        }
        *rgba = ovl_px; *w = OVL_W; *h = OVL_H; *gen = g;
        return 1;
    }
    int kind = compose(&title, line, sizeof line);
    if (!kind) return 0;
    if (kind == 7) {                                  /* one line in the game's font, no panel */
        if (ovl_built_gen != q_gen) {
            if (!font_state) font_load();
            hud_line_render(title);
            ovl_built_gen = q_gen;
            ovl_debug_dump();
        }
        *rgba = ovl_px; *w = OVL_W; *h = OVL_H; *gen = q_gen;
        return 1;
    }
    if (ovl_built_gen != q_gen) {
        if (!font_state) font_load();
        panel();
        float ty = PR_Y0 + (line[0] || kind == 1 ? 98.0f : 162.0f);
        text_c(OVL_W * 0.5f, ty, title, 56.0f, 0.97f, 0.97f, 0.97f, 1.0f);
        if (line[0]) {
            float px = 38.0f;
            while (px > 24.0f && rich(OVL_W * 0.5f, 0.0f, line, px, 0) > OVL_W - 70.0f) px -= 2.0f;
            rich(OVL_W * 0.5f, PR_Y0 + (kind == 1 ? 178.0f : 200.0f), line, px, 1);
        }
        if (kind == 1) {                              /* hold: progress bar */
            float bw = 640.0f, f = q_hold_frac < 0 ? 0 : q_hold_frac > 1 ? 1 : q_hold_frac;
            float bg[5] = { OVL_W * 0.5f, PR_Y0 + 230.0f, bw * 0.5f, 8.0f, 8.0f };
            shape(OVL_W * 0.5f - bw * 0.5f, PR_Y0 + 214, OVL_W * 0.5f + bw * 0.5f, PR_Y0 + 246, sd_rbox, bg, 0.20f, 0.24f, 0.28f, 1.0f);
            if (f > 0.03f) {
                float fg[5] = { OVL_W * 0.5f - bw * 0.5f + bw * f * 0.5f, PR_Y0 + 230.0f, bw * f * 0.5f, 8.0f, 8.0f };
                shape(OVL_W * 0.5f - bw * 0.5f, PR_Y0 + 214, OVL_W * 0.5f + bw * 0.5f, PR_Y0 + 246, sd_rbox, fg, 0.55f, 0.85f, 0.95f, 1.0f);
            }
        }
        ovl_built_gen = q_gen;
        if (kind != 1) ovl_debug_dump();              /* (not every step of the hold bar) */
    }
    *rgba = ovl_px; *w = OVL_W; *h = OVL_H; *gen = q_gen;
    return 1;
}

float ps2_quit_overlay_width_m(void) {
    static float pw = -1.0f;
    if (pw < 0.0f) {
        const char *e = getenv("ACZ_ENH_PAGE_W");
        pw = e && *e ? (float)atof(e) : 1.5f;
        if (!(pw >= 0.3f && pw <= 3.0f)) pw = 1.5f;
    }
    return pg_vis ? pw : q_hint_kind == 4 && q_hint_until > 0.0 && q_state == Q_IDLE ? 0.85f : 0.62f;
}

/* =====================================================================================================================
   The ENHANCEMENTS page
   ===================================================================================================================== */

/* ---- the game's font: its HUD atlas (textures/hud/3F861D67_20_256x256_hud_atlas.png, the 4x replacement of the
   256 x 256 atlas the pause menu draws its texts from). Two fonts: the menu font (11 px rows at v 198 / 210 / 222:
   ' '..'E', 'F'..'f', 'g'..'z') and the small font (8 px, v 22: '0'..'9', 'A'..'Z' and a few signs). Glyph boxes
   (u, width) at 1x as the game's own text entries use them; the game adds 2 px between glyphs. ---- */
typedef struct { short u, v, w, h; } gf_glyph;
static u8 *gf_alpha;                       /* the atlas' coverage, gf_k x 256 square */
static int gf_k, gf_state;                 /* 0 not tried, 1 loaded, -1 missing (system font) */

static void gf_load(void) {
    char p[1100];
    const char *rel = "hud/3F861D67_20_256x256_hud_atlas.png", *dir = getenv("ACZ_TEX_DIR"), *base = SDL_GetBasePath();
    const char *tries[3] = { dir && *dir ? dir : NULL, "textures", NULL };
    char b2[1024];
    if (base) { snprintf(b2, sizeof b2, "%s../../textures", base); tries[2] = b2; }
    gf_state = -1;
    for (int t = 0; t < 3 && gf_state < 0; t++) {
        if (!tries[t]) continue;
        snprintf(p, sizeof p, "%s/%s", tries[t], rel);
        SDL_Surface *s = SDL_LoadPNG(p);
        if (!s) continue;
        SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(s);
        if (!c) continue;
        if (c->w == c->h && c->w >= 256 && c->w % 256 == 0 && SDL_LockSurface(c)) {
            gf_k = c->w / 256;
            gf_alpha = (u8 *)malloc((size_t)c->w * c->h);
            if (gf_alpha) {
                for (int y = 0; y < c->h; y++)
                    for (int x = 0; x < c->w; x++) gf_alpha[(size_t)y * c->w + x] = ((const u8 *)c->pixels)[(size_t)y * c->pitch + 4 * x + 3];
                gf_state = 1;
                ps2_log("quit: ENHANCEMENTS page font: the game's HUD atlas %s (%dx)", p, gf_k);
            }
            SDL_UnlockSurface(c);
        }
        SDL_DestroySurface(c);
    }
    if (gf_state < 0) ps2_log("quit: the game's HUD atlas (textures/%s) not found; the page uses a system font", rel);
}

/* font 0 = menu font, 1 = small font; 0 = found */
static int gf_find(int f, int ch, gf_glyph *g) {
    if (f == 1) {
        static const char sig[] = ".,:;'\"-/[]>";
        static const short su[] = { 216, 218, 220, 222, 224, 226, 230, 236, 242, 247, 252 };
        static const short sw[] = { 1, 1, 1, 1, 1, 3, 5, 5, 4, 4, 4 };
        g->v = 22; g->h = 8; g->w = 5;
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch == ' ') { g->u = -1; g->w = 3; return 0; }
        if (ch >= '0' && ch <= '9') { static const short du[] = { 0, 6, 11, 17, 23, 29, 35, 41, 47, 53 };
                                     g->u = du[ch - '0']; if (ch == '1') g->w = 4; return 0; }
        if (ch >= 'A' && ch <= 'Z') {
            int k = ch - 'A';
            g->u = (short)(59 + 6 * k - (k > 8 ? 2 : 0) + (k > 22 ? 3 : 0));
            if (ch == 'I') g->w = 3;
            if (ch == 'W') g->w = 8;
            return 0;
        }
        const char *q = strchr(sig, ch);
        if (ch && q) { g->u = su[q - sig]; g->w = sw[q - sig]; return 0; }
        return -1;
    }
    g->h = 11; g->w = 7;
    if (ch == ' ') { g->u = -1; g->v = 198; g->w = 3; return 0; }
    if (ch >= '!' && ch <= 'E') {
        static const short u198[] = { 8, 10, 15, 23, 31, 39, 47, 49, 55, 61, 69, 77, 79, 87, 89,     /* ! .. / */
                                      97, 105, 113, 121, 129, 137, 145, 153, 161, 169,             /* 0 .. 9 */
                                      177, 179, 181, 188, 196, 203, 0, 211, 219, 227, 235, 243 };  /* : .. E */
        static const unsigned char w198[] = { 1, 4, 7, 7, 7, 7, 1, 5, 5, 7, 7, 1, 7, 1, 7,
                                              7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 1, 1, 6, 7, 6, 7, 0, 7, 7, 7, 7, 7 };
        int k = ch - '!';
        if (ch == '@' || !w198[k]) return -1;
        g->u = u198[k]; g->v = 198; g->w = w198[k];
        return 0;
    }
    {   static const char c210[] = "FGHIJKLMNOPQRSTUVWXYZ[]^_abcdef";      /* (the yen sign sits between [ and ]) */
        static const short u210[] = { 0, 8, 16, 24, 29, 37, 45, 53, 62, 70, 78, 86, 95, 103, 111, 119, 127, 135, 147, 155,
                                      163, 171, 185, 191, 197, 205, 213, 221, 229, 237, 245 };
        static const unsigned char w210[] = { 7, 7, 7, 4, 7, 7, 7, 8, 7, 7, 7, 8, 7, 7, 7, 7, 7, 11, 7, 7,
                                              7, 5, 5, 5, 7, 7, 7, 7, 7, 7, 7 };
        const char *q = ch ? strchr(c210, ch) : NULL;
        if (q) { g->u = u210[q - c210]; g->v = 210; g->w = w210[q - c210]; return 0; }
    }
    if (ch >= 'g' && ch <= 'z') {
        static const short u222[] = { 0, 8, 16, 22, 27, 35, 38, 49, 57, 65, 73, 81, 89, 97, 105, 113, 121, 132, 140, 148 };
        static const unsigned char w222[] = { 7, 7, 2, 5, 7, 2, 10, 7, 7, 7, 7, 7, 7, 7, 7, 7, 10, 7, 7, 7 };
        int k = ch - 'g';
        g->u = u222[k]; g->v = 222; g->w = w222[k]; g->h = 14;      /* descenders reach v 235 */
        return 0;
    }
    return -1;
}

/* one glyph scaled from the 4x atlas to s image pixels per atlas pixel, box-filtered, at image (x, y) (top left) */
/* one glyph scaled from the 4x atlas to s image pixels per atlas pixel, box-filtered, at image (x, y) (top left);
   the filtered coverage is kept per glyph, scale and quarter-pixel phase (a page redraw then only blends) */
typedef struct { short u, v, w, h; unsigned char px, py; float s; int cw, ch; u8 *cov; } gf_cached;
static gf_cached gf_cache[640];
static int gf_ncache;
static const gf_cached *gf_cover(const gf_glyph *g, float fx, float fy, float s) {
    unsigned char px = (unsigned char)lrintf(fx * 4.0f) & 3u, py = (unsigned char)lrintf(fy * 4.0f) & 3u;
    for (int i = 0; i < gf_ncache; i++) {
        const gf_cached *c = &gf_cache[i];
        if (c->u == g->u && c->v == g->v && c->w == g->w && c->h == g->h && c->px == px && c->py == py && c->s == s)
            return c;
    }
    float ox = (float)px * 0.25f, oy = (float)py * 0.25f, q = (float)gf_k / s;
    int cw = (int)ceilf(ox + g->w * s), ch = (int)ceilf(oy + g->h * s);
    int ax0 = g->u * gf_k, ay0 = g->v * gf_k, aw = g->w * gf_k, ah = g->h * gf_k, side = 256 * gf_k;
    u8 *cov = (u8 *)calloc((size_t)cw * (size_t)ch, 1);
    if (!cov) return NULL;
    for (int yy = 0; yy < ch; yy++) {
        float sy0 = ((float)yy - oy) * q, sy1 = sy0 + q;
        if (sy0 < 0) sy0 = 0;
        if (sy1 > (float)ah) sy1 = (float)ah;
        if (sy1 <= sy0) continue;
        for (int xx = 0; xx < cw; xx++) {
            float sx0 = ((float)xx - ox) * q, sx1 = sx0 + q;
            if (sx0 < 0) sx0 = 0;
            if (sx1 > (float)aw) sx1 = (float)aw;
            if (sx1 <= sx0) continue;
            float sum = 0.0f;
            for (int iy = (int)sy0; iy < (int)ceilf(sy1); iy++) {
                float wy = fminf(sy1, (float)iy + 1.0f) - fmaxf(sy0, (float)iy);
                if (wy <= 0 || ay0 + iy >= side) continue;
                const u8 *row = gf_alpha + (size_t)(ay0 + iy) * side + ax0;
                for (int ix = (int)sx0; ix < (int)ceilf(sx1); ix++) {
                    float wx = fminf(sx1, (float)ix + 1.0f) - fmaxf(sx0, (float)ix);
                    if (wx > 0 && ax0 + ix < side) sum += wx * wy * (float)row[ix];
                }
            }
            float c = sum / (q * q);                    /* 0..255: the share of the image pixel the glyph covers */
            cov[yy * cw + xx] = (u8)(c > 255.0f ? 255.0f : c + 0.5f);
        }
    }
    if (gf_ncache >= (int)(sizeof gf_cache / sizeof gf_cache[0])) {    /* full: replace the oldest */
        free(gf_cache[0].cov);
        memmove(&gf_cache[0], &gf_cache[1], sizeof gf_cache[0] * (size_t)(gf_ncache - 1));
        gf_ncache--;
    }
    gf_cached *c = &gf_cache[gf_ncache++];
    c->u = g->u; c->v = g->v; c->w = g->w; c->h = g->h; c->px = px; c->py = py; c->s = s; c->cw = cw; c->ch = ch;
    c->cov = cov;
    return c;
}
static void gf_blit(const gf_glyph *g, float x, float y, float s, float r, float gg, float b, float a) {
    if (g->u < 0 || !gf_alpha) return;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    const gf_cached *c = gf_cover(g, x - (float)x0, y - (float)y0, s);
    if (!c) return;
    for (int yy = 0; yy < c->ch; yy++)
        for (int xx = 0; xx < c->cw; xx++) {
            u8 v = c->cov[yy * c->cw + xx];
            if (v) px_over(x0 + xx, y0 + yy, r, gg, b, a * (float)v * (1.0f / 255.0f));
        }
}

#define PG_S      2.5f                   /* image pixels per PS2 pixel: the page is 409.6 x 307.2 PS2 pixels */
#define PG_SP     2.0f                   /* the game's glyph spacing */
/* width of a string in PS2 pixels */
static float gf_width(int f, const char *s) {
    float w = 0.0f;
    if (gf_state != 1) return text_width(s, (f ? 8.0f : 11.0f) * PG_S * 1.15f) / PG_S;
    for (; *s; s++) {
        gf_glyph g;
        if (gf_find(f, (unsigned char)*s, &g) < 0) gf_find(f, '?', &g);
        w += (float)g.w + (s[1] ? PG_SP : 0.0f);
    }
    return w;
}
/* a string in the game's font with its top left at PS2 (x, y) of the page; returns the end x */
static float gf_text(int f, float x, float y, const char *s, float r, float g, float b, float a) {
    if (gf_state != 1) {                              /* no atlas: the system font, about the game font's size */
        float px = (f ? 8.0f : 11.0f) * PG_S * 1.15f;
        return text(x * PG_S, (y + (f ? 7.5f : 9.0f)) * PG_S, s, px, r, g, b, a) / PG_S;
    }
    for (; *s; s++) {
        gf_glyph gl;
        if (gf_find(f, (unsigned char)*s, &gl) < 0) gf_find(f, '?', &gl);
        gf_blit(&gl, x * PG_S, y * PG_S, PG_S, r, g, b, a);
        x += (float)gl.w + PG_SP;
    }
    return x - PG_SP;
}

/* ACZ_TOUCH=auto's line (bug #314, ps2_quit_touch_hud_line): the menu font at 3 image pixels per game pixel on the 0.85 m
   quad (letters ~1.5 deg high, the 34-letter line ~35 deg wide), the flight HUD's green on a faint plate, ~8 deg below
   the view's centre. The quad layer is head-locked like the prompt. */
static void hud_line_render(const char *s) {
    if (!gf_state) gf_load();
    memset(ovl_px, 0, sizeof ovl_px);
    float y = (float)OVL_H * 0.72f;
    if (gf_state != 1) {
        float w = text_width(s, 36.0f);
        float pl[5] = { OVL_W * 0.5f, y, w * 0.5f + 18.0f, 24.0f, 8.0f };
        shape(pl[0] - pl[2] - 4.0f, y - 30.0f, pl[0] + pl[2] + 4.0f, y + 30.0f, sd_rbox, pl, 0.035f, 0.055f, 0.075f, 0.55f);
        text_c(OVL_W * 0.5f, y + 12.0f, s, 36.0f, 0.40f, 0.92f, 0.55f, 1.0f);
        return;
    }
    const float k = 3.0f;
    float wg = gf_width(0, s), x = ((float)OVL_W - wg * k) * 0.5f;
    if (x < 8.0f) x = 8.0f;
    float pl[5] = { OVL_W * 0.5f, y + 5.5f * k, wg * k * 0.5f + 18.0f, 5.5f * k + 8.0f, 8.0f };
    shape(pl[0] - pl[2] - 4.0f, pl[1] - pl[3] - 4.0f, pl[0] + pl[2] + 4.0f, pl[1] + pl[3] + 4.0f, sd_rbox, pl, 0.035f, 0.055f, 0.075f, 0.55f);
    for (; *s; s++) {
        gf_glyph gl;
        if (gf_find(0, (unsigned char)*s, &gl) < 0) gf_find(0, '?', &gl);
        gf_blit(&gl, x, y, k, 0.40f, 0.92f, 0.55f, 1.0f);
        x += ((float)gl.w + PG_SP) * k;
    }
}

/* ---- page state (renderer thread) ---- */
enum { PG_CATS = 1, PG_ITEMS, PG_VALUES, PG_NOTICE, PG_CARD };
#define PG_MAXITEMS 160                    /* a category's rows */
#define PG_FITROWS  10                     /* rows that fit above the help line: PG_ROW0 + 9 x PG_PITCH + 13.5 < PG_RULE */
#define PG_OLDROWS  12                     /* ACZ_ENH_PAGE_ROWS=0: the old fixed list (rows 11-12 over the help line) */
static int pg_level, pg_from, pg_cat, pg_row, pg_val, pg_notice;
static int pg_items[PG_MAXITEMS], pg_nitems;
static int pg_top, pg_cat_top;             /* the first row shown of the items / of the categories (scrolling) */
static int pg_wait;                        /* the button that opened the page must be released first */
static int pg_prev_ok, pg_prev_back, pg_prev_start, pg_prev_dir;
static double pg_rep_t, pg_open_t, pg_gone_t;
static int pg_restart_changed;             /* an item waiting for a restart was changed on this visit */
static char pg_msg[200];                   /* the help line after a change (until the cursor moves) */
static int tc_back;                        /* closing the card goes back to: 0 the game, PG_ITEMS the ENHANCEMENTS page */
static int tc_view;                        /* the mode the card draws: PS2_TOUCH_STICK or PS2_TOUCH_PAD */
static int tc_auto;                        /* opened by itself (ACZ_TOUCH_CARD) */
static void tc_level(void);
static void tc_close(void);
static void tc_render(void);

static void pg_dirty(void) { pg_gen++; }
/* RESET ALL TO DEFAULTS asks first (ACZ_ENH_RESET_CONFIRM; the owner, 2026-11-05): it now also resets the key bindings. The first
   press changes nothing: the row reads PRESS A AGAIN, the help says what will be reset. A second press within PG_RESET_WAIT resets;
   moving off the row, B, Menu, the page closing or the time running out cancel it. F4 keeps its own popup. */
#define PG_RESET_WAIT 4.0
static double pg_reset_until;              /* waiting for the second press until this time; 0: not asked */
static void pg_reset_cancel(const char *why) {
    if (pg_reset_until == 0.0) return;
    pg_reset_until = 0.0;
    pg_msg[0] = 0;
    pg_dirty();
    ps2_log("quit: ENHANCEMENTS: RESET ALL cancelled (%s): nothing changed", why);
}
/* the reset row's press: 1 = it only asked; 0 = reset now */
static int pg_reset_press(double now) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ACZ_ENH_RESET_CONFIRM"); on = !(e && *e == '0'); }
    if (!on) return 0;
    if (pg_reset_until != 0.0) {
        pg_reset_until = 0.0;
        ps2_log("quit: ENHANCEMENTS: RESET ALL confirmed by the second press");
        return 0;
    }
    pg_reset_until = now + PG_RESET_WAIT;
    snprintf(pg_msg, sizeof pg_msg, "A second press resets every row of this page and every F4 setting, key bindings included.");
    pg_dirty();
    ps2_log("quit: ENHANCEMENTS: RESET ALL asked: a second press within %.0f s resets, nothing changed yet", PG_RESET_WAIT);
    return 1;
}
/* the reset row's value while it waits, else NULL (ps: the overlay on a PlayStation pad) */
static const char *pg_reset_prompt(int i, int ps) {
    if (pg_reset_until == 0.0 || strcmp(ps2_enh_item_at(i)->key, "reset_all")) return NULL;
    return ps ? "PRESS CROSS AGAIN" : "PRESS A AGAIN";
}
static int pg_drawn_by_game(void) { return pg_native && pg_level != PG_CARD; }
/* ACZ_ENH_PAGE_ROWS: rows shown at once, the list scrolling with the cursor (default PG_FITROWS; 4..PG_FITROWS);
   0 = the old fixed list of the first PG_OLDROWS rows */
static int pg_rows(void) {
    static int v = -1;
    if (pg_native) return PS2_ENH_SNAP_ROWS;   /* the game's page: seven rows at a time */
    if (v < 0) {
        const char *e = getenv("ACZ_ENH_PAGE_ROWS");
        char *end;
        long k = e && *e ? strtol(e, &end, 10) : -1;
        v = PG_FITROWS;
        if (e && *e && end != e && !*end) v = k == 0 ? 0 : k < 4 ? 4 : k > PG_FITROWS ? PG_FITROWS : (int)k;
        ps2_log("quit: ENHANCEMENTS page lists %s (ACZ_ENH_PAGE_ROWS=%d)", v ? "scroll with the cursor" :
                "show their first 12 rows only (the old page)", v);
    }
    return v;
}
static int pg_shown(int n) { int v = pg_rows(); return v == 0 || n < v ? n : v; }   /* rows drawn of a list of n */
/* the first row shown so that row stays in view, with one row of the list beyond it while the list goes on */
static void pg_follow(int row, int n, int *top) {
    int v = pg_rows();
    if (v == 0 || n <= v) { *top = 0; return; }
    if (*top > row - 1) *top = row - 1;
    if (*top < row + 2 - v) *top = row + 2 - v;
    if (*top > n - v) *top = n - v;
    if (*top < 0) *top = 0;
}
static void pg_fill_items(void) {
    int cap = pg_rows() ? PG_MAXITEMS : PG_OLDROWS, n = 0;
    pg_nitems = 0;
    for (int i = 0; i < ps2_enh_count(); i++)
        if (ps2_enh_item_at(i)->cat == pg_cat && n++ < cap) pg_items[pg_nitems++] = i;
    if (n > pg_nitems) ps2_log("quit: ENHANCEMENTS > %s: %d of its %d rows can be reached", ps2_enh_cat_name(pg_cat),
                               pg_nitems, n);
    pg_row = pg_top = 0;
}
static void pg_open_now(int from, double now) {
    if (!q_enh_ok()) return;
    pg_native = __atomic_exchange_n(&pg_native_req, 0, __ATOMIC_ACQ_REL);
    pg_follow(pg_cat, PS2_ENH_CATS, &pg_cat_top);   /* (the visits may list different numbers of rows) */
    pg_from = from;
    pg_level = PG_CATS;
    pg_wait = 1;
    pg_msg[0] = 0;
    pg_restart_changed = 0;
    pg_open_t = now;
    pg_gone_t = 0.0;
    pg_prev_dir = 0;
    pg_vis = 1;
    pg_dirty();
    ps2_log("quit: ENHANCEMENTS page opened (%s)", from == PS2_ENH_FROM_PAUSE ? "pause menu" : "View + Menu panel");
}
static void pg_close(int quit) {
    pg_reset_cancel("the page closed");
    pg_vis = 0;
    pg_level = 0;
    q_swallow = 1;                          /* the button that closed it must not reach the game (B/Y: its cancel) */
    pg_dirty();
    ps2_log("quit: ENHANCEMENTS page closed%s (%d settings wait for a restart, %d for the next mission)",
            quit ? ": quit to apply them" : "", ps2_enh_pending_count(), ps2_enh_mission_count());
    if (quit) ps2_quit_request("ENHANCEMENTS: restart to apply");
}
/* leaving the page: after a change that needs a restart, the notice first */
static void pg_leave(void) {
    if (pg_restart_changed && ps2_enh_pending_count() > 0) {
        pg_level = PG_NOTICE;
        pg_notice = 1;                      /* CONTINUE */
        pg_restart_changed = 0;
        pg_dirty();
        return;
    }
    pg_close(0);
}
static void pg_change(int i, int c) {
    const ps2_enh_item *it = ps2_enh_item_at(i);
    int r = ps2_enh_set(i, c);
    if (r < 0) {
        char hb[96];
        const char *o = ps2_enh_held_by(i, hb, sizeof hb);
        snprintf(pg_msg, sizeof pg_msg, "Set by %s when the game started: it cannot be changed here.", o ? o : "?");
    } else if (r == PS2_ENH_RESTART) {
        pg_restart_changed = 1;
        snprintf(pg_msg, sizeof pg_msg, "Saved. Restart the game to apply.");
    } else if (r == PS2_ENH_MISSION) snprintf(pg_msg, sizeof pg_msg, ps2_enh_retry_waits(i) ? "Saved. Takes effect when the next mission starts."
                                              : "Saved. Takes effect at the next mission or RETRY.");
    else if (r == PS2_ENH_LIVE) snprintf(pg_msg, sizeof pg_msg, "Saved. In effect now.");
    else if (r == PS2_ENH_ACTION) {
        extern const char *ps2_video_rumble_note(void);       /* ps2_video.c: TEST VIBRATION's answer */
        extern const char *ps2_hotas_neutral_note(void);      /* ps2_hotas.c: SET LEVER NEUTRAL's */
        snprintf(pg_msg, sizeof pg_msg, "%s", !strcmp(it->key, "recentre_now") ? "View recentred."
                 : !strcmp(it->key, "rumble_test") ? ps2_video_rumble_note()
                 : !strcmp(it->key, "lever_neutral") ? ps2_hotas_neutral_note()
                 : "Everything is back to its default, F4's settings too. Saved; some wait for the next mission or start.");
    }
    if (r == PS2_ENH_ACTION && strcmp(it->key, "recentre_now") && strcmp(it->key, "rumble_test") && strcmp(it->key, "lever_neutral"))
        pg_restart_changed = 1;
    pg_dirty();
}
/* the saved choice among those the page offers (a combo row can hold a value of the file only: BUILDINGS' red_roofs) */
static int pg_choice(int i) { int c = ps2_enh_choice(i); return c < 0 || c >= ps2_enh_item_at(i)->n ? 0 : c; }

/* one input poll while the page is up */
static int pg_pad(ps2_pad_state *st, int a, int b, int y, double now) {
    u16 bt = st->buttons;
    int ok = (bt & (1u << PS2_ACT_CROSS)) || a;
    int back = (bt & ((1u << PS2_ACT_CIRCLE) | (1u << PS2_ACT_TRIANGLE))) || b || y;
    int start = (bt & (1u << PS2_ACT_START)) != 0;
    int dir = (bt & (1u << PS2_ACT_UP)) || st->ly < 56 ? 1 : (bt & (1u << PS2_ACT_DOWN)) || st->ly > 200 ? 2
            : (bt & (1u << PS2_ACT_LEFT)) || st->lx < 56 ? 3 : (bt & (1u << PS2_ACT_RIGHT)) || st->lx > 200 ? 4 : 0;
    /* the page belongs to its menu: it closes if the game left it (pause ended, a flight started) */
    int gs = game_state();
    if ((pg_from == PS2_ENH_FROM_PAUSE && gs != PS2_QUIT_GAME_PAUSED) || gs == PS2_QUIT_GAME_FLYING) {
        if (pg_gone_t == 0.0) pg_gone_t = now;
        else if (now - pg_gone_t > 1.0) { pg_close(0); return 1; }
    } else pg_gone_t = 0.0;
    if (pg_wait) {
        if (!ok && !back && !start && !dir) pg_wait = 0;
        pg_prev_ok = ok; pg_prev_back = back; pg_prev_start = start; pg_prev_dir = dir;
        return 1;
    }
    if (pg_reset_until != 0.0 && now >= pg_reset_until) pg_reset_cancel("no second press in time");
    int okp = ok && !pg_prev_ok, backp = back && !pg_prev_back, startp = start && !pg_prev_start, step = 0;
    if (dir && dir != pg_prev_dir) { step = dir; pg_rep_t = now + 0.40; }
    else if (dir && now >= pg_rep_t) { step = dir; pg_rep_t = now + 0.11; }
    pg_prev_ok = ok; pg_prev_back = back; pg_prev_start = start; pg_prev_dir = dir;
    if (pg_level == PG_CARD) {                       /* the TOUCH CONTROLS card: any of A B Y Menu closes it */
        if (startp || okp || backp) tc_close();
        else if (step == 3 || step == 4) { tc_view = tc_view == PS2_TOUCH_STICK ? PS2_TOUCH_PAD : PS2_TOUCH_STICK; pg_dirty(); }
        return 1;
    }
    if (startp) { pg_reset_cancel("Menu pressed"); pg_leave(); return 1; }
    switch (pg_level) {
    case PG_CATS:
        if (step == 1 || step == 2) {
            pg_cat = (pg_cat + (step == 1 ? PS2_ENH_CATS - 1 : 1)) % PS2_ENH_CATS;
            pg_follow(pg_cat, PS2_ENH_CATS, &pg_cat_top);
            pg_msg[0] = 0; pg_dirty();
        } else if (okp || step == 4) { pg_fill_items(); pg_level = PG_ITEMS; pg_msg[0] = 0; pg_dirty(); }
        else if (backp) pg_leave();
        break;
    case PG_ITEMS: {
        int i = pg_items[pg_row];
        const ps2_enh_item *it = ps2_enh_item_at(i);
        if (step == 1 || step == 2) {                /* wraps at both ends, as before; the list follows the cursor */
            pg_reset_cancel("moved off the row");
            pg_row = (pg_row + (step == 1 ? pg_nitems - 1 : 1)) % pg_nitems;
            pg_follow(pg_row, pg_nitems, &pg_top);
            pg_msg[0] = 0; pg_dirty();
        }
        else if ((step == 3 || step == 4) && it->apply != PS2_ENH_ACTION) {
            int c = pg_choice(i) + (step == 3 ? it->n - 1 : 1);
            pg_change(i, c % it->n);
        } else if (okp) {
            if (!strcmp(it->key, "touch_card")) { tc_back = PG_ITEMS; tc_level(); }   /* SHOW TOUCH CONTROLS */
            else if (!strcmp(it->key, "reset_all") && pg_reset_press(now)) { }          /* (only asked) */
            else if (it->apply == PS2_ENH_ACTION) pg_change(i, 0);
            else if (ps2_enh_override(i) && strcmp(it->key, "pad_layout")) pg_change(i, pg_choice(i));   /* says why */
            else { pg_val = pg_choice(i); pg_level = PG_VALUES; pg_msg[0] = 0; pg_dirty(); }
        } else if (backp) { pg_reset_cancel("B pressed"); pg_level = PG_CATS; pg_msg[0] = 0; pg_dirty(); }
        break; }
    case PG_VALUES: {
        int i = pg_items[pg_row];
        const ps2_enh_item *it = ps2_enh_item_at(i);
        if (step == 1 || step == 2) { pg_val = (pg_val + (step == 1 ? it->n - 1 : 1)) % it->n; pg_dirty(); }
        else if (okp) { pg_level = PG_ITEMS; pg_change(i, pg_val); }
        else if (backp) { pg_level = PG_ITEMS; pg_dirty(); }
        break; }
    case PG_NOTICE:
        if (step == 1 || step == 2) { pg_notice ^= 1; pg_dirty(); }
        else if (okp) pg_close(pg_notice == 0);
        else if (backp) pg_close(0);
        break;
    }
    return 1;
}

/* ---- drawing (PS2 pixel coordinates of the page; the image is PG_S times larger) ---- */
#define PG_ROW0   29.0f                  /* the first row's top */
#define PG_PITCH  23.0f
#define PG_LX     14.0f                  /* labels */
#define PG_VX     216.0f                 /* values */
#define PG_VXMAX  262.0f                 /* ... or right of the category's longest label (scrolling page), up to here */
#define PG_RULE   255.0f                 /* the rule over the help lines */
static const float pg_yel[3] = { 0.96f, 0.95f, 0.47f }, pg_help[3] = { 0.86f, 0.85f, 0.28f };
static const float pg_dim[3] = { 0.42f, 0.42f, 0.20f }, pg_box[3] = { 1.0f, 1.0f, 0.58f };

static void pg_rect(float x0, float y0, float x1, float y1, float r, float g, float b, float a) {   /* PS2 px */
    int X0 = (int)lrintf(x0 * PG_S), Y0 = (int)lrintf(y0 * PG_S), X1 = (int)lrintf(x1 * PG_S), Y1 = (int)lrintf(y1 * PG_S);
    for (int y = Y0; y < Y1; y++)
        for (int x = X0; x < X1; x++) px_over(x, y, r, g, b, a);
}
static void pg_frame(float x0, float y0, float x1, float y1, const float *c) {     /* the game's 1-px cursor box */
    pg_rect(x0, y0, x1, y0 + 0.8f, c[0], c[1], c[2], 1.0f);
    pg_rect(x0, y1 - 0.8f, x1, y1, c[0], c[1], c[2], 1.0f);
    pg_rect(x0, y0, x0 + 0.8f, y1, c[0], c[1], c[2], 1.0f);
    pg_rect(x1 - 0.8f, y0, x1, y1, c[0], c[1], c[2], 1.0f);
}
/* "more rows above / below": a small filled triangle at the right edge of a shown row (slot), pointing up or down */
#define PG_MX     396.0f
static void pg_more(int slot, int down, const float *c) {
    float cx = PG_MX * PG_S, cy = (PG_ROW0 + PG_PITCH * (float)slot + 5.5f) * PG_S;
    float hw = 4.0f * PG_S, hh = 2.75f * PG_S, s = down ? 1.0f : -1.0f;
    float tri[6] = { cx, cy + s * hh, cx - hw, cy - s * hh, cx + hw, cy - s * hh };
    shape(cx - hw, cy - hh, cx + hw, cy + hh, sd_tri, tri, c[0], c[1], c[2], 1.0f);
}
/* the game's panel: dark green-grey, its fine vertical grid (one light and two dark columns every 4 PS2 pixels) */
static u8 *pg_bg;                          /* the panel, made once (it is the same for every image) */
static void pg_background(void) {
    if (pg_bg) { memcpy(ovl_px, pg_bg, sizeof ovl_px); return; }
    static const float col[10][3] = {
        { 0.090f, 0.145f, 0.133f }, { 0.024f, 0.067f, 0.039f }, { 0.055f, 0.102f, 0.078f }, { 0.055f, 0.106f, 0.082f },
        { 0.059f, 0.106f, 0.086f }, { 0.051f, 0.102f, 0.078f }, { 0.055f, 0.102f, 0.078f }, { 0.020f, 0.063f, 0.031f },
        { 0.133f, 0.192f, 0.192f }, { 0.060f, 0.110f, 0.090f } };
    for (int y = 0; y < OVL_H; y++)
        for (int x = 0; x < OVL_W; x++) {
            const float *c = col[x % 10];
            float k = (y / 3) % 2 ? 1.0f : 0.92f;     /* faint scan lines */
            u8 *p = ovl_px + ((size_t)y * OVL_W + x) * 4;
            float a = 0.90f;
            p[0] = (u8)(c[0] * k * a * 255.0f + 0.5f); p[1] = (u8)(c[1] * k * a * 255.0f + 0.5f);
            p[2] = (u8)(c[2] * k * a * 255.0f + 0.5f); p[3] = (u8)(a * 255.0f + 0.5f);
        }
    const float fy[3] = { 0.99f, 0.99f, 0.56f };
    pg_rect(2.0f, 2.0f, 407.6f, 2.8f, fy[0], fy[1], fy[2], 1.0f);          /* the frame */
    pg_rect(2.0f, 304.4f, 407.6f, 305.2f, fy[0], fy[1], fy[2], 1.0f);
    pg_rect(2.0f, 2.0f, 2.8f, 305.2f, fy[0], fy[1], fy[2], 1.0f);
    pg_rect(406.8f, 2.0f, 407.6f, 305.2f, fy[0], fy[1], fy[2], 1.0f);
    if ((pg_bg = (u8 *)malloc(sizeof ovl_px))) memcpy(pg_bg, ovl_px, sizeof ovl_px);
}
/* help text wrapped onto up to three lines under the rule (draw = 0: nothing drawn, up to 9 lines; returns the lines) */
static int pg_help_wrap(const char *s, int draw) {
    char line[200];
    float maxw = 409.6f - 2.0f * PG_LX, y = PG_RULE + 6.0f;
    int ln;
    for (ln = 0; ln < (draw ? 3 : 9) && *s; ln++) {
        size_t fit = 0, e = 0;
        for (;;) {                                     /* as many whole words as fit */
            while (s[e] && s[e] != ' ') e++;
            size_t n = e < sizeof line - 1 ? e : sizeof line - 1;
            memcpy(line, s, n); line[n] = 0;
            if (fit && gf_width(0, line) > maxw) break;
            fit = n;
            if (!s[e] || n < e) break;
            e++;
        }
        memcpy(line, s, fit); line[fit] = 0;
        if (draw) gf_text(0, PG_LX, y, line, pg_help[0], pg_help[1], pg_help[2], 1.0f);
        s += fit;
        while (*s == ' ') s++;
        y += 12.5f;
    }
    return ln;
}
static void pg_help_text(const char *s) { pg_help_wrap(s, 1); }
/* ACZ_DEBUG_ENH: how every page row's help fits the overlay's three lines, with what the page adds to it */
static void pg_help_report(void) {
    int over = 0, maxl = 0, rows = 0;
    for (int i = 0; i < ps2_enh_count(); i++) {
        const ps2_enh_item *it = ps2_enh_item_at(i);
        char t[400], hp[300];
        if (it->cat >= PS2_ENH_CATS && strcmp(it->key, "throttle_lever") && strcmp(it->key, "lever_neutral")) continue;
        snprintf(t, sizeof t, "%s%s", ps2_enh_help(i, hp, sizeof hp, 0),
                 it->apply == PS2_ENH_RESTART && strcmp(it->key, "quality_preset") ? " * Saved: restart the game to apply."
                 : it->apply == PS2_ENH_MISSION ? " Saved for next mission or RETRY." : "");
        int l = pg_help_wrap(t, 0);
        rows++;
        if (l > maxl) maxl = l;
        if (l > 3) { over++; ps2_log("quit: ENHANCEMENTS overlay: help of %s needs %d lines (%d letters)", it->label, l, (int)strlen(t)); }
    }
    ps2_log("quit: ENHANCEMENTS overlay: help fit: %d rows, %d longer than 3 lines (the longest %d)", rows, over, maxl);
    for (int c = 0; c < PS2_ENH_CATS; c++)
        ps2_log("quit: ENHANCEMENTS overlay: %s help %d line(s), %.0f of %.0f px: %s", ps2_enh_cat_name(c), pg_help_wrap(ps2_enh_cat_help(c), 0),
                (double)gf_width(0, ps2_enh_cat_help(c)), 409.6 - 2.0 * PG_LX, ps2_enh_cat_help(c));
}
/* "{A} CHANGE  {B} BACK" right-aligned at x_right: the prompt's controller discs and the small font */
static void pg_hint(float x_right, float y, const char *s) {
    const float rad = 5.2f;                            /* disc radius, PS2 px */
    float w = 0.0f;
    for (int pass = 0; pass < 2; pass++) {
        float x = x_right - w;
        for (const char *p = s; *p; ) {
            if (p[0] == '{' && p[1] && p[2] == '}') {
                if (pass) glyph(p[1], (x + rad) * PG_S, (y + 4.0f) * PG_S, rad * PG_S);
                x += 2.0f * rad + 3.0f;
                p += 3;
                continue;
            }
            char seg[64];
            size_t n = 0;
            while (p[n] && !(p[n] == '{' && p[n + 1] && p[n + 2] == '}') && n + 1 < sizeof seg) { seg[n] = p[n]; n++; }
            seg[n] = 0;
            p += n;
            if (pass) gf_text(1, x, y, seg, pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
            x += gf_width(1, seg) + (n && seg[n - 1] == ' ' ? 0.0f : 2.0f);
        }
        if (!pass) w = x - x_right;
    }
}

static void pg_render(void) {
    double t0 = ps2_wall_seconds();
    if (!gf_state) gf_load();
    pg_background();
    pg_render_body();
    {   static int dbg = -1, n;
        if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_ENH"); dbg = e && *e == '1'; if (dbg) pg_help_report(); }
        if (dbg && n++ < 40) ps2_log("quit: ENHANCEMENTS page drawn in %.2f ms (renderer thread)", (ps2_wall_seconds() - t0) * 1e3);
    }
}
/* the help line of an item: the last change's result, else the item's help and when it takes effect */
static void pg_item_help(char *h, size_t n) {
    int i = pg_items[pg_row];
    const ps2_enh_item *it = ps2_enh_item_at(i);
    char hb[96], hp[300];
    const char *o = ps2_enh_held_by(i, hb, sizeof hb);
    const char *help = ps2_enh_help(i, hp, sizeof hp, 0);       /* (the QUALITY PRESET's: GPU, rate, hint) */
    if (pg_msg[0]) snprintf(h, n, "%s", pg_msg);
    else if (o && it->apply != PS2_ENH_ACTION)
        snprintf(h, n, "%s Set by %s when the game started.", help, o);
    else if (ps2_enh_pending(i)) snprintf(h, n, "%s * Saved: restart the game to apply.", help);
    else if (ps2_enh_changed_mission(i))                 /* (no plus: the native help has none) */
        snprintf(h, n, "%s%s", help, ps2_enh_retry_waits(i) ? " Saved for the next mission." : " Saved for next mission or RETRY.");
    else if (!strcmp(it->key, "quality_preset")) snprintf(h, n, "%s", help);
    else {
        const int ap = ps2_enh_apply_now(i);
        snprintf(h, n, "%s%s", help, ap == PS2_ENH_RESTART ? " Needs a restart." : ap != PS2_ENH_MISSION ? ""
                 : ps2_enh_retry_waits(i) ? " From the next mission." : " From the next mission or RETRY.");
    }
}
static void pg_render_body(void) {
    char crumb[96], buf[64];
    if (pg_level == PG_CARD) { tc_render(); return; }
    const char *root = pg_from == PS2_ENH_FROM_PAUSE ? "PAUSE MENU>ENHANCEMENTS" : "ENHANCEMENTS";
    if (pg_level >= PG_ITEMS && pg_level != PG_NOTICE) snprintf(crumb, sizeof crumb, "%s>%s", root, ps2_enh_cat_name(pg_cat));
    else snprintf(crumb, sizeof crumb, "%s", root);
    gf_text(1, 10.0f, 8.0f, crumb, pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
    pg_hint(400.0f, 7.0f, pg_level == PG_ITEMS ? "{A} CHANGE  {B} BACK" : "{A} SELECT  {B} BACK");
    pg_rect(8.0f, PG_RULE, 401.6f, PG_RULE + 0.8f, pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
    int pending = ps2_enh_pending_count();
    if (pg_level == PG_CATS) {
        int shown = pg_shown(PS2_ENH_CATS);
        for (int s = 0; s < shown; s++) {
            int c = pg_cat_top + s;
            float y = PG_ROW0 + PG_PITCH * (float)s;
            float x1 = gf_text(0, PG_LX, y, ps2_enh_cat_name(c), pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
            int np = 0, nm = 0;
            for (int i = 0; i < ps2_enh_count(); i++) {
                np += ps2_enh_item_at(i)->cat == c && ps2_enh_pending(i);
                nm += ps2_enh_item_at(i)->cat == c && ps2_enh_changed_mission(i);
            }
            if (np || nm) gf_text(0, x1 + 6.0f, y, np ? "*" : "+", pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
            if (c == pg_cat) pg_frame(PG_LX - 4.0f, y - 3.0f, x1 + 4.0f, y + 13.5f, pg_box);
        }
        if (pg_cat_top > 0) pg_more(0, 0, pg_yel);
        if (pg_cat_top + shown < PS2_ENH_CATS) pg_more(shown - 1, 1, pg_yel);
        char h[200], hint[200];
        if (pg_cat == PS2_ENH_QUALITY && ps2_enh_hint(hint, sizeof hint)[0])   /* the GPU ran behind (perf6_presets) */
            snprintf(h, sizeof h, "%s", hint);
        else snprintf(h, sizeof h, "%s%s%s", ps2_enh_cat_help(pg_cat),
                      pending ? " * = saved, waits for a restart of the game." : "",
                      ps2_enh_mission_count() ? " + = next mission or RETRY." : "");
        pg_help_text(h);
        return;
    }
    if (pg_level == PG_NOTICE) {
        gf_text(0, PG_LX, PG_ROW0 + PG_PITCH, "RESTART THE GAME TO APPLY", pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
        char m[120];
        snprintf(m, sizeof m, "%d setting%s take%s effect after a restart.", pending, pending == 1 ? "" : "s",
                 pending == 1 ? "s" : "");
        gf_text(0, PG_LX, PG_ROW0 + PG_PITCH * 2.0f, m, pg_help[0], pg_help[1], pg_help[2], 1.0f);
        static const char *const rows[2] = { "QUIT GAME NOW", "CONTINUE" };
        for (int k = 0; k < 2; k++) {
            float y = PG_ROW0 + PG_PITCH * (4.0f + (float)k);
            float x1 = gf_text(0, PG_LX, y, rows[k], pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
            if (k == pg_notice) pg_frame(PG_LX - 4.0f, y - 3.0f, x1 + 4.0f, y + 13.5f, pg_box);
        }
        pg_help_text(pg_notice == 0 ? "Quits to the desktop after the memory card is idle; start the game again from "
                                      "the desktop to use the new settings."
                                    : "Keep playing; the new settings apply the next time the game starts.");
        return;
    }
    /* items (and the value list of one: it drops down from the item's row over the values below it, as the game's
       FLIGHT OPTIONS do); a long list shows the rows pg_top.. and scrolls with the cursor */
    int shown = pg_shown(pg_nitems), open = pg_level == PG_VALUES;
    float ly0 = 0.0f, ly1 = -1.0f, vx0 = PG_VX;
    if (pg_rows())                       /* the value column clears the category's longest label (COCKPIT, SOUND: 250) */
        for (int r = 0; r < pg_nitems; r++) {
            float e = PG_LX + gf_width(0, ps2_enh_item_at(pg_items[r])->label);
            if (e + 7.0f > vx0) vx0 = e + 10.0f < PG_VXMAX ? e + 10.0f : PG_VXMAX;   /* GRAPHICS (209) stays */
        }
    if (open) {
        const ps2_enh_item *it = ps2_enh_item_at(pg_items[pg_row]);
        ly0 = PG_ROW0 + PG_PITCH * (float)(pg_row - pg_top);
        if (ly0 + PG_PITCH * (float)it->n > PG_RULE - 4.0f) ly0 = PG_RULE - 4.0f - PG_PITCH * (float)it->n;
        ly1 = ly0 + PG_PITCH * (float)it->n;
    }
    for (int s = 0; s < shown; s++) {
        int r = pg_top + s, i = pg_items[r];
        const ps2_enh_item *it = ps2_enh_item_at(i);
        float y = PG_ROW0 + PG_PITCH * (float)s;
        const float *lc = open && r != pg_row ? pg_dim : pg_yel;
        float x1 = gf_text(0, PG_LX, y, it->label, lc[0], lc[1], lc[2], 1.0f);
        if (r == pg_row && !open) pg_frame(PG_LX - 4.0f, y - 3.0f, x1 + 4.0f, y + 13.5f, pg_box);
        if (open && y > ly0 - 12.0f && y < ly1) continue;          /* under the list */
        const char *o = ps2_enh_override(i);
        const float *vc = open || o ? pg_dim : pg_yel;
        const char *vs = ps2_enh_value_name(i, buf, sizeof buf);
        if (q_ps && !strcmp(vs, "PRESS A")) vs = "PRESS CROSS";   /* (the active controller's button, as the header) */
        if (pg_reset_prompt(i, q_ps)) vs = pg_reset_prompt(i, q_ps);
        float vx = gf_text(0, vx0, y, vs, vc[0], vc[1], vc[2], 1.0f);
        if (ps2_enh_pending(i)) gf_text(0, vx + 5.0f, y, "*", vc[0], vc[1], vc[2], 1.0f);
        else if (ps2_enh_changed_mission(i)) gf_text(0, vx + 5.0f, y, "+", vc[0], vc[1], vc[2], 1.0f);
    }
    {   const char *hd = ps2_enh_cat_head(pg_cat);      /* the small font: above the rule with rows to spare, else under the crumb */
        if (*hd && !open) gf_text(1, PG_LX, pg_nitems < PG_FITROWS ? PG_RULE - 11.0f : 17.0f, hd, pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
    }
    if (pg_top > 0) pg_more(0, 0, open ? pg_dim : pg_yel);                      /* more rows above / below */
    if (pg_top + shown < pg_nitems) pg_more(shown - 1, 1, open ? pg_dim : pg_yel);
    if (pg_level == PG_VALUES) {
        int i = pg_items[pg_row];
        const ps2_enh_item *it = ps2_enh_item_at(i);
        float y0 = ly0;
        for (int k = 0; k < it->n; k++) {
            float y = y0 + PG_PITCH * (float)k;
            float x1 = gf_text(0, vx0, y, it->name[k], pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
            if (k == pg_val) pg_frame(vx0 - 4.0f, y - 3.0f, x1 + 4.0f, y + 13.5f, pg_box);
        }
    }
    char h[300];
    pg_item_help(h, sizeof h);
    pg_help_text(h);
}

/* ---- ACZ_ENH_NATIVE: the page's state for zero_vr.c's zn_* (see ps2_quit.h) ---- */
static pthread_mutex_t pg_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static ps2_enh_snap pg_snap_pub;
static void pg_snap_publish(void) {
    static ps2_enh_snap s;
    static u32 done;
    if (!pg_native || pg_level == PG_CARD || done == pg_gen) return;
    done = pg_gen;
    memset(&s, 0, sizeof s);
    s.gen = pg_gen;
    s.cat = -1;
    s.open = -1;
    int pending = ps2_enh_pending_count();
    if (pg_level == PG_CATS) {
        s.kind = PS2_ENH_SNAP_LIST;
        s.nrows = pg_shown(PS2_ENH_CATS);
        s.cursor = pg_cat - pg_cat_top;
        snprintf(s.crumb, sizeof s.crumb, "ENHANCEMENTS");
        for (int r = 0; r < s.nrows; r++) {
            int c = pg_cat_top + r, np = 0, nm = 0;
            for (int i = 0; i < ps2_enh_count(); i++) {
                np += ps2_enh_item_at(i)->cat == c && ps2_enh_pending(i);
                nm += ps2_enh_item_at(i)->cat == c && ps2_enh_changed_mission(i);
            }
            snprintf(s.label[r], sizeof s.label[r], "%s%s", ps2_enh_cat_name(c), np ? " *" : nm ? " +" : "");
        }
        if (s.nrows < PS2_ENH_CATS)
            snprintf(s.head, sizeof s.head, "%d-%d OF %d", pg_cat_top + 1, pg_cat_top + s.nrows, PS2_ENH_CATS);
        char hint[200];
        if (pg_cat == PS2_ENH_QUALITY && ps2_enh_hint(hint, sizeof hint)[0])   /* the GPU ran behind (perf6_presets) */
            snprintf(s.help, sizeof s.help, "%s", hint);
        else snprintf(s.help, sizeof s.help, "%s%s%s", ps2_enh_cat_help(pg_cat),
                      pending ? " * Saved, waits for a restart of the game." : "",
                      ps2_enh_mission_count() ? " A plus: next mission or RETRY." : "");   /* (the native help has no plus) */
    } else if (pg_level == PG_NOTICE) {
        s.kind = PS2_ENH_SNAP_LIST;
        s.nrows = 2;
        s.cursor = pg_notice;
        snprintf(s.crumb, sizeof s.crumb, "ENHANCEMENTS");
        snprintf(s.head, sizeof s.head, "RESTART THE GAME TO APPLY");
        snprintf(s.label[0], sizeof s.label[0], "QUIT GAME NOW");
        snprintf(s.label[1], sizeof s.label[1], "CONTINUE");
        snprintf(s.help, sizeof s.help, "%d setting%s take%s effect after a restart. %s", pending, pending == 1 ? "" : "s",
                 pending == 1 ? "s" : "", pg_notice == 0 ? "Quits to the desktop after the memory card is idle." :
                 "Keep playing; the new settings apply the next time the game starts.");
    } else if (pg_level == PG_ITEMS || pg_level == PG_VALUES) {
        s.kind = pg_level == PG_VALUES ? PS2_ENH_SNAP_VALUES : PS2_ENH_SNAP_ITEMS;
        s.cat = pg_cat;
        s.nrows = pg_shown(pg_nitems);
        s.cursor = pg_row - pg_top;
        snprintf(s.crumb, sizeof s.crumb, "ENHANCEMENTS>%s", ps2_enh_cat_name(pg_cat));
        for (int r = 0; r < s.nrows; r++) {
            int i = pg_items[pg_top + r];
            char buf[64];
            snprintf(s.label[r], sizeof s.label[r], "%s", ps2_enh_item_at(i)->label);
            snprintf(s.value[r], sizeof s.value[r], "%s%s", ps2_enh_value_name(i, buf, sizeof buf),
                     ps2_enh_pending(i) ? " *" : ps2_enh_changed_mission(i) ? " +" : "");
            if (pg_reset_prompt(i, 0)) snprintf(s.value[r], sizeof s.value[r], "%s", pg_reset_prompt(i, 0));
        }
        {   const char *hd = ps2_enh_cat_head(pg_cat);  /* (at most ZN_GLYPHS, 40, letters: zero_vr.c) */
            if (s.nrows < pg_nitems) snprintf(s.head, sizeof s.head, "%s%s%d-%d OF %d", hd, *hd ? " " : "", pg_top + 1, pg_top + s.nrows, pg_nitems);
            else snprintf(s.head, sizeof s.head, "%s", hd);
        }
        if (pg_level == PG_VALUES) {
            const ps2_enh_item *it = ps2_enh_item_at(pg_items[pg_row]);
            s.open = s.cursor;
            s.nvals = it->n < PS2_ENH_SNAP_VALS ? it->n : PS2_ENH_SNAP_VALS;
            s.val = pg_val;
            for (int k = 0; k < s.nvals; k++) snprintf(s.list[k], sizeof s.list[k], "%s", it->name[k]);
        }
        pg_item_help(s.help, sizeof s.help);
    }
    pthread_mutex_lock(&pg_snap_mu);
    pg_snap_pub = s;
    pthread_mutex_unlock(&pg_snap_mu);
}
void ps2_quit_enh_open_native(void) {
    pthread_mutex_lock(&pg_snap_mu);
    pg_snap_pub.gen = 0;                       /* nothing of an earlier visit */
    pthread_mutex_unlock(&pg_snap_mu);
    __atomic_store_n(&pg_native_req, 1, __ATOMIC_RELEASE);
    ps2_quit_enh_open(PS2_ENH_FROM_PAUSE);
}
int ps2_quit_enh_snap(ps2_enh_snap *out, u32 have_gen) {
    int r = 0;
    pthread_mutex_lock(&pg_snap_mu);
    if (pg_snap_pub.gen && pg_snap_pub.gen != have_gen) { *out = pg_snap_pub; r = 1; }
    pthread_mutex_unlock(&pg_snap_mu);
    return r;
}

/* =====================================================================================================================
   The TOUCH CONTROLS card (bug #239, touch_ux 2026-10-29). The user's first try with the Quest controllers: "I don't
   understand how to switch weapons, how to speed up ... I don't know what to press ... a quick option in the menu to
   bring up the Meta Quest controller scheme". Both controllers drawn from above in the page's look (the game's dark
   grid panel, yellow outlines, its menu and small fonts), each control with a leader line to what it does now: line 1
   the action (menu font), line 2 how (small font). The mode in use first; left / right shows the other mode. Four
   lines under the rule say the essentials. It is a level of the ENHANCEMENTS page (PG_CARD): the same quad, input and
   neutral pad; opened standalone (pause menu row, Menu held in flight, by itself) closing it returns to the game.
   ===================================================================================================================== */
int ps2_quit_touch_card_visible(void) {
    return (pg_vis && pg_level == PG_CARD) || tc_req >= 0 || tc_pending >= 0;
}
void ps2_quit_touch_hud_line(const char *line, double secs) {
    snprintf(tq_title, sizeof tq_title, "%s", line ? line : "");
    tq_line[0] = 0;
    q_hint_kind = 4;
    q_hint_until = ps2_wall_seconds() + secs;
    q_gen++;
}
void ps2_quit_touch_hud_line_clear(void) {
    if (q_hint_kind == 4 && q_hint_until > 0.0) { q_hint_until = 0.0; q_gen++; ps2_log("quit: the Touch line is gone (the grab)"); }
}
void ps2_quit_touch_hint(const char *title, const char *line, double secs) {
    snprintf(tq_title, sizeof tq_title, "%s", title ? title : "");
    snprintf(tq_line, sizeof tq_line, "%s", line ? line : "");
    q_hint_kind = 3;
    q_hint_until = ps2_wall_seconds() + secs;
    q_gen++;
}
static void tc_level(void) {
    ps2_touch_info ti;
    ps2_touch_get_info(&ti);
    tc_view = ti.mode == PS2_TOUCH_PAD ? PS2_TOUCH_PAD : PS2_TOUCH_STICK;
    pg_level = PG_CARD;
    pg_wait = 1;
    pg_prev_dir = 0;
    pg_dirty();
    ps2_log("quit: TOUCH CONTROLS card shown (%s; %s)", tc_view == PS2_TOUCH_PAD ? "as a pad" : "hands on stick",
            tc_back ? "from ENHANCEMENTS" : tc_auto ? "by itself, the first time" : tc_resume ? "the flight paused" : "standalone");
}
/* a request: from the page (one of its levels), else standalone once the game is in a menu or paused; from flight the
   game is paused first with a Start press (FROM_FLIGHT, FROM_AUTO) */
static void tc_serve(ps2_pad_state *st, double now) {
    int gs = game_state(), from = tc_pending;
    if (from == PS2_TC_FROM_ENH && pg_vis) {
        tc_pending = -1;
        if (pg_level != PG_CARD) { tc_back = pg_level == PG_CATS ? PG_CATS : PG_ITEMS; tc_level(); }
        return;
    }
    if (gs == PS2_QUIT_GAME_FLYING) {
        if (from != PS2_TC_FROM_FLIGHT && from != PS2_TC_FROM_AUTO) {
            tc_pending = -1;
            ps2_log("quit: TOUCH CONTROLS card not shown in flight (pause first)");
            return;
        }
        if (tc_pulse_until == 0.0) {
            tc_pulse_until = now + 0.10;
            tc_resume = 1;
            ps2_log("quit: TOUCH CONTROLS card: pausing the flight first");
        }
        if (now < tc_pulse_until) st->buttons |= 1u << PS2_ACT_START;
        else if (now - tc_req_t > 2.0) {
            tc_pending = -1; tc_resume = 0;
            ps2_log("quit: TOUCH CONTROLS card: the game did not pause in 2 s; not shown");
        }
        return;
    }
    tc_pending = -1;
    if (q_state != Q_IDLE) { tc_resume = 0; return; }         /* (the quit prompt or a quit under way) */
    if (pg_vis && pg_level == PG_CARD) return;
    tc_auto = from == PS2_TC_FROM_AUTO;
    if (gs != PS2_QUIT_GAME_PAUSED) tc_resume = 0;
    if (!pg_vis) {
        pg_native = 0;
        pg_from = gs == PS2_QUIT_GAME_PAUSED ? PS2_ENH_FROM_PAUSE : PS2_ENH_FROM_MENUS;
        pg_msg[0] = 0;
        pg_restart_changed = 0;
        pg_open_t = now;
        pg_gone_t = 0.0;
        pg_vis = 1;
        tc_back = 0;
    } else tc_back = pg_level == PG_CATS || pg_level == PG_ITEMS ? pg_level : 0;
    tc_level();
}
static void tc_close(void) {
    if (!ps2_cfg.touch_card_seen) {                            /* shown once: not by itself again (ACZ_TOUCH_CARD) */
        ps2_cfg.touch_card_seen = 1;
        ps2_settings_save();
        ps2_log("quit: TOUCH CONTROLS card seen (saved: [input] touch_controls_seen = 1)");
    }
    tc_auto = 0;
    if (tc_back) { pg_level = tc_back; tc_back = 0; pg_dirty(); ps2_log("quit: TOUCH CONTROLS card closed"); return; }
    pg_vis = 0;
    pg_level = 0;
    q_swallow = 1;
    pg_dirty();
    ps2_log("quit: TOUCH CONTROLS card closed%s", tc_resume ? "; the flight resumes" : "");
    if (tc_resume) { tc_resume = 2; tc_resume_t = ps2_wall_seconds() + 0.25; }
}

/* ---- what each control does: per controller (0 left, 1 right) seven slots, top to bottom on the drawing ---- */
enum { TC_TRIG, TC_UPPER, TC_STICK, TC_PRESS, TC_LOWER, TC_SYS, TC_GRIP, TC_SLOTS };
typedef struct { const char *act, *how; } tc_label;
static void tc_labels(int view, tc_label lab[2][TC_SLOTS]) {
    ps2_touch_info ti;
    ps2_touch_get_info(&ti);
    memset(lab, 0, sizeof(tc_label) * 2 * TC_SLOTS);
    lab[1][TC_SYS] = (tc_label){ "", "QUEST MENU" };
    if (view == PS2_TOUCH_STICK) {
        int sh = ti.swap ? 0 : 1, th = 1 - sh;                 /* the stick hand and the throttle hand (physical) */
        tc_label s_lab = { "STICK", ti.toggle ? "TAP, THEN TILT" : "HOLD AND TILT" };   /* (ACZ_TOUCH_GRAB) */
        lab[sh][TC_TRIG] = ti.grab == PS2_TOUCH_GRAB_TRIGGER ? s_lab
                         : (tc_label){ "GUN", ti.grab == PS2_TOUCH_GRAB_EITHER ? "AT THE STICK: GRAB" : "" };
        lab[sh][TC_GRIP] = ti.grab == PS2_TOUCH_GRAB_TRIGGER ? (tc_label){ "GUN", "" } : s_lab;
        lab[sh][TC_LOWER] = (tc_label){ "MISSILE", "OR SPECIAL" };
        lab[sh][TC_UPPER] = (tc_label){ "TARGET", "HOLD: LOOK AT" };
        lab[sh][TC_STICK] = ti.rs_look ? (tc_label){ "LOOK AROUND", "" } : (tc_label){ "WINGMEN", "FLICK 4 WAYS" };
        lab[sh][TC_PRESS] = (tc_label){ "VIEW", "PRESS STICK" };
        tc_label t_lab = ti.thr == PS2_TOUCH_THR_STICK ? (tc_label){ "", "" }
                       : (tc_label){ "THROTTLE", ti.thr == PS2_TOUCH_THR_TILT ? (ti.toggle ? "TAP, THEN TILT" : "HOLD AND TILT")
                                                 : ti.toggle ? "TAP, PUSH/PULL" : "HOLD, PUSH/PULL" };
        tc_label b_lab = { "AIR BRAKE", ti.grab == PS2_TOUCH_GRAB_EITHER && ti.thr != PS2_TOUCH_THR_STICK ? "AT THE LEVER: GRAB" : "SLOW DOWN" };
        lab[th][TC_TRIG] = ti.grab == PS2_TOUCH_GRAB_TRIGGER && ti.thr != PS2_TOUCH_THR_STICK ? t_lab : b_lab;
        lab[th][TC_GRIP] = ti.grab == PS2_TOUCH_GRAB_TRIGGER && ti.thr != PS2_TOUCH_THR_STICK ? b_lab : t_lab;
        lab[th][TC_LOWER] = (tc_label){ "WEAPON", "SWITCH" };
        lab[th][TC_UPPER] = (tc_label){ "MAP", "HOLD: RECENTRE" };
        lab[th][TC_STICK] = ti.thr == PS2_TOUCH_THR_STICK ? (tc_label){ "THROTTLE", "UP/DOWN, YAW" }
                          : ti.thumb_thrust ? (tc_label){ "RUDDER", "UP: THRUST" }
                          : (tc_label){ "RUDDER", "LEFT / RIGHT" };
        lab[th][TC_PRESS] = (tc_label){ "AUTOPILOT", "PRESS STICK" };
        lab[0][TC_SYS] = (tc_label){ "PAUSE", ti.card_hold_ms > 0 ? "HOLD: THIS CARD" : "" };
        return;
    }
    int modern = ps2_cfg.pad_layout == PS2_PAD_LAYOUT_MODERN;
    tc_label slow = { "SLOW DOWN", modern ? "LT" : "LB" }, fast = { "SPEED UP", "FULL: BURNER" };
    tc_label yl = { "YAW LEFT", modern ? "LB" : "LT" }, yr = { "YAW RIGHT", modern ? "RB" : "RT" };
    lab[0][modern ? TC_TRIG : TC_GRIP] = slow;  lab[0][modern ? TC_GRIP : TC_TRIG] = yl;
    lab[1][modern ? TC_TRIG : TC_GRIP] = fast;  lab[1][modern ? TC_GRIP : TC_TRIG] = yr;
    lab[0][TC_STICK] = (tc_label){ "FLY", "PITCH, ROLL" };
    lab[0][TC_PRESS] = (tc_label){ "MAP", "PRESS STICK" };
    lab[0][TC_LOWER] = (tc_label){ "WEAPON", "SWITCH" };
    lab[0][TC_UPPER] = (tc_label){ "TARGET", "HOLD: LOOK AT" };
    lab[0][TC_SYS] = (tc_label){ "PAUSE", "START" };
    lab[1][TC_LOWER] = (tc_label){ "GUN", "" };
    lab[1][TC_UPPER] = (tc_label){ "MISSILE", "OR SPECIAL" };
    lab[1][TC_STICK] = ti.rs_look ? (tc_label){ "LOOK AROUND", "" } : (tc_label){ "WINGMEN", "FLICK 4 WAYS" };
    lab[1][TC_PRESS] = (tc_label){ "VIEW", "PRESS STICK" };
}
static const char *const tc_slot_name[TC_SLOTS] = { "TRIGGER", "UPPER BUTTON", "THUMBSTICK", "THUMBSTICK PRESS",
                                                     "LOWER BUTTON", "MENU BUTTON", "GRIP" };
/* the essentials under the rule (small font: A-Z 0-9 . , : ; ' " - / [ ] > and space) */
#define TC_NOTES 4
static void tc_notes(int view, const char *n[TC_NOTES]) {
    ps2_touch_info ti;
    ps2_touch_get_info(&ti);
    if (view == PS2_TOUCH_STICK) {
        if (ti.grab == PS2_TOUCH_GRAB_TRIGGER) {               /* (ACZ_TOUCH_GRAB) */
            n[0] = ti.swap ? "LEFT TRIGGER HELD: YOU FLY. LET GO: THE STICK CENTRES."
                           : "RIGHT TRIGGER HELD: YOU FLY. LET GO: THE STICK CENTRES.";
            n[1] = ti.thr == PS2_TOUCH_THR_STICK ? "THROTTLE: STICK UP FASTER, DOWN SLOWER, IT SPRINGS."
                 : ti.thr == PS2_TOUCH_THR_TILT ? (ti.swap ? "RIGHT TRIGGER HELD: TILT FORWARD FASTER, BACK SLOWER."
                                                           : "LEFT TRIGGER HELD: TILT FORWARD FASTER, BACK SLOWER.")
                 : ti.swap ? "RIGHT TRIGGER HELD: PUSH FASTER, PULL SLOWER."
                           : "LEFT TRIGGER HELD: PUSH FASTER, PULL SLOWER.";
        } else {
        n[0] = ti.swap ? "LEFT GRIP HELD: YOU FLY. LET GO: THE STICK CENTRES."
                       : "RIGHT GRIP HELD: YOU FLY. LET GO: THE STICK CENTRES.";
        n[1] = ti.thr == PS2_TOUCH_THR_STICK ? "THROTTLE: STICK UP FASTER, DOWN SLOWER, IT SPRINGS."
             : ti.thr == PS2_TOUCH_THR_TILT ? (ti.swap ? "RIGHT GRIP HELD: TILT FORWARD FASTER, BACK SLOWER."
                                                       : "LEFT GRIP HELD: TILT FORWARD FASTER, BACK SLOWER.")
             : ti.swap ? "RIGHT GRIP HELD: PUSH FASTER, PULL SLOWER."
                       : "LEFT GRIP HELD: PUSH FASTER, PULL SLOWER.";
        }
        n[2] = ti.thr == PS2_TOUCH_THR_SPRING ? "PAST MIL: AFTERBURNER. LET GO: BACK TO CRUISE."
             : ti.thumb_thrust ? "PAST MIL: AFTERBURNER. OR LEFT THUMBSTICK UP / DOWN."
             : "PAST MIL: AFTERBURNER. THE LEVER STAYS PUT.";
        n[3] = "MENUS: AN XBOX PAD - A OK, B BACK, MENU: START.";
    } else {
        n[0] = "THE CONTROLLERS ARE AN XBOX PAD: THE SAME LETTERS.";
        n[1] = "VIEW: PRESS THE LEFT STICK; HOLD 1 S: RECENTRE.";
        n[2] = "MENU: START. BOTH STICKS PRESSED: L3.";
        n[3] = ps2_cfg.pad_layout == PS2_PAD_LAYOUT_MODERN ? "MODERN LAYOUT: TRIGGERS THROTTLE, GRIPS YAW."
                                                           : "CLASSIC LAYOUT: GRIPS THROTTLE, TRIGGERS YAW.";
    }
}

/* ---- drawing, PS2 pixel units of the page ---- */
static void tc_seg(float x0, float y0, float x1, float y1, float w, const float *c, float a) {
    float p[6] = { 1, w * PG_S, x0 * PG_S, y0 * PG_S, x1 * PG_S, y1 * PG_S };
    float m = w * PG_S + 2.0f;
    shape(fminf(x0, x1) * PG_S - m, fminf(y0, y1) * PG_S - m, fmaxf(x0, x1) * PG_S + m, fmaxf(y0, y1) * PG_S + m,
          sd_segs, p, c[0], c[1], c[2], a);
}
static void tc_rbox(float cx, float cy, float hw, float hh, float r, const float *c, float a) {
    float p[5] = { cx * PG_S, cy * PG_S, hw * PG_S, hh * PG_S, r * PG_S };
    shape((cx - hw) * PG_S, (cy - hh) * PG_S, (cx + hw) * PG_S, (cy + hh) * PG_S, sd_rbox, p, c[0], c[1], c[2], a);
}
static void tc_rbox_ol(float cx, float cy, float hw, float hh, float r, const float *line, const float *fill) {
    tc_rbox(cx, cy, hw, hh, r, line, 1.0f);
    tc_rbox(cx, cy, hw - 0.9f, hh - 0.9f, r > 0.9f ? r - 0.9f : 0.0f, fill, 1.0f);
}
static void tc_disc(float cx, float cy, float r, const float *c, float a) {
    float p[3] = { cx * PG_S, cy * PG_S, r * PG_S };
    shape((cx - r) * PG_S, (cy - r) * PG_S, (cx + r) * PG_S, (cy + r) * PG_S, sd_circle, p, c[0], c[1], c[2], a);
}
static void tc_ring(float cx, float cy, float r, float w, const float *c) {
    float p[4] = { cx * PG_S, cy * PG_S, r * PG_S, w * PG_S };
    shape((cx - r - w) * PG_S, (cy - r - w) * PG_S, (cx + r + w) * PG_S, (cy + r + w) * PG_S, sd_ring, p, c[0], c[1], c[2], 1.0f);
}
static void tc_text_c(int f, float cx, float y, const char *s, const float *c) {
    gf_text(f, cx - gf_width(f, s) * 0.5f, y, s, c[0], c[1], c[2], 1.0f);
}
#define TC_FACE_Y 102.0f
/* one controller from above (Touch Plus: no ring), centre x cx, side 0 left / 1 right; anchor points per slot out */
static void tc_controller(float cx, int side, float anc[TC_SLOTS][2]) {
    static const float fill[3] = { 0.045f, 0.085f, 0.065f }, fill2[3] = { 0.075f, 0.13f, 0.10f };
    const float *ln = pg_yel;
    float i = side ? -1.0f : 1.0f, o = -i;                   /* the inner side (towards the other hand), the outer */
    float fy = TC_FACE_Y;
    tc_rbox_ol(cx + i * 4.0f, fy - 31.0f, 11.0f, 6.0f, 4.0f, ln, fill2);          /* the trigger, under the face's front */
    tc_rbox_ol(cx + i * 6.0f, fy + 50.0f, 14.0f, 31.0f, 12.0f, ln, fill);          /* the handle */
    tc_rbox_ol(cx + i * 17.5f, fy + 52.0f, 3.6f, 20.0f, 3.0f, ln, fill2);          /* the grip button on its inner side */
    tc_rbox_ol(cx, fy, 27.0f, 29.0f, 15.0f, ln, fill);                             /* the face */
    float sx = cx + i * 9.0f, sy = fy - 8.0f;                                      /* the thumbstick */
    tc_ring(sx, sy, 9.0f, 1.0f, ln);
    tc_disc(sx, sy, 5.2f, fill2, 1.0f);
    tc_ring(sx, sy, 5.2f, 0.8f, pg_help);
    float ux = cx + o * 12.0f, uy = fy - 14.0f, lx = cx + o * 7.0f, ly = fy + 6.0f;   /* upper / lower button */
    tc_disc(ux, uy, 6.0f, ln, 1.0f); tc_disc(ux, uy, 5.1f, fill2, 1.0f);
    tc_disc(lx, ly, 6.0f, ln, 1.0f); tc_disc(lx, ly, 5.1f, fill2, 1.0f);
    tc_text_c(0, ux + 0.5f, uy - 5.0f, side ? "B" : "Y", pg_box);
    tc_text_c(0, lx + 0.5f, ly - 5.0f, side ? "A" : "X", pg_box);
    float bx = cx + i * 5.0f, by = fy + 17.0f;                                     /* Menu (left) / Meta (right) */
    tc_disc(bx, by, 3.6f, ln, 1.0f); tc_disc(bx, by, 2.8f, fill2, 1.0f);
    if (!side) for (int k = -1; k <= 1; k++) tc_seg(bx - 1.6f, by + k * 1.1f, bx + 1.6f, by + k * 1.1f, 0.45f, pg_box, 1.0f);
    else tc_ring(bx, by, 1.3f, 0.5f, pg_dim);
    anc[TC_TRIG][0] = cx + i * 4.0f + o * 11.0f; anc[TC_TRIG][1] = fy - 31.0f;
    anc[TC_UPPER][0] = ux + o * 6.0f;            anc[TC_UPPER][1] = uy;
    anc[TC_STICK][0] = sx + o * 9.0f;            anc[TC_STICK][1] = sy;
    anc[TC_PRESS][0] = sx;                       anc[TC_PRESS][1] = sy + 1.5f;
    anc[TC_LOWER][0] = lx + o * 6.0f;            anc[TC_LOWER][1] = ly;
    anc[TC_SYS][0] = bx;                         anc[TC_SYS][1] = by;
    anc[TC_GRIP][0] = cx + i * 17.5f;            anc[TC_GRIP][1] = fy + 52.0f;
}
static void tc_render(void) {
    ps2_touch_info ti;
    ps2_touch_get_info(&ti);
    const char *crumb = tc_back ? (pg_from == PS2_ENH_FROM_PAUSE ? "PAUSE MENU>ENHANCEMENTS>CONTROLS>TOUCH CONTROLS"
                                                                 : "ENHANCEMENTS>CONTROLS>TOUCH CONTROLS")
                      : pg_from == PS2_ENH_FROM_PAUSE ? "PAUSE MENU>TOUCH CONTROLS" : "TOUCH CONTROLS";
    gf_text(1, 10.0f, 8.0f, crumb, pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
    {   int ps = q_ps, tch = q_touch;                          /* the Quest's letters, whatever pad is connected */
        q_ps = 0; q_touch = 1;
        pg_hint(400.0f, 7.0f, "{B} BACK");
        q_ps = ps; q_touch = tch;
    }
    const char *title = tc_view == PS2_TOUCH_STICK ? "HANDS ON STICK" : "AS A PAD";
    float tw = gf_width(0, title);
    tc_text_c(0, 204.8f, 21.0f, title, pg_yel);
    gf_text(1, 204.8f + tw * 0.5f + 8.0f, 23.0f, ti.mode == tc_view ? "IN USE" : ti.mode == PS2_TOUCH_OFF ? "TOUCH IS OFF"
            : "NOT IN USE", pg_help[0], pg_help[1], pg_help[2], 1.0f);
    tc_label lab[2][TC_SLOTS];
    tc_labels(tc_view, lab);
    static const float slot_y[TC_SLOTS] = { 40.0f, 64.0f, 88.0f, 112.0f, 136.0f, 160.0f, 184.0f };
    const float cxs[2] = { 168.0f, 241.6f };
    for (int c = 0; c < 2; c++) {
        float anc[TC_SLOTS][2];
        tc_controller(cxs[c], c, anc);
        tc_text_c(1, cxs[c] + (c ? 6.0f : -6.0f), TC_FACE_Y + 86.0f, c ? "RIGHT" : "LEFT", pg_dim);
        for (int s = 0; s < TC_SLOTS; s++) {
            const tc_label *L = &lab[c][s];
            if (!L->act || (!L->act[0] && !(L->how && L->how[0]))) continue;
            float y = slot_y[s];
            float w1 = L->act[0] ? gf_width(0, L->act) : 0.0f, w2 = L->how && L->how[0] ? gf_width(1, L->how) : 0.0f;
            float ex = c ? 287.0f : 123.0f, ey = y + (L->act[0] ? 5.5f : 3.5f);
            tc_seg(ex, ey, anc[s][0], anc[s][1], 0.45f, pg_dim, 0.9f);   /* the leader */
            tc_disc(anc[s][0], anc[s][1], 1.1f, pg_help, 1.0f);
            if (L->act[0]) gf_text(0, c ? 290.0f : 120.0f - w1, y, L->act, pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
            if (w2 > 0.0f) gf_text(1, c ? 290.0f : 120.0f - w2, y + (L->act[0] ? 12.5f : 0.0f), L->how,
                                   pg_help[0], pg_help[1], pg_help[2], 1.0f);
        }
    }
    pg_rect(8.0f, 209.0f, 401.6f, 209.8f, pg_yel[0], pg_yel[1], pg_yel[2], 1.0f);
    const char *n[TC_NOTES];
    tc_notes(tc_view, n);
    for (int k = 0; k < TC_NOTES; k++) gf_text(1, 12.0f, 216.0f + 14.0f * (float)k, n[k], pg_help[0], pg_help[1], pg_help[2], 1.0f);
    gf_text(1, 12.0f, 290.0f, tc_view == PS2_TOUCH_STICK ? "LEFT / RIGHT: AS A PAD" : "LEFT / RIGHT: HANDS ON STICK",
            pg_dim[0], pg_dim[1], pg_dim[2], 1.0f);
}
/* the card as text for the window (no headset) */
static size_t tc_text(char *out, size_t n) {
    size_t k = 0;
    tc_label lab[2][TC_SLOTS];
    const char *nt[TC_NOTES];
    tc_labels(tc_view, lab);
    tc_notes(tc_view, nt);
#define TC_OUT(...) do { if (k < n) k += (size_t)snprintf(out + k, n - k, __VA_ARGS__); } while (0)
    TC_OUT("TOUCH CONTROLS: %s\n", tc_view == PS2_TOUCH_STICK ? "HANDS ON STICK" : "AS A PAD");
    for (int c = 0; c < 2; c++)
        for (int s = 0; s < TC_SLOTS; s++)
            if (lab[c][s].act && (lab[c][s].act[0] || (lab[c][s].how && lab[c][s].how[0])))
                TC_OUT("%s %s: %s%s%s\n", c ? "RIGHT" : "LEFT", s == TC_SYS && c ? "META BUTTON" : s == TC_LOWER
                       ? (c ? "A" : "X") : s == TC_UPPER ? (c ? "B" : "Y") : tc_slot_name[s], lab[c][s].act,
                       lab[c][s].how && lab[c][s].how[0] ? " - " : "", lab[c][s].how ? lab[c][s].how : "");
    TC_OUT("\n%s\n%s\n%s\n%s\n\nLeft / right: the other mode.  B: back.\n", nt[0], nt[1], nt[2], nt[3]);
#undef TC_OUT
    return k;
}

/* the page as text for the window (no headset) */
const char *ps2_quit_enh_text(int *sel) {
    static char out[2400];
    char buf[64];
    *sel = -1;
    if (!pg_vis || pg_drawn_by_game()) return NULL;
    if (pg_level == PG_CARD) { tc_text(out, sizeof out); return out; }
    size_t k = 0;
#define PG_OUT(...) do { if (k < sizeof out) k += (size_t)snprintf(out + k, sizeof out - k, __VA_ARGS__); } while (0)
    PG_OUT("%s%s%s\n", pg_from == PS2_ENH_FROM_PAUSE ? "PAUSE MENU > ENHANCEMENTS" : "ENHANCEMENTS",
        pg_level == PG_ITEMS || pg_level == PG_VALUES ? " > " : "",
        pg_level == PG_ITEMS || pg_level == PG_VALUES ? ps2_enh_cat_name(pg_cat) : "");
    int line = 1;
    if (pg_level == PG_CATS) {
        for (int c = 0; c < PS2_ENH_CATS; c++, line++) { if (c == pg_cat) *sel = line; PG_OUT("%s\n", ps2_enh_cat_name(c)); }
        char hint[200];
        PG_OUT("\n%s\n", pg_cat == PS2_ENH_QUALITY && ps2_enh_hint(hint, sizeof hint)[0] ? hint : ps2_enh_cat_help(pg_cat));
    } else if (pg_level == PG_NOTICE) {
        PG_OUT("RESTART THE GAME TO APPLY\n\n");
        line += 2;
        if (pg_notice == 0) *sel = line; else *sel = line + 1;
        PG_OUT("QUIT GAME NOW\nCONTINUE\n");
    } else {
        for (int r = 0; r < pg_nitems; r++, line++) {
            int i = pg_items[r];
            if (r == pg_row) *sel = line;
            PG_OUT("%-26s %s%s%s\n", ps2_enh_item_at(i)->label, pg_reset_prompt(i, 0) ? pg_reset_prompt(i, 0) : ps2_enh_value_name(i, buf, sizeof buf),
                ps2_enh_pending(i) ? " *" : "", ps2_enh_override(i) ? " (set at start)" : "");
        }
        const ps2_enh_item *it = ps2_enh_item_at(pg_items[pg_row]);
        if (pg_level == PG_VALUES) {
            PG_OUT("\nValues:");
            for (int c = 0; c < it->n; c++) PG_OUT(" %s%s%s", c == pg_val ? "[" : "", it->name[c], c == pg_val ? "]" : "");
            PG_OUT("\n");
        }
        char hp[300];
        if (*ps2_enh_cat_head(pg_cat)) PG_OUT("%s\n", ps2_enh_cat_head(pg_cat));
        PG_OUT("\n%s\n", pg_msg[0] ? pg_msg : ps2_enh_help(pg_items[pg_row], hp, sizeof hp, 0));
    }
#undef PG_OUT
    return out;
}
