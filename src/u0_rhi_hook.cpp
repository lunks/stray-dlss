#include "u0_rhi_hook.hpp"

#include "backend_native/descriptor_shadow.hpp"
#include "backend_native/resource_registry.hpp"
#include "core/engine_seam.hpp"
#include "core/taa_signature.hpp"
#include "core/u0_rhi_uav.hpp"
#include "intercept/types.hpp"
#include "log.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace stray_dlss::u0hook {
namespace {

using seam::Image;
using seam::Region;

// The two thunk shapes. On x64 there is one calling convention: `this` in RCX, then RDX, R8,
// R9, then the stack — so a member function is a free function whose first parameter is the
// object. The probe shape covers every slot in [kProbeFirstSlot, kProbeLastSlot]:
//   14 RHISetShaderTexture              (this, FRHIComputeShader*, uint32, FRHITexture*)
//   15 RHISetShaderSampler              (this, FRHIComputeShader*, uint32, FRHISamplerState*)
//   16/17 RHISetUAVParameter            (this, FRHIComputeShader*, uint32, FRHIUnorderedAccessView*)
//         RHISetUAVParameter            (this, FRHIComputeShader*, uint32, FRHIUnorderedAccessView*, uint32 InitialCount)
//   18 RHISetShaderResourceViewParameter(this, FRHIComputeShader*, uint32, FRHIShaderResourceView*)
//   19 RHISetShaderUniformBuffer        (this, FRHIComputeShader*, uint32, FRHIUniformBuffer*)
// Forwarding all five is correct for every one of them: a 4-argument callee ignores the fifth
// register/stack slot, and for the 5-argument callee it is the caller's own InitialCount.
// RHISetShaderParameter (slot 20) takes SIX and is deliberately outside the window.
//
// Slots 14, 15, 18 and 19 are pinned by the layout the discovery validates: no overload pair
// precedes them except the four `{}` UAVOverlap virtuals, whose slots are REQUIRED `ret`
// predictions, and the 16/17 pair swaps only between itself. So 14 is the texture slot and 18
// the SRV slot by construction; which of 16/17 carries a UAV bind is measured.
using ProbeFn = void (*)(void *self, void *shader, std::uint32_t index, void *object, std::uint32_t extra);
using SetComputeShaderFn = void (*)(void *self, void *shader);

std::mutex g_mutex;
int g_level = 0;

// ---- the module ----
constexpr std::size_t kMaxSections = 96;
char g_section_names[kMaxSections][9];
std::vector<Region> g_regions;
Image g_image;
u0::FunctionTable g_table;
std::uint64_t g_module_base = 0;
std::uint64_t g_module_lo = 0;
std::uint64_t g_module_hi = 0;
bool g_mapped = false;

// ---- the seed ----
// The Dispatch hook's return address must resolve to the SAME function this many times running
// before discovery is attempted; a seed that keeps moving is not a seed.
constexpr unsigned kSeedAgreements = 8;
// After this many game dispatches with no stable seed, stop trying and say so.
constexpr std::uint64_t kSeedGiveUp = 4096;
std::uint64_t g_seed_fn = 0;
unsigned g_seed_streak = 0;
std::atomic<std::uint64_t> g_seed_samples{ 0 };
std::atomic<std::uint64_t> g_seed_unresolved{ 0 }; // in the module, but no .pdata entry covers it
std::atomic<std::uint64_t> g_seed_foreign{ 0 };    // outside the game module (ReShade's proxy, NGX, ...)
std::atomic<std::uint64_t> g_seed_changed{ 0 };
std::atomic<bool> g_seed_settled{ false };          // discovery ran, ok or refused
bool g_seed_foreign_logged = false;
u0::CtxDiscovery g_ctx;
double g_discovery_ms = 0.0;

// ---- the hooks ----
constexpr unsigned kPatchCount = u0::kProbeCount + 1; // the probe window plus RHISetComputeShader
void **g_patched[kPatchCount] = {};
void *g_original[kPatchCount] = {};
ProbeFn g_probe_orig[u0::kProbeCount] = {};
SetComputeShaderFn g_set_cs_orig = nullptr;
std::atomic<bool> g_hooked{ false };

// ---- per-slot census ----
struct SlotState
{
	std::atomic<std::uint64_t> calls{ 0 };
	std::atomic<std::uint64_t> null_object{ 0 };
	std::atomic<std::uint64_t> index0{ 0 };
	std::atomic<std::uint64_t> resolved{ 0 }; // index-0 UAV binds that resolved to a resource
	std::atomic<std::uint8_t> role{ static_cast<std::uint8_t>(u0::SlotRole::unknown) };
	u0::SlotCensus census; // under g_mutex, written only while unknown
};
SlotState g_slots[u0::kProbeCount];
std::atomic<std::uint64_t> g_set_cs_calls{ 0 };
std::atomic<std::uint64_t> g_dispatch_no_set_cs{ 0 }; // the silent-miss detector (replan §1.4)

// ---- the handle latches and the scan counters ----
u0::OffsetLatch g_uav_latch; // under g_mutex: where FD3D12UnorderedAccessView keeps its CPU handle
u0::OffsetLatch g_srv_latch; // under g_mutex: the same for FD3D12ShaderResourceView (a different base order)
std::atomic<std::uint64_t> g_scans{ 0 };
std::atomic<std::uint64_t> g_scan_ok{ 0 };
std::atomic<std::uint64_t> g_scan_no_hit{ 0 };
std::atomic<std::uint64_t> g_scan_ambiguous{ 0 };
std::atomic<std::uint64_t> g_scan_dead{ 0 };
std::atomic<std::uint64_t> g_scan_unreadable{ 0 };
std::atomic<std::uint64_t> g_latched_reads{ 0 };
std::atomic<std::uint64_t> g_latched_misses{ 0 }; // the latched offset held no handle of the wanted kind
std::atomic<std::uint64_t> g_guarded_reads{ 0 };
std::atomic<std::uint64_t> g_native_calls{ 0 };   // FRHITexture::GetNativeResource calls
std::atomic<std::uint64_t> g_native_refused{ 0 }; // objects refused before the call (vtable shape)
bool g_uav_latch_logged = false;
bool g_srv_latch_logged = false;
bool g_first_uav_logged = false;

// ---- faults ----
std::atomic<std::uint64_t> g_faults{ 0 };
std::atomic<std::uint64_t> g_fault_va{ 0 };
std::atomic<bool> g_disabled{ false };

// ---- the assertion ----
std::atomic<std::uint64_t> g_verdicts[static_cast<std::size_t>(u0::Verdict::count)] = {};
std::atomic<std::uint64_t> g_extent_ne{ 0 };
std::atomic<std::uint64_t> g_shader_mismatch{ 0 }; // the u0 bind's shader != the shader set on the context
std::atomic<std::uint64_t> g_assert_hook_off{ 0 };
std::atomic<std::uint64_t> g_reg_verdicts[static_cast<std::size_t>(u0::RegVerdict::count)] = {};
std::atomic<std::uint64_t> g_reg_disagree_mask{ 0 }; // OR of registers that ever disagreed
std::atomic<std::uint64_t> g_view_verdicts[static_cast<std::size_t>(u0::ViewRegVerdict::count)] = {};
std::unordered_map<std::uint64_t, bool> g_disagree_logged;      // under g_mutex, per pass
std::unordered_map<std::uint64_t, bool> g_reg_disagree_logged;  // under g_mutex, per pass
std::unordered_map<std::uint64_t, bool> g_view_disagree_logged; // under g_mutex, per pass
bool g_first_agree_logged = false;
bool g_first_regs_logged = false;
int g_no_bind_logged = 0;
constexpr int kNoBindLogLimit = 3;

// ---- per-thread correlation ----
// The RHI thread executes RHISetComputeShader, then the binds, then RHIDispatchComputeShader,
// whose `CommandListHandle->Dispatch` is our hook — all on ONE thread for one context. So the
// bracket is thread-local and is CLOSED by the dispatch that follows it; a claim reads the
// bracket the dispatch closed, never binds from a different dispatch.
struct RegBind
{
	std::uint64_t object = 0; // the engine's RHI object at this register — identity, resolved at the claim
	unsigned slot = 0;        // which vtable slot delivered it (14 texture, 18 SRV)
};
struct UavBind
{
	bool present = false;
	std::uint64_t object = 0;   // the FRHIUnorderedAccessView* — identity only after the call
	std::uint64_t resource = 0; // the ID3D12Resource* the shadow recorded for its handle
	u0::ScanStatus status = u0::ScanStatus::unreadable;
	unsigned slot = 0;
	void *shader = nullptr;     // the FRHIComputeShader* the bind was made against
};
struct Bracket
{
	void *ctx = nullptr;
	void *shader = nullptr;   // from RHISetComputeShader; null if none preceded the binds
	bool set_cs_seen = false;
	RegBind t[u0::kMaxTexRegs];
	UavBind u[u0::kMaxUavRegs];
	std::uint32_t ub_mask = 0; // `b` registers a uniform buffer was bound at
	unsigned ub_count = 0;     // RHISetShaderUniformBuffer calls in the bracket
	unsigned binds = 0;
};
struct Tls
{
	Bracket pending;
	Bracket for_dispatch;
};
thread_local Tls g_tls;

// ---------------------------------------------------------------------------------------
// Mapping the module (the seam's map_own_module, plus the exception directory)
// ---------------------------------------------------------------------------------------

bool map_module(const char *&why)
{
	HMODULE module = GetModuleHandleW(nullptr);
	if (module == nullptr)
	{
		why = "GetModuleHandleW(nullptr) returned nothing";
		return false;
	}
	auto *const base = reinterpret_cast<const unsigned char *>(module);
	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		why = "no MZ header";
		return false;
	}
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
	{
		why = "not a PE32+ x86-64 image";
		return false;
	}
	g_regions.clear();
	const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
	const unsigned count = nt->FileHeader.NumberOfSections;
	for (unsigned i = 0; i < count; ++i, ++section)
	{
		const DWORD size = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize : section->SizeOfRawData;
		if (size == 0 || section->VirtualAddress == 0)
			continue;
		if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0)
			continue;
		if ((section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE) != 0)
			continue;
		Region r;
		r.va = reinterpret_cast<std::uint64_t>(base + section->VirtualAddress);
		r.bytes = base + section->VirtualAddress;
		r.size = size;
		r.executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
		if (i < kMaxSections)
		{
			std::memcpy(g_section_names[i], section->Name, 8);
			g_section_names[i][8] = '\0';
			r.name = g_section_names[i];
		}
		g_regions.push_back(r);
	}
	if (g_regions.empty())
	{
		why = "no readable sections";
		return false;
	}
	g_module_base = reinterpret_cast<std::uint64_t>(base);
	g_module_lo = 0;
	g_module_hi = 0;
	for (const Region &r : g_regions)
	{
		if (g_module_lo == 0 || r.va < g_module_lo)
			g_module_lo = r.va;
		if (r.va + r.size > g_module_hi)
			g_module_hi = r.va + r.size;
	}
	g_image.regions = g_regions.data();
	g_image.count = g_regions.size();

	// The exception directory: IMAGE_RUNTIME_FUNCTION_ENTRY[], sorted by BeginAddress. Every
	// x64 image that can be unwound has one; a Shipping build cannot strip it.
	const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
	if (dir.VirtualAddress == 0 || dir.Size < sizeof(u0::RuntimeFunction))
	{
		why = "the image has no exception directory (.pdata)";
		return false;
	}
	static_assert(sizeof(u0::RuntimeFunction) == 12, "RUNTIME_FUNCTION is three RVAs");
	g_table.entries = reinterpret_cast<const u0::RuntimeFunction *>(base + dir.VirtualAddress);
	g_table.count = dir.Size / sizeof(u0::RuntimeFunction);
	g_table.image_base = g_module_base;
	g_table.image = &g_image;
	g_mapped = true;
	return true;
}

