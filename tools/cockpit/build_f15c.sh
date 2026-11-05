#!/bin/bash
# build_f15c.sh -- F-15C cockpit for the native VR cockpit (FlightGear F-15 by Richard Harrison / Zaretto, GPL-2.0+
# through FGAddon; github.com/Zaretto/F-15, branch release/1.12):
#   1. tools/cockpit/ac2cpit.py: Models/Cockpit/all-interior.xml (the merged interior, eye from f15-common.xml "Cockpit
#      View": x -5.11, y 0, z 1.402) + exterior parts of Models/f15c.ac seen from the seat (canopy, windscreen,
#      mirrors, nose, fuselage, intakes, wings, tail); the 4096^2 DXT1 interior atlas kept at full size
#      Display canvases as --screen (ps2_video.c mfd_*): the VSD (upper left, the real radar display) shows the game's
#      HUD radar, the MPCD (lower left, the real armament display) its weapon block, 6 mm towards the eye (a face of
#      mpcd-mount 4 mm in front of it, which FlightGear culls as a back face); the TEWS on the right (the real
#      threat display: a round scope centred on the own aircraft) is screen 4 since ck_f15 2026-10-29 (bug #191: it
#      stood dark, the transparent canvas quad dropped): the runtime shows the game's radar there, fitted to the round
#      bezel, and the big map on the VSD (ACZ_VR_MFD_DUAL). F15C_TEWS=0 = the dark TEWS as before
#      Concave polygons by ear clipping (F15C_EARCLIP=1, the default; pt_gaze 2026-10-29, the 10:50 session 10:54:37
#      "the screen peeks out from behind ... the texture is a bit higher"): the TEWS bezel's step, 6 mm in front of the
#      screen, is three concave ring pieces whose fan triangles covered 52 % of the scope's opening with stray atlas
#      texels, so the radar showed as a cut, shifted shape; ac2cpit --ear-clip triangulates them as FlightGear (OSG)
#      does, convex polygons unchanged. F15C_EARCLIP=0 = the fans as before (unset: CPIT_EARCLIP, the switch of all
#      the cockpit build scripts since ck_earclip 2026-10-29, bug #208). The MPCD's 6 mm lift above dates from the
#      fans: the mpcd-mount face in front of the screen was their stray fill; ear-clipped, the unlifted screen is
#      covered on 4.6 % only (the bezel's lip at its edges; 94.9 % with the fans), so the lift could go (kept)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/F-15 with Models/Cockpit/{all-interior.ac,.xml,.dds, f15-drums.rgb,
# digital-numbers-amber.rgb, tranbg.png} and Models/{f15c.ac, common.png, canopy-lights.rgb, grey-blue-flood.rgb,
# f-15-standby-compass.rgb, Liveries/livery.AF80-0007.png (the .jpg converted: the converter reads PNG, SGI, DDS)}.
# The HUD's two combiner plates (HUDImage1 / HUDImage2: FlightGear draws the HUD image on them through its f15hud
# effect over the fully transparent tranbg.png, so they were dropped as invisible and the frame stood empty) are
# --glass-obj: clear glass, the runtime's ACZ_VR_HUD_GLASS combiner (bug #154, ck_align 2026-10-28).
# Output: cockpits/f15c.cpit. Use: ACZ_VR_COCKPIT_MODEL=cockpits/f15c.cpit (and ACZ_BOOT_AIRCRAFT=5 to fly an F-15C).
# Seats (F15C_SEAT=1, the default; ck_seats 2026-10-28, bug #168): the model's two ACES II seats (aces2 and its
# seat-cushion, front and rear) take a few texels of the 4096 px interior atlas -- a plain light-grey headbox at 0.2-0.5 m
# looking back. They are replaced by the ACES II of the FlightGear F-16 (NikolaiVChr, GPL-2.0, ../cockpit_src/F-16/
# Models/Cockpit/chair: own textures, the structure 512 px and the cushions 128 px, x4 with CPIT_X4=1), the same seat:
# its 30 deg F-16 recline brought to the F-15's 12 deg (pitch -17 deg) and placed on the old seats (fitted: bottom,
# top and the back line at mid-height within 1 cm; the rear one 1.371 m aft, 2.2 cm lower). The copy in
# build/cockpit/f15c_chair renames the chair's seat-cushion object (the F-15's own is hidden by that name) and the
# F-16's centre ejection handle is left out (the F-15's ACES II has its handles on the bucket sides; the old seat's,
# modelled into its bucket walls, would sit inside the F-16 bucket's).
# F15C_SEAT=0 = the old seats
# ACES II side handles (F15C_HANDLES=1, the default with F15C_SEAT; ck_polish 2026-10-28, bug #176): the F-15's ACES II
# is pulled by two yellow-and-black striped handles on the front of the bucket sides (the F-16's has one between the
# knees instead); the old seat's were inside the replaced bucket's walls. tools/cockpit/eject_handle.py makes them in
# the chair's own frame (a 12 x 6 cm loop on each side, 2 cm tube, leaning 15 deg outward, ahead of the F-16 chair's
# safety lever) and both seats take them at the chairs' offsets. F15C_HANDLES=0 = without (as ck_seats' F15C_SEAT)
set -e
cd "$(dirname "$0")/../.."
SRC=../cockpit_src/F-15/Models
OUT=${1:-cockpits/f15c.cpit}
mkdir -p cockpits build/tools
CK=()
if [ "${F15C_SEAT:-1}" != 0 ]; then
    C=../cockpit_src/F-16/Models/Cockpit/chair
    D=build/cockpit/f15c_chair
    mkdir -p "$D"
    sed 's/^name "seat-cushion"/name "chair-seat-cushion"/' "$C/chair.ac" > "$D/chair.ac"
    cp "$C/chair.xml" "$D/chair.xml"
    if [ "${CPIT_X4:-1}" != 0 ]; then
        bash tools/cockpit/upscale_tex.sh build/tools/f15c_x4 \
            "$C/seat-cushion.png" "$C/seat-cushion2.png" "$C/texture.png"
        CK+=(--tex-dir build/tools/f15c_x4)
    fi
    CK+=(--tex-dir "$C" --hide-obj '^(aces2|seat-cushion|eject-handle)$')
    CK+=(--add-model "$D/chair.xml@-0.9388,0,1.8141,0,-17,0" --add-model "$D/chair.xml@0.4322,0,1.7921,0,-17,0")
    if [ "${F15C_HANDLES:-1}" != 0 ]; then  # ck_polish 2026-10-28: the side handles (see the header)
        python -I tools/cockpit/eject_handle.py "$D" aces2_handles \
            "${F15C_HANDLE_L:--4.600,-0.214,0.198@1,0,0@0,-0.27,1@0.12,0.058,0.010}" \
            "${F15C_HANDLE_R:--4.600,0.214,0.198@1,0,0@0,0.27,1@0.12,0.058,0.010}"
        CK+=(--add-model "$D/aces2_handles.xml@-0.9388,0,1.8141,0,-17,0" --add-model "$D/aces2_handles.xml@0.4322,0,1.7921,0,-17,0")
    fi
