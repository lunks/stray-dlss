// Locating FRenderTargetPool::FindFreeElement by caller-literal agreement
// (src/core/pool_locator.hpp), against a synthetic module that the scan cannot tell from a
// mapped image.
//
// As with the seam's and u0's tests, most of the effort goes on REFUSALS. Level 2 patches the
// GAME'S OWN CODE, so the interesting assertions are the ones that stop it: a helper reached
// from many literals inside ONE function must lose to the real target; two candidates tied on
// distinct enclosing functions must be declined rather than picked; too few agreeing groups or
// names must refuse; and a CALL whose target is not a .pdata function start must not be a
// candidate at all.
#include "core/pool_locator.hpp"

#include <doctest/doctest.h>

#include <cstring>
#include <string>
#include <vector>

using namespace stray_dlss::pool;
using stray_dlss::seam::Image;
using stray_dlss::seam::Region;
using stray_dlss::u0::FunctionTable;
using stray_dlss::u0::RuntimeFunction;

namespace {

// One executable section of 0x100-byte function cells, one read-only section of wide literals,
// and a .pdata table covering every cell. Everything the scan reads comes from here.
struct FakeModule
{
	static constexpr std::uint64_t kBase = 0x0000000140000000ull;
	static constexpr std::uint64_t kTextVa = kBase + 0x1000;
	static constexpr std::uint64_t kDataVa = kBase + 0x100000;
	static constexpr std::size_t kCell = 0x100;

	std::vector<unsigned char> text{ std::vector<unsigned char>(0x4000, 0xCC) };
	std::vector<unsigned char> data{ std::vector<unsigned char>(0x2000, 0x00) };
	std::vector<RuntimeFunction> pdata;
	std::size_t data_at = 0;

	std::uint64_t cell_va(unsigned index) const { return kTextVa + kCell * index; }
	std::size_t cell_off(unsigned index) const { return kCell * index; }

	// A .pdata entry covering the whole cell, so function_start(cell_va) == cell_va.
	void declare(unsigned index)
	{
		const std::uint32_t begin = static_cast<std::uint32_t>(kTextVa - kBase + cell_off(index));
		pdata.push_back({ begin, begin + static_cast<std::uint32_t>(kCell), 0 });
	}

	// A UTF-16LE literal with its terminator, as MSVC lays TEXT("...") down.
	std::uint64_t put_wide(const char *ascii)
	{
		const std::uint64_t va = kDataVa + data_at;
		for (const char *p = ascii; *p != '\0'; ++p)
		{
			data[data_at++] = static_cast<unsigned char>(*p);
			data[data_at++] = 0;
		}
		data[data_at++] = 0;
		data[data_at++] = 0;
		// keep the next literal 2-aligned (it already is) and leave a gap
		data_at += 2;
		return va;
	}

	void put_i32(std::size_t at, std::int32_t v) { std::memcpy(text.data() + at, &v, sizeof(v)); }

	// lea <reg>, [rip+d] -> `target`. `rex` is 0x48 (rax..rdi) or 0x4C (r8..r15).
	void write_lea(std::size_t at, std::uint64_t target, unsigned char modrm = 0x0D,
	               unsigned char rex = 0x48)
	{
		const std::uint64_t va = kTextVa + at;
		text[at + 0] = rex;
		text[at + 1] = 0x8D;
		text[at + 2] = modrm;
		put_i32(at + 3, static_cast<std::int32_t>(static_cast<std::int64_t>(target) -
			static_cast<std::int64_t>(va + 7)));
	}

	void write_call(std::size_t at, std::uint64_t target)
	{
		const std::uint64_t va = kTextVa + at;
		text[at + 0] = 0xE8;
		put_i32(at + 1, static_cast<std::int32_t>(static_cast<std::int64_t>(target) -
			static_cast<std::int64_t>(va + 5)));
	}

	// A caller: `lea rcx,[rip+literal]` at the cell start, then one call per target, back to back.
	void write_caller(unsigned index, std::uint64_t literal, std::initializer_list<std::uint64_t> calls)
	{
		declare(index);
		std::size_t at = cell_off(index);
		write_lea(at, literal);
		at += 7;
		for (std::uint64_t t : calls)
		{
			write_call(at, t);
			at += 5;
		}
		text[at] = 0xC3;
	}