bool in_module(std::uint64_t va)
{
	return va >= g_module_lo && va < g_module_hi;
}

// ---------------------------------------------------------------------------------------
// The guarded reader (L1's, verbatim in spirit)
// ---------------------------------------------------------------------------------------

bool committed_and_readable(std::uint64_t va, std::size_t n)
{
	MEMORY_BASIC_INFORMATION mbi{};
	const SIZE_T got = VirtualQuery(reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(va)), &mbi, sizeof(mbi));
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

// EXPLICIT AND DELIBERATE: this dereferences engine memory at an offset that was scanned for
// or latched, never proven. MSVC only; the mingw lane relies on the VirtualQuery guard.
bool read_u64_guarded(std::uint64_t va, std::uint64_t *out)
{
#if defined(_MSC_VER)
	__try
	{
		std::memcpy(out, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(va)), sizeof(*out));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
#else
	std::memcpy(out, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(va)), sizeof(*out));
	return true;
#endif
}

using GetNativeResourceFn = void *(*)(void *rhi);

// The one CALL into engine code on this path — FRHITexture::GetNativeResource, the slot L1
// already proved on this exe — under the same SEH as the reads and for the same reason.
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

bool plausible_heap_ptr(std::uint64_t va)
{
	if (va == 0 || (va & 7ull) != 0)
		return false;
	return va >= 0x10000ull && va < 0x0000800000000000ull;
}

bool guarded_read(void *, std::uint64_t va, std::uint64_t *out)
{
	if (!plausible_heap_ptr(va) && !in_module(va))
		return false;
	if (!committed_and_readable(va, sizeof(std::uint64_t)))
		return false;
	g_guarded_reads.fetch_add(1, std::memory_order_relaxed);
	if (!read_u64_guarded(va, out))
	{
		g_faults.fetch_add(1, std::memory_order_relaxed);
		g_fault_va.store(va, std::memory_order_relaxed);
		return false;
	}
	return true;
}

// The lookup: a 64-bit value is a CPU descriptor handle iff the descriptor shadow recorded a
// view at it. The shadow keys by the REAL D3D12_CPU_DESCRIPTOR_HANDLE.ptr the game passed to
// Create*View, so this is engine bytes against our own record of a real D3D12 call.
bool shadow_probe(void *, std::uint64_t value, u0::HandleKind *kind, std::uint64_t *resource, bool *dead)
{
	native::shadow::ViewEntry e;
	if (!native::shadow::lookup(static_cast<icept::DescriptorId>(value), e))
		return false;
	if (e.is_null)
		return false;
	switch (e.kind)
	{
	case native::shadow::ViewKind::uav: *kind = u0::HandleKind::uav; break;
	case native::shadow::ViewKind::srv: *kind = u0::HandleKind::srv; break;
	default: *kind = u0::HandleKind::other; break;
	}
	*resource = e.resource;
	*dead = e.dead;
	return true;
}

