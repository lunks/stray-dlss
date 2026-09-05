#!/usr/bin/env python3
"""Convert straydlss_*.bin input dumps (from [STRAYDLSS] NgxDumpInputs=1) to viewable PNGs.

Usage: rawdump2png.py <file.bin> <width> <height> <fmt> [out.png]
  fmt: rgba16f      colour / output dumps (straydlss_colour_*.bin, straydlss_output_*.bin)
       r32f         depth dumps           (straydlss_depth_*.bin)
       rgba16unorm  the ENGINE'S OWN sparse velocity (straydlss_velocity_raw_*.bin)
       rg16f        our resolved dense motion vectors (straydlss_mv_*.bin)
       rgba16f_normal  the RR normal+roughness guide (straydlss_rr_normals_*.bin) — prints the
                    DEGENERATE fraction, which is the one number that says whether the
                    engine-named G-buffer still had CONTENT when we read it
       rgba8        the RR albedo guides (straydlss_rr_diffuse_*.bin / _rr_specular_*.bin)
       r16f         the standalone RR roughness guide (straydlss_rr_roughness_*.bin)
       rgb10a2      the HUD-less copy (straydlss_hudless_*.bin) - the swapchain's own format;
                    the thing to check is that NO HUD element is in it

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
    elif fmt == "rgba16f_normal":
        # THE RR NORMAL+ROUGHNESS GUIDE (straydlss_rr_normals_*.bin): xyz is a SIGNED world-space
        # unit normal and w is linear roughness, so the plain rgba16f path — which scales by a
        # percentile and clips — would render every negative component as black and tell you
        # nothing. Shown as N*0.5+0.5, which is how a normal buffer is read by eye.
        #
        # THE NUMBER TO LOOK AT IS THE DEGENERATE FRACTION. gbuffer_resolve.hlsl falls back to
        # +Z whenever the decoded normal has no length, which is what a CLEARED or RECYCLED
        # GBufferA produces — and a 2026-08-31 measurement of the old heuristically-identified
        # G-buffers found exactly that at the TAA hook, ~95% of the frame. A high number here
        # says the pool named the right resource and its CONTENT was already gone; a low one
        # says the guides are real. Nothing else in the pipeline can distinguish those.
        img = rows_of(raw, w, h, 8).view(np.float16).reshape(h, w, 4).astype(np.float32)
        n = img[:, :, :3]
        rough = img[:, :, 3]
        length = np.sqrt((n * n).sum(axis=2))
        degenerate = np.isclose(n[:, :, 0], 0.0) & np.isclose(n[:, :, 1], 0.0) & (n[:, :, 2] > 0.99)
        rgb = np.clip(n * 0.5 + 0.5, 0.0, 1.0)
        Image.fromarray((rgb * 255).astype(np.uint8), "RGB").save(out)
        print(
            f"{out}: |N| mean={float(np.nanmean(length)):.4f} (want 1.0) "
            f"DEGENERATE (+Z fallback) {100.0 * float(degenerate.mean()):.2f}% of pixels; "
            f"roughness min={float(np.nanmin(rough)):.4f} max={float(np.nanmax(rough)):.4f} "
            f"mean={float(np.nanmean(rough)):.4f}"
        )
    elif fmt == "rgba8":
        # The RR albedo guides (straydlss_rr_diffuse_*.bin, straydlss_rr_specular_*.bin), LINEAR
        # RGBA8_UNORM. gbuffer_resolve.hlsl floors both at 0.05 and writes a flat 0.5 for
        # unlit/sky pixels, so a picture that is uniformly mid-grey is the shader saying it saw
        # no shading model — the same finding the normal dump's degenerate fraction reports.
        img = rows_of(raw, w, h, 4).reshape(h, w, 4).astype(np.float32) / 255.0
        rgb = np.clip(img[:, :, :3], 0.0, 1.0) ** (1 / 2.2)
        flat = np.isclose(img[:, :, :3], 0.5, atol=2.0 / 255.0).all(axis=2)
        Image.fromarray((rgb * 255).astype(np.uint8), "RGB").save(out)
        print(
            f"{out}: mean linear RGB = ({float(img[:, :, 0].mean()):.4f}, "
            f"{float(img[:, :, 1].mean()):.4f}, {float(img[:, :, 2].mean()):.4f}); "
            f"FLAT 0.5 (sky/unlit default) on {100.0 * float(flat.mean()):.2f}% of pixels"
        )
    elif fmt == "rgb10a2":
        # THE HUD-LESS COPY (straydlss_hudless_*.bin) and anything else in the swapchain's own
        # R10G10B10A2_UNORM (CLAUDE.md §2.1): display-referred already, so no tone curve is
        # applied here. The one thing to LOOK for is the absence of the HUD: the copy is taken
        # at Slate's LOAD pass before its first draw, so a HUD element in this image means the
        # copy landed after a UI draw (a carrier arrived late) or the pass was not the UI's.
        raw32 = rows_of(raw, w, h, 4).view(np.uint32).reshape(h, w)
        r = ((raw32 >> 0) & 0x3FF).astype(np.float32) / 1023.0
        g = ((raw32 >> 10) & 0x3FF).astype(np.float32) / 1023.0
        b = ((raw32 >> 20) & 0x3FF).astype(np.float32) / 1023.0
        rgb = np.stack([r, g, b], axis=2)
        Image.fromarray((np.clip(rgb, 0.0, 1.0) * 255).astype(np.uint8), "RGB").save(out)
        print(
            f"{out}: mean RGB = ({float(r.mean()):.4f}, {float(g.mean()):.4f}, {float(b.mean()):.4f}); "
            f"look for the HUD - it must NOT be in this image"
        )
    elif fmt == "r16f":
        img = rows_of(raw, w, h, 2).view(np.float16).reshape(h, w).astype(np.float32)
        lo, hi = float(np.nanmin(img)), float(np.nanmax(img))
        norm = (img - lo) / (hi - lo) if hi > lo else img * 0
        Image.fromarray((norm * 255).astype(np.uint8), "L").save(out)
        print(f"{out}: min={lo:.6f} max={hi:.6f} mean={float(np.nanmean(img)):.6f}")
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
