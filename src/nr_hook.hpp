// DLSS Neural Rendering AS A PRESENT STAGE: our own command list, after the game's last
// submission, over the back buffer.
//
// WHY, in one line: NR is not an upscaler — same resolution in, same resolution out — and it sat
// inside the intercepted TAA dispatch only because that is where we already had a hook. Every
// painful thing about it follows from that one choice. The full argument, both problems it
// removes, and what is different from the two post-tonemap sites that were built and deleted on
// 2026-09-02, are in src/core/nr_hook_plan.hpp. Read that first.
//
// WHAT THIS FILE IS. The stage's live half: it takes the guides the TAA hook publishes, asks the
// pure gate whether this frame may be injected into, and — if so — drives
// src/nr_stage.hpp's copies around a `nr::apply(Site::post_tonemap)`. It owns no D3D12 resources
// of its own and it makes no decisions the gate could have made.
//
// PHASE 1 SHIPS `taa` AS THE DEFAULT. Nothing here changes the shipped image until someone sets
// [STRAYDLSS] NgxNRHook=present. Everything this file claims is UNCONFIRMED until a run on the
// box says otherwise.
#pragma once

#include "core/nr_hook_plan.hpp"
#include "intercept/types.hpp"

#include <cstdint>

struct ID3D12Device;
struct ID3D12Resource;

namespace stray_dlss::nrhook {

// --- configuration, read once at startup ---


// [STRAYDLSS] NgxNRStageBackBufferState, default 0 = D3D12_RESOURCE_STATE_PRESENT (which IS
// D3D12_RESOURCE_STATE_COMMON — the same value, 0).
//
// WHAT IT IS FOR. Our transitions name a StateBefore, and D3D12 has no way to ASK what state a
// resource is in. Get it wrong and vkd3d-proton — which has no debug layer to object — does
// something undefined rather than complaining. The default is not a guess in the way the
// NgxNRRestoreState constant was: D3D12 REQUIRES the back buffer to be in PRESENT when
// IDXGISwapChain::Present is called, and both of our triggers run inside that call, after the
// game has submitted every list of the frame. The knob exists because "required by the API" and
// "true on this stack" are not the same sentence, and one ini key is cheaper than a round trip.
void set_back_buffer_state(std::uint32_t d3d12_resource_states);
std::uint32_t back_buffer_state();

// --- the feed ---

// Published by the TAA hook once per frame, immediately after a successful SR/RR evaluate, with
// the depth and the dense motion vectors it just used.
//
// CALLED unconditionally by the TAA hook — this is the only place in the frame where both are
// known-good and known-fresh, and keeping the call site mode-blind means a future live toggle
// needs no new wiring. It RECORDS nothing at `taa`, though: the promise there is that the shipped
// configuration is byte-identical, and it takes a reference on each guide (see the .cpp for why
// this site needs one and the TAA site does not), which is not free even if it is cheap.
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
};

Counters counters();

} // namespace stray_dlss::nrhook
