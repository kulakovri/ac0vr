// ps2_xr.c - OpenXR output for the recompiled runtime's Vulkan renderer.
//
// PS2_XR=1 enables it. Uses XR_KHR_vulkan_enable (v1): the renderer still creates its own VkInstance/VkDevice,
// adding the extensions OpenXR asks for and using the physical device OpenXR picks. Everything except
// ps2_xr_game_frame runs on the renderer thread, which owns the only VkQueue. Without a runtime or headset the
// renderer continues without XR.
//
// Modes (PS2_XR_MODE):
//   cinema  the game image goes onto a large screen fixed in the world in front of the player (default): the
//           display region of the render target at internal resolution (ps2_video.c scr_pick_rt; the window image
//           with PS2_XR_SCREEN_FROM_RT=0, an overlay composite, a blank or double-height display, the F4 menu).
//   mono    head tracked: a game hook (zero_vr.c) renders every frame from the head pose with a field of view
//           matching the headset; both eyes see that image in a projection layer (no stereo depth yet).
//   stereo  synchronous stereo: the game renders the centre pose once per frame and the renderer draws that frame
//           twice, shifting every world vertex horizontally by its exact parallax for each eye (computed from
//           the GS depth value and the game's projection); both eyes update every frame.
//   stereo-alt  game frames alternate between the left and right eye poses (each eye at half rate).
// Frames without a head-tracked camera (menus, movies, briefings) fall back to the cinema screen.
//
// Env:
//   PS2_XR=1                 enable
//   PS2_XR_MODE=cinema|mono|stereo
//   PS2_XR_RUNTIME=<json>    use this OpenXR runtime manifest (sets XR_RUNTIME_JSON)
//   PS2_XR_SCREEN_W=3.0      cinema screen width in metres
//   PS2_XR_SCREEN_DIST=2.5   cinema screen distance in metres
//   PS2_XR_SCREEN_FROM_RT=1  cinema screen from the render target (0: from the window image, as before 2026-10-24)
//   PS2_XR_TAG_DELAY=0       game frames between a camera update and the draws it belongs to (eye calibration)
// Keys: F9 recentres (screen placement and head reference); so does holding the gamepad's Back/Select ~1 s.
#include "ps2_runtime.h"
#include "ps2_xr.h"
#include "ps2_hle.h"
#include "ps2_quit.h"
#include "ps2_touch.h"
#include <vulkan/vulkan.h>
#include "ps2_vkcen.h"          /* (PS2_VK_CENSUS: vkCmd* calls counted) */
#include <stdio.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <pthread.h>
#include <windows.h>

void ps2_gfxq_tag(u32 kind, u32 a, u32 b);

/* Touch controllers (tc_touch; the block above poll_events): ACZ_TOUCH set = an action set; Touch Plus profile when
   the runtime offers its extension */
#define XT_PLUS_EXT "XR_META_touch_controller_plus"
static int xt_want, xt_plus_ext;
static unsigned xt_ref_gen;            /* +1 at every recentre or LOCAL space change (grabs rebase, ps2_touch.c) */
static void xt_profiles(void);
static void xt_create(void);

static int xr_on = -1;                 // -1 unknown, 0 off, 1 on
static int xmode = PS2_XR_CINEMA;
static XrInstance xi = XR_NULL_HANDLE;
static XrSystemId xsys = XR_NULL_SYSTEM_ID;
static XrSession xs = XR_NULL_HANDLE;
static XrSpace xlocal = XR_NULL_HANDLE, xview = XR_NULL_HANDLE;
static XrSessionState xstate = XR_SESSION_STATE_UNKNOWN;
static int xrunning, xrecenter = 1;
/* Recentre after the headset is put on again (session READY) or the runtime moved the LOCAL space: wait this many
   focused frames first -- at READY the headset is still being put on, and recentring then took a skewed reference
   (yaw 30 deg and the head off its seat in the user's test) */
static int xrecenter_wait;
static XrFrameState xfs;
static int xframe_open;                // xrBeginFrame done, xrEndFrame pending
/* PS2_PERF_RECORD_EARLY (ps2_video.c): a game frame recorded before its xrWaitFrame. Its content needs nothing from the
   wait (the poses come from the frame's tag), so the renderer asks ps2_xr_frame_predict() whether the frame will go to
   the headset -- the session runs in stereo and the last xrWaitFrame said shouldRender -- and ps2_xr_sync_frame answers
   for that predicted frame until ps2_xr_frame_begin (the wait, the real answer). Everything that needs the open frame
   (swapchain images, the display time, xrEndFrame) still comes after xrWaitFrame + xrBeginFrame. */
static int xpredict, xlast_should;
/* how regular the runtime's display times are (PS2_PERF_RECORD_EARLY=2 predicts an in-between frame's display time as
   the last one + the period): waits, and those whose predicted display time moved by more or less than one period
   (+-0.5 ms) since the previous wait, with the sum of |error|; read by ps2_xr_display_step_stats (PS2_REC_PROF) */
static XrTime xprev_pdt; static u64 xpdt_n, xpdt_off; static double xpdt_err_ms;
/* PS2_PERF_RECORD_EARLY=2 (P3a): the display time the next xrWaitFrame is expected to return (the last one + the
   period; 0 before the first frame) */
long long ps2_xr_predict_display_time(void) {
    return xprev_pdt && xfs.predictedDisplayPeriod > 0 ? (long long)(xprev_pdt + xfs.predictedDisplayPeriod) : 0;
}
void ps2_xr_display_step_stats(u64 *n, u64 *off, double *err_ms) {
    *n = xpdt_n; *off = xpdt_off; *err_ms = xpdt_err_ms;
    xpdt_n = xpdt_off = 0; xpdt_err_ms = 0.0;
}
/* Pacing diagnostics (fx_pacing, 2026-10-23; ps2_video.c's xr-pace lines and PS2_XR_PACE_CSV). Each xrWaitFrame: when
   it was called and returned (QueryPerformanceCounter), the predicted display time, its step from the previous one in
   display periods (signed: 1 = the next refresh; 2 = one refresh without a new frame from us; 0 = the same refresh
   again), and the lead = display time - wait return (needs XR_KHR_win32_convert_performance_counter_time; enabled when
   the runtime offers it). XR_META_performance_metrics (the Meta PC runtime offers it): the runtime's own counters --
   app CPU / GPU frame time, motion-to-photon latency, compositor CPU / GPU time, compositor dropped frames (cumulative),
   spacewarp mode -- read by ps2_xr_perf_metrics. PS2_XR_PACE_EXT=0 enables neither extension. */
#define XPACE_CONV_EXT "XR_KHR_win32_convert_performance_counter_time"   /* (openxr_platform.h declares it only under
                                                                            XR_USE_PLATFORM_WIN32, like the audio GUID) */
typedef XrResult (XRAPI_PTR *xr_pfn_time_to_qpc)(XrInstance instance, XrTime time, LARGE_INTEGER *pc);
typedef XrResult (XRAPI_PTR *xr_pfn_qpc_to_time)(XrInstance instance, const LARGE_INTEGER *pc, XrTime *time);
static int xpace_conv, xpm_on;
static xr_pfn_time_to_qpc p_xr_to_qpc;
static xr_pfn_qpc_to_time p_qpc_to_xr;     /* (the Touch input locates the controllers at the poll's own time) */
static PFN_xrSetPerformanceMetricsStateMETA p_pm_set;
static PFN_xrQueryPerformanceMetricsCounterMETA p_pm_query;
#define XPM_N 7
static const char *const xpm_names[XPM_N] = {
    "/perfmetrics_meta/app/cpu_frametime", "/perfmetrics_meta/app/gpu_frametime",
    "/perfmetrics_meta/app/motion_to_photon_latency", "/perfmetrics_meta/compositor/cpu_frametime",
    "/perfmetrics_meta/compositor/gpu_frametime", "/perfmetrics_meta/compositor/dropped_frame_count",
    "/perfmetrics_meta/compositor/spacewarp_mode" };
static XrPath xpm_path[XPM_N];
static int xpm_ready;                    /* the metrics state was enabled on the session and the paths resolved */
static struct { u64 n; long long call_ns, ret_ns, pdt, period; int steps, should, lead_ok; double lead_ms; } xwait;
static long long qpc_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (long long)((double)c.QuadPart * (1e9 / (double)f.QuadPart));
}
/* ---- The display's refresh rate against the game's 60 Hz (perf6_cadence, 2026-11-03) ----
   Every xrWaitFrame's predictedDisplayPeriod classifies the display against the game's field clock (PS2_FIELD_HZ;
   60 in VR): "even" when a whole number of refreshes falls on a field within 1 % (60 Hz 1:1, 120 Hz 2:1: every game
   frame lands on its own refresh), "uneven" otherwise (72 Hz 6:5, 80 Hz 4:3, 90 Hz 3:2, 96 / 100 / 144 Hz). A new
   period counts after 8 waits in a row at it (at once for the session's first one and after the runtime's
   DISPLAY_REFRESH_RATE_CHANGED_FB event); one `xr: display` line per change. ps2_video.c's PS2_XR_SMOOTH_CLOCK=auto
   (the default) turns the smooth clock on for an uneven display (ps2_xr_cadence).
   XR_FB_display_refresh_rate (enabled when the runtime offers it; PS2_XR_REFRESH_EXT=0 never): the session logs the
   rates the runtime offers and the one it runs at, and the change event. PS2_XR_REFRESH_HZ=R asks the runtime for R Hz
   once the session runs (xrRequestDisplayRefreshRateFB; its result and the rate 2 s later are logged); off by default:
   the rate is the Meta Quest Link / Virtual Desktop app's setting (VDXR's request is a verified no-op, Link's
   unverified), and nothing here relies on the request: the display period decides. */
#define XRR_EXT "XR_FB_display_refresh_rate"
static int xrr_ext, xrr_req_done, xrr_chk;
static float xrr_req_hz = -1.0f;
static PFN_xrEnumerateDisplayRefreshRatesFB p_rr_enum;
static PFN_xrGetDisplayRefreshRateFB p_rr_get;
static PFN_xrRequestDisplayRefreshRateFB p_rr_req;
static int xcad_state, xcad_n, xcad_fast;    /* xcad_state: 0 unknown (before the first wait), 1 even, 2 uneven */
static long long xcad_period, xcad_cand;
static double xcad_field_ns;
/* the class of a display period against a field period: 0 unknown, 1 even (k refreshes a field, within 1 %), 2 uneven */
int ps2_xr_cadence_class(long long period_ns, double field_ns) {
    if (period_ns < 1000000 || period_ns > 100000000 || field_ns <= 0.0) return 0;
    double r = field_ns / (double)period_ns, k = floor(r + 0.5);
    return k >= 1.0 && fabs(r - k) < 0.01 * k ? 1 : 2;
}
/* renderer thread: the settled class (0 before the first wait) and the display rate it stands for */
int ps2_xr_cadence(double *hz) {
    if (hz) *hz = xcad_period > 0 ? 1e9 / (double)xcad_period : 0.0;
    return xcad_state;
}
static void xcad_wait(long long period) {                    /* (each xrWaitFrame) */
    int fast = xcad_fast > 0;                                /* (a refresh-rate event in the last ~240 waits) */
    if (xcad_fast > 0) xcad_fast--;
    if (period < 1000000 || period > 100000000) return;
    if (xcad_period && llabs(period - xcad_period) * 100 < xcad_period) { xcad_n = 0; return; }   /* (the same rate) */
    if (!xcad_cand || llabs(period - xcad_cand) * 100 >= xcad_cand) { xcad_cand = period; xcad_n = 0; }
    if (xcad_period && ++xcad_n < 8 && !fast) return;
    if (xcad_field_ns <= 0.0) {
        const char *e = getenv("PS2_FIELD_HZ");
        double hz = e && *e ? atof(e) : 0.0;
        xcad_field_ns = hz > 1.0 && hz < 10000.0 ? 1e9 / hz : 16683333.0;
    }
    long long from = xcad_period;
    xcad_period = period; xcad_cand = 0; xcad_n = 0; xcad_fast = 0;
    xcad_state = ps2_xr_cadence_class(period, xcad_field_ns);
    char was[48] = "";
    if (from) snprintf(was, sizeof was, "; was %.2f Hz", 1e9 / (double)from);
    ps2_log("xr: display %.2f Hz (period %.3f ms%s): %.3f refreshes per game field: %s", 1e9 / (double)period,
            (double)period / 1e6, was, xcad_field_ns / (double)period,
            xcad_state == 1 ? "even (every game frame on its own refresh)"
                            : "uneven (game frames fall between refreshes: PS2_XR_SMOOTH_CLOCK=auto re-times every frame)");
}
/* after xrCreateSession: the rates the runtime offers and the one it runs at (XR_FB_display_refresh_rate) */
static void xrr_session(void) {
    if (!xrr_ext) return;
    if (XR_FAILED(xrGetInstanceProcAddr(xi, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&p_rr_enum))
        || XR_FAILED(xrGetInstanceProcAddr(xi, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *)&p_rr_get))
        || XR_FAILED(xrGetInstanceProcAddr(xi, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&p_rr_req))
        || !p_rr_enum || !p_rr_get || !p_rr_req) {
        ps2_log("xr: %s: entry points missing", XRR_EXT);
        xrr_ext = 0; p_rr_enum = NULL; p_rr_get = NULL; p_rr_req = NULL;
        return;
    }
    float rates[32]; uint32_t n = 0;
    char list[256] = "", rs[XR_MAX_RESULT_STRING_SIZE] = "";
    XrResult r = p_rr_enum(xs, 0, &n, NULL);
    if (XR_SUCCEEDED(r) && n) {
        if (n > 32u) n = 32u;
        r = p_rr_enum(xs, n, &n, rates);
    }
    if (XR_SUCCEEDED(r))
        for (uint32_t i = 0; i < n && i < 32u; i++) {
            size_t l = strlen(list);
            snprintf(list + l, sizeof list - l, "%s%.2f", i ? " / " : "", (double)rates[i]);
        }
    else { xrResultToString(xi, r, rs); snprintf(list, sizeof list, "? (%s)", rs); }
    float now = 0.0f;
    char gs[XR_MAX_RESULT_STRING_SIZE] = "";
    XrResult g = p_rr_get(xs, &now);
    if (XR_FAILED(g)) xrResultToString(xi, g, gs);
    ps2_log("xr: %s: the runtime offers %s Hz; it runs at %.2f Hz%s%s", XRR_EXT, n || XR_FAILED(r) ? list : "no",
            (double)now, *gs ? " " : "", gs);
    if (XR_SUCCEEDED(g)) { extern void ps2_stats_set_refresh(double hz); ps2_stats_set_refresh((double)now); }
}
/* PS2_XR_REFRESH_HZ=R (once the session runs): xrRequestDisplayRefreshRateFB(R), logged; 2 s later the rate it runs at */
static void xrr_request(void) {
    if (xrr_req_hz < 0.0f) { const char *e = getenv("PS2_XR_REFRESH_HZ"); xrr_req_hz = e && *e ? (float)atof(e) : 0.0f; }
    if (xrr_req_done || xrr_req_hz <= 0.0f) return;
    xrr_req_done = 1;
    if (!xrr_ext || !p_rr_req) {
        ps2_log("xr: PS2_XR_REFRESH_HZ=%.2f: the runtime does not offer %s: the rate is the headset app's setting",
                (double)xrr_req_hz, XRR_EXT);
        return;
    }
    char rs[XR_MAX_RESULT_STRING_SIZE] = "";
    XrResult r = p_rr_req(xs, xrr_req_hz);
    xrResultToString(xi, r, rs);
    ps2_log("xr: PS2_XR_REFRESH_HZ: xrRequestDisplayRefreshRateFB(%.2f) -> %s (%d)", (double)xrr_req_hz, rs, (int)r);
    xrr_chk = 240;                                          /* (~2 s of waits) */
}
static void xrr_check(void) {                               /* (each xrWaitFrame while xrr_chk counts down) */
    if (!xrr_chk || --xrr_chk) return;
    float now = 0.0f; char rs[XR_MAX_RESULT_STRING_SIZE] = "";
    XrResult g = p_rr_get ? p_rr_get(xs, &now) : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(g)) xrResultToString(xi, g, rs);
    double hz; ps2_xr_cadence(&hz);
    ps2_log("xr: PS2_XR_REFRESH_HZ=%.2f: 2 s later the runtime reports %.2f Hz%s%s; the display period says %.2f Hz",
            (double)xrr_req_hz, (double)now, *rs ? " " : "", rs, hz);
}
/* PS2_XR_TEST_HZ=R[,S] [test]: the OpenXR Simulator paces at a fixed 90 Hz; this emulates a slower display at R Hz
   (20..89, e.g. 72 or 80) from S seconds (default 0) after the first wait: after each xrWaitFrame the renderer waits
   for its own R Hz grid (the display time minus the runtime's lead at the start), and the frame state reports the
   grid's display time and period (xrEndFrame keeps the runtime's own display time). A grid slot whose wake time has
   passed when the wait returns is skipped (display step 2): `app` when the renderer called the wait after that wake
   time (a real late frame), `runtime` when the runtime's wait itself returned after it (an emulation artefact). Needs
   XR_KHR_win32_convert_performance_counter_time; a line every 10 s. */
static double xtest_hz = -1.0, xtest_from_s;
static long long xtest_grid, xtest_lead, xtest_t0, xtest_line;
static XrTime xtest_real_pdt;                 /* the runtime's own display time of an emulated frame (0: none) */
static u64 xtest_n, xtest_late_app, xtest_late_rt;
static double xtest_sleep_ms;
static void xtest_rate(long long w_call, long long *w_ret) {
    xtest_real_pdt = 0;
    if (xtest_hz < 0.0) {
        const char *e = getenv("PS2_XR_TEST_HZ");
        xtest_hz = e && *e ? atof(e) : 0.0;
        const char *c = e ? strchr(e, ',') : NULL;
        xtest_from_s = c ? atof(c + 1) : 0.0;
        if (xtest_hz != 0.0 && (xtest_hz < 20.0 || xtest_hz >= 89.5 || !p_xr_to_qpc)) {
            ps2_log("xr: PS2_XR_TEST_HZ=%s ignored (20..89 Hz, needs %s)", e, XPACE_CONV_EXT);
            xtest_hz = 0.0;
        } else if (xtest_hz > 0.0)
            ps2_log("xr: TEST: an emulated %.2f Hz display from %.1f s (PS2_XR_TEST_HZ)", xtest_hz, xtest_from_s);
    }
    if (xtest_hz <= 0.0 || xfs.predictedDisplayPeriod <= 0) return;
    LARGE_INTEGER pc, f;
    if (XR_FAILED(p_xr_to_qpc(xi, xfs.predictedDisplayTime, &pc)) || !QueryPerformanceFrequency(&f)) return;
    long long pdt_q = (long long)((double)pc.QuadPart * (1e9 / (double)f.QuadPart));   /* (the runtime's, host ns) */
    if (!xtest_t0) xtest_t0 = *w_ret;
    if ((double)(*w_ret - xtest_t0) < xtest_from_s * 1e9) return;
    long long P = (long long)(1e9 / xtest_hz + 0.5);
    if (!xtest_grid) {
        xtest_lead = pdt_q - *w_ret; xtest_grid = pdt_q; xtest_line = *w_ret;
        ps2_log("xr: TEST: emulated %.2f Hz display starts (lead %.2f ms, the runtime's period %.3f ms)", xtest_hz,
                (double)xtest_lead / 1e6, (double)xfs.predictedDisplayPeriod / 1e6);
    } else {
        xtest_grid += P;
        while (xtest_grid - xtest_lead < *w_ret) {
            if (w_call > xtest_grid - xtest_lead) xtest_late_app++; else xtest_late_rt++;
            xtest_grid += P;
        }
    }
    long long wake = xtest_grid - xtest_lead, t;
    while ((t = qpc_ns()) < wake) { if (wake - t > 2000000) Sleep(1); else YieldProcessor(); }
    xtest_sleep_ms += (double)(t - *w_ret) / 1e6; xtest_n++;
    *w_ret = t;
    xtest_real_pdt = xfs.predictedDisplayTime;
    xfs.predictedDisplayTime += (XrTime)(xtest_grid - pdt_q);       /* (XrTime and host ns tick alike) */
    xfs.predictedDisplayPeriod = (XrDuration)P;
    if (t - xtest_line >= 10000000000ll) {
        xtest_line = t;
        ps2_log("xr: TEST: emulated %.2f Hz: %llu frames, skipped slots %llu app / %llu runtime, mean sleep %.2f ms",
                xtest_hz, (unsigned long long)xtest_n, (unsigned long long)xtest_late_app,
                (unsigned long long)xtest_late_rt, xtest_n ? xtest_sleep_ms / (double)xtest_n : 0.0);
    }
}
/* the last xrWaitFrame (renderer thread); returns its number (0 before the first) */
u64 ps2_xr_wait_info(long long *call_ns, long long *ret_ns, long long *pdt, long long *period, int *steps,
                     double *lead_ms, int *lead_ok) {
    *call_ns = xwait.call_ns; *ret_ns = xwait.ret_ns; *pdt = xwait.pdt; *period = xwait.period; *steps = xwait.steps;
    *lead_ms = xwait.lead_ms; *lead_ok = xwait.lead_ok;
    return xwait.n;
}
/* the runtime's frame counters (XR_META_performance_metrics): v[0..6] = app CPU ms, app GPU ms, motion-to-photon ms,
   compositor CPU ms, compositor GPU ms, compositor dropped frames (cumulative), spacewarp mode; returns a bit mask of
   the valid values (0 = not available) */
unsigned ps2_xr_perf_metrics(double v[XPM_N]) {
    unsigned ok = 0;
    if (!xpm_on || !xs) return 0;
    if (!xpm_ready) {
        XrPerformanceMetricsStateMETA st; memset(&st, 0, sizeof st);
        st.type = XR_TYPE_PERFORMANCE_METRICS_STATE_META; st.enabled = XR_TRUE;
        XrResult r = p_pm_set(xs, &st);
        for (int k = 0; k < XPM_N && XR_SUCCEEDED(r); k++) r = xrStringToPath(xi, xpm_names[k], &xpm_path[k]);
        if (XR_FAILED(r)) { ps2_log("xr: XR_META_performance_metrics failed (%d)", (int)r); xpm_on = 0; return 0; }
        xpm_ready = 1;
    }
    for (int k = 0; k < XPM_N; k++) {
        XrPerformanceMetricsCounterMETA c; memset(&c, 0, sizeof c);
        c.type = XR_TYPE_PERFORMANCE_METRICS_COUNTER_META;
        if (!xpm_path[k] || XR_FAILED(p_pm_query(xs, xpm_path[k], &c))) continue;
        if (c.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META) v[k] = (double)c.uintValue;
        else if (c.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) v[k] = (double)c.floatValue;
        else continue;
        ok |= 1u << k;
    }
    return ok;
}
static XrPosef xquad = { {0, 0, 0, 1}, {0, 0, -2.5f} };
static float xscreen_w = 3.0f, xscreen_d = 2.5f;
static char *xinst_ext_buf, *xdev_ext_buf;
static const char *xinst_exts[32], *xdev_exts[32];
static VkInstance vk_inst; static VkPhysicalDevice vk_phys; static VkDevice vk_dev; static uint32_t vk_qfam;
static u64 xsubmitted, xsub_proj, xsub_quad;
static int xr_runtime_loaded;          /* an OpenXR instance was created: the runtime DLL is in the process */
int ps2_xr_runtime_loaded(void) { return xr_runtime_loaded; }

