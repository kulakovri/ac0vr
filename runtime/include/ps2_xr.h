// ps2_xr.h - interface between the OpenXR output (ps2_xr.c, renderer thread), the GS emulation (ps2_gs.c, GS
// thread), the presenter (ps2_video.c) and game-side camera hooks (zero_vr.c, EE thread).
//
// Flow for head-tracked / stereo rendering:
//   1. Renderer thread: every XR frame locates both eye views and publishes them (ps2_xr_latest_views).
//   2. EE thread, game camera update: ps2_xr_game_frame() picks the eye for this game frame, records the pose it
//      will render with under a sequence number and queues a GS tag carrying that number, then the hook rotates
//      and offsets the game camera by that pose.
//   3. GS thread: the tag sets the "current XR frame"; every draw marks its render target with it; at present
//      the displayed target's number travels with the published frame (ps2_vk_set_xr_seq).
//   4. Renderer thread: the window image of that frame is copied into the swapchain of the eye it was rendered
//      for and submitted in a projection layer with the exact pose and FOV it was rendered with.
#ifndef PS2_XR_H
#define PS2_XR_H

#include "ps2_runtime.h"

#define PS2_XR_TAG_KIND 0x58520001u      /* gfxq / rn_gs_tag kind carrying an XR frame sequence number */

enum { PS2_XR_CINEMA = 0, PS2_XR_MONO = 1, PS2_XR_STEREO = 2, PS2_XR_STEREO_ALT = 3 };

typedef struct {
    int    valid;
    int    eye;            /* 0 left, 1 right, -1 centre (mono) */
    u32    seq;            /* nonzero sequence number of this game frame */
    float  q[4];           /* head/eye orientation relative to the recentred reference (x, y, z, w) */
    float  p[3];           /* position relative to the reference, metres, OpenXR view axes */
    float  tan_h, tan_v;   /* symmetric half-FOV tangents the game should render with */
    float  ipd;            /* metres between the eyes at sampling time */
} ps2_xr_render_pose;

/* Synchronous stereo (PS2_XR_MODE=stereo): the game renders the centre pose once; the renderer draws the frame twice,
   shifting world vertices by x' = x -+ a * (Z + c) (Z = GS depth value, a = m00 * half_ipd_units / K, from the
   game's projection) for the left/right eye. */
void ps2_xr_set_shift(u32 seq, float a, float c, float mh); /* EE: after the camera update (mh = m00 * half_ipd) */
int  ps2_xr_shift_of(u32 seq, float *a, float *c, float *mh); /* GS thread */
void ps2_xr_set_camera(u32 seq, const float eye[3], const float rot[9], float fx, float fy, float cx, float cy); /* EE */
int  ps2_xr_camera_of(u32 seq, float eye[3], float rot[9], float f[2], float c[2]);   /* renderer */
void ps2_xr_set_camera_base(u32 seq, const float eye[3], const float rot[9]);   /* EE: the same camera without the head */
int  ps2_xr_camera_base_of(u32 seq, float eye[3], float rot[9]);                /* renderer */
void ps2_xr_set_cloud_roll(u32 seq, int which, float roll); /* EE: roll the cloud positions (0) / images (1) were built with */
int  ps2_xr_cloud_roll_of(u32 seq, float roll[2]);         /* GS thread: 1 when both are known for that frame */
int  ps2_xr_sync_frame(u32 seq);                       /* renderer: 1 if this frame is drawn per eye */

int  ps2_xr_mode(void);                          /* -1 when XR is off */
void ps2_xr_request_recenter(void);              /* recentre at the next XR frame */
void ps2_xr_request_recenter_hand_over(float keep_deg); /* flight start: keep the reference within keep_deg, else recentre */
int  ps2_xr_game_frame(ps2_xr_render_pose *out); /* EE: once per game frame; 0 when no head pose yet */
void ps2_xr_set_head_locked(int on);             /* EE: next game frames are shown head-locked (stick head look) */
void ps2_gs_xr_tag(u32 seq);                     /* GS thread: tag reached the GS stream */
void ps2_vk_set_xr_seq(u32 seq);                 /* GS thread: sequence number of the frame being presented */

#endif
