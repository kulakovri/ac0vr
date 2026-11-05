#!/bin/bash
# build_fa18c.sh -- F/A-18C cockpit for the native VR cockpit (FGUK's F-18C, GPL-2.0 (COPYING); github.com/
# racerretrocoder/F-18C, master; only Models/Interior/** (without pilot/), Models/FA-18.xml, Models/F-18.ac and its
# airframe textures fetched):
#   1. tools/cockpit/ac2cpit.py: the interior Models/Interior/interior.xml placed as Models/FA-18.xml places it (at
#      x +0.3, z +0.05 in the aircraft frame) by a two-line root XML written here, so that the eye from F18C-set.xml
#      view 0 (x -3.6, y 0, z 1.2, aircraft frame; -3.9 / 1.15 in the interior's own frame) and the exterior parts of
#      Models/F-18.ac seen from the seat (--extra: windscreen and canopy with their glass, anti-glare panel, forward
#      fuselage, nose, wings, fins, stabilators; no stores, gear, lights) share one frame. FA-18.xml itself is not the
#      root: the stores in F-18.ac are chosen by string selects the converter cannot evaluate (they would all show).
#      Dropped: the canvas HUD (Models/HUD, the game draws its HUD), the 961 terrain-map pixels of the moving map
#      (driven by a terrain sampler the game does not have; dark either way), the AoA indexer lamps (its thresholds
#      are not set anywhere, so all three would glow; the real indexer is dark with the gear up). Fuel gauges at 80 %.
#      The HUD frame has no combiner glass in the model: --add-glass puts a clear plate HUD_glass in it (bug #154,
#      ck_align 2026-10-28), in the plane of its posts (leaning 24 deg back), from the posts' inner edges (-6.2 / +7.4
#      deg from the eye: FlightGear's frame sits 1 cm right of the centre line) and the top bar's lower edge (+3.1
#      deg) down into the HUD body (-8 deg): the runtime's ACZ_VR_HUD_GLASS combiner
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/F-18C; the JPEG textures (Models/fuselage.jpg, wing-stab.jpg, rudder.jpg,
# Interior/TEX_CONS.jpg) converted to PNG next to them with System.Drawing. Output: cockpits/fa18c.cpit.
# Player aircraft 8 = F/A-18C; the launcher can map 9 (EA-18G) and 12 (F-117A, no FlightGear cockpit; its real
# cockpit reused F/A-18 parts) to the same file.
# Surfaces (CPIT_MAT=1, the default; ck_audit 2026-10-28): FGUK's interior is untextured apart from the panel faces --
# the whole tub, consoles and canopy frame one pure black material (black also in FlightGear, whose ambient is 0: here a
# void with floating knobs in every light), the seat one flat grey; the black is lifted to a charcoal 0.13 (black
# cockpit paint as it looks in daylight; the real tub and consoles are dark gull grey / black), the seat keeps its grey.
# Untextured share of the inside views 65 % -> 1.6 %; ac2cpit --mat-obj gives them matte paint (generated grain and
# mottling, a few per cent; the colour kept on average). CPIT_MAT=0 = as before
# Console panels: bare, as FGUK's model has them
# Seat (FA18C_SEAT=1, the default; ck_seats 2026-10-28, bugs #161 / #168): FGUK's SJU-5/6 seat is one grey object
# (EjectionSeat, plus the BackSupport1/2 pads); ac2cpit --mat-box colours it by region (eye-frame boxes, the faces'
# normals, the harness by its small connected parts): olive drab fabric seat cushion, back pad and head pad, khaki
# webbing harness, metal buckles, dark grey bucket and headbox. FA18C_SEAT=0 = the one grey seat
# Throttle (FA18C_THROTTLE=1, the default; ck_polish 2026-10-28, bug #174): FGUK's model has no throttle quadrant at
# all. The GPL-2.0 FlightGear FA-18E_CVW5 by Uchi craft room (Hitoshi Uchida; ../cockpit_src/FA-18E_uchi, UCHI_FA18E)
# has the Hornet's two-lever quadrant (Models/Interior/Panel/power.ac: the levers with their grips, finger lifts,
# speed-brake switch and friction lever on the slotted base; the F/A-18E/F quadrant is the C's). Those parts
# (tools/cockpit/ac_flatten.py; Uchi's own EXT LT / APU switch panel beside it is left out, the console has
# them; the grips' light grey darkened towards the real ones' black plastic: --mat-scale 0.45) stand on the forward
# left console where the real quadrant is: the base on the console top (pitched 6 deg with it), centre X -0.364, the
# levers 0.17-0.21 m ahead of the eye at idle, the grips' tops 0.37 m below it. FGUK's console is too narrow for the
# panels that flank the real quadrant (
# they end at Z 0.445, the end of the flat console). The levers slide forward with the game's throttle (Uchi's 14 cm
# travel: 12 cm idle to military power, 2 cm more to the afterburner stop; ac2cpit --controls, ACZ_VR_COCKPIT_CONTROLS),
# and the gloves hold the levers (the left hand round both grips) and the stick (--hands, ACZ_VR_HANDS; the stick does
# not move without FA18C_STICK). FA18C_THROTTLE=0 = no quadrant, no gloves, the plates from Z -0.296 (as before)
# Stick (FA18C_STICK=1, the default; ck_polish 2026-10-28): FGUK's ControlStick has no animation; here it tilts about
# its base on the floor (eye frame X 0.016, Y -0.717, Z -0.380) with the game's stick, 10 deg pitch / 8 deg roll as
# a Super Hornet's stick (a rotate animation in the root XML reaching interior.ac's object:
# ac2cpit --anim-submodels --controls), and the right glove follows it. FA18C_STICK=0 = the still stick (as before)
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/F-18C
OUT=${1:-cockpits/fa18c.cpit}
mkdir -p cockpits build/tools build/cockpit
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
THR=(); [ "${FA18C_THROTTLE:-1}" != 0 ] && THR=(--left-from -0.125)
if [ "${FA18C_SEAT:-1}" != 0 ]; then        # ck_seats 2026-10-28: SJU-5/6 seat colours by region (see the header)
    S='^(EjectionSeat|BackSupport[12])$'
    CK+=(--mat-box "$S=paint@0.1:0.42,0.42,0.43@-0.14,-0.43,0.12,0.15,-0.36,0.18#<70")            # harness buckles
    CK+=(--mat-box "$S=fabric@0.08:0.33,0.33,0.27@-0.3,-0.8,-0.4,0.3,0.2,0.6#<70")                # harness webbing
    CK+=(--mat-box "$S=fabric@0.12:0.235,0.255,0.18@-0.215,-0.77,-0.35,0.225,-0.58,0.17@0,1,0,0.55")  # seat cushion
    CK+=(--mat-box "$S=fabric@0.12:0.235,0.255,0.18@-0.18,-0.68,0.08,0.19,-0.19,0.34@0,0,-1,0.45")   # back pad
    CK+=(--mat-box "$S=fabric@0.12:0.21,0.225,0.165@-0.16,-0.22,0.2,0.17,0.15,0.47@0,0,-1,0.45")    # head pad
    CK+=(--mat-box "$S=paint@0.2:0.14,0.145,0.15@-0.4,-0.9,-0.5,0.4,0.3,0.7")                    # bucket, headbox
fi
if [ "${FA18C_THROTTLE:-1}" != 0 ]; then    # ck_polish 2026-10-28: the throttle quadrant (see the header)
    U=${UCHI_FA18E:-../cockpit_src/FA-18E_uchi}
    TB=build/cockpit/fa18c_thr
    mkdir -p "$TB"
    python -I tools/cockpit/ac_flatten.py "$U/power.ac" "$TB/thr.ac" '^(base|lever[01]|finger-lift1?|fric|spbk-(switch|opn|cls))$' \
        --mat-scale '^(lever[01]|finger-lift1?|fric)$=0.45'
    # Uchi's colors.png / colors2.png under names of their own (FGUK's interior has a colors.png too)
    sed -i 's/^texture "colors\(2\?\)[.]png"/texture "uchi_colors\1.png"/' "$TB/thr.ac"
    cp "$U/colors.png" "$TB/uchi_colors.png"; cp "$U/colors2.png" "$TB/uchi_colors2.png"
    cat > "$TB/thr.xml" <<'EOF'
<?xml version="1.0"?>
<!-- written by tools/cockpit/build_fa18c.sh: Uchi's F/A-18 throttle quadrant (power_lever.xml's lever travel, the
     runtime's lever 0 idle .. 0.95 military power .. 1 afterburner) -->
<PropertyList>
 <path>thr.ac</path>
 <animation><type>translate</type><object-name>lever0</object-name><object-name>finger-lift</object-name>
  <object-name>spbk-switch</object-name><object-name>spbk-opn</object-name><object-name>spbk-cls</object-name>
  <property>controls/engines/engine/throttle</property>
  <interpolation><entry><ind>0</ind><dep>0</dep></entry><entry><ind>0.95</ind><dep>0.12</dep></entry>
   <entry><ind>1</ind><dep>0.14</dep></entry></interpolation><axis><x>-1</x><y>0</y><z>0</z></axis></animation>
 <animation><type>translate</type><object-name>lever1</object-name><object-name>finger-lift1</object-name>
  <property>controls/engines/engine[1]/throttle</property>
  <interpolation><entry><ind>0</ind><dep>0</dep></entry><entry><ind>0.95</ind><dep>0.12</dep></entry>
   <entry><ind>1</ind><dep>0.14</dep></entry></interpolation><axis><x>-1</x><y>0</y><z>0</z></axis></animation>
</PropertyList>
EOF
    # Uchi's frame: x aft, base -0.192..0.120 long, 0.10 wide, its top 6 mm up; here its aft end 1 cm ahead of the
    # first plate (eye-frame Z -0.135), on the console top (Y -0.495 at Z -0.255, rising 1.75 cm per 16.5 cm aft)
    CK+=(--add-model "$TB/thr.xml@-3.855,-0.3635,0.701,0,-6,0" --hands ../cockpit_src/hands/hands.ac
         --hand 'L=^lever0$@-0.376,-0.405,-0.196' --hand 'R=^ControlStick$')
fi
if [ "${FA18C_THROTTLE:-1}" != 0 ] || [ "${FA18C_STICK:-1}" != 0 ]; then CK+=(--controls); fi
if [ "${CPIT_MAT:-1}" != 0 ]; then
    CK+=(--mat-obj '^(Interior_0|CanopyFrameInside)$=paint@0.3:0.13,0.135,0.14')
    CK+=(--mat-obj '^EjectionSeat$=paint@0.2')
fi
ROOT=build/cockpit/fa18c_root.xml
cat > "$ROOT" <<'EOF'
<?xml version="1.0"?>
<!-- written by tools/cockpit/build_fa18c.sh: the interior where Models/FA-18.xml puts it -->
<PropertyList>
 <model>
  <path>Aircraft/F-18C/Models/Interior/interior.xml</path>
  <offsets><x-m>0.3</x-m><y-m>0</y-m><z-m>0.05</z-m></offsets>
 </model>
</PropertyList>
EOF
if [ "${FA18C_STICK:-1}" != 0 ]; then       # ck_polish 2026-10-28: the stick moves (see the header)
    ROOT=build/cockpit/fa18c_root_stick.xml
    cat > "$ROOT" <<'EOF'
<?xml version="1.0"?>
<!-- written by tools/cockpit/build_fa18c.sh: the interior where Models/FA-18.xml puts it, and the stick's travel
     about its base (FlightGear frame of this file: x aft, y right, z up) -->
<PropertyList>
 <model>
  <path>Aircraft/F-18C/Models/Interior/interior.xml</path>
  <offsets><x-m>0.3</x-m><y-m>0</y-m><z-m>0.05</z-m></offsets>
 </model>
 <animation><type>rotate</type><object-name>ControlStick</object-name><property>controls/flight/elevator</property>
  <factor>10</factor><center><x-m>-3.980</x-m><y-m>0.016</y-m><z-m>0.483</z-m></center><axis><y>-1</y></axis></animation>
 <animation><type>rotate</type><object-name>ControlStick</object-name><property>controls/flight/aileron</property>
  <factor>8</factor><center><x-m>-3.980</x-m><y-m>0.016</y-m><z-m>0.483</z-m></center><axis><x>-1</x></axis></animation>
</PropertyList>
EOF
    CK+=(--anim-submodels)
fi
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
python -I tools/cockpit/ac2cpit.py "$ROOT" -3.6 0 1.2 "$OUT" "${EC[@]}" --aircraft-root "$R" --subdiv 0.05 --subdiv-extra 0.25 \
    "${CK[@]}" \
    --add-glass 'HUD_glass=-4.492,-0.096,1.080;-4.492,0.114,1.080;-4.417,0.114,1.246;-4.417,-0.096,1.246' \
    --skip 'Models/HUD/|Interior/pilot/' --hide-tex '^terrain-gradient[.]png$' \
    --sort-tables --hide-obj '^(too-slow|on-speed|too-fast)$' --const consumables/fuel/total-fuel-norm=0.8 \
    --extra "$R/Models/F-18.ac" \
    '^(CanopyFrame(Inside|Outside)?|CanopyGlas|CanopyGlass|ANTIGLAR01|FuselageTop(SS|DS)?|FuselageUnderside|UNDERSID02|CabineClosure|RadarBay|NOSECONE01|Guns|InnerWing(Top|Bottom|Bits|FoldClosure|FrontClosure|BackClosure)|Inner(Left|Right)LeadingEdgeFlap|(Left|Right)(LeadingEdgeFlap|Flap|Aileron|OutboardBody|FinBody|Rudder|Stabilizer)|OuterWing(Top|Bottom|Bit|BigBit|FoldClosure|FrontClosure|BackClosure)|AirbrakeBay|Airbrake)$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
