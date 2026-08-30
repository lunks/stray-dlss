// Platform-independent. Compiles and is unit-tested on Linux as well as MSVC.
#pragma once

#include <cstddef>
#include <cstdint>

namespace stray_dlss {

// fnv1a64 over a shader's DXBC. This is the hash the measured shader identities in
// CLAUDE.md §2.3 are expressed in, so the constants and the seed must not change:
//   0x1708ec956099e259  the FTAAStandaloneCS Main/Fast/Downsample permutation we hook
//   0x52101a15e1a0c5cc  a second candidate; almost certainly motion blur, do NOT hook
//   0x901e041a7cadc9db  a measured false positive; no heuristic may ever select it
constexpr std::uint64_t kFnv1a64OffsetBasis = 0xcbf29ce484222325ull;
constexpr std::uint64_t kFnv1a64Prime = 0x100000001b3ull;

constexpr std::uint64_t fnv1a64(const void *data, std::size_t size) noexcept
{
	const auto *bytes = static_cast<const unsigned char *>(data);
	std::uint64_t hash = kFnv1a64OffsetBasis;
	for (std::size_t i = 0; i < size; ++i)
	{
		hash ^= static_cast<std::uint64_t>(bytes[i]);
		hash *= kFnv1a64Prime;
	}
	return hash;
}

} // namespace stray_dlss
