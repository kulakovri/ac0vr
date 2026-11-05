#define IMGUI_DEFINE_MATH_OPERATORS
#include "ps2_ui.h"
#include "ps2_settings.h"
#include "ps2_hotas.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void ps2_log(const char *fmt, ...);
extern "C" const char *ps2_quit_overlay_text(void);
extern "C" const char *ps2_quit_enh_text(int *sel);
extern "C" int ps2_xr_session_running(void);

namespace {

bool g_init, g_visible;
SDL_Window *g_window;
float g_max_aniso;
float g_dpi = 1.0f;
VkFormat g_color_format;

enum { CAP_NONE, CAP_KEY, CAP_PAD, CAP_JOY };
int g_cap = CAP_NONE, g_cap_act, g_cap_slot;
bool g_cap_armed;
Uint64 g_cap_start;
Sint16 g_cap_base[8][SDL_GAMEPAD_AXIS_COUNT];      /* per connected pad (all of them are read, ctl_priority) */
SDL_JoystickID g_cap_ids[8];
int g_cap_npads;

enum { TAB_NONE = -1, TAB_KEYBOARD = 4, TAB_CONTROLLER = 5, TAB_ANALOG = 6 };
int g_want_tab = TAB_NONE;

const ImVec4 ACCENT(0.40f, 0.86f, 0.50f, 1.0f);
const ImVec4 WARN(1.00f, 0.76f, 0.32f, 1.0f);

void check_vk(VkResult r) {
    if (r < 0) ps2_log("ui: Vulkan call failed (%d)", (int)r);
}

/* PS2_UI_TEST="expert,tall,scroll=N" [test]: for window dumps (PS2_XR_DUMP_SCREEN) of what a mouse would reach: Expert (ini)
   open, the menu as tall as the window, the tab scrolled to N pixels */
const char *ui_test_env() { static const char *e = getenv("PS2_UI_TEST"); return e ? e : ""; }
bool ui_test_has(const char *w) { return std::strstr(ui_test_env(), w) != nullptr; }
int ui_test_scroll() {
    static int v = -2;
    if (v == -2) { const char *p = std::strstr(ui_test_env(), "scroll="); v = p ? atoi(p + 7) : -1; }
    return v;
}

void changed(int dirty) { ps2_settings_touch(dirty | PS2_CFG_SAVE); }

void gfx_changed(int dirty) {
    ps2_cfg.preset = ps2_settings_preset_matches(&ps2_cfg);
    changed(dirty);
}

void help(const char *text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void env_note(const char *var, const char *what) {
    const char *v = getenv(var);
    if (v && *v) ImGui::TextColored(WARN, "%s=%s is set and %s.", var, v, what);
}

bool slider_pct(const char *label, float *v, float lo, float hi) {
    float pct = *v * 100.0f;
    if (ImGui::SliderFloat(label, &pct, lo * 100.0f, hi * 100.0f, "%.1f%%")) {
        *v = pct / 100.0f;
        return true;
    }
    return false;
}

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 5.0f;
    s.TabRounding = 5.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding = ImVec2(16.0f, 14.0f);
    s.FramePadding = ImVec2(9.0f, 5.0f);
    s.ItemSpacing = ImVec2(10.0f, 8.0f);
    s.WindowBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 2.0f;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.07f, 0.09f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.08f, 0.10f, 0.11f, 0.98f);
    c[ImGuiCol_Border]           = ImVec4(0.30f, 0.55f, 0.36f, 0.45f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.13f, 0.17f, 0.16f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.19f, 0.27f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.22f, 0.34f, 0.26f, 1.00f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.07f, 0.12f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.10f, 0.22f, 0.14f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(0.16f, 0.26f, 0.19f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.24f, 0.42f, 0.29f, 1.00f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.30f, 0.55f, 0.36f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.16f, 0.30f, 0.20f, 1.00f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.22f, 0.40f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.28f, 0.50f, 0.33f, 1.00f);
    c[ImGuiCol_Tab]              = ImVec4(0.11f, 0.18f, 0.13f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.24f, 0.44f, 0.30f, 1.00f);
    c[ImGuiCol_TabSelected]      = ImVec4(0.19f, 0.36f, 0.24f, 1.00f);
    c[ImGuiCol_CheckMark]        = ACCENT;
    c[ImGuiCol_SliderGrab]       = ImVec4(0.36f, 0.70f, 0.44f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ACCENT;
    c[ImGuiCol_SeparatorHovered] = ACCENT;
    c[ImGuiCol_TextSelectedBg]   = ImVec4(0.30f, 0.55f, 0.36f, 0.45f);
    c[ImGuiCol_NavCursor]        = ACCENT;
    c[ImGuiCol_PlotHistogram]    = ImVec4(0.36f, 0.70f, 0.44f, 1.00f);
}

void bind_key(int act, int slot, int sc) {
    if (sc > 0)
        for (int a = 0; a < PS2_ACT_COUNT; a++)
            for (int k = 0; k < PS2_BIND_SLOTS; k++)
                if (ps2_cfg.key[a][k] == sc) ps2_cfg.key[a][k] = 0;
    ps2_cfg.key[act][slot] = sc;
    changed(0);
}

void bind_pad(int act, int slot, int code) {
    if (code > 0)
        for (int a = 0; a < PS2_ACT_COUNT; a++)
            for (int k = 0; k < PS2_BIND_SLOTS; k++)
                if (ps2_cfg.pad[a][k] == code) ps2_cfg.pad[a][k] = 0;
    ps2_cfg.pad[act][slot] = code;
    changed(0);
}

void start_capture(int kind, int act, int slot) {
    g_cap = kind;
    g_cap_act = act;
    g_cap_slot = slot;
    g_cap_armed = false;
    g_cap_start = SDL_GetTicks();
}

void end_capture() { g_cap = CAP_NONE; }

void update_capture_arming() {
    if (g_cap == CAP_JOY && !ps2_hotas_capture_active()) { end_capture(); return; }   /* (ps2_hotas.c bound it) */
    if (g_cap == CAP_NONE || g_cap_armed) return;
    if (SDL_GetTicks() - g_cap_start < 150) return;
    int nk = 0;
    const bool *ks = SDL_GetKeyboardState(&nk);
    for (int i = 0; ks && i < nk; i++)
        if (ks[i]) return;
    memset(g_cap_base, 0, sizeof g_cap_base);
    g_cap_npads = 0;
    for (int i = 0; i < ps2_video_pad_count() && g_cap_npads < 8; i++) {
        SDL_Gamepad *g = ps2_video_pad_at(i);
        if (!g) continue;
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) return;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
            g_cap_base[g_cap_npads][a] = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)a);
        g_cap_ids[g_cap_npads++] = SDL_GetGamepadID(g);
    }
    g_cap_armed = true;
}

bool capture_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        if (e->key.repeat || !g_cap_armed) return true;
        SDL_Scancode sc = e->key.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) { if (g_cap == CAP_JOY) ps2_hotas_capture_cancel(); end_capture(); return true; }
        bool clear = sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE;
        if (g_cap == CAP_JOY) {
            if (!clear) return true;
            ps2_hotas_capture_clear();
            end_capture();
            return true;
        }
        if (g_cap == CAP_KEY) bind_key(g_cap_act, g_cap_slot, clear ? 0 : (int)sc);
        else if (clear) bind_pad(g_cap_act, g_cap_slot, 0);
        else return true;
        end_capture();
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (g_cap == CAP_PAD && g_cap_armed) {
            bind_pad(g_cap_act, g_cap_slot, PS2_PADBIND_BUTTON(e->gbutton.button));
            end_capture();
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (g_cap == CAP_PAD && g_cap_armed && e->gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) {
            int axis = e->gaxis.axis, k = 0;
            while (k < g_cap_npads && g_cap_ids[k] != e->gaxis.which) k++;
            int d = (int)e->gaxis.value - (k < g_cap_npads ? (int)g_cap_base[k][axis] : 0);
            bool trig = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                     || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            if (d > 16000 || d < -16000) {
                bind_pad(g_cap_act, g_cap_slot,
                         PS2_PADBIND_AXISDIR(axis, trig ? 1 : d > 0));
                end_capture();
            }
        }
        return true;
    default:
        return false;
    }
}

