#!/bin/sh
# Reproducible gameplay frame for rendering checks: Yankees @ Red Sox, Fenway
# Park at night (the menu defaults), first at-bat, in a headless gamescope at
# 1280x720. Drives the menus with the keyboard-as-controller bindings.
# The controller takes the Yankees (away: you bat first, the CPU pitches and
# the scripted swings strike out - its strike-out camera views; the tutorial
# is turned off); SIDE=home takes the Red Sox instead (you pitch, tutorial on:
# the frame the benchmarks and older captures use).
#
#   tools/atbat.sh <outdir> [game args...]   e.g. --resolution_scale=1
#   RDC=1 tools/atbat.sh ...                 also RenderDoc-capture the at-bat (F9)
#   HOLD=60 tools/atbat.sh ...               then keep playing for 60 s
#
# Writes <outdir>/bat0..3.png (the at-bat) and tut*.png (tutorial pages seen).
# Saves go to <outdir>/data (a fresh profile-less folder), never the user's.
# Needs gamescope, xdotool, python3 + PIL/numpy; the game data in game/.
O="$1"; shift; mkdir -p "$O"; O="$(cd "$O" && pwd)"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -n "$RDC" ] && RDC="${RENDERDOCCMD:-renderdoccmd} capture -w -c $O/cap"
# WRAP="<cmd>": launch the game through it instead (e.g. a gdb sampler, or
# wine for the Windows build: WRAP=wine BIN=out/build/win-amd64-cross/thebigs2.exe;
# set WINEDLLOVERRIDES="msvcp140,msvcp140_atomic_wait,vcruntime140,vcruntime140_1=n",
# Wine's builtin msvcp140 is too old for it - see packaging/build-windows.sh)
[ -n "$WRAP" ] && RDC="$WRAP"
# BIN=<path>: game binary (default: the release build)
BIN="${BIN:-./out/build/linux-amd64-release/thebigs2}"
export RDC ROOT BIN
XDG_CONFIG_HOME="$O/cfg" gamescope --backend headless -W 1280 -H 720 -w 1280 -h 720 -- \
  sh -c 'cd "$ROOT" && exec $RDC "$BIN" --user_data_root="$0" --game_data_root="$ROOT/game" "$@"' "$O/data" "$@" \
  > "$O/gamescope.txt" 2>&1 &
GS=$!
# The nested Xwayland's display: :2 here, but :1 when the session already
# has :0 only (e.g. Steam Deck desktop mode) - gamescope says which.
D=""
for _ in $(seq 30); do
  D=$(grep -o "Starting Xwayland on :[0-9]*" "$O/gamescope.txt" 2>/dev/null | head -1 | grep -o ":[0-9]*$")
  [ -n "$D" ] && break
  sleep 0.5
done
export DISPLAY="${D:-:2}" GAMESCOPE_WAYLAND_DISPLAY=gamescope-0
key() { xdotool keydown "$1"; sleep 0.15; xdotool keyup "$1"; sleep "${2:-1.5}"; }
shot() { gamescopectl screenshot "$O/$1.png" >/dev/null 2>&1; sleep 0.5; }
sleep 45
# title, save notice, "no profile found" (fresh save folder in <outdir>/data: No), Play Ball
key Return 4; key Return 5; key space 6; key space 4; key space 4
SIDE="${SIDE:-away}"
if [ "$SIDE" = home ]; then key d 1.5; else key a 1.5; fi  # controller -> that team
key space 5; key space 4; key space 4; key space 4; key space 6; key space 10   # defaults
sleep 15
clear=0                     # tutorial pages: A (or X) only while one shows (A at the plate pitches)
for t in $(seq 0 19); do
  shot "tut$t"
  # Without numpy / PIL (e.g. on the Deck) the check can't run: press A a few
  # times instead.
  if ! python3 -c "import numpy, PIL" 2>/dev/null; then
    if [ "$SIDE" = home ]; then [ $t -lt 6 ] && { key space 3; continue; } || break; fi
    key l 3; break
  fi
  if python3 "$ROOT/tools/is_tutorial.py" "$O/tut$t.png"; then
    # Away: X (L key) turns the tutorial off (it would also pop up between pitches).
    if [ "$SIDE" = home ]; then key space 3; else key l 3; shot "tutx$t"; fi
    clear=0
  else clear=$((clear + 1)); [ $clear -ge 2 ] && break; sleep 2; fi
done
for i in 0 1 2 3; do shot "bat$i"; sleep 1; done
[ -n "$LATESHOT" ] && { sleep "$LATESHOT"; shot late; }
[ -n "$RDC" ] && key F9 10
# HOLD=<s>: keep playing that long before quitting. Home: A every few seconds
# pitches (HOLD_IDLE=1: don't pitch - the same frame content throughout, for
# benchmarks). Away: a button every few seconds - batting it's an early swing
# (strike-outs), pitching it throws.
if [ -n "$HOLD" ]; then
  end=$(( $(date +%s) + HOLD ))
  # HOLD_SHOTS=1: a screenshot after every pitch (hold000.png, ...)
  n=0
  while [ "$(date +%s)" -lt "$end" ]; do
    if [ -n "$HOLD_IDLE" ]; then sleep 4
    elif [ "$SIDE" = home ]; then key space 4
    else
      # With the tutorial off a pitch takes a second press (the meter), and a
      # pitch drops off the pitcher's list once it's been hit hard: cycle A, B,
      # X, Y.
      # Batting, any of them swings (mostly strikes).
      case $((n % 4)) in 0) k=space ;; 1) k=BackSpace ;; 2) k=l ;; *) k=p ;; esac
      key $k 0.6; key $k 3.4
    fi
    [ -n "$HOLD_SHOTS" ] && shot "hold$(printf %03d $n)"
    n=$((n + 1))
  done
fi
key F10 8
# gamescope ignores SIGTERM while a child hangs on (Wine's helper processes).
pkill -9 -P $GS 2>/dev/null; kill -9 $GS 2>/dev/null; sleep 1
if [ "$WRAP" = wine ]; then  # Wine's services (winedevice.exe, ...) outlive it
  for p in $(pgrep -f 'system32'); do
    grep -qz "^WINEPREFIX=${WINEPREFIX:-$HOME/.wine}\$" /proc/$p/environ 2>/dev/null && kill -9 $p
  done
fi
