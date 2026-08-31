#!/usr/bin/env python3
"""Decompress a pakextract --raw dump whose pak-level compression is Oodle.

Usage: oodle_unblock.py <entry.json> <entry.raw> <out> <path-to-oozraw>

Each pak compression block is an independent Oodle stream of block_size uncompressed bytes
(the last block is whatever remains). oozraw is the trimmed ooz (github.com/powzix/ooz)
front-end built by this project: `oozraw <uncompressed_size> < block > out`.
"""
import json, subprocess, sys

meta_path, raw_path, out_path, oozraw = sys.argv[1:5]
meta = json.load(open(meta_path))
raw = open(raw_path, "rb").read()
usize, bs = meta["usize"], meta["block_size"] or meta["usize"]
out = bytearray()
for i, (start, size) in enumerate(meta["blocks"]):
    want = min(bs, usize - len(out))
    block = raw[start : start + size]
    if size == want:  # stored, not compressed (UE4 stores blocks that do not shrink)
        out += block
        continue
    r = subprocess.run([oozraw, str(want)], input=block, capture_output=True)
    if r.returncode != 0 or len(r.stdout) != want:
        sys.exit(f"block {i}: oozraw failed ({r.returncode}): {r.stderr.decode()[:200]}")
    out += r.stdout
if len(out) != usize:
    sys.exit(f"reassembled {len(out)} != usize {usize}")
open(out_path, "wb").write(out)
print(f"{out_path}: {usize} bytes from {len(meta['blocks'])} blocks")
