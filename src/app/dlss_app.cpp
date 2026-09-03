// stray-dlss — the framework-free application: everything the add-on did, minus the ReShade
// registrations and the overlay, behind icept::Sink. The bodies here moved from addon.cpp
// verbatim; only the types at the seam changed. (Plan Task 7.)

#include "app/dlss_app.hpp"

#include "app/diff_observer.hpp"
#include "backend_native/descriptor_shadow.hpp"

#include <windows.h>
#include "backend_native/fg_present.hpp"
#include "backend_native/native_backend.hpp"
#include "core/fnv1a.hpp"
#include "core/taa_hashes.hpp"
#include "ext_unhook.hpp"
#include "gbuffer_finder.hpp"
#include "gbuffer_resolve.hpp"
#include "host/config.hpp"
#include "input_dump.hpp"
#include "intercept/backend.hpp"
#include "log.hpp"
#include "mv_resolve.hpp"
#include "ngx_backend.hpp"
#include "ngx_fg.hpp"
#include "ngx_nr.hpp"
#include "ngx_snippet.hpp"
#include "nr_hook.hpp"
#include "nr_mask.hpp"
#include "pass_finder.hpp"
#include "perf.hpp"
#include "shader_dump.hpp"
#include "taa_hook.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace stray_dlss::app {
namespace {


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
	std::unordered_map<ID3D12GraphicsCommandList *, uint64_t> bound_compute_pipeline;

	std::atomic<uint64_t> frame_index{ 0 };
	std::atomic<uint32_t> compute_pipelines_seen{ 0 };
	std::atomic<uint32_t> taa_pipelines_seen{ 0 };
	std::atomic<uint32_t> dispatches_seen{ 0 };

	// NGX initialisation is deferred and opt-in. Doing it inside init_device deadlocks the
	// game: NGX re-enters DXGI while ReShade and DXVK-NVAPI still hold their device-creation
	// locks, and the process wedges before the first frame. (docs/RESEARCH.md §1.4)
	std::atomic<bool> ngx_enabled{ false };
	std::atomic<bool> ngx_attempted{ false };
	std::atomic<uint32_t> shader_census{ 0 };

	// [STRAYDLSS] StatusFile / StatusFileFrames. The status file is rewritten every N
	// presents; turn it off (or raise the interval) when chasing a periodic hitch.
	std::atomic<bool> status_file_enabled{ true };
	std::atomic<uint32_t> status_file_frames{ 30 };
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

// Live NR tuning, owned by the overlay (see draw_nr_controls). Seeded from ReShade.ini at
// device init and thereafter editable in-game: every one of these is re-sent to the NGX
// parameter block on EVERY evaluate, so a change takes effect on the next frame with no feature
// recreation and no restart. Only the geometry (Width/Height/ScalingRatio) is create-time, and none of that is

// [STRAYDLSS] hash override file state; -1: no file.
int g_extra_hashes_loaded = -1;

} // namespace


// Which branch of the resolve gets its sign flipped. UE4's velocity buffer is sparse, so the
// shader has two paths and they were derived separately; an inverted one is branch-selective and
// shows only on moving objects. Exposed rather than guessed — see mv_resolve.hpp.
const char *const kMvConventions[4] = {
	"Default (no flip)",
	"Invert moving objects",
	"Invert camera",
	"Invert both",
};

namespace {

// The live NR state (NrLive) and its push-down, file-static so the moved bodies keep their
// names; the host reaches them through DlssApp::nr_live() / apply_nr_live().
NrLive g_nr_ui;

// Push the whole live set down at once. Cheap (three stores plus a few globals) and called only
// when a control actually changed, so there is no per-frame cost to having the overlay open.
void apply_nr_live_impl()
{
	nr::set_tuning(g_nr_ui.intensity, g_nr_ui.local_tone, g_nr_ui.local_structure);
	nr::set_renodx_tuning(g_nr_ui.skin_structure,
		static_cast<unsigned int>(g_nr_ui.preset < 0 ? 0 : g_nr_ui.preset),
		g_nr_ui.auto_mask ? 1u : 0u, g_nr_ui.ui_correction ? 1u : 0u);
	nr::set_style(static_cast<unsigned int>(g_nr_ui.style < 0 ? 0 : (g_nr_ui.style > 2 ? 2 : g_nr_ui.style)));
	// The checkbox is the A/B; the slider only matters while it is on. Forcing exactly 1.0 when
	// off reproduces the unsmoothed behaviour bit for bit rather than approximately.
	nr::set_mvec_scale_override(g_nr_ui.mvec_scale);

	// Branch flip, then the global axis flips on top of both.
	const bool flip_sparse = g_nr_ui.mv_convention == 1 || g_nr_ui.mv_convention == 3;
	const bool flip_camera = g_nr_ui.mv_convention == 2 || g_nr_ui.mv_convention == 3;
	const float gx = g_nr_ui.mv_invert_x ? -1.0f : 1.0f;
	const float gy = g_nr_ui.mv_invert_y ? -1.0f : 1.0f;
	const float sp = flip_sparse ? -1.0f : 1.0f;
	const float cm = flip_camera ? -1.0f : 1.0f;
	mv::set_signs(sp * gx, sp * gy, cm * gx, cm * gy);
	mv::set_legacy_transposed_clip(g_nr_ui.mv_legacy_clip);
}

} // namespace

DlssApp &instance()
{
	static DlssApp app;
	return app;
}

NrLive &DlssApp::nr_live() { return g_nr_ui; }
void DlssApp::apply_nr_live() { apply_nr_live_impl(); }

Status DlssApp::status() const
{
	Status s;
	s.have_device = g_state.native_device != nullptr;
	s.is_vkd3d = g_state.is_vkd3d;
	s.frame = g_state.frame_index.load(std::memory_order_relaxed);
	s.compute_pipelines = g_state.compute_pipelines_seen.load(std::memory_order_relaxed);
	s.taa_pipelines = g_state.taa_pipelines_seen.load(std::memory_order_relaxed);
	s.dispatches = g_state.dispatches_seen.load(std::memory_order_relaxed);
	s.ngx_attempted = g_state.ngx_attempted.load(std::memory_order_relaxed);
	s.shader_census = g_state.shader_census.load(std::memory_order_relaxed);
	s.extra_hashes_loaded = g_extra_hashes_loaded;
	return s;
}

