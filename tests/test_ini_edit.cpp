// The ini save-back, which must never damage the file that drives the box.
//
// StrayDLSS.ini carries the reasoning for every knob as comments above it, and CLAUDE.md's rule
// is that stale keys persist across deploys. A "Save to ini" button that rewrote the file from
// the parsed map would delete both. These tests pin the property that matters: every byte the
// editor was not asked to change comes back unchanged.
#include "host/ini.hpp"
#include "host/ini_edit.hpp"

#include <doctest/doctest.h>

using stray_dlss::host::IniEdit;
using stray_dlss::host::ini_apply_edits;
using stray_dlss::host::ini_format_bool;
using stray_dlss::host::ini_format_float;
using stray_dlss::host::ini_format_int;

namespace {

const char *const kSample =
	"; StrayDLSS - configuration\n"
	"; a second comment line\n"
	"\n"
	"[STRAYDLSS]\n"
	"NativeMode=drive\n"
	"; why NgxNRIntensity is the safe knob\n"
	"NgxNRIntensity=1\n"
	"NgxNRLocalTone=1.05\n"
	"NgxNR=1\n"
	"\n"
	"; trailing commentary about the whole file\n";

} // namespace

TEST_CASE("ini_edit: an existing key is rewritten and nothing else moves")
{
	const std::string out = ini_apply_edits(kSample, "STRAYDLSS", { { "NgxNRIntensity", "1.75" } });

	CHECK(out.find("NgxNRIntensity=1.75") != std::string::npos);
	// The comment above it, the keys around it and the trailing commentary all survive.
	CHECK(out.find("; why NgxNRIntensity is the safe knob") != std::string::npos);
	CHECK(out.find("NativeMode=drive") != std::string::npos);
	CHECK(out.find("NgxNRLocalTone=1.05") != std::string::npos);
	CHECK(out.find("; trailing commentary about the whole file") != std::string::npos);
	CHECK(out.find("; StrayDLSS - configuration") != std::string::npos);
	// And the old value is gone.
	CHECK(out.find("NgxNRIntensity=1\n") == std::string::npos);
}

TEST_CASE("ini_edit: a key absent from the section is appended INSIDE it")
{
	const std::string out = ini_apply_edits(kSample, "STRAYDLSS", { { "NgxNRStyle", "2" } });
	const std::size_t style = out.find("NgxNRStyle=2");
	REQUIRE(style != std::string::npos);
	// It must land after the section's last key and before the trailing comment, so that it is
	// inside [STRAYDLSS] rather than dangling at the end of the file.
	CHECK(style > out.find("NgxNR=1"));
	CHECK(style < out.find("; trailing commentary"));
	CHECK(out.find("[STRAYDLSS]") < style);
}

TEST_CASE("ini_edit: keys match case-insensitively and keep their original spelling")
{
	const std::string out = ini_apply_edits(kSample, "straydlss", { { "ngxnrintensity", "0.5" } });
	// The file's own spelling is preserved; only the value changed.
	CHECK(out.find("NgxNRIntensity=0.5") != std::string::npos);
	CHECK(out.find("ngxnrintensity") == std::string::npos);
}

TEST_CASE("ini_edit: the spacing around '=' is preserved")
{
	const char *const spaced = "[STRAYDLSS]\nNgxNRIntensity = 1\n";
	const std::string out = ini_apply_edits(spaced, "STRAYDLSS", { { "NgxNRIntensity", "2" } });
	CHECK(out == "[STRAYDLSS]\nNgxNRIntensity = 2\n");
}

TEST_CASE("ini_edit: a key in ANOTHER section is not touched")
{
	const char *const two =
		"[OTHER]\n"
		"NgxNRIntensity=9\n"
		"[STRAYDLSS]\n"
		"NgxNRIntensity=1\n";
	const std::string out = ini_apply_edits(two, "STRAYDLSS", { { "NgxNRIntensity", "3" } });
	CHECK(out == "[OTHER]\nNgxNRIntensity=9\n[STRAYDLSS]\nNgxNRIntensity=3\n");
}

TEST_CASE("ini_edit: CRLF is preserved, including on an inserted line")
{
	const char *const crlf = "[STRAYDLSS]\r\nNgxNR=1\r\n";
	const std::string out = ini_apply_edits(crlf, "STRAYDLSS", { { "NgxNR", "0" }, { "NgxFG", "1" } });
	CHECK(out == "[STRAYDLSS]\r\nNgxNR=0\r\nNgxFG=1\r\n");
}

TEST_CASE("ini_edit: a file with no final newline is terminated before an append")
{
	const char *const nonl = "[STRAYDLSS]\nNgxNR=1";
	const std::string out = ini_apply_edits(nonl, "STRAYDLSS", { { "NgxFG", "1" } });
	CHECK(out == "[STRAYDLSS]\nNgxNR=1\nNgxFG=1\n");
}

TEST_CASE("ini_edit: a missing section is appended with its header")
{
	const char *const none = "; just a comment\n";
	const std::string out = ini_apply_edits(none, "STRAYDLSS", { { "NgxNR", "1" } });
	CHECK(out.find("[STRAYDLSS]") != std::string::npos);
	CHECK(out.find("NgxNR=1") != std::string::npos);
	CHECK(out.find("; just a comment") != std::string::npos);
	CHECK(out.find("[STRAYDLSS]") > out.find("; just a comment"));
}

TEST_CASE("ini_edit: EVERY duplicate of a key is rewritten, so the parser cannot read a stale one")
{
	// ini.cpp's map keeps the LAST occurrence. Rewriting only the first would leave the file
	// looking edited and reading unedited - the quiet-wrong-value class.
	const char *const dup = "[STRAYDLSS]\nNgxNR=1\nNgxNR=1\n";
	const std::string out = ini_apply_edits(dup, "STRAYDLSS", { { "NgxNR", "0" } });
	CHECK(out == "[STRAYDLSS]\nNgxNR=0\nNgxNR=0\n");
}

TEST_CASE("ini_edit: no edits is the identity")
{
	CHECK(ini_apply_edits(kSample, "STRAYDLSS", {}) == kSample);
}

TEST_CASE("ini_edit: a saved value round-trips through the parser that reads it back")
{
	// The whole point of the writer: what the UI shows, what the file says and what the plugin
	// reads on the next launch must be the same number.
	using stray_dlss::host::IniFile;
	const float kValues[] = { 0.0f, 1.0f, 1.05f, 1.61f, 2.0f, 0.125f, 12.5f };
	for (const float v : kValues)
	{
		IniFile ini;
		ini.set_for_test("STRAYDLSS", "K", ini_format_float(v).c_str());
		std::string back;
		REQUIRE(ini.get("STRAYDLSS", "K", back));
		CHECK(stray_dlss::host::ini_parse_float(back, -1.0f) == doctest::Approx(v));
	}
	CHECK(ini_format_int(-3) == "-3");
	CHECK(ini_format_bool(true) == "1");
	CHECK(ini_format_bool(false) == "0");
	CHECK(ini_format_float(1.0f) == "1");
	CHECK(ini_format_float(1.05f) == "1.05");
}
