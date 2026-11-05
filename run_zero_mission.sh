#!/usr/bin/env bash
# Boot the Ace Combat Zero recompilation and drive the pad into mission 1 (Glacial Skies) for testing.
# Timeline (vblank fields, 60 per second) measured on 2026-10-19 with an empty virtual memory card:
#   ~400  (was the "files corrupted" dialog, fixed 2026-10-19 by the save-manager queue in zero_vr.c; the Up/Cross
#          presses now land on the logos and are harmless)
#   700-1900  logos / title                 -> Start presses
#   2050-2550 Campaign > New Game > Normal  -> Cross presses
#   2800-3400 intro movie and briefing      -> Start presses (skip)
#   3600+     pre-mission menu, hangar, launch -> Cross presses; flight starts around field 6000
# Usage: ./run_zero_mission.sh [seconds] [extra env assignments...]
#   e.g. ./run_zero_mission.sh 150 PS2_SHOT_EVERY=300 PS2_XR=1
set -u
cd "$(dirname "$0")"
secs=${1:-150}
shift || true
ISO=${ACZ_DISC:-}
[ -n "$ISO" ] || { echo "no disc: set ACZ_DISC to your disc image or to a folder with its files"; exit 2; }
S="400:UP 430:CROSS"
for f in $(seq 700 90 1900);  do S="$S $f:START"; done
for f in $(seq 2050 100 2550); do S="$S $f:CROSS"; done
for f in $(seq 2800 60 3400);  do S="$S $f:START"; done
for f in $(seq 3600 100 6000); do S="$S $f:CROSS"; done
[ -n "${EXTRA_PAD:-}" ] && S="$S $EXTRA_PAD"
export PS2_PAD_SCRIPT="$S"
export ACZ_LAUNCHER=${ACZ_LAUNCHER:-0}
for kv in "$@"; do export "$kv"; done
mkdir -p out
timeout "$secs" ${ACZ_EXE:-build/zero/ac5.exe} --data gen_zero --disc "$(cygpath -w "$ISO")" --watchdog 0 > zero_mission.log 2>&1
echo "exit $? (log: zero_mission.log)"
