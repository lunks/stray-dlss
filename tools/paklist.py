#!/usr/bin/env python3
"""List a UE4 pak (version >= 10, path-hash + full-directory index).

Reads only the two index blobs, not the 5.3 GiB of data.
"""
import struct, sys, re

PATH = sys.argv[1]
PATTERN = re.compile(sys.argv[2], re.I) if len(sys.argv) > 2 else None
MAGIC = 0x5A6F12E1

def fstring(buf, off):
    """UE FString: int32 length; negative means UTF-16."""
    n = struct.unpack_from('<i', buf, off)[0]
    off += 4
    if n == 0:
        return '', off
    if n < 0:
        s = buf[off:off + (-n) * 2].decode('utf-16-le', 'replace').rstrip('\x00')
        return s, off + (-n) * 2
    s = buf[off:off + n].decode('latin1', 'replace').rstrip('\x00')
    return s, off + n

with open(PATH, 'rb') as f:
    f.seek(0, 2); size = f.tell()
    f.seek(size - 512); tail = f.read(512)
    pos = max(i for i in range(len(tail) - 4) if struct.unpack_from('<I', tail, i)[0] == MAGIC)
    index_offset, index_size = struct.unpack_from('<QQ', tail, pos + 8)

    f.seek(index_offset); idx = f.read(index_size)

    off = 0
    mount, off = fstring(idx, off)
    num_entries = struct.unpack_from('<i', idx, off)[0]; off += 4
    off += 8                                        # PathHashSeed
    has_path_hash = struct.unpack_from('<i', idx, off)[0]; off += 4
    if has_path_hash:
        off += 8 + 8 + 20                           # offset, size, hash
    has_full_dir = struct.unpack_from('<i', idx, off)[0]; off += 4
    if not has_full_dir:
        print("no full directory index — filenames unavailable"); sys.exit(1)
    fdi_offset, fdi_size = struct.unpack_from('<qq', idx, off); off += 16
    off += 20

    print(f"mount={mount} entries={num_entries} fdi_offset={fdi_offset} fdi_size={fdi_size}", file=sys.stderr)

    f.seek(fdi_offset); fdi = f.read(fdi_size)

o = 0
num_dirs = struct.unpack_from('<i', fdi, o)[0]; o += 4
total = 0
for _ in range(num_dirs):
    dirname, o = fstring(fdi, o)
    num_files = struct.unpack_from('<i', fdi, o)[0]; o += 4
    for _ in range(num_files):
        fname, o = fstring(fdi, o)
        o += 4                                      # encoded-entry offset
        full = dirname + fname
        total += 1
        if PATTERN is None or PATTERN.search(full):
            print(full)
print(f"# scanned {total} files in {num_dirs} directories", file=sys.stderr)
