#include "backend_native/native_backend.hpp"

#include "backend_native/d3d12_hooks.hpp"
#include "backend_native/descriptor_shadow.hpp"
#include "backend_native/resource_registry.hpp"
#include "backend_native/root_shadow.hpp"
#include "backend_native/vtable_patch.hpp"
#include "core/root_signature_walk.hpp"
#include "d3d12_restore.hpp"
#include "log.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace stray_dlss::native {
namespace {

thread_local int t_own_code = 0;
std::atomic<int> g_mode{ static_cast<int>(Mode::off) };
::ID3D12Device *g_device = nullptr;
std::atomic<bool> g_use_sentinel{ true };
char g_report[512] = "native backend: not installed";
std::atomic<std::uint64_t> g_resolves{ 0 };
std::atomic<std::uint64_t> g_resolves_no_layout{ 0 };
std::atomic<icept::Sink *> g_sink{ nullptr };
std::atomic<std::uint64_t> g_drive_dispatches{ 0 };
std::atomic<std::uint64_t> g_drive_suppressed{ 0 };
std::atomic<std::uint64_t> g_drive_pipelines{ 0 };

} // namespace

void set_sink(icept::Sink *s) { g_sink.store(s, std::memory_order_release); }
icept::Sink *sink() { return g_sink.load(std::memory_order_acquire); }
void count_drive_dispatch(bool suppressed)
{
	g_drive_dispatches.fetch_add(1, std::memory_order_relaxed);
	if (suppressed)
		g_drive_suppressed.fetch_add(1, std::memory_order_relaxed);
}
void count_drive_pipeline() { g_drive_pipelines.fetch_add(1, std::memory_order_relaxed); }

Mode mode_from_string(const char *s)
{
	if (s == nullptr)
		return Mode::off;
	if (std::strcmp(s, "observe") == 0)
		return Mode::observe;
	if (std::strcmp(s, "drive") == 0)
		return Mode::drive;
	return Mode::off;
}

const char *mode_name(Mode m)
{
	switch (m)
	{
	case Mode::off: return "off";
	case Mode::observe: return "observe";
	case Mode::drive: return "drive";
	}
	return "?";
}

OwnCodeScope::OwnCodeScope() { ++t_own_code; }
OwnCodeScope::~OwnCodeScope() { --t_own_code; }
bool in_own_code() { return t_own_code > 0; }

Mode mode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }
::ID3D12Device *game_device() { return g_device; }
const char *attach_report() { return g_report; }

InstallScope install_scope_from_string(const char *s)
{
	if (s != nullptr && std::strcmp(s, "device") == 0)
		return InstallScope::device_only;
	if (s != nullptr && std::strcmp(s, "list") == 0)
		return InstallScope::list_only;
	return InstallScope::all;
}

void set_use_sentinel(bool enabled) { g_use_sentinel.store(enabled, std::memory_order_relaxed); }
bool use_sentinel() { return g_use_sentinel.load(std::memory_order_relaxed); }

bool install(::ID3D12Device *real_device, Mode requested, InstallScope scope, bool on_proxy)
{
	if (real_device == nullptr || requested == Mode::off)
	{
		std::snprintf(g_report, sizeof(g_report), "native backend: off (NativeMode=%s)", mode_name(requested));
		return false;
	}
	if (requested == Mode::drive && sink() == nullptr)
	{
		STRAY_LOG_ERROR("NativeMode=drive with NO SINK installed (set_sink was never called): the hooks "
			"would deliver to nothing. Running as observe instead.");
		requested = Mode::observe;
	}
	if (g_device != nullptr)
	{
		STRAY_LOG_WARN("native backend: already installed on %p; ignoring a second install on %p",
			static_cast<void *>(g_device), static_cast<void *>(real_device));
		return true;
	}

	registry::set_destroy_listener(&shadow::forget_resource);
	registry::arm(); // this life's sentinel page, outside the image (resource_registry.hpp)
	const unsigned dev = scope != InstallScope::list_only ? hooks::install_device_hooks(real_device, !on_proxy) : 0;
	const unsigned list = scope != InstallScope::device_only ? hooks::install_list_hooks(real_device) : 0;
	g_device = real_device;
	g_mode.store(static_cast<int>(requested), std::memory_order_relaxed);
	const char *scope_name = scope == InstallScope::device_only ? "device-only"
		: scope == InstallScope::list_only ? "list-only" : "all";
	std::snprintf(g_report, sizeof(g_report),
		"native backend: mode=%s scope=%s sentinel=%d target=%s device=%p device-slots=%u list-slots=%u patches=%u increment=%u",
		mode_name(requested), scope_name, use_sentinel() ? 1 : 0, on_proxy ? "proxy" : "real", static_cast<void *>(real_device), dev, list,
		patch_count(), hooks::descriptor_increment());
	const bool complete = (scope == InstallScope::device_only && dev != 0) ||
		(scope == InstallScope::list_only && list != 0) || (scope == InstallScope::all && dev != 0 && list != 0);
	if (!complete)
		STRAY_LOG_ERROR("%s - INCOMPLETE, the observer cannot see the game's bindings", g_report);
	else
		STRAY_LOG_INFO("%s", g_report);
	return complete;
}

