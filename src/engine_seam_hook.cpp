#include "engine_seam_hook.hpp"

#include "core/engine_seam.hpp"
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

void add_passes_thunk(const void *self, void *graph_builder, const void *view,
                      const void *pass_inputs, void **out_colour, void *out_rect,
                      void **out_half_colour, void *out_half_rect)
{
	// Read FPassInputs BEFORE forwarding. The three FRDGTexture pointers are taken for
	// IDENTITY only and are never dereferenced: at this point the graph has not executed, so
	// a transient texture has no RHI resource yet (RenderGraphBuilder.cpp:1222-1233 assigns
	// them, :1300-1312 runs the passes), and FRDGTexture's own layout is not established.
	Announcement a;
	a.frame = g_frame.load(std::memory_order_relaxed);
	if (pass_inputs != nullptr)
	{
		a.colour_rdg = read_ptr(pass_inputs, seam::kPassInputsSceneColor);
		a.depth_rdg = read_ptr(pass_inputs, seam::kPassInputsSceneDepth);
		a.velocity_rdg = read_ptr(pass_inputs, seam::kPassInputsSceneVelocity);
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

} // namespace

void configure(int level, bool fallback)
{
	const seam::Mode m = seam::mode_from_level(level);
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_mode = m;
		g_fallback_allowed = fallback;
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
		v.out_width = a->out_width;
		v.out_height = a->out_height;
		v.sequence = a->sequence;
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
	return std::snprintf(buffer, size,
		"seam=%s mode=%s hooked=%d announced=%llu claimed=%llu unclaimed=%llu orphans=%llu "
		"lookalikesRefused=%llu overflow=%llu unreadableRect=%llu",
		on ? "found" : "off", seam::mode_name(m), hooked() ? 1 : 0,
		static_cast<unsigned long long>(c.announced),
		static_cast<unsigned long long>(c.claimed),
		static_cast<unsigned long long>(c.unclaimed),
		static_cast<unsigned long long>(c.orphans),
		static_cast<unsigned long long>(c.rect_mismatch),
		static_cast<unsigned long long>(c.overflow),
		static_cast<unsigned long long>(g_unreadable.load(std::memory_order_relaxed)));
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
	char line[320] = {};
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
