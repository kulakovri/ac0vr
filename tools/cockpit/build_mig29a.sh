#!/bin/bash
# build_mig29a.sh -- MiG-29A cockpit for the native VR cockpit (FlightGear MiG-29 9.12 by Sergey "Mercenary_Mercury"
# Salov, exterior by Enrique Laso Leon; github.com/Mercenary-Mercury/MiG-29_9-12, master; GPL-3.0, the AChS-1 clock
# GPL-2.0; Models/ fetched file by file):
#   1. tools/cockpit/ac2cpit.py: the whole aircraft model Models/MiG-29.xml (exterior MiG-29.ac, coarser subdivision,
#      and the cockpit Models/Cockpit/Cockpit.xml with its instruments and panels through "Aircraft/MiG-29_9-12/..."
#      paths: --aircraft-root) without stores, effects, lights, pilot; eye from MiG-29_9-12-set.xml "Cockpit View"
#      (x -3.3, y 0, z 1.046); the HUD symbology (ILS-31i, 3D symbols on the combiner) and its head-down copy
#      (IPV-31i) are dropped -- the game draws its own HUD; the IPV's screen (IPV.ekran, the head-down display of the
#      SEI-31 that shows the radar picture, right of the HUD) is --screen 1: the runtime shows the game's radar and big
#      map on it (ps2_video.c mfd_*; bug #97)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
#   The controls move (--controls: ps2_video.c ACZ_VR_COCKPIT_CONTROLS) and gloved hands hold them (--hands, from
#   tools/cockpit/build_hands.sh; ACZ_VR_HANDS): the stick RUS with its boot rings, the two
#   throttles RUD1 / RUD2 (translate), the pedals
# Inputs (outside the repo): ../cockpit_src/MiG-29_9-12. Output: cockpits/mig29a.cpit. Player aircraft 29 = MiG-29A
# (sure; docs/aircraft_ids).
# K-36DM seat texture (MIG29_K36_PAINT=1, the default; ck_seats 2026-10-28, bug #168): the seat's 512 px palette
# texture (Submodels/K-36DM/K-36DM.png, 2 KB) draws the headrest pad's and back cushion's channels as 1-texel black
# lines on a flat brown -- in VR the headbox looked made of planks. tools/cockpit/k36_paint.py repaints it at 2048 px
# as quilted padding (soft stitched grooves, padded channels, grain; the author's colours and layout kept) into
# build/tools/mig29a_k36 for --tex-dir. Needs numpy + PIL: PYV (default python).
# MIG29_K36_PAINT=0 = the texture as fetched
# K-36DM ejection handles (K36_HANDLE=1, the default; ck_polish 2026-10-28, bug #175): the seat model has none; the
# yellow-and-black striped loop between the knees is made by tools/cockpit/eject_handle.py in the K-36DM's frame (as
# build_su27.sh) and placed at the seat's offset in Cockpit.xml (x -3.210, z -0.049). K36_HANDLE=0 = without
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
# Rotation axes as SimGear reads them (CPIT_AXES=1, the default; ck_axes 2026-10-31, bug #262): 157 rotate animations
# give their axis by two points (axis/x1-m .. z2-m), which ac2cpit reads only with --axis-objects; their axis was zero,
# and the runtime scales a part about a zero axis by cos(angle) about its centre (here the FlightGear origin, 3.3 m
# behind the eye): 122 with a non-zero constant angle put 130 parts out of place. Seen from the seat: the canopy
# handle (RUF, -25 deg) stood on the left console 40 cm aft of its place on the left panel; the wheel-brake lever on the
# stick (RUS.RTKs, -9 deg) 5 cm off the stick; the main wheels (retracted: scaled by a negative cosine, mirrored) stuck
# out of the spine 2.4 m behind the eye at shoulder height; the dorsal louvres stood 0.7-1.1 m off, the ailerons 6 cm,
# the nozzle petals 0.2-0.3 m. Now all 157 have their axes (--axis-objects --axis-sg-order; the model has no axis line
# objects) and every part stands as FlightGear shows it; the stick, throttle and pedals turn about explicit axes and do
# not change. CPIT_AXES=0 = as before (md5 91ec7fb4)
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/MiG-29_9-12
OUT=${1:-cockpits/mig29a.cpit}
mkdir -p cockpits build/tools
CK=()
if [ "${MIG29_K36_PAINT:-1}" != 0 ]; then
    mkdir -p build/tools/mig29a_k36
    "${PYV:-python}" -I tools/cockpit/k36_paint.py \
        "$R/Models/Submodels/K-36DM/K-36DM.png" build/tools/mig29a_k36/K-36DM.png
    CK+=(--tex-dir build/tools/mig29a_k36)
fi
if [ "${K36_HANDLE:-1}" != 0 ]; then        # ck_polish 2026-10-28: the K-36DM's pull loop (see the header)
    python -I tools/cockpit/eject_handle.py build/cockpit/mig29a_k36h k36_handle "${K36_HANDLE_LOOP:--0.430,0,0.290@0,1,0@-0.21,0,1@0.14,0.115,0.011}"
    CK+=(--add-model build/cockpit/mig29a_k36h/k36_handle.xml@-3.210,0,-0.049)
fi
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
AX=()      # ck_axes 2026-10-31, bug #262: rotation axes as SimGear reads them (see the header)
[ "${CPIT_AXES:-1}" != 0 ] && AX=(--axis-objects --axis-sg-order)
python -I tools/cockpit/ac2cpit.py "$R/Models/MiG-29.xml" -3.3 0 1.046 "$OUT" "${EC[@]}" "${AX[@]}" --aircraft-root "$R" --subdiv 0.05 \
    "${CK[@]}" --screen '^IPV[.]ekran$=1' --subdiv-ac 'Models/MiG-29[.]ac$=0.25' --controls --hands ../cockpit_src/hands/hands.ac \
    --hand 'R=^RUS$' --hand 'L=^RUD1$@-0.384,-0.478,-0.290,0,0,-90' \
    --skip 'Submodels/(Submodels|Pilot/)|Effects|Lights/|JetFlame|Generic/|Instruments/(ILS-31i/|IPV/IPV-31i)'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
