#!/bin/bash
# build_f22a.sh -- F/A-22A cockpit for the native VR cockpit (FlightGear F-22A Raptor by Phoenix, Silver-Snail,
# Pigs, Clash, Developer0607 and others: cockpit by Silver-Snail and POLSKY1, MFDs by Phoenix, airframe improved by
# FGUK; GPL-2.0 (LICENSE.md); github.com/racerretrocoder/Flightgear-F-22A-Raptor, master; fetched file by file: the
# XMLs, .ac files and textures under Models/Interior/ that Interior.xml uses, Models/F-22-Raptor.xml, Models/F-22.ac and
# the airframe textures seen from the seat, F-22-jsbsim-set.xml, System/MFD.xml, f22-jsbsim.xml):
#   1. tools/cockpit/ac2cpit.py: Models/Interior/Interior.xml where Models/F-22-Raptor.xml puts it (origin, no offset)
#      through a root XML written here; eye from F-22-jsbsim-set.xml view 0 (x -5.1, y 0, z 0.3); exterior parts of
#      Models/F-22.ac seen from the seat via --extra (same frame): canopy frame and the inner canopy glass (gold
#      tinted, at the converter's 15 % glass cap), cockpit sills, the nose and forward fuselage, intakes, wing and tail
#      tops (no gear, bays, stores, lights).
#      SimGear animation semantics (--anim-submodels, --axis-objects, --group-names): Interior.xml animates the
#      objects of its protocp.ac sub-model, the engine page turns its needles about "-axis" line objects, and the MFD
#      pages hide whole AC3D groups. The aircraft in flight: both electrical buses up (MFDs and standby screens on),
#      gear up, internal fuel at 75 % (no drop tanks), one page per MFD (--const-selects; FlightGear's classic
#      static-page MFDs, its default -- the Canvas MFDs are off): left ENG (live RPM / EGT / oil needles), centre PRF
#      (the situation display's compass rose), right FUEL, lower SMS (weapons bay); right standby screen on its HSI
#      page (live heading). Text drawn by FlightGear <text> animations is not converted (the pages show their static
#      graphics). Dropped: the HUD canvas placeholder texture (CavasTex.png) and the combiner tint (the game draws its HUD;
#      the combiner itself, hudglass of Instruments/hud/hud.ac, is --glass-obj: clear glass, the runtime's
#      ACZ_VR_HUD_GLASS combiner -- bug #154, ck_align 2026-10-28),
#      the K-14 gunsight model (HUD symbology), the moving-map terrain pixels, the outer canopy copy (FlightGear draws
#      it with an effect at alpha 0; one glass layer). The display page models are matte (--spec-ac: their
#      specular-1 / shininess-0 materials put a grey sheen over the left MFD; FlightGear lights them by emission).
#      Airframe textures of the default livery (Models/Liveries/AK.xml, set by material animations in
#      F-22-Raptor.xml): --tex-obj for the wings, nose, tails and stabilator tops (the .ac's own wing maps are stale,
#      their DDS rows the other way round). Missing textures by the modeller's absolute paths: --tex-paths. Checked
#      against the game's own F-22A cockpit view: glare shield and MFD rows at the same height, HUD frame around the
#      game's flight path symbol; the F-22's high canopy sills hide the wings from the seat (as in FlightGear).
#   2. tools/cockpit/cpit_bake.c: per-vertex ambient occlusion
# Inputs (outside the repo): ../cockpit_src/F-22, ~100 MB (Interior/pedals.jpg converted to PNG next to it with
# System.Drawing). Output: cockpits/f22a.cpit. Player aircraft 13 = F/A-22A (docs/aircraft_ids).
# Textures x4 (CPIT_X4=1, the default; ck_audit 2026-10-28): tools/cockpit/upscale_tex.sh (Real-ESRGAN animevideov3)
# writes build/tools/f22a_x4 for --tex-dir: the ACES II seat (cushions 128 px, structure 512 px). CPIT_X4=0 = the
# textures as fetched
# Screens (CPIT_SCREENS=1, the default; ck_audit 2026-10-28): the centre and right multifunction displays (mfd2screen,
# mfd3screen) are --screen 1 and 2, lifted 9 mm towards the eye over FlightGear's static page pictures (a compass rose,
# the stores page): the game's HUD radar / big map on the centre one (the F-22's tactical situation display), its weapon
# block on the right one (ps2_video.c mfd_*; their missing texture coordinates are made planar by ac2cpit); the compass
# rose and cursor drawn over the centre page (nav, cursor) are left out. Also the F-35C (14) and X-02 (33) stand-ins.
# CPIT_SCREENS=0 = as before
# Tub surface (F22A_FLOOD, default 0 = off; ck_seats 2026-10-28, bug #168): the tub, consoles and housings (objects
# object, box, Housing, ...: 16 m2) carry Models/Interior/grey-blue-flood.png, a 128 px soft noise mapped once over the
# whole tub (0.5 texel per cm: 10-20 cm blotches, nothing finer; box and Housing sample a single texel, so their
# "128 px" is no resolution issue). F22A_FLOOD=1: tools/cockpit/detail_tex.py writes a 2048 px copy (the blotches kept
# as the mottling, a fine paint grain and specks, the mean colour kept) to build/tools/f22a_tex, first in --tex-dir.
# Off by default: the material darkens the flood to 0.2, and at upscale 4 in M4's light the game's eye dumps show no
# visible difference (offline texel density inside 37.8 -> 19.1 % below 5 texels/deg) for +48 MB of textures.
# Needs numpy + PIL: PYV (default python)
# Public build (F22A_ATLAS=0; default 1 = as fetched; night_assets 2026-11-04, RELEASE_RESEARCH section 2): the four engine
# fire lamps (rightengineflight, leftengineflight: 10 triangles each, 1.5 cm square) sample 45 texels of
# Models/Interior/Instruments/EngineFire/0fdd30ec.png, a 2048 px cockpit atlas with a hash-style name (the same file as in the
# F-23C and F-15-ACTIVE trees): it stays out of a public file. F22A_ATLAS=0 puts a solid dark grey of our own under that
# name first in --tex-dir (ac2cpit --tex-dir: by file name; the mesh's texture coordinates do not matter on one colour)
# Concave polygons by ear clipping (CPIT_EARCLIP=1, the default; ck_earclip 2026-10-29, bug #208): ac2cpit --ear-clip
# triangulates them in their plane as FlightGear (OSG) tessellates them, instead of a fan from the first vertex whose
# stray triangles filled their notches with atlas texels; convex polygons unchanged. CPIT_EARCLIP=0 = the fans as before
# Rotation axes in SimGear's order (CPIT_AXES=1, the default; ck_axes 2026-10-31, bug #262): 45 of the model's axes are
# line objects (the ENG page's needles, the standby HSI card, the throttles, the stick, protocp.ac's switches and knobs).
# --axis-objects read each line from its first vertex to its second; SimGear orders a line's ends by x (animation.cxx
# LineCollector: the end further forward is the axis' head; ac2cpit --axis-sg-order), the other way on 34 of the 45:
# the six ENG page needles (RPM, EGT, oil pressure of both engines) turned anticlockwise -- at 90 % RPM the needle stood
# half way round its arc (as if 50 %) instead of beside the red mark -- and now turn clockwise along their arcs (0 at the
# top). The standby HSI's compass card turns with the heading about a forward line, i.e. clockwise, in FlightGear too
# (at a heading of 070 it showed 290 under the lubber line): --axis-flip '^spinaxis$' turns it the right way. The
# throttles' angle comes from f22/throttlel, which the game has no value for: read as 0 = idle, FlightGear's -27 deg
# now puts them 27 deg aft of the modelled lever (the old sense put them 27 deg forward); --const f22/throttle[lr]
# 0.7714 keeps them where they stood (77 % of their travel, about military power). Knobs and switches move by up to
# 7 mm. CPIT_AXES=0 = as before (md5 14d9f3cd)
set -e
cd "$(dirname "$0")/../.."
R=../cockpit_src/F-22
OUT=${1:-cockpits/f22a.cpit}
mkdir -p cockpits build/tools build/cockpit
CK=()      # ck_audit 2026-10-28: the options below (see the header)
CKHIDE=
if [ "${F22A_FLOOD:-0}" != 0 ]; then
    mkdir -p build/tools/f22a_tex
    "${PYV:-python}" -I tools/cockpit/detail_tex.py \
        "$R/Models/Interior/grey-blue-flood.png" build/tools/f22a_tex/grey-blue-flood.png 2048
    CK+=(--tex-dir build/tools/f22a_tex)
