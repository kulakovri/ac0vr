/* zero_menus.c -- Ace Combat Zero: the menus between missions (wish #58: a way to quit the game from them too).

   Every screen between missions (title, main menu and its sub-screens, New Game questions, briefing map, the
   pre-mission menu, hangar / shop, save / load, the menu after a mission) is a swg movie: a Flash-like movie with
   ActionScript and bitmap fonts, one SWAD package per screen in DATA.PAC (MainSelect m1239, CampaignSelect m1251,
   Shop m1253, DetailMap m1254, SaveLoad m1255, MissionEnd m1426, ...), played by a VM (~275 natives) and drawn by
   CSwgAC55Renderer. Their rows are text slots and cursor clips of the movie that its ActionScript fills per menu level
   (setTypeTxt), with the row count and the actions in the movie's bytecode; a real extra row there means editing the
   movie's script and display list at run time. (The in-mission pause menu is native C++, which is why QUIT GAME could
   be a real row there: zero_vr.c zq_*.) Here instead the game's own text code draws one line in the movie's own font
   next to the game's OK / CANCEL bar: "HOLD VIEW + MENU: QUIT GAME" (PlayStation glyphs: SELECT + START), the combo
   that opens the quit prompt (ps2_quit.c). The debriefing results board (native) has no hint; the combo works there.
   The prompt also opens the ENHANCEMENTS page ({X} outside the pause menu; bug #64): where the screen's font has the
   glyphs and the room, the hint reads "HOLD VIEW + MENU: QUIT, ENHANCEMENTS" (ACZ_ENH_MENU=0: the old text).

   ACZ_QUIT_HINT=0                 no hint (the menus exactly as the game draws them)
   ACZ_QUIT_HINT_ALPHA=1           its opacity relative to the game's bar (the bar itself: 0.7; the game's own menu text:
                                   0.2 .. 0.85; up to 1.43 = opaque). 0.5 before 2026-10-23: in the headset's cinema
                                   screen the dim hint went unnoticed (bug #74). The bar's own 32-pixel font has only
                                   the space, - and A-Z without Q, V, X (no + or :), so the hint keeps the 16-pixel font
   ACZ_QUIT_HINT_SKIP=MemCheck,WakeLogo,Title   movies without the hint (memory card check, logos, the title)
   ACZ_DEBUG_QUIT_HINT=1           [debug] log the movies drawn and the hint's font / text per screen
   ACZ_DEBUG_SWGTEXT=1             [debug] log every movie text the first time it shows (record, font, string, x, y)
   ACZ_DEBUG_FNTRACE=addr[:n[r]],...  [debug] log the first n (default 20) calls of each guest function: field, caller,
                                   a0..a3 and any printable string they point to; r: also the return value
                                   (ACZ_DEBUG_FNTRACE_FROM=field: only from that field on)
   ACZ_DEBUG_MEMDUMP=field:addr:len[,...]  [debug] write guest memory (hex addr / len) to out/mem_ADDR.bin
   ACZ_DEBUG_MODE_JUMP=from:to     [debug] the first setNext(from) becomes setNext(to) (hex scene factories) */
#include "ps2_runtime.h"
#include "ps2_hook.h"
#include "ps2_quit.h"
#include "ps2_settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 ps2_kernel_vblank_count(void);

static int zm_gptr(u32 p) { return p >= 0x100000u && p < 0x2000000u; }

/* a printable string at guest address p (at least 2 characters), else "" */
static const char *zm_str(u32 p, char *buf, size_t n) {
    buf[0] = 0;
    if (!zm_gptr(p)) return buf;
    size_t k = 0;
    for (; k + 1 < n; k++) {
        u8 c = ps2_r8(p + (u32)k);
        if (!c) break;
        if (c < 0x20 || c > 0x7E) { k = 0; break; }
        buf[k] = (char)c;
    }
    buf[k] = 0;
    if (k < 2) buf[0] = 0;
    return buf;
}

static void zm_memdump_tick(void);
#define ZM_TRACE_MAX 48
static struct { u32 addr; int left, rleft; } zm_tr[ZM_TRACE_MAX];
static u64 zm_trace_from;
static int zm_trace_ret(ps2_ctx *ctx, void *user) {
    int i = (int)(intptr_t)user;
    if (zm_tr[i].rleft <= 0 || ps2_kernel_vblank_count() < zm_trace_from) return 0;
    zm_tr[i].rleft--;
    char s[96];
    u32 v0 = ctx->r[2].uw[0];
    zm_str(v0, s, sizeof s);
    ps2_log("zero-menus: trace %08X returns %08X%s%s%s", zm_tr[i].addr, v0, s[0] ? " \"" : "", s, s[0] ? "\"" : "");
    return 0;
}
static int zm_trace_hit(ps2_ctx *ctx, void *user) {
    int i = (int)(intptr_t)user;
    zm_memdump_tick();
    if (zm_tr[i].left <= 0 || ps2_kernel_vblank_count() < zm_trace_from) return 0;
    zm_tr[i].left--;
    char s[4][48];
    u32 a[4];
    for (int k = 0; k < 4; k++) { a[k] = ctx->r[4 + k].uw[0]; zm_str(a[k], s[k], sizeof s[k]); }
    ps2_log("zero-menus: trace %08X field %llu ra %08X a0 %08X%s%s%s a1 %08X%s%s%s a2 %08X%s%s%s a3 %08X%s%s%s "
            "f12 %g", zm_tr[i].addr, (unsigned long long)ps2_kernel_vblank_count(), ctx->r[31].uw[0],
            a[0], s[0][0] ? " \"" : "", s[0], s[0][0] ? "\"" : "", a[1], s[1][0] ? " \"" : "", s[1], s[1][0] ? "\"" : "",
            a[2], s[2][0] ? " \"" : "", s[2], s[2][0] ? "\"" : "", a[3], s[3][0] ? " \"" : "", s[3], s[3][0] ? "\"" : "",
            (double)ctx->f[12].f);
    return 0;
}
static void zm_trace_install(void) {
    const char *e = getenv("ACZ_DEBUG_FNTRACE_FROM");
    zm_trace_from = e ? strtoull(e, NULL, 10) : 0;
    e = getenv("ACZ_DEBUG_FNTRACE");
    if (!e || !*e) return;
    int n = 0;
    while (*e && n < ZM_TRACE_MAX) {
        char *end;
        unsigned long a = strtoul(e, &end, 16);
        if (end == e) break;
        int cnt = 20, ret = 0;
        if (*end == ':') cnt = (int)strtol(end + 1, &end, 10);
        if (*end == 'r') { ret = 1; end++; }           /* addr:nr = also log the return value */
        zm_tr[n].addr = (u32)a; zm_tr[n].left = cnt; zm_tr[n].rleft = ret ? cnt : 0;
        if (ps2_hook_before((u32)a, zm_trace_hit, (void *)(intptr_t)n, 90, "zero-menus-trace") >= 0) {
            if (ret) ps2_hook_after((u32)a, zm_trace_ret, (void *)(intptr_t)n, 90, "zero-menus-trace");
            n++;
        } else ps2_log("zero-menus: trace hook at %08lX failed", a);
        e = end;
        while (*e == ',' || *e == ' ') e++;
    }
    ps2_log("zero-menus: tracing %d functions (ACZ_DEBUG_FNTRACE)", n);
}

/* ACZ_DEBUG_MEMDUMP=field:addr:len[,...]: guest memory to out/mem_ADDR.bin once that field has come (checked in traced
   calls and at the end of every movie's display list) */
