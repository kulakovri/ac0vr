#!/bin/bash
# upscale_tex.sh OUTDIR FILE.png... -- x4 copies of cockpit textures for ac2cpit --tex-dir OUTDIR (which prefers
# OUTDIR/<file name> over a model's own texture of that name): Real-ESRGAN (realesrgan-ncnn-vulkan, BSD-3,
# $ACVR/ext_tools/realesrgan; model realesr-animevideov3, x4) on the GPU, a few seconds a
# texture. Only PNG inputs (the replacement keeps the name, and ac2cpit decodes by the extension). An existing copy
# newer than its source is kept. Used by the build scripts' X4 lists (ck_audit 2026-10-28: the seats, floors and walls
# the eye sees from 0.3-0.8 m at 3-6 texels a degree); ac2cpit's --max-tex (2048) caps the results.
set -e
cd "$(dirname "$0")/../.."
# ACVR: the folder with ext_tools, by default the nearest parent of this tree that has one
if [ -z "${ACVR:-}" ]; then
    ACVR=$PWD
    while [ ! -d "$ACVR/ext_tools" ] && [ "$ACVR" != "$(dirname "$ACVR")" ]; do ACVR=$(dirname "$ACVR"); done
fi
ESR=${ESR:-$ACVR/ext_tools/realesrgan}
[ -x "$ESR/realesrgan-ncnn-vulkan.exe" ] || { echo "upscale_tex: no Real-ESRGAN in $ESR (set ESR)"; exit 1; }
out=$1; shift
mkdir -p "$out"
w() { cygpath -w "$1"; }
for f in "$@"; do
    case "$f" in *.png|*.PNG) ;; *) echo "upscale_tex: $f: PNG only"; exit 1 ;; esac
    [ -f "$f" ] || { echo "upscale_tex: missing $f"; exit 1; }
    o="$out/${f##*/}"
    [ -f "$o" ] && [ "$o" -nt "$f" ] && continue
    "$ESR/realesrgan-ncnn-vulkan.exe" -i "$(w "$f")" -o "$(w "$o")" -n realesr-animevideov3 -s 4 > /dev/null 2>&1 \
        || { echo "upscale_tex: Real-ESRGAN failed on $f"; exit 1; }
    echo "upscale_tex: ${f##*/} -> $o"
done
