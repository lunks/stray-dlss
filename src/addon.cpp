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
#include "frame_state.hpp"
#include "shader_dump.hpp"
#include "taa_hook.hpp"

#include "reshade_all.hpp"

#include <descriptor_tracking.hpp>
#include <state_tracking.hpp>

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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
// Identities live in core/taa_signature.hpp, corrected against the live game. See CLAUDE.md
// §2.3: the original measurement had them backwards.

// Far enough into the frame loop that device and swapchain creation have fully settled.
constexpr int kNgxInitFrame = 120;
// If ReShade's vkd3d ext hook has still not appeared by this frame, stop waiting and bring NGX
// up on the native device. Measured in Stray the hook is installed between device creation and
// frame 300, so this only fires on a title (or a ReShade build) that never installs it.
constexpr int kNgxDecisionDeadline = 900;

struct State
{
	std::mutex mutex;

	ID3D12Device *native_device = nullptr;
	bool is_vkd3d = false;

	// pipeline handle -> DXBC hash, for compute pipelines only.
	std::unordered_map<uint64_t, std::uint64_t> compute_pipeline_hashes;
	// Distinct PS+CS shader hashes, which is the census the measured facts are stated in:
	// ~150 during the main menu, rising to ~728 once gameplay is running. That step is the
	// most reliable "are we actually in game" signal available without a GPU readback.
	// (CLAUDE.md §2.3)
	std::unordered_set<std::uint64_t> distinct_shader_hashes;
	// command list -> the compute pipeline handle most recently bound on it.
	std::unordered_map<reshade::api::command_list *, uint64_t> bound_compute_pipeline;

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

	// NGX initialisation is deferred and opt-in. Doing it inside init_device deadlocks the
	// game: NGX re-enters DXGI while ReShade and DXVK-NVAPI still hold their device-creation
	// locks, and the process wedges before the first frame. (docs/RESEARCH.md §1.4)
	std::atomic<bool> ngx_enabled{ false };
	std::atomic<bool> ngx_attempted{ false };
	std::atomic<uint32_t> shader_census{ 0 };
};

State g_state;

// Reports whether ReShade's vkd3d extension hook is installed on the ID3D12DeviceExt vtable.
//
// ReShade 6.8.0 patches vtable slots 7/8 of that interface so GetCudaTextureObject /
// GetCudaSurfaceObject -- the entry points nvngx_dlss.dll uses under vkd3d -- run their handles
// through convert_to_original_cpu_descriptor_handle. That assumes ReShade minted the handle; a
// real vkd3d handle gives a garbage heap index and an out-of-bounds read, with no error and a
// wrong texture sampled.
//
// We hand NGX the native device (§1), so our handles are real, which is safe only while the
// patch is absent. vkd3d uses ONE STATIC VTABLE here, so any component querying ReShade's proxy
// installs it for everyone. Measured reachable on the target hardware, 2026-08-31.
//
// `when` names the call site, because the answer can CHANGE over a session: whoever installs the
// patch may do so long after init_device. Checking once at startup would report "safe" and be
// wrong later, so this runs again immediately before NGX is initialised.
//
// Querying the NATIVE device installs nothing: it goes through vkd3d's own QueryInterface,
// never ReShade's proxy.
bool report_vkd3d_ext_hook(ID3D12Device *native, const char *when)
{
	// {11EA7A1A-0F6A-49BF-B612-3E30F8E201DD}
	constexpr GUID kDeviceExt = { 0x11ea7a1a, 0x0f6a, 0x49bf,
		{ 0xb6, 0x12, 0x3e, 0x30, 0xf8, 0xe2, 0x01, 0xdd } };

	IUnknown *ext = nullptr;
	if (native == nullptr ||
		FAILED(native->QueryInterface(kDeviceExt, reinterpret_cast<void **>(&ext))) ||
		ext == nullptr)
	{
		STRAY_LOG_INFO("[%s] No ID3D12DeviceExt on this device (not vkd3d-proton, or too old).",
			when);
		return false;
	}

	void *const slot = (*reinterpret_cast<void ***>(ext))[8]; // GetCudaSurfaceObject
	HMODULE owner = nullptr;
	::GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(slot), &owner);

	// ReShade is whichever module exports the add-on entry points.
	const bool owned_by_reshade = owner != nullptr &&
		::GetProcAddress(owner, "ReShadeRegisterAddon") != nullptr;

	char path[MAX_PATH] = {};
	if (owner != nullptr)
		::GetModuleFileNameA(owner, path, MAX_PATH);

	if (owned_by_reshade)
	{
		STRAY_LOG_ERROR("[%s] vkd3d ID3D12DeviceExt slot 8 is HOOKED BY RESHADE (%s).",
			when, path[0] ? path : "<unknown>");
		STRAY_LOG_ERROR("  NGX descriptors minted on the native device will be run through "
			"convert_to_original_cpu_descriptor_handle and CORRUPTED.");
		STRAY_LOG_ERROR("  Expect a wrong-looking image rather than an error. See CLAUDE.md "
			"\"The native-device rule has a trap\".");
	}
	else
	{
		STRAY_LOG_INFO("[%s] vkd3d ID3D12DeviceExt present, slot 8 unhooked (owner=%s) - the "
			"native-device NGX path is safe.", when, path[0] ? path : "<unknown>");
	}
	ext->Release();
	return owned_by_reshade;
}