static void zm_memdump_tick(void) {
    static int parsed;
    static struct { u64 field; u32 addr, len; int done; } md[8];
    static int nmd;
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("ACZ_DEBUG_MEMDUMP");
        while (e && *e && nmd < 8) {
            char *end;
            md[nmd].field = strtoull(e, &end, 10);
            if (*end != ':') break;
            md[nmd].addr = (u32)strtoul(end + 1, &end, 16);
            if (*end != ':') break;
            md[nmd].len = (u32)strtoul(end + 1, &end, 16);
            nmd++;
            e = end;
            while (*e == ',') e++;
        }
    }
    u64 f = ps2_kernel_vblank_count();
    for (int i = 0; i < nmd; i++)
        if (!md[i].done && f >= md[i].field) {
            md[i].done = 1;
            char nm[64];
            snprintf(nm, sizeof nm, "out/mem_%08X.bin", md[i].addr);
            FILE *fo = fopen(nm, "wb");
            if (!fo) continue;
            for (u32 k = 0; k < md[i].len; k++) fputc(ps2_r8(md[i].addr + k), fo);
            fclose(fo);
            ps2_log("zero-menus: wrote %s (%u bytes) at field %llu", nm, md[i].len, (unsigned long long)f);
        }
}

/* ---- swg menu text (the movies' text fields) ----
   The menus between missions are swg movies (Flash-like, ActionScript; one SWAD package per screen in DATA.PAC). Their
   text is drawn by CSwgAC55Renderer::drawText 0x149650: a text field "@N" refers to text record N of the font manager
   (CSwgMsgFont, vtable 0x3CAB98, renderer+0x30 -> +0x18): record = mgr + 0x4C8 + N * 0x14 {font index, string index,
   s16, s16, frame counter}; fonts = CSwgGameFont objects at mgr + 8 + i * 0x4C (vtable 0x3CABA8, base GameFont
   0x3C7EE0). Each font is a bitmap font made for that movie with only the glyphs its strings use: +0x10 string count,
   +0x14 glyph count, +4 glyph records (16 bytes: u16 w, u16 h, u16 cache slot, u16, u32 character code, ...), +0x20
   u16 string lengths, +0x24 u32 string offsets, +0x28 u16 glyph indices. 0x14B4A0(mgr, N, ctx = renderer + 0x1C,
   x, t0 = y, t1 = RGBA, t2 = flags) draws record N at (x, y) from the screen centre (via 0x14AB28, a0 = mgr holder). */
#define ZM_TEXT_DRAW   0x0014B4A0u
static int zm_swgtext_debug;
static int zm_font_string(u32 font, u32 si, char *out, size_t n) {
    out[0] = 0;
    if (!zm_gptr(font) || ps2_r32(font) != 0x003CABA8u) return 0;
    u32 ns = ps2_r32(font + 0x10u), ng = ps2_r16(font + 0x14u), recs = ps2_r32(font + 4u);
    u32 lens = ps2_r32(font + 0x20u), offs = ps2_r32(font + 0x24u), data = ps2_r32(font + 0x28u);
    if (si >= ns || !zm_gptr(lens) || !zm_gptr(offs) || !zm_gptr(data) || !zm_gptr(recs)) return 0;
    u32 len = ps2_r16(lens + 2u * si), off = ps2_r32(offs + 4u * si);
    size_t k = 0;
    for (u32 i = 0; i < len && k + 1 < n; i++) {
        u32 g = ps2_r16(data + 2u * (off + i));
        u32 c = g < ng ? ps2_r32(recs + 16u * g + 8u) : '?';
        out[k++] = c >= 0x20u && c < 0x7Fu ? (char)c : '?';
    }
    out[k] = 0;
    return 1;
}
static int zm_text_enter(ps2_ctx *ctx, void *u) {
    (void)u;
    u32 mgr = ctx->r[4].uw[0], id = ctx->r[5].uw[0];
    if (!zm_swgtext_debug || !zm_gptr(mgr) || id >= 0x200u) return 0;
    u32 rec = mgr + 0x4C8u + id * 0x14u, fi = ps2_r32(rec), si = ps2_r32(rec + 4u);
    static u32 seen[512];
    u32 key = (fi & 0xFFu) << 16 | (si & 0xFFFFu);
    if (zm_swgtext_debug >= 2) key ^= ctx->r[9].uw[0] * 2654435761u ^ (u32)ctx->r[8].sw[0] * 40503u;   /* (=2: colour, y too) */
    if (seen[id] == key + 1u) return 0;
    seen[id] = key + 1u;
    char s[128];
    u32 font = mgr + 8u + fi * 0x4Cu;
    zm_font_string(font, si, s, sizeof s);
    ps2_log("zero-menus: text @%u font %d (h %u) string %d x %d y %d rgba %08X rec %04X %04X: \"%s\"", id, (int)fi,
            zm_gptr(font) ? (unsigned)ps2_r16(font + 0x18u) : 0u, (int)si, (int)ctx->r[7].sw[0], (int)ctx->r[8].sw[0],
            ctx->r[9].uw[0], ps2_r16(rec + 8u), ps2_r16(rec + 10u), s);
    return 0;
}

/* ACZ_DEBUG_SWGSHAPE=field,field,... [debug]: every call of the renderer's shape draw (vtable +0x20, 0x148E98: a0
   renderer, a1 the shape object, a2 its transform: matrix +0x00.., translation +0x30 / +0x34, colour +0x40..) in those
   fields: the shape's id (a1 +0x0C), the translation and the matrix diagonal */
static u64 zm_shape_f[8];
static int zm_shape_n;
static int zm_shape_enter(ps2_ctx *ctx, void *u) {
    (void)u;
    u64 f = ps2_kernel_vblank_count();
    int hit = 0;
    for (int i = 0; i < zm_shape_n; i++) hit |= zm_shape_f[i] == f;
    if (!hit) return 0;
    u32 obj = ctx->r[5].uw[0], xf = ctx->r[6].uw[0];
    float m[14] = { 0 };
    for (int i = 0; i < 14 && zm_gptr(xf); i++) { u32 w = ps2_r32(xf + 4u * (u32)i); memcpy(&m[i], &w, 4); }
    ps2_log("zero-menus: shape field %llu ra %08X obj %08X id %d xf %08X m %.3f %.3f %.3f %.3f t %.1f %.1f",
            (unsigned long long)f, ctx->r[31].uw[0], obj, zm_gptr(obj) ? (int)ps2_r32(obj + 0xCu) : -1, xf,
            m[0], m[1], m[4], m[5], m[12], m[13]);
    return 0;
}

/* ---- the quit hint: one line of text in the movie's own font, drawn by the game's own text code ----
   While the controller combo can open the quit prompt (ps2_quit_hint_wanted: combo on, not flying, no prompt up), one
   more text is drawn per field: right after the movie's OK / CANCEL bar (zm_text_after), or at the end of the display
   list when the screen has no bar (CSwgAC55Renderer vtable +0x2C, 0x148E80, a no-op the movie player calls after
   drawing). For the duration of the call a free text record points at string 0 of a font of this movie, whose table
   entry points at our glyph indices (a guest page); both are restored right after. The font is the first one of the
   movie that has every glyph of the line (each movie font holds only the characters its own texts use; 16-pixel fonts
   first), the line the longest form that fits between the bar and the menu panel's right edge. */
#define ZM_REND_END    0x00148E80u   /* CSwgAC55Renderer vtable +0x2C: end of a movie's display list (jr ra) */
#define ZM_TEXT_THUNK  0x0014AB28u   /* (renderer+0x30 object, id, renderer+0x1C, x, t0 y, t1 RGBA, t2 0) */
#define ZM_FONT_VT     0x003CABA8u   /* CSwgGameFont */
#define ZM_MGR_VT      0x003CAB98u   /* CSwgMsgFont (the movie's font manager) */
#define ZM_PAGE        0x0F001000u   /* guest page with the hint's glyph indices */
static u8 zm_page[4096] __attribute__((aligned(4096)));
static int zm_page_mapped;                       /* (the hint and the OPTIONS row share it) */
static int zm_hint_on, zm_hint_log;
static float zm_hint_alpha = 1.0f;
static u64 zm_hint_field;
volatile u64 zero_menus_last_field;              /* field of the last swg movie drawn (any screen; zero_vr.c) */

/* Call a game function from a hook with a0..a3 and t0..t3 (the EE ABI passes arguments 5..8 in t0..t3); every
   register is restored afterwards. Returns v0. */
