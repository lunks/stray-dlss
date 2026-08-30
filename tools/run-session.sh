#!/bin/bash
# One complete instrumented session: launch, reach gameplay, dwell, collect, close.
#
# Closing at the end matters. The add-on's pipeline hooks make ReShade drop the D3D12 PSO
# cache, so a session left running is both slow to restart and a source of stale logs that
# look like fresh ones.
#
#   ./run-session.sh [--dwell SECONDS] [--out DIR] [--keep-open]

set -uo pipefail

GAME_DIR=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
STAGE_DIR=/run/media/deck/GamesLinux/dlss5-stage
APPID=1332010

DWELL=25
OUT=/tmp/stray-session
KEEP_OPEN=0

while [ $# -gt 0 ]; do
    case "$1" in
        --dwell)     DWELL="$2"; shift 2 ;;
        --out)       OUT="$2"; shift 2 ;;
        --keep-open) KEEP_OPEN=1; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
game_running() { pgrep -x Stray-Win64-Shi >/dev/null 2>&1; }

close_game() {
    game_running || return 0
    log "Closing the game"
    su - deck -c "cd '$STAGE_DIR' && python3 cef-eval.py 'SteamClient.Apps.TerminateApp(\"$APPID\", false)'" \
        >/dev/null 2>&1
    for _ in $(seq 1 20); do game_running || break; sleep 1; done
    if game_running; then
        log "  Steam did not stop it; killing"
        pkill -f Stray-Win64-Shipping
        sleep 3
    fi
    game_running && log "  STILL RUNNING" || log "  closed"
}

# Collect whatever exists even if we bail early — a failed session's logs are the point.
collect() {
    mkdir -p "$OUT"
    for f in stray-dlss.log stray-dlss-status.txt ReShade.log; do
        [ -f "$GAME_DIR/$f" ] && cp "$GAME_DIR/$f" "$OUT/" 2>/dev/null
    done
    if [ -d "$GAME_DIR/stray-dlss-shaders" ]; then
        rm -rf "$OUT/stray-dlss-shaders"
        cp -r "$GAME_DIR/stray-dlss-shaders" "$OUT/" 2>/dev/null
    fi
    log "Collected into $OUT"
}

trap 'collect; [ "$KEEP_OPEN" -eq 1 ] || close_game' EXIT

close_game   # never inherit a stale session
rm -f "$GAME_DIR/stray-dlss.log" "$GAME_DIR/stray-dlss-status.txt"

if ! bash /tmp/launch-stray.sh --timeout 600; then
    log "launch-stray.sh failed; collecting what there is"
    exit 1
fi

log "In game. Dwelling ${DWELL}s so the steady-state passes all run."
sleep "$DWELL"

log "--- status ---"
cat "$GAME_DIR/stray-dlss-status.txt" 2>/dev/null | sed 's/^/    /'

REPORTS=$(grep -c "DISPATCH REPORT" "$GAME_DIR/stray-dlss.log" 2>/dev/null || echo 0)
log "dispatch reports: $REPORTS"
