#include "host/config.hpp"
#include "host/ini.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <cstring>
#include <string>

TEST_CASE("IniFile parses sections, comments, and keeps unknown keys")
{
	const char *path = "test_ini_tmp.ini";
	std::FILE *f = std::fopen(path, "w");
	REQUIRE(f != nullptr);
	std::fputs("; comment\n# another\n[STRAYDLSS]\nNgxRR = 2\nNgxNRHook=preui\nHashShaders=false\n"
	           "NgxPassHash = 0xd2e4d8c23c362ed1 \nEmpty=\n[OTHER]\nNgxRR=9\n", f);
	std::fclose(f);

	stray_dlss::host::IniFile ini;
	REQUIRE(ini.load(path));
	std::string v;
	CHECK(ini.get("STRAYDLSS", "NgxRR", v)); CHECK(v == "2");
	CHECK(ini.get("STRAYDLSS", "NgxNRHook", v)); CHECK(v == "preui");
	CHECK(ini.get("STRAYDLSS", "HashShaders", v)); CHECK(v == "false");
	CHECK(ini.get("STRAYDLSS", "NgxPassHash", v)); CHECK(v == "0xd2e4d8c23c362ed1");
	CHECK(ini.get("STRAYDLSS", "Empty", v)); CHECK(v.empty());
	CHECK_FALSE(ini.get("STRAYDLSS", "Missing", v));
	CHECK(ini.get("OTHER", "NgxRR", v)); CHECK(v == "9");
	// Case-insensitive on section and key, like ReShade's parser.
	CHECK(ini.get("straydlss", "ngxrr", v)); CHECK(v == "2");
	CHECK(ini.size() == 6);
	std::remove(path);
}

TEST_CASE("IniFile: a missing file loads false and empty; reload_if_changed only fires on mtime")
{
	stray_dlss::host::IniFile ini;
	CHECK_FALSE(ini.load("definitely_not_here_9f3.ini"));
	CHECK(ini.size() == 0);
	CHECK_FALSE(ini.reload_if_changed()); // no path

	const char *path = "test_ini_reload.ini";
	std::FILE *f = std::fopen(path, "w");
	REQUIRE(f != nullptr);
	std::fputs("[STRAYDLSS]\nA=1\n", f);
	std::fclose(f);
	REQUIRE(ini.load(path));
	CHECK_FALSE(ini.reload_if_changed()); // unchanged
	std::string v;
	ini.set_for_test("STRAYDLSS", "B", " 7 ");
	CHECK(ini.get("STRAYDLSS", "B", v)); CHECK(v == "7");
	std::remove(path);
}

TEST_CASE("ini coercions: bool accepts 0/1/true/false, int via strtol (hex ok), float via strtod")
{
	using namespace stray_dlss::host;
	CHECK(ini_parse_bool("1", false) == true);
	CHECK(ini_parse_bool("true", false) == true);
	CHECK(ini_parse_bool("True", false) == true);
	CHECK(ini_parse_bool("0", true) == false);
	CHECK(ini_parse_bool("false", true) == false);
	CHECK(ini_parse_bool("garbage", true) == true);
	CHECK(ini_parse_int("11", 0) == 11);
	CHECK(ini_parse_int("0x10", 0) == 16);
	CHECK(ini_parse_int("-3", 0) == -3);
	CHECK(ini_parse_int("", 7) == 7);
	CHECK(ini_parse_int("x", 7) == 7);
	CHECK(ini_parse_float("0.056", 1.0f) == doctest::Approx(0.056f));
	CHECK(ini_parse_float("1.605", 1.0f) == doctest::Approx(1.605f));
	CHECK(ini_parse_float("", 2.5f) == doctest::Approx(2.5f));
}

TEST_CASE("cfg getters coerce like ReShade did and honour the Source contract")
{
	struct Src : stray_dlss::host::cfg::Source
	{
		bool get(const char *key, char *buf, std::size_t *size) override
		{
			std::string k = key, v;
			if (k == "B") v = "true";
			else if (k == "I") v = "11";
			else if (k == "H") v = "0xd2e4d8c23c362ed1";
			else if (k == "F") v = "0.056";
			else if (k == "S") v = "preui";
			else if (k == "Long") v = std::string(300, 'x');
			else return false;
			if (*size <= v.size()) { *size = v.size() + 1; return false; }
			std::snprintf(buf, *size, "%s", v.c_str());
			*size = v.size() + 1;
			return true;
		}
	} src;
	namespace cfg = stray_dlss::host::cfg;
	cfg::set_source(&src);
	CHECK(cfg::get_bool("B", false) == true);
	CHECK(cfg::get_int("I", 0) == 11);
	CHECK(cfg::get_float("F", 1.0f) == doctest::Approx(0.056f));
	CHECK(cfg::get_int("nope", 7) == 7);
	CHECK(cfg::has("I"));
	CHECK_FALSE(cfg::has("nope"));
	char buf[8] = "zz";
	CHECK_FALSE(cfg::get_string("nope", buf, sizeof buf));
	CHECK(buf[0] == '\0');
	CHECK(cfg::get_string("S", buf, sizeof buf));
	CHECK(std::strcmp(buf, "preui") == 0);
	char hash[32] = "";
	CHECK(cfg::get_string("H", hash, sizeof hash));
	CHECK(std::strcmp(hash, "0xd2e4d8c23c362ed1") == 0);
	// Too small a caller buffer: false, empty, never overrun.
	char tiny[4] = "";
	CHECK_FALSE(cfg::get_string("S", tiny, sizeof tiny));
	CHECK(tiny[0] == '\0');
	// A scalar longer than the internal buffer is treated as absent rather than truncated.
	CHECK(cfg::get_int("Long", 5) == 5);
	cfg::set_source(nullptr);
	CHECK(cfg::get_bool("B", false) == false);
	CHECK_FALSE(cfg::get_string("S", buf, sizeof buf));
}
