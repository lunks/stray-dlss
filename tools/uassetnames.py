#!/usr/bin/env python3
"""Dump the NameMap of a cooked UE4 .uasset (FPackageFileSummary -> FNameEntrySerialized)."""
import struct, sys

def rd_i32(b, o): return struct.unpack_from('<i', b, o)[0], o + 4
def rd_u32(b, o): return struct.unpack_from('<I', b, o)[0], o + 4

def rd_fstring(b, o):
    n, o = rd_i32(b, o)
    if n == 0: return '', o
    if n < 0:
        return b[o:o + (-n) * 2].decode('utf-16-le', 'replace').rstrip('\x00'), o + (-n) * 2
    return b[o:o + n].decode('latin1', 'replace').rstrip('\x00'), o + n

def names(path):
    b = open(path, 'rb').read()
    o = 0
    tag, o = rd_u32(b, o)
    if tag != 0x9E2A83C1:
        # tools/pakextract.py's pre-2026-09-03 method-0 bug shifts the payload 4 bytes early.
        if len(b) > 4 and struct.unpack_from('<I', b, 4)[0] == 0x9E2A83C1:
            b = b[4:]; o = 4
        else:
            raise ValueError(f'not a UE4 package: {b[:4].hex()}')
    legacy, o = rd_i32(b, o)
    if legacy != -4:
        _, o = rd_i32(b, o)                      # LegacyUE3Version
    _, o = rd_i32(b, o)                          # FileVersionUE4
    _, o = rd_i32(b, o)                          # FileVersionLicenseeUE4
    if legacy <= -2:
        n, o = rd_i32(b, o)                      # CustomVersions
        o += n * 20                              # FGuid(16) + int32
    _, o = rd_i32(b, o)                          # TotalHeaderSize
    _, o = rd_fstring(b, o)                      # FolderName
    _, o = rd_u32(b, o)                          # PackageFlags
    count, o = rd_i32(b, o)
    off, o = rd_i32(b, o)
    if not (0 < off < len(b)) or not (0 <= count < 200000):
        raise ValueError(f'bad name table: count={count} off={off} size={len(b)}')
    out, p = [], off
    for _ in range(count):
        s, p = rd_fstring(b, p)
        p += 4                                   # two uint16 hashes
        out.append(s)
    return out

if __name__ == '__main__':
    pat = sys.argv[2].lower() if len(sys.argv) > 2 else None
    for n in names(sys.argv[1]):
        if pat is None or pat in n.lower():
            print(n)
