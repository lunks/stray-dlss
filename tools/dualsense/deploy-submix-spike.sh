#!/bin/bash
# Deploy a StrayDualSense plugin build, and be able to undo it.
#
#   tools/dualsense/deploy-submix-spike.sh [GAMEDIR] [--dll PATH] [--revert]
#
# WHAT IT CHANGES, and why each one is necessary:
#
#  1. Kills the game. Everything below writes files the running process holds.
#  2. Restores libScePad_orig.dll over libScePad.dll, and sets `StrayTriggers : 0` in
#     ue4ss/Mods/mods.txt. THE SHIM AND THE PLUGIN BOTH DRIVE THE COILS AND THE HID MODE
#     BYTE. Two writers of valid_flag0 is exactly the fight that killed the adaptive triggers
#     twice (docs/STRAY-DUALSENSE.md §12), and two writers of the pad's WASAPI endpoint means
#     whichever opened last wins.
#  3. Sets `StrayDualSense : 1` in mods.txt and installs the plugin at
#     ue4ss/Mods/StrayDualSense/dlls/main.dll.
#  4. Writes ue4ss/Mods/StrayDualSense/dlls/StrayDualSense.ini with the keys 0.4.0 reads.
#     There are no modes any more: the engine's own submixes are the only source, the reroute
#     and the DebugPS5Haptic gate are always on, and Sony's API is the only speaker route.
#  5. chown deck:deck on everything it writes.
#
# It does NOT launch the game. Launch it yourself (tools/launch-stray-safe.sh), then read:
#
#     cat <GAMEDIR>/stray-dualsense-submix.txt        <- two lines, rewritten every second
#     grep -E 'submix|SUBMIX|pad audio' <GAMEDIR>/stray-dualsense.log
#
# --revert puts every one of those back: StrayDualSense : 0, StrayTriggers : 1, and the shim
# (libScePad_shim.dll, if that is what was there) reinstated. The previous mods.txt and ini are
# kept as .straydeploy.bak next to the originals.
set -uo pipefail

GAME_DEFAULT=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
GAME=""
DLL=""
REVERT=0

while [ $# -gt 0 ]; do
  case "$1" in
    --revert)  REVERT=1 ;;
    --dll)     DLL="${2:-}"; shift ;;
    --measure|--fallback|--strict|--reroute|--gate)
      echo "deploy-submix-spike: $1 was retired in 0.4.0 - there are no modes; the submix is the only source" >&2
      exit 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    -*)        echo "unknown option: $1" >&2; exit 2 ;;
    *)         GAME="$1" ;;
  esac
  shift
done
GAME="${GAME:-${GAMEDIR:-$GAME_DEFAULT}}"

say()  { printf '%s\n' "$*"; }
fail() { printf 'deploy-submix-spike: %s\n' "$*" >&2; exit 1; }

[ -d "$GAME" ] || fail "game dir not found: $GAME
  Pass it as the first argument. The mount has moved between sessions before
  (/mnt/GamesLinux -> /run/media/deck/GamesLinux), so a path that worked yesterday can be gone."

MODS="$GAME/ue4ss/Mods"
MODSTXT="$MODS/mods.txt"
[ -f "$MODSTXT" ] || fail "no $MODSTXT — is UE4SS installed in this game dir?"

# ---------------------------------------------------------------------------------------
# 1. The game must not be running.
# ---------------------------------------------------------------------------------------
# -x, not -f: `pgrep -f` matches this script's own command line and reports a game that is
# not there (CLAUDE.md, box operational traps).
if pgrep -x Stray-Win64-Shi >/dev/null 2>&1; then
  say "killing Stray-Win64-Shi..."
  pkill -x Stray-Win64-Shi
  for _ in $(seq 1 20); do
    pgrep -x Stray-Win64-Shi >/dev/null 2>&1 || break
    sleep 0.5
  done
  pgrep -x Stray-Win64-Shi >/dev/null 2>&1 && fail "the game is still running; refusing to write under it"
  say "  game stopped"
  # MEASURED 2026-09-03: launching while Steam's reaper for the killed chain is still alive
  # makes Steam ignore the launch (CLAUDE.md 2.10) and tools/launch-stray-safe.sh refuses with
  # "stale launch chain". Wait for it here so the next command can be the launcher.
  for _ in $(seq 1 40); do
    pgrep -f "13320[1]0" >/dev/null 2>&1 || break
    sleep 1
  done
  pgrep -f "13320[1]0" >/dev/null 2>&1 && say "  WARNING: Steam's reaper is still up; a launch now will be ignored" \
                                        || say "  launch chain reaped"
