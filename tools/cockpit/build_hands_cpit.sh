#!/bin/bash
# build_hands_cpit.sh [OUT] -- the gloves on their own: cockpits/hands.cpit (touch_ux 2026-10-29, bug #238), the gloved
# hands of tools/cockpit/build_hands.sh (../cockpit_src/hands/hands.ac: the CC0 Godot XR Tools hands) with their free
# poses only (open, pointing, fist per hand; no control to hold), ~3 MB. The runtime adds them to every cockpit model
# that has no gloves of its own (ps2_video.c ACZ_VR_HANDS_FALLBACK, ACZ_VR_HANDS_FILE): with the Quest Touch
# controllers the gloves then follow the controllers in any cockpit, a fist while a grip is squeezed.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:-cockpits/hands.cpit}
B=build/cockpit/hands
mkdir -p cockpits "$B"
[ -s ../cockpit_src/hands/hands.ac ] || bash tools/cockpit/build_hands.sh
printf '<?xml version="1.0"?>\n<!-- written by tools/cockpit/build_hands_cpit.sh: no model, only the gloves -->\n<PropertyList>\n</PropertyList>\n' \
    > "$B/hands_root.xml"
python -I tools/cockpit/ac2cpit.py "$B/hands_root.xml" 0 0 0 "$OUT" --hands ../cockpit_src/hands/hands.ac
