#!/bin/bash
# make_player_sdk.sh OUTDIR [SPIRV_DIR] -- the folder the player's build compiles and links against (PS2_SDK_DIR,
# PS2_PREBUILT_SPIRV): headers and import libraries of SDL3, the OpenXR loader and vulkan-1 taken from MSYS2 UCRT64 on the
# release machine, the DLLs we ship, and the compiled shaders. It holds nothing of the game. Run on the developer's
# machine only; the player gets the result inside the bundle and needs no MSYS2. ACZ_SDL3_DLL: the SDL3.dll to ship, if not MSYS2's
# (build/zero/SDL3.dll has been libsdl.org's since 2026-10-18: 2.85 MB, no CRT dependency).
set -e
out=$1; spv=${2:-build/zero/shaders}
[ -n "$out" ] || { echo "usage: make_player_sdk.sh OUTDIR [SPIRV_DIR]"; exit 2; }
M=${ACZ_MSYS2:?the MSYS2 ucrt64 folder of the release machine}
mkdir -p "$out/include" "$out/lib" "$out/dll" "$out/spirv"
for d in SDL3 openxr vulkan vk_video; do cp -r "$M/include/$d" "$out/include/"; done
cp "$M/lib/libSDL3.dll.a" "$M/lib/libopenxr_loader.dll.a" "$M/lib/libvulkan-1.dll.a" "$out/lib/"
# zig cc looks for X.lib or libX.a, not libX.dll.a
cp "$M/lib/libSDL3.dll.a" "$out/lib/SDL3.lib"; cp "$M/lib/libopenxr_loader.dll.a" "$out/lib/openxr_loader.lib"; cp "$M/lib/libvulkan-1.dll.a" "$out/lib/vulkan-1.lib"
cp "${ACZ_SDL3_DLL:-$M/bin/SDL3.dll}" "$M/bin/libopenxr_loader.dll" "$M/bin/libgcc_s_seh-1.dll" "$M/bin/libstdc++-6.dll" "$M/bin/libwinpthread-1.dll" "$out/dll/"
cp "$spv"/*.spv "$out/spirv/"
du -sh "$out"/*
