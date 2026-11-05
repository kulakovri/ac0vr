#include "ps2_runtime.h"
#include "ps2_settings.h"
#include "ps2_menv.h"                      /* the NEXT MISSION tier */
#include "ps2_quit.h"                      /* the TOUCH CONTROLS card (ENHANCEMENTS > CONTROLS > SHOW TOUCH CONTROLS) */
#include "ps2_hotas.h"                     /* [joystick:<GUID>] sections, the throttle lever rows */

#include <SDL3/SDL.h>
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <vulkan/vulkan.h>                /* the GPU probe of a new install (QUALITY PRESET) */

ps2_settings ps2_cfg;

static atomic_int cfg_dirty;
static atomic_int cfg_save_pending;
static int cfg_file_disabled;
static char cfg_path[1024];

static const char *const act_names[PS2_ACT_COUNT] = {
    "Select", "L3", "R3", "Start",
    "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left",
    "L2", "R2", "L1", "R1",
    "Triangle", "Circle", "Cross", "Square",
    "Left stick Up", "Left stick Down", "Left stick Left", "Left stick Right",
    "Right stick Up", "Right stick Down", "Right stick Left", "Right stick Right",
};

static const char *const act_keys[PS2_ACT_COUNT] = {
    "select", "l3", "r3", "start", "up", "right", "down", "left",
    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square",
    "lstick_up", "lstick_down", "lstick_left", "lstick_right",
    "rstick_up", "rstick_down", "rstick_left", "rstick_right",
};

static const char *const pad_button_names[] = {
    "South (A / Cross)", "East (B / Circle)", "West (X / Square)",
    "North (Y / Triangle)", "Back / Select", "Guide", "Start",
    "Left stick click", "Right stick click", "Left bumper", "Right bumper",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Misc",
    "Right paddle 1", "Left paddle 1", "Right paddle 2", "Left paddle 2",
    "Touchpad", "Misc 2", "Misc 3", "Misc 4", "Misc 5", "Misc 6",
};

const char *ps2_action_name(int act) {
    return act >= 0 && act < PS2_ACT_COUNT ? act_names[act] : "?";
}

const char *ps2_key_name(int scancode) {
    const char *n;
    if (scancode <= 0) return "-";
    n = SDL_GetScancodeName((SDL_Scancode)scancode);
    return n && *n ? n : "Unknown key";
}

const char *ps2_padbind_name(int code) {
    static char buf[4][48];
    static unsigned next;
    char *out = buf[next++ % 4];
    if (code <= 0) return "-";
    if (code & PS2_PADBIND_AXIS) {
        int axis = (code & 0xFF) >> 1, pos = code & 1;
        switch (axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:  return pos ? "Left stick Right" : "Left stick Left";
        case SDL_GAMEPAD_AXIS_LEFTY:  return pos ? "Left stick Down" : "Left stick Up";
        case SDL_GAMEPAD_AXIS_RIGHTX: return pos ? "Right stick Right" : "Right stick Left";
        case SDL_GAMEPAD_AXIS_RIGHTY: return pos ? "Right stick Down" : "Right stick Up";
        case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:  return "Left trigger";
        case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return "Right trigger";
        default: snprintf(out, 48, "Axis %d%c", axis, pos ? '+' : '-'); return out;
        }
    }
    if (code - 1 < (int)(sizeof pad_button_names / sizeof pad_button_names[0]))
        return pad_button_names[code - 1];
    snprintf(out, 48, "Button %d", code - 1);
    return out;
}

void ps2_settings_default_bindings(ps2_settings *s, int keyboard, int pad) {
    if (keyboard) {
        memset(s->key, 0, sizeof s->key);
        s->key[PS2_ACT_START][0]    = SDL_SCANCODE_RETURN;
        s->key[PS2_ACT_SELECT][0]   = SDL_SCANCODE_RSHIFT;
        s->key[PS2_ACT_UP][0]       = SDL_SCANCODE_UP;
        s->key[PS2_ACT_DOWN][0]     = SDL_SCANCODE_DOWN;
        s->key[PS2_ACT_LEFT][0]     = SDL_SCANCODE_LEFT;
        s->key[PS2_ACT_RIGHT][0]    = SDL_SCANCODE_RIGHT;
        s->key[PS2_ACT_CROSS][0]    = SDL_SCANCODE_X;
        s->key[PS2_ACT_SQUARE][0]   = SDL_SCANCODE_Z;
        s->key[PS2_ACT_CIRCLE][0]   = SDL_SCANCODE_S;
        s->key[PS2_ACT_TRIANGLE][0] = SDL_SCANCODE_A;
        s->key[PS2_ACT_L1][0]       = SDL_SCANCODE_Q;
        s->key[PS2_ACT_R1][0]       = SDL_SCANCODE_E;
        s->key[PS2_ACT_L2][0]       = SDL_SCANCODE_1;
        s->key[PS2_ACT_R2][0]       = SDL_SCANCODE_3;
        s->key[PS2_ACT_LS_UP][0]    = SDL_SCANCODE_W;
        s->key[PS2_ACT_LS_UP][1]    = SDL_SCANCODE_KP_8;
        s->key[PS2_ACT_LS_DOWN][0]  = SDL_SCANCODE_KP_2;
        s->key[PS2_ACT_LS_LEFT][0]  = SDL_SCANCODE_KP_4;
        s->key[PS2_ACT_LS_RIGHT][0] = SDL_SCANCODE_KP_6;
    }
    if (pad) ps2_settings_layout_bindings(PS2_PAD_LAYOUT_CLASSIC, s->pad);
}

/* The controller bindings of a layout. Classic = the defaults: PS2 positions (LB/RB = L1/R1 throttle, LT/RT = L2/R2
   yaw). Modern = on Xbox: RT/LT = R1/L1 (analog throttle up/down), RB/LB = R2/L2 (yaw; both = the
   autopilot), the rest as classic, which already matches the modern layout by position (A = Cross gun, B = Circle missile,
   X = Square and View = Select: weapon switch and map as the game's Control Scheme assigns them, Y = Triangle target,
   right stick look, its click = R3 view, Menu = Start). */
void ps2_settings_layout_bindings(int layout, int pad[PS2_ACT_COUNT][PS2_BIND_SLOTS]) {
    memset(pad, 0, sizeof(int) * PS2_ACT_COUNT * PS2_BIND_SLOTS);
    pad[PS2_ACT_CROSS][0]    = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_SOUTH);
    pad[PS2_ACT_CIRCLE][0]   = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_EAST);
    pad[PS2_ACT_SQUARE][0]   = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_WEST);
    pad[PS2_ACT_TRIANGLE][0] = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_NORTH);
    pad[PS2_ACT_SELECT][0]   = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_BACK);
    pad[PS2_ACT_START][0]    = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_START);
    pad[PS2_ACT_L3][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    pad[PS2_ACT_R3][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    pad[PS2_ACT_L1][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    pad[PS2_ACT_R1][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    pad[PS2_ACT_UP][0]       = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_UP);
    pad[PS2_ACT_DOWN][0]     = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    pad[PS2_ACT_LEFT][0]     = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    pad[PS2_ACT_RIGHT][0]    = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    pad[PS2_ACT_L2][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1);
    pad[PS2_ACT_R2][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1);
    pad[PS2_ACT_LS_UP][0]    = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTY, 0);
    pad[PS2_ACT_LS_DOWN][0]  = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTY, 1);
    pad[PS2_ACT_LS_LEFT][0]  = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTX, 0);
    pad[PS2_ACT_LS_RIGHT][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFTX, 1);
    pad[PS2_ACT_RS_UP][0]    = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHTY, 0);
    pad[PS2_ACT_RS_DOWN][0]  = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHTY, 1);
    pad[PS2_ACT_RS_LEFT][0]  = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHTX, 0);
    pad[PS2_ACT_RS_RIGHT][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHTX, 1);
    if (layout == PS2_PAD_LAYOUT_MODERN) {
        pad[PS2_ACT_L1][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1);
        pad[PS2_ACT_R1][0] = PS2_PADBIND_AXISDIR(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1);
        pad[PS2_ACT_L2][0] = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        pad[PS2_ACT_R2][0] = PS2_PADBIND_BUTTON(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    }
}

static int modern_pad[PS2_ACT_COUNT][PS2_BIND_SLOTS];

/* the controller bindings in effect for an action (PS2_BIND_SLOTS codes): the modern preset, or the [controller] ones */
const int *ps2_settings_pad_binds(int act) {
    if (act < 0 || act >= PS2_ACT_COUNT) act = 0;
    return ps2_cfg.pad_layout == PS2_PAD_LAYOUT_MODERN ? modern_pad[act] : ps2_cfg.pad[act];
}

typedef struct {
    int internal_res, tex_filter, anisotropy, deinterlace, fxaa, scale_filter;
    float sharpen;
} preset_def;

static const preset_def presets[4] = {
     { 1, PS2_TEXFILTER_GAME,  0, 1, 0, PS2_SCALE_BILINEAR, 0.00f },
     { 2, PS2_TEXFILTER_GAME,  4, 1, 0, PS2_SCALE_BILINEAR, 0.00f },
     { 3, PS2_TEXFILTER_GAME,  8, 1, 0, PS2_SCALE_SHARP,    0.20f },
     { 4, PS2_TEXFILTER_GAME, 16, 1, 0, PS2_SCALE_SHARP,    0.20f },
};

void ps2_settings_apply_preset(ps2_settings *s, int preset) {
    const preset_def *p;
    if (preset < 0 || preset > PS2_PRESET_ULTRA) return;
    p = &presets[preset];
    s->preset       = preset;
    s->internal_res = p->internal_res;
    s->tex_filter   = p->tex_filter;
    s->anisotropy   = p->anisotropy;
    s->deinterlace  = p->deinterlace;
    s->fxaa         = p->fxaa;
    s->scale_filter = p->scale_filter;
    s->sharpen      = p->sharpen;
}

int ps2_settings_preset_matches(const ps2_settings *s) {
    for (int i = 0; i <= PS2_PRESET_ULTRA; i++) {
        const preset_def *p = &presets[i];
        if (s->internal_res == p->internal_res
            && s->tex_filter == p->tex_filter && s->anisotropy == p->anisotropy
            && s->deinterlace == p->deinterlace
            && s->fxaa == p->fxaa && s->scale_filter == p->scale_filter
            && fabsf(s->sharpen - p->sharpen) < 0.005f)
            return i;
    }
    return PS2_PRESET_CUSTOM;
}

void ps2_settings_defaults(ps2_settings *s) {
    memset(s, 0, sizeof *s);
    ps2_settings_apply_preset(s, PS2_PRESET_LOW);
    s->brightness = 0.0f;
    s->contrast = 1.0f;
    s->gamma = 1.0f;
    s->saturation = 1.0f;
    s->window_mode = PS2_WIN_WINDOWED;
    s->fullscreen_type = PS2_WIN_BORDERLESS;
    s->window_w = 1280;
    s->window_h = 896;
    s->aspect = PS2_ASPECT_STRETCH;
    s->aspect_custom = 16.0f / 9.0f;
    s->hud_layout = PS2_HUD_CENTERED;
    s->present_mode = PS2_PRESENT_MAILBOX;
    s->ui_scale = 1.0f;
    s->block_input_in_menu = 1;
    ps2_settings_default_bindings(s, 1, 1);
    ps2_settings_layout_bindings(PS2_PAD_LAYOUT_MODERN, modern_pad);
    s->deadzone = 0.0f;
    s->axis_scale = 1.33f;
    s->button_deadzone = 0.0f;
    s->imm_canopy = 1; s->imm_mist = 1; s->imm_grime = 1;
    s->imm_water = 1.0f; s->imm_drop_size = 1.0f; s->imm_rumble = 1.0f; s->imm_sound = 1.0f;
    s->imm_cloud_volume = 1.0f; s->imm_cloud_light = 1.0f;
    s->imm_ice = 1;
    s->terrain_real = 1;
    s->imm_hrtf = 2; s->imm_doppler = 0.25f; s->imm_muffle = 1.0f; s->imm_engine_grain = 0.0f;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static void sanitize(ps2_settings *s) {
    s->internal_res = clampi(s->internal_res, 0, PS2_INTERNAL_RES_MAX);
    s->tex_filter = clampi(s->tex_filter, 0, 2);
    if (s->anisotropy != 0 && s->anisotropy != 2 && s->anisotropy != 4
        && s->anisotropy != 8 && s->anisotropy != 16)
        s->anisotropy = 0;
    s->deinterlace = s->deinterlace != 0;
    s->fxaa = s->fxaa != 0;
    s->sharpen = clampf(s->sharpen, 0.0f, 1.0f);
    s->scale_filter = clampi(s->scale_filter, 0, 2);
    s->brightness = clampf(s->brightness, -0.5f, 0.5f);
    s->contrast = clampf(s->contrast, 0.5f, 1.5f);
    s->gamma = clampf(s->gamma, 0.5f, 2.0f);
    s->saturation = clampf(s->saturation, 0.0f, 2.0f);
    s->window_mode = clampi(s->window_mode, 0, 2);
    if (s->fullscreen_type != PS2_WIN_EXCLUSIVE) s->fullscreen_type = PS2_WIN_BORDERLESS;
    s->window_w = clampi(s->window_w, 320, 16384);
    s->window_h = clampi(s->window_h, 224, 16384);
    s->fs_w = clampi(s->fs_w, 0, 16384);
    s->fs_h = clampi(s->fs_h, 0, 16384);
    s->fs_hz = clampf(s->fs_hz, 0.0f, 1000.0f);
    s->aspect = clampi(s->aspect, 0, 4);
    s->aspect_custom = clampf(s->aspect_custom, 0.5f, 4.0f);
    s->integer_scale = s->integer_scale != 0;
    s->widescreen = s->widescreen != 0;
    s->hud_layout = clampi(s->hud_layout, 0, 1);
    s->present_mode = clampi(s->present_mode, 0, 3);
    s->fps_limit = clampi(s->fps_limit, 0, 1000);
    s->show_fps = s->show_fps != 0;
    s->ui_scale = clampf(s->ui_scale, 0.5f, 3.0f);
    s->block_input_in_menu = s->block_input_in_menu != 0;
    s->pad_layout = clampi(s->pad_layout, 0, 1);
    for (int a = 0; a < PS2_ACT_COUNT; a++)
        for (int k = 0; k < PS2_BIND_SLOTS; k++) {
            if (s->key[a][k] < 0 || s->key[a][k] >= SDL_SCANCODE_COUNT) s->key[a][k] = 0;
            if (s->pad[a][k] < 0 || s->pad[a][k] > 0x1FF) s->pad[a][k] = 0;
        }
    s->deadzone = clampf(s->deadzone, 0.0f, 1.0f);
    s->axis_scale = clampf(s->axis_scale, 0.01f, 2.0f);
    s->button_deadzone = clampf(s->button_deadzone, 0.0f, 1.0f);
    s->invert[0] = clampi(s->invert[0], 0, 3);
    s->invert[1] = clampi(s->invert[1], 0, 3);
    s->imm_canopy = s->imm_canopy != 0; s->imm_mist = s->imm_mist != 0; s->imm_grime = s->imm_grime != 0;
    s->imm_water = clampf(s->imm_water, 0.0f, 2.0f);
    s->imm_drop_size = clampf(s->imm_drop_size, 0.5f, 2.0f);
    s->imm_rumble = clampf(s->imm_rumble, 0.0f, 2.0f);
    s->imm_sound = clampf(s->imm_sound, 0.0f, 2.0f);
    s->imm_cloud_volume = clampf(s->imm_cloud_volume, 0.0f, 2.0f);
    s->imm_cloud_light = clampf(s->imm_cloud_light, 0.0f, 1.0f);
    s->imm_ice = s->imm_ice != 0;
    s->terrain_real = s->terrain_real != 0;
    s->imm_hrtf = s->imm_hrtf < 0 ? 0 : s->imm_hrtf > 2 ? 2 : s->imm_hrtf;
    s->imm_doppler = clampf(s->imm_doppler, 0.0f, 1.0f);
    s->imm_muffle = clampf(s->imm_muffle, 0.0f, 1.0f);
    s->imm_engine_grain = clampf(s->imm_engine_grain, 0.0f, 1.0f);
}

static float env_or(const char *name, float v, int *seen, float *val) {
    if (!*seen) { const char *e = getenv(name); *seen = e && *e ? 1 : -1; if (*seen > 0) *val = (float)atof(e); }
    return *seen > 0 ? *val : v;
}
int ps2_imm_canopy_on(void) { static int s; static float v; return env_or("ACZ_VR_CANOPY", (float)ps2_cfg.imm_canopy, &s, &v) != 0.0f; }
int ps2_imm_ice_on(void) { static int s; static float v; return env_or("ACZ_VR_CANOPY_ICE", (float)ps2_cfg.imm_ice, &s, &v) != 0.0f; }
/* (TERRAIN LOOK's field: saved at once, but relief_apply reads this every pass: the value of this mission until the edge) */
static signed char enh_treal_latch = -1;         /* >= 0: this mission's value, the field already holds the next one's */
int ps2_terrain_real_on(void) { static int s; static float v; return env_or("ACZ_TERRAIN_REAL", enh_treal_latch >= 0 ? (float)enh_treal_latch : (float)ps2_cfg.terrain_real, &s, &v) != 0.0f; }
float ps2_imm_water(void) { static int s; static float v; return env_or("ACZ_VR_CANOPY_GAIN", ps2_cfg.imm_water, &s, &v); }
float ps2_imm_rumble(void) { static int s; static float v; return env_or("ACZ_IMM_RUMBLE", ps2_cfg.imm_rumble, &s, &v); }
float ps2_imm_sound(void) { static int s; static float v; return env_or("ACZ_IMM_SOUND", ps2_cfg.imm_sound, &s, &v); }
static int imm_master_off(void) { static int m = -1; if (m < 0) { const char *e = getenv("ACZ_IMMERSE"); m = e && *e == '0'; } return m; }
float ps2_imm_veil(void) {
    static int s; static float v;
    return imm_master_off() ? 0.0f : env_or("ACZ_IMM_VEIL", ps2_cfg.imm_cloud_volume, &s, &v);
}
float ps2_imm_cloud_light(void) {
    static int s; static float v;
    return imm_master_off() ? 0.0f : env_or("ACZ_IMM_CLOUD_LIGHT", ps2_cfg.imm_cloud_light, &s, &v);
}

typedef struct {
    const char *section, *key;
    char type;
    size_t off;
    const char *comment;
} cfg_field;

#define FI(sec, k, m, c) { sec, k, 'i', offsetof(ps2_settings, m), c }
#define FF(sec, k, m, c) { sec, k, 'f', offsetof(ps2_settings, m), c }

static const cfg_field fields[] = {
    FI("graphics", "preset", preset, "0 low, 1 medium, 2 high, 3 ultra, 4 custom"),
    FI("graphics", "internal_resolution", internal_res, "0 auto (cover the window), 1 native 640x448, 2..8 times that"),
    FI("graphics", "texture_filter", tex_filter, "0 as the game asks, 1 force nearest, 2 force bilinear"),
    FI("graphics", "anisotropy", anisotropy, "0 off, 2, 4, 8 or 16"),
    FI("graphics", "deinterlace", deinterlace, "1 weave the two fields into whole frames"),
    FI("graphics", "scale_filter", scale_filter, "0 nearest, 1 bilinear, 2 sharp bilinear"),
    FI("graphics", "realistic_terrain", terrain_real, "1 relief light, gullies and mountain shadows on the terrain, baked from the game's heightmap (shading only)"),
    FI("postfx", "fxaa", fxaa, "1 on"),
    FF("postfx", "sharpen", sharpen, "0..1"),
    FF("postfx", "brightness", brightness, "-0.5..0.5"),
    FF("postfx", "contrast", contrast, "0.5..1.5"),
    FF("postfx", "gamma", gamma, "0.5..2.0"),
    FF("postfx", "saturation", saturation, "0..2"),
    FI("display", "window_mode", window_mode, "0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen"),
    FI("display", "fullscreen_type", fullscreen_type, "what F11 enters: 1 borderless, 2 exclusive"),
    FI("display", "window_width", window_w, "windowed size in pixels"),
    FI("display", "window_height", window_h, ""),
    FI("display", "fullscreen_width", fs_w, "exclusive fullscreen; 0 = desktop resolution"),
    FI("display", "fullscreen_height", fs_h, ""),
    FF("display", "fullscreen_refresh", fs_hz, "Hz; 0 = desktop refresh rate"),
    FI("display", "aspect", aspect, "0 stretch to window, 1 auto (4:3, 16:9 with widescreen), 2 4:3, 3 16:9, 4 custom"),
    FF("display", "aspect_custom", aspect_custom, "width / height"),
    FI("display", "integer_scale", integer_scale, "1 whole-number scaling only"),
    FI("display", "widescreen", widescreen, "1 apply the 16:9 game patch"),
    FI("display", "hud_layout", hud_layout, "with widescreen: 0 stretch the HUD with the picture, 1 keep it 4:3 in the centre"),
    FI("display", "present_mode", present_mode, "0 mailbox, 1 vsync (fifo), 2 immediate, 3 adaptive vsync"),
    FI("display", "fps_limit", fps_limit, "0 off"),
    FI("display", "show_fps", show_fps, "1 on"),
    FF("display", "menu_scale", ui_scale, "0.5..3"),
    FI("input", "touch_controls_seen", touch_card_seen, "1 the TOUCH CONTROLS card showed once by itself (ACZ_TOUCH_CARD)"),
    FI("input", "block_game_input_in_menu", block_input_in_menu, "1 on"),
    FI("input", "pad_layout", pad_layout, "0 classic ([controller] below), 1 modern ( throttle on the triggers)"),
    FF("analog", "deadzone", deadzone, "PCSX2 Analog Deadzone, both sticks, 0..1"),
    FF("analog", "axis_scale", axis_scale, "PCSX2 Analog Sensitivity, 0.01..2 (default 1.33)"),
    FF("analog", "button_deadzone", button_deadzone, "PCSX2 Button/Trigger Deadzone, 0..1"),
    FI("analog", "invert_left", invert[0], "0 none, 1 left/right, 2 up/down, 3 both"),
    FI("analog", "invert_right", invert[1], "0 none, 1 left/right, 2 up/down, 3 both"),
    FI("immersion", "canopy_water", imm_canopy, "1 rain, snow and cloud water on the native cockpit's canopy glass"),
    FF("immersion", "water_strength", imm_water, "0..2 (1 default)"),
    FF("immersion", "drop_size", imm_drop_size, "0.5..2 (1 default)"),
    FI("immersion", "canopy_mist", imm_mist, "1 a fine film on the glass inside cloud"),
    FI("immersion", "canopy_grime", imm_grime, "1 grime and wet smears after flying through debris"),
    FI("immersion", "canopy_ice", imm_ice, "1 the canopy water freezes above the freezing level, frost in cold cloud (visual only)"),
    FF("immersion", "rumble", imm_rumble, "weather and debris rumble under the game's own, 0..2 (0 off)"),
    FF("immersion", "sound", imm_sound, "airflow, rain and debris sounds, 0..2 (0 off)"),
    FF("immersion", "cloud_volume", imm_cloud_volume, "vapour streaming past inside cloud, 0..2 (0 off)"),
    FF("immersion", "cloud_light", imm_cloud_light, "the cockpit lit by the cloud instead of the sun inside cloud, 0..1 (0 off)"),
    FI("immersion", "binaural_sound", imm_hrtf, "3D sound effects rendered for headphones (HRTF): 0 off, 1 on, 2 auto (VR only)"),
    FF("immersion", "doppler", imm_doppler, "Doppler pitch shift of moving sounds, 0..1 of the real effect (0 off, 0.25 default)"),
    FF("immersion", "cockpit_muffle", imm_muffle, "outside sounds muffled in the cockpit view, 0..1 (0 off)"),
    FF("immersion", "engine_grain", imm_engine_grain, "granular engine sound from throttle and airspeed (prototype), 0..1 (0 off)"),
};

void ps2_settings_disable_file(void) {
    const char *e = getenv("PS2_SETTINGS_FILE");
    cfg_file_disabled = !(e && *e);
}

const char *ps2_settings_path(void) {
    if (!cfg_path[0]) {
        const char *e = getenv("PS2_SETTINGS_FILE");
        const char *base = SDL_GetBasePath();
        if (e && *e) snprintf(cfg_path, sizeof cfg_path, "%s", e);
        else snprintf(cfg_path, sizeof cfg_path, "%sac5_settings.ini", base ? base : "");
    }
    return cfg_path;
}

static char *trim(char *p) {
    char *e;
    while (*p == ' ' || *p == '\t') p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return p;
}

static int cfg_quiet;                       /* the launcher's early read: ps2_settings_load says it later */
static void log_summary(const char *source) {
    if (cfg_quiet) return;
    static const char *const presets_n[] = { "low", "medium", "high", "ultra", "custom" };
    static const char *const win_n[] = { "windowed", "borderless", "exclusive" };
    static const char *const present_n[] = { "mailbox", "fifo", "immediate", "fifo-relaxed" };
    ps2_log("settings: %s -- preset %s, internal resolution %s%d, texture filter %d, "
            "anisotropy %d, %s %dx%d, aspect %d, widescreen %d, present %s, fps limit %d",
            source, presets_n[ps2_cfg.preset], ps2_cfg.internal_res ? "" : "auto ",
            ps2_cfg.internal_res, ps2_cfg.tex_filter, ps2_cfg.anisotropy,
            win_n[ps2_cfg.window_mode], ps2_cfg.window_w, ps2_cfg.window_h, ps2_cfg.aspect,
            ps2_cfg.widescreen, present_n[ps2_cfg.present_mode], ps2_cfg.fps_limit);
}

static int pad_layout_seen;                 /* the file has [input] pad_layout */
static void enh_cx_file_reset(void);
static void enh_cx_file_key(const char *key, const char *val);
static void enh_cx_file_apply(void);
static void settings_load_file(void) {
    char line[512], section[64] = "";
    FILE *f;
    pad_layout_seen = 0;
    enh_cx_file_reset();
    ps2_hotas_ini_reset();
    ps2_settings_defaults(&ps2_cfg);
    if (cfg_file_disabled) return;
    f = fopen(ps2_settings_path(), "r");
    if (!f) {
        log_summary("no settings file, defaults");
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *p, *eq, *key, *val, *c;
        c = strpbrk(line, ";#");
        if (c) *c = 0;
        p = trim(line);
        if (!*p) continue;
        if (*p == '[') {
            char *e = strchr(p, ']');
            if (e) { *e = 0; snprintf(section, sizeof section, "%s", p + 1); }
            continue;
        }
        eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        key = trim(p);
        val = trim(eq + 1);
        if (!strcmp(section, "keyboard") || !strcmp(section, "controller")) {
            int act, a = 0, b = 0;
            for (act = 0; act < PS2_ACT_COUNT; act++)
                if (!strcmp(key, act_keys[act])) break;
            if (act == PS2_ACT_COUNT) continue;
            if (sscanf(val, "%d %d", &a, &b) < 1) continue;
            if (section[0] == 'k') { ps2_cfg.key[act][0] = a; ps2_cfg.key[act][1] = b; }
            else                   { ps2_cfg.pad[act][0] = a; ps2_cfg.pad[act][1] = b; }
            continue;
        }
        if (!strcmp(section, "enhancements")) {    /* a combo row's key sets its cfg-backed members (below) */
            enh_cx_file_key(key, val);
            continue;
        }
        if (!strncmp(section, "joystick:", 9)) {   /* a joystick profile, by GUID (ps2_hotas.c keeps the text) */
            ps2_hotas_ini_kv(section + 9, key, val);
            continue;
        }
        if (!strcmp(section, "analog")) {
            static const char *const old_invert[4] = {
                "left_invert_x", "left_invert_y", "right_invert_x", "right_invert_y",
            };
            int i = 0;
            while (i < 4 && strcmp(key, old_invert[i])) i++;
            if (i < 4) {
                if (atoi(val)) ps2_cfg.invert[i / 2] |= 1 << (i % 2);
                continue;
            }
        }
        for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
            if (strcmp(fields[i].section, section) || strcmp(fields[i].key, key)) continue;
            if (fields[i].off == offsetof(ps2_settings, pad_layout)) pad_layout_seen = 1;
            if (fields[i].type == 'i') *(int *)((char *)&ps2_cfg + fields[i].off) = atoi(val);
            else *(float *)((char *)&ps2_cfg + fields[i].off) = strtof(val, NULL);
            break;
        }
    }
    fclose(f);
    enh_cx_file_apply();
    sanitize(&ps2_cfg);
    ps2_cfg.preset = ps2_settings_preset_matches(&ps2_cfg);
    if (!cfg_quiet) ps2_log("settings: loaded %s", ps2_settings_path());
    log_summary("in use");
}

