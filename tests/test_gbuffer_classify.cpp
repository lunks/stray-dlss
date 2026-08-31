// The G-buffer classifier, proven on synthetic render-target sets — SECOND ITERATION,
// against the corrected velocity-FREE signature (docs/RESEARCH-RR-GBUFFER.md §1.5).
//
// These tests are the correctness proof for the G-buffer finder (DLSS-RR): the developer
// cannot run the game, so every discrimination is pinned here. The PRIMARY case is the
// shape the live game measurably binds (2026-08-31): a 6-RTV set — R11G11B10F scene
// colour (r.SceneColorFormat=3), RGB10A2 GBufferA, two 8-bit RGBA (B, C), two more 8-bit
// RGBA (D, E) — plus the R32G8X24 DSV, with NO velocity member. The old stock-UE
// shape with velocity at slot 4 stays as the SECONDARY accept (corroboration): Stray's
// own inis predict it and other titles or config changes may bind it. Velocity absence
// is the expected world, never a rejection.

#include <doctest/doctest.h>

#include "core/gbuffer_classify.hpp"

#include <cstring>
#include <string>

using namespace stray_dlss;

namespace {

// Resource identities. Values are arbitrary; only equality matters.
constexpr std::uint64_t kSceneColourRes = 0x100;
constexpr std::uint64_t kGBufferARes = 0x200;
constexpr std::uint64_t kGBufferBRes = 0x300;
constexpr std::uint64_t kGBufferCRes = 0x400;
constexpr std::uint64_t kGBufferDRes = 0x500;
constexpr std::uint64_t kGBufferERes = 0x600;
constexpr std::uint64_t kVelocityRes = 0x700;
constexpr std::uint64_t kDepthRes = 0xD00;
constexpr std::uint64_t kDummyRes = 0x9999; // the 1x1 BlackDummy

// The scene-buffer extent at the measured 2560x1440 output.
constexpr std::uint32_t kW = 2560, kH = 1440;

BoundTexture tex(std::uint32_t slot, std::uint64_t res, TexFormat f,
                 std::uint32_t w = kW, std::uint32_t h = kH)
{
	return BoundTexture{ slot, res, f, w, h };
}

// PRIMARY: the shape the live game binds (RESEARCH-RR-GBUFFER.md §1.4-1.5) — 6 RTVs, no
// velocity, DSV appended by the recorder at slot = RTV count (6).
std::vector<BoundTexture> stray_set()
{
	return {
		tex(0, kSceneColourRes, TexFormat::r11g11b10_float),
		tex(1, kGBufferARes, TexFormat::r10g10b10a2_unorm),
		tex(2, kGBufferBRes, TexFormat::b8g8r8a8_unorm),
		tex(3, kGBufferCRes, TexFormat::b8g8r8a8_unorm),
		tex(4, kGBufferDRes, TexFormat::b8g8r8a8_unorm),
		tex(5, kGBufferERes, TexFormat::b8g8r8a8_unorm),
		tex(6, kDepthRes, TexFormat::r32_float_x8x24_typeless),
	};
}

// SECONDARY: stock 4.27.2 with base-pass velocity, as Stray's own inis predict — 7 RTVs,
// velocity at slot 4 (`check(OutVelocityRTIndex == 4)`), D/E shifted, DSV at slot 7.
std::vector<BoundTexture> stock_velocity_set()
{
	return {
		tex(0, kSceneColourRes, TexFormat::r16g16b16a16_float),
		tex(1, kGBufferARes, TexFormat::r10g10b10a2_unorm),
		tex(2, kGBufferBRes, TexFormat::b8g8r8a8_unorm),
		tex(3, kGBufferCRes, TexFormat::b8g8r8a8_unorm),
		tex(4, kVelocityRes, TexFormat::r16g16b16a16_unorm),
		tex(5, kGBufferDRes, TexFormat::b8g8r8a8_unorm),
		tex(6, kGBufferERes, TexFormat::b8g8r8a8_unorm),
		tex(7, kDepthRes, TexFormat::r32_float_x8x24_typeless),
	};
}

const GBufferTarget *find_role(const GBufferClassification &c, GBufferRole role)
{
	for (const auto &t : c.targets)
		if (t.role == role)
			return &t;
	return nullptr;
}

const GBufferTarget *find_slot(const GBufferClassification &c, std::uint32_t slot)
{
	for (const auto &t : c.targets)
		if (t.tex.slot == slot && t.role != GBufferRole::depth)
			return &t;
	return nullptr;
}

} // namespace

