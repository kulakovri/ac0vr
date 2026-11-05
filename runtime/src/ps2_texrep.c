/* Texture replacement by content hash.

   Packs live in textures/<set>/ (ACZ_TEX_DIR, else ./textures, else <exe>/../../textures). A file
   HHHHHHHH_P_WxH[_label].png stands in for every GS decode (ps2_gs.c current_texture_inner) whose 32-bit content
   hash (palette + texels, as the texture cache computes it) is HHHHHHHH, whose PSM is P (decimal: 0 CT32, 19 T8,
   20 T4, ...) and whose decoded size is W x H. The PNG is RGBA in the decode's convention (alpha 255 = GS 0x80,
   colours as the CLUT holds them) at W x H or an integer multiple of it (2x, 4x, ... up to 8x): the renderer samples
   larger images in decoded-texel space (gs.frag, rep_scale), so the game's UVs, wrap and clamp modes still apply.

   A set is active depending on the words in its folder name: "xbox" while Xbox button glyphs are shown, "ps" while
   PlayStation ones are, "touch" while the Quest Touch controllers' are (ctl_priority: Touch is the active
   controller), "modern" / "classic" with that controller layout; any other name (hud, menu) is always on.
   When several active sets have a file for the same texture, the one with more conditions wins (xbox_modern over
   xbox). Lookups happen only when the GS texture cache misses; the active controller's class (ps2_video.c's
   registry: the device used last) is looked at once per frame on the renderer thread, and a change is published as a
   generation the GS thread checks before its cache lookups.

   Debug switches for building packs: PS2_TEX_HASHLOG=1 logs every new decode once (hash, PSM, size, VRAM address,
   field), PS2_TEX_DUMP=<dir> writes each one as <dir>/HHHHHHHH_P_WxH.png with alpha. */
#include "ps2_runtime.h"
#include "ps2_texrep.h"
#include "ps2_settings.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

extern u32 ps2_gs_frame_count;

enum { C_XBOX = 1, C_PS = 2, C_MODERN = 4, C_CLASSIC = 8, C_TOUCH = 16 };

#define TEXREP_SETS 32
typedef struct {
    char name[48];
    u32  cond;               /* C_* that must all hold */
    int  rank;               /* number of conditions: the more specific set wins */
} texrep_set;

typedef struct {
    u32  hash, psm, w, h;
    u32  set, salt;
    u32  iw, ih;
    char *path;
    u8   *rgba;              /* decoded on first use, kept */
    int  bad, used;
    s32  next;
} texrep_entry;

static texrep_set   sets[TEXREP_SETS];
static u32          nsets;
static texrep_entry *rep;
static u32          nrep, caprep;
#define REP_BUCKETS 1024u
static s32          rep_head[REP_BUCKETS];
static size_t       rep_bytes;
static const char  *rep_last_set;

/* bits 0-1 the glyph class (PS2_GLYPHS_*), bit 2 modern layout, bits 3.. generation; written by the renderer thread
   (ps2_texrep_pad) */
static u32 texrep_state;
static u32 texrep_seen = 0xFFFFFFFFu, texrep_active = C_PS | C_CLASSIC;

static int   log_on, dump_n, dump_max = 4096;
static char  dump_dir[512];

static u32 rep_bucket(u32 hash, u32 psm, u32 w, u32 h) {
    return ((hash ^ (psm * 0x9E3779B1u) ^ (w << 16) ^ h) * 0x85EBCA6Bu) >> 22;
}

static int is_dir(const char *p) {
    SDL_PathInfo pi;
    return SDL_GetPathInfo(p, &pi) && pi.type == SDL_PATHTYPE_DIRECTORY;
}

/* HHHHHHHH_P_WxH[_label].png */
static int parse_name(const char *f, u32 *hash, u32 *psm, u32 *w, u32 *h) {
    char *end;
    for (int k = 0; k < 8; k++) if (!isxdigit((unsigned char)f[k])) return 0;
    if (f[8] != '_') return 0;
    *hash = (u32)strtoul(f, NULL, 16);
    *psm = (u32)strtoul(f + 9, &end, 10);
    if (end == f + 9 || *end != '_') return 0;
    *w = (u32)strtoul(end + 1, &end, 10);
    if (*end != 'x') return 0;
    *h = (u32)strtoul(end + 1, &end, 10);
    if (*end != '_' && *end != '.') return 0;
    return *w && *h && *w <= 1024u && *h <= 1024u && *psm < 64u;
}