struct Group { const char *title; int acts[4]; };
const Group GROUPS[] = {
    { "Face buttons", { PS2_ACT_CROSS, PS2_ACT_CIRCLE, PS2_ACT_SQUARE, PS2_ACT_TRIANGLE } },
    { "D-pad",        { PS2_ACT_UP, PS2_ACT_DOWN, PS2_ACT_LEFT, PS2_ACT_RIGHT } },
    { "Shoulders",    { PS2_ACT_L1, PS2_ACT_R1, PS2_ACT_L2, PS2_ACT_R2 } },
    { "Start, Select and stick clicks", { PS2_ACT_START, PS2_ACT_SELECT, PS2_ACT_L3, PS2_ACT_R3 } },
    { "Left stick",   { PS2_ACT_LS_UP, PS2_ACT_LS_DOWN, PS2_ACT_LS_LEFT, PS2_ACT_LS_RIGHT } },
    { "Right stick",  { PS2_ACT_RS_UP, PS2_ACT_RS_DOWN, PS2_ACT_RS_LEFT, PS2_ACT_RS_RIGHT } },
};

void binding_table(bool pad) {
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
                             | ImGuiTableFlags_SizingStretchProp;
    bool fixed = pad && ps2_cfg.pad_layout == PS2_PAD_LAYOUT_MODERN;   /* the modern preset: shown, not editable */
    if (fixed) ImGui::BeginDisabled();
    for (const Group &grp : GROUPS) {
        ImGui::SeparatorText(grp.title);
        ImGui::PushID(grp.title);
        if (ImGui::BeginTable("binds", 3, tf)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            for (int act : grp.acts) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(ps2_action_name(act));
                for (int slot = 0; slot < PS2_BIND_SLOTS; slot++) {
                    ImGui::TableSetColumnIndex(1 + slot);
                    ImGui::PushID(act * 4 + slot);
                    int code = pad ? ps2_settings_pad_binds(act)[slot] : ps2_cfg.key[act][slot];
                    bool capturing = g_cap == (pad ? CAP_PAD : CAP_KEY)
                                  && g_cap_act == act && g_cap_slot == slot;
                    const char *label = capturing ? "..."
                                      : pad ? ps2_padbind_name(code) : ps2_key_name(code);
                    bool dim = !code && !capturing;
                    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f)))
                        start_capture(pad ? CAP_PAD : CAP_KEY, act, slot);
                    if (dim) ImGui::PopStyleColor();
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        if (pad) bind_pad(act, slot, 0);
                        else bind_key(act, slot, 0);
                    }
                    ImGui::SetItemTooltip("Click to rebind, right-click to clear.");
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    if (fixed) ImGui::EndDisabled();
}

void draw_capture_popup() {
    const char *id = "Rebind###capture";
    bool open = ImGui::IsPopupOpen(id);
    if (g_cap == CAP_NONE && !open) return;
    if (g_cap != CAP_NONE && !open) ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize
                                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;
    if (g_cap == CAP_NONE) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("%s  (%s)", g_cap == CAP_JOY ? ps2_hotas_capture_label() : ps2_action_name(g_cap_act),
                g_cap_slot ? "secondary" : "primary");
    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "%s", g_cap == CAP_JOY ? ps2_hotas_capture_prompt()
                       : g_cap == CAP_KEY
                       ? "Press a key..."
                       : "Press a button, or push a stick or trigger...");
    if (!g_cap_armed) ImGui::TextDisabled("Release all keys and buttons first.");
    ImGui::TextDisabled("Esc cancels, Backspace clears.");
    ImGui::Spacing();
    if (ImGui::Button("Clear binding")) {
        if (g_cap == CAP_JOY) ps2_hotas_capture_clear();
        else if (g_cap == CAP_KEY) bind_key(g_cap_act, g_cap_slot, 0);
        else bind_pad(g_cap_act, g_cap_slot, 0);
        end_capture();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) { if (g_cap == CAP_JOY) ps2_hotas_capture_cancel(); end_capture(); }
    if (g_cap == CAP_NONE) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

const char *const WINDOW_MODES[] = { "Windowed", "Borderless fullscreen", "Exclusive fullscreen" };

struct WinSize { int w, h; const char *tag; };
const WinSize WIN_SIZES[] = {
    { 640, 448, "1x, native" }, { 1280, 896, "2x" }, { 1920, 1344, "3x" }, { 2560, 1792, "4x" },
    { 1280, 720, "720p" }, { 1600, 900, "" }, { 1920, 1080, "1080p" },
    { 2560, 1440, "1440p" }, { 3840, 2160, "4K" },
};

void fs_changed() {
    changed(ps2_cfg.window_mode == PS2_WIN_EXCLUSIVE ? PS2_CFG_WINDOW : 0);
}

