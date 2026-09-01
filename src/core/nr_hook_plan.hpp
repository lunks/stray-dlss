// Where DLSS Neural Rendering is injected, and whether a given frame may be injected into.
//
// WHY THIS EXISTS AS A PURE MODULE. The hook site is now a three-way choice, and every one of
// its preconditions — the colour target's shape, the freshness of the guides, the back-buffer
// format's typed-UAV support, which render-target bind is the UI boundary — is a decision that
// can be made from plain numbers. Keeping them here means CI can prove the refusal rules instead
// of the user's machine discovering them (CLAUDE.md §0.4). src/nr_hook.cpp does nothing but
// gather these numbers from D3D12 and act on the verdict.
#pragma once

#include <cstdint>

namespace stray_dlss::nrplan {

// [STRAYDLSS] NgxNRHook. The three sites NR can run at, in increasing order of how much of the
// engine has already consumed the frame.
//
// The problem this enum exists to solve is MEASURED (CLAUDE.md, "NR's output feeds the engine's
// temporal history"): at the TAA hook we write `u0`, which UE 4.27 extracts as the next frame's
// HistoryBuffer[0] AND hands downstream as scene colour — one resource, two roles — so NR's
// residual re-enters the engine's temporal state every frame and compounds. Screen-space
// reflections read that history directly (ScreenSpaceRayTracing.cpp:596-620) and the eye-adaptation
// histogram downsamples it (PostProcessing.cpp:626-648). Both loops are closed by moving the hook
// PAST the point where the engine extracts anything: on the desktop deferred path every
// QueueTextureExtraction into PrevFrameViewInfo sits at PostProcessing.cpp 576/599/643 while
// AddTonemapPass is at 777, so a post-tonemap hook has no feedback path by construction.
enum class HookMode
{
	// Inside the intercepted TAA compute dispatch, writing the engine's `u0`. The shipped
	// behaviour and the default: it is the only site the HDR colour codec applies to, and it is
	// the fallback if either post-tonemap site misbehaves.
	taa,
	// reshade::addon_event::reshade_begin_effects — ReShade's own immediate command list, with
	// the back buffer already composited and the UI already drawn on top of it.
	present,
	// The frame's Nth bind of a swapchain back buffer as a render target, on the GAME's command
	// list. After the scene composite, before Slate draws the HUD. The intended end state: it has
	// `present`'s freedom from feedback AND keeps HUD pixels out of the network.
	preui,
};

// Parses the config string. Anything unrecognised — including an empty value — is `taa`, because
// the default must be the shipped behaviour and a typo must never silently move the hook.
HookMode hook_mode_from_string(const char *value);
const char *hook_mode_name(HookMode mode);
// True for the two sites that see a tonemapped, display-referred image and therefore must NOT
// run the HDR colour codec (running the soft-clip + sRGB encode over an already-encoded image
// would be a second application of a transfer that was already applied).
bool is_post_tonemap(HookMode mode);

// Why a post-tonemap frame was, or was not, injected into. Every one is counted and named, for
// the same reason the TAA path's gate refusals are: a hook that never fires must never be
// indistinguishable from a hook that fired and did nothing.
enum class PlanResult
{
	ok = 0,
	// The render-target view resolved to no resource, or the resource is not tracked live.
	// (CLAUDE.md §5: ReShade's view->resource map outlives the resource on D3D12.)
	no_colour,
	// Mipped, arrayed or multisampled. A mipped input to feature 18 is a documented
	// DXGI_ERROR_DEVICE_HUNG a few seconds later, not an error return, so it is refused rather
	// than passed through — a distinct reason from the TAA path's `mipped-input`, because the
	// resource involved is the back buffer rather than UE4's scene colour.
	mipped_colour,
	// A zero-sized target, which is what a minimised or mid-resize swapchain looks like.
	zero_extent,
	// The back buffer's format cannot be written through a typed UAV. NGX writes DLSSNR.Output
	// through one, and a format the device cannot store to is the "black output with no further
	// indication" class of failure (CLAUDE.md §0.2), so refuse loudly.
	no_typed_uav_store,
	// No TAA dispatch has published guides yet this session (a loading screen, the first frames,
	// or a session where the TAA pass is never matched).
	guides_absent,
	// Guides exist but this trigger has already consumed them. The depth and motion vectors must
	// describe the frame we are about to modify, and a capture that has been used once is by
	// definition from an earlier frame.
	guides_stale,
	// The frame ended without reaching the configured back-buffer render-target bind ordinal, so
	// the UI boundary was never observed. FAIL SAFE: skip the frame rather than guess a point.
	// A missed frame is invisible; injecting into the middle of the HUD is not.
	boundary_not_reached,
	// Counted, never refused-on: the trigger fired more than once in a frame.
	already_ran,
	// Our own staging colour texture could not be allocated. Its own name rather than a reused
	// one, because "the driver refused an allocation" and "the game handed us nothing" want
	// completely different responses.
	staging_failed,
};

// NOTE: the count is duplicated in src/core/nr_hook_plan.cpp's kPlanResultNames — change both
// together. Same discipline as nr::kNrRefusalNames.
constexpr int kPlanResultCount = 10;
const char *plan_result_name(PlanResult result);

// What D3D12 told us about the colour target we were handed.
struct ColourDesc
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t mip_levels = 1;
	std::uint32_t array_size = 1;
	std::uint32_t sample_count = 1;
	// The three bits CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT) reports, kept separate
	// rather than collapsed to one bool so the log can say WHICH half is missing. Only the view
	// and store bits are load-bearing — NGX reads DLSSNR.Color through its own path, so a missing
	// typed LOAD is worth a warning and nothing more.
	bool typed_uav_view = false;
	bool typed_uav_load = false;
	bool typed_uav_store = false;
	bool live = false; // is_resource_live(), CLAUDE.md §5
};