static u32 set_conditions(const char *name, int *rank) {
    char tok[48];
    u32 c = 0;
    const char *p = name;
    *rank = 0;
    while (*p) {
        size_t n = 0;
        while (*p && *p != '_' && *p != '-' && n + 1 < sizeof tok) tok[n++] = (char)tolower((unsigned char)*p++);
        tok[n] = 0;
        while (*p == '_' || *p == '-') p++;
        if (!strcmp(tok, "xbox"))         c |= C_XBOX;
        else if (!strcmp(tok, "ps"))      c |= C_PS;
        else if (!strcmp(tok, "touch"))   c |= C_TOUCH;
        else if (!strcmp(tok, "modern"))  c |= C_MODERN;
        else if (!strcmp(tok, "classic")) c |= C_CLASSIC;
    }
    for (u32 b = c; b; b &= b - 1u) (*rank)++;
    return c;
}

static int set_wanted(const char *name) {
    /* ACZ_TEX_SETS=hud,xbox: load only these sets (default: every folder) */
    const char *e = getenv("ACZ_TEX_SETS"), *p;
    size_t n = strlen(name);
    if (!e || !*e) return 1;
    for (p = e; *p; ) {
        const char *q = p;
        while (*q && *q != ',' && *q != ' ') q++;
        if ((size_t)(q - p) == n && !SDL_strncasecmp(p, name, n)) return 1;
        p = *q ? q + 1 : q;
    }
    return 0;
}

static void add_file(u32 set, const char *dir, const char *fname) {
    u32 hash, psm, w, h, b;
    texrep_entry *e;
    if (strchr(fname, '/') || strchr(fname, '\\')) return;            /* top level of the set only */
    if (!parse_name(fname, &hash, &psm, &w, &h)) {
        ps2_log("texrep: %s/%s: not named HHHHHHHH_PSM_WxH[_label].png, skipped", sets[set].name, fname);
        return;
    }
    b = rep_bucket(hash, psm, w, h);
    for (s32 i = rep_head[b]; i >= 0; i = rep[i].next)
        if (rep[i].set == set && rep[i].hash == hash && rep[i].psm == psm && rep[i].w == w && rep[i].h == h) {
            ps2_log("texrep: %s/%s duplicates an earlier file of the set, skipped", sets[set].name, fname);
            return;
        }
    if (nrep == caprep) {
        u32 nc = caprep ? caprep * 2u : 256u;
        texrep_entry *n = (texrep_entry *)realloc(rep, nc * sizeof *n);
        if (!n) return;
        rep = n;
        caprep = nc;
    }
    e = &rep[nrep];
    memset(e, 0, sizeof *e);
    e->hash = hash; e->psm = psm; e->w = w; e->h = h; e->set = set;
    e->salt = (nrep + 1u) * 0x9E3779B1u;                               /* odd multiplier: never 0 */
    {   size_t n = strlen(dir) + strlen(fname) + 2u;
        e->path = (char *)malloc(n);
        if (!e->path) return;
        snprintf(e->path, n, "%s/%s", dir, fname);
    }
    e->next = rep_head[b];
    rep_head[b] = (s32)nrep;
    nrep++;
}

static SDL_EnumerationResult SDLCALL set_cb(void *ud, const char *dirname, const char *fname) {
    char path[1024];
    (void)ud;
    if (nsets >= TEXREP_SETS || fname[0] == '.') return SDL_ENUM_CONTINUE;
    {   size_t n = strlen(dirname);
        int sep = n && (dirname[n - 1] == '/' || dirname[n - 1] == '\\');
        snprintf(path, sizeof path, "%s%s%s", dirname, sep ? "" : "/", fname);
    }
    if (!is_dir(path) || !set_wanted(fname)) return SDL_ENUM_CONTINUE;
    snprintf(sets[nsets].name, sizeof sets[nsets].name, "%s", fname);
    sets[nsets].cond = set_conditions(fname, &sets[nsets].rank);
    nsets++;
    return SDL_ENUM_CONTINUE;
}