static u32 zm_call(ps2_ctx *ctx, u32 fn, const u32 *a, int n) {
    ps2_reg128 rs[32];
    ps2_fpr fs[32];
    ps2_reg128 hi = ctx->hi, lo = ctx->lo;
    float acc = ctx->acc;
    u32 fcc = ctx->fcc, fcr = ctx->fcr31, sa = ctx->sa, pc = ctx->pc;
    memcpy(rs, ctx->r, sizeof rs);
    memcpy(fs, ctx->f, sizeof fs);
    for (int i = 0; i < n && i < 8; i++) ctx->r[4 + i].ud[0] = (u64)(s64)(s32)a[i];
    ctx->r[29].ud[0] = (u64)(s64)(s32)((ctx->r[29].uw[0] - 0x100u) & ~15u);
    ctx->r[31].ud[0] = 0;
    ps2_dispatch(ctx, fn);
    u32 v0 = ctx->r[2].uw[0];
    memcpy(ctx->r, rs, sizeof rs);
    memcpy(ctx->f, fs, sizeof fs);
    ctx->hi = hi; ctx->lo = lo; ctx->acc = acc; ctx->fcc = fcc; ctx->fcr31 = fcr; ctx->sa = sa; ctx->pc = pc;
    return v0;
}

static int zm_glyph(u32 font, u32 ch) {
    u32 ng = ps2_r16(font + 0x14u), recs = ps2_r32(font + 4u);
    if (!zm_gptr(recs) || ng > 512u) return -1;
    for (u32 g = 0; g < ng; g++) if (ps2_r32(recs + 16u * g + 8u) == ch) return (int)g;
    return -1;
}
/* the line as glyph indices of font, and its width in pixels; -1 when a glyph is missing */
static int zm_compose(u32 font, const char *s, u16 *out, int max, int *width) {
    int n = 0, w = 0;
    u32 recs = ps2_r32(font + 4u), sx = ps2_r32(font + 0x40u);
    float k;
    memcpy(&k, &sx, 4);
    if (!(k > 0.25f && k < 4.0f)) k = 1.0f;                 /* CSwgGameFont +0x40: horizontal scale (0.857) */
    for (; *s; s++) {
        int g = zm_glyph(font, (unsigned char)*s);
        if (g < 0 || n >= max) return -1;
        out[n++] = (u16)g;
        w += ps2_r16(recs + 16u * (u32)g);
    }
    *width = (int)((float)w * k + 0.5f);
    return n;
}

/* The line, longest first; the shorter forms are used where the movie's OK / CANCEL bar leaves less room. */
static const char *const zm_text_xb[] = { "HOLD VIEW + MENU: QUIT GAME", "VIEW + MENU: QUIT GAME", "VIEW + MENU: QUIT", NULL };
static const char *const zm_text_ps[] = { "HOLD SELECT + START: QUIT GAME", "SELECT + START: QUIT GAME", "SELECT + START: QUIT",
                                          NULL };
/* the same without '+' (a movie font may lack it) */
static const char *const zm_text_xb2[] = { "HOLD VIEW AND MENU: QUIT GAME", "VIEW AND MENU: QUIT GAME", "VIEW/MENU: QUIT", NULL };
static const char *const zm_text_ps2[] = { "HOLD SELECT AND START: QUIT GAME", "SELECT AND START: QUIT GAME",
                                           "SELECT/START: QUIT", NULL };
/* with the ENHANCEMENTS page (the combo's panel offers it as X outside the pause menu): put in front of the forms
   above when the font chosen for them has every glyph (',' included), so the font and the fallbacks stay the same */
static const char *const zm_text_xb_enh[] = { "HOLD VIEW + MENU: QUIT, ENHANCEMENTS", "VIEW + MENU: QUIT, ENHANCEMENTS", NULL };
static const char *const zm_text_ps_enh[] = { "HOLD SELECT + START: QUIT, ENHANCEMENTS", "SELECT + START: QUIT, ENHANCEMENTS",
                                              NULL };
static const char *const zm_text_xb2_enh[] = { "HOLD VIEW AND MENU: QUIT, ENHANCEMENTS", "VIEW AND MENU: QUIT, ENHANCEMENTS",
                                               NULL };
static const char *const zm_text_ps2_enh[] = { "HOLD SELECT AND START: QUIT, ENHANCEMENTS",
                                               "SELECT AND START: QUIT, ENHANCEMENTS", NULL };
#define ZM_VARIANTS 5
static struct {
    u32 mgr, sig;
    int font, nv, set;
    struct { int n, w; u16 g[48]; char text[48]; } v[ZM_VARIANTS];
} zm_choice;
static void zm_choose(u32 mgr, int ps) {
    u32 nf = ps2_r32(mgr + 4u), sig = nf * 2654435761u ^ (u32)ps;
    if (nf > 32u) nf = 0;
    for (u32 i = 0; i < nf; i++) sig = sig * 31u + ps2_r32(mgr + 8u + i * 0x4Cu + 4u);
    if (zm_choice.mgr == mgr && zm_choice.sig == sig) return;
    zm_choice.mgr = mgr; zm_choice.sig = sig; zm_choice.font = -1; zm_choice.nv = 0;
    const char *const *sets[2] = { ps ? zm_text_ps : zm_text_xb, ps ? zm_text_ps2 : zm_text_xb2 };
    for (int set = 0; set < 2 && zm_choice.font < 0; set++)
        for (int pass = 0; pass < 2 && zm_choice.font < 0; pass++)        /* 16-pixel fonts first, then the rest */
            for (u32 i = 0; i < nf && zm_choice.font < 0; i++) {
                u32 f = mgr + 8u + i * 0x4Cu;
                if (ps2_r32(f) != ZM_FONT_VT || ps2_r32(f + 0x10u) == 0u) continue;
                if ((ps2_r16(f + 0x18u) <= 16u) != (pass == 0)) continue;
                int nv = 0;
                for (int t = 0; sets[set][t] && nv < ZM_VARIANTS; t++) {
                    int w, n = zm_compose(f, sets[set][t], zm_choice.v[nv].g, 48, &w);
                    if (n <= 0) { if (t == 0) break; continue; }    /* the font must have the full line */
                    zm_choice.v[nv].n = n; zm_choice.v[nv].w = w;
                    snprintf(zm_choice.v[nv].text, sizeof zm_choice.v[nv].text, "%s", sets[set][t]);
                    nv++;
                }
                if (nv) { zm_choice.font = (int)i; zm_choice.nv = nv; zm_choice.set = set; }
            }
    {   static int enh = -1;                           /* the ENHANCEMENTS forms in front, in the same font */
        if (enh < 0) { const char *e = getenv("ACZ_ENH_MENU"); enh = !(e && *e == '0'); }
        const char *const *es = zm_choice.set ? (ps ? zm_text_ps2_enh : zm_text_xb2_enh) : (ps ? zm_text_ps_enh : zm_text_xb_enh);
        u32 f = mgr + 8u + (u32)zm_choice.font * 0x4Cu;
        int ne = 0, w;
        u16 tmp[48];
        if (enh && zm_choice.font >= 0 && zm_choice.nv + 2 <= ZM_VARIANTS && zm_compose(f, es[0], tmp, 48, &w) > 0) {
            memmove(&zm_choice.v[2], &zm_choice.v[0], sizeof zm_choice.v[0] * (size_t)zm_choice.nv);
            for (int t = 0; es[t] && ne < 2; t++) {
                int n = zm_compose(f, es[t], zm_choice.v[ne].g, 48, &w);
                if (n <= 0) continue;
                zm_choice.v[ne].n = n; zm_choice.v[ne].w = w;
                snprintf(zm_choice.v[ne].text, sizeof zm_choice.v[ne].text, "%s", es[t]);
                ne++;
            }
            if (ne < 2) memmove(&zm_choice.v[ne], &zm_choice.v[2], sizeof zm_choice.v[0] * (size_t)zm_choice.nv);
            zm_choice.nv += ne;
        }
    }
    if (zm_hint_log)
        ps2_log("zero-menus: quit hint for font manager %08X (%u fonts): %s%s%s (font %d, %d px, %d forms)", mgr, nf,
                zm_choice.font >= 0 ? "\"" : "", zm_choice.font >= 0 ? zm_choice.v[0].text : "no font has the glyphs",
                zm_choice.font >= 0 ? "\"" : "", zm_choice.font, zm_choice.font >= 0 ? zm_choice.v[0].w : 0, zm_choice.nv);
}
/* width in pixels of string si of font (the game's bar), -1 if unknown */
static int zm_string_width(u32 font, u32 si) {
    if (!zm_gptr(font) || ps2_r32(font) != ZM_FONT_VT) return -1;
    u32 ns = ps2_r32(font + 0x10u), ng = ps2_r16(font + 0x14u), recs = ps2_r32(font + 4u);
    u32 lens = ps2_r32(font + 0x20u), offs = ps2_r32(font + 0x24u), data = ps2_r32(font + 0x28u), sx = ps2_r32(font + 0x40u);
    if (si >= ns || !zm_gptr(lens) || !zm_gptr(offs) || !zm_gptr(data) || !zm_gptr(recs)) return -1;
    u32 len = ps2_r16(lens + 2u * si), off = ps2_r32(offs + 4u * si), w = 0;
    for (u32 i = 0; i < len && i < 256u; i++) {
        u32 g = ps2_r16(data + 2u * (off + i));
        if (g < ng) w += ps2_r16(recs + 16u * g);
    }
    float k;
    memcpy(&k, &sx, 4);
    if (!(k > 0.25f && k < 4.0f)) k = 1.0f;
    return (int)((float)w * k + 0.5f);
}

