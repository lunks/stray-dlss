#!/usr/bin/env python3
"""Inject input events straight into a /dev/input/eventN node.

Writing `input_event` structs to the node reaches `input_inject_event()`, so every reader
sees them — no uinput, ydotool or evemu, and nothing here EVIOCGRAB's the nodes we use
(CLAUDE.md 2.11). Runs on the SteamOS host as root.

    inject.py pad  <node> <code> <hold_ms>          tap a button (BTN_SOUTH = 304)
    inject.py key  <node> <code> <hold_ms>          tap a key
    inject.py axis <node> <code> <value> <hold_ms>  hold an absolute axis, then centre it

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
        else:
            print("unknown kind: %s" % kind, file=sys.stderr)
            return 2

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