fi
if [ "${CPIT_X4:-1}" != 0 ]; then
    bash tools/cockpit/upscale_tex.sh build/tools/f22a_x4 \
        "$R/Models/Interior/chair/seat-cushion.png" "$R/Models/Interior/chair/seat-cushion-2.png" \
        "$R/Models/Interior/chair/texture.png"
    CK+=(--tex-dir build/tools/f22a_x4)
fi
if [ "${F22A_ATLAS:-1}" = 0 ]; then                        # night_assets 2026-11-04: see the header
    mkdir -p build/tools/f22a_plain
    python -I tools/cockpit/solid_png.py build/tools/f22a_plain/0fdd30ec.png 44,47,50
    CK+=(--tex-dir build/tools/f22a_plain)
fi
if [ "${CPIT_SCREENS:-1}" != 0 ]; then
    CK+=(--screen '^mfd2screen$=1,0.009' --screen '^mfd3screen$=2,0.009')
    CKHIDE="$CKHIDE|^(nav|cursor)$"
fi
ROOT=build/cockpit/f22a_root.xml
cat > "$ROOT" <<'EOF'
<?xml version="1.0"?>
<!-- written by tools/cockpit/build_f22a.sh: the interior where Models/F-22-Raptor.xml puts it -->
<PropertyList>
 <model>
  <path>Aircraft/F-22/Models/Interior/Interior.xml</path>
 </model>
