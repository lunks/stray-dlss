#include "engine_seam_hook.hpp"

#include "core/engine_seam.hpp"
#include "intercept/backend.hpp"
#include "log.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace stray_dlss::seamhook {
namespace {

using seam::Announcement;
using seam::Discovery;
using seam::Image;
using seam::Ledger;
using seam::Region;
using seam::SeamStatus;

// The engine's own signature, spelled out as the MSVC x64 ABI sees it. A `const` member
// function of a class with no virtual bases takes `this` in RCX and every subsequent pointer
// or reference in RDX / R8 / R9 / stack — identical to a free function whose first parameter
// is the object. There is exactly one calling convention on x64, so no __thiscall spelling is
// needed and mingw's MS ABI agrees.
//
//   virtual void AddPasses(FRDGBuilder&, const FViewInfo&, const FPassInputs&,
//                          FRDGTextureRef*, FIntRect*, FRDGTextureRef*, FIntRect*) const
using AddPassesFn = void (*)(const void *self, void *graph_builder, const void *view,
                             const void *pass_inputs, void **out_colour, void *out_rect,
                             void **out_half_colour, void *out_half_rect);

std::mutex g_mutex;
seam::Mode g_mode = seam::Mode::off;
bool g_fallback_allowed = true;
Discovery g_discovery;
Ledger g_ledger;

void **g_patched_slot = nullptr;
AddPassesFn g_original = nullptr;
std::atomic<bool> g_hooked{ false };
std::atomic<std::uint64_t> g_frame{ 0 };
// Counted separately from the ledger because it is a different failure: the engine called us
// and we could not read the out-parameter it had just written.
std::atomic<std::uint64_t> g_unreadable{ 0 };
bool g_first_call_logged = false;

// PE section names are 8 bytes and need not be NUL-terminated, so they are copied into
// storage that outlives the Region vector. Sections are enumerated exactly once.
constexpr std::size_t kMaxSections = 96;
char g_section_names[kMaxSections][9];

// The module map RETAINED past discovery, because L1 needs `is_code` on every claim: the
// GetNativeResource pointer we are about to call must be inside an executable section of the
// game's own module. Written once under g_mutex during discovery, read-only afterwards.
std::vector<Region> g_regions;
std::uint64_t g_module_lo = 0;
std::uint64_t g_module_hi = 0;

// [STRAYDLSS] EngineSeamInputs. L1: take the SR inputs from the engine's own FPassInputs.
bool g_inputs_enabled = true;
// Continuous outcome counters for dispatches the ENGINE announced and we claimed.
std::uint64_t g_outcomes[static_cast<std::size_t>(seam::SeamRefusal::count)] = {};
// L1 resolution outcomes, so a fallback is never silent.
std::uint64_t g_l1_resolved = 0;      // all three of colour/depth/velocity came back registered
std::uint64_t g_l1_partial = 0;       // depth+velocity resolved, colour did not
std::uint64_t g_l1_fell_back = 0;     // neither depth nor velocity resolved; heuristic used
// The claim was served from an announcement whose FRDGBuilder is gone, so the FPassInputs
// pointers must not be dereferenced. NOT an error - L1 declines and the heuristic supplies
// that frame - but THE number to read: a climbing rate means the ledger has slipped a graph
// behind, which is the bug that crashed the game (docs/RESEARCH-ENGINE-TAA-HOOK.md §12).
std::uint64_t g_l1_stale = 0;
bool g_l1_first_logged = false;
bool g_l1_stale_logged = false;
bool g_l1_disagree_logged = false;
// The announce/claim THREAD PAIR, latched on the first claim and reported if it ever changes.
// Nothing is gated on it — requiring the two to be equal is what made L1 inert on the box —
// but the pair is a fingerprint of the engine's threading model, and a session in which it
// moves is a session in which something about RDG execution changed under us. One WARN.
std::uint64_t g_l1_announce_thread = 0;
std::uint64_t g_l1_claim_thread = 0;
bool g_l1_threads_latched = false;
bool g_l1_thread_change_logged = false;
// A read or a call that the guards let through and the CPU refused. Non-zero means an offset
// or a vtable slot in seam::kRdgResourceRhiOffset / kRhiGetNativeResourceSlot is wrong on this
// executable, and L1 disables itself for the session rather than roll the dice again.
std::atomic<std::uint64_t> g_l1_faults{ 0 };
std::atomic<std::uint64_t> g_l1_fault_va{ 0 };
std::atomic<bool> g_l1_disabled{ false };

std::uint64_t read_ptr(const void *base, std::size_t offset)
{
	std::uint64_t v = 0;
	std::memcpy(&v, static_cast<const unsigned char *>(base) + offset, sizeof(v));
	return v;
}

std::int32_t read_i32(const void *base, std::size_t offset)
{
	std::int32_t v = 0;
	std::memcpy(&v, static_cast<const unsigned char *>(base) + offset, sizeof(v));
	return v;
}

// ---------------------------------------------------------------------------------------
// The stand-in
// ---------------------------------------------------------------------------------------

// Defined below with the rest of the guarded reader; declared here because the ONLY safe place
// to call it is inside AddPasses, which is above it in this file.
std::uint64_t resolve_at_announce(std::uint64_t rdg, seam::RhiChain &status);
// The fault latch, shared by the announce-time resolve and the claim-time consumer.
bool l1_faulted();
void l1_note_faults_and_latch();

void add_passes_thunk(const void *self, void *graph_builder, const void *view,
                      const void *pass_inputs, void **out_colour, void *out_rect,
                      void **out_half_colour, void *out_half_rect)
{
	// Read FPassInputs BEFORE forwarding, and RESOLVE IT HERE.
	//
	// This is the one place in the frame where the three FRDGTextures are provably alive: we
	// are inside the FRDGBuilder's own setup, on the render thread, and `Execute()` has not
	// run. It ends with `Clear()` -> `Allocator.ReleaseAll()`, which frees every FRDGTexture,
	// and it neither flushes nor waits for the RHI thread — while `FRHICommandList` is
	// "definitions for queueing up & executing later" and a pass lambda's
	// DispatchComputeShader does `ALLOC_COMMAND` rather than calling the RHI. So the D3D12
	// Dispatch we intercept runs on another thread AFTER the arena is freed: resolving there
	// read freed memory BY CONSTRUCTION, which is what crashed 3365f02 and what left the
	// freshness gate declining every claim in e421e14 (report §12.9).
	//
	// What crosses to the dispatch is therefore an `ID3D12Resource*` that no allocator owns,
	// checked for liveness against our own registry at claim.
	Announcement a;
	a.frame = g_frame.load(std::memory_order_relaxed);
	// Reported, never tested: AddPasses and the dispatch are on different threads by design
	// (§12.8), and requiring them to match is what made L1 inert once already.
	a.thread = static_cast<std::uint64_t>(GetCurrentThreadId());
	if (pass_inputs != nullptr)
	{
		a.colour_rdg = read_ptr(pass_inputs, seam::kPassInputsSceneColor);
		a.depth_rdg = read_ptr(pass_inputs, seam::kPassInputsSceneDepth);
		a.velocity_rdg = read_ptr(pass_inputs, seam::kPassInputsSceneVelocity);

		seam::L1GateInputs gi;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			gi.inputs_enabled = g_inputs_enabled;
			gi.mode = g_mode;
		}
		gi.hooked = true;      // we are standing in for AddPasses, so the seam is live
		gi.announced = true;   // this IS the announcement
		gi.faulted = l1_faulted();
		if (seam::l1_gate(gi) == seam::L1Gate::resolve)
		{
			// Try all three. Depth and velocity are `RegisterExternalTexture` /
			// `TryRegisterExternalTexture` in GetSceneTextureParameters, and
			// RegisterExternalTexture calls SetRHI immediately, so they are expected to
			// resolve here. Scene colour is the post-chain texture and is expected to be
			// graph-allocated and therefore `rhi_null` — but ASSERT NOTHING: the `l1:`
			// counters report which actually did.
			a.colour_res = resolve_at_announce(a.colour_rdg, a.colour_status);
			a.depth_res = resolve_at_announce(a.depth_rdg, a.depth_status);
			a.velocity_res = resolve_at_announce(a.velocity_rdg, a.velocity_status);
			l1_note_faults_and_latch();
		}
	}

