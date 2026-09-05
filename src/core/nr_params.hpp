// The DLSSNR rect parameters, as data — names AND types.
//
// WHY THIS IS NOT JUST A SEQUENCE OF Set() CALLS. `NVSDK_NGX_Parameter` declares SEPARATE virtual
// overloads for `int` and `unsigned int` (nvsdk_ngx_params.h:58-59), so `Set(name, 0u)` and
// `Set(name, 0)` land in different vtable slots and store under different type tags. The block is
// an untyped string->value map with no validation and no error for a mismatch, so a value written
// as unsigned and read as signed is indistinguishable — from our side — from one that was
// accepted and used. That is the identical failure mode as `DLSSNR.Scale`, a key this runtime
// does not have, which we wrote for weeks while every scaling ratio was silently discarded
// (docs/RESEARCH-RENODX-DLSS5.md §2.2.1).
//
// THE AUDIT [SOFT]. A sibling port of this integration states, from its own disassembly of the
// snippet, that "the snippet Gets subrects as int, which is what is written here, whereas that
// fork writes unsigned and absorbs the mismatch in a type-agnostic parameter bag of its own
// rather than the driver's" (RemixProjGroup/dxvk-remix, branch dlss-nr @ 2df9c812). "That fork"
// is a DXVK integration whose parameter bag is its own code; ours is the real
// `NVSDK_NGX_Parameter`, so the mismatch would reach the runtime. We have not disassembled
// nvngx_dlssnr.dll ourselves for this, so the claim is SOFT — but the change is free, the signed
// form is what the one available reading of the binary says, and being wrong in the other
// direction costs a silently ignored rect.
//
// Everything here is pure, so the types are pinned in CI (tests/test_nr_params.cpp) rather than
// asserted in a comment.
#pragma once

#include <cstdint>

namespace stray_dlss::nrparam {

enum class Type
{
	i32,
	u32,
	f32,
};

struct Entry
{
	const char *name = "";
	Type type = Type::i32;
	int i = 0;
	unsigned int u = 0;
	float f = 0.0f;
};

// The three rects feature 18 is told about, plus the output. Colour is the OUTPUT rect under the
// post-process topology (the image is already upscaled by SR); depth and motion vectors share the
// RENDER-resolution guide grid.
//
// The subrects are what tell the runtime where the guides live, which is why DLSSNR.MVecScaleX/Y
// is 1.0 and not the colour/guide ratio: declaring the subrect AND scaling the vectors
// double-counts (CLAUDE.md, "Three ways to hand feature 18 something that hangs the GPU").
struct Rects
{
	std::uint32_t color_width = 0;
	std::uint32_t color_height = 0;
	std::uint32_t guide_width = 0;
	std::uint32_t guide_height = 0;
	std::uint32_t output_width = 0;
	std::uint32_t output_height = 0;
};

constexpr int kMaxRectEntries = 16;  // four rects x baseX/baseY/width/height

// The OPTIONAL fifth rect: DLSSNR.ControlMask's. Separate from build_rects because the mask is
// optional and the other four are not — an absent mask must emit no subrect entries at all, and a
// leftover subrect from a frame that had one is a rect the runtime would keep reading. (The
// parameter block persists across evaluates; nothing clears it.)
constexpr int kMaxMaskRectEntries = 4;
constexpr int kMaxCreateEntries = 3; // Width, Height, ScalingRatio

// Writes the evaluate-time rect block. Returns the number of entries written, or 0 if `cap` is
// too small — never a partial block, because a half-written set of subrects is a rect the runtime
// would read from the previous frame.
int build_rects(const Rects &rects, Entry *out, int cap);

// Writes the create-time extent block. `in_width`/`in_height` are the rect the network works at.
int build_create(std::uint32_t in_width, std::uint32_t in_height, float scaling_ratio,
                 Entry *out, int cap);

// The ControlMask subrect, in the same types as the other four (int, vtable slot +0x58 — the
// runtime reads it through exactly the same getter as the Color/Depth/MVec/Output subrects).
int build_mask_rect(std::uint32_t width, std::uint32_t height, Entry *out, int cap);

// The OPTIONAL Backbuffer subrect (DLSSNR.Backbuffer + SubrectBaseX/Y/Width/Height, every name
// in the runtime's own string table - docs/RESEARCH-DLSSNR-STYLES.md §2, HARD), read by the
// evaluate parameter reader into +0xa8 through the SAME vtable slots as Color's (§8.1), so it is
// written in the same types. Optional for the same reason the mask is: a frame without a HUD-less
// copy must emit no rect, because "Invalid Backbuffer/active Output rect configuration" turns the
// whole evaluate into a silent no-op (§8.3).
constexpr int kMaxBackbufferRectEntries = 4;
int build_backbuffer_rect(std::uint32_t width, std::uint32_t height, Entry *out, int cap);

} // namespace stray_dlss::nrparam
