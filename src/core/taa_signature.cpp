#include "taa_signature.hpp"

#include "taa_hashes.hpp"

#include <algorithm>
#include <atomic>

namespace stray_dlss {
namespace {

std::uint32_t group_count(std::uint32_t extent)
{
	return (extent + kTaaTileSize - 1) / kTaaTileSize;
}

bool is_dummy(const BoundTexture &t)
{
	// GSystemTextures::BlackDummy, substituted for velocity and history on a camera cut.
	return t.width <= 1 && t.height <= 1;
}

} // namespace

// Written once at init (or on an explicit UI reload) and read on the dispatch hot path, so
// the extra table is a fixed array published through an acquire/release counter rather than a
// lock. A reload writes the entries first, then the count.
std::uint64_t g_extra_taa_hashes[64];
std::atomic<std::size_t> g_extra_taa_count{ 0 };

bool is_known_taa_hash(std::uint64_t hash)
{
	for (const std::uint64_t h : kKnownTaaHashes)
		if (h == hash)
			return true;
	const std::size_t n = g_extra_taa_count.load(std::memory_order_acquire);
	for (std::size_t i = 0; i < n; ++i)
		if (g_extra_taa_hashes[i] == hash)
			return true;
	return false;
}

// The baked FSSDTemporalAccumulationCS members (taa_signature.hpp) plus the runtime
// extension, same lock-free pattern as the TAA extras below.
constexpr std::uint64_t kKnownSsdHashes[] = {
	kDenoiserLookalikeHash, // permutation #5 - measured dispatching in live gameplay
	kSecondCandidateHash,   // permutation #0
};
std::uint64_t g_extra_ssd_hashes[64];
std::atomic<std::size_t> g_extra_ssd_count{ 0 };

bool is_ssd_temporal_hash(std::uint64_t hash)
{
	for (const std::uint64_t h : kKnownSsdHashes)
		if (h == hash)
			return true;
	const std::size_t n = g_extra_ssd_count.load(std::memory_order_acquire);
	for (std::size_t i = 0; i < n && i < 64; ++i)
		if (g_extra_ssd_hashes[i] == hash)
			return true;
	return false;
}

void set_extra_ssd_hashes(const std::uint64_t *hashes, std::size_t count)
{
	if (count > 64)
		count = 64;
	g_extra_ssd_count.store(0, std::memory_order_release);
	for (std::size_t i = 0; i < count; ++i)
		g_extra_ssd_hashes[i] = hashes == nullptr ? 0 : hashes[i];
	g_extra_ssd_count.store(count, std::memory_order_release);
}

std::uint64_t find_eye_adaptation_srv(const std::vector<BoundTexture> &srvs)
{
	// The signature is FORMAT + EXTENT, not register alone: 1x1 R32G32B32A32_FLOAT. The
	// 1x1 BlackDummy that replaces velocity/colour on a cut is a different format, so it
	// can never be picked; a 1x1 RGBA32F is the eye-adaptation buffer and nothing else in
	// the frame (CLAUDE.md §2.3).
	std::uint64_t structural = 0;
	for (const auto &t : srvs)
	{
		if (t.width != 1 || t.height != 1 || t.format != TexFormat::r32g32b32a32_float ||
			t.resource == 0)
			continue;
		if (t.slot == 0)
			return t.resource; // the §2.3 register map: t0 wins outright
		if (structural == 0)
			structural = t.resource;
	}
	return structural;
}

void set_extra_taa_hashes(const std::uint64_t *hashes, std::size_t count)
{
	count = std::min<std::size_t>(count, 64);
	g_extra_taa_count.store(0, std::memory_order_release);
	for (std::size_t i = 0; i < count; ++i)
		g_extra_taa_hashes[i] = hashes == nullptr ? 0 : hashes[i];
	g_extra_taa_count.store(count, std::memory_order_release);
}

bool is_hdr_colour(TexFormat f)
{
	// Scene colour is not necessarily RGBA16_FLOAT. UE4 commonly uses R11G11B10_FLOAT for the
	// scene-colour target, and the TAA history can differ from the input, so both must count.
	return f == TexFormat::r16g16b16a16_float || f == TexFormat::r11g11b10_float;
}

bool primary_view_shape_ok(std::uint32_t render_w, std::uint32_t render_h,
                           std::uint32_t out_w, std::uint32_t out_h)
{
	// Nothing to judge. The caller has its own "did we read a rect at all" refusal; inventing a
	// shape verdict here would turn a missing measurement into a rejection.
	if (render_w == 0 || render_h == 0 || out_w == 0 || out_h == 0)
		return true;

	// The engine's own floor, both axes. 64 against 3840 is 0.017; 1024 against 3840 is 0.27.
	if (static_cast<double>(render_w) < static_cast<double>(out_w) * kMinUpsampleFraction ||
		static_cast<double>(render_h) < static_cast<double>(out_h) * kMinUpsampleFraction)
		return false;

	// A real primary-view upscale preserves the aspect ratio. A square render rect against a
	// 16:9 output is not the main view, whoever announced the dispatch.
	const double in_aspect = static_cast<double>(render_w) / static_cast<double>(render_h);
	const double out_aspect = static_cast<double>(out_w) / static_cast<double>(out_h);
	if (in_aspect > out_aspect * (1.0 + kAspectTolerance) ||
		in_aspect < out_aspect * (1.0 - kAspectTolerance))
		return false;

	// Implied by the fraction test at 0.5 / 3.5, and kept because the two bounds come from
	// different authorities — one is UE4's, one is DLSS's. If either constant is ever revised
	// the other still holds its own end.
	return static_cast<double>(out_w) / static_cast<double>(render_w) <= kMaxUpscaleFactor;
}

bool colour_input_acceptable(const BoundTexture &t, std::uint32_t render_w,
                             std::uint32_t render_h, TexFormat output_format)
{
	if (t.resource == 0)
		return false;
	// A buffer, or the 1x1 GSystemTextures::BlackDummy UE4 substitutes on a camera cut. The
	// caller records width/height as 0 for a buffer, so this excludes both.
	if (t.width == 0 || t.height == 0)
		return false;
	// A 3D texture is not a Tex2D, and NGX says so out loud.
	if (t.is_3d)
		return false;
	// THE TEST A STREAMED MATERIAL TEXTURE FAILS. Stray's carpets cook to BC3_TYPELESS /
	// BC1_TYPELESS / BC5_UNORM (measured 2026-09-03 from the pak's own uasset name tables:
	// Content/Data/Props/Carpet and Content/Data/Textures/Tile/Carpet, 1024^2 and 2048^2, 7
	// mips always resident), and every BC format lands on TexFormat::unknown here.
	if (!is_hdr_colour(t.format))
		return false;
	// TAA reads and writes the same buffer kind, so when the output UAV's format is known the
	// colour input must share it — that separates the menu's R11G11B10 scene colour from
	// gameplay's RGBA16 when both happen to be bound.
	if (output_format != TexFormat::unknown && t.format != output_format)
		return false;
	// AT LEAST the render subrect, never equal to it: UE4 allocates the scene buffer at the
	// scene-buffer extent rather than the view size (CLAUDE.md §2.5), and under dynamic
	// resolution the view rect is strictly smaller than the allocation.
	return t.width >= render_w && t.height >= render_h;
}

MatchResult match_taa_dispatch(const DispatchSignature &sig,
                               std::uint32_t view_width,
                               std::uint32_t view_height)
{
	MatchResult r;

	if (sig.shader_hash == kDenoiserLookalikeHash)
	{
		r.verdict = MatchVerdict::excluded;
		r.reason = "0x1708ec956099e259 is a reprojecting denoiser (cb1[126]), not FTAAStandaloneCS";
		return r;
	}
	if (sig.shader_hash == kSecondCandidateHash)
	{
		r.verdict = MatchVerdict::excluded;
		r.reason = "0x52101a15e1a0c5cc is not FTAAStandaloneCS (cb1[131], 11 SRVs, 2 UAVs)";
		return r;
	}

	if (sig.group_count_z != 1)
	{
		r.reason = "dispatch is not 2D";
		return r;
	}

	// The strongest discriminator available at runtime, and the one the original heuristic
	// mistook for a disqualifier: FTAAStandaloneCS binds the depth-stencil resource TWICE,
	// once as R32_FLOAT_X8X24 (SceneDepthTexture) and once as X32_G8X24_UINT (StencilTexture).
	// No other pass in this title does that.
	const BoundTexture *depth = nullptr;
	const BoundTexture *stencil = nullptr;
	for (const auto &a : sig.srvs)
	{
		if (a.format != TexFormat::r32_float_x8x24_typeless)
			continue;
		for (const auto &b : sig.srvs)
		{
			if (b.format == TexFormat::x32_typeless_g8x24_uint && b.resource == a.resource)
			{
				depth = &a;
				stencil = &b;
				break;
			}
		}
		if (depth != nullptr)
			break;
	}

	if (depth == nullptr)
	{
		r.reason = "no depth+stencil SRV pair over one resource";
		return r;
	}

	// Exactly one full-size colour UAV: OutComputeTex_0.
	const BoundTexture *output = nullptr;
	for (const auto &t : sig.uavs)
	{
		if (is_hdr_colour(t.format) && !is_dummy(t))
		{
			if (output != nullptr && t.resource != output->resource)
				continue; // keep the first; extra colour UAVs are aliases of the inputs
			output = &t;
		}
	}

	if (output == nullptr)
	{
		r.reason = "no HDR float colour output UAV";
		return r;
	}

	// The dispatch covers the OUTPUT VIEW RECT — which is NOT the output texture's extent.
	//
	// UE 4.27, verbatim (Release-4.27 branch, TemporalAA.cpp):
	//   :950  GetGroupCount(PracticableDestRect.Size(), GTemporalAATileSizeX)  <- the DISPATCH
	//   :653  PracticableDestRect = DivideAndRoundUp(Inputs.OutputViewRect, ResolutionDivisor)
	//   :596  FTAAPassParameters::GetOutputExtent()                            <- the TEXTURE
	//           Main:           SceneColorInput->Desc.Extent
	//           MainUpsampling: Max(InputExtent, Quantize(OutputViewRect.Size()))
	// so the texture is only ever >= the dispatched rect, never tied to it.
	//
	// Under a real upscale the Max() pins the extent to the output view rect exactly, which is
	// why an equality test always matched at 50% and 70%. AT 1:1 THAT TERM DEGENERATES: the
	// quantized output view size can no longer exceed InputExtent, so GetOutputExtent() returns
	// the scene-colour buffer's own extent and NOTHING relates it to the dispatch. An equality
	// test then makes DLAA depend on UE4's scene-buffer allocation rather than on the pass. That
	// equality can reject a real FTAAStandaloneCS dispatch is not hypothetical: at 200% screen
	// percentage it fired live on the genuine pass (extent 7680x4320, dispatch 3840x2160).
	//
	// Two bounds that hold in BOTH configurations:
	//   * the dispatch must FIT INSIDE the output UAV — it writes it, so it cannot run past it;
	//   * the dispatch must COVER AT LEAST the input view rect — OutputViewRect is equal to
	//     InputViewRect in Main and larger in MainUpsampling, never smaller, so a dispatch
	//     below the view rect is downsampling. That is what keeps 200% rejected.
	// `view_width`/`view_height` are View.ViewSizeAndInvSize, i.e. UE4's InputViewRect.
	if (sig.group_count_x > group_count(output->width) ||
		sig.group_count_y > group_count(output->height))
	{
		r.reason = "dispatch runs past the output UAV at 8x8";
		return r;
	}
	if (sig.group_count_x < group_count(view_width) ||
		sig.group_count_y < group_count(view_height))
	{
		r.reason = "dispatch covers less than the view rect - downsampling, not TAA upscaling";
		return r;
	}
	// When the pass really is UPSCALING — the output UAV is larger than its own inputs — the
	// Max() above pins GetOutputExtent() to exactly Quantize(OutputViewRect), because the input
	// buffer is by definition the smaller of the two. Equality is then guaranteed by the engine
	// and is worth demanding: it is what rejects a pass that writes only part of a large UAV.
	// This branch is every measured 50% and 70% frame, and its behaviour is unchanged.
	if ((output->width > depth->width || output->height > depth->height) &&
		(sig.group_count_x != group_count(output->width) ||
		 sig.group_count_y != group_count(output->height)))
	{
		r.reason = "dispatch size does not cover the output UAV at 8x8";
		return r;
	}

	r.depth_srv = depth->slot;
	r.stencil_srv = stencil->slot;
	r.output_uav = output->slot;
	// The output RECT — what the dispatch writes — not the texture's allocation. They differ
	// whenever UE4's scene-colour buffer is larger than the view rect, and DLSS must be created
	// for the rect: telling it the allocation would size the feature for pixels the engine
	// never writes and never reads back. Clamped to the extent so a partial trailing tile
	// cannot name pixels the texture does not have. Identical to the old value whenever the
	// two agree, which is every measured 50%/70% frame.
	const std::uint32_t covered_w = sig.group_count_x * kTaaTileSize;
	const std::uint32_t covered_h = sig.group_count_y * kTaaTileSize;
	r.output_width = covered_w < output->width ? covered_w : output->width;
	r.output_height = covered_h < output->height ? covered_h : output->height;
	// REPORT the render resolution from the DEPTH SRV's extent rather than the passed-in view
	// rect: that comes from the View constant buffer, which is not always readable, and falling
	// back to the dispatch size makes every pass look 1:1. (The view rect is used above only as
	// a lower bound on the dispatch, where the fallback is harmless.)
	r.is_upsampling = r.output_width > depth->width || r.output_height > depth->height;
	r.render_width = depth->width;
	r.render_height = depth->height;

	// A REAL primary-view upscale preserves the aspect ratio and lands inside DLSS's supported
	// range. Measured 2026-09-01: without these two tests the matcher accepted cubemap faces and
	// reflection captures — one live session offered CreateFeature 248x248, 1016x1016 (twice)
	// and 1024x407 alongside the correct 1920x1080, i.e. 4 of 11 creations were bogus. Each one
	// cost an NGX feature create/release (a frame spike) and put a differently-oriented view of
	// the scene on the display chain, which the user saw as "a flipped version of the scene"
	// with a hitch. A square render target upscaled to 16:9 is not the main view, and
	// 3840/248 = 15.5x is not an upscale any DLSS quality mode performs.
	if (r.render_width != 0 && r.render_height != 0 && r.output_width != 0 && r.output_height != 0)
	{
		const double render_aspect =
			static_cast<double>(r.render_width) / static_cast<double>(r.render_height);
		const double output_aspect =
			static_cast<double>(r.output_width) / static_cast<double>(r.output_height);
		// 4% covers UE4's 8-pixel tile quantisation of the render rect at every ratio this
		// project runs; a genuine mismatch (1:1 against 16:9) is off by 78%.
		if (render_aspect > output_aspect * 1.04 || render_aspect < output_aspect * 0.96)
		{
			r.reason = "render and output aspect ratios differ - a cubemap face or a "
			           "reflection capture, not the primary view";
			return r;
		}
		// DLSS's most aggressive mode is Ultra Performance at 3x linear. Anything beyond that
		// is not a quality mode, it is a different pass entirely. The upper bound is deliberately
		// loose (3.5) so a future mode does not silently break the primary path.
		const double scale = static_cast<double>(r.output_width) / static_cast<double>(r.render_width);
		if (scale > 3.5)
		{
			r.reason = "upscale factor exceeds anything DLSS performs - not the primary view";
			return r;
		}
	}

	// On a camera cut UE4 swaps velocity and the history colour for the 1x1 BlackDummy.
	// EyeAdaptationTexture is ALSO 1x1 and is present on every frame, so a blanket "any 1x1
	// SRV" test reports a cut every frame — only the velocity and colour slots count.
	bool velocity_present = false;
	for (const auto &t : sig.srvs)
	{
		if (t.format == TexFormat::r16g16b16a16_unorm && !is_dummy(t))
		{
			r.velocity_srv = t.slot;
			velocity_present = true;
		}
	}

	std::uint32_t colours[2] = { 0, 0 };
	std::uint32_t n = 0;
	bool dummy_colour = false;
	for (const auto &t : sig.srvs)
	{
		if (!is_hdr_colour(t.format))
			continue;
		if (is_dummy(t))
			dummy_colour = true;
		else if (n < 2)
			colours[n++] = t.slot;
	}
	r.colour_srv_a = colours[0];
	r.colour_srv_b = colours[1];

	r.camera_cut_dummies = !velocity_present || dummy_colour;

	r.has_downsample_uav = sig.uavs.size() > 1;

	r.verdict = is_known_taa_hash(sig.shader_hash) ? MatchVerdict::hash_and_structural
	                                               : MatchVerdict::structural_only;
	r.reason = (r.verdict == MatchVerdict::hash_and_structural)
	               ? "hash is a cooked FTAAStandaloneCS permutation and structure agrees"
	               : "structure matches, hash unfamiliar (game update? regenerate taa_hashes)";
	return r;
}

} // namespace stray_dlss
