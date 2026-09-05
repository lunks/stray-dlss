// The DLSS-NR HDR colour codec, as scalar reference functions.
//
// WHY THIS EXISTS AT ALL. NGX feature 18 (DLSSNR) is a DISPLAY-REFERRED image network: it
// expects a [0,1], sRGB-encoded signal. We were handing it Stray's raw, unbounded,
// pre-exposed linear HDR scene colour, and the measured result was red noise with a neural
// output whose max luminance read 0.0026 — near black. There is no HDR, colour-space or
// exposure parameter anywhere in the runtime (established by exhaustive string search over
// nvngx_dlssnr.dll), so the conversion has to happen in OUR pixels, on both sides of the
// evaluate.
//
// PROVENANCE. Ported from the only known working, 4090-tested deployment of this codec:
// github.com/lunks/dxvk-remix-plus-dlssnr, commit `aa90a180` ("DLSS-NR: HDR colour codec,
// alpha preservation, run with mismatched guides"), files
//   src/dxvk/shaders/rtx/pass/neural_rendering/neural_rendering_codec.slangh
//   src/dxvk/shaders/rtx/pass/neural_rendering/neural_rendering_encode.comp.slang
//   src/dxvk/shaders/rtx/pass/neural_rendering/neural_rendering_decode.comp.slang
// and the C++ scale computation from the consolidated port at `fc4de144`
// (rtx_neural_rendering.cpp, `calcProxyScale`). Every constant below is that tree's, not
// ours. HARD.
//
// THE DISCIPLINE (CLAUDE.md §6). shaders/nr_codec.hlsli executes this math on the GPU and
// CI cannot run it, so every function here has a line-for-line twin over there and
// tests/test_nr_codec.cpp is the only executable proof either is right. Change one, change
// all three.
//
// Pure logic: no Windows, no D3D, no ReShade.
#pragma once