// Recovers ReShade's proxy ID3D12Device from the original one.
//
// ReShade stores the proxy in the original device's private data under __uuidof(D3D12Device)
// (v6.8.0 d3d12_device.cpp:34), which is how ReShade's own extension hook gets back to it. The
// GUID is stable across the 6.x line; if it ever changes, this returns null and we fall back to
// the native device with a warning rather than guessing.
ID3D12Device *reshade_proxy_device(ID3D12Device *native)
{
	if (native == nullptr)
		return nullptr;

	// {2523AFF4-978B-4939-BA16-8EE876A4CB2A} - ReShade's D3D12Device proxy class.
	constexpr GUID kReShadeProxy = { 0x2523aff4, 0x978b, 0x4939,
		{ 0xba, 0x16, 0x8e, 0xe8, 0x76, 0xa4, 0xcb, 0x2a } };

	ID3D12Device *proxy = nullptr;
	UINT size = sizeof(proxy);
	if (FAILED(native->GetPrivateData(kReShadeProxy, &size, &proxy)) || size != sizeof(proxy))
		return nullptr;
	return proxy;
}

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

	report_vkd3d_ext_hook(native, "init_device");

	if (native != nullptr)
	{
		LUID luid = native->GetAdapterLuid();
		STRAY_LOG_INFO("  adapter LUID = %08x:%08x", luid.HighPart, luid.LowPart);
	}

	// Deliberately NOT initialising NGX here. See State::ngx_enabled.
	bool enable_ngx = false;
	reshade::get_config_value(nullptr, "STRAYDLSS", "EnableNGX", enable_ngx);
	g_state.ngx_enabled.store(enable_ngx, std::memory_order_relaxed);
	STRAY_LOG_INFO("NGX is %s ([STRAYDLSS] EnableNGX). It initialises lazily on frame %d, "
		"never during device init.",
		enable_ngx ? "ENABLED" : "disabled", kNgxInitFrame);

	bool mv_resolve = true;
	reshade::get_config_value(nullptr, "STRAYDLSS", "MvResolve", mv_resolve);
	bool restore_heaps = true;
	reshade::get_config_value(nullptr, "STRAYDLSS", "MvRestoreHeaps", restore_heaps);
	bool restore_state = true;
	reshade::get_config_value(nullptr, "STRAYDLSS", "MvRestoreState", restore_state);
	int mv_dispatch_mode = 2;
	reshade::get_config_value(nullptr, "STRAYDLSS", "MvDispatch", mv_dispatch_mode);
	taa_hook::configure(mv_resolve, restore_heaps, restore_state, mv_dispatch_mode);
}

