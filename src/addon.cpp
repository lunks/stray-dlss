// stray-dlss — ReShade D3D12 add-on entry point.
//
// Skeleton stage. This build deliberately does not touch the TAA pass: it proves the whole
// toolchain end to end — the add-on loads under ReShade 6.8, the imgui handshake matches,
// the D3D12 device is reachable, vkd3d-proton is detected, NGX links and initialises, and
// the compute-shader hashing sees Stray's shaders. Everything it learns goes into the log
// and the overlay, because that is the only feedback channel this project has.

#include "core/fnv1a.hpp"
#include "log.hpp"
#include "ngx_backend.hpp"

#include <imgui.h> // MUST precede reshade.hpp — the version handshake is checked there
#include <reshade.hpp>

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>

extern "C" __declspec(dllexport) const char *NAME = "Stray DLSS";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
	"Injects NVIDIA DLSS Super Resolution into Stray (UE 4.27.2, D3D12) by replacing the "
	"engine's FTAAStandaloneCS temporal AA pass.";
extern "C" __declspec(dllexport) const char *WEBSITE = "https://github.com/lunks/stray-dlss";

namespace {

using namespace stray_dlss;

// vkd3d-proton exposes ID3D12GraphicsCommandListExt. ReShade uses exactly this interface as
// its own vkd3d detector, so it costs nothing and is completely reliable.
// (docs/RESEARCH.md §1.4)
// {77a86b09-2bea-4801-b89a-37648e104af1}
constexpr GUID kIID_ID3D12GraphicsCommandListExt = {
	0x77a86b09, 0x2bea, 0x4801, { 0xb8, 0x9a, 0x37, 0x64, 0x8e, 0x10, 0x4a, 0xf1 }
};

// Measured shader identities. CLAUDE.md §2.3.
constexpr std::uint64_t kTaaMainHash = 0x1708ec956099e259ull;   // the pass we will hook
constexpr std::uint64_t kSecondCandidateHash = 0x52101a15e1a0c5ccull; // almost certainly motion blur — do NOT hook
constexpr std::uint64_t kKnownFalsePositiveHash = 0x901e041a7cadc9dbull; // never select this

struct State
{
	std::mutex mutex;

	ID3D12Device *native_device = nullptr;
	bool is_vkd3d = false;

	// pipeline handle -> DXBC hash, for compute pipelines only.
	std::unordered_map<uint64_t, std::uint64_t> compute_pipeline_hashes;

	std::atomic<uint64_t> frame_index{ 0 };
	std::atomic<uint32_t> compute_pipelines_seen{ 0 };
	std::atomic<uint32_t> taa_pipelines_seen{ 0 };
	std::atomic<uint32_t> dispatches_seen{ 0 };

	// The add-on-level events only exist when ReShade was built with RESHADE_ADDON >= 2,
	// i.e. the full add-on build. Without them we can observe nothing useful, and the log
	// must say so plainly rather than leaving "wrong ReShade build" and "UE4 binds
	// differently" indistinguishable. (docs/RESEARCH.md §2.2)
	std::atomic<bool> saw_bind_pipeline{ false };
	std::atomic<bool> saw_push_descriptors{ false };
	std::atomic<bool> reported_capability_verdict{ false };
};

State g_state;

void on_init_device(reshade::api::device *device)
{
	if (device->get_api() != reshade::api::device_api::d3d12)
	{
		STRAY_LOG_ERROR("Device is not D3D12 (api=%d). This add-on does nothing on other APIs.",
			static_cast<int>(device->get_api()));
		return;
	}

	// get_native() returns uint64_t and hands back the ORIGINAL vkd3d device, not a ReShade
	// proxy. That is exactly what NGX must be initialised with. (docs/RESEARCH.md §1.2)
	auto *native = reinterpret_cast<ID3D12Device *>(device->get_native());

	std::lock_guard<std::mutex> lock(g_state.mutex);
	g_state.native_device = native;

	STRAY_LOG_INFO("init_device: ID3D12Device=%p", static_cast<void *>(native));

	if (native != nullptr)
	{
		LUID luid = native->GetAdapterLuid();
		STRAY_LOG_INFO("  adapter LUID = %08x:%08x", luid.HighPart, luid.LowPart);
	}

	ngx::initialise(native);
}

void on_destroy_device(reshade::api::device *device)
{
	if (device->get_api() != reshade::api::device_api::d3d12)
		return;

	std::lock_guard<std::mutex> lock(g_state.mutex);
	ngx::shutdown(g_state.native_device);
	g_state.native_device = nullptr;
	g_state.compute_pipeline_hashes.clear();
}

void on_init_command_list(reshade::api::command_list *cmd_list)
{
	// One-shot vkd3d probe. Doing it here rather than at device init because the detector
	// interface lives on the command list.
	if (g_state.is_vkd3d)
		return;

	auto *native = reinterpret_cast<ID3D12GraphicsCommandList *>(cmd_list->get_native());
	if (native == nullptr)
		return;

	void *ext = nullptr;
	if (SUCCEEDED(native->QueryInterface(kIID_ID3D12GraphicsCommandListExt, &ext)) && ext != nullptr)
	{
		reinterpret_cast<IUnknown *>(ext)->Release();
		g_state.is_vkd3d = true;
		STRAY_LOG_INFO("Running on vkd3d-proton (ID3D12GraphicsCommandListExt present)");
	}
	else
	{
		STRAY_LOG_INFO("Running on native D3D12 (no ID3D12GraphicsCommandListExt)");
	}
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
		if (subobjects[i].type != reshade::api::pipeline_subobject_type::compute_shader)
			continue;

		const auto *shader = static_cast<const reshade::api::shader_desc *>(subobjects[i].data);
		if (shader == nullptr || shader->code == nullptr || shader->code_size == 0)
			continue;

		const std::uint64_t hash = fnv1a64(shader->code, shader->code_size);

		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			g_state.compute_pipeline_hashes[pipeline.handle] = hash;
		}
		g_state.compute_pipelines_seen.fetch_add(1, std::memory_order_relaxed);

