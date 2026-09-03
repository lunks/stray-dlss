// DLSS SR's exposure contract. Every case here is a claim about NVIDIA's headers or about the
// official UE plugin, cited at the assertion, so a future edit that "simplifies" one of them
// fails loudly instead of silently changing what DLSS is told about the image.
#include <doctest/doctest.h>

#include "../src/core/exposure_plan.hpp"

#include <cmath>
#include <limits>
#include <string>

using namespace stray_dlss::exposure;

TEST_CASE("NgxExposure parses to a mode, and anything unknown is the safe default")
{
	CHECK(parse_mode("auto") == Mode::automatic);
	CHECK(parse_mode("texture") == Mode::engine_texture);
	CHECK(parse_mode("owned") == Mode::owned_texture);

	// A typo must not half-configure the feature: it degrades to the shipped default, which is
	// also the official plugin's default (r.NGX.DLSS.AutoExposure = 1).
	CHECK(parse_mode("Texture") == Mode::automatic);
	CHECK(parse_mode("") == Mode::automatic);
	CHECK(parse_mode(nullptr) == Mode::automatic);

	// The name round-trips, because it is what the log line prints and what the user matches
	// against the ini key they set.
	CHECK(parse_mode(mode_name(Mode::automatic)) == Mode::automatic);
	CHECK(parse_mode(mode_name(Mode::engine_texture)) == Mode::engine_texture);
	CHECK(parse_mode(mode_name(Mode::owned_texture)) == Mode::owned_texture);
}

TEST_CASE("AutoExposure is 1<<6 and is set in exactly one mode")
{
	// NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6 (nvsdk_ngx_defs.h:297). The value is
	// duplicated in exposure_plan.hpp so the core library builds without the NGX headers;
	// ngx_backend.cpp static_asserts the two agree.
	CHECK(kAutoExposureFlag == 0x40u);

	CHECK(create_flag_bits(Mode::automatic) == kAutoExposureFlag);

	// Both texture modes DROP it. The plugin's pair is exclusive — `bUseAutoExposure ?
	// Flags_AutoExposure : 0` (NGXRHI.cpp:537-546) with the texture nulled only under auto
	// (NGXD3D12RHI.cpp:267-269) — so a build that set the flag AND passed a texture would be
	// asking DLSS the same question twice with different answers.
	CHECK(create_flag_bits(Mode::engine_texture) == 0u);
	CHECK(create_flag_bits(Mode::owned_texture) == 0u);

	CHECK_FALSE(wants_texture(Mode::automatic));
	CHECK(wants_texture(Mode::engine_texture));
	CHECK(wants_texture(Mode::owned_texture));
}

TEST_CASE("helpers.h:507-508 maps 0 to 1.0, so a zero is a VALUE and never 'unset'")
{
	// `pInDlssEvalParams->InPreExposure == 0.0f ? 1.0f : ...` — nvsdk_ngx_helpers.h:507.
	CHECK(effective_pre_exposure(0.0f) == doctest::Approx(1.0f));
	CHECK(effective_pre_exposure(0.45f) == doctest::Approx(0.45f));
	// :508, the same rewrite for the scale.
	CHECK(effective_exposure_scale(0.0f) == doctest::Approx(1.0f));
	CHECK(effective_exposure_scale(4.0f) == doctest::Approx(4.0f));
}

TEST_CASE("SR's InPreExposure is guarded by the row-135 self-check, not passed blind")
{
	// The happy path: row 135.y measured live on this title.
	CHECK(sr_pre_exposure(0.451940f, true) == doctest::Approx(0.451940f));

	// A failed self-check means we do not know the exposure. Returning exactly 1.0 is the same
	// number helpers.h:507 would have produced from a 0 — but chosen deliberately, which is the
	// difference between a decision and an accident.
	CHECK(sr_pre_exposure(0.451940f, false) == doctest::Approx(1.0f));
	CHECK(sr_pre_exposure(0.0f, true) == doctest::Approx(1.0f));
	CHECK(sr_pre_exposure(-3.0f, true) == doctest::Approx(1.0f));
	CHECK(sr_pre_exposure(std::numeric_limits<float>::quiet_NaN(), true) == doctest::Approx(1.0f));
	CHECK(sr_pre_exposure(std::numeric_limits<float>::infinity(), true) == doctest::Approx(1.0f));

	// A garbage-but-finite-positive row is the dangerous case: helpers.h would forward it
	// untouched. It only survives when the self-check passed, and it is still clamped.
	CHECK(sr_pre_exposure(1e30f, false) == doctest::Approx(1.0f));
	CHECK(sr_pre_exposure(1e30f, true) == doctest::Approx(1e6f));

	// The measured menu-to-gameplay swing (1.000 -> 4.881 -> 0.051 -> 0.452, CLAUDE.md §2.6)
	// must all pass through untouched: none of it is implausible, it is the scene changing.
	CHECK(sr_pre_exposure(1.0f, true) == doctest::Approx(1.0f));
	CHECK(sr_pre_exposure(4.881f, true) == doctest::Approx(4.881f));
	CHECK(sr_pre_exposure(0.051f, true) == doctest::Approx(0.051f));
}

