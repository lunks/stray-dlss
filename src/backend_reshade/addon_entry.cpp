// stray-dlss — the ReShade add-on entry point: DllMain, the event registrations and the
// overlay. Every event is a lambda translating ReShade's arguments into a call on the
// framework-free application (src/app/dlss_app.hpp) through the seam (src/intercept/).
// This directory is the ONLY place ReShade's headers are included (CI enforces it).

#include "reshade_all.hpp"

#include "reshade_backend.hpp"
#include "reshade_bindings.hpp"
#include "reshade_host.hpp"

#include "app/dlss_app.hpp"
#include "backend_native/native_backend.hpp"
#include "core/taa_hashes.hpp"
#include "host/config.hpp"
#include "log.hpp"
#include "ngx_backend.hpp"
#include "ngx_nr.hpp"
#include "shader_dump.hpp"

#include <descriptor_tracking.hpp>
#include <state_tracking.hpp>

#include <d3d12.h>

#include <atomic>
#include <iterator>

extern "C" __declspec(dllexport) const char *NAME = "Stray DLSS";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Injects NVIDIA DLSS Super Resolution into Stray (UE 4.27.2, D3D12) by replacing the "
	"engine's FTAAStandaloneCS temporal AA pass.";
extern "C" __declspec(dllexport) const char *WEBSITE = "https://github.com/lunks/stray-dlss";

