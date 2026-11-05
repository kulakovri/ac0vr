#!/bin/bash
# build_tornado_gpl.sh -- Tornado GR.4 cockpit of our own for the public set (night_tornado 2026-11-04; R2 pilot of
# RELEASE_RESEARCH_2026-11-03.md section 15).
# Every input is GPL or made here:
#   the pit      NikolaiVChr/Tornado (GPL-2.0, Heiko Schulz, Markus Zojer, StuartC, Algernon, Gary Brown; FGUK lineage;
#                ReadMe.3dmodel.txt "This 3d-model is released under GNU GPL"), commit 0dd1e5a: Models/Interior/
#                interior-gr4.xml (cockpit.ac, instruments, stick, pedals) and the exterior parts seen from the seat
#                (Models/TornadoGR4.ac, GR4 livery). tools/cockpit/prep_tornado_gpl.sh makes the input folder.
#   the seat     Martin-Baker Mk 10 of the FlightGear Mirage 2000 (github.com/5H1N0B11/flightgear-mirage2000, GPL-2.0;
#                Models/Interior/interiorB.ac seat.seat-BIPLACE, seat.png x4; its seat.frame is hidden inside it): the GPL pit's own
#                seat is 310 faces of flat brown, the Tornado's real seat is a Mk 10
#   the consoles FlightGear A-10 (github.com/l0k1/A-10, GPL-2.0; Models/Cockpit/cockpit.ac: the panels behind the main
#                panel, detail-cpt.ac "consoles": the deck, A-10-throttle.xml: the twin throttles): the GPL pit has no
#                throttle and bare side decks; it takes other parts from the same A-10 itself (SAS, ILS, TACAN, nav mode)
#   instruments  fgdata parts interior-gr4.xml names but its repository does not hold: from that A-10 the VSI
#                (A-10-vsi-6kfpm.xml, 0.72 scale), the standby compass (mag-compass.xml + its box in detail-cpt.ac, on the
#                coaming: the heading the HUD's heading tape drop needs), the clock (A-10-chronometer.xml, for the repo's
#                cyan-faced chronograph); from the repo the radio (common/VHF-22) and the RWR scope (the IDS interior's
#                Instruments/rwr, for GR4's empty grey generic-screen2 frame)
#   made here    tools/cockpit/tornado_gpl_parts.py: three rear-view mirrors on the bow (none in the GPL exterior; the
#                runtime takes meshes named mirror_* by its name rule, ps2_video.c mir_check) and the wing sweep lever;
#                generated surfaces (ac2cpit --mat-box: panel paint, rubber floor, riveted tub)
# No runtime change: no --screen ids (today's has none either; the game's radar and
# weapon block stay in the HUD), the HUD drops follow from the mapped gauges (airspeed, altimeter, attitude, compass:
# "takes over from the HUD: SPEED, ALT, the heading tape, the pitch ladder" as today), the combiner CPT-hud-screen is on
# ACZ_VR_HUD_GLASS_MESH's default list and inner by its size.
# The eye (TORNADO_GPL_EYE_X / TORNADO_GPL_EYE_Z, default -4.3 / -0.13 in the exterior's frame, so the bow,
# wings and nose are seen exactly as today: first solid above the boresight 13.6 deg on the bare windscreen, the same on
# both). FlightGear's own view of this GPL pit is -4.07 / -0.05 in that frame (tornado-GR4a-set.xml z-offset 3.93 minus the
# model's x-m 8, y-offset -0.05): the whole interior therefore moves with the eye (0.23 m forward, 0.08 m down), keeping
# the panel, HUD and stick where the modeller put them relative to the eye: combiner -9.7..7.9 x -9.8..7.3 deg, boresight
# margin 7.15 deg (today's 8.45). From the old eye without the move it would be 18 deg up and 4 deg down with the panel 0.26 m
# away. The tub (Fuselage_skin, cockpit.001) moves 5 cm less down so that its rim meets the canopy sill (8 cm opens a strip of
# the exterior's skin along the sill). TORNADO_GPL_EYE_X=-4.07 TORNADO_GPL_EYE_Z=-0.05 = FlightGear's view (bow at
# ~14 deg instead of ~26 deg in the audit's ahead view: more open, 23 cm further aft under the canopy).
# Switches (all default 1; 0 = the GPL repository's own part or nothing): TORNADO_GPL_SEAT, TORNADO_GPL_CONSOLES,
# TORNADO_GPL_GAUGES, TORNADO_GPL_MIRRORS. CPIT_X4=0: the textures as fetched (x4: seat, deck, pedals, stick).
# TORNADO_GPL_SEAT_BACK (0.14): the seat aft of the Mirage's eye relation, m (the stick at full back stops 2 mm short of the
# handle; head volume 0.445 m clear behind the eye; today's 0.345);
# TORNADO_GPL_CONSOLE_UP (0.03): the consoles above the A-10's eye relation + 0.17 m.
# The stick (TORNADO_GPL_STICK_THROW, 12 deg): 3 cm aft of interior-gr4.xml's place and 12 deg each way for stick.xml's 20:
# with 20 deg it went through the Mk 10's front at full back and 5 cm into the panel at full forward (cpit_ctlcheck).
# TORNADO_GPL_SUBDIV (0.05), TORNADO_GPL_BAKE (1): the AO subdivision and bake (0 and 0 for a fast look).
# Inputs (outside the repo): ../cockpit_src/Tornado_GPL (prep_tornado_gpl.sh), ../cockpit_src/A-10/Models/Cockpit,
# ../cockpit_src/Mirage2000/Mirage-2000/Models/Interior; COCKPIT_SRC=DIR for another folder. Output: cockpits/tornado_gpl.cpit
# or the first argument. Run through a private script (x4 and the bake: about 3 minutes).
set -e
cd "$(dirname "$0")/../.."
CS=${COCKPIT_SRC:-../cockpit_src}
R=$CS/Tornado_GPL
A=$CS/A-10/Models/Cockpit
M=$CS/Mirage2000/Mirage-2000/Models/Interior
D=build/cockpit/tornado_gpl
OUT=${1:-cockpits/tornado_gpl.cpit}
mkdir -p "$D" "$(dirname "$OUT")" build/tools
EX=${TORNADO_GPL_EYE_X:--4.3}
EZ=${TORNADO_GPL_EYE_Z:--0.13}
calc() { awk "BEGIN{printf \"%.4f\", $1}"; }
IX=$(calc "$EX + 4.07")                  # the interior: FlightGear's eye (-4.07, -0.05) goes to the design eye
IZ=$(calc "$EZ + 0.05 - 1.67")           # (-1.67: Tornado-GR4a.xml's <offsets> of interior-gr4.xml)
TZ=$(awk "BEGIN{d=-0.05-($EZ)-0.03; if(d<0)d=0; printf \"%.4f\", $EZ + 0.05 - 1.67 + d}")   # the tub: rim on the sill
GAUGES=${TORNADO_GPL_GAUGES:-1}
SEAT=${TORNADO_GPL_SEAT:-1}
CONSOLES=${TORNADO_GPL_CONSOLES:-1}
MIRRORS=${TORNADO_GPL_MIRRORS:-1}
THROW=${TORNADO_GPL_STICK_THROW:-12}
# interior-gr4.xml without the WSO's instruments (they face aft, away from this eye) and with GAUGES the two it replaces
python -I - "$R/Models/Interior/interior-gr4.xml" "$D/interior_front.xml" "$GAUGES" "$THROW" <<'PY'
import os, sys, xml.etree.ElementTree as ET
t = ET.parse(sys.argv[1]); r = t.getroot(); n = 0
for m in list(r.findall('model')):
    x = m.find('offsets/x-m'); p = m.findtext('path', '')
    if x is not None and -3.6 < float(x.text) < -1.0:
        r.remove(m); n += 1
    elif sys.argv[3] != '0' and p.endswith(('generic-screen2.xml', 'chronograph.xml')):
        r.remove(m); n += 1
    elif p.endswith('divers/stick.xml'):              # 3 cm aft, its throw cut: see the header
        src = os.path.join(os.path.dirname(sys.argv[1]), 'Instruments', 'common', 'divers', 'stick.xml')
        q = open(src, encoding='latin-1', newline='').read().replace('<path>stick.ac</path>', '<path>' + p[:-3] + 'ac</path>')
        th = sys.argv[4]
        q = q.replace('<factor>20</factor>', '<factor>%s</factor>' % th).replace('<dep>20</dep>', '<dep>%s</dep>' % th)
        q = q.replace('<dep>-20</dep>', '<dep>-%s</dep>' % th)
        open(os.path.join(os.path.dirname(sys.argv[2]), 'stick_t.xml'), 'w', encoding='latin-1', newline='').write(q)
        m.find('path').text = 'stick_t.xml'; x.text = '%.3f' % (float(x.text) + 0.03)
