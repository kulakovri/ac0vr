#ifndef PS2_SETTINGS_H
#define PS2_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PS2_ACT_SELECT = 0, PS2_ACT_L3, PS2_ACT_R3, PS2_ACT_START,
    PS2_ACT_UP, PS2_ACT_RIGHT, PS2_ACT_DOWN, PS2_ACT_LEFT,
    PS2_ACT_L2, PS2_ACT_R2, PS2_ACT_L1, PS2_ACT_R1,
    PS2_ACT_TRIANGLE, PS2_ACT_CIRCLE, PS2_ACT_CROSS, PS2_ACT_SQUARE,
    PS2_ACT_LS_UP, PS2_ACT_LS_DOWN, PS2_ACT_LS_LEFT, PS2_ACT_LS_RIGHT,
    PS2_ACT_RS_UP, PS2_ACT_RS_DOWN, PS2_ACT_RS_LEFT, PS2_ACT_RS_RIGHT,
    PS2_ACT_COUNT
};
#define PS2_ACT_BUTTONS 16
#define PS2_BIND_SLOTS  2

#define PS2_PADBIND_AXIS          0x100
#define PS2_PADBIND_BUTTON(b)     ((int)(b) + 1)
#define PS2_PADBIND_AXISDIR(a, p) (PS2_PADBIND_AXIS | ((int)(a) << 1) | ((p) ? 1 : 0))

enum { PS2_PRESET_LOW, PS2_PRESET_MEDIUM, PS2_PRESET_HIGH, PS2_PRESET_ULTRA,
       PS2_PRESET_CUSTOM };
enum { PS2_TEXFILTER_GAME, PS2_TEXFILTER_NEAREST, PS2_TEXFILTER_BILINEAR };
enum { PS2_SCALE_NEAREST, PS2_SCALE_BILINEAR, PS2_SCALE_SHARP };
enum { PS2_WIN_WINDOWED, PS2_WIN_BORDERLESS, PS2_WIN_EXCLUSIVE };
enum { PS2_ASPECT_STRETCH, PS2_ASPECT_AUTO, PS2_ASPECT_4_3, PS2_ASPECT_16_9,
       PS2_ASPECT_CUSTOM };
enum { PS2_PRESENT_MAILBOX, PS2_PRESENT_FIFO, PS2_PRESENT_IMMEDIATE,
       PS2_PRESENT_FIFO_RELAXED };
enum { PS2_HUD_STRETCHED, PS2_HUD_CENTERED };
/* controller layouts: classic = the [controller] bindings (PS2 positions by default); modern = a fixed
   style preset (throttle on the triggers as analog L1/R1, yaw on the bumpers); the custom bindings are kept */
enum { PS2_PAD_LAYOUT_CLASSIC, PS2_PAD_LAYOUT_MODERN };
#define PS2_HAVE_PAD_LAYOUT 1             /* ps2_texrep.c follows the layout for the LB/RB vs LT/RT glyphs */
#define PS2_INTERNAL_RES_MAX 8

