#include "TweakState.hpp"

#include "app/dlss_app.hpp"
#include "backend_native/fg_present.hpp"
#include "core/dlss_quality.hpp"
#include "host/config.hpp"
#include "host/ini_edit.hpp"
#include "log.hpp"
#include "mv_mask.hpp"
#include "ngx_backend.hpp"
#include "ngx_nr.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace stray_dlss::plugin::tweak {
namespace {

// The one annotation this whole feature exists to carry, in the words the disassembly earned:
// every NR knob but Intensity holds DLSSNR.Reset = 1 for as long as it keeps moving, so the
// whole screen changes for reasons unrelated to the knob's meaning. CLAUDE.md §5 records this
// project mis-diagnosing exactly that once ("skin affects everything").
constexpr const char *kResetWarning =
	"WIPES feature 18's temporal history on EVERY change (CG2R_ResetTemporalHistoryOnControlChange). "
	"Set the value, RELEASE the slider, wait 1-2 s, THEN judge. While it moves, the whole screen "
	"changes for reasons that are not this knob.";

const char *const kStyleItems[] = { "0 (off / as before)", "1", "2" };
const char *const kMaskFormatItems[] = { "R8_UNORM", "R16_FLOAT", "RGBA8_UNORM" };

// The DLSS bias-mask knobs are the only ones with no full read-back on the module side
// (mvmask::stats() carries the value and the format but not the alternation period), so the
// tab keeps a shadow, seeded once from the config source. Everything else is read live.
struct MaskShadow
{
	bool seeded = false;
	bool enabled = false;
	float value = 1.0f;
	int alternate = 0;
	int format = mvmask::kDefaultFormat;
};
MaskShadow g_mask;

void push_mask()
{
	mvmask::configure(g_mask.enabled, g_mask.value,
		static_cast<std::uint32_t>(g_mask.alternate < 0 ? 0 : g_mask.alternate), g_mask.format);
}

// SEEDING ALSO PUSHES, and that is the whole point rather than belt-and-braces. The shadow is
// seeded lazily, and the first thing that touches it may well be apply_from_config() reacting
// to an ini edit - at which point the config source ALREADY holds the new value, so the shadow
// would seed to it, compare equal, and the module would never be told. Pushing on seed makes
// that case correct; in every other case it re-sends what dlss_app configured at startup, which
// is a store of four scalars.
MaskShadow &mask()
{
	if (!g_mask.seeded)
	{
		g_mask.seeded = true;
		g_mask.enabled = host::cfg::get_int("MvMask", 0) != 0;
		g_mask.value = host::cfg::get_float("MvMaskValue", 1.0f);
		g_mask.alternate = host::cfg::get_int("MvMaskAlternate", 0);
		if (g_mask.alternate < 0)
			g_mask.alternate = 0;
		g_mask.format = host::cfg::get_int("MvMaskFormat", mvmask::kDefaultFormat);
		push_mask();
	}
	return g_mask;
}

// ---- the registry ----

const Knob kNr[] = {
	{ "NgxNR", "DLSS Neural Rendering (NgxNR)",
	  "Feature 18, as a PRESENT stage over the back buffer. Turning it OFF here is safe from this "
	  "thread by design: the teardown is QUEUED and carried out at the present boundary once the "
	  "fence has passed the last evaluate (src/core/nr_lifetime.hpp).",
	  Kind::boolean, 0.0f, 1.0f, false, false, nullptr, 0 },
	{ "NgxNRIntensity", "Intensity",
	  "THE ONLY STRENGTH KNOB THAT DOES NOT RESET THE TEMPORAL HISTORY. It is absent from "
	  "CG2R_ResetTemporalHistoryOnControlChange's list, so it can be swept continuously and judged "
	  "immediately. Use this one to answer \"how much NR\"; use the others only in steps.",
	  Kind::real, 0.0f, 3.0f, false, false, nullptr, 0 },
	{ "NgxNRLocalTone", "Local tone strength", kResetWarning, Kind::real, 0.0f, 3.0f, true, false, nullptr, 0 },
	{ "NgxNRLocalStructure", "Local structure strength", kResetWarning, Kind::real, 0.0f, 3.0f, true, false, nullptr, 0 },
	{ "NgxNRSkinStructure", "Skin structure strength",
	  "A GLOBAL SCALAR, not a mask: it is broadcast bit-identically into every pixel's input tile "
	  "as one of sixteen channels, and only the trained weights decide where it acts. It exists "
	  "only on the auto-mask path. Also: WIPES the temporal history on every change.",
	  Kind::real, 0.0f, 3.0f, true, false, nullptr, 0 },
	{ "NgxNRStyle", "Style",
	  "DLSSNR.Style. Community-tested labels only. WIPES the temporal history on every change.",
	  Kind::combo, 0.0f, 2.0f, true, false, kStyleItems, 3 },
	{ "NgxNRAutoMask", "Auto mask",
	  "Turning it OFF drives BOTH resolved strengths to -1, which costs the skin term outright "
	  "(there is no skin channel anywhere else). WIPES the temporal history on every change.",
	  Kind::boolean, 0.0f, 1.0f, true, false, nullptr, 0 },
	{ "NgxNRPreset", "Render preset", "DLSSNR.Hint.Render.Preset. Live; not a history reset.",
	  Kind::integer, 0.0f, 4.0f, false, false, nullptr, 0 },
	{ "NgxNRUICorrection", "UI correction",
	  "MEASURED INERT in this runtime: DLSSNR.UICorrection cannot arm without a DLSSNR.Backbuffer, "
	  "which we do not bind, so this has never done anything (CLAUDE.md §5). Kept visible so its "
	  "inertness is a stated fact rather than a mystery.",
	  Kind::boolean, 0.0f, 1.0f, false, false, nullptr, 0 },
	{ "NgxNRMVecScale", "MVec scale override",
	  "0 = the built-in 1.0. Feature 18 reprojects its OWN history with these vectors, so a scale "
	  "error does not cost one frame - it compounds through the accumulation and reads as drift "
	  "(CLAUDE.md §5). 1.0 is the measured-correct value; anything else is an experiment.",
	  Kind::real, 0.0f, 4.0f, false, false, nullptr, 0 },
};

const Knob kSr[] = {
	{ "MVConvention", "Motion-vector convention",
	  "Which branch of the resolve gets its sign flipped. A wrong answer here compounds through "
	  "every temporal consumer at once - DLSS SR, feature 18 and the engine's own TAA.",
	  Kind::combo, 0.0f, 3.0f, false, false, app::kMvConventions, 4 },
	{ "MVInvertX", "Invert MV X", "Diagnostic sign flip on the resolved field's X.", Kind::boolean, 0.0f, 1.0f, false, false, nullptr, 0 },
	{ "MVInvertY", "Invert MV Y", "Diagnostic sign flip on the resolved field's Y.", Kind::boolean, 0.0f, 1.0f, false, false, nullptr, 0 },
	{ "NgxPreset", "DLSS SR preset (read-only)",
	  "0 default, 10=J, 11=K, 12=L, 13=M. READ-ONLY HERE ON PURPOSE: the five hint keys are read "
	  "at CreateFeature and setting them afterwards has no effect (CLAUDE.md §5), so a live "
	  "slider would move a number and change nothing. Save it to the ini and relaunch.",
	  Kind::integer, 0.0f, 13.0f, false, true, nullptr, 0 },
};

const Knob kFg[] = {
	{ "NgxFG", "Frame generation (NgxFG)",
	  "ON -> OFF takes effect immediately and degrades to a normal present (every FG entry point "
	  "returns early when disabled). OFF -> ON usually does NOTHING for the rest of the session: "
	  "the replacement back buffers are handed to the game at swapchain creation / GetBuffer, so "
	  "a chain created with FG off is never armed. Save it and relaunch.",
	  Kind::boolean, 0.0f, 1.0f, false, false, nullptr, 0 },
};

const Knob kDiag[] = {
	{ "MvMask", "DLSS SR bias-colour mask (MvMask)",
	  "pInBiasCurrentColorMask: DLSS SR's per-pixel \"do not trust the history here\" channel. A "
	  "constant fill. It CHANGES THE IMAGE by design; judge it against the alternation's own "
	  "phases, never across launches.",
	  Kind::boolean, 0.0f, 1.0f, false, false, nullptr, 0 },
	{ "MvMaskValue", "Mask value", "0 = trust the history everywhere (neutral). 1 = distrust it everywhere.",
	  Kind::real, 0.0f, 1.0f, false, false, nullptr, 0 },
	{ "MvMaskAlternate", "Mask alternation period (frames)",
	  "0 = no alternation. N > 0 flips the mask between its value and neutral every N frames, "
	  "which is the only honest way to judge it inside ONE session.",
	  Kind::integer, 0.0f, 1800.0f, false, false, nullptr, 0 },
	{ "MvMaskFormat", "Mask format",
	  "The runtime validates nothing here, so a wrong choice is a silent wrong image rather than "
	  "an error. R8_UNORM is what the one integration known to drive this input uses.",
	  Kind::combo, 0.0f, 2.0f, false, false, kMaskFormatItems, 3 },
};

} // namespace