	AddPassesFn original = nullptr;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		original = g_original;
	}
	// Forward unconditionally. This stage OBSERVES; the engine's own upscale must still run,
	// and our own dispatch interception downstream is what replaces it. If the original is
	// somehow gone, doing nothing would leave the frame's scene colour unwritten, so the
	// only safe answer is to forward or to have never patched at all.
	if (original != nullptr)
		original(self, graph_builder, view, pass_inputs, out_colour, out_rect,
			out_half_colour, out_half_rect);

	// The output view rect, now that the engine has written it. This is the whole prize: the
	// dispatch is GetGroupCount(PracticableDestRect.Size(), 8) and PracticableDestRect is
	// DivideAndRoundUp(OutputViewRect, ResolutionDivisor) with ResolutionDivisor == 1 for
	// every Main* config (TemporalAA.cpp:654-656, :958), so ceil(this rect / 8) is exactly
	// the group count the game is about to issue.
	if (out_rect != nullptr)
	{
		const std::int32_t min_x = read_i32(out_rect, seam::kIntRectMinX);
		const std::int32_t min_y = read_i32(out_rect, seam::kIntRectMinY);
		const std::int32_t max_x = read_i32(out_rect, seam::kIntRectMaxX);
		const std::int32_t max_y = read_i32(out_rect, seam::kIntRectMaxY);
		if (max_x > min_x && max_y > min_y)
		{
			a.out_width = static_cast<std::uint32_t>(max_x - min_x);
			a.out_height = static_cast<std::uint32_t>(max_y - min_y);
		}
	}
	if (a.out_width == 0 || a.out_height == 0)
	{
		g_unreadable.fetch_add(1, std::memory_order_relaxed);
		return;
	}

	bool log_first = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_ledger.announce(a);
		if (!g_first_call_logged)
		{
			g_first_call_logged = true;
			log_first = true;
		}
	}
	if (log_first)
		STRAY_LOG_INFO("ENGINE SEAM: ITemporalUpscaler::AddPasses reached us on frame %llu - "
			"output rect %ux%u, FPassInputs colour=%p depth=%p velocity=%p. This is the "
			"engine's OWN primary temporal upscale; every FTAAStandaloneCS look-alike (DOF, "
			"light shafts, SSR, water, planar reflections) arrives by a different route and "
			"will never appear here. Mode=%s: %s.",
			static_cast<unsigned long long>(a.frame), a.out_width, a.out_height,
			reinterpret_cast<void *>(a.colour_rdg), reinterpret_cast<void *>(a.depth_rdg),
			reinterpret_cast<void *>(a.velocity_rdg), seam::mode_name(mode()),
			mode() == seam::Mode::authoritative
				? "DLSS SR will run ONLY on the dispatch this announcement fits"
				: "observing; the heuristic matcher still gates DLSS");
}

// ---------------------------------------------------------------------------------------
// Mapping the module
// ---------------------------------------------------------------------------------------

