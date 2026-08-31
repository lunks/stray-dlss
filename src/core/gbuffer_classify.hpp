// Classifying one draw's render-target set as UE 4.27's deferred base pass and naming its
// G-buffer members — identification for DLSS Ray Reconstruction's guide inputs. Log-only;
// the runtime recorder (src/gbuffer_finder.hpp) reports it and observation runs on the
// target machine judge it.
//
// SECOND ITERATION. The first anchored on an in-set velocity target and failed in live
// observation (2026-08-31): the game binds 6-RTV base-pass sets with NO velocity member.
// docs/RESEARCH-RR-GBUFFER.md §1.2-1.5 explains why the anchor was wrong twice over:
//
//   * `R16G16B16A16_UNORM` is the velocity format on every ray-tracing-capable shader
//     platform REGARDLESS of which pass writes it (HARD-via-mirror —
//     FVelocityRendering::GetFormat, VelocityRendering.cpp:354-358). The measured TAA
//     input format was evidence of the platform, never of base-pass velocity.
//   * Stray ships r.SelectiveBasePassOutputs=True (DefaultEngine.ini:85, HARD), which
//     forces the SEPARATE opaque velocity pass to run — one RTV plus depth, early-outs
//     when nothing moves (VelocityRendering.cpp:275-315, HARD-via-mirror) — and the live
//     game demonstrably runs a 6-RTV base pass with no velocity slot at all (HARD,
//     measured; a stock-vs-licensee delta §1.4 leaves unresolved).
//
// The corrected signature is velocity-FREE (§1.5). All provenance below is HARD-via-mirror
// from the UE 4.27.2 source (github.com/AlexMercer-MA/UnrealEngine-4.27, Build.version
// 4.27.2) unless marked otherwise; the two game settings are HARD from the game's own
// shipped config (docs/game-config/):
//
//   * MRT order (SceneRenderTargets.cpp:734-779, GetGBufferRenderTargets):
//       MRT0 SceneColor, MRT1 GBufferA, MRT2 GBufferB, MRT3 GBufferC, then SceneVelocity
//       at slot 4 ONLY when base-pass velocity is in effect — `check(OutVelocityRTIndex
//       == 4)` (:754) — then GBufferD, then GBufferE (bAllowStaticLighting; Stray ships
//       r.AllowStaticLighting=True, DefaultEngine.ini:38).
//   * scene colour: PF_FloatR11G11B10 — Stray ships r.SceneColorFormat=3
//     (WindowsEngine.ini:69, HARD). RGBA16F (the stock default 4) is accepted too.
//   * GBufferA: PF_A2B10G10R10 = RGB10A2 world normal (GetGBufferAFormat,
//     SceneRenderTargets.cpp:1026-1034). THE anchor: the only RGB10A2 render target in a
//     stock deferred frame — the swapchain is RGB10A2 as well, but at output extent and
//     never bound with 5 siblings (§1.5).
//   * GBufferB: PF_B8G8R8A8 (:1048-1056) — metallic, specular, roughness, shading model.
//   * GBufferC: PF_B8G8R8A8 with TexCreate_SRGB (:1058-1070, :1137) — base colour; the
//     recorder maps the sRGB view onto the same 8-bit class (frame_state.cpp).
//   * depth: the R32G8X24 family (HARD — measured, CLAUDE.md §2.4).
//
// B/C slot order (B=2, C=3) is HARD-via-mirror in stock source — but the two are
// format-indistinguishable and a licensee reorder is conceivable, so the ambiguity is
// still surfaced and the consumption cross-check (RESEARCH-RR-GBUFFER.md §4.3) or a
// visual guide dump must confirm it before anything trusts the values.
//
// A velocity-format member, when present, is CORROBORATION, not a requirement: at the
// stock slot 4 it upgrades confidence and shifts D/E one slot; anywhere else the layout
// is not assignable and the set is refused loudly.
//
// Pure logic: no Windows, no D3D, no ReShade. The runtime recorder translates real
// RTV/DSV bindings into BoundTexture and CI proves every discrimination
// (tests/test_gbuffer_classify.cpp) — the developer cannot run the game.
#pragma once

#include "taa_signature.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace stray_dlss {

enum class GBufferRole
{
	scene_colour,
	gbuffer_a, // world normal + per-object data
	gbuffer_b, // metallic / specular / roughness / shading-model ID
	gbuffer_c, // base colour (sRGB view; .a is NOT AO under static lighting — §2.4/§5.5)
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
	// True when the set carries the base-pass signature (header comment): at least
	// kMinBasePassColourTargets colour targets at one shared extent, slots 0-3 all
	// populated, RGB10A2 at slot 1 (the anchor), and any velocity member sitting at the
	// stock slot 4. Individual slots may still be `unknown` (an off-format licensee
	// variant) — the flag is about the SET, the roles are per-slot.
	bool is_base_pass = false;
	// Why the set was accepted or rejected. Always a static string; always set.
	const char *reason = "";
	// Every non-dummy member, in slot order, dummies (1x1) dropped. Populated even for
	// rejected sets, because the members ARE the diagnostic when the verdict is "no".
	std::vector<GBufferTarget> targets;
	// GBufferB and GBufferC are format-indistinguishable (both 8-bit RGBA), so the
	// B-before-C assignment rests on the stock slot order alone (HARD-via-mirror in stock
	// source, but a licensee reorder is conceivable). The report must say so: nothing
	// measured on this machine can tell them apart. (RESEARCH-RR-GBUFFER.md §1.5, §5.4)
	bool bc_order_by_slot_only = false;
	// True when a velocity-format target sits at the stock slot 4 — the CORROBORATION
	// case: the set then matches stock-with-base-pass-velocity exactly, and GBufferD/E
	// shift one slot. False in the velocity-free layout the live game shows.
	bool velocity_corroborated = false;
	// The shared colour-target extent — the scene-buffer extent. 0 until established.
	std::uint32_t extent_width = 0;
	std::uint32_t extent_height = 0;
	// How many of the four named colour roles (scene colour, A, B, C) matched their stock
	// format and were assigned. 4 is a clean stock layout.
	int stock_roles_matched = 0;
};

// The stock MRT slot of the velocity target WHEN base-pass velocity is in effect:
// `check(OutVelocityRTIndex == 4)`, SceneRenderTargets.cpp:754 (HARD-via-mirror). The
// live game binds no velocity member at all (§1.4) — this slot only matters for the
// corroboration case.
constexpr std::uint32_t kStockVelocitySlot = 4;

// The minimum colour-target count of a base-pass MRT set: SceneColor + GBufferA/B/C +
// GBufferD = 5 (GBufferD is unconditional under bUseGBuffer; velocity and GBufferE add
// more). The live game binds 6 (SC,A,B,C,D,E); stock-with-velocity predicts 7.
// (RESEARCH-RR-GBUFFER.md §1.2, §1.4)
constexpr std::size_t kMinBasePassColourTargets = 5;

// Classifies one render-target set (RTVs in slot order, optionally the DSV appended after
// them) as recorded at draw time. Never throws, never guesses: every rejection carries a
// reason and the members it did see.
GBufferClassification classify_render_target_set(const std::vector<BoundTexture> &outputs);

} // namespace stray_dlss
