#!/usr/bin/env python3
"""Inject input events straight into a /dev/input/eventN node.

Writing `input_event` structs to the node reaches `input_inject_event()`, so every reader
sees them — no uinput, ydotool or evemu, and nothing here EVIOCGRAB's the nodes we use
(CLAUDE.md 2.11). Runs on the SteamOS host as root.

    inject.py pad  <node> <code> <hold_ms>          tap a button (BTN_SOUTH = 304)
    inject.py key  <node> <code> <hold_ms>          tap a key
    inject.py axis <node> <code> <value> <hold_ms>  hold an absolute axis, then centre it
    inject.py hold <node> <hold_ms> <code> [<code>..] hold one or more keys together, release all
    inject.py traverse <node> [total_s] [swap_s]      the perf traverse: hold UP the whole run,
                                                       alternating LEFT/RIGHT every swap_s

`hold` presses every code, holds for hold_ms (re-emitting autorepeat so a multi-second hold is
not dropped by an input timeout), then releases them in reverse. Linux key codes: W=17 A=30
S=31 D=32 ENTER=28 ESC=1. Used for the deterministic perf traverse (W+A held) and menu drive.

`node` may be a full path or a bare `eventN`. Codes are Linux input codes, not Windows VKs:
ReShade's KeyScreenshot=44 (VK_SNAPSHOT) is KEY_SYSRQ=99 here.
"""

import struct
import sys
import time

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0

# `struct input_event` is {timeval tv; __u16 type; __u16 code; __s32 value}, and the kernel
# fills the timestamp in itself, so zeroes are fine. 'll' matches 64-bit time_t/suseconds_t.
_EVENT = struct.Struct("llHHi")


def _node_path(name):
    return name if name.startswith("/") else "/dev/input/" + name


def _emit(f, ev_type, code, value):
    f.write(_EVENT.pack(0, 0, ev_type, code, value))
    f.write(_EVENT.pack(0, 0, EV_SYN, SYN_REPORT, 0))


def main(argv):
    if len(argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2

    kind, node = argv[1], _node_path(argv[2])

    with open(node, "wb", buffering=0) as f:
        if kind in ("pad", "key"):
            code = int(argv[3])
            hold_ms = int(argv[4]) if len(argv) > 4 else 60
            _emit(f, EV_KEY, code, 1)
            time.sleep(hold_ms / 1000.0)
            _emit(f, EV_KEY, code, 0)
        elif kind == "axis":
            code, value = int(argv[3]), int(argv[4])
            hold_ms = int(argv[5]) if len(argv) > 5 else 1000
            _emit(f, EV_ABS, code, value)
            time.sleep(hold_ms / 1000.0)
            _emit(f, EV_ABS, code, 0)
        elif kind == "hold":
            hold_ms = int(argv[3])
            codes = [int(c) for c in argv[4:]]
            if not codes:
                print("hold needs at least one key code", file=sys.stderr)
                return 2
            for c in codes:            # press all
                _emit(f, EV_KEY, c, 1)
            # Re-emit autorepeat (value=2) every 50 ms so a multi-second hold survives any
            # input-timeout; the kernel keeps the key down regardless, but a real held key
            # repeats and some layers rely on it.
            end = time.time() + hold_ms / 1000.0
            while time.time() < end:
                time.sleep(0.05)
                for c in codes:
                    _emit(f, EV_KEY, c, 2)
            for c in reversed(codes):  # release all
                _emit(f, EV_KEY, c, 0)
        elif kind == "traverse":
            # The deterministic perf/stability traverse (user recipe 2026-09-02): hold ARROW UP
            # for the whole run; while it stays held, alternate ARROW LEFT and ARROW RIGHT, each
            # held for swap_s, until total_s. Codes: UP=103 LEFT=105 RIGHT=106 DOWN=108.
            KEY_UP, KEY_LEFT, KEY_RIGHT = 103, 105, 106
            total_s = float(argv[3]) if len(argv) > 3 else 15.0
            swap_s = float(argv[4]) if len(argv) > 4 else 3.0
            _emit(f, EV_KEY, KEY_UP, 1)                 # UP down for the whole run
            side = KEY_LEFT
            _emit(f, EV_KEY, side, 1)
            end = time.time() + total_s
            seg_end = time.time() + swap_s
            while time.time() < end:
                time.sleep(0.05)
                now = time.time()
                if now >= seg_end:                      # swap side
                    _emit(f, EV_KEY, side, 0)
                    side = KEY_RIGHT if side == KEY_LEFT else KEY_LEFT
                    _emit(f, EV_KEY, side, 1)
                    seg_end = now + swap_s
                else:
                    _emit(f, EV_KEY, KEY_UP, 2)          # autorepeat so the hold is not dropped
                    _emit(f, EV_KEY, side, 2)
            _emit(f, EV_KEY, side, 0)                    # release all
            _emit(f, EV_KEY, KEY_UP, 0)
        else:
            print("unknown kind: %s" % kind, file=sys.stderr)
            return 2

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
