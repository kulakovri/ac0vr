/* The launcher (menu_1105c, the settings review of 2026-11-05; docs/SETTINGS.md layer 1): the QUALITY rows, the GPU line, ACTIVE
   CONTROLLER and PLAY, in the game's window, before anything of the engine exists.

   Why it comes first. The QUALITY rows are environment switches and their readers run at or before the creation of what they
   configure: main()'s zero_*_init, vk_init (PS2_UPSCALE, ACZ_VR_CLOUDS), the renderer's edge_aa_init, cpit_load, lw_init. So
   ps2_launcher_run is main()'s first call after the arguments (only the settings constructor has run), and it blocks main until
   PLAY. A choice reaches the environment at PLAY (ps2_enh_launcher_commit), so every reader then sees it as if the file had said
   so, in this same boot.

   What it does not touch. Its Vulkan instance, device and swapchain and its ImGui context are its own and are gone before main()
   goes on: the renderer's vk_init and ps2_ui_init run as ever. It makes no OpenXR call at all: the headset stays idle until vk_init,
   after PLAY, in today's order, with today's failure handling. Only two things are handed on:
   - the OS window: it belongs to the thread that made it (its messages), so the launcher's thread stays alive after PLAY, pumps the
     window while main() boots, and becomes the renderer thread (ps2_launcher_take_thread, ps2_launcher_take_window);
   - SDL's video subsystem. The gamepad subsystem is closed at PLAY, so the renderer's SDL_Init opens it with its own hints
     (ps2_hotas_pre_init) and the controller registry starts as in a boot without the launcher.
   If anything of this fails (no window, no Vulkan 1.3 device) the launcher says so and the game starts without it. */
#include "ps2_settings.h"
#include "ps2_hotas.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <pthread.h>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void ps2_log(const char *fmt, ...);
void ps2_mkdir_p(const char *path);
void ps2_ui_launcher_style(void);                       /* ps2_ui.cpp: the F4 menu's colours and rounding */
void ps2_enh_pad_names(int n, const char *const *names);
int  ps2_launcher_run(int hidden);
SDL_Window *ps2_launcher_take_window(const char *title);
int  ps2_launcher_take_thread(void *(*fn)(void *), void *arg, pthread_t *out);
}

namespace {

enum { ST_RUNNING, ST_PLAY, ST_QUIT, ST_NONE };
enum { LP_ROWS = 12, LP_PADS = 4 };

struct {
    pthread_t th;
    pthread_mutex_t m;
    pthread_cond_t cv;
    int state;
    void *(*job)(void *);
    void *job_arg;
    SDL_Window *win;
    int hidden;
    double autoplay;                                    /* s, < 0 none */
} L = { {}, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, ST_RUNNING, nullptr, nullptr, nullptr, 0, -1.0 };

/* ---- Vulkan, the launcher's own ---- */
struct {
    VkInstance inst;
    VkSurfaceKHR surf;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t qf;
    VkQueue q;
    VkSwapchainKHR sc;
    VkFormat fmt;
    VkExtent2D ext;
    uint32_t n;
    VkImage img[8];
    VkImageView view[8];
    VkSemaphore done[8];
    VkSemaphore acq;
    VkFence fence;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkBuffer dbuf;
    VkDeviceMemory dmem;
    void *dptr;
    VkDeviceSize dsize;
} V;

bool sc_destroy() {
    if (V.dev) vkDeviceWaitIdle(V.dev);
    for (uint32_t i = 0; i < 8; i++) {
        if (V.view[i]) vkDestroyImageView(V.dev, V.view[i], nullptr);
        if (V.done[i]) vkDestroySemaphore(V.dev, V.done[i], nullptr);
        V.view[i] = VK_NULL_HANDLE; V.done[i] = VK_NULL_HANDLE; V.img[i] = VK_NULL_HANDLE;
    }
    if (V.sc) vkDestroySwapchainKHR(V.dev, V.sc, nullptr);
    V.sc = VK_NULL_HANDLE;
    return true;
}

bool sc_create() {
    VkSurfaceCapabilitiesKHR c;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, V.surf, &c) != VK_SUCCESS) return false;
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(L.win, &pw, &ph);
    VkExtent2D e = c.currentExtent;
    if (e.width == 0xFFFFFFFFu) { e.width = (uint32_t)pw; e.height = (uint32_t)ph; }
    if (e.width == 0 || e.height == 0) return false;     /* minimised: not now */
    VkSurfaceFormatKHR f[32];
    uint32_t nf = 32;
    vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surf, &nf, f);
    if (!nf) return false;
    VkSurfaceFormatKHR use = f[0];
    for (uint32_t i = 0; i < nf; i++) if (f[i].format == VK_FORMAT_B8G8R8A8_UNORM) { use = f[i]; break; }
    uint32_t want = c.minImageCount + 1;
    if (want < 2) want = 2;
    if (want > 8) want = 8;
    if (c.maxImageCount && want > c.maxImageCount) want = c.maxImageCount;
    VkSwapchainCreateInfoKHR ci = {};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = V.surf;
    ci.minImageCount = want;
    ci.imageFormat = use.format;
    ci.imageColorSpace = use.colorSpace;
    ci.imageExtent = e;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = c.currentTransform;
    ci.compositeAlpha = (c.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : (VkCompositeAlphaFlagBitsKHR)(c.supportedCompositeAlpha & -(int)c.supportedCompositeAlpha);
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    {   /* mailbox where there is one, paced by the loop: FIFO on a hidden or covered window can wait 300 ms a frame, and the window's
           messages with it */
        VkPresentModeKHR pm[8];
        uint32_t npm = 8;
        vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surf, &npm, pm);
        for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) ci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    }
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(V.dev, &ci, nullptr, &V.sc) != VK_SUCCESS) { V.sc = VK_NULL_HANDLE; return false; }
    V.n = 8;
    if (vkGetSwapchainImagesKHR(V.dev, V.sc, &V.n, V.img) != VK_SUCCESS) return false;
    V.fmt = use.format;
    V.ext = e;
    VkSemaphoreCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (uint32_t i = 0; i < V.n; i++) {
        VkImageViewCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = V.img[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = V.fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = 1;
        vi.subresourceRange.layerCount = 1;
        if (vkCreateImageView(V.dev, &vi, nullptr, &V.view[i]) != VK_SUCCESS) return false;
        if (vkCreateSemaphore(V.dev, &si, nullptr, &V.done[i]) != VK_SUCCESS) return false;
    }
    return true;
}