// Swapchains: 0 = cinema screen from the render target (the default source of screen frames since 2026-10-24: 2560x1792
// for a 640x448 display at 4x, the chain 2560x2048), 1 = left eye, 2 = right eye (mono uses 1 for both eyes),
// 3 = cinema screen from the window image (different channel order; separate so sources do not recreate chains).
typedef struct {
    XrSwapchain sc; VkImage img[8]; uint32_t nimg, w, h; VkFormat fmt; int raw_copy;
    uint32_t cw, ch;            // size of the image last copied in (the layer's imageRect)
    VkFormat src_fmt;           // source format the chain was chosen for
    u64 retry_after;            // after a failed create, frames to wait before trying again
} xr_chain;
static u64 xr_frames_begun;
static xr_chain xc[4];
static int xcinema;                                 // chain the cinema screen showed last
static int xacq_mask; static uint32_t xacq_index;  // swapchains with an image acquired this frame (bit per chain)
static int xlayer;                                   // 0 none, 1 quad, 2 projection

// Head poses (renderer thread writes, EE thread reads).
static pthread_mutex_t xr_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { int valid; XrPosef eye[2]; XrFovf fov[2]; XrPosef ref; XrTime t; u32 n; } xr_latest;
/* ACZ_VR_HEADLOOK=stick: head rotation drives the game's own look (right stick, zero_vr.c) instead of the camera, and
   the image is shown head-locked (VIEW space), like a screen in front of the eyes with the HUD always ahead. */
static int xheadlook = -1;
static XrPosef xr_view_eye[2];             /* eye poses relative to the head (VIEW space), for head-locked layers */
static int xr_view_eye_ok;
/* Each eye's own (asymmetric) FOV as reported by the runtime. Quest 3: L(-54 +40 / +44 -55), R(-40 +54 / +44 -55). The
   game renders one symmetric image (+-max tangent); each eye is submitted as the crop of that image covering exactly
   its own frustum, with its own FOV. Mathematically the same mapping as submitting the whole image with the symmetric
   FOV, but it does not depend on the runtime honouring a FOV wider than the eye's (on the real Quest Link the HUD
   appeared doubled tens of degrees apart, as if each eye's image was stretched to its native asymmetric FOV).
   PS2_XR_EYE_CROP=0 submits the full symmetric image as before. */
static XrFovf xr_native_fov[2];
static int xr_native_fov_ok;
/* XR_KHR_visibility_mask (ov_passes, 2026-10-25): the runtime's per-eye hidden-area mesh (the lens corners inside the
   eye's FOV that the optics never show) and visible-area mesh. Enabled when the runtime offers it (the Meta PC runtime's
   DLL names it; the OpenXR Simulator does not), unless PS2_XR_VIS_MASK=0. Queried on the renderer thread after the
   session starts and again after XrEventDataVisibilityMaskChangedKHR. Vertices are 2D points of the view on its z = -1
   plane (x right, y up: tangents); a mesh whose bounds do not fit the eye's FOV (+25 %) is refused. The renderer
   (ps2_video.c pm_*) keeps our own passes out of the hidden part; nothing else uses it. */
#define XVM_MAXV 4096u
static int xvm_ext;                                  /* the extension is enabled on the instance */
static PFN_xrGetVisibilityMaskKHR p_xvm_get;
static int xvm_dirty[2] = { 1, 1 };
static uint32_t xvm_gen;                             /* bumps on every new mesh (0 = none yet) */
static struct { float *v[2]; uint32_t *ix[2]; uint32_t nv[2], ni[2]; } xvm[2];   /* [eye]; [0] hidden, [1] visible */
static pthread_mutex_t xvm_lock = PTHREAD_MUTEX_INITIALIZER;
int ps2_xr_headlook_stick(void) {
    if (xheadlook < 0) { const char *e = getenv("ACZ_VR_HEADLOOK"); xheadlook = e && !strcmp(e, "stick"); }
    return xheadlook;
}
static XrPosef xref = { {0, 0, 0, 1}, {0, 0, 0} };  // recentred reference (yaw only), renderer thread copy

// Poses the game rendered with, by sequence number (EE thread writes, renderer thread reads).
#define XR_TAGS 64
static struct {
    volatile u32 seq; int eye; XrPosef pose; float tan_h, tan_v;
    XrPosef eyep[2];                       // both eye poses at sampling time (synchronous stereo layer poses)
    int locked;                            // shown head-locked (stick head look outside the cockpit)
    float qrel[4];                         // head orientation relative to the recentred reference (rendered with)
    float prel[3], ipd;                    // head position relative to the reference (m), eye separation (m)
    volatile u32 shift_seq; float shift_a, shift_c, shift_mh;
    volatile u32 cam_seq; float cam_eye[3], cam_rot[9], cam_f[2], cam_c[2];   /* world camera the frame used */
    volatile u32 bcam_seq; float bcam_eye[3], bcam_rot[9];   /* the same camera without the head (the aircraft's) */
    volatile u32 cloud_seq[2]; float cloud_roll[2];   /* rolls the game's cloud positions / images were built with */
} xr_tags[XR_TAGS];

/* HUD anchored to the aircraft (GS thread, per HUD draw). In frames whose camera was turned by the head (world-locked
   frames: the cockpit, and every view in the plain stereo/mono modes) the game still draws its HUD at the screen
   centre, so it would follow the head. The HUD belongs where the camera's own forward direction (the aircraft's nose
   in the cockpit) appears in the head-turned view: tx, ty = tangents of that direction; c, s = cos/sin of the
   counter-roll. Returns 0 = draw as is, 1 = anchor, 2 = forward direction is behind the view (hide the HUD).
   ACZ_VR_HUD_ANCHOR=0 turns it off. */
int ps2_xr_hud_anchor(u32 seq, float M[9]) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("ACZ_VR_HUD_ANCHOR"); on = !(e && *e == '0'); }
    int slot = (int)(seq % XR_TAGS);
    if (!on || !seq || __atomic_load_n(&xr_tags[slot].seq, __ATOMIC_ACQUIRE) != seq || xr_tags[slot].locked) return 0;
    float x = xr_tags[slot].qrel[0], y = xr_tags[slot].qrel[1], z = xr_tags[slot].qrel[2], w = xr_tags[slot].qrel[3];
    /* H maps head-local vectors into the camera's base frame (zero_vr.c m3_quat); columns of H^T = rows of H */
    float H00 = 1 - 2 * (y * y + z * z), H01 = 2 * (x * y - z * w), H02 = 2 * (x * z + y * w);
    float H10 = 2 * (x * y + z * w), H11 = 1 - 2 * (x * x + z * z), H12 = 2 * (y * z - x * w);
    float H20 = 2 * (x * z - y * w), H21 = 2 * (y * z + x * w), H22 = 1 - 2 * (x * x + y * y);
    /* M = H^T: base-frame directions -> head-frame directions */
    M[0] = H00; M[1] = H10; M[2] = H20;
    M[3] = H01; M[4] = H11; M[5] = H21;
    M[6] = H02; M[7] = H12; M[8] = H22;
    if (-H22 > -0.1f) return 2;                        /* base forward (0,0,-1) is behind the head's view */
    return 1;
}

static u32 xr_seq_next = 1;
/* Wall time of the last game-frame tag. When the camera hook stops tagging (menus, briefings, pause), the last
   number would otherwise stay "recent" forever (the GS keeps marking draws with it), and menus would be shown
   head-tracked instead of on the cinema screen. A number is live only while tags keep coming. */
static volatile double xr_last_tag_s = -1.0;
static int xr_seq_live(u32 seq) {
    return seq && xr_seq_next < seq + 32u && xr_last_tag_s >= 0.0 && ps2_wall_seconds() - xr_last_tag_s < 0.25;
}
static u32 xr_fifo[16]; static u32 xr_fifo_n; static int xr_tag_delay;
static u64 xr_game_frames;

// What each eye shows (renderer thread).
static struct { int have; XrPosef pose; float tan_h, tan_v; u32 seq; int locked; } xr_eye[2];
static volatile int xr_next_locked = 1;
void ps2_xr_set_head_locked(int on) { xr_next_locked = on; }

static float envf(const char *n, float d) {
    const char *e = getenv(n); if (!e || !*e) return d;
    float v = 0, frac = 0, div = 1; int seen = 0, neg = 0; const char *p = e;
    if (*p == '-') { neg = 1; p++; }
    while (*p >= '0' && *p <= '9') { v = v * 10 + (float)(*p++ - '0'); seen = 1; }
    if (*p == '.' || *p == ',') { p++; while (*p >= '0' && *p <= '9') { frac = frac * 10 + (float)(*p++ - '0'); div *= 10; seen = 1; } }
    if (!seen) return d;
    v += frac / div; return neg ? -v : v;
}

static int split_exts(char *buf, const char **out, int max) {
    int n = 0; char *p = buf;
    while (p && *p && n < max) {
        while (*p == ' ') p++;
        if (!*p) break;
        out[n++] = p;
        char *sp = strchr(p, ' ');
        if (sp) { *sp = 0; p = sp + 1; } else p = NULL;
    }
    return n;
}

#define XRLOG(r, what) ps2_log("xr: %s failed (%d)", what, (int)(r))

// ---- quaternion helpers (x, y, z, w; rotate column vectors) ----
static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}
static XrQuaternionf qconj(XrQuaternionf a) { XrQuaternionf r = { -a.x, -a.y, -a.z, a.w }; return r; }
static XrVector3f qrot(XrQuaternionf q, XrVector3f v) {
    XrQuaternionf p = { v.x, v.y, v.z, 0 };
    XrQuaternionf r = qmul(qmul(q, p), qconj(q));
    XrVector3f o = { r.x, r.y, r.z };
    return o;
}

int ps2_xr_mode(void) { return xr_on == 1 ? xmode : -1; }
void ps2_xr_request_recenter(void) { xrecenter = 1; }   /* any thread; done at the next XR frame */
/* The flight-start recentre at the hand-over from the static screen to the head-tracked view (zero_vr.c
   z_recentre_hand_over; pt_flat 2026-10-29, ACZ_VR_RECENTRE_KEEP): within keep_deg of the current reference's yaw the
   head has not moved and the reference is kept (no shift of the cockpit against the world, the cinema screen stays);
   beyond it the reference is taken from the head as before, at the cut. The cinema screen then moves when the first
   head-tracked frame goes out (the last static frames still in flight keep it where it was). Any thread. */
static volatile int xrecenter_ho;
static float xho_keep = 10.0f;
static XrPosef xquad_next;
static int xquad_next_on, xquad_next_wait;
void ps2_xr_request_recenter_hand_over(float keep_deg) {
    xho_keep = keep_deg;
    __atomic_store_n(&xrecenter_ho, 1, __ATOMIC_RELEASE);
}
int ps2_xr_tans_of(u32 seq, float *th, float *tv) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || xr_tags[slot].seq != seq) return 0;
    *th = xr_tags[slot].tan_h; *tv = xr_tags[slot].tan_v;
    return 1;
}
int ps2_xr_is_tracked(u32 seq) {
    int slot = (int)(seq % XR_TAGS);
    return xr_on == 1 && xmode != PS2_XR_CINEMA && seq && xr_tags[slot].seq == seq && xr_seq_live(seq);
}
int ps2_xr_eye_of(u32 seq) {
    int slot = (int)(seq % XR_TAGS);
    return seq && xr_tags[slot].seq == seq ? xr_tags[slot].eye : -9;
}

/* The headset's audio output (XR_OCULUS_audio_device_guid, offered by the Meta runtime for Link): the Windows Core
   Audio endpoint id (IMMDevice::GetId form) of the headset's speakers, read once right after xrCreateInstance on the
   renderer thread, before ps2_audio_start opens the sound device (ps2_audio.c picks the device). Empty when the
   runtime does not offer the extension (the OpenXR Simulator) or PS2_AUDIO_DEVICE is set. The PFN and the size are
   those of openxr_platform.h, which declares them only under XR_USE_PLATFORM_WIN32. */
#define XR_AUDIO_GUID_EXT "XR_OCULUS_audio_device_guid"
typedef XrResult (XRAPI_PTR *xr_pfn_audio_guid)(XrInstance instance, wchar_t buffer[128]);
static wchar_t xr_audio_out_id[128];
const wchar_t *ps2_xr_audio_output_id(void) { return xr_audio_out_id[0] ? xr_audio_out_id : NULL; }
static int xr_has_instance_ext(const char *name) {
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL)) || !n || n > 512) return 0;
    XrExtensionProperties *p = (XrExtensionProperties *)calloc(n, sizeof *p);
    if (!p) return 0;
    for (uint32_t i = 0; i < n; i++) p[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    int found = 0;
    if (XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, n, &n, p)))
        for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) found = 1;
    free(p);
    return found;
}
static void xr_audio_device_query(int enabled) {
    if (!enabled) return;
    xr_pfn_audio_guid fn = NULL;
    if (XR_FAILED(xrGetInstanceProcAddr(xi, "xrGetAudioOutputDeviceGuidOculus", (PFN_xrVoidFunction *)&fn)) || !fn) return;
    wchar_t buf[128];
    memset(buf, 0, sizeof buf);
    XrResult r = fn(xi, buf);
    if (XR_FAILED(r) || !buf[0]) { XRLOG(r, "xrGetAudioOutputDeviceGuidOculus"); return; }
    buf[127] = 0;
    memcpy(xr_audio_out_id, buf, sizeof buf);
    char u[256];
    if (!WideCharToMultiByte(CP_UTF8, 0, buf, -1, u, (int)sizeof u, NULL, NULL)) strcpy(u, "?");
    ps2_log("xr: the runtime names the headset's audio output: %s", u);
}

/* ---- Frame depth to the headset (xr_depth, 2026-10-23; PS2_XR_DEPTH) ----
   XR_KHR_composition_layer_depth: each eye's projection view carries a depth image of the same pixels as its colour
   (ps2_video.c xrd_*: the frame's own depth converted into one reversed mapping), so the compositor can reproject a
   late, repeated or in-between frame with the head's translation as well as its rotation (positional timewarp; ASW 2.0
   on the Meta PC runtime): the 3D cockpit, 0.3-1 m from the eye, then stays in place when the head moves sideways
   during a frame the runtime had to warp. PS2_XR_DEPTH=off (default) | on (whenever the runtime offers the extension) |
   auto (only on the Meta / Oculus PC runtime; the others accept depth and ignore it, so the pass would only cost).
   PS2_XR_DEPTH_NEAR (0.05 m) = the layer's farZ (reversed: depth 1 there), PS2_XR_DEPTH_FAR (0 = infinite, FLT_MAX as
   UEVR does) = its nearZ (depth 0); PS2_XR_DEPTH_FORMAT=d32|d24|d16|d32s8 forces another format the runtime offers
   (default D32_SFLOAT, Meta's recommendation; else the runtime's first depth format). */
#define XD_EXT "XR_KHR_composition_layer_depth"
static int xd_mode = -1;               /* 0 off, 1 on, 2 auto */
static int xd_ext;                     /* the extension is enabled on the instance */
static int xd_use;                     /* depth goes with the projection layer (decided once the runtime is known) */
static float xd_near_m = 0.05f, xd_far_m = 0.0f;
static VkFormat xd_force = VK_FORMAT_UNDEFINED;
static void xd_cfg(void) {
    if (xd_mode >= 0) return;
    const char *e = getenv("PS2_XR_DEPTH");
    xd_mode = !e || !*e || !strcmp(e, "off") || !strcmp(e, "0") ? 0 : !strcmp(e, "auto") || !strcmp(e, "2") ? 2 : 1;
    e = getenv("PS2_XR_DEPTH_NEAR");
    if (e && atof(e) > 0.001 && atof(e) < 10.0) xd_near_m = (float)atof(e);
    e = getenv("PS2_XR_DEPTH_FAR");
    if (e && atof(e) > (double)xd_near_m * 2.0) xd_far_m = (float)atof(e);
    e = getenv("PS2_XR_DEPTH_FORMAT");
    if (e && !strcmp(e, "d24")) xd_force = VK_FORMAT_D24_UNORM_S8_UINT;
    else if (e && !strcmp(e, "d16")) xd_force = VK_FORMAT_D16_UNORM;
    else if (e && !strcmp(e, "d32s8")) xd_force = VK_FORMAT_D32_SFLOAT_S8_UINT;
    else if (e && !strcmp(e, "d32")) xd_force = VK_FORMAT_D32_SFLOAT;
}
/* pre_instance: 1 = enable the extension */
static int xd_want_ext(void) {
    xd_cfg();
    xd_ext = xd_mode > 0 && xr_has_instance_ext(XD_EXT);
    if (xd_mode > 0 && !xd_ext) ps2_log("xr: PS2_XR_DEPTH: the runtime does not offer %s: no depth", XD_EXT);
    return xd_ext;
}
/* after the instance exists: on, or auto on the Meta PC runtime; `kept` = the instance has the optional extensions */
static void xd_decide(const char *runtime, int kept) {
    if (!kept) xd_ext = 0;
    int meta = runtime && (strstr(runtime, "Oculus") || strstr(runtime, "Meta"));
    xd_use = xd_ext && (xd_mode == 1 || (xd_mode == 2 && meta));
    if (xd_mode > 0)
        ps2_log("xr: frame depth to the headset (PS2_XR_DEPTH=%s): %s (near %.3f m, far %s)", xd_mode == 2 ? "auto" : "on",
                xd_use ? "on" : xd_ext ? "off (auto: not the Meta PC runtime)" : "off (extension not enabled)",
                (double)xd_near_m, xd_far_m > 0.0f ? "finite" : "infinite");
}
/* renderer: the session submits depth (ps2_video.c draws the depth pass) */
int ps2_xr_depth_on(void);
/* renderer: the mapping the layer declares -- depth 1 at near_m, 0 at far_m (0 = infinite) */
void ps2_xr_depth_range(float *near_m, float *far_m) { xd_cfg(); *near_m = xd_near_m; *far_m = xd_far_m; }

