#!/bin/bash
# prep_tornado_gpl.sh CLONE [DEST] -- the input folder of build_tornado_gpl.sh out of a clone of NikolaiVChr/Tornado
# (github.com/NikolaiVChr/Tornado, GPL-2.0; commit 0dd1e5a of 2020-01-24 used on 2026-11-04). Copies what the cockpit
# uses with its licence: Models/Interior/**, Models/TornadoGR4.ac, LICENSE, ReadMe.3dmodel.txt, gives the JPEG livery
# a PNG next to it (ac2cpit reads PNG / SGI / DDS; needs Python with Pillow: PY=<python>, default
# `python`) and writes SOURCES.txt: what was changed in the copy (the GPL's notice of changes):
#   - Models/GR4.png: Models/GR4.jpg converted (2048 x 1024, no resampling);
#   - Instruments/common/rpm/rpm.ac: material "face" rgb 0 0 0 + emission 0.3 -> rgb 1 1 1: FlightGear lights these
#     faces by their emission, our cockpit by its materials (black discs otherwise).
# Reads only the clone; no code from it is run.
set -e
S=${1:?usage: prep_tornado_gpl.sh CLONE [DEST]}
D=${2:-../cockpit_src/Tornado_GPL}
PY=${PY:-python}
mkdir -p "$D/Models"
cp -r "$S/Models/Interior" "$D/Models/"
cp "$S/Models/TornadoGR4.ac" "$S/Models/GR4.jpg" "$D/Models/"
cp "$S/LICENSE" "$S/ReadMe.3dmodel.txt" "$D/"
"$PY" -I -c "import sys; from PIL import Image; Image.open(sys.argv[1]).convert('RGB').save(sys.argv[2])" "$D/Models/GR4.jpg" "$D/Models/GR4.png"
sed -i -E 's/^(MATERIAL "face") rgb 0 0 0 +amb 0 0 0 +emis 0\.3 0\.3 0\.3/\1 rgb 1 1 1 amb 1 1 1 emis 0 0 0/' "$D/Models/Interior/Instruments/common/rpm/rpm.ac"
{
    echo "Input of tools/cockpit/build_tornado_gpl.sh: a copy of NikolaiVChr/Tornado (GPL-2.0, LICENSE; ReadMe.3dmodel.txt:"
    echo "\"This 3d-model is released under GNU GPL. Heiko Schulz\"), $(git -C "$S" log -1 --format='%H %ad' --date=short 2>/dev/null)."
    echo "Changed in this copy: Models/GR4.png = Models/GR4.jpg as PNG; Instruments/common/rpm/rpm.ac material \"face\": rgb 1 1 1,"
    echo "no emission (see tools/cockpit/prep_tornado_gpl.sh). Nothing else."
} > "$D/SOURCES.txt"
echo "prep_tornado_gpl: $D ready"