/* the device: the renderer's rule without the OpenXR runtime's word (a discrete Vulkan 1.3 GPU, else the first 1.3 one), and the
   GPU line's name, type and largest device-local heap from it, as vk_init gives them to ps2_enh_set_gpu */
bool vk_up() {
    Uint32 nsdl = 0;
    const char *const *sdl_exts = SDL_Vulkan_GetInstanceExtensions(&nsdl);
    if (!sdl_exts) return false;
    VkApplicationInfo app = {};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ps2recomp launcher";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = nsdl;
    ici.ppEnabledExtensionNames = sdl_exts;
    if (vkCreateInstance(&ici, nullptr, &V.inst) != VK_SUCCESS) { V.inst = VK_NULL_HANDLE; return false; }
    if (!SDL_Vulkan_CreateSurface(L.win, V.inst, nullptr, &V.surf)) { V.surf = VK_NULL_HANDLE; return false; }
    VkPhysicalDevice devs[16];
    uint32_t nd = 16;
    if (vkEnumeratePhysicalDevices(V.inst, &nd, devs) < 0) return false;
    for (uint32_t i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(devs[i], &pr);
        if (pr.apiVersion < VK_API_VERSION_1_3) continue;
        if (!V.phys || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) V.phys = devs[i];
    }
    if (!V.phys) return false;
    {   VkPhysicalDeviceProperties pr;
        VkPhysicalDeviceMemoryProperties mp;
        VkDeviceSize vl = 0;
        vkGetPhysicalDeviceProperties(V.phys, &pr);
        vkGetPhysicalDeviceMemoryProperties(V.phys, &mp);
        for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
            if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > vl) vl = mp.memoryHeaps[h].size;
        ps2_enh_launcher_gpu(pr.deviceName, pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, (double)vl / 1073741824.0);
    }
    VkQueueFamilyProperties qp[16];
    uint32_t nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(V.phys, &nq, qp);
    V.qf = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(V.phys, i, V.surf, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { V.qf = i; break; }
    }
    if (V.qf == 0xFFFFFFFFu) return false;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = V.qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13 = {};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    const char *dext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = &dext;
    if (vkCreateDevice(V.phys, &dci, nullptr, &V.dev) != VK_SUCCESS) { V.dev = VK_NULL_HANDLE; return false; }
    vkGetDeviceQueue(V.dev, V.qf, 0, &V.q);
    VkCommandPoolCreateInfo cp = {};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = V.qf;
    if (vkCreateCommandPool(V.dev, &cp, nullptr, &V.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = V.pool;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(V.dev, &cb, &V.cmd) != VK_SUCCESS) return false;
    VkSemaphoreCreateInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateSemaphore(V.dev, &si, nullptr, &V.acq) != VK_SUCCESS || vkCreateFence(V.dev, &fi, nullptr, &V.fence) != VK_SUCCESS)
        return false;
    return sc_create();
}

void vk_down() {
    if (V.dev) {
        sc_destroy();
        if (V.dbuf) vkDestroyBuffer(V.dev, V.dbuf, nullptr);
        if (V.dmem) vkFreeMemory(V.dev, V.dmem, nullptr);
        if (V.fence) vkDestroyFence(V.dev, V.fence, nullptr);
        if (V.acq) vkDestroySemaphore(V.dev, V.acq, nullptr);
        if (V.pool) vkDestroyCommandPool(V.dev, V.pool, nullptr);
        vkDestroyDevice(V.dev, nullptr);
    }
    if (V.surf) SDL_Vulkan_DestroySurface(V.inst, V.surf, nullptr);
    if (V.inst) vkDestroyInstance(V.inst, nullptr);
    memset(&V, 0, sizeof V);
}

void check_vk(VkResult r) { if (r < 0) ps2_log("launcher: Vulkan call failed (%d)", (int)r); }

/* ---- the rows ---- */
struct {
    int row[LP_ROWS];                                   /* table rows: QUALITY without its action, then ACTIVE CONTROLLER */
    int n, cur;                                         /* cur == n: "do not show again" */
    bool hide;                                          /* the checkbox */
    int changed;                                        /* choices made here */
    char others[300];                                   /* rows of other categories changed last time */
    char gpu[260];
    const char *how;                                    /* what pressed PLAY */
    bool play, quit;
    ImVec2 r_play[2], r_check[2], r_seg[LP_ROWS][PS2_ENH_CHOICES][2];
} U;