u0::ObjectReader reader() { return u0::ObjectReader{ &guarded_read, nullptr }; }
u0::HandleLookup lookup() { return u0::HandleLookup{ &shadow_probe, nullptr }; }

void note_faults_and_latch()
{
	if (g_faults.load(std::memory_order_relaxed) == 0)
		return;
	if (g_disabled.exchange(true, std::memory_order_acq_rel))
		return;
	STRAY_LOG_ERROR("U0 HOOK DISABLED: a guarded read of, or call into, an RHI object FAULTED at %#llx. The "
		"guards caught it (the game is alive), but a fault means a scan window, a latched offset "
		"or the GetNativeResource slot reached memory that is not the object's, so the resolve is "
		"off for the rest of the session. The thunks stay installed and keep forwarding; every "
		"bind is still counted. Paste this line with the [u0] line that follows it.",
		static_cast<unsigned long long>(g_fault_va.load(std::memory_order_relaxed)));
}

void count_scan(const u0::ScanResult &r)
{
	g_scans.fetch_add(1, std::memory_order_relaxed);
	switch (r.status)
	{
	case u0::ScanStatus::ok: g_scan_ok.fetch_add(1, std::memory_order_relaxed); break;
	case u0::ScanStatus::no_hit: g_scan_no_hit.fetch_add(1, std::memory_order_relaxed); break;
	case u0::ScanStatus::ambiguous: g_scan_ambiguous.fetch_add(1, std::memory_order_relaxed); break;
	case u0::ScanStatus::dead: g_scan_dead.fetch_add(1, std::memory_order_relaxed); break;
	default: g_scan_unreadable.fetch_add(1, std::memory_order_relaxed); break;
	}
}

// FRHITexture* -> ID3D12Resource*, the way L1 does it minus the FRDGTexture hop: vptr, then
// slot seam::kRhiGetNativeResourceSlot, then the call. Refused before the call unless the
// vptr is in the module and its first eight slots are all code — an FRHISamplerState's vtable
// has one entry and an FRHIUniformBuffer's three, so an object that is not a texture cannot
// pass. The ANSWER is then checked against the registry by the caller.
std::uint64_t resolve_rhi_texture(std::uint64_t rhi)
{
	std::uint64_t vptr = 0;
	if (!guarded_read(nullptr, rhi, &vptr) || !in_module(vptr))
	{
		g_native_refused.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	std::uint64_t fn = 0;
	for (unsigned s = 0; s <= seam::kRhiGetNativeResourceSlot; ++s)
	{
		std::uint64_t entry = 0;
		if (!guarded_read(nullptr, vptr + 8ull * s, &entry) || !g_image.is_code(entry))
		{
			g_native_refused.fetch_add(1, std::memory_order_relaxed);
			return 0;
		}
		if (s == seam::kRhiGetNativeResourceSlot)
			fn = entry;
	}
	void *native = nullptr;
	g_native_calls.fetch_add(1, std::memory_order_relaxed);
	if (!call_native_resource_guarded(fn, rhi, &native))
	{
		g_faults.fetch_add(1, std::memory_order_relaxed);
		g_fault_va.store(rhi, std::memory_order_relaxed);
		return 0;
	}
	return reinterpret_cast<std::uint64_t>(native);
}

// The SRV twin of the UAV cross-match: scan (until latched) or read at the latched offset.
std::uint64_t resolve_srv_object(std::uint64_t object)
{
	bool latched = false;
	unsigned offset = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		latched = g_srv_latch.latched();
		offset = g_srv_latch.offset();
	}
	u0::ScanResult r;
	if (latched)
	{
		r = u0::read_handle_at(reader(), lookup(), object, offset, u0::HandleKind::srv);
		g_latched_reads.fetch_add(1, std::memory_order_relaxed);
		if (r.status != u0::ScanStatus::ok)
			g_latched_misses.fetch_add(1, std::memory_order_relaxed);
	}
	else
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		r = u0::scan_object_for_handle(reader(), lookup(), object);
		count_scan(r);
		if (r.status == u0::ScanStatus::ok && r.kind == u0::HandleKind::srv &&
			g_srv_latch.observe(r.offset) && !g_srv_latch_logged)
		{
			g_srv_latch_logged = true;
			STRAY_LOG_INFO("U0 HOOK: the SRV object's CPU descriptor handle sits at +%u, latched after %u "
				"agreeing scans (FD3D12ShaderResourceView puts FD3D12View behind two bases, so this "
				"differs from the UAV's offset by design).", r.offset, u0::kLatchAgreements);
		}
	}
	return r.status == u0::ScanStatus::ok && r.kind == u0::HandleKind::srv ? r.resource : 0;
}

// ---------------------------------------------------------------------------------------
// The thunks
// ---------------------------------------------------------------------------------------