namespace {

using namespace stray_dlss;

// The add-on-level events only exist when ReShade was built with RESHADE_ADDON >= 2, i.e.
// the full add-on build. Without them we can observe nothing useful, and the log must say
// so plainly rather than leaving "wrong ReShade build" and "UE4 binds differently"
// indistinguishable. (docs/RESEARCH.md §2.2) A ReShade-host concern, so it lives here.
std::atomic<bool> g_saw_bind_pipeline{ false };
std::atomic<bool> g_saw_push_descriptors{ false };
std::atomic<bool> g_reported_capability_verdict{ false };

app::EventNeeds g_needs;

// NativeMode=drive while ReShade is still the host (plan Stage 3): the native backend answers
// everything the TAA hook asks — bindings, the View CB, liveness, the restore — and the
// ReShade backend keeps exactly the parts that belong to ReShade's own objects: the barrier
// on ITS immediate list at present (recorded through ReShade's API so the list is flushed,
// CLAUDE.md §5), and the graphics-side state its trackers still watch (viewports/scissors for
// the pre-UI NR site, the pass finder's graphics SRVs). Stage 4 replaces those with our own.
class DriveBackend final : public icept::Backend
{
public:
	const char *name() const override { return "native(drive)+reshade(present)"; }
	bool resolve_compute_bindings(const icept::CommandContext &ctx, icept::DispatchBindings &out) override { return native::backend().resolve_compute_bindings(ctx, out); }
	bool resolve_graphics_srvs(const icept::CommandContext &ctx, std::vector<BoundTexture> &out) override { return rsb::backend().resolve_graphics_srvs(ctx, out); }
	void describe_view(icept::DescriptorId view, std::uint32_t reg, std::vector<BoundTexture> &out) override { native::backend().describe_view(view, reg, out); }
	bool describe_resource(icept::ResourceId res, icept::ResourceInfo &out) override { return native::backend().describe_resource(res, out); }
	bool resource_from_view(icept::DescriptorId view, icept::ResourceId &out) override { return native::backend().resource_from_view(view, out); }
	bool read_buffer(const icept::BufferRange &range, std::uint64_t bytes, void *out) override { return native::backend().read_buffer(range, bytes, out); }
	bool is_resource_live(icept::ResourceId res) override { return native::backend().is_resource_live(res); }
	void restore_game_compute_state(const icept::CommandContext &ctx) override { native::backend().restore_game_compute_state(ctx); }
	void present_barrier(const icept::PresentContext &ctx, icept::ResourceId res, std::uint32_t before, std::uint32_t after) override { rsb::backend().present_barrier(ctx, res, before, after); }
	void dump_tracker_state(const icept::CommandContext &ctx, const char *why) override { native::backend().dump_tracker_state(ctx, why); }
};
DriveBackend g_drive_backend;

bool native_drives() { return native::mode() == native::Mode::drive; }

// After the application has read NativeMode and installed (or not) the native hooks: which
// backend the seam hands out, decided from what actually installed rather than from the ini.
void select_backend()
{
	if (native_drives())
	{
		icept::set_backend(&g_drive_backend);
		STRAY_LOG_WARN("seam: backend is %s. The ReShade host still delivers device/list lifetime, "
			"present, swapchain, the render-target/draw/copy taps and PIXEL pipelines; the native "
			"hooks deliver compute pipelines, pipeline binds, list resets and dispatches.",
			icept::backend()->name());
	}
	else
	{
		icept::set_backend(&rsb::backend());
	}
}

// ---- the events, each a translation into the seam ----

void on_init_device(reshade::api::device *device)
{
	if (device->get_api() != reshade::api::device_api::d3d12)
	{
		STRAY_LOG_ERROR("Device is not D3D12 (api=%d). This add-on does nothing on other APIs.",
			static_cast<int>(device->get_api()));
		return;
	}
	rsb::set_device(device);
	// get_native() returns uint64_t and hands back the ORIGINAL vkd3d device, not a ReShade
	// proxy. That is exactly what NGX must be initialised with. (docs/RESEARCH.md §1.2)
	app::instance().on_device(reinterpret_cast<ID3D12Device *>(device->get_native()), true);
	select_backend();
}

void on_destroy_device(reshade::api::device *device)
{
	if (device->get_api() != reshade::api::device_api::d3d12)
		return;
	app::instance().on_device(reinterpret_cast<ID3D12Device *>(device->get_native()), false);
	select_backend(); // the native hooks are gone with the device: back to ReShade's answers
	rsb::set_device(nullptr);
}

void on_init_command_list(reshade::api::command_list *cmd_list)
{
	app::instance().on_command_list(rsb::context_for(cmd_list), true);
}

void on_reset_command_list(reshade::api::command_list *cmd_list)
{
	rsb::reset_command_list_state(cmd_list);
	if (!native_drives()) // the native Reset hook delivers it in drive mode
		app::instance().on_command_list_reset(rsb::context_for(cmd_list));
}

void on_push_constants(
	reshade::api::command_list *cmd_list,
	reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout,
	uint32_t layout_param,
	uint32_t first,
	uint32_t count,
	const void *values) // const void*, not const uint32_t* — see reshade_events.hpp:1896
{
	rsb::note_push_constants(cmd_list, stages, layout, layout_param, first, count, values);
}

void on_init_pipeline_layout(
	reshade::api::device *device,
	uint32_t count,
	const reshade::api::pipeline_layout_param *params,
	reshade::api::pipeline_layout layout)
{
	rsb::note_pipeline_layout(device, count, params, layout);
}

void on_destroy_pipeline_layout(reshade::api::device *device, reshade::api::pipeline_layout layout)
{
	(void)device;
	rsb::forget_pipeline_layout(layout);
}

void on_init_pipeline(
	reshade::api::device *device,
	reshade::api::pipeline_layout layout,
	uint32_t subobject_count,
	const reshade::api::pipeline_subobject *subobjects,
	reshade::api::pipeline pipeline)
{
	(void)device;
	(void)layout;

	for (uint32_t i = 0; i < subobject_count; ++i)
	{
		const bool is_compute =
			subobjects[i].type == reshade::api::pipeline_subobject_type::compute_shader;
		const bool is_pixel =
			subobjects[i].type == reshade::api::pipeline_subobject_type::pixel_shader;
		if (!is_compute && !is_pixel)
			continue;

		const auto *shader = static_cast<const reshade::api::shader_desc *>(subobjects[i].data);
		if (shader == nullptr || shader->code == nullptr || shader->code_size == 0)
			continue;
		// In drive mode the native creation hooks deliver COMPUTE pipelines (the ones the TAA
		// hook identifies); the pixel ones the finders want still come from here.
		if (is_compute && native_drives())
			continue;

		app::instance().on_pipeline(pipeline.handle, shader->code, shader->code_size,
			is_compute ? icept::ShaderKind::compute : icept::ShaderKind::pixel, true);
	}
}

void on_destroy_pipeline(reshade::api::device *device, reshade::api::pipeline pipeline)
{
	(void)device;
	app::instance().on_pipeline(pipeline.handle, nullptr, 0, icept::ShaderKind::compute, false);
}

void on_bind_pipeline(
	reshade::api::command_list *cmd_list,
	reshade::api::pipeline_stage stages,
	reshade::api::pipeline pipeline)
{
	// NOTE: in D3D12 this fires with pipeline_stage::all, NOT compute_shader. Filtering on
	// the compute stage here would silently miss every event. (docs/RESEARCH.md §2.3)
	(void)stages;
	g_saw_bind_pipeline.store(true, std::memory_order_relaxed);
	if (native_drives()) // the native SetPipelineState hook delivers it in drive mode
		return;
	app::instance().on_bind_pipeline(rsb::context_for(cmd_list), pipeline.handle);
}

void on_push_descriptors(
	reshade::api::command_list *cmd_list,
	reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout,
	uint32_t layout_param,
	const reshade::api::descriptor_table_update &update)
{
	g_saw_push_descriptors.store(true, std::memory_order_relaxed);
	rsb::note_push_descriptors(cmd_list, stages, layout, layout_param, update);
}

bool on_dispatch(reshade::api::command_list *cmd_list, uint32_t x, uint32_t y, uint32_t z)
{
	// In drive mode the decision is the native Dispatch hook's, which fires inside ReShade's
	// forward of this very call: let it through here, unconditionally, and never twice.
	if (native_drives())
		return false;
	return app::instance().on_dispatch(rsb::context_for(cmd_list), x, y, z);
}

void on_present(
	reshade::api::command_queue *queue,
	reshade::api::swapchain *swapchain,
	const reshade::api::rect *source_rect,
	const reshade::api::rect *dest_rect,
	uint32_t dirty_rect_count,
	const reshade::api::rect *dirty_rects)
{
	(void)source_rect;
	(void)dest_rect;
	(void)dirty_rect_count;
	(void)dirty_rects;

	app::DlssApp &a = app::instance();
	const std::uint64_t frame = a.next_frame();
	a.on_present(rsb::present_context_for(queue, swapchain, frame));

	// Report the add-on-level capability verdict once, after enough frames that the game has
	// certainly bound something.
	if (frame == 300 && !g_reported_capability_verdict.exchange(true))
	{
		const bool full_addon_build =
			g_saw_bind_pipeline.load(std::memory_order_relaxed) &&
			g_saw_push_descriptors.load(std::memory_order_relaxed);

		if (full_addon_build)
		{
			STRAY_LOG_INFO("Add-on event check OK: bind_pipeline and push_descriptors both observed");
		}
		else
		{
			STRAY_LOG_ERROR("Add-on event check FAILED after 300 frames "
				"(bind_pipeline=%d push_descriptors=%d).",
				g_saw_bind_pipeline.load(std::memory_order_relaxed) ? 1 : 0,
				g_saw_push_descriptors.load(std::memory_order_relaxed) ? 1 : 0);
			STRAY_LOG_ERROR("  These events require a ReShade build with full add-on support "
				"(RESHADE_ADDON >= 2). Reinstall ReShade 6.8+ with add-on support enabled.");
		}

		const app::Status st = a.status();
		STRAY_LOG_INFO("Census at frame 300: compute pipelines=%u, TAA matches=%u, dispatches=%u",
			st.compute_pipelines, st.taa_pipelines, st.dispatches);
	}
}

// ---- finder taps (pass finder + G-buffer finder) and the NR hook sites ----
//
// The render-target and draw events feed BOTH diagnostics and are registered when either
// [STRAYDLSS] PassFinder=1 or [STRAYDLSS] GBufferFinder=1; the copy and execute events are
// pass-finder-only. Conditional registration matters: every extra event ReShade dispatches
// costs a call per operation even when the handler early-outs, and both finders are
// diagnostics. All of the skip-capable ones return false — the finders observe, never
// suppress.

void on_bind_render_targets(reshade::api::command_list *cmd_list, uint32_t count,
	const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
	// resource_view::handle IS the real D3D12_CPU_DESCRIPTOR_HANDLE.ptr on D3D12 (docs/RESEARCH.md
	// §2.4); DescriptorId is that same integer, so this is a reinterpretation.
	static_assert(sizeof(reshade::api::resource_view) == sizeof(icept::DescriptorId));
	app::instance().on_render_targets(rsb::context_for(cmd_list), count,
		reinterpret_cast<const icept::DescriptorId *>(rtvs), dsv.handle, /*via_render_pass=*/false);
}

bool on_begin_render_pass(reshade::api::command_list *cmd_list, uint32_t count,
	const reshade::api::render_pass_render_target_desc *rts,
	const reshade::api::render_pass_depth_stencil_desc *ds,
	reshade::api::render_pass_flags flags)
{
	(void)flags;
	// D3D12 allows at most 8 simultaneous render targets, so the fixed array cannot clip.
	icept::DescriptorId views[8] = {};
	const uint32_t n = count < 8 ? count : 8;
	for (uint32_t i = 0; i < n; ++i)
		views[i] = rts[i].view.handle;
	app::instance().on_render_targets(rsb::context_for(cmd_list), n, views,
		ds != nullptr ? ds->view.handle : 0, /*via_render_pass=*/true);
	return false;
}

bool on_draw(reshade::api::command_list *cmd_list, uint32_t vertex_count,
	uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
	(void)instance_count;
	(void)first_vertex;
	(void)first_instance;
	app::instance().on_draw(rsb::context_for(cmd_list), vertex_count);
	return false;
}

bool on_draw_indexed(reshade::api::command_list *cmd_list, uint32_t index_count,
	uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
	(void)instance_count;
	(void)first_index;
	(void)vertex_offset;
	(void)first_instance;
	app::instance().on_draw(rsb::context_for(cmd_list), index_count);
	return false;
}

bool on_copy_resource(reshade::api::command_list *cmd_list,
	reshade::api::resource source, reshade::api::resource dest)
{
	app::instance().on_copy(rsb::context_for(cmd_list), source.handle, dest.handle);
	return false;
}

bool on_copy_texture_region(reshade::api::command_list *cmd_list,
	reshade::api::resource source, uint32_t source_subresource,
	const reshade::api::subresource_box *source_box, reshade::api::resource dest,
	uint32_t dest_subresource, const reshade::api::subresource_box *dest_box,
	reshade::api::filter_mode filter)
{
	(void)source_subresource;
	(void)source_box;
	(void)dest_subresource;
	(void)dest_box;
	(void)filter;
	app::instance().on_copy(rsb::context_for(cmd_list), source.handle, dest.handle);
	return false;
}

bool on_resolve_texture_region(reshade::api::command_list *cmd_list,
	reshade::api::resource source, uint32_t source_subresource,
	const reshade::api::subresource_box *source_box, reshade::api::resource dest,
	uint32_t dest_subresource, uint32_t dest_x, uint32_t dest_y, uint32_t dest_z,
	reshade::api::format format)
{
	(void)source_subresource;
	(void)source_box;
	(void)dest_subresource;
	(void)dest_x;
	(void)dest_y;
	(void)dest_z;
	(void)format;
	app::instance().on_copy(rsb::context_for(cmd_list), source.handle, dest.handle);
	return false;
}

void on_execute_command_list(reshade::api::command_queue *queue,
	reshade::api::command_list *cmd_list)
{
	(void)queue;
	app::instance().on_execute(rsb::context_for(cmd_list));
}

// Live DLSS-NR controls. Everything here is safe to change mid-frame: each value is written
// into the NGX parameter block on EVERY evaluate, and the codec's three are push constants on
// every dispatch, so an edit lands on the next frame with no feature recreation. The values are
// NOT written back to ReShade.ini automatically — "Save to ReShade.ini" does that on demand,
// because ReShade rewrites its config on exit and a silent autosave would make an experiment
// permanent without the user deciding it should be.
void draw_nr_controls()
{
	app::NrLive &g_nr_ui = app::instance().nr_live();
	if (!ImGui::CollapsingHeader("DLSS Neural Rendering (feature 18)"))
		return;

	std::uint64_t applied = 0, refused = 0;
	std::uint32_t reasons[nr::kNrRefusalCount] = {};
	nr::counters(applied, refused, reasons);
	ImGui::Text("applied %llu  refused %llu  validated %s",
		static_cast<unsigned long long>(applied), static_cast<unsigned long long>(refused),
		nr::validated() ? "yes" : "no");
	ImGui::Separator();

	bool changed = false;

	// The master switch only gates whether apply() runs; the runtime stays loaded either way,
	// so this toggles cleanly in both directions without touching the GPU.
	if (ImGui::Checkbox("Enabled", &g_nr_ui.enabled))
	{
		nr::set_enabled(g_nr_ui.enabled);
	}


	ImGui::TextUnformatted("Motion");
	changed |= ImGui::SliderFloat("MVec scale (0=auto)", &g_nr_ui.mvec_scale, 0.0f, 4.0f, "%.2f");
	// NOTE: this also affects DLSS SR, which consumes the same resolve — so a change here is not
	// isolated to NR, and an improvement may show up in both.
	changed |= ImGui::Combo("MV convention", &g_nr_ui.mv_convention, app::kMvConventions,
		static_cast<int>(std::size(app::kMvConventions)));
	// The row_major fix: ON reproduces the OLD transposed camera branch, so the two can be
	// compared live. Static frames look identical either way (ClipToPrevClip is ~identity with
	// a still camera, and a transposed identity is still the identity) — judge this while MOVING.
	changed |= ImGui::Checkbox("Legacy transposed ClipToPrevClip", &g_nr_ui.mv_legacy_clip);
	changed |= ImGui::Checkbox("Invert MV X", &g_nr_ui.mv_invert_x);
	ImGui::SameLine();
	changed |= ImGui::Checkbox("Invert MV Y", &g_nr_ui.mv_invert_y);

	ImGui::Separator();
	ImGui::TextUnformatted("Network");
	// 0-1: values above 1 change nothing, so the extra range was only a way to waste a test.
	changed |= ImGui::SliderFloat("Intensity", &g_nr_ui.intensity, 0.0f, 1.0f, "%.2f");
	// Named LocalToneStrength, but it is the style blend weight rather than a tone control.
	// RenoDX ships NRLocalTone=1.74, so the range has to reach it comfortably. Despite the name
	// this is the STYLE BLEND WEIGHT, not a tone control.
	changed |= ImGui::SliderFloat("Local tone", &g_nr_ui.local_tone, 0.0f, 2.0f, "%.2f");
	// 1.0 is the snippet's own fallback. Same auto-mask gate as skin structure: with UseAutoMask
	// off, the snippet forces this to -1 internally and the slider does nothing at all — which is
	// why the gate is surfaced right below rather than buried in the ini.
	changed |= ImGui::SliderFloat("Local structure", &g_nr_ui.local_structure, 0.0f, 2.0f, "%.2f");
	// -1 is a sentinel meaning "use local structure", so the range deliberately reaches it.
	// SkinStructureStrength has three regimes and a bare slider hides two of them, so surface the
	// sentinel as a checkbox the way RenoDX does:
	//   negative  -> "inherit localStructureStrength" (the reference's DEFAULT)
	//   0.0       -> NOT neutral: it flattens skin structure
	//   > 0       -> independent strength
	// Both gated on UseAutoMask with no ControlMask bound; with the auto mask off the snippet
	// forces BOTH structure strengths to -1 internally and neither does anything.
	bool skin_matches_local = g_nr_ui.skin_structure < 0.0f;
	if (ImGui::Checkbox("Skin structure: match local", &skin_matches_local))
	{
		// Leaving "match" restores a usable independent value rather than dropping to 0.0, which
		// would silently flatten skin structure and read as "the control does nothing".
		g_nr_ui.skin_structure = skin_matches_local ? -1.0f : 1.33f;
		changed = true;
	}
	if (!skin_matches_local)
	{
		changed |= ImGui::SliderFloat("Skin structure (0 = off)", &g_nr_ui.skin_structure, 0.0f,
			2.0f, "%.2f");
	}
	// 310.8 ships one weight set registered as preset 1 and falls back to it for every other
	// value, so this is expected to change nothing — exposed to confirm that rather than assume.
	changed |= ImGui::SliderInt("Preset", &g_nr_ui.preset, 0, 4);
	// DLSSNR.Style — a DIFFERENT axis from Preset above. Preset selects a trained weight set
	// (the runtime logs embedded config codenames against it); Style is a plain 0/1/2 with no
	// weight-set switch behind it, confirmed present in the runtime by exact string search
	// (docs/RESEARCH-DLSSNR-STYLES.md). The names below are NOT NVIDIA's — the binary carries no
	// string for any of them, only the bare parameter name "DLSSNR.Style" — they are the labels
	// the OptiScaler_DLSSNR fork's own menu settled on after community testing, offered here for
	// exactly that reason: unverified beyond "the fork's authors judged it worked out this way."
	// Changing it forces the snippet's own temporal history to reset for one frame: judge the
	// result a second or two after moving this, never on the frame it changes.
	static const char *const kNrStyleNames[] = { "Default (standard)", "Natural", "Cinematic" };
	changed |= ImGui::Combo("Style", &g_nr_ui.style, kNrStyleNames, 3);
	// GATES BOTH STRUCTURE STRENGTHS. Reference: "with it disabled the snippet forces
	// localStructureStrength and skinStructureStrength to -1 and neither does anything."
	changed |= ImGui::Checkbox("Auto mask (gates both structure sliders)", &g_nr_ui.auto_mask);
	if (!g_nr_ui.auto_mask)
		ImGui::TextUnformatted("  ^ off: the structure sliders above do nothing");
	ImGui::SameLine();
	changed |= ImGui::Checkbox("UI correction", &g_nr_ui.ui_correction);

	if (changed)
		app::instance().apply_nr_live();

	ImGui::Separator();
	if (ImGui::Button("Save to ReShade.ini"))
	{
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRIntensity", g_nr_ui.intensity);
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRLocalTone", g_nr_ui.local_tone);
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRLocalStructure",
			g_nr_ui.local_structure);
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRSkinStructure",
			g_nr_ui.skin_structure);
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRPreset", g_nr_ui.preset);
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRStyle", g_nr_ui.style);
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRMVecScale", g_nr_ui.mvec_scale);
		reshade::set_config_value(nullptr, "STRAYDLSS", "MVConvention", g_nr_ui.mv_convention);
		reshade::set_config_value(nullptr, "STRAYDLSS", "MVInvertX", g_nr_ui.mv_invert_x ? 1 : 0);
		reshade::set_config_value(nullptr, "STRAYDLSS", "MVInvertY", g_nr_ui.mv_invert_y ? 1 : 0);
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRAutoMask",
			g_nr_ui.auto_mask ? 1 : 0);
		reshade::set_config_value(nullptr, "STRAYDLSS", "NgxNRUICorrection",
			g_nr_ui.ui_correction ? 1 : 0);
		STRAY_LOG_WARN("NR: live settings saved to ReShade.ini.");
	}
	ImGui::SameLine();
	if (ImGui::Button("Reset to defaults"))
	{
		const bool was_enabled = g_nr_ui.enabled;
		g_nr_ui = app::NrLive{};
		g_nr_ui.enabled = was_enabled;
		app::instance().apply_nr_live();
	}
}

