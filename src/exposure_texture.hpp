// A 1x1 R32_FLOAT exposure texture that WE own, for [STRAYDLSS] NgxExposure=owned.
//
// WHY OWN ONE. Under NgxExposure=texture we hand DLSS the ENGINE's eye-adaptation texture (TAA
// register t0). The audit says that is legitimate on every axis NVIDIA documents — the guide's
// §3.9 says "Only the first channel is sampled in the texture so multiple formats will work",
// so RGBA32F is fine, and §3.4 puts exposure in the same NON_PIXEL_SHADER_RESOURCE class as
// colour and depth, which is the state the game's own compute dispatch already left it in. The
// texel was measured healthy (~0.45, stable). So nothing about that texture is known to be
// wrong.
//
// What is wrong is the EXPERIMENT. "The texture mode is inert" is a null result, and the only
// instrument built to give it teeth — sweeping [STRAYDLSS] NgxExposureScale — tests
// DLSS.Exposure.Scale, a parameter with ZERO explanatory prose in the whole 84-page Programming
// Guide (revision 310.6.0; the name appears only in the parameter listing). A null result from
// an undocumented parameter is not evidence about the texture.
//
// The one perturbation that unambiguously tests whether the runtime READS the exposure texture
// is changing the number inside it, and that requires a texture we can write. Hence this file.
// It also removes, in one move, every remaining variable we do not control: the format is
// R32_FLOAT (the guide's own "something such as R16F is preferred" direction, single channel,
// no ambiguity about which one is sampled), the resource state is ours from creation and
// transitioned by us on both sides of the copy — never assumed — and the value is exactly what
// exposure::owned_texel_value computed.
//
// The value itself comes from View row 135.y, the one View row that validates itself on every
// read (CLAUDE.md §2.6), rather than from finding t0 — which is what "we are much closer to the
// engine now" buys concretely: the same number, from a source that self-checks, with no
// dependence on a binding surviving a permutation change.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::exposure_texture {

// Records the copy of `value` into our 1x1 R32_FLOAT on `cmd` and returns the texture, already
// transitioned back to NON_PIXEL_SHADER_RESOURCE — the state the DLSS Programming Guide §3.4
// requires for every D3D12 input ("the state must be D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_
// RESOURCE because those resources will be read by a compute shader").
//
// Allocates on first use. Returns nullptr if allocation fails; the caller must then treat the
// frame as having no exposure texture (exposure::EvalPlan::degraded) rather than substituting
// anything.
//
// The returned pointer is owned by this module and stays valid until release(). The caller must
// still AddRef it for its own keep-alive window if it hands it to NGX, exactly as it does for
// colour and depth: EvaluateFeature only RECORDS the work.
ID3D12Resource *update(ID3D12Device *device, ID3D12GraphicsCommandList *cmd, float value);

// The last value actually written, for the diagnostic line. NaN if nothing has been written.
float last_value();

// Frees the texture and the upload ring. GPU must be idle.
void release();

} // namespace stray_dlss::exposure_texture
