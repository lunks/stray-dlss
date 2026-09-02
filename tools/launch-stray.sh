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
# The heartbeat (stray-dlss-status.txt) phase caps SEPARATELY from the whole run. Both hosts
# rewrite that file every 30 frames (dlss_app.cpp:1038 / addon_entry.cpp) and the first write
# lands within ~3 s of the first presents (measured), so 240 s is ~80x margin on a slow first
# load and still far short of a caller's own timeout. A hang in start_mod is caught in seconds
# by the exception/stall checks below, never by this cap.
HEARTBEAT_TIMEOUT=240

while [ $# -gt 0 ]; do
    case "$1" in
        --no-input)          PRESS_INPUT=0; shift ;;
        --timeout)           TIMEOUT="$2"; shift 2 ;;
        --heartbeat-timeout) HEARTBEAT_TIMEOUT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# Claude's Bash tool kills a call at 600 s, so a --timeout above ~540 means the CALLER sees
# nothing — the script is killed mid-run before it can print or write the verdict. Honour it
# (a background invocation reading the verdict file is legitimate), but say so.
if [ "$TIMEOUT" -gt 540 ]; then
    echo "WARNING: --timeout $TIMEOUT exceeds the caller's 600 s Bash-tool cap; a foreground caller" >&2
    echo "         will be killed before this script reports. Use <=540, or run in the background" >&2
    echo "         and read \$GAME_DIR/stray-launch-verdict.txt." >&2
fi

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
UE4SS_LOG="$GAME_DIR/ue4ss/UE4SS.log"
LAUNCH_EPOCH=$(date +%s)
UP_EPOCH=""
# Unhandled-exception lines already in the Proton log before we launched. A crash is a COUNT
# above this, not merely the presence of a line (the log can carry benign .NET exceptions from
# a previous session, and Proton appends rather than truncates).
PROTON_EXC_BASELINE=0
# grep -c prints its count on stdout (0 on no match) but EXITS NON-ZERO on no match, so the
# `cmd && grep || echo 0` idiom emits "0\n0" for an empty-but-present file and breaks every
# numeric test (the pgrep -c trap documented elsewhere in this script). Capture and default
# instead: exactly one number, always.
exc_count() { local n; n=$(grep -acE "err:seh:NtRaiseException|Unhandled exception|wine: Unhandled" "$PROTON_LOG" 2>/dev/null); echo "${n:-0}"; }

newer_than_launch() { [ -e "$1" ] && [ "$(stat -c %Y "$1" 2>/dev/null || echo 0)" -ge "$LAUNCH_EPOCH" ]; }
file_age() { [ -e "$1" ] && echo $(( $(date +%s) - $(stat -c %Y "$1" 2>/dev/null || echo 0) )) || echo 999999; }

# A NEW unhandled exception in the Proton log since launch: the count has risen above the
# pre-launch baseline. This is the fastest crash signal — a c0000005 in start_mod writes it
# ~11 s in, long before any timeout, and before the engine's own crash handler exists.
proton_new_exception() { [ "$(exc_count)" -gt "$PROTON_EXC_BASELINE" ]; }

# start_mod() is hung (or crashed inside it before Unreal's handler existed): UE4SS.log's LAST
# line is still "Starting C++ mod" — it prints the next line only once a mod's start_mod
# returns — and the file has not advanced for a while. Threshold 60 s: the plugin's own
# start_mod does no blocking work (it hooks an export and returns; the device work happens
# later, from the game thread), so a full minute with no further UE4SS line while the process
# lives means it is wedged, not merely slow.
start_mod_hung() {
    newer_than_launch "$UE4SS_LOG" || return 1
    tail -n 1 "$UE4SS_LOG" 2>/dev/null | grep -q "Starting C++ mod" || return 1
    [ "$(file_age "$UE4SS_LOG")" -ge 60 ]
}

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

# The game NEVER RAN LIKE LAST TIME: the process died (or an unhandled exception was raised)
# before the add-on wrote its first heartbeat. This is its own verdict so it is never confused
# with "ran, then no heartbeat" — and it exits within one loop tick, never at a timeout. A
# crash handler that keeps the exe alive does not save it: a NEW Proton unhandled-exception
# line since launch counts as death too.
crash() {
    local secs="?"
    [ -n "$UP_EPOCH" ] && secs=$(( $(date +%s) - UP_EPOCH ))
    log "CRASH: game died ${secs}s after start, before the add-on reported in — $1"
    verdict "CRASH: game died ${secs}s after start, before the add-on reported in — $1"
    exit 1
}
# True when the game must be treated as dead in a pre-heartbeat phase: the process is gone, OR
# it is nominally alive but the Proton log recorded an unhandled exception since launch.
game_dead_before_heartbeat() {
    game_running || return 0
    proton_new_exception && return 0
    return 1
}

# ---------------------------------------------------------------------------------------

if game_running && [ -f "$STATUS" ] && [ "$(file_age "$STATUS")" -lt 30 ]; then
    # A LIVE session: the heartbeat moved within the last 30 s. Leave it alone.
    log "Stray is already running and its heartbeat is fresh ($(file_age "$STATUS")s); leaving it alone."
    UP_EPOCH=$LAUNCH_EPOCH
    PROTON_EXC_BASELINE=$(exc_count)
