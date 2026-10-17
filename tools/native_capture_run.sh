#!/bin/sh
# Native renderer phase 1: play the scripted at-bat (tools/atbat.sh) and take a
# shadow capture (native_capture.cpp, F6) every few seconds, with a screenshot
# each time to tell the scenes apart.
#
#   tools/native_capture_run.sh <outdir> [interval s] [count] [game args...]
#
# Captures: <outdir>/data/captures/native_capture_NNN.txt; screenshots:
# <outdir>/scene/tNN.png (taken just before capture NNN is armed). Uses
# HOLD=30 (keeps pitching) unless HOLD is set. Linux only (kill -s RTMIN+1).
O="$1"; shift
INTERVAL="${1:-15}"; [ $# -gt 0 ] && shift
COUNT="${1:-14}"; [ $# -gt 0 ] && shift
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$O/scene"; O="$(cd "$O" && pwd)"
(
  i=0
  while [ $i -lt "$COUNT" ]; do
    sleep "$INTERVAL"; i=$((i + 1))
    P=$(pgrep -x thebigs2) || continue
    GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 gamescopectl screenshot "$O/scene/t$(printf %02d $i).png" >/dev/null 2>&1
    kill -s RTMIN+1 "$P"
  done
) &
L=$!
HOLD="${HOLD:-30}" "$ROOT/tools/atbat.sh" "$O" "$@" >/dev/null 2>&1
kill $L 2>/dev/null
ls "$O/data/captures" 2>/dev/null | wc -l