/* the movie's name ("MainSelect", "CampaignSelect", ...): the SWG data the renderer's movie plays (+0x5D8 -> "SWG\0",
   name at +8) */
static const char *zm_movie_name(u32 rend, char *buf, size_t n) {
    u32 swg = ps2_r32(rend + 0x5D8u);
    buf[0] = 0;
    if (!zm_gptr(swg) || ps2_r32(swg) != 0x00475753u) return buf;
    size_t k = 0;
    for (; k + 1 < n; k++) {
        u8 c = ps2_r8(swg + 8u + (u32)k);
        if (c < 0x20u || c > 0x7Eu) break;
        buf[k] = (char)c;
    }
    buf[k] = 0;
    return buf;
}
/* screens without the hint: the memory card check, the logos and the title (ACZ_QUIT_HINT_SKIP, comma-separated) */
static char zm_skip[256] = "MemCheck,WakeLogo,Title";
static int zm_skipped(const char *name) {
    size_t n = strlen(name);
    if (!n) return 0;
    for (const char *p = zm_skip; *p; ) {
        const char *e = strchr(p, ',');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == n && !memcmp(p, name, n)) return 1;
        p += l;
        if (*p == ',') p++;
    }
    return 0;
}

/* Draw the hint with the game's own text function 0x14B4A0 (mgr, record, ctx = renderer + 0x1C, x, y, RGBA, flags):
   a free text record and string 0 of the chosen font are pointed at the hint for the duration of the call, then
   restored. alpha_k scales the opacity (follows the OK / CANCEL bar while it fades). Returns 1 when drawn. */
static int zm_busy;
static int zm_draw_hint(ps2_ctx *ctx, u32 rend, u32 mgr, float alpha_k, int left) {
    int ps = 0;
    if (!ps2_quit_hint_wanted(&ps) || !zm_gptr(mgr) || ps2_r32(mgr) != ZM_MGR_VT) return 0;
    char name[48];
    if (zm_skipped(zm_movie_name(rend, name, sizeof name))) return 0;
    zm_choose(mgr, ps);
    if (zm_choice.font < 0) return 0;
    u32 font = mgr + 8u + (u32)zm_choice.font * 0x4Cu;
    u32 lens = ps2_r32(font + 0x20u), offs = ps2_r32(font + 0x24u), data = ps2_r32(font + 0x28u);
    if (!zm_gptr(lens) || !zm_gptr(offs) || !zm_gptr(data)) return 0;
    u32 id = 0x200u;                                       /* a free text record, from the top */
    for (u32 k = 0x1FFu; k >= 0x100u; k--)
        if (ps2_r32(mgr + 0x4C8u + k * 0x14u) == 0xFFFFFFFFu && ps2_r32(mgr + 0x4C8u + k * 0x14u + 4u) == 0xFFFFFFFFu) {
            id = k;
            break;
        }
    if (id >= 0x200u) return 0;
    int v = 0;                                             /* the longest form that ends at x 231 and starts after left */
    while (v < zm_choice.nv && 231 - zm_choice.v[v].w < left) v++;
    if (v >= zm_choice.nv) return 0;
    for (int i = 0; i < zm_choice.v[v].n; i++) ps2_w16(ZM_PAGE + 2u * (u32)i, zm_choice.v[v].g[i]);
    u32 rec = mgr + 0x4C8u + id * 0x14u, saved_rec[5], saved_len = ps2_r16(lens), saved_off = ps2_r32(offs);
    for (int i = 0; i < 5; i++) saved_rec[i] = ps2_r32(rec + 4u * (u32)i);
    ps2_w32(rec, (u32)zm_choice.font);
    ps2_w32(rec + 4u, 0u);
    ps2_w16(rec + 8u, 1u); ps2_w16(rec + 10u, 6u);         /* as the game's static labels */
    ps2_w32(rec + 12u, 0x7FFFu);                           /* typewriter counter: everything shown */
    ps2_w32(rec + 16u, 0x00630100u);
    ps2_w16(lens, (u16)zm_choice.v[v].n);
    ps2_w32(offs, (ZM_PAGE - data) >> 1);
    float af = zm_hint_alpha * alpha_k * 128.0f + 0.5f;
    u32 al = af <= 0.0f ? 0u : af >= 128.0f ? 0x80u : (u32)af;
    s32 x = 231 - zm_choice.v[v].w, y = -197;     /* top right, on the line of the game's OK / CANCEL bar, flush with the
                                                  menu panel's right edge (x 487 of 512) */
    u32 args[7] = { mgr, id, rend + 0x1Cu, (u32)x, (u32)y, al << 24 | 0x000F6E7Du, 0u };
    zm_busy = 1;
    double t0 = zm_hint_log ? ps2_wall_seconds() : 0.0;
    zm_call(ctx, ZM_TEXT_DRAW, args, 7);
    if (zm_hint_log) {                                     /* the cost on the EE thread */
        static double sum, mx;
        static int cnt;
        double dt = ps2_wall_seconds() - t0;
        sum += dt; if (dt > mx) mx = dt;
        if (++cnt == 600) {
            ps2_log("zero-menus: quit hint drawn 600 times, EE time avg %.1f us max %.1f us", sum / cnt * 1e6, mx * 1e6);
            sum = mx = 0.0; cnt = 0;
        }
    }
    zm_busy = 0;
    ps2_w16(lens, (u16)saved_len);
    ps2_w32(offs, saved_off);
    for (int i = 0; i < 5; i++) ps2_w32(rec + 4u * (u32)i, saved_rec[i]);
    return 1;
}

/* Right after the game draws its OK / CANCEL bar (a text at y -200 on the left, from the first movie of the field) the
   hint joins it, so whatever the movie draws over the bar later (the dimming behind a YES / NO question, fades) covers
   the hint too; its opacity follows the bar's (the bar's steady alpha is 0x59). Screens without the bar (the
   briefing map) get it at the end of the display list. */
