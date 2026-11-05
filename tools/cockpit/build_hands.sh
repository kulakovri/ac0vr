#!/bin/bash
# build_hands.sh -- the gloved pilot hands for the native VR cockpit (ACZ_VR_HANDS; ac2cpit.py --hands):
#   1. fetches (once) the Godot XR Tools hand models, poses and glove albedo (CC0 1.0, DigitalN8m4r3 / Miodrag Sejic,
#      made with MakeHuman CE; github.com/GodotVR/godot-xr-tools, commit pinned below) into ../cockpit_src/godot-xr-hands
#   2. tools/cockpit/hands_bake.py: four rigid poses per hand (grip, open, point, fist) in the OpenXR grip frame
#      -> ../cockpit_src/hands/hands.ac + hands_glove.png (+ preview.png)
# The cockpit build scripts that have --hands read ../cockpit_src/hands/hands.ac. Only data is downloaded (glTF JSON,
# Godot binary Animation resources, a PNG); nothing from it runs.
set -e
cd "$(dirname "$0")/../.."
SRC=../cockpit_src/godot-xr-hands
OUT=../cockpit_src/hands
REV=6c6189bdfcdf855052ef1aeb9f3e0ec4c7a7eb60
B=https://raw.githubusercontent.com/GodotVR/godot-xr-tools/$REV/addons/godot-xr-tools/hands
mkdir -p "$SRC/model" "$SRC/anim/left" "$SRC/anim/right" "$SRC/tex" "$OUT"
get() { [ -s "$SRC/$2" ] || curl -fsS -m 60 -o "$SRC/$2" "$B/$1"; }
get License.md License.md
for h in l r; do get "model/hand_$h.gltf" "model/hand_$h.gltf"; done
for p in Grip "Grip Shaft" "Default pose" Sign_Point; do
    for s in left right; do get "animations/$s/${p// /%20}.res" "anim/$s/$p.res"; done
done
get textures/cleaning_glove_baseColor.png tex/cleaning_glove_baseColor.png
python -I tools/cockpit/hands_bake.py "$SRC" "$OUT/hands.ac" --preview "$OUT/preview.png" "$@"
