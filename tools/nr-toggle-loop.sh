#!/usr/bin/env bash
# tools/nr-toggle-loop.sh — the live NgxNR enable->disable A/B, ten times, in gameplay.
#
# This is the check the deferred-destruction work exists for. The UE4SS host re-applies NgxNR
# from the hot-reloaded StrayDLSS.ini on its own tick thread (mods/StrayDLSS/src/Host.cpp), which
# is NOT the render thread: the game is recording and submitting frames while this runs. Every
# flip therefore exercises exactly the transition a sibling port crashed on
# (RemixProjGroup/dxvk-remix @ a69254ab).
#
# Two dwell times, on purpose:
#   * a LONG off-dwell (default 3 s) lets the queued teardown actually run, so the log shows
#     "releasing feature 18 ... at the PRESENT boundary" with its fence values;
#   * a SHORT off-dwell (default 0.3 s) re-enables before the teardown has run, which is the
#     cancel path ("RE-ENABLED before its queued teardown ran").
# Alternating them means one run covers both.
#
# Run inside the container: pct exec 113 -- bash /tmp/nr-toggle-loop.sh [cycles] [long] [short]
set -u

GAME_DIR=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
INI="$GAME_DIR/ue4ss/Mods/StrayDLSS/StrayDLSS.ini"
LOG="$GAME_DIR/stray-dlss-plugin.log"
STATUS="$GAME_DIR/stray-dlss-status.txt"

CYCLES=${1:-10}
LONG=${2:-3}
SHORT=${3:-0.3}

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
frames() { sed -n 's/^frame=//p' "$STATUS" 2>/dev/null | tail -n 1; }
alive() { pgrep -x Stray-Win64-Shi >/dev/null 2>&1; }

set_nr() { sed -i "s/^NgxNR=[01]$/NgxNR=$1/" "$INI"; }

[ -f "$INI" ] || { echo "no ini at $INI"; exit 2; }
alive || { echo "the game is not running"; exit 2; }

START_LINES=$(wc -l < "$LOG")
log "plugin log is $START_LINES lines; frame=$(frames)"
log "starting $CYCLES NgxNR toggles (long off-dwell ${LONG}s, short ${SHORT}s)"

for i in $(seq 1 "$CYCLES"); do
    if ! alive; then log "GAME DIED at cycle $i"; exit 3; fi
    # Alternate: odd cycles let the teardown complete, even ones cancel it.
    if [ $((i % 2)) -eq 1 ]; then DWELL=$LONG; WHAT="long (teardown should RUN)"; else DWELL=$SHORT; WHAT="short (teardown should be CANCELLED)"; fi
    set_nr 0
    sleep "$DWELL"
    set_nr 1
    sleep 1
    log "cycle $i/$CYCLES done, $WHAT, frame=$(frames) alive=$(alive && echo yes || echo NO)"
done

sleep 3
log "final: frame=$(frames) alive=$(alive && echo yes || echo NO)"
echo "=== plugin log, lines added during the loop ==="
tail -n +$((START_LINES + 1)) "$LOG" | grep -a -e "NR " -e "NR:" -e "host: NgxNR" -e teardown -e releasing -e RE-ENABLED
echo "=== NR counters now ==="
grep -a -e "^nr_" "$STATUS"