void resolution_controls() {
    if (ImGui::Combo("Window mode", &ps2_cfg.window_mode, WINDOW_MODES, 3)) {
        if (ps2_cfg.window_mode != PS2_WIN_WINDOWED) ps2_cfg.fullscreen_type = ps2_cfg.window_mode;
        changed(PS2_CFG_WINDOW);
    }
    help("Borderless covers the screen at the desktop resolution and switches instantly. "
         "Exclusive changes the display mode itself. F11 or Alt+Enter toggles fullscreen "
         "using whichever of the two you last picked.");

    if (ps2_cfg.window_mode == PS2_WIN_WINDOWED) {
        char preview[48];
        snprintf(preview, sizeof preview, "%d x %d", ps2_cfg.window_w, ps2_cfg.window_h);
        if (ImGui::BeginCombo("Window size", preview)) {
            for (const WinSize &s : WIN_SIZES) {
                char label[64];
                if (s.tag[0]) snprintf(label, sizeof label, "%d x %d  (%s)", s.w, s.h, s.tag);
                else snprintf(label, sizeof label, "%d x %d", s.w, s.h);
                bool sel = ps2_cfg.window_w == s.w && ps2_cfg.window_h == s.h;
                if (ImGui::Selectable(label, sel)) {
                    ps2_cfg.window_w = s.w;
                    ps2_cfg.window_h = s.h;
                    changed(PS2_CFG_WINDOW);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        help("The window's size in pixels. Dragging the window edge also works and is remembered.");
    } else if (ps2_cfg.window_mode == PS2_WIN_EXCLUSIVE) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
        char preview[64];
        if (ps2_cfg.fs_w <= 0) snprintf(preview, sizeof preview, "Desktop resolution");
        else snprintf(preview, sizeof preview, "%d x %d @ %.0f Hz", ps2_cfg.fs_w, ps2_cfg.fs_h,
                      (double)ps2_cfg.fs_hz);
        if (ImGui::BeginCombo("Fullscreen resolution", preview)) {
            if (ImGui::Selectable("Desktop resolution", ps2_cfg.fs_w <= 0)) {
                ps2_cfg.fs_w = ps2_cfg.fs_h = 0;
                ps2_cfg.fs_hz = 0.0f;
                fs_changed();
            }
            int n = 0;
            SDL_DisplayMode **modes = d ? SDL_GetFullscreenDisplayModes(d, &n) : nullptr;
            for (int i = 0; modes && i < n; i++) {
                const SDL_DisplayMode *m = modes[i];
                if (m->pixel_density != 1.0f) continue;
                char label[64];
                snprintf(label, sizeof label, "%d x %d @ %.0f Hz", m->w, m->h, (double)m->refresh_rate);
                bool sel = ps2_cfg.fs_w == m->w && ps2_cfg.fs_h == m->h
                        && std::fabs(ps2_cfg.fs_hz - m->refresh_rate) < 0.5f;
                ImGui::PushID(i);
                if (ImGui::Selectable(label, sel)) {
                    ps2_cfg.fs_w = m->w;
                    ps2_cfg.fs_h = m->h;
                    ps2_cfg.fs_hz = m->refresh_rate;
                    fs_changed();
                }
                ImGui::PopID();
            }
            SDL_free(modes);
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("Borderless fullscreen always runs at the desktop resolution.");
    }
}

const char *const ASPECTS[] = {
    "Stretch to fill the window", "Auto (4:3, or 16:9 with widescreen)", "4:3", "16:9", "Custom",
};

void aspect_controls() {
    if (ImGui::Combo("Aspect ratio", &ps2_cfg.aspect, ASPECTS, 5)) changed(0);
    help("The shape of the picture inside the window. Auto keeps the PS2's 4:3 picture "
         "undistorted and becomes 16:9 when the widescreen patch is on. Stretch fills the "
         "window whatever its shape, which is how the runtime always behaved.");
    env_note("PS2_ASPECT", "overrides the aspect ratio");
    if (ps2_cfg.aspect == PS2_ASPECT_CUSTOM
        && ImGui::SliderFloat("Custom ratio", &ps2_cfg.aspect_custom, 1.0f, 3.0f, "%.3f : 1"))
        changed(0);
}

void widescreen_control() {
    bool ws = ps2_cfg.widescreen != 0;
    if (ImGui::Checkbox("Widescreen (16:9)", &ws)) {
        ps2_cfg.widescreen = ws;
        if (ws && ps2_cfg.aspect == PS2_ASPECT_4_3) ps2_cfg.aspect = PS2_ASPECT_AUTO;
        changed(0);
    }
    help("Applies the 16:9 patch from PCSX2's patch database (by nemesis2000) to the game's "
         "own camera, so the 3D view really is wider instead of stretched. Use Auto or 16:9 "
         "aspect with it.");
    if (ws) {
        static const char *const HUD_LAYOUTS[] = { "Stretched with the picture",
                                                   "4:3 in the centre" };
        if (ImGui::Combo("HUD layout", &ps2_cfg.hud_layout, HUD_LAYOUTS, 2)) changed(0);
        help("The game's HUD is laid out for 4:3. Centred draws the screen-fixed parts -- "
             "tapes, readouts, radar, captions -- at their own proportions in the middle of "
             "the screen; markers placed on things in the world stay on them. Needs the "
             "native 2D layer (on unless PS2_RN_2D=0).");
    }
    if (ws && ps2_cfg.aspect == PS2_ASPECT_STRETCH && g_window) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
        if (h > 0 && std::fabs((float)w / (float)h - 16.0f / 9.0f) > 0.05f)
            ImGui::TextColored(WARN, "This window is not 16:9, so Stretch distorts the picture. Pick Auto.");
    }
}

const char *const PRESENT_MODES[] = {
    "Mailbox - low latency, no tearing", "VSync (FIFO)",
    "Immediate - tearing, lowest latency", "Adaptive VSync (FIFO relaxed)",
};

void present_controls() {
    if (ImGui::Combo("Presentation", &ps2_cfg.present_mode, PRESENT_MODES, 4))
        changed(PS2_CFG_SWAPCHAIN);
    help("The game always runs at the PS2's 59.94 fields per second; this only decides how "
         "finished frames reach the screen. A mode your display does not offer falls back "
         "to VSync.");
    env_note("PS2_PRESENT_MODE", "overrides the presentation mode");
    bool lim = ps2_cfg.fps_limit > 0;
    if (ImGui::Checkbox("Limit frame rate", &lim)) {
        ps2_cfg.fps_limit = lim ? 60 : 0;
        changed(PS2_CFG_FPS);
    }
    if (lim) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::SliderInt("##fpslimit", &ps2_cfg.fps_limit, 20, 240, "%d FPS"))
            changed(PS2_CFG_FPS);
    }
    help("Shows fewer frames. Below 60, frames are skipped evenly -- every other one at 30 -- "
         "while the game keeps running at the PS2's 59.94 fields a second underneath. It saves "
         "only the final presentation, so it cannot make a slow machine run the game faster; "
         "leave it off unless you want an even, lower frame rate.");
    env_note("PS2_FPS_CAP", "overrides the frame-rate limit");
}

void internal_res_status() {
    ps2_render_info ri;
    ps2_video_render_info(&ri);
    ImGui::TextDisabled("Drawing at %u x %u (%ux the PS2's %u x %u), about %.0f MB of video memory.",
                        ri.guest_w * ri.scale, ri.guest_h * ri.scale, ri.scale,
                        ri.guest_w, ri.guest_h,
                        (double)(ri.bytes_at_1x * ri.scale * ri.scale) / (1024.0 * 1024.0));
    if (ri.failed_scale)
        ImGui::TextColored(WARN, "%ux did not fit in video memory, so it stays at %ux.",
                           ri.failed_scale, ri.scale);
}

