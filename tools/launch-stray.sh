#!/usr/bin/env bash
# Launch Stray unattended inside CT113 and drive it past the main menu into gameplay.
#
# Runs as root under `pct exec 113`. Steam runs as user deck; the URL is handed to the running
# client. Two traps this script exists to handle (CLAUDE.md §2.10 / gotchas ledger):
#   * a leftover `reaper SteamLaunch AppId=1332010` makes Steam silently ignore launches;
#   * the Steam Input pad node ("Microsoft X-Box 360 pad 0") only exists while the game runs,
#     and its eventN number is not stable — discover it, never hardcode it.
# Menu driving: BTN_SOUTH presses on the pad node; extra presses are harmless (in-game they
# jump, in load screens they do nothing).
#
# Usage: launch-stray.sh [--timeout N]   (N seconds to babysit the process; default 700)
set -u
TIMEOUT=700
[ "${1:-}" = "--timeout" ] && TIMEOUT="${2:-700}"

pkill -f 'reaper SteamLaunch AppId=1332010' 2>/dev/null && sleep 3

sudo -u deck env HOME=/home/deck steam "steam://rungameid/1332010" >/dev/null 2>&1 &
sleep 1

for i in $(seq 1 60); do pgrep -x Stray-Win64-Shi >/dev/null && break; sleep 2; done
if ! pgrep -x Stray-Win64-Shi >/dev/null; then
	# Fallback: the client's URL pipe, which needs no environment at all.
	sudo -u deck bash -c 'echo "steam://rungameid/1332010" > /home/deck/.steam/steam.pipe' 2>/dev/null
	for i in $(seq 1 45); do pgrep -x Stray-Win64-Shi >/dev/null && break; sleep 2; done
fi
pgrep -x Stray-Win64-Shi >/dev/null || { echo "LAUNCH FAILED: no game process"; exit 1; }
echo "game process up"

pad=""
for i in $(seq 1 45); do
	pad=$(python3 - <<'PY'
name = None
try:
    for line in open('/proc/bus/input/devices'):
        line = line.strip()
        if line.startswith('N: Name='):
            name = line.split('=', 1)[1].strip('"')
        elif line.startswith('H: Handlers=') and name == 'Microsoft X-Box 360 pad 0':
            for tok in line.split():
                if tok.startswith('event'):
                    print('/dev/input/' + tok)
except OSError:
    pass
PY
)
	[ -n "$pad" ] && break
	sleep 2
done
echo "pad=${pad:-NOT FOUND}"

press() {
	python3 - "$1" <<'PY'
import struct, sys, time
f = open(sys.argv[1], 'wb', buffering=0)
def ev(t, c, v):
    f.write(struct.pack('llHHi', 0, 0, t, c, v))
ev(1, 304, 1); ev(0, 0, 0)
time.sleep(0.12)
ev(1, 304, 0); ev(0, 0, 0)
PY
}

if [ -n "$pad" ]; then
	sleep 18
	for gap in 4 4 12 4; do press "$pad"; sleep "$gap"; done
	press "$pad"
	echo "menu presses sent"
fi

END=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$END" ]; do
	pgrep -x Stray-Win64-Shi >/dev/null || { echo "game exited"; exit 0; }
	sleep 10
done
echo "timeout reached, game still running"
