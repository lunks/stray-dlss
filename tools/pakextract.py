#!/usr/bin/env python3
"""Extract named files from a UE4 pak (version >= 10). Reads only what it needs.

Entry encoding follows FPakFile::DecodePakEntry / FPakEntry::Encode in UE 4.27's
IPlatformFilePak.cpp.
"""
import json, struct, sys, os, re, zlib

# --raw: write each matched entry's COMPRESSED payload plus a .json block map instead of
# decompressing. For Oodle-compressed entries (method name from the pak footer; Stray:
# 1=Zlib 2=Oodle) python cannot decode the blocks — dump them raw here and decompress
# offline with tools/oodle_unblock.py + an ooz build.
RAW_MODE = '--raw' in sys.argv
argv = [a for a in sys.argv if a != '--raw']
PAK, OUTDIR, PATTERN = argv[1], argv[2], re.compile(argv[3], re.I)
MAGIC = 0x5A6F12E1

def fstring(buf, off):
    n = struct.unpack_from('<i', buf, off)[0]; off += 4
    if n == 0: return '', off
    if n < 0:
        return buf[off:off+(-n)*2].decode('utf-16-le','replace').rstrip('\x00'), off+(-n)*2
    return buf[off:off+n].decode('latin1','replace').rstrip('\x00'), off+n

def decode_entry(buf, off):
    """Returns dict with offset/size/uncompressed_size/method/blocks.

    Field order verified against Stray's own pak by hex probe (2026-08-31): when the block-size
    code in the flags word is 0x3F, the explicit CompressionBlockSize uint32 comes IMMEDIATELY
    after the flags word — BEFORE Offset — not after Size. Decoding it late shifts every later
    field and only bites on large-block entries, which is why small zlib entries extracted fine.
    """
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
    o += 4 + 8                       # NumEntries, PathHashSeed
    if struct.unpack_from('<i', idx, o)[0]: o += 4 + 8 + 8 + 20
    else: o += 4
    has_fdi = struct.unpack_from('<i', idx, o)[0]; o += 4
    fdi_offset, fdi_size = struct.unpack_from('<qq', idx, o); o += 16 + 20
    enc_size = struct.unpack_from('<i', idx, o)[0]; o += 4
    encoded = idx[o:o+enc_size]

    f.seek(fdi_offset); fdi = f.read(fdi_size)

    p = 0
    ndirs = struct.unpack_from('<i', fdi, p)[0]; p += 4
    wanted = []
    for _ in range(ndirs):
        d, p = fstring(fdi, p)
        nf = struct.unpack_from('<i', fdi, p)[0]; p += 4
        for _ in range(nf):
            fn, p = fstring(fdi, p)
            eoff = struct.unpack_from('<i', fdi, p)[0]; p += 4
            full = d + fn
            if PATTERN.search(full):
                wanted.append((full, eoff))

    print(f"matched {len(wanted)} files", file=sys.stderr)
    os.makedirs(OUTDIR, exist_ok=True)
    for full, eoff in wanted:
        if eoff < 0:
            print(f"SKIP (non-encoded) {full}", file=sys.stderr); continue
        e, _ = decode_entry(encoded, eoff)
        # The data section repeats a serialised FPakEntry header before the payload.
        hdr = 8 + 8 + 8 + 4 + 20 + 1
        if e['method'] != 0:
            hdr += 4 + 4 + len(e['blocks']) * 16
        f.seek(e['offset'] + hdr)
        raw = f.read(e['size'])
        if RAW_MODE:
            base = os.path.join(OUTDIR, full.replace('/', '_'))
            open(base + '.raw', 'wb').write(raw)
            meta = {'path': full, 'method': e['method'], 'usize': e['usize'],
                    'size': e['size'], 'block_size': e['block_size'], 'blocks': e['blocks']}
            open(base + '.json', 'w').write(json.dumps(meta, indent=1))
            print(f"{e['size']:>9}  RAW {full} (method {e['method']}, {len(e['blocks'])} blocks)")
            continue
        if e['method'] == 0:
            data = raw
        else:
            try:
                data = b''.join(zlib.decompress(raw[s:s+n]) for s, n in e['blocks'])
            except Exception as ex:
                print(f"SKIP (method {e['method']}: {ex}) {full}", file=sys.stderr); continue
        out = os.path.join(OUTDIR, full.replace('/', '_'))
        open(out, 'wb').write(data)
        print(f"{len(data):>9}  {full}")