// Called before vkCreateInstance. Returns 1 if XR is enabled; *exts/*n receive the instance extensions OpenXR needs.
int ps2_xr_pre_instance(const char ***exts, uint32_t *n) {
    *exts = NULL; *n = 0;
    const char *e = getenv("PS2_XR");
    xr_on = (e && *e == '1') ? 1 : 0;
    if (!xr_on) return 0;
    const char *m = getenv("PS2_XR_MODE");
    xmode = (m && !strcmp(m, "stereo")) ? PS2_XR_STEREO : (m && !strcmp(m, "stereo-alt")) ? PS2_XR_STEREO_ALT
          : (m && !strcmp(m, "mono")) ? PS2_XR_MONO : PS2_XR_CINEMA;
    xr_tag_delay = (int)envf("PS2_XR_TAG_DELAY", 0.0f);
    if (xr_tag_delay < 0) xr_tag_delay = 0;
    if (xr_tag_delay > 15) xr_tag_delay = 15;
    const char *rt = getenv("PS2_XR_RUNTIME");
    if (rt && *rt) {
        wchar_t wrt[1100];
        if (ps2_path_wide(rt, wrt, 1100)) SetEnvironmentVariableW(L"XR_RUNTIME_JSON", wrt);
    }
    xscreen_w = envf("PS2_XR_SCREEN_W", 3.0f); xscreen_d = envf("PS2_XR_SCREEN_DIST", 2.5f);
    const char *want[12] = { XR_KHR_VULKAN_ENABLE_EXTENSION_NAME };
    uint32_t nwant = 1;
    if (xd_want_ext()) want[nwant++] = XD_EXT;                 /* frame depth to the headset (PS2_XR_DEPTH) */
    /* the headset's audio device: unless PS2_AUDIO_DEVICE chooses one (ps2_audio.c) */
    const char *ad = getenv("PS2_AUDIO_DEVICE");
    int audio_ext = !(ad && *ad) && xr_has_instance_ext(XR_AUDIO_GUID_EXT);
    if (audio_ext) want[nwant++] = XR_AUDIO_GUID_EXT;
    {   /* pacing diagnostics (xr-pace lines): display times on the host clock, the runtime's own frame counters */
        const char *pe = getenv("PS2_XR_PACE_EXT");
        int pace_ext = !(pe && *pe == '0');
        xpace_conv = pace_ext && xr_has_instance_ext(XPACE_CONV_EXT);
        if (xpace_conv) want[nwant++] = XPACE_CONV_EXT;
        xpm_on = pace_ext && xr_has_instance_ext(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
        if (xpm_on) want[nwant++] = XR_META_PERFORMANCE_METRICS_EXTENSION_NAME;
    }
    /* Touch controllers (ACZ_TOUCH=pad|stick, ps2_touch.c): the Touch Plus interaction profile needs its extension */
    xt_want = ps2_touch_mode() != PS2_TOUCH_OFF;
    xt_plus_ext = xt_want && xr_has_instance_ext(XT_PLUS_EXT);
    if (xt_plus_ext) want[nwant++] = XT_PLUS_EXT;
    {   /* the lens' hidden area (XR_KHR_visibility_mask): our own passes skip it (ps2_video.c pm_*) */
        const char *ve = getenv("PS2_XR_VIS_MASK");
        int offered = xr_has_instance_ext(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
        xvm_ext = !(ve && *ve == '0') && offered && nwant < 8u;
        if (xvm_ext) want[nwant++] = XR_KHR_VISIBILITY_MASK_EXTENSION_NAME;
        ps2_log("xr: %s %s", XR_KHR_VISIBILITY_MASK_EXTENSION_NAME, xvm_ext ? "enabled" : offered ?
                "offered, not enabled (PS2_XR_VIS_MASK=0)" : "not offered by the runtime (masks from the eye crop only)");
    }
    {   /* the display refresh rate (XR_FB_display_refresh_rate, perf6_cadence): the rates offered, the change event,
           PS2_XR_REFRESH_HZ's request; the cadence itself comes from every frame's display period */
        const char *re = getenv("PS2_XR_REFRESH_EXT");
        int offered = xr_has_instance_ext(XRR_EXT);
        xrr_ext = !(re && *re == '0') && offered && nwant < 12u;
        if (xrr_ext) want[nwant++] = XRR_EXT;
        ps2_log("xr: %s %s", XRR_EXT, xrr_ext ? "enabled" : offered ? "offered, not enabled (PS2_XR_REFRESH_EXT=0)"
                : "not offered by the runtime (the refresh rate is the headset app's setting; each frame's display period "
                  "sets the cadence)");
    }
    XrInstanceCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_INSTANCE_CREATE_INFO;
    strcpy(ci.applicationInfo.applicationName, "ps2recomp");
    strcpy(ci.applicationInfo.engineName, "ps2recomp");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = nwant; ci.enabledExtensionNames = want;
    XrResult r = xrCreateInstance(&ci, &xi);
    if (XR_FAILED(r) && nwant > 1) {                     /* never let the optional extensions cost the headset */
        XRLOG(r, "xrCreateInstance with the optional extensions");
        audio_ext = 0; xpace_conv = 0; xpm_on = 0; xt_plus_ext = 0; xvm_ext = 0; xrr_ext = 0;
        ci.enabledExtensionCount = 1;
        r = xrCreateInstance(&ci, &xi);
    }
    if (XR_FAILED(r)) { XRLOG(r, "xrCreateInstance (no runtime, or it lacks XR_KHR_vulkan_enable)"); xr_on = 0; return 0; }
    xr_runtime_loaded = 1;
    if (!(ad && *ad)) ps2_log("xr: %s %s", XR_AUDIO_GUID_EXT, audio_ext ? "enabled" : "not offered by the runtime: sound stays on the default device");
    xr_audio_device_query(audio_ext);
    if (xpace_conv && XR_FAILED(xrGetInstanceProcAddr(xi, "xrConvertTimeToWin32PerformanceCounterKHR",
                                                      (PFN_xrVoidFunction *)&p_xr_to_qpc))) p_xr_to_qpc = NULL;
    if (xpace_conv && XR_FAILED(xrGetInstanceProcAddr(xi, "xrConvertWin32PerformanceCounterToTimeKHR",
                                                      (PFN_xrVoidFunction *)&p_qpc_to_xr))) p_qpc_to_xr = NULL;
    if (xvm_ext && (XR_FAILED(xrGetInstanceProcAddr(xi, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction *)&p_xvm_get))
                    || !p_xvm_get)) { xvm_ext = 0; p_xvm_get = NULL; }
    if (xpm_on && (XR_FAILED(xrGetInstanceProcAddr(xi, "xrSetPerformanceMetricsStateMETA", (PFN_xrVoidFunction *)&p_pm_set))
                   || XR_FAILED(xrGetInstanceProcAddr(xi, "xrQueryPerformanceMetricsCounterMETA",
                                                      (PFN_xrVoidFunction *)&p_pm_query)) || !p_pm_set || !p_pm_query))
        xpm_on = 0;
    ps2_log("xr: pacing diagnostics: display times on the host clock %s, the runtime's frame counters %s (PS2_XR_PACE_EXT=0: "
            "neither)", p_xr_to_qpc ? "yes (" XPACE_CONV_EXT ")" : "no",
            xpm_on ? "yes (" XR_META_PERFORMANCE_METRICS_EXTENSION_NAME ")" : "no");
    XrInstanceProperties ip; memset(&ip, 0, sizeof ip); ip.type = XR_TYPE_INSTANCE_PROPERTIES;
    xrGetInstanceProperties(xi, &ip);
    ps2_log("xr: runtime %s, mode %s, tag delay %d", ip.runtimeName,
            xmode == PS2_XR_STEREO ? "stereo (synchronous)" : xmode == PS2_XR_STEREO_ALT ? "stereo (alternate eyes)"
            : xmode == PS2_XR_MONO ? "mono" : "cinema", xr_tag_delay);
    xd_decide(ip.runtimeName, ci.enabledExtensionCount == nwant);   /* (PS2_XR_DEPTH) */
    XrSystemGetInfo sg; memset(&sg, 0, sizeof sg); sg.type = XR_TYPE_SYSTEM_GET_INFO; sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(xi, &sg, &xsys);
    if (XR_FAILED(r)) { XRLOG(r, "xrGetSystem (headset not connected?)"); xrDestroyInstance(xi); xi = XR_NULL_HANDLE; xr_on = 0; return 0; }
    {   /* usage statistics (ps2_stats.c): the runtime's and the headset's names */
        extern void ps2_stats_set_xr(const char *runtime, const char *system);
        XrSystemProperties sp;
        memset(&sp, 0, sizeof sp);
        sp.type = XR_TYPE_SYSTEM_PROPERTIES;
        ps2_stats_set_xr(ip.runtimeName, XR_SUCCEEDED(xrGetSystemProperties(xi, xsys, &sp)) ? sp.systemName : NULL);
    }
    PFN_xrGetVulkanGraphicsRequirementsKHR req_fn = NULL; PFN_xrGetVulkanInstanceExtensionsKHR iext_fn = NULL;
    xrGetInstanceProcAddr(xi, "xrGetVulkanGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&req_fn);
    xrGetInstanceProcAddr(xi, "xrGetVulkanInstanceExtensionsKHR", (PFN_xrVoidFunction *)&iext_fn);
    if (!req_fn || !iext_fn) { ps2_log("xr: Vulkan entry points missing"); xr_on = 0; return 0; }
    XrGraphicsRequirementsVulkanKHR gr; memset(&gr, 0, sizeof gr); gr.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    req_fn(xi, xsys, &gr);
    uint32_t len = 0; iext_fn(xi, xsys, 0, &len, NULL);
    xinst_ext_buf = (char *)calloc(len + 1, 1);
    iext_fn(xi, xsys, len, &len, xinst_ext_buf);
    int k = split_exts(xinst_ext_buf, xinst_exts, 32);
    ps2_log("xr: Vulkan instance extensions requested by the runtime: %d (%s)", k, len ? xinst_ext_buf : "");
    *exts = xinst_exts; *n = (uint32_t)k;
    return 1;
}

// After vkCreateInstance: the physical device OpenXR wants.
int ps2_xr_pick_physical(VkInstance inst, VkPhysicalDevice *out) {
    if (xr_on != 1) return 0;
    PFN_xrGetVulkanGraphicsDeviceKHR dev_fn = NULL;
    xrGetInstanceProcAddr(xi, "xrGetVulkanGraphicsDeviceKHR", (PFN_xrVoidFunction *)&dev_fn);
    if (!dev_fn) return 0;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    XrResult r = dev_fn(xi, xsys, inst, &pd);
    if (XR_FAILED(r) || !pd) { XRLOG(r, "xrGetVulkanGraphicsDeviceKHR"); return 0; }
    vk_inst = inst; *out = pd;
    return 1;
}

int ps2_xr_device_extensions(const char ***exts, uint32_t *n) {
    *exts = NULL; *n = 0;
    if (xr_on != 1) return 0;
    PFN_xrGetVulkanDeviceExtensionsKHR dext_fn = NULL;
    xrGetInstanceProcAddr(xi, "xrGetVulkanDeviceExtensionsKHR", (PFN_xrVoidFunction *)&dext_fn);
    if (!dext_fn) return 0;
    uint32_t len = 0; dext_fn(xi, xsys, 0, &len, NULL);
    xdev_ext_buf = (char *)calloc(len + 1, 1);
    dext_fn(xi, xsys, len, &len, xdev_ext_buf);
    int k = split_exts(xdev_ext_buf, xdev_exts, 32);
    ps2_log("xr: Vulkan device extensions requested by the runtime: %d (%s)", k, len ? xdev_ext_buf : "");
    *exts = xdev_exts; *n = (uint32_t)k;
    return 1;
}

/* A replaced swapchain may still be on screen in the compositor (layers submitted in the last frames); destroying it
   at once stalled the OpenXR Simulator. Retire it and destroy it a few frames later. */
static struct { XrSwapchain sc; u64 after; } xr_retired[8];
static void retire_sweep(int all) {
    for (int i = 0; i < 8; i++)
        if (xr_retired[i].sc && (all || xr_frames_begun > xr_retired[i].after)) {
            xrDestroySwapchain(xr_retired[i].sc);
            xr_retired[i].sc = XR_NULL_HANDLE;
        }
}
static void chain_destroy(xr_chain *c) {
    if (c->sc) {
        int k;
        for (k = 0; k < 8 && xr_retired[k].sc; k++) ;
        if (k == 8) { retire_sweep(1); k = 0; }
        xr_retired[k].sc = c->sc;
        xr_retired[k].after = xr_frames_begun + 10;
        c->sc = XR_NULL_HANDLE;
    }
    c->nimg = 0; c->w = c->h = 0; c->cw = c->ch = 0;
}

static int chain_create(xr_chain *c, VkFormat src_fmt, uint32_t w, uint32_t h) {
    uint32_t nf = 0; xrEnumerateSwapchainFormats(xs, 0, &nf, NULL);
    int64_t fmts[64]; if (nf > 64) nf = 64;
    xrEnumerateSwapchainFormats(xs, nf, &nf, fmts);
    // Prefer an sRGB format with the window's channel order: the window image holds gamma-encoded values, so a raw
    // vkCmdCopyImage into an sRGB image is exactly right (the compositor decodes). Otherwise blit (may darken/brighten).
    VkFormat bgra[2] = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM };
    VkFormat rgba[2] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM };
    int src_is_bgra = (src_fmt == VK_FORMAT_B8G8R8A8_UNORM || src_fmt == VK_FORMAT_B8G8R8A8_SRGB);
    VkFormat *same = src_is_bgra ? bgra : rgba, *other = src_is_bgra ? rgba : bgra;
    VkFormat pick = VK_FORMAT_UNDEFINED; c->raw_copy = 0;
    for (int k = 0; k < 2 && pick == VK_FORMAT_UNDEFINED; k++)
        for (uint32_t i = 0; i < nf; i++) if ((VkFormat)fmts[i] == same[k]) { pick = same[k]; c->raw_copy = 1; break; }
    for (int k = 0; k < 2 && pick == VK_FORMAT_UNDEFINED; k++)
        for (uint32_t i = 0; i < nf; i++) if ((VkFormat)fmts[i] == other[k]) { pick = other[k]; break; }
    if (pick == VK_FORMAT_UNDEFINED) { ps2_log("xr: no usable 8-bit swapchain format among %u", nf); return -1; }
    XrSwapchainCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = pick; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(xs, &ci, &c->sc);
    if (XR_FAILED(r)) { XRLOG(r, "xrCreateSwapchain"); return -1; }
    XrSwapchainImageVulkanKHR imgs[8]; memset(imgs, 0, sizeof imgs);
    for (int i = 0; i < 8; i++) imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    xrEnumerateSwapchainImages(c->sc, 0, &c->nimg, NULL); if (c->nimg > 8) c->nimg = 8;
    xrEnumerateSwapchainImages(c->sc, c->nimg, &c->nimg, (XrSwapchainImageBaseHeader *)imgs);
    for (uint32_t i = 0; i < c->nimg; i++) c->img[i] = imgs[i].image;
    c->w = w; c->h = h; c->fmt = pick;
    ps2_log("xr: swapchain %d: %ux%u format %d, %u images, %s", (int)(c - xc), w, h, (int)pick, c->nimg,
            c->raw_copy ? "raw copy" : "blit");
    return 0;
}

int ps2_xr_create_session(VkDevice dev, uint32_t qfamily) {
    if (xr_on != 1) return 0;
    vk_dev = dev; vk_qfam = qfamily;
    XrGraphicsBindingVulkanKHR gb; memset(&gb, 0, sizeof gb);
    gb.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
    gb.instance = vk_inst; gb.physicalDevice = vk_phys; gb.device = dev; gb.queueFamilyIndex = qfamily; gb.queueIndex = 0;
    XrSessionCreateInfo sci; memset(&sci, 0, sizeof sci);
    sci.type = XR_TYPE_SESSION_CREATE_INFO; sci.next = &gb; sci.systemId = xsys;
    XrResult r = xrCreateSession(xi, &sci, &xs);
    if (XR_FAILED(r)) { XRLOG(r, "xrCreateSession"); xr_on = 0; return 0; }
    XrReferenceSpaceCreateInfo rs; memset(&rs, 0, sizeof rs);
    rs.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO; rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; xrCreateReferenceSpace(xs, &rs, &xlocal);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; xrCreateReferenceSpace(xs, &rs, &xview);
    xquad.position.z = -xscreen_d;
    ps2_log("xr: session created (screen %.2f m wide at %.2f m)", xscreen_w, xscreen_d);
    xrr_session();                                    /* the refresh rates offered (XR_FB_display_refresh_rate) */
    xt_create();                                      /* Touch controllers (ACZ_TOUCH) */
    return 1;
}

void ps2_xr_set_physical(VkPhysicalDevice pd) { vk_phys = pd; }

/* ---- Touch controllers (tc_touch, 2026-10-23; CONTROLS_RESEARCH_2026-10-23.md 4.1) ----
   ACZ_TOUCH=pad|stick: one action set "flight" with per-hand actions (subaction paths /user/hand/left|right): grip and
   aim poses, trigger and squeeze (floats: ps2_touch.c chooses the thresholds), thumbstick + click, the lower / upper
   face buttons (A / B right, X / Y left), Menu (left), the capacitive trigger and thumb sensors, haptics. Bindings are
   suggested for meta/touch_controller_plus (only when the runtime offers XR_META_touch_controller_plus; enabled at
   instance creation, dropped with the other optional extensions if that fails), oculus/touch_controller and
   khr/simple_controller; a profile refused with the capacitive paths is retried without them. The system button is
   never bound. The set is attached once, right after xrCreateSession. ps2_touch.c calls ps2_xr_touch_sync once per
   renderer loop (poll_input): xrSyncActions while FOCUSED (else nothing is read) and the action states;
   ps2_xr_frame_begin locates the grip and aim spaces at the frame's predicted display time in the recentred reference
   frame (the frame the head is rendered in) and passes them to ps2_touch_on_locate (the cockpit's hands). ACZ_TOUCH
   unset or 0: none of this exists (the session is exactly as before). */
static int xt_attached;
static XrActionSet xt_set;
static XrPath xt_hand[2];
enum { XA_GRIP, XA_AIM, XA_TRIG, XA_SQZ, XA_STICK, XA_CLICK, XA_LOWER, XA_UPPER, XA_MENU, XA_TTOUCH, XA_THTOUCH,
       XA_HAPTIC, XA_N };