	Region regions[2]{};
	Image image()
	{
		regions[0] = Region{ kTextVa, text.data(), text.size(), true, ".text" };
		regions[1] = Region{ kDataVa, data.data(), data.size(), false, ".rdata" };
		return Image{ regions, 2 };
	}
	FunctionTable table(const Image &img)
	{
		FunctionTable t;
		t.entries = pdata.data();
		t.count = pdata.size();
		t.image_base = kBase;
		t.image = &img;
		return t;
	}
	void sort_pdata()
	{
		for (std::size_t i = 1; i < pdata.size(); ++i)
		{
			RuntimeFunction key = pdata[i];
			std::size_t j = i;
			while (j > 0 && pdata[j - 1].begin > key.begin)
			{
				pdata[j] = pdata[j - 1];
				--j;
			}
			pdata[j] = key;
		}
	}
};

// The shape a healthy shipping image has: four DIFFERENT functions, each loading a different
// pooled-render-target name literal and calling one common function.
struct Healthy
{
	FakeModule m;
	std::uint64_t pool_fn = 0;
	Image img{};
	FunctionTable tab{};

	// cells 0..3 callers, 8 the pool function
	Healthy()
	{
		pool_fn = m.cell_va(8);
		m.declare(8);
		m.text[m.cell_off(8)] = 0xC3;

		const std::uint64_t a = m.put_wide("GBufferA");
		const std::uint64_t d = m.put_wide("SceneDepthZ");
		const std::uint64_t f = m.put_wide("GBufferF");
		const std::uint64_t s = m.put_wide("ScreenSpaceAO");
		m.write_caller(0, a, { pool_fn });
		m.write_caller(1, d, { pool_fn });
		m.write_caller(2, f, { pool_fn });
		m.write_caller(3, s, { pool_fn });
		m.sort_pdata();
		img = m.image();
		tab = m.table(img);
	}
};

} // namespace

// ---------------------------------------------------------------------------------------
// The name table
// ---------------------------------------------------------------------------------------

TEST_CASE("the name table is one entry per Target, in Target order, and the RR guides are anchors")
{
	for (std::size_t i = 0; i < kNameCount; ++i)
		CHECK(static_cast<std::size_t>(kNames[i].target) == i);
	// GBufferA-E are what Ray Reconstruction needs and they are all anchorable.
	CHECK(kNames[static_cast<std::size_t>(Target::gbuffer_a)].anchor);
	CHECK(kNames[static_cast<std::size_t>(Target::gbuffer_e)].anchor);
	CHECK(kNames[static_cast<std::size_t>(Target::scene_depth_z)].anchor);
	CHECK(kNames[static_cast<std::size_t>(Target::gbuffer_velocity)].anchor);
	// SceneColorDeferred is NOT: AllocSceneColor passes GetSceneColorTargetName(...), so the
	// reference to the literal lives inside that accessor and nothing useful follows it.
	CHECK_FALSE(kNames[static_cast<std::size_t>(Target::scene_color_deferred)].anchor);
}

TEST_CASE("match_name is exact - a longer name that starts with ours is not a hit")
{
	CHECK(match_name("GBufferA") == static_cast<int>(Target::gbuffer_a));
	CHECK(match_name("SceneDepthZ") == static_cast<int>(Target::scene_depth_z));
	CHECK(match_name("GBufferATexture") == -1);
	CHECK(match_name("gbuffera") == -1);
	CHECK(match_name("") == -1);
	CHECK(match_name(nullptr) == -1);
}

TEST_CASE("the shipped default is DISCOVER - moving the rung is a deliberate edit")
{
	// Level 2 patches the game's own code. Changing this constant is exactly the kind of
	// one-character change that should fail a test rather than pass a review.
	CHECK(kDefaultLevel == 1);
	CHECK(level_from_int(kDefaultLevel) == Level::discover);
	CHECK(level_from_int(0) == Level::off);
	CHECK(level_from_int(-3) == Level::off);
	CHECK(level_from_int(2) == Level::observe);
	CHECK(level_from_int(9) == Level::supply);
}

// ---------------------------------------------------------------------------------------
// The scan primitives
// ---------------------------------------------------------------------------------------

TEST_CASE("find_name_literals finds each name once and requires the terminator")
{
	FakeModule m;
	const std::uint64_t a = m.put_wide("GBufferA");
	m.put_wide("GBufferATexture"); // must NOT be counted as GBufferA
	const std::uint64_t v = m.put_wide("GBufferVelocity");
	Image img = m.image();

	std::uint64_t out[kNameCount][kMaxLiteralHitsPerName] = {};
	unsigned hits[kNameCount] = {};
	find_name_literals(img, out, hits);

	CHECK(hits[static_cast<std::size_t>(Target::gbuffer_a)] == 1);
	CHECK(out[static_cast<std::size_t>(Target::gbuffer_a)][0] == a);
	CHECK(hits[static_cast<std::size_t>(Target::gbuffer_velocity)] == 1);
	CHECK(out[static_cast<std::size_t>(Target::gbuffer_velocity)][0] == v);
	CHECK(hits[static_cast<std::size_t>(Target::gbuffer_b)] == 0);
}

