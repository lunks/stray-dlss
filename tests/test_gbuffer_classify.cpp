// The G-buffer classifier, proven on synthetic UE-4.27-shaped render-target sets.
//
// These tests are the correctness proof for the G-buffer finder (DLSS-RR phase 1): the
// developer cannot run the game, so every discrimination the classifier must make — the
// stock base-pass MRT set in, sets without the velocity signature out, licensee format
// variants reported as unknown rather than misclassified, the B/C slot-order ambiguity
// surfaced — is pinned here instead. Extents and formats follow the measured configuration
// (2560x1440 output, CLAUDE.md §2.1) and the stock layout documented with provenance in
// core/gbuffer_classify.hpp.

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
constexpr std::uint64_t kVelocityRes = 0x500;
constexpr std::uint64_t kGBufferDRes = 0x600;
constexpr std::uint64_t kDepthRes = 0xD00;
constexpr std::uint64_t kDummyRes = 0x9999; // the 1x1 BlackDummy

// The scene-buffer extent at the measured 2560x1440 output.
constexpr std::uint32_t kW = 2560, kH = 1440;

BoundTexture tex(std::uint32_t slot, std::uint64_t res, TexFormat f,
                 std::uint32_t w = kW, std::uint32_t h = kH)
{
	return BoundTexture{ slot, res, f, w, h };
}

// The stock UE 4.27 base-pass MRT set as the recorder would describe it: RTVs 0-4 in slot
// order, DSV appended at slot 5 (= the RTV count, pass_finder-style). Provenance for every
// format: core/gbuffer_classify.hpp header.
std::vector<BoundTexture> stock_set()
{
	return {
		tex(0, kSceneColourRes, TexFormat::r16g16b16a16_float),
		tex(1, kGBufferARes, TexFormat::r10g10b10a2_unorm),
		tex(2, kGBufferBRes, TexFormat::b8g8r8a8_unorm),
		tex(3, kGBufferCRes, TexFormat::b8g8r8a8_unorm),
		tex(4, kVelocityRes, TexFormat::r16g16b16a16_unorm),
		tex(5, kDepthRes, TexFormat::r32_float_x8x24_typeless),
	};
}

const GBufferTarget *find_role(const GBufferClassification &c, GBufferRole role)
{
	for (const auto &t : c.targets)
		if (t.role == role)
			return &t;
	return nullptr;
}

} // namespace

TEST_CASE("the stock base-pass MRT set classifies completely")
{
	const GBufferClassification c = classify_render_target_set(stock_set());

	CHECK(c.is_base_pass);
	CHECK(c.stock_roles_matched == 4);
	CHECK(c.extent_width == kW);
	CHECK(c.extent_height == kH);
	CHECK(c.targets.size() == 6);

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

	const GBufferTarget *v = find_role(c, GBufferRole::velocity);
	REQUIRE(v != nullptr);
	CHECK(v->tex.slot == kStockVelocitySlot);

	const GBufferTarget *d = find_role(c, GBufferRole::depth);
	REQUIRE(d != nullptr);
	CHECK(d->tex.resource == kDepthRes);
}

TEST_CASE("B/C are format-indistinguishable and the ambiguity is REPORTED")
{
	const GBufferClassification c = classify_render_target_set(stock_set());
	// Both classified, both 8-bit RGBA: the assignment rests on the SOFT slot order alone,
	// and the report must say so rather than presenting it as measured.
	CHECK(c.bc_order_by_slot_only);
}

TEST_CASE("the R11G11B10F scene-colour variant is stock too")
{
	auto set = stock_set();
	set[0].format = TexFormat::r11g11b10_float; // r.SceneColorFormat=3
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	const GBufferTarget *sc = find_role(c, GBufferRole::scene_colour);
	REQUIRE(sc != nullptr);
	CHECK(sc->stock_format);
	CHECK(c.stock_roles_matched == 4);
}

TEST_CASE("channel-swapped RGBA8 is accepted for GBufferB/C")
{
	auto set = stock_set();
	set[2].format = TexFormat::r8g8b8a8_unorm;
	set[3].format = TexFormat::r8g8b8a8_unorm;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(find_role(c, GBufferRole::gbuffer_b) != nullptr);
	CHECK(find_role(c, GBufferRole::gbuffer_c) != nullptr);
	CHECK(c.bc_order_by_slot_only);
}

TEST_CASE("a set without the velocity target is rejected: not the base pass")
{
	auto set = stock_set();
	set.erase(set.begin() + 4); // drop velocity
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("velocity") != std::string::npos);
	// The members are still reported — they ARE the diagnostic on a rejection.
	CHECK(c.targets.size() == 5);
}

TEST_CASE("two velocity-format targets are ambiguous, refused")
{
	auto set = stock_set();
	set.push_back(tex(6, 0x777, TexFormat::r16g16b16a16_unorm));
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("ambiguous") != std::string::npos);
}

TEST_CASE("1x1 dummies are ignored, not classified")
{
	auto set = stock_set();
	set.push_back(tex(6, kDummyRes, TexFormat::r16g16b16a16_float, 1, 1));
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(c.targets.size() == 6); // the dummy never appears
	for (const auto &t : c.targets)
		CHECK(t.tex.resource != kDummyRes);
}