const Knob *knobs(Group g, int &count)
{
	switch (g)
	{
	case Group::nr: count = static_cast<int>(sizeof(kNr) / sizeof(kNr[0])); return kNr;
	case Group::sr: count = static_cast<int>(sizeof(kSr) / sizeof(kSr[0])); return kSr;
	case Group::fg: count = static_cast<int>(sizeof(kFg) / sizeof(kFg[0])); return kFg;
	case Group::diagnostics: count = static_cast<int>(sizeof(kDiag) / sizeof(kDiag[0])); return kDiag;
	default: break;
	}
	count = 0;
	return nullptr;
}

const char *group_name(Group g)
{
	switch (g)
	{
	case Group::nr: return "Neural Rendering";
	case Group::sr: return "Super Resolution / motion vectors";
	case Group::fg: return "Frame Generation";
	case Group::diagnostics: return "Diagnostics";
	default: break;
	}
	return "?";
}

float value_of(const Knob &k)
{
	const app::NrLive &n = app::instance().nr_live();
	if (std::strcmp(k.ini_key, "NgxNR") == 0) return nr::enabled() ? 1.0f : 0.0f;
	if (std::strcmp(k.ini_key, "NgxNRIntensity") == 0) return n.intensity;
	if (std::strcmp(k.ini_key, "NgxNRLocalTone") == 0) return n.local_tone;
	if (std::strcmp(k.ini_key, "NgxNRLocalStructure") == 0) return n.local_structure;
	if (std::strcmp(k.ini_key, "NgxNRSkinStructure") == 0) return n.skin_structure;
	if (std::strcmp(k.ini_key, "NgxNRStyle") == 0) return static_cast<float>(n.style);
	if (std::strcmp(k.ini_key, "NgxNRAutoMask") == 0) return n.auto_mask ? 1.0f : 0.0f;
	if (std::strcmp(k.ini_key, "NgxNRPreset") == 0) return static_cast<float>(n.preset);
	if (std::strcmp(k.ini_key, "NgxNRUICorrection") == 0) return n.ui_correction ? 1.0f : 0.0f;
	if (std::strcmp(k.ini_key, "NgxNRMVecScale") == 0) return n.mvec_scale;
	if (std::strcmp(k.ini_key, "MVConvention") == 0) return static_cast<float>(n.mv_convention);
	if (std::strcmp(k.ini_key, "MVInvertX") == 0) return n.mv_invert_x ? 1.0f : 0.0f;
	if (std::strcmp(k.ini_key, "MVInvertY") == 0) return n.mv_invert_y ? 1.0f : 0.0f;
	if (std::strcmp(k.ini_key, "NgxPreset") == 0) return static_cast<float>(ngx::preset());
	if (std::strcmp(k.ini_key, "NgxFG") == 0) return native::fg::enabled() ? 1.0f : 0.0f;
	if (std::strcmp(k.ini_key, "MvMask") == 0) return mask().enabled ? 1.0f : 0.0f;
	if (std::strcmp(k.ini_key, "MvMaskValue") == 0) return mask().value;
	if (std::strcmp(k.ini_key, "MvMaskAlternate") == 0) return static_cast<float>(mask().alternate);
	if (std::strcmp(k.ini_key, "MvMaskFormat") == 0) return static_cast<float>(mask().format);
	return 0.0f;
}