static void index_packs(void) {
    char root[512], dir[640];
    const char *e = getenv("ACZ_TEX_DIR");      /* folder holding the texture sets (default: ./textures, then
                                                    textures next to the build: <exe>/../../textures) */
    u64 t0 = SDL_GetTicksNS();
    for (u32 b = 0; b < REP_BUCKETS; b++) rep_head[b] = -1;
    if (e && *e) snprintf(root, sizeof root, "%s", e);
    else if (is_dir("textures")) snprintf(root, sizeof root, "textures");
    else {
        const char *base = SDL_GetBasePath();
        snprintf(root, sizeof root, "%s../../textures", base ? base : "");
    }
    if (!is_dir(root)) {
        ps2_log("texrep: no texture packs (%s is not a folder)", root);
        return;
    }
    SDL_EnumerateDirectory(root, set_cb, NULL);
    for (u32 s = 0; s < nsets; s++) {
        int n = 0;
        char **names;
        u32 before = nrep;
        snprintf(dir, sizeof dir, "%.511s/%.47s", root, sets[s].name);
        names = SDL_GlobDirectory(dir, "*.png", SDL_GLOB_CASEINSENSITIVE, &n);
        for (int k = 0; names && k < n; k++) add_file(s, dir, names[k]);
        SDL_free(names);
        ps2_log("texrep: set %-14s %4u files%s%s%s%s%s", sets[s].name, nrep - before,
                sets[s].cond & C_XBOX ? "  [Xbox glyphs]" : "", sets[s].cond & C_PS ? "  [PS glyphs]" : "",
                sets[s].cond & C_TOUCH ? "  [Touch glyphs]" : "",
                sets[s].cond & C_MODERN ? "  [modern layout]" : "",
                sets[s].cond & C_CLASSIC ? "  [classic layout]" : "");
    }
    ps2_log("texrep: %u replacements in %u sets under %s, indexed in %.1f ms (ACZ_TEX_REPLACE=0 turns them off)",
            nrep, nsets, root, (double)(SDL_GetTicksNS() - t0) / 1e6);
}

static void preload_start(void);
int ps2_texrep_init(void) {
    const char *e;
    int r = 0;
    e = getenv("PS2_TEX_HASHLOG");     /* [debug] log each new texture decode once: hash, PSM, size, address */
    log_on = e && *e && *e != '0';
    e = getenv("PS2_TEX_DUMP");        /* [debug] <dir> (1 = out/texdump): each new decode as HHHHHHHH_P_WxH.png */
    if (e && *e && strcmp(e, "0")) {
        snprintf(dump_dir, sizeof dump_dir, "%s", strcmp(e, "1") ? e : "out/texdump");
        SDL_CreateDirectory(dump_dir);
        e = getenv("PS2_TEX_DUMP_MAX"); /* files at most (4096) */
        if (e && atoi(e) > 0) dump_max = atoi(e);
        ps2_log("texrep: dumping new textures to %s (at most %d)", dump_dir, dump_max);
    }
    e = getenv("ACZ_TEX_REPLACE");     /* 0: no texture replacements at all (default on) */
    if (!(e && *e == '0')) { index_packs(); preload_start(); }
    else ps2_log("texrep: replacements off (ACZ_TEX_REPLACE=0)");
    ps2_texrep_changed();
    if (nrep) r |= PS2_TEXREP_REPLACE;
    if (log_on || dump_dir[0]) r |= PS2_TEXREP_NOTE;
    return r;
}

int ps2_texrep_changed(void) {
    u32 s = __atomic_load_n(&texrep_state, __ATOMIC_ACQUIRE);
    if (s == texrep_seen) return 0;
    texrep_seen = s;
    texrep_active = ((s & 3u) == PS2_GLYPHS_XBOX ? C_XBOX : (s & 3u) == PS2_GLYPHS_TOUCH ? C_TOUCH : C_PS)
                  | ((s & 4u) ? C_MODERN : C_CLASSIC);
    return 1;
}

/* ACZ_TEX_PRELOAD (ov_hitch, 2026-10-25): the replacement PNGs were decoded on first use by the GS worker thread, inside
   the field that first needs them -- the HUD atlas (1024 x 1024, 6.7-12.3 ms in the headset logs) at the first flight
   frame, a radio message's 20-38 glyphs together in one field (0.1 ms each on an idle machine; with other load 1-5 ms,
   one cold file read 45 ms): in the sims 38-69 decodes a mission, the worst field 6-179 ms, the world standing up to 11
   fields and the headset images held. Default 1: a background thread decodes every set except the menu pictures (menu*:
   95 MB decoded, shown only between missions) right after the index, at start-up (hud + glyph sets: 90 files, 13.6 MB,
   27-674 ms on that thread during the logos); the GS thread then finds them decoded (0 decodes in flight; a texture it
   needs while the thread is still on it waits for that one decode, as before). `all` = every set, `0` = decode on first
   use as before. rep_mx guards a decode (both threads); a decoded entry is published with release. */