TEST_CASE("the measured Stray 6-RTV velocity-free set is the primary accept")
{
	const GBufferClassification c = classify_render_target_set(stray_set());

	CHECK(c.is_base_pass);
	CHECK_FALSE(c.velocity_corroborated);
	CHECK(c.stock_roles_matched == 4);
	CHECK(c.extent_width == kW);
	CHECK(c.extent_height == kH);
	CHECK(c.targets.size() == 7);

	const GBufferTarget *sc = find_role(c, GBufferRole::scene_colour);
	REQUIRE(sc != nullptr);
	CHECK(sc->tex.slot == 0);
	CHECK(sc->tex.resource == kSceneColourRes);
	CHECK(sc->stock_format);

	const GBufferTarget *a = find_role(c, GBufferRole::gbuffer_a);
	REQUIRE(a != nullptr);
	CHECK(a->tex.slot == 1);
	CHECK(a->tex.resource == kGBufferARes);

	const GBufferTarget *b = find_role(c, GBufferRole::gbuffer_b);
	REQUIRE(b != nullptr);
	CHECK(b->tex.slot == 2);
	CHECK(b->tex.resource == kGBufferBRes);

	const GBufferTarget *cc = find_role(c, GBufferRole::gbuffer_c);
	REQUIRE(cc != nullptr);
	CHECK(cc->tex.slot == 3);
	CHECK(cc->tex.resource == kGBufferCRes);

	// No velocity role anywhere, and slots 4/5 are the D/E positions: unknown, noted.
	CHECK(find_role(c, GBufferRole::velocity) == nullptr);
	const GBufferTarget *d = find_slot(c, 4);
	REQUIRE(d != nullptr);
	CHECK(d->role == GBufferRole::unknown);
	CHECK(std::string(d->note).find("GBufferD") != std::string::npos);
	const GBufferTarget *e = find_slot(c, 5);
	REQUIRE(e != nullptr);
	CHECK(std::string(e->note).find("GBufferE") != std::string::npos);

	const GBufferTarget *depth = find_role(c, GBufferRole::depth);
	REQUIRE(depth != nullptr);
	CHECK(depth->tex.resource == kDepthRes);
}

TEST_CASE("the stock 7-RTV set with velocity at slot 4 is the secondary accept")
{
	const GBufferClassification c = classify_render_target_set(stock_velocity_set());

	CHECK(c.is_base_pass);
	CHECK(c.velocity_corroborated);
	CHECK(c.stock_roles_matched == 4);

	const GBufferTarget *v = find_role(c, GBufferRole::velocity);
	REQUIRE(v != nullptr);
	CHECK(v->tex.slot == kStockVelocitySlot);

	// D/E shift one slot when velocity occupies slot 4. (SceneRenderTargets.cpp:734-779)
	const GBufferTarget *d = find_slot(c, 5);
	REQUIRE(d != nullptr);
	CHECK(std::string(d->note).find("GBufferD") != std::string::npos);
	const GBufferTarget *e = find_slot(c, 6);
	REQUIRE(e != nullptr);
	CHECK(std::string(e->note).find("GBufferE") != std::string::npos);
}

TEST_CASE("the minimal 5-RTV stock set (SC,A,B,C,velocity) still classifies")
{
	auto set = stock_velocity_set();
	set.erase(set.begin() + 5, set.begin() + 7); // drop D and E, keep the DSV
	set.back().slot = 5;                         // recorder appends the DSV at count = 5
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(c.velocity_corroborated);
}

TEST_CASE("B/C are format-indistinguishable and the ambiguity is REPORTED")
{
	// Both classified, both 8-bit RGBA: the assignment rests on the stock slot order
	// alone, and the report must say so rather than presenting it as measured.
	CHECK(classify_render_target_set(stray_set()).bc_order_by_slot_only);
	CHECK(classify_render_target_set(stock_velocity_set()).bc_order_by_slot_only);
}