TEST_CASE("a 1x1 velocity-format dummy is not the velocity signature")
{
	// Velocity substituted by the BlackDummy (a cut-like frame) must not anchor anything.
	auto set = stock_set();
	set[4] = tex(4, kDummyRes, TexFormat::r16g16b16a16_unorm, 1, 1);
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("velocity") != std::string::npos);
}

TEST_CASE("a licensee RGBA16F GBufferC is unknown-but-logged, never misclassified")
{
	auto set = stock_set();
	set[3].format = TexFormat::r16g16b16a16_float;
	const GBufferClassification c = classify_render_target_set(set);

	// The SET still carries the base-pass signature...
	CHECK(c.is_base_pass);
	// ...but slot 3 is NOT called GBufferC on format evidence it does not have.
	CHECK(find_role(c, GBufferRole::gbuffer_c) == nullptr);
	CHECK(c.stock_roles_matched == 3);
	CHECK_FALSE(c.bc_order_by_slot_only); // only one 8-bit RGBA left: no ambiguity

	const GBufferTarget *slot3 = nullptr;
	for (const auto &t : c.targets)
		if (t.tex.slot == 3 && t.role != GBufferRole::depth)
			slot3 = &t;
	REQUIRE(slot3 != nullptr);
	CHECK(slot3->role == GBufferRole::unknown);
	CHECK_FALSE(slot3->stock_format);
	CHECK(slot3->note[0] != 0); // the deviation is named for the log
}

TEST_CASE("a high-precision-looking GBufferA is unknown with a note")
{
	auto set = stock_set();
	set[1].format = TexFormat::r16g16b16a16_float;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(find_role(c, GBufferRole::gbuffer_a) == nullptr);
	CHECK(c.stock_roles_matched == 3);
}

TEST_CASE("an off-format scene colour is unknown, the rest still classify")
{
	auto set = stock_set();
	set[0].format = TexFormat::r10g10b10a2_unorm; // swapchain-like, not a scene colour
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(find_role(c, GBufferRole::scene_colour) == nullptr);
	CHECK(find_role(c, GBufferRole::gbuffer_a) != nullptr);
	CHECK(c.stock_roles_matched == 3);
}

TEST_CASE("velocity at a non-stock slot refuses to assign roles")
{
	// Swap velocity into slot 2: the anchor holds but the SOFT slot order does not, so no
	// positional claim can be trusted and the classifier must say so, not guess.
	auto set = stock_set();
	set[2] = tex(2, kVelocityRes, TexFormat::r16g16b16a16_unorm);
	set[4] = tex(4, kGBufferBRes, TexFormat::b8g8r8a8_unorm);
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("slot 4") != std::string::npos);
	CHECK(find_role(c, GBufferRole::gbuffer_b) == nullptr);
}

TEST_CASE("a colour target at a different extent breaks the set")
{
	auto set = stock_set();
	set[1].width = kW / 2;
	set[1].height = kH / 2;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("extent") != std::string::npos);
}

TEST_CASE("a gap in slots 0-3 is rejected, not papered over")
{
	auto set = stock_set();
	set.erase(set.begin() + 1); // GBufferA's RTV was null: slots are 0,2,3,4
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("slots 0-3") != std::string::npos);
}

TEST_CASE("extra targets past velocity are reported, roles stay honest")
{
	auto set = stock_set();
	set.push_back(tex(5, kGBufferDRes, TexFormat::b8g8r8a8_unorm));
	set.back().slot = 5;
	set[5].slot = 6; // move the DSV's recorded slot out of the way (recorder appends at count)
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);

	const GBufferTarget *slot5 = nullptr;
	for (const auto &t : c.targets)
		if (t.tex.slot == 5 && t.role != GBufferRole::depth)
			slot5 = &t;
	REQUIRE(slot5 != nullptr);
	CHECK(slot5->role == GBufferRole::unknown);
	CHECK(slot5->note[0] != 0); // named as the stock GBufferD/E position
	// A third 8-bit RGBA target does not un-assign B/C — they are still slots 2 and 3.
	CHECK(c.bc_order_by_slot_only);
	CHECK(c.stock_roles_matched == 4);
}

TEST_CASE("the stencil view of the depth resource is depth too")
{
	auto set = stock_set();
	set[5].format = TexFormat::x32_typeless_g8x24_uint;
	const GBufferClassification c = classify_render_target_set(set);
	CHECK(c.is_base_pass);
	CHECK(find_role(c, GBufferRole::depth) != nullptr);
}

TEST_CASE("duplicate slots are refused")
{
	auto set = stock_set();
	set[3].slot = 2; // two colour targets claim slot 2
	const GBufferClassification c = classify_render_target_set(set);
	CHECK_FALSE(c.is_base_pass);
	CHECK(std::string(c.reason).find("duplicate") != std::string::npos);
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
	CHECK(std::strcmp(tex_format_name(TexFormat::r16g16b16a16_unorm), "RGBA16_UNORM") == 0);
}
