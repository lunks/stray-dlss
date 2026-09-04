#include "pool_name_hook.hpp"

#include "intercept/backend.hpp"
#include "log.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#endif

namespace stray_dlss::poolhook {
namespace {

using seam::Region;

// ---------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------

std::mutex g_mutex;
pool::Level g_level = pool::Level::off;
bool g_asked_supply = false;

InstallFn g_install = nullptr;
RemoveFn g_remove = nullptr;

// The module image, mapped once. Its byte pointers are the live mapping, so `g_regions` and
// the section-name storage must outlive every scan.
std::vector<char> g_section_names;
std::vector<Region> g_regions;
seam::Image g_image{};
u0::FunctionTable g_table{};
bool g_mapped = false;

pool::Locate g_locate{};
bool g_scanned = false;   // the module does not change under us; scan ONCE per process
bool g_discovered = false;
bool g_hooked = false;
void *g_target = nullptr;

// The forwarding original, written by the installer.
using FindFreeElementFn = bool (*)(void *self, void *cmd_list, const void *desc, void **out,
                                   const wchar_t *name, unsigned char transience, bool defer);
FindFreeElementFn g_original = nullptr;

Record g_records[pool::kNameCount]{};

// Counters. Everything is under g_mutex; the thunk fires on the order of tens of times a frame
// and the lock is never held across an engine call.
std::uint64_t g_calls = 0;                 // every call the engine made through us
std::uint64_t g_status_counts[static_cast<std::size_t>(pool::RecordStatus::count)] = {};
std::uint64_t g_names_unknown = 0;         // plausible wide strings we have no Target for
std::uint64_t g_faults = 0;
std::uint64_t g_fault_va = 0;
bool g_disabled = false;                   // a fault latched the resolve off for the session
bool g_first_call_logged = false;

// The assertion counters, per oracle pair.
struct Assertion
{
	std::uint64_t agree = 0;
	std::uint64_t disagree = 0;
	std::uint64_t absent = 0;
	bool logged = false;
};
Assertion g_assert_depth;      // SceneDepthZ vs L1's FPassInputs.SceneDepthTexture
Assertion g_assert_velocity;   // GBufferVelocity vs L1's FPassInputs.SceneVelocityTexture
Assertion g_assert_colour;     // SceneColorDeferred vs the TAA bind stream's t1 (OBSERVATION)
Assertion g_assert_extent;     // every extent-predicted name vs View row 132
bool g_extent_logged[pool::kNameCount] = {}; // ...but the WARN is per NAME, not per session

// THE NAME CENSUS. UEVR logs every name it has never seen once (RenderTargetPoolHook.cpp's
// m_seen_names + SPDLOG_INFO_ONCE) and the replan's §11.7 asks for the same: one session
// produces this title's complete pooled-render-target list, measured rather than assumed, and
// that list is what a future RR guide table should be built from.
constexpr std::size_t kCensusMax = 96;
constexpr std::size_t kCensusNameMax = 48;
char g_census[kCensusMax][kCensusNameMax] = {};
std::size_t g_census_count = 0;
std::uint64_t g_census_overflow = 0;

// ---------------------------------------------------------------------------------------
// The module image (the seam's map_own_module, duplicated deliberately: each mechanism owns
// its own mapping so a level of one is never silently coupled to a level of another)
// ---------------------------------------------------------------------------------------

#if defined(_WIN32)
bool map_own_module(const char *&why)
{
	HMODULE module = GetModuleHandleW(nullptr);
	if (module == nullptr)
	{
		why = "GetModuleHandleW(nullptr) returned nothing";
		return false;
	}
	auto *base = reinterpret_cast<const unsigned char *>(module);
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		why = "the main module has no MZ header";
		return false;
	}
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
	{
		why = "the main module is not a PE32+ x64 image";
		return false;
	}

	const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
	const unsigned count = nt->FileHeader.NumberOfSections;
	g_section_names.assign(static_cast<std::size_t>(count) * 9u, '\0');
	g_regions.clear();
	for (unsigned i = 0; i < count; ++i, ++section)
	{
		if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0)
			continue;
		if ((section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0)
			continue;
		char *name = g_section_names.data() + static_cast<std::size_t>(i) * 9u;
		std::memcpy(name, section->Name, 8);
		name[8] = '\0';
		Region r;
		r.va = reinterpret_cast<std::uint64_t>(base) + section->VirtualAddress;
		r.bytes = base + section->VirtualAddress;
		r.size = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize : section->SizeOfRawData;
		r.executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
		r.name = name;
		g_regions.push_back(r);
	}
	if (g_regions.empty())
	{
		why = "no readable sections";
		return false;
	}
	g_image.regions = g_regions.data();
	g_image.count = g_regions.size();