void internal_res_control() {
    ps2_render_info ri;
    ps2_video_render_info(&ri);
    static const int VALUES[] = { 0, 1, 2, 3, 4, 5, 6, 8 };
    auto label = [&ri](int v, char *buf, size_t n) {
        double mb = (double)(ri.bytes_at_1x * (uint64_t)(v * v)) / (1024.0 * 1024.0);
        if (v <= 0)
            snprintf(buf, n, "Auto - cover the window (%ux now)",
                     ri.auto_scale < ri.max_scale ? ri.auto_scale : ri.max_scale);
        else if (v == 1)
            snprintf(buf, n, "Native - %u x %u", ri.guest_w, ri.guest_h);
        else
            snprintf(buf, n, "%dx - %u x %u  (about %.0f MB)", v, ri.guest_w * v, ri.guest_h * v, mb);
    };
    char preview[96];
    label(ps2_cfg.internal_res, preview, sizeof preview);
    if (ImGui::BeginCombo("Internal resolution", preview)) {
        for (int v : VALUES) {
            char buf[96];
            label(v, buf, sizeof buf);
            bool too_big = v > (int)ri.max_scale;
            if (too_big) ImGui::BeginDisabled();
            if (ImGui::Selectable(buf, ps2_cfg.internal_res == v)) {
                ps2_cfg.internal_res = v;
                gfx_changed(0);
            }
            if (too_big) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    help("How many pixels the game is drawn with before it is scaled to the window. Native is "
         "the PS2's own 640x448. 2x and up draw the same frame with that many times the pixels "
         "in each direction, so polygon edges, HUD lines and the effects the game renders into "
         "its own buffers come out sharp; the textures themselves keep the game's resolution. "
         "Above the window's size the extra pixels are averaged down, which anti-aliases. Video "
         "memory and GPU time grow with the square of the factor. Auto picks the smallest factor "
         "that covers the window.");
    env_note("PS2_UPSCALE", "overrides the internal resolution");
    internal_res_status();
}

/* RESET ALL asks first wherever the mouse reaches it, F4 > Quick and F4 > Enhancements: it returns the bindings too (the owner, 2026-11-05) */
void reset_all_popup() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Reset all settings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Enhancements, graphics, display, bindings and dead zones all return to their defaults.");
        if (ImGui::Button("Reset", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) {
            ps2_enh_reset_all();                 /* the page's RESET ALL TO DEFAULTS: the only reset */
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* one row of the ENHANCEMENTS table: a combo box or a button, its tooltip, what holds it (ps2_settings.c ps2_enh_*) */
void enh_row(int i) {
    char buf[64], h[1400], hp[1200], hb[96];
    const ps2_enh_item *it = ps2_enh_item_at(i);
    if (!it) return;
    ImGui::PushID(i);
    const char *ovr = ps2_enh_override(i);
    bool lock = ovr && strcmp(it->key, "pad_layout") != 0;    /* a layout chosen here replaces ACZ_PAD_LAYOUT */
    if (it->apply == PS2_ENH_ACTION) {
        const bool is_reset = !strcmp(it->key, "reset_all");
        if (ImGui::Button(it->label)) {
            if (is_reset) ImGui::OpenPopup("Reset all settings?");
            else ps2_enh_set(i, 0);
        }
        if (is_reset) {
            static bool test_asked;              /* (PS2_UI_TEST reset_ask, as a mouse would) */
            if (ui_test_has("reset_ask") && !test_asked) { test_asked = true; ImGui::OpenPopup("Reset all settings?"); }
            reset_all_popup();
        }
    } else {
        int cur = ps2_enh_choice(i);
        ps2_enh_value_name(i, buf, sizeof buf);
        if (lock) ImGui::BeginDisabled();
        if (ImGui::BeginCombo(it->label, buf)) {
            for (int k = 0; k < it->n; k++)
                if (ImGui::Selectable(it->name[k], k == cur)) ps2_enh_set(i, k);
            ImGui::EndCombo();
        }
        if (lock) ImGui::EndDisabled();
    }
    snprintf(h, sizeof h, "%s%s", ps2_enh_help(i, hp, sizeof hp, 1),
             ps2_enh_apply_now(i) == PS2_ENH_RESTART ? "\n\nTakes effect after a restart." :
             ps2_enh_apply_now(i) != PS2_ENH_MISSION ? "" : ps2_enh_retry_waits(i) ? "\n\nTakes effect when the next mission starts."
             : "\n\nTakes effect at the next mission or RETRY.");
    help(h);
    if (ps2_enh_pending(i)) { ImGui::SameLine(); ImGui::TextColored(WARN, "(restart)"); }
    else if (ps2_enh_changed_mission(i)) { ImGui::SameLine(); ImGui::TextColored(WARN, "(next mission)"); }
    if (ovr && ps2_enh_held_by(i, hb, sizeof hb)) { ImGui::SameLine(); ImGui::TextDisabled("%s", hb); }
    ImGui::PopID();
}

void tab_quick() {
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::SeparatorText("Quality preset");
    {   /* the page's QUALITY PRESET row: the one preset (its help line names the GPU and its suggestion) */
        int q = ps2_enh_find("quality_preset");
        const ps2_enh_item *it = ps2_enh_item_at(q);
        if (it) {
            char hp[1200];
            float w = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * (float)(it->n - 1)) / (float)it->n;
            float h = ImGui::GetFrameHeight() * 2.2f;
            int cur = ps2_enh_choice(q);
            bool lock = ps2_enh_override(q) != nullptr;
            if (lock) ImGui::BeginDisabled();
            for (int k = 0; k < it->n; k++) {
                bool sel = cur == k;
                if (sel) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.52f, 0.32f, 1.0f));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 0.38f, 1.0f));
                }
                if (ImGui::Button(it->name[k], ImVec2(w, h))) ps2_enh_set(q, k);
                if (sel) ImGui::PopStyleColor(2);
                if (k + 1 < it->n) ImGui::SameLine();
            }
            if (lock) ImGui::EndDisabled();
            if (cur < 0) ImGui::TextWrapped("Custom: one of its rows was changed by hand.");
            ImGui::TextWrapped("%s", ps2_enh_help(q, hp, sizeof hp, 0));
            ps2_enh_help(q, hp, sizeof hp, 1);
            help(hp);
        }
    }
    internal_res_status();
    {   /* the launcher's "do not show again" comes back here (ACZ_LAUNCHER in the environment holds it) */
        const char *e = getenv("ACZ_LAUNCHER");
        bool show = ps2_enh_launcher_show_get() != 0, held = e && *e;
        if (held) ImGui::BeginDisabled();
        if (ImGui::Checkbox("Launcher before the game", &show)) { ps2_enh_launcher_show_set(show); changed(0); }
        if (held) { ImGui::EndDisabled(); ImGui::SameLine(); ImGui::TextDisabled("(set by ACZ_LAUNCHER=%s)", e); }
        help("At the next start: the quality settings, the GPU's suggestion, the active controller and PLAY, in this window, before "
             "the game boots.");
    }

    ImGui::SeparatorText("Display");
    resolution_controls();
    aspect_controls();
    widescreen_control();
    present_controls();

    ImGui::SeparatorText("Controls");
    if (ImGui::Button("Keyboard bindings")) g_want_tab = TAB_KEYBOARD;
    ImGui::SameLine();
    if (ImGui::Button("Controller bindings")) g_want_tab = TAB_CONTROLLER;
    ImGui::SameLine();
    if (ImGui::Button("Dead zones")) g_want_tab = TAB_ANALOG;

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Reset all settings to defaults")) ImGui::OpenPopup("Reset all settings?");
    reset_all_popup();
}

void tab_display() {
    ImGui::SeparatorText("Window");
    resolution_controls();

    ImGui::SeparatorText("Picture");
    aspect_controls();
    widescreen_control();
    bool is = ps2_cfg.integer_scale != 0;
    if (ImGui::Checkbox("Integer scaling", &is)) { ps2_cfg.integer_scale = is; changed(0); }
    help("Scale only by whole multiples of the game's 448 lines, leaving a border instead of "
         "resampling unevenly. Combine with Nearest for exact pixels.");
    static const char *const SCALE[] = {
        "Nearest - blocky, exact pixels", "Bilinear - smooth", "Sharp bilinear - crisp, even pixels",
    };
    if (ImGui::Combo("Scaling filter", &ps2_cfg.scale_filter, SCALE, 3)) gfx_changed(0);
    help("How the rendered picture is resized to the window. Sharp bilinear keeps pixels crisp "
         "at any size without the uneven pixel widths of Nearest. A picture drawn larger than "
         "the window is averaged down with any filter except Nearest.");

    ImGui::SeparatorText("Presentation");
    present_controls();

    ImGui::SeparatorText("Interface");
    bool fps = ps2_cfg.show_fps != 0;
    if (ImGui::Checkbox("Show FPS counter", &fps)) { ps2_cfg.show_fps = fps; changed(0); }
    if (ImGui::SliderFloat("Menu text size", &ps2_cfg.ui_scale, 0.75f, 2.0f, "%.2fx")) changed(0);
}

void tab_rendering() {
    ImGui::SeparatorText("Resolution");
    /* With PS2_UPSCALE set (every launcher start: the RESOLUTION row always sets it; a test run's own) internal_resolution
       is not read (ps2_video.c choose_scale, zero_fmv.c pick_scale): the page's row here, the old control under Expert. */
    if (getenv("PS2_UPSCALE")) {
        enh_row(ps2_enh_find("resolution"));
        internal_res_status();
    } else internal_res_control();

    ImGui::SeparatorText("Textures");
    static const char *const TF[] = {
        "As the game requests (accurate)", "Force nearest (pixelated)", "Force bilinear (smooth)",
    };
    if (ImGui::Combo("Texture filtering", &ps2_cfg.tex_filter, TF, 3)) gfx_changed(0);
    help("The PS2 picks nearest or bilinear filtering per draw. Forcing one applies to the 3D "
         "world's textures. The HUD, menus and text keep what the game asked for: they are 2D "
         "art laid out texel by texel, and smoothing them pulls in the neighbouring pictures "
         "(lines across the minimap). So do effects such as the clouds.");

    static const int AF_VALUES[] = { 0, 2, 4, 8, 16 };
    static const char *const AF[] = { "Off", "2x", "4x", "8x", "16x" };
    int idx = 0;
    for (int i = 0; i < 5; i++)
        if (AF_VALUES[i] == ps2_cfg.anisotropy) idx = i;
    bool unsupported = g_max_aniso < 2.0f;
    if (unsupported) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("Anisotropic filtering", AF[idx])) {
        for (int i = 0; i < 5; i++) {
            if (AF_VALUES[i] > 0 && (float)AF_VALUES[i] > g_max_aniso) continue;
            if (ImGui::Selectable(AF[i], i == idx)) {
                ps2_cfg.anisotropy = AF_VALUES[i];
                gfx_changed(PS2_CFG_SAMPLER);
            }
        }
        ImGui::EndCombo();
    }
    if (unsupported) ImGui::EndDisabled();
    help(unsupported ? "This GPU does not support anisotropic filtering."
                     : "Sharpens textures seen at a steep angle -- mostly the ground running "
                       "towards the horizon. Costs a little GPU time.");

    env_note("PS2_NO_MIP", "turns the game's mipmaps off, which garbles distant terrain");

    ImGui::SeparatorText("Frames");
    bool di = ps2_cfg.deinterlace != 0;
    if (ImGui::Checkbox("Deinterlace", &di)) { ps2_cfg.deinterlace = di; gfx_changed(0); }
    help("The game draws alternate lines each field, as an interlaced TV expects. On weaves "
         "the two back into one steady frame; off shows the raw fields, which bob by a line.");
}

