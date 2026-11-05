#!/usr/bin/env bash
# map_check.sh -- which cockpit file each aircraft id gets from play_zero_vr.sh's `cockpit` word, against the cockpit
# column of docs/aircraft_ids/README.md (the row's first *.cpit name). The runtime takes an id's FIRST entry in
# ACZ_VR_COCKPIT_MODELS (ps2_video.c cpit_load), so a preferred file listed after a stand-in is never used (bug #252:
# the X-02, YF-23A, FALKEN and Morgan flew with their stand-ins from 2026-10-30 until 2026-10-31). Prints the map an id
# at a time; exit 1 when a row's file is built but the launcher gives that aircraft another one. The launcher's own
# switches are unset: the check is of the defaults the README describes.
#   tools/cockpit/map_check.sh          (from anywhere; reads the files of this checkout)
#   tools/cockpit/map_check.sh FILE     the cockpit word of another copy of the launcher (an absolute path)
#   tools/cockpit/map_check.sh --map MAP.txt [DIR]  a cockpit set's map (`id=file` lines, # comments, as the launcher reads
#       cockpits/map.txt; night_assets 2026-11-04): every id of docs/aircraft_ids/README.md and the file it gets (an id's first
#       entry whose file exists: the launcher skips the others), exit 1 when an id is out of range or a listed file is in
#       none of the folders. DIR (default: the map's folder) may be a colon list of folders: a pack on top of the public set
if [ "${1:-}" = --map ]; then
    mapf=$(realpath -m "$2"); dirs=
    IFS=: read -r -a dl <<< "${3:-$(dirname "$mapf")}"
    for d in "${dl[@]}"; do dirs="$dirs:$(realpath -m "$d")"; done
    dirs=${dirs#:}
    cd "$(dirname "$0")/../.." || exit 1
    bad=0
    declare -A got listed
    while IFS= read -r l || [ -n "$l" ]; do
        l=${l%%#*}; l=${l//[[:space:]]/}
        [ -n "$l" ] || continue
        id=${l%%=*}; f=${l#*=}; f=${f#cockpits/}
        case "$id" in ''|*[!0-9]*) echo "map_check: bad line: $l"; bad=1; continue ;; esac
        if [ "$id" -gt 35 ]; then echo "map_check: id $id out of range"; bad=1; continue; fi
        found=
        IFS=: read -r -a dl <<< "$dirs"
        for d in "${dl[@]}"; do [ -f "$d/$f" ] || [ -f "$d/cockpits/$f" ] && found=1; done
        listed[$f]=1
        if [ -z "$found" ]; then missing="${missing:-} $f"; continue; fi
        [ -n "${got[$id]:-}" ] || got[$id]=$f
    done < "$mapf"
    while IFS='|' read -r _ idc namec _; do
        idc=${idc// /}; namec=${namec# }; namec=${namec% }
        case "$idc" in ''|*[!0-9]*) continue ;; esac
        printf '%2d %-18s %s\n' "$idc" "$namec" "${got[$idc]:-(the default model)}"
    done < docs/aircraft_ids/README.md
    [ -z "${missing:-}" ] || echo "map_check: listed, in no folder (skipped by the launcher; exit 1 unless MAP_ALLOW_MISSING=1):${missing}"
    [ -z "${missing:-}" ] || [ -n "${MAP_ALLOW_MISSING:-}" ] || bad=1
    [ "$bad" = 0 ] || exit 1
    echo "map_check: ok"
    exit 0
fi
play=$(realpath -m "${1:-$(dirname "$0")/../../play_zero_vr.sh}")
cd "$(dirname "$0")/../.." || exit 1
unset ACZ_VR_COCKPIT_MODELS ACZ_VR_COCKPIT_MODEL
map=$(set -- cockpit; eval "$(sed -n '/^for w in "\$@"; do/,/^done/p' "$play")"; echo "${ACZ_VR_COCKPIT_MODELS:-}")
[ -n "$map" ] || { echo "map_check: no ACZ_VR_COCKPIT_MODELS from play_zero_vr.sh's cockpit word"; exit 2; }
awk -F'|' -v map="$map" '
BEGIN {
    n = split(map, it, ",")
    for (i = 1; i <= n; i++) {
        split(it[i], kv, "=")
        if (!(kv[1] in got)) got[kv[1]] = kv[2]
        else if (got[kv[1]] != kv[2]) unused[kv[1]] = unused[kv[1]] " " kv[2]
    }
}
/^\| [0-9]+ \|/ {
    id = $2 + 0; name = $3; gsub(/^ +| +$/, "", name)
    want = ""
    if (match($5, /[a-z0-9_]+\.cpit/)) want = "cockpits/" substr($5, RSTART, RLENGTH)
    have = (id in got) ? got[id] : "(the default model)"
    line = sprintf("%2d %-16s %s", id, name, have)
    if (id in unused) line = line "  (listed later, unused:" unused[id] ")"
    if (want != "" && have != want && system("[ -f " want " ]") == 0) { line = line "  MISMATCH: the README says " want; bad++ }
    print line
}
END {
    if (bad) { print "map_check: " bad " aircraft get another cockpit than the README says"; exit 1 }
    print "map_check: ok"
}' docs/aircraft_ids/README.md