	const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
	if (dir.VirtualAddress == 0 || dir.Size < sizeof(u0::RuntimeFunction))
	{
		why = "the image has no exception directory (.pdata), so a CALL target cannot be held "
		      "to landing on a function start";
		return false;
	}
	static_assert(sizeof(u0::RuntimeFunction) == 12, "RUNTIME_FUNCTION is three RVAs");
	g_table.entries = reinterpret_cast<const u0::RuntimeFunction *>(base + dir.VirtualAddress);
	g_table.count = dir.Size / sizeof(u0::RuntimeFunction);
	g_table.image_base = reinterpret_cast<std::uint64_t>(base);
	g_table.image = &g_image;
	g_mapped = true;
	return true;
}
#else
bool map_own_module(const char *&why)
{
	why = "not a Windows build";
	return false;
}
#endif

// ---------------------------------------------------------------------------------------
// The guards — L1's, verbatim in behaviour (docs/RESEARCH-ENGINE-TAA-HOOK.md §12.5)
// ---------------------------------------------------------------------------------------

bool region_covers(std::uint64_t va, std::size_t n, bool want_code)
{
	for (const Region &r : g_regions)
	{
		if (want_code && !r.executable)
			continue;
		if (r.size < n || va < r.va)
			continue;
		if (va - r.va <= r.size - n)
			return true;
	}
	return false;
}

bool plausible_heap_ptr(std::uint64_t va)
{
	if (va == 0 || (va & 7ull) != 0)
		return false;
	return va >= 0x10000ull && va < 0x0000800000000000ull;
}

#if defined(_WIN32)
bool committed_and_readable(std::uint64_t va, std::size_t n)
{
	MEMORY_BASIC_INFORMATION mbi{};
	const SIZE_T got = VirtualQuery(
		reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(va)), &mbi, sizeof(mbi));
	if (got != sizeof(mbi) || mbi.State != MEM_COMMIT)
		return false;
	if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
		return false;
	constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
	if ((mbi.Protect & kReadable) == 0)
		return false;
	const std::uint64_t base = reinterpret_cast<std::uint64_t>(mbi.BaseAddress);
	const std::uint64_t size = static_cast<std::uint64_t>(mbi.RegionSize);
	if (va < base || size < n)
		return false;
	return va - base <= size - n;
}
#else
bool committed_and_readable(std::uint64_t, std::size_t) { return false; }
#endif

// The read itself, under SEH. Same explicit exception as L1's: this dereferences engine memory
// through offsets that are [derived] from source and cannot be proven from outside the engine,
// so the honest position is that it CAN fault and the process must survive it saying so.
bool read_bytes_guarded(std::uint64_t va, void *out, std::size_t n)
{
#if defined(_MSC_VER)
	__try
	{
		std::memcpy(out, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(va)), n);
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
#else
	std::memcpy(out, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(va)), n);
	return true;
#endif
}

// Caller must hold g_mutex, or be the thunk before it takes one — the fault counters are the
// only shared state touched and they are written under the lock by every caller here.
bool read_u64(std::uint64_t va, std::uint64_t *out)
{
	if (!plausible_heap_ptr(va) && !region_covers(va, sizeof(std::uint64_t), false))
		return false;
	if (!committed_and_readable(va, sizeof(std::uint64_t)))
		return false;
	if (!read_bytes_guarded(va, out, sizeof(std::uint64_t)))
	{
		++g_faults;
		g_fault_va = va;
		return false;
	}
	return true;
}

using GetNativeResourceFn = void *(*)(void *rhi);

