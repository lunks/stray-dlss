// The TYPES the DLSSNR parameter block is written with.
//
// An NGX parameter block is an untyped string->value map, and NVSDK_NGX_Parameter has SEPARATE
// virtual overloads for `int` and `unsigned int` — Set_I and Set_UI land in different vtable
// slots and store under different type tags. There is no validation and no error for a
// mismatch: a value written as unsigned and read as signed is indistinguishable, from the
// caller's side, from a value that was accepted and used. That is the same failure mode as
// DLSSNR.Scale, which we wrote for weeks into a runtime that has no such key
// (docs/RESEARCH-RENODX-DLSS5.md §2.2.1).
//
// So the rects go through a pure builder that states its own types, and this file is what pins
// them. Written first, against no implementation.
#include <doctest/doctest.h>

#include "core/nr_params.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace stray_dlss::nrparam;

namespace {

const Entry *find(const std::vector<Entry> &entries, const char *name)
{
	for (const Entry &e : entries)
		if (std::strcmp(e.name, name) == 0)
			return &e;
	return nullptr;
}

std::vector<Entry> rects(const Rects &r)
{
	Entry buf[kMaxRectEntries];
	const int n = build_rects(r, buf, kMaxRectEntries);
	return std::vector<Entry>(buf, buf + n);
}

} // namespace

TEST_CASE("every subrect and extent parameter is written as SIGNED int")
{
	// The audit: the snippet Gets its subrect and extent parameters as `int`. Ours were all
	// written through the `unsigned int` overload, which is a different vtable slot and a
	// different stored type. See docs/RESEARCH-RENODX-DLSS5.md §2.2.1.
	Rects r;
	r.color_width = 3840;
	r.color_height = 2160;
	r.guide_width = 1920;
	r.guide_height = 1080;
	r.output_width = 3840;
	r.output_height = 2160;

	const std::vector<Entry> e = rects(r);
	CHECK(e.size() == 16); // four rects x (baseX, baseY, width, height)

	for (const Entry &entry : e)
	{
		INFO("parameter: ", entry.name);
		CHECK(entry.type == Type::i32);
	}
}

TEST_CASE("the rect builder emits exactly the names the runtime knows")
{
	// Every name here was confirmed present in nvngx_dlssnr.dll 310.8.0 by exact
	// null-terminated string search. A typo is silently ignored by the runtime, so the name set
	// is pinned rather than trusted.
	Rects r;
	r.color_width = 2560;
	r.color_height = 1440;
	r.guide_width = 1280;
	r.guide_height = 720;
	r.output_width = 2560;
	r.output_height = 1440;
	const std::vector<Entry> e = rects(r);

	static const char *const expected[] = {
		"DLSSNR.ColorSubrectBaseX", "DLSSNR.ColorSubrectBaseY",
		"DLSSNR.ColorSubrectWidth", "DLSSNR.ColorSubrectHeight",
		"DLSSNR.DepthSubrectBaseX", "DLSSNR.DepthSubrectBaseY",
		"DLSSNR.DepthSubrectWidth", "DLSSNR.DepthSubrectHeight",
		"DLSSNR.MVecSubrectBaseX", "DLSSNR.MVecSubrectBaseY",
		"DLSSNR.MVecSubrectWidth", "DLSSNR.MVecSubrectHeight",
		"DLSSNR.OutputSubrectBaseX", "DLSSNR.OutputSubrectBaseY",
		"DLSSNR.OutputSubrectWidth", "DLSSNR.OutputSubrectHeight",
	};
	for (const char *name : expected)
	{
		INFO("expected parameter: ", name);
		CHECK(find(e, name) != nullptr);
	}
	CHECK(e.size() == sizeof(expected) / sizeof(expected[0]));
}

TEST_CASE("the rects carry the values the DLSS-NR contract asks for")
{
	// Colour is the OUTPUT rect (post-process topology: the image is already upscaled), depth
	// and motion vectors share the RENDER-resolution guide grid, and the output rect is the
	// rect the decode writes. Getting colour and guides the same way round is what
	// DLSSNR.MVecScaleX/Y would otherwise have to compensate for, and that scale is 1.0
	// precisely because the subrects already say where the guides live (CLAUDE.md, "declaring a
	// guide's subrect AND scaling its vectors double-counts").
	Rects r;
	r.color_width = 3840;
	r.color_height = 2160;
	r.guide_width = 1920;
	r.guide_height = 1080;
	r.output_width = 3840;
	r.output_height = 2160;
	const std::vector<Entry> e = rects(r);

	CHECK(find(e, "DLSSNR.ColorSubrectWidth")->i == 3840);
	CHECK(find(e, "DLSSNR.ColorSubrectHeight")->i == 2160);
	CHECK(find(e, "DLSSNR.DepthSubrectWidth")->i == 1920);
	CHECK(find(e, "DLSSNR.DepthSubrectHeight")->i == 1080);
	CHECK(find(e, "DLSSNR.MVecSubrectWidth")->i == 1920);
	CHECK(find(e, "DLSSNR.MVecSubrectHeight")->i == 1080);
	CHECK(find(e, "DLSSNR.OutputSubrectWidth")->i == 3840);
	CHECK(find(e, "DLSSNR.OutputSubrectHeight")->i == 2160);

	// Every base is the origin: UE4 shifts its view rects to the buffer top-left, and we never
	// hand the runtime a sub-rectangle of a larger allocation.
	CHECK(find(e, "DLSSNR.ColorSubrectBaseX")->i == 0);
	CHECK(find(e, "DLSSNR.ColorSubrectBaseY")->i == 0);
	CHECK(find(e, "DLSSNR.OutputSubrectBaseX")->i == 0);
	CHECK(find(e, "DLSSNR.OutputSubrectBaseY")->i == 0);
}

