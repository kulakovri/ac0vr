#!/bin/bash
# toolchain_build.sh KIND ROOT BUILD_DIR SDK_DIR [JOBS] -- configure and build the game with a portable compiler and nothing
# else on PATH (no MSYS2, no Git tools): KIND = w64devkit | llvm-mingw | zig, ROOT = its unpacked folder, BUILD_DIR = the build
# folder, SDK_DIR = from make_player_sdk.sh. CMake and Ninja: the folder $ACZ_TC_CMAKE_BIN (default ROOT/bin; w64devkit has
# both, the other two have neither). Prints the wall times; the DLLs and the shaders land next to the exe.
kind=$1; root=$2; bdir=$3; sdk=$4; jobs=${5:-8}
[ -n "$sdk" ] || { echo "usage: toolchain_build.sh KIND ROOT BUILD_DIR SDK_DIR [JOBS]"; exit 2; }
here=$(cd "$(dirname "$0")" && pwd); src=$(cd "$here/../.." && pwd)
cb=${ACZ_TC_CMAKE_BIN:-$root/bin}
case $kind in
    zig) tcbin=$root ;;
    *)   tcbin=$root/bin ;;
esac
win=$(cygpath -u "$SYSTEMROOT")
P="$tcbin:$cb:$win/System32:$win"
mkdir -p "$bdir"
bdir_m=$(cygpath -m "$(cd "$bdir" && pwd)")
t0=$(date +%s)
env PATH="$P" ACZ_TC_ROOT="$(cygpath -m "$root")" ACZ_TC_WRAP="$bdir_m/zigwrap" "$cb/cmake.exe" -S "$(cygpath -m "$src")" -B "$bdir_m" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$(cygpath -m "$cb/ninja.exe")" \
    -DCMAKE_BUILD_TYPE=Release -DPS2_GENERATED_DIR=gen_zero -DPS2_BUILD_GSREPLAY=OFF \
    -DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$here/toolchains/$kind.cmake")" \
    -DPS2_SDK_DIR="$(cygpath -m "$sdk")" -DPS2_PREBUILT_SPIRV="$(cygpath -m "$sdk/spirv")" ${ACZ_TC_CMAKE_ARGS:-} || exit 1
t1=$(date +%s)
env PATH="$P" ACZ_TC_ROOT="$(cygpath -m "$root")" ACZ_TC_WRAP="$bdir_m/zigwrap" "$cb/cmake.exe" --build "$bdir_m" -- -j "$jobs" -k 0
rc=$?
t2=$(date +%s)
cp "$sdk"/dll/*.dll "$bdir/" 2>/dev/null
echo "toolchain_build: $kind rc=$rc configure $((t1-t0)) s, build $((t2-t1)) s"
exit $rc
