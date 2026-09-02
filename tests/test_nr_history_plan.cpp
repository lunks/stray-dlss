// The gate that decides whether a frame's pristine, pre-Neural-Rendering colour is snapshotted,
// and whether that snapshot is put back at end of frame.
//
// This mechanism exists to keep NR's residual out of UE 4.27's temporal history (`u0` is both
// this frame's scene colour and the next frame's HistoryBuffer[0] — TemporalAA.cpp:696/:969), and
// its failure mode is INVISIBLE: a missed restore looks exactly like a working one for tens of
// seconds and then shows up as the slow SSR drift the user reported. So every refusal is pinned
// here rather than discovered on the machine, one expensive round trip at a time (CLAUDE.md §0.1).
#include <doctest/doctest.h>

#include "core/nr_history_plan.hpp"

#include <cstring>
#include <initializer_list>

using namespace stray_dlss::histplan;

namespace {

// A frame that passes everything, so each test can spoil exactly one thing. The rect is the
// measured 4K output (CLAUDE.md §2.1) at 50% screen percentage.
SnapshotInputs good_snapshot()
{
	SnapshotInputs in;
	in.nr_enabled = true;
	in.nr_can_apply = true;
	in.have_image = true;
	in.width = 3840;
	in.height = 2160;
	in.mip_levels = 1;
	in.array_size = 1;
	in.sample_count = 1;
	return in;
}

RestoreInputs good_restore()
{
	RestoreInputs in;
	in.have_snapshot = true;
	in.nr_applied = true;
	return in;
}

// The shipped default is OFF (see the Config comment: the fade stopped reproducing on
// 2026-09-01 and the restore rests on an INFERRED state for u0 at present), so every test of
// what the gate DOES has to arm it first. Written as a helper rather than a line in each test
// so that flipping the default again is one edit here plus the one assertion below.
Config armed()
{
	Config c;
	c.enabled = true;
	return c;
}

} // namespace

TEST_CASE("history restore: the SHIPPED DEFAULT IS OFF")
{
	// Pinned deliberately. This is not a style preference: the restore copies at present time,
	// on a command list that is not the game's, into a resource whose D3D12 state is INFERRED
	// from UE 4.27 source rather than measured — the same class of assumption that made the
	// `preui` hook site wreck a frame. It stays a tool you reach for when the SSR fade returns,
	// not a per-frame behaviour change. A future session flipping this on must do so knowingly,
	// which means changing this assertion too.
	const Config shipped;
	CHECK_FALSE(shipped.enabled);
	CHECK(plan_snapshot(shipped, good_snapshot()) == Step::disabled);
	CHECK(plan_restore(shipped, good_restore()) == Step::disabled);
}

TEST_CASE("history restore: the happy path snapshots and restores")
{
	Config cfg = armed();
	CHECK(plan_snapshot(cfg, good_snapshot()) == Step::ok);
	CHECK(plan_restore(cfg, good_restore()) == Step::ok);
	CHECK_FALSE(restore_miss_is_harmful(cfg, good_restore()));
}

TEST_CASE("history restore: NgxNRRestoreHistory=0 is the shipped, drifting behaviour")
{
	Config cfg = armed();
	cfg.enabled = false;
	CHECK(plan_snapshot(cfg, good_snapshot()) == Step::disabled);
	CHECK(plan_restore(cfg, good_restore()) == Step::disabled);
	// Not "harmful": the user asked for it, so it must not be warned about every session.
	CHECK_FALSE(restore_miss_is_harmful(cfg, good_restore()));
}

TEST_CASE("history restore: nothing is copied while NR cannot modify the image")
{
	const Config cfg = armed();

	SUBCASE("NgxNR=0 — DLSS SR alone leaves no residual to undo")
	{
		SnapshotInputs in = good_snapshot();
		in.nr_enabled = false;
		in.nr_can_apply = false;
		CHECK(plan_snapshot(cfg, in) == Step::nr_disabled);
	}
	SUBCASE("warmup / validating / degenerate — this is what stops a permanent 66 MB/frame cost")
	{
		SnapshotInputs in = good_snapshot();
		in.nr_can_apply = false;
		CHECK(plan_snapshot(cfg, in) == Step::nr_cannot_apply);
	}
}

TEST_CASE("history restore: a frame with no image or no rect refuses by its own name")
{
	const Config cfg = armed();

	SUBCASE("no colour resource")
	{
		SnapshotInputs in = good_snapshot();
		in.have_image = false;
		CHECK(plan_snapshot(cfg, in) == Step::no_image);
	}
	SUBCASE("zero width — a mid-resize or minimised frame")
	{
		SnapshotInputs in = good_snapshot();
		in.width = 0;
		CHECK(plan_snapshot(cfg, in) == Step::zero_extent);
	}
	SUBCASE("zero height")
	{
		SnapshotInputs in = good_snapshot();
		in.height = 0;
		CHECK(plan_snapshot(cfg, in) == Step::zero_extent);
	}
}