else
  say "game is not running"
fi

CHANGED=0
note() { CHANGED=$((CHANGED+1)); say "  CHANGED: $*"; }

# ---------------------------------------------------------------------------------------
# mods.txt: set one entry to one value, idempotently. UE4SS's format is `Name : 0|1`.
# ---------------------------------------------------------------------------------------
#
# awk rather than `sed -i`: the -i suffix syntax differs between GNU and BSD sed, and an
# in-place edit that silently does nothing is exactly the class of bug this script must not
# have — the first version of it reported "added StrayDualSense" on every run while writing
# nothing at all.
set_mod() {
  local name="$1" value="$2" current tmp
  current=$(awk -F: -v n="$name" '
      { key = $1; gsub(/^[ \t]+|[ \t\r]+$/, "", key) }
      key == n { v = $2; gsub(/[ \t\r]/, "", v); print v; exit }' "$MODSTXT")
  if [ "$current" = "$value" ]; then
    say "  mods.txt: $name is already $value"
    return
  fi
  tmp="$MODSTXT.straydeploy.tmp"
  awk -v n="$name" -v v="$value" '
      { line = $0
        key = line; sub(/:.*/, "", key); gsub(/^[ \t]+|[ \t\r]+$/, "", key)
        if (key == n) { print n " : " v; found = 1; next }
        # Keep the new entry above Keybinds: UE4SS loads in file order and Keybinds is
        # conventionally last.
        if (key == "Keybinds" && !found) { print n " : " v; found = 1 }
        print line }
      END { if (!found) print n " : " v }' "$MODSTXT" > "$tmp" || { rm -f "$tmp"; fail "could not rewrite $MODSTXT"; }
  [ -s "$tmp" ] || { rm -f "$tmp"; fail "rewriting $MODSTXT produced an empty file; refusing"; }
  cp -f "$MODSTXT" "$MODSTXT.straydeploy.bak"
  mv -f "$tmp" "$MODSTXT"
  if [ -n "$current" ]; then
    note "mods.txt: $name $current -> $value"
  else
    note "mods.txt: added $name : $value"
  fi
}

# ---------------------------------------------------------------------------------------
# libScePad: the shim and the plugin cannot coexist.
# ---------------------------------------------------------------------------------------
shim_off() {
  if [ -f "$GAME/libScePad_orig.dll" ]; then
    if cmp -s "$GAME/libScePad_orig.dll" "$GAME/libScePad.dll"; then
      say "  libScePad.dll is already the original"
    else
      # Keep whatever is there now so --revert can put it back.
      cp -f "$GAME/libScePad.dll" "$GAME/libScePad_shim.dll" 2>/dev/null || true
      cp -f "$GAME/libScePad_orig.dll" "$GAME/libScePad.dll" || fail "could not restore libScePad.dll"
      note "libScePad.dll <- libScePad_orig.dll (the shim was saved as libScePad_shim.dll)"
    fi
  else
    say "  no libScePad_orig.dll here, so the shim was never installed — nothing to restore"
  fi
}

shim_on() {
  if [ -f "$GAME/libScePad_shim.dll" ]; then
    cp -f "$GAME/libScePad_shim.dll" "$GAME/libScePad.dll" && note "libScePad.dll <- libScePad_shim.dll"
  else
    say "  no libScePad_shim.dll saved; leaving libScePad.dll alone"
  fi
}

# ---------------------------------------------------------------------------------------
# --revert
# ---------------------------------------------------------------------------------------
if [ "$REVERT" = 1 ]; then
  say "REVERTING the plugin deploy in $GAME"
  set_mod StrayDualSense 0
  set_mod StrayTriggers 1
  shim_on
  for d in "$MODS/StrayDualSense/dlls" "$MODS/StrayDualSense"; do
    if [ -f "$d/StrayDualSense.ini.straydeploy.bak" ]; then
      mv -f "$d/StrayDualSense.ini.straydeploy.bak" "$d/StrayDualSense.ini" \
        && note "restored the previous $d/StrayDualSense.ini"
    fi
  done
  chown -R deck:deck "$MODS" 2>/dev/null || true
  say ""
  say "$CHANGED change(s). The plugin DLL is left in place but disabled; the game was NOT launched."
  exit 0
fi

# ---------------------------------------------------------------------------------------
# 2. Find the DLL.
# ---------------------------------------------------------------------------------------
if [ -z "$DLL" ]; then
  for c in \
      "./main.dll" \
      "./StrayDualSense/dlls/main.dll" \
      "./stray-dualsense-plugin/StrayDualSense/dlls/main.dll" \
      "$HOME/Downloads/StrayDualSense/dlls/main.dll" \
      "$HOME/Downloads/stray-dualsense-plugin/StrayDualSense/dlls/main.dll" \
      "/tmp/stray-dualsense-plugin/StrayDualSense/dlls/main.dll"; do
    [ -f "$c" ] && { DLL="$c"; break; }
  done
fi
[ -n "$DLL" ] && [ -f "$DLL" ] || fail "no plugin DLL found. Download the 'stray-dualsense-plugin'
  artifact from the StrayDualSense CI run and pass it:
      $0 --dll /path/to/StrayDualSense/dlls/main.dll"

say "deploying StrayDualSense"
say "  game dir : $GAME"
say "  dll      : $DLL  ($(stat -c %s "$DLL" 2>/dev/null || stat -f %z "$DLL") bytes)"
say ""

# ---------------------------------------------------------------------------------------
# 3. Stand down the shim, stand up the plugin.
# ---------------------------------------------------------------------------------------
shim_off
set_mod StrayTriggers 0
set_mod StrayDualSense 1

mkdir -p "$MODS/StrayDualSense/dlls"
if cmp -s "$DLL" "$MODS/StrayDualSense/dlls/main.dll"; then
  say "  main.dll is already this build"
else
  cp -f "$DLL" "$MODS/StrayDualSense/dlls/main.dll" || fail "could not install main.dll"
  note "installed $MODS/StrayDualSense/dlls/main.dll"
fi
PDB="${DLL%.dll}.pdb"
[ -f "$PDB" ] && cp -f "$PDB" "$MODS/StrayDualSense/dlls/main.pdb"

# ---------------------------------------------------------------------------------------
# 4. The ini, NEXT TO THE DLL.
#
# MEASURED 2026-09-02: writing it to the mod ROOT made the first live run silently use the
# defaults — the plugin searched the DLL's own directory and the game directory, and the mod
# root was neither. The plugin now searches the mod root too, but a deploy must not depend on
# the build being new enough to do that, so it goes where every version looks. A stale copy in
# the mod root would be shadowed by this one; it is removed if present.
# ---------------------------------------------------------------------------------------
INI="$MODS/StrayDualSense/dlls/StrayDualSense.ini"
INI_NEW="$INI.straydeploy.tmp"
cat > "$INI_NEW" <<INI_EOF
; Written by tools/dualsense/deploy-submix-spike.sh for StrayDualSense 0.4.0.
;
; Every value is the plugin's own default; the file exists so a knob can be flipped on the
; box without a rebuild. mods/StrayDualSense/StrayDualSense.ini documents each one.
[StrayDualSense]
Enabled = 1
LogLevel = Info

Triggers = 1
Haptics = 1
Speaker = 1
; 1.0: the engine's own SBFX_Boost is already in the tapped samples. A/B only.
SpeakerGain = 1.0

; Sony's routing call, the shim's measured-working values (docs/STRAY-DUALSENSE.md §16).
PadSpeakerPath = 3
PadSpeakerGain = 80

; The two masters and the parent they are re-parented under (docs §14/§16). Both roots are
; dead on PC; the reroute is always on and the watchdog re-submits it after a level load.
SubmixPath = /Game/Sound/tools/settings/Submix_vibrationMaster.Submix_vibrationMaster
SubmixSpeakerPath = /Game/Sound/tools/settings/Submix_controllerMaster.Submix_controllerMaster
SubmixRerouteParent = /Game/Sound/tools/settings/Submix_unused.Submix_unused
SubmixRegisterSoundSubmixSlot = 14
SubmixRegisterSlot = 16
SubmixRerouteWatchdogSeconds = 5.0

; PS glyphs on the prompts (the game sees an X360 pad and draws Xbox ones otherwise).
Glyphs = ps5

SubmixProbeMaster = 1
SubmixGain = 1.0
SubmixQueueAheadMs = 40
SubmixRingMs = 250
SubmixStatusSeconds = 1.0
SubmixWatchSeconds = 3.0
SubmixLiveThreshold = 0.0001
SubmixWarnSeconds = 10
SubmixStatusFile = stray-dualsense-submix.txt
INI_EOF
if cmp -s "$INI_NEW" "$INI"; then
  rm -f "$INI_NEW"
  say "  StrayDualSense.ini is already this configuration"
else
  # Back up only the PRE-DEPLOY ini, once, so a second run cannot overwrite the thing --revert
  # is supposed to restore.
  [ -f "$INI" ] && [ ! -f "$INI.straydeploy.bak" ] && cp -f "$INI" "$INI.straydeploy.bak"
  mv -f "$INI_NEW" "$INI"
  note "wrote $INI"
fi
# A leftover from an earlier deploy would sit in the search path behind this one and is pure
# confusion in a log; take it out of the way rather than leaving two files disagreeing.
if [ -f "$MODS/StrayDualSense/StrayDualSense.ini" ]; then
  mv -f "$MODS/StrayDualSense/StrayDualSense.ini" \
        "$MODS/StrayDualSense/StrayDualSense.ini.superseded" \
    && note "moved the stale mod-root ini aside (the plugin reads the one beside the DLL)"
fi

chown -R deck:deck "$MODS" 2>/dev/null || true
chown deck:deck "$GAME/libScePad.dll" 2>/dev/null || true

say ""
say "$CHANGED change(s). The game was NOT launched."
say ""
say "mods.txt now reads:"
grep -E "^[[:space:]]*(StrayDualSense|StrayTriggers|StrayProbe|StrayFur|StrayConsole)[[:space:]]*:" "$MODSTXT" | sed 's/^/  /'
say ""
say "Then, with the game running:"
say "  cat $GAME/stray-dualsense-submix.txt"
say "  grep -E 'submix|SUBMIX|pad audio' $GAME/stray-dualsense.log | tail -40"
say ""
say "What to look for, in order:"
say "  0. every SUBMIX/STATUS line starts with 'COILS: ...' / 'SPEAKER: ...' - who drives each pair"
say "  1. 'submix: FAudioDevice ... found ...'                                the pointer is real"
say "  2. 'submix: REROUTE submitted'                                        the links were rebuilt"
say "  3. SUBMIX vibration|speaker ... cb=N (46.9/s) ch=8 rate=48000 ...     BOTH SUBTREES RENDER"
say "  4. 'submix watch [vibration|speaker] ... the engine MIXED it'         THE ENGINE MIXES"
say "  5. 'FIRST REAL SIGNAL on ... HANDOVER'                                the pad is on the mix"
say "  6. 'pad audio: ... SONY ACCEPTED the route'                           the speaker is routed"
say ""
say "Undo everything: $0 $GAME --revert"
