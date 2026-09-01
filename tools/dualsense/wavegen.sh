#!/bin/bash
# Convert the game's VIBE assets into stereo float32 waveforms for the DualSense coils.
#
# These are NOT envelopes. The coils take the waveform itself once the controller is put
# into haptic mode (valid_flag0 = 0xFC); the earlier RMS-envelope approach existed only
# because we were driving the two-byte motor-emulation API instead.
#
# The assets are stereo 48 kHz because the DualSense has TWO coils, one per grip - left
# channel drives the left grip, right drives the right. 48 kHz float32 is the WASAPI
# endpoint's own mix format, so nothing is converted at playback time.
set -e
WORK="${WORK:-/tmp/scepad}"
GAME="${GAME:-/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64}"
mkdir -p "$WORK/hap" "$GAME/haptic"
n=0
for f in "$WORK"/vibe/ogg/*.ogg; do
  [ -f "$f" ] || continue
  b=$(basename "$f" .ogg)
  ffmpeg -v error -y -i "$f" -ar 48000 -ac 2 -f f32le "$WORK/hap/$b.f32" 2>/dev/null && n=$((n+1))
done
cp "$WORK"/hap/*.f32 "$GAME/haptic/"
chown -R "${GAME_USER:-deck}:${GAME_USER:-deck}" "$GAME/haptic" 2>/dev/null || true
echo "installed $n stereo haptic waveforms to $GAME/haptic"