void tab_post() {
    ImGui::SeparatorText("Anti-aliasing and sharpness");
    bool fx = ps2_cfg.fxaa != 0;
    if (ImGui::Checkbox("FXAA", &fx)) { ps2_cfg.fxaa = fx; gfx_changed(0); }
    help("Fast approximate anti-aliasing on the final picture: softens jagged edges on "
         "aircraft and the horizon, and can soften fine HUD text a little.");
    if (ImGui::SliderFloat("Sharpening", &ps2_cfg.sharpen, 0.0f, 1.0f, "%.2f")) gfx_changed(0);
    help("Contrast-limited sharpening of the final picture. Counteracts the softness of "
         "scaling 640x448 up to a large window.");

    ImGui::SeparatorText("Colour");
    bool c = false;
    c |= ImGui::SliderFloat("Brightness", &ps2_cfg.brightness, -0.5f, 0.5f, "%+.2f");
    c |= ImGui::SliderFloat("Contrast", &ps2_cfg.contrast, 0.5f, 1.5f, "%.2f");
    c |= ImGui::SliderFloat("Gamma", &ps2_cfg.gamma, 0.5f, 2.0f, "%.2f");
    c |= ImGui::SliderFloat("Saturation", &ps2_cfg.saturation, 0.0f, 2.0f, "%.2f");
    if (c) changed(0);
    if (ImGui::Button("Reset colour")) {
        ps2_cfg.brightness = 0.0f;
        ps2_cfg.contrast = 1.0f;
        ps2_cfg.gamma = 1.0f;
        ps2_cfg.saturation = 1.0f;
        changed(0);
    }
}

void tab_keyboard() {
    ImGui::TextWrapped("Click a binding, then press the key you want. Esc cancels; Backspace or a "
                       "right-click clears it. Each key drives one action, so binding a key "
                       "again moves it here.");
    if (ImGui::Button("Reset keyboard to defaults")) {
        ps2_settings_default_bindings(&ps2_cfg, 1, 0);
        changed(0);
    }
    binding_table(false);
}