void on_destroy_device(reshade::api::device *device)
{
	if (device->get_api() != reshade::api::device_api::d3d12)
		return;

	std::lock_guard<std::mutex> lock(g_state.mutex);
	if (g_state.ngx_attempted.load(std::memory_order_relaxed))
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

void on_reset_command_list(reshade::api::command_list *cmd_list)
{
	reset_command_list_state(cmd_list);
	taa_hook::forget_command_list(cmd_list);
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
	note_push_constants(cmd_list, stages, layout, layout_param, first, count, values);
}

void on_init_pipeline_layout(
	reshade::api::device *device,
	uint32_t count,
	const reshade::api::pipeline_layout_param *params,
	reshade::api::pipeline_layout layout)
{
	note_pipeline_layout(device, count, params, layout);
}

void on_destroy_pipeline_layout(reshade::api::device *device, reshade::api::pipeline_layout layout)
{
	(void)device;
	forget_pipeline_layout(layout);
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

		const std::uint64_t hash = fnv1a64(shader->code, shader->code_size);

		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			g_state.distinct_shader_hashes.insert(hash);
		}

		if (!is_compute)
			continue; // the census counts PS too, but only CS is hashed for identification

		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			g_state.compute_pipeline_hashes[pipeline.handle] = hash;
		}
		g_state.compute_pipelines_seen.fetch_add(1, std::memory_order_relaxed);

		// Getting the real bytecode off the user's machine settles the binding layout
		// offline, which is worth far more than any inference we could make here.
		shader_dump::dump_compute_shader(hash, shader->code, shader->code_size);
		taa_hook::set_pipeline_hash(pipeline.handle, hash);

		if (hash == kTaaMainHash)
		{
			g_state.taa_pipelines_seen.fetch_add(1, std::memory_order_relaxed);
			STRAY_LOG_INFO("FOUND FTAAStandaloneCS (MainUpsampling): hash=0x%016llx "
				"pipeline=0x%016llx bytes=%zu",
				static_cast<unsigned long long>(hash),
				static_cast<unsigned long long>(pipeline.handle),
				shader->code_size);
		}
		else if (hash == kDenoiserLookalikeHash)
		{
			STRAY_LOG_INFO("Saw the denoiser look-alike 0x%016llx (cb1[126], not TAA) - not hooking",
				static_cast<unsigned long long>(hash));
		}
		else if (hash == kSecondCandidateHash)
		{
			STRAY_LOG_INFO("Saw 0x%016llx (11 SRVs, cb1[131], not TAA) - not hooking",
				static_cast<unsigned long long>(hash));
		}
	}
}

void on_destroy_pipeline(reshade::api::device *device, reshade::api::pipeline pipeline)
{
	(void)device;
	// ID3D12PipelineState pointers get recycled, so eviction is not optional.
	taa_hook::forget_pipeline(pipeline.handle);
	std::lock_guard<std::mutex> lock(g_state.mutex);
	g_state.compute_pipeline_hashes.erase(pipeline.handle);
}

void on_bind_pipeline(
	reshade::api::command_list *cmd_list,
	reshade::api::pipeline_stage stages,
	reshade::api::pipeline pipeline)
{
	// NOTE: in D3D12 this fires with pipeline_stage::all, NOT compute_shader. Filtering on
	// the compute stage here would silently miss every event. (docs/RESEARCH.md §2.3)
	(void)stages;
	g_state.saw_bind_pipeline.store(true, std::memory_order_relaxed);

	// Remember the pipeline per command list so a dispatch can be attributed to a hash.
	taa_hook::set_bound_pipeline(cmd_list, pipeline.handle);
	std::lock_guard<std::mutex> lock(g_state.mutex);
	if (g_state.compute_pipeline_hashes.count(pipeline.handle) != 0)
		g_state.bound_compute_pipeline[cmd_list] = pipeline.handle;
}

void on_push_descriptors(
	reshade::api::command_list *cmd_list,
	reshade::api::shader_stage stages,
	reshade::api::pipeline_layout layout,
	uint32_t layout_param,
	const reshade::api::descriptor_table_update &update)
{
	g_state.saw_push_descriptors.store(true, std::memory_order_relaxed);
	note_push_descriptors(cmd_list, stages, layout, layout_param, update);
}

