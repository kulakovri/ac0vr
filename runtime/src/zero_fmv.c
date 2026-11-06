/* zero_fmv.c -- the AI-upscaled FMV movies (out/fmv, tools/fmv/) in place of the game's MPEG2 ones (ACZ_FMV_PACK,
   default on).

   How the game shows a movie (probed 2026-10-23: intro = MOVIEUS.PAC movie 0, the new-game prologue = movie 8, the M1
   briefing = BRIEF.PAC movie 0): nufile reads the movie's mini-PAC header at movie offset + 0x800 (2 KB), then whole
   chunks (1.2-1.5 MB, two guest buffers, one chunk ahead of the decoder). The EE decodes each picture with IPU BDEC + its
   own motion compensation into a picture buffer in COLUMN-major macroblock order (the macroblocks of a 16-pixel column
   are contiguous) and converts it with IPU CSC (RGB32, thresholds 0/0; 640 x 448 = 1023 + 97 macroblocks, the 640 x
   304 story movies -- 35 of MOVIEUS.PAC's 36 -- 760 in one command) into guest memory (0xEB2580). Then, every second
   field, it clears the frame and draws the picture as 40 sprites, one per column: an image transfer of the column (16 x
   H PSMCT32) to GS 2293760 (tbw 64), and a 16 x H sprite at x = 16 * column (y 0 or 72 for the 304-line ones; point
   sampled, vertex colour 128); subtitles, credits and the briefing's text are the game's own draws on top. The GS
   decodes the 448-line strips as 16 x 448 textures and the 304-line ones as 64 x 640 (the column in the top-left
   corner, the rest stale VRAM). The IPU's pictures come in display order, one per movie frame: picture n of a movie
   is frame n of the .m2v that tools/fmv/pac_split.py extracts and of the upscaled movie_NN.mkv (frames 300 and 600:
   mean abs diff 0.5-0.6 against the m2v, 2.1 against the upscaled frame box-downscaled, 4.5-7.9 against the
   neighbouring frames).

   The replacement: the EE hooks know which movie plays (the nufile reads of SMV/MOVIEUS.PAC or BRIEF.PAC, mapped through
   out/fmv/<pac>/manifest.json; the size from the movie's MPEG2 sequence header) and count the pictures from its start
   (or from the chunk the game seeks to). Each finished picture is published with a content hash of each column as the
   GS decode shows it (FNV-1 over the RGBA texels) and a 40 x 28 block signature (mean green per macroblock). A
   background thread runs the static ffmpeg on out/fmv/<pac>/<model>/movie_NN.mkv (hardware decode, NV12 through a
   pipe), converts each frame with the IPU's own CSC arithmetic straight into 40 column strips of 16s x Hs RGBA (s =
   ACZ_FMV_SCALE) and keeps a few frames ahead. When the GS decodes a texture whose top-left 16 x H texels are a column
   of a published picture and the decoded frame for that picture is ready (and its block signature agrees: the count
   is trusted unless a neighbouring frame matches clearly better), the texture becomes the high-res strip (a texture
   replacement at s x, as ps2_texrep.c's; gs.frag samples it in decoded-texel space, the game's region clamp keeps it
   inside the strip). The game's sprites, timing, fades, overlays, subtitles and sound stay its own; when our frame is
   not ready, or the movie has no complete upscaled file, the game's own strip is drawn.

   Env: ACZ_FMV_PACK (1 = on for both PACs; 0 = off; movieus / brief = only that PAC), ACZ_FMV_DIR (out/fmv),
   ACZ_FMV_MODEL (realesrgan-x4plus), ACZ_FMV_MOVIEUS_MODEL / ACZ_FMV_BRIEF_MODEL (x4plus-text: a PAC's own pack, tried
   first per movie; 0 = ACZ_FMV_MODEL's), ACZ_FMV_TV_SHOTS (1: each pack's "-tv" variant first where it has the movie:
   the TV-news shots with a clean scanline overlay, tools/fmv/tv_lines.py; 0 = without), ACZ_FMV_FFMPEG (path of
   ffmpeg.exe), ACZ_FMV_HWACCEL (cuda; none = CPU decode), ACZ_FMV_SCALE (0 = auto: the internal resolution, at most
   3), ACZ_FMV_AHEAD (frames decoded ahead, 8),
   ACZ_FMV_PACK_AB (N: blocks of N movie frames alternately replaced and original, for in-run A/B), ACZ_DEBUG_FMV
   (1: per-movie and per-second log lines; 2: also each picture), ACZ_DEBUG_FMV_DUMP (N: write N comparison sets
   out/fmv_dbg/<pac>_mMM_fFFFFF_{ipu,ours,diff}.ppm, every ACZ_DEBUG_FMV_EVERY = 150 frames). */
#include "ps2_runtime.h"
#include "ps2_hle.h"
#include "ps2_settings.h"
#include "ps2_vfs.h"
#include "zero_fmv.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <windows.h>
#include <SDL3/SDL.h>

u64 ps2_kernel_vblank_count(void);
extern u32 ps2_gs_frame_count;

#define FM_PACS 2
#define FM_PICS 8                 /* published pictures (EE -> GS) */
#define FM_SLOTS_MAX 32
#define FM_MBW_MAX 64u
#define FM_MBH_MAX 64u

typedef struct { u32 offset, c0, c1, pictures; int have; u32 w, h; } fm_movie;   /* w, h: from its sequence header */
typedef struct { u32 offset, size, movie, first; } fm_chunk;
typedef struct {
    const char *name, *dir;       /* MOVIEUS.PAC, movieus */
    int on, loaded;
    u32 lba, size, w, h;
    u32 nmov, nch;
    fm_movie *mov;
    fm_chunk *ch;
} fm_pac;

static fm_pac pacs[FM_PACS] = { { "MOVIEUS.PAC", "movieus", 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL },
                                { "BRIEF.PAC", "brief", 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL } };

/* config */
static int fm_init_done, fm_on, fm_dbg, fm_dump_n, fm_dump_every = 150, fm_ab, fm_ahead = 8, fm_scale_cfg;
static int fm_tail;               /* [debug] ACZ_DEBUG_FMV_TAIL: the game's pictures from the manifest's count - N on */
static int fm_end_hold = 1;       /* ACZ_FMV_END_HOLD: pictures past a movie's end show its last frame */
volatile int zero_fmv_trace;      /* [debug] from N pictures before a movie's end on (ACZ_DEBUG_FMV_TAIL=N): the IPU
                                     input's start codes and DMA channel 4's tags and CHCR writes are logged */
static double fm_mad_max = 24.0;
static char fm_dir[600], fm_model[64] = "realesrgan-x4plus", fm_ffmpeg[600], fm_hw[32] = "cuda";
/* a PAC's own pack, tried first for each movie ("" = ACZ_FMV_MODEL's): BRIEF.PAC = x4plus-text, the x4plus movies with
   their text regions from realesr-animevideov3 (tools/fmv/text_compose.py, bug #137: x4plus guesses the briefings'
   small letters); MOVIEUS.PAC = x4plus-text too, but only its movies with text shots (maps, captions, interview and name
   cards, documents, credits: tools/fmv/text_ranges.py + movieus_text_ranges.txt, fmv_fix 2026-10-29), the rest of its
   movies play from ACZ_FMV_MODEL's pack (the per-movie fallback) */
static char fm_pac_model[FM_PACS][64] = { "x4plus-text", "x4plus-text" };
/* ACZ_FMV_TV_SHOTS (fmv_tv 2026-10-29, the user's remark 10:51 on the OBC news footage: "show this TV format as TV, so
   these artefacts go away"): MOVIEUS.PAC's TV-news shots carry a faint scanline overlay (2-8 levels) that x4plus drew
   as hard black lines, bricks and planks; the "-tv" packs (tools/fmv/tv_lines.py + movieus_tv_ranges.txt) have those
   shots upscaled without the overlay and the overlay drawn again at 4x (its period, phase and amplitude). Per movie
   the packs are tried in the order <PAC's pack>-tv, <PAC's pack>, <ACZ_FMV_MODEL>-tv, <ACZ_FMV_MODEL>; 0 = without
   the "-tv" packs */
static int fm_tv = 1;
static char fm_tv_pac[FM_PACS][72], fm_tv_model[72];

/* geometry of the playing movie in macroblocks (640 x 448: 40 x 28; 640 x 304: 40 x 19) */
static u32 fm_mbw = 40, fm_mbh = 28;

static pthread_mutex_t fm_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fm_cv = PTHREAD_COND_INITIALIZER;

/* ---- the published pictures (EE writes under fm_mx, GS reads under fm_mx) ---- */
typedef struct {
    u32 serial;                   /* 0 = empty */
    u32 gen;                      /* movie generation (a new movie start bumps it) */
    int pac, movie;
    s32 frame;                    /* the EE's frame guess; -1 = no movie known */
    s32 use;                      /* the GS's verified frame, -1 not yet checked, -2 no match */
    u32 col[FM_MBW_MAX];
    u8  sig[FM_MBW_MAX * FM_MBH_MAX];
    u32 th0, th1;
    int last_col;
    int past_end;                 /* converted after the movie's last picture (ACZ_FMV_END_HOLD) */
    u8 *dump;                     /* a copy of the IPU picture (ACZ_DEBUG_FMV_DUMP) */
} fm_pic;
static fm_pic pics[FM_PICS];
static u32 pic_serial;

