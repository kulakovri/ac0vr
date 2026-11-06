#!/bin/bash
# make_bundle.sh KIND ROOT BUILD_DIR SDK_DIR OUT -- on the developer's machine: the folder a player needs to build the game from
# their own disc with tools/setup/player_build.py. KIND = w64devkit | llvm-mingw | zig, ROOT = the unpacked compiler, BUILD_DIR = a
# full toolchain_build.sh build of this tree with the same compiler (its objects become the prebuilt runtime library), SDK_DIR =
# make_player_sdk.sh's folder. Holds no game data, gen_zero/ or .cpit. Python itself is not copied (the embeddable package, 11 MB).
# Run from the tree's root. Prints the sizes.
set -e
kind=$1; root=$2; bdir=$3; sdk=$4; out=$5
[ -n "$out" ] || { echo "usage: make_bundle.sh KIND ROOT BUILD_DIR SDK_DIR OUT"; exit 2; }
[ ! -e "$out" ] || { echo "$out exists"; exit 1; }
mkdir -p "$out/toolchain" "$out/runtime/include" "$out/runtime/src" "$out/runtime/lib" "$out/recomp/config_zero/sdk" "$out/dll" "$out/spirv"
root=$(cd "$root" && pwd); bdir=$(cd "$bdir" && pwd); sdk=$(cd "$sdk" && pwd); out=$(cd "$out" && pwd)

