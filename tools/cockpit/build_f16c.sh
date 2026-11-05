#!/bin/bash
# build_f16c.sh -- F-16C cockpit for the native VR cockpit (FlightGear F-16 by Nikolai V. Chr. and contributors,
# GPL-2.0; github.com/NikolaiVChr/f16, master):
#   1. tools/cockpit/ac2cpit.py: Models/Cockpit/Main/cockpit.xml (sub-models through "Aircraft/f16/..." paths:
#      --aircraft-root; FlightGear's shared altimeter from fgdata in ../cockpit_src/fgdata), one variant of each
#      alternative (no block-10 stick, basic HUD, helmet sight, MLU threat panel, pilot or kneeboards), eye from
#      f16-base.xml "Cockpit View" (x -4.105, y 0, z 0.8579) lowered 2.5 cm to z 0.8329 (bug #154, ck_align 2026-10-28:
#      from FlightGear's eye the combiner's top was 1.4 deg above the boresight and the HUD frame's top bar ran through
#      it, so the gun cross was cut by the bar and ACZ_VR_HUD_GLASS refused the glass; now the glass reaches 4.8 deg
#      above it, the bar 3.2 deg; over the nose 12.6 deg instead of 14.2 (the real F-16: 15), the eye 0.77 m above the
#      seat cushion) + exterior parts of Models/f16.ac seen from the seat
#      (fuselage with the tub, canopy frame and glass, radome, wing and tail surfaces, pylons)
#      The two MFD canvases (MFDimage1 left, MFDimage2 right) are --screen 1 and 2: the runtime shows the game's HUD
#      radar on the left one and its weapon block on the right one (ps2_video.c mfd_*)
#      The centre stack is the Block 52's (the F-16CJ Block 52 of f16-block-52-set.xml, variant 5; the game's F-16C):
#      airspeed, altimeter, ADI, VSI, AoA and the mechanical HSI (the Block 52's electronic HSI is a FlightGear canvas,
#      black here). FlightGear picks them by sim/variant-id selects on whole sub-models, which the converter does not
#      apply: the Block 70's centre display unit (Avionics/CDU) and the electronic HSI (Instruments/EHSI) stood in front
#      of those instruments (bug #97: "what is in the middle screen?" -- a dark display covering the airspeed and
#      altitude gauges that replace the HUD's SPEED / ALT boxes): left out. (Kept as the Block 70 centre with its
#      display as a map screen -- --screen '^cdu_canvas$=3', the standby instruments and HSIs skipped -- the
#      display's upper right half stayed dark behind a centre-console face, 2026-10-23: not offered.)
#      The HUD's combiner (HUDImage2 of Models/Cockpit/hud.ac: FlightGear draws the HUD image on it through an effect
#      over the fully transparent tranbg.png, so it was dropped as invisible and the frame stood empty) is --glass-obj:
#      clear glass (alpha 0), the runtime's ACZ_VR_HUD_GLASS combiner (bug #154, ck_align 2026-10-28)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
#   The controls move (--controls: ps2_video.c ACZ_VR_COCKPIT_CONTROLS) and gloved hands hold them (--hands, from
#   tools/cockpit/build_hands.sh; ACZ_VR_HANDS): the side stick (5 degrees: a force stick), the
#   throttle (FlightGear's Nasal throttle-movement read as the throttle), the pedals
# Inputs (outside the repo): ../cockpit_src/F-16 (Models/Cockpit/**, Models/f16.ac, f16.png, f16trans.png,
# Effects/glass/canopy2.png), ../cockpit_src/fgdata/Aircraft/Instruments-3d/altimeter. Output: cockpits/f16c.cpit.
# Use: ACZ_VR_COCKPIT_MODELS=10=cockpits/f16c.cpit (player aircraft 10 = F-16C; ACZ_BOOT_AIRCRAFT=10 to test).
# Textures x4 (CPIT_X4=1, the default; ck_audit 2026-10-28): tools/cockpit/upscale_tex.sh (Real-ESRGAN animevideov3)
# writes build/tools/f16c_x4 for --tex-dir: the ACES II seat (cushions 128 px, structure 512 px) and the centre console
# (256 px): 2-6 texels a degree. CPIT_X4=0 = the textures as fetched
# Surfaces (CPIT_MAT=1, the default; ck_audit 2026-10-28): the cockpit sill (cockpit_border) and the HUD frame are
# untextured flat grey (2-5 % of every view); ac2cpit --mat-obj gives them matte paint (generated grain and mottling, a
# few per cent; the colour kept on average). CPIT_MAT=0 = as before
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
# Rotation axes as SimGear reads them (CPIT_AXES=1, the default; ck_axes 2026-10-31, bug #262): the throttle's
# cutoff-release lever (PINKY_SWITCH_HANDLE, 15 deg) turns about two points (axis/x1-m .. z2-m), which ac2cpit reads only
# with --axis-objects: about a zero axis the runtime scaled it by cos 15 deg about the throttle's origin, 1.1 cm off the
# grip. Nothing else changes. CPIT_AXES=0 = as before (md5 ddcbebc5)
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/F-16
OUT=${1:-cockpits/f16c.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
if [ "${CPIT_X4:-1}" != 0 ]; then
    bash tools/cockpit/upscale_tex.sh build/tools/f16c_x4 \
        "$R/Models/Cockpit/chair/seat-cushion.png" "$R/Models/Cockpit/chair/seat-cushion2.png" \
        "$R/Models/Cockpit/chair/texture.png" "$R/Models/Cockpit/Textures/panels.png"
    CK+=(--tex-dir build/tools/f16c_x4)
fi
[ "${CPIT_MAT:-1}" != 0 ] && CK+=(--mat-obj '^(cockpit_border|HUDFrame)$=paint@0.25')
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
AX=()      # ck_axes 2026-10-31, bug #262: rotation axes as SimGear reads them (see the header)
[ "${CPIT_AXES:-1}" != 0 ] && AX=(--axis-objects --axis-sg-order)
python -I tools/cockpit/ac2cpit.py "$R/Models/Cockpit/Main/cockpit.xml" -4.105 0 0.8329 "$OUT" "${EC[@]}" "${AX[@]}" --aircraft-root "$R" \
    "${CK[@]}" \
    --subdiv 0.05 --subdiv-extra 0.25 --screen '^MFDimage1$=1' --screen '^MFDimage2$=2' --glass-obj '^HUDImage2$' \
    --skip 'stick-B10|hud_basic|f16_war_basic|hmcs|MLU_threat|pilot_|Pilots/|kneeboard|Avionics/CDU/|Instruments/EHSI' \
    --controls --prop 'controls/engines/engine/throttle-movement=controls/engines/engine/throttle' --hands ../cockpit_src/hands/hands.ac \
    --hand 'R=^side_stick2$' --hand 'L=^GRIP$' --extra "$R/Models/f16.ac" \
    '^(Fuselage|CanopyFrame|CanopyFrameInner|CanopyForward(Inside|Outside)|CanopyBack(Inside|Outside)|Glass|RadarDomeTop|(Left|Right)Upper(Lef|Aileron|Flap|HorizonTail|Speedbrake)|Tail|Rudder\.00[1-6]|WingTips|[LR]WStation[123])$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