// Builds one Region per PE section of the process's own main module. Read-only: this walks
// headers that are already mapped and never touches memory outside a section's virtual range.
bool map_own_module(std::vector<Region> &regions, const char *&why, std::uint64_t &base_out)
{
	HMODULE module = GetModuleHandleW(nullptr);
	if (module == nullptr)
	{
		why = "GetModuleHandleW(nullptr) returned nothing";
		return false;
	}
	auto *const base = reinterpret_cast<const unsigned char *>(module);
	base_out = reinterpret_cast<std::uint64_t>(base);

	const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
	{
		why = "the main module has no MZ header";
		return false;
	}
	const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE ||
		nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
	{
		why = "the main module is not a PE32+ x86-64 image";
		return false;
	}

	const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(nt);
	const unsigned count = nt->FileHeader.NumberOfSections;
	for (unsigned i = 0; i < count; ++i, ++section)
	{
		const DWORD size = section->Misc.VirtualSize != 0
			? section->Misc.VirtualSize
			: section->SizeOfRawData;
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
		regions.push_back(r);
	}
	if (regions.empty())
	{
		why = "the main module has no readable sections";
		return false;
	}
	return true;
}

void log_undecoded(const Discovery &d)
{
	static const char kNibble[] = "0123456789ABCDEF";
	char hex[3 * sizeof(d.undecoded) + 1] = {};
	std::size_t n = 0;
	for (std::size_t i = 0; i < sizeof(d.undecoded); ++i)
	{
		hex[n++] = kNibble[(d.undecoded[i] >> 4) & 0x0F];
		hex[n++] = kNibble[d.undecoded[i] & 0x0F];
		hex[n++] = ' ';
	}
	hex[n] = '\0';
	STRAY_LOG_WARN("ENGINE SEAM: could not decode the accessor at %#llx statically. Its first "
		"16 bytes are: %s- paste this line and the decoder is one line of code, not another "
		"round trip.", static_cast<unsigned long long>(d.undecoded_va), hex);
}

// Writes one pointer-sized slot in a read-only page, and remembers the original.
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

void run_discovery(int level)
{
	std::vector<Region> regions;
	const char *why = "";
	std::uint64_t base = 0;
	if (!map_own_module(regions, why, base))
	{
		STRAY_LOG_WARN("ENGINE SEAM: cannot scan the game module - %s. Staying off.", why);
		return;
	}

	LARGE_INTEGER freq{};
	LARGE_INTEGER t0{};
	LARGE_INTEGER t1{};
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&t0);

	Image image;
	image.regions = regions.data();
	image.count = regions.size();
	const Discovery d = seam::discover(image);

	// Keep the map: L1 needs `is_code` on every claim, and the module is mapped for the
	// process's life.
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_regions = regions;
		g_module_lo = 0;
		g_module_hi = 0;
		for (const Region &r : g_regions)
		{
			if (g_module_lo == 0 || r.va < g_module_lo)
				g_module_lo = r.va;
			if (r.va + r.size > g_module_hi)
				g_module_hi = r.va + r.size;
		}
	}

	QueryPerformanceCounter(&t1);
	const double ms = freq.QuadPart != 0
		? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) /
			static_cast<double>(freq.QuadPart)
		: 0.0;

	std::size_t scanned = 0;
	for (const Region &r : regions)
		scanned += r.size;

	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_discovery = d;
	}

	if (d.status != SeamStatus::ok)
	{
		STRAY_LOG_WARN("ENGINE SEAM: NOT FOUND - %s. (base=%#llx, %zu sections, %zu MB "
			"scanned in %.1f ms; nameHits=%u debugNameHits=%u name=%#llx getDebugName=%#llx "
			"vtable=%#llx min=%.6f max=%.6f). Nothing was installed and the heuristic matcher "
			"is unaffected.",
			seam::status_text(d.status), static_cast<unsigned long long>(base), regions.size(),
			scanned / (1024u * 1024u), ms, d.name_hits, d.debug_name_hits,
			static_cast<unsigned long long>(d.name_va),
			static_cast<unsigned long long>(d.get_debug_name_va),
			static_cast<unsigned long long>(d.vtable_va),
			static_cast<double>(d.min_fraction), static_cast<double>(d.max_fraction));
		if (d.status == SeamStatus::fraction_shape)
			log_undecoded(d);
		return;
	}

	STRAY_LOG_INFO("ENGINE SEAM FOUND: ITemporalUpscaler vtable at %#llx (base=%#llx, %zu MB "
		"in %.1f ms). Validated by three independent constants from one scan: GetDebugName "
		"returns the literal at %#llx, GetMin/MaxUpsampleResolutionFraction decode to %.6f "
		"and %.6f (want exactly 0.5 and 2.0 - SceneView.h:1438-1439).",
		static_cast<unsigned long long>(d.vtable_va), static_cast<unsigned long long>(base),
		scanned / (1024u * 1024u), ms, static_cast<unsigned long long>(d.name_va),
		static_cast<double>(d.min_fraction), static_cast<double>(d.max_fraction));
	STRAY_LOG_INFO("ENGINE SEAM slots: dtor=%#llx GetDebugName=%#llx AddPasses=%#llx "
		"GetMin=%#llx GetMax=%#llx (candidates: name=%u getDebugName=%u vtable=%u)",
		static_cast<unsigned long long>(d.slot[seam::kSlotDestructor]),
		static_cast<unsigned long long>(d.slot[seam::kSlotGetDebugName]),
		static_cast<unsigned long long>(d.slot[seam::kSlotAddPasses]),
		static_cast<unsigned long long>(d.slot[seam::kSlotGetMinFraction]),
		static_cast<unsigned long long>(d.slot[seam::kSlotGetMaxFraction]),
		d.name_hits, d.debug_name_hits, d.vtable_hits);

	if (level < 2)
	{
		STRAY_LOG_INFO("ENGINE SEAM: EngineSeam=1, so nothing is installed. Set EngineSeam=2 "
			"to stand in for AddPasses and cross-check the heuristic matcher against it, or "
			"EngineSeam=3 (the default) to let the engine's answer gate DLSS.");
		return;
	}
	if (d.vtable_hits != 1)
	{
		STRAY_LOG_WARN("ENGINE SEAM: %u vtables passed validation, not 1. Refusing to patch - "
			"an ambiguous answer is not an answer.", d.vtable_hits);
		return;
	}

	auto **slot = reinterpret_cast<void **>(
		static_cast<std::uintptr_t>(d.vtable_va) + 8u * seam::kSlotAddPasses);
	void *original = nullptr;
	if (!patch_slot(slot, reinterpret_cast<void *>(&add_passes_thunk), original))
	{
		STRAY_LOG_ERROR("ENGINE SEAM: VirtualProtect on the vtable slot at %p failed (%lu). "
			"Not installed.", static_cast<void *>(slot), GetLastError());
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_patched_slot = slot;
		g_original = reinterpret_cast<AddPassesFn>(original);
	}
	g_hooked.store(true, std::memory_order_release);
	STRAY_LOG_INFO("ENGINE SEAM INSTALLED: AddPasses slot %p now points at our stand-in; the "
		"engine's own implementation at %p is forwarded to on every call, so the image is "
		"unchanged. %s",
		static_cast<void *>(slot), original,
		level >= 3
			? "MODE=AUTHORITATIVE: DLSS SR runs only on the dispatch the engine announced; "
			  "the cooked-hash table and the structural signature are assertions now, not gates."
			: "MODE=OBSERVE: counting only - DLSS is still gated by the heuristic matcher.");
}

