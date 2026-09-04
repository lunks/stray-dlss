#!/usr/bin/env python3
"""Turn a `DumpShaders=1` manifest into a census of the compute passes the game actually RAN.

Why this exists: this project keeps answering "does pass X still run?" by argument. It does not
have to. `[STRAYDLSS] DumpShaders=1` already writes one `dispatch 0x<hash>  XxYxZ` line for the
first three dispatches of every compute shader the game issues (src/shader_dump.cpp:103-118,
src/app/dlss_app.cpp:1040-1058) -- so one launch produces a complete list of what is in the
frame. tools/shaderlib_extract.py --find-hash already turns a live-measured fnv1a64 into the
owning .usf. This script is the join, so the readout is one command instead of hand-assembly.

Usage:

    # what dispatched, and how big
    python3 tools/dispatch_census.py stray-dlss-shaders/manifest.txt

    # ...and what each one IS (needs the global shader cache; see shaderlib_extract's docstring
    # for how to pull it out of the pak with tools/pakextract.py)
    python3 tools/dispatch_census.py stray-dlss-shaders/manifest.txt \\
        --cache /tmp/shaderlib/Engine_GlobalShaderCache-PCD3D_SM5.bin

    # just the comma list, to paste at --find-hash yourself
    python3 tools/dispatch_census.py stray-dlss-shaders/manifest.txt --comma

    python3 tools/dispatch_census.py --self-test

Reading the group counts: the TAA tile is 8x8, so a pass's rect is roughly groups*8. At Stray's
1920x1080 render rect, 240x135 groups is full render resolution and 120x68 is exactly half
(ceil(960/8)=120, ceil(540/8)=68) -- which is what a half-res SSR pass looks like under the
shipped r.SSR.HalfResSceneColor=1. That arithmetic is a hypothesis; --cache is the answer.

Only GLOBAL shaders can be named: material shaders (the base pass, for instance) are not in
GlobalShaderCache-PCD3D_SM5.bin and come back as NOT FOUND, which is information, not an error.
"""

import os
import re
import sys

# "  dispatch 0x0123456789abcdef  240x135x1"
_DISPATCH = re.compile(r'^\s*dispatch\s+0x([0-9a-fA-F]{1,16})\s+(\d+)x(\d+)x(\d+)\s*$')
# "0x0123456789abcdef      4096           0  cs_0123456789abcdef.dxbc"
_DUMPED = re.compile(r'^0x([0-9a-fA-F]{1,16})\s+(\d+)\s+(\d+)\s+\S+\s*$')


def parse_manifest(text):
    """-> (dispatched, dumped_only). `dispatched` maps hash -> sorted list of (x, y, z) group
    counts observed; `dumped_only` is the set of hashes that were created but never dispatched.
    Unrecognised and comment lines are ignored: the manifest's own header is comments, and a
    session killed mid-frame can leave a torn last line."""
    dispatched = {}
    dumped = set()
    for line in text.splitlines():
        m = _DISPATCH.match(line)
        if m is not None:
            h = int(m.group(1), 16)
            dims = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
            seen = dispatched.setdefault(h, [])
            if dims not in seen:
                seen.append(dims)
            continue
        m = _DUMPED.match(line)
        if m is not None:
            dumped.add(int(m.group(1), 16))
    for h in dispatched:
        dispatched[h].sort()
    return dispatched, dumped - set(dispatched)


def format_table(dispatched, dumped_only):
    lines = []
    lines.append(f"{len(dispatched)} distinct compute shaders DISPATCHED "
                 f"({len(dumped_only)} more created but never dispatched)")
    lines.append("")
    lines.append("hash                  group counts observed")
    for h in sorted(dispatched, key=lambda k: (-dispatched[k][-1][0] * dispatched[k][-1][1], k)):
        dims = "  ".join(f"{x}x{y}x{z}" for (x, y, z) in dispatched[h])
        lines.append(f"0x{h:016x}    {dims}")
    return "\n".join(lines)


def name_them(cache_path, hashes):
    """Delegate to shaderlib_extract.find_hashes, which prints its own report. Imported rather
    than shelled out so a missing/renamed sibling fails here with a clear message."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    try:
        import shaderlib_extract
    except ImportError as ex:
        print(f"cannot import shaderlib_extract.py from this tools/ directory: {ex}",
              file=sys.stderr)
        return 3
    return shaderlib_extract.find_hashes(cache_path, set(hashes))


def self_test():
    sample = (
        "# stray-dlss compute shader dump\n"
        "# hash                 bytes    dispatched  file\n"
        "0x0000000000000aaa      4096           0  cs_0000000000000aaa.dxbc\n"
        "  dispatch 0x0000000000000aaa  240x135x1\n"
        "  dispatch 0x0000000000000aaa  240x135x1\n"
        "0x0000000000000bbb      2048           0  cs_0000000000000bbb.dxbc\n"
        "  dispatch 0x0000000000000bbb  120x68x1\n"
        "0x0000000000000ccc      1024           0  cs_0000000000000ccc.dxbc\n"
        "# 3 distinct compute shaders dumped\n"
    )
    dispatched, dumped_only = parse_manifest(sample)
    assert set(dispatched) == {0xaaa, 0xbbb}, dispatched
    # Repeats collapse: the manifest writes up to three lines per hash.
    assert dispatched[0xaaa] == [(240, 135, 1)], dispatched[0xaaa]
    assert dispatched[0xbbb] == [(120, 68, 1)], dispatched[0xbbb]
    # Created but never dispatched is a distinct, reportable state.
    assert dumped_only == {0xccc}, dumped_only

    # A torn final line (session killed mid-write) must not throw.
    torn, _ = parse_manifest(sample + "  dispatch 0x000000000000")
    assert set(torn) == {0xaaa, 0xbbb}, torn

    # Two different rects for one hash are both kept, sorted.
    multi, _ = parse_manifest(
        "  dispatch 0x1  240x135x1\n  dispatch 0x1  120x68x1\n")
    assert multi[0x1] == [(120, 68, 1), (240, 135, 1)], multi[0x1]

    assert "0x0000000000000aaa" in format_table(dispatched, dumped_only)
    print("self-test OK")
    return 0


def main(argv):
    if '--self-test' in argv:
        return self_test()

    cache = None
    if '--cache' in argv:
        i = argv.index('--cache')
        try:
            cache = argv[i + 1]
        except IndexError:
            print("--cache needs a GlobalShaderCache-PCD3D_SM5.bin path", file=sys.stderr)
            return 2
        del argv[i:i + 2]

    comma = '--comma' in argv
    if comma:
        argv.remove('--comma')

    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2

    try:
        with open(argv[0], 'r', encoding='utf-8', errors='replace') as f:
            text = f.read()
    except OSError as ex:
        print(f"cannot read manifest: {ex}", file=sys.stderr)
        return 2

    dispatched, dumped_only = parse_manifest(text)
    if not dispatched:
        print("no `dispatch` lines in that manifest -- was DumpShaders=1 set, and did the "
              "session reach gameplay?", file=sys.stderr)
        return 4

    if comma:
        print(",".join(f"0x{h:016x}" for h in sorted(dispatched)))
        return 0

    print(format_table(dispatched, dumped_only))
    if cache is None:
        print("\n(pass --cache <GlobalShaderCache-PCD3D_SM5.bin> to name these)")
        return 0
    print("\n--- naming, via shaderlib_extract.find_hashes ---")
    return name_them(cache, dispatched.keys())


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