void uninstall()
{
	if (g_device == nullptr)
		return;
	restore_all_patches();
	// After the hooks, so no hook can register a resource into a registry that is going away;
	// makes every sentinel of this life inert before the host can unmap us (§15 hazard).
	registry::detach();
	g_device = nullptr;
	g_mode.store(static_cast<int>(Mode::off), std::memory_order_relaxed);
	std::snprintf(g_report, sizeof(g_report), "native backend: uninstalled");
	STRAY_LOG_INFO("%s", g_report);
}

NativeBackend &backend()
{
	static NativeBackend b;
	return b;
}

namespace {

BoundTexture to_bound(std::uint32_t reg, icept::DescriptorId cpu, const shadow::ViewEntry &e)
{
	BoundTexture t;
	t.slot = reg;
	t.resource = e.resource;
	t.descriptor = cpu;
	t.format = e.is_buffer ? TexFormat::unknown : e.format;
	t.width = e.is_buffer ? 0 : e.width;
	t.height = e.is_buffer ? 0 : e.height;
	t.is_3d = e.is_3d;
	return t;
}

} // namespace

bool NativeBackend::resolve_compute_bindings(const icept::CommandContext &ctx, icept::DispatchBindings &out)
{
	g_resolves.fetch_add(1, std::memory_order_relaxed);
	root::ListState st;
	if (ctx.native == nullptr || !root::snapshot(ctx.native, st))
		return false;
	core::RootLayout layout;
	if (st.compute_root_signature == nullptr || !hooks::layout_for(st.compute_root_signature, layout))
	{
		g_resolves_no_layout.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const std::uint32_t inc = hooks::descriptor_increment();

	for (std::uint32_t param = 0; param < st.compute_tables.size(); ++param)
	{
		const std::uint64_t base = st.compute_tables[param];
		if (base == 0)
			continue;
		core::WalkResult walk;
		if (!core::walk_table(layout, param, walk))
			continue;

		// The heap this table lives in — the oracle reports the heaps its tables resolved
		// into, so the same rule here (any range type, the table's base handle).
		{
			icept::DescriptorId cpu_base = 0;
			if (shadow::gpu_to_cpu(base, cpu_base))
			{
				for (unsigned h = 0; h < st.heap_count && out.heap_count < 2; ++h)
				{
					// The bound heap whose span holds this handle: identify by re-deriving from
					// the list's SetDescriptorHeaps record.
					::ID3D12DescriptorHeap *heap = st.heaps[h];
					if (heap == nullptr)
						continue;
					const D3D12_DESCRIPTOR_HEAP_DESC hd = heap->GetDesc();
					if (hd.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && hd.Type != D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
						continue;
					const std::uint64_t gbase = heap->GetGPUDescriptorHandleForHeapStart().ptr;
					const std::uint64_t gsize = static_cast<std::uint64_t>(hd.NumDescriptors) * inc;
					if (base >= gbase && base < gbase + gsize)
					{
						bool known = false;
						for (unsigned k = 0; k < out.heap_count; ++k)
							known = known || out.heaps[k] == heap;
						if (!known)
							out.heaps[out.heap_count++] = heap;
					}
				}
			}
		}

		for (const core::TableSlot &s : walk.slots)
		{
			if (s.kind == core::RangeKind::sampler)
				continue;
			const std::uint64_t gpu = base + static_cast<std::uint64_t>(s.table_index) * inc;
			icept::DescriptorId cpu = 0;
			shadow::ViewEntry e;
			if (!shadow::gpu_to_cpu(gpu, cpu) || !shadow::lookup(cpu, e))
			{
				shadow::count_unknown_lookup();
				continue;
			}
			if (e.is_null)
			{
				shadow::count_null_lookup(); // a known-null slot: nothing bound there, by the game's own hand
				continue;
			}
			switch (s.kind)
			{
			case core::RangeKind::srv:
				if (e.resource != 0)
					out.srvs.push_back(to_bound(s.reg, cpu, e));
				break;
			case core::RangeKind::uav:
				if (e.resource != 0)
					out.uavs.push_back(to_bound(s.reg, cpu, e));
				break;
			case core::RangeKind::cbv:
				if (e.resource != 0 && !out.view_cb_valid)
				{
					out.view_cb = icept::BufferRange{ e.resource, e.buffer_offset, e.buffer_size };
					out.view_cb_valid = true;
					out.view_cb_register = s.reg;
				}
				break;
			default:
				break;
			}
		}
	}

	// Root descriptors, keyed by ROOT PARAMETER as the oracle keys them. Root SRV/UAV carry
	// only an address and contribute nothing to srvs/uavs — exactly as the oracle treats them.
	for (const auto &cbv : st.compute_root_cbv)
	{
		icept::BufferRange br;
		if (registry::buffer_for_va(cbv.second, br))
		{
			out.constant_buffers.emplace_back(cbv.first, icept::BufferRange{ br.buffer, br.offset, icept::kUnknownSize });
		}
		else
		{
			shadow::count_unknown_lookup();
			static std::atomic<int> s_said{ 0 };
			if (s_said.fetch_add(1) < 6)
				STRAY_LOG_WARN("native resolve: root CBV param %u at GPU VA %llx is in no registered buffer "
					"(registry: %llu live). Logged 6x.", cbv.first, static_cast<unsigned long long>(cbv.second),
					static_cast<unsigned long long>(registry::stats().live));
		}
	}

	return !out.srvs.empty() || !out.uavs.empty() || !out.constant_buffers.empty();
}

bool NativeBackend::resolve_graphics_srvs(const icept::CommandContext &, std::vector<BoundTexture> &)
{
	static bool said = false;
	if (!said)
	{
		said = true;
		STRAY_LOG_WARN("native backend: resolve_graphics_srvs is not implemented in this version "
			"(the graphics root state is not shadowed); the pass finder needs the ReShade backend.");
	}
	return false;
}

void NativeBackend::describe_view(icept::DescriptorId view, std::uint32_t reg, std::vector<BoundTexture> &out)
{
	shadow::ViewEntry e;
	if (!shadow::lookup(view, e) || e.resource == 0)
		return;
	if (!registry::is_live(e.resource))
		return;
	out.push_back(to_bound(reg, view, e));
}

bool NativeBackend::describe_resource(icept::ResourceId res, icept::ResourceInfo &out)
{
	return registry::describe(res, out);
}

bool NativeBackend::resource_from_view(icept::DescriptorId view, icept::ResourceId &out)
{
	shadow::ViewEntry e;
	out = 0;
	if (!shadow::lookup(view, e))
		return false;
	out = e.resource;
	return out != 0;
}

bool NativeBackend::read_buffer(const icept::BufferRange &range, std::uint64_t bytes, void *out)
{
	icept::ResourceInfo info;
	if (out == nullptr || !registry::describe(range.buffer, info) || !info.is_buffer || !info.upload_heap)
		return false;
	if (info.buffer_size < range.offset + bytes)
		return false;
	auto *res = reinterpret_cast<ID3D12Resource *>(range.buffer);
	const D3D12_RANGE read = { static_cast<SIZE_T>(range.offset), static_cast<SIZE_T>(range.offset + bytes) };
	void *mapped = nullptr;
	if (FAILED(res->Map(0, &read, &mapped)) || mapped == nullptr)
		return false;
	std::memcpy(out, static_cast<const unsigned char *>(mapped) + range.offset, static_cast<size_t>(bytes));
	const D3D12_RANGE none = { 0, 0 };
	res->Unmap(0, &none);
	return true;
}

bool NativeBackend::is_resource_live(icept::ResourceId res)
{
	return registry::is_live(res);
}

void NativeBackend::restore_game_compute_state(const icept::CommandContext &ctx)
{
	// Replays the OBSERVED root arguments — the complete list, root CBVs and constants
	// included (assessment §3.3). Not exercised by the observer; kept honest by the WARP test.
	root::ListState st;
	if (ctx.native == nullptr || !root::snapshot(ctx.native, st))
		return;
	OwnCodeScope own; // our replay must not re-enter the shadow
	if (st.heap_count != 0)
		ctx.native->SetDescriptorHeaps(st.heap_count, st.heaps);
	NativeComputeState s;
	s.root_signature = st.compute_root_signature;
	s.tables = st.compute_tables;
	for (const auto &c : st.compute_root_cbv) s.root_cbv.emplace_back(c.first, c.second);
	for (const auto &c : st.compute_root_srv) s.root_srv.emplace_back(c.first, c.second);
	for (const auto &c : st.compute_root_uav) s.root_uav.emplace_back(c.first, c.second);
	for (const auto &c : st.compute_constants) s.root_constants.emplace_back(c.first, c.second);
	s.pso = st.pso;
	restore_native_compute_state(ctx.native, s);
}

void NativeBackend::restore_viewports_and_scissors(const icept::CommandContext &)
{
	static bool said = false;
	if (!said)
	{
		said = true;
		STRAY_LOG_WARN("native backend: viewports/scissors are not shadowed in this version; nothing restored.");
	}
}

void NativeBackend::present_barrier(const icept::PresentContext &ctx, icept::ResourceId res,
                                    std::uint32_t before, std::uint32_t after)
{
	if (ctx.present_list == nullptr || res == 0)
		return;
	D3D12_RESOURCE_BARRIER b = {};
	b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	b.Transition.pResource = reinterpret_cast<ID3D12Resource *>(res);
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(before);
	b.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(after);
	ctx.present_list->ResourceBarrier(1, &b);
}

void NativeBackend::dump_tracker_state(const icept::CommandContext &ctx, const char *why)
{
	root::ListState st;
	if (ctx.native == nullptr || !root::snapshot(ctx.native, st))
	{
		STRAY_LOG_INFO("native shadow (%s): list %p never seen", why, static_cast<void *>(ctx.native));
		return;
	}
	core::RootLayout layout;
	const bool have_layout = st.compute_root_signature != nullptr && hooks::layout_for(st.compute_root_signature, layout);
	STRAY_LOG_INFO("native shadow (%s): list %p rs=%p (layout %s, %zu params) tables=%zu root-cbv=%zu "
		"root-srv=%zu root-uav=%zu constants=%zu pso=%p heaps=%u",
		why, static_cast<void *>(ctx.native), static_cast<void *>(st.compute_root_signature),
		have_layout ? "known" : "UNKNOWN", layout.params.size(), st.compute_tables.size(),
		st.compute_root_cbv.size(), st.compute_root_srv.size(), st.compute_root_uav.size(),
		st.compute_constants.size(), static_cast<void *>(st.pso), st.heap_count);
}

Stats stats()
{
	Stats s;
	s.unknown_lookups = shadow::unknown_lookups();
	s.null_lookups = shadow::null_lookups();
	s.unknown_copies = shadow::unknown_copies();
	s.resolves = g_resolves.load(std::memory_order_relaxed);
	s.resolves_no_layout = g_resolves_no_layout.load(std::memory_order_relaxed);
	s.root_signatures = hooks::root_signature_count();
	s.pipelines_hashed = hooks::pipeline_count();
	s.resources_live = registry::stats().live;
	const shadow::Stats sh = shadow::stats();
	s.slots = sh.slots;
	s.heaps = sh.heaps;
	s.patches = patch_count();
	s.drive_dispatches = g_drive_dispatches.load(std::memory_order_relaxed);
	s.drive_suppressed = g_drive_suppressed.load(std::memory_order_relaxed);
	s.drive_pipelines = g_drive_pipelines.load(std::memory_order_relaxed);
	return s;
}

void log_stats(const char *when)
{
	const Stats s = stats();
	const registry::Stats r = registry::stats();
	STRAY_LOG_INFO("NATIVE SHADOW [%s] mode=%s patches=%u resolves=%llu (no-layout %llu) unknown-lookups=%llu "
		"null-lookups=%llu unknown-copies=%llu root-signatures=%llu pipelines=%llu resources live=%llu (registered %llu, "
		"destroyed %llu, sentinel-failures %llu, unarmed %llu) slots=%llu heaps=%llu",
		when, mode_name(mode()), s.patches,
		static_cast<unsigned long long>(s.resolves), static_cast<unsigned long long>(s.resolves_no_layout),
		static_cast<unsigned long long>(s.unknown_lookups), static_cast<unsigned long long>(s.null_lookups),
		static_cast<unsigned long long>(s.unknown_copies),
		static_cast<unsigned long long>(s.root_signatures), static_cast<unsigned long long>(s.pipelines_hashed),
		static_cast<unsigned long long>(s.resources_live), static_cast<unsigned long long>(r.registered),
		static_cast<unsigned long long>(r.destroyed), static_cast<unsigned long long>(r.sentinel_failures),
		static_cast<unsigned long long>(r.unarmed),
		static_cast<unsigned long long>(s.slots), static_cast<unsigned long long>(s.heaps));
	if (mode() == Mode::drive)
		STRAY_LOG_INFO("NATIVE DRIVE [%s] dispatches delivered=%llu suppressed=%llu compute-pipelines delivered=%llu",
			when, static_cast<unsigned long long>(s.drive_dispatches), static_cast<unsigned long long>(s.drive_suppressed),
			static_cast<unsigned long long>(s.drive_pipelines));
}

std::uint64_t pipeline_hash(void *pso)
{
	return hooks::pipeline_hash(pso);
}

} // namespace stray_dlss::native
