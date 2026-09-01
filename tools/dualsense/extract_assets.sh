#!/bin/bash
# Rebuild the controller sound library: pak -> Oodle-decompress -> carve Ogg.
# /tmp does not survive reboots on this box, so this has to be repeatable.
#
# Selection is by SOUND CLASS, pak-wide - never by folder. A folder-scoped pass over
# Sound/SFX/controllers/ silently missed two coil assets that live elsewhere
# (character/sentinel/TazerElectricity_VIBE, gpe/window/window_oneWay_squeak_VIBE).
# Measured pak-wide: 4 SoundWaves on SCLASS_controller (speaker), 66 on
# SCLASS_controllerVibration (coils).
set -e

# Paths are overridable; the defaults match the dev box.
#   WORK  scratch dir for intermediate files
#   GAME  the game's Win64 binaries dir (where libScePad.dll and vibe/ live)
WORK="${WORK:-/tmp/scepad}"
GAME="${GAME:-/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64}"
mkdir -p "$WORK" && cd "$WORK"

if [ ! -x "$WORK/oozraw" ]; then
  echo "building oozraw (open-source Kraken decompressor)..."
  rm -rf ooz && timeout 180 git clone --depth 1 -q https://github.com/rarten/ooz.git ooz
  cd ooz && cp "$WORK/oozraw.cpp" .
  for f in kraken bitknit lzna compress compr_entropy compr_kraken compr_leviathan \
           compr_match_finder compr_mermaid compr_multiarray compr_tans; do
    g++ -O2 -std=c++14 -w -msse4.1 -Dmain=ooz_cli_main -c $f.cpp -o $f.o
  done
  g++ -O2 -std=c++14 -w -msse4.1 -c oozraw.cpp -o oozraw.o
  g++ -o "$WORK/oozraw" oozraw.o kraken.o bitknit.o lzna.o compress.o compr_entropy.o \
      compr_kraken.o compr_leviathan.o compr_match_finder.o compr_mermaid.o \
      compr_multiarray.o compr_tans.o
  cd "$WORK"
fi
test -x "$WORK/oozraw" && echo "oozraw OK"

PAK="${PAK:-$(find "$(dirname "$(dirname "$(dirname "$GAME")")")" -name "*.pak" | head -1)}"

# Decompress one raw pak entry into $2. Method 0 entries are stored, not compressed,
# and oodle_unblock does not handle the zero-block case, so copy those straight through.
unblock() {
  local j="$1" out="$2" b="${1%.json}"
  [ -f "${b}.raw" ] || return 1
  if [ "$(python3 -c "import json;print(json.load(open('$j'))['method'])")" = "0" ]; then
    cp "${b}.raw" "$out"
  else
    python3 oodle_unblock.py "$j" "${b}.raw" "$out" "$WORK/oozraw" >/dev/null 2>&1
  fi
}

# Pass 1: every Sound/ .uasset HEADER (small), so each one's SoundClass can be read.
rm -rf hdrraw hdrdec && mkdir -p hdrraw hdrdec
timeout 1200 python3 pakextract.py --raw "$PAK" hdrraw 'Sound/.*\.uasset$' 2>&1 | tail -1
for j in hdrraw/*.json; do unblock "$j" "hdrdec/$(basename "${j%.json}")" || true; done
echo "sound headers: $(ls hdrdec | wc -l)"

# Pass 2: the SoundWaves on the two controller classes. The JSON carries the TRUE pak
# path, so the asset's real name comes from there - not from guessing folder prefixes.
: > wanted_stems.txt
for j in hdrraw/*.json; do
  f="hdrdec/$(basename "${j%.json}")"; [ -f "$f" ] || continue
  if strings -a "$f" | grep -qxE 'SCLASS_controller|SCLASS_controllerVibration'; then
    p=$(python3 -c "import json;print(json.load(open('$j'))['path'])")
    case "$p" in *tools/settings/*) continue;; esac       # the class assets themselves
    basename "$p" .uasset >> wanted_stems.txt
  fi
done
sort -u -o wanted_stems.txt wanted_stems.txt
echo "controller-class SoundWaves: $(wc -l < wanted_stems.txt)"

PAT="Sound/.*/($(python3 -c "import re,sys;print('|'.join(re.escape(l.strip()) for l in open('wanted_stems.txt') if l.strip()))"))\\.(uasset|uexp|ubulk)$"
rm -rf raw dec vibe && mkdir -p raw dec vibe/ogg
timeout 900 python3 pakextract.py --raw "$PAK" raw "$PAT" 2>&1 | tail -1
ok=0
for j in raw/*.json; do unblock "$j" "dec/$(basename "${j%.json}")" && ok=$((ok+1)); done
echo "decompressed: $ok"

# Carve the Ogg out of each .uexp, named by the asset's real name.
n=0; nonogg=0
for j in raw/*.uexp.json; do
  f="dec/$(basename "${j%.json}")"; [ -f "$f" ] || continue
  name=$(basename "$(python3 -c "import json;print(json.load(open('$j'))['path'])")" .uexp)
  if python3 - "$f" "vibe/ogg/$name.ogg" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read(); i = d.find(b'OggS')
sys.exit(1) if i < 0 else open(sys.argv[2], 'wb').write(d[i:])
EOF
  then n=$((n+1)); else nonogg=$((nonogg+1)); echo "  not Ogg (streamed/ADPCM - use ue4_soundwave_extract.py): $name"; fi
done
echo "carved oggs: $n  (non-Ogg: $nonogg)"

# The game's own loop flag: UE4 only serialises bLooping when TRUE, so its presence in
# the header IS the flag. The asset overrides whatever a caller asks for.
: > "$GAME/haptic_loops.txt"
for j in raw/*.uasset.json; do
  f="dec/$(basename "${j%.json}")"; [ -f "$f" ] || continue
  if strings -a "$f" | grep -qx bLooping; then
    basename "$(python3 -c "import json;print(json.load(open('$j'))['path'])")" .uasset >> "$GAME/haptic_loops.txt"
  fi
done
sort -u -o "$GAME/haptic_loops.txt" "$GAME/haptic_loops.txt"
echo "looping assets: $(wc -l < "$GAME/haptic_loops.txt")"
chown -R "${GAME_USER:-deck}:${GAME_USER:-deck}" "$WORK" "$GAME/haptic_loops.txt" 2>/dev/null || true
