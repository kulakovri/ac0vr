#ifndef PS2_UI_H
#define PS2_UI_H

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    SDL_Window      *window;
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    uint32_t         queue_family;
    VkQueue          queue;
    VkFormat         color_format;
    uint32_t         image_count;
    float            max_anisotropy;
} ps2_ui_init_info;

int  ps2_ui_init(const ps2_ui_init_info *info);
void ps2_ui_shutdown(void);
int  ps2_ui_event(const SDL_Event *e);
int  ps2_ui_visible(void);
void ps2_ui_set_visible(int on);
int  ps2_ui_blocks_game_input(void);
void ps2_ui_draw(VkCommandBuffer cmd);
void ps2_ui_swapchain_changed(uint32_t image_count);

typedef struct {
    uint32_t scale;
    uint32_t guest_w, guest_h;
    uint32_t targets;
    uint64_t bytes_at_1x;
    uint32_t max_scale;
    uint32_t auto_scale;
    uint32_t failed_scale;
    float    max_line_width;
} ps2_render_info;

SDL_Gamepad *ps2_video_gamepad(void);              /* the active pad (while Touch is active: the pad used last) */
void ps2_video_select_gamepad(SDL_JoystickID id);  /* F4's list: that pad becomes the active controller (forced) */
/* the controller registry (ps2_video.c, ctl_priority; renderer thread) */
int ps2_video_pad_count(void);                     /* every connected gamepad (all are read; one is active) */
SDL_Gamepad *ps2_video_pad_at(int i);
int ps2_video_pad_force(const char *v);            /* "" / "auto": the one used last; "touch"; a part of a pad's name */
int ps2_video_pad_forced_kind(void);               /* 0 auto, 1 Touch, 2 a pad by name, 3 a pad by F4's list */
int ps2_video_pad_touch_state(void);               /* Quest Touch: 0 not there, 1 there, 2 the active controller */
const char *ps2_video_pad_active_name(void);       /* NULL: no controller */
void ps2_video_render_info(ps2_render_info *out);

#ifdef __cplusplus
}
#endif

#endif