void DlssApp::on_device(ID3D12Device *native, bool created)
{
	if (!created)
	{

		std::lock_guard<std::mutex> lock(g_state.mutex);
		ngxfg::shutdown();
		// The present stage records onto a command list of its own and hands work to nr::,
		// so it must be torn down before nr:: is.
		nrhook::shutdown();
		nr::shutdown();
		if (g_state.ngx_attempted.load(std::memory_order_relaxed))
			ngx::shutdown(g_state.native_device);
		g_state.native_device = nullptr;
		g_state.compute_pipeline_hashes.clear();
		// The native hooks belong to THIS device: they hold its pointer as the CommandContext's
		// device and the sentinels ride on its resources. The next init_device re-installs
		// (the vtable is static per class, so the slots would still work, but the pointer
		// would not). Under ReShade the add-on is reloaded across the recreate anyway (§14).
		diff::set_enabled(false);
		native::uninstall();
		return;
	}

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
	enable_ngx = host::cfg::get_bool("EnableNGX", enable_ngx);
	g_state.ngx_enabled.store(enable_ngx, std::memory_order_relaxed);
	STRAY_LOG_INFO("NGX is %s ([STRAYDLSS] EnableNGX). It initialises lazily on frame %d, "
		"never during device init.",
		enable_ngx ? "ENABLED" : "disabled", kNgxInitFrame);

	bool mv_resolve = true;
	mv_resolve = host::cfg::get_bool("MvResolve", mv_resolve);
	bool restore_heaps = true;
	restore_heaps = host::cfg::get_bool("MvRestoreHeaps", restore_heaps);
	bool restore_state = true;
	restore_state = host::cfg::get_bool("MvRestoreState", restore_state);
	int mv_dispatch_mode = 2;
	mv_dispatch_mode = host::cfg::get_int("MvDispatch", mv_dispatch_mode);
	taa_hook::configure(mv_resolve, restore_heaps, restore_state, mv_dispatch_mode);

	bool ngx_evaluate = false;
	ngx_evaluate = host::cfg::get_bool("NgxEvaluate", ngx_evaluate);
	taa_hook::set_ngx_evaluate(ngx_evaluate);

	// [STRAYDLSS] StageFile, default OFF. The per-dispatch crash breadcrumb
	// (stray-dlss-stage.txt) was written for the Phase B access violation, which is long
	// closed. What it still costs is SIX synchronous open-write-close cycles per intercepted
	// dispatch — roughly 400-1000 file operations a second through Wine's filesystem layer, on
	// the render thread. Set it to 1 to get the breadcrumb back when a new crash needs naming.
	taa_hook::set_stage_file(host::cfg::get_bool("StageFile", false));

	// [STRAYDLSS] StatusFile, default ON: the machine-readable heartbeat rewritten every
	// StatusFileFrames presents (stray-dlss-status.txt). Turn StatusFile off, or raise
	// StatusFileFrames, when chasing a periodic hitch — this is a per-N-frames file write.
	g_state.status_file_enabled.store(host::cfg::get_bool("StatusFile", true), std::memory_order_relaxed);
	{
		int status_file_frames = 30;
		status_file_frames = host::cfg::get_int("StatusFileFrames", status_file_frames);
		if (status_file_frames < 1)
			status_file_frames = 1;
		g_state.status_file_frames.store(static_cast<uint32_t>(status_file_frames), std::memory_order_relaxed);
	}

	// DLSS Frame Generation's NGX half (src/ngx_fg.hpp). The present-twice path itself is the
	// present owner's ([STRAYDLSS] NgxFG, read there); these are the feature's own knobs. The
	// generator is installed unconditionally — fg_present only calls it in ngx mode.
	{
		ngxfg::Config fc;
		fc.hdr = host::cfg::get_int("NgxFGHDR", fc.hdr);
		fc.warmup_presents = host::cfg::get_int("NgxFGWarmupFrames", fc.warmup_presents);
		fc.camera_far = host::cfg::get_float("NgxFGCameraFar", fc.camera_far);
		fc.camera_near = host::cfg::get_float("NgxFGCameraNear", fc.camera_near);
		fc.mvec_scale_mode = host::cfg::get_int("NgxFGMvecScale", fc.mvec_scale_mode);
		fc.provide_output_real = host::cfg::get_bool("NgxFGOutputReal", fc.provide_output_real);
		ngxfg::configure(fc);
		native::fg::set_generator(&ngxfg::generator());
	}

	// [STRAYDLSS] PerfLog, default ON: frame-time + our-CPU-share reports every 600 presents.
	// Left on by default because the instrumentation is cheap (one clock read per present;
	// per-call timers only on the few size-gated dispatches) and a perf report nobody enabled
	// is a wasted round trip. Set 0 to silence it.
	bool perf_log = true;
	perf_log = host::cfg::get_bool("PerfLog", perf_log);
	perf::set_enabled(perf_log);
	// [STRAYDLSS] StallWatch (default ON): one [stall] line per present whose interval exceeds
	// 3x the running median, attributing the frame (facts §32.13). Cheap (a few atomics/present).
	perf::set_stall_watch(host::cfg::get_bool("StallWatch", true));

	// [STRAYDLSS] ShadowMode: fast (flat lock-free arrays), debug (sharded map with provenance),
	// or absent = AUTO. Selected once, before the game creates any descriptor heap; logged and
	// written to the status file so a number is never attributed to the wrong shadow (facts §28).
	// AUTO picks DEBUG when ReShade is in the process (Config B): the fast path needs every heap
	// registered before use, and under ReShade some staging heaps are created before our device
	// hook or through its proxy device, so ~15-19% of frames resolved the pinned TAA to unknown,
	// the gate refused, and the image flickered (facts §29). Debug keys the shadow by the raw
	// handle and handles any heap; Config B is NR-bound (~52 fps) so its cost is irrelevant.
	// Config A (no ReShade) stays fast - the parity win.
	char shadow_mode[16] = {};
	native::shadow::Mode sm;
	if (host::cfg::get_string("ShadowMode", shadow_mode, sizeof(shadow_mode)) && shadow_mode[0] != '\0')
	{
		sm = (shadow_mode[0] == 'd' || shadow_mode[0] == 'D') ? native::shadow::Mode::debug : native::shadow::Mode::fast;
	}
	else
	{
		bool reshade = false;
		if (HMODULE dxgi = ::GetModuleHandleW(L"dxgi.dll"))
			reshade = ::GetProcAddress(dxgi, "ReShadeRegisterAddon") != nullptr;
		sm = reshade ? native::shadow::Mode::debug : native::shadow::Mode::fast;
		STRAY_LOG_INFO("ShadowMode=auto: ReShade %s in the process -> %s shadow",
			reshade ? "IS" : "is NOT", reshade ? "debug" : "fast");
	}
	native::shadow::set_mode(sm);

	// [STRAYDLSS] NgxRR: 0 off (default, SR unchanged), 1 = probe DLSSD existence on this
	// stack (one CreateFeature attempt, released; SR keeps running), 2 = full RR-first
	// evaluate with per-frame SR fallback. Both non-zero modes still require EnableNGX=1
	// and NgxEvaluate=1 — the probe rides the SR feature-creation path and mode 2 rides
	// the SR evaluate site.
	int ngx_rr = 0;
	ngx_rr = host::cfg::get_int("NgxRR", ngx_rr);
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
	host::cfg::get_string("GBufferResolveAt", resolve_at, sizeof(resolve_at));
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
	host::cfg::get_string("NgxExposure", exposure_mode, sizeof(exposure_mode));
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
	exposure_scale = host::cfg::get_float("NgxExposureScale", exposure_scale);
	ngx::set_exposure_scale(exposure_scale);
	if (exposure_texture)
		STRAY_LOG_INFO("DLSS InExposureScale = %.4f ([STRAYDLSS] NgxExposureScale). The "
			"indicator's 'Exposure level' should read this; a 0.25/1.0/4.0 sweep that moves "
			"the image proves DLSS consumes our exposure texture.", exposure_scale);

	// [STRAYDLSS] NgxNR: DLSS Neural Rendering (NGX feature 18 / DLSSNR). 0 = off (default,
	// byte-identical to today), 1 = on. Runs AFTER the SR/RR evaluate as a post-pass on the
	// image they produced. Needs the leaked nvngx_dlssnr.dll staged beside the game exe (or
	// NgxNRDll); on a 4090 it must be an Ada-patched build.
	// (docs/RESEARCH-RENODX-DLSS5.md §2.1-§3.2)
	bool ngx_nr = false;
	ngx_nr = host::cfg::get_bool("NgxNR", ngx_nr);
	nr::set_enabled(ngx_nr);

	// [STRAYDLSS] NgxSnippetPath: extra directory for NGX's own snippet search (additive to
	// its default application-directory search). Must be set before ngx::initialise.
	char snippet_path[480] = "";
	host::cfg::get_string("NgxSnippetPath", snippet_path, sizeof(snippet_path));
	ngx::set_snippet_path(snippet_path);

	char nr_dll[480] = "";
	host::cfg::get_string("NgxNRDll", nr_dll, sizeof(nr_dll));
	nr::set_dll_path(nr_dll);


	// Live quality knobs (the study's DLSSNR.* tuning parameters, §2.2).
	// Defaults are RenoDX's own shipped [RenoDX.DLSS5] values rather than a neutral 1.0 — we
	// follow their configuration instead of inventing one.
	g_nr_ui.intensity = host::cfg::get_float("NgxNRIntensity", g_nr_ui.intensity);
	g_nr_ui.local_tone = host::cfg::get_float("NgxNRLocalTone", g_nr_ui.local_tone);
	g_nr_ui.local_structure = host::cfg::get_float("NgxNRLocalStructure", g_nr_ui.local_structure);
	g_nr_ui.skin_structure = host::cfg::get_float("NgxNRSkinStructure", g_nr_ui.skin_structure);
	g_nr_ui.preset = host::cfg::get_int("NgxNRPreset", g_nr_ui.preset);
	// DLSSNR.Style, docs/RESEARCH-DLSSNR-STYLES.md. Default 0 reproduces today (the parameter
	// was never written before this knob existed). Clamped in apply_nr_live_impl, not here, so
	// an out-of-range ini value is still visible to a host's status display rather than being
	// silently rewritten on load.
	g_nr_ui.style = host::cfg::get_int("NgxNRStyle", g_nr_ui.style);
	int nr_auto_mask = g_nr_ui.auto_mask ? 1 : 0;
	int nr_ui_correction = g_nr_ui.ui_correction ? 1 : 0;
	nr_auto_mask = host::cfg::get_int("NgxNRAutoMask", nr_auto_mask);
	nr_ui_correction = host::cfg::get_int("NgxNRUICorrection", nr_ui_correction);
	g_nr_ui.auto_mask = nr_auto_mask != 0;
	g_nr_ui.ui_correction = nr_ui_correction != 0;
	// [STRAYDLSS] NgxNRMask + NgxNRMaskChannelR/G/B + NgxNRMaskR/G/B + NgxNRMaskFormat.
	//
	// DLSSNR.ControlMask: the per-pixel RGB control texture the runtime genuinely consumes (the
	// disassembly is in src/core/nr_mask_plan.hpp) and the only channel through which structure
	// and tone can vary SPATIALLY. Default OFF, and every channel default 1.0 = the identity, so
	// `NgxNRMask=1` alone binds a mask that provably changes nothing — which is the experiment to
	// run FIRST, because a mask that is silently ignored and a mask that does nothing look
	// identical on a screenshot.
	//
	// The channel switches exist so R, G and B can be tested apart: a channel that is off writes
	// the neutral 1.0, so turning two off still exercises the whole binding path.
	{
		nrmaskplan::Config mask;
		mask.enabled = host::cfg::get_int("NgxNRMask", 0) != 0;
		mask.channel_r = host::cfg::get_int("NgxNRMaskChannelR", 1) != 0;
		mask.channel_g = host::cfg::get_int("NgxNRMaskChannelG", 1) != 0;
		mask.channel_b = host::cfg::get_int("NgxNRMaskChannelB", 1) != 0;
		mask.value_r = host::cfg::get_float("NgxNRMaskR", nrmaskplan::kNeutral);
		mask.value_g = host::cfg::get_float("NgxNRMaskG", nrmaskplan::kNeutral);
		mask.value_b = host::cfg::get_float("NgxNRMaskB", nrmaskplan::kNeutral);
		nrhook::set_mask(mask);

		// The mask's DXGI_FORMAT. A KNOB and not a constant because the runtime never inspects a
		// caller-supplied texture's format on this path, so getting it wrong is the silent-wrong-
		// image class rather than an error (src/nr_mask.hpp carries the evidence for the default).
		nrmask::set_format(host::cfg::get_int("NgxNRMaskFormat", nrmask::kDefaultMaskFormat));
	}

	const float nr_intensity = g_nr_ui.intensity;
	const float nr_local_tone = g_nr_ui.local_tone;
	const float nr_local_structure = g_nr_ui.local_structure;

	// [STRAYDLSS] NgxNRIdentity: what our GetModuleFileNameW hook reports to the snippet —
	// "nvngx" (default), "passthrough", "snippet", "exe". The runtime requires the reported
	// path to CONTAIN the substring "nvngx.dll" (src/ngx_snippet.hpp records the measurement),
	// so "nvngx" is the only mode that gets past its identity check; the rest are diagnostic.
	char nr_identity[24] = "nvngx";
	host::cfg::get_string("NgxNRIdentity", nr_identity, sizeof(nr_identity));
	snippet::set_identity_from_string(nr_identity);

	g_nr_ui.mvec_scale = host::cfg::get_float("NgxNRMVecScale", g_nr_ui.mvec_scale);
	g_nr_ui.mv_convention = host::cfg::get_int("MVConvention", g_nr_ui.mv_convention);
	int mv_ix = 0, mv_iy = 0;
	mv_ix = host::cfg::get_int("MVInvertX", mv_ix);
	mv_iy = host::cfg::get_int("MVInvertY", mv_iy);
	g_nr_ui.mv_invert_x = mv_ix != 0;
	g_nr_ui.mv_invert_y = mv_iy != 0;
	const float nr_mvec_scale = g_nr_ui.mvec_scale;

	g_nr_ui.enabled = ngx_nr;
	apply_nr_live_impl();

	if (ngx_nr)
	{
		STRAY_LOG_WARN("NgxNR=1: DLSS Neural Rendering (feature 18) runs as a PRESENT STAGE "
			"over the back buffer. intensity=%.2f localTone=%.2f localStructure=%.2f "
			"mvecScaleOverride=%.3f. The image is display-referred there, so there is no HDR "
			"codec and no exposure term. Grep 'NR' lines, and 'NR STAGE' for the stage's own "
			"gate.",
			nr_intensity, nr_local_tone, nr_local_structure, nr_mvec_scale);
		// [STRAYDLSS] NgxNRPreload, default ON: LoadLibrary + export resolution + the
		// identity IAT patch at device init — matching RenoDX's "signed NR runtime
		// (nvngx_dlssnr.dll) pre-loaded at device init". All of it is pure memory work with
		// no GPU contact. Init_Ext and the feature create are NOT done here: they move to
		// first use behind the warmup gate (see ngx_nr.hpp), because initialising a leaked
		// pre-release snippet during device creation is both a startup stall and the leading
		// suspect for two measured GPU_IS_LOST events. RenoDX's own fallback string ("will
		// retry lazily on first evaluate") confirms load and init are independent states.
		bool nr_preload = true;
		nr_preload = host::cfg::get_bool("NgxNRPreload", nr_preload);
		int nr_warmup = 60;
		nr_warmup = host::cfg::get_int("NgxNRWarmupFrames", nr_warmup);
		nr::set_warmup_frames(nr_warmup < 0 ? 0u : static_cast<unsigned int>(nr_warmup));
		STRAY_LOG_INFO("NR: preload=%d warmupFrames=%d (RenoDX initialises on the FIRST "
			"evaluate; we wait longer on purpose after two GPU_IS_LOST events).",
			nr_preload ? 1 : 0, nr_warmup);
		if (nr_preload)
			nr::preload();
	}

	// [STRAYDLSS] GBufferResolveOnly: record + dump guides at the SSD trigger, but skip
	// the RR evaluate (SR carries frames) — the record-vs-evaluate fault isolator.
	bool resolve_only = false;
	resolve_only = host::cfg::get_bool("GBufferResolveOnly", resolve_only);
	taa_hook::set_gbuffer_resolve_only(resolve_only);
	if (resolve_only && ngx_rr == 2)
		STRAY_LOG_WARN("GBufferResolveOnly=1: RR evaluate disabled; guide records (and "
			"dumps) still run at the SSD trigger. SR carries every frame.");

	// [STRAYDLSS] GBufferSwapBC: the B/C content-check flip (gbuffer_resolve.hpp). The B/C
	// slot order is unverified by content until the guide dump says otherwise; this flips
	// which identified resource feeds which role, no rebuild.
	bool swap_bc = false;
	swap_bc = host::cfg::get_bool("GBufferSwapBC", swap_bc);
	if (swap_bc || ngx_rr == 2)
		gbr::set_bc_swapped(swap_bc); // logs its state; skipped when RR is off and unswapped

	int ngx_dry_run = 0;
	ngx_dry_run = host::cfg::get_int("NgxDryRun", ngx_dry_run);
	taa_hook::set_ngx_dry_run(ngx_dry_run);

	char dry_hash[384] = "";
	const bool dry_hash_read = host::cfg::get_string("DryRunHash", dry_hash, sizeof(dry_hash));
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
	STRAY_LOG_INFO("config probe: DryRunHash read=%d raw='%.48s' parsed=%zu",
		dry_hash_read ? 1 : 0, dry_hash, dry_hash_count);

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
	host::cfg::get_string("NgxPassHash", ngx_pass, sizeof(ngx_pass));
	const std::uint64_t ngx_pass_value = std::strtoull(ngx_pass, nullptr, 0);
	taa_hook::set_ngx_pass_hash(ngx_pass_value);
	if (ngx_pass_value != 0)
		STRAY_LOG_INFO("DLSS will replace pass 0x%016llx ([STRAYDLSS] NgxPassHash).",
			static_cast<unsigned long long>(ngx_pass_value));

	const int ngx_preset = host::cfg::get_int("NgxPreset", 0);
	if (ngx_preset != 0)
	{
		ngx::set_preset(ngx_preset);
		STRAY_LOG_INFO("NgxPreset=%d requested ([STRAYDLSS] NgxPreset; 10=J 11=K 12=L 13=M).",
			ngx_preset);
	}

	bool ext_unhook_enabled = true;
	ext_unhook_enabled = host::cfg::get_bool("ExtUnhook", ext_unhook_enabled);
	ext_unhook::set_enabled(ext_unhook_enabled);
	if (!ext_unhook_enabled)
		STRAY_LOG_WARN("ExtUnhook=0: ReShade's vkd3d ext-vtable patch will NOT be undone before "
			"NGX calls; expect the frozen-output failure while anything queries the proxy.");

	bool dump_inputs = false;
	dump_inputs = host::cfg::get_bool("NgxDumpInputs", dump_inputs);
	input_dump::set_enabled(dump_inputs);
	// 0 keeps the shipped 600/900. Raise it when the run is still on a loading screen there —
	// a black capture answers nothing and costs a whole round trip (src/core/dump_plan.hpp).
	input_dump::set_points(host::cfg::get_int("NgxDumpAt", 0));
	if (dump_inputs)
		STRAY_LOG_WARN("NgxDumpInputs is ON: the DLSS colour/depth inputs, the engine's raw "
			"sparse velocity, our resolved motion vectors and the output are dumped to "
			"straydlss_*.bin at evaluates %llu and %llu ([STRAYDLSS] NgxDumpAt moves them).",
			static_cast<unsigned long long>(input_dump::first_point()),
			static_cast<unsigned long long>(input_dump::second_point()));

	bool ngx_paint = false;
	ngx_paint = host::cfg::get_bool("NgxPaint", ngx_paint);
	taa_hook::set_ngx_paint(ngx_paint);
	if (ngx_paint)
		STRAY_LOG_WARN("NgxPaint is ON: the TAA output is cleared to MAGENTA instead of "
			"evaluating. If the screen is not magenta, the output handle is wrong.");

	const bool alt_read = host::cfg::has("DryRunAlternate");
	const int dry_alternate = host::cfg::get_int("DryRunAlternate", 0);
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

	// [STRAYDLSS] NativeMode: off (default) | observe | drive. `observe` installs our own D3D12
	// vtable hooks on the ORIGINAL device BESIDE the driving backend and diffs every size-gated
	// dispatch's bindings against the driver's answer (src/app/diff_observer.hpp). It changes
	// nothing on screen; it is how the native backend earns the right to drive (plan Task 15).
	// `drive` (plan Stage 3) makes those hooks THE driver: they deliver the seam's events to
	// this application (the host stops forwarding the ones the hooks now own), the native
	// resolve, View-CB read and restore answer the TAA hook, and the Dispatch hook suppresses
	// the engine's TAA when DLSS produced its output. Which icept::Backend the seam hands out
	// is the HOST's decision, made from native::mode() after this returns.
	{
		char native_mode[16] = "off";
		host::cfg::get_string("NativeMode", native_mode, sizeof(native_mode));
		const native::Mode mode = native::mode_from_string(native_mode);
		if (mode != native::Mode::off)
		{
			char scope[16] = "all";
			host::cfg::get_string("NativeInstall", scope, sizeof(scope));
			native::set_use_sentinel(host::cfg::get_bool("NativeSentinel", true));
			// [STRAYDLSS] NativeTarget: real (default) patches vkd3d's own vtables beneath ReShade's
			// proxies, so the hooks see REAL handles after ReShade's conversion — but also fire for
			// every internal call ReShade itself makes. `proxy` patches ReShade's proxy vtables
			// instead: only the game's calls, in ReShade's minted handle space. MEASURED 2026-09-01:
			// `real` crashes the game at the device recreate with device-only, list-only and
			// sentinel-off alike; `proxy` is the stacking bisection.
			char target[16] = "real";
			host::cfg::get_string("NativeTarget", target, sizeof(target));
			ID3D12Device *install_on = native;
			if (std::strcmp(target, "proxy") == 0)
			{
				ID3D12Device *proxy = reshade_proxy_device(native);
				if (proxy != nullptr)
				{
					install_on = proxy;
					STRAY_LOG_WARN("NativeTarget=proxy: installing the native hooks on ReShade's PROXY device %p "
						"(vkd3d's real device is %p). Descriptor identities in the shadow are ReShade-minted.",
						static_cast<void *>(proxy), static_cast<void *>(native));
				}
				else
				{
					STRAY_LOG_ERROR("NativeTarget=proxy requested but ReShade's proxy device could not be recovered; "
						"installing on the real device instead.");
				}
			}
			// The hooks deliver to this application in drive mode; harmless otherwise.
			native::set_sink(this);
			const bool ok = native::install(install_on, mode, native::install_scope_from_string(scope),
				install_on != native);
			diff::set_enabled(ok && native::mode() == native::Mode::observe);
			STRAY_LOG_WARN("NativeMode=%s ([STRAYDLSS] NativeMode, read as \"%s\"): %s. Grep 'DIFF' and "
				"'NATIVE SHADOW'.", native::mode_name(native::mode()), native_mode, native::attach_report());
			if (mode == native::Mode::drive && (!ok || native::mode() != native::Mode::drive))
				STRAY_LOG_ERROR("NativeMode=drive was requested but the native backend is %s - the ReShade "
					"backend keeps driving. Nothing DLSS does this session comes from the native path.",
					ok ? native::mode_name(native::mode()) : "INCOMPLETE");
			else if (native::mode() == native::Mode::drive)
				STRAY_LOG_WARN("NATIVE DRIVE: the D3D12 vtable hooks now deliver pipeline creation, pipeline "
					"binds, list resets and every Dispatch to this application; the native resolve, "
					"View-CB read, liveness and restore answer the TAA hook; the engine's TAA is "
					"suppressed from the Dispatch hook. ReShade stays loaded as the host (present, "
					"screenshots, overlay) and no longer decides. Grep 'NATIVE DRIVE'.");
		}
		else
		{
			STRAY_LOG_INFO("NativeMode=off ([STRAYDLSS] NativeMode): the native D3D12 hooks are not installed.");
		}
	}

	load_hash_override_file();
}