// --------------------------------------------------------------------------------------
// L1: the guarded reader
// --------------------------------------------------------------------------------------

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

// A plausibility test for a heap pointer we are about to dereference. There is no portable
// way to make the read itself fault-proof (SEH is MSVC-only and this file also has to compile
// under mingw for the fast CI lane), so the defence is in depth: reject the obviously wrong,
// keep the chain short, require the vtable to live inside the game's own module, require the
// function to be in executable memory, and finally require the ANSWER to be a resource our
// own registry already knows. A wrong offset has to pass all five to do damage.
bool plausible_heap_ptr(std::uint64_t va)
{
	if (va == 0 || (va & 7ull) != 0)
		return false;
	// User-mode canonical range on Win64. Wine/Proton place both the exe and the heap well
	// inside it; the point is to reject small integers and sign-extended garbage.
	return va >= 0x10000ull && va < 0x0000800000000000ull;
}

// Is this address actually READABLE, right now? `plausible_heap_ptr` is a RANGE test — it
// says the number could be a pointer, never that anything is mapped there — and shipping a
// dereference behind it is what crashed the game: `rdg+16` came back 0x0000021c000003c0,
// which is two int32s (960, 540) read as one qword, i.e. an FIntPoint from recycled RDG arena
// memory. It is 8-aligned, above 0x10000 and canonical, so every guard L1 had passed it, and
// the next read faulted inside memcpy (docs/RESEARCH-ENGINE-TAA-HOOK.md §12).
bool committed_and_readable(std::uint64_t va, std::size_t n)
{
	MEMORY_BASIC_INFORMATION mbi{};
	const SIZE_T got = VirtualQuery(
		reinterpret_cast<LPCVOID>(static_cast<std::uintptr_t>(va)), &mbi, sizeof(mbi));
	if (got != sizeof(mbi) || mbi.State != MEM_COMMIT)
		return false;
	// PAGE_GUARD and PAGE_NOACCESS both fault; a guard page would additionally arm a
	// STATUS_GUARD_PAGE_VIOLATION for whoever owns it, so never touch one.
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

// The read itself, under SEH. EXPLICIT AND DELIBERATE: this dereferences engine memory whose
// layout (FRDGResource::ResourceRHI @16) and lifetime are both [derived], so the honest
// engineering position is that it CAN fault and the process must survive it saying so — a
// loud failure rather than a quiet wrong image, and here rather than a dead game
// (CLAUDE.md §0.2). MSVC only; the mingw lane has no __try and relies on the VirtualQuery
// guard above, which is why the fault counter says which build it is reading.
// The function holds no object needing unwinding, which is what MSVC requires of __try.
bool read_u64_guarded(std::uint64_t va, std::uint64_t *out)
{
#if defined(_MSC_VER)
	__try
	{
		std::memcpy(out, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(va)),
			sizeof(*out));
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
#else
	std::memcpy(out, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(va)),
		sizeof(*out));
	return true;
#endif
}

bool l1_read_u64(void *, std::uint64_t va, std::uint64_t *out)
{
	// The FRDGTexture field read and the vtable read are heap; the vtable SLOT read must be
	// inside the module (a UE 4.27 vtable is in the exe's read-only data). Accepting either
	// keeps one reader for all three hops while still rejecting nonsense.
	if (!plausible_heap_ptr(va) && !region_covers(va, sizeof(std::uint64_t), false))
		return false;
	if (!committed_and_readable(va, sizeof(std::uint64_t)))
		return false;
	if (!read_u64_guarded(va, out))
	{
		g_l1_faults.fetch_add(1, std::memory_order_relaxed);
		g_l1_fault_va.store(va, std::memory_order_relaxed);
		return false;
	}
	return true;
}

