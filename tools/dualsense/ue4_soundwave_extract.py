#!/usr/bin/env python3
"""
Extract audio from a cooked UE4 (4.27) USoundWave asset.

Handles both shapes seen in Stray:
  * non-streamed  -> CompressedFormatData, a single inline FByteBulkData holding
                     a complete Ogg Vorbis stream ("OGG") or a complete RIFF ("ADPCM").
  * streamed      -> FStreamedAudioPlatformData: NumChunks(int32), AudioFormat(FName),
                     then N x FStreamedAudioChunk. Chunk 0 is inline in the .uexp and
                     begins with the RIFF/WAVE header; later chunks live in the .ubulk,
                     zero-padded to a fixed size, with the real length in AudioDataSize.
                     Concatenating chunk0-after-header + chunkN[:AudioDataSize] and
                     keeping the original 82-byte RIFF header reconstitutes the file.

Usage: ue4_soundwave_extract.py <asset-basename-without-extension> [outdir]
Needs <base>.uasset + <base>.uexp, and <base>.ubulk when the asset is streamed.
"""
import json, os, struct, subprocess, sys

class R:
    def __init__(self, b, o=0): self.b, self.o = b, o
    def u8(self):  v = self.b[self.o]; self.o += 1; return v
    def i32(self): v = struct.unpack_from('<i', self.b, self.o)[0]; self.o += 4; return v
    def u32(self): v = struct.unpack_from('<I', self.b, self.o)[0]; self.o += 4; return v
    def i64(self): v = struct.unpack_from('<q', self.b, self.o)[0]; self.o += 8; return v
    def fstr(self):
        n = self.i32()
        if n == 0: return ''
        if n > 0:
            s = self.b[self.o:self.o + n - 1].decode('utf-8', 'replace'); self.o += n
        else:
            n = -n; s = self.b[self.o:self.o + (n - 1) * 2].decode('utf-16-le', 'replace'); self.o += n * 2
        return s

UASSET_TAG = 0x9E2A83C1

def read_names(uasset):
    """UE4.27 package header -> name table. NameCount/NameOffset live at a fixed
    place after the tag + legacy version fields + FString FolderName + PackageFlags."""
    r = R(uasset)
    assert r.u32() == UASSET_TAG, 'not a .uasset'
    legacy = r.i32()                      # -7 for 4.27
    if legacy != -4: r.i32()              # LegacyUE3Version
    r.i32(); r.i32()                      # FileVersionUE4, FileVersionLicenseeUE4
    if legacy <= -2:                      # CustomVersions array
        for _ in range(r.i32()): r.o += 20   # FGuid + int32
    r.i32()                               # TotalHeaderSize
    r.fstr()                              # FolderName
    r.u32()                               # PackageFlags
    name_count, name_off = r.i32(), r.i32()
    names, n = [], R(uasset, name_off)
    for _ in range(name_count):
        s = n.fstr(); n.o += 4            # two uint16 hashes (4.27)
        names.append(s)
    return names

def fname(r, names):
    idx, num = r.i32(), r.i32()
    return names[idx] if 0 <= idx < len(names) else f'<{idx}>'

# FByteBulkData flags
BULKDATA_PayloadAtEndOfFile   = 1 << 0
BULKDATA_ForceInlinePayload   = 1 << 6
BULKDATA_PayloadInSeperateFile= 1 << 8
BULKDATA_Size64Bit            = 1 << 13

def read_bulkdata(r, ubulk):
    """FUntypedBulkData::Serialize -> (flags, bytes)."""
    flags = r.u32()
    if flags & BULKDATA_Size64Bit: count, on_disk = r.i64(), r.i64()
    else:                          count, on_disk = r.i32(), r.i32()
    offset = r.i64()
    if flags & BULKDATA_ForceInlinePayload:
        data = r.b[r.o:r.o + on_disk]; r.o += on_disk
    elif flags & (BULKDATA_PayloadInSeperateFile | BULKDATA_PayloadAtEndOfFile):
        if ubulk is None: raise RuntimeError('asset needs a .ubulk but none was supplied')
        data = ubulk[offset:offset + on_disk]
    else:
        raise RuntimeError(f'unhandled bulkdata flags 0x{flags:x}')
    return flags, count, on_disk, offset, data

