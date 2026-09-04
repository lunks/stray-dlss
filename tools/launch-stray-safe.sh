#!/usr/bin/env bash
# tools/launch-stray-safe.sh — start Stray on the SteamOS box and PROVE it reached gameplay,
# with no recovery heroics. Written 2026-09-02 after the previous launcher's recovery paths
# (SIGKILL of the launch tree, automatic Steam restarts) wedged the box and an agent then
# blamed the environment for what was the script and an inconsistent configuration.
#
#   launch-stray-safe.sh [--kill] [--no-drive] [--timeout S] [--shot PATH]
#
#   --kill        a game already running is killed first (pkill -x, then wait for Steam to
#                 reap the chain). Without it a running game is REFUSED, not adopted.
#   --no-drive    stop once UE4SS's probe is alive (do not press through the menu).
#   --timeout S   budget for the menu -> gameplay drive (default 300 s).
#   --shot PATH   on IN GAME, take a gamescope screenshot to PATH (proof, no ReShade needed).
#
# Principles, each one a lesson:
#   * Every precondition is checked and named BEFORE anything is launched: Steam's CEF port,
#     the cef-eval helper, the keyboard node, the reaper, which render host is active.
#   * The launch command's output is captured and shown; a helper that fails is never
#     mistaken for "Steam ignored the launch".
#   * Every wait prints progress at least every 10 s and bails the tick its condition can no
#     longer be met (process gone, new crash dump). Nothing waits out a timeout blind.
#   * "In gameplay" comes from mods/StrayProbe (UE4SS asks the engine: player pawn + map),
#     never from a render census — the plugin host's census cannot tell menu from gameplay
#     and the TAA pass runs in the menu.
#   * This script NEVER restarts Steam and never kills without --kill. When it cannot launch
#     it says exactly what it saw and what to do; the operator decides.
#
# Every exit writes stray-launch-verdict.txt in the game dir (OK: / FAILED: / CRASH: ...).
# Run as root inside the container: pct exec 113 -- bash /tmp/launch-stray-safe.sh ...
set -u

APPID=1332010
GAME_DIR=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
STAGE_DIR=/run/media/deck/GamesLinux/dlss5-stage           # cef-eval.py lives here
PROBE="$GAME_DIR/stray-game-state.txt"                      # mods/StrayProbe
STATUS="$GAME_DIR/stray-dlss-status.txt"                    # render host heartbeat, only when StatusFile=1
PLUGIN_LOG="$GAME_DIR/stray-dlss-plugin.log"                # the render host's log: written whatever StatusFile says
VERDICT="$GAME_DIR/stray-launch-verdict.txt"
CRASH_DIR="/home/deck/.local/share/Steam/steamapps/compatdata/$APPID/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Crashes"
PROTON_LOG="/home/deck/steam-$APPID.log"
KEY_ENTER=28

KILL=0; DRIVE=1; TIMEOUT=300; SHOT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --kill) KILL=1; shift ;;
        --no-drive) DRIVE=0; shift ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --shot) SHOT="$2"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
T0=$(date +%s)
UP_EPOCH=""
game_running() { pgrep -x Stray-Win64-Shi >/dev/null 2>&1; }
chain_present() { pgrep -f "AppId=$APPID" >/dev/null 2>&1; }
# The probe is written by the game through Wine's C runtime in text mode, so its lines end
# in \r\n; strip the CR or "ingame=1\r" never equals "1" (this exact bug kept the first run
# pressing Enter in gameplay for minutes, 2026-09-02).
field() { awk -F= -v k="$2" '{ sub(/\r$/, "", $2) } $1 == k { print $2; f = 1 } END { if (!f) print "" }' "$1" 2>/dev/null; }
mtime() { stat -c %Y "$1" 2>/dev/null || echo 0; }
newer() { [ "$(mtime "$1")" -ge "$T0" ]; }
find_keyboard() { awk '/^H: Handlers=/ && /sysrq/ { if (match($0, /event[0-9]+/)) { print substr($0, RSTART, RLENGTH); exit } }' /proc/bus/input/devices; }
crash_dir_new() { local d; d=$(ls -t "$CRASH_DIR" 2>/dev/null | head -n 1); [ -n "$d" ] && newer "$CRASH_DIR/$d" && echo "$d"; }

