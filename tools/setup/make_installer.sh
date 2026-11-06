#!/bin/bash
# make_installer.sh VERSION OUT -- called by make_release.sh: OUT/ac0vr-VERSION-setup.exe from OUT/stage and tools/setup/ac0vr.iss, when Inno Setup 6 is
# there (ACZ_ISCC, else ISCC.exe on PATH or in its two default folders). Without it: says so and goes on; the other files of the release are still made.
# ACZ_COCKPIT_URL and ACZ_COCKPIT_SHA256: the public cockpit set's release asset: only then the wizard shows that component.
set -e
version=$1; out=$2
[ -n "$out" ] || { echo "usage: make_installer.sh VERSION OUT"; exit 2; }
iscc=${ACZ_ISCC:-}
if [ -z "$iscc" ]; then
    for c in "$(command -v ISCC.exe || true)" "$PROGRAMFILES/Inno Setup 6/ISCC.exe" "${PROGRAMFILES:-x}(x86)/Inno Setup 6/ISCC.exe" "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe"; do
        if [ -n "$c" ] && [ -f "$c" ]; then iscc=$c; break; fi
    done
fi
if [ -z "$iscc" ]; then
    echo "make_installer: Inno Setup 6 is not installed: no setup exe. The stage is ready; install Inno Setup and run: tools/setup/make_installer.sh $version $out"
    exit 0
fi
# (absolute: ISCC reads a relative path from the script's own folder)
args=("/DVERSION=$version" "/DSTAGE=$(cygpath -wa "$out/stage")" "/O$(cygpath -wa "$out")")
if [ -n "${ACZ_COCKPIT_URL:-}" ]; then args+=("/DCOCKPIT_URL=$ACZ_COCKPIT_URL" "/DCOCKPIT_SHA256=${ACZ_COCKPIT_SHA256:?the asset hash}"); fi
# (Git Bash would turn /DVERSION=... into a path)
MSYS2_ARG_CONV_EXCL='*' "$iscc" "${args[@]}" tools/setup/ac0vr.iss
ls -la "$out/ac0vr-$version-setup.exe"