bool call_native_resource_guarded(std::uint64_t fn, std::uint64_t rhi, void **out)
{
#if defined(_MSC_VER)
	__try
	{
		*out = reinterpret_cast<GetNativeResourceFn>(static_cast<std::uintptr_t>(fn))(
			reinterpret_cast<void *>(static_cast<std::uintptr_t>(rhi)));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
#else
	*out = reinterpret_cast<GetNativeResourceFn>(static_cast<std::uintptr_t>(fn))(
		reinterpret_cast<void *>(static_cast<std::uintptr_t>(rhi)));
	return true;
#endif
}

void note_fault_and_latch()
{
	if (g_faults == 0 || g_disabled)
		return;
	g_disabled = true;
	STRAY_LOG_ERROR("POOL NAMES DISABLED: a guarded read or call into engine memory FAULTED at "
		"%#llx, inside FRenderTargetPool::FindFreeElement on the render thread. The guards "
		"caught it (the game is alive and this line exists because of it), but a fault means "
		"one of the [derived] constants is wrong on THIS executable - "
		"IPooledRenderTarget::RenderTargetItem.TargetableTexture @%zu or "
		"FRHITexture::GetNativeResource slot %u. The map is frozen and nothing reads it; the "
		"forwarding thunk stays installed and inert so the engine keeps working. Paste this "
		"line: the offset is one constant, not another round trip.",
		static_cast<unsigned long long>(g_fault_va), pool::kPooledTargetableTexture,
		seam::kRhiGetNativeResourceSlot);
}

// FRHITexture* -> ID3D12Resource*. Reads the vptr, indexes slot 7, checks the slot is code and
// calls it — every hop guarded, every failure a named status. Same three hops L1 performs on
// depth and velocity every frame (report §14.1: this IS GetNativeResource, spelled as an
// offset, because the accessor is a virtual we cannot name).
std::uint64_t resolve_texture(std::uint64_t rhi, pool::RecordStatus &status)
{
	if (rhi == 0)
	{
		status = pool::RecordStatus::rhi_null;
		return 0;
	}
	std::uint64_t vtable = 0;
	if (!read_u64(rhi, &vtable) || vtable == 0)
	{
		status = pool::RecordStatus::rhi_unreadable;
		return 0;
	}
	std::uint64_t fn = 0;
	if (!read_u64(vtable + 8ull * seam::kRhiGetNativeResourceSlot, &fn) || fn == 0)
	{
		status = pool::RecordStatus::rhi_unreadable;
		return 0;
	}
	if (!region_covers(fn, 1, true))
	{
		status = pool::RecordStatus::fn_not_code;
		return 0;
	}
	void *native = nullptr;
	if (!call_native_resource_guarded(fn, rhi, &native))
	{
		++g_faults;
		g_fault_va = rhi;
		status = pool::RecordStatus::rhi_unreadable;
		return 0;
	}
	status = pool::RecordStatus::ok;
	return reinterpret_cast<std::uint64_t>(native);
}

// The `const TCHAR*` argument, into ASCII. UE's TCHAR is UTF-16LE on Windows and every pooled
// render-target name in 4.27 is plain ASCII, so anything else is not a name we can act on —
// which is itself the runtime validator that a hook on the WRONG function would fail.
bool read_wide_ascii(std::uint64_t va, char *out, std::size_t cap)
{
	out[0] = '\0';
	if (va == 0 || (va & 1ull) != 0)
		return false;
	// The string may end near a page boundary, so take the largest readable window rather than
	// insisting on one size.
	std::size_t want = 2u * cap;
	unsigned char buf[2 * 96] = {};
	if (want > sizeof(buf))
		want = sizeof(buf);
	while (want >= 4 && !committed_and_readable(va, want))
		want /= 2;
	if (want < 4 || !committed_and_readable(va, want))
		return false;
	if (!read_bytes_guarded(va, buf, want))
	{
		++g_faults;
		g_fault_va = va;
		return false;
	}
	std::size_t n = 0;
	for (std::size_t i = 0; i + 1 < want && n + 1 < cap; i += 2)
	{
		const unsigned unit = static_cast<unsigned>(buf[i]) | (static_cast<unsigned>(buf[i + 1]) << 8);
		if (unit == 0)
		{
			out[n] = '\0';
			return n != 0;
		}
		if (unit < 0x20 || unit > 0x7E)
			return false; // not a plausible pooled-render-target name
		out[n++] = static_cast<char>(unit);
	}
	// No terminator inside the window: either a very long name or not a string at all. Either
	// way we cannot match it, and claiming a truncated name would be worse than declining.
	return false;
}

void note_census(const char *name)
{
	for (std::size_t i = 0; i < g_census_count; ++i)
	{
		if (std::strcmp(g_census[i], name) == 0)
			return;
	}
	if (g_census_count >= kCensusMax)
	{
		++g_census_overflow;
		return;
	}
	std::snprintf(g_census[g_census_count], kCensusNameMax, "%s", name);
	++g_census_count;
	STRAY_LOG_INFO("POOL NAMES: first sight of pooled render target \"%s\" (%zu distinct names "
		"so far). This census is the title's OWN list, measured; it is what an RR guide table "
		"should be built from rather than a table copied out of the engine's source.",
		name, g_census_count);
}

bool resource_is_live(std::uint64_t id)
{
	if (id == 0)
		return false;
	icept::Backend *b = icept::backend();
	return b != nullptr && b->is_resource_live(static_cast<icept::ResourceId>(id));
}

bool describe(std::uint64_t id, icept::ResourceInfo &out)
{
	if (id == 0)
		return false;
	icept::Backend *b = icept::backend();
	return b != nullptr && b->describe_resource(static_cast<icept::ResourceId>(id), out);
}

// ---------------------------------------------------------------------------------------
// The stand-in
// ---------------------------------------------------------------------------------------

bool hk_find_free_element(void *self, void *cmd_list, const void *desc, void **out,
                          const wchar_t *name, unsigned char transience, bool defer)
{
	FindFreeElementFn original = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		original = g_original;
	}
	// FORWARD FIRST AND UNCONDITIONALLY. `Out` is written by the engine — on the keep-current
	// path it was already holding the element and the function early-returns true
	// (RenderTargetPool.cpp:660-672), on the allocate path it is assigned at :719 — so
	// everything we want to read only exists after the call. If the original is somehow gone,
	// doing nothing would leave the engine without a render target, so there is no other
	// defensible behaviour than forwarding or never having patched.
	const bool result = original != nullptr
		? original(self, cmd_list, desc, out, name, transience, defer)
		: false;

	// 96 is not arbitrary: UE 4.27's longest pooled name in the renderer is
	// "TranslucencyLightingVolumeAmbient0" (34), and a name we cannot read to its terminator
	// is counted as `name-unreadable` - the counter that says the hook is on the wrong
	// function. Leaving it tight would put a long but perfectly legitimate name in that bucket
	// and turn the one validator that matters into a liar.
	char ascii[96] = {};
	std::lock_guard<std::mutex> lock(g_mutex);
	++g_calls;
	if (g_disabled)
		return result;

	if (!read_wide_ascii(reinterpret_cast<std::uint64_t>(name), ascii, sizeof(ascii)))
	{
		++g_status_counts[static_cast<std::size_t>(pool::RecordStatus::name_unreadable)];
		note_fault_and_latch();
		return result;
	}
	if (!g_first_call_logged)
	{
		g_first_call_logged = true;
		STRAY_LOG_INFO("POOL NAMES: FRenderTargetPool::FindFreeElement reached us, and its "
			"fourth argument reads as the wide string \"%s\". That is the runtime validator "
			"the static scan cannot give: a hook on the wrong function would be handed "
			"something that is not a name. Every subsequent call is checked the same way.",
			ascii);
	}
	note_census(ascii);

	const int index = pool::match_name(ascii);
	if (index < 0)
	{
		++g_names_unknown;
		++g_status_counts[static_cast<std::size_t>(pool::RecordStatus::unknown_name)];
		return result;
	}

	Record &rec = g_records[static_cast<std::size_t>(index)];
	rec.seen = true;
	++rec.calls;

	const std::uint64_t out_va = reinterpret_cast<std::uint64_t>(out);
	std::uint64_t pooled = 0;
	if (out_va == 0)
		rec.status = pool::RecordStatus::out_null;
	else if (!read_u64(out_va, &pooled))
		rec.status = pool::RecordStatus::pooled_unreadable;
	else if (pooled == 0)
		rec.status = pool::RecordStatus::out_null; // bDeferTextureAllocation, or a failed alloc
	else
		rec.status = pool::RecordStatus::ok;
	if (rec.status != pool::RecordStatus::ok)
	{
		++g_status_counts[static_cast<std::size_t>(rec.status)];
		note_fault_and_latch();
		return result;
	}
	rec.pooled = pooled;

	std::uint64_t targetable_rhi = 0;
	std::uint64_t shader_rhi = 0;
	if (!read_u64(pooled + pool::kPooledTargetableTexture, &targetable_rhi) ||
		!read_u64(pooled + pool::kPooledShaderResourceTexture, &shader_rhi))
	{
		rec.status = pool::RecordStatus::pooled_unreadable;
		++g_status_counts[static_cast<std::size_t>(rec.status)];
		note_fault_and_latch();
		return result;
	}

	pool::RecordStatus st_t = pool::RecordStatus::rhi_null;
	pool::RecordStatus st_s = pool::RecordStatus::rhi_null;
	rec.targetable = resolve_texture(targetable_rhi, st_t);
	rec.shader_resource = shader_rhi == targetable_rhi ? rec.targetable
	                                                   : resolve_texture(shader_rhi, st_s);
	if (shader_rhi == targetable_rhi)
		st_s = st_t;
	rec.status = st_t == pool::RecordStatus::ok ? st_s : st_t;

	// THE VALIDATOR L1 USES, AND IT IS THE ONE THAT MATTERS. Our own registry sees every
	// ID3D12Resource the process creates; a pointer it has never seen is not a resource,
	// whatever the chain thought. A wrong offset yields `not-registered`, never a plausible lie.
	rec.width = 0;
	rec.height = 0;
	rec.dxgi_format = 0;
	if (rec.status == pool::RecordStatus::ok)
	{
		const std::uint64_t probe = rec.shader_resource != 0 ? rec.shader_resource : rec.targetable;
		icept::ResourceInfo info{};
		if (!resource_is_live(probe) || !describe(probe, info))
		{
			rec.status = pool::RecordStatus::not_registered;
		}
		else
		{
			rec.width = info.width;
			rec.height = info.height;
			rec.dxgi_format = info.dxgi_format;
		}
	}
	++g_status_counts[static_cast<std::size_t>(rec.status)];
	note_fault_and_latch();
	return result;
}

