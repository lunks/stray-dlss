// DLSS Super Resolution's exposure contract, as pure decisions. Tested; no D3D12 here.
//
// WHY THIS FILE EXISTS. "The NGX exposure-texture path measured inert" has been carried as a
// conclusion since 2026-08-31. The evidence behind it is real but PARTIAL, and it is worth
// stating precisely because the whole design below follows from the gap:
//
//   * MEASURED (commit 5d848d3, live on the box): with NgxExposure=texture the create flags
//     really were 0x0b (AutoExposure dropped), exactly one feature was created, the eye-
//     adaptation finder never missed, and the SR evaluate really did carry a non-null
//     pInExposureTexture every frame. And yet the DLSS on-screen indicator read
//     "Auto Exposure: ON" in BOTH modes and the image was visually identical.
//   * MEASURED (commit 17265f2): the texel itself is healthy — 1x1 RGBA32F, .x ~ 0.45, stable,
//     tracking InPreExposure frame by frame. So "DLSS rejected a bad VALUE" is ruled out.
//   * NEVER MEASURED: the functional consume test. `NgxExposureScale` was built in 17265f2
//     precisely to settle it (feed 0.25 / 1.0 / 4.0 and watch the image) and there is no record
//     — no log excerpt, no screenshot, no note in docs/STRAY-RENDERING-FACTS.md — of that sweep
//     ever being run. "Inert" therefore rests on a NULL RESULT from an instrument whose
//     sensitivity was never established.
//
// THE LEADING EXPLANATION IS THE PRESET GATE below, and it is documented rather than guessed:
// exposure input is supported by presets J and K only, and this title's shipped 50% screen
// percentage puts a default-preset session on M. Read that section next; it is the one thing
// here that would make every measurement above come out exactly as it did.
//
// `Mode::owned` exists for what the preset gate does NOT settle. Two things survive it:
//
//   1. STATE. The official plugin does not ASSUME the exposure texture's resource state, it
//      TRANSITIONS to it — `RHITransitionResource(CmdList, InTexture,
//      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ALL_SUBRESOURCES)` inside the same
//      GetResidentD3D12Resource helper that resolves pInExposureTexture (NGXD3D12RHI.cpp,
//      plugin v8.3.0-NGX310.4.0). UE's RHI knows the real current state, so that is a correct
//      transition. We cannot reproduce it on a resource the GAME owns — D3D12 cannot be asked
//      for a resource's state and a guessed StateBefore is the hazard CLAUDE.md §5 documents —
//      so under NgxExposure=texture we ASSUME, and under NgxExposure=owned we transition.
//   2. PERTURBABILITY. The only sound test of "does the runtime read this texture" is changing
//      the number inside it, which requires a texture we can write. The instrument built for
//      that job in 17265f2 — sweeping DLSS.Exposure.Scale — cannot do it: that parameter has
//      zero explanatory prose in the entire Programming Guide, and the official plugin never
//      sets it at all (repo-wide search: zero hits for InExposureScale). A null result from an
//      undocumented parameter that the reference integration does not even use is not evidence
//      about the texture.
//
// FORMAT and VALUE are NOT reasons, and saying so matters: the guide settles format ("Only the
// first channel is sampled in the texture so multiple formats will work", §3.9) and the texel
// dump settled value. The engine texture is legitimate on both.
#pragma once

#include "dlss_quality.hpp"

namespace stray_dlss::exposure {

// [STRAYDLSS] NgxExposure.
enum class Mode
{
	// DLSS estimates exposure itself. NVSDK_NGX_DLSS_Feature_Flags_AutoExposure is set at
	// CreateFeature and no exposure texture is passed. This is the official UE plugin's own
	// DEFAULT (r.NGX.DLSS.AutoExposure defaults to 1, DLSSUpscaler.cpp:84-88 — "Enable DLSS
	// internal auto-exposure instead of the application provided one (default)"), and NVIDIA
	// describes the engine-exposure path as the fallback "in some cases this may reduce
	// artifacts", not as the primary. HARD, docs/RESEARCH-OFFICIAL-DLSS-UE-PLUGIN.md.
	automatic,