def read_properties(uexp, names):
    """Walk the export's tagged property list. Returns (dict, offset_after_None)."""
    r, props = R(uexp, 0), {}
    while True:
        name = fname(r, names)
        if name == 'None':
            return props, r.o
        typ = fname(r, names)
        size = r.i32(); r.i32()                       # size, array index
        if typ == 'BoolProperty':
            props[name] = bool(uexp[r.o]); r.o += 2   # value byte + guid byte
        elif typ == 'EnumProperty':
            fname(r, names); r.o += 1
            props[name] = fname(r, names)
        elif typ in ('SetProperty', 'ArrayProperty', 'MapProperty'):
            fname(r, names); r.o += 1
            props[name] = f'<{typ}>'; r.o += size
        elif typ == 'StructProperty':
            fname(r, names); r.o += 16 + 1            # struct name + guid + hasguid
            props[name] = '<StructProperty>'; r.o += size
        else:
            r.o += 1                                  # has-guid byte
            raw = uexp[r.o:r.o + size]; r.o += size
            props[name] = {'FloatProperty':  lambda b: struct.unpack('<f', b)[0],
                           'DoubleProperty': lambda b: struct.unpack('<d', b)[0],
                           'IntProperty':    lambda b: struct.unpack('<i', b)[0],
                           'ObjectProperty': lambda b: struct.unpack('<i', b)[0],
                           'NameProperty':   lambda b: struct.unpack('<i', b)[0],
                           }.get(typ, lambda b: b.hex())(raw)


def find_property_end(uexp, names):
    """Property lists terminate with the FName 'None'. Scan for it as an aligned
    (int32 idx, int32 num) pair; the export body follows immediately."""
    none_idx = names.index('None')
    tag = struct.pack('<ii', none_idx, 0)
    o = 0
    while True:
        o = uexp.find(tag, o)
        if o < 0: raise RuntimeError("no 'None' terminator found")
        if o % 4 == 0: return o + 8
        o += 1