t.write(sys.argv[2]); print('interior_front: %d models left out' % n)
PY
cat > "$D/root.xml" <<XML
<?xml version="1.0"?>
<PropertyList>
  <model>
    <path>interior_front.xml</path>
    <offsets><x-m>$IX</x-m><y-m>-0.07</y-m><z-m>$IZ</z-m></offsets>
  </model>
</PropertyList>
XML
wrap() { printf '<?xml version="1.0"?>\n<PropertyList>\n  <path>%s</path>\n</PropertyList>\n' "$2" > "$D/$1"; }
off() { awk "BEGIN{printf \"%.4f,%.4f,%.4f\", $IX + ($1), -0.07 + ($2), $IZ + ($3)}"; }   # interior-gr4.xml's frame
ADD=()
HIDE='Fuselage_skin|cockpit\.001'
# the tub, on its own offset
python -I tools/cockpit/ac_pick.py "$R/Models/Interior/cockpit.ac" "$D/tub.ac" --name '^(Fuselage_skin|cockpit\.001)$' --prefix tub_
wrap tub.xml tub.ac
ADD+=(--add-model "$D/tub.xml@$IX,-0.07,$TZ")
if [ "$SEAT" != 0 ]; then   # the Mk 10, front seat of the Mirage's frame, for both seats (1.35 m apart: the GPL pit's pilots)
    python -I tools/cockpit/ac_pick.py "$M/interiorB.ac" "$D/seat.ac" --name '^seat\.seat-BIPLACE$' --prefix mk10_
    wrap seat.xml seat.ac
    SX=$(calc "$EX + 2.9 + ${TORNADO_GPL_SEAT_BACK:-0.14}")           # (the Mirage's eye: x -2.9, z 0.0093)
    SZ=$(calc "$EZ - 0.0093")
    ADD+=(--add-model "$D/seat.xml@$SX,0,$SZ" --add-model "$D/seat.xml@$(calc "$SX + 1.35"),0,$SZ")
    HIDE+='|seat-pan(\.001)?_[01]\.001|backrest\.00[23]'
