#!/usr/bin/env bash
# tools/stray-record.sh [--seconds N] [--out FILE] — record the user's keyboard input for a
# replayable perf scenario. Records the sysrq-capable keyboard (the K400, the node the game
# reads and the one we inject into) without grabbing it, so the game keeps playing normally.
# Replay with `stray-traverse.sh --replay FILE` from the same checkpoint.
#
# The pad is NOT recorded for replay: Steam Input is off for this title, so the game reads
# the DualSense over hidraw through the Sony library / our shim, and events written to its
# evdev node never reach the game. Pad replay would have to go through the shim.
set -u
. "$(dirname "$0")/stray-lib.sh"
SECONDS_MAX=60; OUT="$GAME_DIR/stray-recording.txt"
while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) SECONDS_MAX="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
KBD=$(find_keyboard); [ -n "$KBD" ] || { log "FAILED: no sysrq-capable keyboard node"; exit 1; }
log "recording /dev/input/$KBD for up to ${SECONDS_MAX}s into $OUT — play the segment now"
python3 "$INJECT" record "/dev/input/$KBD" "$OUT" "$SECONDS_MAX"
chown deck:deck "$OUT" 2>/dev/null
n=$(wc -l < "$OUT"); dur=$(tail -n 1 "$OUT" | cut -d' ' -f1)
log "recorded $n events over ${dur:-0}s: $OUT"