// ---------------------------------------------------------------------------------------
// Discovery and installation
// ---------------------------------------------------------------------------------------

void log_locate(const pool::Locate &v)
{
	char names[512] = {};
	int at = 0;
	for (std::size_t i = 0; i < pool::kNameCount; ++i)
	{
		if ((v.name_mask & (1u << i)) == 0)
			continue;
		const int wrote = std::snprintf(names + at, sizeof(names) - static_cast<std::size_t>(at),
			"%s%s", at == 0 ? "" : " ", pool::kNames[i].text);
		if (wrote <= 0 || static_cast<std::size_t>(at + wrote) >= sizeof(names))
			break;
		at += wrote;
	}

	STRAY_LOG_INFO("POOL NAMES: scan of %zu sections - literals found %u/%zu, names referenced "
		"%u, lea sites %u%s, distinct call targets %u%s. VERDICT: %s.",
		g_regions.size(), v.literals_found, pool::kNameCount, v.refs_found, v.lea_sites,
		v.lea_sites_overflowed ? " (OVERFLOWED - raise kMaxLeaSites)" : "",
		v.candidates, v.candidates_overflowed ? " (OVERFLOWED)" : "",
		pool::locate_status_text(v.status));

	if (v.target != 0)
	{
		STRAY_LOG_INFO("POOL NAMES: best candidate %#llx - reached from %u DISTINCT enclosing "
			"functions (.pdata-derived, bar is %u) and %u distinct name literals (bar %u), %u "
			"lea sites, nearest call %u bytes after the name load. Names: %s. Runner-up %#llx "
			"with %u groups / %u names / nearest %u. Tie broken by the internal call: %s. "
			"Entry bytes: "
			"%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X%s",
			static_cast<unsigned long long>(v.target), v.groups, pool::kMinAgreeingGroups,
			v.names, pool::kMinAgreeingNames, v.sites, v.min_distance, names,
			static_cast<unsigned long long>(v.runner_up), v.runner_up_groups, v.runner_up_names,
			v.runner_up_distance, v.resolved_by_internal ? "YES" : "no (not needed, or not available)",
			v.entry[0], v.entry[1], v.entry[2], v.entry[3], v.entry[4], v.entry[5], v.entry[6],
			v.entry[7], v.entry[8], v.entry[9], v.entry[10], v.entry[11], v.entry[12],
			v.entry[13], v.entry[14], v.entry[15],
			v.entry_read ? "" : " (UNREADABLE)");
	}

	// THE RESIDUAL docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md §2.5 asked to settle rather than
	// assume: whether this Shipping build still carries FindFreeElementInternal's own UE_LOG
	// format string. It costs one scan and it answers a question that otherwise needs a grep of
	// the exe on the box.
	if (v.internal_log_hits == 0)
	{
		STRAY_LOG_INFO("POOL NAMES: the string \"%%d MB, NewRT %%s %%s\" is NOT in this image, so "
			"this build strips UE_LOG in Shipping and there is no in-function anchor for "
			"FRenderTargetPool::FindFreeElementInternal. That CLOSES route 1 of the three "
			"docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md §2.5 lists for reaching "
			"FindFreeElementForRDG (the RDG path, which is u0's), and it costs this route "
			"nothing: every RR guide goes through the OUTER FindFreeElement, which is what we "
			"found.");
	}
	else
	{
		STRAY_LOG_INFO("POOL NAMES: the string \"%%d MB, NewRT %%s %%s\" IS in this image at %#llx "
			"(USE_LOGGING_IN_SHIPPING), referenced from %u distinct function(s); "
			"FindFreeElementInternal is %#llx and our FindFreeElement candidate %s call it. "
			"That is prior-art §2.5's route 1 open, and the %s is a fourth INDEPENDENT check "
			"on the candidate - RenderTargetPool.cpp:703 is FindFreeElement calling the "
			"internal as its last act.",
			static_cast<unsigned long long>(v.internal_log_literal_va), v.internal_fn_candidates,
			static_cast<unsigned long long>(v.internal_fn),
			v.internal_called_by_target ? "DOES" : "does NOT",
			v.internal_called_by_target ? "agreement" : "DISAGREEMENT");
	}
}

