#!/bin/bash
# build_j35j.sh -- J35J Draken cockpit for the native VR cockpit (FlightGear SaabJ35F, GPL-2.0, FGAddon SVN
# trunk/Aircraft/SaabJ35F; Model/, Instruments/ and Radar/ fetched over SVN-HTTP):
#   1. tools/cockpit/ac2cpit.py: Model/draken_internal.xml (instruments and switches; its "Model/..." paths are
#      relative to the aircraft folder: --aircraft-root), eye from SaabJ35F-set.xml "Cockpit View" (x -3.42, y 0,
#      z 0.90) moved 6 cm forward to x -3.48 (J35J_EYE_X=-3.42 builds the old place; ck_align 2026-10-28, bug #156's head fade: the seat's headrest was 0.145 m
#      behind the eye, a helmeted head needs ~0.20 -- checking six faded 30 %; now 0.205 m, no fade; the combiner 4.1 deg
#      above / 7.4 below the boresight, margin 4.1), exterior parts of Model/draken.ac seen from the seat (fuselage, canopy and glass, mirrors, elevons, fin)
#      The radar scope in the middle of the panel (Radar/radar.xml, included by Model/instruments.xml: bezel plate,
#      two knobs and the round PPI face "Screen", a FlightGear canvas) is --screen 1: the runtime shows the game's HUD
#      radar and its big map there (ps2_video.c mfd_*). Without Radar/ the panel had a blank hole in its middle.
#      Model/drakenc.png as the cockpit's author left it has two islands of raw ambient-occlusion bake that were
#      never painted (upstream Resources/TODO: "More detailed seat"): the ejection seat (a white bucket around a flat
#      brown pan) and the tub around it (foot wells, knee panels, floor), which is what the head sees looking down in
#      VR (bug #50, "a plain box"). With J35J_PAINT=1 (the default) tools/cockpit/j35j_paint.py paints them in the
#      texture's style into build/tools/j35j_tex/drakenc.png (read through --tex-dir; ~12 s): grey-green seat bucket
#      with screws, worn edges and the seat's own "VARNING KRUT" placard on the pan walls, olive drab cushions with the
#      unbuckled harness, the cockpit paint with rivets, grime and scuffed heel rests on the tub. J35J_PAINT=0 builds
#      the texture as fetched (the cockpit file of 2026-10-22, md5 d39d1a77..., byte for byte)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
#   The controls move (--controls: ps2_video.c ACZ_VR_COCKPIT_CONTROLS) and gloved hands hold them (--hands, from
#   tools/cockpit/build_hands.sh; ACZ_VR_HANDS): the control column, the throttle, the pedals
# Inputs (outside the repo): ../cockpit_src/SaabJ35F. Output: cockpits/j35j.cpit. Player aircraft 0 = J35J (sure;
# docs/aircraft_ids).
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/SaabJ35F
OUT=${1:-cockpits/j35j.cpit}
mkdir -p cockpits build/tools
if [ ! -f "$R/Radar/radar.ac" ]; then
    echo "missing $R/Radar (the radar scope): fetch radar.xml radar.ac radar.png scale.png screen.png from"
    echo "https://svn.code.sf.net/p/flightgear/fgaddon/trunk/Aircraft/SaabJ35F/Radar/ into $R/Radar/"
    exit 1
fi
TEXDIR=()
if [ "${J35J_PAINT:-1}" != 0 ]; then
    mkdir -p build/tools/j35j_tex
    python -I tools/cockpit/j35j_paint.py "$R/Model/drakenc.png" "$R/Model/draken_internal.ac" build/tools/j35j_tex/drakenc.png
    TEXDIR=(--tex-dir build/tools/j35j_tex)
fi
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$R/Model/draken_internal.xml" "${J35J_EYE_X:--3.48}" 0 0.90 "$OUT" "${EC[@]}" --aircraft-root "$R" --skip 'Light/' "${TEXDIR[@]}" \
    --subdiv 0.05 --subdiv-extra 0.25 --screen '^Screen$=1' --controls --hands ../cockpit_src/hands/hands.ac --hand 'R=^Control$' \
    --hand 'L=^Throttle$' --extra "$R/Model/draken.ac" \
    '^(draken_fuselage|draken_canopy|draken_canopy_arc|draken_canopy_glass0[12]|draken_canopy_glassI[12]|Mirror(Left|Right|Top)|draken_elevon_(left|right)|draken_seitenruder|draken_quer_(left|right))$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