	// The engine's own eye-adaptation texture (TAA register t0, 1x1 RGBA32F) is passed as
	// pInExposureTexture and the AutoExposure flag is dropped, mirroring the plugin's
	// bUseAutoExposure=false path (NGXRHI.cpp:550-565 keys only the flag off the mode;
	// NGXD3D12RHI.cpp:275-276 nulls only the texture under auto).
	engine_texture,

	// Our own 1x1 R32_FLOAT, written and barriered by us, carrying `View.PreExposure`
	// (row 135.y — the one View row that validates itself on every read, CLAUDE.md §2.6)
	// times NgxExposureValue. Same flags as engine_texture; the ONLY difference is who owns
	// the resource. See the file header for why that difference is the entire point.
	owned_texture,
};

// Parses [STRAYDLSS] NgxExposure. Anything unrecognised — including null — is `automatic`,
// which is the shipped default and the plugin's own default, so a typo degrades to the safe
// behaviour rather than to a half-configured one.
Mode parse_mode(const char *value);
const char *mode_name(Mode mode);

// True when the mode wants an exposure texture at all.
bool wants_texture(Mode mode);

// NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6 (nvsdk_ngx_defs.h:297). Duplicated here
// rather than included so this stays buildable on Linux; tests/test_exposure_plan.cpp pins the
// value, and src/ngx_backend.cpp static_asserts it against the real header.
constexpr unsigned int kAutoExposureFlag = 1u << 6;

// The exposure mode's contribution to InFeatureCreateFlags. CREATION-TIME: DLSS cannot be told
// per frame to fall back to auto, so a mode that loses its texture mid-session gets a default
// exposure, never the auto path.
unsigned int create_flag_bits(Mode mode);

// nvsdk_ngx_helpers.h:507-508 rewrites BOTH exposure floats on the way into the parameter
// block: `x == 0.0f ? 1.0f : x`. A zero is therefore never "unset", it is the literal value 1.0
// — which for InPreExposure means "the colour buffer carries no pre-exposure at all". Anything
// that can produce a 0 here must do so deliberately.
float effective_pre_exposure(float in_pre_exposure);
float effective_exposure_scale(float in_exposure_scale);

// InPreExposure for the SR evaluate.
//
// THE GUARD THIS ADDS, and it is a real hole: the SR path passes View row 135.y straight to NGX
// with no plausibility check, while the NR path has always gated the same row through
// ue4::pre_exposure_plausible (src/taa_hook.cpp:2140). A row-135 misread therefore reaches DLSS
// silently — and if it reads 0, helpers.h:507 turns it into 1.0, telling DLSS the buffer is not
// pre-exposed when it is (measured ~0.45). Un-baking by 1.0 instead of by 0.45 is a ~2.2x error
// in the network's input domain that returns no error and looks like nothing in particular.
//
// So: pass the row's value only when the row validated; otherwise return exactly 1.0 — the same
// number the helper would have produced, but chosen on purpose and reportable.
float sr_pre_exposure(float view_pre_exposure, bool pre_exposure_ok);

// The float written into the OWNED 1x1 R32_FLOAT texel.
//
// VALUE SEMANTICS. nvsdk_ngx_defs.h:766 defines ExposureTexture as "a 1x1 texture containing the
// final exposure scale". UE 4.27 writes that quantity into eye adaptation as
// `OutColor.x = MiddleGreyExposureCompensation * SmoothedExposureScale`
// (PostProcessEyeAdaptation.usf:95-112), and sets `View.PreExposure =
// View.GetLastEyeAdaptationExposure()` when r.UsePreExposure is on
// (PostProcessEyeAdaptation.cpp:917-990). So row 135.y IS the same quantity, one frame stale —
// which our own texel dump corroborated live (texel .x ~ 0.45 tracking InPreExposure ~ 0.45,
// commit 17265f2). One frame of staleness on a value UE deliberately smooths over ~1s is not a
// meaningful error, and it buys independence from ever finding t0.
//
// `multiplier` is [STRAYDLSS] NgxExposureValue and is the CONSUME TEST: it is not a tuning knob,
// it is the deliberately-wrong number. 1.0 is the honest value. A non-plausible row falls back
// to 1.0 before the multiplier, so the test still perturbs something known.
//
// Clamped to (0, 1e6]: a zero or negative exposure is what DLSS treats as invalid and is exactly
// the failure the texel dump ruled out — we must not reintroduce it from our own side.
float owned_texel_value(float view_pre_exposure, bool pre_exposure_ok, float multiplier);

// ---------------------------------------------------------------------------------------
// THE PRESET GATE. This is the single most important thing in this file.
//
// DLSS Programming Guide, document revision 310.6.0 (NVIDIA/DLSS @ main,
// doc/DLSS_Programming_Guide_Release.pdf), §3.9 "Exposure", opening line:
//
//     "Only supported by Presets J and K. Preset L always uses AutoExposure."
//
// and the v310.5.0 changelog entry that added it: "Updated Section 3.9 Exposure input is only
// supported by Presets J and K." HARD.
//
// So the application-supplied exposure path is PRESET-GATED, and on a preset outside {J, K} the
// runtime uses its own auto-exposure NO MATTER WHAT the create flag says or what texture is
// passed. That is, exactly, the symptom this project measured and recorded as "inert": the flag
// cleared, a healthy texture passed every frame, and the on-screen indicator still reading
// "Auto Exposure: ON".
//
// The trap is that the preset is usually not chosen explicitly. NVSDK_NGX_DLSS_Hint_Render_Preset
// _Default (0) resolves PER QUALITY MODE, from the header's own comments (nvsdk_ngx_defs.h:82-85):
//
//     K = "Default preset for DLAA/Balanced/Quality modes"      -> exposure SUPPORTED
//     L = "Default for Ultra Perf mode"                         -> exposure NOT supported (HARD)
//     M = "Default for Perf mode"                               -> exposure NOT supported (SOFT)
//
// M is SOFT because the guide names only L explicitly; "only supported by Presets J and K" is
// what excludes M, which is an inference from an exclusive list rather than a per-preset
// statement. Treated as unsupported anyway — being wrong that way costs a warning, being wrong
// the other way costs another inconclusive session.
//
// AND THIS TITLE LANDS ON THE WRONG SIDE OF IT. Stray ships ScreenPercentage=50 (CLAUDE.md
// §2.3.1), i.e. a 2.0x ratio, i.e. max_performance — so a session left on the driver default
// preset gets M and cannot use an exposure texture at all.
enum class PresetExposure
{
	supported,      // J or K
	not_supported,  // L, or M
	unknown,        // a preset value the guide does not cover
};

// `preset_hint` is [STRAYDLSS] NgxPreset as handed to NGX: 0 = Default, 10 = J, 11 = K,
// 12 = L, 13 = M. `quality` is the mode the feature is actually created with, which is what
// Default resolves against.
PresetExposure preset_exposure_support(int preset_hint, DlssQuality quality);

// The preset the runtime will actually use, for logging. Returns the hint unchanged unless it
// is 0, in which case it resolves the Default mapping above.
int resolved_preset(int preset_hint, DlssQuality quality);
const char *preset_letter(int preset);

// True when the configuration is self-defeating: a mode that supplies an exposure texture on a
// preset that the guide says ignores it. The caller must WARN — not silently switch — because
// silently switching is how "inert" became a belief in the first place.
bool exposure_will_be_ignored(Mode mode, int preset_hint, DlssQuality quality);

// What the SR evaluate carries this frame, for one mode and one frame's availability.
struct EvalPlan
{
	bool pass_texture = false;     // set eval.pInExposureTexture
	float pre_exposure = 1.0f;     // eval.InPreExposure, already guarded
	float exposure_scale = 0.0f;   // eval.InExposureScale; 0 means "leave the helper's 1.0"
	bool degraded = false;         // the mode wanted a texture and this frame has none
};

// `texture_available` is "a texture pointer will actually be handed to NGX this frame" — for
// engine_texture that is the liveness-checked t0, for owned_texture our allocation succeeding.
//
// Under `automatic` the scale is left at 0 so the auto path stays BYTE-IDENTICAL to the build
// that shipped before any of this existed; that is what makes the A/B honest.
EvalPlan plan_evaluate(Mode mode,
                       bool texture_available,
                       float view_pre_exposure,
                       bool pre_exposure_ok,
                       float exposure_scale);

} // namespace stray_dlss::exposure
