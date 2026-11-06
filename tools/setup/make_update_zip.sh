#!/bin/bash
# make_update_zip.sh VERSION OUT -- called by make_release.sh: OUT/ac0vr-VERSION-update.zip from OUT/stage: everything of the install but Python and the
# compiler (the runtime library, headers, shaders, DLLs, the recompiler and its config, tools/setup/, the glyphs, the joystick profiles, the documents,
# version.txt, ac0vr.exe). ACZ_RELEASE_TOOLCHAIN_ASSET=1 also writes OUT/ac0vr-toolchain-<toolchain_id>.zip, for a release whose compiler changed: a
# player's ac0vr.exe fetches it when bundle.txt's toolchain_id differs from the installed one. Uses the stage's own Python.
set -e
version=$1; out=$2
[ -n "$out" ] || { echo "usage: make_update_zip.sh VERSION OUT"; exit 2; }
"$out/stage/python/python.exe" -I -B - "$version" "$out" "${ACZ_RELEASE_TOOLCHAIN_ASSET:-0}" <<'EOF'
import os
import sys
import zipfile

version, out, tc = sys.argv[1:4]
stage = os.path.join(out, "stage")


def pack(zip_path, base, keep):
    n = 0
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d, sub, files in os.walk(base):
            sub.sort()
            for f in sorted(files):
                full = os.path.join(d, f)
                rel = os.path.relpath(full, base).replace(os.sep, "/")
                if keep(rel):
                    z.write(full, rel)
                    n += 1
    print("%s: %d files, %.1f MB" % (os.path.basename(zip_path), n, os.path.getsize(zip_path) / 1e6))


pack(os.path.join(out, "ac0vr-%s-update.zip" % version), stage,
     lambda r: not r.startswith(("python/", "bundle/toolchain/")))
if tc == "1":
    tid = ""
    with open(os.path.join(stage, "bundle", "bundle.txt"), encoding="utf-8") as f:
        for line in f:
            if line.startswith("toolchain_id="):
                tid = line.strip().split("=", 1)[1]
    pack(os.path.join(out, "ac0vr-toolchain-%s.zip" % tid), os.path.join(stage, "bundle", "toolchain"), lambda r: True)
EOF
