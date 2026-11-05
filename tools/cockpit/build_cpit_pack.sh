#!/usr/bin/env bash
# build_cpit_pack.sh [SRC_DIR] -- builds tools/cockpit/cpit_pack.cpp into build/tools/cpit_pack.exe, with the BC7 encoder
# from source. SRC_DIR holds a checkout of https://github.com/richgel999/bc7enc_rdo (bc7enc.cpp, bc7decomp.cpp, rgbcx.cpp;
# MIT or public domain, its LICENSE file; never installed, nothing of it runs but these three files); default BC7ENC_DIR or
# ../dl/bc7enc_rdo/src/bc7enc_rdo-master. Fetch it yourself, e.g. the master zip into ../dl/bc7enc_rdo/ (night_assets
# 2026-11-04). MSYS2 UCRT64 g++; run it under a private script when agents are testing.
set -e
cd "$(dirname "$0")/../.."
export PATH="${ACZ_UCRT64_BIN:-/c/msys64/ucrt64/bin}:/c/msys64/usr/bin:$PATH"
S=${1:-${BC7ENC_DIR:-../dl/bc7enc_rdo/src/bc7enc_rdo-master}}
[ -f "$S/bc7enc.cpp" ] || { echo "no bc7enc.cpp in $S: fetch https://github.com/richgel999/bc7enc_rdo first"; exit 2; }
mkdir -p build/tools/cpit_pack_obj
for f in bc7enc bc7decomp rgbcx; do
    [ build/tools/cpit_pack_obj/$f.o -nt "$S/$f.cpp" ] || g++ -O3 -DNDEBUG -msse4.1 -w -c "$S/$f.cpp" -o build/tools/cpit_pack_obj/$f.o -I"$S"
done
# rgbcx.cpp holds the implementation already (it defines RGBCX_IMPLEMENTATION itself)
g++ -O2 -std=c++17 -Wall -Wno-unused-function -c tools/cockpit/cpit_pack.cpp -o build/tools/cpit_pack_obj/cpit_pack.o -I"$S" -Iruntime/include
g++ -O2 -o build/tools/cpit_pack.exe build/tools/cpit_pack_obj/cpit_pack.o build/tools/cpit_pack_obj/bc7enc.o \
    build/tools/cpit_pack_obj/bc7decomp.o build/tools/cpit_pack_obj/rgbcx.o -static -lpthread
echo "built build/tools/cpit_pack.exe"
