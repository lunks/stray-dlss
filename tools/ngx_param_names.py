#!/usr/bin/env python3
"""Exact null-terminated parameter-name search over an NGX snippet or Streamline plugin.

CLAUDE.md §5: an NGX parameter block is an untyped string->value map with no validation, so a
name the runtime does not implement is silently ignored. Every DLSSG.* name we write must
therefore be confirmed against the binary that reads it. This is the instrument that does
it, kept as a script so the search is re-runnable against a DIFFERENT copy of the DLL (the box
carries SL 2.13's nvngx_dlssg.dll; this repo's contract was read from SL 2.12.0's).

    python3 tools/ngx_param_names.py nvngx_dlssg.dll [more.dll ...] [--prefix DLSSG.] [--check NAME ...]

Prints every NUL-terminated ASCII string starting with the prefix (and its UTF-16 twins), the
DLL's size and md5, its exports, and — with --check — a PRESENT/ABSENT verdict per name, exit
status 1 if any checked name is absent. "Present" means the exact bytes `\\0NAME\\0` occur; a
substring hit without the terminators is reported separately as "fragment" because a name
built by concatenation at run time would look like that.
"""
import argparse
import hashlib
import re
import struct
import sys


def exports(data):
    try:
        pe = struct.unpack_from("<I", data, 0x3C)[0]
        nsec = struct.unpack_from("<H", data, pe + 6)[0]
        opt = pe + 24
        magic = struct.unpack_from("<H", data, opt)[0]
        dd = opt + (112 if magic == 0x20B else 96)
        erva = struct.unpack_from("<I", data, dd)[0]
        secs = []
        off = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
        for i in range(nsec):
            _, vs, va, rs, ro = struct.unpack_from("<8sIIII", data, off + i * 40)
            secs.append((va, vs, ro, rs))

        def r2o(rva):
            for va, vs, ro, rs in secs:
                if va <= rva < va + max(vs, rs):
                    return rva - va + ro
            return None

        e = r2o(erva)
        if e is None:
            return []
        nn = struct.unpack_from("<I", data, e + 24)[0]
        names = struct.unpack_from("<I", data, e + 32)[0]
        out = []
        for i in range(nn):
            p = struct.unpack_from("<I", data, r2o(names) + i * 4)[0]
            o = r2o(p)
            out.append(data[o:data.index(b"\0", o)].decode())
        return out
    except Exception as exc:  # a packed or odd PE: report, do not crash
        return ["<export table unreadable: %s>" % exc]


def names_with_prefix(data, prefix):
    found = set()
    for m in re.finditer(rb"(?<=\x00)([\x20-\x7e]{2,160})\x00", data):
        s = m.group(1).decode()
        if s.startswith(prefix):
            found.add(s)
    for m in re.finditer(rb"((?:[\x20-\x7e]\x00){2,160})\x00\x00", data):
        s = m.group(1).decode("utf-16le")
        if s.startswith(prefix):
            found.add("U16LE:" + s)
    # UTF-16BE too: its absence was one of the two real holes this tool had, found by the
    # 2026-09-04 re-audit (docs/RESEARCH-DLSSNR-PARAM-AUDIT.md 1). The other hole -- names that
    # exist only inside a compressed container -- cannot be closed here; see that document 2 for
    # the fatbin extraction, and 3 for the call-site enumeration that closes the question outright.
    for m in re.finditer(rb"((?:\x00[\x20-\x7e]){2,160})\x00\x00", data):
        s = m.group(1).decode("utf-16be")
        if s.startswith(prefix):
            found.add("U16BE:" + s)
    return sorted(found)


def check_name(data, name):
    exact = (b"\0" + name.encode() + b"\0") in data
    fragment = (name.encode() in data
                or name.encode("utf-16le") in data
                or name.encode("utf-16be") in data)
    return exact, fragment


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dlls", nargs="+")
    ap.add_argument("--prefix", default="DLSSG.")
    ap.add_argument("--check", nargs="*", default=[])
    ap.add_argument("--no-list", action="store_true")
    args = ap.parse_args()
    missing = 0
    for path in args.dlls:
        data = open(path, "rb").read()
        print("=== %s  size=%d  md5=%s" % (path, len(data), hashlib.md5(data).hexdigest()))
        exp = exports(data)
        print("exports (%d): %s" % (len(exp), " ".join(exp)))
        if not args.no_list:
            found = names_with_prefix(data, args.prefix)
            print("%d name(s) with prefix %r:" % (len(found), args.prefix))
            for s in found:
                print("  " + s)
        for name in args.check:
            exact, fragment = check_name(data, name)
            verdict = "PRESENT" if exact else ("fragment-only" if fragment else "ABSENT")
            if not exact:
                missing += 1
            print("  check %-60s %s" % (name, verdict))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
