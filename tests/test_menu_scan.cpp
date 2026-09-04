// The empirical offset search, pinned before it is trusted against a live object.
//
// This is the half of the menu work that CAN be proven in CI: the caller supplies the object's
// bytes, the engine supplies the bound and the exclusions, and everything interesting about the
// SEARCH is arithmetic over a buffer. What CI cannot prove is that the offset it finds is the
// list the row reads — only the box says that.
#include <doctest/doctest.h>

#include "core/menu_scan.hpp"

#include <cstring>
#include <vector>

using namespace stray_dlss::menuscan;

namespace {

// The shipped Screen Percentage values, measured: sixteen, 50..200 step 10.
const std::uint32_t kNeedle[] = { 50, 60, 70, 80, 90, 100, 110, 120,
                                  130, 140, 150, 160, 170, 180, 190, 200 };
constexpr std::size_t kNeedleLen = sizeof(kNeedle) / sizeof(kNeedle[0]);

void put_u32(std::vector<std::uint8_t> &b, std::size_t off, std::uint32_t v)
{
	std::memcpy(b.data() + off, &v, sizeof(v));
}
void put_u64(std::vector<std::uint8_t> &b, std::size_t off, std::uint64_t v)
{
	std::memcpy(b.data() + off, &v, sizeof(v));
}

} // namespace

TEST_CASE("the inline shape is found at its offset")
{
	std::vector<std::uint8_t> obj(1024, 0);
	const std::size_t at = 256;
	for (std::size_t i = 0; i < kNeedleLen; ++i)
		put_u32(obj, at + i * 4, kNeedle[i]);

	const std::vector<Hit> hits = scan(obj.data(), obj.size(), kNeedle, kNeedleLen, {});
	REQUIRE(hits.size() == 1);
	CHECK(hits[0].kind == HitKind::inline_values);
	CHECK(hits[0].offset == at);
	CHECK(hits[0].count == kNeedleLen);
	CHECK(hits[0].inside_reflected == false);
}

TEST_CASE("a hit inside a reflected property is reported, not dropped")
{
	// This is the expected outcome for m_screenPercentages itself: the search SHOULD find it,
	// and saying so is evidence the search works rather than noise to hide.
	std::vector<std::uint8_t> obj(1024, 0);
	const std::size_t at = 256;
	for (std::size_t i = 0; i < kNeedleLen; ++i)
		put_u32(obj, at + i * 4, kNeedle[i]);

	std::vector<Range> excluded;
	excluded.push_back(Range{ 240, 128 }); // covers 256..368

	const std::vector<Hit> hits = scan(obj.data(), obj.size(), kNeedle, kNeedleLen, excluded);
	REQUIRE(hits.size() == 1);
	CHECK(hits[0].inside_reflected == true);
}

TEST_CASE("the TArray shape is a candidate only when Num matches and Max >= Num")
{
	std::vector<std::uint8_t> obj(256, 0);

	SUBCASE("well-formed header is a candidate")
	{
		put_u64(obj, 64, 0x0000024ABCDEF000ull);
		put_u32(obj, 72, kNeedleLen);
		put_u32(obj, 76, 32);
		const std::vector<Hit> hits = scan(obj.data(), obj.size(), kNeedle, kNeedleLen, {});
		REQUIRE(hits.size() == 1);
		CHECK(hits[0].kind == HitKind::array_header);
		CHECK(hits[0].offset == 64);
		CHECK(hits[0].data_ptr == 0x0000024ABCDEF000ull);
		CHECK(hits[0].capacity == 32);
	}
	SUBCASE("capacity below count is not an array, it is two unrelated integers")
	{
		put_u64(obj, 64, 0x0000024ABCDEF000ull);
		put_u32(obj, 72, kNeedleLen);
		put_u32(obj, 76, 4);
		CHECK(scan(obj.data(), obj.size(), kNeedle, kNeedleLen, {}).empty());
	}
	SUBCASE("a null or unaligned pointer is refused before any dereference is considered")
	{
		put_u32(obj, 72, kNeedleLen);
		put_u32(obj, 76, 32);
		put_u64(obj, 64, 0);
		CHECK(scan(obj.data(), obj.size(), kNeedle, kNeedleLen, {}).empty());
		put_u64(obj, 64, 0x0000024ABCDEF001ull); // 1-aligned
		CHECK(scan(obj.data(), obj.size(), kNeedle, kNeedleLen, {}).empty());
	}
	SUBCASE("a wrong count is not a candidate")
	{
		put_u64(obj, 64, 0x0000024ABCDEF000ull);
		put_u32(obj, 72, 8);
		put_u32(obj, 76, 32);
		CHECK(scan(obj.data(), obj.size(), kNeedle, kNeedleLen, {}).empty());
	}
}

TEST_CASE("nothing is found in an object that does not contain it")
{
	std::vector<std::uint8_t> obj(4096, 0xAB);
	CHECK(scan(obj.data(), obj.size(), kNeedle, kNeedleLen, {}).empty());
}

TEST_CASE("the search never reads past the buffer it was given")
{
	// The needle placed so that one more uint32 would run off the end. If the loop bound were
	// wrong this would read out of bounds, which ASAN in CI would catch; the assertion here is
	// simply that it terminates and finds it.
	std::vector<std::uint8_t> obj(kNeedleLen * 4, 0);
	for (std::size_t i = 0; i < kNeedleLen; ++i)
		put_u32(obj, i * 4, kNeedle[i]);
	const std::vector<Hit> hits = scan(obj.data(), obj.size(), kNeedle, kNeedleLen, {});
	REQUIRE(hits.size() == 1);
	CHECK(hits[0].offset == 0);

	// And a buffer too small for the needle finds nothing rather than misbehaving.
	CHECK(scan(obj.data(), 8, kNeedle, kNeedleLen, {}).empty());
	CHECK(scan(nullptr, 0, kNeedle, kNeedleLen, {}).empty());
	CHECK(scan(obj.data(), obj.size(), nullptr, 0, {}).empty());
}

TEST_CASE("overlap is half-open and a zero-sized range covers nothing")
{
	std::vector<Range> ex;
	ex.push_back(Range{ 100, 10 }); // [100, 110)
	CHECK(overlaps(ex, 90, 10) == false);  // [90,100) touches but does not overlap
	CHECK(overlaps(ex, 110, 10) == false); // [110,120)
	CHECK(overlaps(ex, 99, 2) == true);
	CHECK(overlaps(ex, 109, 2) == true);
	ex.clear();
	ex.push_back(Range{ 100, 0 });
	CHECK(overlaps(ex, 100, 4) == false);
}

TEST_CASE("plausible_heap_ptr admits win64 heap and refuses the rest")
{
	CHECK(plausible_heap_ptr(0x0000024ABCDEF000ull));
	CHECK(!plausible_heap_ptr(0));
	CHECK(!plausible_heap_ptr(0x1000));                // inside the null page region
	CHECK(!plausible_heap_ptr(0x0000024ABCDEF004ull)); // not 8-aligned
	CHECK(!plausible_heap_ptr(0xFFFFFFFFFFFFFFFFull)); // kernel / poison
}
