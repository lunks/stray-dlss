# DLSS injection into Stray — design

**Status:** design, agreed constraints, not yet implemented beyond the skeleton.
**Sources:** `CLAUDE.md` (measured facts), `docs/RESEARCH.md` (verified external research).

---

## 1. Goal

Replace Stray's `FTAAStandaloneCS` compute dispatch with an NVIDIA DLSS evaluation, from
outside the engine, as a ReShade D3D12 add-on running under Proton/vkd3d-proton.

Two milestones:

* **DLAA** — render resolution equals output resolution. Proves colour, depth, motion vectors
  and jitter are all correct without introducing any resolution plumbing.
* **SR** — the engine renders smaller via `r.ScreenPercentage` + `r.TemporalAA.Upsampling=1`,
  and DLSS upscales.

## 2. The governing constraint

The developer cannot run the game. The only feedback is the user installing an artifact and
pasting back a log or a screenshot. **Therefore the work is ordered so that each risky
assumption is validated by a real run before anything is built on top of it**, and every phase
is independently useful.

This is the single most important structural decision in this document. It is why observation
comes before computation and computation comes before replacement, rather than building the
whole pipeline and testing once at the end.

## 3. Phases

| Phase | What ships | What the field test answers |
|---|---|---|
| **A — Observe** | Capture and log everything about the TAA dispatch. Rendering is untouched. | Does the hash match? Which of t5/t6 is scene colour? What are t1/t3/t4? Is the View CB where we think? Which descriptor path carries b1? |
| **B — Resolve** | Run our own MV-resolve compute pass into a debug texture. Rendering still untouched. | Does the dense motion-vector field look correct in a debug view? |
| **C — Replace** | NGX evaluate into u0, state restore, u1 downsample. DLAA. | Does the image look right? |
| **D — Upscale** | `Engine.ini` screen percentage + the `MainUpsampling` permutation. SR presets. | Does it gain performance without artefacts? |

A phase is not started until the previous phase's field test has come back clean.

## 4. Architecture

### 4.1 Interception

```
init_pipeline    → fnv1a64 the compute DXBC → remember pipeline handle -> hash
bind_pipeline    → remember the current compute pipeline for this command list
                   (fires with pipeline_stage::all in D3D12, NOT compute_shader)
push_descriptors ┐
bind_descriptor_tables ┴→ remember bound SRVs / UAVs / CBs by register
dispatch         → is this the TAA pass?  identity = hash match AND structural match
                   yes → do our work, return true (suppresses the engine's dispatch)
                   no  → return false
```

**Identity is never hash-only.** Stray's TAA hash moves with the in-game AA quality setting
(`r.PostProcessAAQuality`, currently 3), so the structural signature — six SRVs with the
expected formats, one or two UAVs, a View CB at b1, and a dispatch size of
`ceil(viewrect/8)` — is what actually gates interception. The hash is a fast pre-filter and a
strong confirmation, not the decision.

`0x901e041a7cadc9db` is a known false positive and is hard-excluded.
`0x52101a15e1a0c5cc` is almost certainly motion blur and is never hooked.

### 4.2 Resource identification

Rules, all from measurement or verified research:

* **Depth vs stencil**: two SRVs over the *same* `ID3D12Resource`. The
  `R32_FLOAT_X8X24_TYPELESS` view is depth; the `X32_TYPELESS_G8X24_UINT` view is stencil.
* **Scene colour vs history**: cache the `ID3D12Resource*` bound at u0 each frame. Next frame,
  the `R16G16B16A16_FLOAT` SRV matching that pointer is the history; **the other is the scene
  colour**. Register order is not used, because it was explicitly refuted.
* **Reset**: OR of `View.CameraCut != 0`, `TemporalAAJitter.zw == .xy`, and "history or
  velocity SRV is a 1×1 texture".

### 4.3 The View constant buffer

Read inside the `dispatch` callback — never deferred — with
`map_buffer_region(..., map_access::read_only, ...)`. One contiguous 2448-byte copy out, then
unmap. Both descriptor paths (root CBV via `push_descriptors`, and descriptor table via
`bind_descriptor_tables`) are registered; the log records which one actually fires.

Decoding the copied bytes into a struct is **pure logic** and is unit-tested against a
synthetic buffer.

### 4.4 The motion-vector resolve pass

