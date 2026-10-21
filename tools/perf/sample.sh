#!/bin/sh
# Sample thread stacks during the Fenway at-bat (perf/ptrace attach are blocked
# on this machine, so the game runs under gdb and gets SIGSTOPs).
#   tools/perf/sample.sh <out.txt> [samples] [thread prefixes] [game args...]
#   e.g. tools/perf/sample.sh out/perf/cp.txt 300 "GPU Commands" --frame_rate=60
# Then: python3 tools/perf/prof.py <out.txt>
cd "$(dirname "$0")/../.."
OUT="$1"; N="${2:-200}"; THREADS="${3:-GPU Commands}"; shift 3 2>/dev/null
mkdir -p "$(dirname "$OUT")"
(sleep 140; P=$(pgrep -x thebigs2); for i in $(seq 1 $((N + 20))); do kill -STOP $P 2>/dev/null; sleep 0.25; done) &
NSAMP=$N SAMP_THREADS="$THREADS" SAMP_OUT="$PWD/$OUT" WRAP="gdb -q -batch -x tools/perf/sample.gdb --args" \
  SIDE=home HOLD=$((N / 4 + 30)) HOLD_IDLE=1 tools/atbat.sh out/perf/sample_run --resolution_scale=1 \
  --render_target_path_vulkan=host "$@" >/dev/null 2>&1
wait
wc -l "$OUT"