// The per-frame capture the TAA path publishes.
//
// `sequence` is a PUBLICATION COUNTER, deliberately not a present index. Freshness is then
// "has this capture been consumed yet?", which is independent of the order in which ReShade
// happens to fire `addon_event::present` and `addon_event::reshade_begin_effects` — an ordering
// this code should not have to depend on, and one a future ReShade could reasonably change. A
// frame with no TAA dispatch at all (a loading screen) simply does not advance the counter, so it
// refuses as stale without needing a separate test.
struct GuideState
{
	bool published = false;
	bool have_depth = false;
	bool have_motion = false;
	std::uint64_t sequence = 0;
	std::uint32_t render_width = 0;
	std::uint32_t render_height = 0;
};

struct Plan
{
	PlanResult result = PlanResult::no_colour;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	// The COLOUR/GUIDE ratio, which is what DLSSNR.MVecScaleX/Y wants: the snippet works on the
	// colour grid while our vectors are on the guide grid, and it takes the colour rect, the mvec
	// rect and this scale as three INDEPENDENT values. dxvk-remix's NGXNeuralRenderingContext
	// sets exactly this and its comment names our case, "2.0 for 4K colour over 1080p guides".
	// Computed per evaluate, never hardcoded: at 50% screen percentage it is 2.0 and at 70% it is
	// 1.42857, and this project runs both. Do NOT regress this to 1.0.
	float mvec_scale_x = 1.0f;
	float mvec_scale_y = 1.0f;
};

// The whole gate, in one place. `last_consumed_sequence` is the highest guide publication this
// trigger has already used; anything at or below it is stale. Zero means "nothing consumed yet".
Plan plan_post_tonemap(const ColourDesc &colour, const GuideState &guides,
                       std::uint64_t last_consumed_sequence);

// The `preui` boundary rule, isolated so it is stated once and tested.
//
// THE SIGNAL, and its provenance. UE 4.27 renders the tonemapped scene into the swapchain back
// buffer through the post-process chain's final pass, and Slate then draws the HUD onto that same
// back buffer in a separate RHI render pass — a separate pass means a separate
// OMSetRenderTargets, which ReShade surfaces as a second
// `bind_render_targets_and_depth_stencil`. So the boundary is a RENDER-TARGET IDENTITY question
// ("is this bind pointing at a swapchain buffer, and is it the Nth such bind this frame?"), not a
// shader-bytecode question — which is the whole point, because pass identification is what has
// cost this project the most time.
//
// UNCONFIRMED, and the reason `[STRAYDLSS] NgxNRPreUiBind` exists: that UE 4.27 in THIS title
// produces exactly two back-buffer render-target binds per frame, composite first and Slate
// second. The add-on logs the per-frame bind census so one run settles the ordinal instead of a
// round trip per guess.
//
// `ordinal` is 1-based and counts only binds whose render target is a swapchain buffer.
bool preui_boundary(std::uint32_t ordinal, std::uint32_t target_ordinal, bool already_ran);

// --- NR's OWN temporal history, and the guide grid it was accumulated against ---
//
// Feature 18 is NOT a per-frame spatial filter. It keeps an internal accumulator: it consumes
// motion vectors and depth (which a spatial-only network would not need) and exposes
// `DLSSNR.Reset`, named `settings.resetAccumulation` in the reference deployment.
//
// The consequence, verbatim from that deployment
// (github.com/lunks/dxvk-remix-plus-dlssnr @ fc4de144,
//  src/dxvk/rtx_render/rtx_neural_rendering.cpp:220-230):
//
//   "The NGX feature is keyed on the colour grid alone, so switching DLSS quality at a fixed
//    output resolution moves the guide grid --- and DLSSNR.MVecScaleX/Y with it --- underneath a
//    temporal history that was accumulated against the old one. Nothing else notices, so latch
//    the guide extent here and force a single reset frame when it moves."
//
// This is a LIVE bug for us and not a hypothetical: the feature is created for the OUTPUT rect,
// which does not change when the screen percentage does, so nothing else in our code notices that
// 1920x1080 guides became 2688x1512 guides and that MVecScale went from 2.0 to 1.42857 under an
// accumulated history.
struct GuideExtentLatch
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

// Records this frame's guide extent and answers whether NR's accumulation must be reset.
//
// Returns FALSE on the first observation, exactly as the reference does (`resetGuideHistory =
// (latched != 0)`): there is no history to invalidate before the first evaluate, and treating the
// first frame as a change would put a spurious reset into every session.
bool latch_guide_extent(GuideExtentLatch &latch, std::uint32_t width, std::uint32_t height);

} // namespace stray_dlss::nrplan