void set_value(const Knob &k, float v)
{
	if (k.read_only)
		return;
	const int i = static_cast<int>(std::lround(v));
	const bool b = v != 0.0f;

	if (std::strcmp(k.ini_key, "NgxNR") == 0)
	{
		if (nr::enabled() != b)
		{
			// The same WARN Host::Tick emits for the ini path, and for the same reason: this is
			// not the render thread, and saying so in the log is what makes a later teardown
			// question answerable without another round trip.
			STRAY_LOG_WARN("tweak-ui: NgxNR -> %d from the UE4SS GUI thread. The teardown is "
				"queued and performed at the present boundary once the fence has passed the "
				"last evaluate.", b ? 1 : 0);
			nr::set_enabled(b);
		}
		return;
	}
	if (std::strcmp(k.ini_key, "NgxFG") == 0)
	{
		native::fg::Config cfg = native::fg::config();
		if (cfg.enabled == b)
			return;
		// ONLY `enabled` moves. Leaving `pacing` alone is what keeps fg::configure from
		// draining and restarting the present worker underneath a frame in flight - the drain
		// path is entered only when pacing changes away from `thread`.
		cfg.enabled = b;
		STRAY_LOG_WARN("tweak-ui: NgxFG -> %d from the UE4SS GUI thread (pacing untouched). "
			"Disabling is immediate; ENABLING mid-session cannot arm a swapchain whose "
			"replacement buffers were never handed out.", b ? 1 : 0);
		native::fg::configure(cfg);
		return;
	}
	if (std::strncmp(k.ini_key, "MvMask", 6) == 0)
	{
		MaskShadow &m = mask();
		if (std::strcmp(k.ini_key, "MvMask") == 0) m.enabled = b;
		else if (std::strcmp(k.ini_key, "MvMaskValue") == 0) m.value = v;
		else if (std::strcmp(k.ini_key, "MvMaskAlternate") == 0) m.alternate = i < 0 ? 0 : i;
		else if (std::strcmp(k.ini_key, "MvMaskFormat") == 0) m.format = i;
		push_mask();
		return;
	}

	// Everything else lives in the app's NrLive block and is re-sent to the NGX parameter block
	// on every evaluate, so one apply_nr_live() is the whole update.
	app::NrLive &n = app::instance().nr_live();
	if (std::strcmp(k.ini_key, "NgxNRIntensity") == 0) n.intensity = v;
	else if (std::strcmp(k.ini_key, "NgxNRLocalTone") == 0) n.local_tone = v;
	else if (std::strcmp(k.ini_key, "NgxNRLocalStructure") == 0) n.local_structure = v;
	else if (std::strcmp(k.ini_key, "NgxNRSkinStructure") == 0) n.skin_structure = v;
	else if (std::strcmp(k.ini_key, "NgxNRStyle") == 0) n.style = i;
	else if (std::strcmp(k.ini_key, "NgxNRAutoMask") == 0) n.auto_mask = b;
	else if (std::strcmp(k.ini_key, "NgxNRPreset") == 0) n.preset = i;
	else if (std::strcmp(k.ini_key, "NgxNRUICorrection") == 0) n.ui_correction = b;
	else if (std::strcmp(k.ini_key, "NgxNRMVecScale") == 0) n.mvec_scale = v;
	else if (std::strcmp(k.ini_key, "MVConvention") == 0) n.mv_convention = i;
	else if (std::strcmp(k.ini_key, "MVInvertX") == 0) n.mv_invert_x = b;
	else if (std::strcmp(k.ini_key, "MVInvertY") == 0) n.mv_invert_y = b;
	else
		return;
	app::instance().apply_nr_live();
}