TEST_CASE("the owned texel carries the engine's exposure, times the consume-test multiplier")
{
	// nvsdk_ngx_defs.h:766: "a 1x1 texture containing the final exposure scale". Row 135.y is
	// that quantity one frame stale (View.PreExposure = GetLastEyeAdaptationExposure()).
	CHECK(owned_texel_value(0.45f, true, 1.0f) == doctest::Approx(0.45f));

	// The whole point of the knob: a deliberately wrong number. If the image does not move
	// between these two, the runtime is not reading the texture.
	CHECK(owned_texel_value(0.45f, true, 0.25f) == doctest::Approx(0.1125f));
	CHECK(owned_texel_value(0.45f, true, 4.0f) == doctest::Approx(1.8f));

	// An unusable row still yields a perturbable value rather than nothing, so the consume test
	// survives a frame the View CB did not decode.
	CHECK(owned_texel_value(0.0f, false, 4.0f) == doctest::Approx(4.0f));

	// NEVER zero, negative or NaN: that is precisely the input DLSS treats as invalid, and
	// re-introducing it from our own side would recreate the failure the texel dump ruled out.
	CHECK(owned_texel_value(0.45f, true, 0.0f) == doctest::Approx(0.45f));
	CHECK(owned_texel_value(0.45f, true, -1.0f) == doctest::Approx(0.45f));
	CHECK(owned_texel_value(0.45f, true, std::numeric_limits<float>::quiet_NaN())
		== doctest::Approx(0.45f));
	CHECK(owned_texel_value(1e-30f, true, 1e-30f) > 0.0f);
	CHECK(owned_texel_value(1e30f, true, 1e30f) <= 1e6f);
}

TEST_CASE("auto mode is byte-identical to the build that predates all of this")
{
	const EvalPlan p = plan_evaluate(Mode::automatic, /*texture_available=*/true, 0.45f, true,
		/*exposure_scale=*/4.0f);
	CHECK_FALSE(p.pass_texture);
	// Left at 0 so helpers.h:508 supplies its own 1.0 — the exact call the original build made.
	CHECK(p.exposure_scale == doctest::Approx(0.0f));
	CHECK_FALSE(p.degraded);
	// PreExposure is still sent. The plugin sends it in BOTH modes (DLSSUpscaler.cpp:1085-1089).
	CHECK(p.pre_exposure == doctest::Approx(0.45f));
}

TEST_CASE("a texture mode passes the texture and the scale, and says so when it cannot")
{
	const EvalPlan ok = plan_evaluate(Mode::engine_texture, true, 0.45f, true, 4.0f);
	CHECK(ok.pass_texture);
	CHECK(ok.exposure_scale == doctest::Approx(4.0f));
	CHECK_FALSE(ok.degraded);
	CHECK(ok.pre_exposure == doctest::Approx(0.45f));

	// A frame with no live t0. The AutoExposure flag is a CREATION-time property, so this frame
	// cannot fall back to auto — it gets DLSS's default exposure. The flag on the plan is what
	// makes that reportable instead of invisible.
	const EvalPlan miss = plan_evaluate(Mode::engine_texture, false, 0.45f, true, 4.0f);
	CHECK_FALSE(miss.pass_texture);
	CHECK(miss.degraded);
	// No texture, so no scale: an "Exposure level" readout describing nothing is worse than none.
	CHECK(miss.exposure_scale == doctest::Approx(0.0f));
	CHECK(miss.pre_exposure == doctest::Approx(0.45f));

	// owned behaves identically at this layer; only the resource's owner differs.
	const EvalPlan owned = plan_evaluate(Mode::owned_texture, true, 0.45f, true, 1.0f);
	CHECK(owned.pass_texture);
	CHECK(owned.exposure_scale == doctest::Approx(1.0f));
	CHECK_FALSE(owned.degraded);

	// A nonsense scale is dropped rather than forwarded: helpers.h:508 would pass any finite
	// non-zero straight through, including a negative.
	CHECK(plan_evaluate(Mode::owned_texture, true, 0.45f, true, -1.0f).exposure_scale
		== doctest::Approx(0.0f));
	CHECK(plan_evaluate(Mode::owned_texture, true, 0.45f, true,
		std::numeric_limits<float>::quiet_NaN()).exposure_scale == doctest::Approx(0.0f));
}

