#!/usr/bin/env bash
# tools/stray-reload.sh [--timeout S] [--shot PATH] — restart from the last checkpoint and
# prove the game is back in gameplay.
#
# The user's sequence (2026-09-02): START opens the pause menu, DOWN, DOWN reaches
# "Restart from checkpoint", ENTER opens its confirmation screen (which defaults to the
# "No" side), RIGHT moves to confirm, ENTER confirms. START is the pad button when Steam
# Input exposes the X-Box node, else Esc on the keyboard; arrows and Enter are keyboard.
#
# Success is read from mods/StrayProbe: the load is observed (ingame drops to 0 or the pawn
# is recreated) and gameplay state returns and holds for ~3 s. A crash during the reload —
# process gone, or a fresh UE4 crash dump with its fatal-error dialog — is caught on the
# tick it happens (stray-lib.sh: crash()), never waited out. Exit 0 = back in gameplay,
# 1 = failed, 3 = crashed. Composable: stray-bench.sh chains this with stray-traverse.sh.
set -u
. "$(dirname "$0")/stray-lib.sh"

TIMEOUT=120; SHOT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --shot) SHOT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

require_ingame
T0=$(date +%s)
seq0=$(probe seq)

# Step 1: open the pause menu and SEE it open (probe paused=1). Right after a load the
# level's intro keeps the menu locked for a while (measured 2026-09-02: a sequence sent
# 2 s after reaching gameplay did nothing), so retry the pause press with gaps rather
# than fire the whole sequence blind.
log "reload: opening the pause menu (pad=${PAD:-none}, keyboard=$KBD)"
opened=0
for attempt in $(seq 1 12); do
    tick_checks "pause" "$T0"
    if [ -n "$PAD" ]; then python3 "$INJECT" pad "/dev/input/$PAD" $BTN_START 80 >/dev/null 2>&1; else tap_key $KEY_ESC; fi
    for _ in $(seq 1 6); do sleep 0.5; [ "$(probe paused)" = "1" ] && break; done
    [ "$(probe paused)" = "1" ] && { opened=1; break; }
    log "  attempt $attempt: not paused yet (map=$(probe map)); waiting 3 s"
    sleep 3
done
[ "$opened" -eq 1 ] || { log "FAILED: the pause menu never opened (probe paused stays 0); START/Esc is not reaching the game or the level keeps it locked"; exit 1; }
log "  paused=1 at +$(( $(date +%s) - T0 ))s"
[ -n "$SHOT" ] && bash "$TOOLS_DIR/screenshot-gamescope.sh" "${SHOT%.png}-pause.png" 4 >/dev/null 2>&1

# Step 2: DOWN, DOWN, ENTER (reload checkpoint), RIGHT, ENTER (confirm; the dialog
# defaults to the No side).
log "reload: DOWN, DOWN, ENTER, RIGHT, ENTER"
sleep 0.5
tap_key $KEY_DOWN;  sleep 0.5
tap_key $KEY_DOWN;  sleep 0.5
tap_key $KEY_ENTER; sleep 1.0
[ -n "$SHOT" ] && bash "$TOOLS_DIR/screenshot-gamescope.sh" "${SHOT%.png}-confirm.png" 4 >/dev/null 2>&1
tap_key $KEY_RIGHT; sleep 0.5
tap_key $KEY_ENTER

# Phase 1: see the reload get ACCEPTED. Measured 2026-09-02 (user watching the screen):
# a checkpoint reload keeps the player pawn and the map, so "ingame drops to 0" never
# happens and is the wrong signal. What does happen is that the game unpauses by itself:
# paused 1 -> 0 within a few seconds of the confirm. Had RIGHT+ENTER landed on "No", the
# dialog would close but the pause menu would still be up (paused stays 1).
log "waiting for the game to unpause (reload accepted), up to 15 s"
accepted=0
for i in $(seq 1 30); do
    tick_checks "reload accept" "$T0"
    [ "$(probe paused)" = "0" ] && { accepted=1; break; }
    sleep 0.5
done
[ "$accepted" -eq 1 ] || { log "FAILED: still paused 15 s after the confirm (pawn=$(probe pawn) map=$(probe map)); the confirm landed on No or never reached the dialog"; tap_key $KEY_ESC; exit 1; }
log "  unpaused at +$(( $(date +%s) - T0 ))s: reload accepted (pawn=$(probe pawnname))"

# Phase 2: gameplay holds after the reload (the reload is fast; this is a settle check).
deadline=$(( T0 + TIMEOUT )); stable=0; last=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    tick_checks "reload settle" "$T0"
    now="pawn=$(probe pawn) pc=$(probe pc) map=$(probe map) paused=$(probe paused) ingame=$(probe ingame) seq=$(probe seq)"
    [ "$now" != "$last" ] && { log "  $now"; last="$now"; }
    if [ "$(probe ingame)" = "1" ] && [ "$(probe paused)" = "0" ]; then
        stable=$((stable + 1))
        if [ "$stable" -ge 6 ]; then
            log "BACK IN GAME after $(( $(date +%s) - T0 ))s"
            [ -n "$SHOT" ] && bash "$TOOLS_DIR/screenshot-gamescope.sh" "$SHOT" 4 >/dev/null 2>&1 && log "  screenshot: $SHOT"
            exit 0
        fi
    else
        stable=0
    fi
    sleep 0.5
done
[ "$(probe seq)" = "$seq0" ] && log "  probe seq never advanced: the game is hung"
log "FAILED: not back in gameplay ${TIMEOUT}s after the reload; last probe: $last"
exit 1