fi
LEVER=()
if [ "$CONSOLES" != 0 ]; then
    # the A-10's panels on its deck: x_T = x_A + CX (the deck from 0.51 m ahead of the eye to 0.42 m behind it),
    # z_T = z_A + CZ (its panels' top 0.56 m under the eye, on the tub's deck). The A-10 file's lit lamps, canopy and
    # handle parts and duplicates of the GPL pit's own right-hand panels (ILS, TACAN) are left out
    python -I tools/cockpit/ac_pick.py "$A/cockpit.ac" "$D/consoles.ac" --box 2.46,3.40,0.4,1.0,-0.7,0.7 --prefix a10_ \
        --drop 'canopy|eject|-axis$|^ILS|^Tcn' --drop-tex '-on\.png$'
    python -I tools/cockpit/ac_pick.py "$A/detail-cpt.ac" "$D/deck.ac" --name '^consoles$' --prefix a10_
    wrap consoles.xml consoles.ac
    wrap deck.xml deck.ac
    CX=$(calc "$EX - 2.93")
    CZ=$(calc "$EZ - 1.30 + ${TORNADO_GPL_CONSOLE_UP:-0.03}")
    ADD+=(--add-model "$D/consoles.xml@$CX,0,$CZ" --add-model "$D/deck.xml@$CX,0,$CZ")
    ADD+=(--add-model "$A/A-10-throttle.xml@$(calc "2.6942 + $CX"),-0.3552,$(calc "0.7440 + $CZ"),0,22.74,0")
    # the right console's bare middle (the A-10 keeps blank plates there): a second TV-monitor and a blank plate of the
    # left console's, 0.71 m across (the GPL pit's own uhf-vhf.ac panel would cost 27,000 triangles for 12 cm)
    python -I tools/cockpit/ac_pick.py "$A/cockpit.ac" "$D/console2.ac" --name '^(TV_(Base|CDU_Brt|Test_Base|Test_Switch|Wpn_Brt|Wpn_Cont)|Blank_Base\.001)$' --prefix a10b_
    wrap console2.xml console2.ac
    ADD+=(--add-model "$D/console2.xml@$CX,0.71,$CZ")
    LEVER=(--lever)
fi
if [ "$GAUGES" != 0 ]; then
    python -I tools/cockpit/ac_pick.py "$A/detail-cpt.ac" "$D/compass_box.ac" --name '^CPT-ribbon-mag-compass$' --drop fixing --prefix a10_
    wrap compass_box.xml compass_box.ac
    # the compass (A-10 frame) on the coaming right of the up-front box: its face at eye frame X 0.30, Y -0.184, Z -0.55
    KX=$(calc "$EX - 0.55 - 2.28")
    KZ=$(calc "$EZ - 0.184 - 1.48")
    ADD+=(--add-model "$A/A-10-vsi-6kfpm.xml@$(off -4.564 -0.178 1.318),0,-5,0,0.72")
    ADD+=(--add-model "$A/A-10-chronometer.xml@$(off -4.57 0.43 1.313),0,-5,0")
    ADD+=(--add-model "$A/mag-compass.xml@$(calc "$KX + 2.28"),0.30,$(calc "$KZ + 1.48")")
    ADD+=(--add-model "$D/compass_box.xml@$KX,0.133,$KZ")
    ADD+=(--add-model "$R/Models/Interior/Instruments/common/VHF-22/ctl22-com1.xml@$(off -4.58 0.35 1.37),0,-5,0")
    ADD+=(--add-model "$R/Models/Interior/Instruments/rwr/rwr.xml@$(off -4.57 -0.095 1.431),0,-10,0")