bool l1_is_code(void *, std::uint64_t va)
{
	return region_covers(va, 1, true);
}

using GetNativeResourceFn = void *(*)(void *rhi);

// The one CALL into engine code on this path, under the same SEH as the reads and for the
// same reason: `fn` came out of a vtable slot index that is [derived], and it is invoked with
// a `this` that came out of an offset that is [derived]. Both survived four guards; neither is
// proven. A fault here is a counted refusal, not a dead game.
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

bool l1_faulted() { return g_l1_disabled.load(std::memory_order_acquire); }

// A fault means an offset is wrong, not that this frame was unlucky. Say so once, at ERROR,
// with the address the CPU refused - and stop. Called on the render thread right after the
// three resolves, so the latch is set before the next AddPasses.
void l1_note_faults_and_latch()
{
	if (g_l1_faults.load(std::memory_order_relaxed) == 0)
		return;
	if (g_l1_disabled.exchange(true, std::memory_order_acq_rel))
		return;
	STRAY_LOG_ERROR("ENGINE SEAM L1 DISABLED: a guarded read or call into engine memory "
		"FAULTED at %#llx, inside AddPasses on the render thread. The guards caught it (the "
		"game is alive and this line exists because of it), but a fault means one of the "
		"[derived] offsets is wrong on THIS executable - FRDGResource::ResourceRHI @%zu or "
		"FRHITexture::GetNativeResource slot %u. L1 is off for the rest of the session and "
		"the heuristic supplies DLSS SR's inputs; set EngineSeamInputs=0 to make that the "
		"deliberate configuration. Paste this line: the offset is one constant, not another "
		"round trip.",
		static_cast<unsigned long long>(g_l1_fault_va.load(std::memory_order_relaxed)),
		seam::kRdgResourceRhiOffset, seam::kRhiGetNativeResourceSlot);
}

// One FRDGTexture -> ID3D12Resource, called ONLY from inside AddPasses. `status` always says
// why on failure. The registry liveness check is deliberately NOT here: it belongs at claim,
// on the thread that is about to use the pointer, and it is the only check that still means
// anything once the FRDGTexture is gone.
std::uint64_t resolve_at_announce(std::uint64_t rdg, seam::RhiChain &status)
{
	seam::RdgReader reader;
	reader.read_u64 = &l1_read_u64;
	reader.is_code = &l1_is_code;
	reader.ctx = nullptr;

	std::uint64_t rhi = 0;
	std::uint64_t fn = 0;
	status = seam::resolve_rhi_fn(reader, rdg, &rhi, &fn);
	if (status != seam::RhiChain::ok)
		return 0;

	// The one call into engine code on this path. It is FRHITexture::GetNativeResource, whose
	// D3D12 override returns FD3D12Resource::GetResource() — an ID3D12Resource* — and which
	// takes nothing but `this` (D3D12Texture.h:311-331).
	void *native = nullptr;
	if (!call_native_resource_guarded(fn, rhi, &native))
	{
		g_l1_faults.fetch_add(1, std::memory_order_relaxed);
		g_l1_fault_va.store(rhi, std::memory_order_relaxed);
		status = seam::RhiChain::rhi_unreadable;
		return 0;
	}
	return reinterpret_cast<std::uint64_t>(native);
}

// THE VALIDATION, at claim, on the thread about to use the pointer. Our own registry sees every
// ID3D12Resource the process creates; a pointer it has never seen is not a resource, whatever
// the chain thought — and by this point the FRDGTexture it came from no longer exists, so this
// is the only check left that means anything.
bool resource_is_live(std::uint64_t id)
{
	if (id == 0)
		return false;
	icept::Backend *b = icept::backend();
	return b != nullptr && b->is_resource_live(static_cast<icept::ResourceId>(id));
}

} // namespace

void configure(int level, bool fallback, bool inputs)
{
	const seam::Mode m = seam::mode_from_level(level);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_mode = m;
		g_fallback_allowed = fallback;
		g_inputs_enabled = inputs;
	}
	if (m == seam::Mode::off)
	{
		STRAY_LOG_INFO("ENGINE SEAM MODE: off ([STRAYDLSS] EngineSeam=0). The heuristic matcher "
			"gates DLSS: cooked-hash table + structural signature + pin.");
		return;
	}
	STRAY_LOG_INFO("ENGINE SEAM MODE: %s ([STRAYDLSS] EngineSeam=%d, EngineSeamFallback=%d). "
		"Looking for UE 4.27's ITemporalUpscaler in the game module - a read-only scan of our "
		"own process that executes nothing it finds; a wrong answer is refused, never hooked.",
		seam::mode_name(m), level, fallback ? 1 : 0);
	run_discovery(level);

	// The one line that says what gates DLSS this session. Written before the first frame,
	// so it is readable from the main menu.
	if (m == seam::Mode::authoritative && !hooked())
	{
		if (fallback)
			STRAY_LOG_ERROR("ENGINE SEAM MODE: authoritative was requested but the seam is NOT "
				"live (see the ENGINE SEAM line above). EngineSeamFallback=1, so the HEURISTIC "
				"matcher gates DLSS this session - the pre-seam path, with its known "
				"look-alike exposure. Fix the seam; do not get used to this line.");
		else
			STRAY_LOG_ERROR("ENGINE SEAM MODE: authoritative was requested, the seam is NOT live, "
				"and EngineSeamFallback=0. DLSS SR WILL NOT RUN this session: every candidate "
				"dispatch is refused as no-seam. Set EngineSeamFallback=1 to run the heuristic "
				"instead.");
	}
	else
	{
		STRAY_LOG_INFO("ENGINE SEAM MODE: %s is ACTIVE - %s.", seam::mode_name(m),
			m == seam::Mode::authoritative
				? "the engine's ITemporalUpscaler::AddPasses announcement gates DLSS SR"
				: m == seam::Mode::observe
					? "the heuristic gates DLSS; the engine's announcements are cross-checked"
					: "discovery only; nothing installed and the heuristic gates DLSS");
	}
}