		if (hash == kTaaMainHash)
		{
			g_state.taa_pipelines_seen.fetch_add(1, std::memory_order_relaxed);
			STRAY_LOG_INFO("FOUND FTAAStandaloneCS: hash=0x%016llx pipeline=0x%016llx bytes=%zu",
				static_cast<unsigned long long>(hash),
				static_cast<unsigned long long>(pipeline.handle),
				shader->code_size);
		}
		else if (hash == kSecondCandidateHash)
		{
			STRAY_LOG_INFO("Saw second candidate 0x%016llx (probably motion blur) - not hooking",
				static_cast<unsigned long long>(hash));
		}
		else if (hash == kKnownFalsePositiveHash)
		{
			STRAY_LOG_WARN("Saw known false positive 0x%016llx - never select this",
				static_cast<unsigned long long>(hash));
		}
	}
}

void on_destroy_pipeline(reshade::api::device *device, reshade::api::pipeline pipeline)
{
	(void)device;
	// ID3D12PipelineState pointers get recycled, so eviction is not optional.
	std::lock_guard<std::mutex> lock(g_state.mutex);
	g_state.compute_pipeline_hashes.erase(pipeline.handle);
}

void on_bind_pipeline(
	reshade::api::command_list *cmd_list,
	reshade::api::pipeline_stage stages,
	reshade::api::pipeline pipeline)
{
	(void)cmd_list;
	(void)pipeline;
	// NOTE: in D3D12 this fires with pipeline_stage::all, NOT compute_shader. Filtering on
	// the compute stage here would silently miss every event. (docs/RESEARCH.md §2.3)
	(void)stages;
	g_state.saw_bind_pipeline.store(true, std::memory_order_relaxed);
}

void on_push_descriptors(
	reshade::api::command_list *cmd_list,
	reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout,
	uint32_t layout_param,
	const reshade::api::descriptor_table_update &update)
{
	(void)cmd_list;
	(void)stages;
	(void)layout;
	(void)layout_param;
	(void)update;
	g_state.saw_push_descriptors.store(true, std::memory_order_relaxed);
}

bool on_dispatch(reshade::api::command_list *cmd_list, uint32_t x, uint32_t y, uint32_t z)
{
	(void)cmd_list;
	(void)x;
	(void)y;
	(void)z;
	g_state.dispatches_seen.fetch_add(1, std::memory_order_relaxed);
	// Skeleton stage: observe only. Returning true here is what will eventually suppress the
	// engine's TAA dispatch — it is the only skip-capable event on our path.
	return false;
}

