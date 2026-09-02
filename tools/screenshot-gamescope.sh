#!/usr/bin/env bash
# tools/screenshot-gamescope.sh [out.png] [type] — a screenshot through gamescope ITSELF, so it
# works with no ReShade in the process (the plugin-alone configuration, where the ReShade
# KEY_SYSRQ channel of tools/screenshot-stray.sh does not exist).
#
# Verified 2026-09-02 on the box: `gamescopectl screenshot <path> <type>` returns at once and
# gamescope's detached writer thread lands a 3840x2160 RGBA PNG within ~3 s. Types, from the
# gamescope-control protocol: 1 = base plane only (the game at its render resolution, no
# display colour management), 2 = game + overlays, 3 = full composition, 4 = the buffer shown
# on screen, 1:1. The compositor runs as `deck` and needs its runtime dir and display: on this
# box deck is uid 1001 and the socket is /run/user/1001/gamescope-0 — read both from Steam's
# live environment rather than assuming 1000/gamescope-0.
#
# Corrects CLAUDE.md §2.11, which recorded that gamescope's SIGUSR2 "produced no file": that
# path writes an AVIF to /tmp/gamescope_<date>.avif (TakeScreenshot(bAVIF=true)), so a search
# for a PNG found nothing. Run as root inside the container (pct exec 113 -- bash ...).
set -euo pipefail
OUT=${1:-/tmp/gamescope-$(date +%Y%m%d-%H%M%S).png}
TYPE=${2:-4}
SP=$(pgrep -x steam | head -n 1 || true)
env_of() { tr '\0' '\n' < "/proc/${SP:-self}/environ" 2>/dev/null | sed -n "s/^$1=//p" | head -n 1; }
RT=$(env_of XDG_RUNTIME_DIR); RT=${RT:-/run/user/$(id -u deck)}
GD=$(env_of GAMESCOPE_WAYLAND_DISPLAY); GD=${GD:-gamescope-0}
[ -S "$RT/$GD" ] || { echo "no gamescope socket at $RT/$GD" >&2; exit 1; }
rm -f "$OUT"
su deck -c "XDG_RUNTIME_DIR='$RT' GAMESCOPE_WAYLAND_DISPLAY='$GD' timeout 10 gamescopectl screenshot '$OUT' $TYPE" >/dev/null 2>&1
for _ in $(seq 1 20); do [ -s "$OUT" ] && break; sleep 0.5; done
[ -s "$OUT" ] || { echo "gamescope wrote nothing to $OUT (is the path writable by deck?)" >&2; exit 1; }
echo "$OUT"