struct { SDL_JoystickID id; SDL_Gamepad *g; char name[96]; } P[LP_PADS];
int np;
bool pad_noreal;

/* the same names ps2_enh_pad_names gets from the registry (SDL's name; no flight stick or throttle) */
void pads_publish() {
    const char *nm[LP_PADS];
    for (int i = 0; i < np; i++) nm[i] = P[i].name;
    ps2_enh_pad_names(np, nm);
}
void pad_add(SDL_JoystickID id, bool test_pad) {
    if (np >= LP_PADS || (pad_noreal && !test_pad)) return;
    for (int i = 0; i < np; i++) if (P[i].id == id) return;
    if (ps2_hotas_claims(id)) return;                   /* (a flight stick or throttle with a gamepad mapping: as the registry) */
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (!g) return;
    P[np].id = id;
    P[np].g = g;
    snprintf(P[np].name, sizeof P[np].name, "%s", SDL_GetGamepadName(g) ? SDL_GetGamepadName(g) : "Controller");
    ps2_log("launcher: controller %s (south button %s)", P[np].name,
            SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_CROSS ? "Cross" : "A");
    np++;
    pads_publish();
}
void pad_del(SDL_JoystickID id) {
    for (int i = 0; i < np; i++) {
        if (P[i].id != id) continue;
        SDL_CloseGamepad(P[i].g);
        P[i] = P[--np];
        pads_publish();
        return;
    }
}
void pads_close() {
    for (int i = 0; i < np; i++) SDL_CloseGamepad(P[i].g);
    np = 0;
}

void step_row(int d) {
    int total = U.n + 1;
    U.cur = (U.cur + d + total) % total;
}
void choose(int r, int k) {
    if (ps2_enh_launcher_choose(U.row[r], k) > 0) U.changed++;
}
void step_value(int d) {
    if (U.cur == U.n) { U.hide = !U.hide; ps2_enh_launcher_show_set(!U.hide); return; }
    int i = U.row[U.cur];
    const ps2_enh_item *it = ps2_enh_item_at(i);
    if (!it || !it->n) return;
    int c = ps2_enh_choice(i);
    c = c < 0 ? (d > 0 ? 0 : it->n - 1) : (c + d + it->n) % it->n;
    choose(U.cur, c);
}

/* ---- [test] ACZ_LAUNCHER_TEST: a script in launcher frames (words, space separated):
   pad=xbox|ps5           a virtual SDL gamepad attached at the start (VID/PID as PS2_PAD_SDL's), its buttons go through SDL's events
   F:key:NAME             an SDL key event: down up left right return space
   F:pad:NAME             the virtual pad's button pressed for two frames: a dpup dpdown dpleft dpright (SDL's names)
   F:click:play|check|R.K a mouse motion, press and release on PLAY, the checkbox or row R's choice K (SDL mouse events: ImGui's hit test)
   F:dump                 the window's image into out/launcher_<frame>.ppm (this window only; never a screen capture)
   F:close                the window's close request */
enum { T_KEY, T_PAD, T_CLICK, T_DUMP, T_CLOSE };
struct { int frame, kind, a, b; char word[24]; int stage; bool done; } T[64];
int nt;
SDL_JoystickID t_pad_id;
SDL_Joystick *t_pad;

