#!/usr/bin/env python3
"""Extract every cooked FTAAStandaloneCS permutation's DXBC from Stray's shader library, offline.

Why: the add-on identifies the TAA dispatch by fnv1a64 over its DXBC (src/core/fnv1a.hpp), but
the compiled permutation changes with screen-percentage configuration, so every config change
needs a new hash measured against the running game. The shipped pak already contains every
cooked permutation; walking it offline yields all their hashes at once, with no game running.

The pak (v11, unencrypted -- CLAUDE.md 2.3.1) is not read directly; pull the file(s) first with
tools/pakextract.py. Two modes, decided by the cache's own per-section bShareCode flag (the
argument count only determines whether the shared-code branch CAN be taken):

  cache-only -- STRAY'S ACTUAL LAYOUT [HARD, measured 2026-08-31: all 225 sections of the
  shipped GlobalShaderCache-PCD3D_SM5.bin carry bShareCode=0, the pak contains no
  .ushaderbytecode, and the DXBC is inline in the cache itself]:

    python3 tools/pakextract.py <...>/Hk_project-WindowsNoEditor.pak /tmp/shaderlib \\
        'GlobalShaderCache-PCD3D_SM5\\.bin$'
    python3 tools/shaderlib_extract.py /tmp/shaderlib/Engine_GlobalShaderCache-PCD3D_SM5.bin

  cache+archive -- a title cooked WITH shared shader code (bShareCode=1), where the section
  stores only a 20-byte ResourceHash and the bytes live in the archive:

    python3 tools/shaderlib_extract.py \\
        /tmp/shaderlib/Engine_GlobalShaderCache-PCD3D_SM5.bin \\
        /tmp/shaderlib/Hk_project_Content_ShaderArchive-Global-PCD3D_SM5.ushaderbytecode

A shared-code section without the archive argument fails loudly; an inline section never
consults the archive. pakextract names outputs by flattening the pak path ('/' -> '_'); use
whatever names it printed.
Optional: --dump-dir DIR writes each permutation as <freq>_<fnv1a64>.dxbc (the shader_dump
naming), --emit-header FILE writes the verified permutation hashes as a C++ header (refused
when the verdict fails), --self-test runs the synthetic format tests and touches no files.

Format provenance -- each claim read from UE 4.27.2 source, not inferred:
  * .ushaderbytecode = uint32 version (==2, ShaderCodeLibrary.cpp:60,722-726), then six TArrays
    (int32 count + elements): FSHAHash ShaderMapHashes (20B), FSHAHash ShaderHashes,
    FShaderMapEntry (4x uint32, ShaderCodeArchive.h:11-22), FShaderCodeEntry (uint64 Offset,
    uint32 Size, uint32 UncompressedSize, uint8 Frequency = 17B, ShaderCodeArchive.h:29-45),
    FFileCachePreloadEntry (2x int64), uint32 ShaderIndices -- order per
    FSerializedShaderArchive::Serialize (ShaderCodeArchive.cpp:199-206). The code blob follows
    immediately; entry Offsets are relative to it (ShaderCodeArchive.cpp:704-709).
  * Compression is raw-block LZ4 (ShaderCodeArchive.cpp:34), applied only when
    Size != UncompressedSize (ShaderCodeArchive.cpp:957-963). Decoded here in pure Python so
    the operator's machine needs nothing beyond python3.
  * A decompressed shader is [FD3D12ShaderResourceTable][DXBC container][optional trailer].
    The table is uint32 ResourceTableBits + five TArray<uint32> + TArray<uint32> TextureMap
    (ShaderCore.h:378-387, D3D12ShaderResources.h:28-34); the trailer's last int32 is its own
    total size (ShaderCore.h:692-707). D3D12 receives exactly the bytes between the two
    (D3D12Shaders.cpp:248-275), which is what ReShade's init_pipeline hashes -- so fnv1a64
    over that slice must reproduce live-measured hashes byte for byte. Verified against three
    live-dumped shaders: file == DXBC container == the hashed range, and the container's own
    size field at +24 equals its length.
  * GlobalShaderCache-PCD3D_SM5.bin = int32 NumSections then sections, nothing before it
    (written by FGlobalShaderMap::SaveToGlobalArchive, GlobalShader.cpp:472-481, straight into
    the file at ShaderCompiler.cpp:5530-5546; the pre-4.26 'GSMB' tag is gone). One section
    per shader source file (FindOrAddSection keys on GetHashedShaderFilename,
    GlobalShader.cpp:460-470). Section framing per FShaderMapBase::Serialize (ShaderMap.cpp):
    uint32 FrozenContentSize + frozen blob; patch tables (MemoryImage.cpp,
    FMemoryImageResult::SaveToArchive / ApplyPatchesFromArchive: uint32 NumVTables/
    NumScriptNames/NumMinimalNames, vtables as uint64 hash + uint32 n + n*8B, names as
    FString + uint32 n + n*4B -- FName serializes as FString here, MemoryArchive.h:36-51);
    pointer table (Shader.cpp:430-452: int32 NumTypes, int32 NumVFTypes, then uint64
    FHashedName per type -- THE SHADER TYPE NAMES, our identification signal); int32
    NumDependencies + 32B each; uint32 bShareCode (bool serializes as uint32,
    Archive.cpp:487-502); if true a 20B FSHAHash ResourceHash that equals the shadermap's
    hash in the archive (ShaderMap.cpp bShareCode branch), else the code is inline as
    FShaderMapResourceCode (ShaderResource.cpp:238-243, entries per Shader.h:314-327).
  * Inline FShaderMapResourceCode is: 20B ResourceHash, TArray<FSHAHash> ShaderHashes,
    TArray<FShaderEntry> ShaderEntries where each entry is TArray<uint8> Code +
    int32 UncompressedSize + uint8 Frequency (Shader.h:314-326). ShaderHashes and
    ShaderEntries are PARALLEL, index-aligned arrays: Serialize writes them back to back and
    the engine itself checks Num()==Num() (ShaderResource.cpp:242), and ToString pairs
    ShaderHashes[i] with ShaderEntries[i] (ShaderResource.cpp:226-236) -- so entry i's
    FSHAHash names entry i's bytes. [HARD, read from the 4.27 source]. Per-entry compression
    is LZ4 (ShaderCompressionFormat = NAME_LZ4, ShaderResource.cpp:32), applied exactly when
    Code.Num() != UncompressedSize (the loader's own gate, ShaderResource.cpp:403-406).
  * FHashedName("FTAAStandaloneCS") = CityHash64WithSeed over the uppercased name
    = 0x2a83f8d4225ef8c5 -- confirmed twice: recomputed from UE's own CityHash sources, and
    present in CUE4Parse's shipped ShaderHashedNames.json.

Every structural assumption is asserted; a FATAL names the field, absolute file offset and
observed value, so a licensee format edit fails loudly instead of yielding wrong hashes.
Exit 0 only if the walk is clean AND the known-hash verdict passes.
"""
import os, struct, sys