TEST_CASE("RGBA16F scene colour is accepted too - the format is not an invariant")
{
	// Measured: menu scene colour is R11G11B10F where gameplay's TAA input is RGBA16F
	// (CLAUDE.md §5 gotchas). Both classify.
	auto set = stray_set();
	set[0].format = TexFormat::r16g16b16a16_float;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(find_role(c, GBufferRole::scene_colour) != nullptr);
	CHECK(c.stock_roles_matched == 4);
}

TEST_CASE("channel-swapped RGBA8 is accepted for GBufferB/C")
{
	auto set = stray_set();
	set[2].format = TexFormat::r8g8b8a8_unorm;
	set[3].format = TexFormat::r8g8b8a8_unorm;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(find_role(c, GBufferRole::gbuffer_b) != nullptr);
	CHECK(find_role(c, GBufferRole::gbuffer_c) != nullptr);
	CHECK(c.bc_order_by_slot_only);
}

TEST_CASE("slot 1 off the RGB10A2 anchor is rejected loudly, members still reported")
{
	// The anchor: GBufferA's RGB10A2 is the only such render target in a deferred frame.
	// An off-format slot 1 is the licensee-delta tripwire and must refuse, not guess.
	auto set = stray_set();
	set[1].format = TexFormat::r16g16b16a16_float;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("GBufferA anchor") != std::string::npos);
	CHECK(c.targets.size() == 7); // the members ARE the diagnostic on a rejection
}

TEST_CASE("a licensee RGBA16F GBufferC is unknown-but-logged, never misclassified")
{
	auto set = stray_set();
	set[3].format = TexFormat::r16g16b16a16_float;
	const GBufferClassification c = classify_render_target_set(set);

	// The SET still carries the signature (anchor at slot 1 intact)...
	CHECK(c.is_base_pass);
	// ...but slot 3 is NOT called GBufferC on format evidence it does not have.
	CHECK(find_role(c, GBufferRole::gbuffer_c) == nullptr);
	CHECK(c.stock_roles_matched == 3);
	CHECK_FALSE(c.bc_order_by_slot_only); // only one 8-bit RGBA in B/C: no ambiguity

	const GBufferTarget *slot3 = find_slot(c, 3);
	REQUIRE(slot3 != nullptr);
	CHECK(slot3->role == GBufferRole::unknown);
	CHECK_FALSE(slot3->stock_format);
	CHECK(slot3->note[0] != 0); // the deviation is named for the log
}

TEST_CASE("an off-format scene colour is unknown, the rest still classify")
{
	auto set = stray_set();
	set[0].format = TexFormat::b8g8r8a8_unorm;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(find_role(c, GBufferRole::scene_colour) == nullptr);
	CHECK(find_role(c, GBufferRole::gbuffer_a) != nullptr);
	CHECK(c.stock_roles_matched == 3);
}

TEST_CASE("a velocity-format target away from slot 4 refuses the layout")
{
	// `check(OutVelocityRTIndex == 4)` is stock; anywhere else means the slot map cannot
	// be trusted and no positional claim survives.
	auto set = stray_set();
	set[5] = tex(5, kVelocityRes, TexFormat::r16g16b16a16_unorm);
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("stock slot 4") != std::string::npos);
}

TEST_CASE("two velocity-format targets are ambiguous, refused")
{
	auto set = stock_velocity_set();
	set[6] = tex(6, 0x777, TexFormat::r16g16b16a16_unorm);
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("ambiguous") != std::string::npos);
}

TEST_CASE("1x1 dummies are ignored, not classified")
{
	auto set = stray_set();
	set.push_back(tex(7, kDummyRes, TexFormat::r16g16b16a16_float, 1, 1));
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(c.targets.size() == 7); // the dummy never appears
	for (const auto &t : c.targets)
		CHECK(t.tex.resource != kDummyRes);
}

TEST_CASE("a 1x1 velocity-format dummy neither anchors nor rejects anything")
{
	// Velocity is no longer a signal in either direction: a BlackDummy in the set is
	// simply dropped and the velocity-free classification proceeds unchanged.
	auto set = stray_set();
	set.push_back(tex(7, kDummyRes, TexFormat::r16g16b16a16_unorm, 1, 1));
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK_FALSE(c.velocity_corroborated);
}