void test_parse() {
    const char *e = getenv("ACZ_LAUNCHER_TEST");
    if (!e || !*e) return;
    char buf[2000];
    snprintf(buf, sizeof buf, "%s", e);
    char *save = nullptr;
    for (char *w = SDL_strtok_r(buf, " ", &save); w; w = SDL_strtok_r(nullptr, " ", &save)) {
        if (!strncmp(w, "pad=", 4)) {
            bool ps5 = !strcmp(w + 4, "ps5");
            SDL_VirtualJoystickDesc d;
            SDL_INIT_INTERFACE(&d);
            d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            d.vendor_id = ps5 ? 0x054c : 0x045e;
            d.product_id = ps5 ? 0x0ce6 : 0x0b12;
            d.naxes = SDL_GAMEPAD_AXIS_COUNT;
            d.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
            d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1u;
            d.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1u;
            d.name = ps5 ? "DualSense Wireless Controller (virtual)" : "Xbox Series X Controller (virtual)";
            t_pad_id = SDL_AttachVirtualJoystick(&d);
            t_pad = t_pad_id ? SDL_OpenJoystick(t_pad_id) : nullptr;
            ps2_log("launcher test: virtual pad %s: %s", d.name, t_pad ? "attached" : SDL_GetError());
            continue;
        }
        char *c1 = strchr(w, ':');
        if (nt >= 64 || !c1) continue;
        T[nt].frame = atoi(w);
        const char *rest = c1 + 1, *arg = strchr(rest, ':');
        size_t l = arg ? (size_t)(arg - rest) : strlen(rest);
        snprintf(T[nt].word, sizeof T[nt].word, "%s", arg ? arg + 1 : "");
        if (l == 3 && !strncmp(rest, "key", 3)) T[nt].kind = T_KEY;
        else if (l == 3 && !strncmp(rest, "pad", 3)) T[nt].kind = T_PAD;
        else if (l == 5 && !strncmp(rest, "click", 5)) T[nt].kind = T_CLICK;
        else if (l == 4 && !strncmp(rest, "dump", 4)) T[nt].kind = T_DUMP;
        else if (l == 5 && !strncmp(rest, "close", 5)) T[nt].kind = T_CLOSE;
        else continue;
        nt++;
    }
}
void push_key(SDL_Keycode k) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = SDL_GetWindowID(L.win);
    e.key.key = k;
    e.key.down = true;
    SDL_PushEvent(&e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    SDL_PushEvent(&e);
}
void push_mouse(float x, float y, int stage) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    if (stage == 0) {
        e.type = SDL_EVENT_MOUSE_MOTION;
        e.motion.windowID = SDL_GetWindowID(L.win);
        e.motion.x = x; e.motion.y = y;
    } else {
        e.type = stage == 1 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        e.button.windowID = SDL_GetWindowID(L.win);
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = stage == 1;
        e.button.clicks = 1;
        e.button.x = x; e.button.y = y;
    }
    SDL_PushEvent(&e);
}
bool dump_wanted;
void test_step(int frame) {
    for (int i = 0; i < nt; i++) {
        if (T[i].done || frame < T[i].frame) continue;
        const char *w = T[i].word;
        switch (T[i].kind) {
        case T_KEY:
            push_key(!strcmp(w, "down") ? SDLK_DOWN : !strcmp(w, "up") ? SDLK_UP : !strcmp(w, "left") ? SDLK_LEFT
                     : !strcmp(w, "right") ? SDLK_RIGHT : !strcmp(w, "return") ? SDLK_RETURN : SDLK_SPACE);
            T[i].done = true;
            break;
        case T_PAD: {
            int b = SDL_GetGamepadButtonFromString(w);
            if (t_pad && b >= 0) SDL_SetJoystickVirtualButton(t_pad, b, T[i].stage == 0);
            if (++T[i].stage >= 2) T[i].done = true;
            T[i].frame = frame + 2;
            break; }
        case T_CLICK: {
            ImVec2 *r = !strcmp(w, "play") ? U.r_play : !strcmp(w, "check") ? U.r_check : nullptr;
            if (!r) {
                int a = atoi(w), b = strchr(w, '.') ? atoi(strchr(w, '.') + 1) : 0;
                if (a >= 0 && a < LP_ROWS && b >= 0 && b < PS2_ENH_CHOICES) r = U.r_seg[a][b];
            }
            if (r) push_mouse((r[0].x + r[1].x) * 0.5f, (r[0].y + r[1].y) * 0.5f, T[i].stage);
            if (++T[i].stage >= 3) T[i].done = true;
            T[i].frame = frame + 2;
            break; }
        case T_DUMP: dump_wanted = true; T[i].done = true; break;
        case T_CLOSE: {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            e.window.windowID = SDL_GetWindowID(L.win);
            SDL_PushEvent(&e);
            T[i].done = true;
            break; }
        }
    }
}

void dump_write(int frame) {
    ps2_mkdir_p("out");
    char path[64];
    snprintf(path, sizeof path, "out/launcher_%04d.ppm", frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%u %u\n255\n", V.ext.width, V.ext.height);
    bool bgr = V.fmt == VK_FORMAT_B8G8R8A8_UNORM || V.fmt == VK_FORMAT_B8G8R8A8_SRGB;
    const unsigned char *p = (const unsigned char *)V.dptr;
    unsigned char *row = (unsigned char *)malloc((size_t)V.ext.width * 3u);
    for (uint32_t y = 0; row && y < V.ext.height; y++) {
        for (uint32_t x = 0; x < V.ext.width; x++, p += 4) {
            row[x * 3] = p[bgr ? 2 : 0];
            row[x * 3 + 1] = p[1];
            row[x * 3 + 2] = p[bgr ? 0 : 2];
        }
        fwrite(row, 1, (size_t)V.ext.width * 3u, f);
    }
    free(row);
    fclose(f);
    ps2_log("launcher: window dump %s (%ux%u)", path, V.ext.width, V.ext.height);
}
bool dump_buffer() {
    VkDeviceSize need = (VkDeviceSize)V.ext.width * V.ext.height * 4;
    if (V.dbuf && V.dsize >= need) return true;
    if (V.dbuf) { vkDestroyBuffer(V.dev, V.dbuf, nullptr); vkFreeMemory(V.dev, V.dmem, nullptr); V.dbuf = VK_NULL_HANDLE; V.dmem = VK_NULL_HANDLE; }
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = need;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(V.dev, &bi, nullptr, &V.dbuf) != VK_SUCCESS) { V.dbuf = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(V.dev, V.dbuf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(V.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (!(mr.memoryTypeBits & (1u << i)) || (mp.memoryTypes[i].propertyFlags & want) != want) continue;
        VkMemoryAllocateInfo ai = {};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = i;
        if (vkAllocateMemory(V.dev, &ai, nullptr, &V.dmem) != VK_SUCCESS) return false;
        vkBindBufferMemory(V.dev, V.dbuf, V.dmem, 0);
        vkMapMemory(V.dev, V.dmem, 0, VK_WHOLE_SIZE, 0, &V.dptr);
        V.dsize = need;
        return true;
    }
    return false;
}

void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da, VkPipelineStageFlags ss,
             VkPipelineStageFlags ds) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(V.cmd, ss, ds, 0, 0, nullptr, 0, nullptr, 1, &b);
}

/* ---- the page ---- */
const ImVec4 ACCENT(0.40f, 0.86f, 0.50f, 1.0f);
const ImVec4 WARN(1.00f, 0.76f, 0.32f, 1.0f);