def extract(base, outdir='.'):
    uasset = open(base + '.uasset', 'rb').read()
    uexp   = open(base + '.uexp', 'rb').read()
    ubulk  = open(base + '.ubulk', 'rb').read() if os.path.exists(base + '.ubulk') else None
    names  = read_names(uasset)

    props, prop_end = read_properties(uexp, names)
    r = R(uexp, prop_end)
    r.i32()                       # 4 bytes preceding bCooked (0 in every observed asset)
    b_cooked = r.i32()            # USoundWave::Serialize: FArchive << bool == int32
    info = {'asset': os.path.basename(base), 'bCooked': b_cooked,
            'properties': props}

    # Peek: streamed assets serialise CompressedDataGuid (FGuid) then the platform data;
    # non-streamed serialise FFormatContainer (NumFormats, FName, FByteBulkData) first.
    save = r.o
    guid = uexp[r.o:r.o + 16]; r.o += 16
    num_chunks = r.i32()
    audio_format = fname(r, names)
    streamed = 0 < num_chunks < 4096 and audio_format in ('ADPCM', 'OGG', 'OPUS', 'PCM', 'OGG-')

    if not streamed:                                   # inline CompressedFormatData
        r.o = save
        num_formats = r.i32()
        fmt = fname(r, names)
        _, _, _, _, payload = read_bulkdata(r, ubulk)
        info.update(streamed=False, format=fmt, size=len(payload))
        ext = '.ogg' if payload[:4] == b'OggS' else ('.wav' if payload[:4] == b'RIFF' else '.bin')
        out = os.path.join(outdir, os.path.basename(base) + ext)
        open(out, 'wb').write(payload)
        info['output'] = out
        return info

    info.update(streamed=True, format=audio_format, num_chunks=num_chunks,
                compressed_data_guid=guid.hex(), chunks=[])
    pieces = []
    for i in range(num_chunks):
        r.i32()                                        # FStreamedAudioChunk bCooked
        flags, count, on_disk, offset, data = read_bulkdata(r, ubulk)
        data_size, audio_data_size = r.i32(), r.i32()
        info['chunks'].append(dict(index=i, flags=hex(flags), on_disk=on_disk,
                                   offset=offset, DataSize=data_size,
                                   AudioDataSize=audio_data_size,
                                   inline=bool(flags & BULKDATA_ForceInlinePayload)))
        pieces.append(data[:audio_data_size])           # trim the zero padding

    blob = b''.join(pieces)
    if blob[:4] != b'RIFF':
        out = os.path.join(outdir, os.path.basename(base) + '.bin')
        open(out, 'wb').write(blob); info['output'] = out; info['note'] = 'no RIFF header'
        return info

    # Chunk 0 leads with the RIFF header. The header's own 'data' size already
    # covers every chunk, so the concatenation is a complete, valid WAV as-is.
    riff_size = struct.unpack_from('<I', blob, 4)[0]
    o, hdr_end, data_size = 12, None, None
    while o + 8 <= len(blob):
        cid, csz = blob[o:o+4], struct.unpack_from('<I', blob, o+4)[0]
        if cid == b'fmt ':
            tag, ch, rate, abps, balign, bits = struct.unpack_from('<HHIIHH', blob, o+8)
            info['fmt'] = dict(wFormatTag=tag, channels=ch, sample_rate=rate,
                               avg_bytes_per_sec=abps, block_align=balign, bits=bits)
            if csz >= 20:
                cb, spb, nc = struct.unpack_from('<HHH', blob, o+8+16)
                info['fmt'].update(cbSize=cb, samples_per_block=spb, num_coef=nc)
        if cid == b'data':
            hdr_end, data_size = o + 8, csz
            break
        o += 8 + csz + (csz & 1)
    info.update(riff_size=riff_size, header_bytes=hdr_end,
                declared_data_size=data_size, actual_data_size=len(blob) - hdr_end)
    if info['fmt'].get('block_align'):
        ba = info['fmt']['block_align']
        info['blocks'] = info['actual_data_size'] / ba
        info['est_samples'] = int(info['actual_data_size'] // ba) * info['fmt']['samples_per_block']
        info['est_duration_s'] = round(info['est_samples'] / info['fmt']['sample_rate'], 3)

    out = os.path.join(outdir, os.path.basename(base) + '.wav')
    open(out, 'wb').write(blob)
    info['output'] = out

    # MS-ADPCM blocks are fixed size, so the encoder pads the final block. The
    # asset's own TotalSamples is the authoritative length -- it matters for a
    # bLooping asset, where the padding would be an audible seam.
    total = props.get('TotalSamples')
    if total:
        info['total_samples_property'] = total
        info['padding_samples'] = info.get('est_samples', 0) - int(total)
    return info

def decode_pcm(info):
    """ffmpeg-decode the carved WAV to PCM, trimmed to the asset's TotalSamples."""
    src = info['output']
    if not src.endswith('.wav'): return None
    dst = src[:-4] + '_pcm.wav'
    cmd = ['ffmpeg', '-hide_banner', '-v', 'error', '-y', '-i', src, '-c:a', 'pcm_s16le']
    total = info.get('total_samples_property')
    if total: cmd += ['-af', f'atrim=end_sample={int(total)}']
    cmd.append(dst)
    subprocess.run(cmd, check=True)
    return dst


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--decode']
    base = args[0]
    if base.endswith(('.uasset', '.uexp', '.ubulk')): base = os.path.splitext(base)[0]
    info = extract(base, args[1] if len(args) > 1 else os.path.dirname(base) or '.')
    if '--decode' in sys.argv:
        info['decoded_pcm'] = decode_pcm(info)
    print(json.dumps(info, indent=2))
