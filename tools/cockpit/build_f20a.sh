#!/bin/bash
# build_f20a.sh -- F-20A Tigershark cockpit for the native VR cockpit (FlightGear F-20 by Enrique Laso and
# contributors; github.com/FGMEMBERS/F-20, master; COPYING is the GPL-2.0, the author once called it "hopefully GPL";
# Models/ fetched file by file):
#   1. tools/cockpit/ac2cpit.py: the production F-20A model Models/f-20A.xml (exterior f-20.ac, coarser subdivision;
#      the common cockpit f-20-cockpit.xml with its included instrument XMLs and the F-20A panel f-20A-cockpit.xml,
#      not the prototype's f-20p-cockpit.xml) without effects, afterburner, lights, nozzle, ground equipment, stores;
#      eye from F-20A-set.xml "Cockpit View" (x -3.42, y 0, z 0.55). The DDIs and the engine/clock displays are
#      Canvas screens (screenbg.png: black); the HUD image placeholder is fully transparent (dropped); the small upper
#      HUD combiner plate (HUDglass2, a second glass layer over the main combiner) and the near-transparent helper
#      surfaces (transparent.png) are dropped; the attitude indicator's ball and symbol (near-black diffuse, 0.5
#      ambient material: mid grey in FlightGear) take their ambient colour
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/F-20. Output: cockpits/f20a.cpit. Player aircraft 16 = F-20A (sure;
# docs/aircraft_ids).
# Textures x4 (CPIT_X4=1, the default; ck_audit 2026-10-28): tools/cockpit/upscale_tex.sh (Real-ESRGAN animevideov3)
# writes build/tools/f20a_x4 for --tex-dir: the Stencel SIIIS seat (512 px). CPIT_X4=0 = the textures as fetched
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/F-20
OUT=${1:-cockpits/f20a.cpit}
mkdir -p cockpits build/tools
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
if [ "${CPIT_X4:-1}" != 0 ]; then
    bash tools/cockpit/upscale_tex.sh build/tools/f20a_x4 \
        "$R/Models/SIIIS.png"
    CK+=(--tex-dir build/tools/f20a_x4)
fi
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$R/Models/f-20A.xml" -3.42 0 0.55 "$OUT" "${EC[@]}" --aircraft-root "$R" --subdiv 0.05 \
    "${CK[@]}" \
    --subdiv-ac 'Models/f-20[.]ac$=0.25' --hide-obj '^HUDglass2$' --hide-tex '^transparent[.]png$' \
    --ambient-obj '^(Horizon|Mockup|LateralDeviation|VerticalDeviation|CageKnob)$' \
    --skip 'Effects/|AfterBurner|Models/lighting[.]xml|start-cart|chocks|smoke-generator|lasagna|inner-nozzle|nozzle-pre-C|Stores/'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