verdict() {
    {
        echo "VERDICT: $1"
        [ -n "$UP_EPOCH" ] && echo "process: up at $(date -d "@$UP_EPOCH" +%H:%M:%S), alive $(( $(date +%s) - UP_EPOCH ))s, running=$(game_running && echo yes || echo no)" || echo "process: never appeared"
        [ -f "$PROBE" ] && newer "$PROBE" && { echo "--- probe"; cat "$PROBE"; }
        [ -f "$STATUS" ] && newer "$STATUS" && { echo "--- heartbeat"; head -n 4 "$STATUS"; }
        for f in "$GAME_DIR/ue4ss/UE4SS.log" "$GAME_DIR/stray-dlss-plugin.log" "$GAME_DIR/ReShade.log"; do
            newer "$f" && { echo "--- $(basename "$f"), last 4"; tail -n 4 "$f"; }
        done
        newer "$PROTON_LOG" && { echo "--- proton log, exceptions"; grep -aE "err:seh:NtRaiseException|Unhandled exception|wine: Unhandled" "$PROTON_LOG" | tail -n 3; }
        local d; d=$(crash_dir_new)
        [ -n "$d" ] && { echo "--- UE4 crash $d"; grep -oE "<ErrorMessage>[^<]*" "$CRASH_DIR/$d/CrashContext.runtime-xml" 2>/dev/null | head -n 1; }
        echo "--- dmesg GPU since launch"; dmesg -T --since "@$T0" 2>/dev/null | grep -iE "xid|NVRM" | tail -n 3
    } | tee "$VERDICT"
}
fail()  { log "FAILED: $1"; verdict "FAILED: $1" >/dev/null; exit 1; }

# A crash dialog keeps the process alive; the user's instruction is that Enter clears it.
# Capture it first if asked, clear it, and only kill if it will not go.
crash() {
    log "CRASH: $1"
    [ -n "$SHOT" ] && bash "$(dirname "$0")/screenshot-gamescope.sh" "${SHOT%.png}-crash.png" 4 >/dev/null 2>&1 && log "  dialog captured to ${SHOT%.png}-crash.png"
    verdict "CRASH: $1" >/dev/null
    if game_running; then
        local kbd; kbd=$(find_keyboard)
        [ -n "$kbd" ] && python3 "$INJECT" key "/dev/input/$kbd" $KEY_ENTER 60 >/dev/null 2>&1
        for _ in $(seq 1 10); do game_running || break; sleep 1; done
        game_running && { log "  process survived Enter; killing it"; pkill -x Stray-Win64-Shi; }
    fi
    exit 3
}
# Per-tick checks shared by every wait. Prints nothing when all is well.
tick_checks() {
    game_running || crash "the game process is gone ($1)"
    local d; d=$(crash_dir_new)
    [ -n "$d" ] && crash "UE4 wrote a crash dump ($1): $(grep -oE '<ErrorMessage>[^<]*' "$CRASH_DIR/$d/CrashContext.runtime-xml" 2>/dev/null | head -n 1)"
}

# ------------------------------------------------------------------ preconditions
log "box boot_id=$(cat /proc/sys/kernel/random/boot_id) uptime=$(cut -d. -f1 /proc/uptime)s"
INJECT="$(dirname "$0")/inject.py"; [ -f "$INJECT" ] || INJECT=/tmp/inject.py
[ -f "$INJECT" ] || fail "inject.py not found beside this script or in /tmp"
[ -f "$STAGE_DIR/cef-eval.py" ] || fail "$STAGE_DIR/cef-eval.py is missing; the Steam launch helper must be staged"
curl -s --max-time 3 http://127.0.0.1:8080/json/list >/dev/null || fail "Steam's CEF debugger (port 8080) is not answering; Steam is down or the debugger is off"
KBD=$(find_keyboard); [ -n "$KBD" ] || fail "no sysrq-capable keyboard node; cannot drive the menu"
[ -d "$GAME_DIR/ue4ss/Mods/StrayProbe" ] || fail "mods/StrayProbe is not installed under ue4ss/Mods; the gameplay gate needs it"