// ---- the ini save ----

namespace {

std::string encode(const Knob &k, float v)
{
	switch (k.kind)
	{
	case Kind::boolean: return host::ini_format_bool(v != 0.0f);
	case Kind::integer:
	case Kind::combo: return host::ini_format_int(static_cast<int>(std::lround(v)));
	case Kind::real: break;
	}
	return host::ini_format_float(v);
}

} // namespace

bool save_to_ini(const std::string &ini_path, std::string &error)
{
	error.clear();
	if (ini_path.empty())
	{
		error = "no StrayDLSS.ini path is known (the host never found one at startup)";
		return false;
	}
	std::FILE *in = nullptr;
#ifdef _WIN32
	if (fopen_s(&in, ini_path.c_str(), "rb") != 0)
		in = nullptr;
#else
	in = std::fopen(ini_path.c_str(), "rb");
#endif
	if (in == nullptr)
	{
		error = "cannot read " + ini_path;
		return false;
	}
	std::string text;
	char chunk[4096];
	std::size_t got = 0;
	while ((got = std::fread(chunk, 1, sizeof(chunk), in)) > 0)
		text.append(chunk, got);
	std::fclose(in);

	std::vector<host::IniEdit> edits;
	for (int g = 0; g < static_cast<int>(Group::count); ++g)
	{
		int n = 0;
		const Knob *ks = knobs(static_cast<Group>(g), n);
		for (int i = 0; i < n; ++i)
			edits.push_back({ ks[i].ini_key, encode(ks[i], value_of(ks[i])) });
	}

	const std::string out = host::ini_apply_edits(text, "STRAYDLSS", edits);

	std::FILE *f = nullptr;
#ifdef _WIN32
	if (fopen_s(&f, ini_path.c_str(), "wb") != 0)
		f = nullptr;
#else
	f = std::fopen(ini_path.c_str(), "wb");
#endif
	if (f == nullptr)
	{
		error = "cannot write " + ini_path;
		return false;
	}
	const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
	std::fclose(f);
	if (!ok)
	{
		error = "short write to " + ini_path;
		return false;
	}
	STRAY_LOG_WARN("tweak-ui: saved %zu keys into %s. Comments and every key the tab does not "
		"know about were preserved verbatim.", edits.size(), ini_path.c_str());
	return true;
}