elif game_running; then
    # The process exists but its heartbeat is stale (or never appeared): a hung or crashed
    # session from a previous run, not something to inherit. Clear it and launch fresh.
    log "Stray-Win64-Shipping is running but its heartbeat is stale ($([ -f "$STATUS" ] && file_age "$STATUS" || echo 'no status file')s); treating it as a dead session and clearing it."
    clear_stale_chain
    pkill -x Stray-Win64-Shi 2>/dev/null
    for _ in $(seq 1 10); do game_running || break; sleep 1; done
fi
if ! game_running; then
    clear_stale_chain

    log "Clearing stale add-on output"
    rm -f "$STATUS" "$GAME_DIR/stray-dlss.log" "$VERDICT"

    log "Asking Steam to launch $APPID"
    su - deck -c "cd '$STAGE_DIR' && python3 cef-eval.py 'SteamClient.Apps.RunGame(\"$APPID\", \"\", -1, 100)'" \
        >/dev/null 2>&1

    log "Waiting for the process"
    chain_seen=0
    for _ in $(seq 1 60); do
        game_running && break
        # Steam GAVE UP: the launch chain was up and is now gone with no game exe. Do not wait
        # out the 120 s — nothing is coming. (The chain takes a moment to appear, so only
        # conclude this once we have actually seen it.)
        if pgrep -f "AppId=$APPID" >/dev/null 2>&1; then
            chain_seen=1
        elif [ "$chain_seen" = 1 ]; then
            log "The Steam launch chain (AppId=$APPID) disappeared without a game process — Steam gave up."
            break
        fi
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
    PROTON_EXC_BASELINE=$(exc_count)
    log "Process up (proton-log exception baseline=$PROTON_EXC_BASELINE)."
fi

# The first load after a shader dump is very slow: registering the pipeline events makes
# ReShade drop the D3D12 PSO cache, so every shader recompiles. Be patient here.
log "Waiting for the add-on heartbeat (first load recompiles every shader — this is slow), cap ${HEARTBEAT_TIMEOUT}s"
hb_deadline=$(( $(date +%s) + HEARTBEAT_TIMEOUT ))
while [ "$(date +%s)" -lt "$hb_deadline" ]; do
    [ -f "$STATUS" ] && break
    # Death or an unhandled exception before the heartbeat is a CRASH — exit THIS tick, with
    # the crash verdict, never at the cap.
    game_running || crash "the process is gone; the add-on never wrote its status file"
    proton_new_exception && crash "unhandled exception in the Proton log (a c0000005 in start_mod writes one here)"
    # A wedged start_mod with NO exception is a hang, not a crash — its own reason, still no wait.
    start_mod_hung && fail "UE4SS is wedged in a mod's start_mod (UE4SS.log stuck at 'Starting C++ mod' for $(file_age "$UE4SS_LOG")s, no heartbeat)"
    sleep 1
done

if [ ! -f "$STATUS" ]; then
    log "  no heartbeat after ${HEARTBEAT_TIMEOUT}s. Is the add-on loading? Check:"
    log "  grep -i stray-dlss '$GAME_DIR/ReShade.log' and $GAME_DIR/stray-dlss-plugin.log"
    fail "no heartbeat after ${HEARTBEAT_TIMEOUT}s while the game kept running"
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
# Frame-stall: once the heartbeat exists, frame= must keep advancing (both hosts bump it every
# present). Unchanged for 30 s while the process lives means the game is hung on the GPU or in a
# wait — 30 s is ~1600 frames at 55 fps, far beyond any legitimate stutter or load hitch.
last_frame=-1; frame_since=$(date +%s)
# Census-stall: if the shader census has not moved for 150 s and we are not in game, the button
# is not reaching the game (the diagnosis this script's old timeout text already gave). 150 s is
# generous for a first load that recompiles every shader, and still far short of TIMEOUT.
census_since=$(date +%s)

while [ "$(date +%s)" -lt "$deadline" ]; do
    if ! game_running; then
        fail "the game exited while driving the menu"
    fi
    proton_new_exception && fail "unhandled exception in the Proton log while driving the menu (crash mid-session)"

    now=$(date +%s)
    frame=$(status_field frame)
    if [ "$frame" != "$last_frame" ]; then
        last_frame=$frame; frame_since=$now
    elif [ $(( now - frame_since )) -ge 30 ]; then
        fail "frame= stuck at $frame for 30s while the process lives — the game is hung, not loading"
    fi

    census=$(status_field shader_census)
    if [ "$census" != "$last_census" ]; then
        log "  census=$census  frame=$frame  dispatches=$(status_field dispatches)"
        last_census=$census; census_since=$now
    elif [ "$(status_field in_game)" -ne 1 ] && [ $(( now - census_since )) -ge 150 ]; then
        fail "shader census stuck at $census for 150s and not in game — the button is not reaching the game (check the input node)"
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
verdict "FAILED: timeout after ${TIMEOUT}s at census=$(status_field shader_census), game still running" >/dev/null
exit 1