TEST_CASE("an extent too large for a signed int is clamped, never wrapped")
{
	// The builder takes uint32 because that is what D3D12 reports, and emits int32 because that
	// is what the runtime reads. A value above INT32_MAX cannot be a real rect, and the one
	// thing it must NOT become is a negative number the runtime would take at face value.
	Rects r;
	r.color_width = 0xFFFFFFFFu;
	r.color_height = 0x80000000u;
	r.guide_width = 1920;
	r.guide_height = 1080;
	r.output_width = 3840;
	r.output_height = 2160;
	const std::vector<Entry> e = rects(r);
	CHECK(find(e, "DLSSNR.ColorSubrectWidth")->i > 0);
	CHECK(find(e, "DLSSNR.ColorSubrectHeight")->i > 0);
}

TEST_CASE("the create-time extent is signed and the scaling ratio is float")
{
	Entry buf[kMaxCreateEntries];
	const int n = build_create(1920, 1080, 2.0f, buf, kMaxCreateEntries);
	const std::vector<Entry> e(buf, buf + n);

	CHECK(find(e, "DLSSNR.Width") != nullptr);
	CHECK(find(e, "DLSSNR.Width")->type == Type::i32);
	CHECK(find(e, "DLSSNR.Width")->i == 1920);
	CHECK(find(e, "DLSSNR.Height")->type == Type::i32);
	CHECK(find(e, "DLSSNR.Height")->i == 1080);

	// ScalingRatio is a float in the runtime and is measured INERT there (it is read and then
	// unconditionally overwritten with 1.0f at 0x18001a96a), but it is a real key, so it is
	// written with the type it declares rather than dropped.
	CHECK(find(e, "DLSSNR.ScalingRatio")->type == Type::f32);
	CHECK(find(e, "DLSSNR.ScalingRatio")->f == doctest::Approx(2.0f));

	// NOT "DLSSNR.Scale". That name does not exist in the 310.8.0 runtime, so everything we ever
	// wrote to it was silently discarded (docs/RESEARCH-RENODX-DLSS5.md §2.2.1). Pinned here so
	// it cannot come back.
	CHECK(find(e, "DLSSNR.Scale") == nullptr);
}

TEST_CASE("a builder that would overflow its buffer writes nothing")
{
	Rects r;
	r.color_width = r.color_height = r.guide_width = r.guide_height = 8;
	r.output_width = r.output_height = 8;
	Entry buf[4];
	CHECK(build_rects(r, buf, 4) == 0);
	CHECK(build_create(8, 8, 1.0f, buf, 1) == 0);
}

TEST_CASE("the ControlMask subrect is written in the same TYPES as the other four")
{
	// The runtime reads DLSSNR.ControlMaskSubrect* through the SAME parameter-block vtable slot
	// (+0x58) as the Color/Depth/MVec/Output subrects, so writing it through a different overload
	// would store it under a different type tag and the runtime would read a rect it never wrote —
	// with no error, exactly as DLSSNR.Scale did.
	Entry buf[kMaxMaskRectEntries];
	const int n = build_mask_rect(2560, 1440, buf, kMaxMaskRectEntries);
	REQUIRE(n == kMaxMaskRectEntries);

	const std::vector<Entry> e(buf, buf + n);
	for (const Entry &x : e)
		CHECK(x.type == Type::i32);

	REQUIRE(find(e, "DLSSNR.ControlMaskSubrectBaseX") != nullptr);
	CHECK(find(e, "DLSSNR.ControlMaskSubrectBaseX")->i == 0);
	CHECK(find(e, "DLSSNR.ControlMaskSubrectBaseY")->i == 0);
	CHECK(find(e, "DLSSNR.ControlMaskSubrectWidth")->i == 2560);
	CHECK(find(e, "DLSSNR.ControlMaskSubrectHeight")->i == 1440);
}

TEST_CASE("build_mask_rect never writes a partial rect")
{
	// Half a subrect is worse than none: the parameter block persists across evaluates, so the
	// missing half would silently keep a previous frame's value.
	Entry buf[kMaxMaskRectEntries];
	CHECK(build_mask_rect(2560, 1440, buf, kMaxMaskRectEntries - 1) == 0);
	CHECK(build_mask_rect(2560, 1440, nullptr, kMaxMaskRectEntries) == 0);
}

TEST_CASE("build_rects does NOT emit a ControlMask rect")
{
	// The four rects are mandatory; the mask's is optional and must not appear unless a mask is
	// actually bound.
	Rects r;
	r.color_width = 2560; r.color_height = 1440;
	r.guide_width = 1280; r.guide_height = 720;
	r.output_width = 2560; r.output_height = 1440;
	const std::vector<Entry> e = rects(r);
	CHECK(find(e, "DLSSNR.ControlMaskSubrectWidth") == nullptr);
}
