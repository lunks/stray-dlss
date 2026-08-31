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
// This finder needs none of that — its per-frame cost is describing the RTVs of the rare
// binds with >= 4 render targets (the base-pass MRT width; everything else in the frame
// binds 0-2) — so it observes EVERY frame, which is what makes "the identification held
// for N consecutive frames" a meaningful stability claim. Welding the two recorders
// together would force one cadence onto both and destabilise a diagnostic that already
// works. The one expensive thing here, the SSR-denoiser cross-check, resolves compute
// bindings only for dispatches of the one known denoiser hash.
//
// Cross-frame identity is (slot, resource, format) per member — by REGISTER first, never
// by pointer alone (CLAUDE.md §2.9) — and every view is liveness-checked before it is
// described, via the same describe_bound_view every other recorder uses (CLAUDE.md §5,
// "Two descriptor hazards"). No descriptor is ever copied out of the game's heap.
#pragma once

#include "core/gbuffer_classify.hpp"

#include "reshade_all.hpp"

#include <cstdint>

namespace stray_dlss::gbuffer_finder {

// Read once at attach, before any event can fire.
void set_enabled(bool enabled);
bool enabled();

// Recording taps, called from the add-on's shared finder event handlers. Cheap no-ops
// when disabled.
void note_render_targets(reshade::api::command_list *cmd_list, uint32_t count,
                         const reshade::api::resource_view *rtvs, reshade::api::resource_view dsv);
void note_draw(reshade::api::command_list *cmd_list);

// The SSR-denoiser cross-check tap: `shader_hash` is the DXBC hash of the compute pipeline
// bound at this dispatch (0 when unknown). Bindings are resolved ONLY when it is the known
// denoiser look-alike 0x1708ec956099e259 (CLAUDE.md §2.3) — a resource appearing both as a
// base-pass MRT and as one of that dispatch's SRVs corroborates a G-buffer identity. The
// hash is a permutation fingerprint measured at one configuration, so its absence proves
// nothing and the report says so.
void note_dispatch(reshade::api::command_list *cmd_list, std::uint64_t shader_hash);

void forget_command_list(reshade::api::command_list *cmd_list);

// Frame boundary: merges the frame's candidates, tracks stability, and logs the
// identification once stable (again if it changes). While no base-pass candidate is seen
// it logs a FAILED block — first after kFailFrames, then RE-FIRING once a minute for as
// long as the drought lasts — each carrying that window's event census (bind counts, RTV
// histogram, velocity sightings, rejection tallies), because in a negative world the
// census IS the observation. Every line contains the grep token GBUF.
void on_present(std::uint64_t frame);

} // namespace stray_dlss::gbuffer_finder