void draw_status(reshade::api::effect_runtime *runtime)
{
	(void)runtime;

	const auto &ngx_status = ngx::status();
	const app::Status st = app::instance().status();

	ImGui::Text("stray-dlss %s", "0.1.0-skeleton");
	ImGui::Separator();
	ImGui::Text("D3D12 device:   %s", st.have_device ? "yes" : "no");
	ImGui::Text("vkd3d-proton:   %s", st.is_vkd3d ? "yes" : "no");
	ImGui::Text("NGX init:       %s", ngx_status.initialised ? "ok" : "FAILED");
	ImGui::Text("DLSS SR:        %s", ngx_status.super_sampling_available ? "available" : "unavailable");
	ImGui::Text("TAA hash table: %zu baked%s", std::size(kKnownTaaHashes),
		st.extra_hashes_loaded < 0 ? " (no override file)" : "");
	if (st.extra_hashes_loaded >= 0)
		ImGui::Text("                +%d from stray-dlss-hashes.txt", st.extra_hashes_loaded);
	if (ImGui::Button("Reload hash file"))
		app::instance().load_hash_override_file();
	if (ngx_status.needs_updated_driver)
		ImGui::Text("Driver:         needs >= %u.%u", ngx_status.min_driver_major, ngx_status.min_driver_minor);
	ImGui::Separator();
	ImGui::Text("Frame:          %llu", static_cast<unsigned long long>(st.frame));
	ImGui::Text("Compute PSOs:   %u", st.compute_pipelines);
	ImGui::Text("TAA matches:    %u", st.taa_pipelines);
	ImGui::Text("Dispatches:     %u", st.dispatches);
	ImGui::Text("Addon events:   bind_pipeline=%s push_descriptors=%s",
		g_saw_bind_pipeline.load() ? "yes" : "no",
		g_saw_push_descriptors.load() ? "yes" : "no");

	ImGui::Separator();
	draw_nr_controls();
}