static XrAction xt_act[XA_N];
static XrSpace xt_space[2][2];         /* [hand][0 grip, 1 aim] */
static ps2_touch_raw xt;               /* the poses (xt_locate) and the states (ps2_xr_touch_sync) */
static int xt_opt_ok[5];               /* per profile id: the capacitive paths were accepted */
static u64 xt_syncs, xt_haptics;
typedef struct { unsigned char act, hands, opt; const char *sub; } xt_bind;   /* hands: 1 left, 2 right */
static const xt_bind xt_touch_binds[] = {
    { XA_GRIP, 3, 0, "/input/grip/pose" }, { XA_AIM, 3, 0, "/input/aim/pose" },
    { XA_TRIG, 3, 0, "/input/trigger/value" }, { XA_SQZ, 3, 0, "/input/squeeze/value" },
    { XA_STICK, 3, 0, "/input/thumbstick" }, { XA_CLICK, 3, 0, "/input/thumbstick/click" },
    { XA_LOWER, 1, 0, "/input/x/click" }, { XA_LOWER, 2, 0, "/input/a/click" },
    { XA_UPPER, 1, 0, "/input/y/click" }, { XA_UPPER, 2, 0, "/input/b/click" },
    { XA_MENU, 1, 0, "/input/menu/click" }, { XA_HAPTIC, 3, 0, "/output/haptic" },
    { XA_TTOUCH, 3, 1, "/input/trigger/touch" }, { XA_THTOUCH, 3, 1, "/input/thumbstick/touch" },
    { XA_THTOUCH, 1, 1, "/input/x/touch" }, { XA_THTOUCH, 1, 1, "/input/y/touch" },
    { XA_THTOUCH, 2, 1, "/input/a/touch" }, { XA_THTOUCH, 2, 1, "/input/b/touch" },
    { XA_THTOUCH, 3, 1, "/input/thumbrest/touch" },
};
static const xt_bind xt_simple_binds[] = {
    { XA_GRIP, 3, 0, "/input/grip/pose" }, { XA_AIM, 3, 0, "/input/aim/pose" },
    { XA_TRIG, 3, 0, "/input/select/click" }, { XA_MENU, 1, 0, "/input/menu/click" },
    { XA_HAPTIC, 3, 0, "/output/haptic" },
};
static int xt_suggest(const char *profile, const xt_bind *b, int n, int with_opt) {
    XrActionSuggestedBinding sb[48];
    uint32_t k = 0;
    for (int i = 0; i < n; i++) {
        if (b[i].opt && !with_opt) continue;
        for (int h = 0; h < 2; h++) {
            char path[128];
            if (!(b[i].hands & (1 << h)) || k >= 48) continue;
            snprintf(path, sizeof path, "/user/hand/%s%s", h ? "right" : "left", b[i].sub);
            if (XR_FAILED(xrStringToPath(xi, path, &sb[k].binding))) continue;
            sb[k].action = xt_act[b[i].act];
            k++;
        }
    }
    XrPath pp;
    if (XR_FAILED(xrStringToPath(xi, profile, &pp))) return 0;
    XrInteractionProfileSuggestedBinding s; memset(&s, 0, sizeof s);
    s.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    s.interactionProfile = pp; s.countSuggestedBindings = k; s.suggestedBindings = sb;
    XrResult r = xrSuggestInteractionProfileBindings(xi, &s);
    ps2_log("touch: %u bindings for %s%s: %s (%d)", k, profile, with_opt ? "" : " without the capacitive paths",
            XR_SUCCEEDED(r) ? "accepted" : "refused", (int)r);
    return XR_SUCCEEDED(r);
}
static int xt_make(int i, const char *name, const char *loc, XrActionType type) {
    XrActionCreateInfo ai; memset(&ai, 0, sizeof ai);
    ai.type = XR_TYPE_ACTION_CREATE_INFO; ai.actionType = type;
    snprintf(ai.actionName, sizeof ai.actionName, "%s", name);
    snprintf(ai.localizedActionName, sizeof ai.localizedActionName, "%s", loc);
    ai.countSubactionPaths = 2; ai.subactionPaths = xt_hand;
    XrResult r = xrCreateAction(xt_set, &ai, &xt_act[i]);
    if (XR_FAILED(r)) { ps2_log("touch: xrCreateAction %s failed (%d)", name, (int)r); return 0; }
    return 1;
}
static void xt_create(void) {
    if (!xt_want || !xs || xt_set) return;
    if (XR_FAILED(xrStringToPath(xi, "/user/hand/left", &xt_hand[0]))
        || XR_FAILED(xrStringToPath(xi, "/user/hand/right", &xt_hand[1]))) return;
    XrActionSetCreateInfo si; memset(&si, 0, sizeof si);
    si.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    strcpy(si.actionSetName, "flight"); strcpy(si.localizedActionSetName, "Flight");
    XrResult r = xrCreateActionSet(xi, &si, &xt_set);
    if (XR_FAILED(r)) { XRLOG(r, "xrCreateActionSet (Touch controllers off)"); xt_set = XR_NULL_HANDLE; return; }
    int ok = xt_make(XA_GRIP, "grip_pose", "Grip pose", XR_ACTION_TYPE_POSE_INPUT)
          && xt_make(XA_AIM, "aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT)
          && xt_make(XA_TRIG, "trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT)
          && xt_make(XA_SQZ, "squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT)
          && xt_make(XA_STICK, "thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT)
          && xt_make(XA_CLICK, "thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT)
          && xt_make(XA_LOWER, "lower_button", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT)
          && xt_make(XA_UPPER, "upper_button", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT)
          && xt_make(XA_MENU, "menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT)
          && xt_make(XA_TTOUCH, "trigger_touch", "Finger on the trigger", XR_ACTION_TYPE_BOOLEAN_INPUT)
          && xt_make(XA_THTOUCH, "thumb_touch", "Thumb down", XR_ACTION_TYPE_BOOLEAN_INPUT)
          && xt_make(XA_HAPTIC, "haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT);
    if (!ok) return;
    int nt = (int)(sizeof xt_touch_binds / sizeof xt_touch_binds[0]);
    if (xt_plus_ext) {
        xt_opt_ok[PS2_TOUCH_PROFILE_TOUCH_PLUS] = xt_suggest("/interaction_profiles/meta/touch_controller_plus", xt_touch_binds, nt, 1);
        if (!xt_opt_ok[PS2_TOUCH_PROFILE_TOUCH_PLUS])
            xt_suggest("/interaction_profiles/meta/touch_controller_plus", xt_touch_binds, nt, 0);
    }
    xt_opt_ok[PS2_TOUCH_PROFILE_TOUCH] = xt_suggest("/interaction_profiles/oculus/touch_controller", xt_touch_binds, nt, 1);
    if (!xt_opt_ok[PS2_TOUCH_PROFILE_TOUCH]) xt_suggest("/interaction_profiles/oculus/touch_controller", xt_touch_binds, nt, 0);
    xt_suggest("/interaction_profiles/khr/simple_controller", xt_simple_binds,
               (int)(sizeof xt_simple_binds / sizeof xt_simple_binds[0]), 1);
    for (int h = 0; h < 2; h++)
        for (int k = 0; k < 2; k++) {
            XrActionSpaceCreateInfo ac; memset(&ac, 0, sizeof ac);
            ac.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
            ac.action = xt_act[k ? XA_AIM : XA_GRIP]; ac.subactionPath = xt_hand[h];
            ac.poseInActionSpace.orientation.w = 1.0f;
            r = xrCreateActionSpace(xs, &ac, &xt_space[h][k]);
            if (XR_FAILED(r)) { XRLOG(r, "xrCreateActionSpace"); xt_space[h][k] = XR_NULL_HANDLE; }
        }
    XrSessionActionSetsAttachInfo at; memset(&at, 0, sizeof at);
    at.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO; at.countActionSets = 1; at.actionSets = &xt_set;
    r = xrAttachSessionActionSets(xs, &at);
    xt_attached = XR_SUCCEEDED(r);
    ps2_log("touch: action set %s (%d)%s", xt_attached ? "attached" : "NOT attached", (int)r,
            xt_plus_ext ? "; " XT_PLUS_EXT " enabled" : "; no Touch Plus profile (extension not offered)");
}
/* after XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: the profile each hand is bound to */
static void xt_profiles(void) {
    if (!xt_attached) return;
    for (int h = 0; h < 2; h++) {
        XrInteractionProfileState ps; memset(&ps, 0, sizeof ps);
        ps.type = XR_TYPE_INTERACTION_PROFILE_STATE;
        char name[XR_MAX_PATH_LENGTH] = "none";
        uint32_t n = 0;
        int id = PS2_TOUCH_PROFILE_NONE;
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(xs, xt_hand[h], &ps)) && ps.interactionProfile != XR_NULL_PATH
            && XR_SUCCEEDED(xrPathToString(xi, ps.interactionProfile, sizeof name, &n, name))) {
            id = strstr(name, "touch_controller_plus") ? PS2_TOUCH_PROFILE_TOUCH_PLUS
               : strstr(name, "oculus/touch") ? PS2_TOUCH_PROFILE_TOUCH
               : strstr(name, "simple") ? PS2_TOUCH_PROFILE_SIMPLE : PS2_TOUCH_PROFILE_NONE;
        }
        xt.profile[h] = id;
        ps2_log("touch: %s hand profile %s", h ? "right" : "left", name);
    }
}
/* the grip and aim poses at time t in the recentred reference frame (renderer thread): ps2_xr_frame_begin at the
   frame's predicted display time (the hands drawn in that frame); ps2_xr_touch_sync again at the poll's own time for
   the input (ACZ_TOUCH_POSE_TIME=now, the default when the runtime converts host times: on Link the display time is
   27-39 ms ahead, and a stick read from that extrapolation overshoots at every stop and amplifies tremor) */
static int xt_pose_now = -1;
static void xt_locate_at(XrTime t) {
    XrQuaternionf rq = qconj(xref.orientation);
    for (int h = 0; h < 2; h++)
        for (int k = 0; k < 2; k++) {
            ps2_touch_hand_raw *o = &xt.h[h];
            XrSpaceLocation loc; memset(&loc, 0, sizeof loc); loc.type = XR_TYPE_SPACE_LOCATION;
            if (!xt_space[h][k] || XR_FAILED(xrLocateSpace(xt_space[h][k], xlocal, t, &loc))) loc.locationFlags = 0;
            XrSpaceLocationFlags f = loc.locationFlags;
            XrQuaternionf q = qmul(rq, loc.pose.orientation);
            XrVector3f d = { loc.pose.position.x - xref.position.x, loc.pose.position.y - xref.position.y,
                             loc.pose.position.z - xref.position.z };
            XrVector3f p = qrot(rq, d);
            float *oq = k ? o->aq : o->q, *op = k ? o->ap : o->p;
            if (f & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) { oq[0] = q.x; oq[1] = q.y; oq[2] = q.z; oq[3] = q.w; }
            if (f & XR_SPACE_LOCATION_POSITION_VALID_BIT) { op[0] = p.x; op[1] = p.y; op[2] = p.z; }
            if (!k) {
                o->rot_ok = (f & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
                o->pos_ok = (f & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                o->rot_trk = (f & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0;
                o->pos_trk = (f & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
            }
        }
    xt.ref_gen = xt_ref_gen;
    xt.pose_age_ms = 0.0;
    LARGE_INTEGER pc, fq;
    if (p_xr_to_qpc && XR_SUCCEEDED(p_xr_to_qpc(xi, t, &pc)) && QueryPerformanceFrequency(&fq))
        xt.pose_age_ms = (double)pc.QuadPart * (1e3 / (double)fq.QuadPart) - (double)qpc_ns() / 1e6;
}
static void xt_locate(XrTime t) {
    if (!xt_attached) return;
    xt_locate_at(t);
    ps2_touch_on_locate(&xt);
}
int ps2_xr_touch_sync(ps2_touch_raw *out) {
    xt.on = xt_attached && xs && xrunning;
    xt.focused = xt.on && xstate == XR_SESSION_STATE_FOCUSED;
    if (xt_pose_now < 0) {
        const char *e = getenv("ACZ_TOUCH_POSE_TIME");
        xt_pose_now = !(e && !strcmp(e, "display")) && p_qpc_to_xr != NULL;
        if (xt_attached)
            ps2_log("touch: the stick reads the controllers %s", xt_pose_now ? "at the poll's own time (ACZ_TOUCH_POSE_TIME=now)"
                    : "at the last frame's predicted display time (ACZ_TOUCH_POSE_TIME=display, or no host-time conversion)");
    }
    if (xt.on && xt_pose_now) {                              /* the input's poses: now, not the display's prediction */
        LARGE_INTEGER pc; XrTime t;
        QueryPerformanceCounter(&pc);
        if (XR_SUCCEEDED(p_qpc_to_xr(xi, &pc, &t))) xt_locate_at(t);
    }
    if (xt.focused) {
        XrActiveActionSet aas = { xt_set, XR_NULL_PATH };
        XrActionsSyncInfo si; memset(&si, 0, sizeof si);
        si.type = XR_TYPE_ACTIONS_SYNC_INFO; si.countActiveActionSets = 1; si.activeActionSets = &aas;
        XrResult r = xrSyncActions(xs, &si);
        if (r != XR_SUCCESS) xt.focused = 0;                 /* XR_SESSION_NOT_FOCUSED: no input this time */
        else xt_syncs++;
    }
    for (int h = 0; h < 2; h++) {
        ps2_touch_hand_raw *o = &xt.h[h];
        o->active = 0; o->trigger = o->squeeze = o->sx = o->sy = 0.0f;
        o->click = o->lower = o->upper = o->menu = 0;
        o->trig_touch = o->thumb_touch = -1;
        if (!xt.focused) continue;
        XrActionStateGetInfo gi; memset(&gi, 0, sizeof gi);
        gi.type = XR_TYPE_ACTION_STATE_GET_INFO; gi.subactionPath = xt_hand[h];
        XrActionStatePose sp; memset(&sp, 0, sizeof sp); sp.type = XR_TYPE_ACTION_STATE_POSE;
        gi.action = xt_act[XA_GRIP];
        if (XR_SUCCEEDED(xrGetActionStatePose(xs, &gi, &sp))) o->active = sp.isActive != 0;
        XrActionStateFloat sf; memset(&sf, 0, sizeof sf); sf.type = XR_TYPE_ACTION_STATE_FLOAT;
        gi.action = xt_act[XA_TRIG];
        if (XR_SUCCEEDED(xrGetActionStateFloat(xs, &gi, &sf)) && sf.isActive) o->trigger = sf.currentState;
        memset(&sf, 0, sizeof sf); sf.type = XR_TYPE_ACTION_STATE_FLOAT;
        gi.action = xt_act[XA_SQZ];
        if (XR_SUCCEEDED(xrGetActionStateFloat(xs, &gi, &sf)) && sf.isActive) o->squeeze = sf.currentState;
        XrActionStateVector2f sv; memset(&sv, 0, sizeof sv); sv.type = XR_TYPE_ACTION_STATE_VECTOR2F;
        gi.action = xt_act[XA_STICK];
        if (XR_SUCCEEDED(xrGetActionStateVector2f(xs, &gi, &sv)) && sv.isActive) { o->sx = sv.currentState.x; o->sy = sv.currentState.y; }
        static const int ba[6] = { XA_CLICK, XA_LOWER, XA_UPPER, XA_MENU, XA_TTOUCH, XA_THTOUCH };
        int bv[6] = { 0, 0, 0, 0, -1, -1 };
        for (int i = 0; i < 6; i++) {
            XrActionStateBoolean sb; memset(&sb, 0, sizeof sb); sb.type = XR_TYPE_ACTION_STATE_BOOLEAN;
            gi.action = xt_act[ba[i]];
            if (XR_SUCCEEDED(xrGetActionStateBoolean(xs, &gi, &sb)) && sb.isActive) bv[i] = sb.currentState != 0;
        }
        o->click = bv[0]; o->lower = bv[1]; o->upper = bv[2]; o->menu = bv[3];
        int caps = xt.profile[h] >= 0 && xt.profile[h] < 5 && xt_opt_ok[xt.profile[h]];
        o->trig_touch = caps ? bv[4] : -1; o->thumb_touch = caps ? bv[5] : -1;
    }
    *out = xt;
    return xt.on;
}
void ps2_xr_touch_haptic(int hand, float amplitude, float seconds) {
    if (!xt_attached || !xs || !xrunning || hand < 0 || hand > 1) return;
    XrHapticActionInfo hi; memset(&hi, 0, sizeof hi);
    hi.type = XR_TYPE_HAPTIC_ACTION_INFO; hi.action = xt_act[XA_HAPTIC]; hi.subactionPath = xt_hand[hand];
    if (amplitude <= 0.0f || seconds <= 0.0f) { xrStopHapticFeedback(xs, &hi); return; }
    if (xstate != XR_SESSION_STATE_FOCUSED) return;
    XrHapticVibration v; memset(&v, 0, sizeof v);
    v.type = XR_TYPE_HAPTIC_VIBRATION;
    v.amplitude = amplitude > 1.0f ? 1.0f : amplitude;
    v.duration = (XrDuration)((double)seconds * 1e9);
    v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrResult r = xrApplyHapticFeedback(xs, &hi, (const XrHapticBaseHeader *)&v);
    xt_haptics++;
    {   static int said;
        if (XR_FAILED(r) && !said) { said = 1; XRLOG(r, "xrApplyHapticFeedback"); }
    }
}

static void poll_events(void) {
    XrEventDataBuffer ev; memset(&ev, 0, sizeof ev); ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(xi, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrEventDataSessionStateChanged *s = (XrEventDataSessionStateChanged *)&ev;
            xstate = s->state;
            ps2_log("xr: session state %d", (int)xstate);
            if (xstate == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi; memset(&bi, 0, sizeof bi); bi.type = XR_TYPE_SESSION_BEGIN_INFO;
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                xrunning = XR_SUCCEEDED(xrBeginSession(xs, &bi));
                xrecenter_wait = 135;                         /* ~1.5 s of focused frames at 90 Hz */
                if (xrunning) xrr_request();                  /* (PS2_XR_REFRESH_HZ: once per process) */
            } else if (xstate == XR_SESSION_STATE_STOPPING) { xrEndSession(xs); xrunning = 0; }
            else if (xstate == XR_SESSION_STATE_EXITING || xstate == XR_SESSION_STATE_LOSS_PENDING) { xrunning = 0; xr_on = 0; }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            XrEventDataReferenceSpaceChangePending *c = (XrEventDataReferenceSpaceChangePending *)&ev;
            if (c->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL) {
                xrecenter_wait = 45;                          /* after the change takes effect */
                xt_ref_gen++;                                 /* (the controllers move with it: Touch grabs rebase) */
                ps2_log("xr: the runtime moves the LOCAL space; recentring shortly");
            }
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) xt_profiles();
        else if (ev.type == XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR) {
            const XrEventDataVisibilityMaskChangedKHR *m = (const XrEventDataVisibilityMaskChangedKHR *)&ev;
            if (m->viewIndex < 2u) xvm_dirty[m->viewIndex] = 1;
            ps2_log("xr: the runtime changed the visibility mask of view %u", m->viewIndex);
        }
        else if (ev.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {   /* (XR_FB_display_refresh_rate) */
            const XrEventDataDisplayRefreshRateChangedFB *c = (const XrEventDataDisplayRefreshRateChangedFB *)&ev;
            ps2_log("xr: the runtime changed the display refresh rate %.2f -> %.2f Hz (%s)", (double)c->fromDisplayRefreshRate,
                    (double)c->toDisplayRefreshRate, XRR_EXT);
            xcad_fast = 240;                                  /* (a new display period in the next ~2 s counts at once) */
        }
        memset(&ev, 0, sizeof ev); ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

// Place the cinema screen in front of the head and take the head's yaw/position as the tracking reference.
static void recenter(XrTime t) {
    XrSpaceLocation loc; memset(&loc, 0, sizeof loc); loc.type = XR_TYPE_SPACE_LOCATION;
    if (XR_FAILED(xrLocateSpace(xview, xlocal, t, &loc))) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;     /* tracking not back yet: retry */
    XrQuaternionf q = loc.pose.orientation;
    float fx = -(2 * (q.x * q.z + q.w * q.y)), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float l = sqrtf(fx * fx + fz * fz); if (l < 1e-4f) { fx = 0; fz = -1; l = 1; }
    fx /= l; fz /= l;
    float yaw = atan2f(-fx, -fz);
    xquad.orientation.x = 0; xquad.orientation.y = sinf(yaw * 0.5f); xquad.orientation.z = 0; xquad.orientation.w = cosf(yaw * 0.5f);
    xquad.position.x = loc.pose.position.x + fx * xscreen_d; xquad.position.y = loc.pose.position.y; xquad.position.z = loc.pose.position.z + fz * xscreen_d;
    xref.orientation = xquad.orientation;
    xref.position = loc.pose.position;
    xrecenter = 0;
    xquad_next_on = 0;
    xt_ref_gen++;
    ps2_log("xr: recentred (yaw %.1f deg)", yaw * 57.29578f);
}
/* The hand-over recentre (ps2_xr_request_recenter_hand_over): the head as the eyes were located last (xr_latest:
   what the frames are drawn from, PS2_XR_TEST_HEAD included), its yaw against the reference's. */
static void recenter_hand_over(void) {
    XrPosef e0, e1;
    pthread_mutex_lock(&xr_lock);
    int ok = xr_latest.valid;
    e0 = xr_latest.eye[0]; e1 = xr_latest.eye[1];
    pthread_mutex_unlock(&xr_lock);
    if (!ok) return;                                       /* no head yet: next frame */
    __atomic_store_n(&xrecenter_ho, 0, __ATOMIC_RELEASE);
    XrQuaternionf q = e0.orientation;
    float fx = -(2 * (q.x * q.z + q.w * q.y)), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    float l = sqrtf(fx * fx + fz * fz); if (l < 1e-4f) { fx = 0; fz = -1; l = 1; }
    fx /= l; fz /= l;
    float yaw = atan2f(-fx, -fz);
    float ref = 2.0f * atan2f(xref.orientation.y, xref.orientation.w);
    float d = yaw - ref;
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    if (fabsf(d) * 57.29578f <= xho_keep) {
        ps2_log("xr: flight start: the head is %.1f deg from the reference (yaw %.1f): reference kept "
                "(ACZ_VR_RECENTRE_KEEP %.0f)", d * 57.29578f, ref * 57.29578f, xho_keep);
        return;
    }
    XrVector3f p = { 0.5f * (e0.position.x + e1.position.x), 0.5f * (e0.position.y + e1.position.y),
                     0.5f * (e0.position.z + e1.position.z) };
    xref.orientation.x = 0; xref.orientation.y = sinf(yaw * 0.5f); xref.orientation.z = 0; xref.orientation.w = cosf(yaw * 0.5f);
    xref.position = p;
    xquad_next.orientation = xref.orientation;
    xquad_next.position.x = p.x + fx * xscreen_d; xquad_next.position.y = p.y; xquad_next.position.z = p.z + fz * xscreen_d;
    xquad_next_on = 1; xquad_next_wait = 0;
    xt_ref_gen++;
    ps2_log("xr: recentred at the flight start (yaw %.1f deg, %.1f from the reference; the cinema screen follows with "
            "the first head-tracked frame)", yaw * 57.29578f, d * 57.29578f);
}

/* XR_KHR_visibility_mask: one mesh of an eye (kind 0 hidden, 1 visible triangles) into xvm; 1 = stored (an empty mesh
   counts), 0 = failed or refused */
static int xvm_fetch(int eye, int kind) {
    XrVisibilityMaskTypeKHR ty = kind ? XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR
                                      : XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR;
    const char *nm = kind ? "visible" : "hidden";
    XrVisibilityMaskKHR m; memset(&m, 0, sizeof m); m.type = XR_TYPE_VISIBILITY_MASK_KHR;
    XrResult r = p_xvm_get(xs, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)eye, ty, &m);
    if (XR_FAILED(r)) { XRLOG(r, "xrGetVisibilityMaskKHR"); return 0; }
    uint32_t nv = m.vertexCountOutput, ni = m.indexCountOutput;
    float *v = NULL; uint32_t *ix = NULL;
    if (nv && ni) {
        if (nv > XVM_MAXV || ni > 3u * XVM_MAXV || ni % 3u) {
            ps2_log("xr: visibility mask: eye %d %s mesh refused (%u vertices, %u indices)", eye, nm, nv, ni);
            return 0;
        }
        v = (float *)malloc((size_t)nv * 2u * sizeof(float)); ix = (uint32_t *)malloc((size_t)ni * sizeof(uint32_t));
        if (!v || !ix) { free(v); free(ix); return 0; }
        m.vertexCapacityInput = nv; m.vertices = (XrVector2f *)v; m.indexCapacityInput = ni; m.indices = ix;
        r = p_xvm_get(xs, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, (uint32_t)eye, ty, &m);
        if (XR_FAILED(r) || m.vertexCountOutput != nv || m.indexCountOutput != ni) {
            if (XR_FAILED(r)) XRLOG(r, "xrGetVisibilityMaskKHR (data)");
            free(v); free(ix); return 0;
        }
        /* plausibility: indices in range, every vertex within the eye's FOV (+25 %) as tangents */
        float l = tanf(xr_native_fov[eye].angleLeft), rr = tanf(xr_native_fov[eye].angleRight);
        float u = tanf(xr_native_fov[eye].angleUp), d = tanf(xr_native_fov[eye].angleDown);
        float b[4] = { 1e9f, -1e9f, 1e9f, -1e9f };
        int bad = 0;
        for (uint32_t i = 0; i < ni; i++) bad |= ix[i] >= nv;
        for (uint32_t i = 0; i < nv; i++) {
            float x = v[2 * i], y = v[2 * i + 1];
            if (x < b[0]) b[0] = x;
            if (x > b[1]) b[1] = x;
            if (y < b[2]) b[2] = y;
            if (y > b[3]) b[3] = y;
            bad |= !(x >= 1.25f * l - 0.05f && x <= 1.25f * rr + 0.05f && y >= 1.25f * d - 0.05f && y <= 1.25f * u + 0.05f);
        }
        ps2_log("xr: visibility mask: eye %d %s mesh %u triangles, bounds x %.3f..%.3f y %.3f..%.3f (FOV tangents x %.3f..%.3f "
                "y %.3f..%.3f)%s", eye, nm, ni / 3u, (double)b[0], (double)b[1], (double)b[2], (double)b[3], (double)l,
                (double)rr, (double)d, (double)u, bad ? ": REFUSED (not tangents of this FOV)" : "");
        if (bad) { free(v); free(ix); return 0; }
    } else ps2_log("xr: visibility mask: eye %d %s mesh empty", eye, nm);
    pthread_mutex_lock(&xvm_lock);
    free(xvm[eye].v[kind]); free(xvm[eye].ix[kind]);
    xvm[eye].v[kind] = v; xvm[eye].ix[kind] = ix; xvm[eye].nv[kind] = v ? nv : 0u; xvm[eye].ni[kind] = ix ? ni : 0u;
    xvm_gen++;
    pthread_mutex_unlock(&xvm_lock);
    return 1;
}
/* the visible mesh must span the eye's FOV (its bounds within 15 % of each FOV edge's tangent): a mesh in other units
   (normalised [-1, 1], [0, 1]) would pass xvm_fetch's loose test and hide parts the headset shows */
static int xvm_spans_fov(int eye) {
    const float *v = xvm[eye].v[1];
    if (!v || !xvm[eye].nv[1]) return 0;
    float b[4] = { 1e9f, -1e9f, 1e9f, -1e9f };
    for (uint32_t i = 0; i < xvm[eye].nv[1]; i++) {
        float x = v[2 * i], y = v[2 * i + 1];
        if (x < b[0]) b[0] = x;
        if (x > b[1]) b[1] = x;
        if (y < b[2]) b[2] = y;
        if (y > b[3]) b[3] = y;
    }
    float f[4] = { tanf(xr_native_fov[eye].angleLeft), tanf(xr_native_fov[eye].angleRight),
                   tanf(xr_native_fov[eye].angleDown), tanf(xr_native_fov[eye].angleUp) };
    for (int k = 0; k < 4; k++) if (fabsf(b[k] - f[k]) > 0.15f * fabsf(f[k]) + 0.02f) return 0;
    return 1;
}
/* both meshes of the eyes marked dirty (renderer thread, after the views are located) */
static void xvm_query(void) {
    static int fails;
    for (int e = 0; e < 2; e++) {
        if (!xvm_dirty[e]) continue;
        int ok = xvm_fetch(e, 0) & xvm_fetch(e, 1);
        if (ok && !xvm_spans_fov(e)) {                       /* units we do not understand: use neither mesh */
            ps2_log("xr: visibility mask: eye %d's visible mesh does not span its FOV: both meshes dropped (crop only)", e);
            pthread_mutex_lock(&xvm_lock);
            for (int k = 0; k < 2; k++) { free(xvm[e].v[k]); free(xvm[e].ix[k]); xvm[e].v[k] = NULL; xvm[e].ix[k] = NULL;
                                          xvm[e].nv[k] = xvm[e].ni[k] = 0u; }
            xvm_gen++;
            pthread_mutex_unlock(&xvm_lock);
        }
        if (ok || ++fails >= 6) xvm_dirty[e] = 0;              /* (a failing runtime is asked at most 6 times) */
    }
}
/* the visibility mask's generation: bumps with every new mesh, 0 = none (no extension, or not yet queried) */
uint32_t ps2_xr_vis_mask_gen(void) { return xvm_gen; }
/* eye's mesh (kind 0 hidden, 1 visible) as triangles of 6 floats (x0 y0 x1 y1 x2 y2: tangents, x right, y up) into out,
   at most cap; returns the number of triangles (0 = none) */
int ps2_xr_vis_mask_tris(int eye, int kind, float *out, int cap) {
    if (eye < 0 || eye > 1 || kind < 0 || kind > 1 || !out || cap <= 0) return 0;
    int n = 0;
    pthread_mutex_lock(&xvm_lock);
    const float *v = xvm[eye].v[kind]; const uint32_t *ix = xvm[eye].ix[kind];
    for (uint32_t t = 0; v && ix && t + 3u <= xvm[eye].ni[kind] && n < cap; t += 3u, n++)
        for (int k = 0; k < 3; k++) { out[6 * n + 2 * k] = v[2u * ix[t + k]]; out[6 * n + 2 * k + 1] = v[2u * ix[t + k] + 1u]; }
    pthread_mutex_unlock(&xvm_lock);
    return n;
}

static void locate_views(XrTime t) {
    XrViewLocateInfo li; memset(&li, 0, sizeof li);
    li.type = XR_TYPE_VIEW_LOCATE_INFO; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = t; li.space = xlocal;
    XrViewState vs; memset(&vs, 0, sizeof vs); vs.type = XR_TYPE_VIEW_STATE;
    XrView v[2]; memset(v, 0, sizeof v); v[0].type = v[1].type = XR_TYPE_VIEW;
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(xs, &li, &vs, 2, &n, v)) || n != 2) return;
    if (!(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return;
    {   /* PS2_XR_TEST_HEAD=yaw,pitch (degrees, + = left / up): turn the tracked head by that much (tests on the
           simulator, whose head does not move); applied in the LOCAL frame about the point between the eyes. The
           head reaches the game camera only where the head turns it (the R3 cockpit view, or ACZ_VR_HEADLOOK unset);
           outside the cockpit in stick head look it becomes the right stick instead. The sweep runs on the game's field
           clock (59.94 per second from boot), so runs with the same script see the same head at the same field. */
        static int on = -1; static float qd[4], yw0, pt0, rate, rl0, rg;
        /* PS2_XR_TEST_HEAD_SCRIPT="field:yaw,pitch[,roll[,x,y,z]] field~yaw,pitch[,roll[,x,y,z]] ..." (cloudcheck):
           head poses by vblank field on top of PS2_XR_TEST_HEAD; ':' holds the pose from that field on (a step), '~'
           ramps linearly from the previous entry's pose to this one, reached at this field. Deterministic like the pad
           script. x, y, z (lb_f15 2026-10-26): the head moved that far (metres in the LOCAL frame: + right, up, back;
           on top of PS2_XR_TEST_HEAD_POS), e.g. the eyes' travel when the pilot twists round to check six. */
        static int hs_n, hs_cur = -2; static struct { long f; float y, p, r, x[3]; int ramp; } hs[64];
        static float hs_pos[3];
        static float hj_deg, hj_mm; static long hj_from;           /* PS2_XR_TEST_HEAD_JITTER */
        if (on < 0) {
            const char *e = getenv("PS2_XR_TEST_HEAD");
            /* yaw,pitch[,yaw rate deg/s (sweep)[,roll (+ = right ear down)[,range: the sweep goes back and forth
               between yaw - range and yaw + range (0: one way, endless)]]] */
            on = e && sscanf(e, "%f,%f,%f,%f,%f", &yw0, &pt0, &rate, &rl0, &rg) >= 1;
            if (on) ps2_log("xr: test head turn %s", e);
            const char *sc = getenv("PS2_XR_TEST_HEAD_SCRIPT");
            for (const char *p = sc; p && *p && hs_n < 64; ) {
                char *end;
                while (*p == ' ') p++;
                long f = strtol(p, &end, 10);
                if (end == p || (*end != ':' && *end != '~')) break;
                hs[hs_n].f = f; hs[hs_n].ramp = *end == '~';
                p = end + 1;
                float a[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                for (int k = 0; k < 6; k++) {
                    a[k] = strtof(p, &end);
                    if (end == p) break;
                    p = end;
                    if (*p != ',') break;
                    p++;
                }
                hs[hs_n].y = a[0]; hs[hs_n].p = a[1]; hs[hs_n].r = a[2];
                hs[hs_n].x[0] = a[3]; hs[hs_n].x[1] = a[4]; hs[hs_n].x[2] = a[5];
                hs_n++;
                while (*p && *p != ' ') p++;
            }
            if (hs_n) { on = 1; ps2_log("xr: test head script, %d entries", hs_n); }
            /* PS2_XR_TEST_HEAD_JITTER=deg[,mm[,from_field]] (rec_cine 2026-10-30, test only): an unsteady head on top of
               the above -- per axis a deterministic sum of sines by the field clock (1.3 to 11.3 Hz: the small nods
               and the tremor of a seated pilot; roll at half the amplitude) peaking near deg degrees, and position
               noise of about mm millimetres, from that field on; the recording's cinematic window (ACZ_REC_CINE) is
               judged against it */
            const char *je = getenv("PS2_XR_TEST_HEAD_JITTER");
            if (je && sscanf(je, "%f,%f,%ld", &hj_deg, &hj_mm, &hj_from) >= 1 && (hj_deg > 0.0f || hj_mm > 0.0f)) {
                on = 1;
                ps2_log("xr: test head jitter %.2f deg, %.1f mm from field %ld (PS2_XR_TEST_HEAD_JITTER)", hj_deg, hj_mm,
                        hj_from);
            } else hj_deg = hj_mm = 0.0f;
        }
        if (on) {
            extern u64 ps2_kernel_vblank_count(void);
            u64 vb = ps2_kernel_vblank_count();
            float sw = rate * (float)((double)vb / 59.94);
            if (rg > 0.0f) {                                   /* triangle wave of amplitude rg */
                float ph = fmodf(fabsf(sw), 4.0f * rg);
                sw = ph < 2.0f * rg ? ph - rg : 3.0f * rg - ph;
            }
            float sy = 0.0f, sp = 0.0f, sr = 0.0f, sx[3] = { 0.0f, 0.0f, 0.0f };
            if (hs_n) {
                int i = -1;
                for (int k = 0; k < hs_n; k++) if ((long)vb >= hs[k].f) i = k;
                if (i >= 0) {
                    sy = hs[i].y; sp = hs[i].p; sr = hs[i].r; sx[0] = hs[i].x[0]; sx[1] = hs[i].x[1]; sx[2] = hs[i].x[2];
                    if (i + 1 < hs_n && hs[i + 1].ramp && hs[i + 1].f > hs[i].f) {
                        float t = (float)((double)((long)vb - hs[i].f) / (double)(hs[i + 1].f - hs[i].f));
                        sy += (hs[i + 1].y - sy) * t; sp += (hs[i + 1].p - sp) * t; sr += (hs[i + 1].r - sr) * t;
                        for (int k = 0; k < 3; k++) sx[k] += (hs[i + 1].x[k] - sx[k]) * t;
                    }
                }
                if (i != hs_cur) {
                    hs_cur = i;
                    if (i >= 0) ps2_log("xr: test head script field %llu entry %d: yaw %.2f pitch %.2f roll %.2f "
                                        "pos %+.3f %+.3f %+.3f m%s", (unsigned long long)vb, i, hs[i].y, hs[i].p, hs[i].r,
                                        hs[i].x[0], hs[i].x[1], hs[i].x[2],
                                        i + 1 < hs_n && hs[i + 1].ramp ? " (ramp to the next entry)" : "");
                }
            }
            if ((hj_deg > 0.0f || hj_mm > 0.0f) && (long)vb >= hj_from) {    /* PS2_XR_TEST_HEAD_JITTER */
                double t = (double)vb / 59.94;
                static const double fq[4] = { 1.3, 3.7, 6.1, 11.3 }, am[4] = { 0.45, 0.30, 0.17, 0.08 };
                float j[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
                for (int a = 0; a < 6; a++)
                    for (int k = 0; k < 4; k++)
                        j[a] += (float)(am[k] * sin(6.283185307 * fq[k] * (1.0 + 0.07 * a) * t + 1.7 * a + 0.9 * k));
                sy += hj_deg * j[0]; sp += hj_deg * j[1]; sr += 0.5f * hj_deg * j[2];
                for (int k = 0; k < 3; k++) sx[k] += 0.001f * hj_mm * j[3 + k];
            }
            hs_pos[0] = sx[0]; hs_pos[1] = sx[1]; hs_pos[2] = sx[2];
            float yw = (yw0 + sw + sy) * 0.00872665f, pt = (pt0 + sp) * 0.00872665f;
            float qy[4] = { 0.0f, sinf(yw), 0.0f, cosf(yw) }, qx[4] = { sinf(pt), 0.0f, 0.0f, cosf(pt) };
            float qa[4];
            qa[0] = qy[3] * qx[0] + qy[0] * qx[3] + qy[1] * qx[2] - qy[2] * qx[1];   /* qa = qy * qx */
            qa[1] = qy[3] * qx[1] - qy[0] * qx[2] + qy[1] * qx[3] + qy[2] * qx[0];
            qa[2] = qy[3] * qx[2] + qy[0] * qx[1] - qy[1] * qx[0] + qy[2] * qx[3];
            qa[3] = qy[3] * qx[3] - qy[0] * qx[0] - qy[1] * qx[1] - qy[2] * qx[2];
            float rl = -(rl0 + sr) * 0.00872665f, qz[4] = { 0.0f, 0.0f, sinf(rl), cosf(rl) };      /* about the view axis */
            qd[0] = qa[3] * qz[0] + qa[0] * qz[3] + qa[1] * qz[2] - qa[2] * qz[1];   /* qd = qa * qz */
            qd[1] = qa[3] * qz[1] - qa[0] * qz[2] + qa[1] * qz[3] + qa[2] * qz[0];
            qd[2] = qa[3] * qz[2] + qa[0] * qz[1] - qa[1] * qz[0] + qa[2] * qz[3];
            qd[3] = qa[3] * qz[3] - qa[0] * qz[0] - qa[1] * qz[1] - qa[2] * qz[2];
            float m[3] = { 0.5f * (v[0].pose.position.x + v[1].pose.position.x),
                           0.5f * (v[0].pose.position.y + v[1].pose.position.y),
                           0.5f * (v[0].pose.position.z + v[1].pose.position.z) };
            for (int k = 0; k < 2; k++) {
                XrQuaternionf o = v[k].pose.orientation;               /* o' = qd * o */
                v[k].pose.orientation.x = qd[3] * o.x + qd[0] * o.w + qd[1] * o.z - qd[2] * o.y;
                v[k].pose.orientation.y = qd[3] * o.y - qd[0] * o.z + qd[1] * o.w + qd[2] * o.x;
                v[k].pose.orientation.z = qd[3] * o.z + qd[0] * o.y - qd[1] * o.x + qd[2] * o.w;
                v[k].pose.orientation.w = qd[3] * o.w - qd[0] * o.x - qd[1] * o.y - qd[2] * o.z;
                float d[3] = { v[k].pose.position.x - m[0], v[k].pose.position.y - m[1], v[k].pose.position.z - m[2] };
                /* rotate d by qd: d + 2w (u x d) + 2 u x (u x d) */
                float u[3] = { qd[0], qd[1], qd[2] }, w = qd[3];
                float c1[3] = { u[1] * d[2] - u[2] * d[1], u[2] * d[0] - u[0] * d[2], u[0] * d[1] - u[1] * d[0] };
                float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
                v[k].pose.position.x = m[0] + d[0] + 2.0f * (w * c1[0] + c2[0]);
                v[k].pose.position.y = m[1] + d[1] + 2.0f * (w * c1[1] + c2[1]);
                v[k].pose.position.z = m[2] + d[2] + 2.0f * (w * c1[2] + c2[2]);
            }
        }
        /* PS2_XR_TEST_HEAD_POS=x,y,z (metres in the LOCAL frame: + right, up, back): move the tracked head by that
           much (the simulator's head does not move; HUD combiner tests: the line of sight through the glass) */
        static int pon = -1; static float pd[3];
        if (pon < 0) {
            const char *e = getenv("PS2_XR_TEST_HEAD_POS");
            pon = e && sscanf(e, "%f,%f,%f", &pd[0], &pd[1], &pd[2]) == 3;
            if (pon) ps2_log("xr: test head offset %.3f %.3f %.3f m", pd[0], pd[1], pd[2]);
        }
        if (pon || hs_pos[0] != 0.0f || hs_pos[1] != 0.0f || hs_pos[2] != 0.0f)
            for (int k = 0; k < 2; k++) {
                float o[3] = { (pon ? pd[0] : 0.0f) + hs_pos[0], (pon ? pd[1] : 0.0f) + hs_pos[1], (pon ? pd[2] : 0.0f) + hs_pos[2] };
                v[k].pose.position.x += o[0]; v[k].pose.position.y += o[1]; v[k].pose.position.z += o[2];
            }
    }
    if (ps2_xr_headlook_stick() && xview) {
        XrViewLocateInfo lv = li; lv.space = xview;
        XrViewState vv; memset(&vv, 0, sizeof vv); vv.type = XR_TYPE_VIEW_STATE;
        XrView w[2]; memset(w, 0, sizeof w); w[0].type = w[1].type = XR_TYPE_VIEW;
        uint32_t m = 0;
        if (!XR_FAILED(xrLocateViews(xs, &lv, &vv, 2, &m, w)) && m == 2) {
            xr_view_eye[0] = w[0].pose; xr_view_eye[1] = w[1].pose; xr_view_eye_ok = 1;
        }
    }
    xr_native_fov[0] = v[0].fov; xr_native_fov[1] = v[1].fov; xr_native_fov_ok = 1;
    if (xvm_ext && xrunning && (xvm_dirty[0] || xvm_dirty[1])) xvm_query();   /* (XR_KHR_visibility_mask) */
    pthread_mutex_lock(&xr_lock);
    xr_latest.valid = 1;
    xr_latest.eye[0] = v[0].pose; xr_latest.eye[1] = v[1].pose;
    xr_latest.fov[0] = v[0].fov; xr_latest.fov[1] = v[1].fov;
    xr_latest.ref = xref;
    xr_latest.t = t; xr_latest.n++;
    pthread_mutex_unlock(&xr_lock);
}

// EE thread, once per game frame from the camera hook. Chooses the eye, records the pose the game will render with
// and queues the GS tag that lets the presenter match the finished image to that pose.
/* EE, head tracking stops (zero_vr.c, ACZ_VR_TRACK_END_TAG; ov_visual 2026-10-25): the GS stream gets the number 0, so the
   draws that follow carry "no head-tracked frame". Without it they kept the last number of the tracked spell: stale
   only by the 0.25 s rule of xr_seq_live, which the first new tag at the resume made live again -- the frames still in
   the pipeline (drawn with the game's flat camera, or the menu's last image) then went to the headset as head-tracked
   stereo frames with the pose from before the pause. */
void ps2_xr_end_tags(void) {
    if (xr_on != 1 || xmode == PS2_XR_CINEMA) return;
    xr_fifo_n = 0;
    ps2_gfxq_tag(PS2_XR_TAG_KIND, 0u, 0u);
}
int ps2_xr_game_frame(ps2_xr_render_pose *out) {
    memset(out, 0, sizeof *out);
    if (xr_on != 1 || xmode == PS2_XR_CINEMA) return 0;
    XrPosef eye[2], ref; XrFovf fov[2];
    pthread_mutex_lock(&xr_lock);
    int valid = xr_latest.valid;
    eye[0] = xr_latest.eye[0]; eye[1] = xr_latest.eye[1]; fov[0] = xr_latest.fov[0]; fov[1] = xr_latest.fov[1];
    ref = xr_latest.ref;
    pthread_mutex_unlock(&xr_lock);
    if (!valid) return 0;
    int e = xmode == PS2_XR_STEREO_ALT ? (int)(xr_game_frames & 1u) : -1;
    xr_game_frames++;
    {   extern void ps2_speed_game_frame(void);   /* PS2_SPEED_LOG: the game's frame rate ("speed: game frames") */
        ps2_speed_game_frame();
    }
    XrPosef pose;
    if (e >= 0) pose = eye[e];
    else {
        pose.orientation = eye[0].orientation;
        pose.position.x = 0.5f * (eye[0].position.x + eye[1].position.x);
        pose.position.y = 0.5f * (eye[0].position.y + eye[1].position.y);
        pose.position.z = 0.5f * (eye[0].position.z + eye[1].position.z);
    }
    // One symmetric FOV for both eyes that covers each eye's (asymmetric) frustum.
    float th = 0, tv = 0;
    for (int i = 0; i < 2; i++) {
        float a = tanf(-fov[i].angleLeft), b = tanf(fov[i].angleRight), c = tanf(fov[i].angleUp), d = tanf(-fov[i].angleDown);
        if (a > th) th = a; if (b > th) th = b; if (c > tv) tv = c; if (d > tv) tv = d;
    }
    {   static int said;
        if (!said) {
            said = 1;
            ps2_log("xr: eye FOV L(%.1f %.1f %.1f %.1f) R(%.1f %.1f %.1f %.1f) deg -> symmetric tangents %.3f x %.3f",
                    fov[0].angleLeft * 57.2958f, fov[0].angleRight * 57.2958f, fov[0].angleUp * 57.2958f,
                    fov[0].angleDown * 57.2958f, fov[1].angleLeft * 57.2958f, fov[1].angleRight * 57.2958f,
                    fov[1].angleUp * 57.2958f, fov[1].angleDown * 57.2958f, th, tv);
        }
    }
    {   // ACZ_VR_FOV_LIMIT / PS2_XR_FOV_LIMIT: cap the half-FOV tangent (game draw lists are sized for ~0.56)
        static float lim = -1.0f;
        if (lim < 0.0f) { lim = envf("PS2_XR_FOV_LIMIT", envf("ACZ_VR_FOV_LIMIT", 0.0f)); }
        if (lim > 0.0f) { if (th > lim) th = lim; if (tv > lim) tv = lim; }
    }
    u32 seq = xr_seq_next++;
    if (!seq) seq = xr_seq_next++;
    int slot = (int)(seq % XR_TAGS);
    xr_tags[slot].eye = e; xr_tags[slot].pose = pose; xr_tags[slot].tan_h = th; xr_tags[slot].tan_v = tv;
    xr_tags[slot].eyep[0] = eye[0]; xr_tags[slot].eyep[1] = eye[1];
    xr_tags[slot].shift_seq = 0;
    xr_tags[slot].locked = xr_next_locked;
    __atomic_store_n(&xr_tags[slot].seq, seq, __ATOMIC_RELEASE);
    xr_last_tag_s = ps2_wall_seconds();
    // The tag goes into the GS stream now; with a delay the draws of this frame are assumed to follow later.
    for (int i = 15; i > 0; i--) xr_fifo[i] = xr_fifo[i - 1];
    xr_fifo[0] = seq; if (xr_fifo_n < 16) xr_fifo_n++;
    if ((u32)xr_tag_delay < xr_fifo_n) ps2_gfxq_tag(PS2_XR_TAG_KIND, xr_fifo[xr_tag_delay], (u32)(e + 1));
    XrQuaternionf rq = qconj(ref.orientation);
    XrQuaternionf q = qmul(rq, pose.orientation);
    XrVector3f dp = { pose.position.x - ref.position.x, pose.position.y - ref.position.y, pose.position.z - ref.position.z };
    XrVector3f p = qrot(rq, dp);
    out->valid = 1; out->eye = e; out->seq = seq;
    out->q[0] = q.x; out->q[1] = q.y; out->q[2] = q.z; out->q[3] = q.w;
    xr_tags[slot].qrel[0] = q.x; xr_tags[slot].qrel[1] = q.y; xr_tags[slot].qrel[2] = q.z; xr_tags[slot].qrel[3] = q.w;
    out->p[0] = p.x; out->p[1] = p.y; out->p[2] = p.z;
    xr_tags[slot].prel[0] = p.x; xr_tags[slot].prel[1] = p.y; xr_tags[slot].prel[2] = p.z;
    out->tan_h = th; out->tan_v = tv;
    {   float dx = eye[1].position.x - eye[0].position.x, dy = eye[1].position.y - eye[0].position.y,
              dz = eye[1].position.z - eye[0].position.z;
        out->ipd = sqrtf(dx * dx + dy * dy + dz * dz);
        if (!(out->ipd > 0.03f && out->ipd < 0.09f)) out->ipd = 0.064f;
        xr_tags[slot].ipd = out->ipd;
    }
    return 1;
}

void ps2_xr_set_shift(u32 seq, float a, float c, float mh) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || xr_tags[slot].seq != seq) return;
    xr_tags[slot].shift_a = a; xr_tags[slot].shift_c = c; xr_tags[slot].shift_mh = mh;
    {   static u32 n;
        if (getenv("PS2_XR_DEBUG_SHIFT") && (n++ % 300u) == 0u)
            ps2_log("xr: shift seq %u a %.4g c %.4g mh %.4g", seq, a, c, mh);
    }
    __atomic_store_n(&xr_tags[slot].shift_seq, seq, __ATOMIC_RELEASE);
}
int ps2_xr_shift_of(u32 seq, float *a, float *c, float *mh) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || __atomic_load_n(&xr_tags[slot].shift_seq, __ATOMIC_ACQUIRE) != seq) return 0;
    *a = xr_tags[slot].shift_a; *c = xr_tags[slot].shift_c; *mh = xr_tags[slot].shift_mh;
    return 1;
}
/* EE thread, after the scene camera update: its world position, rotation (view -> world, view axes x right, y up,
   z back) and screen projection (GS pixels: x = cx + fx * X / -Z, y = cy - fy * Y / -Z) */
void ps2_xr_set_camera(u32 seq, const float eye[3], const float rot[9], float fx, float fy, float cx, float cy) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || xr_tags[slot].seq != seq) return;
    memcpy(xr_tags[slot].cam_eye, eye, sizeof xr_tags[slot].cam_eye);
    memcpy(xr_tags[slot].cam_rot, rot, sizeof xr_tags[slot].cam_rot);
    xr_tags[slot].cam_f[0] = fx; xr_tags[slot].cam_f[1] = fy;
    xr_tags[slot].cam_c[0] = cx; xr_tags[slot].cam_c[1] = cy;
    __atomic_store_n(&xr_tags[slot].cam_seq, seq, __ATOMIC_RELEASE);
    {   static int n;
        if (getenv("PS2_XR_DEBUG_CAM") && n < 40) {
            n++;
            extern u64 ps2_kernel_vblank_count(void);
            ps2_log("xr-cam: seq %u field %llu eye %.2f %.2f %.2f", seq, (unsigned long long)ps2_kernel_vblank_count(),
                    eye[0], eye[1], eye[2]);
        }
    }
}
/* EE thread, with ps2_xr_set_camera: the game's own camera of that update (eye, rotation view -> world), i.e. without
   the head. Frames between game frames extrapolate this one and keep the head pose of the game frame (the layer is
   submitted with that pose and the compositor applies the head motion since). */
void ps2_xr_set_camera_base(u32 seq, const float eye[3], const float rot[9]) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || xr_tags[slot].seq != seq) return;
    memcpy(xr_tags[slot].bcam_eye, eye, sizeof xr_tags[slot].bcam_eye);
    memcpy(xr_tags[slot].bcam_rot, rot, sizeof xr_tags[slot].bcam_rot);
    __atomic_store_n(&xr_tags[slot].bcam_seq, seq, __ATOMIC_RELEASE);
}
int ps2_xr_camera_base_of(u32 seq, float eye[3], float rot[9]) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || __atomic_load_n(&xr_tags[slot].bcam_seq, __ATOMIC_ACQUIRE) != seq) return 0;
    memcpy(eye, xr_tags[slot].bcam_eye, sizeof xr_tags[slot].bcam_eye);
    memcpy(rot, xr_tags[slot].bcam_rot, sizeof xr_tags[slot].bcam_rot);
    return 1;
}
/* EE thread, from the cloud hooks (zero_vr.c): the roll of the camera (world up's angle in the view, atan2 of the view
   rotation's [1][0], [1][1]) the game's cloud field placed its sprites with (which 0) and built the cloud images with
   (which 1) in that frame. The renderer turns the sprites by what the drawn view differs (ps2_video.c
   xr_cloud_upright). */
void ps2_xr_set_cloud_roll(u32 seq, int which, float roll) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || which < 0 || which > 1 || xr_tags[slot].seq != seq) return;
    xr_tags[slot].cloud_roll[which] = roll;
    __atomic_store_n(&xr_tags[slot].cloud_seq[which], seq, __ATOMIC_RELEASE);
}
int ps2_xr_cloud_roll_of(u32 seq, float roll[2]) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || __atomic_load_n(&xr_tags[slot].cloud_seq[0], __ATOMIC_ACQUIRE) != seq
        || __atomic_load_n(&xr_tags[slot].cloud_seq[1], __ATOMIC_ACQUIRE) != seq) return 0;
    roll[0] = xr_tags[slot].cloud_roll[0]; roll[1] = xr_tags[slot].cloud_roll[1];
    return 1;
}
/* ACZ_REC_CINE (rec_cine 2026-10-30, wish #245): while the renderer records the recording's cinematic view of game
   frame seq, that frame's camera (eye, rotation) and head pose (q, p; one centre eye) are the cinematic camera's for
   the calling thread only (ps2_video.c cine_run): the passes that draw from the frame's head and camera (the 3D cockpit,
   our clouds, the trails, the veils) then draw that view. seq 0 ends it. */