static pthread_mutex_t rep_mx = PTHREAD_MUTEX_INITIALIZER;
static int rep_load(texrep_entry *e, int quiet);
static int preload_mode = -1;                      /* 0 off, 1 all but menu*, 2 all */
static int preload_wanted(const char *set) {
    if (preload_mode == 2) return 1;
    return SDL_strncasecmp(set, "menu", 4) != 0;
}
static int rep_ready(texrep_entry *e) {            /* decoded (any thread), decoding here if needed */
    if (__atomic_load_n(&e->rgba, __ATOMIC_ACQUIRE)) return 1;
    pthread_mutex_lock(&rep_mx);
    if (!e->rgba && !e->bad) rep_load(e, 0);
    pthread_mutex_unlock(&rep_mx);
    return __atomic_load_n(&e->rgba, __ATOMIC_ACQUIRE) != NULL;
}
static void *preload_main(void *arg) {
    u64 t0 = SDL_GetTicksNS();
    u32 n = 0, total = (u32)(uintptr_t)arg;
    size_t b0;
    pthread_mutex_lock(&rep_mx); b0 = rep_bytes; pthread_mutex_unlock(&rep_mx);
    for (u32 i = 0; i < total; i++) {
        texrep_entry *e = &rep[i];
        if (!preload_wanted(sets[e->set].name) || __atomic_load_n(&e->rgba, __ATOMIC_ACQUIRE) || e->bad) continue;
        pthread_mutex_lock(&rep_mx);
        if (!e->rgba && !e->bad && rep_load(e, 1)) n++;
        pthread_mutex_unlock(&rep_mx);
    }
    pthread_mutex_lock(&rep_mx);
    ps2_log("texrep: preloaded %u replacements (%.1f MB) in %.0f ms on a background thread (ACZ_TEX_PRELOAD=%s)", n,
            (double)(rep_bytes - b0) / (1024.0 * 1024.0), (double)(SDL_GetTicksNS() - t0) / 1e6,
            preload_mode == 2 ? "all" : "1: all but the menu pictures");
    pthread_mutex_unlock(&rep_mx);
    return NULL;
}
static void preload_start(void) {
    const char *e = getenv("ACZ_TEX_PRELOAD");
    preload_mode = !e || !*e ? 1 : !strcmp(e, "0") ? 0 : !SDL_strcasecmp(e, "all") ? 2 : 1;
    if (!preload_mode || !nrep) return;
    pthread_t th;
    if (pthread_create(&th, NULL, preload_main, (void *)(uintptr_t)nrep) == 0) pthread_detach(th);
}

static int rep_load(texrep_entry *e, int quiet) {
    u64 t0 = SDL_GetTicksNS();
    u8 *px;
    SDL_Surface *s = SDL_LoadPNG(e->path), *c;
    u32 sc;
    if (!s) {
        ps2_log("texrep: %s: %s", e->path, SDL_GetError());
        e->bad = 1;
        return 0;
    }
    c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c) {
        ps2_log("texrep: %s: %s", e->path, SDL_GetError());
        e->bad = 1;
        return 0;
    }
    e->iw = (u32)c->w;
    e->ih = (u32)c->h;
    sc = e->iw / e->w;
    if (sc < 1u || sc > 8u || e->iw != sc * e->w || e->ih != sc * e->h) {
        ps2_log("texrep: %s is %ux%u, not 1x-8x of %ux%u: skipped", e->path, e->iw, e->ih, e->w, e->h);
        SDL_DestroySurface(c);
        e->bad = 1;
        return 0;
    }
    px = (u8 *)malloc((size_t)e->iw * e->ih * 4u);
    if (px)
        for (u32 y = 0; y < e->ih; y++)
            memcpy(px + (size_t)y * e->iw * 4u, (const u8 *)c->pixels + (size_t)y * (u32)c->pitch,
                   (size_t)e->iw * 4u);
    SDL_DestroySurface(c);
    if (!px) { e->bad = 1; return 0; }
    __atomic_store_n(&e->rgba, px, __ATOMIC_RELEASE);          /* (filled first: ACZ_TEX_PRELOAD's thread) */
    rep_bytes += (size_t)e->iw * e->ih * 4u;
    if (!quiet)
    ps2_log("texrep: loaded %s (%ux%u, %ux) in %.1f ms; %.1f MB of replacements in memory", e->path, e->iw, e->ih,
            sc, (double)(SDL_GetTicksNS() - t0) / 1e6, (double)rep_bytes / (1024.0 * 1024.0));
    return 1;
}