void draw_osd(reshade::api::effect_runtime *runtime)
{
	(void)runtime;
	const auto &ngx_status = ngx::status();
	const app::Status st = app::instance().status();
	// The site is on the OSD because a screenshot is often the only evidence available, and
	// "which path produced this image" is the first question to ask of one.
	ImGui::Text("stray-dlss: %s | NGX %s | TAA x%u | NR@taa",
		st.is_vkd3d ? "vkd3d" : "d3d12",
		ngx_status.super_sampling_available ? "ok" : "n/a",
		st.taa_pipelines);
}

// ---- finder event handlers (pass finder + G-buffer finder) ----
//
// The render-target and draw events feed BOTH diagnostics and are registered when either
// [STRAYDLSS] PassFinder=1 or [STRAYDLSS] GBufferFinder=1; the copy and execute events are
// pass-finder-only. Conditional registration matters: every extra event ReShade dispatches
// costs a call per operation even when the handler early-outs, and both finders are
// diagnostics. All of the skip-capable ones return false — the finders observe, never
// suppress.

// Registering the pipeline events is not free: ReShade responds by routing every PSO creation
// through ID3D12Device2::CreatePipelineState and dropping the cached-PSO blob. Under
// vkd3d-proton that means every shader recompiles on each launch, which for a UE4 title is a
// very long first load. Worth it while we need bytecode; wasteful otherwise, so it is a
// switch — decided by app::DlssApp::configure_events() from the ini. (docs/RESEARCH.md §2.5)