# --- the four measured identities the run is judged against ------------------------------------
# fnv1a64 parameters must match src/core/fnv1a.hpp exactly, or no hash here means anything.
FNV_BASIS = 0xcbf29ce484222325
FNV_PRIME = 0x100000001b3

TAA_TYPE_HASHED_NAME = 0x2a83f8d4225ef8c5  # FHashedName("FTAAStandaloneCS"), see docstring

EXPECT_PRESENT = {
    0xd2e4d8c23c362ed1,  # measured live 2026-08-31 @ 2560x1440 / 100%SP. CLAUDE.md argues from
                         # its live UAV set that the DISPATCH wearing it is a composite, not the
                         # TAA -- but the DXBC itself is cooked inside the FTAAStandaloneCS
                         # section (this tool's own walk), so its presence here is expected
                         # either way and remains a hard reproduction check
    0x901e041a7cadc9db,  # MainUpsampling perm @ 3840x2160 / 50%SP -- CLAUDE.md 2.3. THE
                         # acceptance hash: if this one is missing, the parse is wrong
}
# Informational only -- printed, never affects the verdict. Live-measured hashes whose absence
# would be a finding to report, not proof of a broken walk (CLAUDE.md 2.3: structural candidate
# measured @ 2560x1440 alongside 0xd2e4...).
EXPECT_NOTE = {
    0xe14e7fc8d0db9b0f,
}
EXPECT_ABSENT = {
    0x1708ec956099e259,  # reprojecting denoiser look-alike, NOT TAA -- CLAUDE.md 2.3
    0x52101a15e1a0c5cc,  # eleven-SRV pass, NOT TAA -- CLAUDE.md 2.3
}

SF_COMPUTE = 5  # RHIDefinitions.h:29-34
FREQ_NAMES = {0: 'vs', 1: 'hs', 2: 'ds', 3: 'ps', 4: 'gs', 5: 'cs'}
SHA_LEN = 20


def die(msg):
    raise SystemExit("FATAL: " + msg)


