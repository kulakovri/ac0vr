#!/bin/bash
# make_release.sh VERSION -- on the developer's machine, from a tree's root: out/release/VERSION/ with everything a player needs but the game
# (setup_1105 package B, 2026-11-05). stage/ is the install image: ac0vr.exe, version.txt, the bundle (the pruned compiler, the runtime library
# built from this tree, the SDK, the shaders, the DLLs, the recompiler and its config, the texture glyphs, the joystick profiles), Python
# embeddable, tools/setup/, the documents and licences. Then the installer (make_installer.sh, package C) and the update zip (make_update_zip.sh,
# package D) when those scripts are here, SHA256SUMS and a RELEASE_NOTES.md skeleton. It refuses to finish if anything game-derived is in the
# folder (release_scan.py: names, machine paths and, with the ELF, the game's bytes).
#
# Needs, in the environment (nothing of this machine is written here):
#   ACZ_LLVM_MINGW    the unpacked llvm-mingw folder
#   ACZ_TC_CMAKE_BIN  a folder with cmake.exe and ninja.exe (w64devkit's bin)
#   ACZ_PY_EMBED_ZIP  python-3.12.x-embed-amd64.zip
#   ACZ_MSYS2         the MSYS2 ucrt64 folder (make_player_sdk.sh: headers, import libraries, DLLs, licences)
#   ACZ_SDL3_DLL      the SDL3.dll to ship, if not MSYS2's
#   ACZ_RELEASE_ELF   the game's executable from the developer's own disc (default game/SLUS_213.46): the rehearsal build that prunes the
#                     compiler and measures the stages, and the scan; none of its output stays
#   ACZ_RELEASE_GIT   the commit for version.txt (default: git rev-parse --short HEAD when this tree is a repository)
#   ACZ_SPIRV_DIR     the compiled shaders (default build/zero/shaders of this tree)
# Takes about 6 minutes at 8 threads: the runtime is compiled twice over (the full build and the rehearsals). Run it under the machine's job
# wrapper when agents are testing.
set -e
version=$1
[ -n "$version" ] || { echo "usage: make_release.sh VERSION"; exit 2; }
case $version in v*) version=${version#v} ;; esac
[ -f CMakeLists.txt ] && [ -d tools/setup ] || { echo "run from the tree's root"; exit 2; }
: "${ACZ_LLVM_MINGW:?the unpacked llvm-mingw folder}" "${ACZ_TC_CMAKE_BIN:?a folder with cmake.exe and ninja.exe}" "${ACZ_PY_EMBED_ZIP:?python-3.12.x-embed-amd64.zip}" "${ACZ_MSYS2:?the MSYS2 ucrt64 folder}"
elf=${ACZ_RELEASE_ELF:-game/SLUS_213.46}
[ -f "$elf" ] || { echo "no $elf: the rehearsal build needs the game's executable from your own disc (ACZ_RELEASE_ELF)"; exit 2; }
spv=${ACZ_SPIRV_DIR:-build/zero/shaders}
[ -f "$spv/gs.vert.spv" ] || { echo "no shaders in $spv (ACZ_SPIRV_DIR)"; exit 2; }
rev=${ACZ_RELEASE_GIT:-$(git rev-parse --show-toplevel >/dev/null 2>&1 && [ "$(cd "$(git rev-parse --show-toplevel)" && pwd)" = "$PWD" ] && git rev-parse --short HEAD || true)}
line=$version; [ -n "$rev" ] && line="$version ($rev)"
out=out/release/$version
work=out/release/.work-$version
rm -rf "$out" "$work"
mkdir -p "$out/stage" "$work"
stage=$out/stage
t_start=$(date +%s)
export ACZ_TC_CMAKE_BIN

echo "== 1. the player's SDK"
"$PWD/tools/setup/make_player_sdk.sh" "$work/sdk" "$spv" > "$work/sdk.log"

echo "== 2. the runtime, built with the player's compiler: version $line"
export ACZ_TC_CMAKE_ARGS="-DACZ_VERSION=$version -DACZ_VERSION_GIT=$rev -DPS2_GEN_OPT=-O2 -DPS2_STRIP_PATHS=ON"
tools/setup/toolchain_build.sh llvm-mingw "$ACZ_LLVM_MINGW" "$work/build" "$work/sdk" 8 > "$work/build.log" 2>&1 || { tail -20 "$work/build.log"; exit 1; }
tail -1 "$work/build.log"

echo "== 3. the bundle, the install image"
tools/setup/make_bundle.sh llvm-mingw "$ACZ_LLVM_MINGW" "$work/build" "$work/sdk" "$stage/bundle" > "$work/bundle.log"
mkdir -p "$stage/python" "$stage/tools/setup" "$stage/docs" "$stage/licenses"
unzip -q "$ACZ_PY_EMBED_ZIP" -d "$stage/python"
cp tools/setup/player_build.py tools/setup/vu_images_from_elf.py "$stage/tools/setup/"
cp "$work/build/ac0vr.exe" "$stage/"
printf '%s\n' "$line" > "$stage/version.txt"
PY=$PWD/$stage/python/python.exe

echo "== 4. a rehearsal with the full compiler: what a build really uses"
ACZ_LINK_VERBOSE=1 "$PY" -I -B tools/setup/player_build.py --bundle "$stage/bundle" --elf "$elf" --out "$work/reh1" --release "$line" > "$work/reh1.log" 2>&1 || { tail -30 "$work/reh1.log"; exit 1; }
"$PY" -I -B tools/setup/prune_bundle.py "$stage/bundle" "$work/reh1" "$work/reh1.log"

echo "== 5. the same from the pruned bundle, timed: the stages' times go into bundle.txt for the progress bar"
"$PY" -I -B tools/setup/player_build.py --bundle "$stage/bundle" --elf "$elf" --out "$work/reh2" --release "$line" > "$work/reh2.log" 2>&1 || { tail -30 "$work/reh2.log"; exit 1; }
grep '^@done' "$work/reh2.log" | while read -r _ name secs; do printf 'stage_%s=%s\n' "$name" "$secs"; done >> "$stage/bundle/bundle.txt"
grep '^@done\|^@total\|^done:' "$work/reh2.log"

echo "== 6. the documents and licences"
# as the public tree has them: scrubbed, with the switch reference made from what is exported. The private tree makes that export here (and a
# hit of its scan stops the release); a clone of the public tree has them at its root.
docs=.
if [ -x tools/release/make_public.sh ]; then
    docs=$(mktemp -d)/public
    tools/release/make_public.sh "$docs" > "$work/public.log" 2>&1 || { grep -v '^stale' "$work/public.log" | tail -12; echo "the public scan has hits: no release"; exit 1; }
fi
for f in LICENSE NOTICE THIRD_PARTY_NOTICES.md README.md; do cp "$docs/$f" "$stage/"; done
for f in SETTINGS.md HOTAS.md ENV_REFERENCE.md; do cp "$docs/docs/$f" "$stage/docs/"; done
if [ "$docs" != . ]; then rm -rf "$(dirname "$docs")"; fi
for p in SDL3 openxr-sdk vulkan-loader vulkan-headers libgcc libstdc++ libwinpthread ninja; do
    d=$ACZ_MSYS2/share/licenses/$p
    if [ -d "$d" ]; then cp -r "$d" "$stage/licenses/$p"; fi
done
cp "$stage/bundle/toolchain/LICENSE.TXT" "$stage/licenses/llvm-mingw.txt"
if [ -f "$stage/python/LICENSE.txt" ]; then cp "$stage/python/LICENSE.txt" "$stage/licenses/python.txt"; fi

echo "== 7. the scan"
"$PY" -I -B tools/setup/release_scan.py "$out" --elf "$elf"

# the installer and the update zip, when their packages are in this tree
if [ -x tools/setup/make_update_zip.sh ]; then echo "== 8. the update zip"; tools/setup/make_update_zip.sh "$version" "$out"; fi
if [ -x tools/setup/make_installer.sh ]; then echo "== 9. the installer"; tools/setup/make_installer.sh "$version" "$out"; fi

cat > "$out/RELEASE_NOTES.md" <<EOT
# ac0vr $version

(What changed for a player, in their words. The first lines show in the launcher's update offer.)

## Install
Run ac0vr-$version-setup.exe with your own US disc image beside it, or point it at the image. Nothing from the game is in this release.

## Files
$(cd "$out" && ls -1 -- *.exe *.zip 2>/dev/null | sed 's/^/- /')
EOT
( cd "$out" && for f in *.exe *.zip; do [ -f "$f" ] && sha256sum -b "$f"; done ) > "$out/SHA256SUMS" || true
rm -rf "$work"
echo "== done in $(( $(date +%s) - t_start )) s: $out, stage $(du -sm "$stage" | cut -f1) MB"
ls -la "$out"