void on_probe(unsigned slot, void *self, void *shader, std::uint32_t index, void *object)
{
	SlotState &s = g_slots[slot - u0::kProbeFirstSlot];
	s.calls.fetch_add(1, std::memory_order_relaxed);
	if (object == nullptr)
	{
		// UnsetShaderUAVs after every dispatch (ShaderParameterStruct.h:177): the unbind. Never
		// paired with anything.
		s.null_object.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	if (index == 0)
		s.index0.fetch_add(1, std::memory_order_relaxed);

	Bracket &br = g_tls.pending;
	br.ctx = self;
	++br.binds;
	const std::uint64_t obj = reinterpret_cast<std::uint64_t>(object);

	// The pinned slots: record the IDENTITY per register and resolve at the claim, when only
	// the engine-announced dispatch pays for it and every object is provably alive.
	if (slot == u0::kSlotSetShaderTexture || slot == u0::kSlotSetShaderResourceView)
	{
		if (index < u0::kMaxTexRegs)
		{
			br.t[index].object = obj;
			br.t[index].slot = slot;
		}
		return;
	}
	if (slot == u0::kSlotSetShaderUniformBuffer)
	{
		++br.ub_count;
		if (index < u0::kMaxCbRegs)
			br.ub_mask |= 1u << index;
		return;
	}
	if (slot == u0::kSlotSetShaderSampler)
		return;

	// 16 / 17: the UAV pair. The cross-match is what classifies them, so it runs here.
	if (g_disabled.load(std::memory_order_acquire))
		return;
	auto role = static_cast<u0::SlotRole>(s.role.load(std::memory_order_acquire));
	u0::ScanResult r;
	bool resolved_this_call = false;

	if (role == u0::SlotRole::unknown)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (s.census.classified < u0::kClassifySamples)
		{
			r = u0::scan_object_for_handle(reader(), lookup(), obj);
			count_scan(r);
			++s.census.classified;
			if (r.status == u0::ScanStatus::ambiguous)
				++s.census.ambiguous;
			else if (r.kind == u0::HandleKind::uav && r.hits == 1)
				++s.census.uav_hits;
			else if (r.kind == u0::HandleKind::srv && r.hits == 1)
				++s.census.srv_hits;
			else
				++s.census.no_hits;
			const u0::SlotRole now = u0::classify_slot(s.census);
			if (now != u0::SlotRole::unknown)
			{
				s.role.store(static_cast<std::uint8_t>(now), std::memory_order_release);
				STRAY_LOG_INFO("U0 HOOK: vtable slot %u classified as %s after %llu objects "
					"(uav=%llu srv=%llu none=%llu ambiguous=%llu). Slots 16 and 17 are the two "
					"RHISetUAVParameter overloads in an order MSVC decides (reversed from the "
					"header); which one the TAA pass uses is measured, not counted.",
					slot, u0::slot_role_name(now),
					static_cast<unsigned long long>(s.census.classified),
					static_cast<unsigned long long>(s.census.uav_hits),
					static_cast<unsigned long long>(s.census.srv_hits),
					static_cast<unsigned long long>(s.census.no_hits),
					static_cast<unsigned long long>(s.census.ambiguous));
			}
			if (r.status == u0::ScanStatus::ok && r.kind == u0::HandleKind::uav)
			{
				if (g_uav_latch.observe(r.offset) && !g_uav_latch_logged)
				{
					g_uav_latch_logged = true;
					STRAY_LOG_INFO("U0 HOOK: the UAV object's CPU descriptor handle sits at +%u, "
						"latched after %u agreeing scans. [derived] expectation was +40 "
						"(FRHIUnorderedAccessView 24 bytes, then FD3D12View's vptr, then "
						"TD3D12ViewDescriptorHandle{Parent, Handle}) - %s. From here the "
						"per-call path reads ONE qword at that offset and asks the shadow.",
						r.offset, u0::kLatchAgreements,
						r.offset == 40 ? "MATCHES" : "DIFFERS; the layout on this exe is not the header's");
				}
				resolved_this_call = true;
			}
		}
	}
	else if (role == u0::SlotRole::uav)
	{
		bool latched = false;
		unsigned offset = 0;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			latched = g_uav_latch.latched();
			offset = g_uav_latch.offset();
		}
		if (latched)
		{
			r = u0::read_handle_at(reader(), lookup(), obj, offset, u0::HandleKind::uav);
			g_latched_reads.fetch_add(1, std::memory_order_relaxed);
			if (r.status != u0::ScanStatus::ok)
				g_latched_misses.fetch_add(1, std::memory_order_relaxed);
		}
		else
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			r = u0::scan_object_for_handle(reader(), lookup(), obj);
			count_scan(r);
			if (r.status == u0::ScanStatus::ok && r.kind == u0::HandleKind::uav)
				g_uav_latch.observe(r.offset);
		}
		resolved_this_call = true;
	}
	else
	{
		return;
	}
	note_faults_and_latch();
	if (!resolved_this_call || index >= u0::kMaxUavRegs)
		return;

	// The bind for the dispatch that follows on this thread. Carried as an ID3D12Resource*
	// (refcounted, long-lived, checked against the registry at the claim); the
	// FRHIUnorderedAccessView* is kept as an identity for the log and never dereferenced again.
	UavBind &b = br.u[index];
	b.present = true;
	b.object = obj;
	b.resource = r.status == u0::ScanStatus::ok ? r.resource : 0;
	b.status = r.status;
	b.slot = slot;
	b.shader = shader;
	if (index == 0 && r.status == u0::ScanStatus::ok)
	{
		s.resolved.fetch_add(1, std::memory_order_relaxed);
		bool first = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			if (!g_first_uav_logged)
			{
				g_first_uav_logged = true;
				first = true;
			}
		}
		if (first)
			STRAY_LOG_INFO("U0 HOOK: first UAVIndex==0 bind resolved - slot %u, FRHIUnorderedAccessView %p "
				"holds CPU handle %#llx at +%u, which our shadow recorded from "
				"ID3D12Device::CreateUnorderedAccessView over resource %p (shader %p, context %p, "
				"thread %lu). Two bookkeepers agreeing on one 64-bit value; nothing derived.",
				slot, object, static_cast<unsigned long long>(r.handle), r.offset,
				reinterpret_cast<void *>(r.resource), shader, self, GetCurrentThreadId());
	}
}

template <unsigned Slot>
void probe_thunk(void *self, void *shader, std::uint32_t index, void *object, std::uint32_t extra)
{
	on_probe(Slot, self, shader, index, object);
	ProbeFn original = g_probe_orig[Slot - u0::kProbeFirstSlot];
	if (original != nullptr)
		original(self, shader, index, object, extra);
}

void set_compute_shader_thunk(void *self, void *shader)
{
	g_set_cs_calls.fetch_add(1, std::memory_order_relaxed);
	// A new bracket begins (RenderGraphUtils.h:247: SetComputeShader is the first call of
	// FComputeShaderUtils::Dispatch). Whatever was pending belonged to no dispatch.
	Bracket &br = g_tls.pending;
	br = Bracket{};
	br.ctx = self;
	br.shader = shader;
	br.set_cs_seen = true;
	SetComputeShaderFn original = g_set_cs_orig;
	if (original != nullptr)
		original(self, shader);
}

// The six probe thunks, one per slot, so each call knows which slot it came through.
ProbeFn probe_for_slot(unsigned slot)
{
	switch (slot)
	{
	case u0::kProbeFirstSlot + 0: return &probe_thunk<u0::kProbeFirstSlot + 0>;
	case u0::kProbeFirstSlot + 1: return &probe_thunk<u0::kProbeFirstSlot + 1>;
	case u0::kProbeFirstSlot + 2: return &probe_thunk<u0::kProbeFirstSlot + 2>;
	case u0::kProbeFirstSlot + 3: return &probe_thunk<u0::kProbeFirstSlot + 3>;
	case u0::kProbeFirstSlot + 4: return &probe_thunk<u0::kProbeFirstSlot + 4>;
	case u0::kProbeFirstSlot + 5: return &probe_thunk<u0::kProbeFirstSlot + 5>;
	default: return nullptr;
	}
}
static_assert(u0::kProbeCount == 6, "probe_for_slot enumerates exactly six thunks");

// ---------------------------------------------------------------------------------------
// Discovery and installation
// ---------------------------------------------------------------------------------------

bool patch_slot(void **slot, void *replacement, void *&original_out)
{
	DWORD old = 0;
	if (VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old) == 0)
		return false;
	original_out = *slot;
	*slot = replacement;
	DWORD ignored = 0;
	VirtualProtect(slot, sizeof(void *), old, &ignored);
	return true;
}