typedef struct ps2_settings {
    int   preset;
    int   internal_res;
    int   tex_filter;
    int   anisotropy;
    int   deinterlace;
    int   fxaa;
    float sharpen;
    int   scale_filter;
    float brightness;
    float contrast;
    float gamma;
    float saturation;
    int   window_mode;
    int   fullscreen_type;
    int   window_w, window_h;
    int   fs_w, fs_h;
    float fs_hz;
    int   aspect;
    float aspect_custom;
    int   integer_scale;
    int   widescreen;
    int   hud_layout;
    int   present_mode;
    int   fps_limit;
    int   show_fps;
    float ui_scale;
    int   block_input_in_menu;
    int   pad_layout;
    int   key[PS2_ACT_COUNT][PS2_BIND_SLOTS];
    int   pad[PS2_ACT_COUNT][PS2_BIND_SLOTS];
    float deadzone;
    float axis_scale;
    float button_deadzone;
    int   invert[2];
    /* immersion (F4 > Immersion, [immersion]): canopy water on the native cockpit's glass, its mist (cloud) and grime
       (debris), their strength and drop size; weather/debris rumble and sound levels */
    int   imm_canopy, imm_mist, imm_grime;
    float imm_water, imm_drop_size, imm_rumble, imm_sound;
    /* inside cloud: the vapour volume around the aircraft (strength, 0 off) and the cockpit's diffuse cloud light */
    float imm_cloud_volume, imm_cloud_light;
    /* canopy ice when cold (visual only): the water freezes above the freezing level, rime / frost in cold cloud */
    int   imm_ice;
    /* realistic terrain (F4 > Rendering, [graphics] realistic_terrain): relief light, gullies, cavity and the terrain's
       sun shadow baked from the game's heightmap (zero_terrain.c; ACZ_TERRAIN_REAL overrides) */
    int   terrain_real;
    /* spatial audio (F4 > Immersion, [immersion]; ps2_spatial.c): binaural 3D sound (0 off, 1 on, 2 auto = VR only),
       Doppler strength (fraction of the real speed of sound, 0 off), cockpit muffling 0..1, granular engine 0..1 */
    int   imm_hrtf;
    float imm_doppler, imm_muffle, imm_engine_grain;
    /* the TOUCH CONTROLS card (bug #239, ps2_quit.c) was shown once by itself for HANDS ON STICK ([input]) */
    int   touch_card_seen;
} ps2_settings;

extern ps2_settings ps2_cfg;

/* the immersion settings as used: an environment variable, when set, overrides the saved value (launcher, tests):
   ACZ_VR_CANOPY (0 off), ACZ_VR_CANOPY_GAIN, ACZ_IMM_RUMBLE, ACZ_IMM_SOUND */
int   ps2_imm_canopy_on(void);
int   ps2_imm_ice_on(void);        /* ACZ_VR_CANOPY_ICE (0 off) */
float ps2_imm_water(void);
float ps2_imm_rumble(void);
float ps2_imm_sound(void);
float ps2_imm_veil(void);          /* ACZ_IMM_VEIL; 0 also with ACZ_IMMERSE=0 */
float ps2_imm_cloud_light(void);   /* ACZ_IMM_CLOUD_LIGHT; 0 also with ACZ_IMMERSE=0 */

enum { PS2_CFG_SWAPCHAIN = 1, PS2_CFG_WINDOW = 2, PS2_CFG_SAMPLER = 4,
       PS2_CFG_FPS = 8, PS2_CFG_SAVE = 16 };

void ps2_settings_defaults(ps2_settings *s);
void ps2_settings_default_bindings(ps2_settings *s, int keyboard, int pad);
void ps2_settings_layout_bindings(int layout, int pad[PS2_ACT_COUNT][PS2_BIND_SLOTS]);
const int *ps2_settings_pad_binds(int act);
void ps2_settings_set_pad_layout(int layout);
void ps2_settings_apply_preset(ps2_settings *s, int preset);
int  ps2_settings_preset_matches(const ps2_settings *s);
void ps2_settings_load(void);
void ps2_settings_save(void);
int  ps2_settings_save_pending(void);
void ps2_settings_disable_file(void);
const char *ps2_settings_path(void);
void ps2_settings_touch(int dirty);
int  ps2_settings_take_dirty(void);

/* ---- ENHANCEMENTS: the player-facing improvements as settings (ps2_settings.c, [enhancements] in the file) ----
   One table drives the in-game page (pause menu > ENHANCEMENTS, ps2_quit.c), F4 > Enhancements (ps2_ui.cpp) and the
   settings file. Most items set an environment switch at start-up: with ACZ_MENU_SETTINGS=1 (the launchers set it)
   every item whose switch is not set in the environment gets its saved value before anything reads it (a switch set
   by hand or by a launcher word still wins: the page shows it locked). Without ACZ_MENU_SETTINGS (test runs) the
   switches keep their built-in defaults. The immersion and sound items are the existing F4 settings ([immersion],
   [graphics] realistic_terrain, [input] pad_layout) and work in every run. */
enum { PS2_ENH_LIVE, PS2_ENH_MISSION, PS2_ENH_RESTART, PS2_ENH_ACTION };   /* when a change takes effect */
/* the page's seven categories (docs/SETTINGS.md). An item's cat can also be PS2_ENH_CAT_INI (an expert key: in the file, set
   at start as ever, listed by F4 > Enhancements > Expert (ini), never on the page) or PS2_ENH_CAT_OFF (listed nowhere: a
   combo row's member, a conditional row while its hardware is absent). */