TEST_CASE("the row-135 guard reaches the evaluate plan in every mode")
{
	CHECK(plan_evaluate(Mode::automatic, false, 0.45f, false, 0.0f).pre_exposure
		== doctest::Approx(1.0f));
	CHECK(plan_evaluate(Mode::engine_texture, true, 0.45f, false, 1.0f).pre_exposure
		== doctest::Approx(1.0f));
	CHECK(plan_evaluate(Mode::owned_texture, true, 0.45f, false, 1.0f).pre_exposure
		== doctest::Approx(1.0f));
}

TEST_CASE("the preset gate: exposure input is only supported by presets J and K")
{
	using stray_dlss::DlssQuality;

	// DLSS Programming Guide 310.6.0 §3.9: "Only supported by Presets J and K. Preset L always
	// uses AutoExposure." HARD. This is the documented mechanism that produces EXACTLY the
	// symptom recorded as "inert" — flag cleared, healthy texture, indicator still ON.
	CHECK(preset_exposure_support(10, DlssQuality::max_performance) == PresetExposure::supported);
	CHECK(preset_exposure_support(11, DlssQuality::max_performance) == PresetExposure::supported);
	CHECK(preset_exposure_support(12, DlssQuality::max_quality) == PresetExposure::not_supported);
	CHECK(preset_exposure_support(13, DlssQuality::max_quality) == PresetExposure::not_supported);
	CHECK(preset_exposure_support(7, DlssQuality::max_quality) == PresetExposure::unknown);
}

TEST_CASE("Preset_Default resolves per quality mode, and THIS TITLE lands on the wrong side")
{
	using stray_dlss::DlssQuality;

	// nvsdk_ngx_defs.h:82-85, the header's own comments:
	//   K "Default preset for DLAA/Balanced/Quality modes", L "Default for Ultra Perf mode",
	//   M "Default for Perf mode".
	CHECK(resolved_preset(0, DlssQuality::dlaa) == 11);
	CHECK(resolved_preset(0, DlssQuality::max_quality) == 11);
	CHECK(resolved_preset(0, DlssQuality::balanced) == 11);
	CHECK(resolved_preset(0, DlssQuality::max_performance) == 13);
	CHECK(resolved_preset(0, DlssQuality::ultra_performance) == 12);

	// An explicit hint is never second-guessed.
	CHECK(resolved_preset(11, DlssQuality::ultra_performance) == 11);

	// Stray ships ScreenPercentage=50 (CLAUDE.md §2.3.1) = a 2.0x ratio = max_performance. On
	// the driver default preset that is M, and the exposure texture cannot be used at all.
	CHECK(preset_exposure_support(0, DlssQuality::max_performance)
		== PresetExposure::not_supported);
	// At 70% (~1.43x, the highest working setting) the default is K and it can.
	CHECK(preset_exposure_support(0, DlssQuality::max_quality) == PresetExposure::supported);
}

TEST_CASE("a self-defeating exposure configuration is detectable before the session, not after")
{
	using stray_dlss::DlssQuality;

	// The whole point: this must be knowable from the config, so a run is never spent
	// measuring a path the runtime was never going to take.
	CHECK(exposure_will_be_ignored(Mode::engine_texture, 0, DlssQuality::max_performance));
	CHECK(exposure_will_be_ignored(Mode::owned_texture, 13, DlssQuality::max_quality));
	CHECK(exposure_will_be_ignored(Mode::engine_texture, 12, DlssQuality::dlaa));

	// Explicit K at Performance is the corrected configuration.
	CHECK_FALSE(exposure_will_be_ignored(Mode::engine_texture, 11, DlssQuality::max_performance));

	// auto mode supplies no texture, so nothing is being ignored and there is nothing to warn
	// about — the preset is free to be whatever the ratio wants.
	CHECK_FALSE(exposure_will_be_ignored(Mode::automatic, 13, DlssQuality::max_performance));
	CHECK_FALSE(exposure_will_be_ignored(Mode::automatic, 0, DlssQuality::ultra_performance));

	// An unknown preset is not asserted to be broken: unknown is not not_supported.
	CHECK_FALSE(exposure_will_be_ignored(Mode::engine_texture, 7, DlssQuality::max_quality));
}

TEST_CASE("preset letters are what the log prints")
{
	CHECK(std::string(preset_letter(0)) == "Default");
	CHECK(std::string(preset_letter(10)) == "J");
	CHECK(std::string(preset_letter(11)) == "K");
	CHECK(std::string(preset_letter(12)) == "L");
	CHECK(std::string(preset_letter(13)) == "M");
	CHECK(std::string(preset_letter(99)) == "?");
}