void install_hooks()
{
	// Under g_mutex.
	auto **base = reinterpret_cast<void **>(static_cast<std::uintptr_t>(g_ctx.vtable_va));
	unsigned installed = 0;
	for (unsigned i = 0; i < u0::kProbeCount; ++i)
	{
		const unsigned slot = u0::kProbeFirstSlot + i;
		void **where = base + slot;
		void *original = nullptr;
		if (!patch_slot(where, reinterpret_cast<void *>(probe_for_slot(slot)), original))
		{
			STRAY_LOG_ERROR("U0 HOOK: VirtualProtect on vtable slot %u at %p failed (%lu); not installed.",
				slot, static_cast<void *>(where), GetLastError());
			continue;
		}
		g_patched[i] = where;
		g_original[i] = original;
		g_probe_orig[i] = reinterpret_cast<ProbeFn>(original);
		++installed;
	}
	{
		void **where = base + u0::kSlotSetComputeShader;
		void *original = nullptr;
		if (patch_slot(where, reinterpret_cast<void *>(&set_compute_shader_thunk), original))
		{
			g_patched[u0::kProbeCount] = where;
			g_original[u0::kProbeCount] = original;
			g_set_cs_orig = reinterpret_cast<SetComputeShaderFn>(original);
			++installed;
		}
		else
			STRAY_LOG_ERROR("U0 HOOK: VirtualProtect on the RHISetComputeShader slot at %p failed (%lu).",
				static_cast<void *>(where), GetLastError());
	}
	if (installed == 0)
		return;
	g_hooked.store(true, std::memory_order_release);
	STRAY_LOG_INFO("U0 HOOK INSTALLED: %u of %u slots of the FD3D12CommandContext vtable at %#llx now "
		"point at forwarding thunks (RHISetComputeShader at slot %u, and slots %u..%u: texture, "
		"sampler, the two RHISetUAVParameter overloads, SRV, uniform buffer). Every call is "
		"forwarded with all its arguments, so the image is unchanged; the thunks record each "
		"bracket's registers on its own thread and the engine-announced claim asserts them against "
		"the descriptor walk. MODE=OBSERVE: the walk still names everything DLSS uses; this is an "
		"oracle beside it.",
		installed, kPatchCount, static_cast<unsigned long long>(g_ctx.vtable_va),
		u0::kSlotSetComputeShader, u0::kProbeFirstSlot, u0::kProbeLastSlot);
}

void run_discovery()
{
	// Under g_mutex, on the thread that recorded the eighth agreeing dispatch.
	LARGE_INTEGER freq{};
	LARGE_INTEGER t0{};
	LARGE_INTEGER t1{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);
	g_ctx = u0::discover_context_vtable(g_image, g_seed_fn);
	QueryPerformanceCounter(&t1);
	g_discovery_ms = freq.QuadPart != 0
		? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(freq.QuadPart)
		: 0.0;
	g_seed_settled.store(true, std::memory_order_release);

	if (g_ctx.status != u0::CtxStatus::ok)
	{
		STRAY_LOG_WARN("U0 HOOK: FD3D12CommandContext vtable NOT FOUND - %s (seed=%#llx from %u agreeing "
			"Dispatch return addresses, qwordHits=%u survivors=%u failedSlot=%u, %.1f ms). Nothing is "
			"installed; the descriptor walk is unaffected. The seed is the start of the function "
			"that called ID3D12GraphicsCommandList::Dispatch, resolved through the exe's own .pdata.",
			u0::ctx_status_text(g_ctx.status), static_cast<unsigned long long>(g_seed_fn),
			kSeedAgreements, g_ctx.qword_hits, g_ctx.survivors, g_ctx.failed_slot, g_discovery_ms);
		return;
	}

	char held[u0::kSlotExpectationCount * 8 + 1] = {};
	char missed[u0::kSlotExpectationCount * 8 + 1] = {};
	std::size_t held_n = 0;
	std::size_t missed_n = 0;
	for (std::size_t k = 0; k < u0::kSlotExpectationCount; ++k)
	{
		const bool ok = (g_ctx.expectation_mask & (1u << k)) != 0;
		char *dst = ok ? held : missed;
		std::size_t &n = ok ? held_n : missed_n;
		const std::size_t cap = sizeof(held);
		if (n < cap)
		{
			const int w = std::snprintf(dst + n, cap - n, " %u", u0::kSlotExpectations[k].slot);
			if (w > 0)
				n += static_cast<std::size_t>(w);
		}
	}
	STRAY_LOG_INFO("U0 HOOK FOUND: FD3D12CommandContext vtable at %#llx (%.1f ms). The seed - "
		"FD3D12CommandContext::RHIDispatchComputeShader at %#llx, the function our Dispatch hook's "
		"return address lies in - sits at slot %u, and the candidate passed every required "
		"prediction: %u slots all inside the game's code, and the six virtuals whose only body on "
		"this class is empty (SetAsyncComputeBudget, the four UAVOverlap, InvalidateCachedState) "
		"each begin with `ret`. Predictions held at slots:%s; not held (reported, never gated):%s. "
		"ICF: %u of the 6 empty bodies share one address. qwordHits=%u survivors=%u.",
		static_cast<unsigned long long>(g_ctx.vtable_va), g_discovery_ms,
		static_cast<unsigned long long>(g_seed_fn), u0::kSlotDispatchComputeShader,
		u0::kSlotsChecked, held, missed[0] ? missed : " (none)", g_ctx.ret_fold,
		g_ctx.qword_hits, g_ctx.survivors);
	STRAY_LOG_INFO("U0 HOOK slots: SetComputeShader=%#llx | 14 SetShaderTexture=%#llx 15 SetShaderSampler=%#llx "
		"16=%#llx 17=%#llx (the two RHISetUAVParameter overloads, order MEASURED below) 18 "
		"SetShaderResourceView=%#llx 19 SetShaderUniformBuffer=%#llx | GetNativeCommandBuffer=%#llx",
		static_cast<unsigned long long>(g_ctx.slot[u0::kSlotSetComputeShader]),
		static_cast<unsigned long long>(g_ctx.slot[14]), static_cast<unsigned long long>(g_ctx.slot[15]),
		static_cast<unsigned long long>(g_ctx.slot[16]), static_cast<unsigned long long>(g_ctx.slot[17]),
		static_cast<unsigned long long>(g_ctx.slot[18]), static_cast<unsigned long long>(g_ctx.slot[19]),
		static_cast<unsigned long long>(g_ctx.slot[u0::kSlotGetNativeCommandBuffer]));

	if (g_level < 2)
	{
		STRAY_LOG_INFO("U0 HOOK: U0Hook=1, so nothing is installed. Set U0Hook=2 to install the "
			"forwarding thunks and assert the engine's bind stream against the descriptor walk.");
		return;
	}
	install_hooks();
}

void log_foreign_seed_once(const void *ret)
{
	if (g_seed_foreign_logged)
		return;
	g_seed_foreign_logged = true;
	char name[MAX_PATH] = "?";
	HMODULE owner = nullptr;
	if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(ret), &owner) && owner != nullptr)
		GetModuleFileNameA(owner, name, sizeof(name));
	STRAY_LOG_WARN("U0 HOOK: a game dispatch reached our Dispatch hook from OUTSIDE the game module "
		"(return address %p, in %s). Under a proxied command list (ReShade as dxgi.dll, Config B) "
		"every dispatch does, and the seed cannot be taken from the return address; discovery will "
		"refuse if no in-module seed ever settles. Once per session.", ret, name);
}

} // namespace

// ---------------------------------------------------------------------------------------
// The API
// ---------------------------------------------------------------------------------------