void register_events(const app::EventNeeds &needs)
{
	reshade::register_event<reshade::addon_event::init_device>(on_init_device);
	reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
	// Resource lifetime. ReShade never calls destroy_resource_view on D3D12, so its
	// view->resource map hands back destroyed resources for recycled descriptor slots; viewing
	// one faults inside vkd3d. Track liveness ourselves. (reshade_bindings.hpp)
	reshade::register_event<reshade::addon_event::init_resource>(
		[](reshade::api::device *, const reshade::api::resource_desc &,
		   const reshade::api::subresource_data *, reshade::api::resource_usage,
		   reshade::api::resource res) { rsb::note_resource_created(res); });
	reshade::register_event<reshade::addon_event::destroy_resource>(
		[](reshade::api::device *, reshade::api::resource res) { rsb::note_resource_destroyed(res); });

	reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
	reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
	reshade::register_event<reshade::addon_event::init_pipeline_layout>(on_init_pipeline_layout);
	reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(on_destroy_pipeline_layout);

	if (needs.pipeline_events)
	{
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
	}
	reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
	reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	// ReShade's own state_tracking does not register this, which is why its state_block can
	// never replay root constants.
	reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
	reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
	reshade::register_event<reshade::addon_event::present>(on_present);

	if (needs.finder_rt_events)
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
			on_bind_render_targets);
	if (needs.finder_rt_events)
	{
		reshade::register_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
		reshade::register_event<reshade::addon_event::draw>(on_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
	}
	if (needs.pass_finder_events)
	{
		reshade::register_event<reshade::addon_event::copy_resource>(on_copy_resource);
		reshade::register_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
		reshade::register_event<reshade::addon_event::resolve_texture_region>(on_resolve_texture_region);
		reshade::register_event<reshade::addon_event::execute_command_list>(on_execute_command_list);
	}

	reshade::register_overlay(nullptr, draw_status); // settings page under our add-on entry
	reshade::register_overlay("OSD", draw_osd);      // always-visible one-liner
}

