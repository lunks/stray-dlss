#!/usr/bin/env bash
# tools/stray-bench.sh [--runs N] [--label TEXT] [--shot-dir DIR]
# The user's protocol (2026-09-02): from a live gameplay session, N times {restart from
# checkpoint, run the 15 s traverse}, all in ONE process, never a fresh launch per run.
# One scenario answers both questions: a death at any reload or traverse is the stability
# failure (exit 3, crash verdict written by stray-lib.sh), and the surviving windows are
# the performance result (one CSV row each). Run the same thing against the other render
# host with a different --label and compare rows.
set -u
. "$(dirname "$0")/stray-lib.sh"

# The scenario is the USER'S OWN RECORDING by default (2026-09-02, "let's use this going
# forward"): tools/data/stray-recording-user1.txt, staged into the game dir. It starts with
# ~1.6 s of Left held before Up (turning the cat); trimming that lead-in changed the path, so
# it stays. 301 events, 16.9 s, cut on a frame boundary. --synthetic forces the old
# Up + Left/Right pattern; --replay FILE uses another recording.
DEFAULT_REPLAY="$GAME_DIR/stray-recording-user1.txt"
RUNS=3; LABEL="run"; SHOT_DIR=""; REPLAY=""; SYNTHETIC=0
while [ $# -gt 0 ]; do
    case "$1" in
        --replay) REPLAY="$2"; shift 2 ;;
        --synthetic) SYNTHETIC=1; shift ;;
        --runs) RUNS="$2"; shift 2 ;;
        --label) LABEL="$2"; shift 2 ;;
        --shot-dir) SHOT_DIR="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$REPLAY" ] && [ "$SYNTHETIC" -eq 0 ]; then
    [ -s "$DEFAULT_REPLAY" ] || { log "FAILED: default recording $DEFAULT_REPLAY is missing; stage tools/data/stray-recording-user1.txt or pass --synthetic"; exit 1; }
    REPLAY="$DEFAULT_REPLAY"
fi
require_ingame
log "bench: scenario=${REPLAY:-synthetic Up+Left/Right}"
log "bench: $RUNS x (reload + traverse), label=$LABEL, hosts: $( [ -f "$GAME_DIR/dxgi.dll" ] && echo -n 'ReShade ' ; grep -qE '^StrayDLSS *: *1' "$GAME_DIR/ue4ss/Mods/mods.txt" && echo -n 'plugin' ; echo )"
for i in $(seq 1 "$RUNS"); do
    shot=""; [ -n "$SHOT_DIR" ] && shot="$SHOT_DIR/$LABEL-$i.png"
    bash "$TOOLS_DIR/stray-reload.sh" ${shot:+--shot "$shot"} || { rc=$?; log "bench: reload $i failed (exit $rc)"; exit "$rc"; }
    sleep 3   # let the load settle before moving (a 1 s settle changed the replayed path, 2026-09-02)
    bash "$TOOLS_DIR/stray-traverse.sh" --label "$LABEL-$i" ${REPLAY:+--replay "$REPLAY"} || { rc=$?; log "bench: traverse $i failed (exit $rc)"; exit "$rc"; }
done
log "bench: $RUNS runs completed; rows tagged $LABEL-* in $GAME_DIR/stray-bench.csv"
tail -n "$RUNS" "$GAME_DIR/stray-bench.csv"