/* F4 > Controller > Joysticks (ctl_hotas): only with a joystick connected */
void hotas_section() {
    int n = ps2_hotas_count();
    if (!n || !ImGui::CollapsingHeader("Joysticks and throttles", ImGuiTreeNodeFlags_DefaultOpen)) return;
    ImGui::TextWrapped("A joystick flies beside the pad; its profile is its own layout, saved by device in the settings "
                       "file (not by port). Axis order: the settings here, then the dead zones of the Analog tab.");
    for (int i = 0; i < n; i++) {
        ps2_hotas_info in;
        if (!ps2_hotas_info_at(i, &in)) break;
        ImGui::PushID(i);
        ImGui::SeparatorText(in.name);
        ImGui::TextDisabled("%s, %04x:%04x, GUID %s", in.type, in.vid, in.pid, in.guid);
        ImGui::TextDisabled("Profile: %s%s%s", in.profile, in.status[0] ? ", " : "", in.status);
        if (ImGui::BeginTable("axes", 6, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
            ImGui::TableSetupColumn("Axis"); ImGui::TableSetupColumn("Invert"); ImGui::TableSetupColumn("Slider");
            ImGui::TableSetupColumn("Dead zone"); ImGui::TableSetupColumn("Curve"); ImGui::TableSetupColumn("Neutral");
            ImGui::TableHeadersRow();
            for (int a = 0; a < in.naxes && a < 16; a++) {
                int inv, sl; float dz, cv, nt;
                ps2_hotas_axis_get(i, a, &inv, &dz, &cv, &sl, &nt);
                bool binv = inv != 0, bsl = sl != 0, ch = false;
                char lb[16];
                ImGui::PushID(a);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                snprintf(lb, sizeof lb, "%d", a);
                ImGui::ProgressBar((ps2_hotas_axis_now(i, a) + 1.0f) * 0.5f, ImVec2(-FLT_MIN, 0.0f), lb);
                ImGui::TableSetColumnIndex(1); ch |= ImGui::Checkbox("##inv", &binv);
                ImGui::TableSetColumnIndex(2); ch |= ImGui::Checkbox("##sl", &bsl);
                ImGui::TableSetColumnIndex(3); ImGui::SetNextItemWidth(-FLT_MIN); ch |= ImGui::SliderFloat("##dz", &dz, 0.0f, 0.5f, "%.2f");
                ImGui::TableSetColumnIndex(4); ImGui::SetNextItemWidth(-FLT_MIN); ch |= ImGui::SliderFloat("##cv", &cv, 0.5f, 3.0f, "%.2f");
                ImGui::TableSetColumnIndex(5); ImGui::SetNextItemWidth(-FLT_MIN); ch |= ImGui::SliderFloat("##nt", &nt, 0.0f, 1.0f, "%.2f");
                if (ch) ps2_hotas_axis_set(i, a, binv, dz, cv, bsl, nt);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        char held[160]; int l = 0;
        held[0] = 0;
        for (int b = 0; b < in.nbuttons && l < 140; b++)
            if (ps2_hotas_button_now(i, b)) l += snprintf(held + l, sizeof held - (size_t)l, " %d", b);
        ImGui::Text("Buttons %d, hats %d. Pressed:%s", in.nbuttons, in.nhats, l ? held : " none");
        ImGui::PopID();
    }
    if (ImGui::Button("Set the lever's neutral from where it is now")) ps2_hotas_neutral_now();
    ImGui::SameLine();
    if (ImGui::Button("Copy the device report")) {
        static char rep[8192];
        ps2_hotas_report(rep, sizeof rep);
        SDL_SetClipboardText(rep);
    }
    help("The report (device lines and profiles) is what a tester mails back: docs/HOTAS.md.");
    ImGui::TextWrapped("Click a binding, then move the control. Levers and sticks need most of their travel. Esc "
                       "cancels; Backspace or a right-click clears it. No binding here: the pad's stays in force.");
    if (ImGui::BeginTable("joybinds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (int r = 0; r < ps2_hotas_rows(); r++) {
            ImGui::PushID(r);
            if (const char *g = ps2_hotas_row_group(r)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%s", g);
            }
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ps2_hotas_row_label(r));
            for (int k = 0; k < PS2_BIND_SLOTS; k++) {
                ImGui::TableSetColumnIndex(1 + k);
                ImGui::PushID(k);
                bool capturing = g_cap == CAP_JOY && g_cap_act == r && g_cap_slot == k;
                if (ImGui::Button(capturing ? "..." : ps2_hotas_binding_name(r, k), ImVec2(-FLT_MIN, 0.0f))) {
                    start_capture(CAP_JOY, r, k);
                    ps2_hotas_capture_begin(r, k);
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ps2_hotas_unbind(r, k);
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void tab_controller() {
    /* the controller registry (ctl_priority): every pad is read; the active one gives the button names and gets the
       vibration -- automatically the one used last, or the one chosen here (as ENHANCEMENTS > ACTIVE CONTROLLER) */
    SDL_Gamepad *cur = ps2_video_gamepad();
    int forced = ps2_video_pad_forced_kind(), touch = ps2_video_pad_touch_state();
    const char *act = ps2_video_pad_active_name();
    char preview[192];
    snprintf(preview, sizeof preview, "%s%s", forced ? "" : "Automatic: ", act ? act : "no controller connected");
    if (ImGui::BeginCombo("Controller", preview)) {
        if (ImGui::Selectable("Automatic (the one used last)", forced == 0)) ps2_video_pad_force("");
        if (touch && ImGui::Selectable("Quest Touch", forced == 1)) ps2_video_pad_force("touch");
        for (int i = 0; i < ps2_video_pad_count(); i++) {
            SDL_Gamepad *g = ps2_video_pad_at(i);
            const char *name = g ? SDL_GetGamepadName(g) : NULL;
            char label[192];
            snprintf(label, sizeof label, "%s##%u", name ? name : "Controller", g ? (unsigned)SDL_GetGamepadID(g) : 0u);
            bool sel = forced >= 2 && cur == g && touch != 2;
            if (g && ImGui::Selectable(label, sel)) ps2_video_select_gamepad(SDL_GetGamepadID(g));
        }
        ImGui::EndCombo();
    }
    help("All connected controllers fly the aircraft. The active one shows its button names in the menus and gets the "
         "vibration: automatically the one you used last, or the one chosen here. Also ENHANCEMENTS > CONTROLS > "
         "ACTIVE CONTROLLER.");
    env_note("ACZ_PAD_ACTIVE", "chose the active controller at start");
    hotas_section();
    bool block = ps2_cfg.block_input_in_menu != 0;
    if (ImGui::Checkbox("Hold the game's controls while this menu is open", &block)) {
        ps2_cfg.block_input_in_menu = block;
        changed(0);
    }
    static const char *const LAYOUTS[] = {
        "Classic - PS2 positions, your bindings below", "Modern - throttle on the triggers",
    };
    int layout = ps2_cfg.pad_layout;
    if (ImGui::Combo("Layout", &layout, LAYOUTS, 2)) {
        ps2_settings_set_pad_layout(layout);
        changed(0);
    }
    help("Classic: the bindings below, by default where the PS2 had them (LB/RB throttle, LT/RT yaw). "
         "Modern: RT accelerates and LT slows down as far as you pull them (the PS2's "
         "pressure-sensitive R1/L1), RB/LB yaw, both together the autopilot; A, B, X, Y, View, Menu, the sticks "
         "and their clicks as in Classic. Your own bindings are kept while Modern is in use.");
    env_note("ACZ_PAD_LAYOUT", "chose the layout at start (a choice here replaces it)");
    if (ps2_cfg.pad_layout == PS2_PAD_LAYOUT_MODERN) {
        ImGui::TextWrapped("The modern layout is fixed. Choose Classic to edit your own bindings, or start "
                           "them from this layout:");
        if (ImGui::Button("Copy the modern layout into my bindings")) {
            ps2_settings_layout_bindings(PS2_PAD_LAYOUT_MODERN, ps2_cfg.pad);
            ps2_settings_set_pad_layout(PS2_PAD_LAYOUT_CLASSIC);
            changed(0);
        }
        help("Replaces your classic bindings with the modern ones and switches to Classic, where they can be "
             "changed. The triggers stay analog throttle.");
    } else {
        ImGui::TextWrapped("Click a binding, then press a button, or push a stick or trigger in the "
                           "direction you want. Esc cancels; Backspace or a right-click clears it.");
        if (ImGui::Button("Reset controller to defaults")) {
            ps2_settings_default_bindings(&ps2_cfg, 0, 1);
            changed(0);
        }
    }
    binding_table(true);
}

void stick_view(int s, float size) {
    SDL_Gamepad *g = ps2_video_gamepad();
    int ax = 0, ay = 0;
    if (g) {
        ax = SDL_GetGamepadAxis(g, s ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX);
        ay = SDL_GetGamepadAxis(g, s ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY);
    }
    const float v[4] = { ps2_pad_axis_value(ay, 0), ps2_pad_axis_value(ay, 1),
                         ps2_pad_axis_value(ax, 0), ps2_pad_axis_value(ax, 1) };
    unsigned char bx, by;
    ps2_pad_stick(ps2_cfg.deadzone, ps2_cfg.axis_scale, ps2_cfg.invert[s], v, &bx, &by);
    float rx = v[3] - v[2], ry = v[1] - v[0];
    float ox = bx < 127 ? -(127 - bx) / 127.0f : (bx - 127) / 128.0f;
    float oy = by < 127 ? -(127 - by) / 127.0f : (by - 127) / 128.0f;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = size * 0.5f - 4.0f;
    ImVec2 ctr = p + ImVec2(size * 0.5f, size * 0.5f);
    dl->AddRectFilled(p, p + ImVec2(size, size), IM_COL32(18, 24, 22, 255), 6.0f);
    float scale = ps2_cfg.axis_scale > 0.01f ? ps2_cfg.axis_scale : 0.01f;
    float dzr = ps2_cfg.deadzone / scale;
    if (dzr > 0.0f)
        dl->AddCircleFilled(ctr, r * (dzr < 1.0f ? dzr : 1.0f), IM_COL32(190, 70, 70, 80), 48);
    dl->AddLine(ctr - ImVec2(r, 0.0f), ctr + ImVec2(r, 0.0f), IM_COL32(60, 72, 66, 255));
    dl->AddLine(ctr - ImVec2(0.0f, r), ctr + ImVec2(0.0f, r), IM_COL32(60, 72, 66, 255));
    dl->AddCircle(ctr, r, IM_COL32(130, 150, 140, 255), 64, 1.5f);
    if (scale > 1.0f) dl->AddCircle(ctr, r / scale, IM_COL32(100, 120, 110, 200), 64, 1.0f);
    dl->AddCircleFilled(ctr + ImVec2(rx * r, ry * r), 4.0f, IM_COL32(210, 210, 210, 220));
    dl->AddCircleFilled(ctr + ImVec2(ox * r, oy * r), 5.5f, IM_COL32(100, 235, 130, 255));
    ImGui::Text("X %3u  Y %3u", (unsigned)bx, (unsigned)by);
}

extern "C" int ps2_clouds_toggle(int which, int on);           /* clouds (ps2_clouds.c) */
extern "C" int ps2_clouds_enabled(void);
extern "C" int ps2_vk_gshadow_toggle(int on);    /* ps2_video.c: cloud shadows on the ground (-1 = not available) */
void tab_immersion() {
    {   /* our 3D clouds (ACZ_VR_CLOUDS): runtime A/B against the game's own, not saved */
        ImGui::SeparatorText("3D clouds");
        if (!ps2_clouds_enabled()) {
            ImGui::TextDisabled("Off. Start with ACZ_VR_CLOUDS=all (or near / far) to try them.");
        } else {
            static const char *const NAME[5] = { "Far clouds (ours)", "Near cloud volume (ours)", "Keep the game's clouds",
                                                 "Cloud shadows (ours)", "Optical phenomena" };
            static const char *const HELP[5] = {
                "The far cloud layer marched into a direction map shared by both eyes. Replaces the game's cloud "
                "sprites unless they are kept below.",
                "The cloud volume around the aircraft (up to 600-1000 m), per eye, with real stereo depth when "
                "flying into cloud.",
                "Draw the game's own cloud sprites too (A/B against ours).",
                "A shadow map of our clouds toward the sun: cloud bases and the far side darken with the cloud above "
                "them, clouds shade the clouds behind them, and the cockpit dims in a cloud's shadow. Off = the "
                "simple one-sample shading of before (ACZ_VR_CLOUD_SHADOW).",
                "Rare rewards when the weather and the sun allow it: a 22 degree halo around the sun through a thin icy "
                "veil, the glory's coloured rings and your aircraft's shadow (Brocken spectre) on cloud tops below with "
                "the sun behind you, and a rainbow in sunlit rain with the sun low. Subtle, in the game's colours "
                "(ACZ_VR_OPTICS)." };
            for (int k = 0; k < 5; k++) {
                int st = ps2_clouds_toggle(k, -1);
                if (st < 0) continue;
                bool b = st != 0;
                if (ImGui::Checkbox(NAME[k], &b)) ps2_clouds_toggle(k, b ? 1 : 0);
                help(HELP[k]);
            }
            {   int st = ps2_vk_gshadow_toggle(-1);
                if (st >= 0) {
                    bool b = st != 0;
                    if (ImGui::Checkbox("Cloud shadows on the ground", &b)) ps2_vk_gshadow_toggle(b ? 1 : 0);
                    help("Soft shadows of our clouds on the terrain, the sea, buildings, vehicles and trees, moving with "
                         "the clouds; the ground in a shadow keeps the sky's light. Needs \"Cloud shadows (ours)\" "
                         "(ACZ_VR_CLOUD_SHADOW_GROUND, strength ACZ_VR_CLOUD_SHADOW_GROUND_K).");
                }
            }
            ImGui::TextDisabled("For comparison only; the start-up choice is ACZ_VR_CLOUDS.");
        }
    }
    /* (the canopy, cloud, rumble and sound rows of this tab moved to F4 > Enhancements, the same fields: settings review) */
    ImGui::TextDisabled("Canopy, cloud, rumble and sound settings: F4 > Enhancements, as on the in-game page.");
}

/* F4 > Enhancements: the same settings as the in-game ENHANCEMENTS page (pause menu), from the same table
   (ps2_settings.c ps2_enh_*), saved at once. */
void tab_enhancements() {
    {   static bool test_done;                       /* (PS2_UI_TEST set=KEY:CHOICE, as a mouse would) */
        for (const char *t = ui_test_env(); !test_done && (t = std::strstr(t, "set=")) != nullptr; t += 4) {
            char k[48]; int c;
            if (sscanf(t + 4, "%47[^:]:%d", k, &c) == 2) ps2_enh_set(ps2_enh_find(k), c);
        }
        test_done = true;
    }
    ImGui::TextWrapped("The same settings as ENHANCEMENTS in the game's pause menu (and View + Menu outside a mission). "
                       "Saved at once; items marked (next mission) take effect at the next mission or RETRY, those marked "
                       "(restart) the next time the game starts.");
    if (!ps2_enh_applied())
        ImGui::TextColored(WARN, "This run was not started by a launcher (ACZ_MENU_SETTINGS unset): the switches keep "
                                 "their built-in defaults now; the saved values apply when a launcher starts the game.");
    if (int n = ps2_enh_pending_count())
        ImGui::TextColored(WARN, "%d setting%s saved for the next start.", n, n == 1 ? " is" : "s are");
    if (int n = ps2_enh_mission_count())
        ImGui::TextColored(WARN, "%d setting%s saved for the next mission or RETRY.", n, n == 1 ? " is" : "s are");
    char hint[200];
    if (ps2_enh_hint(hint, sizeof hint)[0]) ImGui::TextColored(WARN, "%s", hint);    /* the GPU ran behind (perf6_presets) */
    for (int c = 0; c < PS2_ENH_CATS; c++) {
        ImGui::SeparatorText(ps2_enh_cat_name(c));
        if (*ps2_enh_cat_head(c)) ImGui::TextDisabled("%s", ps2_enh_cat_head(c));
        ImGui::PushID(c);
        for (int i = 0; i < ps2_enh_count(); i++)
            if (ps2_enh_item_at(i)->cat == c) enh_row(i);
        ImGui::PopID();
    }
    /* the third layer (docs/SETTINGS.md): keys that are on no in-game page; only here, with the mouse */
    if (ui_test_has("expert")) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Expert (ini)")) {
        ImGui::TextWrapped("Keys of ac5_settings.ini that are on no in-game page. They work as any row: saved at once, set at "
                           "start-up, also by hand in the file.");
        ImGui::PushID(PS2_ENH_CAT_INI);
        for (int i = 0; i < ps2_enh_count(); i++)
            if (ps2_enh_item_at(i)->cat == PS2_ENH_CAT_INI) enh_row(i);
        internal_res_control();                  /* (not read while PS2_UPSCALE is set: every launcher start) */
        ImGui::PopID();
    }
}

/* The ENHANCEMENTS page (ps2_quit.c) on the window when no headset shows it: its text, the cursor's line highlighted. */
void draw_enh_page(const char *t, int sel) {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.90f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                             | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##enhpage", nullptr, f)) {
        ImGui::SetWindowFontScale(1.25f);
        int line = 0;
        for (const char *p = t; *p; line++) {
            const char *nl = std::strchr(p, '\n');
            int n = nl ? (int)(nl - p) : (int)std::strlen(p);
            const ImVec4 YEL(0.96f, 0.95f, 0.47f, 1.0f);
            if (line == sel) ImGui::TextColored(ACCENT, "> %.*s", n, p);
            else ImGui::TextColored(YEL, "  %.*s", n, p);
            p += n + (nl ? 1 : 0);
        }
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
}

void tab_analog() {
    ImGui::TextWrapped("These are PCSX2's DualShock 2 analog settings, and the game receives what "
                       "PCSX2 would send it for the same controller: sensitivity is applied first, "
                       "a stick inside the dead zone's circle reads as centred, and movement "
                       "outside it is passed on as it is, not rescaled. The defaults are PCSX2's: "
                       "no dead zone and 133%% sensitivity.");
    env_note("PS2_PAD_DEADZONE", "overrides the analog dead zone");
    if (!ps2_video_gamepad())
        ImGui::TextDisabled("Connect a controller to see the live preview.");
    else
        ImGui::TextDisabled("Preview: grey is the raw stick, green is what the game receives.");

    static const char *const INVERT[] = {
        "Not inverted", "Invert left/right", "Invert up/down", "Invert left/right + up/down",
    };
    bool ch = false;
    ch |= slider_pct("Analog dead zone", &ps2_cfg.deadzone, 0.0f, 1.0f);
    help("PCSX2's Analog Deadzone: the fraction of stick movement that is ignored, for both "
         "sticks. It is a circle, measured after sensitivity. Raise it if the aircraft drifts "
         "with the stick released.");
    ch |= slider_pct("Analog sensitivity", &ps2_cfg.axis_scale, 0.01f, 2.0f);
    help("PCSX2's Analog Sensitivity: the stick axis scaling factor. PCSX2 recommends 130% to "
         "140% for recent controllers such as the DualShock 4 or Xbox One controller.");
    ch |= slider_pct("Button / trigger dead zone", &ps2_cfg.button_deadzone, 0.0f, 1.0f);
    help("PCSX2's Button/Trigger Deadzone: how far a trigger, or a stick bound to a button, "
         "travels before it counts as pressed at all.");
    ch |= ImGui::Combo("Invert left stick", &ps2_cfg.invert[0], INVERT, 4);
    ch |= ImGui::Combo("Invert right stick", &ps2_cfg.invert[1], INVERT, 4);
    if (ImGui::Button("Reset to PCSX2 defaults")) {
        ps2_settings d;
        ps2_settings_defaults(&d);
        ps2_cfg.deadzone = d.deadzone;
        ps2_cfg.axis_scale = d.axis_scale;
        ps2_cfg.button_deadzone = d.button_deadzone;
        ps2_cfg.invert[0] = d.invert[0];
        ps2_cfg.invert[1] = d.invert[1];
        ch = true;
    }
    if (ch) changed(0);

    float view = ImGui::GetFontSize() * 11.0f;
    if (ImGui::BeginTable("sticks", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (int s = 0; s < 2; s++) {
            ImGui::TableNextColumn();
            ImGui::PushID(s);
            ImGui::TextUnformatted(s ? "Right stick" : "Left stick");
            stick_view(s, view);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (SDL_Gamepad *g = ps2_video_gamepad()) {
        ImGui::SeparatorText("Triggers");
        for (int t = 0; t < 2; t++) {
            int raw = SDL_GetGamepadAxis(g, t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                                              : SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
            float v = ps2_pad_axis_value(raw, 1);
            unsigned char pressure;
            int held = ps2_pad_trigger(ps2_cfg.button_deadzone, v, &pressure);
            char overlay[48];
            snprintf(overlay, sizeof overlay, "%s raw %.0f%%  %s, pressure %u", t ? "R2" : "L2",
                     (double)(v * 100.0f), held ? "held" : "released", (unsigned)pressure);
            ImGui::ProgressBar(v, ImVec2(-FLT_MIN, 0.0f), overlay);
        }
    }
}

extern "C" volatile float ps2_vk_window_fps;   /* ps2_video.c (PS2_PERF_WINDOW_RATE): shown frames a second, 0 = ImGui's */
void draw_fps() {
    ImGuiIO &io = ImGui::GetIO();
    const float rate = ps2_vk_window_fps > 0.0f ? ps2_vk_window_fps : io.Framerate;
    ImGui::SetNextWindowPos(ImVec2(10.0f * g_dpi, 10.0f * g_dpi), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                             | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##fps", nullptr, f))
        ImGui::Text("%.0f FPS  %.1f ms", (double)rate, rate > 0.0f ? 1000.0 / (double)rate : 0.0);
    ImGui::End();
}

/* The quit prompt (ps2_quit.c) on the window, when no headset shows it (no OpenXR session running). */
void draw_quit_prompt(const char *t) {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.90f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                             | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##quitprompt", nullptr, f)) {
        const char *nl = std::strchr(t, '\n');
        ImGui::SetWindowFontScale(1.6f);
        if (nl) ImGui::TextColored(ACCENT, "%.*s", (int)(nl - t), t);
        ImGui::SetWindowFontScale(1.25f);
        ImGui::TextUnformatted(nl ? nl + 1 : t);
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
}

void draw_menu() {
    ImGuiIO &io = ImGui::GetIO();
    float s = g_dpi * ps2_cfg.ui_scale;
    ImVec2 size(std::fmin(io.DisplaySize.x * 0.94f, 880.0f * s),
                std::fmin(io.DisplaySize.y * 0.92f, 700.0f * s));
    if (ui_test_has("tall")) size.y = io.DisplaySize.y * 0.92f;
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    const char *win_title = g_window ? SDL_GetWindowTitle(g_window) : nullptr;   /* (ps2_main.c names the game) */
    if (ImGui::Begin(win_title && std::strstr(win_title, "Zero") ? "Ace Combat Zero - Settings###ps2settings" : "Settings###ps2settings", &open,
                     ImGuiWindowFlags_NoCollapse)) {
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        if (ImGui::BeginTabBar("tabs")) {
            static const char *const TABS[] = {
                "Quick", "Display", "Rendering", "Post-processing",
                "Keyboard", "Controller", "Analog & dead zones", "Clouds A/B", "Enhancements",
            };
            static void (*const PAGES[])() = {
                tab_quick, tab_display, tab_rendering, tab_post,
                tab_keyboard, tab_controller, tab_analog, tab_immersion, tab_enhancements,
            };
            for (int i = 0; i < 9; i++) {
                ImGuiTabItemFlags f = g_want_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(TABS[i], nullptr, f)) {
                    if (ImGui::BeginChild("page", ImVec2(0.0f, -footer))) {
                        PAGES[i]();
                        if (ui_test_scroll() >= 0) ImGui::SetScrollY((float)ui_test_scroll());
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            g_want_tab = TAB_NONE;
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("F4 or Esc closes  |  F11 or Alt+Enter toggles fullscreen  |  %s",
                            ps2_settings_path());
    }
    ImGui::End();
    if (!open) ps2_ui_set_visible(0);
}

}

extern "C" void ps2_ui_launcher_style(void) { apply_style(); }   /* ps2_launcher.cpp: the same look, in its own ImGui context */

int ps2_ui_init(const ps2_ui_init_info *info) {
    if (g_init) return 0;
    if (!info || !info->window) return -1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad
                    | ImGuiConfigFlags_NoMouseCursorChange;
    g_window = info->window;
    g_max_aniso = info->max_anisotropy;
    g_dpi = SDL_GetWindowDisplayScale(info->window);
    if (!(g_dpi > 0.0f)) g_dpi = 1.0f;
    apply_style();
    ImGui::GetStyle().ScaleAllSizes(g_dpi);
    ImGui::GetStyle().FontScaleDpi = g_dpi;

    if (!ImGui_ImplSDL3_InitForVulkan(info->window)) {
        ps2_log("ui: the Dear ImGui SDL3 backend did not initialise");
        ImGui::DestroyContext();
        return -1;
    }
    g_color_format = info->color_format;
    ImGui_ImplVulkan_InitInfo vi = {};
    vi.ApiVersion = VK_API_VERSION_1_3;
    vi.Instance = info->instance;
    vi.PhysicalDevice = info->phys;
    vi.Device = info->device;
    vi.QueueFamily = info->queue_family;
    vi.Queue = info->queue;
    vi.DescriptorPoolSize = 32;
    vi.MinImageCount = info->image_count >= 2 ? info->image_count : 2;
    vi.ImageCount = vi.MinImageCount;
    vi.UseDynamicRendering = true;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_color_format;
    vi.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&vi)) {
        ps2_log("ui: the Dear ImGui Vulkan backend did not initialise");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return -1;
    }
    g_init = true;
    ps2_log("ui: Dear ImGui %s ready -- F4 opens the settings menu", IMGUI_VERSION);
    if (const char *e = getenv("PS2_UI_OPEN")) {
        ps2_ui_set_visible(1);
        g_want_tab = atoi(e);
    }
    return 0;
}

void ps2_ui_shutdown(void) {
    if (!g_init) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g_init = false;
    g_visible = false;
}

/* PS2_PAD_JOY's "f4" entry: this tab and the capture popup, for a window-only capture of it */
void ps2_ui_hotas_test(int row, int slot) {
    ps2_ui_set_visible(1);
    g_want_tab = TAB_CONTROLLER;
    start_capture(CAP_JOY, row, slot);
    ps2_hotas_capture_begin(row, slot);
}

int ps2_ui_visible(void) { return g_init && g_visible; }

void ps2_ui_set_visible(int on) {
    if (!g_init || (on != 0) == g_visible) return;
    g_visible = on != 0;
    ImGuiIO &io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    g_cap = CAP_NONE;
    ps2_hotas_capture_cancel();                   /* (a capture must not outlive the window) */
    if (g_visible) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
}

int ps2_ui_blocks_game_input(void) {
    return g_init && g_visible && ps2_cfg.block_input_in_menu;
}

int ps2_ui_event(const SDL_Event *e) {
    if (!g_init) return 0;
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_F4 && !e->key.repeat) {
        ps2_ui_set_visible(!g_visible);
        return 1;
    }
    if (g_cap != CAP_NONE && capture_event(e)) return 1;
    if (!g_visible) {
        if (e->type == SDL_EVENT_GAMEPAD_ADDED || e->type == SDL_EVENT_GAMEPAD_REMOVED)
            ImGui_ImplSDL3_ProcessEvent(e);
        return 0;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
        ps2_ui_set_visible(0);
        return 1;
    }
    ImGui_ImplSDL3_ProcessEvent(e);
    return 0;
}

extern "C" volatile int ps2_vk_window_band;   /* ps2_video.c: the window shows ACZ_REC_CLEAN_FRAME's band */
void ps2_ui_draw(VkCommandBuffer cmd) {
    const char *quit_text = ps2_xr_session_running() ? nullptr : ps2_quit_overlay_text();
    int enh_sel = -1;
    const char *enh_text = ps2_xr_session_running() ? nullptr : ps2_quit_enh_text(&enh_sel);
    const bool fps = ps2_cfg.show_fps && !ps2_vk_window_band;   /* (not over the clean recording's band) */
    if (!g_init || (!g_visible && !fps && !quit_text && !enh_text)) return;
    ImGuiIO &io = ImGui::GetIO();
    if (g_cap == CAP_PAD) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::GetStyle().FontScaleMain = ps2_cfg.ui_scale;
    update_capture_arming();

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (fps) draw_fps();
    if (quit_text) draw_quit_prompt(quit_text);
    if (enh_text) draw_enh_page(enh_text, enh_sel);
    if (g_visible) {
        draw_menu();
        draw_capture_popup();
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ps2_ui_swapchain_changed(uint32_t image_count) {
    if (g_init && image_count >= 2) ImGui_ImplVulkan_SetMinImageCount(image_count);
}