/* ACZ_PAD_LAYOUT=modern|classic: the controller layout for this session (launcher words `modern` / `classic`),
   over the saved [input] pad_layout. It is not written back to the file unless the layout is changed in F4. */
static int layout_env = -1, layout_file;

static void enh_log_start(void);
static int enh_new_install_preset(void);
static void enh_cfg_loaded(void);
int ps2_enh_applied(void);
void ps2_settings_load(void) {
    const char *e = getenv("ACZ_PAD_LAYOUT");
    settings_load_file();
    enh_cfg_loaded();
    enh_log_start();
    /* the launchers used to pass `modern`: with ACZ_MENU_SETTINGS the layout's default is modern until the file has
       a pad_layout of its own (the first save writes it) */
    if (!pad_layout_seen && ps2_enh_applied()) ps2_cfg.pad_layout = PS2_PAD_LAYOUT_MODERN;
    layout_env = -1;
    if (e && *e) {
        if (!strcmp(e, "modern") || !strcmp(e, "1")) layout_env = PS2_PAD_LAYOUT_MODERN;
        else if (!strcmp(e, "classic") || !strcmp(e, "0")) layout_env = PS2_PAD_LAYOUT_CLASSIC;
        else ps2_log("settings: ACZ_PAD_LAYOUT=%s not understood (modern or classic)", e);
    }
    layout_file = ps2_cfg.pad_layout;
    if (layout_env >= 0) ps2_cfg.pad_layout = layout_env;
    ps2_log("settings: controller layout %s%s", ps2_cfg.pad_layout == PS2_PAD_LAYOUT_MODERN
            ? "modern ( RT/LT = R1/L1 analog throttle, RB/LB = R2/L2 yaw)" : "classic",
            layout_env >= 0 ? " (ACZ_PAD_LAYOUT)" : "");
    if (enh_new_install_preset() >= 0) ps2_settings_save();   /* a new install's GPU preset: saved once (QUALITY PRESET) */
}

/* the F4 menu's choice: in effect at once and saved (an ACZ_PAD_LAYOUT override no longer applies to the file) */
void ps2_settings_set_pad_layout(int layout) {
    ps2_cfg.pad_layout = layout == PS2_PAD_LAYOUT_MODERN ? PS2_PAD_LAYOUT_MODERN : PS2_PAD_LAYOUT_CLASSIC;
    layout_env = -1;
    ps2_log("settings: controller layout %s", ps2_cfg.pad_layout == PS2_PAD_LAYOUT_MODERN ? "modern" : "classic");
}

/* ---------------------------------------------------------------------------------------------------------------
   ENHANCEMENTS (bug #64): every player-facing improvement as a setting, one table for the in-game page (pause menu >
   ENHANCEMENTS, ps2_quit.c), F4 > Enhancements (ps2_ui.cpp) and the [enhancements] section of the settings file.

   Most improvements are environment switches that their module reads once (docs/ENV_REFERENCE.md). Instead of
   touching every reader, enh_preload() runs before main() (a constructor): with ACZ_MENU_SETTINGS=1 (play_zero_vr.sh
   exports it) it reads [enhancements] and sets each item's switch to the saved value unless the switch is already in
   the environment -- a value set by hand or by a launcher word (clouds3d, nosmooth, ...) still wins and the page shows
   it locked. Choices whose value is "" leave the switch unset (its built-in default). Without ACZ_MENU_SETTINGS
   (run_test.sh and the other test scripts) nothing is set: every switch keeps its built-in default, so test runs do
   not depend on the user's file. The immersion and sound items, realistic terrain and the controller layout are the
   existing F4 settings (ps2_cfg: [immersion], [graphics], [input]) and work in every run; with ACZ_MENU_SETTINGS the
   controller layout defaults to modern while the file has no pad_layout (the desktop shortcut used to pass `modern`).
   Live: the ps2_cfg items and the controller layout; the cloud shadow / ground shadow / optics toggles while our clouds
   run; the LOD bias and terrain detail ring (game constants rewritten by zero_vr.c); ACTIVE CONTROLLER and RUMBLE
   STRENGTH (ps2_video.c's controller registry, ctl_priority). MISSION: realistic terrain (baked when a mission loads).
   Everything else is read at start-up: saved at once, in effect after a restart; a row of PS2_ENH_MISSION at the next
   mission (ps2_menv.h, below the table).
   ACTIVE CONTROLLER's list changes with the connected pads (ps2_enh_pad_names: AUTO, TOUCH, then each pad by name);
   a pad is saved by its name (a hand-written value of ACZ_PAD_ACTIVE), so it still means that pad after a replug or
   a restart, and stays shown while that pad is away (the automatic rule then decides).
   The layers (settings review 2026-11-05, docs/SETTINGS.md): a row stays on the page only if a player sees, hears or feels
   the difference within ten seconds of flying AND two reasonable players would choose differently; the page has seven
   categories (PS2_ENH_QUALITY ...). PS2_ENH_CAT_INI: an expert key: its line in the file, its switch set at start as
   ever, listed by F4 > Enhancements > Expert (ini), never on the page. PS2_ENH_CAT_OFF: not listed at all: a combo row's
   member (K_COMBO, below the table), or a conditional row while its hardware is absent (ps2_enh_hotas_rows,
   ps2_enh_touch_rows).
   --------------------------------------------------------------------------------------------------------------- */
enum { K_ENV, K_CFG_I, K_CFG_F, K_PATHS, K_ACT_RECENTRE, K_ACT_RESET, K_ACT_CARD, K_ACT_RUMBLE, K_ACT_NEUTRAL, K_PRESET, K_COMBO };
/* perf6 placeholders (2026-11-04): the rows of switches other packages deliver without a row -- CLOUD DETAIL
   (perf6_clouds, ACZ_VR_CLOUD_DETAIL), CANOPY QUALITY (perf6_glassladder). Set to 1 when the switch exists (and check its
   values against the row and enh_preset_tab); until then the row is not on the page and the presets skip it. A package
   that brings its own row (COCKPIT DETAIL, perf6_cockpitladder) needs nothing here: enh_preset_tab finds rows by key. */
#ifndef ENH_HAVE_CLOUD_DETAIL
#define ENH_HAVE_CLOUD_DETAIL 1
#endif
#ifndef ENH_HAVE_CANOPY_QUALITY
#define ENH_HAVE_CANOPY_QUALITY 1
#endif
enum { L_NONE, L_CLOUD_SHADOW, L_GROUND_SHADOW, L_OPTICS, L_LOD, L_TDETAIL, L_PAD, L_PAD_ACTIVE, L_RUMBLE, L_TRIGGERS, L_HOTAS,
       L_HAPTICS, L_TOUCH };
typedef struct { ps2_enh_item it; size_t off; unsigned char live; } enh_row;