void run_discovery()
{
	// The game destroys and recreates its first D3D12 device at startup (facts §14), so
	// configure() runs more than once per process. The image is the same image; re-scanning it
	// would be a second multi-hundred-millisecond stall on the device-creation thread and a
	// second copy of every verdict line in the log.
	if (g_scanned)
		return;
	g_scanned = true;
	const char *why = "";
	if (!g_mapped && !map_own_module(why))
	{
		STRAY_LOG_WARN("POOL NAMES: cannot map the main module (%s); nothing is scanned and "
			"nothing is installed.", why);
		return;
	}
	// The scan walks every readable section of a ~200 MB shipping executable a few times, on
	// the thread that is creating the D3D12 device. This project has paid for a startup stall
	// once already (NR's eager Init_Ext), so the cost is MEASURED and printed rather than
	// asserted to be small.
	const auto started = std::chrono::steady_clock::now();
	g_locate = pool::locate(g_image, g_table);
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - started).count();
	g_discovered = g_locate.status == pool::LocateStatus::ok;
	STRAY_LOG_INFO("POOL NAMES: the scan took %lld ms on the device-creation thread. It runs "
		"ONCE. If that number is large enough to be felt, it is the thing to fix - not by "
		"scanning less, but by folding the passes together.", static_cast<long long>(elapsed));
	log_locate(g_locate);
	if (!g_discovered)
	{
		STRAY_LOG_WARN("POOL NAMES: DISCOVERY REFUSED (%s). Nothing is installed and Ray "
			"Reconstruction's guide route stays closed - which is the correct outcome for an "
			"answer we cannot warrant, not a bug to work around. §9's rule is that nothing is "
			"installed on a guess.",
			pool::locate_status_text(g_locate.status));
	}
}