seam::Mode mode()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_mode;
}

bool fallback_allowed()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_fallback_allowed;
}

seam::Gate gate(bool announced)
{
	seam::GateInputs in;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		in.mode = g_mode;
		in.fallback_allowed = g_fallback_allowed;
	}
	in.hooked = hooked();
	in.announced = announced;
	return seam::decide(in);
}

bool discovered()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	return g_discovery.status == SeamStatus::ok;
}

bool hooked() { return g_hooked.load(std::memory_order_acquire); }

void note_present(std::uint64_t frame)
{
	g_frame.store(frame, std::memory_order_relaxed);
	if (!g_hooked.load(std::memory_order_acquire))
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	g_ledger.begin_frame(frame);
}

Verdict claim(std::uint32_t group_x, std::uint32_t group_y)
{
	Verdict v;
	if (!g_hooked.load(std::memory_order_acquire))
		return v;
	v.active = true;
	std::lock_guard<std::mutex> lock(g_mutex);
	if (const Announcement *a = g_ledger.claim(group_x, group_y))
	{
		v.announced = true;
		v.colour_rdg = a->colour_rdg;
		v.depth_rdg = a->depth_rdg;
		v.velocity_rdg = a->velocity_rdg;
		// Already resolved, inside AddPasses, while the FRDGBuilder was alive.
		v.colour_res = a->colour_res;
		v.depth_res = a->depth_res;
		v.velocity_res = a->velocity_res;
		v.colour_status = a->colour_status;
		v.depth_status = a->depth_status;
		v.velocity_status = a->velocity_status;
		v.out_width = a->out_width;
		v.out_height = a->out_height;
		v.sequence = a->sequence;
		// IDENTITY is claimed with the ledger's deliberate slack (up to kRetireAfter*), which
		// is what keeps `unclaimed` honest when a dispatch arrives late. POINTERS are not:
		// they are only dereferenceable while the announcing FRDGBuilder still lives. Decide
		// that here, once, next to the claim that knows both clocks.
		seam::Freshness f;
		f.announce_sequence = a->sequence;
		f.ledger_sequence = g_ledger.sequence();
		f.announce_frame = a->frame;
		f.current_frame = g_ledger.frame();
		f.announce_thread = a->thread;
		f.current_thread = static_cast<std::uint64_t>(GetCurrentThreadId());
		v.fresh = seam::announcement_is_fresh(f);
		// Latch the thread pair. Not a gate — a REPORT, so that "AddPasses and the dispatch
		// are on different threads" stays a visible, named property of this engine build
		// rather than an assumption buried in a predicate.
		if (!g_l1_threads_latched)
		{
			g_l1_threads_latched = true;
			g_l1_announce_thread = f.announce_thread;
			g_l1_claim_thread = f.current_thread;
			v.threads_first_seen = true;
		}
		else if (f.announce_thread != g_l1_announce_thread ||
			f.current_thread != g_l1_claim_thread)
		{
			if (!g_l1_thread_change_logged)
			{
				g_l1_thread_change_logged = true;
				v.threads_changed = true;
			}
		}
		v.announce_frame = f.announce_frame;
		v.current_frame = f.current_frame;
		v.announce_thread = f.announce_thread;
		v.current_thread = f.current_thread;
		v.ledger_sequence = f.ledger_sequence;
	}
	return v;
}