static __thread struct { u32 seq; float eye[3], rot[9], q[4], p[3]; } xr_cine;
void ps2_xr_cine_override(u32 seq, const float eye[3], const float rot[9], const float q[4], const float p[3]) {
    xr_cine.seq = seq;
    if (!seq) return;
    memcpy(xr_cine.eye, eye, sizeof xr_cine.eye); memcpy(xr_cine.rot, rot, sizeof xr_cine.rot);
    memcpy(xr_cine.q, q, sizeof xr_cine.q); memcpy(xr_cine.p, p, sizeof xr_cine.p);
}
/* renderer: the head pose a game frame was drawn with (relative to the recentred reference) */
/* The newest located head (the point between the eyes, the left eye's orientation) relative to the recentred
   reference, made as ps2_xr_game_frame makes a game frame's; *t = its predicted display time (ns), *n = the count of
   locates (renderer: the 3D cockpit's eye position at record time, ps2_video.c ACZ_VR_COCKPIT_FRESH_HEAD) */
int ps2_xr_head_latest(float q[4], float p[3], long long *t, u32 *n) {
    if (xr_cine.seq) {                     /* (ACZ_REC_CINE: the cinematic view's head, not the newest located one) */
        memcpy(q, xr_cine.q, sizeof xr_cine.q); memcpy(p, xr_cine.p, sizeof xr_cine.p);
        if (t) *t = 0;
        if (n) *n = 0;
        return 1;
    }
    if (xr_on != 1 || xmode == PS2_XR_CINEMA || xmode == PS2_XR_STEREO_ALT) return 0;   /* (alternate eyes: one eye's pose) */
    pthread_mutex_lock(&xr_lock);
    int valid = xr_latest.valid;
    XrPosef e0 = xr_latest.eye[0], e1 = xr_latest.eye[1], ref = xr_latest.ref;
    long long tt = (long long)xr_latest.t; u32 nn = xr_latest.n;
    pthread_mutex_unlock(&xr_lock);
    if (!valid) return 0;
    XrQuaternionf rq = qconj(ref.orientation), qq = qmul(rq, e0.orientation);
    XrVector3f dp = { 0.5f * (e0.position.x + e1.position.x) - ref.position.x, 0.5f * (e0.position.y + e1.position.y) - ref.position.y,
                      0.5f * (e0.position.z + e1.position.z) - ref.position.z };
    XrVector3f pr = qrot(rq, dp);
    q[0] = qq.x; q[1] = qq.y; q[2] = qq.z; q[3] = qq.w;
    p[0] = pr.x; p[1] = pr.y; p[2] = pr.z;
    if (t) *t = tt;
    if (n) *n = nn;
    return 1;
}
int ps2_xr_head_of(u32 seq, float q[4], float p[3], float *ipd) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || __atomic_load_n(&xr_tags[slot].seq, __ATOMIC_ACQUIRE) != seq) return 0;
    if (seq == xr_cine.seq) {              /* (ACZ_REC_CINE: the cinematic view, one centre eye) */
        memcpy(q, xr_cine.q, sizeof xr_cine.q); memcpy(p, xr_cine.p, sizeof xr_cine.p);
        *ipd = 0.0f;
        return 1;
    }
    memcpy(q, xr_tags[slot].qrel, 4 * sizeof *q);
    memcpy(p, xr_tags[slot].prel, 3 * sizeof *p);
    *ipd = xr_tags[slot].ipd;
    return 1;
}
int ps2_xr_camera_of(u32 seq, float eye[3], float rot[9], float f[2], float c[2]) {
    int slot = (int)(seq % XR_TAGS);
    if (!seq || __atomic_load_n(&xr_tags[slot].cam_seq, __ATOMIC_ACQUIRE) != seq) return 0;
    if (seq == xr_cine.seq) {              /* (ACZ_REC_CINE: the cinematic camera; the frame's projection) */
        memcpy(eye, xr_cine.eye, sizeof xr_cine.eye); memcpy(rot, xr_cine.rot, sizeof xr_cine.rot);
        f[0] = xr_tags[slot].cam_f[0]; f[1] = xr_tags[slot].cam_f[1];
        c[0] = xr_tags[slot].cam_c[0]; c[1] = xr_tags[slot].cam_c[1];
        return 1;
    }
    memcpy(eye, xr_tags[slot].cam_eye, sizeof xr_tags[slot].cam_eye);
    memcpy(rot, xr_tags[slot].cam_rot, sizeof xr_tags[slot].cam_rot);
    f[0] = xr_tags[slot].cam_f[0]; f[1] = xr_tags[slot].cam_f[1];
    c[0] = xr_tags[slot].cam_c[0]; c[1] = xr_tags[slot].cam_c[1];
    return 1;
}
int ps2_xr_sync_frame(u32 seq) {
    int slot = (int)(seq % XR_TAGS);
    /* Two passes only with real parallax: frames drawn with crash/replay cameras get none (mh = 0) and go out
       as a single image for both eyes, which also avoids second-pass artifacts with off-screen targets. */
    return xr_on == 1 && xmode == PS2_XR_STEREO && ((xframe_open && xfs.shouldRender) || xpredict) && seq
           && __atomic_load_n(&xr_tags[slot].seq, __ATOMIC_ACQUIRE) == seq && xr_seq_live(seq)
           && xr_tags[slot].eye < 0
           && __atomic_load_n(&xr_tags[slot].shift_seq, __ATOMIC_ACQUIRE) == seq && xr_tags[slot].shift_mh > 0.0f;
}