void install()
{
	if (!g_discovered || g_hooked)
		return;
	if (g_install == nullptr)
	{
		STRAY_LOG_WARN("POOL NAMES: level 2 asked for the recorder but no trampoline installer "
			"is registered, so nothing is patched and this session behaves as level 1. That is "
			"expected under the ReShade add-on host, which does not own MinHook; the UE4SS "
			"plugin host registers one (mods/StrayDLSS/src/Host.cpp).");
		return;
	}
	void *target = reinterpret_cast<void *>(static_cast<std::uintptr_t>(g_locate.target));
	void *original = nullptr;
	if (!g_install(target, reinterpret_cast<void *>(&hk_find_free_element), &original) ||
		original == nullptr)
	{
		STRAY_LOG_ERROR("POOL NAMES: the installer REFUSED %#llx. Nothing is patched. MinHook "
			"declines a function whose first instructions it cannot relocate, which is a "
			"reason to doubt the target as much as the patcher - re-read the entry bytes on "
			"the scan line above.",
			static_cast<unsigned long long>(g_locate.target));
		return;
	}
	g_original = reinterpret_cast<FindFreeElementFn>(original);
	g_target = target;
	g_hooked = true;
	STRAY_LOG_INFO("POOL NAMES: forwarding recorder INSTALLED on FRenderTargetPool::"
		"FindFreeElement at %#llx (original trampoline %p). It forwards every call unchanged "
		"and records (name -> IPooledRenderTarget -> FRHITexture -> ID3D12Resource) for the "
		"%zu names we know; the image is byte-identical and nothing consumes the map.",
		static_cast<unsigned long long>(g_locate.target), original, pool::kNameCount);
}

void judge(Assertion &a, pool::AssertVerdict v, const char *what,
           std::uint64_t ours, std::uint64_t theirs, bool predicted, bool *logged = nullptr)
{
	switch (v)
	{
	case pool::AssertVerdict::agree: ++a.agree; break;
	case pool::AssertVerdict::disagree: ++a.disagree; break;
	default: ++a.absent; return;
	}
	bool *seen = logged != nullptr ? logged : &a.logged;
	if (v != pool::AssertVerdict::disagree || *seen)
		return;
	*seen = true;
	if (predicted)
	{
		STRAY_LOG_WARN("POOL NAMES ASSERTION: %s - the pool says %#llx and the other engine "
			"route says %#llx. These are two INDEPENDENT engine answers for one texture, so a "
			"disagreement is a finding: either the pool map is naming the wrong view's "
			"allocation (several views per frame allocate the same names) or one of the two "
			"chains is wrong. Nothing is gated on this; the counters say how often.",
			what, static_cast<unsigned long long>(ours), static_cast<unsigned long long>(theirs));
	}
	else
	{
		STRAY_LOG_INFO("POOL NAMES OBSERVATION: %s - the pool says %#llx and the bind stream "
			"says %#llx. NO PREDICTION was attached to this pair; it is measured rather than "
			"reasoned about, and the counters are the answer.",
			what, static_cast<unsigned long long>(ours), static_cast<unsigned long long>(theirs));
	}
}

} // namespace

// ---------------------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------------------