void draw_rows() {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float full = ImGui::GetContentRegionAvail().x, lab = full * 0.30f;
    for (int r = 0; r < U.n; r++) {
        int i = U.row[r];
        const ps2_enh_item *it = ps2_enh_item_at(i);
        char hb[96];
        const char *held = ps2_enh_held_by(i, hb, sizeof hb);
        const char *block = ps2_enh_launcher_blocked(i);
        const bool locked = held || block;
        int cur = ps2_enh_choice(i);
        char val[64];
        if (held) {                                     /* held by the environment: the value in effect, as the page shows it */
            ps2_enh_value_name(i, val, sizeof val);
            cur = -1;
            for (int k = 0; k < it->n; k++) if (!strcmp(it->name[k], val)) cur = k;
        } else if (block) cur = ps2_enh_running(i);
        const bool mark = ps2_enh_launcher_changed(i) != 0;
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::PushID(r);
        if (mark) ImGui::PushStyleColor(ImGuiCol_Text, WARN);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(it->label);
        if (mark) ImGui::PopStyleColor();
        ImGui::SameLine(lab);
        if (locked) ImGui::BeginDisabled();
        float x = lab;
        bool first = true;
        for (int k = 0; k < it->n; k++) {
            const float w = ImGui::CalcTextSize(it->name[k]).x + ImGui::GetStyle().FramePadding.x * 2.0f + 6.0f;
            if (!first) {
                if (x + w + ImGui::GetStyle().ItemSpacing.x < full) ImGui::SameLine(0.0f, ImGui::GetStyle().ItemSpacing.x);
                else { x = lab; ImGui::SetCursorPosX(lab); }
            }
            x += w + ImGui::GetStyle().ItemSpacing.x;
            first = false;
            const bool sel = cur == k;
            if (sel) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.52f, 0.32f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 0.38f, 1.0f));
            }
            if (ImGui::Button(it->name[k], ImVec2(w, 0.0f))) { U.cur = r; if (!locked) choose(r, k); }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) U.cur = r;
            U.r_seg[r][k][0] = ImGui::GetItemRectMin();
            U.r_seg[r][k][1] = ImGui::GetItemRectMax();
            if (sel) ImGui::PopStyleColor(2);
        }
        if (cur < 0) {                                  /* CUSTOM, or a value that is no choice */
            ImGui::SameLine();
            ImGui::TextColored(WARN, "%s", ps2_enh_value_name(i, val, sizeof val));
        }
        if (locked) ImGui::EndDisabled();
        if (held) { ImGui::SameLine(); ImGui::TextColored(WARN, "held: %s", held); }
        if (mark) { ImGui::SameLine(); ImGui::TextColored(WARN, "changed last time"); }
        ImVec2 p1 = ImGui::GetCursorScreenPos();
        if (U.cur == r)
            dl->AddRect(ImVec2(p0.x - 6.0f, p0.y - 3.0f), ImVec2(p0.x + full + 6.0f, p1.y - ImGui::GetStyle().ItemSpacing.y + 3.0f),
                        ImGui::GetColorU32(ACCENT), 4.0f);
        ImGui::PopID();
    }
}

void draw_page() {
    /* a panel in the middle of the window, as wide as about 72 letters, as high as it has rows (the last frame's height) */
    static float last_h;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    float pw = ImGui::GetFontSize() * 72.0f; if (pw > vp->Size.x) pw = vp->Size.x;
    const float ph = last_h > 0.0f && last_h < vp->Size.y ? last_h : vp->Size.y;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - pw) * 0.5f, vp->Pos.y + (vp->Size.y - ph) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
                 | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ACCENT, "ACE COMBAT ZERO");
    ImGui::SameLine();
    ImGui::TextDisabled("  These settings apply when you press PLAY.");
    ImGui::Separator();
    ImGui::TextWrapped("%s", U.gpu);
    ImGui::Spacing();
    draw_rows();
    {   ImVec2 p0 = ImGui::GetCursorScreenPos();
        bool hide = U.hide;
        if (ImGui::Checkbox("Do not show this again (F4 > Quick brings it back)", &hide)) {
            U.hide = hide;
            ps2_enh_launcher_show_set(!hide);
            U.cur = U.n;
        }
        if (ImGui::IsItemHovered()) U.cur = U.n;
        U.r_check[0] = ImGui::GetItemRectMin();
        U.r_check[1] = ImGui::GetItemRectMax();
        if (U.cur == U.n)
            ImGui::GetWindowDrawList()->AddRect(ImVec2(p0.x - 6.0f, p0.y - 3.0f), ImVec2(p0.x + ImGui::GetContentRegionAvail().x + 6.0f,
                                                ImGui::GetItemRectMax().y + 3.0f), ImGui::GetColorU32(ACCENT), 4.0f);
    }
    ImGui::Separator();
    {   /* the line under the rows: the row's own help, what holds it */
        char hp[1200], hb[96];
        if (U.cur < U.n) {
            int i = U.row[U.cur];
            const char *held = ps2_enh_held_by(i, hb, sizeof hb);
            const char *block = ps2_enh_launcher_blocked(i);
            if (held) ImGui::TextColored(WARN, "Set by %s when the game started; it keeps that value in this run.", held);
            else if (block) ImGui::TextColored(WARN, "%s", block);
            ImGui::TextWrapped("%s", ps2_enh_help(i, hp, sizeof hp, 0));
        } else ImGui::TextWrapped("The game then starts directly with these settings; they stay on the pause menu's ENHANCEMENTS page.");
    }
    if (U.others[0]) ImGui::TextColored(WARN, "Also changed last time, in effect from now on: %s.", U.others);
    ImGui::Spacing();
    const float h = ImGui::GetFrameHeight() * 2.0f;
    if (ImGui::Button("PLAY", ImVec2(-FLT_MIN, h))) { U.play = true; U.how = "mouse"; }
    U.r_play[0] = ImGui::GetItemRectMin();
    U.r_play[1] = ImGui::GetItemRectMax();
    ImGui::TextDisabled("Up / Down: a row.  Left / Right: its value.  PLAY: Enter, Space, a pad's A or Cross, the mouse.");
    ImGui::TextDisabled("Close the window to quit; the choices made here are saved by PLAY.");
    last_h = ImGui::GetCursorPosY() + ImGui::GetStyle().WindowPadding.y;
    ImGui::End();
}