TEST_CASE("find_lea_rip_refs decodes both REX forms and every destination register")
{
	FakeModule m;
	const std::uint64_t lit = m.put_wide("SceneDepthZ");
	m.declare(0);
	m.write_lea(m.cell_off(0) + 0x00, lit, 0x05, 0x48); // lea rax
	m.write_lea(m.cell_off(0) + 0x10, lit, 0x3D, 0x48); // lea rdi
	m.write_lea(m.cell_off(0) + 0x20, lit, 0x05, 0x4C); // lea r8
	// A non-rip-relative modrm at the same shape must be ignored (mod=01, rm=101 => [rbp+d8]).
	m.text[m.cell_off(0) + 0x30] = 0x48;
	m.text[m.cell_off(0) + 0x31] = 0x8D;
	m.text[m.cell_off(0) + 0x32] = 0x45;
	Image img = m.image();

	const std::uint32_t owner = 7;
	LeaSite sites[8] = {};
	const std::size_t n = find_lea_rip_refs(img, &lit, &owner, 1, sites, 8);
	CHECK(n == 3);
	for (std::size_t i = 0; i < n; ++i)
		CHECK(sites[i].which == 7);
}

TEST_CASE("collect_calls_after skips the lea's own bytes and demands a .pdata function start")
{
	FakeModule m;
	const std::uint64_t lit = m.put_wide("GBufferA");
	const std::uint64_t good = m.cell_va(8);
	m.declare(8);
	m.text[m.cell_off(8)] = 0xC3;
	// A "call" to the MIDDLE of that function: a real call never does this, and a stray 0xE8
	// inside another instruction's operand effectively always does.
	const std::uint64_t middle = good + 0x40;

	m.declare(0);
	m.write_lea(m.cell_off(0), lit);
	m.write_call(m.cell_off(0) + 7, good);
	m.write_call(m.cell_off(0) + 12, middle);
	m.sort_pdata();
	Image img = m.image();
	FunctionTable tab = m.table(img);

	CallHit hits[8] = {};
	const std::size_t n = collect_calls_after(img, tab, m.cell_va(0), hits, 8);
	REQUIRE(n == 1);
	CHECK(hits[0].target == good);
	CHECK(hits[0].distance == 7);
}

TEST_CASE("function_calls sees a direct call inside the container's own .pdata extent")
{
	FakeModule m;
	const std::uint64_t callee = m.cell_va(8);
	m.declare(8);
	m.text[m.cell_off(8)] = 0xC3;
	m.declare(0);
	m.write_call(m.cell_off(0) + 0x20, callee);
	m.declare(1); // a function that calls nothing
	m.text[m.cell_off(1)] = 0xC3;
	m.sort_pdata();
	Image img = m.image();
	FunctionTable tab = m.table(img);

	CHECK(function_calls(img, tab, m.cell_va(0), callee));
	CHECK_FALSE(function_calls(img, tab, m.cell_va(1), callee));
	CHECK_FALSE(function_calls(img, tab, m.cell_va(0), 0));
}

// ---------------------------------------------------------------------------------------
// The verdict
// ---------------------------------------------------------------------------------------

TEST_CASE("locate finds the function four independent call sites agree on")
{
	Healthy h;
	const Locate v = locate(h.img, h.tab);

	CHECK(v.status == LocateStatus::ok);
	CHECK(v.target == h.pool_fn);
	CHECK(v.groups == 4);
	CHECK(v.names == 4);
	CHECK(v.sites == 4);
	CHECK(v.literals_found == 4);
	CHECK(v.refs_found == 4);
	CHECK(v.candidates == 1);
	CHECK(v.runner_up == 0);
	CHECK_FALSE(v.lea_sites_overflowed);
	CHECK_FALSE(v.candidates_overflowed);
	CHECK(v.entry_read);
	// No UE_LOG format string in this image, so the FindFreeElementInternal residual is closed
	// rather than assumed either way.
	CHECK(v.internal_log_hits == 0);
	CHECK(v.internal_fn == 0);
	CHECK_FALSE(v.internal_called_by_target);
}

