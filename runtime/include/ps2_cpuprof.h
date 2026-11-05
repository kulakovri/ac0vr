#ifndef PS2_CPUPROF_H
#define PS2_CPUPROF_H
/* PS2_CPU_PROF (diagnostic, ov_cpu 2026-10-25): named CPU zones on the host threads -- where each thread's time goes per
   game field, and its spikes. A zone is a stretch of one thread's work (the renderer's xrWaitFrame, its recording, the
   EE's trails sampler hook, the audio callback's game mix, ...); each instance's duration (TSC) is added to the zone's
   5 s window: calls, total, max, a log2 histogram (p50 / p99) and the instances over PS2_CPU_PROF_SPIKE_US. With
   PS2_SPEED_LOG the window is logged as `cpu:` lines (ps2_gfxq.c, next to the speed lines); with PS2_THREAD_PROF the
   instances of at least PS2_CPU_PROF_TL_US also go into the pipeline timeline as events 20 (a = zone, b = start ns,
   c = duration ns) and the zone names as events 21 lines `#zone,id,name` (tools/perf/cpu_timeline.py).
   Off (unset or 0): one predictable branch per zone.
   Every guest hook (ps2_hook.c) is a zone of its own too ("ee hook OWNER before/after/replace ADDR"), so the EE side of
   our effects (trails sampler, props, syssnd, gaze, the VR camera, ...) shows up without a zone of its own. */
#include <stdint.h>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#include <x86intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* renderer thread (ps2_video.c) */
    CZ_VK_PUMP,          /* SDL events, pad, rumble, Touch sync (pump_events) */
    CZ_VK_TOUCH,         /*   of it: ps2_touch_update (xrSyncActions, the virtual pad) */
    CZ_VK_FENCE,         /* the frame slot's fence + GPU time collection */
    CZ_VK_PREFILL,       /* PS2_PERF_PREFILL (before the wait) */
    CZ_VK_PREUPLOAD,     /* command buffer begin + texture uploads before the wait (PS2_PERF_PREUPLOAD) */
    CZ_VK_XRWAIT,        /* ps2_xr_frame_begin: xrWaitFrame + xrBeginFrame (display pacing) */
    CZ_VK_RECORD,        /* recording after the wait, to vkEndCommandBuffer */
    CZ_VK_LIST,          /*   of it: render_list (the game frame's draws) */
    CZ_VK_EARLY,         /* a game frame recorded before the wait (PS2_PERF_RECORD_EARLY) */
    CZ_VK_SUBMIT,        /* final vkQueueSubmit + xrEndFrame */
    CZ_VK_PRESENT,       /* window vkQueuePresentKHR */
    CZ_VK_PIPEWARM,      /* pipeline warm-up steps */
    CZ_VK_PIPESAVE,      /* pipeline cache saves */
    CZ_VK_IDLE,          /* waiting for a list / the next attempt (list_cv) */
    CZ_VK_FRAME,         /* one whole draw_and_present */
    /* graphics worker (ps2_gfxq.c): its busy time per field, and the present (display list hand-off) */
    CZ_GFX_FIELD,
    CZ_GFX_PRESENT,
    CZ_GFX2_FIELD,       /* stage 2 of PS2_GFX_SPLIT */
    /* EE */
    CZ_EE_FIELD,         /* vblank -> the EE's first idle poll (its work per field) */
    /* audio callback (ps2_audio.c) */
    CZ_AUD_CB,           /* one whole callback */
    CZ_AUD_GAME,         /*   the game's mix (SPU2 + NUSOUND streams) */
    CZ_AUD_IMM,          /*   immersion layer (wind, debris, G, cracks) */
    CZ_AUD_SYS,          /*   cockpit system sounds (zero_syssnd.c) */
    /* other workers */
    CZ_CRK_WORKER,       /* canopy cracks: a crack pattern built (ps2_cracks.c worker) */
    CZ_N_STATIC,
    CZ_MAX = 512         /* + dynamic zones (guest hooks) */
};

extern int ps2_cz_on;
void ps2_cz_add(int zone, uint64_t t0_tsc, uint64_t t1_tsc);
int  ps2_cz_dynamic(const char *name);      /* a new zone (-1 when full / off) */
void ps2_cz_init(void);

static inline uint64_t ps2_cz_t(void) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    return ps2_cz_on ? (uint64_t)__rdtsc() : 0;
#else
    return 0;
#endif
}
static inline void ps2_cz_end(int zone, uint64_t t0) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    if (t0 && zone >= 0) ps2_cz_add(zone, t0, (uint64_t)__rdtsc());
#else
    (void)zone; (void)t0;
#endif
}

#ifdef __cplusplus
}
#endif

#endif
