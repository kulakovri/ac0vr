/* PS2_VK_CENSUS (diagnostic, perf4_timeline 2026-11-03): the Vulkan command census per frame part and the pass census
   (ps2_vkcen.h has the how). This file keeps the counted frame's events, folds each counted frame into a window by frame
   kind and logs the window:

     vk-census: window S s, frames game G in-between I other O (values per counted frame: game / in-between) ...
     vk-census part "NAME": begin a/b end a/b barriers a/b (images a/b) pipelines a/b descsets a/b push a/b draws a/b
                            dispatch a/b copies a/b clears a/b dyn a/b
     vk-census total: (the same over all parts)
     vk-census pass: a/b begin FILE:LINE FUNC <- end FILE:LINE FUNC | part "NAME" | flags | ret 0xRVA
     vk-census barrier: a/b (images a/b) FILE:LINE FUNC | part "NAME" | flags | ret 0xRVA
     vk-census copy: / clear: a/b (regions a/b) FILE:LINE FUNC | part "NAME" | flags | ret 0xRVA

   `ret` is the return address of the function the call is in (its caller: barrier_flush's, begin_rendering's ...) as
   an offset from the exe's base (the window line gives the base): tools/perf/vk_census.py names it from the exe's
   symbols (nm). Flags: begin -- same (the same colour / depth view as the rendering before it in the frame), first (no
   end before it in the frame), area (same target, another render area), mv (multiview), clear (load op CLEAR);
   barrier -- layout (an image changes layout), all (ALL_COMMANDS in a stage mask), region (BY_REGION), mem (global
   memory barriers), buf (buffer barriers), inpass (inside a rendering); clear -- inpass.

   PS2_VK_CENSUS: 1 = on, a window every 30 s; N >= 5 = a window every N s. PS2_VK_CENSUS_EVERY=K: only every K-th
   frame is counted (default 1). PS2_VK_CENSUS_AB=N (measurement): counted in odd blocks of N frames only (the
   PS2_THREAD_PROF event 24 marks the blocks: a = block, b = 1 counted), so the census' own recording cost is the
   paired difference of the recording time (tools/perf/frame_timeline.py --census-ab). */
#define PS2_VKCEN_IMPL
#include "ps2_runtime.h"
#include "ps2_vkcen.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

int ps2_vkcen_on;
unsigned ps2_vkcen_part;
unsigned ps2_vkcen_cnt[PS2_VKCEN_PARTS][VKC_N];

static int cen_cfg = -1;                 /* -1 unread, 0 off, 1 on */
static int cen_secs = 30, cen_every = 1, cen_ab = 0, cen_ab_blk = -1;
static const char *const *cen_pnames;
static unsigned cen_npnames;
static uintptr_t cen_base;

#define SITE_MAX 2048u
static struct { const char *file, *func; int line; } sites[SITE_MAX];
static unsigned nsites = 1;              /* 0 = none */
static pthread_mutex_t site_lock = PTHREAD_MUTEX_INITIALIZER;

unsigned short ps2_vkcen_site(const char *file, int line, const char *func) {
    unsigned i;
    pthread_mutex_lock(&site_lock);
    for (i = 1; i < nsites; i++) if (sites[i].line == line && !strcmp(sites[i].file, file)) break;
    if (i == nsites) {
        if (nsites < SITE_MAX) { sites[i].file = file; sites[i].func = func; sites[i].line = line; nsites++; }
        else i = SITE_MAX - 1u;
    }
    pthread_mutex_unlock(&site_lock);
    return (unsigned short)i;
}

/* the counted frame's pass events */
typedef struct { unsigned short site, prev; unsigned char what, part, flags, pad; unsigned n, ret; } fev_t;
#define FEV_MAX 4096u
static fev_t fev[FEV_MAX];
static unsigned nfev, fev_lost;
static unsigned short last_end;
static int had_end, in_pass;
static uint64_t last_att;
static VkRect2D last_area;

