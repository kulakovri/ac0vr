"""prune_bundle.py -- cut a bundle's compiler down to the headers and libraries one build really used (night_setup 2026-11-04).
  python -I prune_bundle.py BUNDLE BUILD_OUT LINK_TRACE
BUNDLE = a make_bundle.sh folder (changed in place: work on a copy). BUILD_OUT = the OUT of a finished player_build.py run with it
(its obj/*.d name the headers). LINK_TRACE = a text file with the output of one link with -Wl,--trace (GNU ld) or -Wl,--verbose
(lld): every path in it under toolchain/ is kept. Removes the other files under the toolchain's include and lib folders; programs
stay. Run player_build.py again on the result: the build must not notice."""
import glob
import os
import re
import sys

bundle, out, trace = sys.argv[1:4]
tc = os.path.normcase(os.path.abspath(os.path.join(bundle, "toolchain")))
keep = set()


def add(path):
    p = os.path.normcase(os.path.abspath(path))
    if p.startswith(tc):
        keep.add(p)


for d in glob.glob(os.path.join(out, "build", "obj", "*.d")) + glob.glob(os.path.join(out, "obj", "*.d")):
    text = open(d, encoding="utf-8", errors="replace").read().replace("\\\n", " ")
    for tok in re.split(r"(?<!\\)\s+", text):
        tok = tok.replace("\\ ", " ").rstrip(":")
        if tok:                                     # (player_build.py gives the compilers paths relative to its OUT)
            add(os.path.join(out, tok))

path_rx = re.compile(r"(?:[A-Za-z]:[\\/]|[.][.]?[\\/])[^\s()\"',]+|\S*toolchain[\\/][^\s()\"',]+")
for line in open(trace, encoding="utf-8", errors="replace"):
    for m in path_rx.findall(line):
        add(os.path.join(out, m))

removed = kept = 0
freed = 0
for sub, dirs, files in os.walk(tc):
    rel = os.path.relpath(sub, tc).replace("\\", "/")
    parts = rel.split("/")
    prunable = "include" in parts or "include-fixed" in parts or "lib" in parts
    if not prunable:
        continue
    for f in files:
        p = os.path.normcase(os.path.join(sub, f))
        if p in keep:
            kept += 1
            continue
        freed += os.path.getsize(p)
        os.remove(p)
        removed += 1
print("kept %d files, removed %d (%.1f MB)" % (kept, removed, freed / 1e6))
