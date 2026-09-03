#!/bin/bash
# A REPEATABLE HAPTIC SCENARIO, the DualSense counterpart to tools/stray-bench.sh.
#
# The bench answers "is it fast and stable". This answers "did the pad actually do anything,
# and which path drove it" — the question every DualSense build has needed a human for.
#
# It drives the cat through the actions the game's own input map says produce haptics
# (DefaultInput.ini): Q = CatButton, the contextual scratch/interact; LeftAlt = Meow;
# SpaceBar = JumpUp; E = Interact; Two = ToggleLight. Each is held for a beat and separated
# by a quiet gap, so a counter that moves can be attributed to ONE action rather than to the
# sequence as a whole.
#
# It reads the plugin's own counters before and after and prints the delta. Nothing here
# judges feel — only the user can — but "starts went 0 -> 3, the tap saw peak 0.71, the coils
# were driven by the SUBMIX" is the difference between a build that works and one that only
# logs that it works.
#
#   tools/stray-haptics.sh [--label L] [--gap S]
#
# THE PAD ITSELF IS NOT REPLAYABLE: the game reads the DualSense over hidraw through
# libScePad, not evdev, so injected pad events reach nothing (CLAUDE.md §2.11). Keyboard is
# the only channel that drives the cat from a script, which is why this is keyboard-only.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=stray-lib.sh
[ -f "$HERE/stray-lib.sh" ] && . "$HERE/stray-lib.sh"

GAME_DEFAULT=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
GAME="${GAMEDIR:-$GAME_DEFAULT}"
LOG="$GAME/stray-dualsense.log"
LABEL="run"
GAP=3

while [ $# -gt 0 ]; do
  case "$1" in
    --label) LABEL="${2:-run}"; shift ;;
    --gap)   GAP="${2:-3}"; shift ;;
    -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

# The keyboard node whose Handlers include sysrq — a Power Button and a PC speaker also
# advertise kbd and injecting into those reaches nothing (CLAUDE.md §2.11).
KBD=$(grep -B8 sysrq /proc/bus/input/devices | grep -oE 'event[0-9]+' | tail -1)
[ -n "$KBD" ] || { echo "no sysrq keyboard node found" >&2; exit 1; }
NODE=/dev/input/$KBD

# Linux keycodes for the actions DefaultInput.ini names.
declare -A KEY=( [q]=16 [leftalt]=56 [space]=57 [e]=18 [two]=3 [up]=103 )

counters() {
  # One line each, so a diff is readable.
  grep -oE 'hap\[[^]]*\]'   "$LOG" | tail -1
  grep -oE 'trig\[[^]]*\]'  "$LOG" | tail -1
  grep -oE 'COILS: [^|]*'   "$LOG" | tail -1
  grep -oE 'peak=[0-9.]+ rms=[0-9.]+' "$LOG" | tail -1
}

press() { python3 "$HERE/inject.py" key "$NODE" "${KEY[$1]}" "${2:-0.15}" >/dev/null 2>&1 \
          || python3 /tmp/press.py "$NODE" "${KEY[$1]}" >/dev/null 2>&1; }

echo "=== haptic scenario '$LABEL' on $NODE (gap ${GAP}s)"
echo "--- before"; counters
BEFORE_LINE=$(wc -l < "$LOG")

for action in q leftalt space e two q; do
  echo "  -> $action"
  press "$action" 0.20
  sleep "$GAP"
done
# One walk-and-scratch, because the contextual action depends on what the cat is next to.
python3 "$HERE/inject.py" hold "$NODE" "${KEY[up]}" 2 >/dev/null 2>&1 || true
sleep 1; press q 0.20; sleep "$GAP"

echo "--- after"; counters
echo "--- vibration starts during the run"
sed -n "$((BEFORE_LINE+1)),\$p" "$LOG" | grep -E "StartPS5Vibration .* -> " | tail -10
echo "--- submix peak during the run (max)"
sed -n "$((BEFORE_LINE+1)),\$p" "$LOG" | grep -oE 'peak=[0-9.]+' | sort -t= -k2 -g | tail -1