void configure(int level)
{
	if (level >= 3)
	{
		STRAY_LOG_WARN("U0 HOOK: U0Hook=%d requested. Level 3 (authoritative: the engine's bind stream "
			"replaces the descriptor walk and the View-CB search) is DECLARED, NOT IMPLEMENTED - it "
			"is a separate decision after the level-2 assertion has run clean across gameplay. "
			"Treating as 2.", level);
		level = 2;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_level = level;
	}
	if (level <= 0)
	{
		STRAY_LOG_INFO("U0 HOOK MODE: off ([STRAYDLSS] U0Hook=0). The descriptor walk names u0 alone.");
		return;
	}
	const char *why = "";
	bool mapped = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		mapped = map_module(why);
	}
	if (!mapped)
	{
		STRAY_LOG_WARN("U0 HOOK: cannot map the game module - %s. Staying off.", why);
		std::lock_guard<std::mutex> lock(g_mutex);
		g_level = 0;
		return;
	}
	STRAY_LOG_INFO("U0 HOOK MODE: %s ([STRAYDLSS] U0Hook=%d). Waiting for %u agreeing Dispatch return "
		"addresses to seed the FD3D12CommandContext vtable search (module base %#llx, %zu sections, "
		".pdata entries=%zu). Nothing is installed until the vtable is found and validated%s.",
		level >= 2 ? "observe" : "discover", level, kSeedAgreements,
		static_cast<unsigned long long>(g_module_base), g_regions.size(), g_table.count,
		level >= 2 ? "; then the forwarding thunks go in" : ", and at level 1 not even then");
}

int level()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_level;
}

bool discovered()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_ctx.status == u0::CtxStatus::ok;
}

bool hooked() { return g_hooked.load(std::memory_order_acquire); }

void note_dispatch(const void *return_address)
{
	// Close this thread's bracket FIRST and unconditionally: the dispatch that follows the
	// binds is this one, whatever else happens below.
	Tls &t = g_tls;
	t.for_dispatch = t.pending;
	t.pending = Bracket{};
	if (g_hooked.load(std::memory_order_acquire) && !t.for_dispatch.set_cs_seen)
		g_dispatch_no_set_cs.fetch_add(1, std::memory_order_relaxed);

	if (g_seed_settled.load(std::memory_order_acquire))
		return;
	int lvl = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		lvl = g_level;
	}
	if (lvl <= 0 || !g_mapped)
		return;

	const std::uint64_t samples = g_seed_samples.fetch_add(1, std::memory_order_relaxed) + 1;
	const std::uint64_t ret = reinterpret_cast<std::uint64_t>(return_address);
	std::uint64_t fn = 0;
	if (!in_module(ret))
	{
		g_seed_foreign.fetch_add(1, std::memory_order_relaxed);
		std::lock_guard<std::mutex> lock(g_mutex);
		log_foreign_seed_once(return_address);
	}
	else
	{
		fn = u0::function_start(g_table, ret);
		if (fn == 0)
			g_seed_unresolved.fetch_add(1, std::memory_order_relaxed);
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_seed_settled.load(std::memory_order_relaxed))
		return;
	if (fn != 0)
	{
		if (fn == g_seed_fn)
			++g_seed_streak;
		else
		{
			if (g_seed_fn != 0)
				g_seed_changed.fetch_add(1, std::memory_order_relaxed);
			g_seed_fn = fn;
			g_seed_streak = 1;
		}
		if (g_seed_streak >= kSeedAgreements)
		{
			run_discovery();
			return;
		}
	}
	if (samples >= kSeedGiveUp)
	{
		g_seed_settled.store(true, std::memory_order_release);
		g_ctx.status = u0::CtxStatus::no_seed;
		STRAY_LOG_WARN("U0 HOOK: no stable seed after %llu game dispatches (foreign=%llu unresolved=%llu "
			"changed=%llu, last fn=%#llx streak=%u). Giving up for the session; nothing installed.",
			static_cast<unsigned long long>(samples),
			static_cast<unsigned long long>(g_seed_foreign.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_seed_unresolved.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_seed_changed.load(std::memory_order_relaxed)),
			static_cast<unsigned long long>(g_seed_fn), g_seed_streak);
	}
}