</PropertyList>
EOF
F=consumables/fuel/tank
J=fdm/jsbsim/fcs
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)            # ck_earclip 2026-10-29: concave polygons (see the header)
AX=()      # ck_axes 2026-10-31, bug #262: rotation axes as SimGear reads them (see the header)
[ "${CPIT_AXES:-1}" != 0 ] && AX=(--axis-sg-order --axis-flip '^spinaxis$' --const f22/throttlel=0.7714 --const f22/throttler=0.7714)
python -I tools/cockpit/ac2cpit.py "$ROOT" -5.1 0 0.3 "$OUT" "${EC[@]}" "${AX[@]}" --aircraft-root "$R" --subdiv 0.05 --subdiv-extra 0.25 \
    "${CK[@]}" \
    --anim-submodels --axis-objects --group-names --const-selects --sort-tables \
    --skip 'Instruments/gs/' --hide-tex '^(CavasTex|terrain-gradient)[.]png$' --hide-obj '^(tint|canopy out)$'"$CKHIDE" --glass-obj '^hudglass$' \
    --spec-ac 'Instruments/MFD/=0' --tex-paths \
    --tex-obj '^Left_Wing_1$=Models/Liveries/AK/wing.dds' --tex-obj '^Right_Wing_1$=Models/Liveries/AK/wingright.dds' \
    --tex-obj '^nose-top$=Models/Liveries/AK/attemptnose.dds' --tex-obj '^Left_Keel$=Models/Liveries/AK/AK-TAIL-L.png' \
    --tex-obj '^Right_Keel$=Models/Liveries/AK/AK-TAIL-R.png' \
    --tex-obj '^left-elevator_0$=Models/Liveries/AK/leftelev.dds' \
    --tex-obj '^right-elevator_0$=Models/Liveries/AK/rightelev.dds' \
    --const $J/elec-1=1 --const $J/elec-2=1 --const controls/small-screens/rightscreen/mode=2 \
    --const systems/MFD/modemfdl=7 --const systems/MFD/modemfdc=8 --const systems/MFD/modemfdr=6 \
    --const systems/MFD/modemfdll=1 \
    --const "$F[0]/level-norm=0.75" --const "$F[1]/level-norm=0.75" --const "$F[2]/level-norm=0.75" \
    --const "$F[3]/level-norm=0.75" --const "$F[4]/level-norm=0.75" --const "$F[5]/level-norm=0.75" \
    --const "$F[6]/level-norm=0.75" --const "$F[7]/level-norm=0.75" --const "$F[8]/level-norm=0.75" \
    --prop "$J/effected0n1=engines/engine[0]/n1" --prop "$J/effected1n1=engines/engine[1]/n1" \
    --prop "$J/engine0-egt=engines/engine[0]/egt-degf*0.054" --prop "$J/engine1-egt=engines/engine[1]/egt-degf*0.054" \
    --extra "$R/Models/F-22.ac" \
    '^(canopyframe|canopy in|cabin|fillin|frontcp2|nose-top|Fuselage_[02]|Fuselage_2s|(Left|Right)_Intake|(Left|Right)_Wing_1|(left|right)-(slat_[01]|flap_0|aileron_0|elevator_0)|(Left|Right)_Keel_[01]|TAIL-[LR]-OUT|(left|right)-rudder)$'
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