int apply_from_config()
{
	int moved = 0;
	for (int g = 0; g < static_cast<int>(Group::count); ++g)
	{
		int n = 0;
		const Knob *ks = knobs(static_cast<Group>(g), n);
		for (int i = 0; i < n; ++i)
		{
			const Knob &k = ks[i];
			if (k.read_only || !host::cfg::has(k.ini_key))
				continue;
			const float now = value_of(k);
			const float want = k.kind == Kind::real
				? host::cfg::get_float(k.ini_key, now)
				: static_cast<float>(host::cfg::get_int(k.ini_key, static_cast<int>(std::lround(now))));
			if (want == now)
				continue;
			set_value(k, want);
			++moved;
			STRAY_LOG_INFO("tweak-ui: %s %g -> %g from StrayDLSS.ini", k.ini_key,
				static_cast<double>(now), static_cast<double>(want));
		}
	}
	return moved;
}

// ---- status ----

int format_sr_status(char *buf, std::size_t size)
{
	const ngx::Status &s = ngx::status();
	const ngx::FeatureDesc d = ngx::live_feature_desc();
	if (d.output_width == 0 || d.render_width == 0)
		return std::snprintf(buf, size, "SR: no live feature (ngx init=%d available=%d)",
			s.initialised ? 1 : 0, s.super_sampling_available ? 1 : 0);
	const DlssQuality q = dlss_quality_for(d.render_width, d.render_height, d.output_width, d.output_height);
	return std::snprintf(buf, size, "SR: %ux%u -> %ux%u (%.3fx, %s), preset %d",
		d.render_width, d.render_height, d.output_width, d.output_height,
		static_cast<double>(d.output_width) / (d.render_width != 0 ? d.render_width : 1),
		dlss_quality_name(q), ngx::preset());
}

int format_nr_status(char *buf, std::size_t size)
{
	std::uint64_t applied = 0, refused = 0;
	std::uint32_t by_reason[nr::kNrRefusalCount] = {};
	nr::counters(applied, refused, by_reason);
	int top = -1;
	for (int i = 0; i < nr::kNrRefusalCount; ++i)
		if (top < 0 || by_reason[i] > by_reason[top])
			top = i;
	const bool any = top >= 0 && by_reason[top] > 0;
	return std::snprintf(buf, size, "NR: %s applied=%llu refused=%llu%s%s",
		nr::validated() ? "validated" : (nr::enabled() ? "not validated yet" : "off"),
		static_cast<unsigned long long>(applied), static_cast<unsigned long long>(refused),
		any ? ", top reason: " : "", any ? nr::kNrRefusalNames[top] : "");
}

int format_fg_status(char *buf, std::size_t size)
{
	const native::fg::Stats s = native::fg::stats();
	if (!native::fg::enabled())
		return std::snprintf(buf, size, "FG: off");
	const double ratio = s.game_presents != 0
		? static_cast<double>(s.presents_issued) / static_cast<double>(s.game_presents)
		: 0.0;
	return std::snprintf(buf, size, "FG: %s gamePresents=%llu issued=%llu (%.2fx) generated=%llu",
		s.validated ? "validated" : "not validated",
		static_cast<unsigned long long>(s.game_presents),
		static_cast<unsigned long long>(s.presents_issued), ratio,
		static_cast<unsigned long long>(s.generated_presented));
}

} // namespace stray_dlss::plugin::tweak
