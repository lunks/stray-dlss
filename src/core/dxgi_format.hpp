// DXGI_FORMAT (as a plain integer) -> the handful of formats the TAA signature distinguishes.
//
// Pure: no d3d12.h, no ReShade. ReShade's api::format enum uses DXGI's numeric values
// (reshade_api_format.hpp), so the ReShade backend passes its enum through unchanged and the
// native backend passes DXGI_FORMAT directly; both land on the same table, which CI tests
// against the formats measured in CLAUDE.md §2.3.
#pragma once

#include "core/taa_signature.hpp"

#include <cstdint>

namespace stray_dlss {

// The DXGI_FORMAT values this project decides on, named so no bare literal ever appears.
namespace dxgi {
constexpr std::uint32_t kUnknown = 0;
constexpr std::uint32_t kR32G32B32A32Float = 2;
constexpr std::uint32_t kR16G16B16A16Float = 10;
constexpr std::uint32_t kR16G16B16A16Unorm = 11;
constexpr std::uint32_t kR32G8X24Typeless = 19;
constexpr std::uint32_t kD32FloatS8X24Uint = 20;
constexpr std::uint32_t kR32FloatX8X24Typeless = 21;
constexpr std::uint32_t kX32TypelessG8X24Uint = 22;
constexpr std::uint32_t kR10G10B10A2Unorm = 24;
constexpr std::uint32_t kR11G11B10Float = 26;
constexpr std::uint32_t kR8G8B8A8Unorm = 28;
constexpr std::uint32_t kR8G8B8A8UnormSrgb = 29;
constexpr std::uint32_t kR16G16Float = 34;
constexpr std::uint32_t kD32Float = 40;
constexpr std::uint32_t kR32Float = 41;
constexpr std::uint32_t kD24UnormS8Uint = 45;
constexpr std::uint32_t kR16Float = 54;
constexpr std::uint32_t kD16Unorm = 55;
constexpr std::uint32_t kR8Unorm = 61;
constexpr std::uint32_t kB8G8R8A8Unorm = 87;
constexpr std::uint32_t kB8G8R8A8Typeless = 90;
constexpr std::uint32_t kB8G8R8A8UnormSrgb = 91;
} // namespace dxgi

TexFormat tex_format_from_dxgi(std::uint32_t dxgi_format);
const char *dxgi_format_name(std::uint32_t dxgi_format);

} // namespace stray_dlss
