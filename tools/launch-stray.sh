#!/bin/bash
# Launch Stray and drive it to gameplay, unattended.
#
# Runs on the SteamOS host (CT113). Steam is asked to start the game through its own CEF
# console, then X (BTN_SOUTH on the "Microsoft X-Box 360 pad 0" node Steam Input synthesises)
# is tapped twice a second to get through the splash and menu.
#
# Stopping is driven by evidence rather than a timer: the add-on writes a shader census to
# stray-dlss-status.txt, and the measured census is ~150 in the main menu against ~728 in
# gameplay (CLAUDE.md 2.3). The threshold sits between them with wide margin either side.
#
#   ./launch-stray.sh [--no-input] [--timeout SECONDS]

set -uo pipefail

GAME_DIR=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
STAGE_DIR=/run/media/deck/GamesLinux/dlss5-stage
STATUS="$GAME_DIR/stray-dlss-status.txt"
APPID=1332010

BTN_SOUTH=304          # X on a DualSense, A in X360 mapping — "confirm"
PAD_NAME="Microsoft X-Box 360 pad 0"

PRESS_INPUT=1
TIMEOUT=420

while [ $# -gt 0 ]; do
    case "$1" in
        --no-input) PRESS_INPUT=0; shift ;;
        --timeout)  TIMEOUT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# inject.py ships beside this script. Resolving it relatively means staging the two
# together is enough — /tmp gets wiped and a hardcoded path there silently stops working.
INJECT="${INJECT:-$(cd "$(dirname "$0")" && pwd)/inject.py}"
if [ ! -f "$INJECT" ]; then
    echo "inject.py not found at $INJECT — stage it next to launch-stray.sh" >&2
    exit 2
fi

# Restarting Steam is cheap here and clears the states that no amount of process killing
# fixes: a wedged launch chain, a stale app-running flag, or an error dialog left on screen.
# The gamescope session respawns Steam automatically.
restart_steam() {
    log "Restarting Steam"
    su - deck -c "cd '$STAGE_DIR' && python3 cef-eval.py 'SteamClient.User.StartRestart(false)'" \
        >/dev/null 2>&1 || pkill -x steam 2>/dev/null

    # Wait for it to go away and come back with its CEF console listening again.
    sleep 5
    for _ in $(seq 1 60); do
        if curl -s --max-time 2 http://127.0.0.1:8080/json/list >/dev/null 2>&1; then
            log "  Steam is back"
            sleep 5
            return 0
        fi
        sleep 2
    done
    log "  Steam did not come back within 120s"
    return 1
}

find_pad_node() {
    # Steam tears this node down with the game, so its number is not stable and must be
    # resolved every run. (CLAUDE.md 2.11)
    awk -v want="$PAD_NAME" '
        /^N: Name=/ { name = $0; sub(/^N: Name="/, "", name); sub(/"$/, "", name) }
        /^H: Handlers=/ && name == want {
            if (match($0, /event[0-9]+/)) { print substr($0, RSTART, RLENGTH); exit }
        }' /proc/bus/input/devices
}

# pgrep -c prints "0" AND exits non-zero when nothing matches, so a `|| echo 0`
# fallback yields "0\n0" and breaks the test. Just use the exit status.
game_running() { pgrep -x Stray-Win64-Shi >/dev/null 2>&1; }

# A reaper or half-started Proton chain from a previous session blocks every future launch
# silently. Clear it before asking Steam for anything. (CLAUDE.md 2.10)
clear_stale_chain() {
    pgrep -f "AppId=$APPID" >/dev/null 2>&1 || return 0
    log "Stale launch chain from a previous session; clearing it first"
    # Descend the tree rather than pattern-killing: pv-adverb and srt-bwrap also run Steam's
    # own webhelper, so a pattern kill takes Steam down with the game.
    for pid in $(pgrep -f "AppId=$APPID" 2>/dev/null); do
        for child in $(pgrep -P "$pid" 2>/dev/null); do
            for g in $(pgrep -P "$child" 2>/dev/null); do kill -9 "$g" 2>/dev/null; done
            kill -9 "$child" 2>/dev/null
        done
        kill -9 "$pid" 2>/dev/null
    done
    for _ in $(seq 1 15); do
        pgrep -f "AppId=$APPID" >/dev/null 2>&1 || break
        sleep 1
    done
}

status_field() {
    [ -f "$STATUS" ] || { echo 0; return; }
    awk -F= -v k="$1" '$1 == k { print $2; found = 1 } END { if (!found) print 0 }' "$STATUS"
}