const u8 *ps2_texrep_find(u32 hash, u32 psm, u32 w, u32 h, u32 *iw, u32 *ih, u32 *salt) {
    s32 best = -1;
    texrep_entry *e;
    rep_last_set = NULL;
    if (!nrep) return NULL;
    for (s32 i = rep_head[rep_bucket(hash, psm, w, h)]; i >= 0; i = rep[i].next) {
        const texrep_entry *c = &rep[i];
        if (c->hash != hash || c->psm != psm || c->w != w || c->h != h || c->bad) continue;
        if (sets[c->set].cond & ~texrep_active) continue;
        if (best < 0 || sets[c->set].rank > sets[rep[best].set].rank) best = i;
    }
    if (best < 0) return NULL;
    e = &rep[best];
    if (preload_mode <= 0 ? !e->rgba && !rep_load(e, 0) : !rep_ready(e)) return NULL;
    if (!e->used) {
        e->used = 1;
        ps2_log("texrep: %08X_%u_%ux%u -> %s (field %u)", hash, psm, w, h, e->path, ps2_gs_frame_count);
    }
    rep_last_set = sets[e->set].name;
    *iw = e->iw;
    *ih = e->ih;
    *salt = e->salt;
    return e->rgba;
}

/* 1 when some file of any set (active or not) is for a decode of this PSM and size: such decodes need the content hash
   the packs are named by (ps2_gs.c PS2_PERF_TEX_DIRECT keys other decodes by their memo key instead) */
int ps2_texrep_has_size(u32 psm, u32 w, u32 h) {
    for (u32 i = 0; i < nrep; i++)
        if (rep[i].psm == psm && rep[i].w == w && rep[i].h == h) return 1;
    return 0;
}

void ps2_texrep_note(u32 hash, u32 psm, u32 w, u32 h, u32 tbp, u32 tbw, u32 cbp, const u8 *rgba, u32 idx,
                     int replaced) {
    /* each (hash, psm, size) once */
    #define SEEN_N (1u << 16)
    static u64 *seen;
    static u32 nseen;
    u64 k = ((u64)hash << 32) | ((u64)psm << 26) | ((u64)(w & 0x1FFFu) << 13) | (h & 0x1FFFu);
    u32 i;
    if (!seen) {
        seen = (u64 *)calloc(SEEN_N, sizeof *seen);
        if (!seen) return;
    }
    if (nseen >= SEEN_N / 4u * 3u) return;
    for (i = (u32)((k * 0x9E3779B97F4A7C15ull) >> 48); seen[i]; i = (i + 1u) & (SEEN_N - 1u))
        if (seen[i] == k) return;
    seen[i] = k;
    nseen++;
    {   /* textures streamed into one address (movies, render-to-texture copies, terrain streaming): once one size
           at one address shows more than 16 different contents within 60 fields (or 256 in all), it is not logged
           or dumped any more. A rate, not a count: the menus upload each of their small pictures (button glyphs,
           icons, font cells) into one shared address a few at a time, and every one of those is a texture a pack
           may replace */
        #define ADDR_N 4096u
        static struct { u32 key, n, total, field0; } addr[ADDR_N];
        u32 ak = (tbp << 8) ^ (w << 20) ^ (h << 4) ^ psm, j = (ak * 2654435761u) >> 20, tries;
        for (tries = 0; tries < 16u; tries++, j = (j + 1u) & (ADDR_N - 1u))
            if (addr[j].total == 0 || addr[j].key == ak) break;
        if (tries < 16u) {
            int was = addr[j].n > 16u || addr[j].total > 256u;
            addr[j].key = ak;
            if (!was && ps2_gs_frame_count - addr[j].field0 > 60u) {
                addr[j].field0 = ps2_gs_frame_count;
                addr[j].n = 0;
            }
            addr[j].n++;
            addr[j].total++;
            if (addr[j].n > 16u || addr[j].total > 256u) {
                if (!was && log_on)
                    ps2_log("texhash: tbp %u %ux%u psm %u changes all the time (streamed): not logged any more",
                            tbp, w, h, psm);
                return;
            }
        }
    }
    if (log_on)
        ps2_log("texhash: %08X_%u_%ux%u tbp %u tbw %u cbp %u field %u store %d%s%s", hash, psm, w, h, tbp, tbw,
                cbp, ps2_gs_frame_count, (int)idx, replaced && rep_last_set ? " -> set " : "",
                replaced && rep_last_set ? rep_last_set : "");
    if (dump_dir[0] && dump_n < dump_max) {
        char nm[640];
        SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_RGBA32, (void *)rgba, (int)(w * 4u));
        snprintf(nm, sizeof nm, "%s/%08X_%u_%ux%u.png", dump_dir, hash, psm, w, h);
        if (s && SDL_SavePNG(s, nm)) dump_n++;
        else ps2_log("texrep: could not write %s: %s", nm, SDL_GetError());
        if (s) SDL_DestroySurface(s);
    }
}

