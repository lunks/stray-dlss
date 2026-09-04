#include <doctest/doctest.h>

#include "core/rr_guides.hpp"

#include <cstring>

using namespace stray_dlss;

namespace {

// Stray at 50% screen percentage: 3840x2160 output, 1920x1080 render, scene buffer 1920x1080
// (CLAUDE.md §2.3.1). The G-buffers sit at the SCENE-BUFFER extent, not the view's.
constexpr std::uint32_t kBufferW = 1920;
constexpr std::uint32_t kBufferH = 1080;

constexpr std::uint32_t kR10G10B10A2Unorm = 24;
constexpr std::uint32_t kR8G8B8A8Unorm = 28;
constexpr std::uint32_t kB8G8R8A8UnormSrgb = 91;
constexpr std::uint32_t kR16G16B16A16Float = 10;

rrguides::Record good(std::uint32_t format, std::uint64_t resource)
{
	rrguides::Record r;
	r.seen = true;
	r.status_ok = true;
	r.live = true;
	r.epoch = 7;
	r.frame = 1000;
	r.resource = resource;
	r.width = kBufferW;
	r.height = kBufferH;
	r.dxgi_format = format;
	return r;
}

rrguides::Set healthy()
{
	rrguides::Set s;
	s.a = good(kR10G10B10A2Unorm, 0xA000);
	s.b = good(kR8G8B8A8Unorm, 0xB000);
	s.c = good(kB8G8R8A8UnormSrgb, 0xC000);
	return s;
}

rrguides::Expect expecting()
{
	rrguides::Expect e;
	e.frame_now = 1000;
	e.buffer_width = kBufferW;
	e.buffer_height = kBufferH;
	e.render_width = kBufferW;
	e.render_height = kBufferH;
	e.supplying = true;
	e.hooked = true;
	return e;
}

} // namespace

TEST_CASE("rr guides: the healthy Stray set is accepted")
{
	CHECK(rrguides::judge(healthy(), expecting()) == rrguides::Refusal::none);
}

TEST_CASE("rr guides: the level gates before anything else is even looked at")
{
	rrguides::Expect e = expecting();
	e.supplying = false;
	// Deliberately also break the set: the level must win, so a PoolNames=1 session never
	// reports a confusing downstream reason for a mechanism that is simply switched off.
	rrguides::Set s = healthy();
	s.a.seen = false;
	CHECK(rrguides::judge(s, e) == rrguides::Refusal::not_supplying);

	e.supplying = true;
	e.hooked = false;
	CHECK(rrguides::judge(healthy(), e) == rrguides::Refusal::not_hooked);
}

TEST_CASE("rr guides: a name the engine never used is nameMissing, not a silent substitution")
{
	for (int which = 0; which < 3; ++which)
	{
		rrguides::Set s = healthy();
		(which == 0 ? s.a : which == 1 ? s.b : s.c).seen = false;
		CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::name_missing);
	}
}

TEST_CASE("rr guides: a record whose chain did not reach a registered resource is recordBad")
{
	rrguides::Set s = healthy();
	s.b.status_ok = false;
	CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::record_bad);

	s = healthy();
	s.c.resource = 0;
	CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::record_bad);
}

TEST_CASE("rr guides: THREE RECORDS FROM TWO CYCLES ARE NOT A SET")
{
	// The whole point of the epoch. Each member is individually perfect; pairing one frame's
	// normals with another frame's base colour is the wrong-but-plausible input the project
	// exists to refuse, and nothing downstream could ever notice it.
	rrguides::Set s = healthy();
	s.c.epoch = s.a.epoch + 1;
	CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::epoch_split);

	s = healthy();
	s.a.epoch = 0;
	s.b.epoch = 0;
	s.c.epoch = 0;
	CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::epoch_split);
}

TEST_CASE("rr guides: freshness is measured from the OLDEST member")
{
	rrguides::Expect e = expecting();
	rrguides::Set s = healthy();
	s.a.frame = e.frame_now - rrguides::kMaxSetAgeFrames;
	CHECK(rrguides::judge(s, e) == rrguides::Refusal::none);

	s.a.frame = e.frame_now - rrguides::kMaxSetAgeFrames - 1;
	CHECK(rrguides::judge(s, e) == rrguides::Refusal::stale);

	// The youngest member must not be able to carry the set.
	s.b.frame = e.frame_now;
	s.c.frame = e.frame_now;
	CHECK(rrguides::judge(s, e) == rrguides::Refusal::stale);
}

