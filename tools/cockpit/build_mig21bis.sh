#!/bin/bash
# build_mig21bis.sh -- MiG-21bis cockpit for the native VR cockpit (FlightGear MiG-21bis by l0k1 and contributors,
# github.com/l0k1/MiG-21bis, GPL-3.0; personal use):
#   1. tools/cockpit/ac2cpit.py: Models/Cockpit.xml (Cockpit.ac + Panel.xml instruments through "Aircraft/MiG-21bis/..."
#      paths: --aircraft-root), eye from Systems/views.xml "Cockpit View" (x -3.33, y 0, z 1.2813) minus the model's
#      own +0.15 m z offset (MiG-21bis.xml), exterior parts of Models/MiG-21bis.ac seen from the seat (hull, canopy,
#      nose cone, pitot, wings, tail); the radar scope placeholder (radarDisplay.png) becomes a dark screen and the
#      gunsight reflector glass with its placeholder scribble (Sight.png) is dropped (the game draws its own HUD)
#      The radar scope (radarDisplay) is --screen 1: the runtime shows the game's HUD radar on it (ps2_video.c mfd_*;
#      the weapon block stays in the HUD)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/MiG-21bis. Output: cockpits/mig21bis.cpit. Player aircraft 28 = MiG-21bis.
# Surfaces (CPIT_MAT=1, the default; ck_audit 2026-10-28): the KM-1 seat and the instrument cage are untextured (flat
# beige / white; 18 % of the view looking back); ac2cpit --mat-obj gives them matte paint (generated grain and mottling,
# a few per cent; the colour kept on average). CPIT_MAT=0 = as before
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/MiG-21bis
OUT=${1:-cockpits/mig21bis.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
[ "${CPIT_MAT:-1}" != 0 ] && CK+=(--mat-obj '^(seat|cage)$=paint@0.2')
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$R/Models/Cockpit.xml" -3.33 0 1.131 "$OUT" "${EC[@]}" --aircraft-root "$R" --skip 'Pilot' \
    "${CK[@]}" \
    --subdiv 0.05 --subdiv-extra 0.25 --blank '^radardisplay\.png$' --hide-tex '^sight\.png$' \
    --screen '^radarDisplay$=1' --extra "$R/Models/MiG-21bis.ac" \
    '^(hull|canopy|canopyFront|canopyFrontGlass|canopyFrontInside(\.002)?|canopyGlass|canopyInside|wings|cone|pitot[A-Za-z]*|hstab|(left|right)_(aileron|flap|elevator)|rudder|spoiler|pylons)$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