static unsigned rva(void *ret) {
    uintptr_t r = (uintptr_t)ret;
    return r > cen_base && r - cen_base < 0xFFFFFFFFu ? (unsigned)(r - cen_base) : 0u;
}
static void fev_add(int what, unsigned short site, unsigned short prev, unsigned flags, unsigned n, void *ret) {
    if (nfev >= FEV_MAX) { fev_lost++; return; }
    fev_t *e = &fev[nfev++];
    e->site = site; e->prev = prev; e->what = (unsigned char)what; e->part = (unsigned char)ps2_vkcen_part;
    e->flags = (unsigned char)flags; e->pad = 0; e->n = n; e->ret = rva(ret);
}

#define ALLC (VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT)
void ps2_vkcen_ev(int what, unsigned short site, const void *arg, unsigned n, void *ret) {
    unsigned (*c)[VKC_N] = ps2_vkcen_cnt;
    unsigned p = ps2_vkcen_part;
    if (what == VKCE_BEGIN) {
        const VkRenderingInfo *ri = (const VkRenderingInfo *)arg;
        uint64_t att = 0; unsigned fl = 0;
        if (ri->colorAttachmentCount && ri->pColorAttachments) {
            att = (uint64_t)(uintptr_t)ri->pColorAttachments[0].imageView;
            if (ri->pColorAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) fl |= 16u;
        } else if (ri->pDepthAttachment) att = (uint64_t)(uintptr_t)ri->pDepthAttachment->imageView;
        if (att && att == last_att) {
            fl |= 1u;
            if (memcmp(&ri->renderArea, &last_area, sizeof last_area)) fl |= 4u;
        }
        if (!had_end) fl |= 2u;
        if (ri->viewMask) fl |= 8u;
        last_att = att; last_area = ri->renderArea;
        c[p][VKC_BEGIN]++;
        fev_add(VKCE_BEGIN, site, last_end, fl, 1u, ret);
        in_pass = 1;
    } else if (what == VKCE_END) {
        c[p][VKC_END]++;
        fev_add(VKCE_END, site, 0, 0u, 1u, ret);
        last_end = site; had_end = 1; in_pass = 0;
    } else if (what == VKCE_BARRIER) {
        const VkDependencyInfo *di = (const VkDependencyInfo *)arg;
        unsigned fl = 0, ni = di->imageMemoryBarrierCount;
        for (unsigned k = 0; k < ni; k++) {
            const VkImageMemoryBarrier2 *b = &di->pImageMemoryBarriers[k];
            if (b->oldLayout != b->newLayout) fl |= 1u;
            if ((b->srcStageMask | b->dstStageMask) & ALLC) fl |= 2u;
        }
        for (unsigned k = 0; k < di->memoryBarrierCount; k++)
            if ((di->pMemoryBarriers[k].srcStageMask | di->pMemoryBarriers[k].dstStageMask) & ALLC) fl |= 2u;
        for (unsigned k = 0; k < di->bufferMemoryBarrierCount; k++)
            if ((di->pBufferMemoryBarriers[k].srcStageMask | di->pBufferMemoryBarriers[k].dstStageMask) & ALLC) fl |= 2u;
        if (di->dependencyFlags & VK_DEPENDENCY_BY_REGION_BIT) fl |= 4u;
        if (di->memoryBarrierCount) fl |= 8u;
        if (di->bufferMemoryBarrierCount) fl |= 16u;
        if (in_pass) fl |= 32u;
        c[p][VKC_BARC]++; c[p][VKC_BARI] += ni;
        fev_add(VKCE_BARRIER, site, 0, fl, ni, ret);
    } else if (what == VKCE_COPY) {
        c[p][VKC_COPY]++;
        fev_add(VKCE_COPY, site, 0, 0u, n, ret);
    } else if (what == VKCE_CLEAR) {
        c[p][VKC_CLEAR]++;
        fev_add(VKCE_CLEAR, site, 0, in_pass ? 32u : 0u, n, ret);
    }
}
void ps2_vkcen_barrier1(unsigned short site, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dfl,
                        unsigned nmem, unsigned nbuf, unsigned nimg, const VkImageMemoryBarrier *img, void *ret) {
    unsigned fl = 0, p = ps2_vkcen_part;
    for (unsigned k = 0; k < nimg; k++) if (img[k].oldLayout != img[k].newLayout) fl |= 1u;
    if ((src | dst) & (VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT)) fl |= 2u;
    if (dfl & VK_DEPENDENCY_BY_REGION_BIT) fl |= 4u;
    if (nmem) fl |= 8u;
    if (nbuf) fl |= 16u;
    if (in_pass) fl |= 32u;
    ps2_vkcen_cnt[p][VKC_BARC]++; ps2_vkcen_cnt[p][VKC_BARI] += nimg;
    fev_add(VKCE_BARRIER, site, 0, fl, nimg, ret);
}

