#!/usr/bin/env python3
"""Print (and optionally assert) the PDB path a PE records in its debug directory.

WHY THIS EXISTS. A UE4 crash dump names a module by its FILENAME and prints a base and an
offset:

    main    0x00006ffff4720000 + 771f6

Until 2026-09-03 both C++ plugins here were deployed as `Mods/<Name>/dlls/main.dll`, so both
answered to `main` and a dump could not say which had crashed - working that out cost real time,
and symbolizing then only worked after the shipped `main.pdb` was renamed by hand to the basename
the DLL's debug directory recorded. Two things changed, and the first is the real fix:

1. The DLL ships under its own name. UE4SS does NOT require `main.dll`: at the pinned SHA
   68caddcf, `CppMod.cpp:24-35` tries `dlls/main.dll` first and falls back to
   `dlls/<ModName>.dll` ("dlls folder must contain either main.dll or {}"), where `<ModName>`
   is the mod DIRECTORY name verbatim (`UE4SSProgram.cpp:1422`, `path().stem()`) and is still
   intact at that point because `Mod::Mod` COPIES it (`Mod.cpp:45`, `m_mod_name(mod_name)`)
   rather than moving it. So `dlls/StrayDLSS.dll` under `Mods/StrayDLSS/` loads, and the dump
   says `StrayDLSS`.

   **`main.dll` still WINS whenever both files exist**, and it would win silently. Every install
   path must therefore delete a stale `dlls/main.dll` (and `dlls/main.pdb`) as part of
   installing, not merely write the new file beside it.

2. The PDB keeps the module's name too, because a symbolizer resolves symbols by looking for a
   file with EXACTLY the basename in the CodeView (RSDS) record next to the binary. Both plugin
   CMakeLists pass `/PDBALTPATH:<Name>.pdb` - the only lever that changes the recorded STRING,
   which by default is the linker's absolute build-machine path - and both workflows ship the
   PDB under that same name.

This script is the check that all of it agrees, run in CI against the built DLL and again
against the staged artifact.

    tools/pe_debug_dir.py <pe-file> [--expect StrayDLSS.pdb] [--expect-dll StrayDLSS.dll]

`--expect` compares the recorded PDB BASENAME and exits non-zero on a mismatch; it additionally
requires the recorded path to be a bare basename (no directory part), because an absolute
build-machine path is what sends a symbolizer looking on a machine that does not exist.
`--expect-dll` asserts the FILENAME of the PE being examined, so a regression to `main.dll`
fails the job that stages it.
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
    ap.add_argument("--expect-dll", help="required filename of the PE itself, e.g. StrayDLSS.dll")
    args = ap.parse_args()

    guid, recorded = read_codeview(args.pe)
    print(f"{args.pe}")
    print(f"  CodeView : {guid}")
    print(f"  PDB path : {recorded}")

    if args.expect_dll:
        # A UE4 crash dump names the module by this filename, so the deployed name IS the
        # module identity. `main.dll` loads too and takes precedence, which is exactly why a
        # regression to it has to fail here rather than on the box.
        actual = args.pe.replace("\\", "/").rsplit("/", 1)[-1]
        if actual != args.expect_dll:
            print(
                f"FAIL: this PE is named {actual!r}, expected {args.expect_dll!r}. A crash dump "
                f"names the module by its filename, so shipping it as anything else - `main.dll` "
                f"above all - throws away the identity this whole check exists to preserve.",
                file=sys.stderr,
            )
            return 1
        print(f"  OK: filename {actual!r}, which is the name a crash dump will print (minus .dll).")

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
