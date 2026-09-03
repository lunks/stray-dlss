#!/usr/bin/env bash
# tools/nr-screenpct.sh — change r.ScreenPercentage in the LIVE game and watch what NR does.
#
# The check this exists for: a screen-percentage change moves the render rect, which is the
# guide grid feature 18's temporal history was accumulated against AND part of the rect the
# feature is keyed on. The NR path must therefore (a) decline for a few frames with the
# `recreating` reason rather than releasing the feature from inside the intercepted TAA
# dispatch, (b) release it once at the present boundary with the fence values, and (c) come
# back. It must NOT crash and must NOT sit in `recreating` forever.
#
# Driven through mods/StrayConsole, which executes lines from stray-console.cmd on the game
# thread — no Engine.ini edit (the game rewrites it on exit) and no relaunch.
#
# Run inside the container: pct exec 113 -- bash /tmp/nr-screenpct.sh 70 [settle_s]
set -u

GAME_DIR=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
CMD="$GAME_DIR/stray-console.cmd"
LOG="$GAME_DIR/stray-dlss-plugin.log"
STATUS="$GAME_DIR/stray-dlss-status.txt"

PCT=${1:-70}
SETTLE=${2:-12}

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
counter() { sed -n "s/^nr_refused_$1=//p" "$STATUS" 2>/dev/null | tail -n 1; }
frames() { sed -n 's/^frame=//p' "$STATUS" 2>/dev/null | tail -n 1; }

pgrep -x Stray-Win64-Shi >/dev/null 2>&1 || { echo "the game is not running"; exit 2; }

START_LINES=$(wc -l < "$LOG")
log "before: frame=$(frames) recreating=$(counter recreating) applied=$(sed -n 's/^nr_applied=//p' "$STATUS" | tail -1)"

log "asking the game for r.ScreenPercentage $PCT"
echo "r.ScreenPercentage $PCT" > "$CMD"

for i in $(seq 1 "$SETTLE"); do
    sleep 1
    pgrep -x Stray-Win64-Shi >/dev/null 2>&1 || { log "GAME DIED $i s after the change"; exit 3; }
    [ $((i % 4)) -eq 0 ] && log "  ${i}s: frame=$(frames) recreating=$(counter recreating) applied=$(sed -n 's/^nr_applied=//p' "$STATUS" | tail -1)"
done

log "after: frame=$(frames) recreating=$(counter recreating) applied=$(sed -n 's/^nr_applied=//p' "$STATUS" | tail -1)"
echo "=== plugin log since the change ==="
tail -n +$((START_LINES + 1)) "$LOG" | grep -a -e "NR" -e "DLSS feature" -e "rects moved" -e "guide grid" -e "releasing"