HOSTS=""
[ -f "$GAME_DIR/dxgi.dll" ] && [ -f "$GAME_DIR/stray-dlss.addon64" ] && HOSTS="$HOSTS reshade-addon"
grep -qE "^StrayDLSS *: *1" "$GAME_DIR/ue4ss/Mods/mods.txt" 2>/dev/null && HOSTS="$HOSTS plugin"
[ -f "$GAME_DIR/dxgi.dll" ] && HOSTS="$HOSTS (ReShade present)"
log "render hosts active:${HOSTS:- NONE - no DLSS heartbeat is possible in this configuration}"
EXPECT_HEARTBEAT=0; case "$HOSTS" in *reshade-addon*|*plugin*) EXPECT_HEARTBEAT=1 ;; esac

if game_running; then
    if [ "$KILL" -eq 1 ]; then
        log "Stray is running; --kill given: pkill -x Stray-Win64-Shi and waiting for Steam to reap the chain"
        pkill -x Stray-Win64-Shi
        for i in $(seq 1 30); do game_running || chain_present || break; sleep 1; [ $((i % 10)) -eq 0 ] && log "  still winding down (${i}s)"; done
        chain_present && fail "the launch chain is still present 30 s after the kill; Steam has not reaped it. Inspect: pgrep -af AppId=$APPID"
    else
        fail "Stray is already running (probe age $(( $(date +%s) - $(mtime "$PROBE") ))s). Pass --kill to replace it; this script does not adopt a session it did not start"
    fi
fi
if chain_present; then
    log "A Steam launch chain for $APPID exists with no game process:"; pgrep -af "AppId=$APPID" | head -n 3 | sed 's/^/    /'
    fail "stale launch chain (a leftover reaper makes Steam ignore every launch, CLAUDE.md 2.10). Clear it by hand: kill that tree, wait 10 s, re-run"
fi

# ------------------------------------------------------------------ launch
rm -f "$PROBE" "$STATUS" "$VERDICT"

# ARM THE PROBE FOR THIS LAUNCH ONLY. mods/StrayProbe does nothing unless it finds this flag,
# and it DELETES the flag as it reads it — so a session the user starts from Steam finds none
# and schedules no loops at all. The probe costs a file write plus one game-thread engine read
# every second, which is the same shape as the ~1 Hz frame-time blip; it should exist while a
# script needs to tell gameplay from the title screen, and not otherwise.
: > "$GAME_DIR/stray-probe-armed"
chown deck:deck "$GAME_DIR/stray-probe-armed" 2>/dev/null || true
log "Armed mods/StrayProbe for this launch (one-shot flag; a Steam launch leaves it idle)"
log "Asking Steam to launch $APPID (cef-eval output shown, never hidden)"
OUT=$(su - deck -c "cd '$STAGE_DIR' && python3 cef-eval.py 'SteamClient.Apps.RunGame(\"$APPID\", \"\", -1, 100)'" 2>&1); RC=$?
printf '%s\n' "$OUT" | tail -n 3 | sed 's/^/    /'
[ $RC -eq 0 ] || fail "cef-eval.py exited $RC; the launch was never issued"

log "Waiting for Stray-Win64-Shipping (up to 180 s; Proton's first launch is slow)"
for i in $(seq 1 180); do
    game_running && break
    [ $((i % 10)) -eq 0 ] && log "  ${i}s: chain=$(chain_present && echo present || echo absent) process=absent"
    sleep 1
done
if ! game_running; then
    if chain_present; then WHY="chain present but no exe: an error dialog inside the prefix, or a slow first-run setup"
    else WHY="chain absent: Steam did not start it - a stale running state in the Steam UI or a leftover reaper"; fi
    fail "no game process after 180 s ($WHY). Not retrying, not restarting Steam"
fi
UP_EPOCH=$(date +%s); log "Process up."

log "Waiting for the UE4SS probe (proves UE4SS + Lua are alive; up to 120 s)"
for i in $(seq 1 120); do
    [ -f "$PROBE" ] && newer "$PROBE" && break
    tick_checks "waiting for the probe"
    [ $((i % 10)) -eq 0 ] && log "  ${i}s: no probe yet (UE4SS.log: $(tail -n 1 "$GAME_DIR/ue4ss/UE4SS.log" 2>/dev/null | cut -c1-90))"
    sleep 1
done
[ -f "$PROBE" ] && newer "$PROBE" || fail "UE4SS probe never appeared while the game ran: UE4SS (dwmapi=n,b) or mods/StrayProbe is not loading"
log "Probe alive: $(tr '\n' ' ' < "$PROBE")"