fi
MIR=(--mirrors); [ "$MIRRORS" != 0 ] || MIR=()
if [ ${#MIR[@]} -gt 0 ] || [ ${#LEVER[@]} -gt 0 ]; then
    python -I tools/cockpit/tornado_gpl_parts.py "$D/parts.ac" "${MIR[@]}" "${LEVER[@]}"
    wrap parts.xml parts.ac
    ADD+=(--add-model "$D/parts.xml@$EX,0,$EZ")
fi
CK=()
if [ "${CPIT_X4:-1}" != 0 ]; then
    X4=("$R/Models/Interior/Instruments/common/divers/pedals.png" "$R/Models/Interior/Instruments/common/divers/stick.png")
    [ "$SEAT" != 0 ] && X4+=("$M/seat.png")
    [ "$CONSOLES" != 0 ] && X4+=("$A/panel-bkg.png")
    bash tools/cockpit/upscale_tex.sh build/tools/tornado_gpl_x4 "${X4[@]}"
    CK+=(--tex-dir build/tools/tornado_gpl_x4)
fi
EC=()
[ "${CPIT_EARCLIP:-1}" != 0 ] && EC=(--ear-clip)
python -I tools/cockpit/ac2cpit.py "$D/root.xml" "$EX" 0 "$EZ" "$OUT" "${EC[@]}" --aircraft-root "$R" --sort-tables \
    --controls --const-selects --fold-selects --skip 'Interior/pilot/' \
    --subdiv "${TORNADO_GPL_SUBDIV:-0.05}" --subdiv-extra 0.25 "${CK[@]}" --max-tex-for 'TISL|an-arc-164|arc-186' 1024 \
    --hide-obj "^($HIDE)\$" \
    --const 'instrumentation/MFD[1]/page-selected=enav' --const 'instrumentation/radar[0]/selected=2' \
    --const 'sim/current-view/internal=1' --const 'sim/current-view/name=Cockpit View' \
    --const 'consumables/fuel/total-fuel-norm=0.7' --const 'consumables/fuel/tank[0]/level-lbs=6000' \
    --prop 'engines/engine[0]/epr=engines/engine[0]/n1*0.026' --prop 'engines/engine[1]/epr=engines/engine[1]/n1*0.026' \
    --prop 'engines/engine/fuel-flow-gph=engines/engine[0]/fuel-flow-gph' \
    --prop 'sim/model/A-10/controls/engines/engine[0]/throttle=controls/engines/engine[0]/throttle' \
    --prop 'sim/model/A-10/controls/engines/engine[1]/throttle=controls/engines/engine[1]/throttle' \
    --mat-obj '^(rpm-Needle[12]|Airspeedneedle)$=paint:0.85,0.85,0.85' \
    --mat-box '^panel\.002$=paint@0.2:0.17,0.18,0.19@-1,-2,-3,1,1,0' \
    --mat-box '^Cube\.025$=paint@0.2:0.15,0.155,0.16@-1,-2,-3,1,1,0' \
    --mat-box '^Cube\.02[01]$=paint@0.2:0.20,0.21,0.22@-1,-2,-3,1,1,0' \
    --glass-obj '^CPT-hud-screen$=0.06' \
    --mat-box '^CPT-hud-lens-frame$=paint@0.1:0.10,0.105,0.11@-1,-1,-2,1,1,1' \
    --mat-box '^tub_Fuselage_skin$=rubber@0.1:0.10,0.105,0.11@-1,-1.3,-1,1,-0.72,1@0,1,0,0.7' \
    --mat-box '^tub_(Fuselage_skin|cockpit\.001)$=riveted@0.22:0.34,0.365,0.38@-3,-3,-3,3,3,5' \
    --mat-obj '^panel\.003$=paint@0.25' \
    "${ADD[@]}" \
    --extra "$R/Models/TornadoGR4.ac" \
    '^(Fuselage|Fuselage2|Canopy|CanopyGlass|Windscreen|WindscreenGlass|Radom|sweepwing[.][LR]|slat(left|right)|spoiler(left|right)|flaps[.][LR]|fin[.]001|rudder|airintakes)$'
[ "${TORNADO_GPL_BAKE:-1}" != 0 ] || exit 0
if [ ! -x build/tools/cpit_bake.exe ] || [ tools/cockpit/cpit_bake.c -nt build/tools/cpit_bake.exe ]; then
    PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" gcc -O2 -march=native -o build/tools/cpit_bake.exe tools/cockpit/cpit_bake.c -lm -lpthread
fi
build/tools/cpit_bake.exe "$OUT" "$OUT" 192 1536