# ---------------------------------------------------------------------------------------

if game_running; then
    log "Stray is already running; leaving it alone."
else
    clear_stale_chain

    log "Clearing stale add-on output"
    rm -f "$STATUS" "$GAME_DIR/stray-dlss.log"

    log "Asking Steam to launch $APPID"
    su - deck -c "cd '$STAGE_DIR' && python3 cef-eval.py 'SteamClient.Apps.RunGame(\"$APPID\", \"\", -1, 100)'" \
        >/dev/null 2>&1

    log "Waiting for the process"
    for _ in $(seq 1 60); do
        game_running && break
        sleep 2
    done

    if ! game_running; then
        log "Stray-Win64-Shipping did not appear within 120s."
        if pgrep -f "AppId=$APPID" >/dev/null 2>&1; then
            log "  The Proton chain is up but the game exe never spawned — usually an error"
            log "  dialog inside the prefix. Chain:"
            pgrep -af "AppId=$APPID" | head -3 | sed 's/^/    /'
        fi

        # One automatic recovery attempt: tear the chain down, restart Steam, try once more.
        if [ "${STRAY_NO_RETRY:-0}" != "1" ]; then
            log "Attempting recovery: teardown + Steam restart + one retry"
            clear_stale_chain
            restart_steam
            su - deck -c "cd '$STAGE_DIR' && python3 cef-eval.py 'SteamClient.Apps.RunGame(\"$APPID\", \"\", -1, 100)'" \
                >/dev/null 2>&1
            for _ in $(seq 1 60); do
                game_running && break
                sleep 2
            done
        fi
    fi

    if ! game_running; then
        log "FAILED: the game would not start even after a Steam restart."
        exit 1
    fi
    log "Process up."
fi

# The first load after a shader dump is very slow: registering the pipeline events makes
# ReShade drop the D3D12 PSO cache, so every shader recompiles. Be patient here.
log "Waiting for the add-on heartbeat (first load recompiles every shader — this is slow)"
for _ in $(seq 1 "$TIMEOUT"); do
    [ -f "$STATUS" ] && break
    game_running || { log "FAILED: the game exited before the add-on reported in."; exit 1; }
    sleep 1
done

if [ ! -f "$STATUS" ]; then
    log "FAILED: no heartbeat after ${TIMEOUT}s. Is the add-on loading? Check:"
    log "  grep -i stray-dlss '$GAME_DIR/ReShade.log'"
    exit 1
fi
log "Add-on is alive (vkd3d=$(status_field vkd3d))"

if [ "$PRESS_INPUT" -eq 0 ]; then
    log "--no-input given; not pressing anything."
    exit 0
fi

# Steam Input creates the virtual pad node some seconds AFTER the game process appears,
# and the add-on's heartbeat now arrives within ~3 s of that (it is written from the first
# presents), so the node is routinely not there yet at this point. Poll for it: measured
# 2026-09-01, an immediate lookup failed on a box whose DualSense was connected all along.
PAD_NODE=""
for _ in $(seq 1 60); do
    PAD_NODE=$(find_pad_node)
    [ -n "$PAD_NODE" ] && break
    game_running || { log "FAILED: the game exited while waiting for the pad node."; exit 1; }
    sleep 2
done
if [ -z "$PAD_NODE" ]; then
    log "FAILED: could not find the '$PAD_NAME' node within 120s."
    log "  Steam Input creates it when the game starts; check /proc/bus/input/devices"
    exit 1
fi
log "Pad node: /dev/input/$PAD_NODE"

log "Pressing X every 0.5s until the shader census says we are in game"
deadline=$(( $(date +%s) + TIMEOUT ))
last_census=-1

while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! game_running; then
        log "FAILED: the game exited."
        exit 1
    fi

    census=$(status_field shader_census)
    if [ "$census" != "$last_census" ]; then
        log "  census=$census  frame=$(status_field frame)  dispatches=$(status_field dispatches)"
        last_census=$census
    fi

    if [ "$(status_field in_game)" -eq 1 ]; then
        log "IN GAME (census=$census, taa_pipelines=$(status_field taa_pipelines))"
        exit 0
    fi

    python3 "$INJECT" pad "/dev/input/$PAD_NODE" "$BTN_SOUTH" 60 >/dev/null 2>&1
    sleep 0.5
done

log "TIMEOUT after ${TIMEOUT}s at census=$(status_field shader_census)."
log "  If the census is stuck near the menu value the button is not reaching the game;"
log "  if it is climbing, just raise --timeout."
exit 1