enum { PS2_ENH_QUALITY, PS2_ENH_PICTURE, PS2_ENH_COCKPIT, PS2_ENH_HUD, PS2_ENH_COMFORT, PS2_ENH_SOUND, PS2_ENH_CONTROLS,
       PS2_ENH_CATS, PS2_ENH_CAT_INI = PS2_ENH_CATS, PS2_ENH_CAT_OFF };
#define PS2_ENH_CHOICES 6
typedef struct ps2_enh_item {
    const char *key;                       /* [enhancements] key (cfg-backed items: their own section's key) */
    const char *env;                       /* the switch it sets, or (cfg-backed) the one that overrides it */
    unsigned char cat, apply, n, def;      /* category, PS2_ENH_*, choices, the player's default choice */
    signed char code_def;                  /* the choice that equals the switch's built-in default (-1 none) */
    unsigned char kind;                    /* internal (ps2_settings.c) */
    const char *label;                     /* page row (upper case, the game's font) */
    const char *help;                      /* help line (mixed case) */
    const char *val[PS2_ENH_CHOICES];      /* switch values ("" = leave the switch unset: its built-in default) */
    const char *name[PS2_ENH_CHOICES];     /* value names (upper case) */
    float cfgv[PS2_ENH_CHOICES];           /* cfg-backed: the setting's value per choice */
} ps2_enh_item;

int  ps2_enh_count(void);
const ps2_enh_item *ps2_enh_item_at(int i);
int  ps2_enh_find(const char *key);        /* an item by its key, -1 none */
int  ps2_enh_members(int i, int *rows, int max);   /* a combo row: its member rows (at most max), their count; 0 for other rows */
const char *ps2_enh_cat_name(int cat);     /* "QUALITY", ... */
const char *ps2_enh_cat_help(int cat);
const char *ps2_enh_cat_head(int cat);     /* the category's heading: "THESE APPLY AT THE NEXT START", the changes waiting
                                              ("3 CHANGES AT THE NEXT MISSION", at most 30 letters), else "" */
int  ps2_enh_choice(int i);                /* the saved choice (cfg-backed: nearest to the setting); -1 custom value */
const char *ps2_enh_value_name(int i, char *buf, int n);   /* the saved value as the page shows it */
int  ps2_enh_running(int i);               /* the choice in effect in this run (-1 other / unknown) */
const char *ps2_enh_override(int i);       /* the environment value that holds the item in this run, NULL none */
int  ps2_enh_pending(int i);               /* 1: the saved value differs from this run's and needs a restart */
int  ps2_enh_pending_count(void);
int  ps2_enh_changed_mission(int i);       /* 1: changed this session, takes effect at the next mission */
int  ps2_enh_mission_count(void);          /* the page's rows (F4's too) that wait for the next mission */
int  ps2_enh_apply_now(int i);             /* PS2_ENH_*: the row's tier in this run: a combo row's from its members; a row
                                              that started off and needs what is built at start: PS2_ENH_RESTART */
/* zero_vr.c, GameControl::enter, the game thread: publishes the rows changed since (ps2_menv.h); their count, 0 = no-op */
int  ps2_enh_mission_edge(void);
/* zero_vr.c, the mission (re)start at a new attempt (the pause menu's RETRY, MISSION FAILED's RESTART): the same edge, but a row
   of ps2_enh_retry_waits keeps waiting, and a combo row with such a member waiting keeps waiting whole */
int  ps2_enh_retry_edge(void);
const char *ps2_enh_retry_waits(int i);    /* why row i takes effect only when the game enters a mission (a combo row: a member's reason), else NULL */
/* Save a choice and apply it at once where the item allows; returns PS2_ENH_LIVE (in effect now, also a row back at the
   value in effect), PS2_ENH_MISSION, PS2_ENH_RESTART (saved, after a restart), PS2_ENH_ACTION (an action ran) or -1 (held
   by the environment).
   Renderer thread (the page, F4); writes the settings file at once. */