void unregister_events(const app::EventNeeds &needs)
{
	reshade::unregister_overlay("OSD", draw_osd);
	reshade::unregister_overlay(nullptr, draw_status);

	if (needs.pass_finder_events)
	{
		reshade::unregister_event<reshade::addon_event::execute_command_list>(on_execute_command_list);
		reshade::unregister_event<reshade::addon_event::resolve_texture_region>(on_resolve_texture_region);
		reshade::unregister_event<reshade::addon_event::copy_texture_region>(on_copy_texture_region);
		reshade::unregister_event<reshade::addon_event::copy_resource>(on_copy_resource);
	}
	if (needs.finder_rt_events)
	{
		reshade::unregister_event<reshade::addon_event::draw_indexed>(on_draw_indexed);
		reshade::unregister_event<reshade::addon_event::draw>(on_draw);
		reshade::unregister_event<reshade::addon_event::begin_render_pass>(on_begin_render_pass);
	}
	if (needs.finder_rt_events)
		reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
			on_bind_render_targets);

	reshade::unregister_event<reshade::addon_event::present>(on_present);
	reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
	reshade::unregister_event<reshade::addon_event::push_constants>(on_push_constants);
	reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
	if (needs.pipeline_events)
	{
		reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
		reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
	}
	reshade::unregister_event<reshade::addon_event::destroy_pipeline_layout>(on_destroy_pipeline_layout);
	reshade::unregister_event<reshade::addon_event::init_pipeline_layout>(on_init_pipeline_layout);
	reshade::unregister_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
	reshade::unregister_event<reshade::addon_event::init_command_list>(on_init_command_list);
	reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		// The file sink comes up FIRST, before register_addon, so that a failed registration
		// (an imgui version mismatch is the classic silent one) is distinguishable from the
		// add-on never being loaded at all.
		log::init_file_sink();
		// Config comes from ReShade's ini for as long as ReShade is the host; every read in
		// this add-on goes through host::cfg, so the source must be in place before any.
		host::cfg::set_source(rsb::reshade_config_source());
		log::write(log::Level::info, "stray-dlss 0.1.0-skeleton attaching");
		log::writef(log::Level::info, "  built against ReShade API version %u, imgui %d",
			RESHADE_API_VERSION, IMGUI_VERSION_NUM);

		if (!reshade::register_addon(module))
		{
			log::write(log::Level::error,
				"reshade::register_addon FAILED. Either ReShade is older than the API version "
				"above, or the imgui version handshake did not match.");
			return FALSE;
		}

		log::set_external_sink(rsb::reshade_log_sink);
		log::write(log::Level::info, "reshade::register_addon succeeded");
		// The seam: ReShade's trackers answer every binding question for the application.
		icept::set_backend(&rsb::backend());
		shader_dump::initialise();
		// These must come first: they populate the descriptor and command-list state our own
		// callbacks read, and there is no retroactive recovery of a heap we attached late to.
		// (docs/RESEARCH.md §2.6)
		descriptor_tracking::register_events();
		state_tracking::register_events();
		g_needs = app::instance().configure_events();
		register_events(g_needs);
		break;

	case DLL_PROCESS_DETACH:
		shader_dump::finish();
		app::instance().log_final_census(
			g_saw_bind_pipeline.load(std::memory_order_relaxed),
			g_saw_push_descriptors.load(std::memory_order_relaxed));
		// The native hooks must not outlive this DLL: ReShade unloads and reloads add-ons across
		// the game's device recreate (measured), and a patched slot pointing into an unmapped
		// image is the address-0 startup crash.
		app::instance().shutdown();
		native::set_sink(nullptr);
		unregister_events(g_needs);
		state_tracking::unregister_events();
		descriptor_tracking::unregister_events();
		rsb::forget_all_command_lists();
		rsb::forget_all_resources();
		icept::set_backend(nullptr);
		reshade::unregister_addon(module);
		log::write(log::Level::info, "stray-dlss detaching");
		log::shutdown_file_sink();
		break;

	default:
		break;
	}

	return TRUE;
}