void DlssApp::on_command_list(const icept::CommandContext &ctx, bool created)
{
	if (!created)
		return;

	// One-shot vkd3d probe. Doing it here rather than at device init because the detector
	// interface lives on the command list.
	if (g_state.is_vkd3d)
		return;

	ID3D12GraphicsCommandList *native = ctx.native;
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

void DlssApp::on_command_list_reset(const icept::CommandContext &ctx)
{

	taa_hook::forget_command_list(ctx);
	pass_finder::forget_command_list(ctx);
	gbuffer_finder::forget_command_list(ctx);
}


void DlssApp::on_pipeline(std::uint64_t pipeline, const void *code, std::size_t code_size,
                          icept::ShaderKind kind, bool created)
{
	if (!created)
	{
		// ID3D12PipelineState pointers get recycled, so eviction is not optional.
		taa_hook::forget_pipeline(pipeline);
		pass_finder::forget_pipeline(pipeline);
		std::lock_guard<std::mutex> lock(g_state.mutex);
		g_state.compute_pipeline_hashes.erase(pipeline);
		return;
	}

	const bool is_compute = kind == icept::ShaderKind::compute;
	if (code == nullptr || code_size == 0)
		return;

	const std::uint64_t hash = fnv1a64(code, code_size);

	{
		std::lock_guard<std::mutex> lock(g_state.mutex);
		g_state.distinct_shader_hashes.insert(hash);
	}

	if (!is_compute)
	{
		// The census counts PS too, but only CS is hashed for identification. The pass
		// finder additionally attributes draws by pixel-shader hash.
		pass_finder::note_pipeline(pipeline, hash, /*is_compute=*/false);
		return;
	}

	{
		std::lock_guard<std::mutex> lock(g_state.mutex);
		g_state.compute_pipeline_hashes[pipeline] = hash;
	}
	g_state.compute_pipelines_seen.fetch_add(1, std::memory_order_relaxed);
	pass_finder::note_pipeline(pipeline, hash, /*is_compute=*/true);

	// Getting the real bytecode off the user's machine settles the binding layout
	// offline, which is worth far more than any inference we could make here.
	shader_dump::dump_compute_shader(hash, code, code_size);
	taa_hook::set_pipeline_hash(pipeline, hash);

	if (is_known_taa_hash(hash))
	{
		g_state.taa_pipelines_seen.fetch_add(1, std::memory_order_relaxed);
		STRAY_LOG_INFO("FOUND FTAAStandaloneCS permutation: hash=0x%016llx "
			"pipeline=0x%016llx bytes=%zu",
			static_cast<unsigned long long>(hash),
			static_cast<unsigned long long>(pipeline),
			code_size);
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

void DlssApp::on_bind_pipeline(const icept::CommandContext &ctx, std::uint64_t pipeline)
{
	// NOTE: in D3D12 this fires with pipeline_stage::all, NOT compute_shader. Filtering on
	// the compute stage would silently miss every event. (docs/RESEARCH.md §2.3)

	// Remember the pipeline per command list so a dispatch can be attributed to a hash.
	taa_hook::set_bound_pipeline(ctx, pipeline);
	pass_finder::note_bind_pipeline(ctx, pipeline);
	std::lock_guard<std::mutex> lock(g_state.mutex);
	if (g_state.compute_pipeline_hashes.count(pipeline) != 0)
		g_state.bound_compute_pipeline[ctx.native] = pipeline;
}

bool DlssApp::on_dispatch(const icept::CommandContext &ctx, std::uint32_t x, std::uint32_t y, std::uint32_t z)
{

	g_state.dispatches_seen.fetch_add(1, std::memory_order_relaxed);

	{
		// Everything we record onto the game's list from here (the resolve and the NGX evaluate)
		// is OUR state, and the native shadow must not mistake it for the game's
		// (assessment §8.3).
		native::OwnCodeScope own;
		if (taa_hook::intercept_dispatch(ctx, x, y, z))
			return true;
	}

	// THE DIFFERENTIAL OBSERVER. The driver's resolve of this dispatch, parked for the native
	// Dispatch hook — which fires inside the driver's forward of this very call, on this
	// thread — to compare against its own. Only for dispatches the TAA path would look at
	// (the same size gate), because a resolve per dispatch is the expensive operation.
	if (diff::enabled() && z == 1 && x >= 32 && y >= 18)
	{
		std::uint64_t hash = 0;
		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			const auto bound = g_state.bound_compute_pipeline.find(ctx.native);
			if (bound != g_state.bound_compute_pipeline.end())
			{
				const auto it = g_state.compute_pipeline_hashes.find(bound->second);
				if (it != g_state.compute_pipeline_hashes.end())
					hash = it->second;
			}
		}
		icept::DispatchBindings expected;
		icept::backend()->resolve_compute_bindings(ctx, expected);
		diff::publish_expected(ctx.native, hash, x, y, expected);
	}

	// Strictly AFTER the intercept decision: a suppressed dispatch never executes, and
	// recording one would enter a phantom writer into the pass finder's last-writer table.
	pass_finder::note_dispatch(ctx, x, y, z);

	if (shader_dump::enabled() || gbuffer_finder::enabled())
	{
		std::uint64_t hash = 0;
		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			const auto bound = g_state.bound_compute_pipeline.find(ctx.native);
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
			gbuffer_finder::note_dispatch(ctx, hash);
		}
	}
	// Skeleton stage: observe only. Returning true here is what will eventually suppress the
	// engine's TAA dispatch — it is the only skip-capable event on our path.
	return false;
}

void DlssApp::on_render_targets(const icept::CommandContext &ctx, std::uint32_t count,
                                const icept::DescriptorId *rtvs, icept::DescriptorId dsv,
                                bool via_render_pass)
{
	// The render-target event feeds both finders, each a cheap no-op when its feature is off.
	//
	// `via_render_pass` is currently unread. It distinguished begin_render_pass from a plain
	// OMSetRenderTargets for the `preui` NR trigger, which was removed on 2026-09-02 and is not
	// coming back — the present stage needs no render-target event at all. The parameter stays
	// because the two hosts already compute it and a finder that needs the distinction should not
	// have to re-plumb it.
	(void)via_render_pass;
	pass_finder::note_render_targets(ctx, count, rtvs, dsv);
	gbuffer_finder::note_render_targets(ctx, count, rtvs, dsv);
}

void DlssApp::on_draw(const icept::CommandContext &ctx, std::uint32_t vertex_or_index_count)
{
	pass_finder::note_draw(ctx, vertex_or_index_count);
	gbuffer_finder::note_draw(ctx);
}

void DlssApp::on_copy(const icept::CommandContext &ctx, icept::ResourceId src, icept::ResourceId dst)
{
	pass_finder::note_copy(ctx, src, dst);
}

void DlssApp::on_execute(const icept::CommandContext &ctx)
{
	pass_finder::note_execute(ctx);
}

void DlssApp::on_swapchain(const icept::ResourceId *back_buffers, std::uint32_t count, bool created)
{
	// No consumer since the pre-UI NR site was removed (2026-09-02); the event stays on the seam
	// because a host that owns the swapchain reports it, and the log is where it lands.
	if (created && count != 0 && back_buffers != nullptr)
		STRAY_LOG_INFO("swapchain: %u back buffer(s), first %p", count, reinterpret_cast<void *>(back_buffers[0]));
}

std::uint64_t DlssApp::next_frame()
{
	return g_state.frame_index.fetch_add(1, std::memory_order_relaxed);
}

void DlssApp::on_present(const icept::PresentContext &pc)
{

	const uint64_t frame = pc.frame;

	// Frame-time sampling and the periodic CPU-share report. Fed the cumulative counters we
	// already maintain, so it adds no hot-path cost of its own. (src/perf.hpp)
	// The NR path's present boundary: signals its lifetime fence on the swapchain's own queue,
	// performs every deferred free the fence has cleared (a queued NgxNR=0 teardown, a resolution
	// change's ReleaseFeature), and drains the validation readback that gates NR ever touching
	// the screen. The queue is what makes "the GPU is done with this" answerable at all
	// (src/core/nr_lifetime.hpp); with none, NR falls back to a conservative present ring.
	nr::on_present(pc.queue);
	// THE NR PRESENT STAGE: where DLSS Neural Rendering actually runs.
	//
	// STRICTLY AFTER nr::on_present, and that ordering is load-bearing rather than tidy.
	// nr::on_present signals the NR lifetime fence on this queue and advances the timeline, but
	// our present list is only EXECUTED after this whole callback returns — so work recorded
	// before the signal would be tagged with a value the GPU reaches before our commands run, and
	// every deferred free (ReleaseFeature included) would happen under an in-flight list.
	// Recording after it tags the work with the NEXT present's value. (src/nr_hook.hpp)
	{
		ID3D12Device *nr_device = nullptr;
		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			nr_device = g_state.native_device;
		}
		nrhook::on_present(pc, nr_device);
	}
	perf::on_present(g_state.dispatches_seen.load(std::memory_order_relaxed),
		taa_hook::diagnostics().large_dispatches);

	// The observer's periodic report: the DIFF SUMMARY (disagreements=) and the native
	// shadow's own counters, every 600 presents while it is on.
	if (native::mode() != native::Mode::off && frame != 0 && (frame % 600) == 0)
	{
		char when[32];
		std::snprintf(when, sizeof(when), "frame %llu", static_cast<unsigned long long>(frame));
		if (diff::enabled())
			diff::log_summary(when);
		native::log_stats(when); // in drive mode this carries the NATIVE DRIVE delivered/suppressed counters
		// DLSS-G's own accounting, so a source-missing refusal in the [fg] line can be
		// attributed: no fresh guides (no TAA dispatch this frame: loads, menus, cuts), NGX not
		// initialised yet, or the warm-up. Measured 2026-09-02: steady gameplay is 2.00x and
		// every gap was one of those three (facts §32.10).
		if (native::fg::enabled() && native::fg::config().mode == native::fg::Mode::ngx)
		{
			const ngxfg::Stats gs = ngxfg::stats();
			STRAY_LOG_INFO("[fg/ngx] %s: created=%d publishes=%llu generates=%llu evaluate-failures=%llu | refused: no-publish=%llu not-ready=%llu warmup=%llu | %ux%u hdr=%d guides %ux%u | last evaluate 0x%08x",
				when, gs.created ? 1 : 0, static_cast<unsigned long long>(gs.publishes), static_cast<unsigned long long>(gs.generates),
				static_cast<unsigned long long>(gs.evaluate_failures), static_cast<unsigned long long>(gs.refused_no_publish),
				static_cast<unsigned long long>(gs.refused_not_ready), static_cast<unsigned long long>(gs.refused_warmup),
				gs.width, gs.height, gs.hdr, gs.render_width, gs.render_height, gs.last_evaluate_result);
		}
	}

	// Drives DryRunAlternate's phase and logs each transition, so a screenshot's timestamp
	// identifies which state produced it.
	taa_hook::note_present(frame);
	input_dump::on_present();

	// The pass finder's frame boundary. The presented back buffer is its fallback anchor
	// when no tonemapper (3D LUT SRV) was seen this frame.
	if (pass_finder::enabled())
		pass_finder::on_present(frame, reinterpret_cast<icept::ResourceId>(pc.back_buffer));

	// The G-buffer finder's frame boundary: stability accounting and the (log-only)
	// identification report. (gbuffer_finder.hpp, DLSS-RR phase 1)
	if (gbuffer_finder::enabled())
		gbuffer_finder::on_present(frame);

	// A machine-readable heartbeat, so automation can tell menu from gameplay without a human
	// looking at the screen. Rewritten in place every StatusFileFrames presents (default 30);
	// cheap and always current. [STRAYDLSS] StatusFileFrames (default 30) sets the cadence;
	// the census below (read by the overlay/log via DlssApp::status()) keeps updating on that
	// cadence regardless. [STRAYDLSS] StatusFile (default ON) gates only the file WRITE — turn
	// it off, or raise StatusFileFrames, when chasing a periodic hitch.
	if ((frame % g_state.status_file_frames.load(std::memory_order_relaxed)) == 0)
	{
		uint32_t census = 0;
		{
			std::lock_guard<std::mutex> lock(g_state.mutex);
			census = static_cast<uint32_t>(g_state.distinct_shader_hashes.size());
		}
		g_state.shader_census.store(census, std::memory_order_relaxed);

		std::FILE *f = nullptr;
		if (g_state.status_file_enabled.load(std::memory_order_relaxed) &&
			fopen_s(&f, "stray-dlss-status.txt", "w") == 0 && f != nullptr)
		{
			std::fprintf(f, "frame=%llu\n", static_cast<unsigned long long>(frame));
			std::fprintf(f, "shader_census=%u\n", census);
			std::fprintf(f, "compute_pipelines=%u\n", g_state.compute_pipelines_seen.load());
			std::fprintf(f, "taa_pipelines=%u\n", g_state.taa_pipelines_seen.load());
			std::fprintf(f, "dispatches=%u\n", g_state.dispatches_seen.load());
			std::fprintf(f, "vkd3d=%d\n", g_state.is_vkd3d ? 1 : 0);
			{
				// Frame generation (src/backend_native/fg_present.hpp): the probe's engine frame
				// counter against fg_presents_issued is the "~2x presents per engine frame" check.
				const native::fg::Stats fs = native::fg::stats();
				std::fprintf(f, "fg_enabled=%d\n", native::fg::enabled() ? 1 : 0);
				std::fprintf(f, "fg_game_presents=%llu\n", (unsigned long long)fs.game_presents);
				std::fprintf(f, "fg_presents_issued=%llu\n", (unsigned long long)fs.presents_issued);
				std::fprintf(f, "fg_generated_presented=%llu\n", (unsigned long long)fs.generated_presented);
				std::fprintf(f, "fg_reconfigures=%llu\n", (unsigned long long)fs.reconfigures);
				std::fprintf(f, "fg_replacements=%u\n", fs.replacement_count);
				std::fprintf(f, "fg_pacer_ms=%.2f\n", fs.pacer_interval_ms);
				std::fprintf(f, "fg_issued_p50_ms=%u\n", fs.issued_p50_ms);
				std::fprintf(f, "fg_issued_p99_ms=%u\n", fs.issued_p99_ms);
				std::fprintf(f, "fg_issued_bimodal=%d\n", fs.issued_second_peak_ms >= 0 ? 1 : 0);
				std::fprintf(f, "fg_validated=%d\n", fs.validated ? 1 : 0);
				std::fprintf(f, "fg_crop_black=%llu\n", (unsigned long long)fs.crop_black);
				std::fprintf(f, "fg_crop_stale=%llu\n", (unsigned long long)fs.crop_stale);
				// The flip-queue throttle: armed, refused and inert are three different states
				// and the status file has to tell them apart without the log.
				std::fprintf(f, "fg_throttle_armed=%d\n", fs.throttle.armed ? 1 : 0);
				std::fprintf(f, "fg_throttle_refusal=%s\n", core::fg::throttle_refusal_name(fs.throttle.refusal));
				std::fprintf(f, "fg_throttle_flag=%s\n", core::fg::flag_verdict_name(fs.flag_verdict));
				std::fprintf(f, "fg_throttle_latency=%d\n", fs.throttle.max_latency_after == ~0u ? -1 : (int)fs.throttle.max_latency_after);
				std::fprintf(f, "fg_throttle_waits=%llu\n", (unsigned long long)fs.throttle.waits);
				std::fprintf(f, "fg_throttle_slots=%llu\n", (unsigned long long)fs.throttle.slots);
				std::fprintf(f, "fg_throttle_timeouts=%llu\n", (unsigned long long)fs.throttle.timeouts);
				std::fprintf(f, "fg_throttle_bypassed=%llu\n", (unsigned long long)fs.throttle.bypassed);
				std::fprintf(f, "fg_throttle_blocked_mean_ms=%.3f\n", core::fg::throttle_blocked_mean_ms(fs.throttle));
				unsigned long long refused = 0;
				for (int i = 1; i < static_cast<int>(core::fg::Refusal::count); ++i)
					refused += fs.refused[i];
				std::fprintf(f, "fg_refused=%llu\n", refused);
				const ngxfg::Stats gs = ngxfg::stats();
				std::fprintf(f, "fg_ngx_created=%d\n", gs.created ? 1 : 0);
				std::fprintf(f, "fg_ngx_generates=%llu\n", (unsigned long long)gs.generates);
				std::fprintf(f, "fg_ngx_evaluate_failures=%llu\n", (unsigned long long)gs.evaluate_failures);
				std::fprintf(f, "fg_ngx_refused_no_publish=%llu\n", (unsigned long long)gs.refused_no_publish);
				std::fprintf(f, "fg_ngx_refused_not_ready=%llu\n", (unsigned long long)gs.refused_not_ready);
				std::fprintf(f, "fg_ngx_refused_warmup=%llu\n", (unsigned long long)gs.refused_warmup);
			}
			std::fprintf(f, "shadow_mode=%s\n", native::shadow::mode() == native::shadow::Mode::fast ? "fast" : "debug");
			std::fprintf(f, "ngx_attempted=%d\n",
				g_state.ngx_attempted.load(std::memory_order_relaxed) ? 1 : 0);
			// in_game under the NATIVE HOST cannot use the shader census: the host feeds
			// on_pipeline COMPUTE pipelines only (store_pipeline is called from
			// CreateComputePipelineState / the stream CreatePipelineState's CS), so the census
			// counts distinct COMPUTE shaders (~34 in The Slums gameplay) where the ReShade
			// host's init_pipeline saw ALL pipelines (~390-728). The >=300 gate is therefore
			// structurally false under the host (facts §20/§22). Use a signal the host
			// demonstrably has: the TAA pass identified AND DLSS driving frames (dispatches
			// advancing between writes). This says "DLSS is live and driving", which is what an
			// unattended A/B needs; it is NOT a strict menu-vs-gameplay classifier (that needs
			// the §2.4 depth-range gate the host does not sample).
			static std::atomic<std::uint32_t> s_last_dispatches{ 0 };
			const std::uint32_t disp_now = g_state.dispatches_seen.load();
			const std::uint32_t disp_prev = s_last_dispatches.exchange(disp_now);
			const bool advancing = disp_now > disp_prev;
			const bool driving = g_state.taa_pipelines_seen.load() >= 1 && advancing;
			std::fprintf(f, "in_game=%d\n", driving ? 1 : 0);
			const auto &d = taa_hook::diagnostics();
			std::fprintf(f, "large_dispatches=%llu\n", (unsigned long long)d.large_dispatches);
			std::fprintf(f, "no_bound_pipeline=%llu\n", (unsigned long long)d.no_bound_pipeline);
			std::fprintf(f, "no_hash=%llu\n", (unsigned long long)d.no_hash);
			std::fprintf(f, "resolve_failed=%llu\n", (unsigned long long)d.resolve_failed);
			std::fprintf(f, "reports=%llu\n", (unsigned long long)d.candidates_reported);
			if (diff::enabled())
			{
				const diff::Summary ds = diff::summary();
				const native::Stats ns = native::stats();
				std::fprintf(f, "diff_dispatches=%llu\n", (unsigned long long)ds.dispatches);
				std::fprintf(f, "diff_disagreements=%llu\n", (unsigned long long)(ds.dispatches - ds.agree));
				std::fprintf(f, "diff_mismatch=%llu\n", (unsigned long long)ds.mismatch);
				std::fprintf(f, "diff_unknown=%llu\n", (unsigned long long)ds.unknown);
				std::fprintf(f, "diff_extra=%llu\n", (unsigned long long)ds.extra);
				std::fprintf(f, "diff_taa=%llu\n", (unsigned long long)ds.taa_dispatches);
				std::fprintf(f, "diff_taa_disagree=%llu\n", (unsigned long long)ds.taa_disagree);
				std::fprintf(f, "native_unknown_lookups=%llu\n", (unsigned long long)ns.unknown_lookups);
			}
			if (native::mode() == native::Mode::drive)
			{
				const native::Stats ns = native::stats();
				std::fprintf(f, "native_drive_dispatches=%llu\n", (unsigned long long)ns.drive_dispatches);
				std::fprintf(f, "native_drive_suppressed=%llu\n", (unsigned long long)ns.drive_suppressed);
			}
			if (nr::enabled())
			{
				// NR's refusal breakdown, machine-readable. The periodic log line carries the
				// same numbers, but a log line has to be found and read; an A/B harness reads
				// this file.
				std::uint64_t nr_applied = 0, nr_refused = 0;
				std::uint32_t nr_reasons[nr::kNrRefusalCount] = {};
				nr::counters(nr_applied, nr_refused, nr_reasons);
				std::fprintf(f, "nr_applied=%llu\n", (unsigned long long)nr_applied);
				std::fprintf(f, "nr_refused=%llu\n", (unsigned long long)nr_refused);
				std::fprintf(f, "nr_validated=%d\n", nr::validated() ? 1 : 0);
				for (int i = 0; i < nr::kNrRefusalCount; ++i)
					std::fprintf(f, "nr_refused_%s=%u\n", nr::kNrRefusalNames[i], nr_reasons[i]);
			}
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

		// WAIT FOR THE HOOK ONLY IF SOMETHING COULD STILL INSTALL IT.
		//
		// The deadline above is a ReShade-era assumption that outlived its host. ONLY ReShade
		// installs this patch, and only from inside its own proxy device's QueryInterface
		// (v6.8.0 d3d12_extensions.cpp) - so if no ReShade proxy is wrapping this device, the
		// patch can never appear and `hooked` is false forever. Under the UE4SS plugin that is
		// every session, and it cost the full deadline to decide something already decided:
		// kNgxDecisionDeadline - kNgxInitFrame presents of DLSS-less startup, every launch.
		//
		// The test is THE PROXY, not icept::backend()->name(). The name answers "who drives
		// our seam", which is a different question and is wrong in BOTH directions here:
		//   * the ReShade host running NativeMode=drive reports
		//     "native(drive)+reshade(present)" while ReShade is plainly present, and
		//   * the plugin host reports "native" even when ReShade is loaded alongside it -
		//     which is not hypothetical but a DOCUMENTED, SUPPORTED setup:
		//     mods/StrayDLSS/README.md's "Config B (ReShade coexists): keep ReShade as
		//     dxgi.dll; the plugin unwraps its proxies".
		// Either misreading skips a wait that is load-bearing and lands us on the silent
		// wrong-texture row of the truth table in CLAUDE.md §1. Config B is exactly the case
		// a name test gets wrong AND the case where getting it wrong costs an image.
		//
		// Evaluated ONCE. ReShade registers the proxy in its D3D12Device constructor, inside
		// D3D12CreateDevice, so by the first present it is either there or never will be - and
		// a per-present GetPrivateData would AddRef a proxy we do not own. A null device latches
		// -1 and keeps the old deadline behaviour rather than initialising NGX on nothing.
		static const int s_reshade_proxy =
			native == nullptr ? -1 : (reshade_proxy_device(native) != nullptr ? 1 : 0);
		const bool waiting_is_pointless = s_reshade_proxy == 0;

		if (hooked || waiting_is_pointless || frame >= kNgxDecisionDeadline)
		{
			g_state.ngx_attempted.store(true, std::memory_order_relaxed);

			// Say which path this session took, and when. The two are indistinguishable from
			// the rest of the log otherwise, and the whole point of the change is a frame
			// number a pasted log can be checked against.
			if (waiting_is_pointless && !hooked)
				STRAY_LOG_INFO("NGX decision taken at frame %llu WITHOUT waiting for the "
					"deadline (%d): no ReShade proxy wraps this device, so nothing in this "
					"process can install the vkd3d ext-vtable patch. Under a ReShade host this "
					"waits exactly as it always did.",
					static_cast<unsigned long long>(frame), kNgxDecisionDeadline);
			else
				STRAY_LOG_INFO("NGX decision taken at frame %llu after waiting for the ext-hook "
					"question to settle (hook %s, deadline %d, ReShade proxy %s).",
					static_cast<unsigned long long>(frame), hooked ? "INSTALLED" : "still absent",
					kNgxDecisionDeadline,
					s_reshade_proxy == 1 ? "present - the patch could appear at any time"
						: s_reshade_proxy == 0 ? "absent"
						: "unknown, the device was null at the first check");

			// [STRAYDLSS] NgxDevice: auto (default), native, or proxy.
			//
			// The automatic choice follows the measured ext-hook state, which is the reasoning
			// in CLAUDE.md §1. But the crash we are chasing faults inside d3d12core.dll —
			// vkd3d-proton itself — with registers holding packed handle-shaped values rather
			// than pointers, which is what an unconverted descriptor handle reaching vkd3d
			// looks like. That makes the device choice worth testing rather than assuming, so
			// it can be forced either way from the ini.
			char device_pref[16] = "auto";
			host::cfg::get_string("NgxDevice", device_pref, sizeof(device_pref));
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
				STRAY_LOG_INFO("NGX will use the NATIVE device (%p): no ext hook at frame "
					"%llu, so nothing will convert our handles.",
					static_cast<void *>(native), static_cast<unsigned long long>(frame));
			}

			STRAY_LOG_INFO("Initialising NGX (frame %llu)...",
				static_cast<unsigned long long>(frame));
			ngx::initialise(device);
		}
		else if (frame == kNgxInitFrame)
		{
			STRAY_LOG_INFO("Deferring NGX: a ReShade proxy wraps this device, so its vkd3d ext "
				"hook may still be installed, and that decides which device NGX must use "
				"(deadline frame %d).", kNgxDecisionDeadline);
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

		if (nr::enabled())
		{
			std::uint64_t nr_applied = 0, nr_refused = 0;
			std::uint32_t nr_reasons[nr::kNrRefusalCount] = {};
			nr::counters(nr_applied, nr_refused, nr_reasons);
			// Sized for EVERY reason name plus its count. This was 256, which the list has
			// outgrown — and a truncated diagnostic drops the reasons at the end of the array,
			// which are the newest ones and therefore the ones a session is most likely asking
			// about.
			char nr_line[640];
			int off = std::snprintf(nr_line, sizeof(nr_line),
				"[%s] NR applied=%llu refused=%llu validated=%d reasons:", when,
				static_cast<unsigned long long>(nr_applied),
				static_cast<unsigned long long>(nr_refused), nr::validated() ? 1 : 0);
			for (int i = 0; i < nr::kNrRefusalCount; ++i)
				if (off > 0 && off < static_cast<int>(sizeof(nr_line)))
					off += std::snprintf(nr_line + off, sizeof(nr_line) - off, " %s=%u",
						nr::kNrRefusalNames[i], nr_reasons[i]);
			STRAY_LOG_INFO("%s", nr_line);

			// EVERY DLSSNR.Reset DISCARDS FEATURE 18'S WHOLE ACCUMULATION, so a source that
			// fires often is a flicker source rather than a fix for one. `frame-gap` is the one
			// that still follows a continuously varying quantity — how reliably the TAA hook
			// matches — and CLAUDE.md has already measured a latch on such a quantity firing 52
			// times and making the image WORSE. So this line is the one that says whether our own
			// reset machinery is the problem. Read it while WALKING: a static camera moves
			// neither exposure nor the hook rate.
			//
			// A sixth source, `codec-scale`, went with the HDR colour codec on 2026-09-03. Its
			// field outlived its argument here for one commit, which is a printf with more
			// conversions than values — so if a source is ever removed again, remove the field in
			// the same edit.
			const nr::ResetCounts rc = nr::reset_counters();
			STRAY_LOG_INFO("[%s] NR RESETS: total=%u  from: frame-gap=%u guide-grid=%u "
				"camera-cut=%u new-feature=%u", when,
				rc.frame_gap + rc.guide_grid + rc.camera_cut + rc.new_feature,
				rc.frame_gap, rc.guide_grid, rc.camera_cut, rc.new_feature);

			// THE PRESENT STAGE's own gate, which sits IN FRONT of nr::apply and therefore in
			// front of every counter above. Without this line "NR applied=0" cannot be told apart
			// from "the stage never got as far as asking", which is the exact ambiguity the TAA
			// path's gate refusals exist to remove.
				const nrhook::Counters sc = nrhook::counters();
				char line[512];
				int stage_off = std::snprintf(line, sizeof(line),
					"[%s] NR STAGE: triggered=%llu applied=%llu backbuffer=%ux%u fmt=%u "
					"staging=%lluMB colour/guide ratio=%.4f/%.4f (REPORTED, not sent — NGX gets "
					"1.0) reasons:",
					when, static_cast<unsigned long long>(sc.triggered),
					static_cast<unsigned long long>(sc.applied), sc.last_back_buffer_width,
					sc.last_back_buffer_height, sc.last_back_buffer_format,
					static_cast<unsigned long long>(sc.staging_bytes >> 20),
					static_cast<double>(sc.last_mvec_scale_x),
					static_cast<double>(sc.last_mvec_scale_y));
				for (int i = 0; i < nrplan::kPlanResultCount; ++i)
					if (stage_off > 0 && stage_off < static_cast<int>(sizeof(line)))
						stage_off += std::snprintf(line + stage_off, sizeof(line) - stage_off,
							" %s=%u",
							nrplan::plan_result_name(static_cast<nrplan::PlanResult>(i)),
							sc.reasons[i]);
				STRAY_LOG_INFO("%s", line);

				// THE MASK, reported separately and by its ACTUAL content. "Bound and
				// deliberately neutral" and "refused" must never look alike here — an identity
				// mask is a real experimental state, not an absence.
				STRAY_LOG_INFO("[%s] NR MASK: %s bound=%d verdict=%s live RGB=(%.3f, %.3f, %.3f) "
					"fills=%llu format=%d %lluKB", when,
					sc.mask_bound ? "DLSSNR.ControlMask IS BOUND" : "no ControlMask",
					sc.mask_bound ? 1 : 0, nrmaskplan::mask_result_name(sc.last_mask_result),
					static_cast<double>(sc.mask_r), static_cast<double>(sc.mask_g),
					static_cast<double>(sc.mask_b),
					static_cast<unsigned long long>(sc.mask_fills), nrmask::format(),
					static_cast<unsigned long long>(sc.mask_bytes >> 10));
		}
	}

	// Report the add-on-level capability verdict once, after enough frames that the game has
	// certainly bound something.
	// Well after gameplay has started, so every steady-state pass has had a chance to run.
	if (frame == 2400)
		taa_hook::dump_summary();

}

EventNeeds DlssApp::configure_events()
{
	EventNeeds needs;

	bool hash_shaders = true;
	hash_shaders = host::cfg::get_bool("HashShaders", hash_shaders);

	// [STRAYDLSS] PassFinder, default OFF: dataflow identification of the TAA pass — the
	// bounded walk backwards from the tonemapper (core/pass_walk.hpp), proven in CI and here
	// only LOGGING its verdict against the live game. It attributes events by shader hash,
	// so enabling it forces the pipeline events (and their PSO-cache cost) on.
	bool pass_finder_enabled = false;
	pass_finder_enabled = host::cfg::get_bool("PassFinder", pass_finder_enabled);
	pass_finder::set_enabled(pass_finder_enabled);

	// [STRAYDLSS] GBufferFinder, default OFF: log-only identification of the base pass's
	// G-buffer targets for DLSS Ray Reconstruction (gbuffer_finder.hpp). It needs the
	// render-target/draw events below, and the pipeline events for the SSR-denoiser
	// cross-check's shader hash.
	bool gbuffer_finder_enabled = false;
	gbuffer_finder_enabled = host::cfg::get_bool("GBufferFinder", gbuffer_finder_enabled);
	{
		// NgxRR=2 consumes the finder's identification (taa_hook::try_evaluate_rr), so the
		// finder must observe even when GBufferFinder was not set explicitly. Read here as
		// well as in on_init_device because event registration happens first.
		int ngx_rr_for_finder = 0;
		ngx_rr_for_finder = host::cfg::get_int("NgxRR", ngx_rr_for_finder);
		if ((ngx_rr_for_finder == 2 || ngx_rr_for_finder == 3) && !gbuffer_finder_enabled)
		{
			gbuffer_finder_enabled = true;
			STRAY_LOG_WARN("GBufferFinder forced ON: NgxRR=2/3 needs the G-buffer "
				"identification it produces.");
		}
	}
	gbuffer_finder::set_enabled(gbuffer_finder_enabled);

	needs.pipeline_events = hash_shaders || shader_dump::enabled() ||
		pass_finder_enabled || gbuffer_finder_enabled;

	if (needs.pipeline_events)
	{
		STRAY_LOG_WARN("Pipeline events registered: ReShade will drop the D3D12 PSO cache, so "
			"EXPECT A VERY SLOW FIRST LOAD while every shader recompiles.");
		STRAY_LOG_WARN("  Set [STRAYDLSS] HashShaders=0 (and DumpShaders=0) to get normal load "
			"times once the bytecode has been captured.");
	}
	else
	{
		STRAY_LOG_INFO("Pipeline hashing disabled; the TAA pass will be identified structurally.");
	}
	// ReShade's own state_tracking does not register this, which is why its state_block can
	// never replay root constants.

	if (pass_finder_enabled || gbuffer_finder_enabled)
	{
		needs.finder_rt_events = true;
		// Logged so a pasted log PROVES the events were registered for this combination of
		// flags — the 2026-08-31 GBufferFinder run could not distinguish "tap registered
		// but never fired" from "tap never registered" after the fact.
		STRAY_LOG_INFO("Finder RT/draw events registered (PassFinder=%d GBufferFinder=%d).",
			pass_finder_enabled ? 1 : 0, gbuffer_finder_enabled ? 1 : 0);
	}
	// [STRAYDLSS] NgxNRStageBackBufferState, default 0 = D3D12_RESOURCE_STATE_PRESENT (which
	// equals COMMON). D3D12 REQUIRES the back buffer to be in that state at Present and the
	// stage runs inside it, so this is stronger than a derivation — but "required by the API"
	// is not "true on this stack", and vkd3d has no debug layer to say otherwise, so it stays
	// overridable.
	{
		int bb_state = 0;
		bb_state = host::cfg::get_int("NgxNRStageBackBufferState", bb_state);
		nrhook::set_back_buffer_state(bb_state < 0 ? 0u : static_cast<std::uint32_t>(bb_state));
	}

	if (pass_finder_enabled)
		needs.pass_finder_events = true;


	return needs;
}

void DlssApp::log_final_census(bool saw_bind_pipeline, bool saw_push_descriptors)
{
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
	if (diff::enabled())
	{
		diff::log_summary("final");
		native::log_stats("final");
	}
	STRAY_LOG_INFO("Final census: compute pipelines=%u, TAA matches=%u, dispatches=%u, "
		"bind_pipeline=%d, push_descriptors=%d",
		g_state.compute_pipelines_seen.load(std::memory_order_relaxed),
		g_state.taa_pipelines_seen.load(std::memory_order_relaxed),
		g_state.dispatches_seen.load(std::memory_order_relaxed),
		saw_bind_pipeline ? 1 : 0, saw_push_descriptors ? 1 : 0);
}

// [STRAYDLSS] hash override file: one fnv1a64 per line (0x-prefixed or bare hex, # comments),
// generated offline by tools/stray_taa_hashes.py after a game update. Read from the game's
// working directory, the same place ReShade.log lands.
void DlssApp::shutdown()
{
	diff::set_enabled(false);
	native::uninstall();
	native::set_sink(nullptr);
}

void DlssApp::load_hash_override_file()
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

// Live DLSS-NR controls. Everything here is safe to change mid-frame: each value is written
// into the NGX parameter block on EVERY evaluate, so an edit lands on the next frame with no
// feature recreation. The values are
// NOT written back to ReShade.ini automatically — "Save to ReShade.ini" does that on demand,
// because ReShade rewrites its config on exit and a silent autosave would make an experiment

} // namespace stray_dlss::app
