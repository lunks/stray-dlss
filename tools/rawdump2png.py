#!/usr/bin/env python3
"""Convert straydlss_*.bin input dumps (from [STRAYDLSS] NgxDumpInputs=1) to viewable PNGs.

Usage: rawdump2png.py <file.bin> <width> <height> <fmt> [out.png]
  fmt: rgba16f      colour / output dumps (straydlss_colour_*.bin, straydlss_output_*.bin)
       r32f         depth dumps           (straydlss_depth_*.bin)
       rgba16unorm  the ENGINE'S OWN sparse velocity (straydlss_velocity_raw_*.bin)
       rg16f        our resolved dense motion vectors (straydlss_mv_*.bin)

Width, height, format and row pitch are all in the `input_dump: captured ...` log line beside
each capture — read them off it rather than guessing.

Row pitch is assumed to be width*bpp aligned up to 256 bytes, matching
D3D12_TEXTURE_DATA_PITCH_ALIGNMENT. Colour dumps are pre-exposed HDR; exposure here is a
percentile normalisation for viewing, not a faithful tonemap.

WHY rgba16unorm MATTERS, and it is not a colour picture. UE 4.27 writes GBufferVelocity only
where a primitive is judged to be moving; everything else is left at the clear value, and UE's
own test for "this pixel has an object velocity" is the strict `EncodedVelocity.x > 0`
(CLAUDE.md §2.5). Our resolve takes that branch or reconstructs the camera's motion from depth,
and the two are indistinguishable in the resolved field — a plausible vector comes out either
way. So this mode writes TWO images:

  <out>              the decoded velocity, |v| as brightness (black where there is none)
  <out stem>_mask    the VALIDITY MASK: white where the engine wrote a velocity, black where
                     our resolve had to reconstruct one

and prints the coverage percentage. Open the mask beside the colour dump of the same evaluate:
an object that is black in the mask while visibly moving in the colour frame is being handed
the motion of a static world point at its depth, which is the wrong answer for anything that
moves relative to the camera.
"""
import sys

import numpy as np
from PIL import Image

# UE 4.27 Common.ush:1537-1570, the same constants shaders/mv_resolve.hlsl computes. The bias is
# 32767/65535, not 0.5 (CLAUDE.md §2.5).
INV_DIV = 1.0 / (0.499 * 0.5)
DECODE_BIAS = (32767.0 / 65535.0) * INV_DIV


def align(v, a):
    return (v + a - 1) // a * a


def rows_of(raw, w, h, bpp):
    pitch = align(w * bpp, 256)
    return np.frombuffer(raw[: pitch * h], dtype=np.uint8).reshape(h, pitch)[:, : w * bpp]


def save_magnitude(mag, out, label):
    hi = float(np.nanpercentile(mag, 99.5)) or 1.0
    img = np.clip(mag / hi, 0.0, 1.0)
    Image.fromarray((img * 255).astype(np.uint8), "L").save(out)
    print(f"{out}: {label} 99.5pct={hi:.6f} max={float(np.nanmax(mag)):.6f}")


def main():
    path, w, h, fmt = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    out = sys.argv[5] if len(sys.argv) > 5 else path.rsplit(".", 1)[0] + ".png"
    raw = open(path, "rb").read()
    if fmt == "rgba16f":
        img = rows_of(raw, w, h, 8).view(np.float16).reshape(h, w, 4).astype(np.float32)
        rgb = img[:, :, :3]
        scale = np.percentile(rgb[np.isfinite(rgb)], 99.5) or 1.0
        rgb = np.clip(rgb / scale, 0.0, 1.0) ** (1 / 2.2)
        Image.fromarray((rgb * 255).astype(np.uint8), "RGB").save(out)
        print(f"{out}: 99.5pct={scale:.5f} max={np.nanmax(img[:, :, :3]):.5f}")
    elif fmt == "r32f":
        img = rows_of(raw, w, h, 4).view(np.float32).reshape(h, w)
        lo, hi = np.nanmin(img), np.nanmax(img)
        norm = (img - lo) / (hi - lo) if hi > lo else img * 0
        Image.fromarray((norm * 255).astype(np.uint8), "L").save(out)
        print(f"{out}: min={lo:.6f} max={hi:.6f} mean={np.nanmean(img):.6f}")
    elif fmt == "rgba16unorm":
        # The engine's own GBufferVelocity, undecoded. UNORM16 -> [0,1].
        enc = rows_of(raw, w, h, 8).view(np.uint16).reshape(h, w, 4).astype(np.float32) / 65535.0
        # UE's own validity test, reproduced exactly: strict, red channel only, because the
        # target is cleared to 0. A magnitude threshold would be wrong.
        mask = enc[:, :, 0] > 0.0
        coverage = 100.0 * float(mask.mean())
        stem = out.rsplit(".", 1)[0]
        mask_out = stem + "_mask.png"
        Image.fromarray((mask * 255).astype(np.uint8), "L").save(mask_out)
        vel = enc[:, :, :2] * INV_DIV - DECODE_BIAS
        vel[~mask] = 0.0
        save_magnitude(np.hypot(vel[:, :, 0], vel[:, :, 1]), out, "|object velocity| (NDC)")
        print(
            f"{mask_out}: ENGINE-WRITTEN VELOCITY COVERAGE {coverage:.2f}% of pixels "
            f"(white = decoded object motion, black = our resolve reconstructed camera motion "
            f"from depth)"
        )
    elif fmt == "rg16f":
        # Our resolved dense field: render-resolution PIXELS, [0,0] upper-left, pointing back.
        mv = rows_of(raw, w, h, 4).view(np.float16).reshape(h, w, 2).astype(np.float32)
        mag = np.hypot(mv[:, :, 0], mv[:, :, 1])
        save_magnitude(mag, out, "|resolved motion| (render-res pixels)")
        print(
            f"  dx range [{float(np.nanmin(mv[:, :, 0])):.3f}, "
            f"{float(np.nanmax(mv[:, :, 0])):.3f}]  "
            f"dy range [{float(np.nanmin(mv[:, :, 1])):.3f}, "
            f"{float(np.nanmax(mv[:, :, 1])):.3f}]"
        )
    else:
        sys.exit(f"unknown fmt {fmt}")


if __name__ == "__main__":
    main()
