#!/bin/bash
# build_xbrz.sh: download xBRZ 1.9 (Zenju, GPL-3.0) from its SourceForge project into its own folder and build the
# small xbrz_cli used by upscale_hud.py. Downloaded code stays in ext_tools/xbrz_src_dl (read it before building);
# the executable goes to ext_tools/xbrz_build. MSYS2 UCRT64 g++.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
# ext_tools: the nearest parent of this tree that has one, else next to the repository
d=$HERE
while [ ! -d "$d/ext_tools" ] && [ "$d" != "$(dirname "$d")" ]; do d=$(dirname "$d"); done
if [ -d "$d/ext_tools" ]; then EXT=$d/ext_tools; else EXT=$(cd "$HERE/../../.." && pwd)/ext_tools; fi
DL=$EXT/xbrz_src_dl
OUT=$EXT/xbrz_build
mkdir -p "$DL" "$OUT"
if [ ! -f "$DL/src/xbrz.cpp" ]; then
    curl -sSL -m 120 -o "$DL/xBRZ_1.9.zip" "https://sourceforge.net/projects/xbrz/files/xBRZ/xBRZ_1.9.zip/download"
    python -I -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$DL/xBRZ_1.9.zip" "$DL/src"
fi
PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:$PATH" g++ -O2 -std=c++20 -DNDEBUG -I"$DL/src" "$HERE/xbrz_cli.cpp" "$DL/src/xbrz.cpp" \
    -static -o "$OUT/xbrz_cli.exe"
echo "$OUT/xbrz_cli.exe"