TEST_CASE("a helper reached from MANY names inside ONE function loses to the real target")
{
	// This is the false positive a byte scan invites: AllocGBufferTargets alone holds seven
	// name literals, so anything it calls is reached by seven names. Ranking on DISTINCT
	// ENCLOSING FUNCTIONS is what refuses it, and this pins that.
	FakeModule m;
	const std::uint64_t pool_fn = m.cell_va(8);
	const std::uint64_t helper = m.cell_va(9);
	m.declare(8);
	m.text[m.cell_off(8)] = 0xC3;
	m.declare(9);
	m.text[m.cell_off(9)] = 0xC3;

	// One function holding five GBuffer literals, each followed by a call to BOTH.
	const char *many[] = { "GBufferA", "GBufferB", "GBufferC", "GBufferD", "GBufferE" };
	m.declare(0);
	std::size_t at = m.cell_off(0);
	for (const char *n : many)
	{
		m.write_lea(at, m.put_wide(n));
		at += 7;
		m.write_call(at, pool_fn);
		at += 5;
		m.write_call(at, helper);
		at += 5;
	}
	// Three more functions, one literal each, calling only the pool.
	m.write_caller(1, m.put_wide("SceneDepthZ"), { pool_fn });
	m.write_caller(2, m.put_wide("GBufferF"), { pool_fn });
	m.write_caller(3, m.put_wide("ScreenSpaceAO"), { pool_fn });
	m.sort_pdata();
	Image img = m.image();
	FunctionTable tab = m.table(img);

	const Locate v = locate(img, tab);
	CHECK(v.status == LocateStatus::ok);
	CHECK(v.target == pool_fn);
	CHECK(v.groups == 4);
	CHECK(v.names == 8);
	CHECK(v.runner_up == helper);
	CHECK(v.runner_up_groups == 1);
	CHECK(v.runner_up_names == 5);
}

TEST_CASE("two candidates tied on distinct enclosing functions are REFUSED, never picked")
{
	FakeModule m;
	const std::uint64_t a = m.cell_va(8);
	const std::uint64_t b = m.cell_va(9);
	m.declare(8);
	m.text[m.cell_off(8)] = 0xC3;
	m.declare(9);
	m.text[m.cell_off(9)] = 0xC3;
	m.write_caller(0, m.put_wide("GBufferA"), { a, b });
	m.write_caller(1, m.put_wide("SceneDepthZ"), { a, b });
	m.write_caller(2, m.put_wide("GBufferF"), { a, b });
	m.write_caller(3, m.put_wide("ScreenSpaceAO"), { a, b });
	m.sort_pdata();
	Image img = m.image();
	FunctionTable tab = m.table(img);

	const Locate v = locate(img, tab);
	CHECK(v.status == LocateStatus::ambiguous);
	CHECK(v.groups == 4);
	CHECK(v.runner_up_groups == 4);
}

TEST_CASE("too few agreeing enclosing functions is a refusal, and it names the stage")
{
	FakeModule m;
	const std::uint64_t pool_fn = m.cell_va(8);
	m.declare(8);
	m.text[m.cell_off(8)] = 0xC3;
	m.write_caller(0, m.put_wide("GBufferA"), { pool_fn });
	m.write_caller(1, m.put_wide("SceneDepthZ"), { pool_fn });
	m.sort_pdata();
	Image img = m.image();
	FunctionTable tab = m.table(img);

	const Locate v = locate(img, tab);
	CHECK(v.status == LocateStatus::insufficient);
	CHECK(v.target == pool_fn); // reported, so the log can say how close it came
	CHECK(v.groups == 2);
	CHECK(v.names == 2);
}

TEST_CASE("a call that is not a function start yields no candidate at all")
{
	FakeModule m;
	const std::uint64_t body = m.cell_va(8) + 0x40;
	m.declare(8);
	m.text[m.cell_off(8)] = 0xC3;
	m.write_caller(0, m.put_wide("GBufferA"), { body });
	m.write_caller(1, m.put_wide("SceneDepthZ"), { body });
	m.write_caller(2, m.put_wide("GBufferF"), { body });
	m.write_caller(3, m.put_wide("ScreenSpaceAO"), { body });
	m.sort_pdata();
	Image img = m.image();
	FunctionTable tab = m.table(img);

	const Locate v = locate(img, tab);
	CHECK(v.status == LocateStatus::no_candidates);
	CHECK(v.candidates == 0);
	CHECK(v.refs_found == 4);
}

TEST_CASE("no literals and no image are distinct, named refusals")
{
	FakeModule m;
	m.declare(0);
	m.sort_pdata();
	Image img = m.image();
	FunctionTable tab = m.table(img);
	CHECK(locate(img, tab).status == LocateStatus::no_literals);

	Image empty{};
	CHECK(locate(empty, tab).status == LocateStatus::no_regions);
}