static struct { u32 mgr, rend, rgba; int on, right; } zm_bar;
static int zm_text_before(ps2_ctx *ctx, void *u) {
    (void)u;
    zm_bar.on = 0;
    if (zm_busy || !zm_hint_on) return 0;
    s32 x = ctx->r[7].sw[0], y = ctx->r[8].sw[0];
    if (y != -200 || x > -100) return 0;
    zm_bar.mgr = ctx->r[4].uw[0];
    zm_bar.rend = ctx->r[6].uw[0] - 0x1Cu;
    zm_bar.rgba = ctx->r[9].uw[0];
    {   u32 id = ctx->r[5].uw[0], rec = zm_bar.mgr + 0x4C8u + (id & 0x1FFu) * 0x14u;
        int w = id < 0x200u ? zm_string_width(zm_bar.mgr + 8u + ps2_r32(rec) * 0x4Cu, ps2_r32(rec + 4u)) : -1;
        zm_bar.right = w >= 0 ? x + w : 0;                 /* the bar's right end (unknown: assume half the screen) */
    }
    zm_bar.on = 1;
    return 0;
}
static int zm_text_after(ps2_ctx *ctx, void *u) {
    (void)u;
    if (!zm_bar.on || zm_busy) return 0;
    zm_bar.on = 0;
    u64 field = ps2_kernel_vblank_count();
    if (field == zm_hint_field || !zm_gptr(zm_bar.rend)) return 0;
    float k = (float)(zm_bar.rgba >> 24) / (float)0x59;
    if (k > 1.0f) k = 1.0f;
    if (zm_draw_hint(ctx, zm_bar.rend, zm_bar.mgr, k, zm_bar.right + 16)) zm_hint_field = field;
    return 0;
}

static void zm_mc_report(u64 field);
static int zm_rend_end(ps2_ctx *ctx, void *u) {
    (void)u;
    u32 rend = ctx->r[4].uw[0];
    u64 field = ps2_kernel_vblank_count();
    zero_menus_last_field = field;
    zm_memdump_tick();
    if (zm_gptr(rend)) {                                   /* a menu on screen (the TOUCH CONTROLS card by itself) */
        char nm[48];
        zm_movie_name(rend, nm, sizeof nm);
        if (strcmp(nm, "MemCheck") && strcmp(nm, "WakeLogo")) ps2_quit_note_menu();
    }
    zm_mc_report(field);
    if (!zm_hint_on || !zm_gptr(rend)) return 0;
    if (zm_swgtext_debug >= 2) {                           /* (=2: the renderer's vtable once) */
        static int vt_logged;
        u32 vt = ps2_r32(rend);
        if (!vt_logged && zm_gptr(vt)) {
            vt_logged = 1;
            char line[600]; int k = 0;
            for (u32 i = 0; i < 40u && k < (int)sizeof line - 16; i++) k += snprintf(line + k, sizeof line - (size_t)k, " %X", ps2_r32(vt + 4u * i));
            ps2_log("zero-menus: renderer %08X vtable %08X:%s", rend, vt, line);
        }
    }
    if (zm_hint_log) {                                     /* the movies drawn, whenever the set changes */
        static u64 lf;
        static char seq[160], last[160];
        char name[48];
        zm_movie_name(rend, name, sizeof name);
        if (field != lf) {
            if (strcmp(seq, last)) { ps2_log("zero-menus: field %llu movies: %s", (unsigned long long)lf, seq); strcpy(last, seq); }
            seq[0] = 0;
            lf = field;
        }
        size_t l = strlen(seq);
        snprintf(seq + l, sizeof seq - l, "%s%s@%08X", l ? " " : "", name[0] ? name : "?", rend);
    }
    if (field == zm_hint_field) return 0;                  /* once per field (drawn with the bar, or the first movie) */
    u32 holder = ps2_r32(rend + 0x30u), mgr = zm_gptr(holder) ? ps2_r32(holder + 0x18u) : 0u;
    if (zm_draw_hint(ctx, rend, mgr, 1.0f, -256)) zm_hint_field = field;
    return 0;
}

/* ---- the memory card check at boot, hidden (pt_flat 2026-10-29; the user: "it says PS2 Memory Card ... we are not
   playing on a PS2 ... no need to show this warning at the start, we know the saves load") ----
   InitMode's first scene is CModeWakeMemorycard (vtable 0x3CEA30): its update 0x288EF0 starts the card handler
   (CSelectMemorycard, scene + 0x10; 0x2B8DF8) and runs it each frame (0x2B8D38) until the check ends; its draw 0x289038
   draws the handler's swg movie "MemCheck" through 0x2B8DE0(handler, packet) -> the movie player 0x147368(handler +
   0x6110, packet), which returns the packet pointer past the movie's GS packets. The normal boot shows two texts on a
   black screen: "Accessing memory card (PS2). Do not turn power off or remove memory card (PS2)." (font 7 string 478,
   fields ~22-200) and "Load complete." (string 484, to ~255), then the logos. ACZ_MEMCHECK_HIDE (default on; 0 = the
   screen as the game draws it): the movie still runs and draws (its script, the card reads and their timing stay the
   game's), but its packets are dropped -- the packet pointer goes back to where it was -- so the screen stays black
   until the logos. Any other text of the movie (a card error, a question that waits for the pad: no card, damaged or
   missing save data, format) shows the screen from then on, and so does a check longer than ACZ_MEMCHECK_HIDE_MAX
   fields (default 900, 15 s). Saving and loading later go through other scenes (ModeMemoryCard*): untouched. */
#define ZM_MC_DRAW      0x002B8DE0u   /* CSelectMemorycard draw (a0 handler, a1 packet; returns the packet pointer) */
#define ZM_MC_DRAW_RA   0x00289074u   /* its call from the card scenes' draw 0x289038 (a0 = scene + 0x10) */
#define ZM_WAKEMC_VT    0x003CEA30u   /* CModeWakeMemorycard */
static int zm_mc_hide_on, zm_mc_in, zm_mc_show, zm_mc_max = 900;
static u32 zm_mc_pkt;
static int zm_mc_seen[2];
static u64 zm_mc_first, zm_mc_last, zm_mc_dropped;
static int zm_mc_draw_before(ps2_ctx *ctx, void *u) {
    (void)u;
    zm_mc_in = 0;
    u32 h = ctx->r[4].uw[0];
    if (!zm_gptr(h) || ctx->r[31].uw[0] != ZM_MC_DRAW_RA || ps2_r32(h - 0x10u) != ZM_WAKEMC_VT) return 0;
    u64 f = ps2_kernel_vblank_count();
    if (!zm_mc_first || f > zm_mc_last + 120u) {          /* a new check (one per boot) */
        zm_mc_first = f; zm_mc_show = 0; zm_mc_dropped = 0;
        zm_mc_seen[0] = zm_mc_seen[1] = 0;
    }
    zm_mc_last = f;
    if (!zm_mc_show && f - zm_mc_first > (u64)zm_mc_max) {
        zm_mc_show = 1;
        ps2_log("zero-menus: the memory card check runs past %d fields: shown (ACZ_MEMCHECK_HIDE_MAX)", zm_mc_max);
    }
    zm_mc_pkt = ctx->r[5].uw[0];
    zm_mc_in = 1;
    return 0;
}
static int zm_mc_text(ps2_ctx *ctx, void *u) {         /* (0x14B4A0 inside that draw) */
    (void)u;
    if (!zm_mc_in || zm_mc_show) return 0;
    u32 mgr = ctx->r[4].uw[0], id = ctx->r[5].uw[0];
    if (!zm_gptr(mgr) || id >= 0x200u) return 0;
    u32 rec = mgr + 0x4C8u + id * 0x14u, fi = ps2_r32(rec), si = ps2_r32(rec + 4u);
    if (fi == 7u && (si == 478u || si == 484u)) {        /* "Accessing memory card (PS2) ...", "Load complete." */
        zm_mc_seen[si == 484u]++;
        return 0;
    }
    char s[128];
    zm_font_string(mgr + 8u + fi * 0x4Cu, si, s, sizeof s);
    zm_mc_show = 1;
    ps2_log("zero-menus: the memory card check shows something else (font %u string %u: \"%s\"): shown from field %llu",
            fi, si, s, (unsigned long long)ps2_kernel_vblank_count());
    return 0;
}
static int zm_mc_draw_after(ps2_ctx *ctx, void *u) {
    (void)u;
    if (!zm_mc_in) return 0;
    zm_mc_in = 0;
    if (zm_mc_show) return 0;
    ctx->r[2].ud[0] = (u64)(s64)(s32)zm_mc_pkt;            /* the movie's packets are not sent */
    if (!zm_mc_dropped++)
        ps2_log("zero-menus: memory card check hidden from field %llu (ACZ_MEMCHECK_HIDE=0 shows it)",
                (unsigned long long)ps2_kernel_vblank_count());
    return 0;
}
/* one line once the check is over (the next movie drawn: zm_rend_end) */
static void zm_mc_report(u64 field) {
    if (!zm_mc_first || !zm_mc_dropped || field <= zm_mc_last + 2u) return;
    ps2_log("zero-menus: memory card check over (fields %llu-%llu, %llu frames hidden; frames with the access text %d, "
            "with \"Load complete.\" %d%s)", (unsigned long long)zm_mc_first, (unsigned long long)zm_mc_last,
            (unsigned long long)zm_mc_dropped, zm_mc_seen[0], zm_mc_seen[1], zm_mc_show ? "; shown at the end" : "");
    zm_mc_dropped = 0;
}
static void zm_mc_install(void) {
    const char *e = getenv("ACZ_MEMCHECK_HIDE");
    zm_mc_hide_on = !(e && *e == '0');
    e = getenv("ACZ_MEMCHECK_HIDE_MAX");
    if (e && *e) { zm_mc_max = atoi(e); if (zm_mc_max < 60) zm_mc_max = 60; }
    if (!zm_mc_hide_on) return;
    static const struct { u32 a, w; } chk[] = {
        { ZM_MC_DRAW, 0x27BDFFF0u }, { ZM_MC_DRAW + 4u, 0x24846110u }, { ZM_MC_DRAW + 0x10u, 0x08051CDAu },
        { ZM_MC_DRAW_RA - 8u, 0x0C0AE378u }, { ZM_WAKEMC_VT + 0x18u, 0x00289038u }, { ZM_TEXT_DRAW, 0x27BDFF80u } };
    for (unsigned i = 0; i < sizeof chk / sizeof chk[0]; i++)
        if (ps2_r32(chk[i].a) != chk[i].w) {
            ps2_log("zero-menus: code mismatch at %08X (%08X); the memory card check is shown", chk[i].a, ps2_r32(chk[i].a));
            zm_mc_hide_on = 0;
            return;
        }
    if (ps2_hook_before(ZM_MC_DRAW, zm_mc_draw_before, NULL, 60, "zero-memcheck") < 0
        || ps2_hook_after(ZM_MC_DRAW, zm_mc_draw_after, NULL, 60, "zero-memcheck") < 0
        || ps2_hook_before(ZM_TEXT_DRAW, zm_mc_text, NULL, 62, "zero-memcheck") < 0) {
        ps2_log("zero-menus: memory card check hooks FAILED; shown");
        zm_mc_hide_on = 0;
        return;
    }
    ps2_log("zero-menus: the memory card check at boot is hidden while it only accesses and loads (ACZ_MEMCHECK_HIDE=0 "
            "shows it)");
}

