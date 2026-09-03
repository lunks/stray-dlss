#!/usr/bin/env python3
"""Parse EVERY cooked .uasset name map and build the complete vocabulary of the content.

This is the strongest available instrument for "does any shipped Blueprint refer to X": in a
cooked package the NameMap holds every name the package mentions -- import ObjectNames (which
is how EX_FinalFunction reaches a UFunction), EX_VirtualFunction's FName, property names, class
names, default-value struct names. The .uexp holds export BYTES only, and its bytecode addresses
names by index into this table, so a name absent here cannot be referenced there.
"""
import glob, os, sys, re, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from uassetnames import names

pats = [re.compile(p, re.I) for p in sys.argv[2:]]
vocab = collections.defaultdict(set)     # name -> set of files
nfiles = nfail = 0
fails = []
for p in sorted(glob.glob(os.path.join(sys.argv[1], '*.uasset'))):
    try:
        ns = names(p)
    except Exception as ex:
        nfail += 1; fails.append((os.path.basename(p), str(ex))); continue
    nfiles += 1
    b = os.path.basename(p)
    for n in ns:
        if not pats or any(r.search(n) for r in pats):
            vocab[n].add(b)

print(f"parsed {nfiles} name maps ({nfail} unparseable), {len(vocab)} distinct matching names\n")
for n, fs in sorted(vocab.items(), key=lambda kv: (-len(kv[1]), kv[0].lower())):
    ex = sorted(fs)[:3]
    more = f" +{len(fs)-3}" if len(fs) > 3 else ""
    print(f"{len(fs):6d}  {n}")
    if len(fs) <= 6:
        for f in sorted(fs):
            print(f"          {f}")
    else:
        for f in ex:
            print(f"          {f}")
        print(f"          ...{more} more")
if fails:
    print(f"\nUNPARSEABLE ({len(fails)}):")
    for f, e in fails[:10]:
        print(f"  {f}: {e}")
