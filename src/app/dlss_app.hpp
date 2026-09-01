// The framework-free application: what used to be addon.cpp's event handlers, behind
// icept::Sink. It knows the D3D12 device, the seam and the project's own modules — and no
// ReShade, UE4SS or ImGui type. A host (src/backend_reshade/addon_entry.cpp today, the UE4SS
// plugin later) installs a config Source, a log sink and an icept::Backend, asks
// configure_events() which events it must deliver, and forwards them here.
#pragma once

#include "intercept/backend.hpp"

#include <cstddef>
#include <cstdint>

namespace stray_dlss::app {

// Which events the host has to deliver, decided from the ini (HashShaders, PassFinder,
// GBufferFinder, NgxRR, NgxNRHook). Registering the pipeline events is not free under
// ReShade — it drops the PSO cache (docs/RESEARCH.md §2.5) — which is why this is a decision
// rather than "everything, always".
struct EventNeeds
{
	bool pipeline_events = false;    // on_pipeline (init/destroy)
	bool finder_rt_events = false;   // on_render_targets + on_draw (either finder)
	bool pass_finder_events = false; // on_copy + on_execute
	bool nr_preui_events = false;    // on_swapchain + on_render_targets for NgxNRHook=preui
	bool nr_present_events = false;  // reshade_begin_effects (ReShade-only; the host wires it)
};

// Live DLSS Neural Rendering tuning (the overlay's state, formerly NrUiState). Every value is
// re-sent to the NGX parameter block on EVERY evaluate, and the codec's three ride in push
// constants on every dispatch, so a change takes effect on the next frame with no feature
// recreation. Seeded from the ini in on_device; a host may edit it and call apply_nr_live().
struct NrLive
{
	bool  enabled = false;
	float intensity = 1.0f;
	float local_tone = 1.74f;
	float local_structure = 1.0f;
	float skin_structure = 1.33f;
	int   preset = 1;
	bool  auto_mask = true;
	bool  ui_correction = true;
	float paper_white = 1.0f;
	// The reference's `trackAutoExposure`, default TRUE there and here. See ngx_nr.hpp.
	bool  track_exposure = true;
	float color_strength = 1.0f;
	float transfer_strength = 1.0f;
	bool  smooth_exposure = true;     // the toggle; when off the rate is forced to 1.0
	float scale_reset_tol = 0.15f;    // ratio drift before NR's history is discarded; 0 = never
	float exposure_smoothing = 0.05f; // per-frame weight of the new exposure sample; 1.0 = off
	float mvec_scale = 0.0f; // 0 = use the built-in default (1.0)
	int   mv_convention = 0;  // index into kMvConventions
	bool  mv_invert_x = false;
	bool  mv_invert_y = false;
	bool  mv_legacy_clip = false; // reproduce the pre-row_major transposed camera branch
	// [STRAYDLSS] NgxNRRestoreHistory, default OFF (histplan::Config carries the reasoning).
	// Live, and deliberately so: it is the A/B for the SSR drift. Overwritten at startup with
	// what the ini said. (src/nr_history.hpp)
	bool  restore_history = false;
};

// Which branch of the resolve gets its sign flipped (mv_resolve.hpp). Four entries.
extern const char *const kMvConventions[4];

// A snapshot for a host's status display.
struct Status
{
	bool have_device = false;
	bool is_vkd3d = false;
	std::uint64_t frame = 0;
	std::uint32_t compute_pipelines = 0;
	std::uint32_t taa_pipelines = 0;
	std::uint32_t dispatches = 0;
	bool ngx_attempted = false;
	std::uint32_t shader_census = 0;
	int extra_hashes_loaded = -1; // -1: no stray-dlss-hashes.txt
};

class DlssApp final : public icept::Sink
{
public:
	// Reads every [STRAYDLSS] key that decides WHICH events the host must deliver, and
	// configures the modules those keys govern. Call once, before any event.
	EventNeeds configure_events();

	// --- icept::Sink ---
	void on_device(::ID3D12Device *native, bool created) override;
	void on_pipeline(std::uint64_t pso, const void *dxbc, std::size_t len, icept::ShaderKind kind,
	                 bool created) override;
	void on_command_list(const icept::CommandContext &ctx, bool created) override;
	void on_command_list_reset(const icept::CommandContext &ctx) override;
	void on_bind_pipeline(const icept::CommandContext &ctx, std::uint64_t pso) override;
	bool on_dispatch(const icept::CommandContext &ctx, std::uint32_t x, std::uint32_t y, std::uint32_t z) override;
	void on_render_targets(const icept::CommandContext &ctx, std::uint32_t count,
	                       const icept::DescriptorId *rtvs, icept::DescriptorId dsv) override
	{
		on_render_targets(ctx, count, rtvs, dsv, /*via_render_pass=*/false);
	}
	// `via_render_pass` distinguishes begin_render_pass from OMSetRenderTargets: the finders
	// take both, the NR pre-UI trigger only the latter (as it always did).
	void on_render_targets(const icept::CommandContext &ctx, std::uint32_t count,
	                       const icept::DescriptorId *rtvs, icept::DescriptorId dsv, bool via_render_pass);
	void on_draw(const icept::CommandContext &ctx, std::uint32_t vertex_or_index_count) override;
	void on_copy(const icept::CommandContext &ctx, icept::ResourceId src, icept::ResourceId dst) override;
	void on_execute(const icept::CommandContext &ctx) override;
	void on_swapchain(const icept::ResourceId *back_buffers, std::uint32_t count, bool created) override;
	void on_present(const icept::PresentContext &ctx) override;

	// The present index is the app's; the host asks for the next one when it builds the
	// PresentContext, so on_present and the host's own per-frame work agree on the number.
	std::uint64_t next_frame();

	// --- for hosts ---
	Status status() const;
	NrLive &nr_live();
	void apply_nr_live();
	// stray-dlss-hashes.txt / stray-dlss-ssd-hashes.txt in the game directory.
	void load_hash_override_file();
	// On detach. The two flags are the ReShade host's capability check (bind_pipeline and
	// push_descriptors both observed); a native host passes true.
	void log_final_census(bool saw_bind_pipeline, bool saw_push_descriptors);
};

DlssApp &instance();

} // namespace stray_dlss::app
