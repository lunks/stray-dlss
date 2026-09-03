#!/usr/bin/env python3
"""Index-only census of a UE4 pak: path, method, sizes, blocks. Reads NO data section."""
import struct, sys, re

PAK = sys.argv[1]
PATTERN = re.compile(sys.argv[2], re.I) if len(sys.argv) > 2 else None
MAGIC = 0x5A6F12E1

def fstring(buf, off):
    n = struct.unpack_from('<i', buf, off)[0]; off += 4
    if n == 0: return '', off
    if n < 0:
        return buf[off:off+(-n)*2].decode('utf-16-le','replace').rstrip('\x00'), off+(-n)*2
    return buf[off:off+n].decode('latin1','replace').rstrip('\x00'), off+n

def decode_entry(buf, off):
    v = struct.unpack_from('<I', buf, off)[0]; off += 4
    e = {}
    e['method'] = (v >> 23) & 0x3F
    e['encrypted'] = bool(v & (1 << 22))
    nblocks = (v >> 6) & 0xFFFF
    e['block_size'] = 0
    if nblocks > 0:
        bs = v & 0x3F
        if bs == 0x3F: e['block_size'] = struct.unpack_from('<I', buf, off)[0]; off += 4
        else:          e['block_size'] = bs << 11
    if v & (1 << 31): e['offset'] = struct.unpack_from('<I', buf, off)[0]; off += 4
    else:             e['offset'] = struct.unpack_from('<Q', buf, off)[0]; off += 8
    if v & (1 << 30): e['usize'] = struct.unpack_from('<I', buf, off)[0]; off += 4
    else:             e['usize'] = struct.unpack_from('<Q', buf, off)[0]; off += 8
    if e['method'] != 0:
        if v & (1 << 29): e['size'] = struct.unpack_from('<I', buf, off)[0]; off += 4
        else:             e['size'] = struct.unpack_from('<Q', buf, off)[0]; off += 8
    else:
        e['size'] = e['usize']
    blocks = []
    if nblocks == 1 and not e['encrypted']:
        blocks = [(0, e['size'])]
    elif nblocks > 0:
        start = 0
        for _ in range(nblocks):
            bsz = struct.unpack_from('<I', buf, off)[0]; off += 4
            blocks.append((start, bsz)); start += bsz
    e['blocks'] = blocks
    return e, off

with open(PAK, 'rb') as f:
    f.seek(0, 2); size = f.tell()
    f.seek(size - 512); tail = f.read(512)
    pos = max(i for i in range(len(tail)-4) if struct.unpack_from('<I', tail, i)[0] == MAGIC)
    index_offset, index_size = struct.unpack_from('<QQ', tail, pos + 8)
    f.seek(index_offset); idx = f.read(index_size)
    o = 0
    mount, o = fstring(idx, o)
    o += 4 + 8
    if struct.unpack_from('<i', idx, o)[0]: o += 4 + 8 + 8 + 20
    else: o += 4
    o += 4
    fdi_offset, fdi_size = struct.unpack_from('<qq', idx, o); o += 16 + 20
    enc_size = struct.unpack_from('<i', idx, o)[0]; o += 4
    encoded = idx[o:o+enc_size]
    f.seek(fdi_offset); fdi = f.read(fdi_size)

p = 0
ndirs = struct.unpack_from('<i', fdi, p)[0]; p += 4
tot_u = tot_c = n = 0
for _ in range(ndirs):
    d, p = fstring(fdi, p)
    nf = struct.unpack_from('<i', fdi, p)[0]; p += 4
    for _ in range(nf):
        fn, p = fstring(fdi, p)
        eoff = struct.unpack_from('<i', fdi, p)[0]; p += 4
        full = d + fn
        if PATTERN and not PATTERN.search(full): continue
        if eoff < 0:
            print(f"NONENC\t{full}"); continue
        e, _ = decode_entry(encoded, eoff)
        n += 1; tot_u += e['usize']; tot_c += e['size']
        print(f"{e['method']}\t{e['usize']}\t{e['size']}\t{len(e['blocks'])}\t{e['block_size']}\t{full}")
print(f"# {n} entries  uncompressed={tot_u}  compressed={tot_c}", file=sys.stderr)
