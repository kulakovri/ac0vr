#!/usr/bin/env bash
# make_source_pack.sh OUT.tar [COCKPIT_SRC] -- the complete corresponding source of the public cockpit set's GPL files, for the
# offer in its LICENSES.md (GPL section 3 / section 6; night_assets 2026-11-04): the fetched upstream parts the build scripts read,
# unchanged, the T-38's x4 upscales among them; the converters and build scripts are tools/cockpit of the repository. Left out:
# the F-22's Instruments/EngineFire/0fdd30ec.png (a hash-named atlas; build_f22a.sh F22A_ATLAS=0 does not read it). Writes nothing
# else, uploads nothing. OUT.tar "-" = stdout (to measure it: make_source_pack.sh - | wc -c). TORNADO_GPL_PARENT: the folder that holds
# Tornado_GPL (tools/cockpit/prep_tornado_gpl.sh), default COCKPIT_SRC.
set -e
out=${1:--}
src=$(realpath -m "${2:-$(dirname "$0")/../../../cockpit_src}")
tor=$(realpath -m "${TORNADO_GPL_PARENT:-$src}")
FOLDERS="A-10 F-15 F-16 fgdata F-18C FA-18E_uchi F-20 F-22 Grumman-EA-6B-Prowler JAS39-Gripen McDonnell-F-4-Phantom MiG-21bis MiG-29_9-12 Mirage2000
 SaabJ35F Su-27SK T-38 T-38_x4 eurofighter godot-xr-hands hands"
tar --exclude='0fdd30ec.png' -cf "$out" -C "$src" $FOLDERS -C "$tor" Tornado_GPL
