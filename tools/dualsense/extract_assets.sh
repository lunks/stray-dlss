#!/bin/bash
# Rebuild the VIBE waveform library: pak -> Oodle-decompress -> carve Ogg.
# /tmp does not survive reboots on this box, so this has to be repeatable.
set -e

# Paths are overridable; the defaults match the dev box.
#   WORK  scratch dir for intermediate files
#   GAME  the game's Win64 binaries dir (where libScePad.dll and vibe/ live)
WORK="${WORK:-$WORK}"
GAME="${GAME:-/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64}"
cd "$WORK"
if [ ! -x $WORK/oozraw ]; then
  echo "building oozraw (open-source Kraken decompressor)..."
  rm -rf ooz && timeout 180 git clone --depth 1 -q https://github.com/rarten/ooz.git ooz
  cd ooz && cp $WORK/oozraw.cpp .
  for f in kraken bitknit lzna compress compr_entropy compr_kraken compr_leviathan \
           compr_match_finder compr_mermaid compr_multiarray compr_tans; do
    g++ -O2 -std=c++14 -w -msse4.1 -Dmain=ooz_cli_main -c $f.cpp -o $f.o
  done
  g++ -O2 -std=c++14 -w -msse4.1 -c oozraw.cpp -o oozraw.o
  g++ -o $WORK/oozraw oozraw.o kraken.o bitknit.o lzna.o compress.o compr_entropy.o \
      compr_kraken.o compr_leviathan.o compr_match_finder.o compr_mermaid.o \
      compr_multiarray.o compr_tans.o
  cd "$WORK"
fi
test -x $WORK/oozraw && echo "oozraw OK"

PAK="${PAK:-$(find "$(dirname "$(dirname "$(dirname "$GAME")")")" -name "*.pak" | head -1)}"
rm -rf raw dec vibe && mkdir -p raw dec vibe/ogg
timeout 900 python3 pakextract.py --raw "$PAK" raw "Sound/SFX/controllers/.*" 2>&1 | tail -2
ok=0
for j in raw/*.json; do
  b="${j%.json}"; [ -f "${b}.raw" ] || continue
  python3 oodle_unblock.py "$j" "${b}.raw" "dec/$(basename $b)" $WORK/oozraw >/dev/null 2>&1 && ok=$((ok+1))
done
echo "decompressed: $ok"
n=0
for f in dec/*.uexp; do
  name=$(basename "$f" .uexp | sed 's/.*_Vibrations_//; s/.*_sounds_//')
  python3 - "$f" "vibe/ogg/$name.ogg" <<'PY' && n=$((n+1))
import sys
d=open(sys.argv[1],'rb').read(); i=d.find(b'OggS')
sys.exit(1) if i<0 else open(sys.argv[2],'wb').write(d[i:])
PY
done
echo "carved oggs: $(ls vibe/ogg 2>/dev/null | wc -l)"
ls vibe/ogg | grep -iE "scratch|purr|hit" | head -6
chown -R "${GAME_USER:-deck}:${GAME_USER:-deck}" $WORK
