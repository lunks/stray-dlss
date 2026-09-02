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
#
# Every exit, success or failure, is also written to $GAME_DIR/stray-launch-verdict.txt: a
# caller whose ssh/tool call is cut off by its own timeout can still read what happened.
# When the game dies, the verdict carries the LAST LINES of UE4SS.log / ReShade.log, the
# Proton log's unhandled-exception lines, the newest UE4 crash dump's ErrorMessage and any
# GPU dmesg since launch — so a crash never again reads as a bare "the game exited"
# (measured 2026-09-02: the first hardware run of the UE4SS plugin died 11 s in at
# start_mod, before the engine's crash handler existed, and the script said only that).
# NOTE: a --timeout longer than the caller's own limit (Claude's Bash tool caps at 600 s)
# means the caller sees nothing at all; keep --timeout <= 540 there and read the file.

set -uo pipefail

GAME_DIR=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
STAGE_DIR=/run/media/deck/GamesLinux/dlss5-stage
STATUS="$GAME_DIR/stray-dlss-status.txt"
APPID=1332010

BTN_SOUTH=304          # X on a DualSense, A in X360 mapping — "confirm"
KEY_ENTER=28           # the keyboard's "confirm", for when Steam Input is off
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

# The keyboard whose Handlers include `sysrq` — the one that reaches the game
# (tools/screenshot-stray.sh). Used when Steam Input is OFF for this title
# (UseSteamControllerConfig=0 in localconfig.vdf, measured 2026-09-01 after the DualSense
# work, which needs the game to see the real pad): then no X360 node ever appears and the
# menu is driven with Enter instead of X.
find_keyboard_node() {
    awk '/^H: Handlers=/ && /sysrq/ { if (match($0, /event[0-9]+/)) { print substr($0, RSTART, RLENGTH); exit } }' \
        /proc/bus/input/devices
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

# Where the game's own evidence lands (CLAUDE.md 2.2 / 2.10).
PROTON_LOG="/home/deck/steam-$APPID.log"
CRASH_DIR="/home/deck/.local/share/Steam/steamapps/compatdata/$APPID/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Crashes"
VERDICT="$GAME_DIR/stray-launch-verdict.txt"
LAUNCH_EPOCH=$(date +%s)
UP_EPOCH=""

newer_than_launch() { [ -e "$1" ] && [ "$(stat -c %Y "$1" 2>/dev/null || echo 0)" -ge "$LAUNCH_EPOCH" ]; }

# $1 = one-line reason. Prints AND writes the verdict file. Only files touched since this
# launch are quoted, so a stale log from the previous session cannot pose as evidence
# (CLAUDE.md 5, gotchas: "confirm the timestamps are from the NEW session").
verdict() {
    {
        echo "VERDICT: $1"
        if [ -n "$UP_EPOCH" ]; then
            echo "process: up at $(date -d "@$UP_EPOCH" +%H:%M:%S), alive $(( $(date +%s) - UP_EPOCH ))s, running=$(game_running && echo yes || echo no)"
        else
            echo "process: never appeared"
        fi
        for f in "$GAME_DIR/ue4ss/UE4SS.log" "$GAME_DIR/ReShade.log" "$GAME_DIR/stray-dlss.log"; do
            newer_than_launch "$f" || continue
            echo "--- $(basename "$f"), last 4 lines"
            tail -n 4 "$f"
        done
        if newer_than_launch "$PROTON_LOG"; then
            echo "--- proton log, exceptions"
            grep -aE "err:seh:NtRaiseException|Unhandled exception|wine: Unhandled|err:module:" "$PROTON_LOG" | tail -n 4
        fi
        d=$(ls -t "$CRASH_DIR" 2>/dev/null | head -n 1)
        if [ -n "$d" ] && newer_than_launch "$CRASH_DIR/$d"; then
            echo "--- UE4 crash dump $d"
            grep -oE "<ErrorMessage>[^<]*" "$CRASH_DIR/$d/CrashContext.runtime-xml" 2>/dev/null | head -n 1
        else
            echo "--- no UE4 crash dump from this launch: died before the engine's handler existed, or was killed"
        fi
        echo "--- dmesg GPU lines since launch"
        dmesg -T --since "@$LAUNCH_EPOCH" 2>/dev/null | grep -iE "xid|NVRM" | tail -n 3
    } | tee "$VERDICT"
}
fail() { log "FAILED: $1"; verdict "FAILED: $1"; exit 1; }

# ---------------------------------------------------------------------------------------

if game_running; then
    log "Stray is already running; leaving it alone."
    UP_EPOCH=$LAUNCH_EPOCH
else
    clear_stale_chain

    log "Clearing stale add-on output"
    rm -f "$STATUS" "$GAME_DIR/stray-dlss.log" "$VERDICT"

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
        fail "the game would not start even after a Steam restart"
    fi
    UP_EPOCH=$(date +%s)
    log "Process up."
fi

# The first load after a shader dump is very slow: registering the pipeline events makes
# ReShade drop the D3D12 PSO cache, so every shader recompiles. Be patient here.
log "Waiting for the add-on heartbeat (first load recompiles every shader — this is slow)"
for _ in $(seq 1 "$TIMEOUT"); do
    [ -f "$STATUS" ] && break
    game_running || fail "the game exited before the add-on reported in"
    sleep 1
done

if [ ! -f "$STATUS" ]; then
    log "  no heartbeat after ${TIMEOUT}s. Is the add-on loading? Check:"
    log "  grep -i stray-dlss '$GAME_DIR/ReShade.log'"
    fail "no heartbeat after ${TIMEOUT}s while the game kept running"
fi
log "Add-on is alive (vkd3d=$(status_field vkd3d))"

if [ "$PRESS_INPUT" -eq 0 ]; then
    log "--no-input given; not pressing anything."
    verdict "OK: add-on alive, --no-input" >/dev/null
    exit 0
fi

# Steam Input creates the virtual pad node some seconds AFTER the game process appears,
# and the add-on's heartbeat now arrives within ~3 s of that (it is written from the first
# presents), so the node is routinely not there yet at this point. Poll for it: measured
# 2026-09-01, an immediate lookup failed on a box whose DualSense was connected all along.
PAD_NODE=""
for _ in $(seq 1 10); do
    PAD_NODE=$(find_pad_node)
    [ -n "$PAD_NODE" ] && break
    game_running || fail "the game exited while waiting for the pad node"
    sleep 2
done
INPUT_KIND=pad
INPUT_NODE="$PAD_NODE"
INPUT_CODE=$BTN_SOUTH
if [ -z "$PAD_NODE" ]; then
    KBD_NODE=$(find_keyboard_node)
    if [ -z "$KBD_NODE" ]; then
        log "  Steam Input creates the pad node when it is enabled for the title; check /proc/bus/input/devices"
        fail "no '$PAD_NAME' node within 20s and no sysrq-capable keyboard node either"
    fi
    log "No '$PAD_NAME' node (Steam Input off for this title?); driving the menu with Enter on /dev/input/$KBD_NODE"
    INPUT_KIND=key
    INPUT_NODE="$KBD_NODE"
    INPUT_CODE=$KEY_ENTER
else
    log "Pad node: /dev/input/$PAD_NODE"
fi

log "Pressing $([ "$INPUT_KIND" = pad ] && echo X || echo Enter) every 0.5s until the shader census says we are in game"
deadline=$(( $(date +%s) + TIMEOUT ))
last_census=-1

while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! game_running; then
        fail "the game exited while driving the menu"
    fi

    census=$(status_field shader_census)
    if [ "$census" != "$last_census" ]; then
        log "  census=$census  frame=$(status_field frame)  dispatches=$(status_field dispatches)"
        last_census=$census
    fi

    if [ "$(status_field in_game)" -eq 1 ]; then
        log "IN GAME (census=$census, taa_pipelines=$(status_field taa_pipelines))"
        verdict "OK: IN GAME census=$census taa_pipelines=$(status_field taa_pipelines)" >/dev/null
        exit 0
    fi

    python3 "$INJECT" "$INPUT_KIND" "/dev/input/$INPUT_NODE" "$INPUT_CODE" 60 >/dev/null 2>&1
    sleep 0.5
done

log "TIMEOUT after ${TIMEOUT}s at census=$(status_field shader_census)."
log "  If the census is stuck near the menu value the button is not reaching the game;"
log "  if it is climbing, just raise --timeout."
verdict "FAILED: timeout after ${TIMEOUT}s at census=$(status_field shader_census), game still running" >/dev/null
exit 1