#define V(...) { __VA_ARGS__ }
/* switch item: key, switch, category, apply, choices, default, built-in choice, label, help, values, names, live */
#define EI(k, e, c, a, n, d, cd, lab, h, vals, names, lv) \
    { { k, e, PS2_ENH_##c, PS2_ENH_##a, n, d, cd, K_ENV, lab, h, vals, names, { 0 } }, 0, lv }
/* ps2_cfg item: key, overriding switch, category, field, K_CFG_I / K_CFG_F, choices, default, built-in choice, label,
   help, names, field values, live */
#define CI(k, e, c, m, kd, n, d, cd, lab, h, names, cv, lv) \
    { { k, e, PS2_ENH_##c, PS2_ENH_LIVE, n, d, cd, kd, lab, h, { 0 }, names, cv }, offsetof(ps2_settings, m), lv }
/* combo row (K_COMBO, its members: enh_combo_tab): key, category, apply, choices the page offers, default, label, help,
   the file's values, names */
#define CO(k, c, a, n, d, lab, h, vals, names) \
    { { k, "", PS2_ENH_##c, PS2_ENH_##a, n, d, -1, K_COMBO, lab, h, vals, names, { 0 } }, 0, L_NONE }
#define ON_OFF V("ON", "OFF")
#define OFF_ON V("OFF", "ON")
#define PCT5 V("OFF", "50%", "100%", "150%", "200%")
#define PCT5V V(0.0f, 0.5f, 1.0f, 1.5f, 2.0f)
static enh_row enh_rows[] = {                /* (not const: ACTIVE CONTROLLER's choices follow the connected pads) */
    /* ---- QUALITY: they apply at the next start (what the swapchain, the pipelines and the packs are built from) ---- */
    { { "quality_preset", "ACZ_QUALITY_PRESET", PS2_ENH_QUALITY, PS2_ENH_RESTART, 3, 2, -1, K_PRESET, "QUALITY PRESET",
        "Sets resolution and the other costly rows at once; HIGH is the full picture.",
        V("low", "medium", "high"), V("LOW", "MEDIUM", "HIGH"), { 0 } }, 0, L_NONE },
    EI("resolution", "PS2_UPSCALE", QUALITY, RESTART, 4, 2, -1, "RESOLUTION",
       "Multiple of the PS2's 640 x 448: 3X saves a third of 4X's GPU time, 2X over half but blurs cockpit text.",
       V("2", "3", "4", "5"), V("2X", "3X", "4X", "5X"), L_NONE),
    CO("edge_level", QUALITY, RESTART, 3, 1, "EDGE SMOOTHING",
       "Jagged world edges smoothed before the HUD and cockpit are drawn; LIGHT keeps far detail sharper.",
       V("off", "light", "strong"), V("OFF", "LIGHT", "STRONG")),
    EI("clouds", "ACZ_VR_CLOUDS", QUALITY, RESTART, 2, 1, 0, "CLOUDS",
       "The game's cloud sprites, or volumetric clouds with real depth that surround you when you fly in.",
       V("", "all"), V("GAME", "VOLUMETRIC"), L_NONE),
    { { "cockpit_3d", "ACZ_VR_COCKPIT_MODEL", PS2_ENH_QUALITY, PS2_ENH_RESTART, 2, 0, -1, K_PATHS, "3D COCKPIT",
        "Stereo 3D cockpits with live instruments in the cockpit view; OFF shows the game's own cockpit.",
        V("1", "0"), ON_OFF, { 0 } }, 0, L_NONE },
    EI("smooth_motion", "PS2_XR_RERENDER", QUALITY, RESTART, 2, 0, 1, "SMOOTH MOTION",
       "Frames between the game's 60 a second are drawn from the motion at the headset's rate.",
       V("1", "0"), ON_OFF, L_NONE),
    EI("movies", "ACZ_FMV_PACK", QUALITY, RESTART, 2, 0, 0, "MOVIES",
       "The game's movies from the 4X upscaled pack where it is installed, else the originals.",
       V("", "0"), V("UPSCALED", "ORIGINAL"), L_NONE),
    { { "reset_all", "", PS2_ENH_QUALITY, PS2_ENH_ACTION, 1, 0, -1, K_ACT_RESET, "RESET ALL TO DEFAULTS",
        "Every row of this page and every F4 setting back to its default, saved at once.",
        { "" }, { "PRESS A" }, { 0 } }, 0, L_NONE },
    /* ---- PICTURE ---- */
    EI("aircraft_detail", "ACZ_LOD_BIAS", PICTURE, LIVE, 3, 2, 0, "AIRCRAFT DETAIL",
       "Distant aircraft keep their detailed models longer: the game's distance x1, x2 or x4.",
       V("", "2", "4"), V("NORMAL", "HIGH", "VERY HIGH"), L_LOD),
    CO("terrain_look", PICTURE, MISSION, 2, 1, "TERRAIN LOOK",
       "REALISTIC adds relief light, mountain shadows and close-up surface detail to the game's ground.",
       V("game", "realistic"), V("GAME", "REALISTIC")),
    EI("ground_textures", "ACZ_TERRAIN_TEXPACK", PICTURE, RESTART, 4, 1, 0, "GROUND TEXTURES",
       "The ground photo upscaled offline: FAITHFUL crisper, CRISP grainier, REAL 4X grass and soil low down.",
       V("", "faithful", "crisp", "x4"), V("GAME", "FAITHFUL 2X", "CRISP 2X", "REAL 4X"), L_NONE),
    EI("shader_water", "ACZ_TERRAIN_WATER", PICTURE, MISSION, 2, 0, 0, "SHADER WATER",
       "Waves, sky and cloud reflections and the sun's glitter on the sea, lakes and rivers.",
       V("", "0"), ON_OFF, L_NONE),
    CO("buildings", PICTURE, MISSION, 3, 2, "BUILDINGS",
       "Houses of the ground photo raised into low buildings, in cities with windows; never solid.",
       V("game", "towns", "towns_cities", "red_roofs", "red_roofs_only"),
       V("GAME", "TOWNS", "TOWNS AND CITIES", "RED ROOFS", "RED ROOFS ONLY")),
    CO("trails_vapour", PICTURE, MISSION, 3, 0, "TRAILS AND VAPOUR",
       "Contrails up high, cloud torn out in wakes and vapour over the wings in hard turns.",
       V("on", "subtle", "off"), V("ON", "SUBTLE", "OFF")),
    CO("smoke", PICTURE, MISSION, 3, 0, "SMOKE",
       "Explosion and missile smoke shaded by the sun as a volume; the 2006 sprites, no GPU cost.",
       V("on", "subtle", "off"), V("ON", "SUBTLE", "OFF")),
    CO("explosions", PICTURE, MISSION, 3, 2, "EXPLOSIONS",
       "SOFT keeps explosions upright with soft edges; VOLUMES turns the biggest bursts into real fireballs.",
       V("game", "soft", "soft_volumes"), V("GAME", "SOFT", "SOFT WITH VOLUMES")),
    EI("forest_distance", "ACZ_TREES_DIST", PICTURE, MISSION, 2, 1, 0, "FOREST DISTANCE",
       "Forests drawn out to the horizon: 3-6x the trees, about 0.4 ms more GPU time low over forests.",
       V("", "2"), V("NORMAL", "FAR"), L_NONE),
    CO("cloud_shadow_reach", PICTURE, LIVE, 3, 2, "CLOUD SHADOWS",
       "Volumetric clouds shade each other and the cockpit's sunlight, and EVERYWHERE also the ground.",
       V("off", "aircraft", "everywhere"), V("OFF", "ON THE AIRCRAFT", "EVERYWHERE")),
    EI("optics", "ACZ_VR_OPTICS", PICTURE, LIVE, 2, 0, 0, "OPTICAL PHENOMENA",
       "Volumetric clouds: halo, glory, your aircraft's shadow and rainbows when sun and weather allow.",
       V("", "0"), ON_OFF, L_OPTICS),
    EI("eye_adapt", "ACZ_VR_EYE_ADAPT", PICTURE, MISSION, 3, 0, 0, "EYE ADAPTATION",
       "Your eyes adapt over seconds: dimmer entering a cloud, bright leaving it, darker after the sun.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    /* ---- COCKPIT ---- */
    EI("start_view", "ACZ_VR_START_VIEW", COCKPIT, MISSION, 2, 0, 0, "START VIEW",
       "The view every mission starts in; R3 still cycles the views.",
       V("", "game"), V("COCKPIT", "GAME"), L_NONE),
    EI("cockpit_screens", "ACZ_VR_MFD", COCKPIT, MISSION, 3, 0, 0, "COCKPIT SCREENS",
       "Radar, weapons, the big map and wingman commands on the cockpit's own screens, else in the HUD.",
       V("", "radar,weapons", "0"), V("ALL", "NO BIG MAP", "OFF"), L_NONE),
    EI("cockpit_sun", "ACZ_VR_COCKPIT_KEY_LIGHT", COCKPIT, MISSION, 2, 0, 0, "SHADOWS FROM THE SUN",
       "Shadows in the 3D cockpit follow the sun; GAME uses the game's strongest light, which can jump.",
       V("", "game"), V("SUN", "GAME"), L_NONE),
    EI("cockpit_scene_light", "ACZ_VR_COCKPIT_SCENE_LIGHT", COCKPIT, MISSION, 3, 0, 0, "LIGHT FROM THE WORLD",
       "The 3D cockpit lit by the sky and ground around you: dim under a storm, bright over snow.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    EI("flash_light", "ACZ_VR_FLASH_LIGHT", COCKPIT, MISSION, 3, 0, 0, "FLASHES LIGHT THE COCKPIT",
       "Launches, explosions, the gun and the afterburner briefly light the cockpit and nearby clouds.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    CI("cloud_light", "ACZ_IMM_CLOUD_LIGHT", COCKPIT, imm_cloud_light, K_CFG_F, 3, 0, 0, "CLOUD LIGHT INSIDE",
       "Inside a cloud the cockpit loses the hard sunlight and takes the cloud's flat grey light.",
       V("ON", "SUBTLE", "OFF"), V(1.0f, 0.5f, 0.0f), L_NONE),
    EI("cockpit_controls", "ACZ_VR_COCKPIT_CONTROLS", COCKPIT, MISSION, 2, 0, 0, "STICK AND THROTTLE MOVE",
       "The 3D cockpit's stick, throttle and rudder pedals follow your controls.",
       V("", "0"), ON_OFF, L_NONE),
    EI("cockpit_mirrors", "ACZ_VR_COCKPIT_MIRRORS", COCKPIT, RESTART, 4, 0, 0, "REAR-VIEW MIRRORS",
       "TONE: the sky's colour, almost free; LIVE: the real view behind; SKY: no ground; OFF: flat grey.",
       V("", "live", "sky", "0"), V("TONE", "LIVE", "SKY", "OFF"), L_NONE),
    CO("canopy_water_level", COCKPIT, LIVE, 4, 2, "WATER ON THE CANOPY",
       "Rain, snow and cloud water on the 3D cockpit's glass: drops that refract the world and run aft.",
       V("off", "light", "normal", "heavy"), V("OFF", "LIGHT", "NORMAL", "HEAVY")),
    CI("canopy_mist", "", COCKPIT, imm_mist, K_CFG_I, 2, 0, 0, "MIST AND CONDENSATION",
       "A fine film inside cloud, and condensation creeping in from the frame that clears slowly.",
       ON_OFF, V(1, 0), L_NONE),
    CI("canopy_ice", "ACZ_VR_CANOPY_ICE", COCKPIT, imm_ice, K_CFG_I, 2, 0, 0, "ICE ON THE CANOPY",
       "Above the freezing level the drops freeze and frost creeps in from the sides; visual only.",
       ON_OFF, V(1, 0), L_NONE),
    CI("canopy_grime", "", COCKPIT, imm_grime, K_CFG_I, 2, 0, 0, "GRIME FROM DEBRIS",
       "Faint smudges and smears after flying through the debris of a close kill; they blow off.",
       ON_OFF, V(1, 0), L_NONE),
    EI("canopy_scratches", "ACZ_VR_CANOPY_SCRATCH", COCKPIT, MISSION, 3, 0, 0, "SCRATCHES ON THE CANOPY",
       "Fine scratches and swirls in the glass catch the sun: a faint halo of lit hairlines.",
       V("", "2", "0"), V("ON", "STRONG", "OFF"), L_NONE),
    CO("glass_reflections", COCKPIT, MISSION, 2, 0, "GLASS REFLECTIONS",
       "Faint glare near the sun, and the cockpit mirrored in the glass against dark ground and at dusk.",
       V("on", "off"), ON_OFF),
    EI("canopy_cracks", "ACZ_VR_CANOPY_CRACKS", COCKPIT, MISSION, 3, 0, 0, "CANOPY DAMAGE",
       "Hits crack the glass from where they came, in proportion to the damage; they stay for the mission.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    /* ---- HUD ---- */
    EI("hud_size", "ACZ_VR_HUD_SCALE", HUD, MISSION, 4, 2, 0, "HUD SIZE",
       "Against the HUD's original angular size; at 1X the pitch ladder matches the horizon.",
       V("1", "1.15", "1.3", "1.5"), V("1X", "1.15X", "1.3X", "1.5X"), L_NONE),
    CO("hud_placement", HUD, MISSION, 3, 2, "HUD PLACEMENT",
       "BEHIND FRAMES: hidden by the cockpit's frames; IN THE GLASS: aiming symbols only through its HUD glass.",
       V("free", "behind_frames", "in_the_glass"), V("FREE", "BEHIND FRAMES", "IN THE GLASS")),
    EI("hud_gauges", "ACZ_VR_HUD_DROP", HUD, MISSION, 2, 0, 0, "HUD ITEMS ON GAUGES",
       "Speed, altitude, heading and the pitch ladder left out where the cockpit has the instrument.",
       V("", "0"), V("HIDDEN", "SHOWN"), L_NONE),
    EI("target_arrow", "ACZ_VR_HUD_ARROW", HUD, MISSION, 3, 0, 0, "TARGET ARROW",
       "The arrow to the target fixed to your head like a helmet cue, to the aircraft, or on the cockpit plane.",
       V("", "aircraft", "cockpit"), V("HELMET", "AIRCRAFT", "COCKPIT"), L_NONE),
    EI("arrow_place", "ACZ_VR_HUD_ARROW_RING", HUD, MISSION, 2, 0, 0, "ARROW PLACE",
       "Nearer the middle of the view (about 16 degrees out), or at the game's place; not for COCKPIT.",
       V("", "1"), V("NEAR CENTRE", "AT THE EDGE"), L_NONE),
    EI("box_size", "ACZ_VR_MARKER_SCALE", HUD, MISSION, 4, 0, 0, "TARGET BOX SIZE",
       "Target boxes and their labels enlarged about the box's centre.",
       V("", "1.25", "1.5", "2"), V("1X", "1.25X", "1.5X", "2X"), L_NONE),
    EI("sharp_boxes", "ACZ_VR_MARKER_SHARP", HUD, MISSION, 3, 0, 0, "SHARP TARGET BOXES",
       "Box outlines from the game's own pixels: smooth line drawings, flat pixels, or the blurred HUD texture.",
       V("", "1", "0"), V("SMOOTH", "PIXELS", "OFF"), L_NONE),
    EI("marker_range", "ACZ_VR_MARKER_RANGE", HUD, MISSION, 2, 1, 1, "MARKER SIZE BY RANGE",
       "Target boxes shrink with range: full size to 1 km, half from 8 km, text unchanged; GAME: one size.",
       V("0", ""), V("GAME", "BY RANGE"), L_NONE),
    EI("wingman_place", "ACZ_VR_HUD_WING", HUD, MISSION, 2, 0, 0, "WINGMAN COMMANDS",
       "In the 3D cockpit the COVER / wingman command icon moves off the cockpit's screens.",
       V("", "0"), V("BESIDE THE SCREENS", "GAME'S PLACE"), L_NONE),
    EI("gaze_target", "ACZ_VR_GAZE_TARGET", HUD, MISSION, 4, 2, 2, "GAZE TARGETING",
       "In the cockpit view the target button picks the target you look at, within this cone.",
       V("0", "10", "", "30"), V("OFF", "10 DEG", "20 DEG", "30 DEG"), L_NONE),
    /* ---- COMFORT: what you feel, and the VR comfort rows ---- */
    EI("g_vision", "ACZ_IMM_GLOC", COMFORT, MISSION, 3, 0, 0, "G EFFECTS ON VISION",
       "Sustained high G greys the edges of your view as with a G-suit; it clears in seconds.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    EI("buffet", "ACZ_IMM_BUFFET", COMFORT, MISSION, 3, 0, 0, "BUFFET",
       "Hard pulls near the wing's limit, the stall and the air brake shake sound, controller and needles.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    EI("blasts", "ACZ_IMM_BLAST", COMFORT, MISSION, 3, 0, 0, "BLASTS ON THE CANOPY",
       "A nearby explosion thumps and rattles the canopy and the controller; no gameplay effect.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    EI("debris", "ACZ_IMM_DEBRIS", COMFORT, MISSION, 2, 0, 0, "FLYING THROUGH DEBRIS",
       "Close kills leave debris: zips and ticks on the canopy, soot flecks and rumble; no gameplay effect.",
       V("", "none"), ON_OFF, L_NONE),
    EI("hit_effects", "ACZ_IMM_HIT_FX", COMFORT, MISSION, 2, 0, 0, "HIT EFFECTS",
       "The crack's snap from its side and a short jolt on the controller when you are hit.",
       V("", "0"), ON_OFF, L_NONE),
    EI("vr_camera_shake", "ACZ_VR_CAMERA_SHAKE", COMFORT, MISSION, 2, 1, 1, "CAMERA SHAKE ON HITS",
       "The game's camera shake at hits; in VR it moves the world against your head.",
       V("1", ""), V("ON", "OFF IN VR"), L_NONE),
    CI("cloud_volume", "ACZ_IMM_VEIL", COMFORT, imm_cloud_volume, K_CFG_F, 5, 2, 2, "CLOUD AROUND YOU",
       "Inside a cloud: a whiteout haze and vapour streaming past in stereo; the sun fades to a glow.",
       PCT5, PCT5V, L_NONE),
    CI("rumble", "ACZ_IMM_RUMBLE", COMFORT, imm_rumble, K_CFG_F, 5, 2, 2, "WEATHER RUMBLE",
       "Cloud turbulence, debris knocks and nearby blasts on the controller, under the game's own rumble.",
       PCT5, PCT5V, L_NONE),
    CO("pilot_sounds", COMFORT, MISSION, 3, 0, "PILOT SOUNDS",
       "Your breathing on the mask's mic, the G-suit and the strain; UNDER EFFORT: no breathing at rest.",
       V("always", "under_effort", "off"), V("ALWAYS", "UNDER EFFORT", "OFF")),
    EI("stereo_depth", "ACZ_VR_STEREO_SCALE", COMFORT, MISSION, 5, 2, 2, "STEREO DEPTH",
       "Eye separation: more gives stronger depth and a smaller-looking world.",
       V("0.8", "0.9", "", "1.1", "1.2"), V("0.8X", "0.9X", "1X", "1.1X", "1.2X"), L_NONE),
    EI("flat_intro", "ACZ_VR_INTRO_FLAT", COMFORT, MISSION, 2, 0, 0, "FLAT MISSION INTRO",
       "The mission intro flyby stays on the flat screen until you have control.",
       V("", "0"), ON_OFF, L_NONE),
    EI("flat_events", "ACZ_VR_EVENT_FLAT", COMFORT, MISSION, 2, 0, 0, "FLAT CUTSCENES",
       "Scenes with the game's own camera stay on the flat screen; your own cockpit view stays 3D.",
       V("", "0"), ON_OFF, L_NONE),
    EI("takeoff_recentre", "ACZ_VR_NO_AUTO_RECENTER", COMFORT, MISSION, 2, 0, 0, "RECENTRE AT TAKE-OFF",
       "The view recentres on your head when a flight starts; holding View for a second does it any time.",
       V("", "1"), ON_OFF, L_NONE),
    { { "recentre_now", "", PS2_ENH_COMFORT, PS2_ENH_ACTION, 1, 0, -1, K_ACT_RECENTRE, "RECENTRE VIEW NOW",
        "Makes the direction you look now the forward direction.", { "" }, { "PRESS A" }, { 0 } }, 0, L_NONE },
    /* ---- SOUND ---- */
    CI("binaural_sound", "ACZ_AUDIO_HRTF", SOUND, imm_hrtf, K_CFG_I, 3, 0, 0, "3D SOUND (HEADPHONES)",
       "Aircraft, missiles and explosions placed around your head as headphones hear them; AUTO: on in VR.",
       V("AUTO", "ON", "OFF"), V(2, 1, 0), L_NONE),
    CI("doppler", "ACZ_AUDIO_DOPPLER", SOUND, imm_doppler, K_CFG_F, 5, 2, 2, "DOPPLER EFFECT",
       "Passing sounds rise and fall in pitch; 100% is the real effect at jet speeds, very strong.",
       V("OFF", "10%", "25%", "50%", "100%"), V(0.0f, 0.1f, 0.25f, 0.5f, 1.0f), L_NONE),
    CI("cockpit_muffle", "ACZ_AUDIO_MUFFLE", SOUND, imm_muffle, K_CFG_F, 3, 2, 2, "COCKPIT MUFFLING",
       "In the cockpit view the canopy dulls the sounds from outside a little.",
       V("OFF", "50%", "100%"), V(0.0f, 0.5f, 1.0f), L_NONE),
    CI("engine_grain", "ACZ_IMM_ENGINE_GRAIN", SOUND, imm_engine_grain, K_CFG_F, 3, 0, 0, "GRANULAR ENGINE",
       "Prototype: your engine rebuilt from grains of its own sound, following throttle and airspeed.",
       V("OFF", "50%", "100%"), V(0.0f, 0.5f, 1.0f), L_NONE),
    CI("sound", "ACZ_IMM_SOUND", SOUND, imm_sound, K_CFG_F, 5, 2, 2, "WEATHER SOUND",
       "Airflow in cloud, rain and snow on the canopy and debris ticks, under the game's sound.",
       PCT5, PCT5V, L_NONE),
    EI("system_sounds", "ACZ_SND_SYSTEMS", SOUND, MISSION, 4, 2, 2, "COCKPIT SYSTEM SOUNDS",
       "Switch clicks, landing gear, flaps, air brake and wheels on the runway, from their place.",
       V("0", "0.5", "", "1.5"), V("OFF", "50%", "100%", "150%"), L_NONE),
    EI("warning_panel", "ACZ_SND_WARN_PLACE", SOUND, MISSION, 2, 0, 0, "WARNING TONES FROM THE PANEL",
       "3D sound: lock, missile and pull-up tones from the panel; the missile alert leans toward it.",
       V("", "0"), ON_OFF, L_NONE),
    EI("flyby", "ACZ_SND_FLYBY", SOUND, MISSION, 3, 0, 0, "CLOSE FLY-BYS",
       "An aircraft passing close: its roar arrives after it, the pitch drops, the exhaust rips past.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    /* ---- CONTROLS ---- */
    EI("active_controller", "ACZ_PAD_ACTIVE", CONTROLS, LIVE, 2, 0, 0, "ACTIVE CONTROLLER",
       "The controller the game follows for button names and vibration; AUTO: the one you used last.",
       V("", "touch"), V("AUTO", "TOUCH"), L_PAD_ACTIVE),
    CI("pad_layout", "ACZ_PAD_LAYOUT", CONTROLS, pad_layout, K_CFG_I, 2, 0, 1, "CONTROLLER LAYOUT",
       "MODERN: RT and LT analog throttle, RB and LB yaw; CLASSIC: the PS2's positions.",
       V("MODERN", "CLASSIC"), V(1, 0), L_PAD),
    CO("rumble_level", CONTROLS, LIVE, 5, 0, "RUMBLE STRENGTH",
       "Vibration on the active controller, pad or Touch; the game's CONTROLLER SETTINGS switch it off.",
       V("auto", "50", "100", "150", "200", "off"), V("AUTO", "50%", "100%", "150%", "200%", "OFF")),
    EI("adaptive_triggers", "ACZ_PAD_TRIGGERS", CONTROLS, LIVE, 3, 0, 0, "ADAPTIVE TRIGGERS",
       "DualSense: R2 has a gate where the afterburner starts; L2 stiffens with airspeed at the air brake.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_TRIGGERS),
    { { "rumble_test", "", PS2_ENH_CONTROLS, PS2_ENH_ACTION, 1, 0, -1, K_ACT_RUMBLE, "TEST VIBRATION",
        "Three short pulses on the active controller; on a DualSense L2 and R2 then resist for 4 s.",
        { "" }, { "PRESS A" }, { 0 } }, 0, L_NONE },
    EI("stick_dead_zone", "ACZ_PAD_STICK_REMAP", CONTROLS, MISSION, 2, 0, 0, "STICK DEAD ZONE",
       "The game ignores the inner half of each stick axis; NONE: the aircraft answers from the first movement.",
       V("", "1"), V("THE GAME'S", "NONE"), L_NONE),
    /* (shown only with a joystick connected: ps2_enh_hotas_rows) */
    EI("throttle_lever", "ACZ_HOTAS_THROTTLE", CONTROLS, LIVE, 3, 0, 0, "THROTTLE LEVER",
       "A joystick's throttle: CENTRED ahead of neutral accelerates, SPRING a centred axis, SPLIT two axes.",
       V("", "spring", "split"), V("CENTRED", "SPRING", "SPLIT"), L_HOTAS),
    { { "lever_neutral", "", PS2_ENH_CONTROLS, PS2_ENH_ACTION, 1, 0, -1, K_ACT_NEUTRAL, "SET LEVER NEUTRAL",
        "Leave the throttle lever where it should neither accelerate nor brake, then press.",
        { "" }, { "PRESS A" }, { 0 } }, 0, L_NONE },
    /* (shown only once a Touch controller has been seen, or with ACZ_TOUCH set: ps2_enh_touch_rows) */
    EI("touch_controllers", "ACZ_TOUCH", CONTROLS, RESTART, 4, 0, 0, "TOUCH CONTROLLERS",
       "Quest controllers as a pad or as hands on stick and throttle; AUTO: a pad until a hand takes the stick.",
       V("", "pad", "stick", "off"), V("AUTO", "AS A PAD", "HANDS ON STICK", "OFF"), L_TOUCH),
    EI("touch_grab", "ACZ_TOUCH_GRAB", CONTROLS, MISSION, 3, 2, 2, "GRAB WITH",
       "The button that closes your hand; EITHER: grip anywhere (AUTO: at the lever), trigger at the lever.",
       V("grip", "trigger", ""), V("GRIP", "TRIGGER", "EITHER"), L_TOUCH),
    EI("touch_grip", "ACZ_TOUCH_GRIP", CONTROLS, MISSION, 2, 0, 0, "GRIP",
       "Hands on stick: hold the grip to hold the stick, or tap to take and tap again to let go.",
       V("", "toggle"), V("HOLD", "TAP TO TAKE"), L_TOUCH),
    EI("stick_throw", "ACZ_TOUCH_STICK_THROW", CONTROLS, MISSION, 4, 0, 0, "STICK THROW",
       "How far you tilt the controller for full stick; AS THE COCKPIT: its own stick, or 20 degrees.",
       V("", "15", "25", "30"), V("AS THE COCKPIT", "15 DEGREES", "25 DEGREES", "30 DEGREES"), L_TOUCH),
    EI("stick_response", "ACZ_TOUCH_STICK_EXPO", CONTROLS, MISSION, 3, 1, 1, "STICK RESPONSE",
       "Soft centre: small tilts move the aircraft gently, full tilt is still full stick.",
       V("0", "", "0.5"), V("LINEAR", "SOFT CENTRE", "VERY SOFT"), L_TOUCH),
    EI("touch_throttle", "ACZ_TOUCH_THROTTLE", CONTROLS, MISSION, 4, 0, 0, "THROTTLE",
       "Hands on stick: the left hand fore and aft with detents, or its tilt, or the left thumbstick.",
       V("", "spring", "tilt", "thumbstick"), V("LEVER STAYS", "LEVER SPRINGS BACK", "TILT", "THUMBSTICK"), L_TOUCH),
    EI("right_thumbstick", "ACZ_TOUCH_RS", CONTROLS, MISSION, 2, 0, 0, "RIGHT THUMBSTICK",
       "Touch: flick for the D-pad (wingman commands, menus), or look around with it.",
       V("", "look"), V("D-PAD", "LOOK"), L_TOUCH),
    EI("left_handed", "ACZ_TOUCH_SWAP", CONTROLS, MISSION, 2, 1, 1, "LEFT-HANDED",
       "Hands on stick: the stick in the left hand, the throttle in the right.",
       V("1", ""), ON_OFF, L_TOUCH),
    EI("pilot_hands", "ACZ_VR_HANDS", CONTROLS, MISSION, 3, 0, 0, "PILOT HANDS",
       "Gloved hands on the stick and throttle; AUTO: only in HANDS ON STICK with the Touch controllers.",
       V("", "1", "0"), V("AUTO", "ON", "OFF"), L_TOUCH),
    { { "touch_card", "", PS2_ENH_CONTROLS, PS2_ENH_ACTION, 1, 0, -1, K_ACT_CARD, "SHOW TOUCH CONTROLS",
        "Both Quest controllers and what each button does; also in the pause menu, or hold Menu 1 s in flight.",
        { "" }, { "PRESS A" }, { 0 } }, 0, L_NONE },
    /* ---- ini only (PS2_ENH_CAT_INI): their keys and injection as ever, F4 > Enhancements > Expert (ini), never the page ---- */
    EI("terrain_detail", "ACZ_TERRAIN_DETAIL", CAT_INI, LIVE, 2, 1, 0, "TERRAIN DETAIL",
       "The fine terrain mesh reaches twice as far, as in the PC version.",
       V("", "2"), V("NORMAL", "HIGH"), L_TDETAIL),
    EI("city_detail", "ACZ_TERRAIN_CITY_LOOK", CAT_INI, RESTART, 2, 0, 0, "CITY DETAIL",
       "The city buildings close up: windows with glass and frames on whole bays, cornices and eaves, the street's shade at their feet, the low sun on the upper floors, parapets, roof boxes and tiles instead of the photo's blurred roof texels. No cost far out. OFF: the plain boxes.",
       V("", "0"), ON_OFF, L_NONE),
    EI("heat_haze", "ACZ_VR_HEAT_HAZE", CAT_INI, MISSION, 2, 0, 0, "HEAT HAZE",
       "Shimmering air behind nearby engines and your own afterburner.",
       V("", "0"), ON_OFF, L_NONE),
    EI("tunnel_light", "ACZ_VR_TUNNEL_LIGHT", CAT_INI, RESTART, 2, 0, 0, "LIGHT IN TUNNELS",
       "Under a roof, in a tunnel or behind a ridge the sun no longer lights the cockpit, glints on the canopy's scratches or lights the smoke; your eyes adapt to the dark and the bright exit dazzles for a moment (with EYE ADAPTATION on).",
       V("", "0"), ON_OFF, L_NONE),
    EI("cloud_wind", "ACZ_VR_CLOUD_DRIFT", CAT_INI, MISSION, 3, 0, 0, "CLOUDS DRIFT WITH THE WIND",
       "Volumetric clouds: the game moves its clouds with the wind (over 400 km/h in missions 8 and 9), but your aircraft never drifts with that air, so a cloud deck slides sideways past you. SLOW: a quarter of the game's drift; NONE: the clouds stay over the ground.",
       V("", "0.25", "0"), V("GAME", "SLOW", "NONE"), L_NONE),
    EI("game_cloud_base", "ACZ_VR_CLOUD_BASE", CAT_INI, RESTART, 2, 1, 1, "GAME CLOUDS STAY IN PLACE",
       "Game clouds: a near cloud no longer pops in or out when you turn your head. Costs more in overcast.",
       V("1", ""), ON_OFF, L_NONE),
#if ENH_HAVE_CLOUD_DETAIL
    EI("cloud_detail", "ACZ_VR_CLOUD_DETAIL", CAT_INI, RESTART, 3, 0, 0, "CLOUD DETAIL",
       "Volumetric clouds: HIGH as made; MEDIUM and LOW draw the nearby cloud volume at a lower internal resolution for slower GPUs.",
       V("", "medium", "low"), V("HIGH", "MEDIUM", "LOW"), L_NONE),
#endif
    EI("weapon_screen", "ACZ_VR_MFD_ROWS", CAT_INI, MISSION, 2, 0, 0, "WEAPON SCREEN",
       "Large text: only the four weapon rows, 1.3-1.6x larger (no damage silhouette or missile icons).",
       V("", "1"), V("NORMAL", "LARGE TEXT"), L_NONE),
    EI("cockpit_aa", "ACZ_VR_COCKPIT_MSAA", CAT_INI, RESTART, 3, 0, 0, "COCKPIT ANTI-ALIASING",
       "Multisampling of the 3D cockpit. Lower values save GPU time.",
       V("", "2", "1"), V("4X", "2X", "OFF"), L_NONE),
    EI("cockpit_edge", "ACZ_VR_COCKPIT_MSAA_AUTO", CAT_INI, RESTART, 2, 0, 0, "COCKPIT EDGE QUALITY",
       "Auto: the Su-27 and F-15 cockpits at 2X anti-aliasing, 0.15-0.25 ms less GPU time, slightly stepped edges.",
       V("", "su27.cpit,f15c.cpit"), V("BEST", "AUTO"), L_NONE),
    EI("cockpit_detail", "ACZ_VR_COCKPIT_DETAIL", CAT_INI, RESTART, 3, 0, 0, "COCKPIT DETAIL",
       "For weaker PCs. MEDIUM: fewer tiny triangles (bolt heads, the shading's fine mesh), 2X anti-aliasing on the densest cockpits, textures within 256 MB. LOW: simpler still, 2X on all, 128 MB (softer up close). HIGH: as built.",
       V("", "medium", "low"), V("HIGH", "MEDIUM", "LOW"), L_NONE),
#if ENH_HAVE_CANOPY_QUALITY
    EI("canopy_quality", "ACZ_VR_CANOPY_QUALITY", CAT_INI, RESTART, 3, 0, 0, "CANOPY QUALITY",
       "The canopy glass and its weather: HIGH as made; MEDIUM and LOW shade them more cheaply for slower GPUs (softer towards the edges of the view).",
       V("", "medium", "low"), V("HIGH", "MEDIUM", "LOW"), L_NONE),
#endif
    CI("drop_size", "", CAT_INI, imm_drop_size, K_CFG_F, 4, 1, 1, "DROP SIZE",
       "Size of the drops and rivulets; larger drops read better in the headset.",
       V("0.75X", "1X", "1.5X", "2X"), V(0.75f, 1.0f, 1.5f, 2.0f), L_NONE),
    EI("drop_look", "ACZ_VR_CANOPY_DROPS", CAT_INI, MISSION, 2, 0, 0, "DROP LOOK",
       "Each drop a lens of water (the world upside down in it, a dark rim, a trail at speed), or the classic drops.",
       V("", "classic"), V("LENS", "CLASSIC"), L_NONE),
    EI("canopy_crack_growth", "ACZ_VR_CANOPY_CRACK_CREEP", CAT_INI, MISSION, 2, 0, 0, "CRACK GROWTH",
       "Cracked acrylic creeps on slowly under hard G, buffet and high speed.",
       V("", "0"), ON_OFF, L_NONE),
    EI("sound_bus", "ACZ_IMM_DUCK", CAT_INI, MISSION, 2, 0, 0, "ADDED SOUNDS GIVE WAY",
       "Wheels, airflow and breathing step back when the game is loud, so the engine, the music and the radio stay clear.",
       V("", "0"), ON_OFF, L_NONE),
    EI("head_tracked_sound", "ACZ_VR_AUDIO_HEAD", CAT_INI, RESTART, 2, 0, 0, "HEAD-TRACKED SOUND",
       "The game's sound listener turns with your head, so sounds stay in place when you look around.",
       V("", "0"), ON_OFF, L_NONE),
    EI("memcard_notice", "ACZ_MEMCHECK_HIDE", CAT_INI, RESTART, 2, 0, 0, "MEMORY CARD NOTICE",
       "At start-up the PS2 memory card check runs on a black screen. A card error or a question still shows.",
       V("", "0"), V("HIDDEN", "SHOWN"), L_NONE),
    EI("briefing_text", "ACZ_FMV_BRIEF_MODEL", CAT_INI, RESTART, 2, 0, 0, "BRIEFING TEXT",
       "Briefing movies: their letters upscaled faithfully (as on the disc) inside the AI picture, or the AI's sharper but guessed letters.",
       V("", "0"), V("FAITHFUL", "AI GUESSED"), L_NONE),
    EI("story_text", "ACZ_FMV_MOVIEUS_MODEL", CAT_INI, RESTART, 2, 0, 0, "STORY MOVIE TEXT",
       "Story movies: the letters of their maps, captions, name cards and credits upscaled faithfully, or the AI's sharper but guessed letters.",
       V("", "0"), V("FAITHFUL", "AI GUESSED"), L_NONE),
    EI("tv_shots", "ACZ_FMV_TV_SHOTS", CAT_INI, RESTART, 2, 0, 0, "TV NEWS FOOTAGE",
       "Story movies: the OBC news and archive footage with its TV scanlines drawn cleanly, as on a TV, or as the AI upscaled them (heavy stripes and bricks).",
       V("", "0"), V("CLEAN TV LINES", "AS UPSCALED"), L_NONE),
    EI("governor", "ACZ_VR_GOVERNOR", CAT_INI, RESTART, 2, 1, 0, "FRAME-TIME GOVERNOR",
       "Over the headset's frame budget it turns effects down (glints, edge smoothing, water). Off: always the full picture.",
       V("", "0"), ON_OFF, L_NONE),
    EI("gpu_boost", "PS2_PERF_GPU_BOOST", CAT_INI, RESTART, 2, 0, 0, "GPU CLOCK BOOST",
       "NVIDIA: keeps the GPU clocks up, so frames after pauses and loading are not slow.",
       V("", "0"), ON_OFF, L_NONE),
    EI("foveated", "PS2_VRS_FOV", CAT_INI, RESTART, 3, 0, 0, "FOVEATED SHADING",
       "Coarse shading where the headset's lenses never show the image (lens edge), or in rings (zones).",
       V("", "zones", "0"), V("LENS EDGE", "ZONES", "OFF"), L_NONE),
    EI("record_early", "PS2_PERF_RECORD_EARLY", CAT_INI, RESTART, 2, 1, 0, "EARLY FRAME RECORDING",
       "Each game frame is recorded for the GPU as soon as it is ready: lower latency. Off by default until a headset test: it may cost lost frames (bug #89).",
       V("", "0"), ON_OFF, L_NONE),
    EI("ee_idle", "PS2_PERF_EE_IDLE", CAT_INI, RESTART, 2, 1, 1, "EE IDLE WAIT",
       "Opt-in: the emulated CPU sleeps instead of spinning while it waits for the next field (frees a core).",
       V("1", ""), ON_OFF, L_NONE),
    EI("gfx_split", "PS2_GFX_SPLIT", CAT_INI, RESTART, 2, 1, 1, "TWO-STAGE GRAPHICS",
       "Opt-in: the graphics worker in two threads, faster in the heaviest scenes (not yet tried in the headset).",
       V("1", ""), ON_OFF, L_NONE),
    EI("xr_depth", "PS2_XR_DEPTH", CAT_INI, RESTART, 2, 1, 1, "DEPTH TO THE HEADSET",
       "Opt-in: each frame's depth goes to the headset, so when it has to fill in a frame the cockpit follows your head's movement, not only its turn (not yet tried in the headset).",
       V("auto", ""), ON_OFF, L_NONE),
    /* ---- the combo rows' members (PS2_ENH_CAT_OFF): each still sets its switch at start exactly as a row did; only the
       file loses their keys (enh_combo_tab; the first save after the migration writes the combo row's key instead) ---- */
    EI("edge_smoothing", "PS2_EDGE_AA", CAT_OFF, RESTART, 2, 0, 1, "EDGE SMOOTHING",
       "Smooths the jagged edges of the world before the HUD and the cockpit are drawn (about 0.15 ms at 4X).",
       V("1", "0"), ON_OFF, L_NONE),
    EI("edge_strength", "PS2_EDGE_AA_PARAM", CAT_OFF, RESTART, 2, 0, 1, "EDGE SMOOTHING STRENGTH",
       "Light keeps textures and the far shore sharper (bug #54); strong smooths more of each pixel (FXAA default).",
       V("0.125,0.0312,0.25", ""), V("LIGHT", "STRONG"), L_NONE),
    CI("realistic_terrain", "ACZ_TERRAIN_REAL", CAT_OFF, terrain_real, K_CFG_I, 2, 0, 0, "REALISTIC TERRAIN",
       "Relief light, gullies and mountain shadows from the game's own height map. Shading only.",
       ON_OFF, V(1, 0), L_NONE),
    EI("ground_materials", "ACZ_TERRAIN_MATERIALS", CAT_OFF, MISSION, 2, 0, 0, "GROUND MATERIALS",
       "Close to the ground the photo gets real surface detail: grass, sand, rock, snow, soil (about 0.2 ms low down).",
       V("", "0"), ON_OFF, L_NONE),
    EI("ground_detail", "ACZ_TERRAIN_DETAIL_HOLD", CAT_OFF, MISSION, 2, 1, 0, "GROUND DETAIL",
       "The ground's fine detail texture keeps its full strength further out (low flight, e.g. mission 4).",
       V("", "0.6"), V("GAME", "STRONGER"), L_NONE),
    EI("town_houses", "ACZ_TERRAIN_PROPS", CAT_OFF, MISSION, 3, 2, 0, "HOUSES IN TOWNS",
       "Houses of the ground photo raised into low buildings: red roofs (missions 2, 9, 12), or all (also mission 4's desert buildings and white tanks). Never near targets; not solid.",
       V("", "1", "2"), V("OFF", "RED ROOFS", "ALL"), L_NONE),
    EI("city_buildings", "ACZ_TERRAIN_CITY", CAT_OFF, MISSION, 2, 0, 0, "CITY BUILDINGS",
       "With HOUSES IN TOWNS on: the dense houses of a city photo raised into buildings with rows of windows, taller in the centre (mission 6's capital). They grow out of the photo between 1.7 and 2.5 km, so nothing pops up. About 0.1 ms GPU over the city. Not solid.",
       V("", "0"), ON_OFF, L_NONE),
    EI("contrails", "ACZ_VR_CONTRAILS", CAT_OFF, MISSION, 2, 0, 0, "CONTRAILS",
       "White trails behind the engines where the air is cold enough: high up (above about 6.5 km in the snow missions, 9 km elsewhere), long-lived in moist air.",
       V("", "0"), ON_OFF, L_NONE),
    EI("cloud_wake", "ACZ_VR_CLOUD_WAKE", CAT_OFF, MISSION, 3, 0, 0, "CLOUD WAKE",
       "Leaving a cloud, an aircraft drags a short torn veil of it out in its wake, and near the cloud its two wing-tip vortices draw thin threads; they sink, fray and evaporate within seconds. Yours and the other aircraft's; needs the volumetric clouds.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    EI("wing_vapour", "ACZ_VR_VAPOUR", CAT_OFF, MISSION, 2, 0, 0, "WING VAPOUR",
       "Vapour over the wings in hard turns in humid air.",
       V("", "0"), ON_OFF, L_NONE),
    EI("smoke_volume", "ACZ_VR_SMOKE_VOLUME", CAT_OFF, MISSION, 3, 0, 0, "SMOKE VOLUME",
       "Explosion smoke shaded as a volume from the game's own sprites: the side toward the sun and sky bright, the core and underside dark, the fire glowing through it while it burns. The 2006 sprites, only their shading; no GPU cost.",
       V("", "0.5", "0"), V("ON", "SUBTLE", "OFF"), L_NONE),
    EI("smoke_light", "ACZ_VR_SMOKE_LIGHT", CAT_OFF, MISSION, 2, 0, 0, "SMOKE LIGHT",
       "Missile and wreck smoke brighter toward the sun, darker away from it and under cloud shadows.",
       V("", "0"), ON_OFF, L_NONE),
    EI("explosion_cards", "ACZ_VR_FX_BILLBOARD", CAT_OFF, MISSION, 2, 0, 0, "EXPLOSIONS STAY IN THE WORLD",
       "Explosions, smoke and fire stay upright in the world and face you: they no longer turn when you tilt your head or the aircraft rolls, nor shrink when you look away. The 2006 sprites, only placed; no GPU cost.",
       V("", "0"), ON_OFF, L_NONE),
    EI("explosion_soft", "ACZ_VR_FX_SOFT", CAT_OFF, MISSION, 2, 0, 0, "SOFT EXPLOSION EDGES",
       "Smoke and fire fade where they meet the ground, an aircraft, haze or a cloud instead of being cut along a hard line. About 0.1 ms of GPU time with explosions in view.",
       V("", "0"), ON_OFF, L_NONE),
    EI("explosion_burst", "ACZ_VR_FX_BURST", CAT_OFF, MISSION, 2, 0, 0, "BIG BURSTS AS VOLUMES",
       "Pixy's huge MPBM burst in the last mission becomes a real fireball you see in depth: a white-hot core cooling through yellow and orange to deep red and smoke, soft edges, its size and timing the game's own. About 0.15 ms of GPU time while it shows, up to 0.5 ms with you inside it.",
       V("", "0"), ON_OFF, L_NONE),
    EI("cloud_shadows", "ACZ_VR_CLOUD_SHADOW", CAT_OFF, LIVE, 2, 0, 0, "CLOUD SHADOWS",
       "Volumetric clouds: they shade each other, and the cockpit's sunlight dims in their shadow.",
       V("", "0"), ON_OFF, L_CLOUD_SHADOW),
    EI("ground_shadows", "ACZ_VR_CLOUD_SHADOW_GROUND", CAT_OFF, LIVE, 2, 0, 0, "SHADOWS ON THE GROUND",
       "Volumetric clouds: soft cloud shadows moving over the terrain, the sea and the trees.",
       V("", "0"), ON_OFF, L_GROUND_SHADOW),
    CI("canopy_water", "ACZ_VR_CANOPY", CAT_OFF, imm_canopy, K_CFG_I, 2, 0, 0, "WATER ON THE CANOPY",
       "Rain, snow and cloud water on the 3D cockpit's glass: drops that refract the world and run aft.",
       ON_OFF, V(1, 0), L_NONE),
    CI("water_strength", "ACZ_VR_CANOPY_GAIN", CAT_OFF, imm_water, K_CFG_F, 4, 1, 1, "WATER AMOUNT",
       "How much water collects for a given rain, snow or cloud density.",
       V("50%", "100%", "150%", "200%"), V(0.5f, 1.0f, 1.5f, 2.0f), L_NONE),
    EI("canopy_glare", "ACZ_VR_CANOPY_GLARE", CAT_OFF, MISSION, 2, 0, 0, "GLARE ON THE CANOPY",
       "The canopy glass brightens faintly when you look close to the sun.",
       V("", "0"), ON_OFF, L_NONE),
    EI("canopy_reflections", "ACZ_VR_CANOPY_REFLECT", CAT_OFF, MISSION, 2, 0, 0, "COCKPIT REFLECTIONS",
       "The cockpit faintly mirrored in the canopy: against dark ground, at dusk and at night.",
       V("", "0"), ON_OFF, L_NONE),
    EI("hud_behind_frames", "ACZ_VR_HUD_OCCLUDE", CAT_OFF, MISSION, 2, 0, 0, "HUD BEHIND FRAMES",
       "Target boxes and the gun cross are hidden by the cockpit's frames and panels, seen through the glass.",
       V("", "0"), ON_OFF, L_NONE),
    EI("hud_glass", "ACZ_VR_HUD_GLASS", CAT_OFF, MISSION, 2, 0, 0, "HUD ONLY IN THE GLASS",
       "Aiming symbols only through the cockpit's HUD glass, like a real collimator (cockpits that have one).",
       V("", "0"), ON_OFF, L_NONE),
    EI("breathing", "ACZ_IMM_BREATH", CAT_OFF, MISSION, 3, 2, 0, "BREATHING IN THE MASK",
       "Your breathing on the mask's hot mic, band-limited like the intercom: under effort (hard turns, missile alerts) or always. Real pilots mostly turn it down.",
       V("", "1", "1,calm"), V("OFF", "UNDER EFFORT", "ALWAYS"), L_NONE),
    EI("g_suit", "ACZ_IMM_GSUIT", CAT_OFF, MISSION, 2, 1, 0, "G-SUIT",
       "A low whump below you when the G-suit starts to fill in a hard pull, a soft puff when it vents. Felt more than heard in a real jet.",
       V("", "1"), OFF_ON, L_NONE),
    EI("strain", "ACZ_IMM_STRAIN", CAT_OFF, MISSION, 2, 0, 0, "STRAINING UNDER G",
       "In sustained hard turns (above about 5.5 G) the anti-G straining on the hot mic: a gasp, a grunt, the held breath, the release.",
       V("", "0"), ON_OFF, L_NONE),
    EI("rumble_strength", "ACZ_RUMBLE_GAIN", CAT_OFF, LIVE, 6, 0, 0, "RUMBLE STRENGTH",
       "The game's own vibration on the controller. AUTO: 150% on an Xbox pad, whose motors are weaker than the PS2 pad's; 100% on a PlayStation pad and Touch.",
       V("", "0", "0.5", "1", "1.5", "2"), V("AUTO", "OFF", "50%", "100%", "150%", "200%"), L_RUMBLE),
    EI("touch_vibration", "ACZ_TOUCH_HAPTICS", CAT_OFF, LIVE, 5, 2, 2, "CONTROLLER VIBRATION",
       "Touch: the game's rumble, the weather, and the throttle's detent clicks. Strong vibration can make the Quest controllers report phantom input.",
       V("0", "0.25", "", "0.75", "1"), V("OFF", "25%", "50%", "75%", "100%"), L_HAPTICS),
};
#undef EI
#undef CI
#undef CO
#undef V
#define ENH_N ((int)(sizeof enh_rows / sizeof enh_rows[0]))

static const char *const enh_cat_names[PS2_ENH_CATS] = {
    "QUALITY", "PICTURE", "COCKPIT", "HUD", "COMFORT", "SOUND", "CONTROLS" };
static const char *enh_cat_helps[PS2_ENH_CATS] = {     /* (one line of both help fonts: ACZ_DEBUG_ENH, ACZ_DEBUG_PAUSE measure them) */
    "Preset, resolution, edges, clouds and movies.",
    "Aircraft, terrain, water, smoke, explosions.",
    "Start view, screens, light, canopy weather.",
    "HUD size and place, target arrow, boxes, gaze.",
    "G, buffet, hits, pilot sounds, depth, recentring.",
    "3D sound, Doppler, engine and cockpit sounds.",
    "Controller, layout, vibration; joystick and Touch." };

/* ---- K_COMBO: one page row for several switches (menu_1105a, the settings review of 2026-11-05) ----------------------
   A row of kind K_COMBO has no switch of its own. Its choices each set several member rows, by this sub-table, as
   enh_preset_tab does for the QUALITY PRESET. A member is an ordinary row of the table (category PS2_ENH_CAT_OFF): it sets
   its switch at start-up exactly as before (enh_preload), it applies live as before, and ENV_REFERENCE documents it;
   only the file loses its key. A member's value per choice is written as the member's own val[] string, or the number
   for a cfg-backed member. "*" = no effect under this choice, so any value of it matches; when the choice is set the
   member goes to its default ("*0": to this value instead).
   - nall >= it->n: choices beyond the row's n are reachable in the file only (BUILDINGS' red_roofs: on no page).
   - The row's saved choice is derived from its members (enh_combo_sync), the nearest one when none matches exactly.
   - Held by the environment when any member is (ps2_enh_override, ps2_enh_held_by name that switch).
   - The file (A5): the row's key wins; else, when any member's old key is present, enh_combo_resolve leaves the members
     at their old values for this run (nothing changes), logs `settings: migrated <row>: a=.., b=.. -> <row>=<NAME>
     (exact|nearest)` and the next save writes the row's key instead of the old ones. "nearest" then changes the look at
     the next start: `*` on the row until then (ps2_enh_pending). */
#define ENH_COMBO_MEMBERS 3
typedef struct { const char *key, *v[PS2_ENH_CHOICES]; } enh_combo_member;
typedef struct { const char *row; unsigned char nall; enh_combo_member m[ENH_COMBO_MEMBERS]; } enh_combo_def;
static const enh_combo_def enh_combo_tab[] = {
    { "edge_level", 3, { { "edge_smoothing", { "0", "1", "1" } },
                         { "edge_strength",  { "*", "0.125,0.0312,0.25", "" } } } },   /* (PS2_EDGE_AA=0: the parameters are not read) */
    { "terrain_look", 2, { { "realistic_terrain", { "0", "1" } },
                           { "ground_materials",  { "0", "" } },
                           { "ground_detail",     { "", "0.6" } } } },
    { "buildings", 5, { { "town_houses",    { "", "2", "2", "1", "1" } },
                        { "city_buildings", { "*", "0", "", "", "0" } } } },            /* (no houses: no city, zero_props.c) */
    { "trails_vapour", 3, { { "contrails",   { "", "", "0" } },
                            { "cloud_wake",  { "", "0.5", "0" } },
                            { "wing_vapour", { "", "", "0" } } } },
    { "smoke", 3, { { "smoke_volume", { "", "0.5", "0" } },
                    { "smoke_light",  { "", "", "0" } } } },
    { "explosions", 3, { { "explosion_cards", { "0", "", "" } },
                         { "explosion_soft",  { "0", "", "" } },
                         { "explosion_burst", { "0", "0", "" } } } },
    { "cloud_shadow_reach", 3, { { "cloud_shadows",  { "0", "", "" } },
                                 { "ground_shadows", { "*0", "0", "" } } } },           /* (live: the ground's map would freeze) */
    { "canopy_water_level", 4, { { "canopy_water",   { "0", "1", "1", "1" } },
                                 { "water_strength", { "*", "0.5", "1", "1.5" } } } },
    { "glass_reflections", 2, { { "canopy_glare",       { "", "0" } },
                                { "canopy_reflections", { "", "0" } } } },
    { "hud_placement", 3, { { "hud_behind_frames", { "0", "", "" } },
                            { "hud_glass",         { "*", "0", "" } } } },              /* (ps2_video.c hg_init: glass needs occlusion) */
    { "pilot_sounds", 3, { { "breathing", { "1,calm", "1", "" } },
                           { "g_suit",    { "1", "1", "" } },
                           { "strain",    { "", "", "0" } } } },
    { "rumble_level", 6, { { "rumble_strength", { "", "0.5", "1", "1.5", "2", "0" } },     /* (off: in the file only; the page's off
                                                                                               switch is the game's VIBRATION) */
                           { "touch_vibration", { "", "0.25", "", "0.75", "1", "" } } } },
};
#define ENH_CX_N ((int)(sizeof enh_combo_tab / sizeof enh_combo_tab[0]))
typedef struct {
    int row;                                 /* the member's row */
    signed char ch[PS2_ENH_CHOICES];         /* its choice per combo choice, -1 any */
    signed char set[PS2_ENH_CHOICES];        /* ... the choice it takes when the combo choice is set */
} enh_cmember;
static struct { int row, nall, nm; enh_cmember m[ENH_COMBO_MEMBERS]; unsigned char file_key, migrated, exact; } enh_cx[ENH_CX_N];

static int enh_marker;                       /* ACZ_MENU_SETTINGS=1 */
static signed char enh_saved[ENH_N];         /* switch items: the saved choice (-1: a hand-written value, enh_raw) */
static char enh_raw[ENH_N][64];
static signed char enh_run[ENH_N];           /* the choice in effect in this run (-1 other) */
static char *enh_ovr[ENH_N];                 /* the environment value that holds the item in this run */
static unsigned char enh_mission[ENH_N];     /* changed this session, takes effect at the next mission */
static int enh_injected, enh_kept;
static char enh_injected_list[1200];
/* the NEXT MISSION tier (ps2_menv.h): the rows published at the edges of this run: their value (a literal of the table, NULL =
   the built-in default) and the value's validity; enh_mission[] above = saved on the page, waits for the next edge */
volatile unsigned ps2_menv_gen;
volatile unsigned long long ps2_menv_field;
static const char *volatile enh_mv[ENH_N];
static volatile unsigned char enh_mv_on[ENH_N];
static const char *enh_boot_gate[ENH_N];     /* a row whose resources are made only when it starts on: its "off" value */
static unsigned char enh_boot_off[ENH_N];    /* ... it started off: going on needs a restart */
static volatile char enh_mlock;              /* the page's writes to a row's members against the edge */
static void enh_mlock_take(void) { while (__atomic_test_and_set(&enh_mlock, __ATOMIC_ACQUIRE)) { } }
static void enh_mlock_give(void) { __atomic_clear(&enh_mlock, __ATOMIC_RELEASE); }
static signed char enh_row_cx[ENH_N];        /* a K_COMBO row's index in enh_cx, else -1 */
static unsigned char enh_member[ENH_N];      /* 1: the row is a member of a combo row: the file does not have its key */
static unsigned char enh_file_has[ENH_N];    /* the file has the row's own key (a member's old key, read at start) */
static float enh_file_cfg[ENH_N];            /* ... its value, for a cfg-backed member */
static int enh_cfg_ready;                    /* ps2_cfg has been loaded (before: a cfg-backed member's value is the file's) */
static char enh_cx_msg[300];                 /* the start-up log: what enh_combo_tab could not use */
static int enh_lch_show = 1;                 /* [enhancements] launcher: 0 = the player's "do not show again" */
static char enh_lch_changed[400];            /* [enhancements] launcher_changed: the keys that waited for this start */
static int enh_lch_dirty;                    /* the launcher changed something: its commit saves */
static char *enh_cpit_keep[2];               /* the launcher word's model files, while 3D COCKPIT OFF keeps them out of the environment */
static int (*volatile enh_detail_fn)(float, float);
void ps2_enh_set_detail_fn(int (*fn)(float, float)) { enh_detail_fn = fn; }

int ps2_enh_count(void) { return ENH_N; }
const ps2_enh_item *ps2_enh_item_at(int i) { return i >= 0 && i < ENH_N ? &enh_rows[i].it : NULL; }
const char *ps2_enh_cat_name(int c) { return c >= 0 && c < PS2_ENH_CATS ? enh_cat_names[c] : "?"; }
const char *ps2_enh_cat_help(int c) { return c >= 0 && c < PS2_ENH_CATS ? enh_cat_helps[c] : ""; }
const char *ps2_menv(const char *name) {
    if (__atomic_load_n(&ps2_menv_gen, __ATOMIC_ACQUIRE))
        for (int i = 0; i < ENH_N; i++)
            if (enh_mv_on[i] && !strcmp(enh_rows[i].it.env, name)) return enh_mv[i];
    return getenv(name);
}
void ps2_menv_note(const char *name, double o, double n) {
    if (o != n && ps2_menv_gen) ps2_log("mission: %s %g -> %g", name, o, n);   /* (never before the first edge) */
}
int ps2_enh_find(const char *key) {
    for (int i = 0; i < ENH_N; i++) if (!strcmp(enh_rows[i].it.key, key)) return i;
    return -1;
}
int ps2_enh_applied(void) { return enh_marker; }

static int enh_is_cfg(int i) { return enh_rows[i].it.kind == K_CFG_I || enh_rows[i].it.kind == K_CFG_F; }
static float enh_cfg_get(int i) {
    const char *p = (const char *)&ps2_cfg + enh_rows[i].off;
    return enh_rows[i].it.kind == K_CFG_I ? (float)*(const int *)p : *(const float *)p;
}
static void enh_cfg_put(int i, float v) {
    char *p = (char *)&ps2_cfg + enh_rows[i].off;
    if (enh_rows[i].it.kind == K_CFG_I) *(int *)p = (int)lrintf(v);
    else *(float *)p = v;
}
static int enh_cfg_choice(int i) {                   /* the choice nearest to the setting */
    const ps2_enh_item *it = &enh_rows[i].it;
    float v = enh_cfg_get(i), bd = 1e30f;
    int best = 0;
    for (int k = 0; k < it->n; k++)
        if (fabsf(it->cfgv[k] - v) < bd) { bd = fabsf(it->cfgv[k] - v); best = k; }
    return best;
}
/* the choice a switch value stands for (case-insensitive; numbers by value: "1.0" = "1"), -1 none */
static int enh_match(int i, const char *v) {
    const ps2_enh_item *it = &enh_rows[i].it;
    for (int k = 0; k < it->n; k++)
        if (!SDL_strcasecmp(it->val[k], v)) return k;
    char *end;
    double d = strtod(v, &end);
    if (end == v || *end) return -1;
    for (int k = 0; k < it->n; k++) {
        char *e2;
        double c = strtod(it->val[k], &e2);
        if (e2 != it->val[k] && !*e2 && fabs(c - d) < 1e-6) return k;
    }
    return -1;
}
static void enh_setenv(const char *name, const char *v) {
#ifdef _WIN32
    _putenv_s(name, v ? v : "");                     /* "" removes the variable */
#else
    if (v && *v) setenv(name, v, 1); else unsetenv(name);
#endif
}

/* ---- K_COMBO's machinery (the table's comment: above enh_combo_tab) ---- */
static int enh_cx_choice_of(int m, const char *v) {     /* a member's choice for a value of enh_combo_tab, -1 none */
    const ps2_enh_item *it = &enh_rows[m].it;
    if (enh_is_cfg(m)) {
        float f = (float)atof(v);
        for (int k = 0; k < it->n; k++) if (fabsf(it->cfgv[k] - f) < 1e-4f) return k;
        return -1;
    }
    return enh_match(m, v);
}
static void enh_combo_init(void) {
    for (int i = 0; i < ENH_N; i++) enh_row_cx[i] = -1;
    for (int x = 0; x < ENH_CX_N; x++) {
        const enh_combo_def *d = &enh_combo_tab[x];
        int r = -1;
        for (int i = 0; i < ENH_N; i++) if (enh_rows[i].it.kind == K_COMBO && !strcmp(enh_rows[i].it.key, d->row)) r = i;
        enh_cx[x].row = r;
        enh_cx[x].nall = d->nall;
        if (r < 0) {
            size_t l = strlen(enh_cx_msg);
            snprintf(enh_cx_msg + l, sizeof enh_cx_msg - l, "%sno row %s", l ? "; " : "", d->row);
            continue;
        }
        enh_row_cx[r] = (signed char)x;
        for (int j = 0; j < ENH_COMBO_MEMBERS && d->m[j].key; j++) {
            int m = -1;
            for (int i = 0; i < ENH_N; i++) if (i != r && !strcmp(enh_rows[i].it.key, d->m[j].key)) m = i;
            size_t l = strlen(enh_cx_msg);
            if (m < 0) {
                snprintf(enh_cx_msg + l, sizeof enh_cx_msg - l, "%s%s: no member %s", l ? "; " : "", d->row, d->m[j].key);
                continue;
            }
            enh_cmember *cm = &enh_cx[x].m[enh_cx[x].nm++];
            cm->row = m;
            enh_member[m] = 1;
            for (int k = 0; k < d->nall; k++) {
                const char *v = d->m[j].v[k];
                int any = v[0] == '*', c = any && !v[1] ? (int)enh_rows[m].it.def : enh_cx_choice_of(m, any ? v + 1 : v);
                if (c < 0) {
                    l = strlen(enh_cx_msg);
                    snprintf(enh_cx_msg + l, sizeof enh_cx_msg - l, "%s%s: %s has no choice \"%s\"", l ? "; " : "", d->row,
                             d->m[j].key, v);
                    c = enh_rows[m].it.def;
                }
                cm->ch[k] = (signed char)(any ? -1 : c);
                cm->set[k] = (signed char)c;
            }
        }
    }
}
/* a cfg-backed member's value: saved / in effect (the switch in the environment wins); before ps2_cfg is loaded, the file's */
static float enh_cx_cfg_now(int m, int run) {
    if (run && enh_rows[m].it.env[0]) {
        const char *e = getenv(enh_rows[m].it.env);
        if (e && *e) return (float)atof(e);
    }
    if (enh_cfg_ready) return enh_cfg_get(m);
    return enh_file_has[m] ? enh_file_cfg[m] : enh_rows[m].it.cfgv[enh_rows[m].it.def];
}
/* how far the members are from a combo choice: 0 = exact; a differing switch costs 1, a cfg value by its distance */
static float enh_cx_dist(int x, int k, int run) {
    float d = 0.0f;
    for (int j = 0; j < enh_cx[x].nm; j++) {
        const enh_cmember *cm = &enh_cx[x].m[j];
        int want = cm->ch[k], m = cm->row;
        if (want < 0) continue;
        if (enh_is_cfg(m)) {
            const ps2_enh_item *it = &enh_rows[m].it;
            float lo = it->cfgv[0], hi = lo, have = enh_cx_cfg_now(m, run), dv = fabsf(have - it->cfgv[want]);
            for (int q = 1; q < it->n; q++) { if (it->cfgv[q] < lo) lo = it->cfgv[q]; if (it->cfgv[q] > hi) hi = it->cfgv[q]; }
            if (dv >= 1e-3f) d += 0.01f + dv / (2.0f * (hi - lo > 1e-3f ? hi - lo : 1.0f));
        } else if ((run ? enh_run[m] : enh_saved[m]) != want) d += 1.0f;
    }
    return d;
}
static int enh_cx_nearest(int x, int run, float *dist) {  /* the first of the nearest choices */
    int best = 0;
    float bd = 1e30f;
    for (int k = 0; k < enh_cx[x].nall; k++) {
        float d = enh_cx_dist(x, k, run);
        if (d < bd) { bd = d; best = k; }
    }
    if (dist) *dist = bd;
    return best;
}
/* the combo rows' saved and running choices, from their members (running: -1 when no choice is exact) */
static void enh_combo_sync(void) {
    for (int x = 0; x < ENH_CX_N; x++) {
        int r = enh_cx[x].row;
        float d;
        if (r < 0) continue;
        enh_saved[r] = (signed char)enh_cx_nearest(x, 0, NULL);
        int k = enh_cx_nearest(x, 1, &d);
        enh_run[r] = (signed char)(d == 0.0f ? k : -1);
    }
}
int ps2_enh_members(int i, int *rows, int max) {
    int x = i >= 0 && i < ENH_N ? enh_row_cx[i] : -1, n = 0;
    for (int j = 0; x >= 0 && j < enh_cx[x].nm && n < max; j++) rows[n++] = enh_cx[x].m[j].row;
    return n;
}
/* a combo row's value in the file, -1 none */
static int enh_cx_token(int x, const char *val) {
    int r = enh_cx[x].row;
    for (int k = 0; r >= 0 && k < enh_cx[x].nall; k++) if (!SDL_strcasecmp(enh_rows[r].it.val[k], val)) return k;
    return -1;
}
/* after the file was read, at start: a combo row's key wins and sets its members; else the migration: the members keep the
   old keys' values for this run (nothing changes), the row shows the choice they match or the nearest one, and the next
   save writes that choice instead of the old keys */
static void enh_combo_resolve(void) {
    for (int x = 0; x < ENH_CX_N; x++) {
        int r = enh_cx[x].row, old = 0;
        if (r < 0) continue;
        if (enh_cx[x].file_key) {
            int k = enh_saved[r];
            for (int j = 0; j < enh_cx[x].nm; j++)
                if (!enh_is_cfg(enh_cx[x].m[j].row)) enh_saved[enh_cx[x].m[j].row] = enh_cx[x].m[j].set[k];
            continue;
        }
        for (int j = 0; j < enh_cx[x].nm; j++) old |= enh_file_has[enh_cx[x].m[j].row];
        if (old) {
            float d;
            enh_saved[r] = (signed char)enh_cx_nearest(x, 0, &d);
            enh_cx[x].migrated = 1;
            enh_cx[x].exact = d == 0.0f;
        }
    }
}
/* settings_load_file: a combo row's key sets its cfg-backed members over whatever the file's old keys said */
static int enh_cx_file_k[ENH_CX_N];
static void enh_cx_file_reset(void) { for (int x = 0; x < ENH_CX_N; x++) enh_cx_file_k[x] = -1; }
static void enh_cx_file_key(const char *key, const char *val) {
    for (int x = 0; x < ENH_CX_N; x++)
        if (enh_cx[x].row >= 0 && !strcmp(enh_rows[enh_cx[x].row].it.key, key)) {
            enh_cx_file_k[x] = enh_cx_token(x, val);
            if (enh_cx_file_k[x] < 0 && !SDL_strcasecmp(val, "default")) enh_cx_file_k[x] = enh_rows[enh_cx[x].row].it.def;
        }
}
static void enh_cx_file_apply(void) {
    for (int x = 0; x < ENH_CX_N; x++) {
        int k = enh_cx_file_k[x];
        if (k < 0) continue;
        for (int j = 0; j < enh_cx[x].nm; j++) {
            int m = enh_cx[x].m[j].row;
            if (enh_is_cfg(m)) enh_cfg_put(m, enh_rows[m].it.cfgv[enh_cx[x].m[j].set[k]]);
        }
    }
}
static int enh_cfg_folded(size_t off) {                 /* a field of ps2_cfg whose file key a combo row replaced */
    for (int i = 0; i < ENH_N; i++) if (enh_member[i] && enh_is_cfg(i) && enh_rows[i].off == off) return 1;
    return 0;
}

/* ---- QUALITY PRESET (perf6_presets, 2026-11-03; RELEASE_RESEARCH_2026-11-03 section 12, brief R7) ----------------
   ENHANCEMENTS > QUALITY's first row. LOW / MEDIUM / HIGH write the rows of enh_preset_tab (saved at once, as RESET ALL
   does; nearly all are RESTART rows, so the page's RESTART-pending flow follows). The row shows the preset whose values
   the saved rows all have, else CUSTOM: derived every time (the file's quality_preset line is only a record), so a row
   changed by hand turns it CUSTOM and nothing is ever rewritten by itself. HIGH = every row at its default (it->def),
   i.e. today's picture, by construction. A row held by the environment keeps its switch's value in that run (env >
   saved [enhancements] > built-in); the preset still saves it for the next start. A new install (no [enhancements] section yet, started by a launcher: ACZ_MENU_SETTINGS=1) starts with the
   preset of its GPU's class (enh_gpu_class; ACZ_GPU_DETECT=0 off), saved once; an existing file is never touched.
   ACZ_QUALITY_PRESET=low|medium|high holds the preset's rows for one run without touching the file (tests, launcher
   words); a row's own switch still wins over it. The table: v[] = the LOW and MEDIUM values (NULL = the row's default,
   HIGH's); a key without a row (a switch another package delivers, ENH_HAVE_*) is skipped; `unless`: the entry is left out
   when a row with that key exists (COCKPIT DETAIL bundles the cockpit's MSAA rule). Why these rows and these
   values (measured: NOTES_ZERO_VR.md perf6_presets, docs/ENV_REFERENCE.md ACZ_QUALITY_PRESET): RESOLUTION is the lever
   that pays -- it scales every pixel pass, ours included (near clouds, glass, cockpit samples, edge AA, materials, water),
   so those rows keep their looks and get cheaper with it; the other rows are costs that do not shrink with the
   resolution: forests out to the horizon, the cloud far map and march (CLOUD DETAIL), the cockpits' triangles (COCKPIT
   DETAIL), plus the governor as the safety net. SMOOTH MOTION is never set (layered in-between frames are cheap). */
typedef struct { const char *key, *v[2], *unless; } enh_preset_def;   /* v: LOW, MEDIUM */
static const enh_preset_def enh_preset_tab[] = {
    { "resolution",      { "2", "3" }, NULL },        /* PS2_UPSCALE */
    { "forest_distance", { "", NULL }, NULL },        /* LOW: the game's own forest distance (FAR draws 3-6x the trees) */
    { "governor",        { "", "" }, NULL },          /* LOW / MEDIUM: the frame-time governor on (HIGH off: the owner's rule) */
    { "cockpit_aa",      { "2", "2" }, "cockpit_detail" },   /* until COCKPIT DETAIL exists: it carries the MSAA rule */
    { "cloud_detail",    { "low", "medium" }, NULL }, /* perf6_clouds: ACZ_VR_CLOUD_DETAIL */
    { "canopy_quality",  { "low", "medium" }, NULL }, /* perf6_glassladder: ACZ_VR_CANOPY_QUALITY */
    { "cockpit_detail",  { "low", "medium" }, NULL }, /* perf6_cockpitladder: ACZ_VR_COCKPIT_DETAIL */
};
#define ENH_PT_N ((int)(sizeof enh_preset_tab / sizeof enh_preset_tab[0]))
static const char *const enh_qp_names[PS2_QP_N] = { "LOW", "MEDIUM", "HIGH" };
static int enh_pt_row[ENH_PT_N];                /* each entry's row, -1 none */
static signed char enh_pt_ch[ENH_PT_N][PS2_QP_N];   /* its choice per preset */
static int enh_qp_row = -1;                     /* the QUALITY PRESET row */
static unsigned char enh_pheld[ENH_N];          /* held by ACZ_QUALITY_PRESET in this run ... */
static signed char enh_qpv[ENH_N];              /* ... at this choice */
static int enh_qp_env = -1;                     /* ACZ_QUALITY_PRESET's preset, -1 none */
static char enh_pt_msg[400];                    /* the start-up log: what the table could not use */
static void enh_preset_init(void) {
    for (int i = 0; i < ENH_N; i++) if (enh_rows[i].it.kind == K_PRESET) enh_qp_row = i;
    for (int t = 0; t < ENH_PT_N; t++) {
        int j = -1, skip = 0;
        for (int i = 0; i < ENH_N; i++) {
            if (enh_rows[i].it.kind == K_ENV && !strcmp(enh_rows[i].it.key, enh_preset_tab[t].key)) j = i;
            if (enh_preset_tab[t].unless && !strcmp(enh_rows[i].it.key, enh_preset_tab[t].unless)) skip = 1;
        }
        if (skip) j = -1;
        enh_pt_row[t] = j;
        for (int p = 0; j >= 0 && p < PS2_QP_N; p++) {
            const char *v = p < PS2_QP_HIGH ? enh_preset_tab[t].v[p] : NULL;
            int k = v ? enh_match(j, v) : enh_rows[j].it.def;
            if (k < 0) {
                size_t l = strlen(enh_pt_msg);
                snprintf(enh_pt_msg + l, sizeof enh_pt_msg - l, "%s%s has no choice \"%s\"", l ? "; " : "",
                         enh_preset_tab[t].key, v);
                enh_pt_row[t] = -1;
                break;
            }
            enh_pt_ch[t][p] = (signed char)k;
        }
    }
}
/* the preset whose values these choices all have, -1 none (CUSTOM) */
static int enh_preset_of(const signed char *ch) {
    for (int p = PS2_QP_N - 1; p >= 0; p--) {
        int ok = 1;
        for (int t = 0; t < ENH_PT_N && ok; t++)
            if (enh_pt_row[t] >= 0 && ch[enh_pt_row[t]] != enh_pt_ch[t][p]) ok = 0;
        if (ok) return p;
    }
    return -1;
}
static void enh_preset_sync(void) {             /* the preset row's saved and running values, from the rows */
    if (enh_qp_row < 0) return;
    enh_saved[enh_qp_row] = (signed char)enh_preset_of(enh_saved);
    enh_run[enh_qp_row] = (signed char)enh_preset_of(enh_run);
    snprintf(enh_raw[enh_qp_row], sizeof enh_raw[0], "custom");     /* (ps2_enh_value_name: CUSTOM) */
}
static void enh_preset_rows(char *buf, size_t n) {   /* "RESOLUTION 2X / 3X / 4X, FOREST DISTANCE ..." */
    size_t l = 0;
    buf[0] = 0;
    for (int t = 0; t < ENH_PT_N; t++) {
        int j = enh_pt_row[t];
        if (j < 0 || l + 1 >= n) continue;
        const ps2_enh_item *it = &enh_rows[j].it;
        l += (size_t)snprintf(buf + l, n - l, "%s%s %s / %s / %s", l ? ", " : "", it->label, it->name[enh_pt_ch[t][0]],
                              it->name[enh_pt_ch[t][1]], it->name[enh_pt_ch[t][2]]);
        if (l >= n) l = n - 1;
    }
}

/* ---- GPU classes (perf6_presets): the device -> the preset a new install starts with, and the page's suggestion ----
   From the Vulkan device name (and its type and largest device-local heap): NVIDIA RTX by generation and tier, AMD RX by
   series, Intel Arc, laptops one step down, 6 GB of VRAM or less LOW, an unknown discrete GPU MEDIUM with >= 8 GB.
   Measured on the RTX 3090 Ti only; the other cards are placed by their throughput ratio r to it (spec-based, rs_perf6
   11 / 12; no other card was measured): r >= ~0.8 HIGH (3080 / 4070 / 5070 and up, RX 6800 / 7800 XT / 9070 and up),
   ~0.42-0.7 MEDIUM (2070, 2080, 3060, 3070, 4060, 5060 with their Ti / Super, RX 6600-6700, 7600-7700, 9060), below that
   LOW (2060, 3050, GTX, RX 6500 / 7500 and below, RX 5000, Arc, integrated). 3x at 72-90 Hz fits the 3060 / 4060 class by
   the model at either headset factor and 2x blurs the cockpit's legends, so LOW is for the cards below it. The table
   numbers: NOTES_ZERO_VR.md perf6_presets. */
static char enh_gpu_name[160], enh_gpu_why[96], enh_auto_msg[400];
static double enh_gpu_vram;                     /* GB, the largest device-local heap (0: unknown) */
static int enh_gpu_cls = -1;                    /* the class of the GPU in use (ps2_enh_set_gpu) or probed */
static int enh_auto_preset = -1;                /* a new install's preset from its GPU: saved by ps2_settings_load */
static int enh_file_section;                    /* the file has an [enhancements] section */
static const char *enh_num4(const char *s, const char *tag) {   /* the 4 digits after "tag" (and spaces), else NULL */
    for (const char *p = strstr(s, tag); p; p = strstr(p + 1, tag)) {
        const char *q = p + strlen(tag);
        while (*q == ' ') q++;
        if (isdigit((unsigned char)q[0]) && isdigit((unsigned char)q[1]) && isdigit((unsigned char)q[2])
            && isdigit((unsigned char)q[3]) && !isdigit((unsigned char)q[4]))
            return q;
    }
    return NULL;
}
static int enh_gpu_class(const char *name, int discrete, double vram, char *why, size_t wn) {
    char s[160];
    size_t k;
    for (k = 0; name[k] && k + 1 < sizeof s; k++) s[k] = (char)tolower((unsigned char)name[k]);
    s[k] = 0;
    int cls = -1, lap = strstr(s, "laptop") || strstr(s, "mobile") || strstr(s, "max-q");
    const char *p, *what = "unknown model";
    if (!discrete) { cls = PS2_QP_LOW; what = "integrated or software GPU"; }
    else if ((p = enh_num4(s, "rtx")) != NULL) {
        int gen = p[0] - '0', tier = (p[2] - '0') * 10 + (p[3] - '0');   /* 3090: 3 / 90 */
        if (tier >= 90) { cls = PS2_QP_HIGH; what = "RTX x090"; }
        else if (tier >= 80) { cls = gen >= 3 ? PS2_QP_HIGH : PS2_QP_MEDIUM; what = gen >= 3 ? "RTX x080" : "RTX 2080"; }
        else if (tier >= 70) { cls = gen >= 4 ? PS2_QP_HIGH : PS2_QP_MEDIUM;
                               what = gen >= 4 ? "RTX 4070 / 5070 class" : "RTX 2070 / 3070 class"; }
        else if (tier >= 60) { cls = gen >= 3 ? PS2_QP_MEDIUM : PS2_QP_LOW;
                               what = gen >= 3 ? "RTX 3060 / 4060 / 5060 class" : "RTX 2060"; }
        else { cls = PS2_QP_LOW; what = "RTX x050"; }
    } else if (strstr(s, "gtx") || strstr(s, "quadro")) { cls = PS2_QP_LOW; what = "GTX"; }
    else if ((p = enh_num4(s, "rx")) != NULL) {
        int ser = p[0] - '0', tier = ser == 9 ? p[2] - '0' : p[1] - '0';   /* 6800: 6 / 8; 9070: 9 / 7 */
        if (ser == 9) { cls = tier >= 7 ? PS2_QP_HIGH : PS2_QP_MEDIUM; what = tier >= 7 ? "RX 9070" : "RX 9060"; }
        else if (ser >= 6) { cls = tier >= 8 ? PS2_QP_HIGH : tier >= 6 ? PS2_QP_MEDIUM : PS2_QP_LOW;
                             what = tier >= 8 ? "RX 6800 / 7800 and up" : tier >= 6 ? "RX 6600-6700 / 7600-7700" : "RX 6500 / 7500 and below"; }
        else { cls = PS2_QP_LOW; what = "RX 5000 and older"; }
    } else if (strstr(s, "radeon") || strstr(s, "arc") || strstr(s, "intel")) { cls = PS2_QP_LOW; what = "Radeon / Intel"; }
    else if (vram >= 7.5) { cls = PS2_QP_MEDIUM; what = "unknown model, 8 GB or more"; }
    else { cls = PS2_QP_LOW; what = "unknown model"; }
    int c0 = cls;
    if (lap && cls > PS2_QP_LOW) cls--;
    if (vram > 0.0 && vram < 7.0) cls = PS2_QP_LOW;
    snprintf(why, wn, "%s%s%s", what, lap && c0 > PS2_QP_LOW ? ", laptop: one step down" : "",
             vram > 0.0 && vram < 7.0 && c0 > PS2_QP_LOW ? ", 6 GB or less" : "");
    return cls;
}
/* the GPU a new install will most likely use: the discrete device with the largest device-local heap (as the renderer
   prefers a discrete GPU; the OpenXR runtime's choice is logged when it differs). ACZ_GPU_DETECT_NAME (+
   ACZ_GPU_DETECT_VRAM_MB) stands in for it (tests). Before main() (a constructor): one short Vulkan instance. */
static int enh_probe_gpu(void) {
    const char *f = getenv("ACZ_GPU_DETECT_NAME");
    if (f && *f) {
        const char *m = getenv("ACZ_GPU_DETECT_VRAM_MB");
        snprintf(enh_gpu_name, sizeof enh_gpu_name, "%s", f);
        enh_gpu_vram = m && *m ? atof(m) / 1024.0 : 0.0;
        enh_gpu_cls = enh_gpu_class(enh_gpu_name, 1, enh_gpu_vram, enh_gpu_why, sizeof enh_gpu_why);
        return 1;
    }
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    VkInstance in = VK_NULL_HANDLE;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ps2recomp gpu probe";
    app.apiVersion = VK_API_VERSION_1_1;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    if (vkCreateInstance(&ci, NULL, &in) != VK_SUCCESS) return 0;
    VkPhysicalDevice d[8];
    uint32_t n = 8;
    int best = -1, bdisc = 0;
    double bv = -1.0;
    VkResult r = vkEnumeratePhysicalDevices(in, &n, d);
    for (uint32_t i = 0; (r == VK_SUCCESS || r == VK_INCOMPLETE) && i < n; i++) {
        VkPhysicalDeviceProperties pr;
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceProperties(d[i], &pr);
        vkGetPhysicalDeviceMemoryProperties(d[i], &mp);
        double v = 0.0;
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && (double)mp.memoryHeaps[h].size > v)
                v = (double)mp.memoryHeaps[h].size;
        int disc = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (best < 0 || disc > bdisc || (disc == bdisc && v > bv)) {
            best = (int)i; bdisc = disc; bv = v;
            snprintf(enh_gpu_name, sizeof enh_gpu_name, "%s", pr.deviceName);
        }
    }
    vkDestroyInstance(in, NULL);
    if (best < 0) return 0;
    enh_gpu_vram = bv / 1073741824.0;
    enh_gpu_cls = enh_gpu_class(enh_gpu_name, bdisc, enh_gpu_vram, enh_gpu_why, sizeof enh_gpu_why);
    return 1;
}
/* a new install: the rows of its GPU class's preset as the saved choices (a row set in the environment is saved too;
   its switch still wins in this run) */
static void enh_autodetect(void) {
    const char *e = getenv("ACZ_GPU_DETECT");
    if (e && *e == '0') {
        snprintf(enh_auto_msg, sizeof enh_auto_msg, "a new install: GPU detection off (ACZ_GPU_DETECT=0), every row at "
                 "its default (HIGH)");
        return;
    }
    if (!enh_probe_gpu()) {
        snprintf(enh_auto_msg, sizeof enh_auto_msg, "a new install: no Vulkan device found before start, every row at "
                 "its default (HIGH)");
        return;
    }
    int p = enh_gpu_cls;
    for (int t = 0; t < ENH_PT_N; t++) {
        int j = enh_pt_row[t];
        if (j >= 0) enh_saved[j] = enh_pt_ch[t][p];   /* (a row set in the environment: saved, the switch wins now) */
    }
    enh_auto_preset = p;
    snprintf(enh_auto_msg, sizeof enh_auto_msg, "a new install (no [enhancements] yet): GPU %s, %.1f GB -> %s (%s); "
             "saved, the page changes it", enh_gpu_name, enh_gpu_vram, enh_qp_names[p], enh_gpu_why);
}

int ps2_enh_choice(int i) {
    if (i < 0 || i >= ENH_N) return 0;
    if (enh_rows[i].live == L_PAD_ACTIVE && enh_saved[i] < 0) return enh_match(i, enh_raw[i]);   /* a pad: if there */
    return enh_is_cfg(i) ? enh_cfg_choice(i) : enh_saved[i];
}
/* ACTIVE CONTROLLER: a pad's name as the page shows it (the game's font: upper case, letters, digits; without the words
   every pad has: CONTROLLER, WIRELESS, GAMEPAD), at most max characters (22 fit the page's value column) */
static void enh_pad_label(const char *name, char *out, size_t n, size_t max) {
    size_t k = 0, cut;
    const char *p = name;
    char full[96];
    if (max + 1 < n) n = max + 1;
    while (*p && k + 1 < sizeof full) {
        while (*p == ' ' || *p == '(' || *p == ')') p++;
        const char *w = p;
        while (*p && *p != ' ' && *p != '(' && *p != ')') p++;
        size_t l = (size_t)(p - w);
        if (!l || (l == 10 && !SDL_strncasecmp(w, "controller", l)) || (l == 8 && !SDL_strncasecmp(w, "wireless", l))
            || (l == 7 && !SDL_strncasecmp(w, "gamepad", l)))
            continue;
        if (k && k + 1 < sizeof full) full[k++] = ' ';
        for (size_t j = 0; j < l && k + 1 < sizeof full; j++) {
            char c = (char)SDL_toupper((unsigned char)w[j]);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '/') full[k++] = c;
        }
    }
    while (k && full[k - 1] == ' ') k--;
    full[k] = 0;
    cut = k;
    if (cut + 1 > n) {                               /* too long: whole words only, where there is a space */
        cut = n - 1;
        while (cut && full[cut] != ' ') cut--;
        if (!cut) cut = n - 1;
    }
    memcpy(out, full, cut);
    out[cut] = 0;
    if (!cut) snprintf(out, n, "CONTROLLER");
}
/* THROTTLE LEVER and SET LEVER NEUTRAL (ps2_hotas.c, at every change of the connected joysticks): the rows of CONTROLS
   only with a joystick connected, so nothing changes on the page for a pad player. (Their values stay in the file.) */
void ps2_enh_hotas_rows(int on) {
    for (int i = 0; i < ENH_N; i++)
        if (enh_rows[i].live == L_HOTAS || enh_rows[i].it.kind == K_ACT_NEUTRAL)
            enh_rows[i].it.cat = (unsigned char)(on ? PS2_ENH_CONTROLS : PS2_ENH_CAT_OFF);
}
/* The Touch rows of CONTROLS (and PILOT HANDS, SHOW TOUCH CONTROLS) only once a Touch controller has been seen: a pose valid
   at least once this run (ps2_touch.c), or ACZ_TOUCH set (a launcher word, a test script, the saved TOUCH CONTROLLERS: OFF
   must stay reachable). Their values stay in the file while hidden. */
void ps2_enh_touch_rows(int on) {
    for (int i = 0; i < ENH_N; i++)
        if (enh_rows[i].live == L_TOUCH || enh_rows[i].it.kind == K_ACT_CARD)
            enh_rows[i].it.cat = (unsigned char)(on ? PS2_ENH_CONTROLS : PS2_ENH_CAT_OFF);
}
static char enh_pad_val[PS2_ENH_CHOICES][64], enh_pad_name[PS2_ENH_CHOICES][24];
/* ps2_video.c, renderer thread, at every change of the connected pads: AUTO, TOUCH, then up to four pads by name */
void ps2_enh_pad_names(int n, const char *const *names) {
    for (int i = 0; i < ENH_N; i++) {
        if (enh_rows[i].live != L_PAD_ACTIVE) continue;
        ps2_enh_item *it = &enh_rows[i].it;
        int k = 2;
        for (int j = 0; j < n && k < PS2_ENH_CHOICES; j++) {
            int dup = 0;
            for (int q = 2; q < k; q++) dup |= !strcmp(enh_pad_val[q], names[j]);
            if (dup) continue;                       /* (two pads of one kind: one choice, the first of them) */
            snprintf(enh_pad_val[k], sizeof enh_pad_val[k], "%s", names[j]);
            enh_pad_label(names[j], enh_pad_name[k], sizeof enh_pad_name[k], 22);
            it->val[k] = enh_pad_val[k];
            it->name[k] = enh_pad_name[k];
            k++;
        }
        it->n = (unsigned char)k;
    }
}
const char *ps2_enh_value_name(int i, char *buf, int n) {
    const ps2_enh_item *it = ps2_enh_item_at(i);
    int c = it ? ps2_enh_choice(i) : 0, k = 0;
    const char *raw = it && c < 0 ? enh_raw[i] : NULL;
    if (it && it->kind == K_COMBO) {                 /* (held by the environment: in effect, CUSTOM when no choice is it) */
        int r = ps2_enh_override(i) ? enh_run[i] : enh_saved[i];
        snprintf(buf, (size_t)n, "%s", r >= 0 ? it->name[r] : "CUSTOM");
        return buf;
    }
    if (it && (it->kind == K_ENV || it->kind == K_PRESET) && enh_ovr[i]) {   /* held by the environment: in effect */
        c = enh_match(i, enh_ovr[i]);
        raw = c < 0 ? enh_ovr[i] : NULL;
    }
    if (!it) snprintf(buf, (size_t)n, "?");
    else if (!raw && enh_rows[i].live == L_PAD_ACTIVE && c == 0) {   /* AUTO: which one that is now */
        extern const char *ps2_video_pad_active_name(void);          /* ps2_video.c (NULL: none) */
        const char *a = ps2_video_pad_active_name();
        char l[24];
        if (a) enh_pad_label(a, l, sizeof l, 16);                  /* ("AUTO: " + 16 = the column's 22) */
        snprintf(buf, (size_t)n, "AUTO: %s", !a ? "NONE" : !strcmp(a, "Quest Touch") ? "TOUCH" : l);
    } else if (!raw) snprintf(buf, (size_t)n, "%s", it->name[c]);
    else if (enh_rows[i].live == L_PAD_ACTIVE) enh_pad_label(raw, buf, (size_t)n, 22);   /* a pad not connected */
    else {                                           /* a value that is not one of the choices, as it is */
        for (const char *s = raw; *s && k + 1 < n; s++) buf[k++] = (char)SDL_toupper((unsigned char)*s);
        buf[k] = 0;
    }
    return buf;
}
int ps2_enh_running(int i) {
    if (i < 0 || i >= ENH_N) return -1;
    return enh_is_cfg(i) ? enh_cfg_choice(i) : enh_run[i];
}
const char *ps2_enh_override(int i) {
    if (i < 0 || i >= ENH_N) return NULL;
    if (enh_rows[i].it.kind == K_COMBO) {            /* any of its switches set in the environment holds it */
        int x = enh_row_cx[i];
        for (int j = 0; x >= 0 && j < enh_cx[x].nm; j++) {
            const char *o = ps2_enh_override(enh_cx[x].m[j].row);
            if (o) return o;
        }
        return NULL;
    }
    if (enh_rows[i].live == L_PAD) return layout_env >= 0 ? getenv("ACZ_PAD_LAYOUT") : NULL;
    if (enh_is_cfg(i)) {
        const char *e = enh_rows[i].it.env[0] ? getenv(enh_rows[i].it.env) : NULL;
        return e && *e ? e : NULL;
    }
    return enh_ovr[i];
}
int ps2_enh_pending(int i) {
    if (i >= 0 && i < ENH_N && enh_rows[i].it.kind == K_COMBO) {   /* a member's switch differs from what the choice sets */
        int x = enh_row_cx[i], k = enh_saved[i];
        if (x < 0 || ps2_enh_override(i)) return 0;
        for (int j = 0; j < enh_cx[x].nm; j++) {
            const enh_cmember *cm = &enh_cx[x].m[j];
            if (cm->ch[k] < 0) continue;
            if (enh_is_cfg(cm->row) ? fabsf(enh_cx_cfg_now(cm->row, 0) - enh_rows[cm->row].it.cfgv[cm->ch[k]]) >= 1e-3f
                                    : !enh_mission[cm->row] && enh_run[cm->row] != cm->ch[k]) return 1;
        }
        return 0;
    }
    if (i < 0 || i >= ENH_N || enh_member[i] || enh_is_cfg(i) || enh_rows[i].it.apply == PS2_ENH_ACTION || ps2_enh_override(i)
        || enh_rows[i].it.kind == K_PRESET || enh_rows[i].live == L_PAD_ACTIVE)
        return 0;
    if (enh_mission[i]) return 0;                    /* (it waits for the next mission, not for a restart) */
    return enh_saved[i] < 0 || enh_saved[i] != enh_run[i];
}
int ps2_enh_pending_count(void) {
    int n = 0;
    for (int i = 0; i < ENH_N; i++) n += ps2_enh_pending(i);
    return n;
}
int ps2_enh_changed_mission(int i) {
    if (i >= 0 && i < ENH_N && enh_rows[i].it.kind == K_COMBO) {
        int x = enh_row_cx[i];
        for (int j = 0; x >= 0 && j < enh_cx[x].nm; j++) if (enh_mission[enh_cx[x].m[j].row]) return 1;
        return 0;
    }
    return i >= 0 && i < ENH_N && enh_mission[i];
}
int ps2_enh_mission_count(void) {
    int n = 0;
    for (int i = 0; i < ENH_N; i++) n += !enh_member[i] && enh_rows[i].it.cat != PS2_ENH_CAT_OFF && ps2_enh_changed_mission(i) && !ps2_enh_pending(i);
    return n;
}
/* the heading (at most 30 letters: the native page adds " 1-7 OF 15", and ZN_GLYPHS is 40) */
const char *ps2_enh_cat_head(int c) {
    static char head[PS2_ENH_CATS][32];
    if (c == PS2_ENH_QUALITY) return "THESE APPLY AT THE NEXT START";
    if (c < 0 || c >= PS2_ENH_CATS) return "";
    int nm = 0, nr = 0;
    for (int i = 0; i < ENH_N; i++) {
        if (enh_rows[i].it.cat != c) continue;
        if (ps2_enh_pending(i)) nr++;
        else if (ps2_enh_changed_mission(i)) nm++;
    }
    if (nm && nr) snprintf(head[c], sizeof head[c], "%d NEXT MISSION, %d NEXT START", nm, nr);
    else if (nm) snprintf(head[c], sizeof head[c], "%d CHANGE%s AT THE NEXT MISSION", nm, nm == 1 ? "" : "S");
    else if (nr) snprintf(head[c], sizeof head[c], "%d CHANGE%s AT THE NEXT START", nr, nr == 1 ? "" : "S");
    else return "";
    return head[c];
}
/* "VAR=value" of what holds an item in this run (ACZ_QUALITY_PRESET=low for the preset's rows under it), NULL none */
const char *ps2_enh_held_by(int i, char *buf, int n) {
    const char *o = ps2_enh_override(i);
    if (!o) return NULL;
    if (enh_rows[i].it.kind == K_COMBO) {            /* the switch that holds it */
        int x = enh_row_cx[i];
        for (int j = 0; x >= 0 && j < enh_cx[x].nm; j++)
            if (ps2_enh_override(enh_cx[x].m[j].row)) return ps2_enh_held_by(enh_cx[x].m[j].row, buf, n);
        return NULL;
    }
    if (enh_pheld[i]) snprintf(buf, (size_t)n, "ACZ_QUALITY_PRESET=%s", enh_qp_env >= 0 ? enh_rows[enh_qp_row].it.val[enh_qp_env] : "?");
    else snprintf(buf, (size_t)n, "%s=%s", enh_rows[i].it.env, o);
    return buf;
}

/* [enhancements]: key = switch value ("default" = leave the switch unset) */
static void enh_read_file(void) {
    char line[512], section[64] = "";
    FILE *f = fopen(ps2_settings_path(), "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *c = strpbrk(line, ";#"), *p, *eq, *key, *val;
        if (c) *c = 0;
        p = trim(line);
        if (!*p) continue;
        if (*p == '[') {
            char *b = strchr(p, ']');
            if (b) { *b = 0; snprintf(section, sizeof section, "%s", p + 1); }
            continue;
        }
        if (!strcmp(section, "enhancements")) enh_file_section = 1;
        if (!strcmp(section, "graphics") || !strcmp(section, "immersion")) {   /* a cfg-backed member's old key */
            if (!(eq = strchr(p, '='))) continue;
            *eq = 0;
            key = trim(p);
            val = trim(eq + 1);
            for (int i = 0; i < ENH_N; i++)
                if (enh_member[i] && enh_is_cfg(i) && !strcmp(enh_rows[i].it.key, key)) {
                    enh_file_has[i] = 1;
                    enh_file_cfg[i] = strtof(val, NULL);
                }
            continue;
        }
        if (strcmp(section, "enhancements") || !(eq = strchr(p, '='))) continue;
        *eq = 0;
        key = trim(p);
        val = trim(eq + 1);
        if (!strcmp(key, "launcher")) { ps2_enh_launcher_show_set(atoi(val) != 0); enh_lch_dirty = 0; continue; }
        if (!strcmp(key, "launcher_changed")) { snprintf(enh_lch_changed, sizeof enh_lch_changed, "%s", val); continue; }
        for (int i = 0; i < ENH_N; i++) {
            if (enh_is_cfg(i) || enh_rows[i].it.apply == PS2_ENH_ACTION || strcmp(enh_rows[i].it.key, key)) continue;
            if (enh_rows[i].it.kind == K_PRESET) break;  /* QUALITY PRESET: derived from the rows (only a record) */
            enh_file_has[i] = 1;
            if (enh_rows[i].it.kind == K_COMBO) {      /* its own values; "default" = the row's default */
                int x = enh_row_cx[i], k = x >= 0 ? enh_cx_token(x, val) : -1;
                if (k < 0 && !SDL_strcasecmp(val, "default")) k = enh_rows[i].it.def;
                if (k >= 0) { enh_saved[i] = (signed char)k; enh_cx[x].file_key = 1; }
                break;
            }
            if (!SDL_strcasecmp(val, "default")) {     /* the built-in default: the "" choice, if the item has one */
                int k = enh_match(i, "");
                if (k >= 0) enh_saved[i] = (signed char)k;
                break;
            }
            int k = enh_match(i, val);
            if (k >= 0) enh_saved[i] = (signed char)k;
            else if (enh_rows[i].it.kind == K_ENV && *val && strlen(val) < sizeof enh_raw[i]) {
                enh_saved[i] = -1;                     /* a hand-written value is kept and used as it is */
                snprintf(enh_raw[i], sizeof enh_raw[i], "%s", val);
            }
            break;
        }
    }
    fclose(f);
}

static void enh_cpit_save(void) {
    const char *m = getenv("ACZ_VR_COCKPIT_MODEL"), *ms = getenv("ACZ_VR_COCKPIT_MODELS");
    enh_cpit_keep[0] = m ? SDL_strdup(m) : NULL;
    enh_cpit_keep[1] = ms ? SDL_strdup(ms) : NULL;
}
static void enh_list_add(const char *a, const char *b, const char *c) {      /* the start-up log's list */
    const char *parts[4] = { enh_injected_list[0] ? " " : "", a, b, c };
    size_t l = strlen(enh_injected_list);
    for (int k = 0; k < 4; k++) {
        size_t n = strlen(parts[k]);
        if (l + n + 1 >= sizeof enh_injected_list) return;
        memcpy(enh_injected_list + l, parts[k], n + 1);
        l += n;
    }
}

/* Before main(): the saved values become the switches' values (ACZ_MENU_SETTINGS=1), so every module reads them as if
   the launcher had exported them. Logged later by ps2_settings_load (the log is not open yet). */
static void enh_boot_init(void);
__attribute__((constructor)) static void enh_preload(void) {
    const char *m = getenv("ACZ_MENU_SETTINGS");
    {   const char *u = getenv("ACZ_DEBUG_UNHOLD"); if (u && *u) enh_setenv(u, ""); }   /* (test: run_test.sh sets ACZ_VR_START_VIEW) */
    enh_marker = m && *m == '1';
    for (int i = 0; i < ENH_N; i++) enh_saved[i] = (signed char)enh_rows[i].it.def;
    enh_combo_init();
    enh_read_file();              /* always: a save in any run (F4, a test run) keeps the saved choices */
    enh_combo_resolve();          /* a combo row's key sets its members; else the migration from the old keys */
    enh_preset_init();
    /* QUALITY PRESET: ACZ_QUALITY_PRESET holds its rows for this run (the file unchanged; a row's own switch wins);
       else a new install (no [enhancements] yet, a launcher's start) takes its GPU class's preset, saved once */
    const char *qe = getenv("ACZ_QUALITY_PRESET");
    if (qe && *qe && enh_qp_row >= 0) {
        enh_qp_env = enh_match(enh_qp_row, qe);
        enh_ovr[enh_qp_row] = SDL_strdup(qe);
        if (enh_qp_env < 0)
            snprintf(enh_auto_msg, sizeof enh_auto_msg, "ACZ_QUALITY_PRESET=%s not understood (low, medium, high)", qe);
        for (int t = 0; t < ENH_PT_N && enh_qp_env >= 0; t++) {
            int j = enh_pt_row[t];
            if (j >= 0 && !getenv(enh_rows[j].it.env)) { enh_pheld[j] = 1; enh_qpv[j] = enh_pt_ch[t][enh_qp_env]; }
        }
    } else if (enh_marker && !enh_file_section) enh_autodetect();
    for (int i = 0; i < ENH_N; i++) {
        const ps2_enh_item *it = &enh_rows[i].it;
        enh_run[i] = it->code_def;
        if (it->kind == K_ENV && enh_pheld[i]) {       /* ACZ_QUALITY_PRESET: the preset's value in this run */
            const char *v = it->val[enh_qpv[i]];
            if (*v) enh_setenv(it->env, v);
            enh_ovr[i] = SDL_strdup(v);
            enh_run[i] = enh_qpv[i];
            enh_injected++;
            enh_list_add(it->env, "=", *v ? v : "(default)");
        } else if (it->kind == K_ENV) {
            const char *e = getenv(it->env);
            if (e) {                                   /* set by hand or by a launcher word: it wins */
                enh_ovr[i] = SDL_strdup(e);
                enh_run[i] = (signed char)enh_match(i, e);
                enh_kept++;
            } else if (enh_marker) {
                const char *v = enh_saved[i] >= 0 ? it->val[enh_saved[i]] : enh_raw[i];
                if (*v) {
                    enh_setenv(it->env, v);
                    enh_injected++;
                    enh_list_add(it->env, "=", v);
                }
                enh_run[i] = enh_saved[i];
            }
        } else if (it->kind == K_PATHS) {
            /* the 3D cockpit: the launcher's `cockpit` word supplies the model files (ACZ_VR_COCKPIT_MODEL / _MODELS),
               the setting decides whether they are used */
            const char *e = getenv("ACZ_VR_COCKPIT_MODEL");
            int have = e && *e && strcmp(e, "none") != 0;
            if (enh_marker && have && enh_saved[i] == 1) {
                enh_cpit_save();
                enh_setenv("ACZ_VR_COCKPIT_MODEL", "");
                enh_setenv("ACZ_VR_COCKPIT_MODELS", "");
                have = 0;
                enh_list_add("(3D cockpit off)", "", "");
            }
            enh_run[i] = (signed char)(have ? 0 : 1);
        }
    }
    enh_preset_sync();
    enh_combo_sync();
    enh_boot_init();
    {   const char *t = getenv("ACZ_TOUCH");
        ps2_enh_touch_rows((t && *t) || getenv("PS2_XR_TEST_TOUCH")); }
}

static void enh_log_start(void) {
    static int done;
    if (done) return;
    done = 1;
    if (enh_marker)
        ps2_log("settings: ENHANCEMENTS from %s (ACZ_MENU_SETTINGS=1): %d switches set, %d kept from the "
                "environment%s%s", ps2_settings_path(), enh_injected, enh_kept, enh_injected_list[0] ? ": " : "",
                enh_injected_list);
    else
        ps2_log("settings: ENHANCEMENTS not applied (no ACZ_MENU_SETTINGS=1: every switch keeps its built-in default "
                "unless set in the environment)");
    for (int i = 0; i < ENH_N; i++) {
        char hb[96];
        if (enh_ovr[i]) ps2_log("settings: ENHANCEMENTS: %s held by %s", enh_rows[i].it.key, ps2_enh_held_by(i, hb, sizeof hb));
    }
    if (enh_qp_row >= 0) {
        char rows[400];
        int s = enh_saved[enh_qp_row], r = enh_run[enh_qp_row];
        enh_preset_rows(rows, sizeof rows);
        ps2_log("settings: QUALITY PRESET saved %s, in effect %s%s; it sets %s", s >= 0 ? enh_qp_names[s] : "CUSTOM",
                r >= 0 ? enh_qp_names[r] : "CUSTOM", enh_qp_env >= 0 ? " (ACZ_QUALITY_PRESET)" : "", rows);
    }
    if (enh_auto_msg[0]) ps2_log("settings: QUALITY PRESET: %s", enh_auto_msg);
    if (enh_pt_msg[0]) ps2_log("settings: QUALITY PRESET table: %s (left out)", enh_pt_msg);
    if (enh_cx_msg[0]) ps2_log("settings: ENHANCEMENTS combo table: %s", enh_cx_msg);
    /* the migration (A5 of the settings review): the old keys are still in effect in this run */
    for (int x = 0; x < ENH_CX_N; x++) {
        char l[300];
        size_t n = 0;
        if (!enh_cx[x].migrated) continue;
        l[0] = 0;
        for (int j = 0; j < enh_cx[x].nm && n + 1 < sizeof l; j++) {
            int m = enh_cx[x].m[j].row;
            if (enh_is_cfg(m)) n += (size_t)snprintf(l + n, sizeof l - n, "%s%s=%g", j ? ", " : "", enh_rows[m].it.key,
                                                     (double)enh_cx_cfg_now(m, 0));
            else {
                const char *v = enh_saved[m] >= 0 ? enh_rows[m].it.val[enh_saved[m]] : enh_raw[m];
                n += (size_t)snprintf(l + n, sizeof l - n, "%s%s=%s", j ? ", " : "", enh_rows[m].it.key, *v ? v : "default");
            }
            if (n >= sizeof l) n = sizeof l - 1;
        }
        int r = enh_cx[x].row;
        ps2_log("settings: migrated %s: %s -> %s=%s (%s)", enh_rows[r].it.key, l, enh_rows[r].it.key,
                enh_rows[r].it.name[enh_saved[r]], enh_cx[x].exact ? "exact" : "nearest");
    }
}

static void enh_lch_write(FILE *f);
static void enh_write(FILE *f) {
    fprintf(f, "\n[enhancements]\n"
               "; The layers (docs/SETTINGS.md): the ENHANCEMENTS page (pause menu, F4 > Enhancements) shows the rows a player\n"
               "; tells apart while flying; its QUALITY rows apply at the next start, most others at the next mission. Lines\n"
               "; marked [ini] are expert keys:\n"
               "; on no in-game page, F4 > Enhancements > Expert (ini) lists them.\n"
               "; Each line sets the switch(es) in its comment at start-up when a launcher starts the game\n"
               "; (ACZ_MENU_SETTINGS=1); a switch already set in the environment wins. default = the switch's built-in default.\n");
    enh_lch_write(f);
    for (int i = 0; i < ENH_N; i++) {
        const ps2_enh_item *it = &enh_rows[i].it;
        char vals[200] = "";
        if (enh_member[i]) continue;                 /* (its combo row's key stands for it) */
        if (it->kind == K_COMBO) {
            int x = enh_row_cx[i];
            char sw[160] = "";
            for (int j = 0; x >= 0 && j < enh_cx[x].nm; j++) {
                size_t l = strlen(sw);
                snprintf(sw + l, sizeof sw - l, "%s%s", j ? " + " : "", enh_rows[enh_cx[x].m[j].row].it.env);
            }
            for (int k = 0; x >= 0 && k < enh_cx[x].nall; k++) {
                size_t l = strlen(vals);
                snprintf(vals + l, sizeof vals - l, "%s%s %s%s", k ? ", " : "", it->val[k], it->name[k], k >= it->n ? " [ini]" : "");
            }
            fprintf(f, "%-26s = %-10s ; %s (%s): %s%s\n", it->key, it->val[enh_saved[i]], it->label, sw, vals,
                    it->apply == PS2_ENH_RESTART ? "; restart" : it->apply == PS2_ENH_MISSION ? "; next mission" : "");
            continue;
        }
        if (it->kind == K_PRESET) {
            int s = enh_saved[i];
            fprintf(f, "%-26s = %-10s ; %s (%s): low LOW, medium MEDIUM, high HIGH, custom CUSTOM -- a record: the rows "
                       "below decide it\n", it->key, s >= 0 ? it->val[s] : "custom", it->label, it->env);
            continue;
        }
        if (it->kind != K_ENV && it->kind != K_PATHS) continue;
        for (int k = 0; k < it->n; k++) {
            size_t l = strlen(vals);
            snprintf(vals + l, sizeof vals - l, "%s%s %s", k ? ", " : "", it->val[k][0] ? it->val[k] : "default",
                     it->name[k]);
        }
        const char *v = enh_saved[i] >= 0 ? it->val[enh_saved[i]] : enh_raw[i];
        fprintf(f, "%-26s = %-10s ; %s%s (%s): %s%s\n", it->key, *v ? v : "default", it->cat == PS2_ENH_CAT_INI ? "[ini] " : "", it->label,
                it->kind == K_PATHS ? "the launcher's cockpit models" : it->env, vals,
                it->apply == PS2_ENH_RESTART ? "; restart" : it->apply == PS2_ENH_MISSION ? "; next mission" : "");
    }
}

extern int ps2_clouds_toggle(int which, int on);         /* ps2_clouds.c */
extern int ps2_vk_gshadow_toggle(int on);                 /* ps2_video.c */
extern void ps2_xr_request_recenter(void);                /* ps2_xr.c */
extern int ps2_video_pad_force(const char *v);            /* ps2_video.c: "" auto, "touch", a pad's name */
extern void ps2_video_set_rumble_gain(float g);           /* ps2_video.c: < 0 auto */
extern void ps2_video_set_triggers(float g);              /* ps2_video.c: ADAPTIVE TRIGGERS, 0 off */
extern int ps2_video_rumble_test(void);                   /* ps2_video.c: TEST VIBRATION */
extern void ps2_touch_set_haptics(float v);               /* ps2_touch.c: RUMBLE STRENGTH on Touch */
static float enh_num(int i, int c) {
    const char *v = c >= 0 ? enh_rows[i].it.val[c] : enh_raw[i];
    return *v ? (float)atof(v) : 1.0f;
}
/* a live switch item now; returns PS2_ENH_LIVE or PS2_ENH_RESTART (not possible in this run) */
static int enh_apply_live(int i) {
    const enh_row *r = &enh_rows[i];
    int c = enh_saved[i], on = c >= 0 && strcmp(r->it.val[c], "0") != 0;
    switch (r->live) {
    case L_PAD_ACTIVE: ps2_video_pad_force(c >= 0 ? r->it.val[c] : enh_raw[i]); break;
    case L_RUMBLE: ps2_video_set_rumble_gain(c >= 0 && r->it.val[c][0] ? (float)atof(r->it.val[c]) : -1.0f); break;
    case L_TRIGGERS: ps2_video_set_triggers(c >= 0 && r->it.val[c][0] ? (float)atof(r->it.val[c]) : 1.0f); break;
    case L_HOTAS: ps2_hotas_set_throttle(c >= 0 ? r->it.val[c] : enh_raw[i]); break;
    case L_HAPTICS: ps2_touch_set_haptics(c >= 0 && r->it.val[c][0] ? (float)atof(r->it.val[c]) : 0.5f); break;
    case L_CLOUD_SHADOW: if (ps2_clouds_toggle(3, on) < 0) return PS2_ENH_RESTART; break;
    case L_OPTICS: if (ps2_clouds_toggle(4, on) < 0) return PS2_ENH_RESTART; break;
    case L_GROUND_SHADOW: if (ps2_vk_gshadow_toggle(on) < 0) return PS2_ENH_RESTART; break;
    case L_LOD: case L_TDETAIL: {
        int (*fn)(float, float) = enh_detail_fn;
        int il = -1, itd = -1;
        for (int k = 0; k < ENH_N; k++) {
            if (enh_rows[k].live == L_LOD) il = k;
            if (enh_rows[k].live == L_TDETAIL) itd = k;
        }
        if (!fn || il < 0 || itd < 0) return PS2_ENH_RESTART;
        /* the other one keeps the value in effect */
        float lod = enh_num(il, i == il ? c : enh_run[il]), td = enh_num(itd, i == itd ? c : enh_run[itd]);
        if (fn(lod, td) != 0) return PS2_ENH_RESTART;
        break; }
    default: return PS2_ENH_RESTART;
    }
    enh_run[i] = (signed char)c;
    return PS2_ENH_LIVE;
}

/* the frame-time governor's terrain detail lever (ps2_settings.h) */
static int enh_detail_rows(int *il, int *itd) {
    *il = *itd = -1;
    for (int k = 0; k < ENH_N; k++) {
        if (enh_rows[k].live == L_LOD) *il = k;
        if (enh_rows[k].live == L_TDETAIL) *itd = k;
    }
    return *il >= 0 && *itd >= 0;
}
float ps2_enh_tdetail_now(void) {
    int il, itd;
    if (!enh_detail_rows(&il, &itd)) return 1.0f;
    const char *e = getenv("ACZ_TERRAIN_DETAIL");
    if (enh_run[itd] < 0 && e && *e) return (float)atof(e);          /* (a value set by hand that is not a choice) */
    return enh_num(itd, enh_run[itd]);
}
int ps2_enh_gov_tdetail(float k) {
    static float last = 1.0f;
    int il, itd;
    if (k == last) return 0;
    int (*fn)(float, float) = enh_detail_fn;
    if (!fn || !enh_detail_rows(&il, &itd)) return -1;
    float td = ps2_enh_tdetail_now(), lod = enh_num(il, enh_run[il]);
    const char *e = getenv("ACZ_LOD_BIAS");
    if (enh_run[il] < 0 && e && *e) lod = (float)atof(e);
    if (!(td > 1.0f)) { last = k; return 0; }
    if (fn(lod, 1.0f + (td - 1.0f) * k) != 0) return -1;
    last = k;
    return 0;
}

/* ---- the NEXT MISSION tier (ps2_menv.h; REPORT of menu_1105b: every row's readers and what derives from them) ---- */
/* What is made only when the row starts on: at boot or at a cockpit's load (a pipeline, a game hook, images, the glass's bake).
   Such a row can go off at a mission and on again, but from "off at the start" only a restart turns it on. A group: all of
   its rows off at the start (the trails' pipelines and sampler serve any of the four). */
static const struct { const char *key, *off; int group; } enh_boot_tab[] = {
    { "ground_materials", "0", 0 },              /* zero_terrain_mat.c, mesh_det_fs: the material images and shaders */
    { "shader_water", "0", 0 },                  /* zero_water.c, wat_create, mesh_wat_fs: the masks, the hook */
    { "town_houses", "", 0 },                    /* zero_props.c: the hooks; mesh_city_fs */
    { "city_buildings", "0", 0 },                /* mesh_city_fs */
    { "explosion_burst", "0", 0 },               /* bst_init: the volume's pipeline; zero_vr.c burst_install: the hook */
    { "forest_distance", "", 0 },                /* zero_vr.c: the draw's hook only with a distance */
    { "eye_adapt", "0", 0 },                     /* expo_init: the pipeline */
    { "flash_light", "0", 0 },                   /* zero_vr.c flash_install: the hooks */
    { "canopy_cracks", "0", 0 },                 /* the glass's triangles at a cockpit's load */
    { "canopy_reflections", "0", 0 },            /* the interior cube baked at a cockpit's load */
    { "hud_behind_frames", "0", 0 },             /* hg_init: the combiner's meshes and the depth bounds of the pipelines */
    { "hud_glass", "0", 0 },                     /* ... */
    { "gaze_target", "0", 0 },                   /* zg_install: the hooks */
    { "g_vision", "0", 0 },                      /* gfv_init: the pipeline (shared with the head fade) */
    { "blasts", "0", 0 },                        /* zbl_install: the hook */
    { "vr_camera_shake", "1", 0 },               /* zero_cracks.c: the hook only when it may take the shake out */
    { "flyby", "0", 0 },                         /* zero_flyby_install: the hooks and the mixer */
    { "contrails", "0", 1 }, { "cloud_wake", "0", 1 }, { "wing_vapour", "0", 1 }, { "heat_haze", "0", 1 },   /* trl_init, zt_on */
    { "smoke_light", "0", 2 },                   /* (zero_trails_install: the hooks are in with any of the group, or with it) */
};
static void enh_boot_init(void) {
    int all = 1;
    for (unsigned t = 0; t < sizeof enh_boot_tab / sizeof enh_boot_tab[0]; t++) {
        int i = ps2_enh_find(enh_boot_tab[t].key);
        if (i < 0) continue;
        const char *e = getenv(enh_rows[i].it.env);
        int off = !strcmp(e ? e : "", enh_boot_tab[t].off);
        enh_boot_gate[i] = enh_boot_tab[t].off;
        if (enh_boot_tab[t].group == 1) all &= off; else enh_boot_off[i] = (unsigned char)off;
    }
    for (unsigned t = 0; t < sizeof enh_boot_tab / sizeof enh_boot_tab[0]; t++) {
        int i = ps2_enh_find(enh_boot_tab[t].key);
        if (i < 0) continue;
        if (enh_boot_tab[t].group == 1) enh_boot_off[i] = (unsigned char)all;
        if (enh_boot_tab[t].group == 2) enh_boot_off[i] = (unsigned char)(enh_boot_off[i] && all);
    }
}
static int enh_is_treal(int i) { return enh_is_cfg(i) && enh_rows[i].off == offsetof(ps2_settings, terrain_real); }
int ps2_enh_apply_now(int i) {
    if (i < 0 || i >= ENH_N) return PS2_ENH_RESTART;
    const ps2_enh_item *it = &enh_rows[i].it;
    if (it->kind == K_COMBO) {
        int x = enh_row_cx[i], t = PS2_ENH_LIVE;
        for (int j = 0; x >= 0 && j < enh_cx[x].nm; j++) {
            int a = ps2_enh_apply_now(enh_cx[x].m[j].row);
            if (a == PS2_ENH_RESTART) return a;
            if (a == PS2_ENH_MISSION) t = a;
        }
        return t;
    }
    if (enh_is_treal(i)) return PS2_ENH_MISSION;
    return it->apply == PS2_ENH_MISSION && enh_boot_off[i] ? PS2_ENH_RESTART : it->apply;
}
/* a row of the tier was saved: what its new value waits for. PS2_ENH_LIVE: it is the value in effect, nothing waits. */
static int enh_mission_mark(int i) {
    int c = enh_saved[i];
    if (c == enh_run[i]) { enh_mission[i] = 0; return PS2_ENH_LIVE; }
    if (c < 0 || (enh_boot_off[i] && strcmp(enh_rows[i].it.val[c], enh_boot_gate[i]))) { enh_mission[i] = 0; return PS2_ENH_RESTART; }
    enh_mission[i] = 1;
    return PS2_ENH_MISSION;
}
static void enh_treal_hold(void) { if (enh_treal_latch < 0) enh_treal_latch = (signed char)(ps2_cfg.terrain_real != 0); }
static int enh_treal_mark(int i) {
    enh_mission[i] = enh_treal_latch >= 0 && (ps2_cfg.terrain_real != 0) != enh_treal_latch;
    return enh_mission[i];
}
/* A new attempt in the same mission (RETRY, RESTART: zero_vr.c z_mission_init): the game does not enter the mission and the map
   stays loaded. These rows work only where it enters one; every other row of the tier is read per draw, frame, chunk or event.
   Their switches stay as they are, so what derives from them is not touched. */
static const struct { const char *key, *why; } enh_retry_tab[] = {
    { "start_view",        "set when the game enters a mission; a RETRY keeps the player's view" },
    { "town_houses",       "built with the map, and a RETRY does not load it: they would pop in" },
    { "city_buildings",    "built with the map, and a RETRY does not load it: they would pop in" },
    { "realistic_terrain", "the relief is baked with the map: it would pop in" },
    { "takeoff_recentre",  "a RETRY has no take-off recentre" },
};
static const char *enh_retry_why(int i) {
    for (size_t t = 0; t < sizeof enh_retry_tab / sizeof enh_retry_tab[0]; t++)
        if (!strcmp(enh_rows[i].it.key, enh_retry_tab[t].key)) return enh_retry_tab[t].why;
    return NULL;
}
const char *ps2_enh_retry_waits(int i) {
    if (i < 0 || i >= ENH_N) return NULL;
    int x = enh_row_cx[i];
    for (int j = 0; x >= 0 && j < enh_cx[x].nm; j++) {
        const char *w = enh_retry_why(enh_cx[x].m[j].row);
        if (w) return w;
    }
    return enh_retry_why(i);
}
/* not half a row: a member goes on waiting with a waiting member of its combo row */
static const char *enh_retry_keep(int i) {
    const char *w = enh_retry_why(i);
    for (int x = 0; !w && enh_member[i] && x < ENH_CX_N; x++) {
        int mine = 0;
        const char *held = NULL;
        for (int j = 0; j < enh_cx[x].nm; j++) {
            int m = enh_cx[x].m[j].row;
            mine |= m == i;
            if (enh_mission[m] && enh_retry_why(m)) held = enh_retry_why(m);
        }
        if (mine) w = held;
    }
    return w;
}
/* GameControl::enter, the game thread: the rows changed since the last edge take their saved values. Nothing changed: nothing
   happens, no reader runs again. retry: a new attempt, as above. */
static int enh_edge(int retry) {
    int n = 0, nk = 0;
    static short done[ENH_N], kept[ENH_N];
    static signed char was[ENH_N];
    enh_mlock_take();
    for (int i = 0; i < ENH_N; i++) {
        if (!enh_mission[i]) continue;
        if (retry && enh_retry_keep(i)) { kept[nk++] = (short)i; continue; }
        was[n] = enh_run[i];
        if (enh_is_treal(i)) enh_treal_latch = -1;
        else {
            int c = enh_saved[i];
            enh_mv[i] = enh_rows[i].it.val[c][0] ? enh_rows[i].it.val[c] : NULL;
            __atomic_store_n(&enh_mv_on[i], 1, __ATOMIC_RELEASE);
            enh_run[i] = (signed char)c;
        }
        enh_mission[i] = 0;
        done[n++] = (short)i;
    }
    if (n) {
        enh_combo_sync();
        enh_preset_sync();
        __atomic_add_fetch(&ps2_menv_gen, 1u, __ATOMIC_RELEASE);
    }
    enh_mlock_give();
    for (int k = 0; k < n; k++) {
        const ps2_enh_item *it = &enh_rows[done[k]].it;
        int c = enh_is_treal(done[k]) ? -1 : enh_saved[done[k]];
        if (enh_is_treal(done[k])) ps2_log("mission: %s: realistic terrain %d", it->key, ps2_cfg.terrain_real != 0);
        else ps2_log("mission: %s: %s -> %s (%s=%s)", it->key, was[k] >= 0 ? it->name[was[k]] : "custom", it->name[c], it->env,
                     it->val[c][0] ? it->val[c] : "default");
    }
    for (int k = 0; k < nk; k++)
        ps2_log("mission: %s keeps waiting for the next mission (a RETRY / RESTART: %s)", enh_rows[kept[k]].it.key, enh_retry_keep(kept[k]));
    ps2_log("mission: reloaded %d switch%s%s%s", n, n == 1 ? "" : "es", n ? "" : nk ? " (the rest keeps waiting: no reader runs again)"
            : " (nothing changed: no reader runs again)", retry ? " at a RETRY / RESTART" : "");
    return n;
}
int ps2_enh_mission_edge(void) { return enh_edge(0); }
int ps2_enh_retry_edge(void) { return enh_edge(1); }

int ps2_enh_set(int i, int c) {
    if (i < 0 || i >= ENH_N) return -1;
    const enh_row *r = &enh_rows[i];
    if (r->it.kind == K_ACT_RECENTRE) {
        ps2_xr_request_recenter();
        ps2_log("settings: ENHANCEMENTS: recentre the view");
        return PS2_ENH_ACTION;
    }
    if (r->it.kind == K_ACT_RESET) { ps2_enh_reset_all(); return PS2_ENH_ACTION; }
    if (r->it.kind == K_ACT_CARD) { ps2_quit_touch_card_open(PS2_TC_FROM_ENH); return PS2_ENH_ACTION; }
    if (r->it.kind == K_ACT_RUMBLE) { ps2_video_rumble_test(); return PS2_ENH_ACTION; }   /* (its answer: the page) */
    if (r->it.kind == K_ACT_NEUTRAL) { ps2_hotas_neutral_now(); return PS2_ENH_ACTION; }  /* (its answer: the page) */
    if (c < 0 || c >= r->it.n) return -1;
    if (r->it.kind == K_COMBO) {
        /* its members' values, saved; those that apply live do it now; held by the environment: no change at all */
        int x = enh_row_cx[i], live_ok = 1, mission = 0;
        if (x < 0 || ps2_enh_override(i)) return -1;
        char l[200];
        size_t n = 0;
        l[0] = 0;
        enh_mlock_take();                            /* (the edge sees all of a row's members or none) */
        for (int j = 0; j < enh_cx[x].nm; j++) {
            int m = enh_cx[x].m[j].row, k = enh_cx[x].m[j].set[c];
            const enh_row *mr = &enh_rows[m];
            if (enh_is_cfg(m)) {
                if (enh_is_treal(m)) enh_treal_hold();
                enh_cfg_put(m, mr->it.cfgv[k]);
                if (enh_is_treal(m) && enh_treal_mark(m)) mission = 1;
            } else {
                enh_saved[m] = (signed char)k;
                if (mr->it.apply == PS2_ENH_LIVE && enh_apply_live(m) != PS2_ENH_LIVE) live_ok = 0;
                if (mr->it.apply == PS2_ENH_MISSION && enh_mission_mark(m) == PS2_ENH_MISSION) mission = 1;
            }
            if (n + 1 < sizeof l) {
                n += (size_t)snprintf(l + n, sizeof l - n, "%s%s=%s", j ? ", " : "", mr->it.env[0] ? mr->it.env : mr->it.key,
                                      enh_is_cfg(m) ? mr->it.name[k] : mr->it.val[k][0] ? mr->it.val[k] : "default");
                if (n >= sizeof l) n = sizeof l - 1;
            }
        }
        enh_mlock_give();
        enh_preset_sync();
        enh_combo_sync();
        enh_saved[i] = (signed char)c;
        int res = ps2_enh_pending(i) || !live_ok ? PS2_ENH_RESTART : mission ? PS2_ENH_MISSION : PS2_ENH_LIVE;
        if (res == PS2_ENH_MISSION) ps2_log("settings: ENHANCEMENTS: %s = %s (next mission) %s", r->it.key, r->it.name[c], l);
        else ps2_log("settings: ENHANCEMENTS: %s = %s (%s; %s)", r->it.key, r->it.name[c], l,
                     res == PS2_ENH_LIVE ? "in effect now" : "after a restart");
        ps2_settings_save();
        return res;
    }
    if (r->it.kind == K_PRESET) {
        /* QUALITY PRESET: its rows' values, saved. A row the environment holds in this run is saved too (its switch
           still wins now; the file then says the preset for the next start without it), but not applied live. */
        if (enh_ovr[i]) return -1;
        int changed = 0, held = 0, restart = 0;
        for (int t = 0; t < ENH_PT_N; t++) {
            int j = enh_pt_row[t], want = j >= 0 ? enh_pt_ch[t][c] : 0;
            if (j < 0) continue;
            held += enh_ovr[j] != NULL;
            if (enh_saved[j] != want) {
                enh_saved[j] = (signed char)want; changed++;
                if (enh_rows[j].it.apply == PS2_ENH_LIVE && !enh_ovr[j]) enh_apply_live(j);
                if (enh_rows[j].it.apply == PS2_ENH_MISSION && !enh_ovr[j]) enh_mission_mark(j);
            }
            restart += ps2_enh_pending(j);
        }
        enh_preset_sync();
        ps2_log("settings: ENHANCEMENTS: quality_preset = %s (%d rows changed; %d rows held by the environment in this "
                "run, saved for the next start; %d wait for a restart)", r->it.name[c], changed, held, restart);
        ps2_settings_save();
        return restart ? PS2_ENH_RESTART : PS2_ENH_LIVE;
    }
    if (enh_is_cfg(i)) {
        if (r->live != L_PAD && ps2_enh_override(i)) return -1;      /* the switch in the environment holds it */
        if (r->live == L_PAD) ps2_settings_set_pad_layout((int)r->it.cfgv[c]);
        else {
            if (enh_is_treal(i)) enh_treal_hold();
            enh_cfg_put(i, r->it.cfgv[c]);
        }
        int mission = enh_is_treal(i) && enh_treal_mark(i);
        ps2_log("settings: ENHANCEMENTS: %s = %s%s", r->it.key, r->it.name[c], mission ? " (next mission)" : " (now)");
        ps2_settings_save();
        return mission ? PS2_ENH_MISSION : PS2_ENH_LIVE;
    }
    if (r->it.kind == K_ENV && enh_ovr[i]) return -1;
    enh_saved[i] = (signed char)c;
    if (r->live == L_PAD_ACTIVE && c >= 2) {        /* a pad: kept by its name (the list changes with the pads) */
        snprintf(enh_raw[i], sizeof enh_raw[i], "%s", r->it.val[c]);
        enh_saved[i] = -1;
    }
    int res = r->it.apply == PS2_ENH_LIVE && !enh_ovr[i] ? enh_apply_live(i)
            : r->it.apply == PS2_ENH_MISSION ? enh_mission_mark(i) : PS2_ENH_RESTART;
    enh_preset_sync();
    if (res == PS2_ENH_MISSION) ps2_log("settings: ENHANCEMENTS: %s = %s (next mission) %s=%s", r->it.key, r->it.name[c], r->it.env,
                                        r->it.val[c][0] ? r->it.val[c] : "default");
    else ps2_log("settings: ENHANCEMENTS: %s = %s (%s=%s, %s)", r->it.key, r->it.name[c], r->it.env,
                 r->it.val[c][0] ? r->it.val[c] : "default", res == PS2_ENH_LIVE ? "in effect now" : "after a restart");
    ps2_settings_save();
    return res;
}

/* RESET ALL TO DEFAULTS, the only reset (the page, F4 > Quick): the page's rows and ps2_cfg, what F4 > Quick's reset did
   (graphics, display, bindings, dead zones). Renderer thread. */
void ps2_enh_reset_all(void) {
    {   int seen = ps2_cfg.touch_card_seen;          /* (not a setting: the card must not show again by itself) */
        enh_treal_hold();                            /* (this mission keeps its terrain until the edge) */
        ps2_settings_defaults(&ps2_cfg);
        ps2_cfg.touch_card_seen = seen;
        ps2_settings_touch(PS2_CFG_SWAPCHAIN | PS2_CFG_WINDOW | PS2_CFG_SAMPLER | PS2_CFG_FPS);
    }
    for (int i = 0; i < ENH_N; i++) {
        const enh_row *r = &enh_rows[i];
        if (r->it.apply == PS2_ENH_ACTION || r->it.kind == K_PRESET || r->it.kind == K_COMBO) continue;
        if (enh_is_cfg(i)) {
            if (r->live == L_PAD) ps2_settings_set_pad_layout((int)r->it.cfgv[r->it.def]);
            else if (!ps2_enh_override(i)) enh_cfg_put(i, r->it.cfgv[r->it.def]);
            if (enh_is_treal(i)) enh_treal_mark(i);
        } else if (!enh_ovr[i] || r->it.kind == K_PATHS) {
            enh_mlock_take();
            enh_saved[i] = (signed char)r->it.def;
            if (r->it.apply == PS2_ENH_LIVE) enh_apply_live(i);
            if (r->it.apply == PS2_ENH_MISSION) enh_mission_mark(i);
            enh_mlock_give();
        }
    }
    enh_preset_sync();
    enh_combo_sync();
    ps2_log("settings: ENHANCEMENTS: everything back to the defaults (the page's rows and the F4 settings)");
    ps2_settings_save();
}

static int enh_new_install_preset(void) { return enh_auto_preset; }
static void enh_cfg_loaded(void) { enh_cfg_ready = 1; enh_combo_sync(); }

/* ---- the QUALITY PRESET row's help (perf6_presets): the GPU, its class's suggestion, the headset's rate and the cadence
   advice (the rate is the Meta Quest Link app's / Virtual Desktop's setting, not ours); when the GPU ran behind for long
   (ps2_vk_perf_behind, ACZ_PERF_HINT) the hint instead. F4's tooltip (long_form) also lists every row a preset sets. */
extern long long ps2_xr_display_period(void);   /* ps2_xr.c (ns; 0 without a headset) */
extern int ps2_vk_perf_behind(int *of, int *gov_level);   /* ps2_video.c */
static int enh_hz_now(void) {
    long long per = ps2_xr_display_period();
    return per > 1000000 ? (int)(1e9 / (double)per + 0.5) : 0;
}
static const char *enh_gpu_short(void) {        /* "RTX 3090 Ti" of "NVIDIA GeForce RTX 3090 Ti" */
    static const char *const pre[] = { "NVIDIA ", "GeForce ", "AMD ", "Radeon(TM) ", "Intel(R) " };
    const char *s = enh_gpu_name;
    for (int again = 1; again; ) {
        again = 0;
        for (size_t k = 0; k < sizeof pre / sizeof pre[0]; k++)
            if (!strncmp(s, pre[k], strlen(pre[k]))) { s += strlen(pre[k]); again = 1; }
    }
    return s;
}
const char *ps2_enh_hint(char *buf, int n) {
    int of = 0, lv = -1, b = ps2_vk_perf_behind(&of, &lv), hz = enh_hz_now();
    buf[0] = 0;
    if (b <= 0) return buf;
    int cur = enh_qp_row >= 0 ? enh_run[enh_qp_row] : -1;
    const char *next = cur == PS2_QP_LOW ? NULL : cur == PS2_QP_MEDIUM ? "LOW" : "MEDIUM";
    char r[40] = "";
    if (hz > 90) snprintf(r, sizeof r, ", or 90 Hz in the Meta Quest Link app");
    else if (hz > 72) snprintf(r, sizeof r, ", or 72 Hz in the Meta Quest Link app");
    snprintf(buf, (size_t)n, "Your GPU ran behind the headset in %d of the last %d s: %s%s%s.", b, of,
             next ? "try QUALITY PRESET " : "", next ? next : "", next ? r
             : hz > 72 ? "try a lower rate in the Meta Quest Link app" : "close other programs");
    return buf;
}
const char *ps2_enh_help(int i, char *buf, int n, int long_form) {
    const ps2_enh_item *it = ps2_enh_item_at(i);
    if (!it) { snprintf(buf, (size_t)n, "?"); return buf; }
    if (it->kind == K_COMBO && long_form) {          /* F4's tooltip: the switches behind the row */
        int x = enh_row_cx[i];
        size_t l = (size_t)snprintf(buf, (size_t)n, "%s\n\nSets:", it->help);
        for (int j = 0; x >= 0 && j < enh_cx[x].nm && l + 1 < (size_t)n; j++)
            l += (size_t)snprintf(buf + l, (size_t)n - l, "%s %s", j ? "," : "", enh_rows[enh_cx[x].m[j].row].it.env[0]
                                  ? enh_rows[enh_cx[x].m[j].row].it.env : enh_rows[enh_cx[x].m[j].row].it.key);
        return buf;
    }
    if (it->kind != K_PRESET) return it->help;
    char hint[200], gpu[120] = "", rate[24] = "";
    int hz = enh_hz_now();
    ps2_enh_hint(hint, sizeof hint);
    if (enh_gpu_name[0] && enh_gpu_cls >= 0) snprintf(gpu, sizeof gpu, " Your %s: %s.", enh_gpu_short(), enh_qp_names[enh_gpu_cls]);
    if (hz) snprintf(rate, sizeof rate, " (now %d Hz)", hz);
    if (hint[0]) snprintf(buf, (size_t)n, "%s", hint);    /* (the page shows 3 lines of ~46 letters) */
    else snprintf(buf, (size_t)n, "Sets resolution and other costly rows.%s Slower PCs: 90 or 72 Hz in the Meta Quest "
                  "Link app%s.", gpu, rate);
    if (long_form) {
        char rows[400];
        size_t l = strlen(buf);
        enh_preset_rows(rows, sizeof rows);
        snprintf(buf + l, (size_t)n - l, "\n\nLOW / MEDIUM / HIGH set: %s. CUSTOM: one of them was changed by hand. A "
                 "row set in the environment (a launcher word) keeps its value. Smooth motion and every look and comfort "
                 "row stay as you set them. The headset's refresh rate is chosen in the Meta Quest Link app or in Virtual "
                 "Desktop: 120 Hz with HIGH on fast GPUs, 90 Hz with MEDIUM (72 Hz on a 3060 / 4060), 72-90 Hz with LOW.", rows);
        l = strlen(buf);
        if (enh_gpu_name[0] && enh_gpu_cls >= 0)
            snprintf(buf + l, (size_t)n - l, "\n\nGPU: %s, %.1f GB (%s): %s suggested.", enh_gpu_name, enh_gpu_vram,
                     enh_gpu_why, enh_qp_names[enh_gpu_cls]);
    }
    return buf;
}
/* ps2_video.c, once the device is chosen: the GPU in use (its class drives the page's suggestion; a new install's
   preset came from the probe before start) */
void ps2_enh_set_gpu(const char *name, int discrete, double vram_gb) {
    char why[96];
    int cls = enh_gpu_class(name, discrete, vram_gb, why, sizeof why);
    const char *fake = getenv("ACZ_GPU_DETECT_NAME");
    int s = enh_qp_row >= 0 ? enh_saved[enh_qp_row] : -1, r = enh_qp_row >= 0 ? enh_run[enh_qp_row] : -1;
    ps2_log("settings: GPU %s (%s), %.1f GB device-local: class %s (%s)%s; QUALITY PRESET saved %s, in effect %s", name,
            discrete ? "discrete" : "not discrete", vram_gb, enh_qp_names[cls], why,
            fake && *fake ? "; ACZ_GPU_DETECT_NAME stands in for it" : "", s >= 0 ? enh_qp_names[s] : "CUSTOM",
            r >= 0 ? enh_qp_names[r] : "CUSTOM");
    if (enh_auto_preset >= 0 && !(fake && *fake) && strcmp(enh_gpu_name, name))
        ps2_log("settings: QUALITY PRESET: the new install's preset came from %s (probed before start)", enh_gpu_name);
    if (fake && *fake) return;                  /* (the stand-in keeps the page's suggestion) */
    snprintf(enh_gpu_name, sizeof enh_gpu_name, "%s", name);
    snprintf(enh_gpu_why, sizeof enh_gpu_why, "%s", why);
    enh_gpu_vram = vram_gb;
    enh_gpu_cls = cls;
}

/* ---- the launcher (ps2_launcher.cpp, menu_1105c; docs/SETTINGS.md layer 1) ----------------------------------------------------
   It runs before anything reads the switches, on its own thread while main() waits. Its choices stay in memory (enh_saved, as
   the page's); PLAY commits them: the same switches the constructor would have set from a file that says so, so the readers see
   them in this same boot and the page does not call them pending. ACZ_LAUNCHER wins over the file's `launcher`, as every switch
   wins over its row; the file's two keys are no table rows: they set no switch, the injected list does not hear of them. */
int ps2_enh_launcher_wanted(void) {
    const char *e = getenv("ACZ_LAUNCHER");
    return e && *e ? *e != '0' : enh_lch_show;
}
int ps2_enh_launcher_show_get(void) { return enh_lch_show; }
void ps2_enh_launcher_show_set(int on) {
    on = on != 0;
    enh_lch_dirty |= on != enh_lch_show;
    enh_lch_show = on;
    enh_cat_helps[PS2_ENH_QUALITY] = on ? "Preset, resolution, edges, clouds and movies."
                                        : "Preset, resolution, edges, clouds, movies. Launcher off: F4 > Quick brings it back.";
}
void ps2_enh_launcher_begin(void) {          /* ps2_cfg from the file, without the log lines (ps2_settings_load says them) */
    cfg_quiet = 1;
    settings_load_file();
    cfg_quiet = 0;
    if (!pad_layout_seen && ps2_enh_applied()) ps2_cfg.pad_layout = PS2_PAD_LAYOUT_MODERN;   /* (as ps2_settings_load; a save may follow) */
    enh_cfg_loaded();
}
/* the launcher's device, for the GPU line until vk_init says the one in use; a new install's probe or a stand-in comes first */
void ps2_enh_launcher_gpu(const char *name, int discrete, double vram_gb) {
    const char *fake = getenv("ACZ_GPU_DETECT_NAME");
    if (enh_gpu_name[0]) return;
    if (fake && *fake) { enh_probe_gpu(); return; }
    snprintf(enh_gpu_name, sizeof enh_gpu_name, "%s", name);
    enh_gpu_vram = vram_gb;
    enh_gpu_cls = enh_gpu_class(name, discrete, vram_gb, enh_gpu_why, sizeof enh_gpu_why);
}
const char *ps2_enh_launcher_gpu_name(char *buf, int n) { snprintf(buf, (size_t)n, "%s", enh_gpu_name); return buf; }
const char *ps2_enh_launcher_gpu_line(char *buf, int n) {
    if (!enh_gpu_name[0] || enh_gpu_cls < 0) { snprintf(buf, (size_t)n, "GPU unknown."); return buf; }
    snprintf(buf, (size_t)n, "Your %s: %s. (%s, %.1f GB; %s.)%s", enh_gpu_short(), enh_qp_names[enh_gpu_cls], enh_gpu_name, enh_gpu_vram,
             enh_gpu_why, enh_auto_preset >= 0 ? " First start: this preset was chosen for your GPU." : "");
    return buf;
}
/* what no choice can change in this run although nothing holds it: 3D COCKPIT with no model files given */
const char *ps2_enh_launcher_blocked(int i) {
    if (i < 0 || i >= ENH_N || enh_rows[i].it.kind != K_PATHS) return NULL;
    const char *e = enh_cpit_keep[0] ? enh_cpit_keep[0] : getenv("ACZ_VR_COCKPIT_MODEL");
    return e && *e && strcmp(e, "none") ? NULL : "This start has no 3D cockpit files (the word cockpit of play_zero_vr.sh).";
}
/* a choice, in memory; 1 changed, 0 the same, -1 held. A combo row through its members, the preset through its sub-table (the
   ini-only ladder rows too), a pad by its name: as ps2_enh_set, without the save and the restart it would ask for */
int ps2_enh_launcher_choose(int i, int c) {
    if (i < 0 || i >= ENH_N || c < 0 || c >= enh_rows[i].it.n || ps2_enh_launcher_blocked(i)) return -1;
    const enh_row *r = &enh_rows[i];
    if (r->it.kind == K_PRESET ? enh_ovr[i] != NULL : ps2_enh_override(i) != NULL) return -1;
    if (ps2_enh_choice(i) == c) return 0;
    if (r->it.kind == K_COMBO) {
        int x = enh_row_cx[i];
        for (int j = 0; x >= 0 && j < enh_cx[x].nm; j++) {
            int m = enh_cx[x].m[j].row, k = enh_cx[x].m[j].set[c];
            if (enh_is_cfg(m)) enh_cfg_put(m, enh_rows[m].it.cfgv[k]);
            else enh_saved[m] = (signed char)k;
        }
    } else if (r->it.kind == K_PRESET) {
        for (int t = 0; t < ENH_PT_N; t++)
            if (enh_pt_row[t] >= 0) enh_saved[enh_pt_row[t]] = enh_pt_ch[t][c];   /* (a row the environment holds: saved, its switch wins) */
    } else if (r->live == L_PAD_ACTIVE && c >= 2) {
        snprintf(enh_raw[i], sizeof enh_raw[i], "%s", r->it.val[c]);
        enh_saved[i] = -1;
    } else enh_saved[i] = (signed char)c;
    enh_preset_sync();
    enh_combo_sync();
    if (r->it.kind == K_COMBO) enh_saved[i] = (signed char)c;
    enh_lch_dirty = 1;
    ps2_log("launcher: %s = %s", r->it.key, r->it.name[c]);
    return 1;
}
static int enh_lch_in_list(const char *key) {
    size_t l = strlen(key);
    for (const char *p = strstr(enh_lch_changed, key); p; p = strstr(p + 1, key))
        if ((p == enh_lch_changed || p[-1] == ',') && (p[l] == 0 || p[l] == ',')) return 1;
    return 0;
}
int ps2_enh_launcher_changed(int i) { return i >= 0 && i < ENH_N && enh_lch_in_list(enh_rows[i].it.key); }
/* the rows of the other categories in the list, by their labels */
void ps2_enh_launcher_changed_others(char *buf, int n) {
    size_t l = 0;
    buf[0] = 0;
    for (int i = 0; i < ENH_N && l + 1 < (size_t)n; i++) {
        if (!ps2_enh_launcher_changed(i) || enh_rows[i].it.cat == PS2_ENH_QUALITY || enh_rows[i].live == L_PAD_ACTIVE) continue;
        l += (size_t)snprintf(buf + l, (size_t)n - l, "%s%s", l ? ", " : "", enh_rows[i].it.label);
    }
}
/* the file's two keys. launcher_changed: what waits for the next start, as this moment's pending rows (a row changed and changed
   back is not in it; a row the environment holds or a hand-written value never was asked of the page); it goes with the next save */
static void enh_lch_write(FILE *f) {
    char l[sizeof enh_lch_changed];
    size_t n = 0;
    int preset = 0;
    l[0] = 0;
    for (int t = 0; t < ENH_PT_N; t++) if (enh_pt_row[t] >= 0 && ps2_enh_pending(enh_pt_row[t])) preset = 1;
    if (preset && enh_qp_row >= 0) n += (size_t)snprintf(l, sizeof l, "%s", enh_rows[enh_qp_row].it.key);
    for (int i = 0; i < ENH_N; i++) {
        if (enh_saved[i] < 0 || !ps2_enh_pending(i) || ps2_enh_launcher_blocked(i) || n + strlen(enh_rows[i].it.key) + 2 >= sizeof l) continue;
        n += (size_t)snprintf(l + n, sizeof l - n, "%s%s", n ? "," : "", enh_rows[i].it.key);
    }
    fprintf(f, "%-26s = %d ; the launcher before the game; 0: \"do not show again\" (ACZ_LAUNCHER wins)\n", "launcher", enh_lch_show);
    if (l[0]) fprintf(f, "%-26s = %s ; waited for the next start: the launcher marks them once\n", "launcher_changed", l);
}
/* PLAY: every row's switch as the constructor would set it from these choices (a switch of the environment stays; "" removes one),
   the run's state with it, the start-up log's list rebuilt in the table's order. Returns the switches that changed. */
int ps2_enh_launcher_commit(void) {
    int changed = 0;
    enh_injected = 0;
    enh_injected_list[0] = 0;
    for (int i = 0; i < ENH_N; i++) {
        const ps2_enh_item *it = &enh_rows[i].it;
        if (it->kind == K_ENV && enh_pheld[i]) {       /* ACZ_QUALITY_PRESET: as the constructor left it */
            const char *v = it->val[enh_qpv[i]];
            enh_injected++;
            enh_list_add(it->env, "=", *v ? v : "(default)");
        } else if (it->kind == K_ENV && !enh_ovr[i] && enh_marker) {
            const char *v = enh_saved[i] >= 0 ? it->val[enh_saved[i]] : enh_raw[i];
            const char *was = getenv(it->env);
            if (strcmp(was ? was : "", v)) {
                ps2_log("launcher: %s=%s (was %s)", it->env, *v ? v : "(unset)", was ? was : "(unset)");
                enh_setenv(it->env, v);
                changed++;
            }
            if (*v) {
                enh_injected++;
                enh_list_add(it->env, "=", v);
            }
            enh_run[i] = enh_saved[i];
        } else if (it->kind == K_PATHS && enh_marker) {
            const char *e = enh_cpit_keep[0] ? enh_cpit_keep[0] : getenv("ACZ_VR_COCKPIT_MODEL");
            if (e && *e && strcmp(e, "none")) {
                int off = enh_saved[i] == 1, was_off = enh_cpit_keep[0] != NULL && !getenv("ACZ_VR_COCKPIT_MODEL");
                if (off && !was_off) {
                    enh_cpit_save();
                    enh_setenv("ACZ_VR_COCKPIT_MODEL", "");
                    enh_setenv("ACZ_VR_COCKPIT_MODELS", "");
                    ps2_log("launcher: ACZ_VR_COCKPIT_MODEL(S) removed (3D COCKPIT OFF)");
                    changed++;
                } else if (!off && was_off) {
                    enh_setenv("ACZ_VR_COCKPIT_MODEL", enh_cpit_keep[0]);
                    enh_setenv("ACZ_VR_COCKPIT_MODELS", enh_cpit_keep[1] ? enh_cpit_keep[1] : "");
                    ps2_log("launcher: ACZ_VR_COCKPIT_MODEL(S) back (3D COCKPIT ON)");
                    changed++;
                }
                if (off) enh_list_add("(3D cockpit off)", "", "");
                enh_run[i] = (signed char)(off ? 1 : 0);
            }
        }
    }
    enh_preset_sync();
    enh_combo_sync();
    int had = enh_lch_changed[0] != 0;
    enh_lch_changed[0] = 0;
    if (enh_lch_dirty || had) ps2_settings_save();
    enh_lch_dirty = 0;
    return changed;
}

void ps2_settings_save(void) {
    char tmp[1100];
    const char *path;
    const char *last = "";
    FILE *f;
    if (cfg_file_disabled) return;
    atomic_store(&cfg_save_pending, 0);
    path = ps2_settings_path();
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "w");
    if (!f) { ps2_log("settings: cannot write %s", tmp); return; }
    fprintf(f, "# Settings.\n"
               "# Written by the in-game menus (F4, ENHANCEMENTS).  Hand edits are kept as long as\n"
               "# each line stays key = value; unknown keys are ignored.\n");
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        const cfg_field *fd = &fields[i];
        char val[32];
        if (enh_cfg_folded(fd->off)) continue;      /* (a combo row's key in [enhancements] stands for it) */
        if (strcmp(last, fd->section)) {
            fprintf(f, "\n[%s]\n", fd->section);
            last = fd->section;
        }
        if (fd->off == offsetof(ps2_settings, pad_layout) && layout_env >= 0)
            snprintf(val, sizeof val, "%d", layout_file);       /* keep the saved layout under ACZ_PAD_LAYOUT */
        else if (fd->type == 'i') snprintf(val, sizeof val, "%d", *(int *)((char *)&ps2_cfg + fd->off));
        else snprintf(val, sizeof val, "%.6f", (double)*(float *)((char *)&ps2_cfg + fd->off));
        if (fd->comment[0]) fprintf(f, "%-26s = %-10s ; %s\n", fd->key, val, fd->comment);
        else fprintf(f, "%-26s = %s\n", fd->key, val);
    }
    enh_write(f);
    fprintf(f, "\n[keyboard]\n; action = primary secondary, as SDL scancodes (0 = unbound)\n");
    for (int a = 0; a < PS2_ACT_COUNT; a++)
        fprintf(f, "%-26s = %3d %3d    ; %s, %s\n", act_keys[a], ps2_cfg.key[a][0],
                ps2_cfg.key[a][1], ps2_key_name(ps2_cfg.key[a][0]),
                ps2_key_name(ps2_cfg.key[a][1]));
    fprintf(f, "\n[controller]\n; action = primary secondary: 1 + SDL button, or 256 | axis << 1 | positive\n");
    for (int a = 0; a < PS2_ACT_COUNT; a++)
        fprintf(f, "%-26s = %3d %3d    ; %s, %s\n", act_keys[a], ps2_cfg.pad[a][0],
                ps2_cfg.pad[a][1], ps2_padbind_name(ps2_cfg.pad[a][0]),
                ps2_padbind_name(ps2_cfg.pad[a][1]));
    ps2_hotas_ini_write(f);                     /* the [joystick:<GUID>] sections, last */
    if (fclose(f) != 0) { ps2_log("settings: write to %s failed", tmp); return; }
    remove(path);
    if (rename(tmp, path) != 0) ps2_log("settings: cannot replace %s", path);
}

int ps2_settings_save_pending(void) { return atomic_load(&cfg_save_pending); }

void ps2_settings_touch(int dirty) {
    if (dirty & PS2_CFG_SAVE) atomic_store(&cfg_save_pending, 1);
    atomic_fetch_or(&cfg_dirty, dirty);
}

int ps2_settings_take_dirty(void) { return atomic_exchange(&cfg_dirty, 0); }

static const u32 ws_addr[2]  = { 0x00440828u, 0x0044082Cu };
static const u32 ws_orig[2]  = { 0x440315C2u, 0x441013D7u };
static const u32 ws_patch[2] = { 0x43D638F3u, 0x43EB7385u };

void ps2_settings_apply_game_patches(void) {
    static int logged = -1;
    if (!ps2_ram) return;
    for (int i = 0; i < 2; i++) {
        u32 cur = ps2_r32(ws_addr[i]);
        if (ps2_cfg.widescreen) {
            if (cur == ws_orig[i]) ps2_w32(ws_addr[i], ws_patch[i]);
        } else if (cur == ws_patch[i]) {
            ps2_w32(ws_addr[i], ws_orig[i]);
        }
    }
    if (logged != ps2_cfg.widescreen) {
        logged = ps2_cfg.widescreen;
        ps2_log("settings: widescreen patch %s", logged ? "on" : "off");
    }
}

float ps2_pad_axis_value(int raw, int positive) {
    float v = (float)raw / (raw < 0 ? 32768.0f : 32767.0f);
    return clampf(positive ? v : -v, 0.0f, 1.0f);
}

void ps2_pad_stick(float deadzone, float axis_scale, int invert, const float v[4],
                   unsigned char *x, unsigned char *y) {
    unsigned raw[4], px, nx, py, ny;
    for (int i = 0; i < 4; i++)
        raw[i] = (unsigned char)clampf(v[i] * axis_scale * 255.0f, 0.0f, 255.0f);
    px = raw[invert & 1 ? 2 : 3];
    nx = raw[invert & 1 ? 3 : 2];
    py = raw[invert & 2 ? 0 : 1];
    ny = raw[invert & 2 ? 1 : 0];
#define MERGE(pos, neg) ((pos) != 0 ? 127u + ((pos) + 1u) / 2u : 127u - (neg) / 2u)
    *x = (unsigned char)MERGE(px, nx);
    *y = (unsigned char)MERGE(py, ny);
#undef MERGE
    if (deadzone > 0.0f) {
        float fx = px != 0 ? (float)px / 255.0f : (float)nx / -255.0f;
        float fy = py != 0 ? (float)py / 255.0f : (float)ny / -255.0f;
        if (fx != 0.0f || fy != 0.0f) {
            float theta = atan2f(fy, fx);
            float dzx = cosf(theta) * deadzone, dzy = sinf(theta) * deadzone;
            int in_x = fx < 0.0f ? fx > dzx : fx <= dzx;
            int in_y = fy < 0.0f ? fy > dzy : fy <= dzy;
            if (in_x && in_y) *x = *y = 127;
        }
    }
}

int ps2_pad_trigger(float button_deadzone, float value, unsigned char *pressure) {
    float s = clampf(value, 0.0f, 1.0f);
    float d = button_deadzone > 0.0f && s < button_deadzone ? 0.0f : s;
    *pressure = (unsigned char)(d * 255.0f);
    return d > 0.0f;
}

int ps2_pad_button(float button_deadzone, float value) {
    return (value < button_deadzone ? 0.0f : value) > 0.0f;
}
