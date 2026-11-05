#!/bin/bash
# build_f5e_t38.sh -- the F-5E stand-in cockpit (FlightGear T-38 Talon, GPL-2.0) for the native VR cockpit:
#   1. textures 4x by Real-ESRGAN (ncnn-vulkan build, BSD-3; skipped when the tool or its output is missing)
#   2. tools/cockpit/ac2cpit.py: interior + exterior fuselage/wings/tail, smoothed normals, 5 cm subdivision near the eye
#   3. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
#   The T-38 model has no stick and no throttle in its cockpit (its pilot model held them): the FlightGear F-5E's
#   (GPL-2.0, same family) stick (Instruments/Stick: stick.xml, animated by controls/flight/elevator, aileron) and power
#   levers (Instruments/power_lever: two throttles) are added with --add-model where the T-38's are; the controls move
#   (--controls: ps2_video.c ACZ_VR_COCKPIT_CONTROLS) and gloved hands hold them (--hands, from
#   tools/cockpit/build_hands.sh; ACZ_VR_HANDS)
# Inputs (outside the repo): ../cockpit_src/T-38 (FGAddon), ../cockpit_src/F-5E (FGAddon), ../ext_tools/realesrgan.
# Output: cockpits/f5e_t38.cpit.
# Public controls (T38_CTL=f15; default f5e = the F-5E's as above; night_assets 2026-11-04, RELEASE_RESEARCH section 2): the
# FlightGear F-5E package (PRESTES Hangar) is CC BY-NC-SA 3.0 as a whole (its README: "Instruments (from others GPLed FG
# models)", but stick.xml and power_lever.xml carry no author or licence), so it cannot go into a GPL pack. T38_CTL=f15
# takes the FlightGear F-15's (Zaretto, GPL-2.0-only; ../cockpit_src/F-15) front stick and throttle quadrant instead, as
# the cockpit builds graft them: the stick's foot 2 cm ahead of where the F-5E's stood (eye frame 0, -0.976, -0.42; 5 cm of shaft taken
# out: the grip's top 0.49 m above the floor, as the F-5E's 0.47), the quadrant where the F-5E's stood on the left console
# (T38_THR_AT), the gloves on them; cpit_ctlcheck.py: only the stick's foot on the floor and the quadrant's own base cross
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
SRC=../cockpit_src/T-38/Models
UP=../cockpit_src/T-38_x4
F5=../cockpit_src/F-5E/Models/Instruments
ESR=../ext_tools/realesrgan/realesrgan-ncnn-vulkan.exe
OUT=${1:-cockpits/f5e_t38.cpit}
mkdir -p cockpits build/tools
if [ -x "$ESR" ]; then
    mkdir -p "$UP"
    find "$SRC/Interior" -iname '*.png' | while read -r f; do
        b=$(basename "$f")
        [ -f "$UP/$b" ] && continue
        echo "upscale $b"
        "$ESR" -i "$(cygpath -w "$f")" -o "$(cygpath -w "$UP/$b")" -n realesrgan-x4plus \
               -m "$(cygpath -w "$(dirname "$ESR")/models")" > /dev/null 2>&1 || echo "  failed: $b"
    done
fi
TEXOPT=()
[ -d "$UP" ] && TEXOPT=(--tex-dir "$UP" --max-tex-for 'Instruments/' 1024)
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
CTL=(--add-model "$F5/Stick/stick.xml@-3.004,0,-0.896" --add-model "$F5/power_lever/power_lever.xml@-3.064,-0.41,-0.569")
HANDS=(--hand 'R=^stick-handle$' --hand 'L=^lever0$@-0.34,-0.515,-0.318')
if [ "${T38_CTL:-f5e}" = f15 ]; then                       # night_assets 2026-11-04: GPL controls (see the header)
    CB=build/cockpit/t38_ctl
    mkdir -p "$CB"
    F15=../cockpit_src/F-15/Models/Cockpit/all-interior.ac
    python -I tools/cockpit/ac_flatten.py "$F15" "$CB/stick.ac" '^(stick-mast|stick-handle-parts|pickle-button)$' \
        --within -5.7,-5.3,-0.2,0.2,0.3,1.1 --stretch '^stick-mast$@0.52,0.05' --offset 5.508,0.012,-0.465 --crop-tex 0.5
    python -I tools/cockpit/ac_flatten.py "$F15" "$CB/thr.ac" '^(throttle-.*|left-throttle-grip|poly|cylinder)$' \
        --within -5.62,-5.33,-0.45,-0.26,0.6,0.86 --offset 5.48,0.35,-0.651 --crop-tex 0.5
    TH=${T38_STICK_THROW:-10}; TR=${T38_STICK_ROLL:-9}     # (further, the shaft meets the seat pan: cpit_ctlcheck.py)
    cat > "$CB/stick.xml" <<EOF
