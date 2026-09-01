// The two compute dispatches that wrap the DLSS-NR evaluate: encode the engine's linear HDR
// image into a display-referred proxy, and decode the network's answer back onto it.
//
// WHY. NGX feature 18 is a display-referred image network. We were feeding it Stray's raw
// unbounded pre-exposed linear HDR and getting red noise with a neural output whose max
// luminance read 0.0026 — near black. The runtime has no HDR, colour-space or exposure
// parameter anywhere (exhaustive string search over nvngx_dlssnr.dll), so the conversion has
// to be in our pixels. The math, its provenance and the derivation of the residual transfer
// are in src/core/nr_codec.hpp; the shaders are shaders/nr_{encode,decode}.hlsl over
// shaders/nr_codec.hlsli; CI pins the scalar twin in tests/test_nr_codec.cpp.
//
// Written in raw D3D12 in the gbuffer_resolve / mv_resolve mould and for the same reasons:
// build-time-compiled DXBC (never D3DCompile at runtime — CLAUDE.md §6), our own descriptor
// heap and root signature (NGX clobbers whatever is bound), ring-buffered per-frame
// descriptors and constants (core/ring.hpp), UAVs recreated from resource pointers every frame
// (a descriptor COPY out of the game's shader-visible heap is illegal, and a cached handle can
// alias a destroyed resource — CLAUDE.md §5), and grow-only allocation with retirement (the
// resolution-churn OOM lesson).
//
// The engine image is bound as a UAV rather than an SRV on purpose: it arrives from the SR/RR
// evaluate in D3D12_RESOURCE_STATE_UNORDERED_ACCESS, so reading it through a UAV costs no
// transition pair. The reference deployment binds it the same way.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace stray_dlss::nrp {

// The three bits CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT) reports about a format's
// typed-UAV behaviour, kept separate rather than collapsed to one bool so a caller's log can name
// WHICH half is missing.
struct TypedUavSupport
{
	bool queried = false; // false = CheckFeatureSupport itself failed
	bool view = false;    // D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW
	bool load = false;    // D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD
	bool store = false;   // D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE
};

// THE single typed-UAV probe in this codebase. The codec uses it on the engine's colour image;
// the post-tonemap NR hook uses it on the back buffer, whose DXGI_FORMAT_R10G10B10A2_UNORM
// (CLAUDE.md §2.1) is exactly the kind of format that can fail it. `format` is a DXGI_FORMAT,
// taken as an int so this header stays free of <dxgi.h>.
TypedUavSupport probe_typed_uav(ID3D12Device *device, int format);

// Idempotent; safe to call every frame. `image` is the engine's colour target — the proxy is
// allocated to cover it and its format is validated for typed UAV load/store, because a format
// the device cannot write through a UAV is exactly the "black output with no error" class of
// failure CLAUDE.md §0.2 exists to prevent. Grow-only across resolution changes.
bool initialise(ID3D12Device *device, ID3D12Resource *image, std::uint32_t width,
                std::uint32_t height);

void shutdown();

bool is_ready();

// The proxy image: R16G16B16A16_FLOAT, single-mip, single-slice, non-MSAA, ALLOW_UNORDERED_ACCESS.
// This is what gets bound as DLSSNR.Color — never the engine's raw HDR image.
ID3D12Resource *proxy();

// Records the encode. `scale` is the scene-linear -> display-referred multiplier
// (nrc::proxy_scale of the paper-white setting). Must be called before record_decode in the
// same frame: it claims the ring slot both dispatches share and writes the constants.
bool record_encode(ID3D12GraphicsCommandList *cmd, ID3D12Resource *image, std::uint32_t width,
                   std::uint32_t height, float scale, float color_strength,
                   float transfer_strength);

// Transitions the proxy between UNORDERED_ACCESS (written by the encode, read by the decode)
// and NON_PIXEL_SHADER_RESOURCE (the state NVIDIA's guide wants for an NGX input). Must be
// balanced: every call with `to_shader_resource` true needs its false partner recorded on the
// same list, whatever the evaluate returned.
void transition_proxy(ID3D12GraphicsCommandList *cmd, bool to_shader_resource);

// Records the decode: reads the proxy and the neural output, writes the result over `image`
// in place, preserving its alpha. Uses the ring slot and constants record_encode claimed, so
// it must follow one in the same frame.
bool record_decode(ID3D12GraphicsCommandList *cmd, ID3D12Resource *image,
                   ID3D12Resource *neural, std::uint32_t width, std::uint32_t height);

// Allocation accounting, mirroring gbr::Stats — the churn that OOM'd the GPU once must stay
// measurable.
struct Stats
{
	std::uint64_t resource_sets_created = 0;
	std::uint64_t resource_sets_retired = 0;
	std::uint64_t resource_sets_released = 0;
	std::uint32_t live_retired = 0;
	std::uint64_t bytes_live = 0;
};

const Stats &stats();

const char *last_error();

} // namespace stray_dlss::nrp
