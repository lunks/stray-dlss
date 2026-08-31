#include <doctest/doctest.h>

#include "core/ring.hpp"

#include <set>

using namespace stray_dlss;

TEST_CASE("frame slots cycle and never collide within a ring")
{
	// Any two frames closer together than kFrameCount must land on different slots, or the CPU
	// would rewrite data a still-executing dispatch is reading.
	for (std::uint64_t f = 0; f < 100; ++f)
	{
		std::set<std::uint32_t> window;
		for (std::uint64_t k = 0; k < ring::kFrameCount; ++k)
			window.insert(ring::slot_for_frame(f + k));
		CHECK(window.size() == ring::kFrameCount);
	}
}

TEST_CASE("descriptor slices do not overlap and stay inside the heap")
{
	constexpr std::uint32_t kDescriptorSize = 32; // typical D3D12 CBV/SRV/UAV increment

	std::set<std::size_t> seen;
	for (std::uint32_t slot = 0; slot < ring::kFrameCount; ++slot)
	{
		for (std::uint32_t i = 0; i < ring::kDescriptorsPerFrame; ++i)
		{
			const std::size_t off = ring::descriptor_offset_in_slot(slot, i, kDescriptorSize);

			// Every descriptor in the heap must have a distinct offset. An overlap here means
			// two frames sharing a descriptor, which is precisely the bug being fixed.
			CHECK(seen.insert(off).second);

			// And must lie inside the allocated heap.
			CHECK(off < static_cast<std::size_t>(ring::kDescriptorCount) * kDescriptorSize);
		}
	}
	CHECK(seen.size() == ring::kDescriptorCount);
}

TEST_CASE("a slice's base matches its first descriptor")
{
	constexpr std::uint32_t kDescriptorSize = 32;
	for (std::uint32_t slot = 0; slot < ring::kFrameCount; ++slot)
		CHECK(ring::descriptor_offset(slot, kDescriptorSize) ==
			ring::descriptor_offset_in_slot(slot, 0, kDescriptorSize));
}

TEST_CASE("descriptor offsets scale with the device's descriptor size")
{
	// The increment is queried from the device and is not the same on every driver, so nothing
	// may assume 32.
	CHECK(ring::descriptor_offset_in_slot(1, 0, 64) == ring::kDescriptorsPerFrame * 64u);
	CHECK(ring::descriptor_offset_in_slot(2, 2, 16) == (2u * ring::kDescriptorsPerFrame + 2) * 16u);
}

TEST_CASE("constant stride is 256-byte aligned and large enough")
{
	// D3D12 requires constant buffer views at 256-byte aligned offsets.
	for (std::size_t size : { std::size_t{ 1 }, std::size_t{ 96 }, std::size_t{ 255 },
	                          std::size_t{ 256 }, std::size_t{ 257 }, std::size_t{ 1000 } })
	{
		const std::uint32_t stride = ring::aligned_constant_stride(size);
		CHECK(stride % ring::kConstantAlignment == 0);
		CHECK(stride >= size);
		CHECK(stride - size < ring::kConstantAlignment);
	}
}

TEST_CASE("constant slices do not overlap")
{
	// The resolve's Params struct is 96 bytes: a 4x4 matrix plus four float2s.
	const std::uint32_t stride = ring::aligned_constant_stride(96);
	CHECK(stride == 256);

	for (std::uint32_t a = 0; a < ring::kFrameCount; ++a)
	{
		for (std::uint32_t b = a + 1; b < ring::kFrameCount; ++b)
		{
			const std::size_t oa = ring::constant_offset(a, stride);
			const std::size_t ob = ring::constant_offset(b, stride);
			CHECK(oa != ob);
			CHECK((oa + stride <= ob || ob + stride <= oa));
		}
	}
}

TEST_CASE("retirement waits two full ring cycles")
{
	// Releasing a descriptor heap an in-flight command list still binds is a GPU fault, so the
	// predicate is deliberately conservative rather than minimal.
	constexpr std::uint64_t retired_at = 1000;

	CHECK_FALSE(ring::is_safe_to_release(retired_at, retired_at));
	CHECK_FALSE(ring::is_safe_to_release(retired_at + ring::kFrameCount, retired_at));
	CHECK_FALSE(ring::is_safe_to_release(retired_at + ring::kFrameCount * 2 - 1, retired_at));
	CHECK(ring::is_safe_to_release(retired_at + ring::kFrameCount * 2, retired_at));
	CHECK(ring::is_safe_to_release(retired_at + 1000, retired_at));
}

TEST_CASE("retirement is safe for a resource retired on frame zero")
{
	CHECK_FALSE(ring::is_safe_to_release(0, 0));
	CHECK(ring::is_safe_to_release(ring::kFrameCount * 2, 0));
}