void on_present(
	reshade::api::command_queue *queue,
	reshade::api::swapchain *swapchain,
	const reshade::api::rect *source_rect,
	const reshade::api::rect *dest_rect,
	uint32_t dirty_rect_count,
	const reshade::api::rect *dirty_rects)
{
	(void)queue;
	(void)swapchain;
	(void)source_rect;
	(void)dest_rect;
	(void)dirty_rect_count;
	(void)dirty_rects;

	const uint64_t frame = g_state.frame_index.fetch_add(1, std::memory_order_relaxed);

	// Report the add-on-level capability verdict once, after enough frames that the game has
	// certainly bound something.
	if (frame == 300 && !g_state.reported_capability_verdict.exchange(true))
	{
		const bool full_addon_build =
			g_state.saw_bind_pipeline.load(std::memory_order_relaxed) &&
			g_state.saw_push_descriptors.load(std::memory_order_relaxed);

		if (full_addon_build)
		{
			STRAY_LOG_INFO("Add-on event check OK: bind_pipeline and push_descriptors both observed");
		}
		else
		{
			STRAY_LOG_ERROR("Add-on event check FAILED after 300 frames "
				"(bind_pipeline=%d push_descriptors=%d).",
				g_state.saw_bind_pipeline.load(std::memory_order_relaxed) ? 1 : 0,
				g_state.saw_push_descriptors.load(std::memory_order_relaxed) ? 1 : 0);
			STRAY_LOG_ERROR("  These events require a ReShade build with full add-on support "
				"(RESHADE_ADDON >= 2). Reinstall ReShade 6.8+ with add-on support enabled.");
		}

		STRAY_LOG_INFO("Census at frame 300: compute pipelines=%u, TAA matches=%u, dispatches=%u",
			g_state.compute_pipelines_seen.load(std::memory_order_relaxed),
			g_state.taa_pipelines_seen.load(std::memory_order_relaxed),
			g_state.dispatches_seen.load(std::memory_order_relaxed));
	}
}

void draw_status(reshade::api::effect_runtime *runtime)
{
	(void)runtime;

	const auto &ngx_status = ngx::status();

	ImGui::Text("stray-dlss %s", "0.1.0-skeleton");
	ImGui::Separator();
	ImGui::Text("D3D12 device:   %s", g_state.native_device != nullptr ? "yes" : "no");
	ImGui::Text("vkd3d-proton:   %s", g_state.is_vkd3d ? "yes" : "no");
	ImGui::Text("NGX init:       %s", ngx_status.initialised ? "ok" : "FAILED");
	ImGui::Text("DLSS SR:        %s", ngx_status.super_sampling_available ? "available" : "unavailable");
	if (ngx_status.needs_updated_driver)
		ImGui::Text("Driver:         needs >= %u.%u", ngx_status.min_driver_major, ngx_status.min_driver_minor);
	ImGui::Separator();
	ImGui::Text("Frame:          %llu", static_cast<unsigned long long>(g_state.frame_index.load()));
	ImGui::Text("Compute PSOs:   %u", g_state.compute_pipelines_seen.load());
	ImGui::Text("TAA matches:    %u", g_state.taa_pipelines_seen.load());
	ImGui::Text("Dispatches:     %u", g_state.dispatches_seen.load());
	ImGui::Text("Addon events:   bind_pipeline=%s push_descriptors=%s",
		g_state.saw_bind_pipeline.load() ? "yes" : "no",
		g_state.saw_push_descriptors.load() ? "yes" : "no");
}

void draw_osd(reshade::api::effect_runtime *runtime)
{
	(void)runtime;
	const auto &ngx_status = ngx::status();
	ImGui::Text("stray-dlss: %s | NGX %s | TAA x%u",
		g_state.is_vkd3d ? "vkd3d" : "d3d12",
		ngx_status.super_sampling_available ? "ok" : "n/a",
		g_state.taa_pipelines_seen.load());
}

void register_events()
{
	reshade::register_event<reshade::addon_event::init_device>(on_init_device);
	reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
	reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
	reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
	reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
	reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
	reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::register_event<reshade::addon_event::dispatch>(on_dispatch);
	reshade::register_event<reshade::addon_event::present>(on_present);

	reshade::register_overlay(nullptr, draw_status); // settings page under our add-on entry
	reshade::register_overlay("OSD", draw_osd);      // always-visible one-liner
}

void unregister_events()
{
	reshade::unregister_overlay("OSD", draw_osd);
	reshade::unregister_overlay(nullptr, draw_status);

	reshade::unregister_event<reshade::addon_event::present>(on_present);
	reshade::unregister_event<reshade::addon_event::dispatch>(on_dispatch);
	reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
	reshade::unregister_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
	reshade::unregister_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
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

		log::enable_reshade_sink();
		log::write(log::Level::info, "reshade::register_addon succeeded");
		register_events();
		break;

	case DLL_PROCESS_DETACH:
		unregister_events();
		reshade::unregister_addon(module);
		log::write(log::Level::info, "stray-dlss detaching");
		log::shutdown_file_sink();
		break;

	default:
		break;
	}

	return TRUE;
}