bool on_dispatch(reshade::api::command_list *cmd_list, uint32_t x, uint32_t y, uint32_t z)
{
	g_state.dispatches_seen.fetch_add(1, std::memory_order_relaxed);

	if (taa_hook::intercept_dispatch(cmd_list, x, y, z))
		return true;

	if (shader_dump::enabled())
	{
		std::uint64_t hash = 0;
		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			const auto bound = g_state.bound_compute_pipeline.find(cmd_list);
			if (bound != g_state.bound_compute_pipeline.end())
			{
				const auto it = g_state.compute_pipeline_hashes.find(bound->second);
				if (it != g_state.compute_pipeline_hashes.end())
					hash = it->second;
			}
		}
		if (hash != 0)
			shader_dump::note_dispatch(hash, x, y, z);
	}
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

	// A machine-readable heartbeat, so automation can tell menu from gameplay without a human
	// looking at the screen. Rewritten in place every 30 frames; cheap and always current.
	if ((frame % 30) == 0)
	{
		uint32_t census = 0;
		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			census = static_cast<uint32_t>(g_state.distinct_shader_hashes.size());
		}
		g_state.shader_census.store(census, std::memory_order_relaxed);

		std::FILE *f = nullptr;
		if (fopen_s(&f, "stray-dlss-status.txt", "w") == 0 && f != nullptr)
		{
			std::fprintf(f, "frame=%llu\n", static_cast<unsigned long long>(frame));
			std::fprintf(f, "shader_census=%u\n", census);
			std::fprintf(f, "compute_pipelines=%u\n", g_state.compute_pipelines_seen.load());
			std::fprintf(f, "taa_pipelines=%u\n", g_state.taa_pipelines_seen.load());
			std::fprintf(f, "dispatches=%u\n", g_state.dispatches_seen.load());
			std::fprintf(f, "vkd3d=%d\n", g_state.is_vkd3d ? 1 : 0);
			std::fprintf(f, "ngx_attempted=%d\n",
				g_state.ngx_attempted.load(std::memory_order_relaxed) ? 1 : 0);
			// The measured census is ~150 in the main menu and ~728 in gameplay, so a
			// threshold in between separates them with wide margin either side.
			std::fprintf(f, "in_game=%d\n", census >= 400 ? 1 : 0);
			const auto &d = taa_hook::diagnostics();
			std::fprintf(f, "large_dispatches=%llu\n", (unsigned long long)d.large_dispatches);
			std::fprintf(f, "no_bound_pipeline=%llu\n", (unsigned long long)d.no_bound_pipeline);
			std::fprintf(f, "no_hash=%llu\n", (unsigned long long)d.no_hash);
			std::fprintf(f, "resolve_failed=%llu\n", (unsigned long long)d.resolve_failed);
			std::fprintf(f, "reports=%llu\n", (unsigned long long)d.candidates_reported);
			std::fclose(f);
		}
	}

	// Lazy NGX bring-up, well clear of device creation and on a frame boundary.
	//
	// WHICH DEVICE NGX GETS IS NOT A FIXED CHOICE. Two configurations are self-consistent, and
	// each is broken under the other's condition (CLAUDE.md, "The native-device rule has a trap"):
	//
	//   * hook ABSENT  -> NGX must use the NATIVE device. Its descriptors are then real vkd3d
	//                     handles and nothing converts them.
	//   * hook PRESENT -> NGX must use ReShade's PROXY device, so its descriptors are
	//                     ReShade-minted and the hook's conversion is correct.
	//
	// Picking either one unconditionally is wrong half the time, and the failure is silent: a
	// wrong texture sampled, no error. Measured in Stray the hook is absent at device creation
	// and present by frame 300 — the game installs it itself — so the answer is not knowable at
	// startup. Wait for it, and only give up at kNgxDecisionDeadline.
	if (frame >= kNgxInitFrame &&
		g_state.ngx_enabled.load(std::memory_order_relaxed) &&
		!g_state.ngx_attempted.load(std::memory_order_relaxed))
	{
		ID3D12Device *native = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			native = g_state.native_device;
		}

		const bool hooked = report_vkd3d_ext_hook(native, "pre-NGX");
		if (hooked || frame >= kNgxDecisionDeadline)
		{
			g_state.ngx_attempted.store(true, std::memory_order_relaxed);

			ID3D12Device *device = native;
			if (hooked)
			{
				if (ID3D12Device *proxy = reshade_proxy_device(native))
				{
					device = proxy;
					STRAY_LOG_INFO("NGX will use ReShade's PROXY device (%p): the ext hook is "
						"installed, so descriptors must be ReShade-minted for its conversion "
						"to be correct.", static_cast<void *>(proxy));
				}
				else
				{
					STRAY_LOG_ERROR("The ext hook is installed but ReShade's proxy device could "
						"not be recovered. Falling back to the native device — this is the "
						"BROKEN combination and the image is expected to be wrong.");
				}
			}
			else
			{
				STRAY_LOG_INFO("NGX will use the NATIVE device (%p): no ext hook after %d "
					"frames, so nothing will convert our handles.",
					static_cast<void *>(native), kNgxDecisionDeadline);
			}

			STRAY_LOG_INFO("Initialising NGX (frame %llu)...",
				static_cast<unsigned long long>(frame));
			ngx::initialise(device);
		}
		else if (frame == kNgxInitFrame)
		{
			STRAY_LOG_INFO("Deferring NGX: waiting to see whether ReShade's vkd3d ext hook gets "
				"installed, which decides which device NGX must use (deadline frame %d).",
				kNgxDecisionDeadline);
		}
	}

	// Re-check the vkd3d ext hook periodically.
	//
	// The answer can change mid-session: whoever installs the patch may do so at any point, and
	// the most likely trigger is the game's own first NvAPI call, which can happen long after
	// device creation. A startup-only check would report "safe" and be wrong later. These are
	// passive reads of a vtable slot — no interception, no NGX, nothing that can affect the
	// image — so they are safe to leave on.
	if (frame == 300 || frame == 1200 || frame == 3600)
	{
		ID3D12Device *native = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			native = g_state.native_device;
		}
		char when[32];
		std::snprintf(when, sizeof(when), "frame %llu", static_cast<unsigned long long>(frame));
		report_vkd3d_ext_hook(native, when);

		// Report the resolve counters here too. The detach census is unreliable in practice:
		// killing the game — which is how every unattended run ends — gives Wine no chance to
		// run DllMain(PROCESS_DETACH), so the only census that survives is the launcher's, with
		// everything at zero.
		std::uint32_t attempts = 0, skipped = 0;
		taa_hook::resolve_counters(attempts, skipped);
		STRAY_LOG_INFO("[%s] resolve attempts=%u skipped_stale=%u (%.1f%%)", when, attempts,
			skipped, attempts ? (100.0 * skipped / attempts) : 0.0);
	}

	// Report the add-on-level capability verdict once, after enough frames that the game has
	// certainly bound something.
	// Well after gameplay has started, so every steady-state pass has had a chance to run.
	if (frame == 2400)
		taa_hook::dump_summary();

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