/* ACZ_DEBUG_MODE_JUMP=from:to (hex scene factories, zero_vr.c zb_name)  [debug] the first setNext(from) becomes
   setNext(to): e.g. 282A28:282BB8 makes a New Game open the debrief (results, the NEXT / RETRY / SAVE menu) at once */
static u32 zm_jump_from, zm_jump_to;
static int zm_setnext(ps2_ctx *ctx, void *u) {
    (void)u;
    if (zm_jump_from && ctx->r[4].uw[0] == zm_jump_from) {
        ps2_log("zero-menus: debug: setNext %08X -> %08X (ACZ_DEBUG_MODE_JUMP)", zm_jump_from, zm_jump_to);
        ctx->r[4].ud[0] = zm_jump_to;
        zm_jump_from = 0;
    }
    return 0;
}

/* ---- ENHANCEMENTS as a sixth row of MAIN MENU > OPTIONS (bug #237, touch_ux 2026-10-29) ----
   The user, on the title's OPTIONS screen: "the ENHANCEMENTS menu should be added here". The screen is a level of the
   MainSelect movie: five rows @51..@55 (CONTROLLER / LANGUAGE / DISPLAY / SOUND / SCREEN SETTINGS, font 4, x -224,
   y -146 + 30 k), the highlighted row's text drawn dark (RGB 000538 instead of 0F6E7D) on the cursor bar, a shape of
   the movie (character id 15, drawn by the renderer's shape draw 0x148E98 with its transform's translation at +0x30 /
   +0x34: x 22.5, y 57.5 + 30 k), the highlighted row's description at the bottom (@0, font 7, y 128); DOWN on the last
   row wraps to the first. A real row would need the movie's script edited, so the row is drawn and driven from here:
   - ENHANCEMENTS is drawn under SCREEN SETTINGS (y +30) in the rows' own font and colour through the game's text code
     (as the quit hint), whenever the five rows show (their texts checked, so other languages / levels are left alone);
   - DOWN on SCREEN SETTINGS selects it instead of wrapping (the press is kept from the game): the game's cursor stays
     on SCREEN SETTINGS, its bar is drawn one row lower (the shape's translation +30 for that call), SCREEN SETTINGS in
     the normal colour, ENHANCEMENTS in the highlighted one, the description hidden (or ours where the help font has
     the glyphs); UP goes back to SCREEN SETTINGS, DOWN passes on to the game (it wraps to CONTROLLER SETTINGS), A /
     Cross opens the ENHANCEMENTS page (ps2_quit.c; the TOUCH CONTROLLERS row is in its CONTROLS category), B / Circle
     goes back as the game's own.
   ACZ_ENH_OPTIONS_ROW=0: the OPTIONS screen as the game draws it (the View + Menu panel still offers ENHANCEMENTS). */
#define ZM_SHAPE_DRAW  0x00148E98u   /* CSwgAC55Renderer vtable +0x20: a shape (a0 renderer, a1 shape, a2 transform) */
#define ZM_OPT_PAGE    (ZM_PAGE + 0x800u)   /* our glyph indices on the hint's guest page (second half) */
static const char *const zm_opt_rows[5] = { "CONTROLLER SETTINGS", "LANGUAGE SETTINGS", "DISPLAY SETTINGS",
                                            "SOUND SETTINGS", "SCREEN SETTINGS" };
static int zm_opt_on;
static volatile u64 zm_opt_field;            /* the last field the five OPTIONS rows were drawn */
static volatile int zm_opt_hl = -1;          /* the row the game highlights (0..4) in that field */
static volatile int zm_opt_active;           /* our row is selected (the renderer thread's input side sets it) */
static u32 zm_opt_rowok;                     /* this field: bit k = row k's text checked */
static u64 zm_opt_rowf;
static struct { u32 mgr, key[5]; int ok[5]; } zm_opt_cache;   /* (mgr, font << 16 | string) of each row, checked once */
static struct { u32 mgr, rend, ctx, rgba, font; s32 x, y; int on; } zm_opt_last;   /* the SCREEN SETTINGS call */
static int zm_opt_logged;

