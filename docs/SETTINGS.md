# The settings: three layers

The settings review of 2026-11-05 (bug #317, `a private script`). Why: "when I go into the menu I feel I
am getting lost in the settings".

**The rule.** A row stays on the in-game page only if a player sees, hears or feels the difference within ten seconds of
flying AND two reasonable players would choose differently. Everything else is an ini key: still injected at start exactly
as before, still in `docs/ENV_REFERENCE.md`; nothing is lost.

| Layer | What it holds | Where |
|---|---|---|
| 1. The launcher | the rows that need a restart anyway, before the engine boots, with PLAY | the game's window, before the boot (`ACZ_LAUNCHER`, below); its list is the QUALITY category and ACTIVE CONTROLLER |
| 2. The page | 7 categories, none over 15 rows (CONTROLS 18 with a joystick and Touch), one vocabulary, one preset, one reset; most rows take effect at the next mission (below) | pause menu > ENHANCEMENTS, the title's OPTIONS > ENHANCEMENTS, View + Menu; F4 > Enhancements on the window |
| 3. The ini | the expert keys (`[ini]` in the file's comment) | `build/zero/ac5_settings.ini`; F4 > Enhancements > Expert (ini) lists them for a desktop session with a mouse; the headset page never shows them |

ENV_REFERENCE documents every switch (about 1580); the page shows the small part of them that a player tells apart.

## The counts

| | Before (0d091ca) | After |
|---|---|---|
| Rows in the table | 128 | 141: 128 as before + 12 combo rows + MARKER SIZE BY RANGE (bug #316) |
| Categories | 10 | 7 |
| On the page, always | 126 | 73, of them 12 combo rows |
| On the page, conditional | 2 (a joystick) | 12: 2 with a joystick, 10 once a Touch controller was seen |
| ini only | 0 | 28 |
| Single rows folded into combo rows | 0 | 28, into 12 rows (16 rows saved) |
| When the page's rows apply (the 85 shown at most) | 25 now, 98 next start, 5 actions (all 128) | 20 now, 50 next mission, 10 next start, 5 actions |
| When all 141 rows apply | - | 29 now, 75 next mission (+ TERRAIN LOOK's field), 32 next start, 5 actions |
| Lines of `ac5_settings.ini` (the owner's) | 232 lines, `[enhancements]` 107 keys | 218 lines, `[enhancements]` 94 keys (after the first save) |

Per category: QUALITY 8, PICTURE 12, COCKPIT 15, HUD 10, COMFORT 14, SOUND 8, CONTROLS 6 + 2 + 10.

## The vocabulary

Three-step rows read ON / SUBTLE / OFF, ON first, wherever the switch has a half value. Two-step rows read ON / OFF, ON first,
unless the names carry meaning (GAME / VOLUMETRIC, COCKPIT / GAME, HIDDEN / SHOWN). SCRATCHES has no half value: ON / STRONG /
OFF. Percentages only where the row is a level (CLOUD AROUND YOU, WEATHER RUMBLE, WEATHER SOUND, DOPPLER, MUFFLING, ENGINE,
SYSTEM SOUNDS, RUMBLE). Help lines: one sentence, not starting with the row's name; every one fits the overlay's 3 lines and
the native panel's 4 with " * Saved: restart the game to apply." (or the shorter " Saved for next mission or RETRY.") added (`ACZ_DEBUG_ENH=1`, `ACZ_DEBUG_PAUSE=1` measure them
with the real fonts); category help lines fit one line of both.

## When a change takes effect

The file is written at once, whatever the row. "Applies" in the tables below:

| Applies | What happens | On the page | In F4 |
|---|---|---|---|
| **now** | the row is live | "Saved. In effect now." | - |
| **next mission** | nothing in this attempt; from the next mission or RETRY on (package B, bug #318) | `+` beside the value, "Saved. Takes effect at the next mission or RETRY." (the four rows below: "when the next mission starts"), the category's heading counts it ("3 CHANGES AT THE NEXT MISSION") | "(next mission)", the top of the tab counts them |
| **next start** | what the swapchain, the pipelines and the packs are built from: at the next start of the game | `*`, "Saved. Restart the game to apply.", the RESTART THE GAME notice when leaving the page | "(restart)" |

**What "next mission" means to a player.** The next time the game enters a mission: the next mission of a campaign, a mission started from the menus after QUIT MISSION or from the title, a free mission. Also the very first mission of a session, for a row changed at the title's OPTIONS > ENHANCEMENTS. And a new attempt at the mission: the pause menu's RETRY, and RESTART after MISSION FAILED (the owner's answer, 2026-11-05).

**A RETRY counts as a next mission**, with four rows that cannot honestly take effect there, because the game does not enter the mission and the map stays loaded. They keep waiting for the game to enter a mission: `+` stays, the row's help says "next mission" without RETRY, and the log gives the reason (`mission: start_view keeps waiting for the next mission (a RETRY / RESTART: ...)`). A combo row waits whole.

| Row | Why it waits through a RETRY |
|---|---|
| START VIEW | set when the game enters a mission; a RETRY keeps the view the player has |
| BUILDINGS | the houses are built with the map, and a RETRY does not load it: built now, they would pop in under the running mission |
| TERRAIN LOOK | the relief is baked with the map, and it would pop in the same way |
| RECENTRE AT TAKE-OFF | a RETRY has no take-off recentre |

The menu after a mission's end has a second way to restart (its other menu, id 1, return address 0x1D43FC). It was never seen in any run, so it publishes nothing and what is saved goes on waiting: the log says `mission: no edge at this restart`. A stage change within a mission, a respawn that only restores the aircraft and the crash replay do not run the mission (re)start, so nothing happens in flight. `ACZ_MENU_RETRY_EDGE=0`: RETRY publishes nothing (the words still say RETRY).

**Changed and changed back** before the next mission or RETRY: nothing waits, it is untouched. **Held rows** (a switch set in the environment, a launcher word) stay held: the row is locked, as before. **Back to a default**: the row's built-in default, also when the saved value was set at the start.

**A row that started off.** These can go off at a mission and on again at another, because what they need was made at the start. Turning one on that was off when the game started needs a restart: the row then says so (`*`), and it takes effect at the next start.

| Row (a combo row's member) | Off value at the start | What was not made |
|---|---|---|
| SHADER WATER | OFF | the water's images and shader, its map hook |
| TERRAIN LOOK (`ground_materials`) | GAME | the materials' images and shaders |
| BUILDINGS (`town_houses`) | GAME | the hooks, the city's shader |
| BUILDINGS (`city_buildings`) | TOWNS | the city's shader |
| EXPLOSIONS (`explosion_burst`) | not WITH VOLUMES | the volume's pipeline, its hook |
| FOREST DISTANCE | NORMAL | the draw's hook |
| EYE ADAPTATION | OFF | its pipeline |
| FLASHES LIGHT THE COCKPIT | OFF | the game hooks |
| CANOPY DAMAGE | OFF | the glass's triangles at the cockpit's load |
| GLASS REFLECTIONS (`canopy_reflections`) | OFF | the interior cube baked at the cockpit's load |
| HUD PLACEMENT | FREE, or BEHIND FRAMES for IN THE GLASS | the combiner's meshes, the pipelines' depth bounds |
| GAZE TARGETING | OFF | its hooks |
| G EFFECTS ON VISION | OFF | the veil's pipeline |
| BLASTS ON THE CANOPY | OFF | its hook |
| CAMERA SHAKE ON HITS | ON | the hook that takes the shake out |
| CLOSE FLY-BYS | OFF | its hooks and mixer |
| TRAILS AND VAPOUR, with `heat_haze` | all four off | the trails' pipelines and sampler |

**How it works.** `runtime/include/ps2_menv.h`. The page records the row; the environment is never written after the start (UCRT's `_putenv_s` frees a string an earlier `getenv` returned, and these switches are read on five threads). At `GameControl::enter`, and at a new attempt at the mission (re)start 0x1D1F00 (`zero_env_reload_mission`, zero_vr.c; `enh_retry_tab` in ps2_settings.c names the rows that wait there), the rows changed since are published in a table of string literals and a serial moves on; readers ask `ps2_menv` instead of `getenv`, and read their statics again when the serial moved: the game thread at once, the renderer and the graphics thread at their next frame, the audio thread in its next chunk. At a RETRY the first flight frame can come one field after the edge (M4 has no intro): what holds is the order, each thread reloads before its first frame after the edge, and what was recorded before it is the pause menu's frame. Log: `settings: ENHANCEMENTS: <row> = <v> (next mission)`, `mission: <row>: <old> -> <new> (<switch>=<v>)`, `mission: reloaded N switches` (`at a RETRY / RESTART`), `mission: <row> keeps waiting for the next mission (...)`, `mission: <switch> <old> -> <new>`, `mission: renderer reload N us`. With nothing changed: `mission: reloaded 0 switches` and nothing is read again.

## Layer 1: the launcher

`runtime/src/ps2_launcher.cpp`, `ps2_enh_launcher_*` in `runtime/src/ps2_settings.c`. Before the engine boots the game's window shows:

- the QUALITY rows, the page's own table rows (`cat == PS2_ENH_QUALITY` without RESET ALL): QUALITY PRESET, RESOLUTION, EDGE SMOOTHING, CLOUDS, 3D COCKPIT, SMOOTH MOTION, MOVIES;
- ACTIVE CONTROLLER, with the connected pads by name (SDL's list, without flight sticks and throttles, as the controller registry's);
- the GPU line ("Your RTX 3090 Ti: HIGH.", the same words as the QUALITY PRESET help on the page; on a new ini it adds that the preset was chosen for this GPU);
- "Do not show this again"; the focused row's help line; PLAY.

**Input.** Up / Down or a pad's D-pad / left stick: a row. Left / Right: its value. PLAY: Enter, Space, a pad's A / Cross, the mouse. The mouse also picks a value and the checkbox. Closing the window quits: nothing of the game has started, nothing is saved.

**When it comes.** `main()`'s first call after the arguments: only the settings constructor has run, no reader of a switch, no thread, not the log's first line. So every QUALITY row takes effect in the same boot. PLAY writes the switches the constructor would have written from a file that says so (a switch set in the environment stays: the row is greyed "held"; `""` removes a switch), sets the run's state (the page then shows no `*`), rebuilds the start-up log's list and saves, only if something changed. QUALITY PRESET writes its whole sub-table, the ini-only ladder rows too; EDGE SMOOTHING its members.

| Row | Switch(es) | Read first, in the boot after PLAY |
|---|---|---|
| QUALITY PRESET | its rows: `PS2_UPSCALE`, `ACZ_TREES_DIST`, `ACZ_VR_GOVERNOR`, `ACZ_VR_CLOUD_DETAIL`, `ACZ_VR_CANOPY_QUALITY`, `ACZ_VR_COCKPIT_DETAIL` | as RESOLUTION; `zero_vr_init`; the renderer's governor, cloud, canopy and cockpit setup |
| RESOLUTION | `PS2_UPSCALE` | `choose_scale`, first call in `vk_init`'s `create_targets`; `pick_scale` at the first movie |
| EDGE SMOOTHING | `PS2_EDGE_AA`, `PS2_EDGE_AA_PARAM` | `edge_aa_init`, in `renderer_main` |
| CLOUDS | `ACZ_VR_CLOUDS` | `ps2_clouds_cfg`, first call in `zero_clouds_init` (`main`) or `vk_init`, whichever comes first |
| 3D COCKPIT | `ACZ_VR_COCKPIT_MODEL`, `ACZ_VR_COCKPIT_MODELS` | `zero_vr_init`'s start view; `cpit_load`, in `renderer_main` |
| SMOOTH MOTION | `PS2_XR_RERENDER` | `lw_cfg`, `xr_rerender_wanted`, `pace_clock_on`: the renderer |
| MOVIES | `ACZ_FMV_PACK` | `fm_init`, the first disc read of the game |
| ACTIVE CONTROLLER | `ACZ_PAD_ACTIVE` | `pad_force_read`, in `pad_init` (`renderer_main`) |

**What it leaves behind.** Nothing but the switches, the file and the OS window. Its Vulkan instance, device and swapchain and its ImGui context are its own and gone before `main()` goes on; the gamepad subsystem is closed, so the renderer opens it as in a boot without the launcher. It makes **no OpenXR call**: the instance, the system and the session come in `vk_init`, after PLAY, in the order and with the failure handling of a boot without it, and the headset stays idle until then. The window stays, on the launcher's thread, which becomes the renderer thread (a window's messages belong to the thread that made it).

**The file.** Two keys in `[enhancements]`, no table rows, no switches: nothing in the injected list moves.

| Key | Values | Meaning |
|---|---|---|
| `launcher` | `1` (default), `0` | `0`: the checkbox "Do not show this again". F4 > Quick > "Launcher before the game" brings it back, and with it off the QUALITY category's help on the page says so. `ACZ_LAUNCHER` wins over it, either way |
| `launcher_changed` | keys, comma separated | Written by every save: the rows that wait for a restart at that moment (the page's `*`; QUALITY PRESET when one of its rows does; a row changed and changed back is not in it). The launcher marks them "changed last time", QUALITY rows in place, others in one line under the rows; PLAY clears it. Closed without PLAY, it stays |

**Not shown** (logged, the game starts as without it): `ACZ_LAUNCHER=0`, `launcher = 0`; no `ACZ_MENU_SETTINGS=1` (its rows would not apply in that run); `--hidden` unless `ACZ_LAUNCHER_AUTOPLAY` or `ACZ_LAUNCHER_TEST` is set (nobody could answer); `--novideo`, the self-tests, `--autoplay`, `--stop-scene`, `--capture*`, `--frames`, `--vblanks`; no window or no Vulkan 1.3 device.

**The owner's scripts** set `ACZ_LAUNCHER=0` (D6 of the review): `play_zero_vr.sh`, `play_zero_desktop.sh`, `run_zero_mission.sh`, `run_zero_soak.sh`, `bench_flat.sh`, `sweep_missions.sh`, `run_test.sh`, `tools/scratch/run_cpit.sh`, `tools/scratch/run_mission_any.sh`. `tools/release/scrub_rules.txt` turns `play_zero_vr.sh`'s default to 1 in the public tree. **The restart prompt stays** (RESTART THE GAME TO APPLY / QUIT GAME NOW); after QUIT GAME NOW the next start lands on the launcher with the marks; the runtime never relaunches itself.

## Paths: everything beside the exe

One rule (setup_1105, 2026-11-05): a player's install is per user and writable, so what the game reads and writes lies next to `ac5.exe`:

| What | Where, next to the exe |
|---|---|
| The settings | `ac5_settings.ini` (`PS2_SETTINGS_FILE` moves it) |
| The memory cards | `saves/` (`PS2_SAVE_DIR`) |
| The cockpit set | `cockpits/`, with `cockpits/map.txt`; `cockpits/env.txt`, when a set has one: `ACZ_...=value` lines, its per-model switches, each only when unset |
| The upscaled movies, their decoder | `out/fmv/` (`ACZ_FMV_DIR`), `ffmpeg.exe` (`ACZ_FMV_FFMPEG`) |
| The ground textures | `out/terrain_t5/` (`ACZ_TERRAIN_TEXPACK_DIR`) |
| The generated code and the image | `gen_zero/` (`--data` moves it) |
| Crash reports | `crash/`, else `%TEMP%\ac5_crash` |
| The log | `logs/zero_play_<stamp>.log`, the 8 newest kept, when no script redirects stdout |
| The shaders, the pipeline cache | `shaders/`, `ac5_pipelines.cache`, `ac5_pipelines.keys` |
| The texture glyphs and packs, the joystick profiles | `textures/`, `profiles/joystick/` |

**An asset folder.** A folder `ac0vr-assets` beside the setup exe, laid out like this table, is copied in by the setup: only newer files on an install over an install. With its ground textures the row GROUND TEXTURES starts at `x4` (a value already in the settings file stays). The uninstaller's second question decides about these files. Without the folder the game runs on what the setup holds. The upscaled movies need no NVIDIA card: when ffmpeg stops before the first frame with `ACZ_FMV_HWACCEL`'s decoder, the CPU decodes them from then on.

**The home folder.** A player's start makes the exe's folder the current folder, so the relative paths above just work. The one exception is a developer's build: when there is no `gen_zero/ps2_image.bin` beside the exe but there is one two folders up (`build/zero/ac5.exe` in a source tree), the tree's root is the home folder, and `saves/`, `cockpits/` and `logs/` lie there, as with the scripts. The log's second line names it.

**The disc.** `[paths] disc =` in the settings file: an ISO image or a folder with the disc's files. The setup program writes it (the installer's disc page, `ac0vr.exe`'s picker). When it is not set, or the file is gone, the launcher asks again (its two buttons, ISO IMAGE and DISC FOLDER; PLAY waits; also with `launcher = 0`), and a `--disc` on the command line wins for that start. `[paths] xr_runtime =` names an OpenXR runtime's json without a shell, for Virtual Desktop or SteamVR on a PC that also has Meta's app; the default is below. A save keeps the sections `[paths]` and `[update]` as they are: the setup program owns them too. A value runs to the line's end, so a folder name may hold `;` and `#`.

**Names outside ASCII.** Every path in the runtime is UTF-8, whatever the Windows code page is: `runtime/include/ps2_path.h` is forced into the runtime's C files and routes `fopen`, `remove`, `rename`, `_stat64`, `_mkdir` and `opendir` through `ps2_path.c`'s `_wfopen` and friends; the arguments come from `CommandLineToArgvW`, and `ACZ_*` / `PS2_*` values outside ASCII are converted at the start. SDL's file functions, the OpenXR loader and the crash report were wide or UTF-8 before. A name that is pure ASCII takes the C library's own function, as before. Tested in a folder `тест ÄÖ` on code page 1252: the settings, the shaders, the pipeline cache, the log, the disc through a folder with such a name, an absolute `PS2_SAVE_DIR`.

**Who started me.** `--data` on the command line means a script: every script of this tree passes it, and nothing below happens, so their runs are as they were but for the log's first line. So does any option but `--mode`, `--disc`, `--watchdog`, `--hidden` and `--verbose` (a self-test, `--novideo`, `--capture`...). Everything else is a player's start. `ACZ_PROFILE=script` or `=player` overrides this. `ac5.exe --version` prints the version and exits, whatever else is on the line.

**A player's start** does what `play_zero_vr.sh sharp native cockpit smooth modern` does: `runtime/src/ps2_profile.c`, a constructor that runs before the settings file is read. Each entry applies only when the variable is not set. A console that nobody else shares goes away. The watchdog is off, as the script's `--watchdog 0`.

| Variable | Value | `--mode` |
|---|---|---|
| `ACZ_MENU_SETTINGS` | `1` | all |
| `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS` | `1` | all |
| `PS2_RN_VP` | `1` | all |
| `ACZ_VR_COCKPIT_MODEL`, `ACZ_VR_COCKPIT_MODELS` | `cockpits/f5e_t38.cpit` when it is there (else the map's first file), the map from `cockpits/map.txt` | all |
| `PS2_XR`, `PS2_FIELD_HZ` | `1`, `60` | not flat |
| `PS2_XR_MODE` | `stereo` / `mono` / `cinema` | sharp (the default) and stereo / mono / cinema |
| `ACZ_VR_HEADLOOK`, `PS2_XR_MULTIVIEW` | `stick`, `1` | sharp |
| `PS2_UPSCALE` | `3` | not sharp (sharp: the row RESOLUTION) |
| `PS2_XR_RUNTIME` | `[paths] xr_runtime`, else Meta's json when it is installed, else unset (the system's active runtime). Nothing when `XR_RUNTIME_JSON` is set | not flat |

Three words of that command are not in the table, because a saved row owns each of them and an exported switch would lock the row for the player: SMOOTH MOTION (`PS2_XR_RERENDER`, default 1), CONTROLLER LAYOUT (`ACZ_PAD_LAYOUT`, default modern with `ACZ_MENU_SETTINGS`) and the launcher (`ACZ_LAUNCHER`; the file's `launcher`, default 1, whose checkbox would be dead). On a new settings file they come out the same as the words.

## The page: every row

settings: loaded out\settings_test\md.ini
settings: in use -- preset low, internal resolution 1, texture filter 0, anisotropy 0, windowed 1280x896, aspect 0, widescreen 0, present mailbox, fps limit 0
settings: ENHANCEMENTS from out\settings_test\md.ini (ACZ_MENU_SETTINGS=1): 16 switches set, 0 kept from the environment: PS2_UPSCALE=4 ACZ_VR_CLOUDS=all PS2_XR_RERENDER=1 ACZ_LOD_BIAS=4 ACZ_TERRAIN_TEXPACK=faithful ACZ_TREES_DIST=2 ACZ_VR_HUD_SCALE=1.3 ACZ_TERRAIN_DETAIL=2 ACZ_VR_GOVERNOR=0 PS2_PERF_RECORD_EARLY=0 PS2_EDGE_AA=1 PS2_EDGE_AA_PARAM=0.125,0.0312,0.25 ACZ_TERRAIN_DETAIL_HOLD=0.6 ACZ_TERRAIN_PROPS=2 ACZ_IMM_BREATH=1,calm ACZ_IMM_GSUIT=1
settings: QUALITY PRESET saved HIGH, in effect HIGH; it sets RESOLUTION 2X / 3X / 4X, FOREST DISTANCE NORMAL / FAR / FAR, FRAME-TIME GOVERNOR ON / ON / OFF, CLOUD DETAIL LOW / MEDIUM / HIGH, CANOPY QUALITY LOW / MEDIUM / HIGH, COCKPIT DETAIL LOW / MEDIUM / HIGH
settings: QUALITY PRESET: a new install: GPU detection off (ACZ_GPU_DETECT=0), every row at its default (HIGH)
settings: controller layout modern ( RT/LT = R1/L1 analog throttle, RB/LB = R2/L2 yaw)

### QUALITY

| Row | Key | Switch(es) | Applies | Default | Choices |
|---|---|---|---|---|---|
| QUALITY PRESET | `quality_preset` | `ACZ_QUALITY_PRESET` | next start | HIGH | LOW / MEDIUM / HIGH |
| RESOLUTION | `resolution` | `PS2_UPSCALE` | next start | 4X | 2X / 3X / 4X / 5X |
| EDGE SMOOTHING | `edge_level` | PS2_EDGE_AA, PS2_EDGE_AA_PARAM | next start | LIGHT | OFF / LIGHT / STRONG |
| CLOUDS | `clouds` | `ACZ_VR_CLOUDS` | next start | VOLUMETRIC | GAME / VOLUMETRIC |
| 3D COCKPIT | `cockpit_3d` | `ACZ_VR_COCKPIT_MODEL` | next start | ON | ON / OFF |
| SMOOTH MOTION | `smooth_motion` | `PS2_XR_RERENDER` | next start | ON | ON / OFF |
| MOVIES | `movies` | `ACZ_FMV_PACK` | next start | UPSCALED | UPSCALED / ORIGINAL |
| RESET ALL TO DEFAULTS | `reset_all` | - | action | - | PRESS A |

### PICTURE

| Row | Key | Switch(es) | Applies | Default | Choices |
|---|---|---|---|---|---|
| AIRCRAFT DETAIL | `aircraft_detail` | `ACZ_LOD_BIAS` | now | VERY HIGH | NORMAL / HIGH / VERY HIGH |
| TERRAIN LOOK | `terrain_look` | ACZ_TERRAIN_REAL, ACZ_TERRAIN_MATERIALS, ACZ_TERRAIN_DETAIL_HOLD | next mission, not a RETRY | REALISTIC | GAME / REALISTIC |
| GROUND TEXTURES | `ground_textures` | `ACZ_TERRAIN_TEXPACK` | next start | FAITHFUL 2X | GAME / FAITHFUL 2X / CRISP 2X / REAL 4X |
| SHADER WATER | `shader_water` | `ACZ_TERRAIN_WATER` | next mission | ON | ON / OFF |
| BUILDINGS | `buildings` | ACZ_TERRAIN_PROPS, ACZ_TERRAIN_CITY | next mission, not a RETRY | TOWNS AND CITIES | GAME / TOWNS / TOWNS AND CITIES |
| TRAILS AND VAPOUR | `trails_vapour` | ACZ_VR_CONTRAILS, ACZ_VR_CLOUD_WAKE, ACZ_VR_VAPOUR | next mission | ON | ON / SUBTLE / OFF |
| SMOKE | `smoke` | ACZ_VR_SMOKE_VOLUME, ACZ_VR_SMOKE_LIGHT | next mission | ON | ON / SUBTLE / OFF |
| EXPLOSIONS | `explosions` | ACZ_VR_FX_BILLBOARD, ACZ_VR_FX_SOFT, ACZ_VR_FX_BURST | next mission | SOFT WITH VOLUMES | GAME / SOFT / SOFT WITH VOLUMES |
| FOREST DISTANCE | `forest_distance` | `ACZ_TREES_DIST` | next mission | FAR | NORMAL / FAR |
| CLOUD SHADOWS | `cloud_shadow_reach` | ACZ_VR_CLOUD_SHADOW, ACZ_VR_CLOUD_SHADOW_GROUND | now | EVERYWHERE | OFF / ON THE AIRCRAFT / EVERYWHERE |
| OPTICAL PHENOMENA | `optics` | `ACZ_VR_OPTICS` | now | ON | ON / OFF |
| EYE ADAPTATION | `eye_adapt` | `ACZ_VR_EYE_ADAPT` | next mission | ON | ON / SUBTLE / OFF |

### COCKPIT

| Row | Key | Switch(es) | Applies | Default | Choices |
|---|---|---|---|---|---|
| START VIEW | `start_view` | `ACZ_VR_START_VIEW` | next mission, not a RETRY | COCKPIT | COCKPIT / GAME |
| COCKPIT SCREENS | `cockpit_screens` | `ACZ_VR_MFD` | next mission | ALL | ALL / NO BIG MAP / OFF |
| SHADOWS FROM THE SUN | `cockpit_sun` | `ACZ_VR_COCKPIT_KEY_LIGHT` | next mission | SUN | SUN / GAME |
| LIGHT FROM THE WORLD | `cockpit_scene_light` | `ACZ_VR_COCKPIT_SCENE_LIGHT` | next mission | ON | ON / SUBTLE / OFF |
| FLASHES LIGHT THE COCKPIT | `flash_light` | `ACZ_VR_FLASH_LIGHT` | next mission | ON | ON / SUBTLE / OFF |
| CLOUD LIGHT INSIDE | `cloud_light` | `ACZ_IMM_CLOUD_LIGHT` | now | ON | ON / SUBTLE / OFF |
| STICK AND THROTTLE MOVE | `cockpit_controls` | `ACZ_VR_COCKPIT_CONTROLS` | next mission | ON | ON / OFF |
| REAR-VIEW MIRRORS | `cockpit_mirrors` | `ACZ_VR_COCKPIT_MIRRORS` | next start | TONE | TONE / LIVE / SKY / OFF |
| WATER ON THE CANOPY | `canopy_water_level` | ACZ_VR_CANOPY, ACZ_VR_CANOPY_GAIN | now | NORMAL | OFF / LIGHT / NORMAL / HEAVY |
| MIST AND CONDENSATION | `canopy_mist` | (its field in the file) | now | ON | ON / OFF |
| ICE ON THE CANOPY | `canopy_ice` | `ACZ_VR_CANOPY_ICE` | now | ON | ON / OFF |
| GRIME FROM DEBRIS | `canopy_grime` | (its field in the file) | now | ON | ON / OFF |
| SCRATCHES ON THE CANOPY | `canopy_scratches` | `ACZ_VR_CANOPY_SCRATCH` | next mission | ON | ON / STRONG / OFF |
| GLASS REFLECTIONS | `glass_reflections` | ACZ_VR_CANOPY_GLARE, ACZ_VR_CANOPY_REFLECT | next mission | ON | ON / OFF |
| CANOPY DAMAGE | `canopy_cracks` | `ACZ_VR_CANOPY_CRACKS` | next mission | ON | ON / SUBTLE / OFF |

### HUD

| Row | Key | Switch(es) | Applies | Default | Choices |
|---|---|---|---|---|---|
| HUD SIZE | `hud_size` | `ACZ_VR_HUD_SCALE` | next mission | 1.3X | 1X / 1.15X / 1.3X / 1.5X |
| HUD PLACEMENT | `hud_placement` | ACZ_VR_HUD_OCCLUDE, ACZ_VR_HUD_GLASS | next mission | IN THE GLASS | FREE / BEHIND FRAMES / IN THE GLASS |
| HUD ITEMS ON GAUGES | `hud_gauges` | `ACZ_VR_HUD_DROP` | next mission | HIDDEN | HIDDEN / SHOWN |
| TARGET ARROW | `target_arrow` | `ACZ_VR_HUD_ARROW` | next mission | HELMET | HELMET / AIRCRAFT / COCKPIT |
| ARROW PLACE | `arrow_place` | `ACZ_VR_HUD_ARROW_RING` | next mission | NEAR CENTRE | NEAR CENTRE / AT THE EDGE |
| TARGET BOX SIZE | `box_size` | `ACZ_VR_MARKER_SCALE` | next mission | 1X | 1X / 1.25X / 1.5X / 2X |
| SHARP TARGET BOXES | `sharp_boxes` | `ACZ_VR_MARKER_SHARP` | next mission | SMOOTH | SMOOTH / PIXELS / OFF |
| MARKER SIZE BY RANGE | `marker_range` | `ACZ_VR_MARKER_RANGE` | next mission | BY RANGE | GAME / BY RANGE |
| WINGMAN COMMANDS | `wingman_place` | `ACZ_VR_HUD_WING` | next mission | BESIDE THE SCREENS | BESIDE THE SCREENS / GAME'S PLACE |
| GAZE TARGETING | `gaze_target` | `ACZ_VR_GAZE_TARGET` | next mission | 20 DEG | OFF / 10 DEG / 20 DEG / 30 DEG |

### COMFORT

| Row | Key | Switch(es) | Applies | Default | Choices |
|---|---|---|---|---|---|
| G EFFECTS ON VISION | `g_vision` | `ACZ_IMM_GLOC` | next mission | ON | ON / SUBTLE / OFF |
| BUFFET | `buffet` | `ACZ_IMM_BUFFET` | next mission | ON | ON / SUBTLE / OFF |
| BLASTS ON THE CANOPY | `blasts` | `ACZ_IMM_BLAST` | next mission | ON | ON / SUBTLE / OFF |
| FLYING THROUGH DEBRIS | `debris` | `ACZ_IMM_DEBRIS` | next mission | ON | ON / OFF |
| HIT EFFECTS | `hit_effects` | `ACZ_IMM_HIT_FX` | next mission | ON | ON / OFF |
| CAMERA SHAKE ON HITS | `vr_camera_shake` | `ACZ_VR_CAMERA_SHAKE` | next mission | OFF IN VR | ON / OFF IN VR |
| CLOUD AROUND YOU | `cloud_volume` | `ACZ_IMM_VEIL` | now | 100% | OFF / 50% / 100% / 150% / 200% |
| WEATHER RUMBLE | `rumble` | `ACZ_IMM_RUMBLE` | now | 100% | OFF / 50% / 100% / 150% / 200% |
| PILOT SOUNDS | `pilot_sounds` | ACZ_IMM_BREATH, ACZ_IMM_GSUIT, ACZ_IMM_STRAIN | next mission | ALWAYS | ALWAYS / UNDER EFFORT / OFF |
| STEREO DEPTH | `stereo_depth` | `ACZ_VR_STEREO_SCALE` | next mission | 1X | 0.8X / 0.9X / 1X / 1.1X / 1.2X |
| FLAT MISSION INTRO | `flat_intro` | `ACZ_VR_INTRO_FLAT` | next mission | ON | ON / OFF |
| FLAT CUTSCENES | `flat_events` | `ACZ_VR_EVENT_FLAT` | next mission | ON | ON / OFF |
| RECENTRE AT TAKE-OFF | `takeoff_recentre` | `ACZ_VR_NO_AUTO_RECENTER` | next mission, not a RETRY | ON | ON / OFF |
| RECENTRE VIEW NOW | `recentre_now` | - | action | - | PRESS A |

### SOUND

| Row | Key | Switch(es) | Applies | Default | Choices |
|---|---|---|---|---|---|
| 3D SOUND (HEADPHONES) | `binaural_sound` | `ACZ_AUDIO_HRTF` | now | AUTO | AUTO / ON / OFF |
| DOPPLER EFFECT | `doppler` | `ACZ_AUDIO_DOPPLER` | now | 25% | OFF / 10% / 25% / 50% / 100% |
| COCKPIT MUFFLING | `cockpit_muffle` | `ACZ_AUDIO_MUFFLE` | now | 100% | OFF / 50% / 100% |
| GRANULAR ENGINE | `engine_grain` | `ACZ_IMM_ENGINE_GRAIN` | now | OFF | OFF / 50% / 100% |
| WEATHER SOUND | `sound` | `ACZ_IMM_SOUND` | now | 100% | OFF / 50% / 100% / 150% / 200% |
| COCKPIT SYSTEM SOUNDS | `system_sounds` | `ACZ_SND_SYSTEMS` | next mission | 100% | OFF / 50% / 100% / 150% |
| WARNING TONES FROM THE PANEL | `warning_panel` | `ACZ_SND_WARN_PLACE` | next mission | ON | ON / OFF |
| CLOSE FLY-BYS | `flyby` | `ACZ_SND_FLYBY` | next mission | ON | ON / SUBTLE / OFF |

### CONTROLS

| Row | Key | Switch(es) | Applies | Default | Choices |
|---|---|---|---|---|---|
| ACTIVE CONTROLLER | `active_controller` | `ACZ_PAD_ACTIVE` | now | AUTO | AUTO / TOUCH |
| CONTROLLER LAYOUT | `pad_layout` | `ACZ_PAD_LAYOUT` | now | MODERN | MODERN / CLASSIC |
| RUMBLE STRENGTH | `rumble_level` | ACZ_RUMBLE_GAIN, ACZ_TOUCH_HAPTICS | now | AUTO | AUTO / 50% / 100% / 150% / 200% |
| ADAPTIVE TRIGGERS | `adaptive_triggers` | `ACZ_PAD_TRIGGERS` | now | ON | ON / SUBTLE / OFF |
| TEST VIBRATION | `rumble_test` | - | action | - | PRESS A |
| STICK DEAD ZONE | `stick_dead_zone` | `ACZ_PAD_STICK_REMAP` | next mission | THE GAME'S | THE GAME'S / NONE |
| THROTTLE LEVER | `throttle_lever` | `ACZ_HOTAS_THROTTLE` | now | CENTRED | CENTRED / SPRING / SPLIT |
| SET LEVER NEUTRAL | `lever_neutral` | - | action | - | PRESS A |
| TOUCH CONTROLLERS | `touch_controllers` | `ACZ_TOUCH` | next start | AUTO | AUTO / AS A PAD / HANDS ON STICK / OFF |
| GRAB WITH | `touch_grab` | `ACZ_TOUCH_GRAB` | next mission | EITHER | GRIP / TRIGGER / EITHER |
| GRIP | `touch_grip` | `ACZ_TOUCH_GRIP` | next mission | HOLD | HOLD / TAP TO TAKE |
| STICK THROW | `stick_throw` | `ACZ_TOUCH_STICK_THROW` | next mission | AS THE COCKPIT | AS THE COCKPIT / 15 DEGREES / 25 DEGREES / 30 DEGREES |
| STICK RESPONSE | `stick_response` | `ACZ_TOUCH_STICK_EXPO` | next mission | SOFT CENTRE | LINEAR / SOFT CENTRE / VERY SOFT |
| THROTTLE | `touch_throttle` | `ACZ_TOUCH_THROTTLE` | next mission | LEVER STAYS | LEVER STAYS / LEVER SPRINGS BACK / TILT / THUMBSTICK |
| RIGHT THUMBSTICK | `right_thumbstick` | `ACZ_TOUCH_RS` | next mission | D-PAD | D-PAD / LOOK |
| LEFT-HANDED | `left_handed` | `ACZ_TOUCH_SWAP` | next mission | OFF | ON / OFF |
| PILOT HANDS | `pilot_hands` | `ACZ_VR_HANDS` | next mission | AUTO | AUTO / ON / OFF |
| SHOW TOUCH CONTROLS | `touch_card` | - | action | - | PRESS A |

## Updates

`tools/setup/ac0vr_update.c` (setup_1105), part of `ac0vr.exe`. At start, and from its CHECK FOR UPDATES button, it asks GitHub's API for the public repository's latest release, through WinHTTP, 3 seconds in all. Offline, rate-limited or no release yet: silent, the game starts. A `tag_name` newer than `version.txt` opens the window with UPDATE, the release's name and the first lines of its notes; the game can also be started as it is.

| Switch | Default | Meaning |
|---|---|---|
| `[update] check` in the settings file | `1` | `0`: no question at start; the button still asks. The game keeps this section as it is |
| `ACZ_UPDATE_CHECK` | unset | `0` / `1` wins over the file |
| `ACZ_UPDATE_URL` | that address | another JSON of the same shape; `file:///` works for it and for its assets (the tests) |

**The update.** `ac0vr-<version>-update.zip` and `SHA256SUMS` from the release's assets go to `staging.dl/`; the zip's hash must be in `SHA256SUMS` (a damaged download is refused and nothing changes; the hash comes from the same place as the zip, so it is no signature). It is unpacked into `staging/` (miniz, `third_party/miniz`; a name that leaves the folder is refused), and only then swapped into place: first what the zip's folders no longer hold goes away, then each file that differs moves in, so a file that stays keeps its date for the compile stage and the pipeline cache; `version.txt` last. `ac0vr.exe` replaces itself through a renamed `ac0vr.exe.old`, removed at the next start. Then the first-run build with its cache: a runtime-only release relinks in seconds, a recompiler release rebuilds. Progress and the result are in the same window.

**What can fail.** Nothing under `build/zero/` changes before the new exe is linked, so a failed download, a damaged zip, a full disk or a failed build leaves the previous build runnable (the window offers it). Its log still says the old version: `version.txt` beside the exe moves with the link.

**The compiler** comes only when `toolchain_id` in `bundle.txt` changes: then the asset `ac0vr-toolchain-<id>.zip` (`ACZ_RELEASE_TOOLCHAIN_ASSET=1` for `make_release.sh`) is fetched and verified too. Python stays as installed.

**A release** (the owner's: the web page; no token is stored anywhere): tag `vX.Y.Z`, the notes from `RELEASE_NOTES.md`, the assets `ac0vr-X.Y.Z-setup.exe`, `ac0vr-X.Y.Z-update.zip`, `SHA256SUMS`.

## The one reset

QUALITY > RESET ALL TO DEFAULTS, F4 > Quick's button and F4 > Enhancements' button call the same function, `ps2_enh_reset_all`: every row of the page and all of `ps2_cfg` (graphics, display, key and pad bindings, dead zones); the TOUCH CONTROLS card's "seen" flag stays.

On the page it asks first (`ACZ_ENH_RESET_CONFIRM`, default `1`; the owner, 2026-11-05). The first press changes nothing: the row reads PRESS A AGAIN (the overlay on a PlayStation pad: PRESS CROSS AGAIN) and the help line says what will be reset. A second press within 4 s resets. Moving off the row, B, Menu, the page closing or the 4 s cancel it and restore the row's text. `ACZ_ENH_RESET_CONFIRM=0`: one press resets. F4 is unchanged: F4 > Quick has its popup, and the RESET ALL TO DEFAULTS button in F4 > Enhancements' QUALITY list is one click.

In F4 both buttons ask in the same window first (Reset / Cancel; the owner, 2026-11-05).

## The ini: the expert keys

On no in-game page; F4 > Enhancements > Expert (ini) lists them; `internal_resolution` of `[graphics]` too (not read while PS2_UPSCALE is set: every launcher start). The QUALITY PRESET writes the ladder keys among them (`governor`, `cloud_detail`, `canopy_quality`, `cockpit_detail`) as before.

| Row | Key | Switch(es) | Applies | Default | Choices |
|---|---|---|---|---|---|
| TERRAIN DETAIL | `terrain_detail` | `ACZ_TERRAIN_DETAIL` | now | HIGH | NORMAL / HIGH |
| CITY DETAIL | `city_detail` | `ACZ_TERRAIN_CITY_LOOK` | next start | ON | ON / OFF |
| HEAT HAZE | `heat_haze` | `ACZ_VR_HEAT_HAZE` | next mission | ON | ON / OFF |
| LIGHT IN TUNNELS | `tunnel_light` | `ACZ_VR_TUNNEL_LIGHT` | next start | ON | ON / OFF |
| CLOUDS DRIFT WITH THE WIND | `cloud_wind` | `ACZ_VR_CLOUD_DRIFT` | next mission | GAME | GAME / SLOW / NONE |
| GAME CLOUDS STAY IN PLACE | `game_cloud_base` | `ACZ_VR_CLOUD_BASE` | next start | OFF | ON / OFF |
| CLOUD DETAIL | `cloud_detail` | `ACZ_VR_CLOUD_DETAIL` | next start | HIGH | HIGH / MEDIUM / LOW |
| WEAPON SCREEN | `weapon_screen` | `ACZ_VR_MFD_ROWS` | next mission | NORMAL | NORMAL / LARGE TEXT |
| COCKPIT ANTI-ALIASING | `cockpit_aa` | `ACZ_VR_COCKPIT_MSAA` | next start | 4X | 4X / 2X / OFF |
| COCKPIT EDGE QUALITY | `cockpit_edge` | `ACZ_VR_COCKPIT_MSAA_AUTO` | next start | BEST | BEST / AUTO |
| COCKPIT DETAIL | `cockpit_detail` | `ACZ_VR_COCKPIT_DETAIL` | next start | HIGH | HIGH / MEDIUM / LOW |
| CANOPY QUALITY | `canopy_quality` | `ACZ_VR_CANOPY_QUALITY` | next start | HIGH | HIGH / MEDIUM / LOW |
| DROP SIZE | `drop_size` | (its field in the file) | now | 1X | 0.75X / 1X / 1.5X / 2X |
| DROP LOOK | `drop_look` | `ACZ_VR_CANOPY_DROPS` | next mission | LENS | LENS / CLASSIC |
| CRACK GROWTH | `canopy_crack_growth` | `ACZ_VR_CANOPY_CRACK_CREEP` | next mission | ON | ON / OFF |
| ADDED SOUNDS GIVE WAY | `sound_bus` | `ACZ_IMM_DUCK` | next mission | ON | ON / OFF |
| HEAD-TRACKED SOUND | `head_tracked_sound` | `ACZ_VR_AUDIO_HEAD` | next start | ON | ON / OFF |
| MEMORY CARD NOTICE | `memcard_notice` | `ACZ_MEMCHECK_HIDE` | next start | HIDDEN | HIDDEN / SHOWN |
| BRIEFING TEXT | `briefing_text` | `ACZ_FMV_BRIEF_MODEL` | next start | FAITHFUL | FAITHFUL / AI GUESSED |
| STORY MOVIE TEXT | `story_text` | `ACZ_FMV_MOVIEUS_MODEL` | next start | FAITHFUL | FAITHFUL / AI GUESSED |
| TV NEWS FOOTAGE | `tv_shots` | `ACZ_FMV_TV_SHOTS` | next start | CLEAN TV LINES | CLEAN TV LINES / AS UPSCALED |
| FRAME-TIME GOVERNOR | `governor` | `ACZ_VR_GOVERNOR` | next start | OFF | ON / OFF |
| GPU CLOCK BOOST | `gpu_boost` | `PS2_PERF_GPU_BOOST` | next start | ON | ON / OFF |
| FOVEATED SHADING | `foveated` | `PS2_VRS_FOV` | next start | LENS EDGE | LENS EDGE / ZONES / OFF |
| EARLY FRAME RECORDING | `record_early` | `PS2_PERF_RECORD_EARLY` | next start | OFF | ON / OFF |
| EE IDLE WAIT | `ee_idle` | `PS2_PERF_EE_IDLE` | next start | OFF | ON / OFF |
| TWO-STAGE GRAPHICS | `gfx_split` | `PS2_GFX_SPLIT` | next start | OFF | ON / OFF |
| DEPTH TO THE HEADSET | `xr_depth` | `PS2_XR_DEPTH` | next start | OFF | ON / OFF |

## The first run: ac0vr.exe

`tools/setup/ac0vr_launcher.c` (setup_1105), the player's entry point and the shortcut's target: Win32 only, no SDL, no game code, built by CMake (target `ac0vr`) and shipped prebuilt. It looks at `build/zero/ac5.exe` beside it and `build_state.json`. When the game is built for the installed release (`version.txt`) and the disc is there, it starts `ac5.exe` and exits. Otherwise a small window: the disc (a picker for an ISO or a folder; `tools/setup/disc_check.c` in-process: the serial and the executable's SHA-256, the US disc only; a wrong disc is refused with the message), then "Build the game from your disc" with a bar, the stage's name and the time left (the stages' measured times are in `bundle/bundle.txt`), running `python\python.exe -I -B tools\setup\player_build.py` through a pipe. On success the game starts. On failure the window shows the last 30 lines, `build/player_build.log` stays, and a previous build can still be started. Cancel kills the build's processes; what was compiled stays for the next try.

| Command line | What it does |
|---|---|
| `ac0vr.exe --dialog` | the window even when nothing is needed: another disc, a rebuild, updates |
| `ac0vr.exe --check-disc PATH [--out FILE]` | the check only: exit 0 ok, 1 wrong, 2 nothing readable; the words into FILE. The installer's disc page |
| `ac0vr.exe --set-disc PATH` | the check, then `[paths] disc =` in `build/zero/ac5_settings.ini` |
| `--auto`, `--no-start`, `--hidden`, `-- ARGS` | [test] no clicks; no game at the end; no window; arguments for the game |

**The cache** (`tools/setup/player_build.py`, `build_state.json`): per stage a hash of its inputs; a rerun does only the stages whose inputs changed.

| Stage | Inputs | Output | Time here, 24 threads |
|---|---|---|---|
| recompile | the ELF, `bundle/recomp/` (the recompilers, the config) | `build/zero/gen_zero/`; a file that comes out the same keeps its date | 10 s |
| compile | `gen_zero/`, `ps2_vu.c`, the runtime headers they really use (from the `.d` files), the toolchain's identity and flags | `build/obj/` | 42 s |
| link | the objects, `libac5rt.a`, the SDK's import libraries | `build/zero/ac5.new.exe`, swapped at the end | 0.3 s |
| install, always | | the DLLs, the shaders, `data/`, `version.txt` next to the exe, only files that changed | 0.1 s |

**The folder of an install.** `ac0vr.exe`, `version.txt`, `build_state.json`; `bundle/` (the compiler, the runtime library, the SDK, the shaders, the DLLs, the recompiler, the data), `python/`, `tools/setup/`, `docs/`, `licenses/`; `build/obj/` and `build/zero/`, the game and everything of "Paths" above. All paths the compilers see are relative to the install, so a folder outside the Windows code page does not matter to them.

**The release** (`tools/setup/make_release.sh VERSION`, on the developer's machine): `out/release/<version>/stage/` is the install image, then the installer, the update zip, `SHA256SUMS`, a `RELEASE_NOTES.md` skeleton. It refuses to finish on any hit of `tools/setup/release_scan.py`: names (`SLUS`, `ps2_image.bin`, `gen_zero/`, `.cpit`, `.iso`, memory cards, the textures made from the disc's pictures), machine paths, and, with the developer's own ELF, 96 pieces of the game's bytes searched in every file.

## The combo rows: what each choice sets

A row of kind `K_COMBO` has no switch of its own; each choice sets several member rows (`enh_combo_tab`, `runtime/src/ps2_settings.c`).
The members are ordinary rows of the table, of category `PS2_ENH_CAT_OFF`: each sets its switch at start-up as before; the file
has only the combo row's key. A switch not named below is left unset (its built-in default). "Field" = a value of `ps2_cfg`.

| Row, key | File value | Sets |
|---|---|---|
| EDGE SMOOTHING `edge_level` | `off` | `PS2_EDGE_AA=0` (any `PS2_EDGE_AA_PARAM`: not read then, ps2_video.c edge_aa_init) |
| | `light` (default) | `PS2_EDGE_AA=1`, `PS2_EDGE_AA_PARAM=0.125,0.0312,0.25` |
| | `strong` | `PS2_EDGE_AA=1` |
| TERRAIN LOOK `terrain_look` | `game` | field `terrain_real` 0, `ACZ_TERRAIN_MATERIALS=0` |
| | `realistic` (default) | field `terrain_real` 1, `ACZ_TERRAIN_DETAIL_HOLD=0.6` |
| BUILDINGS `buildings` | `game` | no `ACZ_TERRAIN_PROPS` (any `ACZ_TERRAIN_CITY`: no houses, no city, zero_props.c) |
| | `towns` | `ACZ_TERRAIN_PROPS=2`, `ACZ_TERRAIN_CITY=0` |
| | `towns_cities` (default) | `ACZ_TERRAIN_PROPS=2` |
| | `red_roofs` (file only) | `ACZ_TERRAIN_PROPS=1` |
| | `red_roofs_only` (file only) | `ACZ_TERRAIN_PROPS=1`, `ACZ_TERRAIN_CITY=0` |
| TRAILS AND VAPOUR `trails_vapour` | `on` (default) | - |
| | `subtle` | `ACZ_VR_CLOUD_WAKE=0.5` |
| | `off` | `ACZ_VR_CONTRAILS=0`, `ACZ_VR_CLOUD_WAKE=0`, `ACZ_VR_VAPOUR=0` |
| SMOKE `smoke` | `on` (default) | - |
| | `subtle` | `ACZ_VR_SMOKE_VOLUME=0.5` |
| | `off` | `ACZ_VR_SMOKE_VOLUME=0`, `ACZ_VR_SMOKE_LIGHT=0` |
| EXPLOSIONS `explosions` | `game` | `ACZ_VR_FX_BILLBOARD=0`, `ACZ_VR_FX_SOFT=0`, `ACZ_VR_FX_BURST=0` |
| | `soft` | `ACZ_VR_FX_BURST=0` |
| | `soft_volumes` (default) | - |
| CLOUD SHADOWS `cloud_shadow_reach` | `off` | `ACZ_VR_CLOUD_SHADOW=0`, `ACZ_VR_CLOUD_SHADOW_GROUND=0` |
| | `aircraft` | `ACZ_VR_CLOUD_SHADOW_GROUND=0` |
| | `everywhere` (default) | - |
| WATER ON THE CANOPY `canopy_water_level` | `off` | field `imm_canopy` 0 (`imm_water` goes to 1: no effect with no water) |
| | `light` / `normal` (default) / `heavy` | field `imm_canopy` 1, `imm_water` 0.5 / 1 / 1.5 |
| GLASS REFLECTIONS `glass_reflections` | `on` (default) | - |
| | `off` | `ACZ_VR_CANOPY_GLARE=0`, `ACZ_VR_CANOPY_REFLECT=0` |
| HUD PLACEMENT `hud_placement` | `free` | `ACZ_VR_HUD_OCCLUDE=0` (glass needs occlusion, ps2_video.c hg_init) |
| | `behind_frames` | `ACZ_VR_HUD_GLASS=0` |
| | `in_the_glass` (default) | - |
| PILOT SOUNDS `pilot_sounds` | `always` (default) | `ACZ_IMM_BREATH=1,calm`, `ACZ_IMM_GSUIT=1` |
| | `under_effort` | `ACZ_IMM_BREATH=1`, `ACZ_IMM_GSUIT=1` |
| | `off` | `ACZ_IMM_STRAIN=0` (breath and G-suit are off when unset) |
| RUMBLE STRENGTH `rumble_level` | `auto` (default) | - (a pad's AUTO gain, Touch 0.5) |
| | `50` / `100` / `150` / `200` | `ACZ_RUMBLE_GAIN=0.5` / `1` / `1.5` / `2`, `ACZ_TOUCH_HAPTICS=0.25` / unset (0.5) / `0.75` / `1` |
| | `off` (file only) | `ACZ_RUMBLE_GAIN=0`. On the page the off switch is the game's own CONTROLLER SETTINGS > VIBRATION |

A combo row is held by the environment when any of its switches is set there; `ps2_enh_held_by` names that switch and the page
shows the row locked at what is in effect (CUSTOM when no choice is it).

## The migration of an old file

At start, for each combo row: its key wins if the file has it. Else, if any of its old keys is present, the row shows the choice
those values match, or the nearest one, and logs

```
settings: migrated <row>: a=.., b=.. -> <row>=<NAME> (exact|nearest)
```

The old values stay in effect for that run and until a save; nothing is saved by the migration itself. The next save (any change on
the page or in F4) writes only the new key; from the next start on the choice is what counts. So "nearest" changes the picture at
the first start after a save, never before: until then the row carries the page's `*` ("saved, waits for a restart"). Rows of the
third layer keep their keys. Unknown keys behave as ever: ignored on load, dropped at the next save.

"Nearest": each member that differs costs 1 (a field: by its distance); the first of the cheapest choices wins.

An old value that no choice reproduces also includes any hand-written value of an old key (`ground_detail = 0.8`).

### Every old combination (47 exact, 59 nearest of 106)

| Row | Old combinations | Exact | Nearest |
|---|---|---|---|
| `edge_level` | 4 | 4 | 0 |
| `terrain_look` | 8 | 2 | 6 |
| `buildings` | 6 | 6 | 0 |
| `trails_vapour` | 12 | 3 | 9 |
| `smoke` | 6 | 3 | 3 |
| `explosions` | 8 | 3 | 5 |
| `cloud_shadow_reach` | 4 | 4 | 0 |
| `canopy_water_level` | 8 | 7 | 1 |
| `glass_reflections` | 4 | 2 | 2 |
| `hud_placement` | 4 | 4 | 0 |
| `pilot_sounds` | 12 | 3 | 9 |
| `rumble_level` | 30 | 6 | 24 |

**`edge_level`**
- exact LIGHT: `edge_smoothing=1, edge_strength=0.125,0.0312,0.25`
- exact STRONG: `edge_smoothing=1, edge_strength=default`
- exact OFF: `edge_smoothing=0, edge_strength=0.125,0.0312,0.25`; `edge_smoothing=0, edge_strength=default`

**`terrain_look`**
- exact REALISTIC: `realistic_terrain=1, ground_materials=default, ground_detail=0.6`
- exact GAME: `realistic_terrain=0, ground_materials=0, ground_detail=default`
- nearest:
  - `realistic_terrain=1, ground_materials=default, ground_detail=default` -> REALISTIC
  - `realistic_terrain=1, ground_materials=0, ground_detail=default` -> GAME
  - `realistic_terrain=1, ground_materials=0, ground_detail=0.6` -> REALISTIC
  - `realistic_terrain=0, ground_materials=default, ground_detail=default` -> GAME
  - `realistic_terrain=0, ground_materials=default, ground_detail=0.6` -> REALISTIC
  - `realistic_terrain=0, ground_materials=0, ground_detail=0.6` -> GAME

**`buildings`**
- exact GAME: `town_houses=default, city_buildings=default`; `town_houses=default, city_buildings=0`
- exact RED ROOFS: `town_houses=1, city_buildings=default`
- exact RED ROOFS ONLY: `town_houses=1, city_buildings=0`
- exact TOWNS AND CITIES: `town_houses=2, city_buildings=default`
- exact TOWNS: `town_houses=2, city_buildings=0`

**`trails_vapour`**
- exact ON: `contrails=default, cloud_wake=default, wing_vapour=default`
- exact SUBTLE: `contrails=default, cloud_wake=0.5, wing_vapour=default`
- exact OFF: `contrails=0, cloud_wake=0, wing_vapour=0`
- nearest:
  - `contrails=default, cloud_wake=default, wing_vapour=0` -> ON
  - `contrails=default, cloud_wake=0.5, wing_vapour=0` -> SUBTLE
  - `contrails=default, cloud_wake=0, wing_vapour=default` -> ON
  - `contrails=default, cloud_wake=0, wing_vapour=0` -> OFF
  - `contrails=0, cloud_wake=default, wing_vapour=default` -> ON
  - `contrails=0, cloud_wake=default, wing_vapour=0` -> OFF
  - `contrails=0, cloud_wake=0.5, wing_vapour=default` -> SUBTLE
  - `contrails=0, cloud_wake=0.5, wing_vapour=0` -> OFF
  - `contrails=0, cloud_wake=0, wing_vapour=default` -> OFF

**`smoke`**
- exact ON: `smoke_volume=default, smoke_light=default`
- exact SUBTLE: `smoke_volume=0.5, smoke_light=default`
- exact OFF: `smoke_volume=0, smoke_light=0`
- nearest:
  - `smoke_volume=default, smoke_light=0` -> ON
  - `smoke_volume=0.5, smoke_light=0` -> SUBTLE
  - `smoke_volume=0, smoke_light=default` -> ON

**`explosions`**
- exact SOFT WITH VOLUMES: `explosion_cards=default, explosion_soft=default, explosion_burst=default`
- exact SOFT: `explosion_cards=default, explosion_soft=default, explosion_burst=0`
- exact GAME: `explosion_cards=0, explosion_soft=0, explosion_burst=0`
- nearest:
  - `explosion_cards=default, explosion_soft=0, explosion_burst=default` -> SOFT WITH VOLUMES
  - `explosion_cards=default, explosion_soft=0, explosion_burst=0` -> GAME
  - `explosion_cards=0, explosion_soft=default, explosion_burst=default` -> SOFT WITH VOLUMES
  - `explosion_cards=0, explosion_soft=default, explosion_burst=0` -> GAME
  - `explosion_cards=0, explosion_soft=0, explosion_burst=default` -> GAME

**`cloud_shadow_reach`**
- exact EVERYWHERE: `cloud_shadows=default, ground_shadows=default`
- exact ON THE AIRCRAFT: `cloud_shadows=default, ground_shadows=0`
- exact OFF: `cloud_shadows=0, ground_shadows=default`; `cloud_shadows=0, ground_shadows=0`

**`canopy_water_level`**
- exact LIGHT: `canopy_water=1, water_strength=0.5`
- exact NORMAL: `canopy_water=1, water_strength=1`
- exact HEAVY: `canopy_water=1, water_strength=1.5`
- exact OFF: `canopy_water=0, water_strength=0.5`; `canopy_water=0, water_strength=1`; `canopy_water=0, water_strength=1.5`; `canopy_water=0, water_strength=2`
- nearest:
  - `canopy_water=1, water_strength=2` -> HEAVY

**`glass_reflections`**
- exact ON: `canopy_glare=default, canopy_reflections=default`
- exact OFF: `canopy_glare=0, canopy_reflections=0`
- nearest:
  - `canopy_glare=default, canopy_reflections=0` -> ON
  - `canopy_glare=0, canopy_reflections=default` -> ON

**`hud_placement`**
- exact IN THE GLASS: `hud_behind_frames=default, hud_glass=default`
- exact BEHIND FRAMES: `hud_behind_frames=default, hud_glass=0`
- exact FREE: `hud_behind_frames=0, hud_glass=default`; `hud_behind_frames=0, hud_glass=0`

**`pilot_sounds`**
- exact OFF: `breathing=default, g_suit=default, strain=0`
- exact UNDER EFFORT: `breathing=1, g_suit=1, strain=default`
- exact ALWAYS: `breathing=1,calm, g_suit=1, strain=default`
- nearest:
  - `breathing=default, g_suit=default, strain=default` -> OFF
  - `breathing=default, g_suit=1, strain=default` -> ALWAYS
  - `breathing=default, g_suit=1, strain=0` -> OFF
  - `breathing=1, g_suit=default, strain=default` -> UNDER EFFORT
  - `breathing=1, g_suit=default, strain=0` -> OFF
  - `breathing=1, g_suit=1, strain=0` -> UNDER EFFORT
  - `breathing=1,calm, g_suit=default, strain=default` -> ALWAYS
  - `breathing=1,calm, g_suit=default, strain=0` -> OFF
  - `breathing=1,calm, g_suit=1, strain=0` -> ALWAYS

**`rumble_level`**
- exact AUTO: `rumble_strength=default, touch_vibration=default`
- exact OFF: `rumble_strength=0, touch_vibration=default`
- exact 50%: `rumble_strength=0.5, touch_vibration=0.25`
- exact 100%: `rumble_strength=1, touch_vibration=default`
- exact 150%: `rumble_strength=1.5, touch_vibration=0.75`
- exact 200%: `rumble_strength=2, touch_vibration=1`
- nearest:
  - `rumble_strength=default, touch_vibration=0` -> AUTO
  - `rumble_strength=default, touch_vibration=0.25` -> AUTO
  - `rumble_strength=default, touch_vibration=0.75` -> AUTO
  - `rumble_strength=default, touch_vibration=1` -> AUTO
  - `rumble_strength=0, touch_vibration=0` -> OFF
  - `rumble_strength=0, touch_vibration=0.25` -> 50%
  - `rumble_strength=0, touch_vibration=0.75` -> 150%
  - `rumble_strength=0, touch_vibration=1` -> 200%
  - `rumble_strength=0.5, touch_vibration=0` -> 50%
  - `rumble_strength=0.5, touch_vibration=default` -> AUTO
  - `rumble_strength=0.5, touch_vibration=0.75` -> 50%
  - `rumble_strength=0.5, touch_vibration=1` -> 50%
  - `rumble_strength=1, touch_vibration=0` -> 100%
  - `rumble_strength=1, touch_vibration=0.25` -> 50%
  - `rumble_strength=1, touch_vibration=0.75` -> 100%
  - `rumble_strength=1, touch_vibration=1` -> 100%
  - `rumble_strength=1.5, touch_vibration=0` -> 150%
  - `rumble_strength=1.5, touch_vibration=0.25` -> 50%
  - `rumble_strength=1.5, touch_vibration=default` -> AUTO
  - `rumble_strength=1.5, touch_vibration=1` -> 150%
  - `rumble_strength=2, touch_vibration=0` -> 200%
  - `rumble_strength=2, touch_vibration=0.25` -> 50%
  - `rumble_strength=2, touch_vibration=default` -> AUTO
  - `rumble_strength=2, touch_vibration=0.75` -> 150%

## For the next rows

- A new page row: the rule above decides. A new key goes into `enh_rows` in its category; `tools/test_settings.py` counts rows and
  checks the help line's rules and the vocabulary.
- A row of the next mission's tier: `MISSION` in its table line; its readers ask `ps2_menv` (never `getenv`: the test checks it) and read
  their static again when `ps2_menv_gen` moved; what is made once goes by `ps2_menv_boot` and gets a line in `enh_boot_tab`. Say what derives
  from the switch and rebuild it, or the row stays `RESTART`. A row that needs the game to enter a mission or to load a map gets a line in
  `enh_retry_tab`: a RETRY does neither.
- A new combo row: one line with `CO(...)` in `enh_rows`, one entry in `enh_combo_tab` with the members' values per choice, its old
  keys as members with category `CAT_OFF`. The default choice must reproduce the members' defaults (the test checks it).
- Conditional rows: `ps2_enh_hotas_rows(on)` (a joystick), `ps2_enh_touch_rows(on)` (a Touch pose valid once, or `ACZ_TOUCH` set).
- These tables: `tools/settings_test.c`'s `md` mode prints them (`settings_test.exe md`).
