#!/usr/bin/env bash
# cpit_parity.sh OLD.cpit NEW.cpit NAME OUTDIR [cpit_view options] -- the offline parity check of a packed cockpit file
# (night_assets 2026-11-04, RELEASE_RESEARCH_2026-11-03.md R1): the audit views of both files by tools/cockpit/cpit_view.c
# (the same software renderer; format 3 and format 4), per view the pixel differences (tools/cockpit/cpit_diff.py) in
# OUTDIR/NAME.tsv, the worst view's image pair and x8 diff as PNG in OUTDIR/png/. The PPMs are removed. Default size 1024 x 1024
# at 100 deg: about 10 px / deg (the audit's). PYV: the numpy + Pillow python (a venv of your own).
set -e
old=$1; new=$2; name=$3; out=$4; shift 4
d=$(dirname "$0")
V=$d/../../build/tools/cpit_view.exe
P=${PYV:-python}
t=$out/tmp_$name
mkdir -p "$t/old" "$t/new" "$out/png"
OPTS=(-w 1024 -h 1024 "$@")
"$V" "$old" "$t/old" "${OPTS[@]}" > /dev/null
"$V" "$new" "$t/new" "${OPTS[@]}" > /dev/null
rm -f "$t"/old/*_d.ppm "$t"/new/*_d.ppm
"$P" -I "$d/cpit_diff.py" "$t/old" "$t/new" --png "$out/png" --name "$name" > "$out/$name.tsv"
rm -rf "$t"
tail -1 "$out/$name.tsv" | sed "s/^/$name\t/"