TEST_CASE("the FindFreeElementInternal residual is settled, not assumed")
{
	// docs/RESEARCH-U0-EXTERNAL-PRIOR-ART.md §2.5: FindFreeElement (what the anchors reach) and
	// FindFreeElementForRDG (u0's path) both forward to FindFreeElementInternal, whose own
	// UE_LOG format string survives only under USE_LOGGING_IN_SHIPPING. When it IS there, the
	// internal is nameable AND our winner calling it is a fourth independent check.
	Healthy h;
	FakeModule &m = h.m;
	const std::uint64_t internal_fn = m.cell_va(10);
	m.declare(10);
	const std::uint64_t fmt = m.put_wide("%d MB, NewRT %s %s");
	m.write_lea(m.cell_off(10), fmt);
	m.text[m.cell_off(10) + 7] = 0xC3;
	// FindFreeElement's last act is FindFreeElementInternal (RenderTargetPool.cpp:703).
	m.write_call(m.cell_off(8) + 0x10, internal_fn);
	m.sort_pdata();
	Image img = m.image();
	FunctionTable tab = m.table(img);

	const Locate v = locate(img, tab);
	CHECK(v.status == LocateStatus::ok);
	CHECK(v.target == h.pool_fn);
	CHECK(v.internal_log_hits == 1);
	CHECK(v.internal_log_literal_va == fmt);
	CHECK(v.internal_fn_candidates == 1);
	CHECK(v.internal_fn == internal_fn);
	CHECK(v.internal_called_by_target);
}

// ---------------------------------------------------------------------------------------
// The assertions
// ---------------------------------------------------------------------------------------

TEST_CASE("judge_resource: absent is not a disagreement, and either texture matching is agreement")
{
	// Targetable and ShaderResource are the same object for a non-MSAA target, and either
	// matching the other route's answer is agreement.
	CHECK(judge_resource(0x1000, 0x1000, 0x1000) == AssertVerdict::agree);
	CHECK(judge_resource(0x1000, 0x2000, 0x2000) == AssertVerdict::agree);
	CHECK(judge_resource(0x1000, 0x2000, 0x3000) == AssertVerdict::disagree);
	// A frame where one side has nothing is evidence of NOTHING and must never be counted as a
	// disagreement - that conflation is what turns a clean oracle into a false alarm.
	CHECK(judge_resource(0x1000, 0x2000, 0) == AssertVerdict::absent);
	CHECK(judge_resource(0, 0, 0x3000) == AssertVerdict::absent);
	CHECK(judge_resource(0, 0, 0) == AssertVerdict::absent);
}

TEST_CASE("judge_extent treats an unknown extent as absent, never as a mismatch")
{
	CHECK(judge_extent(1920, 1080, 1920, 1080) == AssertVerdict::agree);
	CHECK(judge_extent(1920, 1080, 2560, 1440) == AssertVerdict::disagree);
	CHECK(judge_extent(0, 0, 1920, 1080) == AssertVerdict::absent);
	CHECK(judge_extent(1920, 1080, 0, 0) == AssertVerdict::absent);
}

TEST_CASE("only the scene-buffer-extent targets carry an extent prediction")
{
	CHECK(extent_is_predicted(Target::gbuffer_a));
	CHECK(extent_is_predicted(Target::scene_depth_z));
	CHECK(extent_is_predicted(Target::gbuffer_velocity));
	// SmallDepthZ is half-res and ScreenSpaceAO / the reflection cubemaps have their own sizes;
	// predicting what we have not read is how a counter starts lying.
	CHECK_FALSE(extent_is_predicted(Target::small_depth_z));
	CHECK_FALSE(extent_is_predicted(Target::screen_space_ao));
	CHECK_FALSE(extent_is_predicted(Target::sky_sh_irradiance_map));
}

TEST_CASE("every status and verdict has a name, so a log line can never print a bare number")
{
	for (unsigned i = 0; i < static_cast<unsigned>(LocateStatus::count); ++i)
		CHECK(std::strlen(locate_status_text(static_cast<LocateStatus>(i))) > 0);
	for (unsigned i = 0; i < static_cast<unsigned>(RecordStatus::count); ++i)
		CHECK(std::strlen(record_status_text(static_cast<RecordStatus>(i))) > 0);
	for (unsigned i = 0; i < static_cast<unsigned>(AssertVerdict::count); ++i)
		CHECK(std::strlen(assert_verdict_text(static_cast<AssertVerdict>(i))) > 0);
	for (unsigned i = 0; i <= static_cast<unsigned>(Level::supply); ++i)
		CHECK(std::strlen(level_name(static_cast<Level>(i))) > 0);
	for (std::size_t i = 0; i < kNameCount; ++i)
		CHECK(std::strlen(target_name(static_cast<Target>(i))) > 0);
}
