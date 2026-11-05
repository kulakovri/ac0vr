#ifndef PS2_VKCEN_H
#define PS2_VKCEN_H
/* PS2_VK_CENSUS (diagnostic, perf4_timeline 2026-11-03): the Vulkan command census of a recorded frame, split by the
   part of the frame that recorded it, and the pass census: every vkCmdBeginRendering with the vkCmdEndRendering before it
   (why the pass starts), every pipeline barrier with its image count and flags, every copy / blit / clear.

   How: the renderer's files (ps2_video.c, ps2_clouds*.c, ps2_xr.c) include this header after <vulkan/vulkan.h>; the
   macros below redirect their vkCmd* calls through a counter. The part is the GPU class of ps2_video.c (the same pass
   stamps as PS2_GPU_CLASSES: game draws by draw class, the HUD (xr class 2), our passes: cockpit model / glass /
   composite, clouds, edge AA, eye copies, frame setup, ...), set by VKCEN_CLASS / VKCEN_PART at the stamp sites whether
   the class timer is on or not. Begin / end / barrier / copy / clear calls also record their call site (file, line,
   function, and the return address of the function they are in: the helper's caller) for the pass census.
   ps2_vkcen.c folds each counted frame into a window by frame kind (game stereo / in-between stereo / other) and logs it
   (`vk-census` lines) every PS2_VK_CENSUS seconds; per frame totals go into PS2_GPU_TRACE's `cen_*` columns.
   Off (unset or 0): each redirected call costs one predictable branch on ps2_vkcen_on (0), nothing else. */
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { VKC_BEGIN, VKC_END, VKC_BARC, VKC_BARI, VKC_PIPE, VKC_DESC, VKC_PUSH, VKC_DRAW, VKC_DISP, VKC_COPY, VKC_CLEAR,
       VKC_DYN, VKC_N };
enum { VKCE_BEGIN, VKCE_END, VKCE_BARRIER, VKCE_COPY, VKCE_CLEAR, VKCE_N };
#define PS2_VKCEN_PARTS 32u

extern int ps2_vkcen_on;                         /* this frame is counted (set by ps2_vkcen_frame_begin) */
extern unsigned ps2_vkcen_part;                  /* the part being recorded (< PS2_VKCEN_PARTS) */
extern unsigned ps2_vkcen_cnt[PS2_VKCEN_PARTS][VKC_N];

unsigned short ps2_vkcen_site(const char *file, int line, const char *func);
void ps2_vkcen_ev(int what, unsigned short site, const void *arg, unsigned n, void *ret);
void ps2_vkcen_barrier1(unsigned short site, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags fl,
                        unsigned nmem, unsigned nbuf, unsigned nimg, const VkImageMemoryBarrier *img, void *ret);

/* ps2_video.c: the switch, the part names, the frame window */
int  ps2_vkcen_enabled(void);                    /* PS2_VK_CENSUS set (read once) */
void ps2_vkcen_names(const char *const *names, unsigned n);
int  ps2_vkcen_frame_begin(unsigned long long frame, unsigned part);   /* 1 = this frame is counted */
void ps2_vkcen_frame_end(unsigned long long frame, int kind);   /* kind 0 game stereo, 1 in-between stereo, 2 other */
int  ps2_vkcen_totals(unsigned long long frame, unsigned out[8]);   /* begin, barriers, images, pipelines, descsets,
                                                 dyn, draws, copies of a counted frame (the GPU trace) */
int  ps2_vkcen_pause(void);                      /* (PS2_BENCH_REPEAT's extra drawings): returns the old state */
void ps2_vkcen_resume(int was);

#define VKCEN_PART(p) do { if (__builtin_expect(ps2_vkcen_on, 0)) ps2_vkcen_part = (unsigned)(p) & (PS2_VKCEN_PARTS - 1u); } while (0)

#ifndef PS2_VKCEN_IMPL
#define VKCEN_SITE_() ({ static unsigned short vkcs_; if (__builtin_expect(!vkcs_, 0)) \
                          { vkcs_ = ps2_vkcen_site(__FILE__, __LINE__, __func__); } vkcs_; })
#define VKCEN_N_(c, n) (ps2_vkcen_cnt[ps2_vkcen_part][(c)] += (unsigned)(n))
#define VKCEN_CNT_(c, call) ({ if (__builtin_expect(ps2_vkcen_on, 0)) { VKCEN_N_((c), 1); } call; })
#define VKCEN_EV_(w, a, n, call) ({ if (__builtin_expect(ps2_vkcen_on, 0)) \
                                     { ps2_vkcen_ev((w), VKCEN_SITE_(), (a), (n), __builtin_return_address(0)); } call; })

