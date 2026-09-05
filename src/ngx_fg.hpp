// DLSS Frame Generation through the NGX core: NVSDK_NGX_Feature_FrameGeneration (11), the
// `nvngx_dlssg.dll` snippet, driven WITHOUT Streamline. The snippet has no present path, no
// queue and no pacer (facts §32.1) — EvaluateFeature records interpolation work onto the
// command list it is handed and presenting is the caller's job, which is the present owner's
// present-twice path (src/backend_native/fg_present.hpp). This module is the fg::Generator
// that path calls: it owns the feature, the parameter block and the copies of the per-frame
// guides the TAA hook publishes.
//
// Every DLSSG.* name written here was confirmed by exact null-terminated search over the
// snippet (facts §32.2, tools/ngx_param_names.py). An unknown name is silently ignored
// (CLAUDE.md §5), so a name NOT in that list must never be added without re-running the search.
//
// INPUTS (facts §32.2/31.3): DLSSG.Backbuffer = the game's final frame (the replacement back
// buffer), DLSSG.Depth and DLSSG.MVecs = the same depth and dense RG16F motion vectors SR
// consumes, copied into FG-owned textures at the TAA hook because the engine's depth is a
// pooled target whose content at present time is not guaranteed; jitter, camera-cut reset,
// the jitter-free ViewToClip and ClipToPrevClip (row-major, row-vector, UE4's own), near
// plane, FOV. HUD-less (stage 3) is the graphics seam's copy, src/hudless.hpp, bound as
// DLSSG.HUDLess when a copy exists for the frame and NULLed when it does not.
#pragma once

#include "backend_native/fg_present.hpp"

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace stray_dlss::ngxfg {

struct Config
{
	int hdr = -1;              // [STRAYDLSS] NgxFGHDR: -1 auto (R10G10B10A2 + HDR10 colour space, or R10G10B10A2 alone), 0, 1
	int warmup_presents = 60;  // [STRAYDLSS] NgxFGWarmupFrames: presents before the first CreateFeature (NR's lesson: never at device init)
	// [STRAYDLSS] NgxFGCameraFar. NVIDIA's own value on BOTH engine generations, from
	// StreamlineViewExtension.cpp's r.Streamline.CustomCameraFarPlane, whose help text says
	// outright that it "does not need to match corresponding value used by engine". Passing 0
	// to mean "infinite" was OUR invention: sl_consts.h makes near/far non-optional with
	// INVALID_FLOAT as the unset sentinel, so 0.0 is a degenerate near-zero depth RANGE, not a
	// way of saying infinite — and DLSS-G's camera reconstruction divides by that range.
	// (docs/RESEARCH-DLSS-UE5-PLUGIN.md §5.3, HARD.) 0 still means "send 0" for A/B.
	float camera_far = 75000.0f;
	// [STRAYDLSS] NgxFGCameraNear. Same source, same "need not match the engine" caveat:
	// NVIDIA hardcodes 0.01. We previously sent the real measured View.NearPlane (row 142.x),
	// which is more faithful to the engine and is exactly why it may be wrong here — the pair
	// is what DLSS-G reconstructs against, and NVIDIA ships a constant pair. <= 0 keeps the
	// measured value, so the old behaviour is one key away.
	float camera_near = 0.01f;
	int mvec_scale_mode = 1;   // [STRAYDLSS] NgxFGMvecScale: 1 = 1/renderW,1/renderH (pixels -> SL's [-1,1] convention, guide §7.0), 2 = 1.0
	bool provide_output_real = true; // [STRAYDLSS] NgxFGOutputReal: also hand the snippet a DLSSG.OutputReal texture (Nukem's replacement copies the back buffer into it)
};
void configure(const Config &cfg);
const Config &config();

// The per-frame constants the TAA hook publishes with the guide copies.
struct FrameConstants
{
	float jitter_x = 0.0f, jitter_y = 0.0f; // TemporalAAParams.zw, render-res pixels (CLAUDE.md §2.7)
	bool reset = false;                     // the camera-cut OR (CLAUDE.md §2.8)
	std::uint32_t render_width = 0, render_height = 0;
	float clip_to_prev_clip[16] = {};       // View row 122, row-major
	float view_to_clip_no_aa[16] = {};      // View row 32, row-major, jitter-free
	float translated_world_to_view[16] = {}; // View rows 12-15
	float near_plane = 0.0f;                // View row 142.x
	float delta_time_s = 0.0f;              // View row 143.x
	float pre_exposure = 1.0f;
	std::uint64_t frame = 0;                // the present index the TAA hook saw
};

// Called from the TAA hook on the GAME's list right after the SR evaluate, while depth is in
// NON_PIXEL_SHADER_RESOURCE and the resolved motion vectors are in the SRV state the evaluate
// used. Copies both into FG-owned textures (recreated on a size/format change) and publishes
// the constants; the present-time generate() consumes the newest publish once.
void publish(ID3D12Device *device, ID3D12GraphicsCommandList *game_list, ID3D12Resource *depth,
             ID3D12Resource *motion_vectors, const FrameConstants &c);

// The fg::Generator the present owner drives. Installed by the application at device init
// when NgxFG=1 and NgxFGMode=2; NGX must already be initialised (ngx::status().initialised)
// before the first generate() will attempt CreateFeature.
native::fg::Generator &generator();

// Releases the feature and every texture. GPU idle first.
void shutdown();

struct Stats
{
	bool created = false;
	unsigned create_result = 0;      // raw NVSDK_NGX_Result of the last CreateFeature
	std::uint64_t publishes = 0;
	std::uint64_t generates = 0;      // generate() calls that evaluated
	std::uint64_t evaluate_failures = 0;
	std::uint64_t refused_no_publish = 0; // generate() with no fresh guides this frame
	std::uint64_t refused_warmup = 0;
	std::uint64_t refused_not_ready = 0;  // NGX not initialised / create failed / latched
	std::uint64_t hudless_bound = 0;      // evaluates handed a DLSSG.HUDLess copy of this frame
	std::uint64_t hudless_absent = 0;     // ...and evaluates that wrote NULL (no copy this frame)
	std::uint32_t width = 0, height = 0, render_width = 0, render_height = 0;
	int hdr = -1;
	unsigned multi_frame_count_max = 0; // DLSSG.MultiFrameCountMax as populated by the core, 0 if absent
	unsigned last_evaluate_result = 0;
};
Stats stats();
const char *last_error();

} // namespace stray_dlss::ngxfg