/* ---- the window ---- */
typedef struct { unsigned short site, prev; unsigned char what, part, flags, used; unsigned ret; double n[3], sum[3]; } wev_t;
#define WEV_N 8192u
static wev_t wev[WEV_N];
static unsigned wev_lost;
static double win_cnt[3][PS2_VKCEN_PARTS][VKC_N];
static double win_frames[3];
static double win_t0;
static struct { unsigned long long frame; unsigned tot[8]; } ring[32];

static double mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int ps2_vkcen_enabled(void) {
    if (cen_cfg >= 0) return cen_cfg;
    const char *e = getenv("PS2_VK_CENSUS");
    cen_cfg = e && *e && strcmp(e, "0") != 0;
    if (cen_cfg) {
        int s = atoi(e);
        cen_secs = s >= 5 ? s : 30;
        e = getenv("PS2_VK_CENSUS_EVERY"); cen_every = e ? atoi(e) : 1; if (cen_every < 1) cen_every = 1;
        e = getenv("PS2_VK_CENSUS_AB"); cen_ab = e ? atoi(e) : 0; if (cen_ab < 0) cen_ab = 0;
#ifdef _WIN32
        cen_base = (uintptr_t)GetModuleHandleA(NULL);
#endif
        ps2_log("vk-census: on (PS2_VK_CENSUS): a window every %d s, every %d. frame counted%s; exe base 0x%llX", cen_secs,
                cen_every, cen_ab ? " in odd blocks only (PS2_VK_CENSUS_AB)" : "", (unsigned long long)cen_base);
    }
    return cen_cfg;
}
void ps2_vkcen_names(const char *const *names, unsigned n) { cen_pnames = names; cen_npnames = n; }

int ps2_vkcen_frame_begin(unsigned long long frame, unsigned part) {
    if (!ps2_vkcen_enabled()) return 0;
    if (cen_ab > 0) {
        int blk = (int)(frame / (unsigned long long)cen_ab);
        if (blk != cen_ab_blk) {
            extern void ps2_tprof_ev(u32 type, u32 a, u64 b, u64 c);
            cen_ab_blk = blk;
            ps2_tprof_ev(24, (u32)blk, (u64)(blk & 1), 0);
        }
        if (!(blk & 1)) return 0;
    }
    if (cen_every > 1 && frame % (unsigned long long)cen_every) return 0;
    memset(ps2_vkcen_cnt, 0, sizeof ps2_vkcen_cnt);
    nfev = 0; last_end = 0; had_end = 0; in_pass = 0; last_att = 0;
    memset(&last_area, 0, sizeof last_area);
    ps2_vkcen_part = part & (PS2_VKCEN_PARTS - 1u);
    ps2_vkcen_on = 1;
    return 1;
}
int ps2_vkcen_pause(void) { int w = ps2_vkcen_on; ps2_vkcen_on = 0; return w; }
void ps2_vkcen_resume(int was) { ps2_vkcen_on = was; }