void set_installer(InstallFn install_fn, RemoveFn remove_fn)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_install = install_fn;
	g_remove = remove_fn;
}

void configure(int level)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	const pool::Level requested = pool::level_from_int(level);
	g_level = requested;
	if (requested == pool::Level::supply && !g_asked_supply)
	{
		g_asked_supply = true;
		g_level = pool::Level::observe;
		STRAY_LOG_ERROR("[STRAYDLSS] PoolNames=3 (supply) is DECLARED, NOT IMPLEMENTED, and is "
			"being run as 2 (observe). Feeding DLSS Ray Reconstruction the G-buffer this map "
			"names is a separate decision after level 2 has run clean in GAMEPLAY - the menu "
			"has no shadow, capture or planar-reflection view to disagree, and this project "
			"has been caught by that four times.");
	}
	if (g_level == pool::Level::off)
	{
		STRAY_LOG_INFO("[STRAYDLSS] PoolNames=0: the render-target-pool name hook is off. "
			"Nothing is scanned and nothing is patched.");
		return;
	}
	STRAY_LOG_INFO("[STRAYDLSS] PoolNames=%d (%s): locating FRenderTargetPool::FindFreeElement "
		"by caller-literal agreement, the way praydog/UEVR does. %s",
		level, pool::level_name(g_level),
		g_level == pool::Level::discover
			? "Level 1 SCANS AND LOGS ONLY - nothing is patched, and it cannot change a pixel."
			: "Level 2 additionally installs a FORWARDING recorder into the game's own code, "
			  "gated on the scan clearing its agreement bar.");
	run_discovery();
	if (g_level >= pool::Level::observe)
		install();
}

pool::Level level()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_level;
}

bool discovered()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_discovered;
}

bool hooked()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_hooked;
}

bool record(pool::Target t, Record &out)
{
	const std::size_t i = static_cast<std::size_t>(t);
	if (i >= pool::kNameCount)
		return false;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_records[i].seen)
		return false;
	out = g_records[i];
	return true;
}

void note_engine_frame(std::uint64_t depth_res, std::uint64_t velocity_res,
                       std::uint64_t colour_res,
                       std::uint32_t buffer_width, std::uint32_t buffer_height,
                       std::uint64_t frame)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_hooked)
		return;

	const auto &depth = g_records[static_cast<std::size_t>(pool::Target::scene_depth_z)];
	judge(g_assert_depth,
		pool::judge_resource(depth.targetable, depth.shader_resource, depth_res),
		"SceneDepthZ against L1's FPassInputs.SceneDepthTexture",
		depth.shader_resource != 0 ? depth.shader_resource : depth.targetable, depth_res, true);

	const auto &vel = g_records[static_cast<std::size_t>(pool::Target::gbuffer_velocity)];
	judge(g_assert_velocity,
		pool::judge_resource(vel.targetable, vel.shader_resource, velocity_res),
		"GBufferVelocity against L1's FPassInputs.SceneVelocityTexture",
		vel.shader_resource != 0 ? vel.shader_resource : vel.targetable, velocity_res, true);

	const auto &col = g_records[static_cast<std::size_t>(pool::Target::scene_color_deferred)];
	judge(g_assert_colour,
		pool::judge_resource(col.targetable, col.shader_resource, colour_res),
		"SceneColorDeferred against L1's FPassInputs.SceneColorTexture",
		col.shader_resource != 0 ? col.shader_resource : col.targetable, colour_res, false);

	// The extent assertion, over every name the engine allocates at the scene-buffer extent.
	// `buffer_*` is View row 132 (BufferSizeAndInvSize = SceneContext.GetBufferSizeXY()), which
	// reaches us from the engine's own CachedViewUniformShaderParameters by a route that shares
	// nothing with this one.
	for (std::size_t i = 0; i < pool::kNameCount; ++i)
	{
		const Record &r = g_records[i];
		if (!r.seen || r.status != pool::RecordStatus::ok)
			continue;
		if (!pool::extent_is_predicted(static_cast<pool::Target>(i)))
			continue;
		const pool::AssertVerdict v =
			pool::judge_extent(r.width, r.height, buffer_width, buffer_height);
		char what[128];
		std::snprintf(what, sizeof(what), "%s's extent %ux%u against View row 132's %ux%u",
			pool::kNames[i].text, r.width, r.height, buffer_width, buffer_height);
		judge(g_assert_extent, v, what, r.width, buffer_width, true, &g_extent_logged[i]);
	}

	for (Record &r : g_records)
	{
		if (r.seen)
			r.frame = frame;
	}
}

