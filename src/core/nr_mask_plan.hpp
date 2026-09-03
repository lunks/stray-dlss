// DLSSNR.ControlMask — the per-pixel control texture, and what binding one costs.
//
// Pure; tested. src/nr_mask.cpp does the D3D12, src/ngx_nr.cpp writes the parameters, and neither
// makes a decision this file could have made (CLAUDE.md §0.4).
//
// ============================================================================================
// WHAT THE RUNTIME ACTUALLY DOES WITH THIS PARAMETER — all HARD, read out of the shipped
// nvngx_dlssnr.dll (310.8.0, md5 eea91faf55a8993656c66815f0497b3b) on 2026-09-03.
// ============================================================================================
//
// `DLSSNR.ControlMask` and its four Subrect fields are read by the evaluate-time parameter
// reader at 0x180019f30 into the 0x140-byte input struct, at +0x60 (the ID3D12Resource*) and
// +0x68/+0x6c/+0x70/+0x74 (BaseX/BaseY/Width/Height). The resource is fetched through the SAME
// parameter-block vtable slot (+0x40) as DLSSNR.Color / .Depth / .MVec / .Output, and the four
// subrect ints through the SAME slot (+0x58) as their subrects. That is the whole reason this can
// be built with any confidence: we do not have to decode the vtable to know the TYPES, because
// the runtime reads this exactly the way it reads four parameters we already set successfully.
// Write it the way nr_params.cpp already writes those, and the type question does not arise.
// (This is the `DLSSNR.Scale` lesson applied in reverse — that name was never read AT ALL.)
//
// It is then genuinely CONSUMED, not merely stored. At 0x18001cb7b the runtime loads the pointer,
// registers the resource through the cubin device's vtable slot +0xa8 (the same call it makes for
// Color and MVec, with the same read-only usage word), and at 0x18001cbc0 hands the descriptor to
// the guide-rect builder at 0x18001c520, which writes a 24-byte float record
// `{baseX, baseY, activeW, activeH, 1.0f/texW, 1.0f/texH}` into the launch array at rbp+0x198 —
// the same array that carries Color (rbp+0x150), UI (rbp+0x1b0) and UIAlpha (rbp+0x1c8).
// A zero/absent subrect Width or Height is filled in from the resource's own extent by that
// builder (0x18001c596), so a correct rect and an omitted rect agree; we send it anyway.
//
// TWO THINGS IT COSTS, and the second is the one to argue about:
//
//  1. Binding a mask FORCES DLSSNR.UseAutoMask to 0, unconditionally, whatever the caller set:
//     `cmp qword ptr [rdi+0x60], 0 / je / mov dword ptr [rdi+0xf0], 0` at 0x18001aa4b.
//
//  2. With UseAutoMask == 0 the runtime resolves BOTH structure strengths to the constant at
//     0x1800afc40, which is -1.0f, and neither raw value reaches the resolved pair
//     (0x18001aa59 -> 0x18001aa84). See `resolve_structure` below, which is that code
//     transcribed, and tests/test_nr_mask_plan.cpp, which pins it.
//
// SO: "does the skin channel get DISABLED, or does it INHERIT local structure?" — the question
// this module exists to answer. CLAUDE.md already records both halves correctly; what it does not
// do is put them next to each other, and read separately they invite the wrong conclusion. The
// sign test IS an inherit (`comiss`/`jae`, so any negative and any NaN takes the local value), but
// it lives on the AUTO path, at 0x18001aa62, and is reached only when UseAutoMask is non-zero. Binding a ControlMask takes the OTHER branch, where both resolved values become the
// -1.0f sentinel and no inherit happens at all. Binding a mask therefore does not slave skin to
// local structure; it retires the resolved pair entirely and leaves the per-pixel texture as the
// only spatial control. (The RAW LocalStructureStrength is a separate matter: the kernel is
// documented in CLAUDE.md as feeding its flag channel from the raw value when the resolved pair is
// negative, so the global knob is not lost — only the resolved pair is.)
//
// WHAT WE DELIBERATELY DO NOT BIND, and it is worth recording as a closed door rather than an
// unexplored one: `DLSSNR.BidirectionalDistortionField` is read into the same struct at +0xc0 and
// is then TOUCHED EXACTLY ONCE more in the entire module — `cmp qword ptr [rbp+0xe0], 0` at
// 0x180019cd8, which ORs bit 2 into a presence bitmask in the telemetry record the runtime hands
// to its stats callback. Its pointer is never registered, its subrect fields are never read, and
// no guide-rect record is ever built for it. It is a declared, parsed, entirely INERT parameter in
// this build. Supplying forward motion vectors to it would cost a texture and change nothing.
#pragma once

#include <cstdint>

