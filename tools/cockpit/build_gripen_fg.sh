#!/bin/bash
# build_gripen_fg.sh -- the FlightGear JAS 39 Gripen cockpit (Emmanuel Baranger; GPL-2.0, FGAddon SVN
# trunk/Aircraft/JAS39-Gripen; Models/, Systems/ and the -set.xml fetched over HTTPS):
# cockpits/gripen_fg.cpit (ACZ_VR_COCKPIT_MODELS=1=cockpits/gripen_fg.cpit);
#   1. tools/cockpit/ac2cpit.py: the whole aircraft model Models/gripen.xml (exterior gripen.ac with a coarser
#      subdivision, interior Models/Interior/interior.xml: panel, three MFDs, up-front control, seat) without effects,
#      lights, exhausts and pilot; eye from Systems/views.xml "Cockpit View" (x -2.966, y 0, z 0.051) lowered 8 cm
#      to z -0.029 and the seat with it (seat height: Interior/seat at z -0.430 instead of -0.350, by --add-model):
#      FlightGear's view looks 10 deg down (pitch-offset-deg -10), and from its point the HUD's combiner glass (HUD2,
#      0.22 x 0.22 m at 0.68-0.78 m) spans -8.8..9.0 x -17.1..-1.3 deg: the gun cross straight ahead sat above the
#      glass (playtest 2026-10-27 20:07 / 20:13: "outside the collimator I see the gunsight", "my head should be a
#      bit lower"; bug #150). From the lowered eye the glass spans 5.3 deg above to 11.5 deg below the boresight: the
#      gun cross is seen through it with ~4 cm of head travel up, and the HUD's glass clip (ACZ_VR_HUD_GLASS) takes
#      it. The eye stays 0.674 m above the seat cushion and 0.35 m ahead of the headrest, as before.
#      Dropped objects: the inner copies of the windscreen and canopy glass (intcockpit, intverriere: with the outer
#      glass they made two lit layers that greyed the view), the undercarriage and gear doors (out of sight below the
#      cockpit). The right MFD's canvas placeholder (canvasTex.png) becomes a dark screen (automatic). --texrotate:
#      the left MFD's attitude disc (texmultiple: horizon shifted by pitch, turned by roll) is live.
#      The right MFD's canvas (canvasCadre; the left and centre MFDs are fixed pictures on curved bezels) is
#      --screen 1: the runtime shows the game's HUD radar on it (ps2_video.c mfd_*; the weapon block stays in the HUD)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/JAS39-Gripen. Output: cockpits/gripen_fg.cpit. Player aircraft 1 = Gripen
# C (sure; docs/aircraft_ids).
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/JAS39-Gripen
OUT=${1:-cockpits/gripen_fg.cpit}
mkdir -p cockpits build/tools
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$R/Models/gripen.xml" -2.966 0 -0.029 "$OUT" "${EC[@]}" --aircraft-root "$R" --subdiv 0.05 \
    --subdiv-ac 'Models/gripen[.]ac$=0.25' --fold-selects --texrotate \
    --hide-obj '^(int(cockpit|verriere)$|axe|roue|porte|lampe(rouge|verte)$)' --screen '^canvasCadre$=1' \
    --skip 'Models/Effects/|Models/light/|Models/Exhausts/|Models/Pilot/|Interior/seat/seat[.]xml' \
    --add-model "$R/Models/Interior/seat/seat.xml@-2.790,0,-0.430"
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
