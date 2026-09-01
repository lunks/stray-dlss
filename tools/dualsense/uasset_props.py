#!/usr/bin/env python3
"""Read each USoundWave's authored Volume/Pitch out of its .uasset.

Raw waveform RMS is NOT the level the game plays: UE4 multiplies every SoundWave by
its own authored Volume before the submix chain. Parsing the name table lets us match
the property by NAME rather than scanning for plausible floats.
"""
import struct, sys, glob, os, json

def rd_fstring(b, o):
    n = struct.unpack_from('<i', b, o)[0]; o += 4
    if n == 0: return "", o
    if n < 0:
        s = b[o:o-2*n-2].decode('utf-16-le', 'replace'); o += -2*n
    else:
        s = b[o:o+n-1].decode('latin-1', 'replace'); o += n
    return s, o

def names(b):
    # UE4 package header: tag, legacy version, then version fields, then the name table.
    tag = struct.unpack_from('<I', b, 0)[0]
    if tag != 0x9E2A83C1: return None
    o = 4
    legacy = struct.unpack_from('<i', b, o)[0]; o += 4
    if legacy != -4: o += 4            # LegacyUE3Version
    o += 4                             # FileVersionUE4
    o += 4                             # FileVersionLicenseeUE4
    cc = struct.unpack_from('<i', b, o)[0]; o += 4   # CustomVersions
    o += cc * 20
    o += 4                             # TotalHeaderSize
    _, o = rd_fstring(b, o)            # FolderName
    o += 4                             # PackageFlags
    ncount, noff = struct.unpack_from('<ii', b, o)
    out, p = [], noff
    for _ in range(ncount):
        s, p = rd_fstring(b, p)
        p += 4                         # name hashes (UE4.27)
        out.append(s)
    return out

def find_float_prop(b, nt, prop):
    if prop not in nt: return None
    pi = nt.index(prop)
    try: fi = nt.index('FloatProperty')
    except ValueError: return None
    # property tag: nameIdx(4) nameNum(4) typeIdx(4) typeNum(4) size(8) arrayIdx(4) value(4)
    for o in range(0, len(b) - 32):
        if struct.unpack_from('<i', b, o)[0] != pi: continue
        if struct.unpack_from('<i', b, o + 4)[0] != 0: continue
        if struct.unpack_from('<i', b, o + 8)[0] != fi: continue
        size = struct.unpack_from('<q', b, o + 16)[0]
        if size != 4: continue
        return struct.unpack_from('<f', b, o + 28)[0]
    return None

rows = []
for f in sorted(glob.glob(sys.argv[1] + '/*.uasset')):
    b = open(f, 'rb').read()
    try: nt = names(b)
    except Exception: nt = None
    if not nt: continue
    name = os.path.basename(f).replace('Hk_project_Content_Sound_SFX_controllers_Vibrations_', '')[:-7]
    vol = find_float_prop(b, nt, 'Volume')
    pit = find_float_prop(b, nt, 'Pitch')
    rows.append((name, vol, pit))

print("%-38s %8s %8s" % ("asset", "Volume", "Pitch"))
seen = 0
for n, v, p in rows:
    if v is not None: seen += 1
    print("%-38s %8s %8s" % (n, "-" if v is None else "%.4f" % v, "-" if p is None else "%.3f" % p))
print("\n%d/%d assets carry an explicit Volume" % (seen, len(rows)))
json.dump({n: v for n, v, _ in rows if v is not None}, open(os.environ.get('WORK', '/tmp/scepad') + '/asset_volumes.json', 'w'), indent=1)