void assert_at_claim(const WalkAnswer &walk, std::uint32_t out_width, std::uint32_t out_height,
                     std::uint64_t pass_hash)
{
	if (!g_hooked.load(std::memory_order_acquire))
	{
		g_assert_hook_off.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const Bracket &br = g_tls.for_dispatch;
	const bool disabled = g_disabled.load(std::memory_order_acquire);

	// ---- u0 ----
	const UavBind &b = br.u[0];
	u0::DescFacts d;
	if (b.present && b.status == u0::ScanStatus::ok && b.resource != 0)
	{
		d.live = native::registry::is_live(static_cast<icept::ResourceId>(b.resource));
		icept::ResourceInfo info;
		if (d.live && native::registry::describe(static_cast<icept::ResourceId>(b.resource), info))
		{
			d.is_buffer = info.is_buffer;
			d.is_3d = info.is_3d;
			d.allow_uav = info.allow_uav;
			d.hdr_colour = is_hdr_colour(info.format);
			d.width = info.width;
			d.height = info.height;
		}
	}
	const u0::Judgement j = u0::judge(b.present, b.status, b.resource, walk.u0, d, out_width, out_height);
	g_verdicts[static_cast<std::size_t>(j.verdict)].fetch_add(1, std::memory_order_relaxed);
	if ((j.verdict == u0::Verdict::agree || j.verdict == u0::Verdict::disagree) && !j.extent_equal)
		g_extent_ne.fetch_add(1, std::memory_order_relaxed);
	if (b.present && b.shader != nullptr && br.shader != nullptr && b.shader != br.shader)
		g_shader_mismatch.fetch_add(1, std::memory_order_relaxed);

	// ---- t0..t7: resolve the engine's objects now, while they are alive ----
	std::uint64_t engine_t[u0::kMaxTexRegs] = {};
	u0::RegVerdict tv[u0::kMaxTexRegs] = {};
	std::uint32_t disagree_mask = 0;
	for (unsigned r = 0; r < u0::kMaxTexRegs; ++r)
	{
		const RegBind &rb = br.t[r];
		if (rb.object != 0 && !disabled)
		{
			if (rb.slot == u0::kSlotSetShaderTexture)
				engine_t[r] = resolve_rhi_texture(rb.object);
			else if (rb.slot == u0::kSlotSetShaderResourceView)
				engine_t[r] = resolve_srv_object(rb.object);
			if (engine_t[r] != 0 &&
				!native::registry::is_live(static_cast<icept::ResourceId>(engine_t[r])))
				engine_t[r] = 0; // a pointer nothing knows is not an answer
		}
		tv[r] = u0::judge_register(rb.object != 0, engine_t[r], walk.t[r]);
		g_reg_verdicts[static_cast<std::size_t>(tv[r])].fetch_add(1, std::memory_order_relaxed);
		if (tv[r] == u0::RegVerdict::disagree)
			disagree_mask |= 1u << r;
	}
	if (disagree_mask != 0)
		g_reg_disagree_mask.fetch_or(disagree_mask, std::memory_order_relaxed);
	note_faults_and_latch();

	// ---- the View constant buffer's register ----
	const u0::ViewRegVerdict vv = u0::judge_view_register(br.ub_mask, walk.view_cb_valid, walk.view_cb_register);
	g_view_verdicts[static_cast<std::size_t>(vv)].fetch_add(1, std::memory_order_relaxed);

	bool log_agree = false;
	bool log_disagree = false;
	bool log_no_bind = false;
	bool log_regs = false;
	bool log_reg_disagree = false;
	bool log_view_disagree = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (j.verdict == u0::Verdict::agree && !g_first_agree_logged)
		{
			g_first_agree_logged = true;
			log_agree = true;
		}
		if (j.verdict == u0::Verdict::disagree && !g_disagree_logged[pass_hash])
		{
			g_disagree_logged[pass_hash] = true;
			log_disagree = true;
		}
		if (j.verdict == u0::Verdict::no_bind && g_no_bind_logged < kNoBindLogLimit)
		{
			++g_no_bind_logged;
			log_no_bind = true;
		}
		if (!g_first_regs_logged && br.binds != 0)
		{
			g_first_regs_logged = true;
			log_regs = true;
		}
		if (disagree_mask != 0 && !g_reg_disagree_logged[pass_hash])
		{
			g_reg_disagree_logged[pass_hash] = true;
			log_reg_disagree = true;
		}
		if (vv == u0::ViewRegVerdict::disagree && !g_view_disagree_logged[pass_hash])
		{
			g_view_disagree_logged[pass_hash] = true;
			log_view_disagree = true;
		}
	}
	if (log_agree)
		STRAY_LOG_INFO("U0 HOOK AGREES: on the engine-announced pass 0x%016llx the RHI bound u0 = %p "
			"(slot %u, shader %p) and the descriptor walk resolved u0 = %p - the SAME resource, "
			"%ux%u %s the announced %ux%u rect, live, 2D, UAV-capable, HDR. Two routes to one "
			"identity. The walk is still what DLSS uses; this is the oracle. Once per session; "
			"the rate is the [u0] line's assert: group.",
			static_cast<unsigned long long>(pass_hash), reinterpret_cast<void *>(b.resource),
			b.slot, b.shader, reinterpret_cast<void *>(walk.u0), d.width, d.height,
			j.extent_equal ? "==" : ">=", out_width, out_height);
	if (log_disagree)
		STRAY_LOG_WARN("U0 HOOK ASSERTION: on the engine-announced pass 0x%016llx the RHI bound u0 = %p "
			"(slot %u, FRHIUnorderedAccessView %p, shader %p, %ux%u) and the descriptor walk "
			"resolved u0 = %p. THEY DIFFER. The walk's answer is what DLSS SR wrote into; if the "
			"image is wrong this is the first line to read. Once per pass; the rate is the [u0] "
			"line's assert: disagree=.",
			static_cast<unsigned long long>(pass_hash), reinterpret_cast<void *>(b.resource),
			b.slot, reinterpret_cast<void *>(b.object), b.shader, d.width, d.height,
			reinterpret_cast<void *>(walk.u0));
	if (log_no_bind)
		STRAY_LOG_WARN("U0 HOOK: the engine-announced pass 0x%016llx was claimed on thread %lu and NO "
			"UAVIndex==0 bind was recorded in its bracket (binds=%u setCS=%d). Either the "
			"RHISetUAVParameter slot is not 16 or 17 on this exe (read the per-slot roles on the "
			"[u0] line), or the bind arrived on another thread. Logged %d times.",
			static_cast<unsigned long long>(pass_hash), GetCurrentThreadId(), br.binds,
			br.set_cs_seen ? 1 : 0, kNoBindLogLimit);
	if (log_regs)
		STRAY_LOG_INFO("U0 HOOK REGISTERS on pass 0x%016llx (first claimed bracket: %u binds, shader %p): "
			"t0 %s e=%p w=%p | t1 %s e=%p w=%p | t2 %s e=%p w=%p | t3 %s e=%p w=%p | t4 %s e=%p w=%p "
			"| t5 %s e=%p w=%p | View at b-mask %#x (%u uniform buffers bound), walk chose b%u (valid=%d): %s. "
			"e = the engine's own bind (FRHITexture::GetNativeResource for t0-t3/t5, the SRV "
			"cross-match for t4); w = the descriptor walk. CLAUDE.md §2.3's register map, from the engine.",
			static_cast<unsigned long long>(pass_hash), br.binds, br.shader,
			u0::reg_verdict_name(tv[0]), reinterpret_cast<void *>(engine_t[0]), reinterpret_cast<void *>(walk.t[0]),
			u0::reg_verdict_name(tv[1]), reinterpret_cast<void *>(engine_t[1]), reinterpret_cast<void *>(walk.t[1]),
			u0::reg_verdict_name(tv[2]), reinterpret_cast<void *>(engine_t[2]), reinterpret_cast<void *>(walk.t[2]),
			u0::reg_verdict_name(tv[3]), reinterpret_cast<void *>(engine_t[3]), reinterpret_cast<void *>(walk.t[3]),
			u0::reg_verdict_name(tv[4]), reinterpret_cast<void *>(engine_t[4]), reinterpret_cast<void *>(walk.t[4]),
			u0::reg_verdict_name(tv[5]), reinterpret_cast<void *>(engine_t[5]), reinterpret_cast<void *>(walk.t[5]),
			br.ub_mask, br.ub_count, walk.view_cb_register, walk.view_cb_valid ? 1 : 0,
			u0::view_reg_verdict_name(vv));
	if (log_reg_disagree)
		STRAY_LOG_WARN("U0 HOOK ASSERTION: on pass 0x%016llx the engine's bind and the descriptor walk "
			"DISAGREE on SRV register(s) mask %#x: t0 e=%p w=%p t1 e=%p w=%p t2 e=%p w=%p t3 e=%p w=%p "
			"t4 e=%p w=%p t5 e=%p w=%p. The walk's answer is what DLSS SR was fed (t1 colour, t2 "
			"depth, t3 velocity, t5 history); L1 already replaces depth and velocity with the "
			"engine's. Once per pass; the rate is the [u0] line's regs: group.",
			static_cast<unsigned long long>(pass_hash), disagree_mask,
			reinterpret_cast<void *>(engine_t[0]), reinterpret_cast<void *>(walk.t[0]),
			reinterpret_cast<void *>(engine_t[1]), reinterpret_cast<void *>(walk.t[1]),
			reinterpret_cast<void *>(engine_t[2]), reinterpret_cast<void *>(walk.t[2]),
			reinterpret_cast<void *>(engine_t[3]), reinterpret_cast<void *>(walk.t[3]),
			reinterpret_cast<void *>(engine_t[4]), reinterpret_cast<void *>(walk.t[4]),
			reinterpret_cast<void *>(engine_t[5]), reinterpret_cast<void *>(walk.t[5]));
	if (log_view_disagree)
		STRAY_LOG_WARN("U0 HOOK ASSERTION: on pass 0x%016llx the engine bound its one uniform buffer "
			"(ViewUniformBuffer) at b-mask %#x and the View-CB search chose b%u. THE SEARCH TOOK "
			"ANOTHER VIEW'S BUFFER - the facts §36.18 class, now caught by identity rather than by "
			"rect arithmetic. Once per pass; the rate is the [u0] line's viewReg: disagree=.",
			static_cast<unsigned long long>(pass_hash), br.ub_mask, walk.view_cb_register);
}