namespace stray_dlss::nrmaskplan {

// The sentinel the runtime resolves both structure strengths to when the auto mask is off.
// 0x1800afc40, read as a float: exactly -1.0f. [HARD]
constexpr float kStructureSentinel = -1.0f;

// The value that means "leave this pixel alone" in every channel. The mask is a set of
// MULTIPLIERS and a blend weight, so 1.0 is the identity and a mask filled with 1.0 must be
// visually indistinguishable from no mask at all. That is the first experiment this module
// exists to make possible, and the reason it is a named constant rather than a literal.
constexpr float kNeutral = 1.0f;

// What the runtime resolves the structure pair to, transcribed from 0x18001aa4b-0x18001aaa1.
// `mask_bound` is "DLSSNR.ControlMask is a non-null resource".
struct ResolvedStructure
{
	float skin = kStructureSentinel;
	float local = kStructureSentinel;
	// The value the runtime will have written to +0xf0, which is NOT necessarily what the caller
	// asked for.
	unsigned int effective_auto_mask = 0;
};

ResolvedStructure resolve_structure(bool mask_bound, unsigned int use_auto_mask, float skin_raw,
                                    float local_raw);

// --- the bind decision -------------------------------------------------------------------

// Why a frame did or did not get a ControlMask. Counted and named for the same reason every other
// refusal in this codebase is: a mask that is silently ignored and a mask that does nothing look
// identical from a screenshot, and this project has been caught by exactly that class of
// ambiguity more than once.
enum class MaskResult
{
	ok = 0,
	// [STRAYDLSS] NgxNRMask is 0. The default.
	disabled,
	// A zero-sized colour rect: a minimised or mid-resize swapchain.
	zero_extent,
	// The device cannot write the chosen mask format through a typed UAV, so our compute fill
	// would produce nothing and we would hand the network an uninitialised texture. Refuse rather
	// than bind: an uninitialised control texture is the "silently wrong image" failure this
	// project exists to avoid.
	no_typed_uav_store,
	// Allocation or descriptor creation failed.
	alloc_failed,
};

constexpr int kMaskResultCount = 5;
const char *mask_result_name(MaskResult result);

// The three channels, from CLAUDE.md's own disassembly notes: R is the per-pixel FINAL BLEND
// WEIGHT, G scales local tone, B scales local structure. There is no skin channel — skin is not
// expressible per-pixel at all.
//
// Each channel has its own switch so R, G and B can be tested APART. A channel that is off writes
// kNeutral, so turning two off is not the same as not binding a mask: the plumbing, the format and
// the registration are all still exercised, and only the value changes. That distinction is the
// whole point of having per-channel switches rather than one.
struct Config
{
	bool enabled = false;
	bool channel_r = false; // final blend weight
	bool channel_g = false; // local tone scale
	bool channel_b = false; // local structure scale
	float value_r = kNeutral;
	float value_g = kNeutral;
	float value_b = kNeutral;
};

// What CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT) said about the mask format. Same shape
// as nrplan::ColourDesc's three bits, and separate for the same reason: the log must be able to
// name WHICH half is missing.
struct FormatSupport
{
	bool queried = false;
	bool view = false;
	bool store = false;
};

struct Plan
{
	MaskResult result = MaskResult::disabled;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	// What the fill pass writes. A disabled channel is kNeutral, never the configured value.
	float value_r = kNeutral;
	float value_g = kNeutral;
	float value_b = kNeutral;
	// Alpha is not a documented channel. It is written as kNeutral so the texture is fully
	// initialised whatever the kernel does with the fourth component — an uninitialised component
	// is exactly the kind of thing that reads as a nondeterministic image.
	float value_a = kNeutral;
	// True when every enabled channel is at kNeutral anyway, i.e. this mask is the identity. The
	// FIRST experiment, and the log has to be able to say so: "bound and doing nothing on purpose"
	// must be distinguishable from "bound and being ignored".
	bool is_identity = false;
};

Plan plan_mask(const Config &cfg, std::uint32_t colour_width, std::uint32_t colour_height,
               const FormatSupport &support);

// The mask's subrect. It is the COLOUR rect, not the guide rect: the mask is per-pixel over the
// image the network is improving, and at the present stage the colour rect is the back buffer's.
// Base is (0,0) for the same reason every other rect's is — we never hand the runtime a
// sub-rectangle of a larger allocation.
//
// Returned as its own type rather than reusing nrparam::Rects because the mask is optional and
// the four other rects are not: an absent mask must emit NO subrect entries at all, and a struct
// that cannot represent "absent" would make that a caller's discipline instead of a compiler's.
struct Subrect
{
	bool present = false;
	std::uint32_t base_x = 0;
	std::uint32_t base_y = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
};

Subrect subrect_for(const Plan &plan);

} // namespace stray_dlss::nrmaskplan