int format_report(char *buffer, std::size_t size)
{
	if (buffer == nullptr || size == 0)
		return 0;
	seam::LedgerCounters c;
	bool on = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		on = g_discovery.status == SeamStatus::ok;
		c = g_ledger.counters();
	}
	seam::Mode m = seam::Mode::off;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		m = g_mode;
	}
	std::uint64_t out[static_cast<std::size_t>(seam::SeamRefusal::count)] = {};
	std::uint64_t l1r = 0;
	std::uint64_t l1p = 0;
	std::uint64_t l1f = 0;
	std::uint64_t l1s = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (std::size_t i = 0; i < static_cast<std::size_t>(seam::SeamRefusal::count); ++i)
			out[i] = g_outcomes[i];
		l1r = g_l1_resolved;
		l1p = g_l1_partial;
		l1f = g_l1_fell_back;
		l1s = g_l1_stale;
	}
	// `unclaimed` and every non-zero refusal below it are the SAME failure seen at different
	// depths: the engine ran its primary upscale and DLSS did not. Their sum is the number of
	// frames that published no guides, which is what NR reports as guides-stale / frame-gap.
	return std::snprintf(buffer, size,
		"seam=%s mode=%s hooked=%d announced=%llu claimed=%llu unclaimed=%llu orphans=%llu "
		"lookalikesRefused=%llu overflow=%llu unreadableRect=%llu | notClaimed: %s=%llu | "
		"claimedButNoSR: %s=%llu %s=%llu %s=%llu %s=%llu %s=%llu %s=%llu | evaluated=%llu | "
		"l1: resolved=%llu partial=%llu fellBack=%llu stale=%llu faults=%llu off=%d",
		on ? "found" : "off", seam::mode_name(m), hooked() ? 1 : 0,
		static_cast<unsigned long long>(c.announced),
		static_cast<unsigned long long>(c.claimed),
		static_cast<unsigned long long>(c.unclaimed),
		static_cast<unsigned long long>(c.orphans),
		static_cast<unsigned long long>(c.rect_mismatch),
		static_cast<unsigned long long>(c.overflow),
		static_cast<unsigned long long>(g_unreadable.load(std::memory_order_relaxed)),
		"noDispatch", static_cast<unsigned long long>(c.unclaimed),
		seam::seam_refusal_name(seam::SeamRefusal::view_unreadable),
		static_cast<unsigned long long>(out[static_cast<std::size_t>(seam::SeamRefusal::view_unreadable)]),
		seam::seam_refusal_name(seam::SeamRefusal::dead_inputs),
		static_cast<unsigned long long>(out[static_cast<std::size_t>(seam::SeamRefusal::dead_inputs)]),
		seam::seam_refusal_name(seam::SeamRefusal::role_unresolved),
		static_cast<unsigned long long>(out[static_cast<std::size_t>(seam::SeamRefusal::role_unresolved)]),
		seam::seam_refusal_name(seam::SeamRefusal::mv_failed),
		static_cast<unsigned long long>(out[static_cast<std::size_t>(seam::SeamRefusal::mv_failed)]),
		seam::seam_refusal_name(seam::SeamRefusal::create_failed),
		static_cast<unsigned long long>(out[static_cast<std::size_t>(seam::SeamRefusal::create_failed)]),
		seam::seam_refusal_name(seam::SeamRefusal::eval_failed),
		static_cast<unsigned long long>(out[static_cast<std::size_t>(seam::SeamRefusal::eval_failed)]),
		static_cast<unsigned long long>(out[static_cast<std::size_t>(seam::SeamRefusal::none)]),
		static_cast<unsigned long long>(l1r), static_cast<unsigned long long>(l1p),
		static_cast<unsigned long long>(l1f), static_cast<unsigned long long>(l1s),
		static_cast<unsigned long long>(g_l1_faults.load(std::memory_order_relaxed)),
		g_l1_disabled.load(std::memory_order_relaxed) ? 1 : 0);
}

EngineInputs resolve_inputs(const Verdict &v)
{
	EngineInputs out;
	seam::L1GateInputs gi;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		gi.inputs_enabled = g_inputs_enabled;
		gi.mode = g_mode;
	}
	gi.hooked = hooked();
	gi.announced = v.announced;
	// A fault already told us an offset is wrong on this executable. Rolling the dice again
	// every frame is how a diagnosis becomes a crash report; L1 stays off for the session.
	gi.faulted = l1_faulted();

	// The thread pair, said once. It is not a gate and must never become one again; it is
	// here so that "AddPasses and the dispatch run on different threads" is a MEASUREMENT in
	// the log rather than an assumption in a predicate.
	if (v.threads_first_seen)
		STRAY_LOG_INFO("ENGINE SEAM L1: AddPasses announced on thread %llu and the dispatch "
			"was recorded on thread %llu%s. UE 4.27 runs RDG setup on one and drains the RHI "
			"command list on the other, so these differing is NORMAL and nothing is gated on "
			"it. It is also WHY the resolve happens inside AddPasses: by the time this thread "
			"records the dispatch, Execute() has already run Allocator.ReleaseAll(). Latched: "
			"if this pair changes mid-session it gets one WARN.",
			static_cast<unsigned long long>(v.announce_thread),
			static_cast<unsigned long long>(v.current_thread),
			v.announce_thread == v.current_thread ? " (the same thread)" : "");
	if (v.threads_changed)
		STRAY_LOG_WARN("ENGINE SEAM L1: the announce/claim thread pair CHANGED - now %llu -> "
			"%llu. Nothing is gated on it, so this costs nothing by itself, but the engine's "
			"RDG threading moved under us and that is worth knowing before trusting any "
			"lifetime argument that assumed it did not. Once per session.",
			static_cast<unsigned long long>(v.announce_thread),
			static_cast<unsigned long long>(v.current_thread));

	// STALENESS IS NO LONGER A REFUSAL. It is counted, and that is all it is: the announcement
	// carries plain ID3D12Resource* that no allocator owns, so "a newer graph exists" says
	// nothing about whether they are safe. It says how far the RHI thread lags — measured on
	// the box as exactly one announcement and one frame, every frame, which is why gating on
	// it declined 4147 of 4147 claims (report §12.8, §12.9).
	if (!v.fresh)
	{
		bool first = false;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			++g_l1_stale;
			if (!g_l1_stale_logged)
			{
				g_l1_stale_logged = true;
				first = true;
			}
		}
		if (first)
			STRAY_LOG_INFO("ENGINE SEAM L1: the claim is one graph behind the announcement, "
				"which is NORMAL and is no longer a refusal - %s%s(seq %llu vs newest %llu, "
				"frame %llu vs %llu; threads %llu -> %llu). UE 4.27 drains the RHI command "
				"list a frame behind RDG setup, so this is the pipeline depth, not an error. "
				"It is safe because the resources were resolved inside AddPasses while the "
				"FRDGBuilder was alive; nothing dereferences an FRDGTexture here. The rate is "
				"the [seam] line's l1: stale=.",
				v.sequence != v.ledger_sequence ? "a newer announcement exists; " : "",
				v.announce_frame != v.current_frame ? "the frame turned over; " : "",
				static_cast<unsigned long long>(v.sequence),
				static_cast<unsigned long long>(v.ledger_sequence),
				static_cast<unsigned long long>(v.announce_frame),
				static_cast<unsigned long long>(v.current_frame),
				static_cast<unsigned long long>(v.announce_thread),
				static_cast<unsigned long long>(v.current_thread));
	}

	const seam::L1Gate gate_verdict = seam::l1_gate(gi);
	if (gate_verdict != seam::L1Gate::resolve)
		return out;
	out.enabled = true;

	// No dereference here. The chain walk happened on the render thread inside AddPasses; all
	// that is left is to ask our own registry whether each pointer is still a live resource.
	out.colour = v.colour_res;
	out.depth = v.depth_res;
	out.velocity = v.velocity_res;
	out.colour_status = v.colour_status;
	out.depth_status = v.depth_status;
	out.velocity_status = v.velocity_status;
	out.colour_registered = resource_is_live(out.colour);
	out.depth_registered = resource_is_live(out.depth);
	out.velocity_registered = resource_is_live(out.velocity);

	bool log_first = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (out.depth_ok() && out.velocity_ok() && out.colour_ok())
			++g_l1_resolved;
		else if (out.depth_ok() && out.velocity_ok())
			++g_l1_partial;
		else
			++g_l1_fell_back;
		if (!g_l1_first_logged)
		{
			g_l1_first_logged = true;
			log_first = true;
		}
	}
	if (log_first)
		STRAY_LOG_INFO("ENGINE SEAM L1: first use of the engine's own FPassInputs, RESOLVED "
			"INSIDE AddPasses - colour=%p (%s, registered=%d) depth=%p (%s, registered=%d) "
			"velocity=%p (%s, registered=%d). DEPTH AND VELOCITY ARE THE ONES THAT MATTER "
			"(GetSceneTextureParameters registers both externally, so RegisterExternalTexture "
			"has already called SetRHI); colour is the post-chain texture and `rhi_null` here "
			"is EXPECTED, not a failure. These REPLACE the heuristic's register-role guesses "
			"and its liveness verdict for every frame they resolve. Offsets "
			"(FRDGResource::ResourceRHI @%zu, FRHITexture::GetNativeResource slot %u) are what "
			"this line confirms. FRDGTexture in: colour=%p depth=%p velocity=%p (seq %llu, "
			"frame %llu, announced on thread %llu) - paste these with any L1 fault line.",
			reinterpret_cast<void *>(out.colour), seam::rhi_chain_name(out.colour_status),
			out.colour_registered ? 1 : 0,
			reinterpret_cast<void *>(out.depth), seam::rhi_chain_name(out.depth_status),
			out.depth_registered ? 1 : 0,
			reinterpret_cast<void *>(out.velocity), seam::rhi_chain_name(out.velocity_status),
			out.velocity_registered ? 1 : 0,
			seam::kRdgResourceRhiOffset, seam::kRhiGetNativeResourceSlot,
			reinterpret_cast<void *>(v.colour_rdg), reinterpret_cast<void *>(v.depth_rdg),
			reinterpret_cast<void *>(v.velocity_rdg),
			static_cast<unsigned long long>(v.sequence),
			static_cast<unsigned long long>(v.announce_frame),
			static_cast<unsigned long long>(v.announce_thread));
	return out;
}

