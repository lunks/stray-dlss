#!/usr/bin/env python3
"""One-command TAA hash regeneration for a game update, producing stray-dlss-hashes.txt.

Usage: stray_taa_hashes.py <Hk_project-WindowsNoEditor.pak> <path-to-oozraw> [-o OUT.txt]

Chains the whole offline pipeline: pakextract --raw (the shader cache is Oodle-compressed at
the pak level) -> oodle_unblock (through an ooz build; see the Gotchas ledger in CLAUDE.md for
building oozraw) -> shaderlib_extract --emit-hashes. Drop the output beside stray-dlss.addon64
as `stray-dlss-hashes.txt` and press "Reload hash file" in the ReShade overlay (or restart).
"""
import os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def main(argv):
    out = "stray-dlss-hashes.txt"
    if "-o" in argv:
        i = argv.index("-o")
        out = argv[i + 1]
        del argv[i:i + 2]
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    pak, oozraw = argv
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([sys.executable, os.path.join(HERE, "pakextract.py"), pak, tmp,
                        "GlobalShaderCache-PCD3D_SM5", "--raw"], check=True)
        base = next(os.path.join(tmp, f[:-4]) for f in os.listdir(tmp) if f.endswith(".raw"))
        cache = os.path.join(tmp, "GlobalShaderCache-PCD3D_SM5.bin")
        subprocess.run([sys.executable, os.path.join(HERE, "oodle_unblock.py"),
                        base + ".json", base + ".raw", cache, oozraw], check=True)
        r = subprocess.run([sys.executable, os.path.join(HERE, "shaderlib_extract.py"),
                            cache, "--emit-hashes", out])
        return r.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
