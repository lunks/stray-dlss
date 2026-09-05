// NR at MODEL RESOLUTION - the D3D12 half. NGX-free on purpose, like nr_stage and nr_mask, so
// the WARP harness could judge the two dispatches if it ever needs to.
//
// Owns: two small colour textures at the model's extent (R16G16B16A16_FLOAT, typed UAV store on
// every device), one shader-visible descriptor heap ring, two root signatures and two PSOs.
// Records, on the present list: the exact-area downsample of the stage's full-resolution
// staging copy into both small textures, and after feature 18 has answered into one of them,
// the matched-residual resolve back over the staging copy in place. See core/nr_model_plan.hpp
// for what is decided and why, and shaders/nr_{downsample,resolve}.hlsl for the arithmetic.
//
// States: the small textures rest at UNORDERED_ACCESS; record_downsample leaves both at
// NON_PIXEL_SHADER_RESOURCE (0x40) - `model()` in the state nr::apply assumes for its image,
// `shown()` ready for the resolve's SRV - and record_resolve puts both back. The full staging
// copy rests at nrstage::kStagingRestState (0x40) and is transitioned to UAV for the resolve
// and back; record_downsample reads it as an SRV at rest with no transition.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::nrmodel {

// Creates or resizes the small textures. `full_format` is the staging copy's DXGI format, which
// the resolve writes through a typed UAV; the plan has already refused formats that cannot.
bool ensure(ID3D12Device *device, std::uint32_t width, std::uint32_t height, int full_format,
	std::uint64_t frame);

// Full staging (SRV, at rest) -> shown + model (both small). Leaves both small textures at 0x40.
bool record_downsample(ID3D12GraphicsCommandList *cmd, ID3D12Resource *full,
	std::uint32_t full_width, std::uint32_t full_height);

// The texture feature 18 is given as its image (holds the model's answer afterwards), and the
// untouched copy of what it was shown.
ID3D12Resource *model();
ID3D12Resource *shown();

// Full staging <- full + upsample(model - shown), cube-scaled. Transitions the staging copy
// 0x40 -> UAV -> 0x40 and the small textures 0x40 -> UAV (their rest state).
bool record_resolve(ID3D12GraphicsCommandList *cmd, ID3D12Resource *full,
	std::uint32_t full_width, std::uint32_t full_height, float transfer_strength);

void collect(std::uint64_t frame);
void shutdown();
const char *last_error();

struct Stats
{
	std::uint64_t downsamples = 0;
	std::uint64_t resolves = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint64_t bytes = 0;
};
Stats stats();

} // namespace stray_dlss::nrmodel