void note_taa_colour(std::uint64_t colour_res)
{
	if (colour_res == 0)
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (!g_hooked)
		return;
	const auto &col = g_records[static_cast<std::size_t>(pool::Target::scene_color_deferred)];
	judge(g_assert_colour,
		pool::judge_resource(col.targetable, col.shader_resource, colour_res),
		"SceneColorDeferred against the TAA pass's t1 (InputSceneColor)",
		col.shader_resource != 0 ? col.shader_resource : col.targetable, colour_res, false);
}

int format_report(char *buffer, std::size_t size)
{
	if (buffer == nullptr || size == 0)
		return 0;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_level == pool::Level::off)
	{
		buffer[0] = '\0';
		return 0;
	}
	int at = std::snprintf(buffer, size,
		"[pool] mode=%s discovered=%d hooked=%d target=%#llx groups=%u names=%u calls=%llu "
		"census=%zu(+%llu) unknown=%llu faults=%llu off=%d | ok=%llu notRegistered=%llu rhiNull=%llu "
		"outNull=%llu nameBad=%llu | assert: depth %llu/%llu/%llu velocity %llu/%llu/%llu "
		"extent %llu/%llu/%llu colour %llu/%llu/%llu (agree/disagree/absent) |",
		pool::level_name(g_level), g_discovered ? 1 : 0, g_hooked ? 1 : 0,
		static_cast<unsigned long long>(g_locate.target), g_locate.groups, g_locate.names,
		static_cast<unsigned long long>(g_calls), g_census_count,
		static_cast<unsigned long long>(g_census_overflow),
		static_cast<unsigned long long>(g_names_unknown),
		static_cast<unsigned long long>(g_faults), g_disabled ? 1 : 0,
		static_cast<unsigned long long>(g_status_counts[static_cast<std::size_t>(pool::RecordStatus::ok)]),
		static_cast<unsigned long long>(g_status_counts[static_cast<std::size_t>(pool::RecordStatus::not_registered)]),
		static_cast<unsigned long long>(g_status_counts[static_cast<std::size_t>(pool::RecordStatus::rhi_null)]),
		static_cast<unsigned long long>(g_status_counts[static_cast<std::size_t>(pool::RecordStatus::out_null)]),
		static_cast<unsigned long long>(g_status_counts[static_cast<std::size_t>(pool::RecordStatus::name_unreadable)]),
		static_cast<unsigned long long>(g_assert_depth.agree),
		static_cast<unsigned long long>(g_assert_depth.disagree),
		static_cast<unsigned long long>(g_assert_depth.absent),
		static_cast<unsigned long long>(g_assert_velocity.agree),
		static_cast<unsigned long long>(g_assert_velocity.disagree),
		static_cast<unsigned long long>(g_assert_velocity.absent),
		static_cast<unsigned long long>(g_assert_extent.agree),
		static_cast<unsigned long long>(g_assert_extent.disagree),
		static_cast<unsigned long long>(g_assert_extent.absent),
		static_cast<unsigned long long>(g_assert_colour.agree),
		static_cast<unsigned long long>(g_assert_colour.disagree),
		static_cast<unsigned long long>(g_assert_colour.absent));
	if (at < 0)
		return 0;

	// The RR guide set by name, because that is the whole point of the mechanism: whether
	// GBufferA-E are named, resolved and registered decides whether Ray Reconstruction has
	// guides at all.
	for (std::size_t i = 0; i < pool::kNameCount && static_cast<std::size_t>(at) + 1 < size; ++i)
	{
		const Record &r = g_records[i];
		if (!r.seen)
			continue;
		const int wrote = std::snprintf(buffer + at, size - static_cast<std::size_t>(at),
			" %s=%s(%ux%u,fmt%u)", pool::kNames[i].text, pool::record_status_text(r.status),
			r.width, r.height, r.dxgi_format);
		if (wrote <= 0 || static_cast<std::size_t>(at + wrote) >= size)
			break;
		at += wrote;
	}
	return at;
}

void log_report(const char *when)
{
	char line[2048] = {};
	if (format_report(line, sizeof(line)) > 0)
		STRAY_LOG_INFO("%s %s", when != nullptr ? when : "", line);
}

void shutdown()
{
	RemoveFn remove = nullptr;
	void *target = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (!g_hooked)
			return;
		remove = g_remove;
		target = g_target;
		g_hooked = false;
		g_original = nullptr;
		g_target = nullptr;
	}
	if (remove != nullptr && target != nullptr)
	{
		// A detour into a module that is being unloaded is an address-0 crash on the next call
		// (docs/RESEARCH-UE4SS-MIGRATION.md). Removing it is not optional.
		const bool ok = remove(target);
		STRAY_LOG_INFO("POOL NAMES: trampoline on %p %s.", target,
			ok ? "removed" : "COULD NOT BE REMOVED - the process must not unload this DLL");
	}
}

} // namespace stray_dlss::poolhook