static const char *base_of(const char *f) {
    const char *b = f;
    for (const char *s = f; *s; s++) if (*s == '/' || *s == '\\') b = s + 1;
    return b;
}
static const char *pname(unsigned p) {
    return p < cen_npnames && cen_pnames && cen_pnames[p] ? cen_pnames[p] : "?";
}
static void site_str(unsigned short s, char *out, size_t cap) {
    if (!s) { snprintf(out, cap, "(none)"); return; }
    snprintf(out, cap, "%s:%d %s", base_of(sites[s].file), sites[s].line, sites[s].func ? sites[s].func : "?");
}
static void flags_str(int what, unsigned f, char *out, size_t cap) {
    static const char *const fb[] = { "same", "first", "area", "mv", "clear" };
    static const char *const fr[] = { "layout", "all", "region", "mem", "buf", "inpass" };
    int o = 0; out[0] = 0;
    if (what == VKCE_BEGIN) { for (int k = 0; k < 5; k++) if (f >> k & 1u) o += snprintf(out + o, cap - (size_t)o, "%s%s", o ? "," : "", fb[k]); }
    else for (int k = 0; k < 6; k++) if (f >> k & 1u) o += snprintf(out + o, cap - (size_t)o, "%s%s", o ? "," : "", fr[k]);
    if (!o) snprintf(out, cap, "-");
}
static int wev_cmp(const void *a, const void *b) {
    const wev_t *x = *(const wev_t *const *)a, *y = *(const wev_t *const *)b;
    double nx = x->n[0] + x->n[1], ny = y->n[0] + y->n[1];
    return nx < ny ? 1 : nx > ny ? -1 : 0;
}
static void win_log(double dt) {
    static const char *const cn[VKC_N] = { "begin", "end", "barriers", "images", "pipelines", "descsets", "push", "draws",
                                           "dispatch", "copies", "clears", "dyn" };
    double fg = win_frames[0] > 0 ? win_frames[0] : 1.0, fi = win_frames[1] > 0 ? win_frames[1] : 1.0;
    ps2_log("vk-census: window %.1f s, frames game %.0f in-between %.0f other %.0f (values per counted frame: game / "
            "in-between); exe base 0x%llX; %u sites, %u events lost", dt, win_frames[0], win_frames[1], win_frames[2],
            (unsigned long long)cen_base, nsites - 1u, fev_lost + wev_lost);
    double tot[2][VKC_N]; memset(tot, 0, sizeof tot);
    for (unsigned p = 0; p < PS2_VKCEN_PARTS; p++) {
        double any = 0.0;
        for (int c = 0; c < VKC_N; c++) any += win_cnt[0][p][c] + win_cnt[1][p][c];
        if (any <= 0.0) continue;
        char l[700]; int o = 0;
        for (int c = 0; c < VKC_N; c++) {
            tot[0][c] += win_cnt[0][p][c]; tot[1][c] += win_cnt[1][p][c];
            o += snprintf(l + o, sizeof l - (size_t)o, " %s %.2f/%.2f", cn[c], win_cnt[0][p][c] / fg, win_cnt[1][p][c] / fi);
        }
        ps2_log("vk-census part \"%s\":%s", pname(p), l);
    }
    {   char l[700]; int o = 0;
        for (int c = 0; c < VKC_N; c++) o += snprintf(l + o, sizeof l - (size_t)o, " %s %.2f/%.2f", cn[c], tot[0][c] / fg, tot[1][c] / fi);
        ps2_log("vk-census total:%s", l);
    }
    static wev_t *ord[WEV_N];
    unsigned n = 0;
    for (unsigned k = 0; k < WEV_N; k++) if (wev[k].used && (wev[k].n[0] + wev[k].n[1]) > 0.0) ord[n++] = &wev[k];
    qsort(ord, n, sizeof ord[0], wev_cmp);
    static const char *const wn[VKCE_N] = { "pass", "end", "barrier", "copy", "clear" };
    int shown[VKCE_N] = {0};
    static const int lim[VKCE_N] = { 80, 0, 80, 40, 30 };
    for (unsigned k = 0; k < n; k++) {
        const wev_t *e = ord[k];
        if (shown[e->what] >= lim[e->what]) continue;
        if ((e->n[0] / fg + e->n[1] / fi) < 0.02) continue;
        shown[e->what]++;
        char s1[160], s2[160], fl[64];
        site_str(e->site, s1, sizeof s1);
        flags_str(e->what, e->flags, fl, sizeof fl);
        if (e->what == VKCE_BEGIN) {
            site_str(e->prev, s2, sizeof s2);
            ps2_log("vk-census pass: %.2f/%.2f begin %s <- end %s | part \"%s\" | %s | ret 0x%X", e->n[0] / fg, e->n[1] / fi,
                    s1, s2, pname(e->part), fl, e->ret);
        } else
            ps2_log("vk-census %s: %.2f/%.2f (%s %.1f/%.1f) %s | part \"%s\" | %s | ret 0x%X", wn[e->what], e->n[0] / fg,
                    e->n[1] / fi, e->what == VKCE_BARRIER ? "images" : "regions", e->sum[0] / fg, e->sum[1] / fi, s1,
                    pname(e->part), fl, e->ret);
    }
    memset(wev, 0, sizeof wev); wev_lost = 0; fev_lost = 0;
    memset(win_cnt, 0, sizeof win_cnt); memset(win_frames, 0, sizeof win_frames);
}