TEST_CASE("rr guides: a record written AFTER the frame counter moved is not stale")
{
	// The pool hook stamps on the render thread and the frame counter advances at present, so a
	// record can legitimately carry a stamp equal to or greater than frame_now. Unsigned
	// arithmetic must not turn that into an enormous age.
	rrguides::Expect e = expecting();
	rrguides::Set s = healthy();
	s.a.frame = e.frame_now + 3;
	s.b.frame = e.frame_now + 3;
	s.c.frame = e.frame_now + 3;
	CHECK(rrguides::judge(s, e) == rrguides::Refusal::none);
}

TEST_CASE("rr guides: LIVENESS IS OUR REGISTRY'S ANSWER, and a dead member refuses the frame")
{
	for (int which = 0; which < 3; ++which)
	{
		rrguides::Set s = healthy();
		(which == 0 ? s.a : which == 1 ? s.b : s.c).live = false;
		CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::not_live);
	}
}

TEST_CASE("rr guides: the three must share one extent, and it must be the scene buffer's")
{
	rrguides::Set s = healthy();
	s.b.width = kBufferW / 2;
	CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::extent_disagree);

	s = healthy();
	s.a.width = s.b.width = s.c.width = 2560;
	CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::extent_mismatch);
}

TEST_CASE("rr guides: an UNKNOWN row-132 extent skips that check rather than failing it")
{
	rrguides::Expect e = expecting();
	e.buffer_width = 0;
	e.buffer_height = 0;
	CHECK(rrguides::judge(healthy(), e) == rrguides::Refusal::none);
}

TEST_CASE("rr guides: the set must COVER the render rect, never equal it")
{
	// At 50% screen percentage the scene buffer and the view happen to coincide here, but a
	// buffer left oversized from an earlier resolution is normal and must not be refused —
	// CLAUDE.md §5's GetOutputExtent() Max() lesson, in a different place.
	rrguides::Expect e = expecting();
	rrguides::Set s = healthy();
	s.a.width = s.b.width = s.c.width = 2560;
	s.a.height = s.b.height = s.c.height = 1440;
	e.buffer_width = 2560;
	e.buffer_height = 1440;
	CHECK(rrguides::judge(s, e) == rrguides::Refusal::none);

	e.render_width = 2561;
	CHECK(rrguides::judge(s, e) == rrguides::Refusal::extent_too_small);

	e.render_width = 0;
	CHECK(rrguides::judge(s, e) == rrguides::Refusal::extent_too_small);
}

TEST_CASE("rr guides: GBufferA outside the RGB10A2 family is REFUSED, not decoded anyway")
{
	// The high-precision G-buffer paths use an OCTAHEDRAL normal encoding. Our shader's
	// N*2-1 would read it as a plausible unit vector pointing the wrong way, everywhere, with
	// nothing to notice — so the format is a gate.
	rrguides::Set s = healthy();
	s.a.dxgi_format = kR16G16B16A16Float;
	CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::format_a);

	CHECK(rrguides::gbuffer_a_format_ok(kR10G10B10A2Unorm));
	CHECK(rrguides::gbuffer_a_format_ok(23)); // R10G10B10A2_TYPELESS
	CHECK_FALSE(rrguides::gbuffer_a_format_ok(kR8G8B8A8Unorm));
}

TEST_CASE("rr guides: B and C accept both channel orders and all three sRGB spellings")
{
	const std::uint32_t ok[] = { 27, 28, 29, 87, 90, 91 };
	for (std::uint32_t f : ok)
		CHECK(rrguides::gbuffer_bc_format_ok(f));
	CHECK_FALSE(rrguides::gbuffer_bc_format_ok(kR10G10B10A2Unorm));
	CHECK_FALSE(rrguides::gbuffer_bc_format_ok(kR16G16B16A16Float));

	rrguides::Set s = healthy();
	s.c.dxgi_format = kR16G16B16A16Float;
	CHECK(rrguides::judge(s, expecting()) == rrguides::Refusal::format_bc);
}

TEST_CASE("rr guides: every refusal has a distinct name for the log line")
{
	for (std::size_t i = 0; i < rrguides::kRefusalCount; ++i)
	{
		const char *a = rrguides::refusal_name(static_cast<rrguides::Refusal>(i));
		REQUIRE(a != nullptr);
		CHECK(a[0] != '\0');
		CHECK(std::strcmp(a, "?") != 0);
		for (std::size_t j = i + 1; j < rrguides::kRefusalCount; ++j)
			CHECK(std::strcmp(a, rrguides::refusal_name(static_cast<rrguides::Refusal>(j))) != 0);
	}
}
