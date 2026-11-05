#!/bin/bash
# build_f4e.sh -- F-4E Phantom II cockpit for the native VR cockpit (FlightGear McDonnell-F-4-Phantom by Emmanuel
# Baranger, Paolo Amoroso, Manuel Ace; GPL-2.0, FGAddon SVN trunk/Aircraft/McDonnell-F-4-Phantom; Models/, Systems/
# and the -set.xml fetched over HTTPS):
#   1. tools/cockpit/ac2cpit.py: the whole aircraft model Models/f4.xml (exterior f4.ac with a coarser subdivision,
#      interior Models/Interior/interior.xml with both seats: front panel, the WSO's rear panel and radar) without
#      effects, lights, exhausts and pilots; eye from Systems/views.xml "Cockpit View" (x -5.494, y 0, z 0.063).
#      Dropped objects: the inner copies of the canopy glass (intvitre*: with the outer glass they made two lit
#      layers that greyed the view), and the undercarriage, gear doors, pylons, hook, wingtip lamps and afterburner
#      cans of the exterior (out of sight below and behind the cockpit). --fold-selects: the radar scopes' target
#      blips and range labels have compound conditions (AI contacts in range ...) that fold to "hidden" here; without
#      it they all showed at their rest positions. The small scope at the upper right of the panel (radar3: where the
#      real F-4E and the game's own cockpit have the warning-receiver scope) shows a lit screen without FlightGear's
#      English "Radar Menu" text page and its static dots (Menu, blip*, sTarget5, the latter without a select)
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/McDonnell-F-4-Phantom. Output: cockpits/f4e.cpit. Player aircraft 4 =
# F-4E (sure; docs/aircraft_ids).
# Textures x4 (CPIT_X4=1, the default; ck_audit 2026-10-28): tools/cockpit/upscale_tex.sh (Real-ESRGAN animevideov3)
# writes build/tools/f4e_x4 for --tex-dir: the seat (512 px), the canopy frame / tub (interior.png, 256 px), the side
# walls (1024 px). CPIT_X4=0 = the textures as fetched
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/McDonnell-F-4-Phantom
OUT=${1:-cockpits/f4e.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
if [ "${CPIT_X4:-1}" != 0 ]; then
    bash tools/cockpit/upscale_tex.sh build/tools/f4e_x4 \
        "$R/Models/Interior/Seat/seat.png" "$R/Models/Interior/interior.png" \
        "$R/Models/Interior/Panel/Sides/sides.png"
    CK+=(--tex-dir build/tools/f4e_x4)
fi
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$R/Models/f4.xml" -5.494 0 0.063 "$OUT" "${EC[@]}" --aircraft-root "$R" --subdiv 0.05 \
    "${CK[@]}" \
    --subdiv-ac 'Models/f4[.]ac$=0.25' --fold-selects \
    --hide-obj '^(intvitre|roue|axe|articule|porte|catap|hook|lampes[GD]$|pylons|postcomb|Menu$|blip[0-9]+$|sTarget5$)' \
    --skip 'Models/Effects/|Models/lights/|Models/Exhausts/|Models/Pilot/'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