/* ---- EE state ---- */
static u32 *ee_pic;               /* the picture being assembled from CSC output (column-major macroblocks) */
static u32 ee_acc;                /* macroblocks so far */
static u64 ee_last_csc;
static int ee_pac = -1, ee_movie = -1;
static s32 ee_next = -1;          /* frame index the next finished picture gets */
static u64 ee_read_end[FM_PACS];
static volatile s32 fm_fix;       /* frame correction from the GS's checks (applied to ee_next) */
static u32 ee_th0, ee_th1;        /* the IPU's CSC thresholds (alpha), as the last picture used them */

/* ---- the job (EE sets it under fm_mx, the decoder takes it) ---- */
static u32 job_gen;               /* bumped at each movie start */
static int job_pac = -1, job_movie = -1;
static s32 job_start;
static volatile s32 game_frame = -1;   /* the newest picture's frame (EE) */
static volatile u64 game_field;        /* field of the newest picture */

/* ---- decoded frames ---- */
typedef struct {
    int state;                    /* 0 free, 1 being filled, 2 ready */
    u32 gen;
    s32 frame;
    int pins;
    u8 *strips;                   /* fm_mbw strips of sw x sh RGBA */
    u8  sig[FM_MBW_MAX * FM_MBH_MAX];
} fm_slot;
static fm_slot slots[FM_SLOTS_MAX];
static int nslots;
static u32 dec_gen;               /* generation the decoder works on */
static u32 sw, sh, sscale;        /* strip size, scale of the slots' strips */
static size_t strip_bytes, slot_bytes;
static HANDLE dec_proc, dec_job;
static pthread_t dec_thread;
static int dec_started;

/* ---- GS state ---- */
static fm_slot *gs_pinned;
static u64 gs_t0;
static s32 gs_last_frame = -1;
static int gs_fix_votes, gs_fix_delta;

/* ---- stats (per movie) ---- */
static struct {
    u64 strips_rep, strips_orig_notready, strips_orig_nomatch, strips_orig_ab, strips_nohash;
    u64 pics, pics_ok, pics_loose, pics_shift, pics_bad, frames_dec, frames_drop, restarts, pics_past;
    double mad_sum; u64 mad_n; double mad_max;
    u64 gs_ns, conv_ns, read_ns;
    u64 t_start_ns, first_rep_field, start_field;
    double ff_cpu_ms, ff_wall_ms;     /* ffmpeg processes of this movie: CPU and lifetime */
} st;
static double dec_spawn_ms;

static double now_ms(void) { return (double)SDL_GetTicksNS() / 1e6; }
static void movie_summary_at(int pac, int movie, const char *why, int reset);
static void movie_summary_dec(int pac, int movie, int frame) {
    if (fm_dbg) ps2_log("fmv: %s movie %d: no picture for 2 s after frame %d: decoder stopped", pacs[pac].name, movie, frame);
    movie_summary_at(pac, movie, "stopped", 1);
}

/* ------------------------------------------------------------------------------------------------ manifest */

/* a streaming JSON reader: calls back every number with its path (keys and array indices) */
typedef struct { const char *p, *e; int depth; char key[8][32]; int idx[8]; int isarr[8]; } js;
typedef void (*js_cb)(void *u, js *j, double v);

static void js_ws(js *j) { while (j->p < j->e && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++; }
static int js_value(js *j, js_cb cb, void *u);
static int js_string(js *j, char *out, size_t cap) {
    size_t n = 0;
    if (j->p >= j->e || *j->p != '"') return 0;
    j->p++;
    while (j->p < j->e && *j->p != '"') {
        if (*j->p == '\\' && j->p + 1 < j->e) j->p++;
        if (out && n + 1 < cap) out[n++] = *j->p;
        j->p++;
    }
    if (out) out[n] = 0;
    if (j->p < j->e) j->p++;
    return 1;
}
static int js_value(js *j, js_cb cb, void *u) {
    js_ws(j);
    if (j->p >= j->e) return 0;
    if (*j->p == '{' || *j->p == '[') {
        int arr = *j->p == '[', d = j->depth;
        char close = arr ? ']' : '}';
        j->p++;
        if (d >= 7) return 0;
        j->depth = d + 1;
        j->isarr[d + 1] = arr;
        j->idx[d + 1] = 0;
        j->key[d + 1][0] = 0;
        js_ws(j);
        if (j->p < j->e && *j->p == close) { j->p++; j->depth = d; return 1; }
        for (;;) {
            js_ws(j);
            if (!arr) {
                if (!js_string(j, j->key[d + 1], sizeof j->key[0])) return 0;
                js_ws(j);
                if (j->p >= j->e || *j->p != ':') return 0;
                j->p++;
            }
            if (!js_value(j, cb, u)) return 0;
            js_ws(j);
            if (j->p < j->e && *j->p == ',') { j->p++; if (arr) j->idx[d + 1]++; continue; }
            if (j->p < j->e && *j->p == close) { j->p++; break; }
            return 0;
        }
        j->depth = d;
        return 1;
    }
    if (*j->p == '"') return js_string(j, NULL, 0);
    {
        char *end;
        double v = strtod(j->p, &end);
        if (end == j->p) {           /* true / false / null */
            while (j->p < j->e && *j->p >= 'a' && *j->p <= 'z') j->p++;
            return 1;
        }
        j->p = end;
        cb(u, j, v);
        return 1;
    }
}

static void man_cb(void *u, js *j, double v) {
    fm_pac *p = (fm_pac *)u;
    u32 x = (u32)v;
    if (j->depth == 1) {
        if (!strcmp(j->key[1], "lba")) p->lba = x;
        else if (!strcmp(j->key[1], "size")) p->size = x;
        else if (!strcmp(j->key[1], "width")) p->w = x;
        else if (!strcmp(j->key[1], "height")) p->h = x;
        return;
    }
    if (j->depth >= 2 && !strcmp(j->key[1], "movies") && j->isarr[1] == 0) {
        u32 i = (u32)j->idx[2];
        if (i >= p->nmov) {
            fm_movie *n = (fm_movie *)realloc(p->mov, (i + 1u) * sizeof *n);
            if (!n) return;
            memset(n + p->nmov, 0, (i + 1u - p->nmov) * sizeof *n);
            p->mov = n;
            p->nmov = i + 1u;
        }
        if (j->depth == 3 && !strcmp(j->key[3], "offset")) p->mov[i].offset = x;
        else if (j->depth == 3 && !strcmp(j->key[3], "pictures")) p->mov[i].pictures = x;
        else if (j->depth == 4 && !strcmp(j->key[3], "chunks")) { if (j->idx[4] == 0) p->mov[i].c0 = x; else p->mov[i].c1 = x; }
        return;
    }
    if (j->depth == 3 && !strcmp(j->key[1], "chunks")) {
        u32 i = (u32)j->idx[2];
        if (i >= p->nch) {
            fm_chunk *n = (fm_chunk *)realloc(p->ch, (i + 1u) * sizeof *n);
            if (!n) return;
            memset(n + p->nch, 0, (i + 1u - p->nch) * sizeof *n);
            p->ch = n;
            p->nch = i + 1u;
        }
        if (!strcmp(j->key[3], "offset")) p->ch[i].offset = x;
        else if (!strcmp(j->key[3], "size")) p->ch[i].size = x;
        else if (!strcmp(j->key[3], "movie")) p->ch[i].movie = x;
        else if (!strcmp(j->key[3], "movie_first_picture")) p->ch[i].first = x;
    }
}

static int file_size_ok(const char *path, long long *sz) {
    struct _stat64 s;
    if (_stat64(path, &s) != 0 || !(s.st_mode & _S_IFREG)) return 0;
    if (sz) *sz = (long long)s.st_size;
    return s.st_size > 0;
}

/* the packs tried for a movie of PAC pi, best first: with ACZ_FMV_TV_SHOTS the "-tv" variant before each of the PAC's
   own pack (fm_pac_model) and ACZ_FMV_MODEL's; returns their number (<= 4) */
static int pack_list(int pi, const char *nm[4]) {
    int n = 0;
    if (fm_pac_model[pi][0]) {
        if (fm_tv) nm[n++] = fm_tv_pac[pi];
        nm[n++] = fm_pac_model[pi];
    }
    if (fm_tv) nm[n++] = fm_tv_model;
    nm[n++] = fm_model;
    return n;
}

/* the movie's file: from the first pack of pack_list that has it (else the last one's path); returns that pack's
   number in the list + 1 (pack_name gives its folder name) */
static int movie_path(int pi, int m, char *out, size_t cap) {
    const char *nm[4];
    int n = pack_list(pi, nm);
    for (int i = 0; i < n; i++) {
        snprintf(out, cap, "%s/%s/%s/movie_%02d.mkv", fm_dir, pacs[pi].dir, nm[i], m);
        if (file_size_ok(out, NULL)) return i + 1;
    }
    return n;
}

static const char *pack_name(int pi, int have) {
    const char *nm[4];
    int n = pack_list(pi, nm);
    return have >= 1 && have <= n ? nm[have - 1] : "?";
}

static void man_load(int pi) {
    fm_pac *p = &pacs[pi];
    char path[700];
    FILE *fp;
    long n;
    char *buf;
    js j;
    u32 have = 0, per[4] = { 0, 0, 0, 0 };
    const char *nm[4];
    int npk;
    p->loaded = -1;
    snprintf(path, sizeof path, "%s/%s/manifest.json", fm_dir, p->dir);
    fp = fopen(path, "rb");
    if (!fp) { ps2_log("fmv: no %s: %s plays its own movies", path, p->name); return; }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1u);
    if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(buf); return; }
    fclose(fp);
    memset(&j, 0, sizeof j);
    j.p = buf; j.e = buf + n;
    if (!js_value(&j, man_cb, p) || !p->nmov || !p->nch || !p->w || !p->h) {
        ps2_log("fmv: %s could not be read (%u movies, %u chunks)", path, p->nmov, p->nch);
        free(buf);
        p->nmov = p->nch = 0;
        return;
    }
    free(buf);
    for (u32 m = 0; m < p->nmov; m++) {
        int k = movie_path(pi, (int)m, path, sizeof path);
        p->mov[m].have = file_size_ok(path, NULL) ? k : 0;
        have += p->mov[m].have != 0;
        if (p->mov[m].have) per[p->mov[m].have - 1]++;
    }
    p->loaded = 1;
    npk = pack_list(pi, nm);
    {   /* "36 movies (36 upscaled in out/fmv/movieus: 10 x4plus-text-tv, 16 x4plus-text, 2 realesrgan-x4plus-tv, ...)" */
        char lst[400];
        int o = 0;
        for (int i = 0; i < npk && o < (int)sizeof lst - 80; i++)
            o += snprintf(lst + o, sizeof lst - (size_t)o, "%s%u %s", i ? ", " : "", per[i], nm[i]);
        ps2_log("fmv: %s: %u movies (%u upscaled in %s/%s: %s), %u chunks", p->name, p->nmov, have, fm_dir, p->dir, lst,
                p->nch);
    }
}

