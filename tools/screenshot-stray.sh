#!/usr/bin/env bash
# Take a ReShade screenshot of the running game, and bring it back.
#
# This is the only way to SEE what the add-on produces without a human at the machine, and
# seeing it is the only proof that matters: everything else we can measure says the pass runs,
# not that the image is right.
#
# How it works (CLAUDE.md §2.11): ReShade's screenshot bind is KeyScreenshot=44, which is
# VK_SNAPSHOT, which is Linux KEY_SYSRQ=99. Writing input_event structs straight to the
# keyboard's /dev/input/eventN reaches input_inject_event() and every reader sees it — no
# uinput, no ydotool, and nothing has the node EVIOCGRAB'd. ReShade then writes a PNG into the
# game directory (SavePath=.\).
set -euo pipefail

HOST="${HOST:-root@192.168.0.210}"
CT="${CT:-113}"
GAME_DIR=/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64
OUT="${1:-/tmp/stray-shot.png}"

ssh -o ConnectTimeout=8 "$HOST" "pct exec $CT -- python3 - " <<'PY'
import glob, os, struct, time

GAME_DIR = "/run/media/deck/GamesLinux/SteamLibrary/steamapps/common/Stray/Hk_project/Binaries/Win64"
KEY_SYSRQ, EV_KEY, EV_SYN = 99, 0x01, 0x00

def keyboard_node():
    """The keyboard whose Handlers list includes `sysrq`.

    That is precisely the node the kernel routes SysRq through, and it is the real keyboard
    rather than the several other things that claim `kbd` — a Power Button, a PC Speaker and a
    USB audio device all do, and injecting into those reaches nothing."""
    name = None
    for line in open("/proc/bus/input/devices"):
        line = line.strip()
        if line.startswith("N: Name="):
            name = line.split("=", 1)[1].strip('"')
        elif line.startswith("H: Handlers=") and "sysrq" in line:
            for tok in line.split("=", 1)[1].split():
                if tok.startswith("event"):
                    return "/dev/input/" + tok, name
    return None

def pad_node():
    """Steam Input re-emits the DualSense as "Microsoft X-Box 360 pad 0"; that node is what the
    game reads, and its eventN is not stable (CLAUDE.md §2.11)."""
    name = None
    for line in open("/proc/bus/input/devices"):
        line = line.strip()
        if line.startswith("N: Name="):
            name = line.split("=", 1)[1].strip('"')
        elif line.startswith("H: Handlers=") and name == "Microsoft X-Box 360 pad 0":
            for tok in line.split("=", 1)[1].split():
                if tok.startswith("event"):
                    return "/dev/input/" + tok
    return None


def pan_camera(seconds=1.2):
    """Pan the camera with the right stick before the shot.

    A STATIC scene hides everything temporal: with the camera still, TAA's history is already
    converged, so suppressing the pass looks identical to leaving it in place. Any judgement
    about a temporal pass has to be made while the image is moving.
    """
    import os
    pad = pad_node()
    if not pad:
        print("no pad node; shot will be static")
        return
    EV_ABS, ABS_RX, EV_SYN = 0x03, 0x03, 0x00
    with open(pad, "wb") as f:
        deadline = time.time() + seconds
        while time.time() < deadline:
            f.write(struct.pack("llHHi", 0, 0, EV_ABS, ABS_RX, 22000))
            f.write(struct.pack("llHHi", 0, 0, EV_SYN, 0, 0))
            f.flush()
            time.sleep(0.05)
        f.write(struct.pack("llHHi", 0, 0, EV_ABS, ABS_RX, 0))
        f.write(struct.pack("llHHi", 0, 0, EV_SYN, 0, 0))
        f.flush()
    print(f"panned the camera for {seconds}s via {pad}")


if os.environ.get("PAN", "1") == "1":
    pan_camera()

kb = keyboard_node()
if not kb:
    raise SystemExit("no sysrq-capable keyboard node found")
dev, name = kb
print(f"keyboard: {dev} ({name})")

before = set(glob.glob(os.path.join(GAME_DIR, "*.png")))

def emit(f, typ, code, val):
    # struct input_event { struct timeval time; __u16 type, code; __s32 value; }
    f.write(struct.pack("llHHi", 0, 0, typ, code, val))
    f.flush()

with open(dev, "wb") as f:
    emit(f, EV_KEY, KEY_SYSRQ, 1)
    emit(f, EV_SYN, 0, 0)
    time.sleep(0.05)
    emit(f, EV_KEY, KEY_SYSRQ, 0)
    emit(f, EV_SYN, 0, 0)

for _ in range(40):
    time.sleep(0.5)
    new = set(glob.glob(os.path.join(GAME_DIR, "*.png"))) - before
    if new:
        shot = max(new, key=os.path.getmtime)
        print("SHOT:" + shot)
        break
else:
    print("no new PNG appeared")
PY
