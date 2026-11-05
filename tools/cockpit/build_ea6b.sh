#!/bin/bash
# build_ea6b.sh -- EA-6B Prowler cockpit for the native VR cockpit (FlightGear Grumman-EA-6B-Prowler by Emmanuel
# Baranger; GPL-2.0, FGAddon SVN trunk/Aircraft/Grumman-EA-6B-Prowler; Models/, Systems/ and the -set.xml fetched over
# HTTPS):
#   1. tools/cockpit/ac2cpit.py: the whole aircraft model Models/ea-6b.xml (exterior ea-6b.ac with a coarser
#      subdivision, interior Models/Interior/interior.xml: the pilot's panel, the four seats) without effects, lights
#      and crew; eye from Systems/views.xml "Cockpit View" n=0 (x -5.835, y -0.303: the pilot sits on the left, z 0.820).
#      Dropped objects: the inner copies of the canopy glass (intvitres, inttrscanopy*: with the outer glass they made
#      two lit layers that greyed the view); the stores (ALQ-99 pods and their RAM turbines, AGM-88, tanks, pylons:
#      hidden in FlightGear unless loaded, kept out of the AO and shadows here), undercarriage, gear doors, launch
#      bar, hook and boarding ladders (out of sight below the cockpit); the 31x31 pixel quads of the terrain map
#      (case01..31 of Map/Row: elevations from a Nasal terrain sampler, all 0 here, so they only repeated the map's
#      blue background, at the cost of 961 meshes). The refuelling probe ahead of the windscreen stays. --fold-selects with the AoA indexer thresholds of Systems/ea-6b-base.xml (--const): the indexer and the
#      WHEELS lamp compare the AoA with those properties inside nested conditions; without it all their lamps showed
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/Grumman-EA-6B-Prowler. Output: cockpits/ea6b.cpit. Player aircraft 20 =
# EA-6B (sure; docs/aircraft_ids).
# Textures x4 (CPIT_X4=1, the default; ck_audit 2026-10-28): tools/cockpit/upscale_tex.sh (Real-ESRGAN animevideov3)
# writes build/tools/ea6b_x4 for --tex-dir: the seats (512 px) and the canopy frame / tub (interior.png, 512 px).
# CPIT_X4=0 = the textures as fetched
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/Grumman-EA-6B-Prowler
OUT=${1:-cockpits/ea6b.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
if [ "${CPIT_X4:-1}" != 0 ]; then
    bash tools/cockpit/upscale_tex.sh build/tools/ea6b_x4 \
        "$R/Models/Interior/seat/seat.png" "$R/Models/Interior/interior.png"
    CK+=(--tex-dir build/tools/ea6b_x4)
fi
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$R/Models/ea-6b.xml" -5.835 -0.303 0.820 "$OUT" "${EC[@]}" --aircraft-root "$R" --subdiv 0.05 \
    "${CK[@]}" \
    --subdiv-ac 'Models/ea-6b[.]ac$=0.25' --fold-selects \
    --const sim/aoa-indexer/too-slow-deg=9 --const sim/aoa-indexer/slow-deg=8 --const sim/aoa-indexer/fast-deg=7 \
    --const sim/aoa-indexer/too-fast-deg=6 \
    --hide-obj '^(intvitres|inttrscanopy|AGM88|AN-ALQ|RAMTurbine|Tank[LR]$|attache|axe|roue|porte|launchbar|hook|[LR]stairs|case[0-9]+$)' \
    --skip 'Models/Effects/|Models/light/|Models/Pilot/'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