/* ------------------------------------------------------------------------------------------------ config */

static void posix_to_win(char *s) {
    /* "/c/Users/..." (a Git Bash path) -> "c:/Users/..." */
    if (s[0] == '/' && s[1] && s[2] == '/' && ((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z'))) {
        s[0] = s[1];
        s[1] = ':';
    }
}

static int is_dir(const char *p) {
    struct _stat64 s;
    return _stat64(p, &s) == 0 && (s.st_mode & _S_IFDIR);
}

static void fm_init(void) {
    const char *e;
    if (fm_init_done) return;
    fm_init_done = 1;
    e = getenv("ACZ_FMV_PACK");          /* 1 (default): both PACs; 0: off; movieus / brief: that PAC only */
    if (!e || !*e || !strcmp(e, "1")) { pacs[0].on = pacs[1].on = 1; }
    else if (strcmp(e, "0")) {
        pacs[0].on = SDL_strcasestr(e, "movieus") != NULL;
        pacs[1].on = SDL_strcasestr(e, "brief") != NULL;
    }
    fm_on = pacs[0].on || pacs[1].on;
    e = getenv("ACZ_DEBUG_FMV");         /* [debug] 1: per-movie / per-second lines; 2: each picture */
    fm_dbg = e && *e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_FMV_DUMP");    /* [debug] N comparison image sets into out/fmv_dbg */
    fm_dump_n = e && *e ? atoi(e) : 0;
    e = getenv("ACZ_DEBUG_FMV_EVERY");   /* [debug] frames between dumps (150) */
    if (e && atoi(e) > 0) fm_dump_every = atoi(e);
    e = getenv("ACZ_FMV_END_HOLD");      /* 1 (default): pictures past a movie's end show its last frame; 0 = the game's */
    if (e && *e) fm_end_hold = atoi(e) != 0;
    e = getenv("ACZ_DEBUG_FMV_TAIL");    /* [debug] N: dump / log the game's last N + extra pictures of each movie */
    fm_tail = e && *e ? atoi(e) : 0;
    e = getenv("ACZ_FMV_PACK_AB");       /* [test] N: N movie frames replaced, N original, ... */
    fm_ab = e && *e ? atoi(e) : 0;
    e = getenv("ACZ_FMV_AHEAD");         /* frames decoded ahead (8; 3..32) */
    if (e && atoi(e) > 0) fm_ahead = atoi(e);
    if (fm_ahead < 3) fm_ahead = 3;
    if (fm_ahead > FM_SLOTS_MAX) fm_ahead = FM_SLOTS_MAX;
    e = getenv("ACZ_FMV_MAD_MAX");       /* block MAD (0..255) above which a picture keeps the game's own frame (24) */
    if (e && atof(e) > 0.0) fm_mad_max = atof(e);
    e = getenv("ACZ_FMV_SCALE");         /* 0 auto (the internal resolution, at most 3), 1..4 */
    fm_scale_cfg = e && *e ? atoi(e) : 0;
    e = getenv("ACZ_FMV_MODEL");
    if (e && *e) snprintf(fm_model, sizeof fm_model, "%s", e);
    e = getenv("ACZ_FMV_MOVIEUS_MODEL"); /* MOVIEUS.PAC's own pack (x4plus-text, the movies with text shots: fmv_fix
                                            2026-10-29); 0 = ACZ_FMV_MODEL's for every movie */
    if (e && *e) snprintf(fm_pac_model[0], sizeof fm_pac_model[0], "%s", strcmp(e, "0") ? e : "");
    e = getenv("ACZ_FMV_BRIEF_MODEL");   /* BRIEF.PAC's own pack (x4plus-text); 0 = ACZ_FMV_MODEL's for every movie */
    if (e && *e) snprintf(fm_pac_model[1], sizeof fm_pac_model[1], "%s", strcmp(e, "0") ? e : "");
    e = getenv("ACZ_FMV_TV_SHOTS");      /* 1 (default): the "-tv" packs first (clean TV scanlines); 0 = without */
    if (e && *e) fm_tv = atoi(e) != 0;
    for (int i = 0; i < FM_PACS; i++) snprintf(fm_tv_pac[i], sizeof fm_tv_pac[i], "%.63s-tv", fm_pac_model[i]);
    snprintf(fm_tv_model, sizeof fm_tv_model, "%.63s-tv", fm_model);
    e = getenv("ACZ_FMV_HWACCEL");       /* cuda (default; ffmpeg failing with it: none from then on), d3d11va, auto, none */
    if (e && *e) snprintf(fm_hw, sizeof fm_hw, "%s", e);
    e = getenv("ACZ_FMV_DIR");
    if (e && *e) snprintf(fm_dir, sizeof fm_dir, "%s", e);
    else if (is_dir("out/fmv")) snprintf(fm_dir, sizeof fm_dir, "out/fmv");
    else {
        const char *base = SDL_GetBasePath();
        snprintf(fm_dir, sizeof fm_dir, "%s../../out/fmv", base ? base : "");
    }
    posix_to_win(fm_dir);
    e = getenv("ACZ_FMV_FFMPEG");
    if (e && *e) snprintf(fm_ffmpeg, sizeof fm_ffmpeg, "%s", e);
    else {
        static const char *rel = "ext_tools/ffmpeg/ffmpeg-master-latest-win64-gpl/bin/ffmpeg.exe";
        const char *base = SDL_GetBasePath();
        snprintf(fm_ffmpeg, sizeof fm_ffmpeg, "../%s", rel);
        if (!file_size_ok(fm_ffmpeg, NULL)) snprintf(fm_ffmpeg, sizeof fm_ffmpeg, "%s../../../%s", base ? base : "", rel);
        /* a player's install: the asset folder puts it beside the exe */
        if (!file_size_ok(fm_ffmpeg, NULL) && file_size_ok("ffmpeg.exe", NULL)) snprintf(fm_ffmpeg, sizeof fm_ffmpeg, "ffmpeg.exe");
    }
    posix_to_win(fm_ffmpeg);
    if (!fm_on) { ps2_log("fmv: upscaled movies off (ACZ_FMV_PACK=0)"); return; }
    if (!file_size_ok(fm_ffmpeg, NULL)) {
        ps2_log("fmv: no ffmpeg at %s (ACZ_FMV_FFMPEG): the game plays its own movies", fm_ffmpeg);
        fm_on = 0;
        return;
    }
    for (int i = 0; i < FM_PACS; i++) if (pacs[i].on) man_load(i);
    ps2_log("fmv: upscaled movies on (ACZ_FMV_PACK; ffmpeg %s, hwaccel %s, scale %s, %d frames ahead%s; TV shots %s)",
            fm_ffmpeg, fm_hw,
            fm_scale_cfg > 0 ? (fm_scale_cfg == 1 ? "1" : fm_scale_cfg == 2 ? "2" : fm_scale_cfg == 3 ? "3" : "4") : "auto",
            fm_ahead, fm_ab ? ", A/B" : "", fm_tv ? "clean (ACZ_FMV_TV_SHOTS)" : "as upscaled (ACZ_FMV_TV_SHOTS=0)");
}

static u32 pick_scale(void) {
    /* auto: the internal resolution, at most 3 -- the window (1920 x 1009 in the user's sessions) and the headset's
       cinema screen (a copy of the window image) show ~1000 rows: 3x (1344 rows) already over-samples them, and 4x
       costs 7x the GPU time (+1.07 against +0.14 ms a frame, its 18 MB of strips a picture overflow the 12 MB
       device-local staging) and 1.8x the memory (sim A/B 2026-10-23, intro movie) */
    int s = fm_scale_cfg;
    if (s <= 0) {
        const char *e = getenv("PS2_UPSCALE");
        s = e && *e ? atoi(e) : ps2_cfg.internal_res;
        if (s <= 0 || s > 3) s = 3;
    }
    if (s < 1) s = 1;
    if (s > 4) s = 4;
    return (u32)s;
}

/* ------------------------------------------------------------------------------------------------ decoder */

/* the IPU's CSC (ps2_ipu.c csc_pixel) as tables */
static int t_y3[256], t_cr4[256], t_cr5[256], t_cb4[256], t_cb5[256];
static u8 t_clamp[4096];
static void csc_tables(void) {
    for (int v = 0; v < 256; v++) {
        int c1 = v - 128;
        t_y3[v] = (0x095 * (v - 16)) >> 6;
        t_cr4[v] = (0x0cc * c1) >> 6;
        t_cr5[v] = (0x068 * c1) >> 6;
        t_cb4[v] = (0x032 * c1) >> 6;
        t_cb5[v] = (0x102 * c1) >> 6;
    }
    for (int i = 0; i < 4096; i++) {   /* index = sum + 2048, value = clamp((sum + 1) >> 1) */
        int v = ((i - 2048) + 1) >> 1;
        t_clamp[i] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
}

static void slot_free_all(void) {
    for (int i = 0; i < nslots; i++) { free(slots[i].strips); slots[i].strips = NULL; slots[i].state = 0; }
    nslots = 0;
}

static int slots_alloc(u32 scale) {
    size_t bytes;
    sscale = scale;
    sw = 16u * scale;
    sh = fm_mbh * 16u * scale;
    strip_bytes = (size_t)sw * sh * 4u;
    bytes = strip_bytes * fm_mbw;
    slot_bytes = bytes;
    for (int i = 0; i < fm_ahead; i++) {
        slots[i].strips = (u8 *)malloc(bytes);
        if (!slots[i].strips) { nslots = i; return i >= 3; }
        slots[i].state = 0;
        slots[i].pins = 0;
    }
    nslots = fm_ahead;
    return 1;
}

static double proc_cpu_ms(HANDLE h) {
    FILETIME c, e, k, u;
    if (!h || !GetProcessTimes(h, &c, &e, &k, &u)) return 0.0;
    return ((double)(((u64)k.dwHighDateTime << 32) | k.dwLowDateTime)
            + (double)(((u64)u.dwHighDateTime << 32) | u.dwLowDateTime)) / 1e4;
}

static void proc_kill(void) {
    if (dec_proc) {
        st.ff_cpu_ms += proc_cpu_ms(dec_proc);
        st.ff_wall_ms += now_ms() - dec_spawn_ms;
        TerminateProcess(dec_proc, 1);
        CloseHandle(dec_proc);
        dec_proc = NULL;
    }
}

static HANDLE proc_spawn(const char *cmd, HANDLE *out_rd) {
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL, nul_in, err;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *line = _strdup(cmd);
    char logp[300];
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!line) return NULL;
    if (!CreatePipe(&rd, &wr, &sa, 8u << 20)) { free(line); return NULL; }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    nul_in = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    snprintf(logp, sizeof logp, "out/fmv_ffmpeg.log");
    err = fm_dbg ? CreateFileA(logp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, 0, NULL)
                 : INVALID_HANDLE_VALUE;
    if (err == INVALID_HANDLE_VALUE)
        err = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_in;
    si.hStdOutput = wr;
    si.hStdError = err;
    memset(&pi, 0, sizeof pi);
    if (!dec_job) {
        /* ffmpeg ends with the game (TerminateProcess, timeout kill): a job that kills its processes on close */
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        dec_job = CreateJobObjectA(NULL, NULL);
        memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (dec_job) SetInformationJobObject(dec_job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS,
                        NULL, NULL, &si, &pi)) {
        ps2_log("fmv: could not start ffmpeg (error %lu): %s", (unsigned long)GetLastError(), cmd);
        CloseHandle(rd); CloseHandle(wr); CloseHandle(nul_in); CloseHandle(err);
        free(line);
        return NULL;
    }
    if (dec_job) AssignProcessToJobObject(dec_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    CloseHandle(nul_in);
    CloseHandle(err);
    free(line);
    *out_rd = rd;
    return pi.hProcess;
}

static int read_full(HANDLE h, u8 *dst, size_t n) {
    size_t got = 0;
    while (got < n) {
        DWORD k = 0, want = (DWORD)(n - got > (64u << 20) ? (64u << 20) : n - got);
        if (!ReadFile(h, dst + got, want, &k, NULL) || !k) return 0;
        got += k;
    }
    return 1;
}

/* NV12 (w x h luma, then interleaved CbCr at half size) -> the slot's column strips (RGBA, the decode's alpha
   convention: IPU alpha 0x80 = 255) with the IPU's CSC arithmetic, and the block signature (mean green per macroblock,
   sampled every scale-th pixel) */
static void convert(const u8 *nv, fm_slot *s, u32 th0, u32 th1) {
    const u32 W = fm_mbw * sw, H = sh, sc = sscale, bs = 16u * sc;
    const u8 *yp = nv, *uv = nv + (size_t)W * H;
    u32 sig_acc[FM_MBW_MAX * FM_MBH_MAX];
    const int thr = th0 || th1;
    memset(sig_acc, 0, sizeof(u32) * fm_mbw * fm_mbh);
    for (u32 y = 0; y < H; y++) {
        const u8 *yr = yp + (size_t)y * W, *cr = uv + (size_t)(y >> 1) * W;
        const int sample_row = (y % sc) == 0u;
        u32 *sig_row = sig_acc + (y / bs) * fm_mbw;
        for (u32 c = 0; c < fm_mbw; c++) {
            u32 *dst = (u32 *)(s->strips + (size_t)c * strip_bytes + (size_t)y * sw * 4u);
            const u32 x0 = c * sw;
            for (u32 x = 0; x < sw; x += 2u) {
                const int cb = cr[x0 + x], crv = cr[x0 + x + 1u];
                const int rr = t_cr4[crv] + 2048, gg = 2048 - t_cb4[cb] - t_cr5[crv], bb = t_cb5[cb] + 2048;
                for (u32 k = 0; k < 2u; k++) {
                    const int y3 = t_y3[yr[x0 + x + k]];
                    u32 r = t_clamp[y3 + rr], g = t_clamp[y3 + gg], b = t_clamp[y3 + bb], a = 255u;
                    if (thr) {
                        if (r < th0 && g < th0 && b < th0) { r = g = b = 0; a = 0; }
                        else if (r < th1 && g < th1 && b < th1) a = 128u;
                    }
                    dst[x + k] = r | (g << 8) | (b << 16) | (a << 24);
                }
            }
            if (sample_row) {          /* the signature: every sc-th pixel of every sc-th row (16 x 16 a block) */
                u32 gsum = 0;
                for (u32 x = 0; x < sw; x += sc) gsum += (dst[x] >> 8) & 0xFFu;
                sig_row[c] += gsum;
            }
        }
    }
    for (u32 i = 0; i < fm_mbw * fm_mbh; i++) s->sig[i] = (u8)((sig_acc[i] + 128u) / 256u);
}

static fm_slot *slot_for_write(s32 need_lo) {
    /* a free slot, or a ready one of an old frame no strip copy holds (fm_mx held) */
    fm_slot *best = NULL;
    for (int i = 0; i < nslots; i++) {
        fm_slot *s = &slots[i];
        if (s->pins) continue;
        if (s->state == 0 || s->gen != dec_gen) return s;
        if (s->state == 2 && s->frame < need_lo && (!best || s->frame < best->frame)) best = s;
    }
    return best;
}

/* the pipe from ffmpeg ended: its exit code. Not one frame yet (`first`) with a hardware decoder: a PC without that GPU, where ffmpeg stops at once. The CPU decodes from here on */
static DWORD dec_ended(int first) {
    DWORD code = 0;
    if (dec_proc) { WaitForSingleObject(dec_proc, 500); GetExitCodeProcess(dec_proc, &code); }
    if (code != 0 && first && *fm_hw && strcmp(fm_hw, "none")) {
        ps2_log("fmv: ffmpeg stopped before the first frame with hwaccel %s (exit code %lu): the CPU decodes the movies", fm_hw, (unsigned long)code);
        snprintf(fm_hw, sizeof fm_hw, "none");
        pthread_mutex_lock(&fm_mx);
        job_gen++;
        pthread_mutex_unlock(&fm_mx);
    }
    return code;
}

static void *dec_main(void *arg) {
    u8 *nv = NULL;
    size_t nv_bytes = 0;
    HANDLE pipe = NULL;
    u32 my_gen = 0;
    int pac = -1, movie = -1, eof = 0;
    s32 next = 0, start = 0;
    double t_spawn = 0;
    (void)arg;
    csc_tables();
    for (;;) {
        fm_slot *s = NULL;
        s32 gf;
        int act = 0;               /* 1 new job, 2 decode into s, 3 drop a frame (behind) */
        pthread_mutex_lock(&fm_mx);
        for (;;) {
            gf = game_frame;
            if (job_gen != my_gen) { act = 1; break; }
            if (pac >= 0 && !eof) {
                if (game_field && ps2_kernel_vblank_count() - game_field > 120u) {
                    /* no picture for 2 s: the movie ended, was skipped or the game is paused */
                    job_pac = job_movie = -1;
                    job_gen++;
                    pthread_mutex_unlock(&fm_mx);
                    movie_summary_dec(pac, movie, (int)gf);
                    pthread_mutex_lock(&fm_mx);
                    act = 1;
                    break;
                }
                if (next < gf - 2) {
                    if (gf - next > 45) {          /* far behind the game: restart at its frame */
                        job_start = gf + 6;
                        job_gen++;
                        if (fm_dbg) ps2_log("fmv: decoder at frame %d, the game at %d: restart", (int)next, (int)gf);
                        act = 1;
                    } else act = 3;
                    break;
                }
                s = slot_for_write(gf - 3);
                if (s) { act = 2; break; }
            }
            {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_nsec += 20 * 1000000;
                if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&fm_cv, &fm_mx, &ts);
            }
        }
        if (act == 1) {
            int npac = job_pac, nmov = job_movie;
            u32 scale = pick_scale();
            start = job_start;
            my_gen = job_gen;
            /* the GS lets go of its strip first; then the old frames are dropped */
            while (gs_pinned) { pthread_mutex_unlock(&fm_mx); Sleep(1); pthread_mutex_lock(&fm_mx); }
            dec_gen = my_gen;
            for (int i = 0; i < nslots; i++) slots[i].state = 0;
            if (npac < 0) slot_free_all();
            else if (!nslots || scale != sscale || sh != fm_mbh * 16u * scale || strip_bytes * fm_mbw != slot_bytes) {
                slot_free_all();
                slots_alloc(scale);
            }
            pthread_mutex_unlock(&fm_mx);
            proc_kill();
            if (pipe) { CloseHandle(pipe); pipe = NULL; }
            if (npac != pac || nmov != movie) st.ff_cpu_ms = st.ff_wall_ms = 0.0;   /* (a restart in a movie adds up) */
            pac = npac; movie = nmov; eof = 0; next = start;
            if (pac < 0) { free(nv); nv = NULL; nv_bytes = 0; continue; }
            {
                char path[700], cmd[2000], ss[64] = "", hw[64] = "";
                const u32 W = fm_mbw * sw, H = sh;
                if ((size_t)W * H * 3u / 2u != nv_bytes) {
                    free(nv);
                    nv_bytes = (size_t)W * H * 3u / 2u;
                    nv = (u8 *)malloc(nv_bytes);
                }
                if (!nv || !nslots) { ps2_log("fmv: out of memory for the decoded frames"); pac = -1; continue; }
                movie_path(pac, movie, path, sizeof path);
                if (start > 0) snprintf(ss, sizeof ss, "-ss %.4f ", (double)start * 1001.0 / 30000.0 - 0.010);
                if (strcmp(fm_hw, "none") && *fm_hw) snprintf(hw, sizeof hw, "-hwaccel %s ", fm_hw);
                snprintf(cmd, sizeof cmd,
                         "\"%s\" -nostdin -hide_banner -loglevel error %s%s-i \"%s\" -an -sn -dn "
                         "-vf scale=%u:%u:flags=lanczos -fps_mode passthrough -pix_fmt nv12 -f rawvideo -",
                         fm_ffmpeg, hw, ss, path, W, H);
                t_spawn = now_ms();
                dec_proc = proc_spawn(cmd, &pipe);
                dec_spawn_ms = t_spawn;
                st.restarts++;
                if (fm_dbg) ps2_log("fmv: decoder %s movie %d from frame %d at %ux%u (x%u, %d frames): %s", pacs[pac].name,
                                    movie, (int)start, W, H, sscale, nslots, cmd);
                if (!dec_proc) pac = -1;
            }
            continue;
        }
        if (act == 3) {
            pthread_mutex_unlock(&fm_mx);
            if (!read_full(pipe, nv, nv_bytes)) { eof = 1; dec_ended(next == start); continue; }
            next++;
            st.frames_drop++;
            continue;
        }
        /* act == 2: decode frame `next` into s */
        s->state = 1;
        s->gen = my_gen;
        s->frame = next;
        pthread_mutex_unlock(&fm_mx);
        {
            u64 t0 = SDL_GetTicksNS(), t1;
            if (!read_full(pipe, nv, nv_bytes)) {
                DWORD code;
                pthread_mutex_lock(&fm_mx);
                s->state = 0;
                eof = 1;
                pthread_mutex_unlock(&fm_mx);
                /* ffmpeg's own end (exit code 0) is the file's end; anything else (killed with the game) is not */
                code = dec_ended(next == start);
                if (code != 0) {
                    if (fm_dbg) ps2_log("fmv: %s movie %d: ffmpeg stopped (exit code %lu) at frame %d", pacs[pac].name,
                                        movie, (unsigned long)code, (int)next);
                } else if ((u32)next < pacs[pac].mov[movie].pictures || fm_dbg)
                    ps2_log("fmv: %s movie %d: the upscaled file ends at frame %d (manifest %u)%s", pacs[pac].name, movie,
                            (int)next, pacs[pac].mov[movie].pictures,
                            (u32)next < pacs[pac].mov[movie].pictures ? ": the rest plays the game's own frames" : "");
                continue;
            }
            t1 = SDL_GetTicksNS();
            if (next == start && fm_dbg)
                ps2_log("fmv: first frame %d from ffmpeg %.0f ms after the start", (int)next, now_ms() - t_spawn);
            convert(nv, s, ee_th0, ee_th1);
            st.read_ns += t1 - t0;
            st.conv_ns += SDL_GetTicksNS() - t1;
            st.frames_dec++;
        }
        pthread_mutex_lock(&fm_mx);
        s->state = (s->gen == my_gen && job_gen == my_gen) ? 2 : 0;
        next++;
        pthread_mutex_unlock(&fm_mx);
    }
    return NULL;
}

static void dec_ensure(void) {
    if (dec_started) return;
    dec_started = 1;
    if (pthread_create(&dec_thread, NULL, dec_main, NULL) != 0) { dec_started = 0; fm_on = 0; }
}

/* ------------------------------------------------------------------------------------------------ EE side */

static void movie_summary_at(int pac, int movie, const char *why, int reset) {
    const int ee_pac = pac, ee_movie = movie;
    if (ee_pac < 0 || ee_movie < 0) return;
    if (!st.pics && !st.strips_rep) return;
    {
        double dec = st.frames_dec ? (double)st.conv_ns / 1e6 / (double)st.frames_dec : 0.0;
        double ffc = st.ff_cpu_ms + proc_cpu_ms(dec_proc), ffw = st.ff_wall_ms + (dec_proc ? now_ms() - dec_spawn_ms : 0.0);
        double wall = (double)(SDL_GetTicksNS() - st.t_start_ns) / 1e6;
        double rd = st.frames_dec ? (double)st.read_ns / 1e6 / (double)st.frames_dec : 0.0;
        u64 strips = st.strips_rep + st.strips_orig_notready + st.strips_orig_nomatch + st.strips_orig_ab;
        if (st.pics_past)
            ps2_log("fmv: %s movie %d %s: %llu of its pictures came after the stream's last one (%s)", pacs[ee_pac].name,
                    ee_movie, why, (unsigned long long)st.pics_past,
                    fm_end_hold ? "ACZ_FMV_END_HOLD: our last frame shown" : "the game's own shown");
        ps2_log("fmv: %s movie %d %s: %llu pictures (%llu matched (%llu above MAD 4), %llu by a neighbouring frame, %llu no match; block "
                "MAD avg %.2f max %.2f), strips %llu replaced / %llu not ready / %llu no match / %llu A-B / %llu unknown; decoded %llu "
                "(%llu dropped, %llu starts; pipe wait %.2f ms, convert %.2f ms a frame); GS %.1f us a replaced strip; "
                "first replaced at field +%lld; ffmpeg CPU %.0f ms in %.1f s (%.0f %% of a core), our conversion %.0f %% of a "
                "core, %.0f MB of decoded frames", pacs[ee_pac].name, ee_movie, why, (unsigned long long)st.pics,
                (unsigned long long)st.pics_ok, (unsigned long long)st.pics_loose, (unsigned long long)st.pics_shift,
                (unsigned long long)st.pics_bad,
                st.mad_n ? st.mad_sum / (double)st.mad_n : 0.0, st.mad_max, (unsigned long long)st.strips_rep,
                (unsigned long long)st.strips_orig_notready, (unsigned long long)st.strips_orig_nomatch,
                (unsigned long long)st.strips_orig_ab, (unsigned long long)st.strips_nohash, (unsigned long long)st.frames_dec,
                (unsigned long long)st.frames_drop, (unsigned long long)st.restarts, rd, dec,
                st.strips_rep ? (double)st.gs_ns / 1e3 / (double)st.strips_rep : 0.0,
                st.first_rep_field ? (long long)(st.first_rep_field - st.start_field) : -1LL, ffc, ffw / 1e3,
                ffw > 0 ? 100.0 * ffc / ffw : 0.0, wall > 0 ? 100.0 * (double)st.conv_ns / 1e6 / wall : 0.0,
                (double)slot_bytes * nslots / (1024.0 * 1024.0));
        (void)strips;
    }
    if (reset) memset(&st, 0, sizeof st);
}

static void movie_summary(const char *why) { movie_summary_at(ee_pac, ee_movie, why, 1); }

static void movie_dims(int pi, int m, const struct ps2_disc_file *f) {
    /* the picture size from the MPEG2 sequence header at the start of the movie's first chunk (MOVIEUS.PAC: movie 0
       is 640 x 448, the 35 story movies 640 x 304; BRIEF.PAC: 640 x 448) */
    fm_movie *mv = &pacs[pi].mov[m];
    u8 b[16];
    if (mv->w) return;
    mv->w = pacs[pi].w; mv->h = pacs[pi].h;
    if (mv->c0 < pacs[pi].nch && ps2_vfs_read(f, (u64)pacs[pi].ch[mv->c0].offset + 16u, 16u, b) == 16
        && b[0] == 0 && b[1] == 0 && b[2] == 1 && b[3] == 0xB3) {
        u32 w = ((u32)b[4] << 4) | (b[5] >> 4), h = ((u32)(b[5] & 15u) << 8) | b[6];
        if (w >= 16u && h >= 16u && w % 16u == 0u && h % 16u == 0u && w / 16u <= FM_MBW_MAX && h / 16u <= FM_MBH_MAX) {
            mv->w = w; mv->h = h;
        }
    }
}

static void movie_start(int pi, int m, s32 frame, const char *how) {
    movie_summary("ended");
    zero_fmv_trace = 0;
    ee_pac = pi;
    ee_movie = m;
    ee_next = frame;
    ee_acc = 0;
    __atomic_store_n(&fm_fix, 0, __ATOMIC_RELEASE);
    st.start_field = ps2_kernel_vblank_count();
    st.t_start_ns = SDL_GetTicksNS();
    fm_mbw = pacs[pi].mov[m].w / 16u;
    fm_mbh = pacs[pi].mov[m].h / 16u;
    if (fm_mbw > FM_MBW_MAX) fm_mbw = FM_MBW_MAX;
    if (fm_mbh > FM_MBH_MAX) fm_mbh = FM_MBH_MAX;
    {   /* looked at again at each start: an upscale run finishing during the session (upscale_fmv.sh writes movie_NN.mkv
           whole, through a .tmp rename) is played from then on */
        char path[700];
        int k = movie_path(pi, m, path, sizeof path);
        pacs[pi].mov[m].have = file_size_ok(path, NULL) ? k : 0;
    }
    pthread_mutex_lock(&fm_mx);
    job_gen++;
    if (pacs[pi].mov[m].have) {
        job_pac = pi; job_movie = m; job_start = frame;
    } else {
        job_pac = job_movie = -1;
    }
    game_frame = frame - 1;
    game_field = ps2_kernel_vblank_count();
    pthread_cond_broadcast(&fm_cv);
    pthread_mutex_unlock(&fm_mx);
    if (pacs[pi].mov[m].have) dec_ensure();
    ps2_log("fmv: %s movie %d (%ux%u) starts at frame %d (%s, field %llu): %s", pacs[pi].name, m, pacs[pi].mov[m].w,
            pacs[pi].mov[m].h, (int)frame, how, (unsigned long long)ps2_kernel_vblank_count(),
            pacs[pi].mov[m].have ? pack_name(pi, pacs[pi].mov[m].have) : "no upscaled file, the game's own frames");
}

void zero_fmv_read(const struct ps2_disc_file *f, u64 pos, u32 len, u32 buf, int got) {
    int pi;
    fm_pac *p;
    u32 m, lo, hi;
    if (!fm_init_done) fm_init();
    if (!fm_on || got <= 0) return;
    {   /* the PAC by name (the VFS numbers its files itself: f->lsn is not the disc's) and size */
        const char *b = strrchr(f->name, '/'), *b2 = strrchr(f->name, '\\');
        if (b2 > b) b = b2;
        b = b ? b + 1 : f->name;
        for (pi = 0; pi < FM_PACS; pi++)
            if (pacs[pi].loaded > 0 && !SDL_strcasecmp(b, pacs[pi].name) && f->size == pacs[pi].size) break;
        if (pi == FM_PACS) return;
    }
    p = &pacs[pi];
    /* the movie holding pos */
    lo = 0; hi = p->nmov;
    while (hi - lo > 1u) { u32 mid = (lo + hi) / 2u; if (p->mov[mid].offset <= pos) lo = mid; else hi = mid; }
    m = lo;
    if (fm_dbg >= 2) ps2_log("fmv: read %s pos 0x%llX len %u -> %08X (movie %u)", p->name, (unsigned long long)pos, len, buf, m);
    movie_dims(pi, (int)m, f);
    if (pos == (u64)p->mov[m].offset + 0x800u || pos == (u64)p->mov[m].offset) {
        movie_start(pi, (int)m, 0, "header read");
    } else if (pos != ee_read_end[pi] || pi != ee_pac || (int)m != ee_movie) {
        /* a seek into the movie: the chunk read starts at pos */
        u32 c;
        s32 fr = -1;
        for (c = p->mov[m].c0; c <= p->mov[m].c1 && c < p->nch; c++)
            if (p->ch[c].offset == pos) { fr = (s32)p->ch[c].first; break; }
        if (fr >= 0) movie_start(pi, (int)m, fr, "chunk read");
    }
    ee_read_end[pi] = pos + (u64)(u32)got;
}

static u32 alpha_conv[256];          /* GS alpha -> the decode's (a >= 0x80 -> 255, else 2a) */

static u32 strip_hash(const u8 *tx, u32 tw, u32 h) {
    /* the top-left 16 x h texels of a decode (rows of tw RGBA texels) */
    u32 hs = 2166136261u;
    for (u32 y = 0; y < h; y++) {
        const u32 *row = (const u32 *)(tx + (size_t)y * tw * 4u);
        for (u32 x = 0; x < 16u; x++) hs = (hs ^ row[x]) * 16777619u;
    }
    return hs;
}

static double sig_mad(const u8 *a, const u8 *b);
static void pic_finish(u32 th0, u32 th1) {
    fm_pic *pp;
    u32 c, slot, k;
    s32 fix = __atomic_exchange_n(&fm_fix, 0, __ATOMIC_ACQ_REL);
    u8 sig[FM_MBW_MAX * FM_MBH_MAX];
    u32 col[FM_MBW_MAX];
    const u32 words = fm_mbh * 256u;
    if (ee_next >= 0) ee_next += fix;
    for (c = 0; c < fm_mbw; c++) {
        const u32 *w = ee_pic + (size_t)c * words;
        u32 h = 2166136261u;
        /* the column as the GS decode shows it (RGBA, alpha 0x80 -> 255; ps2_gs.c texel_rgba): its content hash */
        for (k = 0; k < words; k++) h = (h ^ ((w[k] & 0x00FFFFFFu) | (alpha_conv[w[k] >> 24] << 24))) * 16777619u;
        col[c] = h;
        for (u32 r = 0; r < fm_mbh; r++) {
            const u32 *mb = w + r * 256u;
            u32 g = 0;
            for (k = 0; k < 256u; k++) g += (mb[k] >> 8) & 0xFFu;
            sig[r * fm_mbw + c] = (u8)((g + 128u) / 256u);
        }
    }
    if (fm_tail > 0 && ee_movie >= 0 && ee_next >= 0
        && (u32)ee_next + (u32)fm_tail >= pacs[ee_pac].mov[ee_movie].pictures) {
        /* [debug] the movie's last pictures as the game converted them, and whether each repeats the previous one */
        const fm_pic *q = pic_serial ? &pics[(pic_serial + FM_PICS - 1u) % FM_PICS] : NULL;
        u32 same = 0;
        char nm[300];
        FILE *fp;
        if (q && q->movie == ee_movie)
            for (c = 0; c < fm_mbw; c++) same += q->col[c] == col[c];
        ps2_log("fmv: tail %s movie %d picture %d (manifest %u, field %llu): %u of %u columns as the previous picture, "
                "block MAD to it %.2f", pacs[ee_pac].name, ee_movie, (int)ee_next, pacs[ee_pac].mov[ee_movie].pictures,
                (unsigned long long)ps2_kernel_vblank_count(), same, fm_mbw,
                q && q->movie == ee_movie ? sig_mad(q->sig, sig) : -1.0);
        CreateDirectoryA("out/fmv_dbg", NULL);
        snprintf(nm, sizeof nm, "out/fmv_dbg/%s_m%02d_tail_%06d.ppm", pacs[ee_pac].dir, ee_movie, (int)ee_next);
        if ((fp = fopen(nm, "wb")) != NULL) {
            const u32 W = fm_mbw * 16u, H = fm_mbh * 16u;
            fprintf(fp, "P6\n%u %u\n255\n", W, H);
            for (u32 y = 0; y < H; y++)
                for (u32 x = 0; x < W; x++) {
                    u32 v = ee_pic[((size_t)(x >> 4) * fm_mbh + (y >> 4)) * 256u + (y & 15u) * 16u + (x & 15u)];
                    fputc((int)(v & 0xFFu), fp); fputc((int)((v >> 8) & 0xFFu), fp); fputc((int)((v >> 16) & 0xFFu), fp);
                }
            fclose(fp);
        }
    }
    pthread_mutex_lock(&fm_mx);
    slot = pic_serial % FM_PICS;
    pp = &pics[slot];
    pp->serial = ++pic_serial;
    pp->gen = job_gen;
    pp->pac = ee_pac;
    pp->movie = ee_movie;
    pp->frame = ee_movie >= 0 ? ee_next : -1;
    pp->use = -1;
    /* Past the movie's end (a safety net, ACZ_FMV_END_HOLD): until PS2_DMA_IPU_TAG_KEEP (ps2_hw.c, fmv_fix 2026-10-29)
       the IPU input chain went on into a stale tag after a movie's last chunk whenever that chunk was under ~1 MB,
       and the game's player converted 17-41 more pictures from it -- a few broken ones, then a replay of frames
       43-70 before the end (the 2026-10-28 session: MOVIEUS 2, 3, 4, 11, 12, 17, 18, BRIEF 4, 8, 10, 11). Such
       pictures show the movie's last upscaled frame. */
    pp->past_end = ee_movie >= 0 && ee_next >= 0 && (u32)ee_next >= pacs[ee_pac].mov[ee_movie].pictures
                   && pacs[ee_pac].mov[ee_movie].pictures > 0u;
    if (pp->past_end) st.pics_past++;
    pp->th0 = th0; pp->th1 = th1;
    pp->last_col = -1;
    memcpy(pp->col, col, sizeof(u32) * fm_mbw);
    memcpy(pp->sig, sig, fm_mbw * fm_mbh);
    if ((fm_dump_n > 0 && pp->frame >= 0 && pp->frame % fm_dump_every == fm_dump_every / 2) || fm_dbg >= 2) {
        if (!pp->dump) pp->dump = (u8 *)malloc((size_t)FM_MBW_MAX * FM_MBH_MAX * 1024u);   /* (any movie size) */
        if (pp->dump) memcpy(pp->dump, ee_pic, (size_t)fm_mbw * fm_mbh * 1024u);
    } else if (pp->dump) { free(pp->dump); pp->dump = NULL; }
    if (ee_movie >= 0) {
        game_frame = ee_next;
        game_field = ps2_kernel_vblank_count();
        if (job_pac < 0 && pacs[ee_pac].mov[ee_movie].have && (u32)ee_next + 30u < pacs[ee_pac].mov[ee_movie].pictures) {
            /* pictures again after the decoder stopped (a pause of more than 2 s): restart a little ahead */
            job_gen++;
            job_pac = ee_pac; job_movie = ee_movie; job_start = ee_next + 8;
            pp->gen = job_gen;
            if (fm_dbg) ps2_log("fmv: %s movie %d goes on at frame %d: decoder restarted", pacs[ee_pac].name, ee_movie,
                                (int)ee_next);
        }
        pthread_cond_broadcast(&fm_cv);
    }
    pthread_mutex_unlock(&fm_mx);
    if (ee_movie >= 0) {
        st.pics++;
        if (fm_dbg >= 2)
            ps2_log("fmv: picture %u = %s movie %d frame %d (field %llu)", pic_serial, pacs[ee_pac].name, ee_movie,
                    (int)ee_next, (unsigned long long)ps2_kernel_vblank_count());
        if (fm_ab > 0 && ee_next % fm_ab == 0)
            ps2_log("fmv-ab: frame %d field %llu: %s", (int)ee_next, (unsigned long long)ps2_kernel_vblank_count(),
                    (ee_next / fm_ab) % 2 ? "original" : "replaced");
        if (fm_dbg && ee_next > 0 && ee_next % 300 == 0) movie_summary_at(ee_pac, ee_movie, "so far", 0);
        ee_next++;
        if (fm_tail > 0 && (u32)ee_next + (u32)fm_tail >= pacs[ee_pac].mov[ee_movie].pictures) zero_fmv_trace = 1;
        if ((u32)ee_next >= pacs[ee_pac].mov[ee_movie].pictures && fm_dbg)
            ps2_log("fmv: %s movie %d: last frame (%d) converted", pacs[ee_pac].name, ee_movie, (int)ee_next - 1);
    }
}

void zero_fmv_csc(const u8 *out, u32 mbc, int ofm, u32 th0, u32 th1) {
    u64 now;
    u32 mbn;
    if (!fm_init_done) fm_init();
    if (!fm_on) return;
    now = ps2_kernel_vblank_count();
    mbn = fm_mbw * fm_mbh;
    if (!ee_pic) {
        ee_pic = (u32 *)malloc((size_t)FM_MBW_MAX * FM_MBH_MAX * 1024u);
        for (u32 a = 0; a < 256u; a++) alpha_conv[a] = a >= 128u ? 255u : a * 2u;
    }
    if (!ee_pic) return;
    if (fm_dbg >= 2) {
        static u32 last_mbc = 0xFFFFFFFFu, n;
        if ((mbc != last_mbc || ofm) && n++ < 200u)
            ps2_log("fmv: csc %u macroblocks ofm %d th %u/%u (acc %u, field %llu)", mbc, ofm, th0, th1, ee_acc,
                    (unsigned long long)now);
        last_mbc = mbc;
    }
    if (ofm) { ee_acc = 0; return; }
    ee_th0 = th0; ee_th1 = th1;
    if (ee_acc && now - ee_last_csc > 2u) ee_acc = 0;     /* a picture never completed: start again */
    ee_last_csc = now;
    if (ee_acc + mbc > mbn) ee_acc = 0;
    if (mbc > mbn) return;
    memcpy(ee_pic + (size_t)ee_acc * 256u, out, (size_t)mbc * 1024u);
    ee_acc += mbc;
    if (ee_acc == mbn) { pic_finish(th0, th1); ee_acc = 0; }
}

/* ------------------------------------------------------------------------------------------------ GS side */

static double sig_mad(const u8 *a, const u8 *b) {
    u32 n = fm_mbw * fm_mbh, s = 0;
    for (u32 i = 0; i < n; i++) s += (u32)(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
    return (double)s / (double)n;
}

static fm_slot *slot_of(u32 gen, s32 frame) {
    for (int i = 0; i < nslots; i++)
        if (slots[i].state == 2 && slots[i].gen == gen && slots[i].frame == frame) return &slots[i];
    return NULL;
}

static void dump_compare(const fm_pic *p, const fm_slot *s) {
    /* the IPU picture, our frame box-downscaled to its size, and |difference| x 4 (GS thread, debug only) */
    static int done;
    char nm[300];
    FILE *fi, *fo, *fd;
    const u32 W = fm_mbw * 16u, H = fm_mbh * 16u;
    if (done >= fm_dump_n || !p->dump) return;
    done++;
    CreateDirectoryA("out/fmv_dbg", NULL);
    snprintf(nm, sizeof nm, "out/fmv_dbg/%s_m%02d_f%06d_ipu.ppm", pacs[p->pac].dir, p->movie, (int)p->use);
    fi = fopen(nm, "wb");
    snprintf(nm, sizeof nm, "out/fmv_dbg/%s_m%02d_f%06d_ours.ppm", pacs[p->pac].dir, p->movie, (int)p->use);
    fo = fopen(nm, "wb");
    snprintf(nm, sizeof nm, "out/fmv_dbg/%s_m%02d_f%06d_diff.ppm", pacs[p->pac].dir, p->movie, (int)p->use);
    fd = fopen(nm, "wb");
    if (fi && fo && fd) {
        double sum = 0;
        fprintf(fi, "P6\n%u %u\n255\n", W, H);
        fprintf(fo, "P6\n%u %u\n255\n", W, H);
        fprintf(fd, "P6\n%u %u\n255\n", W, H);
        for (u32 y = 0; y < H; y++)
            for (u32 x = 0; x < W; x++) {
                u32 c = x >> 4, r = y >> 4;
                u32 ip = ((const u32 *)p->dump)[((size_t)c * fm_mbh + r) * 256u + (y & 15u) * 16u + (x & 15u)];
                u32 acc[3] = { 0, 0, 0 };
                const u8 *strip = s->strips + (size_t)c * strip_bytes;
                for (u32 dy = 0; dy < sscale; dy++)
                    for (u32 dx = 0; dx < sscale; dx++) {
                        const u8 *q = strip + ((size_t)(y * sscale + dy) * sw + (x & 15u) * sscale + dx) * 4u;
                        acc[0] += q[0]; acc[1] += q[1]; acc[2] += q[2];
                    }
                for (int k = 0; k < 3; k++) {
                    int a = (int)((ip >> (8 * k)) & 0xFFu), b = (int)((acc[k] + sscale * sscale / 2u) / (sscale * sscale));
                    int d = a > b ? a - b : b - a;
                    sum += d;
                    fputc(a, fi); fputc(b, fo); fputc(d * 4 > 255 ? 255 : d * 4, fd);
                }
            }
        ps2_log("fmv: dump %s movie %d frame %d: mean abs difference IPU vs ours (downscaled) %.2f", pacs[p->pac].name,
                p->movie, (int)p->use, sum / ((double)W * H * 3.0));
    }
    if (fi) fclose(fi);
    if (fo) fclose(fo);
    if (fd) fclose(fd);
}

static void pic_verify(fm_pic *p) {
    /* The count is trusted (the game converts every picture of a movie, in order): the expected frame, unless a
       neighbour (+-1, +-2) matches clearly better -- then that one, and three such pictures in a row move the count.
       Same frame: block MAD (mean |difference| of the 40 x 28 macroblock means of green, 0..255) 0.3-2.2 on CG, up to
       ~4-16 on fast live action (bwdif mends the combed fields of the original; the upscaler's detail), neighbours
       then 6-24. No match (the game's own picture) only above ACZ_FMV_MAD_MAX (24) -- not this movie's content.
       (fm_mx held) */
    double mad[5], best_m = 1e9;
    int best = -1;
    fm_slot *ss[5];
    if (p->past_end) {
        const s32 last = (s32)pacs[p->pac].mov[p->movie].pictures - 1;
        if (!fm_end_hold) { p->use = -2; return; }          /* the game's own picture */
        if (slot_of(p->gen, last)) p->use = last;
        else p->use = -2;                                    /* (our last frame is gone: the game's own) */
        return;
    }
    for (int d = -2; d <= 2; d++) {
        ss[d + 2] = slot_of(p->gen, p->frame + d);
        mad[d + 2] = ss[d + 2] ? sig_mad(p->sig, ss[d + 2]->sig) : 1e9;
        if (d != 0 && mad[d + 2] < best_m) { best_m = mad[d + 2]; best = d; }
    }
    if (!ss[2]) return;           /* the expected frame is not decoded yet: check again at the next strip */
    if (!(best_m < mad[2] - 1.5 && best_m < 0.6 * mad[2]) && mad[2] <= fm_mad_max) {
        p->use = p->frame;
        st.pics_ok++;
        if (mad[2] > 4.0) {
            st.pics_loose++;
            if (fm_dbg && st.pics_loose <= 6)
                ps2_log("fmv: %s movie %d frame %d: block MAD %.1f (neighbours %.1f %.1f | %.1f %.1f): the expected frame",
                        pacs[p->pac].name, p->movie, (int)p->frame, mad[2], mad[0], mad[1], mad[3], mad[4]);
        }
        gs_fix_votes = 0;
    } else if (best_m <= fm_mad_max) {
        p->use = p->frame + best;
        st.pics_shift++;
        if (best == gs_fix_delta) gs_fix_votes++; else { gs_fix_delta = best; gs_fix_votes = 1; }
        if (gs_fix_votes >= 3) {
            __atomic_store_n(&fm_fix, gs_fix_delta, __ATOMIC_RELEASE);
            gs_fix_votes = 0;
            ps2_log("fmv: %s movie %d: the game's pictures run %+d frame(s) from the count: corrected", pacs[p->pac].name,
                    p->movie, gs_fix_delta);
        }
    } else {
        p->use = -2;
        st.pics_bad++;
        if (fm_dbg && st.pics_bad <= 8)
            ps2_log("fmv: %s movie %d frame %d: no decoded frame matches the game's picture (block MAD %.1f / %.1f / "
                    "%.1f / %.1f / %.1f): the game's own", pacs[p->pac].name, p->movie, (int)p->frame, mad[0], mad[1],
                    mad[2], mad[3], mad[4]);
        return;
    }
    {
        double m = mad[p->use != p->frame ? best + 2 : 2];
        st.mad_sum += m; st.mad_n++;
        if (m > st.mad_max) st.mad_max = m;
    }
    if (fm_dbg >= 2)
        ps2_log("fmv: picture %u frame %d -> %d (MAD %.2f %.2f %.2f %.2f %.2f)", p->serial, (int)p->frame, (int)p->use,
                mad[0], mad[1], mad[2], mad[3], mad[4]);
}

static void strip_probe(const u8 *tx, u32 tw, u32 hash) {
    /* [debug] a movie-sized strip whose hash is no column of the newest picture: how close is it? (fm_mx held) */
    static u32 n;
    const fm_pic *q = &pics[(pic_serial + FM_PICS - 1u) % FM_PICS];
    u32 best = 0, bc = 0, k;
    u64 bs = ~0ull;
    if (n >= 24u || !q->dump) return;
    for (u32 c = 0; c < fm_mbw; c++) {
        const u32 *w = (const u32 *)q->dump + (size_t)c * fm_mbh * 256u;
        u64 s = 0;
        for (k = 0; k < fm_mbh * 256u; k += 7u) {
            const u8 *t = tx + ((size_t)(k / 16u) * tw + (k % 16u)) * 4u;
            for (int ch = 0; ch < 3; ch++) {
                int a = (int)((w[k] >> (8 * ch)) & 0xFFu), b = t[ch];
                s += (u64)(a > b ? a - b : b - a);
            }
        }
        if (s < bs) { bs = s; bc = c; }
    }
    {
        const u32 *w = (const u32 *)q->dump + (size_t)bc * fm_mbh * 256u;
        const u32 *t = (const u32 *)tx;
        (void)tw;
        best = (u32)(bs * 7u / (fm_mbh * 256u * 3u));
        n++;
        ps2_log("fmv: strip %08X matches no column of picture %u: nearest column %u (mean abs diff ~%u); texels %08X %08X "
                "%08X %08X, IPU %08X %08X %08X %08X", hash, q->serial, bc, best, t[0], t[1], t[3000], t[5000], w[0], w[1],
                w[3000], w[5000]);
    }
}

const u8 *zero_fmv_strip(const u8 *texels, u32 tw, u32 th, u32 *rw, u32 *rh, u32 *scale, u32 *salt) {
    fm_pic *p = NULL;
    fm_slot *s;
    int col = -1;
    u64 t0;
    u32 hash, H = fm_mbh * 16u;
    /* only while a movie plays (a picture within the last 2 s) and for decodes that can hold a column (the 640 x 448
       movies' strips decode as 16 x 448; the 640 x 304 ones' as 64 x 640, the column in the top-left corner) */
    if (!fm_on || tw < 16u || th < H || tw > 1024u || !texels || !__atomic_load_n(&pic_serial, __ATOMIC_ACQUIRE)
        || ps2_kernel_vblank_count() - game_field > 120u)
        return NULL;
    t0 = SDL_GetTicksNS();
    hash = strip_hash(texels, tw, H);
    pthread_mutex_lock(&fm_mx);
    /* the newest picture with a column of this content; among equal columns the one after the last drawn */
    for (u32 k = 0; k < FM_PICS; k++) {
        fm_pic *q = &pics[(pic_serial - k + FM_PICS * 4u - 1u) % FM_PICS];
        int found = -1;
        if (!q->serial || q->movie < 0) continue;
        for (u32 c = 0; c < fm_mbw; c++)
            if (q->col[c] == hash) {
                if (found < 0) found = (int)c;
                if ((int)c == q->last_col + 1) { found = (int)c; break; }
            }
        if (found >= 0) { p = q; col = found; break; }
    }
    if (!p) {
        st.strips_nohash++;
        if (fm_dbg >= 2) strip_probe(texels, tw, hash);
        pthread_mutex_unlock(&fm_mx);
        return NULL;
    }
    p->last_col = col;
    if (fm_ab > 0 && p->frame >= 0 && (p->frame / fm_ab) % 2 == 1) {
        st.strips_orig_ab++;
        pthread_mutex_unlock(&fm_mx);
        return NULL;
    }
    if (p->use == -1) pic_verify(p);
    if (p->use < 0) {
        if (p->use == -2) st.strips_orig_nomatch++; else st.strips_orig_notready++;
        pthread_mutex_unlock(&fm_mx);
        return NULL;
    }
    s = slot_of(p->gen, p->use);
    if (!s) { st.strips_orig_notready++; pthread_mutex_unlock(&fm_mx); return NULL; }
    if (p->dump && col == 0) dump_compare(p, s);
    s->pins++;
    gs_pinned = s;
    gs_last_frame = p->use;
    if (!st.first_rep_field) st.first_rep_field = ps2_kernel_vblank_count();
    st.strips_rep++;
    pthread_mutex_unlock(&fm_mx);
    *rw = sw;
    *rh = sh;
    *scale = sscale;
    *salt = ((((u32)p->movie << 20) ^ ((u32)p->use << 6) ^ (u32)col) * 0x9E3779B1u) | 1u;
    gs_t0 = t0;
    return s->strips + (size_t)col * strip_bytes;
}

void zero_fmv_strip_done(void) {
    if (!gs_pinned) return;
    st.gs_ns += SDL_GetTicksNS() - gs_t0;      /* (with the texture store's copy of the strip) */
    pthread_mutex_lock(&fm_mx);
    gs_pinned->pins--;
    gs_pinned = NULL;
    pthread_cond_broadcast(&fm_cv);
    pthread_mutex_unlock(&fm_mx);
}