fi
TEWS=()
[ "${F15C_TEWS:-1}" != 0 ] && TEWS=(--screen '^TEWSImage$=4')   # ck_f15 2026-10-29: the TEWS scope (see the header)
EC=()
[ "${F15C_EARCLIP:-${CPIT_EARCLIP:-1}}" != 0 ] && EC=(--ear-clip)               # pt_gaze 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$SRC/Cockpit/all-interior.xml" -5.11 0 1.402 "$OUT" --subdiv 0.05 "${CK[@]}" \
    --max-tex-for 'all-interior' 4096 --subdiv-extra 0.25 --screen '^VSDImage$=1' --screen '^MPCDImage$=2,0.006' --glass-obj '^HUDImage[12]$' \
    "${TEWS[@]}" "${EC[@]}" \
    --extra "$SRC/f15c.ac" \
    '^(Fuselage(\.[0-9]+)?|Nose Section\.002|Radome|Windscreen(CanopyFrame|Glass|GlassInside|Fasteners)|Canopy(Glass|GlassInside|BackCover|Fasteners|Mirror[A-Za-z]*)?|StandbyCompass[A-Za-z]*|canopy-outside-handle|Wing[LR](\.001)?|[LR]WingSlimmer|Stabilator[LR]|Fins|left-engine-and-inlet\.002|right-engine-and-inlet|Intake\.002)$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