/* the controller layout (pad-layout work: ACZ_PAD_LAYOUT=modern|classic over [input] pad_layout). Once that is in
   the tree, ps2_settings.h defines PS2_HAVE_PAD_LAYOUT and the live setting is used (F4 changes follow at once);
   without it the session's ACZ_PAD_LAYOUT is read, classic by default. */
static int pad_layout_modern(void) {
#ifdef PS2_HAVE_PAD_LAYOUT
    return ps2_cfg.pad_layout == PS2_PAD_LAYOUT_MODERN;
#else
    static int modern = -1;
    if (modern < 0) {
        const char *e = getenv("ACZ_PAD_LAYOUT");
        modern = e && !SDL_strcasecmp(e, "modern");
    }
    return modern;
#endif
}

/* PS2_GLYPHS_*: the set shown now (the active controller or ACZ_PAD_GLYPHS): the quit prompt draws the same kind */
int ps2_texrep_glyph_class(void) { return (int)(__atomic_load_n(&texrep_state, __ATOMIC_ACQUIRE) & 3u); }

/* the active controller's class (ps2_video.c's registry: an Xbox-type pad, a PlayStation-type pad, Touch; other pads
   show PlayStation glyphs as before); ACZ_PAD_GLYPHS=xbox|ps|touch forces one (default auto) */
void ps2_texrep_pad(int cls, const char *who) {
    static int mode = -1;                 /* -1 not read, 0 auto, else PS2_GLYPHS_* + 1 */
    static int said;
    u32 cur, want;
    if (mode < 0) {
        const char *e = getenv("ACZ_PAD_GLYPHS");
        mode = !e || !*e || !SDL_strcasecmp(e, "auto") ? 0 : !SDL_strcasecmp(e, "xbox") ? 1 + PS2_GLYPHS_XBOX
             : !SDL_strcasecmp(e, "ps") ? 1 + PS2_GLYPHS_PS : !SDL_strcasecmp(e, "touch") ? 1 + PS2_GLYPHS_TOUCH : 0;
        if (e && *e && mode == 0 && SDL_strcasecmp(e, "auto"))
            ps2_log("texrep: ACZ_PAD_GLYPHS=%s not understood (auto, xbox, ps or touch)", e);
    }
    if (mode > 0) cls = mode - 1;
    if (cls < PS2_GLYPHS_PS || cls > PS2_GLYPHS_TOUCH) cls = PS2_GLYPHS_PS;
    want = (u32)cls | (pad_layout_modern() ? 4u : 0u);
    cur = __atomic_load_n(&texrep_state, __ATOMIC_ACQUIRE);
    if ((cur & 7u) == want && said) return;
    said = 1;
    if ((cur & 7u) != want) __atomic_store_n(&texrep_state, ((cur & ~7u) + 8u) | want, __ATOMIC_RELEASE);
    static const char *const cn[3] = { "PlayStation", "Xbox", "Touch" };
    ps2_log("texrep: button glyphs %s (%s%s), %s layout", cn[cls],
            mode > 0 ? "ACZ_PAD_GLYPHS" : who ? "controller: " : "no controller", mode == 0 && who ? who : "",
            want & 4u ? "modern" : "classic");
}