// Registering the pipeline events is not free: ReShade responds by routing every PSO creation
// through ID3D12Device2::CreatePipelineState and dropping the cached-PSO blob. Under
// vkd3d-proton that means every shader recompiles on each launch, which for a UE4 title is a
// very long first load. Worth it while we need bytecode; wasteful otherwise, so it is a
// switch. (docs/RESEARCH.md §2.5)
bool g_pipeline_events_registered = false;

void register_events()
{
	reshade::register_event<reshade::addon_event::init_device>(on_init_device);
	reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
	// Resource lifetime. ReShade never calls destroy_resource_view on D3D12, so its
	// view->resource map hands back destroyed resources for recycled descriptor slots; viewing
	// one faults inside vkd3d. Track liveness ourselves. (frame_state.hpp)
	reshade::register_event<reshade::addon_event::init_resource>(
		[](reshade::api::device *, const reshade::api::resource_desc &,
		   const reshade::api::subresource_data *, reshade::api::resource_usage,
		   reshade::api::resource res) { note_resource_created(res); });
	reshade::register_event<reshade::addon_event::destroy_resource>(
		[](reshade::api::device *, reshade::api::resource res) { note_resource_destroyed(res); });

	reshade::register_event<reshade::addon_event::init_command_list>(on_init_command_list);
	reshade::register_event<reshade::addon_event::reset_command_list>(on_reset_command_list);
	reshade::register_event<reshade::addon_event::init_pipeline_layout>(on_init_pipeline_layout);
	reshade::register_event<reshade::addon_event::destroy_pipeline_layout>(on_destroy_pipeline_layout);

	bool hash_shaders = true;
	reshade::get_config_value(nullptr, "STRAYDLSS", "HashShaders", hash_shaders);
	g_pipeline_events_registered = hash_shaders || shader_dump::enabled();

	if (g_pipeline_events_registered)
	{
		STRAY_LOG_WARN("Pipeline events registered: ReShade will drop the D3D12 PSO cache, so "
			"EXPECT A VERY SLOW FIRST LOAD while every shader recompiles.");
		STRAY_LOG_WARN("  Set [STRAYDLSS] HashShaders=0 (and DumpShaders=0) to get normal load "
			"times once the bytecode has been captured.");
		reshade::register_event<reshade::addon_event::init_pipeline>(on_init_pipeline);
		reshade::register_event<reshade::addon_event::destroy_pipeline>(on_destroy_pipeline);
	}
	else
	{
		STRAY_LOG_INFO("Pipeline hashing disabled; the TAA pass will be identified structurally.");
	}
	reshade::register_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
	reshade::register_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	// ReShade's own state_tracking does not register this, which is why its state_block can
	// never replay root constants.
	reshade::register_event<reshade::addon_event::push_constants>(on_push_constants);
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
	reshade::unregister_event<reshade::addon_event::push_constants>(on_push_constants);
	reshade::unregister_event<reshade::addon_event::push_descriptors>(on_push_descriptors);
	reshade::unregister_event<reshade::addon_event::bind_pipeline>(on_bind_pipeline);
	if (g_pipeline_events_registered)
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
		shader_dump::initialise();
		// These must come first: they populate the descriptor and command-list state our own
		// callbacks read, and there is no retroactive recovery of a heap we attached late to.
		// (docs/RESEARCH.md §2.6)
		descriptor_tracking::register_events();
		state_tracking::register_events();
		register_events();
		break;

	case DLL_PROCESS_DETACH:
		shader_dump::finish();
		// A final census, unconditionally. The frame-300 census is driven by present events,
		// so it never runs in a headless host — and a headless host is exactly how CI loads
		// this add-on into the real ReShade with no game. Logging it on detach also gives
		// field reports a definitive "what did the add-on actually see" line even when the
		// user closes the game before frame 300.
		{
			std::uint32_t attempts = 0, skipped = 0;
			taa_hook::resolve_counters(attempts, skipped);
			STRAY_LOG_INFO("Resolve attempts=%u skipped_stale=%u (%.1f%%) — a skipped frame "
				"contributes no motion vectors.", attempts, skipped,
				attempts ? (100.0 * skipped / attempts) : 0.0);
		}
		STRAY_LOG_INFO("Final census: compute pipelines=%u, TAA matches=%u, dispatches=%u, "
			"bind_pipeline=%d, push_descriptors=%d",
			g_state.compute_pipelines_seen.load(std::memory_order_relaxed),
			g_state.taa_pipelines_seen.load(std::memory_order_relaxed),
			g_state.dispatches_seen.load(std::memory_order_relaxed),
			g_state.saw_bind_pipeline.load(std::memory_order_relaxed) ? 1 : 0,
			g_state.saw_push_descriptors.load(std::memory_order_relaxed) ? 1 : 0);
		unregister_events();
		state_tracking::unregister_events();
		descriptor_tracking::unregister_events();
		forget_all_command_lists();
		forget_all_resources();
		reshade::unregister_addon(module);
		log::write(log::Level::info, "stray-dlss detaching");
		log::shutdown_file_sink();
		break;

	default:
		break;
	}

	return TRUE;
}
