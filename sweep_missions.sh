#!/usr/bin/env bash
# Boots every campaign mission with the debug boot (ACZ_BOOT_MISSION) and flies ~45 s in each, one instance at a time:
# checks that the boot works for each mission and collects their pipeline states (ac5_pipelines.keys / .cache), so
# later sessions build them at start-up instead of stuttering the first time an effect appears.
# Usage: ./sweep_missions.sh [first] [last] [seconds]      -> out/sweep/mNN_*.ppm, sweep_mNN.log, a summary
set -u
cd "$(dirname "$0")"
first=${1:-2}; last=${2:-18}; secs=${3:-45}
ISO=${ACZ_DISC:-}
[ -n "$ISO" ] || { echo "no disc: set ACZ_DISC to your disc image or to a folder with its files"; exit 2; }
export ACZ_LAUNCHER=${ACZ_LAUNCHER:-0}
mkdir -p out/sweep
export ACZ_TELEMETRY=${ACZ_TELEMETRY:-off}   # (a test run sends no stream, whatever the settings file says)
export ACZ_STATS=${ACZ_STATS:-0}             # (a test run never asks and never sends: docs/PRIVACY.md)
for m in $(seq "$first" "$last"); do
    mm=$(printf '%02d' "$m")
    ACZ_BOOT_MISSION=$m PS2_RN_VP=1 PS2_UPSCALE=2 PS2_SPEED_LOG=1 PS2_SHOT_EVERY=600 PS2_SHOT_FROM=1200 PS2_SHOT_MAX=2 \
        timeout "$secs" build/zero/ac5.exe --data gen_zero --disc "$(cygpath -w "$ISO")" --watchdog 0 > "sweep_m$mm.log" 2>&1
    for f in out/shot_*.ppm; do [ -e "$f" ] && mv "$f" "out/sweep/m${mm}_$(basename "$f")"; done
    enter=$(grep -a -c 'GameControl::enter' "sweep_m$mm.log")
    stage=$(grep -a 'GameControl::enter' "sweep_m$mm.log" | head -1 | sed 's/.* stage \([-0-9]*\).*/\1/')
    newp=$(grep -a 'new_pipelines=' "sweep_m$mm.log" | sed 's/.*new_pipelines=\([0-9]*\).*/\1/' | awk '{n+=$1} END{print n+0}')
    crash=$(grep -a -c -i 'fatal\|crash\|exception\|exhausted' "sweep_m$mm.log")
    echo "mission $mm: entered $enter, stage ${stage:-?}, pipelines built $newp, problems $crash, shots $(ls out/sweep/m${mm}_* 2>/dev/null | wc -l)"
done