TEST_CASE("history restore: mipped / arrayed / MSAA is refused, agreeing with nr::apply")
{
	// ngx_nr refuses the identical resource under `mipped-input` (a mipped input to feature 18 is
	// a documented DXGI_ERROR_DEVICE_HUNG rather than an error return), so if this fires NR did
	// not run and there is genuinely nothing to restore. Named separately anyway: "NR was
	// refused" and "NR ran and we could not undo it" want completely different responses.
	const Config cfg = armed();

	SUBCASE("mipped")
	{
		SnapshotInputs in = good_snapshot();
		in.mip_levels = 4;
		CHECK(plan_snapshot(cfg, in) == Step::unsupported_image);
	}
	SUBCASE("arrayed")
	{
		SnapshotInputs in = good_snapshot();
		in.array_size = 2;
		CHECK(plan_snapshot(cfg, in) == Step::unsupported_image);
	}
	SUBCASE("multisampled")
	{
		SnapshotInputs in = good_snapshot();
		in.sample_count = 4;
		CHECK(plan_snapshot(cfg, in) == Step::unsupported_image);
	}
}

TEST_CASE("history restore: the refusal order names the real problem, not a symptom")
{
	// Every gate spoiled at once. The verdict must be the FIRST and most fundamental one, so the
	// counted reason points at what to fix rather than at whatever happened to be checked last.
	Config cfg = armed();
	cfg.enabled = false;
	SnapshotInputs in = good_snapshot();
	in.nr_enabled = false;
	in.nr_can_apply = false;
	in.have_image = false;
	in.width = 0;
	in.mip_levels = 8;
	CHECK(plan_snapshot(cfg, in) == Step::disabled);

	cfg.enabled = true;
	CHECK(plan_snapshot(cfg, in) == Step::nr_disabled);

	in.nr_enabled = true;
	CHECK(plan_snapshot(cfg, in) == Step::nr_cannot_apply);

	in.nr_can_apply = true;
	CHECK(plan_snapshot(cfg, in) == Step::no_image);

	in.have_image = true;
	CHECK(plan_snapshot(cfg, in) == Step::zero_extent);

	in.width = 3840;
	CHECK(plan_snapshot(cfg, in) == Step::unsupported_image);

	in.mip_levels = 1;
	CHECK(plan_snapshot(cfg, in) == Step::ok);
}

TEST_CASE("history restore: a frame NR did not touch is skipped, not reported as a miss")
{
	// This is the steady state of a session with NgxNR off, and of every warmup frame. Reporting
	// it as `no-snapshot` would bury the one instance of that reason that actually matters — so
	// `nr_not_applied` is checked BEFORE the snapshot test, and is never harmful.
	const Config cfg = armed();
	RestoreInputs in;
	in.nr_applied = false;

	SUBCASE("no snapshot either")
	{
		in.have_snapshot = false;
		CHECK(plan_restore(cfg, in) == Step::nr_not_applied);
		CHECK_FALSE(restore_miss_is_harmful(cfg, in));
	}
	SUBCASE("a snapshot was taken but NR then refused, so u0 is the SR image byte for byte")
	{
		in.have_snapshot = true;
		CHECK(plan_restore(cfg, in) == Step::nr_not_applied);
		CHECK_FALSE(restore_miss_is_harmful(cfg, in));
	}
}

TEST_CASE("history restore: NR modified u0 with no pristine copy is the ONE harmful miss")
{
	// The bug returning, in one predicate. This frame's neural residual reached the engine's
	// temporal history and nothing can put it back, so it earns a warning where every other
	// no-restore path earns silence.
	const Config cfg = armed();
	RestoreInputs in;
	in.have_snapshot = false;
	in.nr_applied = true;

	CHECK(plan_restore(cfg, in) == Step::no_snapshot);
	CHECK(restore_miss_is_harmful(cfg, in));

	// ...but only where the mechanism is supposed to be doing anything at all.
	Config off = cfg;
	off.enabled = false;
	CHECK_FALSE(restore_miss_is_harmful(off, in));
}

TEST_CASE("history restore: every Step has a distinct, non-empty name")
{
	// The periodic report and the one-WARN-per-reason logic both index by Step, so a name table
	// that has drifted out of sync with the enum silently mislabels every refusal. kStepCount is
	// duplicated in nr_history_plan.cpp for exactly this reason; this is the check that the
	// duplication was kept honest.
	for (int i = 0; i < kStepCount; ++i)
	{
		const char *a = step_name(static_cast<Step>(i));
		REQUIRE(a != nullptr);
		CHECK(a[0] != '\0');
		CHECK(std::strcmp(a, "?") != 0);
		for (int j = i + 1; j < kStepCount; ++j)
			CHECK(std::strcmp(a, step_name(static_cast<Step>(j))) != 0);
	}
	// Out of range stays diagnosable rather than reading past the table.
	CHECK(std::strcmp(step_name(static_cast<Step>(kStepCount)), "?") == 0);
	CHECK(std::strcmp(step_name(static_cast<Step>(-1)), "?") == 0);
	// The last enumerator really is the last name: a Step added without a name would make this
	// fail rather than silently report "?" from the field.
	CHECK(std::strcmp(step_name(Step::no_command_list), "no-command-list") == 0);
}
