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
// Ledger
// ---------------------------------------------------------------------------------------

std::uint32_t Ledger::expected_groups(std::uint32_t extent)
{
	return (extent + kTaaTileSize - 1u) / kTaaTileSize;
}

void Ledger::begin_frame(std::uint64_t frame)
{
	m_frame = frame;
	std::size_t kept = 0;
	for (std::size_t i = 0; i < m_count; ++i)
	{
		const Announcement &a = m_slots[i];
		if (a.frame + kRetireAfterFrames > frame)
		{
			m_slots[kept++] = a;
			continue;
		}
		if (!a.consumed)
			++m_counters.unclaimed;
	}
	m_count = kept;
}

void Ledger::announce(const Announcement &in)
{
	++m_counters.announced;
	if (m_count >= kCapacity)
	{
		++m_counters.overflow;
		return;
	}
	Announcement a = in;
	a.consumed = false;
	a.sequence = ++m_sequence;
	m_slots[m_count++] = a;
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

std::size_t Ledger::pending() const
{
	std::size_t live = 0;
	for (std::size_t i = 0; i < m_count; ++i)
		if (!m_slots[i].consumed)
			++live;
	return live;
}

} // namespace stray_dlss::seam
