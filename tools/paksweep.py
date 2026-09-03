#!/usr/bin/env python3
"""Extract every matching entry's RAW payload, in PAK-OFFSET ORDER, as one concatenated blob.

Why offset order: the naive extractor walks the directory index, which scatters ~24k seeks over
a 5.65 GB file that a running game streams from. Sorted by offset the same bytes come off the
disk as a forward scan, which is what readahead and the idle IO class are good at.

Why one blob: 24k tiny files cost more in metadata than in data, and the consumer only wants to
decompress and grep them.

Output: <out>.bin (payloads back to back) and <out>.idx (TSV: path, method, usize, block_size,
blob_offset, size, blocks) — everything oozbatch needs.

Aborts if the game reappears, so a window that closes does not turn into IO contention.
"""
import struct, sys, os, re, subprocess

PAK, OUT, PATTERN = sys.argv[1], sys.argv[2], re.compile(sys.argv[3], re.I)
MAGIC = 0x5A6F12E1
CHECK_EVERY = 512

def game_running():
    return subprocess.run(['pgrep', '-x', 'Stray-Win64-Shi'],
                          capture_output=True).returncode == 0

def fstring(buf, off):
    n = struct.unpack_from('<i', buf, off)[0]; off += 4
    if n == 0: return '', off
    if n < 0:
        return buf[off:off+(-n)*2].decode('utf-16-le','replace').rstrip('\x00'), off+(-n)*2
    return buf[off:off+n].decode('latin1','replace').rstrip('\x00'), off+n

def decode_entry(buf, off):
    v = struct.unpack_from('<I', buf, off)[0]; off += 4
    e = {'method': (v >> 23) & 0x3F, 'encrypted': bool(v & (1 << 22))}
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

if game_running():
    sys.exit('REFUSING: Stray-Win64-Shi is running')

with open(PAK, 'rb') as f:
    f.seek(0, 2); size = f.tell()
    f.seek(size - 512); tail = f.read(512)
    pos = max(i for i in range(len(tail)-4) if struct.unpack_from('<I', tail, i)[0] == MAGIC)
    index_offset, index_size = struct.unpack_from('<QQ', tail, pos + 8)
    f.seek(index_offset); idx = f.read(index_size)
    o = 0
    _, o = fstring(idx, o)
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
    wanted = []
    for _ in range(ndirs):
        d, p = fstring(fdi, p)
        nf = struct.unpack_from('<i', fdi, p)[0]; p += 4
        for _ in range(nf):
            fn, p = fstring(fdi, p)
            eoff = struct.unpack_from('<i', fdi, p)[0]; p += 4
            full = d + fn
            if PATTERN.search(full) and eoff >= 0:
                e, _ = decode_entry(encoded, eoff)
                wanted.append((e['offset'], full, e))
    wanted.sort(key=lambda t: t[0])
    print(f"{len(wanted)} entries, {sum(e['size'] for _,_,e in wanted)} compressed bytes",
          file=sys.stderr)

    blob = open(OUT + '.bin', 'wb')
    index = open(OUT + '.idx', 'w')
    at = 0
    for i, (_, full, e) in enumerate(wanted):
        if i % CHECK_EVERY == 0 and game_running():
            sys.exit(f'ABORTED at {i}/{len(wanted)}: the game came back')
        # FPakEntry::Serialize: Offset Size UncompressedSize CompressionMethodIndex Hash
        # [blocks] bEncrypted CompressionBlockSize -- the last two unconditional.
        hdr = 8 + 8 + 8 + 4 + 20 + 1 + 4
        if e['method'] != 0:
            hdr += 4 + len(e['blocks']) * 16
        f.seek(e['offset'] + hdr)
        raw = f.read(e['size'])
        blob.write(raw)
        blocks = ','.join(f"{s}:{n}" for s, n in e['blocks']) or '0:0'
        index.write(f"{full}\t{e['method']}\t{e['usize']}\t{e['block_size']}\t{at}\t{e['size']}\t{blocks}\n")
        at += len(raw)
    blob.close(); index.close()
    print(f"wrote {at} bytes for {len(wanted)} entries", file=sys.stderr)
