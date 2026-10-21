#!/bin/sh
# Command-processor benchmark: Fenway at-bat at 1x host, 60 fps target, 60 s.
#   tools/perf/bench.sh <name> [extra game args]     e.g. --scene_msaa=false
#   HOLD_IDLE= tools/perf/bench.sh ...                pitch during the hold (real play)
# Default HOLD_IDLE=1 stays at the plate without pitching: identical frames,
# so "CP frame work" (SDK cp_frame_stats) is repeatable to ~0.1 ms.
# Prints fps, per-frame CPU ms of the GPU command thread and the game's render
# thread (from --fps_log_interval), and the CP's working time per frame.
cd "$(dirname "$0")/../.."
N="$1"; shift
SIDE=${SIDE:-home} HOLD=60 HOLD_IDLE=${HOLD_IDLE-1} tools/atbat.sh out/perf/b_$N --resolution_scale=1 \
  --render_target_path_vulkan=host --frame_rate=60 --fps_log_interval=5 --cp_frame_stats=true \
  "$@" >/dev/null 2>&1
L=out/build/linux-amd64-release/logs/$(ls -t out/build/linux-amd64-release/logs | head -1)
python3 - "$L" "$N" <<'PY'
import sys, re
lines = [l for l in open(sys.argv[1]) if '[fps]' in l]
rows = []
for a, b in zip(lines, lines[1:]):
    m = re.search(r'\[fps\] ([\d.]+) fps', a)
    c = re.search(r'GPU Commands \(F (\d+)%', b)
    g = re.search(r'game\* (\d+)%', b)
    if m and c and g and 'cpu' in b:
        fps = float(m.group(1)); rows.append((fps, int(c.group(1)) * 10 / fps, int(g.group(1)) * 10 / fps))
rows = rows[-12:]  # the last minute (the at-bat)
cp = [tuple(map(float, re.search(r'CP frame work: ([\d.]+) ms avg, ([\d.]+) ms max', l).groups()))
      for l in open(sys.argv[1]) if 'CP frame work' in l][-12:]
if rows:
    n = len(rows)
    print('%-12s fps %.1f  cp %.2f ms/frame  game %.2f ms/frame  (fps %s)' % (
        sys.argv[2], sum(r[0] for r in rows) / n, sum(r[1] for r in rows) / n,
        sum(r[2] for r in rows) / n, ' '.join('%.0f' % r[0] for r in rows)))
if cp:
    print('%-12s CP frame work %.2f ms avg (windows %s), max %.1f' % (
        sys.argv[2], sum(c[0] for c in cp) / len(cp), ' '.join('%.1f' % c[0] for c in cp),
        max(c[1] for c in cp)))
PY