int  ps2_enh_set(int i, int choice);
void ps2_enh_reset_all(void);              /* every item and all of ps2_cfg back to the defaults (saved; live ones applied);
                                              the only reset (the page's RESET ALL TO DEFAULTS, F4 > Quick) */
void ps2_enh_touch_rows(int on);           /* ps2_touch.c: a Touch controller's pose was valid: CONTROLS shows its rows */
int  ps2_enh_applied(void);                /* 1: ACZ_MENU_SETTINGS=1, the saved values were applied at start */
/* QUALITY PRESET (perf6_presets): ENHANCEMENTS > QUALITY's first row writes the rows of enh_preset_tab
   (ps2_settings.c); it shows the preset the saved rows match, else CUSTOM. */
enum { PS2_QP_LOW, PS2_QP_MEDIUM, PS2_QP_HIGH, PS2_QP_N };
/* an item's help line; the QUALITY PRESET's names the GPU, the suggestion and the headset's rate (long_form: F4's
   tooltip, also every row a preset sets) */
const char *ps2_enh_help(int i, char *buf, int n, int long_form);
/* "Your GPU ran behind the headset in N of the last 30 s ..." while ACZ_PERF_HINT says so, else "" */
const char *ps2_enh_hint(char *buf, int n);
/* "VAR=value" of what holds an item in this run (ACZ_QUALITY_PRESET=low for the preset's rows under it), NULL none */
const char *ps2_enh_held_by(int i, char *buf, int n);
/* ps2_video.c, once the device is chosen: its name, discrete or not, its largest device-local heap in GB */
void ps2_enh_set_gpu(const char *name, int discrete, double vram_gb);
/* zero_vr.c: the LOD bias and terrain detail ring are game constants it can rewrite at run time */
void ps2_enh_set_detail_fn(int (*fn)(float lod_bias, float terrain_detail));
/* the frame-time governor (ps2_video.c, lever "tdetail"): the terrain detail ring in effect (1 = the PS2's), and the
   ring set to 1 + (that - 1) x k now (the LOD bias kept; only when k changes; renderer thread); 0, or -1 when the live
   path is not there */
float ps2_enh_tdetail_now(void);
int  ps2_enh_gov_tdetail(float k);

/* the launcher (ps2_launcher.cpp, before the engine boots; docs/SETTINGS.md layer 1) */
int  ps2_enh_launcher_wanted(void);        /* ACZ_LAUNCHER over the file's `launcher` (default 1) */
int  ps2_enh_launcher_show_get(void);      /* the file's `launcher` */
void ps2_enh_launcher_show_set(int on);    /* ... the checkbox, F4 > Quick (saved by the launcher's PLAY or the caller) */
void ps2_enh_launcher_begin(void);         /* ps2_cfg from the file, without the log lines */
void ps2_enh_launcher_gpu(const char *name, int discrete, double vram_gb);
const char *ps2_enh_launcher_gpu_name(char *buf, int n);
const char *ps2_enh_launcher_gpu_line(char *buf, int n);
const char *ps2_enh_launcher_blocked(int i);   /* why no choice would work in this run though nothing holds it, NULL none */
int  ps2_enh_launcher_choose(int i, int choice);   /* in memory: 1 changed, 0 the same, -1 held */
int  ps2_enh_launcher_changed(int i);      /* 1: it waited for this start (the page changed it last time) */
void ps2_enh_launcher_changed_others(char *buf, int n);
int  ps2_enh_launcher_commit(void);        /* PLAY: the choices into the environment and the run's state; the switches changed */

const char *ps2_action_name(int act);
const char *ps2_key_name(int scancode);
const char *ps2_padbind_name(int code);

void ps2_settings_apply_game_patches(void);

float ps2_pad_axis_value(int raw, int positive);
void  ps2_pad_stick(float deadzone, float axis_scale, int invert, const float v[4],
                    unsigned char *x, unsigned char *y);
int   ps2_pad_trigger(float button_deadzone, float value, unsigned char *pressure);
int   ps2_pad_button(float button_deadzone, float value);

#ifdef __cplusplus
}
#endif

#endif