/* 1 when text record id of mgr is row k of OPTIONS (cached per record content) */
static int zm_opt_is_row(u32 mgr, u32 id, int k) {
    u32 rec = mgr + 0x4C8u + id * 0x14u, key = (ps2_r32(rec) & 0xFFu) << 16 | (ps2_r32(rec + 4u) & 0xFFFFu);
    if (zm_opt_cache.mgr != mgr) { memset(&zm_opt_cache, 0, sizeof zm_opt_cache); zm_opt_cache.mgr = mgr; }
    if (zm_opt_cache.key[k] != key + 1u) {
        char s[64];
        zm_opt_cache.key[k] = key + 1u;
        zm_font_string(mgr + 8u + ps2_r32(rec) * 0x4Cu, ps2_r32(rec + 4u), s, sizeof s);
        zm_opt_cache.ok[k] = !strcmp(s, zm_opt_rows[k]);
    }
    return zm_opt_cache.ok[k];
}
/* our text (the glyph indices of `s` in font fi) drawn with the game's text code at (x, y), as the quit hint is */
static int zm_draw_text(ps2_ctx *ctx, u32 rend, u32 mgr, int fi, const char *s, s32 x, s32 y, u32 rgba, u32 page) {
    u32 font = mgr + 8u + (u32)fi * 0x4Cu;
    if (ps2_r32(font) != ZM_FONT_VT) return 0;
    u16 g[64];
    int w, n = zm_compose(font, s, g, 64, &w);
    if (n <= 0) return 0;
    u32 lens = ps2_r32(font + 0x20u), offs = ps2_r32(font + 0x24u), data = ps2_r32(font + 0x28u);
    if (!zm_gptr(lens) || !zm_gptr(offs) || !zm_gptr(data)) return 0;
    u32 id = 0x200u;
    for (u32 k = 0x1FFu; k >= 0x100u; k--)
        if (ps2_r32(mgr + 0x4C8u + k * 0x14u) == 0xFFFFFFFFu && ps2_r32(mgr + 0x4C8u + k * 0x14u + 4u) == 0xFFFFFFFFu) {
            id = k;
            break;
        }
    if (id >= 0x200u) return 0;
    for (int i = 0; i < n; i++) ps2_w16(page + 2u * (u32)i, g[i]);
    u32 rec = mgr + 0x4C8u + id * 0x14u, saved_rec[5], saved_len = ps2_r16(lens), saved_off = ps2_r32(offs);
    for (int i = 0; i < 5; i++) saved_rec[i] = ps2_r32(rec + 4u * (u32)i);
    ps2_w32(rec, (u32)fi);
    ps2_w32(rec + 4u, 0u);
    ps2_w16(rec + 8u, 1u); ps2_w16(rec + 10u, 6u);
    ps2_w32(rec + 12u, 0x7FFFu);
    ps2_w32(rec + 16u, 0x00630100u);
    ps2_w16(lens, (u16)n);
    ps2_w32(offs, (page - data) >> 1);
    u32 args[7] = { mgr, id, rend + 0x1Cu, (u32)x, (u32)y, rgba, 0u };
    zm_busy = 1;
    zm_call(ctx, ZM_TEXT_DRAW, args, 7);
    zm_busy = 0;
    ps2_w16(lens, (u16)saved_len);
    ps2_w32(offs, saved_off);
    for (int i = 0; i < 5; i++) ps2_w32(rec + 4u * (u32)i, saved_rec[i]);
    return 1;
}
static int zm_opt_text_before(ps2_ctx *ctx, void *u) {
    (void)u;
    zm_opt_last.on = 0;
    if (zm_busy) return 0;
    u32 mgr = ctx->r[4].uw[0], id = ctx->r[5].uw[0];
    if (!zm_gptr(mgr) || ps2_r32(mgr) != ZM_MGR_VT) return 0;
    u64 f = ps2_kernel_vblank_count();
    if (f != zm_opt_rowf) { zm_opt_rowf = f; zm_opt_rowok = 0; }
    s32 x = ctx->r[7].sw[0], y = ctx->r[8].sw[0];
    u32 rgba = ctx->r[9].uw[0];
    int active = zm_opt_active && f - zm_opt_field <= 3u;
    if (id >= 51u && id <= 55u && x == -224 && y == -146 + 30 * (s32)(id - 51u) && zm_opt_is_row(mgr, id, (int)(id - 51u))) {
        int k = (int)(id - 51u);
        zm_opt_rowok |= 1u << k;
        if ((rgba & 0xFFFFFFu) == 0x000538u) zm_opt_hl = k;
        if (k == 4) {
            if (zm_opt_rowok == 31u) zm_opt_field = f;       /* all five rows of OPTIONS, in order */
            if (active && (rgba & 0xFFFFFFu) == 0x000538u) ctx->r[9].ud[0] = (u64)((rgba & 0xFF000000u) | 0x0F6E7Du);
            zm_opt_last.mgr = mgr; zm_opt_last.rend = ctx->r[6].uw[0] - 0x1Cu; zm_opt_last.ctx = ctx->r[6].uw[0];
            zm_opt_last.rgba = rgba; zm_opt_last.x = x; zm_opt_last.y = y; zm_opt_last.on = zm_opt_rowok == 31u;
            zm_opt_last.font = ps2_r32(mgr + 0x4C8u + id * 0x14u) & 0xFFu;
        }
    } else if (id == 0u && y == 128 && active && f - zm_opt_field <= 1u) {
        ctx->r[9].ud[0] = (u64)(rgba & 0x00FFFFFFu);        /* the game's description of SCREEN SETTINGS: hidden */
    }
    return 0;
}
static int zm_opt_text_after(ps2_ctx *ctx, void *u) {
    (void)u;
    if (!zm_opt_last.on || zm_busy) return 0;
    zm_opt_last.on = 0;
    int active = zm_opt_active;
    u32 a = zm_opt_last.rgba & 0xFF000000u;
    if (!zm_draw_text(ctx, zm_opt_last.rend, zm_opt_last.mgr, (int)zm_opt_last.font, "ENHANCEMENTS", zm_opt_last.x, zm_opt_last.y + 30,
                      a | (active ? 0x000538u : 0x0F6E7Du), ZM_OPT_PAGE)) {
        if (!zm_opt_logged) { zm_opt_logged = 1; ps2_log("zero-menus: OPTIONS: the rows' font lacks a glyph of ENHANCEMENTS: no row"); }
        return 0;
    }
    if (!zm_opt_logged) { zm_opt_logged = 1; ps2_log("zero-menus: OPTIONS: ENHANCEMENTS drawn as a sixth row (ACZ_ENH_OPTIONS_ROW)"); }
    if (active) {                                          /* our description, where the help font has the glyphs */
        static const char *const desc[] = { "Adjust the enhancements.", NULL };   /* (the help font has no l g v b S) */
        for (int i = 0; desc[i]; i++)
            if (zm_draw_text(ctx, zm_opt_last.rend, zm_opt_last.mgr, 7, desc[i], -229, 128, 0x6B0F6E7Du, ZM_OPT_PAGE + 0x200u))
                break;
    }
    return 0;
}
/* the cursor bar (shape 15) one row lower while our row is selected; the transform is restored after the call */
static struct { u32 xf; u32 ty; int on; } zm_opt_bar;
static int zm_opt_shape_before(ps2_ctx *ctx, void *u) {
    (void)u;
    zm_opt_bar.on = 0;
    u64 f = ps2_kernel_vblank_count();
    if (!zm_opt_active || f - zm_opt_field > 3u) return 0;
    u32 obj = ctx->r[5].uw[0], xf = ctx->r[6].uw[0];
    if (!zm_gptr(obj) || !zm_gptr(xf) || ps2_r32(obj + 0xCu) != 15u) return 0;
    u32 tw = ps2_r32(xf + 0x34u);
    float ty;
    memcpy(&ty, &tw, 4);
    if (fabsf(ty - (57.5f + 30.0f * 4.0f)) > 2.0f) return 0;      /* the bar on SCREEN SETTINGS */
    zm_opt_bar.xf = xf; zm_opt_bar.ty = tw; zm_opt_bar.on = 1;
    ty += 30.0f;
    memcpy(&tw, &ty, 4);
    ps2_w32(xf + 0x34u, tw);
    return 0;
}
static int zm_opt_shape_after(ps2_ctx *ctx, void *u) {
    (void)ctx; (void)u;
    if (zm_opt_bar.on) { ps2_w32(zm_opt_bar.xf + 0x34u, zm_opt_bar.ty); zm_opt_bar.on = 0; }
    return 0;
}
/* the input side (renderer thread; ps2_quit.c calls it before the game reads the pad): the OPTIONS screen's row */
static int zm_opt_pad(ps2_pad_state *st) {
    static int held_up, held_down, held_ok, swallow;          /* 1 = the press is ours until released */
    u64 f = ps2_kernel_vblank_count();
    int shown = zm_opt_field && f - zm_opt_field <= 3u;
    int up = (st->buttons & (1u << PS2_ACT_UP)) || st->ly < 56, down = (st->buttons & (1u << PS2_ACT_DOWN)) || st->ly > 200;
    int ok = (st->buttons & (1u << PS2_ACT_CROSS)) != 0;
    int pu = up && !held_up, pd = down && !held_down, po = ok && !held_ok;
    held_up = up; held_down = down; held_ok = ok;
    if (!shown) { if (zm_opt_active) ps2_log("zero-menus: OPTIONS left: ENHANCEMENTS row deselected"); zm_opt_active = 0; swallow = 0; return 0; }
    if (!zm_opt_active) {
        if (pd && zm_opt_hl == 4) { zm_opt_active = 1; swallow = 1; ps2_log("zero-menus: OPTIONS: ENHANCEMENTS row selected"); }
    } else if (pu) { zm_opt_active = 0; swallow = 1; ps2_log("zero-menus: OPTIONS: back to SCREEN SETTINGS"); }
    else if (pd) { zm_opt_active = 0; swallow = 0; }          /* the game wraps to CONTROLLER SETTINGS */
    else if (po) { swallow = 1; ps2_log("zero-menus: OPTIONS: ENHANCEMENTS row chosen: the page opens");
                   ps2_quit_enh_open(PS2_ENH_FROM_MENUS); }
    else if (st->buttons & ((1u << PS2_ACT_CIRCLE) | (1u << PS2_ACT_TRIANGLE))) zm_opt_active = 0;
    if (swallow) {                                            /* the press that moved our cursor stays ours */
        if (!up && !down && !ok) swallow = 0;
        st->buttons &= (u16)~((1u << PS2_ACT_UP) | (1u << PS2_ACT_DOWN) | (1u << PS2_ACT_CROSS));
        st->ly = PS2_PAD_ANALOG_NEUTRAL;
    }
    return 0;
}
static void zm_opt_install(void) {
    const char *e = getenv("ACZ_ENH_OPTIONS_ROW");
    zm_opt_on = !(e && *e == '0');
    e = getenv("ACZ_ENH_MENU");
    if (e && *e == '0') zm_opt_on = 0;
    if (!zm_opt_on) { ps2_log("zero-menus: OPTIONS left as the game draws it (ACZ_ENH_OPTIONS_ROW=0)"); return; }
    if (ps2_r32(ZM_SHAPE_DRAW) != 0x27BDFEF0u) { ps2_log("zero-menus: shape draw code mismatch; no ENHANCEMENTS row in OPTIONS"); return; }
    if (!zm_page_mapped) {
        if (ps2_pt[ZM_PAGE >> PS2_PAGE_BITS]) { ps2_log("zero-menus: guest page %08X in use; no ENHANCEMENTS row in OPTIONS", ZM_PAGE); return; }
        ps2_mem_map(ZM_PAGE, sizeof zm_page, zm_page);
        zm_page_mapped = 1;
    }
    if (ps2_hook_before(ZM_TEXT_DRAW, zm_opt_text_before, NULL, 59, "zero-menus-options") < 0
        || ps2_hook_after(ZM_TEXT_DRAW, zm_opt_text_after, NULL, 59, "zero-menus-options") < 0
        || ps2_hook_before(ZM_SHAPE_DRAW, zm_opt_shape_before, NULL, 60, "zero-menus-options") < 0
        || ps2_hook_after(ZM_SHAPE_DRAW, zm_opt_shape_after, NULL, 60, "zero-menus-options") < 0) {
        ps2_log("zero-menus: OPTIONS row hooks FAILED");
        return;
    }
    ps2_quit_set_menu_pad_fn(zm_opt_pad);
    ps2_log("zero-menus: MAIN MENU > OPTIONS gets an ENHANCEMENTS row (ACZ_ENH_OPTIONS_ROW=0 off)");
}