void note_outcome(seam::SeamRefusal r)
{
	const std::size_t i = static_cast<std::size_t>(r);
	if (i >= static_cast<std::size_t>(seam::SeamRefusal::count))
		return;
	std::lock_guard<std::mutex> lock(g_mutex);
	++g_outcomes[i];
}

// One WARN, once, when the engine's answer and the heuristic's disagree about a resource.
void note_input_disagreement(const char *which, std::uint64_t engine, std::uint64_t heuristic)
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_l1_disagree_logged)
			return;
		g_l1_disagree_logged = true;
	}
	STRAY_LOG_WARN("ENGINE SEAM L1 ASSERTION: the engine's %s is %p and the heuristic's "
		"register walk says %p. The ENGINE's is used. This is not necessarily a bug - the "
		"heuristic infers colour-vs-history from last frame's u0 and can pick the other slot - "
		"but it is the first thing to read if the image is wrong. Once per session.",
		which, reinterpret_cast<void *>(engine), reinterpret_cast<void *>(heuristic));
}

void log_report(const char *when)
{
	seam::Mode m = seam::Mode::off;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		m = g_mode;
	}
	if (m == seam::Mode::off)
		return;
	char line[768] = {};
	format_report(line, sizeof(line));
	// `unclaimed` is the number that must stay at zero: an announced primary upscale we never
	// intercepted is a frame that ran the engine's TAA. `lookalikesRefused` is EXPECTED to
	// grow (the SSD passes ask every frame and are told no); `orphans` counts candidates in
	// frames where the engine announced nothing at all.
	STRAY_LOG_INFO("[seam] %s: %s%s", when != nullptr ? when : "", line,
		m == seam::Mode::authoritative
			? "  (unclaimed must stay 0; lookalikesRefused is expected to grow)" : "");
}

void shutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_patched_slot == nullptr || g_original == nullptr)
		return;
	// Only put the original back if the slot is still ours: another patcher may have chained
	// on top, and clobbering that is worse than leaving ours in place.
	if (*g_patched_slot == reinterpret_cast<void *>(&add_passes_thunk))
	{
		void *ignored = nullptr;
		patch_slot(g_patched_slot, reinterpret_cast<void *>(g_original), ignored);
		STRAY_LOG_INFO("ENGINE SEAM: AddPasses slot restored.");
	}
	else
	{
		STRAY_LOG_WARN("ENGINE SEAM: the AddPasses slot is no longer ours; leaving it alone. "
			"If this DLL unloads, the frame after is a crash.");
	}
	g_patched_slot = nullptr;
	g_original = nullptr;
	g_hooked.store(false, std::memory_order_release);
}

} // namespace stray_dlss::seamhook