Mandatory for two independent reasons: Stray's velocity is sparse, and its
`R16G16B16A16_UNORM` format is not one DLSS accepts.

Per pixel, following NVIDIA's own `VelocityCombine.usf`:

```
if (EncodedVelocity.x > 0.0)            // strict, red channel only — UE4's own test
    v = DecodeVelocityFromTexture(...)  // NDC delta, jitter-free
else
    v = ThisScreen - PrevScreen         // from reversed-Z depth + ClipToPrevClip
out = -(v * float2(0.5 * RenderW, -0.5 * RenderH))   // render-res pixels, backward-pointing
```

Output is `R16G16_FLOAT` at render resolution, and `InMVScaleX/Y = (1, 1)`.

**Implemented in raw D3D12**, not through ReShade's abstraction: we have to save and restore
command-list state around NGX anyway, so owning our own descriptor heap, root signature and
PSO is simpler and more predictable than interleaving with ReShade's heap management.

The shader is **compiled to DXBC at build time** on the CI runner. `D3DCompile` is never called
at runtime — `d3dcompiler_47` is routinely absent from a Proton prefix, and that is the single
most common Linux failure mode for tools of this shape.

### 4.5 NGX evaluation

Output goes **directly into u0**, which is already a UAV of the right format, so the engine
picks it up as next frame's `HistoryBuffer[0]` with no copy. Guarded: if the scene-colour
resource ever equals the u0 resource, fall back to a scratch texture and log it, because DLSS
must not read and write the same resource.

Feature flags: `IsHDR | MVLowRes | DepthInverted | AutoExposure` (`0x4B`). Preset **K**.
Quality mode is derived from `NGX_DLSS_GET_OPTIMAL_SETTINGS` rather than chosen.

### 4.6 State restoration

NGX clobbers D3D12 command-list state and the game will not re-set it. D3D12 has no state
getters, so the state must be *tracked* from ReShade's own events and re-applied. ReShade ships
exactly this: `examples/utils/state_tracking.cpp` (`state_block` with `capture()`/`apply()`) and
`examples/utils/descriptor_tracking.cpp`. Both are copied verbatim rather than reimplemented.

### 4.7 The u1 obligation

When `TAA_DOWNSAMPLE` is active — which it is, since Stray runs `r.PostProcessAAQuality == 3` —
the engine's TAA also writes a half-resolution `OutComputeTexDownsampled`, consumed downstream
by bloom and depth of field. **Skipping the dispatch without producing u1 leaves those passes
reading garbage.** A second small compute pass produces it from our DLSS output.

`r.TemporalAA.AllowDownsampling=0` in `Engine.ini` removes u1 entirely and is offered as a
simplification, but the add-on must be correct without it.

### 4.8 Failure policy

**Any failure returns `false` from the dispatch callback and lets the engine's own TAA run.**
No black screens, no half-composited frames. Every fallback logs loudly, once, with the reason.
This is what makes the artifact safe for the user to leave installed while we iterate.

## 5. File structure

```
src/core/          pure logic, no Windows/D3D/ReShade, built and tested on Linux too
  fnv1a.hpp          DXBC hashing                                    [exists]
  ue4_view.hpp/cpp   View CB offsets, velocity codec, jitter, Halton [exists]
  view_params.hpp/cpp decode the 2448-byte View prefix into a struct
  taa_signature.hpp/cpp structural matcher for the TAA dispatch
  dlss_settings.hpp/cpp quality-mode selection from optimal settings

src/               Windows-only
  addon.cpp          entry, event registration, config, overlay      [exists]
  log.hpp/cpp        dual-sink logging                               [exists]
  ngx_backend.hpp/cpp NGX lifecycle, feature create/evaluate         [exists, init only]
  taa_hook.hpp/cpp   identification and interception orchestration
  frame_state.hpp/cpp per-command-list captured bindings
  mv_resolve.hpp/cpp  the resolve and downsample compute passes
  d3d12_util.hpp/cpp  descriptor heap, PSO and barrier helpers

third_party/reshade_utils/  descriptor_tracking, state_tracking (verbatim from ReShade)
shaders/            mv_resolve.hlsl, downsample.hlsl, compiled to DXBC at build time
```

## 6. Non-goals

Ray Reconstruction, frame generation, sharpening (deprecated in NGX), non-NVIDIA hardware,
Windows support, and any attempt to make DLSS work where `NVSDK_NGX_D3D12_Init` fails.
