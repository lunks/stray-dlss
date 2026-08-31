// Classifying one draw's render-target set as UE 4.27's deferred base pass and naming its
// G-buffer members. Phase 1 of DLSS Ray Reconstruction: IDENTIFICATION ONLY — nothing acts
// on the result; the runtime recorder (src/gbuffer_finder.hpp) logs it and a later
// observation run on the target machine judges it.
//
// The anchor is the velocity target. Stray ships r.BasePassOutputsVelocity=True
// (HARD — the game's own DefaultEngine.ini:59, CLAUDE.md §2.3.1), so the base pass's MRT
// set contains the R16G16B16A16_UNORM velocity target (HARD — measured live, CLAUDE.md
// §2.5) alongside scene colour and the G-buffers. No other pass in a deferred UE4 frame
// renders to a wide MRT set containing that format, which makes it a strong base-pass
// signature; the sibling render targets, in slot order, are then the G-buffers.
//
// The slot order and the per-slot formats below are EXPECTED values, not gates on the set
// as a whole: a slot whose format does not fit its stock role classifies as `unknown` and
// is reported, never silently misclassified (CLAUDE.md §0.2 — prefer a loud failure to a
// quiet wrong answer). Only the velocity signature itself decides whether the set is a
// base-pass candidate at all.
//
// Provenance of the stock layout, and an honest caveat: every claim marked SOFT below is
// from prior reading of the UE 4.27 renderer source
// (Engine/Source/Runtime/Renderer/Private/SceneRenderTargets.cpp) — that source is not on
// this machine to re-read, so functions are cited but line numbers are not. A licensee
// build can differ in any of them. The observation run is what hardens or overturns each:
//
//   * MRT order (SOFT — FSceneRenderTargets::GetGBufferRenderTargets):
//       MRT0 scene colour, MRT1 GBufferA, MRT2 GBufferB, MRT3 GBufferC, then the velocity
//       target inserted when using base-pass velocity, then GBufferD / GBufferE.
//   * scene colour  (SOFT — GetSceneColorFormat / r.SceneColorFormat, default 4):
//       PF_FloatRGBA = RGBA16F, or PF_FloatR11G11B10 under r.SceneColorFormat=3.
//   * GBufferA      (SOFT — GetGBufferAFormat): PF_A2B10G10R10 = RGB10A2 world normal +
//       per-object data in A; PF_FloatRGBA under high-precision G-buffers.
//   * GBufferB      (SOFT — GetGBufferBFormat): PF_B8G8R8A8 — metallic, specular,
//       roughness, shading-model ID.
//   * GBufferC      (SOFT — GetGBufferCFormat): PF_B8G8R8A8 — base colour + AO.
//   * velocity      (HARD for Stray — measured R16G16B16A16_UNORM, CLAUDE.md §2.5;
//       consistent with SOFT FVelocityRendering::GetFormat returning PF_A16B16G16R16
//       when base-pass velocity is enabled).
//   * depth         (HARD — R32G8X24 family, CLAUDE.md §2.4).
//
// Pure logic: no Windows, no D3D, no ReShade. The runtime recorder translates real RTV/DSV
// bindings into BoundTexture and the tests prove every discrimination in CI
// (tests/test_gbuffer_classify.cpp) — the developer cannot run the game.
#pragma once

#include "taa_signature.hpp"

#include <cstdint>
#include <vector>

namespace stray_dlss {

enum class GBufferRole
{
	scene_colour,
	gbuffer_a, // world normal + per-object data
	gbuffer_b, // metallic / specular / roughness / shading-model ID
	gbuffer_c, // base colour + AO
	velocity,
	depth,     // the DSV, when the recorder captured one
	unknown,   // present in the set but not confidently nameable — reported, never guessed
};

const char *gbuffer_role_name(GBufferRole role);

// A log-friendly name for a TexFormat, matching the spellings the measured facts use.
const char *tex_format_name(TexFormat f);

// One member of a classified render-target set.
struct GBufferTarget
{
	GBufferRole role = GBufferRole::unknown;
	// The binding as recorded: slot is the RTV index (the recorder appends the DSV at
	// slot = RTV count), resource/format/extent as described at bind time.
	BoundTexture tex;
	// True when the format matches the stock UE 4.27 expectation for the assigned role.
	// Always false for `unknown` — that is what unknown means.
	bool stock_format = false;
	// Static string naming the deviation when the slot did not classify cleanly; "" when
	// there is nothing to say.
	const char *note = "";
};

struct GBufferClassification
{
	// True when the set carries the base-pass signature: exactly one full-extent velocity
	// target, at the stock slot, with the four stock G-buffer slots ahead of it at the same
	// extent. Individual slots may still be `unknown` (a licensee format) — the flag is
	// about the SET, the roles are per-slot.
	bool is_base_pass = false;
	// Why the set was accepted or rejected. Always a static string; always set.
	const char *reason = "";
	// Every non-dummy member, in slot order, dummies (1x1) dropped. Populated even for
	// rejected sets, because the members ARE the diagnostic when the verdict is "no".
	std::vector<GBufferTarget> targets;
	// GBufferB and GBufferC are format-indistinguishable (both 8-bit RGBA), so when both
	// classify, the B-before-C assignment rests on the SOFT stock slot order ALONE. The
	// report must say so: nothing measured on this machine can tell them apart.
	bool bc_order_by_slot_only = false;
	// The velocity target's extent — the scene-buffer extent (CLAUDE.md §2.5). 0 until a
	// velocity target is found.
	std::uint32_t extent_width = 0;
	std::uint32_t extent_height = 0;
	// How many of the four named colour roles (scene colour, A, B, C) matched their stock
	// format and were assigned. 4 is a clean stock layout.
	int stock_roles_matched = 0;
};

// The stock MRT slot of the velocity target when base-pass velocity is enabled: after
// scene colour + GBufferA/B/C, before GBufferD/E. (SOFT — GetGBufferRenderTargets, see the
// header comment; Stray ships base-pass velocity, HARD §2.3.1.)
constexpr std::uint32_t kStockVelocitySlot = 4;

// Classifies one render-target set (RTVs in slot order, optionally the DSV appended after
// them) as recorded at draw time. Never throws, never guesses: every rejection carries a
// reason and the members it did see.
GBufferClassification classify_render_target_set(const std::vector<BoundTexture> &outputs);

} // namespace stray_dlss
