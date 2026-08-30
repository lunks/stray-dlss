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

find_pad_node() {
    # Steam tears this node down with the game, so its number is not stable and must be
    # resolved every run. (CLAUDE.md 2.11)
    awk -v want="$PAD_NAME" '
        /^N: Name=/ { name = $0; sub(/^N: Name="/, "", name); sub(/"$/, "", name) }
        /^H: Handlers=/ && name == want {
            if (match($0, /event[0-9]+/)) { print substr($0, RSTART, RLENGTH); exit }
        }' /proc/bus/input/devices
}

game_running() { [ "$(pgrep -xc Stray-Win64-Shi 2>/dev/null || echo 0)" -gt 0 ]; }

status_field() {
    [ -f "$STATUS" ] || { echo 0; return; }
    awk -F= -v k="$1" '$1 == k { print $2; found = 1 } END { if (!found) print 0 }' "$STATUS"
}

# ---------------------------------------------------------------------------------------

if game_running; then
    log "Stray is already running; leaving it alone."
else
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
        log "FAILED: the process never appeared."
        log "  A leftover 'reaper' from a previous session makes Steam silently ignore"
        log "  further launch requests. Check: pgrep -af reaper"
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

PAD_NODE=$(find_pad_node)
if [ -z "$PAD_NODE" ]; then
    log "FAILED: could not find the '$PAD_NAME' node."
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

    python3 /tmp/inject.py pad "/dev/input/$PAD_NODE" "$BTN_SOUTH" 60 >/dev/null 2>&1
    sleep 0.5
done

log "TIMEOUT after ${TIMEOUT}s at census=$(status_field shader_census)."
log "  If the census is stuck near the menu value the button is not reaching the game;"
log "  if it is climbing, just raise --timeout."
exit 1
