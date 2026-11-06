#!/usr/bin/env bash
# GPU stress benchmark without a headset: boots straight into mission 1 (ACZ_BOOT_MISSION), flat window, high internal
# resolution. Flat 6x (3840x2688) is about the pixel load of 4x stereo (2 x 2560x1792).
# Usage: ./bench_flat.sh [gs|native] [scale] [seconds] [extra VAR=value ...]
#   ./bench_flat.sh native 6        -> bench_native_6x.log and a summary
# Each field is drawn REPEAT times (default 4, PS2_BENCH_REPEAT) so the GPU runs at full clocks; the reported times
# are per drawing (total / REPEAT). With 1 the GPU idles in a power-saving state and the times are inflated.
# Phases (fields): mission loads ~25, in-engine intro flyby ~300-800 (the heavy part), Start skips it at ~800,
# flight from ~900.
set -u
cd "$(dirname "$0")"
mode=${1:-native}
scale=${2:-6}
secs=${3:-75}
shift 3 2>/dev/null || shift $#
ISO=${ACZ_DISC:-}
[ -n "$ISO" ] || { echo "no disc: set ACZ_DISC to your disc image or to a folder with its files"; exit 2; }
log=bench_${mode}_${scale}x.log
rep=${REPEAT:-4}
export ACZ_BOOT_MISSION=1 PS2_UPSCALE=$scale PS2_SPEED_LOG=1 PS2_BENCH_REPEAT=$rep
unset PS2_XR PS2_XR_MODE PS2_RN_VP
[ "$mode" = native ] && export PS2_RN_VP=1
export ACZ_LAUNCHER=${ACZ_LAUNCHER:-0}
for kv in "$@"; do export "$kv"; done
export ACZ_TELEMETRY=${ACZ_TELEMETRY:-off}   # (a test run sends no stream, whatever the settings file says)
export ACZ_STATS=${ACZ_STATS:-0}             # (a test run never asks and never sends: docs/PRIVACY.md)
timeout "$secs" build/zero/ac5.exe --data gen_zero --disc "$(cygpath -w "$ISO")" --watchdog 0 > "$log" 2>&1
echo "== $mode ${scale}x, each field drawn $rep times ($log)"
# gpu-speed windows are 5 s each: window 2-3 = intro flyby (heavy), window 5+ = flight
grep -a 'gpu-speed: dt' "$log" | awk -v r="$rep" '{a=$5; m=$6; sub("avg=","",a); sub("max=","",m); a/=r; m/=r; n++;
    if (n>=2 && n<=3) { ia+=a; ic++; if (m>im) im=m }
    if (n>=5) { fa+=a; fc++; if (m>fm) fm=m; if (a>fw) fw=a }
  } END { if (ic) printf "intro  avg %.2f ms  max %.2f\n", ia/ic, im;
          if (fc) printf "flight avg %.2f ms  worst window %.2f  max %.2f  (%d windows)\n", fa/fc, fw, fm, fc }'
grep -a 'vk-count per frame' "$log" | tail -1 | sed 's/.*draws \([0-9]*\).*begin-rendering \([0-9]*\) barrier-calls \([0-9]*\).*snapshots \([0-9.]*\).*/draws \1, render passes \2, barriers \3, snapshots \4/'
grep -a 'exhausted\|fatal\|crash' "$log" | head -3
