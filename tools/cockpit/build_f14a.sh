#!/bin/bash
# build_f14a.sh -- F-14A cockpit for the native VR cockpit (FlightGear F-14 by Richard Harrison / Zaretto;
# github.com/Zaretto/f-14b, release/1.12; GPL via FGAddon, the GitHub copy has no licence file: personal use):
#   1. tools/cockpit/ac2cpit.py: the merged Models/Cockpit/all-cockpit.xml, eye from f-14-common.xml "Cockpit View"
#      (x -5.3, y 0, z 0.35), exterior parts of Models/f-14b.ac seen from the seat; the JPEG atlases
#      (Cockpit/all-cockpit-atlas-0.jpg, f-14b-all-atlas-0.jpg) converted to PNG next to them with System.Drawing
#      The two centre displays are --screen 1 (the upper one, screen-bkg-off) and 2 (the lower one, screen-bkg): the
#      runtime shows the game's HUD radar and weapon block on them (ps2_video.c mfd_*)
#      FlightGear's own HUD symbols (29 textured quads 0.55 m ahead on the combiner: pipper, seeker, TACAN and
#      ILS pointers, range scales, labels) are left out with --hide-obj: static here, they were a second, frozen HUD
#      over the game's (HUD_COCKPIT_RESEARCH_2026-10-20.md section 7)
#      The canopy and windscreen glass (Canopy_0, Windshield_0: FlightGear draws them by an effect over the fully
#      transparent tranbg.png, so they were dropped as invisible) are --glass-obj: clear glass (alpha 0) that carries
#      the runtime's canopy water, mist and ice (bug #71: no water on the F-14's glass in cloud)
#      The HUD has no combiner glass in the model (the Tomcat's HUD image is seen on the centre windscreen panel,
#      where the windscreen's inner frame InnerWindshield_1 leaves an opening and FlightGear draws its own HUD
#      symbols): --add-glass puts a clear plate HUD_glass 1 cm in front of Windshield_0 over that opening (bug #154,
#      ck_align 2026-10-28; the opening's top is 4.7 deg above the boresight): the runtime's ACZ_VR_HUD_GLASS
#      combiner, while the windscreen itself keeps its weather
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
#   The controls move (--controls: ps2_video.c ACZ_VR_COCKPIT_CONTROLS) and gloved hands hold them (--hands, from
#   tools/cockpit/build_hands.sh; ACZ_VR_HANDS): the stick (handle, roll and mast turn about line objects), the two
#   throttles (their table has MIL at 0.91: --throttle-mil), the pedals (translate); the model's own pilot arms and
#   hands (resting on its thighs: arm1, arm2, wrist, hand, thumb) hide while the gloves show (--hands-hide); its head
#   (L-head, the helmet liv_helmet_pilot, L-visor, L-face-mask, L-mask-tube) is left out (--hide-obj; F14_PILOT_HEAD=1
#   keeps it; ck_align
#   2026-10-28, bug #156's head fade): the figure sits 14 cm behind and 8 cm below FlightGear's cockpit-view eye, so its
#   helmet began 0.105-0.118 m behind the design eye -- inside the user's own head -- and checking six faded 86 %; the
#   seat's headrest is 0.445 m behind the eye (torso and legs stay: the body seen looking down)
# Rotation axes as FlightGear reads them (F14_AXES=1, the default; ck_polish 2026-10-31, bug #258: "very strange
# artefacts when I move the stick ... a piece flies off"): 406 of the model's rotate animations give their axis as a
# line object (<axis><object-name>), which ac2cpit read only for the controls; the others got a zero axis, and the
# runtime's rotation about a zero axis scales the part by cos(angle) about the FlightGear origin 5.3 m behind the eye:
# 38 live parts (airspeed, altimeter, VSI, G and clock pointers, the VDI pitch ladder and bank index, the standby
# attitude drums, the HSD / BDHI compass roses and needles) flew between the panel and 10 m behind the pilot as the
# values changed (a roll of 30 deg moved the VDI ladder 0.7 m towards the pilot), 20 parts with a constant angle
# (gear handle, oil-pressure and other needles) sat displaced or collapsed. --axis-objects gives them their line
# objects; --axis-fg-sense the direction FlightGear turns them with (the stick, throttles, pointers all ran backwards
# with ac2cpit's old sense: push brought the stick aft, right roll tilted it left, power pulled the throttles back);
# --axis-flip the stick's linkage plate stick-roll, whose two roll animations (+20 deg with the handle, -20 deg about
# its own line, meant to cancel) added up to 40 deg because its line is drawn the other way: it swung 15 cm out of the
# stick foot at full roll (in FlightGear too); now it stays upright and follows the handle's roll by 1.8 cm.
# F14_AXES=0 = as before
# Axis lines in SimGear's order (CPIT_AXES=1, the default; ck_axes 2026-10-31, bug #262): --axis-fg-sense reads every
# line from its second vertex to its first, which is SimGear's order (animation.cxx LineCollector: the end with the
# smaller x, i.e. further forward, is the axis' head; at equal x the second vertex -> the first) only on the 288 lines
# drawn so. On the other 119 it turned the parts backwards: the VDI pitch ladder and the standby attitude drum against
# the bank (in a 35 deg right bank the standby horizon fell to the right instead of rising), the HSD compass rose and
# bearings and the BDHI card and needles against the heading (at 070 the BDHI read 290), the RIO's dials, and the stick's
# linkage plate, which needed --axis-flip. --axis-sg-order (without the flip) keeps the stick, throttles, pedals,
# airspeed, altimeter, VSI, G and clock as they were; constants: the gear handle 13.5 cm, the hydraulic isolation switch
# 3 cm. CPIT_AXES=0 = ck_polish's senses (md5 aabd3e29)
# The pilot and RIO figures (F14_FIGURES=0, the default; ck_polish 2026-10-31, bug #258: "legs and arms are extra here
# in the gamepad mode"): FlightGear draws mm_pilot / mm_rio only from 2 m away (a range animation), never from the
# cockpit; ac2cpit --range does the same, so the gamepad view shows the seat, stick and pedals as in the other
# cockpits, and the figure's static right hand no longer stays behind while the stick moves out of it (with Touch the
# gloves hold the controls as before). F14_FIGURES=1 = both figures as before (the pilot without his head)
# Inputs (outside the repo): ../cockpit_src/F-14. Output: cockpits/f14a.cpit. Player aircraft 18 = F-14D (sure; docs/aircraft_ids).
# Surfaces (CPIT_MAT=1, the default; ck_audit 2026-10-28): the canopy frame inside (CanopyInside) is untextured
# near-black; ac2cpit --mat-obj gives them matte paint (generated grain and mottling, a few per cent; the colour kept on
# average). CPIT_MAT=0 = as before
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/F-14
OUT=${1:-cockpits/f14a.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
[ "${CPIT_MAT:-1}" != 0 ] && CK+=(--mat-obj '^CanopyInside$=paint@0.3')
HEAD='|L-head|liv_helmet_pilot|L-visor|L-face-mask|L-mask-tube'      # the pilot figure's head (see above)
[ "${F14_PILOT_HEAD:-0}" = 1 ] && HEAD=
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
PL=()      # ck_polish 2026-10-31, bug #258 (see the header)
if [ "${F14_AXES:-1}" != 0 ]; then       # (CPIT_AXES: ck_axes 2026-10-31, bug #262, see the header)
    if [ "${CPIT_AXES:-1}" != 0 ]; then PL+=(--axis-objects --axis-sg-order)
    else PL+=(--axis-objects --axis-fg-sense --axis-flip '^stick-roll-axis$'); fi
fi
[ "${F14_FIGURES:-0}" != 1 ] && PL+=(--range)
python -I tools/cockpit/ac2cpit.py "$R/Models/Cockpit/all-cockpit.xml" -5.3 0 0.35 "$OUT" "${EC[@]}" --aircraft-root "$R" \
    "${CK[@]}" "${PL[@]}" \
    --add-glass 'HUD_glass=-6.3171,-0.17,0.1611;-6.3171,0.17,0.1611;-5.8148,0.17,0.4161;-5.8148,-0.17,0.4161' \
    --subdiv 0.05 --subdiv-extra 0.25 --screen '^screen-bkg-off$=1' --screen '^screen-bkg$=2' \
    --hide-obj '^(locpointer|gspointer|sw-seeker|piper|tacan-(from|to)|ac-reticle|(g|sw|sp)-label|lock-1|lock-circle-1|master-arm-locked|(sw|gun)-digit|sw-rng-[0-9]+-(scale|pointer|max-tick|min-tick)|closure-rate-(scale|pointer)'"$HEAD"')$' \
    --glass-obj '^(Canopy_0|Windshield_0)$' --controls --throttle-mil 0.91 --hands ../cockpit_src/hands/hands.ac --hand 'R=^handle$' \
    --hand 'L=^throttle-L$' --hands-hide '^(hand|thumb|L[LR]-(wrist|arm[12]))$' --extra "$R/Models/f-14b.ac" \
    '^(Fuselage([.][0-9]+)?(_[0-9])?|Canopy(_[0-9])?(Inside)?|CanopyInside|InnerWindshield_1|Windshield_0|Radome([.]001)?(_[0-9])?|Fin([.]00[12])?_[0-9]|FinRoot[.]001_[0-9]|Rudder[LR]|Stabilator[LR]|Strakes|Pitots|Slat[12][LR]|Spoiler[1-4][LR]|Flap[123](Brow)?[LR]|NACA_[01])$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
