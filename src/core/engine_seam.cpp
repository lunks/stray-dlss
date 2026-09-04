#include "core/engine_seam.hpp"

#include <cstring>

namespace stray_dlss::seam {
namespace {

// A qword read that cannot fault: the Image only ever describes memory the caller mapped.
bool read_u64(const Image &image, std::uint64_t va, std::uint64_t &out)
{
	return image.read(va, &out, sizeof(out));
}

// The three fixed instruction shapes, spelled out so the scan reads like the disassembly.
constexpr unsigned char kLeaRaxRip[3] = { 0x48, 0x8D, 0x05 };
constexpr unsigned char kRet = 0xC3;
constexpr std::size_t kLeaRetLength = 8; // 3 opcode + 4 displacement + 1 ret

constexpr unsigned char kMovabsRax[2] = { 0x48, 0xB8 };
constexpr std::size_t kMovabsRetLength = 11; // 2 opcode + 8 immediate + 1 ret

constexpr unsigned char kMovssXmm0Rip[4] = { 0xF3, 0x0F, 0x10, 0x05 };
constexpr unsigned char kVmovssXmm0Rip[4] = { 0xC5, 0xFA, 0x10, 0x05 };
constexpr unsigned char kMovEaxImm = 0xB8;
constexpr unsigned char kMovdXmm0Eax[4] = { 0x66, 0x0F, 0x6E, 0xC0 };

std::int32_t read_i32(const unsigned char *p)
{
	std::uint32_t v = static_cast<std::uint32_t>(p[0]) |
		(static_cast<std::uint32_t>(p[1]) << 8) |
		(static_cast<std::uint32_t>(p[2]) << 16) |
		(static_cast<std::uint32_t>(p[3]) << 24);
	std::int32_t out = 0;
	std::memcpy(&out, &v, sizeof(out));
	return out;
}

// va + instruction_length + displacement, in the 64-bit wraparound the CPU itself uses.
std::uint64_t rip_target(std::uint64_t instruction_va, std::size_t instruction_length,
                         std::int32_t displacement)
{
	const std::uint64_t next = instruction_va + instruction_length;
	return next + static_cast<std::uint64_t>(static_cast<std::int64_t>(displacement));
}

} // namespace

const char *status_text(SeamStatus s)
{
	switch (s)
	{
	case SeamStatus::ok:                   return "ok";
	case SeamStatus::no_regions:           return "no module regions to scan";
	case SeamStatus::name_not_found:
		return "no UTF-16LE \"FDefaultTemporalUpscaler\" anywhere in the image - either this "
		       "is not a UE 4.27 build or the literal was pooled somewhere we do not scan";
	case SeamStatus::debug_name_not_found:
		return "the literal is present but nothing in code returns it as `lea rax,[rip+d]; ret` "
		       "- MSVC emitted a shape this scan does not know";
	case SeamStatus::vtable_not_found:
		return "GetDebugName was found but no 5-slot run of code pointers holds it, so the "
		       "ITemporalUpscaler vtable was not located";
	case SeamStatus::fraction_shape:
		return "the vtable's two fraction accessors are not a shape we can decode statically; "
		       "their bytes are logged so this costs one round trip, not a guess";
	case SeamStatus::fraction_mismatch:
		return "the fraction accessors decoded but do not return 0.5 and 2.0, so the candidate "
		       "is not FDefaultTemporalUpscaler - refusing rather than hooking it";
	case SeamStatus::count:               break;
	}
	return "?";
}

bool Image::read(std::uint64_t va, void *out, std::size_t n) const
{
	if (regions == nullptr || out == nullptr || n == 0)
		return false;
	for (std::size_t i = 0; i < count; ++i)
	{
		const Region &r = regions[i];
		if (r.bytes == nullptr || r.size < n)
			continue;
		if (va < r.va)
			continue;
		const std::uint64_t offset = va - r.va;
		if (offset > r.size - n)
			continue;
		std::memcpy(out, r.bytes + static_cast<std::size_t>(offset), n);
		return true;
	}
	return false;
}

bool Image::is_code(std::uint64_t va) const
{
	if (regions == nullptr)
		return false;
	for (std::size_t i = 0; i < count; ++i)
	{
		const Region &r = regions[i];
		if (!r.executable || r.size == 0)
			continue;
		if (va >= r.va && va - r.va < r.size)
			return true;
	}
	return false;
}

std::size_t find_utf16_literal(const Image &image, const char *ascii,
                               std::uint64_t *out, std::size_t max_out)
{
	if (image.regions == nullptr || ascii == nullptr)
		return 0;

	// Build the needle: each ASCII character as a little-endian UTF-16 unit, plus the
	// terminator, so a longer identifier that merely starts with ours is not a hit.
	unsigned char needle[128];
	std::size_t n = 0;
	for (const char *p = ascii; *p != '\0'; ++p)
	{
		if (n + 4 > sizeof(needle))
			return 0;
		needle[n++] = static_cast<unsigned char>(*p);
		needle[n++] = 0;
	}
	needle[n++] = 0;
	needle[n++] = 0;

	std::size_t hits = 0;
	for (std::size_t i = 0; i < image.count; ++i)
	{
		const Region &r = image.regions[i];
		if (r.bytes == nullptr || r.size < n)
			continue;
		// Wide literals are 2-byte aligned by construction. The first-byte test before the
		// memcmp is not micro-optimisation: this walks every readable section of a ~200 MB
		// shipping executable, once, on the thread that is creating the D3D12 device.
		const unsigned char first = needle[0];
		for (std::size_t off = 0; off + n <= r.size; off += 2)
		{
			if (r.bytes[off] != first || std::memcmp(r.bytes + off, needle, n) != 0)
				continue;
			if (out != nullptr && hits < max_out)
				out[hits] = r.va + off;
			++hits;
		}
	}
	return hits;
}

std::size_t find_lea_ret_to(const Image &image, std::uint64_t target_va,
                            std::uint64_t *out, std::size_t max_out)
{
	if (image.regions == nullptr)
		return 0;
	std::size_t hits = 0;
	for (std::size_t i = 0; i < image.count; ++i)
	{
		const Region &r = image.regions[i];
		if (!r.executable || r.bytes == nullptr || r.size < kLeaRetLength)
			continue;
		for (std::size_t off = 0; off + kLeaRetLength <= r.size; ++off)
		{
			// Same reasoning: byte 0 and byte 7 first, both single compares, before any call.
			if (r.bytes[off] != kLeaRaxRip[0] || r.bytes[off + 7] != kRet)
				continue;
			if (std::memcmp(r.bytes + off, kLeaRaxRip, sizeof(kLeaRaxRip)) != 0)
				continue;
			const std::uint64_t va = r.va + off;
			// The lea itself is 7 bytes; the displacement is relative to what follows it.
			if (rip_target(va, 7, read_i32(r.bytes + off + 3)) != target_va)
				continue;
			if (out != nullptr && hits < max_out)
				out[hits] = va;
			++hits;
		}
	}
	return hits;
}

std::size_t find_movabs_ret_to(const Image &image, std::uint64_t target_va,
                               std::uint64_t *out, std::size_t max_out)
{
	if (image.regions == nullptr)
		return 0;
	std::size_t hits = 0;
	for (std::size_t i = 0; i < image.count; ++i)
	{
		const Region &r = image.regions[i];
		if (!r.executable || r.bytes == nullptr || r.size < kMovabsRetLength)
			continue;
		for (std::size_t off = 0; off + kMovabsRetLength <= r.size; ++off)
		{
			if (r.bytes[off] != kMovabsRax[0] || r.bytes[off + 1] != kMovabsRax[1] ||
				r.bytes[off + 10] != kRet)
				continue;
			std::uint64_t imm = 0;
			std::memcpy(&imm, r.bytes + off + 2, sizeof(imm));
			if (imm != target_va)
				continue;
			if (out != nullptr && hits < max_out)
				out[hits] = r.va + off;
			++hits;
		}
	}
	return hits;
}

std::size_t find_qword(const Image &image, std::uint64_t value,
                       std::uint64_t *out, std::size_t max_out)
{
	if (image.regions == nullptr)
		return 0;
	std::size_t hits = 0;
	for (std::size_t i = 0; i < image.count; ++i)
	{
		const Region &r = image.regions[i];
		if (r.executable || r.bytes == nullptr || r.size < sizeof(std::uint64_t))
			continue;
		// Start at the first 8-byte-aligned virtual address inside the region: a vtable is
		// pointer-aligned, and scanning every byte offset would invent candidates.
		const std::uint64_t first = (r.va + 7u) & ~static_cast<std::uint64_t>(7u);
		for (std::uint64_t va = first; va + sizeof(std::uint64_t) <= r.va + r.size; va += 8)
		{
			std::uint64_t got = 0;
			std::memcpy(&got, r.bytes + static_cast<std::size_t>(va - r.va), sizeof(got));
			if (got != value)
				continue;
			if (out != nullptr && hits < max_out)
				out[hits] = va;
			++hits;
		}
	}
	return hits;
}

bool read_float_constant_return(const Image &image, std::uint64_t fn_va, float *out)
{
	if (out == nullptr)
		return false;
	unsigned char b[16] = {};
	if (!image.read(fn_va, b, sizeof(b)))
		return false;

	// movss xmm0, [rip+d] ; ret  — the ordinary MSVC /O2 shape for `return <float const>;`
	if (std::memcmp(b, kMovssXmm0Rip, sizeof(kMovssXmm0Rip)) == 0 && b[8] == kRet)
	{
		const std::uint64_t pool = rip_target(fn_va, 8, read_i32(b + 4));
		return image.read(pool, out, sizeof(*out));
	}
	// The AVX encoding of the same instruction.
	if (std::memcmp(b, kVmovssXmm0Rip, sizeof(kVmovssXmm0Rip)) == 0 && b[8] == kRet)
	{
		const std::uint64_t pool = rip_target(fn_va, 8, read_i32(b + 4));
		return image.read(pool, out, sizeof(*out));
	}
	// mov eax, imm32 ; movd xmm0, eax ; ret — the immediate carries the float's bit pattern.
	if (b[0] == kMovEaxImm && std::memcmp(b + 5, kMovdXmm0Eax, sizeof(kMovdXmm0Eax)) == 0 &&
		b[9] == kRet)
	{
		std::uint32_t bits = 0;
		std::memcpy(&bits, b + 1, sizeof(bits));
		std::memcpy(out, &bits, sizeof(*out));
		return true;
	}
	return false;
}

Discovery discover(const Image &image)
{
	Discovery d;
	if (image.regions == nullptr || image.count == 0)
	{
		d.status = SeamStatus::no_regions;
		return d;
	}

	// 1. The literal. More than one hit is legal — the linker may pool it per translation
	//    unit — so every hit is tried rather than only the first.
	constexpr std::size_t kMaxNames = 8;
	std::uint64_t names[kMaxNames] = {};
	const std::size_t name_hits = find_utf16_literal(image, kDefaultUpscalerName,
	                                                 names, kMaxNames);
	d.name_hits = static_cast<std::uint32_t>(name_hits);
	if (name_hits == 0)
	{
		d.status = SeamStatus::name_not_found;
		return d;
	}
	d.name_va = names[0];

	// 2. The function that returns it. FDefaultTemporalUpscaler::GetDebugName is
	//    `return TEXT("...")` and nothing else, so at /O2 it is exactly lea+ret.
	constexpr std::size_t kMaxFns = 8;
	std::uint64_t fns[kMaxFns] = {};
	std::size_t fn_hits = 0;
	for (std::size_t i = 0; i < name_hits && i < kMaxNames && fn_hits == 0; ++i)
	{
		fn_hits = find_lea_ret_to(image, names[i], fns, kMaxFns);
		if (fn_hits == 0)
			fn_hits = find_movabs_ret_to(image, names[i], fns, kMaxFns);
		if (fn_hits != 0)
			d.name_va = names[i];
	}
	d.debug_name_hits = static_cast<std::uint32_t>(fn_hits);
	if (fn_hits == 0)
	{
		d.status = SeamStatus::debug_name_not_found;
		return d;
	}
	d.get_debug_name_va = fns[0];

	// 3. The vtable. Any qword holding that function address is a candidate slot 1; the run
	//    of five must all point at code. That filter alone rejects a stray copy of the
	//    pointer in initialised data, and the fraction check below rejects everything else.
	// `failure` remembers the most informative near-miss, so a refusal names the stage it
	// reached rather than only the last thing tried. fraction_mismatch outranks
	// fraction_shape outranks vtable_not_found.
	Discovery failure = d;
	failure.status = SeamStatus::vtable_not_found;
	Discovery winner;
	std::uint32_t validated = 0;

	for (std::size_t f = 0; f < fn_hits && f < kMaxFns; ++f)
	{
		constexpr std::size_t kMaxSlots = 32;
		std::uint64_t slot1[kMaxSlots] = {};
		const std::size_t slot_hits = find_qword(image, fns[f], slot1, kMaxSlots);
		for (std::size_t i = 0; i < slot_hits && i < kMaxSlots; ++i)
		{
			const std::uint64_t vtable = slot1[i] - 8ull * kSlotGetDebugName;
			Discovery c = d;
			c.get_debug_name_va = fns[f];
			c.vtable_va = vtable;

			bool plausible = true;
			for (unsigned s = 0; s < kVtableSlots; ++s)
			{
				if (!read_u64(image, vtable + 8ull * s, c.slot[s]) || !image.is_code(c.slot[s]))
				{
					plausible = false;
					break;
				}
			}
			if (!plausible)
				continue;

			// 4. The self-check. Two accessors, two exactly representable constants, decoded
			//    without executing a single instruction of the candidate.
			float mn = 0.0f;
			float mx = 0.0f;
			if (!read_float_constant_return(image, c.slot[kSlotGetMinFraction], &mn) ||
				!read_float_constant_return(image, c.slot[kSlotGetMaxFraction], &mx))
			{
				c.status = SeamStatus::fraction_shape;
				// Report whichever accessor actually failed, not always the first.
				c.undecoded_va = read_float_constant_return(image, c.slot[kSlotGetMinFraction], &mn)
					? c.slot[kSlotGetMaxFraction]
					: c.slot[kSlotGetMinFraction];
				image.read(c.undecoded_va, c.undecoded, sizeof(c.undecoded));
				if (failure.status == SeamStatus::vtable_not_found)
					failure = c;
				continue;
			}
			c.min_fraction = mn;
			c.max_fraction = mx;
			if (mn != kMinTaaUpsampleResolutionFraction || mx != kMaxTaaUpsampleResolutionFraction)
			{
				c.status = SeamStatus::fraction_mismatch;
				failure = c;
				continue;
			}

			c.status = SeamStatus::ok;
			// Keep the FIRST validated candidate and only count the rest: two vtables that
			// both pass are indistinguishable, and switching mid-scan would make the
			// reported address depend on region order.
			if (validated == 0)
				winner = c;
			++validated;
		}
	}

	if (validated == 0)
		return failure;
	winner.vtable_hits = validated;
	return winner;
}

// ---------------------------------------------------------------------------------------
// L1: the FRDGTexture -> FRHITexture -> GetNativeResource chain
// ---------------------------------------------------------------------------------------

const char *rhi_chain_name(RhiChain c)
{
	switch (c)
	{
	case RhiChain::ok:             return "ok";
	case RhiChain::null_rdg:       return "null-rdg";
	case RhiChain::rhi_null:       return "rhi-null";
	case RhiChain::rhi_unreadable: return "rhi-unreadable";
	case RhiChain::fn_not_code:    return "fn-not-code";
	case RhiChain::count:          break;
	}
	return "?";
}

const char *seam_refusal_name(SeamRefusal r)
{
	switch (r)
	{
	case SeamRefusal::none:            return "evaluated";
	case SeamRefusal::view_unreadable: return "viewUnreadable";
	case SeamRefusal::dead_inputs:     return "deadInputs";
	case SeamRefusal::role_unresolved: return "roleUnresolved";
	case SeamRefusal::bad_render_rect: return "badRenderRect";
	case SeamRefusal::mv_failed:       return "mvFailed";
	case SeamRefusal::create_failed:   return "createFailed";
	case SeamRefusal::eval_failed:     return "evalFailed";
	case SeamRefusal::count:           break;
	}
	return "?";
}

RhiChain resolve_rhi_fn(const RdgReader &r, std::uint64_t rdg,
                        std::uint64_t *out_rhi, std::uint64_t *out_fn)
{
	if (out_rhi != nullptr)
		*out_rhi = 0;
	if (out_fn != nullptr)
		*out_fn = 0;
	if (r.read_u64 == nullptr || r.is_code == nullptr)
		return RhiChain::rhi_unreadable;
	if (rdg == 0)
		return RhiChain::null_rdg;

	// FRDGResource::ResourceRHI. Null is the ordinary answer for a texture the graph has not
	// allocated yet, and it is NOT an error — it is why this runs at dispatch time rather than
	// at AddPasses time.
	std::uint64_t rhi = 0;
	if (!r.read_u64(r.ctx, rdg + kRdgResourceRhiOffset, &rhi))
		return RhiChain::rhi_unreadable;
	if (rhi == 0)
		return RhiChain::rhi_null;

	// The FRHITexture's vtable, then the GetNativeResource slot in it.
	std::uint64_t vtable = 0;
	if (!r.read_u64(r.ctx, rhi, &vtable) || vtable == 0)
		return RhiChain::rhi_unreadable;
	std::uint64_t fn = 0;
	if (!r.read_u64(r.ctx, vtable + 8ull * kRhiGetNativeResourceSlot, &fn) || fn == 0)
		return RhiChain::rhi_unreadable;
	// The last guard before the live half calls it. A wrong offset that survived every read
	// above almost always lands here, and a refusal costs one fallback frame.
	if (!r.is_code(r.ctx, fn))
		return RhiChain::fn_not_code;

	if (out_rhi != nullptr)
		*out_rhi = rhi;
	if (out_fn != nullptr)
		*out_fn = fn;
	return RhiChain::ok;
}

// ---------------------------------------------------------------------------------------
// Mode and gate
// ---------------------------------------------------------------------------------------

Mode mode_from_level(int level)
{
	if (level <= 0) return Mode::off;
	if (level == 1) return Mode::discover;
	if (level == 2) return Mode::observe;
	return Mode::authoritative;
}

const char *mode_name(Mode m)
{
	switch (m)
	{
	case Mode::off:           return "off";
	case Mode::discover:      return "discover";
	case Mode::observe:       return "observe";
	case Mode::authoritative: return "authoritative";
	}
	return "?";
}

const char *gate_name(Gate g)
{
	switch (g)
	{
	case Gate::heuristic:            return "heuristic";
	case Gate::engine:               return "engine";
	case Gate::refuse_not_announced: return "refuse-not-announced";
	case Gate::refuse_no_seam:       return "refuse-no-seam";
	}
	return "?";
}

Gate decide(const GateInputs &in)
{
	if (in.mode != Mode::authoritative)
		return Gate::heuristic;
	if (!in.hooked)
		return in.fallback_allowed ? Gate::heuristic : Gate::refuse_no_seam;
	return in.announced ? Gate::engine : Gate::refuse_not_announced;
}

// ---------------------------------------------------------------------------------------
// Ledger
// ---------------------------------------------------------------------------------------

const char *l1_gate_name(L1Gate g)
{
	switch (g)
	{
	case L1Gate::off:     return "off";
	case L1Gate::faulted: return "faulted";
	case L1Gate::resolve: return "resolve";
	}
	return "?";
}

L1Gate l1_gate(const L1GateInputs &in)
{
	// The off-switch first and unconditionally, so EngineSeamInputs=0 can never reach a
	// dereference by any other route — including the fault latch.
	if (!in.inputs_enabled || in.mode != Mode::authoritative || !in.hooked || !in.announced)
		return L1Gate::off;
	if (in.faulted)
		return L1Gate::faulted;
	return L1Gate::resolve;
}

bool announcement_is_fresh(const Freshness &f)
{
	// Newest: any newer announcement means a newer FRDGBuilder exists, and the older one's
	// allocator has been (or is about to be) reset under us.
	if (f.announce_sequence == 0 || f.announce_sequence != f.ledger_sequence)
		return false;
	// Same frame: a present between announce and claim means the graph completed.
	if (f.announce_frame != f.current_frame)
		return false;
	// AND NOTHING ABOUT THE THREAD. The first version of this predicate also demanded
	// `announce_thread == current_thread`, and it was WRONG — structurally, not marginally.
	// It made L1 inert on the box: `stale=4147` of 4147 claims, `resolved=0`, every claim
	// declined with a stable pair (announced on 1400, claimed on 1152) all session. UE 4.27
	// calls `ITemporalUpscaler::AddPasses` during RDG graph SETUP and issues the dispatch
	// during graph EXECUTION, and those are not required to be the same thread.
	//
	// The reasoning error is worth naming because it is easy to repeat: thread identity
	// governs OWNERSHIP, not VALIDITY. `FRDGBuilder` being a stack object means the
	// ANNOUNCING thread's frame must not have returned — it does not mean only that thread
	// may read what the allocator holds. Memory owned by a live stack frame is readable from
	// any thread, and if the engine can read `ResourceRHI` on the recording thread (it must,
	// to bind the texture) then so can we. The two conditions above are the lifetime, and
	// they are sufficient: a newer announcement means a newer graph, and a present between
	// announce and claim means the announcing graph has completed. Neither is about threads.
	//
	// The thread ids stay in this struct because the CALLER reports them — the pair is
	// latched and a change is worth one WARN — but nothing is gated on them.
	return true;
}

std::uint32_t Ledger::expected_groups(std::uint32_t extent)
{
	return (extent + kTaaTileSize - 1u) / kTaaTileSize;
}

void Ledger::retire_stale()
{
	std::size_t kept = 0;
	for (std::size_t i = 0; i < m_count; ++i)
	{
		const Announcement &a = m_slots[i];
		const bool stale_by_sequence = a.sequence + kRetireAfterAnnouncements <= m_sequence;
		const bool stale_by_present = a.frame + kRetireAfterFrames <= m_frame;
		if (!stale_by_sequence && !stale_by_present)
		{
			m_slots[kept++] = a;
			continue;
		}
		if (!a.consumed)
			++m_counters.unclaimed;
	}
	m_count = kept;
}

void Ledger::begin_frame(std::uint64_t frame)
{
	m_frame = frame;
	retire_stale();
}

void Ledger::announce(const Announcement &in)
{
	++m_counters.announced;
	// Make room the honest way first: anything already dead by the engine's own clock retires
	// (and is counted) before the ring can be called full.
	retire_stale();
	if (m_count >= kCapacity)
	{
		++m_counters.overflow;
		return;
	}
	Announcement a = in;
	a.consumed = false;
	a.sequence = ++m_sequence;
	m_slots[m_count++] = a;
	// The engine's own clock has ticked; anything four announcements old is dead.
	retire_stale();
}

const Announcement *Ledger::claim(std::uint32_t group_x, std::uint32_t group_y)
{
	std::size_t live = 0;
	for (std::size_t i = 0; i < m_count; ++i)
		if (!m_slots[i].consumed)
			++live;

	if (live == 0)
	{
		++m_counters.orphans;
		return nullptr;
	}
	for (std::size_t i = 0; i < m_count; ++i)
	{
		Announcement &a = m_slots[i];
		if (a.consumed)
			continue;
		if (expected_groups(a.out_width) != group_x || expected_groups(a.out_height) != group_y)
			continue;
		a.consumed = true;
		++m_counters.claimed;
		return &a;
	}
	++m_counters.rect_mismatch;
	return nullptr;
}

bool Ledger::note_unmatched(std::uint32_t group_x, std::uint32_t group_y)
{
	for (std::size_t i = 0; i < m_count; ++i)
	{
		const Announcement &a = m_slots[i];
		if (a.consumed)
			continue;
		if (expected_groups(a.out_width) == group_x && expected_groups(a.out_height) == group_y)
		{
			++m_counters.near_misses;
			return true;
		}
	}
	return false;
}

std::size_t Ledger::pending() const
{
	std::size_t live = 0;
	for (std::size_t i = 0; i < m_count; ++i)
		if (!m_slots[i].consumed)
			++live;
	return live;
}

} // namespace stray_dlss::seam