<?xml version="1.0"?>
<!-- written by tools/cockpit/build_f5e_t38.sh: the FlightGear F-15's front stick (GPL-2.0), its frame: x aft, y right,
     z up, the origin at the foot of the boot (its gimbal) -->
<PropertyList>
 <path>stick.ac</path>
 <animation><type>rotate</type><object-name>stick-mast</object-name><object-name>stick-handle-parts</object-name>
  <object-name>pickle-button</object-name><property>controls/flight/elevator</property><factor>$TH</factor>
  <center><x-m>0</x-m><y-m>0</y-m><z-m>0</z-m></center><axis><y>-1</y></axis></animation>
 <animation><type>rotate</type><object-name>stick-mast</object-name><object-name>stick-handle-parts</object-name>
  <object-name>pickle-button</object-name><property>controls/flight/aileron</property><factor>$TR</factor>
  <center><x-m>0</x-m><y-m>0</y-m><z-m>0</z-m></center><axis><x>-1</x></axis></animation>
</PropertyList>
EOF
    cat > "$CB/thr.xml" <<EOF
<?xml version="1.0"?>
<!-- written by tools/cockpit/build_f5e_t38.sh: the FlightGear F-15's throttle quadrant (GPL-2.0), its frame: x aft, y
     right, z up, the origin at the foot of the base; the levers about their shaft, modelled at MIL: the runtime's lever
     0 IDLE -35 deg, 0.95 MIL 0, 1 MAX +15 -->
<PropertyList>
 <path>thr.ac</path>
 <animation><type>rotate</type><object-name>throttle-left-wire</object-name><object-name>throttle-left-manual-sbr-switch</object-name>
  <object-name>throttle-left-finger-lift</object-name><object-name>left-throttle-grip</object-name>
  <property>controls/engines/engine[0]/throttle</property>
  <interpolation><entry><ind>0</ind><dep>-35</dep></entry><entry><ind>0.95</ind><dep>0</dep></entry>
   <entry><ind>1</ind><dep>15</dep></entry></interpolation>
  <center><x-m>-0.0052</x-m><y-m>-0.0214</y-m><z-m>-0.006</z-m></center><axis><y>-1</y></axis></animation>
 <animation><type>rotate</type><object-name>throttle-missile-select-switch</object-name><object-name>cylinder</object-name>
  <object-name>throttle-speedbrake-switch-mount</object-name><object-name>throttle-microphone-switch</object-name>
  <object-name>throttle-target-designator-switch</object-name><object-name>throttle-right-grip</object-name>
  <object-name>poly</object-name><object-name>throttle-right-finger-lift</object-name>
  <object-name>throttle-target-designator-switch-mount</object-name><object-name>throttle-speedbrake-switch</object-name>
  <property>controls/engines/engine[1]/throttle</property>
  <interpolation><entry><ind>0</ind><dep>-35</dep></entry><entry><ind>0.95</ind><dep>0</dep></entry>
   <entry><ind>1</ind><dep>15</dep></entry></interpolation>
  <center><x-m>-0.0119</x-m><y-m>0.0213</y-m><z-m>-0.0084</z-m></center><axis><y>-1</y></axis></animation>
</PropertyList>
EOF
    # the eye frame (X right, Y up, Z back; eye at -2.604, 0, 0.08) to the model's FlightGear frame (x aft, y right, z up)
    THRAT=${T38_THR_AT:--0.335,-0.649,-0.38}
    IFS=, read -r tx ty tz <<< "$THRAT"
    TFG=$(awk "BEGIN { printf \"%.4f,%.4f,%.4f\", -2.604 + ($tz), $tx, 0.08 + ($ty) }")
    CTL=(--add-model "$CB/stick.xml@-3.024,0,-0.896" --add-model "$CB/thr.xml@$TFG")
    HANDS=(--hand 'R=^stick-handle-parts$@0.004,-0.554,-0.428,0,-8,0' --hand 'L=^left-throttle-grip$')
fi
python -I tools/cockpit/ac2cpit.py "$SRC/Interior/interior.xml" -2.604 0 0.08 "$OUT" "${EC[@]}" --skip 'Pilot|Effects|light' \
    --subdiv 0.05 "${TEXOPT[@]}" --controls "${CTL[@]}" --hands ../cockpit_src/hands/hands.ac \
    "${HANDS[@]}" --extra "$SRC/t38talon.ac" \
    '^(fuselage|ailes2?|slat[DG]|aileron[DG]|volet[DG]|direction|airs|echappes|tanks|porte[A-Z][0-9]|spoiler[DG]|antennes)$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
