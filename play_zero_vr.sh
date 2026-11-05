#!/usr/bin/env bash
# Play the native Ace Combat Zero recompilation in VR (Meta Quest 3 over Quest Link / Air Link).
#
#   ./play_zero_vr.sh              = stick (default): stereo; outside the cockpit head turns act as the right stick
#                                  (game look, HUD as the game draws it); in the R3 cockpit the head looks freely
#                                  and the direction part of the HUD stays with the aircraft
#   ./play_zero_vr.sh stereo       free head look in every view, direction HUD anchored to the aircraft
#   ./play_zero_vr.sh sharp        like stick, but both eyes in one pass (multiview) at 4x: much sharper
#   ./play_zero_vr.sh stick        stereo, but head turns act as the right stick (game look), image head-locked
#   ./play_zero_vr.sh mono         head tracked without stereo (if stereo looks wrong)
#   ./play_zero_vr.sh cinema       big virtual screen only
#   ./play_zero_vr.sh flat         no VR, normal window
#
# Extra words after the mode (any order):
#   m1 .. m18   boot straight into that campaign mission (no logos, menus, movie, briefing or intro; flying ~15 s
#               after launch, F-5E with Pixy). Debug/test mode: campaign progress is not what a real save would hold.
#   native      native renderer for the 3D models and terrain (the PS2 vector unit programs run as GPU shaders
#               instead of being emulated); experimental
#   cockpit     in the R3 cockpit view, replace the game's cockpit by the 3D cockpit model in cockpits/f5e_t38.cpit
#               (FlightGear T-38 cockpit, GPL; build it with tools/cockpit/build_f5e_t38.sh): live instruments, the
#               game's own sun and sky light with shadows (needs native), baked ambient occlusion, 4x anti-aliasing;
#               flying the F-15C, F-16C, A-10A, MiG-21bis, J35J, Su-27, F-14D or Mirage 2000D it uses their own model instead when
#               built (tools/cockpit/build_*.sh). In the stereo modes (stick, stereo, sharp) every mission then starts in
#               the cockpit view (from the first frame you fly, after the intro); R3 still cycles the views
#               (ACZ_VR_START_VIEW=game keeps the game's own first-person start view)
#   smooth      frames between the 60 Hz game frames are drawn at the headset rate (90/120 Hz) from the extrapolated
#               camera and object motion, instead of the headset repeating the old image; best with native. Without
#               the word the ENHANCEMENTS setting SMOOTH MOTION decides (default on); with it, on for this session
#   modern      trigger-throttle controller layout for this session: RT/LT = throttle up/down as far as you pull them
#               (analog, the PS2's pressure-sensitive R1/L1), RB/LB = yaw (both: autopilot), the rest as below;
#               `classic` forces the usual layout. Without either word the F4 > Controller "Layout" choice applies;
#               your own bindings there are kept either way (ACZ_PAD_LAYOUT=modern|classic)
#   e.g. ./play_zero_vr.sh sharp m1 native smooth
#
# ENHANCEMENTS (bug #64): everything else -- resolution in the sharp mode, edge smoothing, detail, HUD size, clouds,
# cockpit screens, immersion, sound, smooth motion, the pad layout, ... -- comes from the settings file
# (build/zero/ac5_settings.ini [enhancements]; the pause menu's ENHANCEMENTS page, View + Menu > X in the menus between
# missions, F4 > Enhancements). This script exports ACZ_MENU_SETTINGS=1 so the game applies them at start-up (test
# scripts do not: their switches keep the built-in defaults). Any variable exported before running wins over the menu
# for that session (the page shows it locked); so do the words above (smooth, modern / classic, cockpit's models).
#
# Before starting: connect the Quest, enable Quest Link, put it on. Without a headset the game runs on the monitor.
# Best smoothness: set the refresh rate to 120 Hz in the Meta Horizon Link app (Devices > Graphics preferences). The game
# runs at 60 fps, so 120 Hz shows every frame exactly twice; 90 Hz and 72 Hz cannot divide 60 evenly (slight judder).
# Keys: F9, the gamepad Share button, or holding Back ~1 s recentres the view (it also recentres when a flight starts);
# in VR a short tap on Back is passed to the game as Select. F4 settings menu, Esc twice to quit (game window).
# Quitting from the headset: in a mission pause (Start/Menu) > QUIT GAME > YES; anywhere but in flight hold Back + Start
# (View + Menu) ~1.5 s: a panel appears in the headset, A = quit to desktop, X = main menu (from the pause menu: the
# game's QUIT MISSION), B = back to the game. Quitting waits for a memory card save to finish, ends the VR session and
# exits normally ("exit 0" below).
# Gamepad: the first SDL gamepad (Xbox controller) is player 1: A=Cross, B=Circle, X=Square, Y=Triangle, LB/RB=L1/R1,
# LT/RT=L2/R2 (modern: LT/RT=L1/R1 analog, LB/RB=L2/R2), Back=Select, Start=Start, R3 in flight = cockpit view
# (recommended in VR).
# Keep the game's Screen Ratio at 4:3 (head tracking only engages on the 640x480 flight camera).
# Saves go to saves/card0.ps2mc: back it up before trusting long campaign progress to it (save/load not yet tested).
#
# Tuning (export before running, all optional):
#   ACZ_VR_HUD_SCALE=1.0      HUD size; 1 = original angular size (pitch ladder matches the horizon), 1.3 bigger
#                             (ENHANCEMENTS > HUD > HUD SIZE, default 1.3X; the variable overrides it)
#   ACZ_VR_STEREO_SCALE=1.0   eye separation multiplier (more = stronger depth, world looks smaller)
#   ACZ_VR_UNITS_PER_M=6      game units per metre (scales head movement and eye separation)
#   PS2_XR_FOV_LIMIT=0        cap on the half-FOV tangent if the game ever misbehaves with the full headset FOV
#   ACZ_VR_COCKPIT_SCALE=8    cockpit depth in stereo (higher = cockpit closer)
#   ACZ_VR_COCKPIT_BRIGHT=1   3D cockpit brightness (1 = as the game lights its models); ACZ_VR_COCKPIT_MSAA=4 (0 off), ACZ_VR_COCKPIT_SHADOWS=1,
#                             ACZ_VR_COCKPIT_SPEC=0.3 (shine), ACZ_VR_COCKPIT_GAME_LIGHT=1 (0: fixed light),
#                             ACZ_VR_COCKPIT_REAR=0 hides the T-38 rear cockpit (F-5E-like; only seen looking back)
#   PS2_UPSCALE=3             internal resolution multiplier (1-4); 3 needs ~9.6 ms of GPU per frame on an RTX 3090 Ti,
#                             use 2 if frames drop at 120 Hz (the sharp mode: ENHANCEMENTS > QUALITY > RESOLUTION, 4X;
#                             PS2_UPSCALE_SHARP=n or PS2_UPSCALE=n override it)
#   ACZ_MENU_SETTINGS=0       start without the ENHANCEMENTS settings (every switch at its built-in default)
#   ACZ_LAUNCHER=1            the launcher window before the game: QUALITY rows, GPU, active controller, PLAY (docs/SETTINGS.md)
set -u
cd "$(dirname "$0")"
mode=${1:-stick}
shift || true
for w in "$@"; do
    case "$w" in
        m[0-9]*) export ACZ_BOOT_MISSION=${w#m} ;;
        native)  export PS2_RN_VP=1 ;;
        smooth)  export PS2_XR_RERENDER=1 ;;
        modern|classic) export ACZ_PAD_LAYOUT=$w ;;
        cockpit) export ACZ_VR_COCKPIT_MODEL=${ACZ_VR_COCKPIT_MODEL:-cockpits/f5e_t38.cpit}
                 # per aircraft (player aircraft id = model; ids: docs/aircraft_ids/README.md): this machine's own map script when it
                 # has one, else the map of the installed cockpit set, cockpits/map.txt (`id=file` lines). The runtime uses an
                 # aircraft's FIRST entry (ps2_video.c cpit_load); an aircraft without one gets ACZ_VR_COCKPIT_MODEL;
                 # tools/cockpit/map_check.sh checks the map
                 if [ -z "${ACZ_VR_COCKPIT_MODELS:-}" ]; then
                     m=""
                     if [ -f tools/cockpit/local_map.sh ]; then
                         . tools/cockpit/local_map.sh
                     elif [ -f cockpits/map.txt ]; then
                         while IFS= read -r l || [ -n "$l" ]; do
                             l=${l%%#*}; l=${l//[[:space:]]/}
                             [ -n "$l" ] || continue
                             f=${l#*=}
                             case "$f" in */*) ;; *) f=cockpits/$f ;; esac
                             [ -f "$f" ] && m="$m,${l%%=*}=$f"
                         done < cockpits/map.txt
                     fi
                     [ -n "$m" ] && export ACZ_VR_COCKPIT_MODELS=${m#,}
                 fi ;;
        *) echo "unknown option '$w' (expected mN, native, smooth, cockpit, modern or classic)"; exit 2 ;;
    esac
done
ISO=${ACZ_DISC:-}
[ -n "$ISO" ] || { echo "no disc: set ACZ_DISC to your disc image or to a folder with its files"; exit 2; }
META_JSON='C:\Program Files\Meta Horizon\Support\oculus-runtime\oculus_openxr_64.json'
# the ENHANCEMENTS settings (ac5_settings.ini [enhancements]) become the defaults of their switches at start-up; the
# HUD scale 1.3, edge AA, LOD bias 2, terrain detail 2, 4x in the sharp mode and smooth motion are among them
export ACZ_MENU_SETTINGS=${ACZ_MENU_SETTINGS:-1}
# the launcher window before the game (QUALITY rows, PLAY); ACZ_LAUNCHER=0 ./play_zero_vr.sh ... starts without it
export ACZ_LAUNCHER=${ACZ_LAUNCHER:-1}
# keep reading the gamepad when the game window is not focused (Quest Link / the headset may take focus)
export SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1
if [ "$mode" = sharp ]; then
    # both eyes in one pass (multiview); the internal resolution is the menu's RESOLUTION (4x by default) unless
    # PS2_UPSCALE_SHARP (or PS2_UPSCALE) is given for this session
    export PS2_XR_MULTIVIEW=1
    [ -n "${PS2_UPSCALE_SHARP:-}" ] && export PS2_UPSCALE=$PS2_UPSCALE_SHARP
    mode=stick
else
    export PS2_UPSCALE=${PS2_UPSCALE:-3}
fi
if [ "$mode" = stick ]; then
    # head turns drive the game's look (right stick); image and HUD stay in front of the eyes
    export ACZ_VR_HEADLOOK=stick
    mode=stereo
fi
if [ "$mode" != flat ]; then
    export PS2_XR=1 PS2_XR_MODE=$mode
    # Meta's runtime when it is installed, else the system's active OpenXR runtime; PS2_XR_RUNTIME picks another one
    [ -n "${PS2_XR_RUNTIME:-}" ] || { [ -f "$(cygpath -u "$META_JSON")" ] && export PS2_XR_RUNTIME=$META_JSON; }
    # 60.00 Hz fields instead of NTSC 59.94 so frames line up with a 120 Hz headset (the game runs 0.1% faster)
    export PS2_FIELD_HZ=${PS2_FIELD_HZ:-60}
fi
mkdir -p out saves
echo "Ace Combat Zero recomp: mode=$mode upscale=${PS2_UPSCALE:-menu}${ACZ_BOOT_MISSION:+ mission=$ACZ_BOOT_MISSION}${PS2_RN_VP:+ native}${PS2_XR_RERENDER:+ smooth}${ACZ_VR_COCKPIT_MODEL:+ cockpit}${ACZ_PAD_LAYOUT:+ $ACZ_PAD_LAYOUT} (log: zero_play.log)"
build/zero/ac5.exe --data gen_zero --disc "$(cygpath -w "$ISO")" --watchdog 0 > zero_play.log 2>&1
echo "exit $?"
