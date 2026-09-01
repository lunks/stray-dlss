#!/bin/bash
# Convert the game's controller assets into the float32 waveforms the plugin plays, and emit
# the game's own bLooping flags as loop lists.
#
#   $GAME/haptic/<name>.f32     stereo float32 @ 48 kHz  — the VIBE assets, for the two coils
#   $GAME/spk/<name>.f32        mono   float32 @ 48 kHz  — the _CONTROL assets, for the speaker
#   $GAME/haptic_loops.txt      coil assets whose SoundWave carries bLooping (23 of 66, pak-wide)
#   $GAME/spk_loops.txt         _CONTROL assets whose SoundWave carries bLooping
#
# These are NOT envelopes. The coils take the waveform itself once the controller is put into
# waveform mode (valid_flag0 = 0x00, docs/STRAY-DUALSENSE.md §12); the earlier RMS-envelope
# approach existed only because we were driving the two-byte motor-emulation API instead.
#
# The VIBE assets are stereo 48 kHz because the DualSense has TWO coils, one per grip — left
# channel drives the left grip. 48 kHz float32 is the WASAPI endpoint's own mix format, so
# nothing is converted at playback time.
#
# Loops: UE4 serialises bLooping only when TRUE, so its presence in the SoundWave's name
# table IS the flag. read_names() in ue4_soundwave_extract.py parses that table. The plugin
# consults these lists and the asset overrides whatever the caller asks for: looping every
# haptic made a 0.24 s bump buzz forever.
#
# Expects the layout extract_assets.sh leaves in $WORK: dec/<flattened>.uasset/.uexp and
# vibe/ogg/<name>.ogg. The purr (cat_purr_loop_01_CONTROL) is MS-ADPCM, not Ogg (§10), and
# comes out of ue4_soundwave_extract.py --decode instead.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${WORK:-/tmp/scepad}"
GAME="${GAME:-/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64}"

# read_names() and the ADPCM purr decode both come from this sibling; without it the loop-list
# step dies on an import mid-run, after files have already been copied. Fail before doing anything.
if [ ! -f "$HERE/ue4_soundwave_extract.py" ]; then
  echo "wavegen.sh: ue4_soundwave_extract.py must sit beside this script (looked in $HERE)" >&2
  exit 1
fi

# Regenerate from scratch. A stale $WORK/hap from an earlier run once carried three _CONTROL
# files into $GAME/haptic/ - the split below is only correct over a clean work dir.
rm -rf "$WORK/hap" "$WORK/spk"
mkdir -p "$WORK/hap" "$WORK/spk" "$GAME/haptic" "$GAME/spk"
rm -f "$GAME"/haptic/*_CONTROL.f32

nh=0; ns=0
for f in "$WORK"/vibe/ogg/*.ogg; do
  [ -f "$f" ] || continue
  b=$(basename "$f" .ogg)
  case "$b" in
    *_CONTROL)
      ffmpeg -v error -y -i "$f" -ar 48000 -ac 1 -f f32le "$WORK/spk/$b.f32" 2>/dev/null && ns=$((ns+1)) ;;
    *)
      ffmpeg -v error -y -i "$f" -ar 48000 -ac 2 -f f32le "$WORK/hap/$b.f32" 2>/dev/null && nh=$((nh+1)) ;;
  esac
done

# The ADPCM purr: carve + trim to TotalSamples via the extractor, then convert like the rest.
for u in "$WORK"/dec/*cat_purr_loop_01_CONTROL.uasset; do
  [ -f "$u" ] || continue
  base="${u%.uasset}"
  if python3 "$HERE/ue4_soundwave_extract.py" "$base" "$WORK/spk" --decode >/dev/null 2>&1; then
    pcm="$WORK/spk/$(basename "$base")_pcm.wav"
    [ -f "$pcm" ] && ffmpeg -v error -y -i "$pcm" -ar 48000 -ac 1 -f f32le \
        "$WORK/spk/cat_purr_loop_01_CONTROL.f32" 2>/dev/null && ns=$((ns+1))
  fi
done

cp "$WORK"/hap/*.f32 "$GAME/haptic/" 2>/dev/null || true
cp "$WORK"/spk/*_CONTROL.f32 "$GAME/spk/" 2>/dev/null || true

# Loop lists from the assets' own name tables. The asset's REAL name comes from the pak path
# recorded in raw/<entry>.uasset.json (extract_assets.sh selects by sound class, pak-wide, so
# the folder is not a reliable family marker); the family is the _CONTROL suffix. Split into
# the two files the plugin reads. extract_assets.sh also writes a combined haptic_loops.txt,
# which the plugin accepts for the speaker when spk_loops.txt is absent.
python3 - "$HERE" "$WORK" "$GAME" <<'PY'
import glob, json, os, sys
here, work, game = sys.argv[1:4]
sys.path.insert(0, here)
from ue4_soundwave_extract import read_names
hap, spk = [], []
for j in sorted(glob.glob(os.path.join(work, 'raw', '*.uasset.json'))):
    dec = os.path.join(work, 'dec', os.path.basename(j)[:-5])
    if not os.path.isfile(dec): continue
    name = os.path.basename(json.load(open(j))['path'])[:-7]
    try:
        loops = 'bLooping' in read_names(open(dec, 'rb').read())
    except Exception as e:
        print('  skip %s: %s' % (name, e)); continue
    if not loops: continue
    (spk if name.endswith('_CONTROL') else hap).append(name)
for fn, rows in (('haptic_loops.txt', sorted(set(hap))), ('spk_loops.txt', sorted(set(spk)))):
    open(os.path.join(game, fn), 'w').write('\n'.join(rows) + ('\n' if rows else ''))
    print('  %s: %d looping asset(s)' % (fn, len(rows)))
PY

chown -R "${GAME_USER:-deck}:${GAME_USER:-deck}" "$GAME/haptic" "$GAME/spk" \
    "$GAME/haptic_loops.txt" "$GAME/spk_loops.txt" 2>/dev/null || true
echo "installed $nh stereo haptic waveforms to $GAME/haptic and $ns mono speaker assets to $GAME/spk"