void event(const SDL_Event &e, double since) {
    ImGui_ImplSDL3_ProcessEvent(&e);
    static int stick_y, stick_x;
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        U.quit = true;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (e.key.repeat && e.key.key != SDLK_UP && e.key.key != SDLK_DOWN) break;
        if (e.key.key == SDLK_UP) step_row(-1);
        else if (e.key.key == SDLK_DOWN) step_row(1);
        else if (e.key.key == SDLK_LEFT) step_value(-1);
        else if (e.key.key == SDLK_RIGHT) step_value(1);
        else if (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER) { U.play = true; U.how = "Enter"; }
        else if (e.key.key == SDLK_SPACE) { U.play = true; U.how = "Space"; }
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        pad_add(e.gdevice.which, e.gdevice.which == t_pad_id);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        pad_del(e.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_UP) step_row(-1);
        else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_DOWN) step_row(1);
        else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_LEFT) step_value(-1);
        else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) step_value(1);
        else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH && since > 0.3) {   /* (a button already held when it opened: no press) */
            SDL_Gamepad *g = SDL_GetGamepadFromID(e.gbutton.which);
            U.play = true;
            U.how = g && SDL_GetGamepadButtonLabel(g, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_CROSS ? "pad Cross" : "pad A";
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:                 /* the left stick steps too */
        if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            int &st = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY ? stick_y : stick_x;
            int d = e.gaxis.value > 16000 ? 1 : e.gaxis.value < -16000 ? -1 : 0;
            if (d && !st) { if (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) step_row(d); else step_value(d); }
            if (d || (e.gaxis.value > -8000 && e.gaxis.value < 8000)) st = d;
        }
        break;
    default:
        break;
    }
}

/* The launcher's thread: everything SDL of the launcher happens here; main() waits. After PLAY it stays as the window's owner. */
void finish(int st) {
    pthread_mutex_lock(&L.m);
    L.state = st;
    pthread_cond_broadcast(&L.cv);
    pthread_mutex_unlock(&L.m);
}

double now_s() { return (double)SDL_GetTicksNS() * 1e-9; }