void ps2_vkcen_frame_end(unsigned long long frame, int kind) {
    if (!ps2_vkcen_on) return;
    ps2_vkcen_on = 0;
    if (kind < 0 || kind > 2) kind = 2;
    unsigned tot[8] = {0};
    for (unsigned p = 0; p < PS2_VKCEN_PARTS; p++) {
        const unsigned *c = ps2_vkcen_cnt[p];
        for (int k = 0; k < VKC_N; k++) win_cnt[kind][p][k] += (double)c[k];
        tot[0] += c[VKC_BEGIN]; tot[1] += c[VKC_BARC]; tot[2] += c[VKC_BARI]; tot[3] += c[VKC_PIPE];
        tot[4] += c[VKC_DESC]; tot[5] += c[VKC_DYN]; tot[6] += c[VKC_DRAW]; tot[7] += c[VKC_COPY];
    }
    win_frames[kind] += 1.0;
    ring[frame % 32u].frame = frame + 1u;
    memcpy(ring[frame % 32u].tot, tot, sizeof tot);
    if (kind < 2)
        for (unsigned k = 0; k < nfev; k++) {
            const fev_t *e = &fev[k];
            if (e->what == VKCE_END) continue;
            unsigned h = ((unsigned)e->site * 2654435761u) ^ ((unsigned)e->prev * 40503u) ^ ((unsigned)e->what << 24)
                       ^ ((unsigned)e->part << 16) ^ ((unsigned)e->flags << 8) ^ (e->ret * 2246822519u);
            unsigned j = h & (WEV_N - 1u), probe = 0;
            for (; probe < 64u; probe++, j = (j + 1u) & (WEV_N - 1u)) {
                wev_t *w = &wev[j];
                if (!w->used) { w->used = 1; w->site = e->site; w->prev = e->prev; w->what = e->what; w->part = e->part;
                                w->flags = e->flags; w->ret = e->ret; }
                if (w->site == e->site && w->prev == e->prev && w->what == e->what && w->part == e->part
                    && w->flags == e->flags && w->ret == e->ret) { w->n[kind] += 1.0; w->sum[kind] += (double)e->n; break; }
            }
            if (probe == 64u) wev_lost++;
        }
    double now = mono_s();
    if (win_t0 <= 0.0) win_t0 = now;
    if (now - win_t0 >= (double)cen_secs) { win_log(now - win_t0); win_t0 = now; }
}
int ps2_vkcen_totals(unsigned long long frame, unsigned out[8]) {
    if (ring[frame % 32u].frame != frame + 1u) return 0;
    memcpy(out, ring[frame % 32u].tot, sizeof ring[0].tot);
    return 1;
}
