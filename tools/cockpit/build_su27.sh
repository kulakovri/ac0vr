#!/bin/bash
# build_su27.sh -- Su-27 cockpit for the native VR cockpit (FlightGear SU-27SK by yanes19 and contributors, GPL-2.0;
# github.com/yanes19/SU-27SK, master):
#   1. tools/cockpit/ac2cpit.py: the aircraft model Models/SU-27.xml (exterior Su-27.ac, coarser subdivision, and the
#      interior Models/Interior/panel.xml at its own offset) without stores, effects, lights, pilot, the J-11A panel;
#      eye from Su-27SK-set.xml "Cockpit View" (x 5.23, y 0, z 1.42); the HUD glass placeholder (CavasTex.png, a
#      scribble FlightGear's canvas HUD replaces) is dropped -- the game draws its own HUD; the glass itself
#      (hudglass of Models/Interior/Instruments/hud/hud.ac) is --glass-obj: clear glass, the runtime's ACZ_VR_HUD_GLASS
#      combiner (bug #154, ck_align 2026-10-28)
#      The radar / TV screen on the right (radar.BG and the TWS.screen overlay) is --screen 1, 4 mm towards the eye
#      (in front of the IPV.ekran.Off cover): the runtime shows the game's HUD radar on it (ps2_video.c mfd_*)
#      The canopy glass (canopy.glass: a ~30 % blue texture the runtime's alpha cut dropped, so the cockpit had no
#      glass) is --glass-obj: clear glass (alpha 0) that carries the runtime's canopy water, mist and ice (bug #71)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/Su-27SK. Output: cockpits/su27.cpit. Player aircraft 24 = Su-27.
# Textures x4 (CPIT_X4=1, the default; ck_audit 2026-10-28): tools/cockpit/upscale_tex.sh (Real-ESRGAN animevideov3)
# writes build/tools/su27_x4 for --tex-dir: the seat structure and headbox (512 px). CPIT_X4=0 = the textures as fetched
# Windscreen glass (SU27_WINDSCREEN=1, the default; ck_fit 2026-10-28, bug #169): the windscreen of Su-27.ac is
# object.001 (x 3.72..4.6 up to the front arch, Glass_Cockpit.png: the same ~30 % blue texture as canopy.glass, so the
# runtime's alpha cut dropped it too): inside the front arch 8 % of the view ahead was open air, without rain, drops,
# mist or defects. It is --glass-obj now: clear glass (alpha 0) covering az -39..39, el -25..12 deg from the eye; no
# direction crosses both it and canopy.glass (no double weather layer). GLS_Canopy_Rear.001 stays opaque: it is the
# riveted canopy arch and rails (the model's XML lists it among the solid surfaces; the glass is canopy.glass).
# SU27_WINDSCREEN=0 = no windscreen glass as before
# K-36DM seat (SU27_K36=1, the default; ck_seats 2026-10-28, bug #168): the model's seat (Interior/seat2.ac) is an F-16
# ACES II chair with the F-16 textures and only the K-36's arch and stabiliser booms -- not the Su-27's K-36DM. It is
# left out (--skip) and the K-36DM submodel of the FlightGear MiG-29 9.12 (../cockpit_src/MiG-29_9-12/Models/Submodels/
# K-36DM, GPL-3.0) is added (--add-model; SU27_K36_X / SU27_K36_Z: its offset in SU-27.xml's frame): 0.17 m ahead of
# the old seat's arch (ck_fit 2026-10-28: the old seat back sat 0.17 m behind the model's own pilot figure; the head
# pad now 0.28 m behind the eye, the old headbox 0.45-0.55 m), the bucket 15 cm above the old bucket's bottom, the
# headbox top 13.5 cm above the eye as in the MiG-29; its 2 KB palette texture repainted by tools/cockpit/k36_paint.py
# (as MIG29_K36_PAINT) into the --tex-dir. Needs numpy + PIL: PYV (default python).
# SU27_K36=0 = the old seat
# K-36DM ejection handles (K36_HANDLE=1, the default with SU27_K36; ck_polish 2026-10-28, bug #175): the MiG-29 9.12's
# K-36DM model has no handles (the old seat2.ac had red loops); the real seat is fired by the yellow-and-black striped
# loop between the knees, rising from the front of the seat pan. tools/cockpit/eject_handle.py makes it in the K-36DM's
# own frame (14 cm wide, 11.5 cm high, 2.2 cm tube, leaning 12 deg forward; as build_mig29a.sh) and it takes the
# seat's offset. K36_HANDLE=0 = without (as ck_seats' SU27_K36)
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/Su-27SK
OUT=${1:-cockpits/su27.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
SKIP='Stores|Effects|Lights|sim-helpr|pilot|panel-zh|Explosion'
if [ "${SU27_K36:-1}" != 0 ]; then         # ck_seats 2026-10-28: the K-36DM seat (see the header)
    mkdir -p build/tools/su27_k36          # (its own folder: seat2.ac has a K-36DM.png of its own)
    K36=../cockpit_src/MiG-29_9-12/Models/Submodels/K-36DM
    "${PYV:-python}" -I tools/cockpit/k36_paint.py "$K36/K-36DM.png" \
        build/tools/su27_k36/K-36DM.png
    SKIP="$SKIP|Interior/seat2[.]ac"
    CK+=(--tex-dir build/tools/su27_k36 --add-model "$K36/K-36DM.xml@${SU27_K36_X:-5.467},0,${SU27_K36_Z:-0.32}")
    if [ "${K36_HANDLE:-1}" != 0 ]; then    # ck_polish 2026-10-28: the K-36DM's pull loop (see the header)
        python -I tools/cockpit/eject_handle.py build/cockpit/su27_k36h k36_handle "${K36_HANDLE_LOOP:--0.430,0,0.290@0,1,0@-0.21,0,1@0.14,0.115,0.011}"
        CK+=(--add-model "build/cockpit/su27_k36h/k36_handle.xml@${SU27_K36_X:-5.467},0,${SU27_K36_Z:-0.32}")
    fi
fi
if [ "${CPIT_X4:-1}" != 0 ]; then
    bash tools/cockpit/upscale_tex.sh build/tools/su27_x4 \
        "$R/Models/Interior/texture.png"
    CK+=(--tex-dir build/tools/su27_x4)
fi
[ "${SU27_WINDSCREEN:-1}" != 0 ] && CK+=(--glass-obj '^object[.]001$')
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$R/Models/SU-27.xml" 5.23 0 1.42 "$OUT" "${EC[@]}" --aircraft-root "$R" --subdiv 0.05 \
    "${CK[@]}" \
    --subdiv-ac 'Models/Su-27[.]ac$=0.25' --hide-tex '^cavastex[.]png$' --screen '^(radar[.]BG|TWS[.]screen)$=1,0.004' \
    --skip "$SKIP" --glass-obj '^canopy[.]glass$' --glass-obj '^hudglass$' \
    --controls --hands ../cockpit_src/hands/hands.ac --hand 'R=^stick[.]001$@-0.011,-0.452,-0.456' \
    --hand 'L=^ENG1[.]THRTL$@-0.445,-0.491,-0.237,0,0,-90'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
