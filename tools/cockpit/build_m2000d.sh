#!/bin/bash
# build_m2000d.sh -- Mirage 2000D cockpit for the native VR cockpit (FlightGear Mirage 2000 by 5H1N0B1 and
# contributors, GPL-2.0; github.com/5H1N0B11/flightgear-mirage2000, master; Models/ fetched file by file):
#   1. tools/cockpit/ac2cpit.py: the two-seat Mirage 2000B model Models/m2000-5B.xml (exterior m2000-5.ac, coarser
#      subdivision; interior Interior/interiorB.xml with the textured panel and its instruments, front and rear seat)
#      without effects, stores, lights, pilots. The repository's own 2000D (m2000-D.xml) replaces that panel with
#      interior_d/cockpit_front.ac, an untextured model for FlightGear's experimental HDR renderer that comes out
#      black here. The single-seat canopy hidden by constant selects stays hidden; the inner copy of the canopy glass
#      (FlightGear shades it nearly invisible, here a second lit layer greyed the view) is dropped. Eye from
#      Systems/views.xml "Cockpit View" (x -2.9, y 0, z 0.0093)
#      The two display screens (left_mfd / right_mfd .canvasCadre, front and rear seat) are --screen 1 and 2: the
#      runtime shows the game's HUD radar on the left one and its weapon block on the right one (ps2_video.c mfd_*)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/Mirage2000/Mirage-2000. Output: cockpits/m2000d.cpit. Player aircraft
# 22 = Mirage 2000D (sure; docs/aircraft_ids).
# Textures x4 (CPIT_X4=1, the default; ck_audit 2026-10-28): tools/cockpit/upscale_tex.sh (Real-ESRGAN animevideov3)
# writes build/tools/m2000d_x4 for --tex-dir: the seat (512 px) and the tub / side walls (interieur-BIPLACE.png, 1024
# px). CPIT_X4=0 = the textures as fetched
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
# Rotation axes as SimGear reads them (CPIT_AXES=1, the default; ck_axes 2026-10-31, bug #262): 38 rotate animations give
# their axis by two points (axis/x1-m .. z2-m), which ac2cpit reads only with --axis-objects; their axis was zero, and
# the runtime scales a part about a zero axis by cos(angle): the four main-gear axle parts (axeG1/G2, axeD1/D2, +-22
# deg, outside 4.9 m behind the eye) stood 23 cm off. Nothing seen from the seat changes. CPIT_AXES=0 = as before (md5
# c6c857a6)
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/Mirage2000/Mirage-2000
OUT=${1:-cockpits/m2000d.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
if [ "${CPIT_X4:-1}" != 0 ]; then
    bash tools/cockpit/upscale_tex.sh build/tools/m2000d_x4 \
        "$R/Models/Interior/seat.png" "$R/Models/Interior/interieur-BIPLACE.png"
    CK+=(--tex-dir build/tools/m2000d_x4)
fi
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
AX=()      # ck_axes 2026-10-31, bug #262: rotation axes as SimGear reads them (see the header)
[ "${CPIT_AXES:-1}" != 0 ] && AX=(--axis-objects --axis-sg-order)
python -I tools/cockpit/ac2cpit.py "$R/Models/m2000-5B.xml" -2.9 0 0.0093 "$OUT" "${EC[@]}" "${AX[@]}" --aircraft-root "$R" --subdiv 0.05 \
    "${CK[@]}" \
    --subdiv-ac 'Models/m2000-5[.]ac$=0.25' --hide-obj '^int(vitres|verriere)|canvasHUD' \
    --screen '^left_mfd[.]canvasCadre$=1' --screen '^right_mfd[.]canvasCadre$=2' \
    --skip 'Sounds|Parking|Pushback|External-objects|Effects|turbine|shadow|Exhausts|Logos|Service-door|Models/Pilot/|lights/|Light-Effect'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
