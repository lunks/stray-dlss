#!/usr/bin/env python3
"""Print (and optionally assert) the PDB path a PE records in its debug directory.

WHY THIS EXISTS. UE4SS hardcodes `Mods/<Name>/dlls/main.dll` (UE4SSProgram.cpp: both the
mods.txt and enabled.txt discovery loops skip anything that is not a directory, and CppMod
then opens `dlls/main.dll` inside), so EVERY C++ mod in this repository loads as a module
literally named `main`. A UE4 crash dump prints that module name and an offset:

    main    0x00006ffff4720000 + 771f6

which is ambiguous the moment two of our plugins are deployed. The only thing in the shipped
file that still names the module is the CodeView (RSDS) record in the debug directory - the
PDB path the linker stamped in. A symbolizer resolves symbols by looking for a file with
EXACTLY that basename next to the binary, so if the DLL says `StrayDLSS.pdb` and the artifact
ships `main.pdb`, symbolization silently finds nothing until somebody renames the file by
hand. That happened on 2026-09-03 and cost real time.

Both plugin CMakeLists therefore pass `/PDBALTPATH:<Name>.pdb` (which changes the recorded
string only, never the file on disk) and both workflows ship the PDB under that same name.
This script is the check that the two halves agree, run in CI against the built DLL.

    tools/pe_debug_dir.py <pe-file> [--expect StrayDLSS.pdb]

`--expect` compares the recorded BASENAME and exits non-zero on a mismatch; it additionally
requires the recorded path to be a bare basename (no directory part), because an absolute
build-machine path is what sends a symbolizer looking on a machine that does not exist.
"""

from __future__ import annotations

import argparse
import struct
import sys

IMAGE_DIRECTORY_ENTRY_DEBUG = 6
IMAGE_DEBUG_TYPE_CODEVIEW = 2


def _u16(b: bytes, o: int) -> int:
    return struct.unpack_from("<H", b, o)[0]


def _u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def read_codeview(path: str) -> tuple[str, str]:
    """Return (guid_age, pdb_path) from the PE's CodeView debug record."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:2] != b"MZ":
        raise SystemExit(f"{path}: not a PE (no MZ)")
    pe = _u32(data, 0x3C)
    if data[pe : pe + 4] != b"PE\0\0":
        raise SystemExit(f"{path}: not a PE (no PE\\0\\0 at e_lfanew)")

    n_sections = _u16(data, pe + 6)
    opt = pe + 24
    magic = _u16(data, opt)
    if magic == 0x20B:  # PE32+
        dir_off = opt + 112
    elif magic == 0x10B:  # PE32
        dir_off = opt + 96
    else:
        raise SystemExit(f"{path}: unknown optional header magic {magic:#x}")

    n_dirs = _u32(data, opt + (108 if magic == 0x20B else 92))
    if n_dirs <= IMAGE_DIRECTORY_ENTRY_DEBUG:
        raise SystemExit(f"{path}: no debug data directory")
    debug_rva = _u32(data, dir_off + IMAGE_DIRECTORY_ENTRY_DEBUG * 8)
    debug_size = _u32(data, dir_off + IMAGE_DIRECTORY_ENTRY_DEBUG * 8 + 4)
    if debug_rva == 0 or debug_size == 0:
        raise SystemExit(f"{path}: debug directory is empty - the DLL was linked without /DEBUG")

    # Section table follows the optional header.
    sec_off = opt + _u16(data, pe + 20)
    sections = []
    for i in range(n_sections):
        s = sec_off + i * 40
        sections.append((_u32(data, s + 12), _u32(data, s + 8), _u32(data, s + 20)))  # rva, vsize, raw

    def to_file_offset(rva: int) -> int:
        for va, vsize, raw in sections:
            if va <= rva < va + max(vsize, 1):
                return raw + (rva - va)
        raise SystemExit(f"{path}: RVA {rva:#x} is in no section")

    entries = debug_size // 28
    for i in range(entries):
        e = to_file_offset(debug_rva) + i * 28
        if _u32(data, e + 12) != IMAGE_DEBUG_TYPE_CODEVIEW:
            continue
        size = _u32(data, e + 16)
        raw = _u32(data, e + 24)
        cv = data[raw : raw + size]
        if cv[:4] != b"RSDS":
            continue
        d1, d2, d3 = struct.unpack_from("<IHH", cv, 4)
        d4 = cv[12:20]
        age = _u32(cv, 20)
        name = cv[24:].split(b"\0", 1)[0].decode("utf-8", "replace")
        guid = "%08X-%04X-%04X-%s-%s" % (d1, d2, d3, d4[:2].hex().upper(), d4[2:].hex().upper())
        return f"{{{guid}}} age {age}", name
    raise SystemExit(f"{path}: debug directory has no RSDS CodeView record")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pe")
    ap.add_argument("--expect", help="required PDB basename, e.g. StrayDLSS.pdb")
    args = ap.parse_args()

    guid, recorded = read_codeview(args.pe)
    print(f"{args.pe}")
    print(f"  CodeView : {guid}")
    print(f"  PDB path : {recorded}")

    if not args.expect:
        return 0

    base = recorded.replace("\\", "/").rsplit("/", 1)[-1]
    if base != args.expect:
        print(
            f"FAIL: the debug directory records {base!r}, expected {args.expect!r}. A symbolizer "
            f"resolves a crash offset by looking for that exact name next to the DLL, so the "
            f"artifact must ship the PDB under it.",
            file=sys.stderr,
        )
        return 1
    if base != recorded:
        print(
            f"FAIL: the debug directory records the full path {recorded!r}. That is a build-machine "
            f"path nobody else has; pass /PDBALTPATH:{args.expect} so the record is a bare basename "
            f"resolved next to the DLL.",
            file=sys.stderr,
        )
        return 1
    print(f"  OK: bare basename {base!r}, which is the name the artifact ships the PDB under.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