def fnv1a64(data):
    h = FNV_BASIS
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def lz4_block_decompress(src, expected_size, what):
    """Raw LZ4 block (no frame). Pure Python so the operator installs nothing."""
    out = bytearray()
    i, n = 0, len(src)
    while True:
        if i >= n:
            die(f"{what}: LZ4 stream truncated at token (byte {i} of {n})")
        token = src[i]; i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if i >= n: die(f"{what}: LZ4 truncated in literal-length extension")
                b = src[i]; i += 1; lit += b
                if b != 255: break
        if i + lit > n:
            die(f"{what}: LZ4 literal run of {lit} overruns input at byte {i}")
        out += src[i:i + lit]; i += lit
        if i == n:
            break  # spec: the last sequence is literals-only
        if i + 2 > n: die(f"{what}: LZ4 truncated at match offset")
        off = src[i] | (src[i + 1] << 8); i += 2
        if off == 0 or off > len(out):
            die(f"{what}: LZ4 match offset {off} invalid with {len(out)} bytes decoded")
        mlen = (token & 0xF) + 4
        if (token & 0xF) == 15:
            while True:
                if i >= n: die(f"{what}: LZ4 truncated in match-length extension")
                b = src[i]; i += 1; mlen += b
                if b != 255: break
        if off >= mlen:
            out += out[-off:len(out) - off + mlen]
        else:  # overlap: the copy source grows as we write, i.e. a repeating pattern
            pat = out[-off:]
            out += (pat * (mlen // off + 1))[:mlen]
    if len(out) != expected_size:
        die(f"{what}: LZ4 decoded {len(out)} bytes, entry says UncompressedSize={expected_size}")
    return bytes(out)


class Reader:
    """Bounds-checked little-endian cursor; every failure names field + absolute offset."""
    def __init__(self, buf, name):
        self.buf, self.name, self.off = buf, name, 0

    def _get(self, fmt, size, field):
        if self.off + size > len(self.buf):
            die(f"{self.name}: reading {field} at offset {self.off} runs past EOF ({len(self.buf)})")
        v = struct.unpack_from(fmt, self.buf, self.off)[0]
        self.off += size
        return v

    def u8(self, f):  return self._get('<B', 1, f)
    def u32(self, f): return self._get('<I', 4, f)
    def i32(self, f): return self._get('<i', 4, f)
    def u64(self, f): return self._get('<Q', 8, f)

    def count(self, f, limit=10_000_000):
        n = self.i32(f)
        if not 0 <= n <= limit:
            die(f"{self.name}: {f} = {n} at offset {self.off - 4} is not a sane TArray count")
        return n

    def take(self, size, f):
        if self.off + size > len(self.buf):
            die(f"{self.name}: {f} ({size} bytes) at offset {self.off} runs past EOF")
        v = self.buf[self.off:self.off + size]
        self.off += size
        return v

    def fstring(self, f):
        # UE FString: int32 length incl. NUL; negative means UTF-16 (same as tools/paklist.py).
        n = self.i32(f)
        if n == 0: return ''
        if abs(n) > 4096:
            die(f"{self.name}: {f} FString length {n} at offset {self.off - 4} is implausible")
        if n < 0:
            return self.take(-n * 2, f).decode('utf-16-le', 'replace').rstrip('\x00')
        return self.take(n, f).decode('latin1', 'replace').rstrip('\x00')


# --- the shader library archive (.ushaderbytecode) ---------------------------------------------

def parse_archive(buf, name):
    r = Reader(buf, name)
    version = r.u32("archive version")
    if version != 2:
        die(f"{name}: version {version}, expected GShaderCodeArchiveVersion == 2 "
            "(ShaderCodeLibrary.cpp:60) -- not a UE 4.2x cooked shader archive?")
    map_hashes = [r.take(SHA_LEN, "ShaderMapHashes[]") for _ in range(r.count("ShaderMapHashes"))]
    shader_hashes = [r.take(SHA_LEN, "ShaderHashes[]") for _ in range(r.count("ShaderHashes"))]
    map_entries = [struct.unpack('<IIII', r.take(16, "ShaderMapEntries[]"))
                   for _ in range(r.count("ShaderMapEntries"))]
    shader_entries = []
    for _ in range(r.count("ShaderEntries")):
        off = r.u64("ShaderEntry.Offset")
        size = r.u32("ShaderEntry.Size")
        usize = r.u32("ShaderEntry.UncompressedSize")
        freq = r.u8("ShaderEntry.Frequency")
        if freq > 15:
            die(f"{name}: ShaderEntry.Frequency = {freq} at offset {r.off - 1}; the 17-byte "
                "entry layout (ShaderCodeArchive.h:29-45) does not hold")
        shader_entries.append((off, size, usize, freq))
    n_preload = r.count("PreloadEntries")
    r.take(16 * n_preload, "PreloadEntries[]")
    n_idx = r.count("ShaderIndices")
    indices = list(struct.unpack(f'<{n_idx}I', r.take(4 * n_idx, "ShaderIndices[]")))
    # Mirrors the loader's own invariants (ShaderCodeArchive.cpp:208-209).
    if len(map_hashes) != len(map_entries):
        die(f"{name}: {len(map_hashes)} ShaderMapHashes vs {len(map_entries)} ShaderMapEntries")
    if len(shader_hashes) != len(shader_entries):
        die(f"{name}: {len(shader_hashes)} ShaderHashes vs {len(shader_entries)} ShaderEntries")
    return {'map_hashes': map_hashes, 'map_entries': map_entries,
            'shader_hashes': shader_hashes,
            'shader_entries': shader_entries, 'indices': indices,
            'code_off': r.off,  # blob begins right here (ShaderCodeArchive.cpp:704-709)
            'buf': buf, 'name': name}


def slice_dxbc(blob, what):
    """[resource table][DXBC][trailer] -> the exact bytes D3D12 (and so ReShade) sees."""
    r = Reader(blob, what)
    r.u32("ResourceTableBits")
    for field in ("ShaderResourceViewMap", "SamplerMap", "UnorderedAccessViewMap",
                  "ResourceTableLayoutHashes", "TextureMap"):
        r.take(4 * r.count(field, limit=100_000), field + "[]")
    start = r.off
    if blob[start:start + 4] != b'DXBC':
        die(f"{what}: no DXBC fourcc at offset {start} after the resource table "
            f"(got {blob[start:start + 4].hex()}); FD3D12ShaderResourceTable layout "
            "(ShaderCore.h:378-387 + D3D12ShaderResources.h:28-34) does not hold")
    if start + 28 > len(blob):
        die(f"{what}: blob ends inside the DXBC header")
    container = struct.unpack_from('<I', blob, start + 24)[0]  # container's own total size
    opt = struct.unpack_from('<i', blob, len(blob) - 4)[0]     # ShaderCore.h:692-707
    if not 4 <= opt <= len(blob):
        die(f"{what}: optional-data trailer size {opt} vs blob of {len(blob)}")
    # Three independent measurements of the same boundary must agree exactly.
    if start + container != len(blob) - opt:
        die(f"{what}: table ends at {start}, DXBC declares {container}, trailer claims code "
            f"ends at {len(blob) - opt} -- the three framings disagree; refusing to guess")
    return blob[start:start + container]


def extract_shader(arch, index):
    off, size, usize, freq = arch['shader_entries'][index]
    what = f"{arch['name']}: shader #{index}"
    lo = arch['code_off'] + off
    if lo + size > len(arch['buf']):
        die(f"{what}: code [{lo}, {lo + size}) runs past EOF ({len(arch['buf'])})")
    raw = arch['buf'][lo:lo + size]
    # Size == UncompressedSize means stored raw (ShaderCodeArchive.cpp:957-963).
    blob = raw if size == usize else lz4_block_decompress(raw, usize, what)
    return arch['shader_hashes'][index], freq, size, usize, slice_dxbc(blob, what)


# --- the global shader cache (.bin) ------------------------------------------------------------

def parse_global_cache(buf, name):
    r = Reader(buf, name)
    n_sections = r.count("NumSections", limit=100_000)
    if n_sections == 0:
        die(f"{name}: zero sections -- wrong file, or not a cooked global shader cache")
    sections = []
    for s in range(n_sections):
        what = f"{name}: section #{s}"
        begin = r.off
        frozen_size = r.u32("FrozenContentSize")
        if frozen_size == 0:
            die(f"{what}: FrozenContentSize == 0 at offset {r.off - 4} (saver checks > 0)")
        frozen_at = r.off
        r.take(frozen_size, "frozen content")
        n_vt = r.u32("NumVTables"); n_sn = r.u32("NumScriptNames"); n_mn = r.u32("NumMinimalNames")
        if max(n_vt, n_sn, n_mn) > 100_000:
            die(f"{what}: patch-table counts {n_vt}/{n_sn}/{n_mn} implausible -- walk is off "
                f"the rails near offset {r.off - 12}")
        for _ in range(n_vt):
            r.u64("VTable.TypeNameHash")
            r.take(8 * r.count("VTable.NumPatches", 1_000_000), "VTable patches")
        for group, n in (("ScriptName", n_sn), ("MinimalName", n_mn)):
            for _ in range(n):
                r.fstring(group + ".Name")
                r.take(4 * r.count(group + ".NumPatches", 1_000_000), group + " patches")
        n_types = r.count("PointerTable.NumTypes", 100_000)
        n_vf = r.count("PointerTable.NumVFTypes", 100_000)
        type_hashes = [r.u64("PointerTable.ShaderType") for _ in range(n_types)]
        vf_hashes = [r.u64("PointerTable.VFType") for _ in range(n_vf)]
        r.take(32 * r.count("NumDependencies", 100_000), "dependency records")
        share = r.u32("bShareCode")
        if share not in (0, 1):
            die(f"{what}: bShareCode = {share} at offset {r.off - 4}; a bool serializes as "
                "uint32 0/1 (Archive.cpp:487-502) -- the walk has desynchronized")
        resource_hash, inline_shaders = None, None
        if share:
            resource_hash = r.take(SHA_LEN, "ResourceHash")
        else:
            # Inline FShaderMapResourceCode (ShaderResource.cpp:238-243; Shader.h:314-327).
            # ShaderHashes[] and ShaderEntries[] are parallel, index-aligned arrays: the
            # engine's own Serialize checks the counts match (ShaderResource.cpp:242) and
            # its ToString pairs hash i with entry i (ShaderResource.cpp:226-236). [HARD]
            r.take(SHA_LEN, "inline ResourceHash")
            n_hashes = r.count("inline ShaderHashes")
            inline_hashes = [r.take(SHA_LEN, "inline ShaderHashes[]")
                             for _ in range(n_hashes)]
            n_entries = r.count("inline ShaderEntries")
            if n_entries != n_hashes:
                die(f"{what}: {n_hashes} inline ShaderHashes vs {n_entries} ShaderEntries -- "
                    "parallel by contract (ShaderResource.cpp:242); the walk has desynchronized")
            inline_shaders = []
            for i in range(n_entries):
                code = r.take(r.count("inline Code"), "inline Code[]")
                iusize = r.i32("inline UncompressedSize")
                ifreq = r.u8("inline Frequency")
                inline_shaders.append((inline_hashes[i], code, iusize, ifreq))
        sections.append({'index': s, 'span': (begin, r.off), 'frozen': (frozen_at, frozen_size),
                         'type_hashes': type_hashes, 'vf_hashes': vf_hashes,
                         'share': share, 'resource_hash': resource_hash,
                         'inline_shaders': inline_shaders})
    if r.off != len(buf):
        die(f"{name}: walked {n_sections} sections to offset {r.off} but the file has "
            f"{len(buf)} bytes -- {len(buf) - r.off} unexplained trailing bytes")
    return sections


def find_taa_section(sections, buf, name):
    hits = [s for s in sections if TAA_TYPE_HASHED_NAME in s['type_hashes']]
    if len(hits) == 1:
        s = hits[0]
        # Cross-check: the frozen FShaderMapContent stores the same FHashedName in its
        # ShaderTypes array, so the little-endian pattern should occur inside this section too.
        pat = struct.pack('<Q', TAA_TYPE_HASHED_NAME)
        at, sz = s['frozen']
        if buf[at:at + sz].find(pat) < 0:
            print(f"WARNING: {name}: section #{s['index']} pointer table names FTAAStandaloneCS "
                  "but its frozen content does not contain the hash -- unexpected, continuing",
                  file=sys.stderr)
        return s
    pat = struct.pack('<Q', TAA_TYPE_HASHED_NAME)
    raw_hits = []
    at = buf.find(pat)
    while at >= 0:
        raw_hits.append(at)
        at = buf.find(pat, at + 1)
    if len(hits) == 0:
        die(f"{name}: no section's pointer table names FTAAStandaloneCS "
            f"(0x{TAA_TYPE_HASHED_NAME:016x}). Raw byte-scan finds the pattern at offsets "
            f"{raw_hits or 'nowhere'} -- if nowhere, this build does not cook that shader type; "
            "if somewhere, the section walk mis-parsed and the offsets say where to look")
    die(f"{name}: sections {[s['index'] for s in hits]} ALL name FTAAStandaloneCS -- ambiguous, "
        "which contradicts one-section-per-source-file (GlobalShader.cpp:460-470)")


# --- putting it together -----------------------------------------------------------------------

def emit_header(path, hashes, cache_path, cache_bytes, section_index):
    """The generated table the add-on's matcher consults: hash membership == "this DXBC is a
    cooked FTAAStandaloneCS permutation". Emitted only from a PASSING run, so the table always
    carries the live-verified acceptance hashes."""
    import hashlib
    sha1 = hashlib.sha1(cache_bytes).hexdigest()
    lines = [
        '// GENERATED by tools/shaderlib_extract.py --emit-header. DO NOT EDIT.',
        '//',
        '// Every cooked FTAAStandaloneCS permutation in the game build, fnv1a64 over the DXBC',
        '// container exactly as ReShade hashes it at init_pipeline. Membership here is the',
        '// hash half of TAA identification (CLAUDE.md #2.3); the structural signature is the',
        '// other half and neither is sufficient alone.',
        '//',
        f'// Source: {os.path.basename(cache_path)} ({len(cache_bytes)} bytes,',
        f'//         sha1 {sha1}), FTAAStandaloneCS section #{section_index},',
        f'//         {len(hashes)} permutations. Regenerate after a game update:',
        '//   pakextract.py <pak> out GlobalShaderCache-PCD3D_SM5 --raw',
        '//   oodle_unblock.py out/...json out/...raw cache.bin <oozraw>',
        '//   shaderlib_extract.py cache.bin --emit-header src/core/taa_hashes.hpp',
        '#pragma once',
        '',
        '#include <cstddef>',
        '#include <cstdint>',
        '',
        'namespace stray_dlss {',
        '',
        'inline constexpr std::uint64_t kKnownTaaHashes[] = {',
    ]
    for h in sorted(hashes):
        lines.append(f'\t0x{h:016x}ull,')
    lines += ['};', '', '} // namespace stray_dlss', '']
    with open(path, 'w') as f:
        f.write('\n'.join(lines))
    print(f"[emit] {path}: {len(hashes)} hashes", file=sys.stderr)


def run(cache_path, archive_path=None, dump_dir=None, emit_header_path=None,
        expect_present=EXPECT_PRESENT, expect_absent=EXPECT_ABSENT, expect_note=EXPECT_NOTE):
    cache = open(cache_path, 'rb').read()
    sections = parse_global_cache(cache, os.path.basename(cache_path))
    print(f"[cache] {len(sections)} sections walked cleanly to EOF ({len(cache)} bytes)",
          file=sys.stderr)
    taa = find_taa_section(sections, cache, os.path.basename(cache_path))
    print(f"[cache] FTAAStandaloneCS is section #{taa['index']}: "
          f"{len(taa['type_hashes'])} shader types "
          f"({', '.join(f'0x{h:016x}' for h in taa['type_hashes'])}), "
          f"bShareCode={taa['share']}", file=sys.stderr)

    results = []  # (sha20, freq, stored, usize, dxbc)
    if taa['share']:
        if archive_path is None:
            die(f"section #{taa['index']} has bShareCode=1: its code lives in the shared "
                "archive, which was not given. Pass ShaderArchive-Global-PCD3D_SM5"
                ".ushaderbytecode as the second argument (extract it from the pak first; "
                "see this file's docstring)")
        arch = parse_archive(open(archive_path, 'rb').read(), os.path.basename(archive_path))
        print(f"[archive] version=2, {len(arch['map_entries'])} shadermaps, "
              f"{len(arch['shader_entries'])} shaders, code blob at 0x{arch['code_off']:x}",
              file=sys.stderr)
        by_hash = {h: i for i, h in enumerate(arch['map_hashes'])}
        rh = taa['resource_hash']
        if rh not in by_hash:
            die(f"section ResourceHash {rh.hex()} is not among the archive's "
                f"{len(arch['map_hashes'])} ShaderMapHashes -- wrong archive file? The global "
                "sections register into the Global library; pass ShaderArchive-Global-*.")
        mi = by_hash[rh]
        idx_off, num, _, _ = arch['map_entries'][mi]
        if idx_off + num > len(arch['indices']):
            die(f"shadermap #{mi}: ShaderIndices[{idx_off}:{idx_off + num}] out of range "
                f"({len(arch['indices'])} indices)")
        print(f"[archive] ResourceHash {rh.hex()} = shadermap #{mi}, {num} shaders",
              file=sys.stderr)
        for i in arch['indices'][idx_off:idx_off + num]:
            if i >= len(arch['shader_entries']):
                die(f"shadermap #{mi} references shader #{i} of {len(arch['shader_entries'])}")
            results.append(extract_shader(arch, i))
    else:
        # bShareCode=false: the code never went to an archive; it is inline in the cache.
        print("[cache] bShareCode=0 -- taking the inline-code branch"
              + (", ignoring the archive argument" if archive_path else
                 " (single-file mode, correct for Stray)"), file=sys.stderr)
        for i, (sha, code, usize, freq) in enumerate(taa['inline_shaders']):
            what = f"inline shader #{i}"
            blob = code if len(code) == usize else lz4_block_decompress(code, usize, what)
            results.append((sha, freq, len(code), usize, slice_dxbc(blob, what)))

    print(f"{'perm':>4}  {'sha1(shader)':<40}  {'freq':>4}  {'stored':>8}  {'raw':>8}  "
          f"{'dxbc':>8}  fnv1a64")
    hashes = set()
    for idx, (sha, freq, stored, usize, dxbc) in enumerate(results):
        h = fnv1a64(dxbc)
        hashes.add(h)
        fname = FREQ_NAMES.get(freq, f"f{freq}")
        print(f"{idx:>4}  {sha.hex():<40}  {fname:>4}  {stored:>8}  {usize:>8}  "
              f"{len(dxbc):>8}  0x{h:016x}")
        if dump_dir is not None:
            os.makedirs(dump_dir, exist_ok=True)
            with open(os.path.join(dump_dir, f"{fname}_{h:016x}.dxbc"), 'wb') as f:
                f.write(dxbc)

    ok = True
    for h in sorted(expect_present):
        got = h in hashes
        ok &= got
        print(f"{'PASS' if got else 'FAIL'}: 0x{h:016x} (known TAA permutation) "
              f"{'present' if got else 'MISSING -- extraction does not reproduce live hashes'}")
    for h in sorted(expect_absent):
        got = h not in hashes
        ok &= got
        print(f"{'PASS' if got else 'FAIL'}: 0x{h:016x} (known non-TAA) "
              f"{'absent' if got else 'PRESENT -- section resolution is wrong'}")
    for h in sorted(expect_note):
        # Informational: a live-measured hash whose absence is a finding, not a parse failure.
        print(f"NOTE: 0x{h:016x} (live-measured candidate) "
              f"{'present' if h in hashes else 'ABSENT -- report this, it does not fail the run'}")
    print("VERDICT: " + ("the offline walk reproduces the live TAA identity" if ok
                         else "FALSIFIED -- do not trust these hashes"))
    if emit_header_path is not None:
        if not ok:
            die("--emit-header refused: the verdict failed, these hashes are not trustworthy")
        emit_header(emit_header_path, hashes, cache_path, cache, taa['index'])
    return 0 if ok else 3


# --- self-test: synthetic files exercising every branch of the walk ----------------------------

def _fstr(s):
    return struct.pack('<i', len(s) + 1) + s.encode('latin1') + b'\x00'

def _fstr16(s):
    return struct.pack('<i', -(len(s) + 1)) + s.encode('utf-16-le') + b'\x00\x00'

def _lz4_literals(data):
    """Minimal valid encoder (single literals-only sequence); exercises the extension path."""
    n = len(data)
    if n < 15:
        return bytes([n << 4]) + data
    out = bytearray([0xF0]); n -= 15
    while n >= 255:
        out.append(255); n -= 255
    out.append(n)
    return bytes(out) + data

def _fake_shader(tag, body_len):
    """A blob laid out exactly as slice_dxbc expects; returns (blob, expected dxbc slice)."""
    body = (tag * ((body_len // len(tag)) + 1))[:body_len]
    dxbc = b'DXBC' + bytes(range(16)) + struct.pack('<HH', 1, 0) \
        + struct.pack('<I', 32 + len(body)) + struct.pack('<I', 0) + body
    table = struct.pack('<I', 0) + struct.pack('<iI', 1, 7) + struct.pack('<i', 0) * 4
    opt = b'\xab' + struct.pack('<I', 3) + b'xyz'
    trailer = opt + struct.pack('<i', len(opt) + 4)
    return table + dxbc + trailer, dxbc

def _fake_archive(stored_list, maps):
    blob, entries = b'', b''
    for stored, usize, freq in stored_list:
        entries += struct.pack('<QIIB', len(blob), len(stored), usize, freq)
        blob += stored
    out = struct.pack('<I', 2)
    out += struct.pack('<i', len(maps)) + b''.join(h for h, _, _ in maps)
    out += struct.pack('<i', len(stored_list)) + b''.join(
        bytes([17 + i]) * SHA_LEN for i in range(len(stored_list)))
    out += struct.pack('<i', len(maps)) + b''.join(
        struct.pack('<IIII', off, num, 0, 0) for _, off, num in maps)
    out += struct.pack('<i', len(stored_list)) + entries
    out += struct.pack('<i', 0)  # no preload entries
    idx = list(range(len(stored_list)))
    out += struct.pack('<i', len(idx)) + struct.pack(f'<{len(idx)}I', *idx)
    return out + blob

def _fake_section(type_hashes, resource_hash, frozen_extra=b'', inline=None):
    frozen = b'\x11' * 48 + frozen_extra + b'\x22' * 16
    out = struct.pack('<I', len(frozen)) + frozen
    out += struct.pack('<III', 1, 1, 1)
    out += struct.pack('<Q', 0xD00D) + struct.pack('<I', 2) + struct.pack('<IIII', 0, 8, 8, 16)
    out += _fstr('AScriptName') + struct.pack('<I', 1) + struct.pack('<I', 12)
    out += _fstr16('WideName') + struct.pack('<I', 1) + struct.pack('<I', 16)
    out += struct.pack('<ii', len(type_hashes), 0)
    out += b''.join(struct.pack('<Q', h) for h in type_hashes)
    out += struct.pack('<i', 1) + struct.pack('<QI', 0xFEED, 128) + b'\x33' * SHA_LEN
    if inline is None:
        out += struct.pack('<I', 1) + resource_hash
    else:
        # bShareCode=0: inline FShaderMapResourceCode, `inline` = [(sha20, stored, usize, freq)].
        out += struct.pack('<I', 0) + resource_hash
        out += struct.pack('<i', len(inline)) + b''.join(sha for sha, _, _, _ in inline)
        out += struct.pack('<i', len(inline))
        for _, stored, usize, freq in inline:
            out += struct.pack('<i', len(stored)) + stored + struct.pack('<iB', usize, freq)
    return out

def self_test():
    # fnv1a64 against the canonical FNV-1a test vectors, i.e. the src/core/fnv1a.hpp behaviour.
    assert fnv1a64(b'') == FNV_BASIS
    assert fnv1a64(b'a') == 0xaf63dc4c8601ec8c
    assert fnv1a64(b'foobar') == 0x85944171f73967e8

    # LZ4: literals only, overlap match (offset < length => repeating pattern), extensions.
    assert lz4_block_decompress(b'\x50ABCDE', 5, 't') == b'ABCDE'
    assert lz4_block_decompress(b'\x44ABCD\x04\x00\x20XY', 14, 't') == b'ABCDABCDABCDXY'
    assert lz4_block_decompress(b'\xf0\x02' + b'Q' * 17, 17, 't') == b'Q' * 17
    data = bytes(range(64)) * 4
    assert lz4_block_decompress(_lz4_literals(data), len(data), 't') == data
    for bad, size in ((b'\x44ABCD\x00\x00\x20XY', 14),  # offset 0
                      (b'\x50ABC', 5)):                  # truncated literals
        try:
            lz4_block_decompress(bad, size, 't'); assert False, "accepted corrupt LZ4"
        except SystemExit:
            pass

    # End-to-end on synthetic files: two TAA shaders (one LZ4'd, one raw) + one decoy.
    blob_a, dxbc_a = _fake_shader(b'TAAMAIN.', 200)
    blob_b, dxbc_b = _fake_shader(b'TAAUPSMP', 900)
    blob_c, dxbc_c = _fake_shader(b'DECOY...', 300)
    hash_taa, hash_decoy = b'\x0a' * SHA_LEN, b'\x0b' * SHA_LEN
    archive = _fake_archive(
        [(_lz4_literals(blob_a), len(blob_a), SF_COMPUTE),
         (blob_b, len(blob_b), SF_COMPUTE),
         (blob_c, len(blob_c), 3)],
        [(hash_taa, 0, 2), (hash_decoy, 2, 1)])
    cache = struct.pack('<i', 2)
    cache += _fake_section([0xBADD1E], hash_decoy)
    cache += _fake_section([0x1234, TAA_TYPE_HASHED_NAME, 0x5678], hash_taa,
                           frozen_extra=struct.pack('<Q', TAA_TYPE_HASHED_NAME))

    sections = parse_global_cache(cache, 'synthetic-cache')
    assert len(sections) == 2
    taa = find_taa_section(sections, cache, 'synthetic-cache')
    assert taa['index'] == 1 and taa['resource_hash'] == hash_taa
    arch = parse_archive(archive, 'synthetic-archive')
    mi = {h: i for i, h in enumerate(arch['map_hashes'])}[taa['resource_hash']]
    idx_off, num, _, _ = arch['map_entries'][mi]
    got = [extract_shader(arch, i) for i in arch['indices'][idx_off:idx_off + num]]
    assert [(f, d) for _, f, _, _, d in got] == [(SF_COMPUTE, dxbc_a), (SF_COMPUTE, dxbc_b)]
    assert fnv1a64(dxbc_c) not in {fnv1a64(d) for _, _, _, _, d in got}

    # A desynchronized walk must fail loudly, not return garbage: corrupt bShareCode.
    bad = cache.replace(struct.pack('<I', 1) + hash_decoy, struct.pack('<I', 7) + hash_decoy)
    try:
        parse_global_cache(bad, 'corrupt-cache'); assert False, "accepted bad bShareCode"
    except SystemExit:
        pass

    # The verdict itself, run through the real entry point on temp copies of the fakes.
    # Output is swallowed: a deliberately-falsified inner run would otherwise print FAIL
    # lines that read like a failing self-test.
    import contextlib, io, tempfile
    with tempfile.TemporaryDirectory() as td:
        cp, ap = os.path.join(td, 'cache.bin'), os.path.join(td, 'arch.ushaderbytecode')
        open(cp, 'wb').write(cache); open(ap, 'wb').write(archive)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc = run(cp, ap, dump_dir=os.path.join(td, 'dump'),
                     expect_present={fnv1a64(dxbc_a), fnv1a64(dxbc_b)},
                     expect_absent={fnv1a64(dxbc_c)}, expect_note=set())
        assert rc == 0, "verdict failed on a correct synthetic library"
        assert sorted(os.listdir(os.path.join(td, 'dump'))) == \
            sorted([f"cs_{fnv1a64(dxbc_a):016x}.dxbc", f"cs_{fnv1a64(dxbc_b):016x}.dxbc"])
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc = run(cp, ap, expect_present={fnv1a64(dxbc_a)}, expect_absent={fnv1a64(dxbc_b)},
                     expect_note=set())
        assert rc == 3, "verdict passed although a forbidden hash was present"

        # A bShareCode=1 section without the archive must fail loudly, never guess.
        try:
            with contextlib.redirect_stdout(io.StringIO()), \
                    contextlib.redirect_stderr(io.StringIO()):
                run(cp, None, expect_note=set())
            assert False, "shared-code cache accepted without the archive"
        except SystemExit:
            pass

    # Cache-only mode: bShareCode=0 puts the code inline in the cache (Stray's actual layout).
    # One LZ4-compressed entry, one stored raw; the decoy section stays bShareCode=1, so one
    # file exercises both parse branches side by side.
    blob_d, dxbc_d = _fake_shader(b'TAAINLIN', 500)
    sha_a, sha_d = b'\x0d' * SHA_LEN, b'\x0e' * SHA_LEN
    inline_cache = struct.pack('<i', 2)
    inline_cache += _fake_section([0xBADD1E], hash_decoy)
    inline_cache += _fake_section(
        [TAA_TYPE_HASHED_NAME], b'\x0c' * SHA_LEN,
        frozen_extra=struct.pack('<Q', TAA_TYPE_HASHED_NAME),
        inline=[(sha_a, _lz4_literals(blob_a), len(blob_a), SF_COMPUTE),
                (sha_d, blob_d, len(blob_d), SF_COMPUTE)])
    isecs = parse_global_cache(inline_cache, 'synthetic-inline-cache')
    itaa = find_taa_section(isecs, inline_cache, 'synthetic-inline-cache')
    assert itaa['share'] == 0 and itaa['resource_hash'] is None
    # Hash i must name entry i (the parallel-array contract, ShaderResource.cpp:242).
    assert [(s, u, f) for s, _, u, f in itaa['inline_shaders']] == \
        [(sha_a, len(blob_a), SF_COMPUTE), (sha_d, len(blob_d), SF_COMPUTE)]
    with tempfile.TemporaryDirectory() as td:
        cp = os.path.join(td, 'inline-cache.bin')
        open(cp, 'wb').write(inline_cache)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            rc = run(cp, None, dump_dir=os.path.join(td, 'dump'),
                     expect_present={fnv1a64(dxbc_a), fnv1a64(dxbc_d)},
                     expect_absent={fnv1a64(dxbc_c)}, expect_note=set())
        assert rc == 0, "cache-only verdict failed on a correct synthetic inline cache"
        assert sorted(os.listdir(os.path.join(td, 'dump'))) == \
            sorted([f"cs_{fnv1a64(dxbc_a):016x}.dxbc", f"cs_{fnv1a64(dxbc_d):016x}.dxbc"])
        # A count mismatch between the parallel arrays must fail loudly: drop one hash so the
        # stream stays aligned (1 hash read cleanly, then 2 entries) and the mismatch check
        # itself -- not a downstream bounds check -- is what fires.
        bad = inline_cache.replace(struct.pack('<i', 2) + sha_a + sha_d,
                                   struct.pack('<i', 1) + sha_a, 1)
        try:
            parse_global_cache(bad, 'corrupt-inline-cache')
            assert False, "accepted mismatched inline ShaderHashes/ShaderEntries"
        except SystemExit:
            pass
    print("SELF-TEST OK: fnv1a64 vectors, LZ4 decode (+corruption), section walk (+corruption), "
          "archive walk, inline cache-only walk (+corruption, +missing-archive), DXBC slicing, "
          "verdict logic")


def main(argv):
    if '--self-test' in argv:
        self_test()
        return 0
    emit_header_path = None
    if '--emit-header' in argv:
        i = argv.index('--emit-header')
        try:
            emit_header_path = argv[i + 1]
        except IndexError:
            die("--emit-header needs a file argument")
        del argv[i:i + 2]
    dump_dir = None
    if '--dump-dir' in argv:
        i = argv.index('--dump-dir')
        try:
            dump_dir = argv[i + 1]
        except IndexError:
            die("--dump-dir needs a directory argument")
        del argv[i:i + 2]
    if len(argv) not in (1, 2):
        print("usage: shaderlib_extract.py <GlobalShaderCache-PCD3D_SM5.bin> "
              "[<ShaderArchive-Global-PCD3D_SM5.ushaderbytecode>] [--dump-dir DIR] | --self-test\n"
              "(the archive is only needed for a cache cooked with shared shader code; Stray's "
              "is all-inline, so the single-file form is the normal one -- see the docstring)",
              file=sys.stderr)
        return 2
    try:
        return run(argv[0], argv[1] if len(argv) == 2 else None, dump_dir,
                   emit_header_path)
    except OSError as ex:
        die(f"cannot read input: {ex}")


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