int launcher_body() {
    const double t0 = now_s();
    {   /* ACZ_PAD_BACKGROUND as renderer_main reads it, which sets it again before its own SDL_Init */
        const char *e = getenv("ACZ_PAD_BACKGROUND"), *x = getenv("PS2_XR");
        int bg = e && *e && strcmp(e, "auto") ? *e != '0' : (x && *x && strcmp(x, "0")) || L.hidden;
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, bg ? "1" : "0");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { ps2_log("launcher: not shown: SDL_Init: %s", SDL_GetError()); return ST_NONE; }
    ps2_enh_launcher_begin();
    L.win = SDL_CreateWindow("Ace Combat Zero - The Belkan War", ps2_cfg.window_w, ps2_cfg.window_h,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | (L.hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!L.win) { ps2_log("launcher: not shown: SDL_CreateWindow: %s", SDL_GetError()); return ST_NONE; }
    if (!vk_up()) {
        ps2_log("launcher: not shown: no Vulkan 1.3 device or swapchain for the window");
        vk_down();
        SDL_DestroyWindow(L.win);
        L.win = nullptr;
        return ST_NONE;
    }
    ps2_hotas_pre_init();                               /* (the device-type hints: the pads' list as the registry's; renderer_main again) */
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    {   const char *ps = getenv("PS2_PAD_SDL");
        pad_noreal = ps && strstr(ps, "noreal"); }
    test_parse();
    {   int n = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&n);
        for (int k = 0; ids && k < n; k++) pad_add(ids[k], ids[k] == t_pad_id);
        SDL_free(ids);
    }
    pads_publish();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;   /* (no ImGui nav: the keys and the pad are this page's own) */
    float dpi = SDL_GetWindowDisplayScale(L.win);
    if (!(dpi > 0.0f)) dpi = 1.0f;
    ps2_ui_launcher_style();
    ImGui::GetStyle().ScaleAllSizes(dpi);
    ImGui::GetStyle().FontScaleDpi = dpi;
    ImGui_ImplSDL3_InitForVulkan(L.win);
    {   static VkFormat cf;
        cf = V.fmt;
        ImGui_ImplVulkan_InitInfo vi = {};
        vi.ApiVersion = VK_API_VERSION_1_3;
        vi.Instance = V.inst;
        vi.PhysicalDevice = V.phys;
        vi.Device = V.dev;
        vi.QueueFamily = V.qf;
        vi.Queue = V.q;
        vi.DescriptorPoolSize = 32;
        vi.MinImageCount = V.n >= 2 ? V.n : 2;
        vi.ImageCount = vi.MinImageCount;
        vi.UseDynamicRendering = true;
        vi.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        vi.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        vi.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &cf;
        vi.CheckVkResultFn = check_vk;
        if (!ImGui_ImplVulkan_Init(&vi)) {
            ps2_log("launcher: not shown: the Dear ImGui Vulkan backend did not initialise");
            ImGui_ImplSDL3_Shutdown();
            ImGui::DestroyContext();
            pads_close();
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
            vk_down();
            SDL_DestroyWindow(L.win);
            L.win = nullptr;
            return ST_NONE;
        }
    }

    /* the rows: the same table rows as the page's QUALITY, without its action; then ACTIVE CONTROLLER */
    for (int i = 0; i < ps2_enh_count() && U.n < LP_ROWS - 1; i++) {
        const ps2_enh_item *it = ps2_enh_item_at(i);
        if (it->cat == PS2_ENH_QUALITY && it->apply != PS2_ENH_ACTION) U.row[U.n++] = i;
    }
    {   int ac = ps2_enh_find("active_controller");
        if (ac >= 0) U.row[U.n++] = ac; }
    U.hide = !ps2_enh_launcher_show_get();
    ps2_enh_launcher_changed_others(U.others, sizeof U.others);
    ps2_enh_launcher_gpu_line(U.gpu, sizeof U.gpu);
    {   int q = ps2_enh_find("quality_preset");
        char v[32], gp[160], tail[48] = "";
        if (L.autoplay >= 0.0) snprintf(tail, sizeof tail, ", PLAY after %.1f s", L.autoplay);
        ps2_log("launcher: shown (GPU %s, preset %s, %d rows)%s", ps2_enh_launcher_gpu_name(gp, sizeof gp),
                ps2_enh_value_name(q, v, sizeof v), U.n, tail);
        ps2_log("launcher: %s", U.gpu);
        for (int r = 0; r < U.n; r++) {
            char hb[96], val[64];
            const char *held = ps2_enh_held_by(U.row[r], hb, sizeof hb);
            const ps2_enh_item *it = ps2_enh_item_at(U.row[r]);
            int c = ps2_enh_choice(U.row[r]);
            ps2_log("launcher: row %d %s = %s%s%s%s", r, it->key, c >= 0 && !held ? it->name[c] : ps2_enh_value_name(U.row[r], val, sizeof val),
                    held ? ", held by " : "", held ? held : "", ps2_enh_launcher_changed(U.row[r]) ? ", changed last time" : "");
        }
        if (U.others[0]) ps2_log("launcher: also changed last time: %s", U.others);
    }

    int frame = 0;
    const double t_first = now_s();
    bool first_logged = false;
    double last_pump = t_first, gap_max = 0.0;
    while (!U.play && !U.quit) {
        double since = now_s() - t_first;
        if (now_s() - last_pump > gap_max) gap_max = now_s() - last_pump;
        last_pump = now_s();
        SDL_Event e;
        test_step(frame);
        while (SDL_PollEvent(&e)) event(e, since);
        if (U.quit || U.play) break;
        if (L.autoplay >= 0.0 && since >= L.autoplay) { U.play = true; U.how = "ACZ_LAUNCHER_AUTOPLAY"; break; }
        if (SDL_GetWindowFlags(L.win) & SDL_WINDOW_MINIMIZED) { SDL_WaitEventTimeout(nullptr, 100); continue; }
        bool rebuild = !V.sc;
        if (rebuild) {
            sc_destroy();
            if (!sc_create()) { SDL_WaitEventTimeout(nullptr, 100); continue; }
        }
        {   /* the page's size follows the window, whatever the desktop's scale: ImGui's 13 px x window height / 600 (F4's ui_scale on top) */
            int ww = 0, wh = 0;
            SDL_GetWindowSize(L.win, &ww, &wh);
            float k = (float)wh / 600.0f / dpi;
            k = k < 0.8f ? 0.8f : k > 3.0f ? 3.0f : k;
            ImGui::GetStyle().FontScaleMain = k * ps2_cfg.ui_scale;
        }
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        draw_page();
        ImGui::Render();

        vkWaitForFences(V.dev, 1, &V.fence, VK_TRUE, UINT64_MAX);
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(V.dev, V.sc, 100000000ull, V.acq, VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_ERROR_SURFACE_LOST_KHR) { sc_destroy(); continue; }
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { SDL_Delay(5); continue; }
        vkResetFences(V.dev, 1, &V.fence);
        vkResetCommandBuffer(V.cmd, 0);
        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(V.cmd, &bi);
        barrier(V.img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkRenderingAttachmentInfo ca = {};
        ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        ca.imageView = V.view[idx];
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color.float32[3] = 1.0f;
        VkRenderingInfo ri = {};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea.extent = V.ext;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(V.cmd, &ri);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), V.cmd);
        vkCmdEndRendering(V.cmd);
        const bool dump = dump_wanted && dump_buffer();
        if (dump) {
            barrier(V.img[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy rg = {};
            rg.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            rg.imageSubresource.layerCount = 1;
            rg.imageExtent.width = V.ext.width;
            rg.imageExtent.height = V.ext.height;
            rg.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(V.cmd, V.img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.dbuf, 1, &rg);
            barrier(V.img[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        } else
            barrier(V.img[idx], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        vkEndCommandBuffer(V.cmd);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &V.acq;
        si.pWaitDstStageMask = &ws;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &V.cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &V.done[idx];
        vkQueueSubmit(V.q, 1, &si, V.fence);
        VkPresentInfoKHR pi = {};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &V.done[idx];
        pi.swapchainCount = 1;
        pi.pSwapchains = &V.sc;
        pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(V.q, &pi);
        if (dump) {
            vkWaitForFences(V.dev, 1, &V.fence, VK_TRUE, UINT64_MAX);
            dump_write(frame);
            dump_wanted = false;
        }
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) sc_destroy();
        frame++;
        if (!first_logged) {
            first_logged = true;
            ps2_log("launcher: first frame %.0f ms after the start of main", (now_s() - t0) * 1e3);
        }
        SDL_Delay(14);                                  /* (about 60 frames a second; a mailbox swapchain does not wait) */
    }

    const double t_end = now_s();
    if (U.play) {
        int sw = ps2_enh_launcher_commit();
        ps2_log("launcher: PLAY (%s) after %.1f s, %d frames, the window's messages at least every %.0f ms: %d choices made, %d switches changed",
                U.how, t_end - t_first, frame, gap_max * 1e3, U.changed, sw);
    } else ps2_log("launcher: the window was closed after %.1f s: quitting, nothing of the game has started and nothing is saved", t_end - t_first);

    if (L.win) SDL_CaptureMouse(false);
    if (V.dev) vkDeviceWaitIdle(V.dev);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    pads_close();
    if (t_pad) { SDL_CloseJoystick(t_pad); SDL_DetachVirtualJoystick(t_pad_id); t_pad = nullptr; t_pad_id = 0; }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    vk_down();
    if (!U.play) {
        SDL_DestroyWindow(L.win);
        L.win = nullptr;
        return ST_QUIT;
    }
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_WHEEL);   /* (the keys and clicks of PLAY are no input for the game) */
    ps2_log("launcher: its Vulkan and ImGui are gone (%.0f ms); the window goes on to the game", (now_s() - t_end) * 1e3);
    return ST_PLAY;
}

void *launcher_thread(void *) {
    int st = launcher_body();
    if (st != ST_PLAY) SDL_Quit();                      /* (no half-open SDL: without the launcher the renderer starts it afresh) */
    finish(st);
    if (st != ST_PLAY) return nullptr;
    /* the window's messages stay with this thread: pump them until main() hands over the renderer's body (an event that comes now
       stays in SDL's queue for the renderer's pump_events) */
    const double t0 = now_s();
    double last = t0, gap = 0.0;
    for (;;) {
        pthread_mutex_lock(&L.m);
        void *(*job)(void *) = L.job;
        void *arg = L.job_arg;
        pthread_mutex_unlock(&L.m);
        if (job) {
            ps2_log("launcher: the window was pumped for %.0f ms while main() booted, at least every %.0f ms; this thread is the renderer now",
                    (now_s() - t0) * 1e3, gap * 1e3);
            return job(arg);
        }
        SDL_PumpEvents();
        SDL_Delay(5);
        if (now_s() - last > gap) gap = now_s() - last;
        last = now_s();
    }
}

}  // namespace

/* main(), right after the arguments: 0 = go on (the launcher was off, failed or PLAY was pressed), 1 = the window was closed: quit */
extern "C" int ps2_launcher_run(int hidden) {
    if (!ps2_enh_launcher_wanted()) return 0;
    const char *why = !ps2_enh_applied() ? "ACZ_MENU_SETTINGS is not 1, its rows would not apply" : nullptr;
    const char *ap = getenv("ACZ_LAUNCHER_AUTOPLAY"), *ts = getenv("ACZ_LAUNCHER_TEST");
    L.autoplay = ap && *ap ? atof(ap) : -1.0;
    if (!why && hidden && L.autoplay < 0.0 && !(ts && *ts)) why = "the window is hidden and ACZ_LAUNCHER_AUTOPLAY is not set: nobody could answer";
    if (why) { ps2_log("launcher: not shown: %s", why); return 0; }
    L.hidden = hidden;
    if (pthread_create(&L.th, nullptr, launcher_thread, nullptr) != 0) {
        ps2_log("launcher: not shown: no thread");
        return 0;
    }
    pthread_mutex_lock(&L.m);
    while (L.state == ST_RUNNING) pthread_cond_wait(&L.cv, &L.m);
    int st = L.state;
    pthread_mutex_unlock(&L.m);
    if (st != ST_PLAY) pthread_join(L.th, nullptr);
    return st == ST_QUIT;
}

/* renderer_main: the launcher's window, once (NULL: none): the title and the saved size the game's own window would have */
extern "C" SDL_Window *ps2_launcher_take_window(const char *title) {
    SDL_Window *w = L.state == ST_PLAY ? L.win : nullptr;
    if (!w) return nullptr;
    L.win = nullptr;
    if (title) SDL_SetWindowTitle(w, title);
    int ww = 0, wh = 0;
    SDL_GetWindowSize(w, &ww, &wh);
    if (ww != ps2_cfg.window_w || wh != ps2_cfg.window_h) SDL_SetWindowSize(w, ps2_cfg.window_w, ps2_cfg.window_h);
    return w;
}

/* ps2_video_init: the launcher's thread, which owns the window, runs the renderer's body (1), or there is none (0) */
extern "C" int ps2_launcher_take_thread(void *(*fn)(void *), void *arg, pthread_t *out) {
    if (L.state != ST_PLAY || !L.win) return 0;
    pthread_mutex_lock(&L.m);
    L.job_arg = arg;
    L.job = fn;
    *out = L.th;
    pthread_mutex_unlock(&L.m);
    return 1;
}
