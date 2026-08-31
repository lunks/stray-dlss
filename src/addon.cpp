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
#include "core/taa_hashes.hpp"
#include "ext_unhook.hpp"
#include "frame_state.hpp"
#include "gbuffer_finder.hpp"
#include "gbuffer_resolve.hpp"
#include "input_dump.hpp"
#include "pass_finder.hpp"
#include "shader_dump.hpp"
#include "taa_hook.hpp"

#include "reshade_all.hpp"

#include <descriptor_tracking.hpp>
#include <state_tracking.hpp>

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>
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

void load_hash_override_file(); // defined beside the overlay below

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
		STRAY_LOG_ERROR("  ext_unhook restores the pristine pointers before every NGX call "
			"(disable with ExtUnhook=0). See CLAUDE.md \"The native-device rule has a trap\".");
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
	// Save the pristine extension vtable NOW — the game's stack installs ReShade's patch by
	// frame ~120, and once installed the originals are unrecoverable from inside the process.
	ext_unhook::capture(native);

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

	bool ngx_evaluate = false;
	reshade::get_config_value(nullptr, "STRAYDLSS", "NgxEvaluate", ngx_evaluate);
	taa_hook::set_ngx_evaluate(ngx_evaluate);

	// [STRAYDLSS] NgxRR: 0 off (default, SR unchanged), 1 = probe DLSSD existence on this
	// stack (one CreateFeature attempt, released; SR keeps running), 2 = full RR-first
	// evaluate with per-frame SR fallback. Both non-zero modes still require EnableNGX=1
	// and NgxEvaluate=1 — the probe rides the SR feature-creation path and mode 2 rides
	// the SR evaluate site.
	int ngx_rr = 0;
	reshade::get_config_value(nullptr, "STRAYDLSS", "NgxRR", ngx_rr);
	ngx::set_rr_mode(ngx_rr);
	taa_hook::set_ngx_rr(ngx_rr);
	if (ngx_rr == 1)
		STRAY_LOG_WARN("NgxRR=1 (PROBE): one DLSSD create attempt will run at the first SR "
			"feature creation and log every result code by name. Needs EnableNGX=1 and "
			"NgxEvaluate=1; nvngx_dlssd.dll must be staged next to the game executable.");
	else if (ngx_rr == 2)
		STRAY_LOG_WARN("NgxRR=2 (FULL): Ray Reconstruction replaces the SR evaluate when "
			"the G-buffer identification is stable; SR is the per-frame fallback. Grep for "
			"'DLSS RR' and 'RR:' lines.");
	else if (ngx_rr == 3)
		STRAY_LOG_WARN("NgxRR=3 (RR-1): Ray Reconstruction PLUS suppression of the SSD "
			"temporal-accumulation family, so RR denoises the raw screen-space signal. "
			"Suppression ARMS only after RR runs reliably (~30 frames) and disarms on any SR "
			"fallback; warm-up runs as RR-0. Grep for 'RR-1' lines. EXPERIMENTAL.");

	// [STRAYDLSS] GBufferResolveAt: "ssd" (default) records the guide resolve at the first
	// SSD temporal-accumulation dispatch each frame — the content-alive point (measured:
	// at the TAA hook the G-buffer objects are alive but their CONTENT is recycled);
	// "taa" keeps the old in-hook record for A/B measurement.
	char resolve_at[16] = "ssd";
	size_t resolve_at_size = sizeof(resolve_at);
	reshade::get_config_value(nullptr, "STRAYDLSS", "GBufferResolveAt", resolve_at,
		&resolve_at_size);
	const bool resolve_at_ssd = std::strcmp(resolve_at, "taa") != 0;
	taa_hook::set_gbuffer_resolve_at(resolve_at_ssd);
	if (ngx_rr == 2)
		STRAY_LOG_INFO("RR guide resolve records at the %s trigger ([STRAYDLSS] "
			"GBufferResolveAt=%s).",
			resolve_at_ssd ? "SSD-dispatch (content-alive)" : "TAA-hook (A/B mode)",
			resolve_at_ssd ? "ssd" : "taa");
	else if (ngx_rr == 3)
		STRAY_LOG_INFO("RR-1 guide resolve records at the SSD-dispatch trigger, hoisted "
			"ABOVE suppression (content alive; GBufferResolveAt is forced to ssd for RR-1).");

	// [STRAYDLSS] NgxExposure: "auto" (default — today's behaviour, DLSS estimates
	// exposure itself via the AutoExposure create flag) | "texture" (the flag is dropped
	// and the engine's eye-adaptation texture rides every SR evaluate as
	// pInExposureTexture). Creation-time property; A/B across launches. The candidate fix
	// for the red/cyan single-pixel pops: bright neon highlights mis-weighted by DLSS's
	// own exposure estimate in a dark scene.
	char exposure_mode[16] = "auto";
	size_t exposure_mode_size = sizeof(exposure_mode);
	reshade::get_config_value(nullptr, "STRAYDLSS", "NgxExposure", exposure_mode,
		&exposure_mode_size);
	const bool exposure_texture = std::strcmp(exposure_mode, "texture") == 0;
	ngx::set_exposure_from_texture(exposure_texture);
	STRAY_LOG_INFO("DLSS exposure source: %s ([STRAYDLSS] NgxExposure=auto|texture).",
		exposure_texture
			? "engine eye-adaptation TEXTURE (AutoExposure flag dropped at create)"
			: "DLSS AUTO-exposure (the AutoExposure create flag, today's behaviour)");

	// [STRAYDLSS] NgxExposureScale (default 1.0): the InExposureScale passed to the SR
	// evaluate under NgxExposure=texture. The DEFINITIVE consume test — the DLSS on-screen
	// indicator's "Exposure level" field echoes this exact value, and a wrong scale
	// (0.25 / 4.0) must move the image if the exposure texture reaches DLSS's math. Only
	// meaningful under NgxExposure=texture; 1.0 is behaviourally identical to before.
	float exposure_scale = 1.0f;
	reshade::get_config_value(nullptr, "STRAYDLSS", "NgxExposureScale", exposure_scale);
	ngx::set_exposure_scale(exposure_scale);
	if (exposure_texture)
		STRAY_LOG_INFO("DLSS InExposureScale = %.4f ([STRAYDLSS] NgxExposureScale). The "
			"indicator's 'Exposure level' should read this; a 0.25/1.0/4.0 sweep that moves "
			"the image proves DLSS consumes our exposure texture.", exposure_scale);

	// [STRAYDLSS] GBufferResolveOnly: record + dump guides at the SSD trigger, but skip
	// the RR evaluate (SR carries frames) — the record-vs-evaluate fault isolator.
	bool resolve_only = false;
	reshade::get_config_value(nullptr, "STRAYDLSS", "GBufferResolveOnly", resolve_only);
	taa_hook::set_gbuffer_resolve_only(resolve_only);
	if (resolve_only && ngx_rr == 2)
		STRAY_LOG_WARN("GBufferResolveOnly=1: RR evaluate disabled; guide records (and "
			"dumps) still run at the SSD trigger. SR carries every frame.");

	// [STRAYDLSS] GBufferSwapBC: the B/C content-check flip (gbuffer_resolve.hpp). The B/C
	// slot order is unverified by content until the guide dump says otherwise; this flips
	// which identified resource feeds which role, no rebuild.
	bool swap_bc = false;
	reshade::get_config_value(nullptr, "STRAYDLSS", "GBufferSwapBC", swap_bc);
	if (swap_bc || ngx_rr == 2)
		gbr::set_bc_swapped(swap_bc); // logs its state; skipped when RR is off and unswapped

	int ngx_dry_run = 0;
	reshade::get_config_value(nullptr, "STRAYDLSS", "NgxDryRun", ngx_dry_run);
	taa_hook::set_ngx_dry_run(ngx_dry_run);

	char dry_hash[384] = "";
	size_t dry_hash_size = sizeof(dry_hash);
	const bool dry_hash_read =
		reshade::get_config_value(nullptr, "STRAYDLSS", "DryRunHash", dry_hash, &dry_hash_size);
	std::uint64_t dry_hashes[16];
	std::size_t dry_hash_count = 0;
	std::uint64_t dry_hash_value = 0; // first entry, for the existing log line
	for (const char *q = dry_hash; dry_hash_count < 16 && *q != 0;)
	{
		char *end = nullptr;
		const std::uint64_t h = std::strtoull(q, &end, 0);
		if (end == q)
			break;
		if (h != 0)
		{
			if (dry_hash_count == 0)
				dry_hash_value = h;
			dry_hashes[dry_hash_count++] = h;
		}
		q = (*end == ',') ? end + 1 : end;
	}
	// Diagnostic for the config mystery measured 2026-08-31: values present in the ini's
	// [STRAYDLSS] section read back empty while sibling keys work. This logs what the API
	// actually returned so one pasted log settles it.
	STRAY_LOG_INFO("config probe: DryRunHash read=%d size=%zu raw='%.48s' parsed=%zu",
		dry_hash_read ? 1 : 0, dry_hash_size, dry_hash, dry_hash_count);

	// stray-dlss-dryrun.txt beside the game overrides BOTH dry-run settings, bypassing
	// ReShade's config entirely: `alternate=<frames>` on one line, one hash per line
	// otherwise, # comments. Written directly by the deploy tooling.
	{
		std::FILE *f = nullptr;
		fopen_s(&f, "stray-dlss-dryrun.txt", "r");
		if (f != nullptr)
		{
			dry_hash_count = 0;
			dry_hash_value = 0;
			int alt_override = -1;
			char line[128];
			while (std::fgets(line, sizeof(line), f) != nullptr)
			{
				const char *q = line;
				while (*q == ' ' || *q == '\t')
					++q;
				if (*q == '#' || *q == '\n' || *q == 0)
					continue;
				if (std::strncmp(q, "alternate=", 10) == 0)
				{
					alt_override = std::atoi(q + 10);
					continue;
				}
				const std::uint64_t h = std::strtoull(q, nullptr, 16);
				if (h != 0 && dry_hash_count < 16)
				{
					if (dry_hash_count == 0)
						dry_hash_value = h;
					dry_hashes[dry_hash_count++] = h;
				}
			}
			std::fclose(f);
			if (alt_override >= 0)
				taa_hook::set_dry_run_alternate(static_cast<std::uint32_t>(alt_override));
			STRAY_LOG_WARN("stray-dlss-dryrun.txt: %zu suppression hashes, alternate=%d "
				"(file overrides the ini).", dry_hash_count, alt_override);
		}
	}
	taa_hook::set_dry_run_hashes(dry_hashes, dry_hash_count);
	if (dry_hash_count > 1)
		STRAY_LOG_WARN("DryRunHash: %zu passes will be suppressed (comma list).",
			dry_hash_count);

	char ngx_pass[32] = "";
	size_t ngx_pass_size = sizeof(ngx_pass);
	reshade::get_config_value(nullptr, "STRAYDLSS", "NgxPassHash", ngx_pass, &ngx_pass_size);
	const std::uint64_t ngx_pass_value = std::strtoull(ngx_pass, nullptr, 0);
	taa_hook::set_ngx_pass_hash(ngx_pass_value);
	if (ngx_pass_value != 0)
		STRAY_LOG_INFO("DLSS will replace pass 0x%016llx ([STRAYDLSS] NgxPassHash).",
			static_cast<unsigned long long>(ngx_pass_value));

	int ngx_preset = 0;
	if (reshade::get_config_value(nullptr, "STRAYDLSS", "NgxPreset", ngx_preset) &&
		ngx_preset != 0)
	{
		ngx::set_preset(ngx_preset);
		STRAY_LOG_INFO("NgxPreset=%d requested ([STRAYDLSS] NgxPreset; 10=J 11=K 12=L 13=M).",
			ngx_preset);
	}

	bool ext_unhook_enabled = true;
	reshade::get_config_value(nullptr, "STRAYDLSS", "ExtUnhook", ext_unhook_enabled);
	ext_unhook::set_enabled(ext_unhook_enabled);
	if (!ext_unhook_enabled)
		STRAY_LOG_WARN("ExtUnhook=0: ReShade's vkd3d ext-vtable patch will NOT be undone before "
			"NGX calls; expect the frozen-output failure while anything queries the proxy.");

	bool dump_inputs = false;
	reshade::get_config_value(nullptr, "STRAYDLSS", "NgxDumpInputs", dump_inputs);
	input_dump::set_enabled(dump_inputs);
	if (dump_inputs)
		STRAY_LOG_WARN("NgxDumpInputs is ON: the DLSS colour/depth inputs and output are dumped "
			"to straydlss_*.bin at evaluates 600 and 900.");

	bool ngx_paint = false;
	reshade::get_config_value(nullptr, "STRAYDLSS", "NgxPaint", ngx_paint);
	taa_hook::set_ngx_paint(ngx_paint);
	if (ngx_paint)
		STRAY_LOG_WARN("NgxPaint is ON: the TAA output is cleared to MAGENTA instead of "
			"evaluating. If the screen is not magenta, the output handle is wrong.");

	int dry_alternate = 0;
	const bool alt_read =
		reshade::get_config_value(nullptr, "STRAYDLSS", "DryRunAlternate", dry_alternate);
	STRAY_LOG_INFO("config probe: DryRunAlternate read=%d value=%d",
		alt_read ? 1 : 0, dry_alternate);
	if (dry_alternate > 0) // never zero a file-based override with an absent ini key
		taa_hook::set_dry_run_alternate(static_cast<std::uint32_t>(dry_alternate));
	if (dry_alternate > 0)
		STRAY_LOG_WARN("DryRunAlternate=%d: the named pass alternates suppressed/normal every "
			"%d frames, so both states appear in one session.", dry_alternate, dry_alternate);
	if (dry_hash_value != 0)
		STRAY_LOG_WARN("DryRunHash=0x%016llx: that pass will be suppressed and nothing written.",
			static_cast<unsigned long long>(dry_hash_value));
	if (ngx_dry_run)
		STRAY_LOG_WARN("NgxDryRun is ON: the pinned pass will be suppressed and NOTHING written "
			"in its place. This is a diagnostic, not a rendering mode.");
	STRAY_LOG_INFO("DLSS evaluation is %s ([STRAYDLSS] NgxEvaluate). This is the switch that "
		"changes the image; EnableNGX alone only brings NGX up.",
		ngx_evaluate ? "ENABLED" : "disabled");

	load_hash_override_file();
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
	pass_finder::forget_command_list(cmd_list);
	gbuffer_finder::forget_command_list(cmd_list);
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
		{
			// The census counts PS too, but only CS is hashed for identification. The pass
			// finder additionally attributes draws by pixel-shader hash.
			pass_finder::note_pipeline(pipeline.handle, hash, /*is_compute=*/false);
			continue;
		}

		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			g_state.compute_pipeline_hashes[pipeline.handle] = hash;
		}
		g_state.compute_pipelines_seen.fetch_add(1, std::memory_order_relaxed);
		pass_finder::note_pipeline(pipeline.handle, hash, /*is_compute=*/true);

		// Getting the real bytecode off the user's machine settles the binding layout
		// offline, which is worth far more than any inference we could make here.
		shader_dump::dump_compute_shader(hash, shader->code, shader->code_size);
		taa_hook::set_pipeline_hash(pipeline.handle, hash);

		if (is_known_taa_hash(hash))
		{
			g_state.taa_pipelines_seen.fetch_add(1, std::memory_order_relaxed);
			STRAY_LOG_INFO("FOUND FTAAStandaloneCS permutation: hash=0x%016llx "
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
	pass_finder::forget_pipeline(pipeline.handle);
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
	pass_finder::note_bind_pipeline(cmd_list, pipeline.handle);
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

	// Strictly AFTER the intercept decision: a suppressed dispatch never executes, and
	// recording one would enter a phantom writer into the pass finder's last-writer table.
	pass_finder::note_dispatch(cmd_list, x, y, z);

	if (shader_dump::enabled() || gbuffer_finder::enabled())
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
		{
			if (shader_dump::enabled())
				shader_dump::note_dispatch(hash, x, y, z);
			// The G-buffer finder's SSR-denoiser cross-check. It resolves bindings only
			// for the one known denoiser hash, so this is a no-op for everything else.
			gbuffer_finder::note_dispatch(cmd_list, hash);
		}
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
	(void)source_rect;
	(void)dest_rect;
	(void)dirty_rect_count;
	(void)dirty_rects;

	const uint64_t frame = g_state.frame_index.fetch_add(1, std::memory_order_relaxed);

	// Drives DryRunAlternate's phase and logs each transition, so a screenshot's timestamp
	// identifies which state produced it.
	taa_hook::note_present(frame);
	input_dump::on_present();

	// The pass finder's frame boundary. The presented back buffer is its fallback anchor
	// when no tonemapper (3D LUT SRV) was seen this frame.
	if (pass_finder::enabled())
		pass_finder::on_present(frame, swapchain != nullptr
			? swapchain->get_current_back_buffer() : reshade::api::resource{ 0 });

	// The G-buffer finder's frame boundary: stability accounting and the (log-only)
	// identification report. (gbuffer_finder.hpp, DLSS-RR phase 1)
	if (gbuffer_finder::enabled())
		gbuffer_finder::on_present(frame);

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

			// [STRAYDLSS] NgxDevice: auto (default), native, or proxy.
			//
			// The automatic choice follows the measured ext-hook state, which is the reasoning
			// in CLAUDE.md §1. But the crash we are chasing faults inside d3d12core.dll —
			// vkd3d-proton itself — with registers holding packed handle-shaped values rather
			// than pointers, which is what an unconverted descriptor handle reaching vkd3d
			// looks like. That makes the device choice worth testing rather than assuming, so
			// it can be forced either way from the ini.
			char device_pref[16] = "auto";
			size_t device_pref_size = sizeof(device_pref);
			reshade::get_config_value(nullptr, "STRAYDLSS", "NgxDevice", device_pref,
				&device_pref_size);
			const bool force_native = std::strcmp(device_pref, "native") == 0;
			const bool force_proxy = std::strcmp(device_pref, "proxy") == 0;

			ID3D12Device *device = native;
			if (force_native)
			{
				STRAY_LOG_WARN("NGX forced onto the NATIVE device by [STRAYDLSS] NgxDevice; the "
					"ext hook is %s, so this is %s.", hooked ? "INSTALLED" : "absent",
					hooked ? "the combination CLAUDE.md calls broken" : "safe");
			}
			else if (hooked || force_proxy)
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
		std::uint32_t named[5] = {};
		taa_hook::named_pass_counters(named);
		STRAY_LOG_INFO("[%s] named pass: seen=%u phaseB=%u live=%u recorded=%u evaluated=%u",
			when, named[0], named[1], named[2], named[3], named[4]);

		std::uint32_t attempts = 0, skipped = 0;
		taa_hook::resolve_counters(attempts, skipped);
		STRAY_LOG_INFO("[%s] resolve attempts=%u skipped_stale=%u (%.1f%%)", when, attempts,
			skipped, attempts ? (100.0 * skipped / attempts) : 0.0);

		std::uint32_t rr_ok = 0, rr_fallback = 0;
		taa_hook::rr_counters(rr_ok, rr_fallback);
		if (rr_ok + rr_fallback > 0)
		{
			STRAY_LOG_INFO("[%s] RR evaluates=%u SR fallbacks=%u (%.1f%% RR)", when, rr_ok,
				rr_fallback, (100.0 * rr_ok) / (rr_ok + rr_fallback));
			// The per-reason breakdown — the starvation run proved totals without reasons
			// cost a whole round-trip. One line, all nine reasons, by name.
			std::uint32_t r[taa_hook::kRrRefusalCount] = {};
			taa_hook::rr_refusal_counters(r);
			char line[256];
			int off = std::snprintf(line, sizeof(line), "[%s] RR refusals:", when);
			for (int i = 0; i < taa_hook::kRrRefusalCount; ++i)
				if (off > 0 && off < static_cast<int>(sizeof(line)))
					off += std::snprintf(line + off, sizeof(line) - off, " %s=%u",
						taa_hook::kRrRefusalNames[i], r[i]);
			STRAY_LOG_INFO("%s", line);

			// RR-1 suppression telemetry: armed state + how much SSD work was skipped.
			bool rr1_armed = false;
			std::uint64_t rr1_total = 0;
			std::uint32_t rr1_last = 0;
			taa_hook::rr1_counters(rr1_armed, rr1_total, rr1_last);
			STRAY_LOG_INFO("[%s] RR-1 SSD suppression: armed=%d last-frame=%u total=%llu",
				when, rr1_armed ? 1 : 0, rr1_last,
				static_cast<unsigned long long>(rr1_total));
		}
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

// [STRAYDLSS] hash override file: one fnv1a64 per line (0x-prefixed or bare hex, # comments),
// generated offline by tools/stray_taa_hashes.py after a game update. Read from the game's
// working directory, the same place ReShade.log lands.
int g_extra_hashes_loaded = -1; // -1: no file

void load_hash_override_file()
{
	std::FILE *f = nullptr;
	fopen_s(&f, "stray-dlss-hashes.txt", "r");
	if (f == nullptr)
	{
		g_extra_hashes_loaded = -1;
		set_extra_taa_hashes(nullptr, 0);
		return;
	}
	std::uint64_t hashes[64];
	std::size_t n = 0;
	char line[128];
	while (n < 64 && std::fgets(line, sizeof(line), f) != nullptr)
	{
		const char *p = line;
		while (*p == ' ' || *p == '\t')
			++p;
		if (*p == '#' || *p == '\n' || *p == 0)
			continue;
		const std::uint64_t h = std::strtoull(p, nullptr, 16);
		if (h != 0)
			hashes[n++] = h;
	}
	std::fclose(f);
	set_extra_taa_hashes(hashes, n);
	g_extra_hashes_loaded = static_cast<int>(n);
	STRAY_LOG_INFO("stray-dlss-hashes.txt: %zu extra TAA permutation hashes loaded.", n);

	// stray-dlss-ssd-hashes.txt: the FSSDTemporalAccumulationCS family beyond the two
	// baked members (core/taa_signature.hpp) — the RR guide-resolve trigger fires on any
	// member. The full nine live in the deploy tooling's list (the same list the 3.4x
	// suppression experiment used); one hex hash per line, # comments.
	std::FILE *sf = nullptr;
	fopen_s(&sf, "stray-dlss-ssd-hashes.txt", "r");
	if (sf != nullptr)
	{
		std::uint64_t ssd_hashes[64];
		std::size_t sn = 0;
		char sline[128];
		while (sn < 64 && std::fgets(sline, sizeof(sline), sf) != nullptr)
		{
			const char *q = sline;
			while (*q == ' ' || *q == '\t')
				++q;
			if (*q == '#' || *q == '\n' || *q == 0)
				continue;
			const std::uint64_t h = std::strtoull(q, nullptr, 16);
			if (h != 0)
				ssd_hashes[sn++] = h;
		}
		std::fclose(sf);
		set_extra_ssd_hashes(ssd_hashes, sn);
		STRAY_LOG_INFO("stray-dlss-ssd-hashes.txt: %zu extra SSD family hashes loaded "
			"(RR guide trigger).", sn);
	}
	else
	{
		set_extra_ssd_hashes(nullptr, 0);
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
	ImGui::Text("TAA hash table: %zu baked%s", std::size(kKnownTaaHashes),
		g_extra_hashes_loaded < 0 ? " (no override file)" : "");
	if (g_extra_hashes_loaded >= 0)
		ImGui::Text("                +%d from stray-dlss-hashes.txt", g_extra_hashes_loaded);
	if (ImGui::Button("Reload hash file"))
		load_hash_override_file();
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

// ---- finder event handlers (pass finder + G-buffer finder) ----
//
// The render-target and draw events feed BOTH diagnostics and are registered when either
// [STRAYDLSS] PassFinder=1 or [STRAYDLSS] GBufferFinder=1; the copy and execute events are
// pass-finder-only. Conditional registration matters: every extra event ReShade dispatches
// costs a call per operation even when the handler early-outs, and both finders are
// diagnostics. All of the skip-capable ones return false — the finders observe, never
// suppress.

void on_pf_bind_render_targets(reshade::api::command_list *cmd_list, uint32_t count,
	const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv)
{
	pass_finder::note_render_targets(cmd_list, count, rtvs, dsv);
	gbuffer_finder::note_render_targets(cmd_list, count, rtvs, dsv);
}

bool on_pf_begin_render_pass(reshade::api::command_list *cmd_list, uint32_t count,
	const reshade::api::render_pass_render_target_desc *rts,
	const reshade::api::render_pass_depth_stencil_desc *ds,
	reshade::api::render_pass_flags flags)
{
	(void)flags;
	// D3D12 allows at most 8 simultaneous render targets, so the fixed array cannot clip.
	reshade::api::resource_view views[8] = {};
	const uint32_t n = count < 8 ? count : 8;
	for (uint32_t i = 0; i < n; ++i)
		views[i] = rts[i].view;
	pass_finder::note_render_targets(cmd_list, n, views,
		ds != nullptr ? ds->view : reshade::api::resource_view{ 0 });
	gbuffer_finder::note_render_targets(cmd_list, n, views,
		ds != nullptr ? ds->view : reshade::api::resource_view{ 0 });
	return false;
}

bool on_pf_draw(reshade::api::command_list *cmd_list, uint32_t vertex_count,
	uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
{
	(void)instance_count;
	(void)first_vertex;
	(void)first_instance;
	pass_finder::note_draw(cmd_list, vertex_count);
	gbuffer_finder::note_draw(cmd_list);
	return false;
}

bool on_pf_draw_indexed(reshade::api::command_list *cmd_list, uint32_t index_count,
	uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
{
	(void)instance_count;
	(void)first_index;
	(void)vertex_offset;
	(void)first_instance;
	pass_finder::note_draw(cmd_list, index_count);
	gbuffer_finder::note_draw(cmd_list);
	return false;
}

bool on_pf_copy_resource(reshade::api::command_list *cmd_list,
	reshade::api::resource source, reshade::api::resource dest)
{
	pass_finder::note_copy(cmd_list, source, dest);
	return false;
}

bool on_pf_copy_texture_region(reshade::api::command_list *cmd_list,
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
	pass_finder::note_copy(cmd_list, source, dest);
	return false;
}

bool on_pf_resolve_texture_region(reshade::api::command_list *cmd_list,
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
	pass_finder::note_copy(cmd_list, source, dest);
	return false;
}

void on_pf_execute_command_list(reshade::api::command_queue *queue,
	reshade::api::command_list *cmd_list)
{
	(void)queue;
	pass_finder::note_execute(cmd_list);
}

// Registering the pipeline events is not free: ReShade responds by routing every PSO creation
// through ID3D12Device2::CreatePipelineState and dropping the cached-PSO blob. Under
// vkd3d-proton that means every shader recompiles on each launch, which for a UE4 title is a
// very long first load. Worth it while we need bytecode; wasteful otherwise, so it is a
// switch. (docs/RESEARCH.md §2.5)
bool g_pipeline_events_registered = false;
// The render-target/draw events feed both finders; the copy/execute events only the pass
// finder. Tracked separately so unregistration mirrors registration exactly.
bool g_finder_rt_events_registered = false;
bool g_pass_finder_events_registered = false;

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

	// [STRAYDLSS] PassFinder, default OFF: dataflow identification of the TAA pass — the
	// bounded walk backwards from the tonemapper (core/pass_walk.hpp), proven in CI and here
	// only LOGGING its verdict against the live game. It attributes events by shader hash,
	// so enabling it forces the pipeline events (and their PSO-cache cost) on.
	bool pass_finder_enabled = false;
	reshade::get_config_value(nullptr, "STRAYDLSS", "PassFinder", pass_finder_enabled);
	pass_finder::set_enabled(pass_finder_enabled);

	// [STRAYDLSS] GBufferFinder, default OFF: log-only identification of the base pass's
	// G-buffer targets for DLSS Ray Reconstruction (gbuffer_finder.hpp). It needs the
	// render-target/draw events below, and the pipeline events for the SSR-denoiser
	// cross-check's shader hash.
	bool gbuffer_finder_enabled = false;
	reshade::get_config_value(nullptr, "STRAYDLSS", "GBufferFinder", gbuffer_finder_enabled);
	{
		// NgxRR=2 consumes the finder's identification (taa_hook::try_evaluate_rr), so the
		// finder must observe even when GBufferFinder was not set explicitly. Read here as
		// well as in on_init_device because event registration happens first.
		int ngx_rr_for_finder = 0;
		reshade::get_config_value(nullptr, "STRAYDLSS", "NgxRR", ngx_rr_for_finder);
		if ((ngx_rr_for_finder == 2 || ngx_rr_for_finder == 3) && !gbuffer_finder_enabled)
		{
			gbuffer_finder_enabled = true;
			STRAY_LOG_WARN("GBufferFinder forced ON: NgxRR=2/3 needs the G-buffer "
				"identification it produces.");
		}
	}
	gbuffer_finder::set_enabled(gbuffer_finder_enabled);

	g_pipeline_events_registered = hash_shaders || shader_dump::enabled() ||
		pass_finder_enabled || gbuffer_finder_enabled;

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

	if (pass_finder_enabled || gbuffer_finder_enabled)
	{
		g_finder_rt_events_registered = true;
		// Logged so a pasted log PROVES the events were registered for this combination of
		// flags — the 2026-08-31 GBufferFinder run could not distinguish "tap registered
		// but never fired" from "tap never registered" after the fact.
		STRAY_LOG_INFO("Finder RT/draw events registered (PassFinder=%d GBufferFinder=%d).",
			pass_finder_enabled ? 1 : 0, gbuffer_finder_enabled ? 1 : 0);
		reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
			on_pf_bind_render_targets);
		reshade::register_event<reshade::addon_event::begin_render_pass>(on_pf_begin_render_pass);
		reshade::register_event<reshade::addon_event::draw>(on_pf_draw);
		reshade::register_event<reshade::addon_event::draw_indexed>(on_pf_draw_indexed);
	}
	if (pass_finder_enabled)
	{
		g_pass_finder_events_registered = true;
		reshade::register_event<reshade::addon_event::copy_resource>(on_pf_copy_resource);
		reshade::register_event<reshade::addon_event::copy_texture_region>(on_pf_copy_texture_region);
		reshade::register_event<reshade::addon_event::resolve_texture_region>(
			on_pf_resolve_texture_region);
		reshade::register_event<reshade::addon_event::execute_command_list>(
			on_pf_execute_command_list);
	}

	reshade::register_overlay(nullptr, draw_status); // settings page under our add-on entry
	reshade::register_overlay("OSD", draw_osd);      // always-visible one-liner
}

void unregister_events()
{
	reshade::unregister_overlay("OSD", draw_osd);
	reshade::unregister_overlay(nullptr, draw_status);

	if (g_pass_finder_events_registered)
	{
		reshade::unregister_event<reshade::addon_event::execute_command_list>(
			on_pf_execute_command_list);
		reshade::unregister_event<reshade::addon_event::resolve_texture_region>(
			on_pf_resolve_texture_region);
		reshade::unregister_event<reshade::addon_event::copy_texture_region>(on_pf_copy_texture_region);
		reshade::unregister_event<reshade::addon_event::copy_resource>(on_pf_copy_resource);
	}
	if (g_finder_rt_events_registered)
	{
		reshade::unregister_event<reshade::addon_event::draw_indexed>(on_pf_draw_indexed);
		reshade::unregister_event<reshade::addon_event::draw>(on_pf_draw);
		reshade::unregister_event<reshade::addon_event::begin_render_pass>(on_pf_begin_render_pass);
		reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
			on_pf_bind_render_targets);
	}

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
