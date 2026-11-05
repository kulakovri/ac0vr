#!/bin/bash
# build_typhoon.sh -- Eurofighter Typhoon cockpit for the native VR cockpit (FlightGear EFA Typhoon by Herbert Wagner
# (HerbyW), Maverick Alex, Detlef Faber, AlMurSi, Algernon; GPL-2.0 per README.md; github.com/HerbyW/eurofighter,
# master; Models/Instruments/**, Models/Typhoon.xml, Models/typhoon.ac and its textures fetched):
#   1. tools/cockpit/ac2cpit.py: the cockpit parts of Models/Typhoon.xml -- Instruments/COCKPIT/cockpit.xml, the three
#      MFDs, MIDS, pedals and stick at Typhoon.xml's offsets -- through a root XML written here (Typhoon.xml itself
#      would bring lights, stores, gear and effects; its HUD models are the HUD symbology the game draws itself);
#      exterior parts of Models/typhoon.ac seen from the seat via --extra (same frame: origin at the nose, x aft);
#      eye from typhoon-config.xml view 0 (x 3.91, y 0, z 1.275).
#      The aircraft in flight: electrics, hydraulics, engines and fuel cocks on (--const), MFDs powered with one page
#      each (FlightGear selects pages by name, all pages are separate models: --const-selects decides those selects
#      here): left EADI (attitude), centre situation, right stores (FlightGear's own default there); MDEF on its
#      default subsystem page, MIDS home page; day lighting (the night-lit label copies and flood light volumes stay
#      hidden). The round HUD combiner glass GlassHud (dark grey at the 15 % glass cap it put a grey disc over the
#      middle of the view, behind the game's own HUD: dropped until 2026-10-28) is --glass-obj: clear glass, the
#      runtime's ACZ_VR_HUD_GLASS combiner (bug #154, ck_align). Dropped: the button pick quads of every MFD page (2 % opaque), the
#      terrain-map pixels of the moving map
#      (961 tiles driven by a terrain sampler the game does not have). Added: the ejection seat of Models/pilotN (at
#      Typhoon.xml's pilot offsets, without the pilot figure): FlightGear hides that whole model within 2 m of the
#      viewer, so its cockpit has no seat, and the view back would show none.
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/eurofighter (Models/Instruments/**, Models/pilotN/**, Models/Typhoon.xml,
# typhoon.ac and its textures); the JPEG textures under Models/Instruments and pilotN/seat.jpg converted to PNG next
# to them with System.Drawing. Output: cockpits/typhoon.cpit. Player aircraft 2 = Typhoon (docs/aircraft_ids).
# Surfaces (CPIT_MAT=1, the default; ck_audit 2026-10-28): the Mk 16A seat and the stick grip are untextured (one flat
# dark grey, 32 % of the view looking back); ac2cpit --mat-obj gives them matte paint (generated grain and mottling, a
# few per cent; the colour kept on average). CPIT_MAT=0 = as before
# Screens (CPIT_SCREENS=1, the default; ck_audit 2026-10-28): the centre and right head-down displays' faces
# (MFDCenterPlane, MFDRightPlane) are --screen 1 and 2: the game's HUD radar / big map on the centre one, its weapon
# block on the right one (ps2_video.c mfd_*), as in the other cockpits with screens; they were a black grid page and the
# static stores page (blacklayer, gridlayer, STORScreen: left out) while the radar and the weapons floated in the HUD.
# The left one keeps its attitude page. Also the Rafale M's (23) stand-in. CPIT_SCREENS=0 = as before
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/eurofighter
OUT=${1:-cockpits/typhoon.cpit}
mkdir -p cockpits build/tools build/cockpit
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
[ "${CPIT_MAT:-1}" != 0 ] && CK+=(--mat-obj '^(Seat|stick-handle.*)$=paint@0.2')
if [ "${CPIT_SCREENS:-1}" != 0 ]; then
    CK+=(--screen '^MFDCenterPlane$=1' --screen '^MFDRightPlane$=2')
    CKHIDE="$CKHIDE|^(blacklayer|gridlayer|STORScreen)$"
fi
ROOT=build/cockpit/typhoon_root.xml
I=Aircraft/eurofighter/Models/Instruments
cat > "$ROOT" <<EOF
<?xml version="1.0"?>
<!-- written by tools/cockpit/build_typhoon.sh: the cockpit models of Models/Typhoon.xml at its offsets -->
<PropertyList>
 <model><path>$I/COCKPIT/cockpit.xml</path></model>
 <model><path>$I/MFDs/mfd1.xml</path>
  <offsets><x-m>3.364</x-m><y-m>0</y-m><z-m>0.85</z-m><pitch-deg>-13</pitch-deg></offsets></model>
 <model><path>$I/MFDs/mfd2.xml</path>
  <offsets><x-m>3.359</x-m><y-m>-0.21</y-m><z-m>0.91</z-m><pitch-deg>-13</pitch-deg></offsets></model>
 <model><path>$I/MFDs/mfd3.xml</path>
  <offsets><x-m>3.359</x-m><y-m>0.21</y-m><z-m>0.91</z-m><pitch-deg>-13</pitch-deg></offsets></model>
 <model><path>$I/MIDS/midsAll.xml</path></model>
 <model><path>$I/HandsOn/pedals.xml</path>
  <offsets><x-m>3.3042</x-m><y-m>0</y-m><z-m>0.5</z-m></offsets></model>
 <model><path>$I/HandsOn/stick.xml</path>
  <offsets><x-m>3.55</x-m><y-m>0</y-m><z-m>0.57</z-m></offsets></model>
 <model><path>Aircraft/eurofighter/Models/pilotN/pilot.xml</path>
  <offsets><x-m>3.83</x-m><y-m>-0.015</y-m><z-m>0.82</z-m><pitch-deg>10</pitch-deg></offsets></model>
</PropertyList>
EOF
E=systems/electrical/outputs
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$ROOT" 3.91 0 1.275 "$OUT" "${EC[@]}" --aircraft-root "$R" --subdiv 0.05 --subdiv-extra 0.25 \
    "${CK[@]}" \
    --sort-tables --hide-tex '^terrain-gradient[.]png$' --glass-obj '^GlassHud$' --hide-obj '^(C[1-5]|L[1-6]|R[1-6]|knob[LR]|(avantbras|bras|corps|lunettes|main|tete)[.]001)$'"$CKHIDE" --const-selects \
    --const $E/bus=28 --const $E/avionics/MIDS=28 --const $E/avionics/MDEF=28 --const $E/avionics/HUD=28 \
    --const $E/avionics/ASI=28 --const $E/avionics/CSG=28 --const $E/FCS=115 --const $E/tacan=28 \
    --const 'systems/typhoon-electrical/outputs/avionics/MFHDD[0]=28' \
    --const 'systems/typhoon-electrical/outputs/avionics/MFHDD[1]=28' \
    --const 'systems/typhoon-electrical/outputs/avionics/MFHDD[2]=28' \
    --const 'instrumentation/MFD[0]/serviceable=1' --const 'instrumentation/MFD[1]/serviceable=1' \
    --const 'instrumentation/MFD[2]/serviceable=1' --const 'instrumentation/MFD[0]/page-selected=situana' \
    --const 'instrumentation/MFD[1]/page-selected=eadi' --const 'instrumentation/MFD[2]/page-selected=stor' \
    --const instrumentation/MIDS/serviceable=1 --const instrumentation/MIDS/page-selected=home \
    --const instrumentation/MDEF/subsystem-page-selected=aids \
    --const systems/hydraulics/circuit1_press=4000 --const systems/hydraulics/circuit2_press=4000 \
    --const 'engines/engine[1]/running=1' --const systems/GPWS/serviceable=1 \
    --const controls/switches/lp-cock-left=1 --const controls/switches/lp-cock-right=1 \
    --const controls/switches/boost-pump-left=1 --const controls/switches/boost-pump-right=1 \
    --const controls/seat/arming-handle=1 --const controls/seat/oxygen=1 \
    --const systems/electrical/suppliers/engine-1-generator=100 \
    --const systems/electrical/suppliers/engine-2-generator=100 \
    --prop 'engines/engine[0]/rpm=engines/engine[0]/n2' --prop 'engines/engine[1]/rpm=engines/engine[1]/n2' \
    --extra "$R/Models/typhoon.ac" \
    '^(fuselage[.]001_(0_0|0_1|1)|canopyframe2?([.]inside)?|canopyglass([.]front)?|CanopyMirror(Left|Right|Top)(Frame|Glass)|Upper[.]cockpit|Base[.]Panel[.]Up|canopyCurve[LR]|cockpitCables|cockpitElectron([.][1-3])?|rearcockpit|Can[.]Rear[.]down|elevator[.][LR]|wing[.]01[LR]|Slat[.][LR]_[01]|vertstab|rudder|speedbrake2?|RefuelProbeFairing|FuelProbeOuter|FLIR[.](base2|base3|lens2)|Intakes|Apu[.]engine|Apu[.]eng[.][0-3])$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