if [ "$EXPECT_HEARTBEAT" -eq 1 ]; then
    # TWO SIGNALS, and the second is why this gate stopped working (2026-09-03). It used to
    # wait only for stray-dlss-status.txt, which the plugin writes ONLY when StatusFile=1 -
    # and that key was set to 0 when the periodic writes were dropped. The file then never
    # appears, so a perfectly healthy plugin failed the gate on every launch ("no heartbeat"
    # for 240 s while stray-dlss-plugin.log was demonstrably being written). A gate that
    # cannot pass in the shipped configuration is worse than no gate: it taught two sessions
    # to ignore it.
    #
    # The LOG is the signal that always exists: the plugin appends to it from attach onwards,
    # so an mtime newer than this launch proves the render host is alive and running THIS
    # session. The status file stays the preferred signal when it is enabled, because it
    # carries the frame counter and the census that the line below prints.
    log "Waiting for the render host heartbeat (up to 240 s; status file or plugin log)"
    HB=""
    for i in $(seq 1 240); do
        [ -f "$STATUS" ]     && newer "$STATUS"     && { HB=status; break; }
        [ -f "$PLUGIN_LOG" ] && newer "$PLUGIN_LOG" && { HB=log;    break; }
        tick_checks "waiting for the heartbeat"
        [ $((i % 10)) -eq 0 ] && log "  ${i}s: no heartbeat yet"
        sleep 1
    done
    case "$HB" in
        status) log "Heartbeat (status file): $(head -n 3 "$STATUS" | tr '\n' ' ')" ;;
        log)    log "Heartbeat (plugin log, StatusFile is off): $(tail -n 1 "$PLUGIN_LOG" | cut -c1-110)" ;;
        *)      fail "render host ($HOSTS) wrote neither $STATUS nor $PLUGIN_LOG while the game ran" ;;
    esac
fi

if [ "$DRIVE" -eq 0 ]; then
    log "--no-drive: done."; verdict "OK: running, probe alive, not driven" >/dev/null; exit 0
fi

# ------------------------------------------------------------------ menu -> gameplay
log "Pressing Enter on /dev/input/$KBD every 0.7 s until the probe reports ingame=1 (budget ${TIMEOUT}s)"
# A MUTE ORACLE IS NOT A SLOW ONE (measured 2026-09-04). A stray-probe-quiet left behind the
# previous day made the probe stop writing pawn/pc/map/ingame while still writing seq/t, so this
# loop drove the menu for the full 420 s against a probe that could not answer - and the game had
# been sitting in gameplay the whole time. Name it before driving, rather than timing out on it.
if [ "$(field "$PROBE" quiet)" = "1" ] && [ -z "$(field "$PROBE" ingame)" ]; then
    fail "the probe is in QUIET mode and is not reporting gameplay state, so no amount of driving \
can satisfy this gate. Delete $GAME_DIR/stray-probe-quiet (a leftover measurement window) and \
re-run. The probe now expires the flag itself after 300 s, so an up-to-date StrayProbe cannot \
reach this."
fi

deadline=$(( $(date +%s) + TIMEOUT )); stable=0; last=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    tick_checks "driving the menu"
    now="pawn=$(field "$PROBE" pawn) pc=$(field "$PROBE" pc) map=$(field "$PROBE" map) ingame=$(field "$PROBE" ingame)"
    [ "$now" != "$last" ] && { log "  $now"; last="$now"; }
    if [ "$(field "$PROBE" ingame)" = "1" ]; then
        stable=$((stable + 1))
        if [ "$stable" -ge 4 ]; then       # ~3 s of continuous gameplay state, not a load-screen blip
            log "IN GAME: $now"
            [ -n "$SHOT" ] && bash "$(dirname "$0")/screenshot-gamescope.sh" "$SHOT" 4 >/dev/null 2>&1 && log "  screenshot: $SHOT"
            verdict "OK: IN GAME $now" >/dev/null; exit 0
        fi
    else
        stable=0
        python3 "$INJECT" key "/dev/input/$KBD" $KEY_ENTER 60 >/dev/null 2>&1
    fi
    sleep 0.7
done
fail "not in gameplay after ${TIMEOUT}s; last probe: $last. If pawn stays 0 with a non-menu map the game is loading slowly (raise --timeout); if the map stays a menu map, Enter is not reaching the game"
