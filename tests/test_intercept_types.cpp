#include "core/dxgi_format.hpp"
#include "intercept/backend.hpp"
#include "intercept/types.hpp"

#include <doctest/doctest.h>

#include <string>
#include <type_traits>

using namespace stray_dlss;

static_assert(std::is_abstract_v<icept::Backend>, "Backend is an interface");
static_assert(std::is_abstract_v<icept::Sink>, "Sink is an interface");
static_assert(sizeof(icept::ResourceId) == 8 && sizeof(icept::DescriptorId) == 8, "handles are 64-bit");

TEST_CASE("the seam's types build without d3d12.h and DispatchBindings carries BoundTexture as before")
{
	icept::DispatchBindings b;
	b.srvs.push_back(BoundTexture{ 2, 0x1234, TexFormat::r32_float_x8x24_typeless, 1920, 1080, 0xabc, false });
	b.constant_buffers.emplace_back(4u, icept::BufferRange{ 0x99, 256, icept::kUnknownSize });
	CHECK(b.srvs[0].slot == 2);
	CHECK(b.srvs[0].resource == 0x1234);
	CHECK(b.srvs[0].descriptor == 0xabc);
	CHECK(b.constant_buffers[0].first == 4);
	CHECK(b.constant_buffers[0].second.size == icept::kUnknownSize);
	CHECK(b.heap_count == 0);
	CHECK(b.heaps[0] == nullptr);
	CHECK_FALSE(b.view_cb_valid);
	icept::CommandContext ctx;
	CHECK(ctx.native == nullptr);
	CHECK(ctx.backend_cookie == 0);
	icept::PresentContext pc;
	CHECK(pc.present_list == nullptr);
}

TEST_CASE("with no backend installed every query is refused, and set_backend(nullptr) restores that")
{
	icept::Backend *b = icept::backend();
	REQUIRE(b != nullptr);
	CHECK(std::string(b->name()) == "null");
	icept::DispatchBindings db;
	icept::CommandContext ctx;
	CHECK_FALSE(b->resolve_compute_bindings(ctx, db));
	CHECK_FALSE(b->is_resource_live(0));
	CHECK_FALSE(b->is_resource_live(0x1000));
	icept::ResourceInfo ri;
	CHECK_FALSE(b->describe_resource(0x1000, ri));
	icept::ResourceId r = 7;
	CHECK_FALSE(b->resource_from_view(0x1000, r));
	unsigned char buf[16];
	CHECK_FALSE(b->read_buffer(icept::BufferRange{ 0x1000, 0, 16 }, 16, buf));
	struct Fake final : icept::Backend
	{
		const char *name() const override { return "fake"; }
		bool resolve_compute_bindings(const icept::CommandContext &, icept::DispatchBindings &) override { return true; }
		bool describe_resource(icept::ResourceId, icept::ResourceInfo &) override { return true; }
		bool resource_from_view(icept::DescriptorId, icept::ResourceId &) override { return true; }
		bool read_buffer(const icept::BufferRange &, std::uint64_t, void *) override { return true; }
		bool is_resource_live(icept::ResourceId) override { return true; }
		void restore_game_compute_state(const icept::CommandContext &) override {}
		void present_barrier(const icept::PresentContext &, icept::ResourceId, std::uint32_t, std::uint32_t) override {}
		void dump_tracker_state(const icept::CommandContext &, const char *) override {}
	} fake;
	icept::set_backend(&fake);
	CHECK(std::string(icept::backend()->name()) == "fake");
	CHECK(icept::backend()->is_resource_live(1));
	icept::set_backend(nullptr);
	CHECK(std::string(icept::backend()->name()) == "null");
}

TEST_CASE("DXGI format mapping matches the formats measured at the TAA dispatch (CLAUDE.md 2.3)")
{
	// t2 depth SRV / t4 stencil SRV: two views of ONE R32G8X24_TYPELESS resource.
	CHECK(tex_format_from_dxgi(dxgi::kR32FloatX8X24Typeless) == TexFormat::r32_float_x8x24_typeless);
	CHECK(tex_format_from_dxgi(dxgi::kX32TypelessG8X24Uint) == TexFormat::x32_typeless_g8x24_uint);
	CHECK(tex_format_from_dxgi(dxgi::kR32G8X24Typeless) == TexFormat::r32_float_x8x24_typeless);
	// t3 velocity, t1/t5/u0 colour+history, t0 eye adaptation, the menu's scene colour, ours.
	CHECK(tex_format_from_dxgi(dxgi::kR16G16B16A16Unorm) == TexFormat::r16g16b16a16_unorm);
	CHECK(tex_format_from_dxgi(dxgi::kR16G16B16A16Float) == TexFormat::r16g16b16a16_float);
	CHECK(tex_format_from_dxgi(dxgi::kR32G32B32A32Float) == TexFormat::r32g32b32a32_float);
	CHECK(tex_format_from_dxgi(dxgi::kR11G11B10Float) == TexFormat::r11g11b10_float);
	CHECK(tex_format_from_dxgi(dxgi::kR16G16Float) == TexFormat::r16g16_float);
	// The swapchain (CLAUDE.md 2.1) and the G-buffer's typeless/srgb variants collapse.
	CHECK(tex_format_from_dxgi(dxgi::kR10G10B10A2Unorm) == TexFormat::r10g10b10a2_unorm);
	CHECK(tex_format_from_dxgi(dxgi::kB8G8R8A8Typeless) == TexFormat::b8g8r8a8_unorm);
	CHECK(tex_format_from_dxgi(dxgi::kB8G8R8A8UnormSrgb) == TexFormat::b8g8r8a8_unorm);
	CHECK(tex_format_from_dxgi(dxgi::kD32FloatS8X24Uint) == TexFormat::r32_float_x8x24_typeless);
	CHECK(tex_format_from_dxgi(999) == TexFormat::unknown);
	CHECK(std::string(dxgi_format_name(dxgi::kR16G16B16A16Float)) == "R16G16B16A16_FLOAT");
	CHECK(std::string(dxgi_format_name(dxgi::kUnknown)) == "unknown");
	CHECK(std::string(dxgi_format_name(999)) == "other");
	// The numeric values are DXGI_FORMAT's; ReShade's api::format shares them, which is what
	// lets the ReShade backend cast its enum straight through.
	CHECK(dxgi::kR16G16B16A16Float == 10);
	CHECK(dxgi::kB8G8R8A8Unorm == 87);
}
