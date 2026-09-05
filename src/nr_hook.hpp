// DLSS Neural Rendering AS A PRESENT STAGE: our own command list, after the game's last
// submission, over the back buffer.
//
// WHY, in one line: NR is not an upscaler — same resolution in, same resolution out — and it sat
// inside the intercepted TAA dispatch only because that is where we already had a hook. Every
// painful thing about it followed from that one choice. The full argument, and both problems this
// site removes, are in src/core/nr_hook_plan.hpp. Read that first.
//
// WHAT THIS FILE IS. The stage's live half, and since 2026-09-03 the ONLY site NR has: it takes
// the guides the TAA hook publishes, asks the pure gate whether this frame may be injected into,
// and — if so — drives src/nr_stage.hpp's copies around a `nr::apply`. It owns no D3D12 resources
// of its own and it makes no decisions the gate could have made.
//
// Confirmed working in the game by the user on 2026-09-03, which is what retired the TAA site,
// the HDR colour codec that site's raw linear HDR needed, and the history restore its feedback
// node needed.
#pragma once

#include "core/nr_hook_plan.hpp"
#include "core/nr_mask_plan.hpp"
#include "intercept/types.hpp"

#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;
struct ID3D12GraphicsCommandList;

namespace stray_dlss::nrhook {

// --- configuration, read once at startup ---

// [STRAYDLSS] NgxNRStageBackBufferState, default 0 = D3D12_RESOURCE_STATE_PRESENT (which IS
// D3D12_RESOURCE_STATE_COMMON — the same value, 0).
//
// WHAT IT IS FOR. Our transitions name a StateBefore, and D3D12 has no way to ASK what state a
// resource is in. Get it wrong and vkd3d-proton — which has no debug layer to object — does
// something undefined rather than complaining. The default is not a guess, though: D3D12 REQUIRES
// the back buffer to be in PRESENT when IDXGISwapChain::Present is called, and our trigger runs
// inside that call, after the game has submitted every list of the frame. (The now-deleted
// history restore had no such guarantee — its state constant was derived from four UE 4.27 source
// anchors and zero measurements, which is the difference this comment used to draw.) The knob
// exists because "required by the API" and "true on this stack" are not the same sentence, and
// one ini key is cheaper than a round trip.
void set_back_buffer_state(std::uint32_t d3d12_resource_states);
std::uint32_t back_buffer_state();

// [STRAYDLSS] NgxNRPreScale (default ON on this branch): run feature 18 BEFORE the upscaler, on
// the 1080p InputSceneColor (t1), with NO codec - raw linear pre-exposed HDR straight into a
// display-referred network - and let RR (or SR) consume the enhanced input. An EXPERIMENT, by
// the user's decision on 2026-09-05: "build in a way I can add it before RR without the codec
// just to try". Three things are traded for a ~4x cheaper NR (1080p instead of 4K) and guides
// at a 1.0 ratio: the input domain (CLAUDE.md: the codec IS the input domain), the fact that the
// pre-TAA frame is sub-pixel jittered every frame and NR has no jitter input (the param audit's
// closed list has nothing jitter-shaped), and RR then denoising a pre-enhanced image. The
// prediction is shimmer; the screen decides. While it is on, the present-time stage is skipped -
// on this branch NR runs before RR ONLY.
void set_prescale(bool on);
bool prescale();
// The pre-scale call itself, from the TAA hook, on the game's list, after the inputs have been
// barriered to NON_PIXEL_SHADER_RESOURCE (which is the state nr::apply assumes for its image)
// and before the RR/SR evaluate. Writes NR's result back over `colour` in place.
bool apply_prescale(ID3D12Device *device, ID3D12GraphicsCommandList *cmd,
	ID3D12Resource *colour, ID3D12Resource *depth, ID3D12Resource *motion_vectors,
	std::uint32_t render_width, std::uint32_t render_height, bool reset);

// [STRAYDLSS] NgxNRMask + NgxNRMaskR/G/B and their three enables, default OFF.
//
// DLSSNR.ControlMask is a per-pixel RGB control texture the runtime genuinely consumes (the
// disassembly is in src/core/nr_mask_plan.hpp). This is the only channel through which structure
// and tone can vary SPATIALLY — every other strength the runtime takes is a global scalar
// broadcast to every pixel.
//
// It costs something, and the cost is not obvious: binding a mask AT ALL forces
// DLSSNR.UseAutoMask to 0 and drives both resolved structure strengths to the -1.0 sentinel,
// whatever NgxNRSkinStructure and NgxNRLocalStructure were set to. So "mask on" is never a pure
// addition, and the A/B has to be judged with that in mind — which is exactly why the first
// experiment is an IDENTITY mask (all channels 1.0): it isolates "does binding a mask change the
// image at all" from "do the mask's VALUES change the image".
void set_mask(const nrmaskplan::Config &cfg);
nrmaskplan::Config mask_config();

// --- the feed ---

// Published by the TAA hook once per frame, immediately after a successful SR/RR evaluate, with
// the depth and the dense motion vectors it just used.
//
// CALLED unconditionally by the TAA hook — that is the only place in the frame where both are
// known-good and known-fresh. It RECORDS nothing: this is a publish, not a pass. It does take a
// reference on each guide (see the .cpp: publish and consume are on different threads and several
// milliseconds apart, so the resource can be recycled in between), which is not free even if it
// is cheap.
//
// `reset` is the camera-cut OR the TAA path already computes (CLAUDE.md §2.8) and MUST travel
// with them: feature 18 keeps its OWN temporal history, and a cut that does not reset it is the
// "flicker between a frozen image and fog" class of bug this project has been bitten by once.
void note_guides(ID3D12Resource *depth, ID3D12Resource *motion_vectors,
                 std::uint32_t render_width, std::uint32_t render_height, bool reset);

// --- the trigger ---

// THE STAGE. Called from DlssApp::on_present, AFTER nr::on_present.
//
// WHY AFTER, and it is not a style choice. nr::on_present signals the NR lifetime fence on the
// presenting queue and advances the timeline; work recorded BEFORE that signal is tagged with the
// value the signal carries. But our present list executes after this callback returns (the native
// present owner runs ExecuteCommandLists inside the Present hook; ReShade flushes its immediate
// list at dxgi_swapchain.cpp:1009), so a tag taken before the signal would be reported complete
// while our commands had not run — and everything NR defers, including ReleaseFeature, would be
// freed under an in-flight list. Recording after the signal tags this work with the NEXT present's
// value, which is queued after our list has been executed. That is correct, and it costs one
// frame of extra retention.
//
// THE ONE COSMETIC CONSEQUENCE, stated so it is not mistaken for a bug: nrplan::note_frame_boundary
// also runs at the top of nr::on_present, so on the very first stage frame it sees "nothing
// evaluated yet" and arms one DLSSNR.Reset. That reset lands on a freshly created feature, which
// carries a reset anyway. It is one frame, once.
//
// A frame the stage cannot serve declines with a named, counted reason and NEVER evaluates on a
// guess. The declined frame is a hole in feature 18's temporal continuity, and it is armed for it
// by note_frame_boundary at the next present with no help from here.
void on_present(const icept::PresentContext &pc, ID3D12Device *device);

// Device destruction. Frees the staging textures; the caller has made the GPU idle.
void shutdown();

// --- telemetry ---

struct Counters
{
	std::uint64_t triggered = 0; // presents at which the stage was asked
	std::uint64_t applied = 0;   // presents at which the neural result reached the back buffer
	std::uint32_t reasons[nrplan::kPlanResultCount] = {};
	std::uint32_t last_back_buffer_width = 0;
	std::uint32_t last_back_buffer_height = 0;
	std::uint32_t last_back_buffer_format = 0;
	// The colour/guide ratio the gate computed for the most recent accepted frame. NOT what is
	// sent to NGX — see nrplan::Plan::mvec_scale_x — but the number to compare against on the box.
	float last_mvec_scale_x = 0.0f;
	float last_mvec_scale_y = 0.0f;
	std::uint64_t staging_bytes = 0;
	// The mask's own verdict for the most recent triggered frame, and what the live texture
	// actually holds. "Bound and deliberately neutral" and "refused" must never look alike in a
	// log.
	nrmaskplan::MaskResult last_mask_result = nrmaskplan::MaskResult::disabled;
	bool mask_bound = false;
	float mask_r = 0.0f;
	float mask_g = 0.0f;
	float mask_b = 0.0f;
	std::uint64_t mask_fills = 0;
	std::uint64_t mask_bytes = 0;
};

Counters counters();

} // namespace stray_dlss::nrhook