namespace stray_dlss::nrc {

struct Float3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

// --- constants, each from the reference tree ---

// Soft-clip shoulder, verbatim from the RenoDX DLSS5 encoder via neural_rendering_codec.slangh.
// Continuous in value at the knee and asymptotic to exactly 1.0, so no amount of input
// radiance can push the proxy out of the [0,1] domain sRGB encoding expects.
//
// 0.25 * 5.770780 = 1.442695 = 1/ln(2), so the slope jumps from 1.0 to ~1.44 at the knee:
// this curve is C0 but NOT C1. That is what the known working deployment ships, so it is
// reproduced exactly rather than "corrected". Do not smooth it.
constexpr float kSoftClipKnee = 0.75f;
constexpr float kSoftClipShoulder = 5.770780f;

// Below this DISPLAY-REFERRED luminance the original's chromaticity stops carrying usable
// information and the chromaticity-preserving path is faded out (see `decode`). A thousandth
// of diffuse white encodes to an sRGB code value of about 3/255 — well inside what any viewer
// reads as black.
constexpr float kMinChromaLuminance = 0.001f;

// Our colour target is R16G16B16A16_FLOAT. Writing a value the format cannot hold turns the
// pixel into an infinity, which then poisons everything downstream of it.
constexpr float kMaxHalf = 65504.0f;

// `calcProxyScale` (rtx_neural_rendering.cpp @ fc4de144) divides by
// `std::max(paperWhiteScale(), 0.01f)`; the reference's own RTX_OPTION additionally bounds the
// knob to [0.01, 64] (`args.minValue = 0.01f, args.maxValue = 64.0f`). Both reproduced — the
// ceiling matters because `proxy_scale`'s own [1e-6, 1e6] clamp is a backstop, and a paper white
// of 1e30 would otherwise reach it and make the decode amplify every delta a millionfold.
constexpr float kMinPaperWhite = 0.01f;
constexpr float kMaxPaperWhite = 64.0f;

// The final clamp `neuralRenderingProxyScale` applies. Load-bearing: the decode divides by
// this scale, so a zero would multiply the whole frame by infinity.
constexpr float kScaleMin = 1e-6f;
constexpr float kScaleMax = 1e6f;

// --- the transfer functions ---

// EXACT piecewise sRGB, both directions (IEC 61966-2-1). Deliberately NOT an x^2.2
// approximation: the network was trained on true sRGB imagery, and RenoDX — the deployment
// this is ported from — feeds it the exact piecewise curve.
float srgb_encode_channel(float c);
Float3 srgb_encode(Float3 c);
float srgb_decode_channel(float c);
Float3 srgb_decode(Float3 c);

// Identity at or below the knee, `k + (1-k)*(1 - exp(-shoulder*(v-k)))` above it.
//
// MEASURED, and it matters when choosing the paper white: the limit is 1.0 and float32 REACHES
// it at an input of about 3.474 (0.999999404 at 3.0, exactly 1.0 from 3.474 up). So anything
// brighter than ~3.5x display white encodes to pure white and the network sees a flat clipped
// region there. The invariant the design needs — the proxy never leaves [0,1] — holds either
// way, but "strictly monotone" does not, in float.
float soft_clip_channel(float v);
Float3 soft_clip(Float3 v);

// Standard Rec. 709 luminance weights, as `calcBt709Luminance` in the reference's
// rtx/utility/color.slangh.
float bt709_luminance(Float3 c);

// The scene-linear -> display-referred multiplier, from `calcProxyScale`:
//
//     const float staticExposure = 1.0f;
//     return staticExposure / std::max(paperWhiteScale(), 0.01f);
//
// so the shader's `scale` is `1 / paperWhite` and the USER-FACING knob is the paper-white
// value — the same thing RenoDX calls NRPaperWhiteScale.
//
// The reference multiplies this by its tonemapper's auto-exposure texture; we have no such
// texture at our hook point — Stray's TAA dispatch is nowhere near a tonemapper we can read —
// so we are unambiguously the FIXED-SCALE case, with nothing underneath the trim.
//
// OUR DEFAULT IS STILL 1.0, for two independent reasons, neither of them "the reference said
// so":
//
//   * The 16.0 in the reference's own option help is a remark about what "a RenoDX style fixed
//     scale WOULD use", not a measured RenoDX default. RenoDX's shipped value is 1.605, and
//     the operator's hands-on experience with RenoDX's HDR path is that paper white above 1.0
//     produces no change there. So 16 is not a documented starting point; it is a rhetorical
//     one.
//   * It is the wrong DIRECTION for this title. scale = 1/paperWhite, so paperWhite > 1
//     multiplies the colour DOWN — and Stray's scene colour at our hook point ALREADY carries
//     UE4's pre-exposure (CLAUDE.md §2.6 row 135.y, measured ~0.056 live), i.e. it has already
//     been scaled toward small values. The symptom we are fixing is a near-black neural output
//     (max luminance 0.0026 over the centre crop); dividing by a further 16 moves further INTO
//     that failure. If the measurement says anything it will most likely call for paper white
//     BELOW 1.0 (scale above 1.0), to lift the pre-exposed signal toward the 0.75 knee.
//
// Values below 1.0 are therefore explicitly legal; only the reference's own [0.01, 64] bounds
// apply.
// The input/proxy/output luminance line in src/ngx_nr.cpp is what actually chooses the value;
// see it before changing this default again.
//
// A non-finite paper white falls back to the caller's default rather than poisoning the
// frame, and the result is clamped to [kScaleMin, kScaleMax] exactly as
// `neuralRenderingProxyScale` does.
float proxy_scale(float paper_white, float fallback_paper_white);

// The same scale, with the engine's exposure folded in — `trackAutoExposure` from the reference,
// which we DROPPED in the port and which turns out to matter a great deal here.
//
// WHY. The reference (`rtx_neural_rendering.h:137-140`) defaults `trackAutoExposure` to TRUE and
// multiplies the proxy scale by its tonemapper's live exposure, so the soft-clip knee follows
// scene brightness. We hardcoded a static constant instead. Stray's scene colour at the TAA hook
// carries UE4's pre-exposure — measured live at 0.056 (CLAUDE.md §2.6, View row 135.y) — so the
// signal has to be lifted by roughly 1/0.056 ~= 18 to land anywhere near the 0.75 knee. The user
// hand-dialled `NgxNRPaperWhiteScale` to about 0.1, an effective scale near 10x: the same order of
// magnitude, arrived at by eye. That is a person compensating manually for a term the code should
// be supplying.
//
// And it is a real bug rather than a missing nicety, because PRE-EXPOSURE MOVES WITH THE SCENE. A
// paper white tuned in Stray's dark starting apartment is wrong in a brighter area, and there is
// no single constant that is right in both.
//
// THE ASYMMETRY WORTH NOT RE-LITIGATING: the SR path's exposure goes through NGX
// (`InPreExposure`, the exposure texture, the AutoExposure flag) and is therefore at the
// runtime's mercy — the texture mode was measured INERT for us, and the NR codec is reported to
// ignore `DLSS.Pre.Exposure` outright. This scale is OUR OWN shader arithmetic, a multiply inside
// a dispatch we record. The runtime cannot ignore it. That is why this is expected to work where
// the SR exposure attempt did not.
//
// `exposure_factor` is View row 135.z, `OneOverPreExposure`. A non-finite or non-positive value
// means the View CB was not readable this frame, and a frame with no usable View data must NOT
// produce a wild multiplier — it falls back to the static scale. The product is clamped to
// [kScaleMin, kScaleMax] exactly as the static scale is.
//
// POST-TONEMAP SITES MUST NOT USE THIS. Their image is already tonemapped and display-referred;
// there is no pre-exposure left to undo, so tracking there would be actively wrong.
float proxy_scale_tracked(float paper_white, float fallback_paper_white, float exposure_factor);

// Exponentially smooth the tracked exposure factor. THIS IS NOT A NICETY.
//
// The scale appears on BOTH sides of the codec — encode multiplies by it, decode divides by it
// — and within a frame the two share one value, so a frame is self-consistent. But DLSSNR keeps
// its OWN temporal history, accumulated in display-referred units at whatever scale was in force
// when each sample was encoded. If the scale moves frame to frame, the history no longer matches
// the current proxy's units and the network reprojects against inconsistently-scaled samples.
// Measured: the user reports the flicker tracks PAPER WHITE, with intensity merely amplifying it
// — exactly this, since intensity scales the residual that the mismatch corrupts.
//
// The reference gets away with an unsmoothed factor for a reason it states explicitly: its
// exposure "is heavily temporally smoothed (autoExposureSpeed defaults to 5 units/second), so a
// one frame lag is not visible". UE4's PreExposure carries no such guarantee and can step. So we
// smooth it ourselves rather than assuming the engine did.
//
// `previous` is the last smoothed value (0 or non-finite = no history yet, adopt `current`).
// `rate` in (0, 1] is the per-frame weight of the new sample; 1.0 disables smoothing entirely.
// Smoothed in LOG space because exposure is multiplicative — a linear average of 1/16 and 16
// is 8, which is nobody's idea of the midpoint.
float smooth_exposure_factor(float previous, float current, float rate);

// Has the codec scale moved far enough from the one NR's history was accumulated at that the
// history is no longer usable? Same reasoning as the reference's guide-extent latch: feature 18
// keeps its OWN temporal accumulation, in display-referred units, and the scale defines those
// units. A scale change silently invalidates the history and nothing else in the pipeline
// notices — so detect it and force one DLSSNR.Reset frame.
//
// Smoothing alone cannot solve this: it makes the mismatch gradual rather than abrupt, which is
// why a static camera recovers quickly while movement — where the engine's exposure genuinely
// swings — keeps re-triggering it.
//
// Compared as a RATIO, not a difference, because the scale is multiplicative: 1.0 -> 1.1 and
// 10.0 -> 11.0 are the same relative change and should behave identically. `latched` of 0 or
// non-finite means "no history yet", which is not a change.
bool codec_scale_invalidates_history(float latched, float current, float tolerance);

// --- the two halves of the codec ---

// `proxy = SrgbEncode(SoftClip(max(rgb, 0) * scale))`.
//
// A NaN/Inf source is flushed to black here: it must not reach the network, and it must not
// reach the decode's subtraction either. `decode` leaves the corresponding original pixel
// completely alone, so the two agree about what a broken pixel means.
Float3 encode(Float3 source, float scale);

// True when every channel is finite. The encode flushes a false to black; the decode returns
// early on one.
bool is_finite3(Float3 c);

// Carries the network's answer back onto the untouched linear HDR original.
//
// -------------------------------------------------------------------------------------------
// DERIVATION (this is the correctness-critical part; preserved verbatim in intent from
// neural_rendering_decode.comp.slang, which is where it was worked out)
// -------------------------------------------------------------------------------------------
//
// Let s be the scene-linear -> display-referred scale, and per channel let
//
//   o  = the original colour                 scene linear, unbounded, >= 0
//   d  = o * s                               display referred original
//   p  = SrgbDecode(proxy)   == SoftClip(d)  display referred, in [0, 1)
//   n  = SrgbDecode(neural)                  display referred, in [0, 1), the network's answer
//
// The encode threw away exactly (d - p): the part of the original the soft clip could not
// represent. Above the knee that is the entire HDR headroom and it can be enormous (d = 100
// while p is pinned just under 1.0). Below the knee SoftClip is the identity, so p == d and
// the discarded residual is exactly zero.
//
// The network's answer is a DISPLAY-REFERRED image, so the meaningful thing it produced is the
// CHANGE it made, (n - p), in display-referred units. Put the discarded residual back and the
// whole transfer is one line:
//
//   result = o + (n - p) / s                                                          (*)
//
// Properties, all algebraic:
//
//   1. EXACT IDENTITY. If the network changed nothing, n == p, so (n - p) is exactly +0.0 and
//      (*) evaluates to o + 0.0 == o, bit for bit, for every pixel and every value of s. A
//      no-op network must produce a no-op frame. The residual is never materialised as a
//      separate quantity, so there is no "d - p + n, then divide" round trip to lose it in.
//      This is the property tests/test_nr_codec.cpp exists to pin.
//
//   2. HDR SURVIVES. o is never scaled, clipped or reconstructed — it is added to. A pixel at
//      radiance 100 stays at radiance ~100, so everything downstream still sees the dynamic
//      range it needs. A naive SrgbDecode(neural) / s would have clamped the frame to the soft
//      clip's SDR range and destroyed all of it.
//
//   3. NO SINGULARITIES. (*) has one division, by the scale, which `proxy_scale` clamps away
//      from zero. There is nothing of the form neural/proxy, so a proxy channel sitting at
//      zero (a saturated primary, a black pixel) cannot produce an infinity or an amplified
//      quantisation step. The chromaticity valve below carries a second division, by the
//      ORIGINAL's luminance, and is bounded separately.
//
//   4. BELOW THE KNEE IT IS THE NETWORK'S ANSWER VERBATIM. There p == d == o*s, so (*) reduces
//      to o + (n - o*s)/s == n/s: the neural result decoded straight back into scene linear.
//      In the bulk of a correctly exposed frame this really is DLSS-NR, not a diluted version.
//
//   5. IN CLIPPED HIGHLIGHTS THE CHANGE IS ADDITIVE, NOT MULTIPLICATIVE, deliberately.
//      o = 100, p = 0.999, and the network answering n = 0.7 gives 99.7, not 70. A
//      multiplicative carry-back would have amplified a display-referred change by 100x and
//      blown the highlight out.
//
// `color_strength` 0 rebuilds the result from the ORIGINAL's chromaticity carrying only the
// network's luminance change, 1 takes the network's colour too — the escape hatch for the
// "everything's red/blue" class of symptom. `transfer_strength` is a global lerp back toward
// the untouched original; 0 is an exact bypass. Both preserve property 1: lerp(x, x, t) == x
// exactly, and at n == p the luminance ratio is exactly 1.0.
// -------------------------------------------------------------------------------------------
//
// Returns false when the pixel must be left EXACTLY as it was — a non-finite source (the
// encode fed the network black for it, so its answer means nothing) or a non-finite delta
// (which can only come from a resource the snippet left in an unexpected state, and must never
// reach the frame). `out` is untouched in that case.
bool decode(Float3 source, Float3 proxy_texel, Float3 neural_texel, float scale,
            float color_strength, float transfer_strength, Float3 &out);

} // namespace stray_dlss::nrc
