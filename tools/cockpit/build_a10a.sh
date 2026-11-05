#!/bin/bash
# build_a10a.sh -- A-10A cockpit for the native VR cockpit (FlightGear A-10 by l0k1 and contributors, GPL-2.0;
# github.com/l0k1/A-10, master):
#   1. tools/cockpit/ac2cpit.py: the whole aircraft model Models/A-10-model.xml (exterior A10-004-015l3.ac and the
#      cockpit sub-models through "Aircraft/A-10/..." paths: --aircraft-root) without stores, effects, lights, pilot;
#      eye: the pilot figure's eyes (x 3.07, y 0, z 1.50; A-10-base.xml "Cockpit View" has x 3.20: see below);
#      canvas placeholders become dark screens (the HUD one is dropped: the game draws its HUD)
#      The right display (canvas_disp, the only canvas) is --screen 1: the runtime shows the game's HUD radar on it
#      (ps2_video.c mfd_*; the weapon block stays in the HUD)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/A-10 (Models/Cockpit/**, Models/A-10-model.xml, A10-004-015l3.ac,
# A-10-000.png, A-10-000B.png). Output: cockpits/a10a.cpit. Player aircraft 21 = A-10A (ACZ_BOOT_AIRCRAFT=21 to test).
# The ACES II ejection seat (A10A_SEAT=1, the default; ck_audit 2026-10-28): FlightGear keeps the seat in the pilot
# model Models/pilot.xml / pilot.ac (groups "seat" and "Pilot"; the pilot is hidden in the cockpit view), which this
# build skipped with the pilot: looking back showed the empty tub and the fuselage behind it.
# tools/cockpit/ac_subtree.py writes Models/pilot_seat.ac = pilot.ac without the "Pilot" group (headbox, back pad,
# cushion, ejection handles, harness, oxygen hose; textures aces2.png, Cockpit/eject-handle.png,
# Cockpit/grey-blue-flood.png, white-black-gradient-stripes.png), taken with --extra in the model's own frame (pilot.xml
# has no offsets); +33k triangles. Inputs from https://raw.githubusercontent.com/l0k1/A-10/master/Models/ (pilot.ac of
# 2025-09-11, commit d680b26; aces2.png, white-black-gradient-stripes.png -- the latter also gives the cockpit heater
# tube its stripes). A10A_SEAT=0 = no seat as before
# Textures x4 (CPIT_X4=1, the default; ck_audit 2026-10-28): tools/cockpit/upscale_tex.sh (Real-ESRGAN animevideov3)
# writes build/tools/a10a_x4 for --tex-dir: the floor (cpt-floor.png, 256 px, blotchy heel scuffs), the quilted side
# insulation (A-10-isolant.png, 1024 x 128) and the seat (aces2.png, 512 px). CPIT_X4=0 = the textures as fetched
# Surfaces (CPIT_MAT=1, the default; ck_audit 2026-10-28): the cockpit rim (CPTBorder) and the canopy frame inside
# (BCanopyFrameInner) are untextured (16 % of the inside views); ac2cpit --mat-obj gives them matte paint (generated
# grain and mottling, a few per cent; the colour kept on average). CPIT_MAT=0 = as before
# Design eye (A10A_EYE_X, default 3.07; ck_fit 2026-10-28): FlightGear's "Cockpit View" x 3.20 sits inside the back of
# the model's own pilot figure's head (pilot.ac: helmet x 2.999..3.264, its back against the seat's headrest pad at
# 3.277; the face 3.034..3.049 and the visor 2.991..3.002 at eye height z 1.47..1.53), 13 cm behind the figure's eyes:
# with the ACES II seat the headbox was 0.105 m (cpit_headvol) behind the eye and checking six faded to black. The eye
# now sits where the figure's eyes are (x 3.07, height unchanged 1.50 = 0.78 m over the cushion): headrest 0.225 m
# behind, the HUD combiner margin 2.5 -> 2.9 deg, over the nose (the HUD camera) 10.4 -> 12.1 deg.
# A10A_EYE_X=3.20 = FlightGear's view as before
# Headbox sides (A10A_HEADBOX=1, the default; ck_seats 2026-10-28): the seat's headbox block (pilot_seat.ac "block",
# 23 x 20 x 7 cm, 0.21-0.28 m behind the eye) maps one flat texel region of aces2.png (std 0.5 of 255): checking six it
# fills half of each over-shoulder view as an even grey slab. ac2cpit --mat-box gives that block (inside its box: FG x
# 3.27..3.355, z 1.42..1.66) matte paint (generated grain and mottling, a few per cent) in its own mean colour
# (0.65 x texel 163 171 171). A10A_HEADBOX=0 = as before
# Headbox rivets (A10A_RIVETS=1, the default with A10A_HEADBOX; ck_polish 2026-10-28, bug #173): the paint alone read as a
# faint mottling in game; the block is fabricated sheet metal, so it takes ac2cpit's 'riveted' paint instead (a panel
# seam with a lit lip and a row of domed rivet heads, 4 mm across, every 2 cm along two edges of each 11 cm tile,
# box-projected). Texture only. A10A_RIVETS=0 = the plain paint (as ck_seats' A10A_HEADBOX)
EX=${A10A_EYE_X:-3.07}
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
# Rotation axes as SimGear reads them (CPIT_AXES=1, the default; ck_axes 2026-10-31, bug #262): the lights panel's
# ANTI-COLLISION and POSITION FLASH / STEADY toggles on the right console (lx_strobe-switch, lx_nav-flash-switch, -30
# deg = OFF) turn about line objects, which ac2cpit read only with --axis-objects: about a zero axis the runtime scaled
# them by cos 30 deg towards the FlightGear origin 3.1 m ahead of the eye, and they were missing from the panel (43 cm
# forward and 10 cm down). Now in their slots, their lines ordered by X, Y, Z as cockpit.xml's
# <defaults><axis-animation-vertex-order-xyz/> asks (ac2cpit --axis-objects --axis-sg-order). CPIT_AXES=0 = as before
# (md5 2a260cf2)
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/A-10
OUT=${1:-cockpits/a10a.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
if [ "${A10A_SEAT:-1}" != 0 ]; then
    for f in pilot.ac aces2.png white-black-gradient-stripes.png; do
        [ -f "$R/Models/$f" ] || { echo "missing $R/Models/$f: fetch https://raw.githubusercontent.com/l0k1/A-10/master/Models/$f"; exit 1; }
    done
    python -I tools/cockpit/ac_subtree.py "$R/Models/pilot.ac" "$R/Models/pilot_seat.ac" --drop Pilot
    CK+=(--extra "$R/Models/pilot_seat.ac" '.')
    if [ "${A10A_HEADBOX:-1}" != 0 ]; then  # ck_seats 2026-10-28 (see the header): the box in the eye frame
        z0=$(awk "BEGIN { print 3.27 - $EX }"); z1=$(awk "BEGIN { print 3.355 - $EX }")
        HBK=paint@0.15; [ "${A10A_RIVETS:-1}" != 0 ] && HBK=riveted@0.11
        CK+=(--mat-box "^block\$=$HBK:0.416,0.437,0.437@-0.13,-0.08,$z0,0.13,0.16,$z1")
    fi
fi
if [ "${CPIT_X4:-1}" != 0 ]; then
    bash tools/cockpit/upscale_tex.sh build/tools/a10a_x4 \
        "$R/Models/Cockpit/cpt-floor.png" "$R/Models/Cockpit/A-10-isolant.png" "$R/Models/aces2.png"
    CK+=(--tex-dir build/tools/a10a_x4)
fi
[ "${CPIT_MAT:-1}" != 0 ] && CK+=(--mat-obj '^(CPTBorder|BCanopyFrameInner)$=paint@0.3')
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
AX=()      # ck_axes 2026-10-31, bug #262: rotation axes as SimGear reads them (see the header)
[ "${CPIT_AXES:-1}" != 0 ] && AX=(--axis-objects --axis-sg-order)
python -I tools/cockpit/ac2cpit.py "$R/Models/A-10-model.xml" "$EX" 0 1.50 "$OUT" "${EC[@]}" "${AX[@]}" --aircraft-root "$R" --subdiv 0.05 \
    "${CK[@]}" \
    --screen '^canvas_disp$=1' \
    --skip 'Stores|Effects|pilot|marker|lights|chute|Trajectory|GAU'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
