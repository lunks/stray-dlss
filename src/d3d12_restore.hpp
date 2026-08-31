// The native half of restoring the game's compute state, separated so it can be tested.
//
// Observing the state needs ReShade; replaying it does not. Splitting them means the replay —
// where the ordering and type rules actually live, and where getting it wrong corrupts the
// game's next dispatch — can be exercised against WARP in CI, with a hand-built state and a
// golden-output comparison. The ReShade-side half (re-syncing ReShade's own root-signature and
// descriptor-heap caches) remains untestable offline and is documented as such.
#pragma once

#include <cstdint>
#include <utility>
#include <vector>

struct ID3D12GraphicsCommandList;
struct ID3D12RootSignature;
struct ID3D12PipelineState;

namespace stray_dlss {

// A snapshot of the compute state a game had bound, in plain D3D12 terms.
struct NativeComputeState
{
	ID3D12RootSignature *root_signature = nullptr;

	// Indexed by ROOT PARAMETER. A zero handle means "this parameter is not a descriptor
	// table" and must be skipped: issuing SetComputeRootDescriptorTable on a parameter that is
	// really a root CBV is undefined behaviour, and it is what ReShade's own state_block does.
	std::vector<std::uint64_t> tables;

	// Root descriptors, by parameter index, as GPU virtual addresses.
	std::vector<std::pair<std::uint32_t, std::uint64_t>> root_cbv;
	std::vector<std::pair<std::uint32_t, std::uint64_t>> root_srv;
	std::vector<std::pair<std::uint32_t, std::uint64_t>> root_uav;

	// Root 32-bit constants, by parameter index.
	std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> root_constants;

	ID3D12PipelineState *pso = nullptr;
};

// Replays the snapshot onto `cmd`.
//
// Order matters and is not arbitrary: the root signature first, because changing it invalidates
// every root argument, then tables and root arguments, then the PSO. The caller is responsible
// for descriptor heaps being current beforehand — a heap change invalidates descriptor tables,
// so binding tables before the heaps are right would bind them against the wrong heap.
void restore_native_compute_state(ID3D12GraphicsCommandList *cmd, const NativeComputeState &state);

} // namespace stray_dlss
