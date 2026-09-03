#include "core/dxgi_format.hpp"

namespace stray_dlss {

TexFormat tex_format_from_dxgi(std::uint32_t f)
{
	switch (f)
	{
	case dxgi::kR32FloatX8X24Typeless:
	case dxgi::kR32G8X24Typeless:     return TexFormat::r32_float_x8x24_typeless;
	// Depth-stencil VIEW formats, seen when the pass finder describes a draw's DSV. Mapped
	// onto the depth TexFormat so the write-set gate recognises a depth write for what it
	// is: a pass writing one can never be replaced by DLSS. (pass_walk.hpp)
	case dxgi::kD32FloatS8X24Uint:
	case dxgi::kD24UnormS8Uint:
	case dxgi::kD32Float:
	case dxgi::kD16Unorm:             return TexFormat::r32_float_x8x24_typeless;
	case dxgi::kX32TypelessG8X24Uint: return TexFormat::x32_typeless_g8x24_uint;
	case dxgi::kR16G16B16A16Unorm:    return TexFormat::r16g16b16a16_unorm;
	case dxgi::kR16G16B16A16Float:    return TexFormat::r16g16b16a16_float;
	case dxgi::kR16G16Float:          return TexFormat::r16g16_float;
	case dxgi::kR32G32B32A32Float:    return TexFormat::r32g32b32a32_float;
	case dxgi::kR11G11B10Float:       return TexFormat::r11g11b10_float;
	case dxgi::kR10G10B10A2Unorm:     return TexFormat::r10g10b10a2_unorm;
	case dxgi::kR32Float:             return TexFormat::r32_float;
	case dxgi::kR16Float:             return TexFormat::r16_float;
	case dxgi::kR8G8B8A8Unorm:
	case dxgi::kR8G8B8A8UnormSrgb:    return TexFormat::r8g8b8a8_unorm;
	// UE4's PF_B8G8R8A8, the stock GBufferB/C format — created typeless, viewed unorm or
	// srgb. The gbuffer classifier needs to see all three as one thing.
	// (was core/gbuffer_classify.hpp, deleted 2026-09-03)
	case dxgi::kB8G8R8A8Typeless:
	case dxgi::kB8G8R8A8Unorm:
	case dxgi::kB8G8R8A8UnormSrgb:    return TexFormat::b8g8r8a8_unorm;
	default:                          return TexFormat::unknown;
	}
}

const char *dxgi_format_name(std::uint32_t f)
{
	switch (f)
	{
	case dxgi::kR32FloatX8X24Typeless: return "R32_FLOAT_X8X24_TYPELESS";
	case dxgi::kR32G8X24Typeless:      return "R32G8X24_TYPELESS";
	case dxgi::kX32TypelessG8X24Uint:  return "X32_TYPELESS_G8X24_UINT";
	case dxgi::kR16G16B16A16Unorm:     return "R16G16B16A16_UNORM";
	case dxgi::kR16G16B16A16Float:     return "R16G16B16A16_FLOAT";
	case dxgi::kR16G16Float:           return "R16G16_FLOAT";
	case dxgi::kR32G32B32A32Float:     return "R32G32B32A32_FLOAT";
	case dxgi::kR11G11B10Float:        return "R11G11B10_FLOAT";
	case dxgi::kR8G8B8A8Unorm:         return "R8G8B8A8_UNORM";
	case dxgi::kB8G8R8A8Unorm:         return "B8G8R8A8_UNORM";
	case dxgi::kR10G10B10A2Unorm:      return "R10G10B10A2_UNORM";
	case dxgi::kR32Float:              return "R32_FLOAT";
	case dxgi::kR16Float:              return "R16_FLOAT";
	case dxgi::kR8Unorm:               return "R8_UNORM";
	case dxgi::kUnknown:               return "unknown";
	default:                           return "other";
	}
}

} // namespace stray_dlss