int format_report(char *buffer, std::size_t size)
{
	if (buffer == nullptr || size == 0)
		return 0;
	int lvl = 0;
	u0::CtxDiscovery ctx;
	bool uav_latched = false;
	unsigned uav_offset = 0;
	bool srv_latched = false;
	unsigned srv_offset = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		lvl = g_level;
		ctx = g_ctx;
		uav_latched = g_uav_latch.latched();
		uav_offset = g_uav_latch.offset();
		srv_latched = g_srv_latch.latched();
		srv_offset = g_srv_latch.offset();
	}
	const bool settled = g_seed_settled.load(std::memory_order_relaxed);
	char slots[256] = {};
	std::size_t n = 0;
	for (unsigned i = 0; i < u0::kProbeCount && n < sizeof(slots); ++i)
	{
		const SlotState &s = g_slots[i];
		const unsigned slot = u0::kProbeFirstSlot + i;
		const bool uav_pair = slot == u0::kSlotSetUAVParameterA || slot == u0::kSlotSetUAVParameterB;
		const int w = uav_pair
			? std::snprintf(slots + n, sizeof(slots) - n, " %u=%llu/%s(i0=%llu res=%llu)", slot,
				static_cast<unsigned long long>(s.calls.load(std::memory_order_relaxed)),
				u0::slot_role_name(static_cast<u0::SlotRole>(s.role.load(std::memory_order_relaxed))),
				static_cast<unsigned long long>(s.index0.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(s.resolved.load(std::memory_order_relaxed)))
			: std::snprintf(slots + n, sizeof(slots) - n, " %u=%llu(null=%llu)", slot,
				static_cast<unsigned long long>(s.calls.load(std::memory_order_relaxed)),
				static_cast<unsigned long long>(s.null_object.load(std::memory_order_relaxed)));
		if (w < 0)
			break;
		n += static_cast<std::size_t>(w);
	}
	const auto v = [](u0::Verdict x) {
		return static_cast<unsigned long long>(g_verdicts[static_cast<std::size_t>(x)].load(std::memory_order_relaxed));
	};
	const auto rv = [](u0::RegVerdict x) {
		return static_cast<unsigned long long>(g_reg_verdicts[static_cast<std::size_t>(x)].load(std::memory_order_relaxed));
	};
	const auto vv = [](u0::ViewRegVerdict x) {
		return static_cast<unsigned long long>(g_view_verdicts[static_cast<std::size_t>(x)].load(std::memory_order_relaxed));
	};
	return std::snprintf(buffer, size,
		"u0hook=%s vtable=%s at=%#llx seed=%#llx hits=%u survivors=%u ms=%.1f seedSamples=%llu foreign=%llu "
		"unresolved=%llu changed=%llu hooked=%d | slots: setCS=%llu noSetCS=%llu%s | latch: uav=%s+%u srv=%s+%u "
		"scans=%llu (ok=%llu noHit=%llu ambiguous=%llu dead=%llu unreadable=%llu) latchedReads=%llu "
		"misses=%llu guardedReads=%llu nativeCalls=%llu nativeRefused=%llu | assert: agree=%llu "
		"disagree=%llu noBind=%llu unresolved=%llu notLive=%llu walkAbsent=%llu descMismatch=%llu "
		"extentNe=%llu shaderMismatch=%llu hookOff=%llu | regs: agree=%llu disagree=%llu "
		"engineAbsent=%llu walkAbsent=%llu unresolved=%llu disagreeMask=%#llx | viewReg: agree=%llu "
		"disagree=%llu noneBound=%llu multipleBound=%llu walkAbsent=%llu | faults=%llu off=%d",
		lvl <= 0 ? "off" : lvl == 1 ? "discover" : "observe",
		!settled ? "pending" : ctx.status == u0::CtxStatus::ok ? "found" : u0::ctx_status_text(ctx.status),
		static_cast<unsigned long long>(ctx.vtable_va), static_cast<unsigned long long>(ctx.seed),
		ctx.qword_hits, ctx.survivors, g_discovery_ms,
		static_cast<unsigned long long>(g_seed_samples.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_seed_foreign.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_seed_unresolved.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_seed_changed.load(std::memory_order_relaxed)),
		hooked() ? 1 : 0,
		static_cast<unsigned long long>(g_set_cs_calls.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_dispatch_no_set_cs.load(std::memory_order_relaxed)), slots,
		uav_latched ? "on" : "off", uav_offset, srv_latched ? "on" : "off", srv_offset,
		static_cast<unsigned long long>(g_scans.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_scan_ok.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_scan_no_hit.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_scan_ambiguous.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_scan_dead.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_scan_unreadable.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_latched_reads.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_latched_misses.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_guarded_reads.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_native_calls.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_native_refused.load(std::memory_order_relaxed)),
		v(u0::Verdict::agree), v(u0::Verdict::disagree), v(u0::Verdict::no_bind),
		v(u0::Verdict::unresolved), v(u0::Verdict::not_live), v(u0::Verdict::walk_absent),
		v(u0::Verdict::desc_mismatch),
		static_cast<unsigned long long>(g_extent_ne.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_shader_mismatch.load(std::memory_order_relaxed)),
		static_cast<unsigned long long>(g_assert_hook_off.load(std::memory_order_relaxed)),
		rv(u0::RegVerdict::agree), rv(u0::RegVerdict::disagree), rv(u0::RegVerdict::engine_absent),
		rv(u0::RegVerdict::walk_absent), rv(u0::RegVerdict::unresolved),
		static_cast<unsigned long long>(g_reg_disagree_mask.load(std::memory_order_relaxed)),
		vv(u0::ViewRegVerdict::agree), vv(u0::ViewRegVerdict::disagree), vv(u0::ViewRegVerdict::none_bound),
		vv(u0::ViewRegVerdict::multiple_bound), vv(u0::ViewRegVerdict::walk_absent),
		static_cast<unsigned long long>(g_faults.load(std::memory_order_relaxed)),
		g_disabled.load(std::memory_order_relaxed) ? 1 : 0);
}

void log_report(const char *when)
{
	if (level() <= 0)
		return;
	char line[1280] = {};
	format_report(line, sizeof(line));
	STRAY_LOG_INFO("[u0] %s: %s%s", when != nullptr ? when : "", line,
		hooked() ? "  (assert/regs/viewReg disagree must stay 0; noBind means the UAV slot is wrong)" : "");
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	unsigned restored = 0;
	unsigned foreign = 0;
	for (unsigned i = 0; i < kPatchCount; ++i)
	{
		if (g_patched[i] == nullptr)
			continue;
		void *ours = i < u0::kProbeCount
			? reinterpret_cast<void *>(probe_for_slot(u0::kProbeFirstSlot + i))
			: reinterpret_cast<void *>(&set_compute_shader_thunk);
		if (*g_patched[i] == ours)
		{
			void *ignored = nullptr;
			patch_slot(g_patched[i], g_original[i], ignored);
			++restored;
		}
		else
			++foreign;
		g_patched[i] = nullptr;
		g_original[i] = nullptr;
	}
	if (restored != 0 || foreign != 0)
		STRAY_LOG_INFO("U0 HOOK: %u slot(s) restored%s.", restored,
			foreign != 0 ? "; some slots were no longer ours and were left alone (a later unload of this DLL is then a crash)" : "");
	g_hooked.store(false, std::memory_order_release);
}

} // namespace stray_dlss::u0hook