# ---- the compiler, cut down to compiling C and linking ----
case $kind in
w64devkit)
    G=lib/gcc/x86_64-w64-mingw32; V=$(basename "$root/$G"/*)
    mkdir -p "$out/toolchain/bin" "$out/toolchain/libexec/gcc/x86_64-w64-mingw32/$V" "$out/toolchain/$G/$V" "$out/toolchain/lib"
    for f in gcc g++ as ld ninja; do cp "$root/bin/$f.exe" "$out/toolchain/bin/"; done
    cp "$root/libexec/gcc/x86_64-w64-mingw32/$V"/{cc1,collect2}.exe "$out/toolchain/libexec/gcc/x86_64-w64-mingw32/$V/"
    cp -r "$root/$G/$V"/{include,include-fixed} "$out/toolchain/$G/$V/" 2>/dev/null || true
    cp "$root/$G/$V"/{crtbegin.o,crtend.o,libgcc.a,libgcc_eh.a,libstdc++.a} "$out/toolchain/$G/$V/" 2>/dev/null || true
    cp -r "$root/include" "$out/toolchain/"
    ( cd "$root/lib" && cp -- *.o *.a "$out/toolchain/lib/" )
    cp "$root/COPYING.MinGW-w64-runtime.txt" "$root/VERSION.txt" "$out/toolchain/" 2>/dev/null || true
    cat > "$out/bundle.txt" <<EOT
kind=w64devkit
bindir=bin
cc=gcc.exe
cxx=g++.exe
ninja=ninja.exe
EOT
    ;;
llvm-mingw)
    T=x86_64-w64-mingw32; V=$(basename "$root/lib/clang"/*)
    mkdir -p "$out/toolchain/bin" "$out/toolchain/lib/clang/$V/lib/windows" "$out/toolchain/$T/bin"
    for f in clang clang++ clang-"$V" lld ld.lld "$T"-clang "$T"-clang++ "$T"-ld; do [ -e "$root/bin/$f.exe" ] && cp "$root/bin/$f.exe" "$out/toolchain/bin/"; done
    for f in "$root"/bin/lib{LLVM,clang-cpp,c++,unwind,winpthread,zstd,z}*.dll "$root"/bin/zlib*.dll; do [ -e "$f" ] && cp "$f" "$out/toolchain/bin/"; done
    cp "$root"/bin/x86_64-*.cfg "$root"/bin/mingw32-common.cfg "$out/toolchain/bin/"
    cp -r "$root/lib/clang/$V/include" "$out/toolchain/lib/clang/$V/"
    cp "$root/lib/clang/$V/lib/windows/"*x86_64* "$out/toolchain/lib/clang/$V/lib/windows/"
    cp -r "$root/$T/lib" "$out/toolchain/$T/"
    cp -r "$root/include" "$out/toolchain/"
    cp "$root/LICENSE.TXT" "$out/toolchain/"
    cp "${ACZ_TC_CMAKE_BIN:?a folder with ninja.exe}/ninja.exe" "$out/toolchain/bin/"
    cat > "$out/bundle.txt" <<EOT
kind=llvm-mingw
toolchain_id=$(basename "$root")
bindir=bin
cc=$T-clang.exe
cxx=$T-clang++.exe
ninja=ninja.exe
cflags=-D_WIN32_WINNT=0x0A00
ldflags=-Wl,--stack,2097152
gen_opt=-O2
EOT
    ;;
zig)
    mkdir -p "$out/toolchain/lib/libc/include"
    cp "$root/zig.exe" "$root/LICENSE" "$out/toolchain/"
    for d in compiler compiler_rt compiler_rt.zig include libcxx libcxxabi libunwind std c c.zig ubsan_rt.zig; do
        [ -e "$root/lib/$d" ] && cp -r "$root/lib/$d" "$out/toolchain/lib/"
    done
    cp -r "$root/lib/libc/mingw" "$out/toolchain/lib/libc/"
    for d in any-windows-any x86_64-windows-any x86-windows-any generic-mingw; do
        [ -e "$root/lib/libc/include/$d" ] && cp -r "$root/lib/libc/include/$d" "$out/toolchain/lib/libc/include/"
    done
    cp "${ACZ_TC_CMAKE_BIN:?a folder with ninja.exe}/ninja.exe" "$out/toolchain/"
    cat > "$out/toolchain/zig-cc.cmd" <<'EOT'
@set ZIG_LOCAL_CACHE_DIR=%~dp0cache
@set ZIG_GLOBAL_CACHE_DIR=%~dp0gcache
@"%~dp0zig.exe" cc -target x86_64-windows-gnu %*
EOT
    cat > "$out/toolchain/zig-c++.cmd" <<'EOT'
@set ZIG_LOCAL_CACHE_DIR=%~dp0cache
@set ZIG_GLOBAL_CACHE_DIR=%~dp0gcache
@"%~dp0zig.exe" c++ -target x86_64-windows-gnu %*
EOT
    cat > "$out/bundle.txt" <<EOT
kind=zig
bindir=.
cc=zig-cc.cmd
cxx=zig-c++.cmd
ninja=ninja.exe
ldflags=-Wl,--stack,2097152
EOT
    ;;
*) echo "unknown kind $kind"; exit 2 ;;
esac

# ---- the prebuilt runtime: every object except the generated code and ps2_vu.c, which includes it ----
objs=$out/objs.txt
( cd "$bdir/CMakeFiles/ac5.dir" && find . -type f | grep '[.]obj$' | grep -v '^[.]/gen_zero/' | grep -v 'ps2_vu[.]c[.]obj$' ) > "$objs"
case $kind in
    w64devkit)  AR=$root/bin/ar.exe ;;
    llvm-mingw) AR=$root/bin/llvm-ar.exe ;;
    zig)        AR="$root/zig.exe ar" ;;
esac
( cd "$bdir/CMakeFiles/ac5.dir" && ZIG_LOCAL_CACHE_DIR=$(cygpath -m /tmp)/zc ZIG_GLOBAL_CACHE_DIR=$(cygpath -m /tmp)/zc PATH="$(dirname "${AR%% *}"):$PATH" $AR rcs "$(cygpath -m "$out/runtime/lib/libac5rt.a")" $(cat "$objs") )
rm -f "$objs"
cp "$bdir/liblua54.a" "$sdk"/lib/*.a "$out/runtime/lib/"
if [ "$kind" = zig ]; then cp "$sdk"/lib/*.lib "$out/runtime/lib/"; fi
cp runtime/src/ps2_vu.c "$out/runtime/src/"
cp runtime/include/*.h "$out/runtime/include/"
[ -d runtime/include/rn ] && cp -r runtime/include/rn "$out/runtime/include/"

# ---- the recompiler and its configuration (addresses and names, nothing of the game) ----
mkdir -p "$out/recomp/ps2recomp" "$out/recomp/vurecomp"
cp tools/ps2recomp/*.py "$out/recomp/ps2recomp/"
cp tools/vurecomp/*.py "$out/recomp/vurecomp/"
cp tools/setup/vu_images_from_elf.py "$out/recomp/"
cp config_zero/ida_like_zero.json config_zero/seeds_zero.json "$out/recomp/config_zero/"
cp config_zero/sdk/sdk_symbols.json config_zero/sdk/manual_symbols.json config_zero/sdk/overrides_draft.json "$out/recomp/config_zero/sdk/"
cp "${ACZ_VU_RECIPE:-config_zero/vu_recipe.json}" "$out/recomp/vu_recipe.json"

# what the game reads beside the exe: only what the public tree may carry (tools/release/public_manifest.txt: the glyphs of our own, not the
# textures made from the disc's pictures); data/ lands next to the exe
mkdir -p "$out/data/config_zero" "$out/data/textures" "$out/data/profiles"
for d in xbox xbox_modern; do cp -r "textures/$d" "$out/data/textures/"; done
rm -f "$out"/data/textures/*/3F861D67_*
cp config_zero/cloud_art.json "$out/data/config_zero/"
cp -r profiles/joystick "$out/data/profiles/"
cp "$sdk"/dll/*.dll "$out/dll/"
cp "$sdk"/spirv/*.spv "$out/spirv/"
du -sh "$out"/* "$out/toolchain"/*
