// THE CAMERA-CUT RESET FROM THE ENGINE ([STRAYDLSS] EngineSeamReset).
//
// CLAUDE.md §2.8: DLSS SR's InReset — and, travelling with it, DLSSG.Reset and DLSSNR.Reset —
// is the OR of three signals:
//   1. View.CameraCut (row 145.x)                       the engine's own bCameraCut
//   2. TemporalAAJitter.zw == .xy                       a measured heuristic for the same bit
//   3. the history or velocity SRV is a 1x1 dummy       !InputHistory.IsValid(), which never
//                                                       reaches the View buffer
//
// Signal 2 exists because the View constant buffer used to be found by SEARCH, and a search can
// hand back another view's — or a stale ring copy's — row 145 (facts §36.20). Since
// EngineSeamViewParams=2 the View is the engine's own FViewInfo::CachedViewUniformShaderParameters
// (src/core/view_cached.hpp), so signal 1 is a STATED fact on every frame the struct is latched,
// and signal 2 is a heuristic standing beside it that can only ADD resets. Every needless reset
// is a whole-history wipe in three temporal accumulators at once — DLSS SR, DLSS-G and feature
// 18 — which is exactly the shape of the flicker the user is chasing.
//
// Signal 3 is KEPT at every level: it is the one half of UE4's `bCameraCut =
// !InputHistory.IsValid() || View.bCameraCut` (TemporalAA.cpp) that the View cannot carry.
//
//   level 0  off      the OR, uncounted (the pre-seam behaviour)
//   level 1  observe  the OR, with every combination of the three signals COUNTED per frame and
//                     the "jitter alone" case named — the number that says how many resets the
//                     heuristic has been adding
//   level 2  engine   when the View came from the engine's struct: reset = CameraCut | 1x1, and
//                     jitter-equality is an ASSERTION (counted, never acted on). When it did not
//                     (no latch yet, or EngineSeamViewParams < 2): the OR, counted as a fallback.
#pragma once

#include <cstdint>

namespace stray_dlss::resetplan {

enum class Mode : std::uint8_t
{
	off = 0,
	observe,
	engine,
};
Mode mode_from_level(int level);
const char *mode_name(Mode m);

// Shipped default, pinned by a test: observe.
constexpr int kDefaultLevel = 1;

struct Signals
{
	bool camera_cut = false;   // View.CameraCut != 0
	bool jitter_equal = false; // TemporalAAJitter.zw == .xy
	bool history_1x1 = false;  // history or velocity SRV is the 1x1 dummy
	bool engine_view = false;  // the View was supplied by the engine's struct this frame
};

// The eight combinations, as a 3-bit index: bit0 camera_cut, bit1 jitter_equal, bit2 history_1x1.
constexpr unsigned kCombos = 8;
constexpr unsigned combo_index(const Signals &s)
{
	return (s.camera_cut ? 1u : 0u) | (s.jitter_equal ? 2u : 0u) | (s.history_1x1 ? 4u : 0u);
}
const char *combo_name(unsigned combo);

enum class Source : std::uint8_t
{
	heuristic_or = 0, // the three-way OR
	engine,           // CameraCut | 1x1, the engine's word
};

struct Decision
{
	bool reset = false;
	Source source = Source::heuristic_or;
	bool fell_back = false;    // level 2 asked for the engine and the View was not the engine's
	bool jitter_only = false;  // jitter_equal fired with neither of the other two: the heuristic's
	                           // own contribution — a reset the OR would take and the engine would not
};
Decision decide(Mode mode, const Signals &s);

struct Counters
{
	std::uint64_t frames = 0;
	std::uint64_t combo[kCombos] = {};
	std::uint64_t resets = 0;             // decisions that reset
	std::uint64_t from_engine = 0;        // decided by the engine's word (level 2, latched)
	std::uint64_t fell_back = 0;          // level 2, View not the engine's: the OR decided
	std::uint64_t jitter_only = 0;        // frames where only signal 2 fired
	std::uint64_t jitter_only_suppressed = 0; // ...and level 2 did NOT reset (the saving)
	std::uint64_t jitter_only_fired = 0;      // ...and the OR did reset (levels 0/1, or fallback)

	void note(const Signals &s, const Decision &d);
};

} // namespace stray_dlss::resetplan
