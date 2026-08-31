#!/usr/bin/env python3
"""Convert straydlss_*.bin input dumps (from [STRAYDLSS] NgxDumpInputs=1) to viewable PNGs.

Usage: rawdump2png.py <file.bin> <width> <height> <fmt> [out.png]
  fmt: rgba16f (colour/output dumps) | r32f (depth dumps)

Row pitch is assumed to be width*bpp aligned up to 256 bytes, matching
D3D12_TEXTURE_DATA_PITCH_ALIGNMENT — the log line beside each capture records the real pitch.
Colour dumps are pre-exposed HDR; exposure here is a percentile normalisation for viewing, not
a faithful tonemap.
"""
import sys

import numpy as np
from PIL import Image


def align(v, a):
    return (v + a - 1) // a * a


def main():
    path, w, h, fmt = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    out = sys.argv[5] if len(sys.argv) > 5 else path.rsplit(".", 1)[0] + ".png"
    raw = open(path, "rb").read()
    bpp = 8 if fmt == "rgba16f" else 4
    pitch = align(w * bpp, 256)
    rows = np.frombuffer(raw[: pitch * h], dtype=np.uint8).reshape(h, pitch)[:, : w * bpp]
    if fmt == "rgba16f":
        img = rows.view(np.float16).reshape(h, w, 4).astype(np.float32)
        rgb = img[:, :, :3]
        scale = np.percentile(rgb[np.isfinite(rgb)], 99.5) or 1.0
        rgb = np.clip(rgb / scale, 0.0, 1.0) ** (1 / 2.2)
        Image.fromarray((rgb * 255).astype(np.uint8), "RGB").save(out)
        print(f"{out}: 99.5pct={scale:.5f} max={np.nanmax(img[:, :, :3]):.5f}")
    elif fmt == "r32f":
        img = rows.view(np.float32).reshape(h, w)
        lo, hi = np.nanmin(img), np.nanmax(img)
        norm = (img - lo) / (hi - lo) if hi > lo else img * 0
        Image.fromarray((norm * 255).astype(np.uint8), "L").save(out)
        print(f"{out}: min={lo:.6f} max={hi:.6f} mean={np.nanmean(img):.6f}")
    else:
        sys.exit(f"unknown fmt {fmt}")


if __name__ == "__main__":
    main()
