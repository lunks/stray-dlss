#!/bin/bash
# Regenerate the amplitude envelopes with ONE shared scale across the whole set.
# v1 normalised each file to its own peak, so a soft purr and a sharp impact both
# reached 255 - that discarded the relative loudness the game authored, and is why no
# single master gain could suit both. Two passes: measure every file, then scale them
# all by the same factor so their relative levels survive.
set -e

# Paths are overridable; the defaults match the dev box.
#   WORK  scratch dir for intermediate files
#   GAME  the game's Win64 binaries dir (where libScePad.dll and vibe/ live)
WORK="${WORK:-$WORK}"
GAME="${GAME:-/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64}"
cd "$WORK"
S="$GAME"
mkdir -p env2 rms
echo "pass 1: measuring $(ls vibe/ogg/*.ogg | wc -l) waveforms"
for o in vibe/ogg/*.ogg; do
  name=$(basename "$o" .ogg)
  [ -f "rms/$name.f32" ] && continue
  ffmpeg -y -loglevel error -i "$o" -ac 2 -ar 48000 -f s16le "$WORK/_tmp.raw" 2>/dev/null || continue
  python3 - "$name" <<'PY'
import struct, sys, math, array
name = sys.argv[1]
d = open('$WORK/_tmp.raw','rb').read()
n = len(d)//4
win = int(48000*0.005)
vals = array.array('f')
for s in range(0, n, win):
    e = min(s+win, n)
    if e <= s: break
    acc = 0.0
    for i in range(s, e):
        l, r = struct.unpack_from('<hh', d, i*4)
        m = (l + r) * 0.5
        acc += m*m
    vals.append(math.sqrt(acc/(e-s)))
open('$WORK/rms/%s.f32' % name, 'wb').write(vals.tobytes())
PY
done
echo "pass 2: one global scale"
python3 - <<'PY'
import array, glob, os
peak = 0.0
files = sorted(glob.glob('$WORK/rms/*.f32'))
for f in files:
    a = array.array('f'); a.frombytes(open(f,'rb').read())
    if len(a): peak = max(peak, max(a))
if peak <= 0: peak = 1.0
print("  global peak RMS = %.1f across %d files" % (peak, len(files)))
os.makedirs('$WORK/env2', exist_ok=True)
rows = []
for f in files:
    name = os.path.basename(f)[:-4]
    a = array.array('f'); a.frombytes(open(f,'rb').read())
    out = bytes(min(255, int(round(255.0 * (v/peak)))) for v in a)
    open('$WORK/env2/%s.env' % name, 'wb').write(out)
    rows.append((max(out) if out else 0, name, len(out)))
rows.sort(reverse=True)
print("  loudest:"); [print("    %3d  %-34s %5.2fs" % (p, n, l*0.005)) for p,n,l in rows[:5]]
print("  quietest:"); [print("    %3d  %-34s %5.2fs" % (p, n, l*0.005)) for p,n,l in rows[-5:]]
PY
cp env2/*.env "$S/vibe/"
chown -R "${GAME_USER:-deck}:${GAME_USER:-deck}" "$S/vibe" $WORK
echo "installed $(ls "$S/vibe" | wc -l) envelopes (relative levels preserved)"
