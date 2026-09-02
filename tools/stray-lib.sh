#!/usr/bin/env bash
# tools/stray-lib.sh — shared helpers for the composable box scripts:
#   launch-stray-safe.sh   start the game and prove gameplay (self-contained, same conventions)
#   stray-reload.sh        restart from checkpoint: START, DOWN, DOWN, ENTER, RIGHT, ENTER
#   stray-traverse.sh      the perf scenario: hold UP, alternate LEFT/RIGHT every 3 s, 15 s
#   stray-bench.sh         N x (reload + traverse), one CSV row per run
# Source it: `. "$(dirname "$0")/stray-lib.sh"`. Runs as root inside the container.
#
# Ground truth for "in gameplay" is mods/StrayProbe's stray-game-state.txt (player pawn +
# player controller + a non-menu map), never a render census. The frame counter used for
# fps comes from the render host heartbeat (stray-dlss-status.txt, rewritten every 30
# frames by the add-on and the plugin alike); with no host active only timing is reported.

APPID=1332010
GAME_DIR=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
PROBE="$GAME_DIR/stray-game-state.txt"
STATUS="$GAME_DIR/stray-dlss-status.txt"
VERDICT="$GAME_DIR/stray-launch-verdict.txt"
CRASH_DIR="/home/deck/.local/share/Steam/steamapps/compatdata/$APPID/pfx/drive_c/users/steamuser/AppData/Local/Hk_project/Saved/Crashes"
PAD_NAME="Microsoft X-Box 360 pad 0"
TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INJECT="$TOOLS_DIR/inject.py"; [ -f "$INJECT" ] || INJECT=/tmp/inject.py

# Linux input codes (input-event-codes.h), not Windows VKs.
KEY_ESC=1; KEY_ENTER=28; KEY_UP=103; KEY_LEFT=105; KEY_RIGHT=106; KEY_DOWN=108
BTN_START=315

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
game_running() { pgrep -x Stray-Win64-Shi >/dev/null 2>&1; }
mtime() { stat -c %Y "$1" 2>/dev/null || echo 0; }
field() { awk -F= -v k="$2" '$1 == k { print $2; f = 1 } END { if (!f) print "" }' "$1" 2>/dev/null; }
probe() { field "$PROBE" "$1"; }
probe_age() { echo $(( $(date +%s) - $(mtime "$PROBE") )); }
hb_frame() { field "$STATUS" frame; }

find_keyboard() { awk '/^H: Handlers=/ && /sysrq/ { if (match($0, /event[0-9]+/)) { print substr($0, RSTART, RLENGTH); exit } }' /proc/bus/input/devices; }
find_pad() {
    awk -v want="$PAD_NAME" '
        /^N: Name=/ { name = $0; sub(/^N: Name="/, "", name); sub(/"$/, "", name) }
        /^H: Handlers=/ && name == want { if (match($0, /event[0-9]+/)) { print substr($0, RSTART, RLENGTH); exit } }' /proc/bus/input/devices
}
tap_key() { python3 "$INJECT" key "/dev/input/$KBD" "$1" "${2:-80}" >/dev/null 2>&1; }

# The scripts below assume a live, in-gameplay session started by launch-stray-safe.sh.
require_ingame() {
    game_running || { log "FAILED: the game is not running; start it with launch-stray-safe.sh"; exit 1; }
    [ "$(probe_age)" -le 5 ] || { log "FAILED: the probe is $(probe_age)s old; UE4SS/StrayProbe is not alive in this session"; exit 1; }
    [ "$(probe ingame)" = "1" ] || { log "FAILED: not in gameplay (pawn=$(probe pawn) pc=$(probe pc) map=$(probe map))"; exit 1; }
    KBD=$(find_keyboard); [ -n "$KBD" ] || { log "FAILED: no sysrq-capable keyboard node"; exit 1; }
    PAD=$(find_pad)
}

crash_dir_new() { local d; d=$(ls -t "$CRASH_DIR" 2>/dev/null | head -n 1); [ -n "$d" ] && [ "$(mtime "$CRASH_DIR/$d")" -ge "$1" ] && echo "$d"; }
crash_message() { grep -oE "<ErrorMessage>[^<]*" "$CRASH_DIR/$1/CrashContext.runtime-xml" 2>/dev/null | head -n 1; }

# A UE4 fatal-error dialog keeps the process alive; Enter clears it (user-verified). Capture
# first if a screenshot path is set, then clear, then kill only if it will not go.
crash() {   # $1 = reason, $2 = since-epoch
    log "CRASH: $1"
    [ -n "${SHOT:-}" ] && bash "$TOOLS_DIR/screenshot-gamescope.sh" "${SHOT%.png}-crash.png" 4 >/dev/null 2>&1 && log "  dialog captured to ${SHOT%.png}-crash.png"
    {
        echo "VERDICT: CRASH: $1"
        echo "--- probe"; cat "$PROBE" 2>/dev/null
        for f in "$GAME_DIR/ue4ss/UE4SS.log" "$GAME_DIR/stray-dlss-plugin.log" "$GAME_DIR/ReShade.log"; do
            [ "$(mtime "$f")" -ge "$2" ] && { echo "--- $(basename "$f"), last 4"; tail -n 4 "$f"; }
        done
        echo "--- dmesg GPU"; dmesg -T --since "@$2" 2>/dev/null | grep -iE "xid|NVRM" | tail -n 3
    } > "$VERDICT"
    if game_running; then
        tap_key $KEY_ENTER
        for _ in $(seq 1 10); do game_running || break; sleep 1; done
        game_running && { log "  process survived Enter; killing it"; pkill -x Stray-Win64-Shi; }
    fi
    exit 3
}
# Call every tick of every wait: bails the instant a wait can no longer succeed.
tick_checks() {   # $1 = phase label, $2 = since-epoch
    game_running || crash "the game process is gone ($1)" "$2"
    local d; d=$(crash_dir_new "$2")
    [ -n "$d" ] && crash "UE4 wrote a crash dump ($1): $(crash_message "$d")" "$2"
    return 0
}