int ps2_xr_session_running(void) { return xr_on == 1 && xs && xrunning; }
/* the headset's native per-eye FOV as tangents (left < 0, right > 0, up > 0, down < 0); 0 = not known yet (clouds:
   the part of each eye image the headset shows, ps2_cloud_view.vis) */
int ps2_xr_native_tangents(int eye, float t[4]) {
    static int crop = -1;                  /* PS2_XR_EYE_CROP=0 submits the whole image: no crop to respect */
    if (crop < 0) { const char *e = getenv("PS2_XR_EYE_CROP"); crop = !(e && *e == '0'); }
    if (!crop || !xr_native_fov_ok || eye < 0 || eye > 1) return 0;
    t[0] = tanf(xr_native_fov[eye].angleLeft); t[1] = tanf(xr_native_fov[eye].angleRight);
    t[2] = tanf(xr_native_fov[eye].angleUp); t[3] = tanf(xr_native_fov[eye].angleDown);
    return 1;
}
/* predicted display time (ns) of the XR frame being recorded, 0 when none is open */
long long ps2_xr_frame_time(void) { return xframe_open ? (long long)xfs.predictedDisplayTime : 0; }
/* the runtime's display period (ns; 0 before the first frame): the renderer's frame-time governor scales its budget by it */
long long ps2_xr_display_period(void) { return (long long)xfs.predictedDisplayPeriod; }

/* Renderer thread, before recording a game frame early (PS2_PERF_RECORD_EARLY): 1 when the next frame is expected to go
   to the headset as a stereo frame; ps2_xr_sync_frame then treats that frame as open until ps2_xr_frame_begin */
int ps2_xr_frame_predict(void) {
    xpredict = xr_on == 1 && xs && xrunning && xmode == PS2_XR_STEREO && !xframe_open && xlast_should;
    return xpredict;
}
// Renderer thread, once per rendered frame, before recording. Returns 1 when this frame goes to the headset.
int ps2_xr_frame_begin(void) {
    xpredict = 0;
    if (xr_on != 1 || !xs) return 0;
    poll_events();
    if (!xrunning) { xlast_should = 0; return 0; }
    XrFrameWaitInfo wi; memset(&wi, 0, sizeof wi); wi.type = XR_TYPE_FRAME_WAIT_INFO;
    memset(&xfs, 0, sizeof xfs); xfs.type = XR_TYPE_FRAME_STATE;
    long long w_call = qpc_ns();
    if (XR_FAILED(xrWaitFrame(xs, &wi, &xfs))) { xlast_should = 0; return 0; }
    long long w_ret = qpc_ns();
    if (xtest_hz != 0.0) xtest_rate(w_call, &w_ret);           /* PS2_XR_TEST_HZ: an emulated slower display (test) */
    xcad_wait((long long)xfs.predictedDisplayPeriod);          /* (the display's cadence: ps2_xr_cadence) */
    if (xrr_chk) xrr_check();                                  /* (PS2_XR_REFRESH_HZ: the rate after the request) */
    xlast_should = xfs.shouldRender ? 1 : 0;
    if (xprev_pdt && xfs.predictedDisplayPeriod > 0) {
        double e = (double)(xfs.predictedDisplayTime - xprev_pdt - xfs.predictedDisplayPeriod) / 1e6;
        xpdt_n++;
        if (e > 0.5 || e < -0.5) { xpdt_off++; xpdt_err_ms += e < 0.0 ? -e : e; }
    }
    {   /* (pacing diagnostics: ps2_xr_wait_info) */
        xwait.n++; xwait.call_ns = w_call; xwait.ret_ns = w_ret; xwait.should = xfs.shouldRender ? 1 : 0;
        xwait.pdt = (long long)xfs.predictedDisplayTime; xwait.period = (long long)xfs.predictedDisplayPeriod;
        xwait.steps = xprev_pdt && xfs.predictedDisplayPeriod > 0
            ? (int)floor((double)(xfs.predictedDisplayTime - xprev_pdt) / (double)xfs.predictedDisplayPeriod + 0.5) : -99;
        xwait.lead_ok = 0;
        LARGE_INTEGER pc, f;
        if (p_xr_to_qpc && XR_SUCCEEDED(p_xr_to_qpc(xi, xfs.predictedDisplayTime, &pc)) && QueryPerformanceFrequency(&f)) {
            xwait.lead_ms = (double)pc.QuadPart * (1e3 / (double)f.QuadPart) - (double)w_ret / 1e6;
            xwait.lead_ok = 1;
        }
    }
    xprev_pdt = xfs.predictedDisplayTime;
    XrFrameBeginInfo bi; memset(&bi, 0, sizeof bi); bi.type = XR_TYPE_FRAME_BEGIN_INFO;
    if (XR_FAILED(xrBeginFrame(xs, &bi))) return 0;
    xframe_open = 1; xlayer = 0; xr_frames_begun++;
    retire_sweep(0);
    {   /* gamepad: hold Back/Select (PS2 SELECT, bit 0) about a second to recentre -- F9 is out of reach in the headset */
        static int hold;
        if (ps2_pad_host[0].connected && (ps2_pad_host[0].buttons & 1u)) { if (++hold == 70) xrecenter = 1; }
        else hold = 0;
    }
    if (xrecenter_wait > 0 && xstate == XR_SESSION_STATE_FOCUSED && --xrecenter_wait == 0) xrecenter = 1;
    if (xrecenter || (GetAsyncKeyState(VK_F9) & 1)) recenter(xfs.predictedDisplayTime);
    else if (__atomic_load_n(&xrecenter_ho, __ATOMIC_ACQUIRE)) recenter_hand_over();   /* (zero_vr.c, flight start) */
    if (xmode != PS2_XR_CINEMA) locate_views(xfs.predictedDisplayTime);
    xt_locate(xfs.predictedDisplayTime);                 /* Touch: the hands' poses for this frame */
    return xfs.shouldRender ? 1 : 0;
}

static void img_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                        VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b; memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = ss; b.srcAccessMask = sa; b.dstStageMask = ds; b.dstAccessMask = da;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img; b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;   /* multiview RTs */
    VkDependencyInfo di; memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

// Copy a w x h region at (sx, sy) of `src` (in layout `lay`) into swapchain `which`; `src` returns to `lay`.
static VkImageLayout src_lay = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
static u32 src_layer;                         /* multiview: array layer of the source render target */
void ps2_xr_set_src_layer(u32 layer) { src_layer = layer; }
static int32_t src_x, src_y;
/* Reuse the chain while the image fits (menus, movies and flight change the copied size); grow with margin so size
   changes do not recreate it every time, and back off after a failed create. 1 = the chain takes a w x h image. */
static int chain_fit(xr_chain *c, VkFormat src_fmt, uint32_t w, uint32_t h) {
    if (!c->sc || w > c->w || h > c->h || c->src_fmt != src_fmt) {
        if (c->retry_after > xr_frames_begun) return 0;
        /* grow with 25% margin, but the margin never takes a chain past 2048 (very large swapchains stalled the
           OpenXR Simulator's compositor) */
        uint32_t mw = w + w / 4, mh = h + h / 4;
        if (mw > 2048) mw = w > 2048 ? w : 2048;
        if (mh > 2048) mh = h > 2048 ? h : 2048;
        uint32_t nw = w > c->w ? mw : c->w, nh = h > c->h ? mh : c->h;
        if (c->src_fmt != src_fmt) { nw = mw; nh = mh; }
        chain_destroy(c);
        if (chain_create(c, src_fmt, nw, nh) != 0) {
            c->retry_after = xr_frames_begun + 120;
            return 0;
        }
        c->src_fmt = src_fmt;
    }
    return 1;
}
/* PS2_PASS_MASK `copy` (ov_passes, 2026-10-25): only this part (x0, y0, x1, y1 of the w x h image) is copied into the
   swapchain image -- the eye's crop (its imageRect) and a margin; all zero = the whole image */
static int32_t copy_rgn[4];
int ps2_xr_eye_crop(int eye, u32 seq, uint32_t W, uint32_t H, int r[4]);
/* the copy (or blit) of copy_into: src (TRANSFER_SRC, layer src_layer, region copy_rgn or all of w x h at src_x / src_y)
   into dst (TRANSFER_DST) of chain c */
