// The recorder half of the G-buffer finder: watches render-target binds for the base
// pass's MRT set, classifies it with core/gbuffer_classify.hpp, and logs the identification
// once it has held stable — phase 1 of DLSS Ray Reconstruction.
//
// Diagnostic ONLY, behind [STRAYDLSS] GBufferFinder=1 (default off). STRICTLY log-only: it
// never suppresses, never replaces, never touches the image, and changes nothing when off.
// Its output is the log lines a later observation run on the target machine validates;
// DLSS-RR will consume the identified resources only in a later phase.
//
// A separate module rather than an extension of pass_finder, deliberately: pass_finder
// records two consecutive frames out of every thirty because its dataflow walk needs
// descriptor-table resolves per dispatch and per full-screen draw, which are expensive.
// This finder needs none of that — its per-frame cost is describing RTVs at bind time
// (a couple per bind; classification itself only runs for the rare >= 4-RTV sets) — so it
// observes EVERY frame, which is what makes "the identification held for N consecutive
// frames" a meaningful stability claim. Welding the two recorders
// together would force one cadence onto both and destabilise a diagnostic that already
// works. The one expensive thing here, the SSR-denoiser cross-check, resolves compute
// bindings only for dispatches of the one known denoiser hash.
//
// Cross-frame identity is the SHAPE — (slot, format, extent) per member — never resource
// pointers: measured 2026-08-31, the same 8-target shape held for minutes while "STABLE"
// never fired, the signature of the render-target pool rotating pointers under a
// pointer-keyed identity (by register, never by pointer alone — CLAUDE.md §2.9, now
// applied fully). Pointers are still reported per frame, and their rotation rate is
// counted in the stable report — phase 3's capture design needs that number. Every view
// is liveness-checked before it is described, via the same describe_bound_view every
// other recorder uses (CLAUDE.md §5, "Two descriptor hazards"). No descriptor is ever
// copied out of the game's heap.
#pragma once

#include "core/gbuffer_classify.hpp"

#include "intercept/types.hpp"

#include <cstdint>

namespace stray_dlss::gbuffer_finder {

// Read once at attach, before any event can fire.
void set_enabled(bool enabled);
bool enabled();

// Recording taps, called from the add-on's shared finder event handlers. Cheap no-ops
// when disabled.
void note_render_targets(const icept::CommandContext &ctx, uint32_t count,
                         const icept::DescriptorId *rtvs, icept::DescriptorId dsv);
void note_draw(const icept::CommandContext &ctx);

// The SSR-denoiser cross-check tap: `shader_hash` is the DXBC hash of the compute pipeline
// bound at this dispatch (0 when unknown). Bindings are resolved ONLY when it is the known
// denoiser look-alike 0x1708ec956099e259 (CLAUDE.md §2.3) — a resource appearing both as a
// base-pass MRT and as one of that dispatch's SRVs corroborates a G-buffer identity. The
// hash is a permutation fingerprint measured at one configuration, so its absence proves
// nothing and the report says so.
void note_dispatch(const icept::CommandContext &ctx, std::uint64_t shader_hash);

void forget_command_list(const icept::CommandContext &ctx);

// The live identification, for the RR path — ROLE-KEYED, deliberately not shape-keyed.
//
// Measured 2026-08-31 (the RR-0 starvation run): stability was earned by the 8-target
// load-boundary shape while frames kept binding accepted candidates whose shape differed,
// and the old "freshest same-shape bind" rule then refused EVERY frame — 0% RR with no
// reason in the log. Both the velocity-free 6-RTV set and the with-velocity 7-RTV set are
// ACCEPTED candidates that name the same A/B/C roles, so the serving rule is now: A/B/C
// resources from the freshest accepted candidate of ANY accepted shape. Stability (any
// candidate shape holding kStableFrames) is only the initial ARMING gate; once armed it
// never gates again — freshness and role presence do.
//
// The measured pointer rotation (29 of 30 stable frames) means a consumer must re-capture
// every frame and still liveness-check each handle before touching it: the table is from
// a bind earlier in the frame, and the pool can have recycled it since.
struct Identification
{
	std::uint64_t gbuffer_a = 0;
	std::uint64_t gbuffer_b = 0;
	std::uint64_t gbuffer_c = 0;
	std::uint32_t extent_width = 0;
	std::uint32_t extent_height = 0;
	// Presents since the serving bind was recorded: 0 = this frame's interval. The caller
	// gets it for telemetry; anything the accessor would refuse as stale never comes back.
	std::uint32_t age_frames = 0;
	// True when the serving candidate carried a velocity member at stock slot 4 — the
	// shape note for the evaluate log line.
	bool velocity_in_set = false;
};

// Why current_identification refused — each maps to a fallback-telemetry counter, because
// the starvation run proved a reasonless refusal costs a whole round-trip.
enum class IdentRefusal
{
	ok = 0,
	not_enabled,  // the finder is off
	not_armed,    // no candidate shape has ever held kStableFrames
	no_candidate, // armed, but no accepted bind recorded yet (transient)
	stale_bind,   // the freshest accepted bind is too many presents old
	roles_missing,// the freshest candidate has an unknown A/B/C slot (licensee format)
};

// Fills `out` and returns ok, or names the refusal. `stale_age_out` reports the bind age
// on stale_bind so the first-occurrence log can carry the number.
IdentRefusal current_identification(Identification &out, std::uint32_t *stale_age_out = nullptr);

// Frame boundary: merges the frame's candidates, tracks stability, and logs the
// identification once stable (again if it changes). While no base-pass candidate is seen
// it logs a FAILED block — first after kFailFrames, then RE-FIRING once a minute for as
// long as the drought lasts — each carrying that window's event census (bind counts, RTV
// histogram, velocity sightings, rejection tallies), because in a negative world the
// census IS the observation. Every line contains the grep token GBUF.
void on_present(std::uint64_t frame);

} // namespace stray_dlss::gbuffer_finder