void zero_menus_install(void) {
    zm_trace_install();
    zm_mc_install();                                       /* the memory card check at boot (ACZ_MEMCHECK_HIDE) */
    zm_opt_install();                                      /* OPTIONS > ENHANCEMENTS (bug #237) */
    {   const char *j = getenv("ACZ_DEBUG_MODE_JUMP");
        char *end;
        if (j && *j) {
            zm_jump_from = (u32)strtoul(j, &end, 16);
            zm_jump_to = *end == ':' ? (u32)strtoul(end + 1, NULL, 16) : 0u;
            if (zm_jump_from && zm_jump_to) ps2_hook_before(0x00281A88u, zm_setnext, NULL, 61, "zero-menus-debug");
            else zm_jump_from = 0;
        }
    }
    zm_swgtext_debug = getenv("ACZ_DEBUG_SWGTEXT") ? (atoi(getenv("ACZ_DEBUG_SWGTEXT")) >= 2 ? 2 : 1) : 0;
    if (zm_swgtext_debug) ps2_hook_before(ZM_TEXT_DRAW, zm_text_enter, NULL, 60, "zero-menus");
    {   const char *s = getenv("ACZ_DEBUG_SWGSHAPE");
        while (s && *s && zm_shape_n < 8) {
            char *end;
            zm_shape_f[zm_shape_n++] = strtoull(s, &end, 10);
            if (end == s) break;
            s = end;
            while (*s == ',') s++;
        }
        if (zm_shape_n) ps2_hook_before(0x00148E98u, zm_shape_enter, NULL, 60, "zero-menus-debug");
    }
    /* ACZ_QUIT_HINT=0 turns the hint off; ACZ_QUIT_HINT_ALPHA=1 its opacity relative to the bar's (the game's own menu
       text: 0.2-0.85; up to 1.43 = opaque; 0.5 before bug #74); ACZ_QUIT_HINT_SKIP=MemCheck,WakeLogo,Title the movies
       without it */
    const char *e = getenv("ACZ_QUIT_HINT");
    zm_hint_on = !(e && *e == '0');
    zm_hint_alpha = 1.0f;
    e = getenv("ACZ_QUIT_HINT_SKIP");
    if (e) snprintf(zm_skip, sizeof zm_skip, "%s", e);
    e = getenv("ACZ_QUIT_HINT_ALPHA");
    if (e && *e) { float v = (float)atof(e); if (v >= 0.05f && v <= 1.43f) zm_hint_alpha = v; }
    zm_hint_log = zm_swgtext_debug || getenv("ACZ_DEBUG_QUIT_HINT") != NULL;
    static const struct { u32 a, w; } chk[] = {
        { ZM_REND_END, 0x27BDFFF0u }, { ZM_REND_END + 0xCu, 0x08052320u }, { ZM_TEXT_THUNK + 8u, 0x8C840018u },
        { ZM_TEXT_DRAW, 0x27BDFF80u }, { ZM_TEXT_DRAW + 0x108u, 0x0C052E7Cu } };
    for (unsigned i = 0; i < sizeof chk / sizeof chk[0]; i++)
        if (ps2_r32(chk[i].a) != chk[i].w) {
            ps2_log("zero-menus: code mismatch at %08X (%08X); no quit hint, menus not seen", chk[i].a, ps2_r32(chk[i].a));
            zm_hint_on = 0;
            return;
        }
    /* the end of every movie's display list: menus on screen (zero_menus_last_field) and the hint's fallback place */
    if (ps2_hook_before(ZM_REND_END, zm_rend_end, NULL, 60, "zero-menus") < 0) {
        zm_hint_on = 0;
        ps2_log("zero-menus: movie hook FAILED; no quit hint");
        return;
    }
    if (!zm_hint_on) { ps2_log("zero-menus: quit hint off (ACZ_QUIT_HINT=0)"); return; }
    if (!zm_page_mapped && ps2_pt[ZM_PAGE >> PS2_PAGE_BITS]) {
        ps2_log("zero-menus: guest page %08X in use; quit hint off", ZM_PAGE);
        zm_hint_on = 0;
        return;
    }
    if (!zm_page_mapped) ps2_mem_map(ZM_PAGE, sizeof zm_page, zm_page);
    zm_page_mapped = 1;
    if (ps2_hook_before(ZM_TEXT_DRAW, zm_text_before, NULL, 61, "zero-menus") < 0
        || ps2_hook_after(ZM_TEXT_DRAW, zm_text_after, NULL, 60, "zero-menus") < 0) {
        zm_hint_on = 0;
        ps2_log("zero-menus: quit hint hooks FAILED");
        return;
    }
    ps2_log("zero-menus: quit hint in the menus between missions on (ACZ_QUIT_HINT=0 off), opacity %.2f", zm_hint_alpha);
}