static void copy_cmd(VkCommandBuffer cmd, const xr_chain *c, VkImage src, VkImage dst, uint32_t w, uint32_t h) {
    if (c->raw_copy) {
        VkImageCopy rc; memset(&rc, 0, sizeof rc);
        rc.srcSubresource.aspectMask = rc.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        rc.srcSubresource.layerCount = rc.dstSubresource.layerCount = 1;
        rc.srcSubresource.baseArrayLayer = src_layer;
        rc.srcOffset.x = src_x; rc.srcOffset.y = src_y;
        rc.extent.width = w; rc.extent.height = h; rc.extent.depth = 1;
        if (copy_rgn[2] > copy_rgn[0] && copy_rgn[3] > copy_rgn[1]) {     /* (the crop and its margin only) */
            rc.srcOffset.x = src_x + copy_rgn[0]; rc.srcOffset.y = src_y + copy_rgn[1];
            rc.dstOffset.x = copy_rgn[0]; rc.dstOffset.y = copy_rgn[1];
            rc.extent.width = (uint32_t)(copy_rgn[2] - copy_rgn[0]); rc.extent.height = (uint32_t)(copy_rgn[3] - copy_rgn[1]);
        }
        vkCmdCopyImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    } else {
        VkImageBlit bl; memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = bl.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = bl.dstSubresource.layerCount = 1;
        bl.srcSubresource.baseArrayLayer = src_layer;
        bl.srcOffsets[0].x = src_x; bl.srcOffsets[0].y = src_y;
        bl.srcOffsets[1].x = src_x + (int32_t)w; bl.srcOffsets[1].y = src_y + (int32_t)h; bl.srcOffsets[1].z = 1;
        bl.dstOffsets[1].x = (int32_t)w; bl.dstOffsets[1].y = (int32_t)h; bl.dstOffsets[1].z = 1;
        if (copy_rgn[2] > copy_rgn[0] && copy_rgn[3] > copy_rgn[1]) {     /* (the crop and its margin only) */
            bl.srcOffsets[0].x = src_x + copy_rgn[0]; bl.srcOffsets[0].y = src_y + copy_rgn[1];
            bl.srcOffsets[1].x = src_x + copy_rgn[2]; bl.srcOffsets[1].y = src_y + copy_rgn[3];
            bl.dstOffsets[0].x = copy_rgn[0]; bl.dstOffsets[0].y = copy_rgn[1];
            bl.dstOffsets[1].x = copy_rgn[2]; bl.dstOffsets[1].y = copy_rgn[3];
        }
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    }
}
static int copy_into(VkCommandBuffer cmd, int which, VkImage src, VkFormat src_fmt, uint32_t w, uint32_t h) {
    xr_chain *c = &xc[which];
    if (!chain_fit(c, src_fmt, w, h)) return 0;
    c->cw = w; c->ch = h;
    XrSwapchainImageAcquireInfo ai; memset(&ai, 0, sizeof ai); ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    if (xacq_mask & (1 << which)) return 0;            // one image per chain per frame
    if (XR_FAILED(xrAcquireSwapchainImage(c->sc, &ai, &xacq_index))) return 0;
    XrSwapchainImageWaitInfo wi; memset(&wi, 0, sizeof wi); wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(c->sc, &wi);
    xacq_mask |= 1 << which;
    VkImage dst = c->img[xacq_index];
    img_barrier(cmd, src, src_lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    img_barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    copy_cmd(cmd, c, src, dst, w, h);
    img_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    img_barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src_lay,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    return 1;
}

// Renderer thread: route the finished window image to the cinema screen or to the eye it was rendered for.
static void record_copy_from(VkCommandBuffer cmd, VkImage src, VkFormat src_fmt, uint32_t w, uint32_t h, u32 xr_seq);
void ps2_xr_record_copy(VkCommandBuffer cmd, VkImage src, VkFormat src_fmt, uint32_t w, uint32_t h, u32 xr_seq) {
    src_lay = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; src_x = src_y = 0;
    record_copy_from(cmd, src, src_fmt, w, h, xr_seq);
}
// Same, from a region of the game's render target at internal resolution (sharper than the window image).
void ps2_xr_record_copy_rt(VkCommandBuffer cmd, VkImage src, VkFormat src_fmt, VkImageLayout lay,
                           int32_t x, int32_t y, uint32_t w, uint32_t h, u32 xr_seq) {
    src_lay = lay; src_x = x; src_y = y;
    record_copy_from(cmd, src, src_fmt, w, h, xr_seq);
    src_lay = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; src_x = src_y = 0;
}
// Synchronous stereo: the render target after the pass for `eye` goes to that eye's swapchain with its own pose.
void ps2_xr_record_copy_rt_eye(VkCommandBuffer cmd, VkImage src, VkFormat src_fmt, VkImageLayout lay,
                               int32_t x, int32_t y, uint32_t w, uint32_t h, u32 xr_seq, int eye) {
    int slot = (int)(xr_seq % XR_TAGS);
    if (!ps2_xr_sync_frame(xr_seq) || eye < 0 || eye > 1) return;
    src_lay = lay; src_x = x; src_y = y;
    {   /* PS2_PASS_MASK `copy`: only the eye's crop (the imageRect ps2_xr_frame_end submits) and a margin of w / 64 px */
        extern int ps2_vk_pm_copy_on(void);
        int r[4];
        memset(copy_rgn, 0, sizeof copy_rgn);
        if (ps2_vk_pm_copy_on() && ps2_xr_eye_crop(eye, xr_seq, w, h, r)) {
            int m = (int)(w / 64u);
            copy_rgn[0] = r[0] - m < 0 ? 0 : r[0] - m; copy_rgn[1] = r[1] - m < 0 ? 0 : r[1] - m;
            copy_rgn[2] = r[2] + m > (int)w ? (int)w : r[2] + m; copy_rgn[3] = r[3] + m > (int)h ? (int)h : r[3] + m;
        }
    }
    int ok = copy_into(cmd, eye + 1, src, src_fmt, w, h);
    memset(copy_rgn, 0, sizeof copy_rgn);
    src_lay = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; src_x = src_y = 0;
    if (!ok) return;
    xr_eye[eye].have = 1; xr_eye[eye].pose = xr_tags[slot].eyep[eye]; xr_eye[eye].seq = xr_seq;
    xr_eye[eye].locked = xr_tags[slot].locked;
    xr_eye[eye].tan_h = xr_tags[slot].tan_h; xr_eye[eye].tan_v = xr_tags[slot].tan_v;
    xlayer = 2;
}
/* PS2_PERF_EYE_COPY_PAIR (perf4_passes 2026-11-03; ps2_video.c decides, default 1): both eyes of a multiview frame
   (layers 0 / 1 of src) into their swapchain images with one barrier call before the two copies (the source and both
   images) and one after them, instead of two before and two after each eye's copy: the same copies (copy_cmd, the same
   crops as ps2_xr_record_copy_rt_eye), the same layouts before and after. 1 = recorded for the eyes whose images
   could be acquired; 0 = nothing recorded (the caller copies per eye as before). */
static void pair_fill(VkImageMemoryBarrier2 *b, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 ss,
                      VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    memset(b, 0, sizeof *b);
    b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b->srcStageMask = ss; b->srcAccessMask = sa; b->dstStageMask = ds; b->dstAccessMask = da;
    b->oldLayout = from; b->newLayout = to;
    b->srcQueueFamilyIndex = b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b->image = img; b->subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b->subresourceRange.levelCount = 1; b->subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;   /* (as img_barrier) */
}
int ps2_xr_record_copy_rt_pair(VkCommandBuffer cmd, VkImage src, VkFormat src_fmt, VkImageLayout lay,
                               int32_t x, int32_t y, uint32_t w, uint32_t h, u32 xr_seq) {
    extern int ps2_vk_pm_copy_on(void);
    int slot = (int)(xr_seq % XR_TAGS);
    if (!ps2_xr_sync_frame(xr_seq)) return 0;
    int32_t rgn[2][4]; VkImage dst[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE }; int ok[2] = { 0, 0 }, n = 0;
    for (int eye = 0; eye < 2; eye++) {
        xr_chain *c = &xc[eye + 1];
        int r[4];
        memset(rgn[eye], 0, sizeof rgn[eye]);
        if (ps2_vk_pm_copy_on() && ps2_xr_eye_crop(eye, xr_seq, w, h, r)) {   /* (PS2_PASS_MASK `copy`, as per eye) */
            int m = (int)(w / 64u);
            rgn[eye][0] = r[0] - m < 0 ? 0 : r[0] - m; rgn[eye][1] = r[1] - m < 0 ? 0 : r[1] - m;
            rgn[eye][2] = r[2] + m > (int)w ? (int)w : r[2] + m; rgn[eye][3] = r[3] + m > (int)h ? (int)h : r[3] + m;
        }
        if (!chain_fit(c, src_fmt, w, h)) continue;
        c->cw = w; c->ch = h;
        if (xacq_mask & (1 << (eye + 1))) continue;          // one image per chain per frame
        XrSwapchainImageAcquireInfo ai; memset(&ai, 0, sizeof ai); ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        if (XR_FAILED(xrAcquireSwapchainImage(c->sc, &ai, &xacq_index))) continue;
        XrSwapchainImageWaitInfo wi; memset(&wi, 0, sizeof wi); wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(c->sc, &wi);
        xacq_mask |= 1 << (eye + 1);
        dst[eye] = c->img[xacq_index]; ok[eye] = 1; n++;
    }
    if (!n) return 0;
    VkImageMemoryBarrier2 b[3]; uint32_t nb = 0;
    pair_fill(&b[nb++], src, lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT);
    for (int eye = 0; eye < 2; eye++)
        if (ok[eye])
            pair_fill(&b[nb++], dst[eye], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkDependencyInfo di; memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO; di.imageMemoryBarrierCount = nb; di.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd, &di);
    for (int eye = 0; eye < 2; eye++) {
        if (!ok[eye]) continue;
        src_lay = lay; src_x = x; src_y = y; src_layer = (u32)eye;
        memcpy(copy_rgn, rgn[eye], sizeof copy_rgn);
        copy_cmd(cmd, &xc[eye + 1], src, dst[eye], w, h);
    }
    memset(copy_rgn, 0, sizeof copy_rgn);
    src_lay = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; src_x = src_y = 0; src_layer = 0;
    nb = 0;
    for (int eye = 0; eye < 2; eye++)
        if (ok[eye])
            pair_fill(&b[nb++], dst[eye], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    pair_fill(&b[nb++], src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lay, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
              VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    di.imageMemoryBarrierCount = nb;
    vkCmdPipelineBarrier2(cmd, &di);
    for (int eye = 0; eye < 2; eye++) {
        if (!ok[eye]) continue;
        xr_eye[eye].have = 1; xr_eye[eye].pose = xr_tags[slot].eyep[eye]; xr_eye[eye].seq = xr_seq;
        xr_eye[eye].locked = xr_tags[slot].locked;
        xr_eye[eye].tan_h = xr_tags[slot].tan_h; xr_eye[eye].tan_v = xr_tags[slot].tan_v;
    }
    xlayer = 2;
    return 1;
}
static void record_copy_from(VkCommandBuffer cmd, VkImage src, VkFormat src_fmt, uint32_t w, uint32_t h, u32 xr_seq) {
    if (!xframe_open || !xfs.shouldRender) return;
    int slot = (int)(xr_seq % XR_TAGS);
    int tracked = xmode != PS2_XR_CINEMA && xr_seq && __atomic_load_n(&xr_tags[slot].seq, __ATOMIC_ACQUIRE) == xr_seq
                  && xr_seq_live(xr_seq);                 // ignore stale numbers (a menu kept on screen)
    if (!tracked) {
        int ch = src_lay == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR ? 3 : 0;
        if (copy_into(cmd, ch, src, src_fmt, w, h)) { xlayer = 1; xcinema = ch; }
        return;
    }
    int e = xr_tags[slot].eye;
    int target = e == 1 ? 2 : 1;                         // mono (-1) and left use chain 1, right uses chain 2
    if (!copy_into(cmd, target, src, src_fmt, w, h)) return;
    if (e < 0) {                                         // mono: chain 1 serves both eyes
        xr_eye[1].have = 0;
        e = 0;
    }
    xr_eye[e].have = 1; xr_eye[e].pose = xr_tags[slot].pose; xr_eye[e].seq = xr_seq;
    xr_eye[e].locked = xr_tags[slot].locked;
    xr_eye[e].tan_h = xr_tags[slot].tan_h; xr_eye[e].tan_v = xr_tags[slot].tan_v;
    xlayer = 2;
}

// Renderer: nothing new for the headset this frame (a field whose image cannot be split per eye): submit both eyes'
// last images again with the poses they were rendered with; the compositor reprojects them.
void ps2_xr_repeat_frame(void) {
    if (xframe_open && xlayer == 0 && (xr_eye[0].have || xr_eye[1].have)) xlayer = 2;
}

// The FOV the game image covers: the camera hook reshapes the game's virtual screen so the image spans exactly
// +-tan_h horizontally and +-tan_v vertically.
static XrFovf image_fov(float th, float tv) {
    XrFovf f; f.angleLeft = -atanf(th); f.angleRight = atanf(th); f.angleUp = atanf(tv); f.angleDown = -atanf(tv);
    return f;
}

/* The quit prompt (ps2_quit.c) as its own quad layer, head-locked in front of the eyes over whatever the game shows
   (menus on the cinema screen, flight, the cockpit). The image comes from the CPU: a host-visible staging buffer
   (three slots, rotated whenever the image changes, so a slot is never rewritten while a frame in flight copies it)
   is copied into the acquired swapchain image every frame the prompt is up. */
#define OVL_SLOTS 3
static xr_chain xovl;
static int xovl_fail, xovl_show, xovl_acq, xovl_slot = -1, xovl_bgra;
static u32 xovl_w, xovl_h, xovl_gen;
static VkBuffer xovl_buf; static VkDeviceMemory xovl_mem; static u8 *xovl_map;
static int ovl_setup(u32 w, u32 h) {
    uint32_t nf = 0; xrEnumerateSwapchainFormats(xs, 0, &nf, NULL);
    int64_t fmts[64]; if (nf > 64) nf = 64;
    xrEnumerateSwapchainFormats(xs, nf, &nf, fmts);
    static const VkFormat want[4] = { VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_FORMAT_B8G8R8A8_UNORM };
    VkFormat pick = VK_FORMAT_UNDEFINED;
    for (int k = 0; k < 4 && pick == VK_FORMAT_UNDEFINED; k++)
        for (uint32_t i = 0; i < nf; i++) if ((VkFormat)fmts[i] == want[k]) { pick = want[k]; break; }
    if (pick == VK_FORMAT_UNDEFINED) { ps2_log("xr: prompt layer: no 8-bit swapchain format"); return -1; }
    xovl_bgra = pick == VK_FORMAT_B8G8R8A8_SRGB || pick == VK_FORMAT_B8G8R8A8_UNORM;
    XrSwapchainCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = pick; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(xs, &ci, &xovl.sc);
    if (XR_FAILED(r)) { XRLOG(r, "xrCreateSwapchain (prompt layer)"); return -1; }
    XrSwapchainImageVulkanKHR imgs[8]; memset(imgs, 0, sizeof imgs);
    for (int i = 0; i < 8; i++) imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    xrEnumerateSwapchainImages(xovl.sc, 0, &xovl.nimg, NULL); if (xovl.nimg > 8) xovl.nimg = 8;
    xrEnumerateSwapchainImages(xovl.sc, xovl.nimg, &xovl.nimg, (XrSwapchainImageBaseHeader *)imgs);
    for (uint32_t i = 0; i < xovl.nimg; i++) xovl.img[i] = imgs[i].image;
    xovl.w = w; xovl.h = h; xovl.fmt = pick;
    VkBufferCreateInfo bi; memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; bi.size = (VkDeviceSize)w * h * 4u * OVL_SLOTS;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk_dev, &bi, NULL, &xovl_buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(vk_dev, xovl_buf, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(vk_phys, &mp);
    const VkMemoryPropertyFlags need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) mt = i;
    VkMemoryAllocateInfo ai; memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO; ai.allocationSize = mr.size; ai.memoryTypeIndex = mt;
    if (mt == UINT32_MAX || vkAllocateMemory(vk_dev, &ai, NULL, &xovl_mem) != VK_SUCCESS) return -1;
    vkBindBufferMemory(vk_dev, xovl_buf, xovl_mem, 0);
    if (vkMapMemory(vk_dev, xovl_mem, 0, VK_WHOLE_SIZE, 0, (void **)&xovl_map) != VK_SUCCESS) return -1;
    xovl_w = w; xovl_h = h;
    ps2_log("xr: prompt layer %ux%u format %d, %u images", w, h, (int)pick, xovl.nimg);
    return 0;
}
void ps2_xr_record_overlay(VkCommandBuffer cmd) {
    xovl_show = 0;
    if (!xframe_open || !xfs.shouldRender || !xs || xovl_fail || !vk_dev) return;
    const u8 *px; u32 w, h, gen;
    if (!ps2_quit_overlay_image(&px, &w, &h, &gen)) return;
    if (!xovl.sc && ovl_setup(w, h) != 0) { xovl_fail = 1; ps2_log("xr: prompt layer unavailable"); return; }
    if (w != xovl_w || h != xovl_h) return;
    if (gen != xovl_gen || xovl_slot < 0) {                 /* a new image: into the next staging slot */
        xovl_slot = (xovl_slot + 1) % OVL_SLOTS;
        u8 *d = xovl_map + (size_t)xovl_slot * w * h * 4u;
        if (xovl_bgra) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                d[4 * i] = px[4 * i + 2]; d[4 * i + 1] = px[4 * i + 1]; d[4 * i + 2] = px[4 * i]; d[4 * i + 3] = px[4 * i + 3];
            }
        } else memcpy(d, px, (size_t)w * h * 4u);
        xovl_gen = gen;
    }
    uint32_t idx;
    XrSwapchainImageAcquireInfo ai; memset(&ai, 0, sizeof ai); ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    if (XR_FAILED(xrAcquireSwapchainImage(xovl.sc, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi; memset(&wi, 0, sizeof wi);
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO; wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(xovl.sc, &wi);
    xovl_acq = 1;
    VkImage dst = xovl.img[idx];
    img_barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy bc; memset(&bc, 0, sizeof bc);
    bc.bufferOffset = (VkDeviceSize)xovl_slot * w * h * 4u;
    bc.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; bc.imageSubresource.layerCount = 1;
    bc.imageExtent.width = w; bc.imageExtent.height = h; bc.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, xovl_buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    img_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    xovl_show = 1;
}

/* ---- Frame depth to the headset (PS2_XR_DEPTH): the depth swapchains and the layer's depth info ----
   One depth swapchain per eye, the size of that eye's colour chain (made again with it), usage depth attachment +
   sampled, one face, one layer, no MSAA; image views made here. Each frame an eye's colour image is copied the renderer
   asks for its depth image (ps2_xr_depth_acquire, right after the copy), writes the same w x h pixels at (0, 0) with a
   depth-only pass and leaves it in DEPTH_STENCIL_ATTACHMENT_OPTIMAL; ps2_xr_frame_end releases it with the colour.
   The depth info goes on both views or neither, with each view's own crop rectangle: not with the head-locked image
   (stick head look: VIEW space, nothing to reproject), not while the quit prompt / settings quad is up (the Meta PC
   runtime depth-tests quad layers against the projection's depth -- iRacing, OpenKneeboard, UEVR: the cockpit would cut
   the prompt), not when an eye's colour image went out without its depth (A/B off blocks, a failed pass). Repeated
   frames keep the last released pair. */
typedef struct { XrSwapchain sc; VkImage img[8]; VkImageView view[8]; uint32_t nimg, w, h; VkFormat fmt; } xd_chain;
static xd_chain xd[2];
static int xd_acq[2], xd_have[2], xd_fail;
static uint32_t xd_idx[2];
static u64 xd_sub, xd_skip[4];
static double xd_acq_ms; static u64 xd_acq_n;          /* host time of the depth images' acquire + wait + release */
static XrCompositionLayerDepthInfoKHR xd_info[2];
static struct { XrSwapchain sc; VkImageView view[8]; uint32_t n; u64 after; } xd_retired[4];
int ps2_xr_depth_on(void) { return xr_on == 1 && xd_use && !xd_fail && xs && xmode == PS2_XR_STEREO; }
/* renderer, before a stereo frame: its depth would be used (not a head-locked image: stick head look outside the
   cockpit shows the image in VIEW space, with nothing to reproject) */
int ps2_xr_depth_frame(u32 seq) {
    if (!ps2_xr_depth_on()) return 0;
    int slot = (int)(seq % XR_TAGS);
    return !(ps2_xr_headlook_stick() && xview && xr_view_eye_ok && seq && xr_tags[slot].seq == seq && xr_tags[slot].locked);
}
static int xd_has_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
static void xd_retire_sweep(int all) {
    for (int i = 0; i < 4; i++)
        if (xd_retired[i].sc && (all || xr_frames_begun > xd_retired[i].after)) {
            for (uint32_t k = 0; k < xd_retired[i].n; k++) if (xd_retired[i].view[k]) vkDestroyImageView(vk_dev, xd_retired[i].view[k], NULL);
            xrDestroySwapchain(xd_retired[i].sc);
            memset(&xd_retired[i], 0, sizeof xd_retired[i]);
        }
}
static void xd_chain_destroy(xd_chain *d) {
    if (d->sc) {
        int k;
        for (k = 0; k < 4 && xd_retired[k].sc; k++) ;
        if (k == 4) { xd_retire_sweep(1); k = 0; }
        xd_retired[k].sc = d->sc; xd_retired[k].n = d->nimg; xd_retired[k].after = xr_frames_begun + 10;
        memcpy(xd_retired[k].view, d->view, sizeof d->view);
    }
    memset(d, 0, sizeof *d);
}
static VkFormat xd_pick(void) {
    uint32_t nf = 0; xrEnumerateSwapchainFormats(xs, 0, &nf, NULL);
    int64_t fmts[64]; if (nf > 64) nf = 64;
    xrEnumerateSwapchainFormats(xs, nf, &nf, fmts);
    static const VkFormat dfs[5] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32,
                                     VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT_S8_UINT };
    char list[256]; int o = 0; list[0] = 0;
    VkFormat first = VK_FORMAT_UNDEFINED; int have_force = 0, have32 = 0;
    for (uint32_t i = 0; i < nf; i++)
        for (int k = 0; k < 5; k++)
            if ((VkFormat)fmts[i] == dfs[k]) {
                if (first == VK_FORMAT_UNDEFINED) first = dfs[k];
                if (dfs[k] == xd_force) have_force = 1;
                if (dfs[k] == VK_FORMAT_D32_SFLOAT) have32 = 1;
                if (o < (int)sizeof list - 8) o += snprintf(list + o, sizeof list - (size_t)o, " %d", (int)dfs[k]);
            }
    VkFormat pick = have_force ? xd_force : have32 ? VK_FORMAT_D32_SFLOAT : first;
    ps2_log("xr: depth formats offered (runtime order):%s; using %d%s", o ? list : " none", (int)pick,
            xd_force != VK_FORMAT_UNDEFINED && !have_force ? " (PS2_XR_DEPTH_FORMAT not offered)" : "");
    return pick;
}
static int xd_chain_create(xd_chain *d, uint32_t w, uint32_t h) {
    static VkFormat fmt = VK_FORMAT_UNDEFINED;
    if (fmt == VK_FORMAT_UNDEFINED) fmt = xd_pick();
    if (fmt == VK_FORMAT_UNDEFINED) return -1;
    XrSwapchainCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    ci.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = fmt; ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(xs, &ci, &d->sc);
    if (XR_FAILED(r)) { XRLOG(r, "xrCreateSwapchain (depth)"); d->sc = XR_NULL_HANDLE; return -1; }
    XrSwapchainImageVulkanKHR imgs[8]; memset(imgs, 0, sizeof imgs);
    for (int i = 0; i < 8; i++) imgs[i].type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    xrEnumerateSwapchainImages(d->sc, 0, &d->nimg, NULL); if (d->nimg > 8) d->nimg = 8;
    xrEnumerateSwapchainImages(d->sc, d->nimg, &d->nimg, (XrSwapchainImageBaseHeader *)imgs);
    for (uint32_t i = 0; i < d->nimg; i++) {
        d->img[i] = imgs[i].image;
        VkImageViewCreateInfo vi; memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image = d->img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | (xd_has_stencil(fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(vk_dev, &vi, NULL, &d->view[i]) != VK_SUCCESS) {
            ps2_log("xr: depth swapchain image view failed");
            xd_chain_destroy(d);
            return -1;
        }
    }
    d->w = w; d->h = h; d->fmt = fmt;
    ps2_log("xr: depth swapchain %d: %ux%u format %d, %u images", (int)(d - xd), w, h, (int)fmt, d->nimg);
    return 0;
}
/* Renderer thread, right after eye `eye`'s colour copy of this frame: that eye's depth image to write (w x h at (0, 0),
   the colour copy's size), made / remade at the colour chain's size. 1 = acquired: call ps2_xr_depth_written. */
int ps2_xr_depth_acquire(int eye, VkImage *img, VkImageView *view, VkFormat *fmt, uint32_t *w, uint32_t *h) {
    if (!ps2_xr_depth_on() || eye < 0 || eye > 1 || !xframe_open || xd_acq[eye]) return 0;
    xr_chain *c = &xc[eye + 1];
    if (!c->sc || !(xacq_mask & (1 << (eye + 1)))) return 0;           /* no colour image this frame */
    xd_retire_sweep(0);
    xd_chain *d = &xd[eye];
    if (!d->sc || d->w != c->w || d->h != c->h) {
        xd_chain_destroy(d);
        xd_have[eye] = 0;
        if (xd_chain_create(d, c->w, c->h) != 0) {
            xd_fail = 1;
            ps2_log("xr: no depth swapchain: frame depth to the headset off for this session");
            return 0;
        }
    }
    long long t0 = qpc_ns();
    XrSwapchainImageAcquireInfo ai; memset(&ai, 0, sizeof ai); ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    if (XR_FAILED(xrAcquireSwapchainImage(d->sc, &ai, &xd_idx[eye]))) return 0;
    XrSwapchainImageWaitInfo wi; memset(&wi, 0, sizeof wi); wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(d->sc, &wi);
    xd_acq_ms += (double)(qpc_ns() - t0) / 1e6; xd_acq_n++;
    xd_acq[eye] = 1; xd_have[eye] = 0;
    *img = d->img[xd_idx[eye]]; *view = d->view[xd_idx[eye]]; *fmt = d->fmt; *w = c->cw; *h = c->ch;
    return 1;
}
void ps2_xr_depth_written(int eye, int ok) { if (eye >= 0 && eye < 2) xd_have[eye] = ok && xd_acq[eye]; }
/* PS2_PREWARM_TARGETS (qw_startup, 2026-10-24; ps2_video.c prewarm_targets): renderer thread, while a mission loads
   (static screen): the eye chains that a head-tracked w x h image of src_fmt needs, at the size copy_into would make
   them (chain_fit: +25 %, capped at 2048), and with PS2_XR_DEPTH their depth chains, so the first head-tracked frame
   of a flight does not create them (the first-flight hitch: 2-4 missed refreshes in the first 2 s of every session).
   Returns the number of chains made now (0: already there, or no stereo / mono session); -1 a create failed (the
   first tracked frame tries again, as before). */
int ps2_xr_prepare_eye_chains(VkFormat src_fmt, uint32_t w, uint32_t h) {
    if (xr_on != 1 || !xs || !xrunning || (xmode != PS2_XR_STEREO && xmode != PS2_XR_STEREO_ALT && xmode != PS2_XR_MONO)
        || !w || !h)
        return 0;
    int made = 0, fail = 0;
    for (int which = 1; which <= (xmode == PS2_XR_MONO ? 1 : 2); which++) {
        xr_chain *c = &xc[which];
        XrSwapchain before = c->sc;
        if (!chain_fit(c, src_fmt, w, h)) { fail = 1; continue; }
        made += c->sc != before;
    }
    if (!fail && ps2_xr_depth_on()) {
        xd_retire_sweep(0);
        for (int eye = 0; eye < 2; eye++) {
            xr_chain *c = &xc[eye + 1];
            xd_chain *d = &xd[eye];
            if (d->sc && d->w == c->w && d->h == c->h) continue;
            xd_chain_destroy(d);
            xd_have[eye] = 0;
            if (xd_chain_create(d, c->w, c->h) != 0) {
                xd_fail = 1;
                ps2_log("xr: no depth swapchain: frame depth to the headset off for this session");
                fail = 1;
                break;
            }
            made++;
        }
    }
    return fail ? -1 : made;
}
/* PS2_PREWARM_TARGETS, the cinema screen's chain 0 (the render target's display region; qw_cinema's
   PS2_XR_SCREEN_FROM_RT): made once at start-up at the size a 640 x 448 display needs (chain_fit: 2560x2048 at 4x), right
   after the session exists (swapchains may be made before xrBeginSession), so the first logo frame does not make it at
   2048x2048 and the first 640-wide display (the title, the menus) does not make it again. 1 made, 0 there / no session,
   -1 failed (made on first use as before). */
int ps2_xr_prepare_screen_chain(VkFormat src_fmt, uint32_t w, uint32_t h) {
    if (xr_on != 1 || !xs || !w || !h) return 0;
    XrSwapchain before = xc[0].sc;
    if (!chain_fit(&xc[0], src_fmt, w, h)) return -1;
    return xc[0].sc != before;
}
/* renderer: the crop (x0, y0, x1, y1 of the W x H eye image) that eye `eye`'s projection view of frame `seq` will
   submit -- the arithmetic of ps2_xr_frame_end; 0 = the whole image. The depth outside it is never shown. */
int ps2_xr_eye_crop(int eye, u32 seq, uint32_t W, uint32_t H, int r[4]) {
    static int crop = -1;
    if (crop < 0) { const char *e = getenv("PS2_XR_EYE_CROP"); crop = !(e && *e == '0'); }
    int slot = (int)(seq % XR_TAGS);
    if (!crop || !xr_native_fov_ok || eye < 0 || eye > 1 || !seq || xr_tags[slot].seq != seq) return 0;
    float th = xr_tags[slot].tan_h, tv = xr_tags[slot].tan_v, Wf = (float)W, Hf = (float)H;
    if (!(th > 0.0f && tv > 0.0f && Wf > 8.0f && Hf > 8.0f)) return 0;
    float l = tanf(xr_native_fov[eye].angleLeft), rr = tanf(xr_native_fov[eye].angleRight);
    float u = tanf(xr_native_fov[eye].angleUp), d = tanf(xr_native_fov[eye].angleDown);
    if (l < -th) l = -th;
    if (rr > th) rr = th;
    if (u > tv) u = tv;
    if (d < -tv) d = -tv;
    if (!(rr > l && u > d)) return 0;
    int x0 = (int)floorf((l + th) / (2.0f * th) * Wf), x1 = (int)ceilf((rr + th) / (2.0f * th) * Wf);
    int y0 = (int)floorf((tv - u) / (2.0f * tv) * Hf), y1 = (int)ceilf((tv - d) / (2.0f * tv) * Hf);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)W) x1 = (int)W;
    if (y1 > (int)H) y1 = (int)H;
    if (!(x1 - x0 > 8 && y1 - y0 > 8)) return 0;
    r[0] = x0; r[1] = y0; r[2] = x1; r[3] = y1;
    return 1;
}
/* frame end, before the colour images' release: an eye whose colour went out without depth loses its depth; the
   acquired depth images are released */
static void xd_release(void) {
    for (int e = 0; e < 2; e++) {
        if ((xacq_mask & (1 << (e + 1))) && !xd_acq[e]) xd_have[e] = 0;
        if (xd_acq[e] && xd[e].sc) {
            long long t0 = qpc_ns();
            XrSwapchainImageReleaseInfo ri; memset(&ri, 0, sizeof ri); ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            xrReleaseSwapchainImage(xd[e].sc, &ri);
            xd_acq_ms += (double)(qpc_ns() - t0) / 1e6;
        }
        xd_acq[e] = 0;
    }
}
/* frame end: the depth info on both projection views (src[i] = the eye whose images view i shows) */
static void xd_chain_views(XrCompositionLayerProjectionView *pv, const int *src, int locked) {
    if (!ps2_xr_depth_on()) return;
    int why = 0;
    if (locked) why = 1;
    else if (xovl_show && xovl.sc && xview) why = 2;
    else for (int i = 0; i < 2; i++) if (src[i] < 0 || src[i] > 1 || !xd_have[src[i]] || !xd[src[i]].sc) why = 3;
    if (why) { xd_skip[why]++; return; }
    for (int i = 0; i < 2; i++) {
        XrCompositionLayerDepthInfoKHR *di = &xd_info[i];
        memset(di, 0, sizeof *di);
        di->type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
        di->subImage.swapchain = xd[src[i]].sc;
        di->subImage.imageRect = pv[i].subImage.imageRect;          /* the colour's crop: same pixels */
        di->subImage.imageArrayIndex = 0;
        di->minDepth = 0.0f; di->maxDepth = 1.0f;
        di->nearZ = xd_far_m > 0.0f ? xd_far_m : FLT_MAX;            /* reversed: depth 0 far away, 1 at the near plane */
        di->farZ = xd_near_m;
        pv[i].next = di;
    }
    xd_sub++;
    if (xd_sub == 1 || (xd_sub % 3000u) == 0u)
        ps2_log("xr: %llu frames with depth (without: %llu head-locked, %llu with the prompt layer, %llu missing an eye's depth); "
                "depth images' acquire + wait + release %.1f us per eye image", (unsigned long long)xd_sub,
                (unsigned long long)xd_skip[1], (unsigned long long)xd_skip[2], (unsigned long long)xd_skip[3],
                xd_acq_n ? xd_acq_ms * 1e3 / (double)xd_acq_n : 0.0);
}
static void xd_shutdown(void) {
    for (int e = 0; e < 2; e++) xd_chain_destroy(&xd[e]);
    xd_retire_sweep(1);
}

// After vkQueueSubmit of the frame's command buffer: release the image and end the XR frame.
/* ACZ_VR_PAUSE_FOLLOW (bug #313, 2026-11-05, M1 11:05:39 with the pause menu and then the ENHANCEMENTS page up: "I turn my head
   and this menu still disappears, exactly its central part"; the third time after the page and the card, #306). While
   the game is paused (ps2_quit.c: PS2_QUIT_GAME_PAUSED; the pause frame is not head-tracked: "menu over the flight: static
   screen", so it is the cinema screen's quad, not hud_place's plane) the screen's quad and the ENHANCEMENTS page on it
   swing round the seat with the head once the head is more than this many degrees from their centre (yaw only, 0.1 s
   to catch up, the card's rule), one yaw for both so they stay stacked. Default 20; 0 = fixed as before this row. Outside a
   pause nothing moves: the menus between missions and the movies keep the screen; the flight is a projection layer.
   The pause ending: the frames that follow are static flight frames until tracking engages (ACZ_VR_ENGAGE_FIELDS, 60 fields
   after the HUD manager's first run) and still on this quad, so it stays where it is, with no follow, until the first
   head-tracked frame takes over (nothing of it remains then); if none comes within 2 s (QUIT MISSION to the menus) it
   eases back onto the screen's place. The TOUCH CONTROLS card (ACZ_TOUCH_CARD_FOLLOW) takes this number when its own
   is not set, and starts on this yaw when it opens over a swung pause; its own switch wins for the card: =0 keeps it
   on the screen's place, and while such a fixed card is up the layer under it holds still. */
static float xpf_dz = -1.0f, xpf_yaw;
static int xpf_on;                                 /* the paused layer's yaw is in charge (else xquad as it is) */
static double xpf_end_t;
static float xcard_follow(void) {                  /* the card's number (ACZ_TOUCH_CARD_FOLLOW; unset: the pause's, 20 if that is 0) */
    static float dz = -1.0f;
    if (dz < 0.0f) {
        const char *e = getenv("ACZ_TOUCH_CARD_FOLLOW");
        dz = e && *e ? (float)atof(e) : xpf_dz > 0.0f ? xpf_dz : 20.0f;
        if (!(dz >= 0.0f)) dz = 20.0f;
    }
    return dz;
}
static float xr_head_yaw(float fallback) {
    pthread_mutex_lock(&xr_lock);
    XrQuaternionf q = xr_latest.eye[0].orientation;
    pthread_mutex_unlock(&xr_lock);
    float fx = -(2 * (q.x * q.z + q.w * q.y)), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    return fx * fx + fz * fz > 1e-6f ? atan2f(-fx, -fz) : fallback;
}
static float xpf_wrap(float d) {
    while (d > 3.14159265f) d -= 6.2831853f;
    while (d < -3.14159265f) d += 6.2831853f;
    return d;
}
static void xpf_step(void) {
    if (xpf_dz < 0.0f) {
        const char *e = getenv("ACZ_VR_PAUSE_FOLLOW");
        xpf_dz = e && *e ? (float)atof(e) : 20.0f;
        if (!(xpf_dz >= 0.0f)) xpf_dz = 20.0f;
        ps2_log("xr: the paused layer %s (ACZ_VR_PAUSE_FOLLOW); the TOUCH CONTROLS card %s (ACZ_TOUCH_CARD_FOLLOW)",
                xpf_dz > 0.0f ? "follows the head beyond a set angle" : "stays on the cinema screen",
                xcard_follow() > 0.0f ? "follows too" : "stays on the cinema screen");
    }
    if (xpf_dz <= 0.0f || !xlocal) return;
    float sq = 2.0f * atan2f(xquad.orientation.y, xquad.orientation.w);
    float dt = (float)xfs.predictedDisplayPeriod * 1e-9f;
    if (dt <= 0.0f) dt = 0.011f;
    if (ps2_quit_game_state() == PS2_QUIT_GAME_PAUSED) {
        if (!xpf_on) { xpf_on = 1; xpf_yaw = sq; }
        xpf_end_t = 0.0;
        if (ps2_quit_touch_card_visible() && xcard_follow() <= 0.0f) return;   /* (a fixed card: the layer holds under it) */
        float hy = xr_head_yaw(xpf_yaw), d = xpf_wrap(hy - xpf_yaw), over = fabsf(d) - xpf_dz * 0.0174533f;
        if (over > 0.0f) xpf_yaw += (d > 0.0f ? over : -over) * (1.0f - expf(-dt / 0.1f));
        static int dbg = -1; static u32 n;
        if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_ENH"); dbg = e && *e == '1'; }
        if (dbg && (n++ % 30u) == 0u)
            ps2_log("xr: paused layer: head yaw %.1f, its centre at yaw %.1f, seen %.1f deg off the head's forward; the screen at "
                    "%.1f (+ = left; frame layer %d)", hy * 57.29578f, xpf_yaw * 57.29578f, xpf_wrap(hy - xpf_yaw) * 57.29578f,
                    sq * 57.29578f, xlayer);
        return;
    }
    if (!xpf_on) return;
    if (xlayer == 2) {                                 /* head-tracked again: the quad is gone, the flight as ever */
        ps2_log("xr: the pause is over: the first head-tracked frame takes over from the paused layer (it stood %.1f deg from "
                "the screen's place, %.2f s after the pause ended)", xpf_wrap(xpf_yaw - sq) * 57.29578f,
                xpf_end_t > 0.0 ? ps2_wall_seconds() - xpf_end_t : 0.0);
        xpf_on = 0;
        return;
    }
    double now = ps2_wall_seconds();
    if (xpf_end_t <= 0.0) {
        xpf_end_t = now;
        ps2_log("xr: the pause ended: the paused layer holds at %.1f deg from the screen's place until the first head-tracked "
                "frame", xpf_wrap(xpf_yaw - sq) * 57.29578f);
    }
    if (now - xpf_end_t > 2.0) {                       /* no flight came: back onto the screen, no jump */
        float d = xpf_wrap(sq - xpf_yaw);
        xpf_yaw += d * (1.0f - expf(-dt / 0.15f));
        if (fabsf(d) < 0.0035f) {
            xpf_on = 0;
            ps2_log("xr: the paused layer is back on the screen's place %.1f s after the pause ended", now - xpf_end_t);
        }
    }
}
/* the screen's quad as the paused layer has it: turned about the seat, xquad itself at yaw = the screen's */
static XrPosef xpf_pose(void) {
    if (!xpf_on) return xquad;
    float sq = 2.0f * atan2f(xquad.orientation.y, xquad.orientation.w);
    XrPosef o = xquad;
    float seat_x = xquad.position.x + sinf(sq) * xscreen_d, seat_z = xquad.position.z + cosf(sq) * xscreen_d;
    o.orientation.x = 0.0f; o.orientation.y = sinf(xpf_yaw * 0.5f); o.orientation.z = 0.0f; o.orientation.w = cosf(xpf_yaw * 0.5f);
    o.position.x = seat_x - sinf(xpf_yaw) * xscreen_d;
    o.position.z = seat_z - cosf(xpf_yaw) * xscreen_d;
    return o;
}
void ps2_xr_frame_end(void) {
    if (!xframe_open) return;
    xd_release();                                       /* (PS2_XR_DEPTH: before the colour images' mask is cleared) */
    for (int k = 0; k < 4; k++)
        if (xacq_mask & (1 << k)) {
            XrSwapchainImageReleaseInfo ri; memset(&ri, 0, sizeof ri); ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            xrReleaseSwapchainImage(xc[k].sc, &ri);
        }
    xacq_mask = 0;
    if (xovl_acq) {
        XrSwapchainImageReleaseInfo ri; memset(&ri, 0, sizeof ri); ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
        xrReleaseSwapchainImage(xovl.sc, &ri);
        xovl_acq = 0;
    }
    XrCompositionLayerQuad quad; memset(&quad, 0, sizeof quad);
    XrCompositionLayerProjection proj; memset(&proj, 0, sizeof proj);
    XrCompositionLayerProjectionView pv[2]; memset(pv, 0, sizeof pv);
    int pv_src[2] = { -1, -1 };                         /* (PS2_XR_DEPTH: the eye whose images each view shows) */
    const XrCompositionLayerBaseHeader *layers[2];
    uint32_t nl = 0;
    /* A frame with no new image (renderer skipped the field, swapchain being recreated, a held stereo field) repeats
       the last layer: an empty layer list would blank the display for a frame (a black flash). */
    static int xlast_layer, xlast_cinema;
    if (xlayer == 0 && xfs.shouldRender && xlast_layer) { xlayer = xlast_layer; xcinema = xlast_cinema; }
    xpf_step();                                         /* ACZ_VR_PAUSE_FOLLOW (bug #313) */
    if (xlayer == 1 && xc[xcinema].sc) {
        xr_chain *qc = &xc[xcinema];
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD; quad.space = xlocal; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = qc->sc;
        quad.subImage.imageRect.extent.width = (int32_t)qc->cw; quad.subImage.imageRect.extent.height = (int32_t)qc->ch;
        quad.pose = xpf_pose(); quad.size.width = xscreen_w; quad.size.height = xscreen_w * 0.75f;   /* PS2 display is 4:3 */
        layers[0] = (const XrCompositionLayerBaseHeader *)&quad; nl = 1; xsub_quad++;
        xr_eye[0].have = xr_eye[1].have = 0;   /* back to head tracking: never resurrect pre-menu eye images */
    } else if (xlayer == 2) {
        for (int i = 0; i < 2; i++) {
            // Each eye shows its own latest image; until both exist (or in mono, where chain 1 holds the image for
            // both eyes) an eye borrows the other's image and pose.
            int src = i;
            if (!xr_eye[src].have || !xc[src + 1].sc) src = 1 - i;
            if (!xr_eye[src].have || !xc[src + 1].sc) continue;
            xr_chain *c = &xc[src + 1];
            pv_src[i] = src;
            pv[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            pv[i].pose = xr_eye[src].pose;
            pv[i].fov = image_fov(xr_eye[src].tan_h, xr_eye[src].tan_v);
            pv[i].subImage.swapchain = c->sc;
            pv[i].subImage.imageRect.extent.width = (int32_t)c->cw; pv[i].subImage.imageRect.extent.height = (int32_t)c->ch;
            {   static int crop = -1;
                if (crop < 0) { const char *e = getenv("PS2_XR_EYE_CROP"); crop = !(e && *e == '0'); }
                float th = xr_eye[src].tan_h, tv = xr_eye[src].tan_v, W = (float)c->cw, H = (float)c->ch;
                if (crop && xr_native_fov_ok && th > 0.0f && tv > 0.0f && W > 8.0f && H > 8.0f) {
                    /* eye i's frustum in tangents, clamped to what the image covers */
                    float l = tanf(xr_native_fov[i].angleLeft), r = tanf(xr_native_fov[i].angleRight);
                    float u = tanf(xr_native_fov[i].angleUp), d = tanf(xr_native_fov[i].angleDown);
                    if (l < -th) l = -th;
                    if (r > th) r = th;
                    if (u > tv) u = tv;
                    if (d < -tv) d = -tv;
                    if (r > l && u > d) {
                        int x0 = (int)floorf((l + th) / (2.0f * th) * W), x1 = (int)ceilf((r + th) / (2.0f * th) * W);
                        int y0 = (int)floorf((tv - u) / (2.0f * tv) * H), y1 = (int)ceilf((tv - d) / (2.0f * tv) * H);
                        if (x0 < 0) x0 = 0;
                        if (y0 < 0) y0 = 0;
                        if (x1 > (int)W) x1 = (int)W;
                        if (y1 > (int)H) y1 = (int)H;
                        if (x1 - x0 > 8 && y1 - y0 > 8) {
                            /* exact tangents of the integer rectangle so the angle -> pixel mapping is unchanged */
                            pv[i].fov.angleLeft = atanf(-th + 2.0f * th * (float)x0 / W);
                            pv[i].fov.angleRight = atanf(-th + 2.0f * th * (float)x1 / W);
                            pv[i].fov.angleUp = atanf(tv - 2.0f * tv * (float)y0 / H);
                            pv[i].fov.angleDown = atanf(tv - 2.0f * tv * (float)y1 / H);
                            pv[i].subImage.imageRect.offset.x = x0; pv[i].subImage.imageRect.offset.y = y0;
                            pv[i].subImage.imageRect.extent.width = x1 - x0; pv[i].subImage.imageRect.extent.height = y1 - y0;
                            static int said;
                            if (said < 2) {
                                said++;
                                ps2_log("xr: eye %d submitted as crop x %d..%d y %d..%d of %.0fx%.0f, fov L %.1f R %.1f U %.1f D %.1f deg",
                                        i, x0, x1, y0, y1, W, H, pv[i].fov.angleLeft * 57.2958f, pv[i].fov.angleRight * 57.2958f,
                                        pv[i].fov.angleUp * 57.2958f, pv[i].fov.angleDown * 57.2958f);
                            }
                        }
                    }
                }
            }
        }
        int locked = ps2_xr_headlook_stick() && xview && xr_view_eye_ok && xr_eye[0].locked;
        if (locked) { pv[0].pose = xr_view_eye[0]; pv[1].pose = xr_view_eye[1]; }   /* head-locked image */
        if (pv[0].subImage.swapchain && pv[1].subImage.swapchain) {
            proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION; proj.space = locked ? xview : xlocal; proj.viewCount = 2; proj.views = pv;
            xd_chain_views(pv, pv_src, locked);         /* (PS2_XR_DEPTH) */
            layers[0] = (const XrCompositionLayerBaseHeader *)&proj; nl = 1; xsub_proj++;
        }
    }
    if (nl) { xsubmitted++; xlast_layer = xlayer; xlast_cinema = xcinema; }
    if (xquad_next_on && nl && (xlayer == 2 || ++xquad_next_wait > 600)) {   /* (recenter_hand_over: the screen follows) */
        xquad = xquad_next;
        xquad_next_on = 0;
        ps2_log("xr: the cinema screen moved to the recentred place (%s, after %d more frames on the screen)",
                xlayer == 2 ? "the first head-tracked frame went out" : "no head-tracked frame came", xquad_next_wait);
    }
    XrCompositionLayerQuad oq; memset(&oq, 0, sizeof oq);
    /* The ENHANCEMENTS page in the world (bug #136, "better in the normal menu, not following me around"): head-locked
       it followed every head movement. ACZ_ENH_PAGE_PLACE: screen (default) = on the cinema screen, where the game's
       own menus are (the pause menu the page replaces, the menus between missions): the screen's pose in LOCAL space,
       moved with it by a recentre, at the same angular size as before (ACZ_ENH_PAGE_W at 1.1 m -> x screen distance /
       1.1: 3.4 m at 2.5 m, 69 deg wide, the menu font ~2.1 deg); front = in front of the head as it was when the page
       opened (yaw only, 1.1 m ahead, a little low), then fixed in the world; head = fixed to the head (before). The
       quit prompt stays head-locked.
       ACZ_TOUCH_CARD_FOLLOW (bug #306, 2026-11-05, "I turn my head to the right and the thing disappears": the TOUCH
       CONTROLS card is this page's quad, so it was fixed on the screen: it does not close and no pose is stale, it stays
       behind): a card is read while the hands try the controls, so with ACZ_ENH_PAGE_PLACE=screen it starts on the screen
       and, once the head is more than this many degrees (default 20; 0 = fixed like the page) from its centre, swings
       round the seat with the head (yaw only, 0.1 s to catch up), its edge still in view. The page itself stays fixed
       (bug #136's wish) outside a pause; over a pause it swings with the paused layer (ACZ_VR_PAUSE_FOLLOW, bug #313). */
    static int enh_place = -1, enh_was, enh_front_ok;
    static XrPosef enh_pose;
    if (enh_place < 0) {
        const char *e = getenv("ACZ_ENH_PAGE_PLACE");
        enh_place = e && !strcmp(e, "head") ? 0 : e && !strcmp(e, "front") ? 2 : 1;
        ps2_log("xr: ENHANCEMENTS page %s (ACZ_ENH_PAGE_PLACE)", enh_place == 0 ? "fixed to the head"
                : enh_place == 2 ? "where the head looked when it opened, fixed in the world" : "on the cinema screen");
    }
    int enh_now = xovl_show && xovl.sc && xview && ps2_quit_enh_visible();
    if (enh_now && !enh_was && enh_place == 2) {        /* placed once, as the page opens */
        enh_front_ok = 0;
        enh_pose.orientation.x = enh_pose.orientation.y = enh_pose.orientation.z = 0.0f; enh_pose.orientation.w = 1.0f;
        enh_pose.position.x = 0.0f; enh_pose.position.y = -0.06f; enh_pose.position.z = -1.1f;
        XrSpaceLocation loc; memset(&loc, 0, sizeof loc); loc.type = XR_TYPE_SPACE_LOCATION;
        if (!XR_FAILED(xrLocateSpace(xview, xlocal, xfs.predictedDisplayTime, &loc))
            && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
            && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
            XrQuaternionf q = loc.pose.orientation;
            float fx = -(2 * (q.x * q.z + q.w * q.y)), fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
            float l = sqrtf(fx * fx + fz * fz); if (l < 1e-4f) { fx = 0; fz = -1; l = 1; }
            fx /= l; fz /= l;
            float yaw = atan2f(-fx, -fz);
            enh_pose.orientation.y = sinf(yaw * 0.5f); enh_pose.orientation.w = cosf(yaw * 0.5f);
            enh_pose.position.x = loc.pose.position.x + fx * 1.1f;
            enh_pose.position.y = loc.pose.position.y - 0.06f;
            enh_pose.position.z = loc.pose.position.z + fz * 1.1f;
            ps2_log("xr: ENHANCEMENTS page placed at yaw %.1f deg, 1.1 m ahead", yaw * 57.29578f);
            enh_front_ok = 1;                            /* (else head-locked for this opening) */
        }
    }
    enh_was = enh_now;
    if (xovl_show && xovl.sc && xview) {                /* the quit prompt (0.62 m wide, head-locked 1.1 m ahead, a little
                                                           low) or the ENHANCEMENTS page (above) */
        oq.type = XR_TYPE_COMPOSITION_LAYER_QUAD; oq.space = xview; oq.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        oq.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        oq.subImage.swapchain = xovl.sc;
        oq.subImage.imageRect.extent.width = (int32_t)xovl_w; oq.subImage.imageRect.extent.height = (int32_t)xovl_h;
        oq.pose.orientation.w = 1.0f; oq.pose.position.y = -0.06f; oq.pose.position.z = -1.1f;
        float ow = ps2_quit_overlay_width_m();
        static float card_yaw; static int card_was; static u32 card_dbg; static u64 card_last;
        float card_dz = xcard_follow();
        int card_now = enh_now && enh_place == 1 && xlocal && card_dz > 0.0f && ps2_quit_touch_card_visible();
        if (card_now) {                                 /* the screen's place, swung round the seat by card_yaw */
            pthread_mutex_lock(&xr_lock);
            XrQuaternionf hq = xr_latest.eye[0].orientation;
            pthread_mutex_unlock(&xr_lock);
            float sq_yaw = 2.0f * atan2f(xquad.orientation.y, xquad.orientation.w);
            if (!card_was || xsubmitted - card_last > 5u) card_yaw = xpf_on ? xpf_yaw : sq_yaw;   /* (opening: on the screen, or the paused layer) */
            card_last = xsubmitted;
            float fx = -(2 * (hq.x * hq.z + hq.w * hq.y)), fz = -(1 - 2 * (hq.x * hq.x + hq.y * hq.y));
            float hy = fx * fx + fz * fz > 1e-6f ? atan2f(-fx, -fz) : card_yaw;
            float d = hy - card_yaw;
            while (d > 3.14159265f) d -= 6.2831853f;
            while (d < -3.14159265f) d += 6.2831853f;
            float over = fabsf(d) - card_dz * 0.0174533f;
            if (over > 0.0f) {
                float dt = (float)xfs.predictedDisplayPeriod * 1e-9f;
                card_yaw += (d > 0.0f ? over : -over) * (1.0f - expf(-(dt > 0.0f ? dt : 0.011f) / 0.1f));
            }
            oq.space = xlocal;
            oq.pose.orientation.x = 0.0f; oq.pose.orientation.y = sinf(card_yaw * 0.5f); oq.pose.orientation.z = 0.0f;
            oq.pose.orientation.w = cosf(card_yaw * 0.5f);
            oq.pose.position.x = xref.position.x - sinf(card_yaw) * xscreen_d;
            oq.pose.position.y = xquad.position.y;
            oq.pose.position.z = xref.position.z - cosf(card_yaw) * xscreen_d;
            ow *= xscreen_d / 1.1f;
            {   static int dbg = -1;
                if (dbg < 0) { const char *e = getenv("ACZ_DEBUG_ENH"); dbg = e && *e == '1'; }
                if (dbg && (card_dbg++ % 30u) == 0u)
                    ps2_log("xr: TOUCH CONTROLS card: head yaw %.1f, the card's centre at yaw %.1f, seen %.1f deg off the "
                            "head's forward; the screen at %.1f (+ = left)", hy * 57.29578f, card_yaw * 57.29578f,
                            (hy - card_yaw) * 57.29578f, sq_yaw * 57.29578f);
            }
        } else if (enh_now && enh_place == 1 && xlocal) {      /* on the cinema screen (the quad's centre, facing the seat) */
            oq.space = xlocal; oq.pose = ps2_quit_touch_card_visible() ? xquad : xpf_pose();   /* (a fixed card stays) */
            ow *= xscreen_d / 1.1f;
        } else if (enh_now && enh_place == 2 && enh_front_ok && xlocal) {
            oq.space = xlocal; oq.pose = enh_pose;
        }
        card_was = card_now;
        oq.size.width = ow; oq.size.height = ow * (float)xovl_h / (float)xovl_w;
        layers[nl++] = (const XrCompositionLayerBaseHeader *)&oq;
        static int said;
        if (!said) { said = 1; ps2_log("xr: prompt layer shown"); }
        static int enh_dbg = -1; static u32 enh_dbg_n;
        if (enh_dbg < 0) { const char *e = getenv("ACZ_DEBUG_ENH"); enh_dbg = e && *e == '1'; }
        if (enh_dbg && enh_now && (enh_dbg_n++ % 30u) == 0u) {
            /* (ACZ_DEBUG_ENH) the page's centre as the head sees it: constant when head-locked, moving against the
               head's turns when fixed in the world */
            XrSpaceLocation loc; memset(&loc, 0, sizeof loc); loc.type = XR_TYPE_SPACE_LOCATION;
            if (!XR_FAILED(xrLocateSpace(xview, xlocal, xfs.predictedDisplayTime, &loc))
                && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
                float p[3] = { oq.pose.position.x, oq.pose.position.y, oq.pose.position.z };
                if (oq.space == xlocal) {                /* LOCAL -> VIEW: inverse of the head pose */
                    XrQuaternionf q = loc.pose.orientation;
                    float d[3] = { p[0] - loc.pose.position.x, p[1] - loc.pose.position.y, p[2] - loc.pose.position.z };
                    /* rotate d by the conjugate of q */
                    float qx = -q.x, qy = -q.y, qz = -q.z, qw = q.w;
                    float tx = 2.0f * (qy * d[2] - qz * d[1]), ty = 2.0f * (qz * d[0] - qx * d[2]);
                    float tz = 2.0f * (qx * d[1] - qy * d[0]);
                    p[0] = d[0] + qw * tx + (qy * tz - qz * ty);
                    p[1] = d[1] + qw * ty + (qz * tx - qx * tz);
                    p[2] = d[2] + qw * tz + (qx * ty - qy * tx);
                }
                float r = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
                ps2_log("xr: ENHANCEMENTS page centre seen at yaw %.1f pitch %.1f deg, %.2f m (%s space), %.2f m wide",
                        atan2f(-p[0], -p[2]) * 57.29578f, asinf(r > 0.0f ? p[1] / r : 0.0f) * 57.29578f, r,
                        oq.space == xlocal ? "LOCAL" : "VIEW", ow);
            }
        }
    }
    XrFrameEndInfo ei; memset(&ei, 0, sizeof ei); ei.type = XR_TYPE_FRAME_END_INFO;
    ei.displayTime = xtest_real_pdt ? xtest_real_pdt : xfs.predictedDisplayTime;   /* (PS2_XR_TEST_HZ: the runtime's own) */
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nl; ei.layers = layers;
    XrResult r = xrEndFrame(xs, &ei);
    if (XR_FAILED(r)) XRLOG(r, "xrEndFrame");
    xframe_open = 0;
    if (nl && (xsubmitted == 1 || xsubmitted % 600 == 0))
        ps2_log("xr: %llu frames submitted (%llu head-tracked, %llu on the screen); game frames tagged %llu",
                (unsigned long long)xsubmitted, (unsigned long long)xsub_proj, (unsigned long long)xsub_quad,
                (unsigned long long)xr_game_frames);
}

/* Renderer thread, when the renderer ends (window closed, Esc twice, a quit from the controller): end the session the
   way OpenXR asks (request exit, keep submitting empty frames until STOPPING, xrEndSession), then destroy the
   swapchains, spaces, session and instance. Before this the process exited with the session running; the Meta
   runtime then cleaned up in its DLL detach handler, where it could not start a thread and waited 60 s. */
void ps2_xr_shutdown(void) {
    if (!xi) return;
    double t0 = ps2_wall_seconds();
    int frames = 0;
    if (xs) {
        if (xframe_open) {                                  /* a frame begun but not ended: end it empty */
            XrFrameEndInfo ei; memset(&ei, 0, sizeof ei); ei.type = XR_TYPE_FRAME_END_INFO;
            ei.displayTime = xtest_real_pdt ? xtest_real_pdt : xfs.predictedDisplayTime;
            ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
            xrEndFrame(xs, &ei);
            xframe_open = 0;
        }
        if (xrunning) {
            XrResult r = xrRequestExitSession(xs);
            if (XR_FAILED(r)) XRLOG(r, "xrRequestExitSession");
            while (xrunning && ps2_wall_seconds() - t0 < 3.0) {
                poll_events();                              /* STOPPING -> xrEndSession, xrunning = 0 */
                if (!xrunning) break;
                XrFrameWaitInfo wi; memset(&wi, 0, sizeof wi); wi.type = XR_TYPE_FRAME_WAIT_INFO;
                XrFrameState fs; memset(&fs, 0, sizeof fs); fs.type = XR_TYPE_FRAME_STATE;
                if (XR_FAILED(xrWaitFrame(xs, &wi, &fs))) break;
                XrFrameBeginInfo bi; memset(&bi, 0, sizeof bi); bi.type = XR_TYPE_FRAME_BEGIN_INFO;
                if (XR_FAILED(xrBeginFrame(xs, &bi))) break;
                XrFrameEndInfo ei; memset(&ei, 0, sizeof ei); ei.type = XR_TYPE_FRAME_END_INFO;
                ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                xrEndFrame(xs, &ei);
                frames++;
            }
            if (xrunning) { XrResult e = xrEndSession(xs); if (XR_FAILED(e)) XRLOG(e, "xrEndSession"); xrunning = 0; }
        }
        for (int k = 0; k < 4; k++) if (xc[k].sc) { xrDestroySwapchain(xc[k].sc); xc[k].sc = XR_NULL_HANDLE; }
        if (xovl.sc) { xrDestroySwapchain(xovl.sc); xovl.sc = XR_NULL_HANDLE; }
        xd_shutdown();                                  /* (PS2_XR_DEPTH) */
        retire_sweep(1);
        if (xlocal) { xrDestroySpace(xlocal); xlocal = XR_NULL_HANDLE; }
        if (xview) { xrDestroySpace(xview); xview = XR_NULL_HANDLE; }
        for (int h = 0; h < 2; h++)
            for (int k = 0; k < 2; k++) if (xt_space[h][k]) { xrDestroySpace(xt_space[h][k]); xt_space[h][k] = XR_NULL_HANDLE; }
        if (xt_attached) ps2_log("touch: %llu action syncs, %llu haptic calls", (unsigned long long)xt_syncs,
                                 (unsigned long long)xt_haptics);
        xt_attached = 0;
        XrResult r = xrDestroySession(xs);
        if (XR_FAILED(r)) XRLOG(r, "xrDestroySession");
        xs = XR_NULL_HANDLE;
    }
    if (xovl_buf) { vkDestroyBuffer(vk_dev, xovl_buf, NULL); xovl_buf = VK_NULL_HANDLE; }
    if (xovl_mem) { vkFreeMemory(vk_dev, xovl_mem, NULL); xovl_mem = VK_NULL_HANDLE; xovl_map = NULL; }
    XrResult r = xrDestroyInstance(xi);
    if (XR_FAILED(r)) XRLOG(r, "xrDestroyInstance");
    xi = XR_NULL_HANDLE;
    xr_on = 0;
    ps2_log("xr: session ended (%d empty frames) and instance destroyed in %.0f ms", frames,
            (ps2_wall_seconds() - t0) * 1000.0);
}