#define vkCmdBindPipeline(...)            VKCEN_CNT_(VKC_PIPE, (vkCmdBindPipeline)(__VA_ARGS__))
#define vkCmdBindDescriptorSets(...)      VKCEN_CNT_(VKC_DESC, (vkCmdBindDescriptorSets)(__VA_ARGS__))
#define vkCmdPushConstants(...)           VKCEN_CNT_(VKC_PUSH, (vkCmdPushConstants)(__VA_ARGS__))
#define vkCmdDraw(...)                    VKCEN_CNT_(VKC_DRAW, (vkCmdDraw)(__VA_ARGS__))
#define vkCmdDrawIndexed(...)             VKCEN_CNT_(VKC_DRAW, (vkCmdDrawIndexed)(__VA_ARGS__))
#define vkCmdDispatch(...)                VKCEN_CNT_(VKC_DISP, (vkCmdDispatch)(__VA_ARGS__))
#define vkCmdDispatchIndirect(...)        VKCEN_CNT_(VKC_DISP, (vkCmdDispatchIndirect)(__VA_ARGS__))
#define p_vkCmdSetColorBlendEquationEXT(...) VKCEN_CNT_(VKC_DYN, (p_vkCmdSetColorBlendEquationEXT)(__VA_ARGS__))
#define p_vkCmdSetColorWriteMaskEXT(...)  VKCEN_CNT_(VKC_DYN, (p_vkCmdSetColorWriteMaskEXT)(__VA_ARGS__))
#define p_vkCmdSetDepthWriteEnable(...)   VKCEN_CNT_(VKC_DYN, (p_vkCmdSetDepthWriteEnable)(__VA_ARGS__))
#define p_vkCmdSetColorBlendEnableEXT(...) VKCEN_CNT_(VKC_DYN, (p_vkCmdSetColorBlendEnableEXT)(__VA_ARGS__))   /* (perf4_passes) */
#define p_vkCmdSetDepthCompareOp(...)     VKCEN_CNT_(VKC_DYN, (p_vkCmdSetDepthCompareOp)(__VA_ARGS__))
#define vkCmdBeginRendering(c_, ri_) ({ const VkRenderingInfo *vkcri_ = (ri_); \
        VKCEN_EV_(VKCE_BEGIN, vkcri_, 1u, (vkCmdBeginRendering)((c_), vkcri_)); })
#define vkCmdEndRendering(c_)             VKCEN_EV_(VKCE_END, NULL, 1u, (vkCmdEndRendering)(c_))
#define vkCmdPipelineBarrier2(c_, di_) ({ const VkDependencyInfo *vkcdi_ = (di_); \
        VKCEN_EV_(VKCE_BARRIER, vkcdi_, 2u, (vkCmdPipelineBarrier2)((c_), vkcdi_)); })
#define vkCmdPipelineBarrier(c_, s_, d_, f_, nm_, pm_, nb_, pb_, ni_, pi_) ({ \
        if (__builtin_expect(ps2_vkcen_on, 0)) { ps2_vkcen_barrier1(VKCEN_SITE_(), (s_), (d_), (f_), (nm_), (nb_), (ni_), \
                                                                    (pi_), __builtin_return_address(0)); } \
        (vkCmdPipelineBarrier)((c_), (s_), (d_), (f_), (nm_), (pm_), (nb_), (pb_), (ni_), (pi_)); })
#define vkCmdCopyImage(c_, a_, b_, x_, y_, n_, r_) \
        VKCEN_EV_(VKCE_COPY, NULL, (n_), (vkCmdCopyImage)((c_), (a_), (b_), (x_), (y_), (n_), (r_)))
#define vkCmdBlitImage(c_, a_, b_, x_, y_, n_, r_, f_) \
        VKCEN_EV_(VKCE_COPY, NULL, (n_), (vkCmdBlitImage)((c_), (a_), (b_), (x_), (y_), (n_), (r_), (f_)))
#define vkCmdCopyBufferToImage(c_, a_, b_, x_, n_, r_) \
        VKCEN_EV_(VKCE_COPY, NULL, (n_), (vkCmdCopyBufferToImage)((c_), (a_), (b_), (x_), (n_), (r_)))
#define vkCmdCopyImageToBuffer(c_, a_, x_, b_, n_, r_) \
        VKCEN_EV_(VKCE_COPY, NULL, (n_), (vkCmdCopyImageToBuffer)((c_), (a_), (x_), (b_), (n_), (r_)))
#define vkCmdClearAttachments(c_, na_, a_, nr_, r_) \
        VKCEN_EV_(VKCE_CLEAR, NULL, (nr_), (vkCmdClearAttachments)((c_), (na_), (a_), (nr_), (r_)))
#define vkCmdClearColorImage(c_, i_, l_, v_, n_, r_) \
        VKCEN_EV_(VKCE_CLEAR, NULL, (n_), (vkCmdClearColorImage)((c_), (i_), (l_), (v_), (n_), (r_)))
#define vkCmdClearDepthStencilImage(c_, i_, l_, v_, n_, r_) \
        VKCEN_EV_(VKCE_CLEAR, NULL, (n_), (vkCmdClearDepthStencilImage)((c_), (i_), (l_), (v_), (n_), (r_)))
#endif

#ifdef __cplusplus
}
#endif
#endif