TEST_CASE("fewer than 5 colour targets is not a G-buffer set")
{
	// SC+A+B+C alone (4 colour targets + DSV): stock always adds GBufferD, so a 4-wide
	// set is something else — and the old velocity-free menu world produced exactly zero
	// of these anyway.
	std::vector<BoundTexture> set = {
		tex(0, kSceneColourRes, TexFormat::r11g11b10_float),
		tex(1, kGBufferARes, TexFormat::r10g10b10a2_unorm),
		tex(2, kGBufferBRes, TexFormat::b8g8r8a8_unorm),
		tex(3, kGBufferCRes, TexFormat::b8g8r8a8_unorm),
		tex(4, kDepthRes, TexFormat::r32_float_x8x24_typeless),
	};
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("fewer than 5") != std::string::npos);
}

TEST_CASE("a colour target at a different extent breaks the set")
{
	auto set = stray_set();
	set[4].width = kW / 2;
	set[4].height = kH / 2;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("extent") != std::string::npos);
}

TEST_CASE("a gap in slots 0-3 is rejected, not papered over")
{
	auto set = stray_set();
	set.erase(set.begin() + 2); // GBufferB's RTV was null: colour slots 0,1,3,4,5
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("slots 0-3") != std::string::npos);
}

TEST_CASE("duplicate slots are refused")
{
	auto set = stray_set();
	set[3].slot = 2; // two colour targets claim slot 2
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("duplicate") != std::string::npos);
}

TEST_CASE("the RGB10A2 swapchain alone is not the G-buffer")
{
	// The swapchain shares GBufferA's format but is bound alone (and at output extent):
	// the width gate rejects it long before the anchor can fire. (§1.5)
	std::vector<BoundTexture> set = { tex(0, 0x1234, TexFormat::r10g10b10a2_unorm) };
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
}

TEST_CASE("the stencil view of the depth resource is depth too")
{
	auto set = stray_set();
	set[6].format = TexFormat::x32_typeless_g8x24_uint;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(find_role(c, GBufferRole::depth) != nullptr);
}

TEST_CASE("empty and dummy-only sets degrade to a clear reason")
{
	CHECK_FALSE(classify_render_target_set({}).is_base_pass);

	std::vector<BoundTexture> dummies = {
		tex(0, kDummyRes, TexFormat::r16g16b16a16_float, 1, 1),
	};
	const GBufferClassification c = classify_render_target_set(dummies);
	CHECK_FALSE(c.is_base_pass);
	CHECK(c.reason[0] != 0);
}

TEST_CASE("role and format names cover every enumerator")
{
	// The names end up in the pasted log; "?" would mean an enumerator was added without
	// its name, which the log must never show.
	CHECK(std::strcmp(gbuffer_role_name(GBufferRole::scene_colour), "scene-colour") == 0);
	CHECK(std::strcmp(gbuffer_role_name(GBufferRole::gbuffer_a), "GBufferA") == 0);
	CHECK(std::strcmp(gbuffer_role_name(GBufferRole::gbuffer_b), "GBufferB") == 0);
	CHECK(std::strcmp(gbuffer_role_name(GBufferRole::gbuffer_c), "GBufferC") == 0);
	CHECK(std::strcmp(gbuffer_role_name(GBufferRole::velocity), "velocity") == 0);
	CHECK(std::strcmp(gbuffer_role_name(GBufferRole::depth), "depth") == 0);
	CHECK(std::strcmp(gbuffer_role_name(GBufferRole::unknown), "unknown") == 0);

	CHECK(std::strcmp(tex_format_name(TexFormat::b8g8r8a8_unorm), "BGRA8") == 0);
	CHECK(std::strcmp(tex_format_name(TexFormat::r10g10b10a2_unorm), "RGB10A2") == 0);
	CHECK(std::strcmp(tex_format_name(TexFormat::r11g11b10_float), "R11G11B10F") == 0);
	CHECK(std::strcmp(tex_format_name(TexFormat::r16g16b16a16_unorm), "RGBA16_UNORM") == 0);
}
