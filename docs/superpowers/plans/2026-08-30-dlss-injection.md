# DLSS Injection Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Stray's `FTAAStandaloneCS` compute dispatch with an NVIDIA DLSS evaluation, from outside the engine, as a ReShade D3D12 add-on running under Proton/vkd3d-proton.

**Architecture:** A ReShade add-on identifies the TAA compute dispatch by DXBC hash *and* structural binding signature, captures its bound resources and the UE4 View constant buffer, runs its own compute pass to turn Stray's sparse `R16G16B16A16_UNORM` velocity into the dense `RG16_FLOAT` field DLSS requires, evaluates NGX directly into the engine's TAA output UAV, restores the D3D12 state NGX clobbers, and suppresses the engine's own dispatch. Work is phased so each risky assumption is validated by a real run on the user's machine before anything is built on it.

**Tech Stack:** C++20, MSVC (CI only), CMake, ReShade add-on API v6.8.0, NVIDIA NGX D3D12 (DLSS SDK v310.7.0), Dear ImGui v1.92.5-docking, doctest, HLSL compiled to DXBC by `fxc` at build time.

**Spec:** `docs/superpowers/specs/2026-08-30-dlss-injection-design.md`

## Global Constraints

- **ReShade 6.8.0 minimum.** `RESHADE_API_VERSION` is 20. Earlier versions crash with D3D12 DLSS under vkd3d-proton.
- **imgui must be v1.92.5-docking.** `IMGUI_VERSION_NUM` 19250. The non-docking tag reports the same number and fails to compile.
- **NGX SDK pinned to v310.7.0.** `nvsdk_ngx_d.lib`, `/MD`, Release. Never `/MT` without switching to `nvsdk_ngx_s.lib`.
- **Never call `D3DCompile` at runtime.** All HLSL is compiled to DXBC at build time by `fxc`.
- **NGX is not thread-safe.** Every NGX call happens on the thread owning the ReShade device/command-list callbacks.
- **Initialise NGX with `device::get_native()`** — the original vkd3d device, never a ReShade proxy.
- **Never gate DLSS availability on `NVSDK_NGX_D3D12_GetFeatureRequirements`** — unimplemented under Proton.
- **`get_native()` returns `uint64_t`.** Always `reinterpret_cast`.
- **In D3D12 `bind_pipeline` fires with `pipeline_stage::all`,** never `compute_shader`.
- **Failure policy:** any failure logs once with a reason and returns `false` from the dispatch callback so the engine's own TAA runs. No black screens, ever.
- **Warnings are errors** (`/W4 /WX`). Pure-logic code in `src/core/` must compile on Linux with `-Wall -Wextra -Werror`.
- **Never commit `nvngx_dlss.dll` or `nvsdk_ngx_d.lib`.** CI fetches them.
- Every constant taken from measurement gets a named symbol citing `CLAUDE.md §2.x`.

---

## Phase A — Observe

Rendering is untouched throughout Phase A. The deliverable is a log that answers the open
questions about Stray's TAA dispatch.

---

### Task 1: Decode the View constant buffer

**Files:**
- Create: `src/core/view_params.hpp`
- Create: `src/core/view_params.cpp`
- Test: `tests/test_view_params.cpp`
- Modify: `CMakeLists.txt` (add sources to `stray_dlss_core`, add test file to `stray_dlss_tests`)

**Interfaces:**
- Consumes: `stray_dlss::ue4::ViewRow`, `row_to_byte`, `kViewPrefixBytes`, `Float2` from `src/core/ue4_view.hpp`.
- Produces:
  ```cpp
  namespace stray_dlss::ue4 {
  struct Float4 { float x, y, z, w; };
  struct Matrix4 { float m[16]; };            // row-major, as UE4 packs it
  struct ViewParams {
      Matrix4 clip_to_prev_clip{};
      Float4  temporal_aa_jitter{};           // (CurX, CurY, PrevX, PrevY), clip units
      Float4  view_rect_min{};
      Float4  view_size_and_inv_size{};
      Float4  buffer_size_and_inv_size{};
      Float4  temporal_aa_params{};           // (Index, Count, JitterPixelsX, JitterPixelsY)
      float   pre_exposure = 1.0f;
      float   one_over_pre_exposure = 1.0f;
      float   near_plane = 0.0f;
      float   delta_time = 0.0f;
      float   camera_cut = 0.0f;
  };
  bool parse_view_params(const void *data, std::size_t size, ViewParams &out);
  bool view_params_plausible(const ViewParams &p);
  Float2 ngx_jitter_offset(const ViewParams &p);
  bool is_camera_cut(const ViewParams &p, bool history_or_velocity_is_1x1);
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/test_view_params.cpp`:

```cpp
#include <doctest/doctest.h>

#include "core/ue4_view.hpp"
#include "core/view_params.hpp"

#include <cstring>
#include <vector>

using namespace stray_dlss;

namespace {

// Builds a synthetic View constant buffer with known values at the measured offsets.
std::vector<unsigned char> make_view_buffer()
{
    std::vector<unsigned char> buf(ue4::kViewPrefixBytes, 0);

    auto put4 = [&](std::uint32_t row, float x, float y, float z, float w) {
        const float v[4] = { x, y, z, w };
        std::memcpy(buf.data() + ue4::row_to_byte(row), v, sizeof(v));
    };

    // ClipToPrevClip: rows 122..125, filled with 0..15 so row-major order is checkable.
    for (std::uint32_t r = 0; r < 4; ++r)
        put4(ue4::ViewRow::kClipToPrevClip + r,
             static_cast<float>(r * 4 + 0), static_cast<float>(r * 4 + 1),
             static_cast<float>(r * 4 + 2), static_cast<float>(r * 4 + 3));

    put4(ue4::ViewRow::kTemporalAAJitter, 0.001f, -0.002f, 0.003f, -0.004f);
    put4(ue4::ViewRow::kViewRectMin, 0.0f, 0.0f, 0.0f, 0.0f);
    put4(ue4::ViewRow::kViewSizeAndInvSize, 2560.0f, 1440.0f, 1.0f / 2560.0f, 1.0f / 1440.0f);
    put4(ue4::ViewRow::kBufferSizeAndInvSize, 2560.0f, 1440.0f, 1.0f / 2560.0f, 1.0f / 1440.0f);
    put4(ue4::ViewRow::kTemporalAAParams, 3.0f, 8.0f, 0.25f, -0.125f);
    put4(ue4::ViewRow::kPreExposureRow, 0.0f, 2.0f, 0.5f, 0.0f);   // .y PreExposure, .z 1/PreExposure
    put4(ue4::ViewRow::kNearPlaneRow, 10.0f, 0.0f, 0.0f, 0.0f);
    put4(ue4::ViewRow::kDeltaTimeRow, 0.016f, 0.0f, 0.0f, 0.0f);
    put4(ue4::ViewRow::kCameraCutRow, 0.0f, 0.0f, 0.0f, 0.0f);

    return buf;
}

} // namespace

TEST_CASE("parse_view_params reads every field from the measured offsets")
{
    const auto buf = make_view_buffer();
    ue4::ViewParams p;
    REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));

    CHECK(p.temporal_aa_params.z == doctest::Approx(0.25f));
    CHECK(p.temporal_aa_params.w == doctest::Approx(-0.125f));
    CHECK(p.view_size_and_inv_size.x == doctest::Approx(2560.0f));
    CHECK(p.view_size_and_inv_size.y == doctest::Approx(1440.0f));
    CHECK(p.buffer_size_and_inv_size.x == doctest::Approx(2560.0f));
    CHECK(p.pre_exposure == doctest::Approx(2.0f));
    CHECK(p.one_over_pre_exposure == doctest::Approx(0.5f));
    CHECK(p.near_plane == doctest::Approx(10.0f));
    CHECK(p.delta_time == doctest::Approx(0.016f));
    CHECK(p.camera_cut == doctest::Approx(0.0f));

    // Row-major: element [r][c] lives at m[r * 4 + c].
    for (int i = 0; i < 16; ++i)
        CHECK(p.clip_to_prev_clip.m[i] == doctest::Approx(static_cast<float>(i)));
}

TEST_CASE("parse_view_params rejects a short buffer")
{
    const auto buf = make_view_buffer();
    ue4::ViewParams p;
    CHECK_FALSE(ue4::parse_view_params(buf.data(), ue4::kViewPrefixBytes - 1, p));
    CHECK_FALSE(ue4::parse_view_params(nullptr, ue4::kViewPrefixBytes, p));
}

TEST_CASE("ngx_jitter_offset passes TemporalAAParams.zw through unchanged")
{
    // docs/RESEARCH.md section 3.3: NVIDIA's own UE plugin assigns View.TemporalJitterPixels
    // straight to InJitterOffsetX/Y. No negation, no scaling. This test exists to stop anyone
    // "fixing" it by reapplying the negative Y from the NDC derivation.
    const auto buf = make_view_buffer();
    ue4::ViewParams p;
    REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));

    const ue4::Float2 jitter = ue4::ngx_jitter_offset(p);
    CHECK(jitter.x == doctest::Approx(0.25f));
    CHECK(jitter.y == doctest::Approx(-0.125f));
}

TEST_CASE("view_params_plausible catches a buffer read at the wrong offset")
{
    const auto buf = make_view_buffer();
    ue4::ViewParams good;
    REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), good));
    CHECK(ue4::view_params_plausible(good));

    ue4::ViewParams bad = good;
    bad.view_size_and_inv_size = { 0.0f, 0.0f, 0.0f, 0.0f };
    CHECK_FALSE(ue4::view_params_plausible(bad));

    // The (1,1,1,1) decoy at row 131 would land here if offsets slipped by one row.
    ue4::ViewParams decoy = good;
    decoy.view_size_and_inv_size = { 1.0f, 1.0f, 1.0f, 1.0f };
    CHECK_FALSE(ue4::view_params_plausible(decoy));

    // Jitter outside [-0.5, 0.5] means we are not reading TemporalAAParams.
    ue4::ViewParams wild = good;
    wild.temporal_aa_params.z = 7.0f;
    CHECK_FALSE(ue4::view_params_plausible(wild));
}

TEST_CASE("is_camera_cut ORs all three signals")
{
    const auto buf = make_view_buffer();
    ue4::ViewParams p;
    REQUIRE(ue4::parse_view_params(buf.data(), buf.size(), p));

    // Baseline: jitter .zw differs from .xy, CameraCut is 0, nothing is 1x1.
    CHECK_FALSE(ue4::is_camera_cut(p, false));

    // Signal 1: View.CameraCut.
    ue4::ViewParams cut = p;
    cut.camera_cut = 1.0f;
    CHECK(ue4::is_camera_cut(cut, false));

    // Signal 2: previous jitter equals current.
    ue4::ViewParams same = p;
    same.temporal_aa_jitter = { 0.001f, -0.002f, 0.001f, -0.002f };
    CHECK(ue4::is_camera_cut(same, false));

    // Signal 3: a 1x1 history or velocity texture (BlackDummy).
    CHECK(ue4::is_camera_cut(p, true));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake -S . -B build-local -DSTRAY_DLSS_BUILD_ADDON=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-local --parallel
```

Expected: compile failure, `core/view_params.hpp` not found.

- [ ] **Step 3: Write the header**

Create `src/core/view_params.hpp`:

```cpp
// Decoding the UE 4.27 View uniform buffer. Pure logic: no Windows, no D3D, no ReShade, so
// this builds and is tested on Linux too.
//
// Offsets come from CLAUDE.md section 2.6, which reproduces them three independent ways.
#pragma once

#include "ue4_view.hpp"

#include <cstddef>

namespace stray_dlss::ue4 {

struct Float4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// UE4 compiles with D3D10_SHADER_PACK_MATRIX_ROW_MAJOR, so cb row r is M[r][0..3] and HLSL
// `mul(v, M)` is the row-vector convention. m[r * 4 + c] == M[r][c]. Transposing this into a
// column-vector library is the caller's job. (docs/RESEARCH.md section 4.7)
struct Matrix4
{
    float m[16] = {};
};

struct ViewParams
{
    Matrix4 clip_to_prev_clip{};
    Float4 temporal_aa_jitter{};   // (CurX, CurY, PrevX, PrevY) in clip/NDC units
    Float4 view_rect_min{};
    Float4 view_size_and_inv_size{};
    Float4 buffer_size_and_inv_size{};
    Float4 temporal_aa_params{};   // (Index, Count, JitterPixelsX, JitterPixelsY)
    float pre_exposure = 1.0f;
    float one_over_pre_exposure = 1.0f;
    float near_plane = 0.0f;
    float delta_time = 0.0f;
    float camera_cut = 0.0f;
};

// `size` must be at least kViewPrefixBytes. Returns false rather than reading past the end.
bool parse_view_params(const void *data, std::size_t size, ViewParams &out);

// A cheap sanity gate on a buffer we located by descriptor tracking rather than by name. It
// catches the two realistic failure modes: reading the wrong buffer entirely, and reading the
// right buffer at offsets that have slipped (row 131 is a (1,1,1,1) decoy).
bool view_params_plausible(const ViewParams &p);

// TemporalAAParams.zw IS TemporalJitterPixels, already in render-resolution pixels. NGX takes
// it with NO sign flip. (docs/RESEARCH.md section 3.3)
Float2 ngx_jitter_offset(const ViewParams &p);

// UE4's bCameraCut is `!InputHistory.IsValid() || View.bCameraCut`, and the history-invalid
// half never reaches the View buffer — hence the third argument. (CLAUDE.md section 2.8)
bool is_camera_cut(const ViewParams &p, bool history_or_velocity_is_1x1);

} // namespace stray_dlss::ue4
```

- [ ] **Step 4: Write the implementation**

Create `src/core/view_params.cpp`:

```cpp
#include "view_params.hpp"

#include <cmath>
#include <cstring>

namespace stray_dlss::ue4 {
namespace {

const unsigned char *row_ptr(const void *data, std::uint32_t row)
{
    return static_cast<const unsigned char *>(data) + row_to_byte(row);
}

Float4 read_float4(const void *data, std::uint32_t row)
{
    float v[4] = {};
    std::memcpy(v, row_ptr(data, row), sizeof(v));
    return Float4{ v[0], v[1], v[2], v[3] };
}

float read_scalar(const void *data, std::uint32_t row, std::uint32_t component)
{
    float v = 0.0f;
    std::memcpy(&v, row_ptr(data, row) + std::size_t{ component } * 4, sizeof(v));
    return v;
}

} // namespace

bool parse_view_params(const void *data, std::size_t size, ViewParams &out)
{
    if (data == nullptr || size < kViewPrefixBytes)
        return false;

    for (std::uint32_t r = 0; r < 4; ++r)
    {
        const Float4 row = read_float4(data, ViewRow::kClipToPrevClip + r);
        out.clip_to_prev_clip.m[r * 4 + 0] = row.x;
        out.clip_to_prev_clip.m[r * 4 + 1] = row.y;
        out.clip_to_prev_clip.m[r * 4 + 2] = row.z;
        out.clip_to_prev_clip.m[r * 4 + 3] = row.w;
    }

    out.temporal_aa_jitter = read_float4(data, ViewRow::kTemporalAAJitter);
    out.view_rect_min = read_float4(data, ViewRow::kViewRectMin);
    out.view_size_and_inv_size = read_float4(data, ViewRow::kViewSizeAndInvSize);
    out.buffer_size_and_inv_size = read_float4(data, ViewRow::kBufferSizeAndInvSize);
    out.temporal_aa_params = read_float4(data, ViewRow::kTemporalAAParams);

    out.pre_exposure = read_scalar(data, ViewRow::kPreExposureRow, kPreExposureComponent);
    out.one_over_pre_exposure = read_scalar(data, ViewRow::kPreExposureRow, kOneOverPreExposureComponent);
    out.near_plane = read_scalar(data, ViewRow::kNearPlaneRow, kNearPlaneComponent);
    out.delta_time = read_scalar(data, ViewRow::kDeltaTimeRow, kDeltaTimeComponent);
    out.camera_cut = read_scalar(data, ViewRow::kCameraCutRow, kCameraCutComponent);

    return true;
}

bool view_params_plausible(const ViewParams &p)
{
    const float w = p.view_size_and_inv_size.x;
    const float h = p.view_size_and_inv_size.y;

    // A real render target, not the (1,1,1,1) decoy at row 131 and not zeros.
    if (!(w >= 32.0f && w <= 16384.0f) || !(h >= 32.0f && h <= 16384.0f))
        return false;

    // .zw must be the reciprocals of .xy, which the decoy row does not satisfy.
    if (std::fabs(p.view_size_and_inv_size.z * w - 1.0f) > 0.01f)
        return false;
    if (std::fabs(p.view_size_and_inv_size.w * h - 1.0f) > 0.01f)
        return false;

    // Jitter is sub-pixel by construction.
    if (std::fabs(p.temporal_aa_params.z) > 0.5f || std::fabs(p.temporal_aa_params.w) > 0.5f)
        return false;

    // The jitter sequence length is small and positive.
    if (!(p.temporal_aa_params.y >= 1.0f && p.temporal_aa_params.y <= 512.0f))
        return false;

    return true;
}

Float2 ngx_jitter_offset(const ViewParams &p)
{
    return Float2{ p.temporal_aa_params.z, p.temporal_aa_params.w };
}

bool is_camera_cut(const ViewParams &p, bool history_or_velocity_is_1x1)
{
    if (p.camera_cut != 0.0f)
        return true;

    if (history_or_velocity_is_1x1)
        return true;

    return jitter_indicates_camera_cut(
        Float2{ p.temporal_aa_jitter.x, p.temporal_aa_jitter.y },
        Float2{ p.temporal_aa_jitter.z, p.temporal_aa_jitter.w });
}

} // namespace stray_dlss::ue4
```

- [ ] **Step 5: Wire the new files into CMake**

In `CMakeLists.txt`, add to the `stray_dlss_core` source list:

```cmake
    src/core/view_params.cpp
    src/core/view_params.hpp
```

and change the test executable to:

```cmake
    add_executable(stray_dlss_tests
        tests/test_core.cpp
        tests/test_view_params.cpp)
```

Then remove `#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN` from `tests/test_core.cpp` and add a
new `tests/test_main.cpp` containing exactly:

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
```

adding `tests/test_main.cpp` to the executable's sources. (Only one translation unit may
define the doctest main.)

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build-local --parallel && ctest --test-dir build-local --output-on-failure
```

Expected: PASS, all cases.

- [ ] **Step 7: Commit**

```bash
git add src/core/view_params.hpp src/core/view_params.cpp tests/test_view_params.cpp \
        tests/test_main.cpp tests/test_core.cpp CMakeLists.txt
git commit -m "Decode the UE4 View constant buffer into a checked struct"
```

---

### Task 2: Structural signature for the TAA dispatch

**Files:**
- Create: `src/core/taa_signature.hpp`
- Create: `src/core/taa_signature.cpp`
- Test: `tests/test_taa_signature.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces:
  ```cpp
  namespace stray_dlss {
  enum class TexFormat { unknown, r32_float_x8x24_typeless, x32_typeless_g8x24_uint,
                         r16g16b16a16_unorm, r16g16b16a16_float, r16g16_float, r32g32b32a32_float };
  struct BoundTexture { std::uint32_t slot = 0; std::uint64_t resource = 0;
                        TexFormat format = TexFormat::unknown;
                        std::uint32_t width = 0, height = 0; };
  struct DispatchSignature {
      std::vector<BoundTexture> srvs, uavs;
      bool has_view_cb = false;
      std::uint32_t group_count_x = 0, group_count_y = 0, group_count_z = 0;
      std::uint64_t shader_hash = 0;
  };
  enum class MatchVerdict { no_match, structural_only, hash_and_structural, excluded };
  struct MatchResult { MatchVerdict verdict = MatchVerdict::no_match; const char *reason = "";
                       std::uint32_t depth_srv = 0, stencil_srv = 0, velocity_srv = 0,
                                     colour_srv_a = 0, colour_srv_b = 0, output_uav = 0;
                       bool has_downsample_uav = false; };
  MatchResult match_taa_dispatch(const DispatchSignature &sig,
                                 std::uint32_t view_width, std::uint32_t view_height);
  constexpr std::uint64_t kTaaMainHash            = 0x1708ec956099e259ull;
  constexpr std::uint64_t kSecondCandidateHash    = 0x52101a15e1a0c5ccull;
  constexpr std::uint64_t kKnownFalsePositiveHash = 0x901e041a7cadc9dbull;
  constexpr std::uint32_t kTaaTileSize = 8;
  }
  ```

- [ ] **Step 1: Write the failing test**

Create `tests/test_taa_signature.cpp`:

```cpp
#include <doctest/doctest.h>

#include "core/taa_signature.hpp"

using namespace stray_dlss;

namespace {

// The measured Stray TAA binding layout at 2560x1440. CLAUDE.md section 2.3.
// t0 depth, t1 stencil (SAME resource as t0), t2 velocity, t3 eye adaptation (1x1),
// t5/t6 colour, u0 output, u1 downsampled.
DispatchSignature make_stray_taa(std::uint32_t w = 2560, std::uint32_t h = 1440)
{
    DispatchSignature s;
    s.shader_hash = kTaaMainHash;
    s.has_view_cb = true;
    s.group_count_x = (w + kTaaTileSize - 1) / kTaaTileSize;
    s.group_count_y = (h + kTaaTileSize - 1) / kTaaTileSize;
    s.group_count_z = 1;

    s.srvs = {
        { 0, 0x1000, TexFormat::r32_float_x8x24_typeless, w, h },
        { 1, 0x1000, TexFormat::x32_typeless_g8x24_uint,  w, h },  // same resource as t0
        { 2, 0x2000, TexFormat::r16g16b16a16_unorm,       w, h },
        { 3, 0x3000, TexFormat::r32g32b32a32_float,       1, 1 },  // eye adaptation
        { 5, 0x4000, TexFormat::r16g16b16a16_float,       w, h },
        { 6, 0x5000, TexFormat::r16g16b16a16_float,       w, h },
    };
    s.uavs = {
        { 0, 0x6000, TexFormat::r16g16b16a16_float, w, h },
        { 1, 0x7000, TexFormat::r16g16b16a16_float, w / 2, h / 2 },
    };
    return s;
}

} // namespace

TEST_CASE("the measured Stray TAA dispatch matches on hash and structure")
{
    const auto sig = make_stray_taa();
    const auto r = match_taa_dispatch(sig, 2560, 1440);

    CHECK(r.verdict == MatchVerdict::hash_and_structural);
    CHECK(r.depth_srv == 0);
    CHECK(r.stencil_srv == 1);
    CHECK(r.velocity_srv == 2);
    CHECK(r.output_uav == 0);
    CHECK(r.has_downsample_uav);
    // Both colour SRVs are reported; deciding which is history needs last frame's u0.
    CHECK(((r.colour_srv_a == 5 && r.colour_srv_b == 6) ||
           (r.colour_srv_a == 6 && r.colour_srv_b == 5)));
}

TEST_CASE("structure alone is enough when the hash moves")
{
    // Stray's TAA hash tracks r.PostProcessAAQuality, so a user changing the in-game AA
    // setting must not break the hook. CLAUDE.md section 2.3.
    auto sig = make_stray_taa();
    sig.shader_hash = 0xdeadbeefcafef00dull;

    const auto r = match_taa_dispatch(sig, 2560, 1440);
    CHECK(r.verdict == MatchVerdict::structural_only);
    CHECK(r.depth_srv == 0);
    CHECK(r.velocity_srv == 2);
}

TEST_CASE("the known false positive is excluded no matter how it scores")
{
    auto sig = make_stray_taa();
    sig.shader_hash = kKnownFalsePositiveHash;

    const auto r = match_taa_dispatch(sig, 2560, 1440);
    CHECK(r.verdict == MatchVerdict::excluded);
}

TEST_CASE("the second candidate is excluded — it is almost certainly motion blur")
{
    auto sig = make_stray_taa();
    sig.shader_hash = kSecondCandidateHash;

    const auto r = match_taa_dispatch(sig, 2560, 1440);
    CHECK(r.verdict == MatchVerdict::excluded);
}

TEST_CASE("depth and stencil are told apart by view format over a shared resource")
{
    auto sig = make_stray_taa();
    // Swap the two views' slots. The matcher must follow the format, not the register.
    sig.srvs[0].format = TexFormat::x32_typeless_g8x24_uint;
    sig.srvs[1].format = TexFormat::r32_float_x8x24_typeless;

    const auto r = match_taa_dispatch(sig, 2560, 1440);
    REQUIRE(r.verdict != MatchVerdict::no_match);
    CHECK(r.depth_srv == 1);
    CHECK(r.stencil_srv == 0);
}

TEST_CASE("a dispatch of the wrong size is rejected")
{
    auto sig = make_stray_taa();
    sig.group_count_x += 3;

    const auto r = match_taa_dispatch(sig, 2560, 1440);
    CHECK(r.verdict == MatchVerdict::no_match);
}

TEST_CASE("a dispatch missing the View constant buffer is rejected")
{
    auto sig = make_stray_taa();
    sig.has_view_cb = false;

    CHECK(match_taa_dispatch(sig, 2560, 1440).verdict == MatchVerdict::no_match);
}

TEST_CASE("a dispatch without a velocity SRV is rejected")
{
    auto sig = make_stray_taa();
    sig.srvs.erase(sig.srvs.begin() + 2);

    CHECK(match_taa_dispatch(sig, 2560, 1440).verdict == MatchVerdict::no_match);
}

TEST_CASE("a dispatch with only one colour SRV is rejected")
{
    auto sig = make_stray_taa();
    sig.srvs.pop_back();

    CHECK(match_taa_dispatch(sig, 2560, 1440).verdict == MatchVerdict::no_match);
}

TEST_CASE("the downsample UAV is optional")
{
    // r.TemporalAA.AllowDownsampling=0 or a different AA quality removes u1.
    auto sig = make_stray_taa();
    sig.uavs.pop_back();

    const auto r = match_taa_dispatch(sig, 2560, 1440);
    CHECK(r.verdict == MatchVerdict::hash_and_structural);
    CHECK_FALSE(r.has_downsample_uav);
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
cmake --build build-local --parallel
```

Expected: compile failure, `core/taa_signature.hpp` not found.

- [ ] **Step 3: Write the header**

Create `src/core/taa_signature.hpp`:

```cpp
// Deciding whether a compute dispatch is Stray's FTAAStandaloneCS.
//
// Identity is deliberately NOT hash-only. Stray's TAA hash tracks r.PostProcessAAQuality
// (currently 3, Medium), so a user changing the in-game AA setting would silently break a
// hash-only hook. The structural signature is what gates interception; the hash is a fast
// pre-filter, a confirmation, and an exclusion list. (CLAUDE.md section 2.3)
//
// Pure logic: no Windows, no D3D, no ReShade. The caller translates real D3D12 formats and
// resource pointers into these types.
#pragma once

#include <cstdint>
#include <vector>

namespace stray_dlss {

// Only the formats this decision needs to distinguish.
enum class TexFormat
{
    unknown,
    r32_float_x8x24_typeless, // the depth view over the R32G8X24_TYPELESS resource
    x32_typeless_g8x24_uint,  // the stencil view over the SAME resource
    r16g16b16a16_unorm,       // UE4 velocity
    r16g16b16a16_float,       // scene colour, history, TAA output
    r16g16_float,             // our resolved motion vectors
    r32g32b32a32_float,       // eye adaptation (1x1)
};

struct BoundTexture
{
    std::uint32_t slot = 0;      // t or u register
    std::uint64_t resource = 0;  // ID3D12Resource*, as an opaque identity
    TexFormat format = TexFormat::unknown;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct DispatchSignature
{
    std::vector<BoundTexture> srvs;
    std::vector<BoundTexture> uavs;
    bool has_view_cb = false;
    std::uint32_t group_count_x = 0;
    std::uint32_t group_count_y = 0;
    std::uint32_t group_count_z = 0;
    std::uint64_t shader_hash = 0;
};

enum class MatchVerdict
{
    no_match,
    structural_only,      // the bindings fit but the hash is unfamiliar
    hash_and_structural,  // both agree — the strongest signal
    excluded,             // a hash we have positively identified as NOT the TAA pass
};

struct MatchResult
{
    MatchVerdict verdict = MatchVerdict::no_match;
    const char *reason = "";

    std::uint32_t depth_srv = 0;
    std::uint32_t stencil_srv = 0;
    std::uint32_t velocity_srv = 0;
    std::uint32_t colour_srv_a = 0; // which is scene colour and which is history cannot be
    std::uint32_t colour_srv_b = 0; // decided here — it needs last frame's u0 pointer
    std::uint32_t output_uav = 0;
    bool has_downsample_uav = false;
};

// Measured shader identities. CLAUDE.md section 2.3.
constexpr std::uint64_t kTaaMainHash = 0x1708ec956099e259ull;
constexpr std::uint64_t kSecondCandidateHash = 0x52101a15e1a0c5ccull;
constexpr std::uint64_t kKnownFalsePositiveHash = 0x901e041a7cadc9dbull;

// GTemporalAATileSizeX/Y. The dispatch is ceil(viewrect / 8). (docs/RESEARCH.md section 4.1)
constexpr std::uint32_t kTaaTileSize = 8;

MatchResult match_taa_dispatch(const DispatchSignature &sig,
                               std::uint32_t view_width,
                               std::uint32_t view_height);

} // namespace stray_dlss
```

- [ ] **Step 4: Write the implementation**

Create `src/core/taa_signature.cpp`:

```cpp
#include "taa_signature.hpp"

#include <algorithm>

namespace stray_dlss {
namespace {

const BoundTexture *find_format(const std::vector<BoundTexture> &v, TexFormat f)
{
    const auto it = std::find_if(v.begin(), v.end(),
        [f](const BoundTexture &t) { return t.format == f; });
    return it == v.end() ? nullptr : &*it;
}

std::uint32_t group_count(std::uint32_t extent)
{
    return (extent + kTaaTileSize - 1) / kTaaTileSize;
}

} // namespace

MatchResult match_taa_dispatch(const DispatchSignature &sig,
                               std::uint32_t view_width,
                               std::uint32_t view_height)
{
    MatchResult r;

    // Exclusions win over everything. These are hashes we have positively identified as not
    // being the pass we want, and no amount of structural resemblance may override them.
    if (sig.shader_hash == kKnownFalsePositiveHash)
    {
        r.verdict = MatchVerdict::excluded;
        r.reason = "known false positive 0x901e041a7cadc9db";
        return r;
    }
    if (sig.shader_hash == kSecondCandidateHash)
    {
        r.verdict = MatchVerdict::excluded;
        r.reason = "second candidate 0x52101a15e1a0c5cc, almost certainly motion blur";
        return r;
    }

    if (!sig.has_view_cb)
    {
        r.reason = "no View constant buffer bound";
        return r;
    }

    if (sig.group_count_z != 1 ||
        sig.group_count_x != group_count(view_width) ||
        sig.group_count_y != group_count(view_height))
    {
        r.reason = "dispatch size is not ceil(viewrect / 8)";
        return r;
    }

    const BoundTexture *depth = find_format(sig.srvs, TexFormat::r32_float_x8x24_typeless);
    if (depth == nullptr)
    {
        r.reason = "no R32_FLOAT_X8X24_TYPELESS depth SRV";
        return r;
    }

    const BoundTexture *velocity = find_format(sig.srvs, TexFormat::r16g16b16a16_unorm);
    if (velocity == nullptr)
    {
        r.reason = "no R16G16B16A16_UNORM velocity SRV";
        return r;
    }

    // Exactly two full-size R16G16B16A16_FLOAT colour SRVs: scene colour and history.
    std::vector<const BoundTexture *> colour;
    for (const auto &t : sig.srvs)
        if (t.format == TexFormat::r16g16b16a16_float)
            colour.push_back(&t);

    if (colour.size() != 2)
    {
        r.reason = "expected exactly two R16G16B16A16_FLOAT colour SRVs";
        return r;
    }

    const BoundTexture *output = find_format(sig.uavs, TexFormat::r16g16b16a16_float);
    if (output == nullptr)
    {
        r.reason = "no R16G16B16A16_FLOAT output UAV";
        return r;
    }

    r.depth_srv = depth->slot;
    r.velocity_srv = velocity->slot;
    r.colour_srv_a = colour[0]->slot;
    r.colour_srv_b = colour[1]->slot;
    r.output_uav = output->slot;

    // The stencil view is the other SRV over the same resource as depth.
    r.stencil_srv = depth->slot;
    for (const auto &t : sig.srvs)
    {
        if (t.resource == depth->resource && t.format == TexFormat::x32_typeless_g8x24_uint)
        {
            r.stencil_srv = t.slot;
            break;
        }
    }

    // u1 (OutComputeTexDownsampled) exists only under TAA_DOWNSAMPLE. When it is present we
    // are obliged to write it, or downstream bloom and DOF read garbage.
    r.has_downsample_uav = sig.uavs.size() > 1;

    r.verdict = (sig.shader_hash == kTaaMainHash) ? MatchVerdict::hash_and_structural
                                                  : MatchVerdict::structural_only;
    r.reason = (r.verdict == MatchVerdict::hash_and_structural)
                   ? "hash and structure agree"
                   : "structure matches, hash unfamiliar (AA quality setting changed?)";
    return r;
}

} // namespace stray_dlss
```

- [ ] **Step 5: Wire into CMake**

Add `src/core/taa_signature.cpp` and `src/core/taa_signature.hpp` to `stray_dlss_core`, and
`tests/test_taa_signature.cpp` to `stray_dlss_tests`.

- [ ] **Step 6: Run the tests to verify they pass**

```bash
cmake --build build-local --parallel && ctest --test-dir build-local --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add src/core/taa_signature.hpp src/core/taa_signature.cpp tests/test_taa_signature.cpp CMakeLists.txt
git commit -m "Identify the TAA dispatch structurally, not by hash alone"
```

---

### Task 3: Capture the bindings of the real dispatch

This is the first task whose deliverable is a field test. It changes no rendering.

**Files:**
- Create: `third_party/reshade_utils/descriptor_tracking.hpp` (verbatim from ReShade v6.8.0 `examples/utils/`)
- Create: `third_party/reshade_utils/descriptor_tracking.cpp` (verbatim)
- Create: `third_party/reshade_utils/state_tracking.hpp` (verbatim)
- Create: `third_party/reshade_utils/state_tracking.cpp` (verbatim)
- Create: `src/frame_state.hpp`
- Create: `src/frame_state.cpp`
- Create: `src/taa_hook.hpp`
- Create: `src/taa_hook.cpp`
- Modify: `src/addon.cpp` (delegate the events to `taa_hook`)
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `match_taa_dispatch`, `DispatchSignature`, `TexFormat`, `BoundTexture`, `MatchResult` from Task 2; `parse_view_params`, `ViewParams` from Task 1.
- Produces:
  ```cpp
  namespace stray_dlss {
  TexFormat to_tex_format(reshade::api::format f);      // frame_state.hpp
  struct CommandListState {
      reshade::api::pipeline current_compute_pipeline{};
      std::vector<BoundTexture> srvs, uavs;
      reshade::api::buffer_range view_cb{};
      bool view_cb_valid = false;
      void reset();
  };
  CommandListState &state_for(reshade::api::command_list *cmd);   // frame_state.hpp
  void forget_command_list(reshade::api::command_list *cmd);

  namespace taa_hook {                                   // taa_hook.hpp
  void register_events();
  void unregister_events();
  bool intercept_dispatch(reshade::api::command_list *cmd, uint32_t x, uint32_t y, uint32_t z);
  struct Diagnostics {                                   // read by the overlay
      std::uint64_t frames = 0, taa_dispatches = 0;
      MatchVerdict last_verdict = MatchVerdict::no_match;
      const char *last_reason = "";
      ue4::ViewParams last_view{};
      bool view_plausible = false;
  };
  const Diagnostics &diagnostics();
  }}
  ```

- [ ] **Step 1: Vendor ReShade's tracking utilities verbatim**

```bash
cd /tmp && rm -rf reshade-utils && git clone --depth 1 --filter=blob:none --sparse \
    https://github.com/crosire/reshade.git reshade-utils
cd reshade-utils && git fetch --depth 1 origin tag v6.8.0 && git checkout v6.8.0 \
    && git sparse-checkout set examples/utils
cp examples/utils/descriptor_tracking.hpp examples/utils/descriptor_tracking.cpp \
   examples/utils/state_tracking.hpp examples/utils/state_tracking.cpp \
   "$OLDPWD/third_party/reshade_utils/"
```

Do not edit these files. They are ReShade's, they already handle the 6.8.0 eight-parameter
`bind_descriptor_tables` signature, and reimplementing them is how people get descriptor
tracking subtly wrong.

- [ ] **Step 2: Write `src/frame_state.hpp`**

```cpp
// Per-command-list capture of what the game bound, keyed by descriptor register.
//
// Resources are tracked BY REGISTER, never by pointer identity alone: the same texture
// appears at different registers meaning different things. (CLAUDE.md section 2.9)
#pragma once

#include "core/taa_signature.hpp"

#include <reshade.hpp>

#include <vector>

namespace stray_dlss {

TexFormat to_tex_format(reshade::api::format f);

struct CommandListState
{
    reshade::api::pipeline current_compute_pipeline{};
    std::vector<BoundTexture> srvs;
    std::vector<BoundTexture> uavs;
    reshade::api::buffer_range view_cb{};
    bool view_cb_valid = false;

    void reset();
};

CommandListState &state_for(reshade::api::command_list *cmd);
void forget_command_list(reshade::api::command_list *cmd);
void forget_all_command_lists();

} // namespace stray_dlss
```

- [ ] **Step 3: Write `src/frame_state.cpp`**

```cpp
#include "frame_state.hpp"

#include <mutex>
#include <unordered_map>

namespace stray_dlss {
namespace {

std::mutex g_mutex;
std::unordered_map<reshade::api::command_list *, CommandListState> g_states;

} // namespace

TexFormat to_tex_format(reshade::api::format f)
{
    using reshade::api::format;
    switch (f)
    {
    case format::r32_float_x8x24_typeless: return TexFormat::r32_float_x8x24_typeless;
    case format::x32_typeless_g8x24_uint:  return TexFormat::x32_typeless_g8x24_uint;
    case format::r16g16b16a16_unorm:       return TexFormat::r16g16b16a16_unorm;
    case format::r16g16b16a16_float:       return TexFormat::r16g16b16a16_float;
    case format::r16g16_float:             return TexFormat::r16g16_float;
    case format::r32g32b32a32_float:       return TexFormat::r32g32b32a32_float;
    default:                               return TexFormat::unknown;
    }
}

void CommandListState::reset()
{
    current_compute_pipeline = {};
    srvs.clear();
    uavs.clear();
    view_cb = {};
    view_cb_valid = false;
}

CommandListState &state_for(reshade::api::command_list *cmd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_states[cmd];
}

void forget_command_list(reshade::api::command_list *cmd)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_states.erase(cmd);
}

void forget_all_command_lists()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_states.clear();
}

} // namespace stray_dlss
```

- [ ] **Step 4: Write `src/taa_hook.hpp`**

```cpp
// Identification and interception of Stray's TAA dispatch.
//
// Phase A: observe and log only. intercept_dispatch always returns false, so the engine's own
// TAA still runs. Nothing here changes rendering yet.
#pragma once

#include "core/taa_signature.hpp"
#include "core/view_params.hpp"

#include <reshade.hpp>

#include <cstdint>

namespace stray_dlss::taa_hook {

void register_events();
void unregister_events();

// Returns true to suppress the game's dispatch. Phase A always returns false.
bool intercept_dispatch(reshade::api::command_list *cmd, uint32_t x, uint32_t y, uint32_t z);

struct Diagnostics
{
    std::uint64_t frames = 0;
    std::uint64_t taa_dispatches = 0;
    MatchVerdict last_verdict = MatchVerdict::no_match;
    const char *last_reason = "";
    ue4::ViewParams last_view{};
    bool view_plausible = false;
    bool logged_full_report = false;
};

const Diagnostics &diagnostics();
void on_present_frame();

} // namespace stray_dlss::taa_hook
```

- [ ] **Step 5: Write `src/taa_hook.cpp`**

The full-detail log is the entire point of this task. It fires **once**, on the first dispatch
whose verdict is not `no_match`, so the log stays readable.

```cpp
#include "taa_hook.hpp"

#include "core/fnv1a.hpp"
#include "frame_state.hpp"
#include "log.hpp"

#include <descriptor_tracking.hpp>

#include <mutex>
#include <unordered_map>

namespace stray_dlss::taa_hook {
namespace {

std::mutex g_mutex;
std::unordered_map<uint64_t, std::uint64_t> g_pipeline_hashes; // pipeline handle -> DXBC hash
Diagnostics g_diag;

// Last frame's u0 resource. The colour SRV matching it is the history; the other is the scene
// colour. Register order cannot decide this. (CLAUDE.md section 2.3)
std::uint64_t g_previous_output_resource = 0;

const char *verdict_name(MatchVerdict v)
{
    switch (v)
    {
    case MatchVerdict::no_match:            return "no_match";
    case MatchVerdict::structural_only:     return "structural_only";
    case MatchVerdict::hash_and_structural: return "hash_and_structural";
    case MatchVerdict::excluded:            return "excluded";
    }
    return "?";
}

const char *format_name(TexFormat f)
{
    switch (f)
    {
    case TexFormat::r32_float_x8x24_typeless: return "R32_FLOAT_X8X24_TYPELESS";
    case TexFormat::x32_typeless_g8x24_uint:  return "X32_TYPELESS_G8X24_UINT";
    case TexFormat::r16g16b16a16_unorm:       return "R16G16B16A16_UNORM";
    case TexFormat::r16g16b16a16_float:       return "R16G16B16A16_FLOAT";
    case TexFormat::r16g16_float:             return "R16G16_FLOAT";
    case TexFormat::r32g32b32a32_float:       return "R32G32B32A32_FLOAT";
    case TexFormat::unknown:                  return "other";
    }
    return "?";
}

void log_full_report(const DispatchSignature &sig, const MatchResult &match,
                     const ue4::ViewParams &view, bool view_ok)
{
    STRAY_LOG_INFO("================ TAA DISPATCH REPORT ================");
    STRAY_LOG_INFO("verdict=%s (%s)", verdict_name(match.verdict), match.reason);
    STRAY_LOG_INFO("shader hash = 0x%016llx", (unsigned long long)sig.shader_hash);
    STRAY_LOG_INFO("dispatch = %ux%ux%u", sig.group_count_x, sig.group_count_y, sig.group_count_z);

    for (const auto &t : sig.srvs)
        STRAY_LOG_INFO("  t%-2u res=0x%016llx %-26s %ux%u", t.slot,
            (unsigned long long)t.resource, format_name(t.format), t.width, t.height);
    for (const auto &t : sig.uavs)
        STRAY_LOG_INFO("  u%-2u res=0x%016llx %-26s %ux%u", t.slot,
            (unsigned long long)t.resource, format_name(t.format), t.width, t.height);

    STRAY_LOG_INFO("resolved: depth=t%u stencil=t%u velocity=t%u colour=t%u,t%u out=u%u ds=%d",
        match.depth_srv, match.stencil_srv, match.velocity_srv,
        match.colour_srv_a, match.colour_srv_b, match.output_uav,
        match.has_downsample_uav ? 1 : 0);

    STRAY_LOG_INFO("View CB plausible = %s", view_ok ? "yes" : "NO");
    STRAY_LOG_INFO("  ViewRectMin        = %.1f %.1f", view.view_rect_min.x, view.view_rect_min.y);
    STRAY_LOG_INFO("  ViewSizeAndInvSize = %.1f %.1f %.6f %.6f",
        view.view_size_and_inv_size.x, view.view_size_and_inv_size.y,
        view.view_size_and_inv_size.z, view.view_size_and_inv_size.w);
    STRAY_LOG_INFO("  BufferSize         = %.1f %.1f",
        view.buffer_size_and_inv_size.x, view.buffer_size_and_inv_size.y);
    STRAY_LOG_INFO("  TemporalAAJitter   = %.6f %.6f %.6f %.6f",
        view.temporal_aa_jitter.x, view.temporal_aa_jitter.y,
        view.temporal_aa_jitter.z, view.temporal_aa_jitter.w);
    STRAY_LOG_INFO("  TemporalAAParams   = %.1f %.1f %.6f %.6f",
        view.temporal_aa_params.x, view.temporal_aa_params.y,
        view.temporal_aa_params.z, view.temporal_aa_params.w);
    STRAY_LOG_INFO("  PreExposure        = %.6f (1/x = %.6f)",
        view.pre_exposure, view.one_over_pre_exposure);
    STRAY_LOG_INFO("  NearPlane          = %.4f  DeltaTime = %.6f  CameraCut = %.1f",
        view.near_plane, view.delta_time, view.camera_cut);

    for (int r = 0; r < 4; ++r)
        STRAY_LOG_INFO("  ClipToPrevClip[%d]  = %.6f %.6f %.6f %.6f", r,
            view.clip_to_prev_clip.m[r * 4 + 0], view.clip_to_prev_clip.m[r * 4 + 1],
            view.clip_to_prev_clip.m[r * 4 + 2], view.clip_to_prev_clip.m[r * 4 + 3]);

    STRAY_LOG_INFO("=====================================================");
}

} // namespace

const Diagnostics &diagnostics() { return g_diag; }
void on_present_frame() { ++g_diag.frames; }

// ... event callbacks: on_init_pipeline (hash and record), on_destroy_pipeline (evict),
// on_bind_pipeline (record current compute pipeline — note: fires with pipeline_stage::all in
// D3D12, so do NOT filter on the compute stage), on_push_descriptors and
// on_bind_descriptor_tables (record SRVs/UAVs by register and the View CB buffer_range),
// on_reset_command_list (clear state).
```

The implementer writes the remaining callbacks following the interfaces above. Three rules
that are not negotiable and are each a documented trap:

1. `bind_pipeline` in D3D12 reports `pipeline_stage::all`. Do not filter on `compute_shader`.
2. Descriptor tracking must be registered from the very first frame. There is no retroactive
   recovery of a heap populated before we attached.
3. The View CB is read inside `intercept_dispatch` via
   `device->map_buffer_region(cb.buffer, cb.offset, ue4::kViewPrefixBytes, map_access::read_only, &p)`,
   copied out in one `memcpy`, and unmapped immediately. Never deferred to `present`.

- [ ] **Step 6: Write `intercept_dispatch`**

```cpp
bool intercept_dispatch(reshade::api::command_list *cmd, uint32_t x, uint32_t y, uint32_t z)
{
    CommandListState &st = state_for(cmd);
    if (!st.view_cb_valid)
        return false;

    reshade::api::device *device = cmd->get_device();

    // Read the View CB now, at recording time, while the upload ring still holds this
    // frame's allocation. (docs/RESEARCH.md section 2.6)
    ue4::ViewParams view;
    bool view_ok = false;
    void *mapped = nullptr;
    if (device->map_buffer_region(st.view_cb.buffer, st.view_cb.offset, ue4::kViewPrefixBytes,
                                  reshade::api::map_access::read_only, &mapped) && mapped != nullptr)
    {
        alignas(16) unsigned char copy[ue4::kViewPrefixBytes];
        std::memcpy(copy, mapped, sizeof(copy));
        device->unmap_buffer_region(st.view_cb.buffer);
        view_ok = ue4::parse_view_params(copy, sizeof(copy), view) &&
                  ue4::view_params_plausible(view);
    }

    if (!view_ok)
        return false;

    DispatchSignature sig;
    sig.srvs = st.srvs;
    sig.uavs = st.uavs;
    sig.has_view_cb = true;
    sig.group_count_x = x;
    sig.group_count_y = y;
    sig.group_count_z = z;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_pipeline_hashes.find(st.current_compute_pipeline.handle);
        sig.shader_hash = (it == g_pipeline_hashes.end()) ? 0 : it->second;
    }

    const auto width = static_cast<std::uint32_t>(view.view_size_and_inv_size.x);
    const auto height = static_cast<std::uint32_t>(view.view_size_and_inv_size.y);
    const MatchResult match = match_taa_dispatch(sig, width, height);

    g_diag.last_verdict = match.verdict;
    g_diag.last_reason = match.reason;
    g_diag.last_view = view;
    g_diag.view_plausible = view_ok;

    if (match.verdict == MatchVerdict::hash_and_structural ||
        match.verdict == MatchVerdict::structural_only)
    {
        ++g_diag.taa_dispatches;

        if (!g_diag.logged_full_report)
        {
            g_diag.logged_full_report = true;
            log_full_report(sig, match, view, view_ok);

            // Which colour SRV is the history, decided by identity rather than register order.
            for (const auto &t : sig.srvs)
                if (t.format == TexFormat::r16g16b16a16_float)
                    STRAY_LOG_INFO("colour t%u %s", t.slot,
                        t.resource == g_previous_output_resource
                            ? "== last frame's u0 -> HISTORY"
                            : "-> scene colour (candidate)");
            if (g_previous_output_resource == 0)
                STRAY_LOG_INFO("  (first TAA dispatch seen; history identity resolves next frame)");
        }

        for (const auto &t : sig.uavs)
            if (t.slot == match.output_uav)
                g_previous_output_resource = t.resource;
    }

    return false; // Phase A never suppresses the engine's dispatch.
}
```

- [ ] **Step 7: Wire into CMake and `addon.cpp`**

Add `third_party/reshade_utils` to the add-on's include directories and its two `.cpp` files
plus `src/frame_state.cpp` and `src/taa_hook.cpp` to the `stray-dlss` target. Call
`descriptor_tracking::register_events()` before our own registrations in `DllMain`, and have
`addon.cpp`'s `on_dispatch` delegate to `taa_hook::intercept_dispatch`.

- [ ] **Step 8: Verify it builds and CI is green**

```bash
git push && gh run watch "$(gh run list --limit 1 --json databaseId --jq '.[0].databaseId')" --exit-status
```

Expected: all jobs green, artifact produced.

- [ ] **Step 9: Commit**

```bash
git add -A && git commit -m "Capture and report the real TAA dispatch's bindings and View CB"
```

- [ ] **Step 10: FIELD TEST — the first real run**

Ask the user to install the artifact and play until gameplay, then send back `stray-dlss.log`.

The report answers, in one run: whether the hash still matches; what t1/t3/t4 actually are;
which colour SRV is the history; whether the View CB offsets hold on the live game; whether the
dispatch size matches `ceil(viewrect/8)`; and which descriptor path carries b1.

**Do not start Task 4 until this log has come back and every value in it has been checked
against `CLAUDE.md` §2.** Any discrepancy updates §2 and `docs/STRAY-RENDERING-FACTS.md` in
the same change that reacts to it.

---

## Phase B — Resolve

### Task 4: The dense motion-vector resolve pass

**Files:**
- Create: `shaders/mv_resolve.hlsl`
- Create: `src/mv_resolve.hpp`
- Create: `src/mv_resolve.cpp`
- Create: `src/d3d12_util.hpp`
- Create: `src/d3d12_util.cpp`
- Create: `cmake/CompileShaders.cmake`
- Modify: `CMakeLists.txt`, `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: `ViewParams` (Task 1), `MatchResult` (Task 2), `CommandListState` (Task 3).
- Produces:
  ```cpp
  namespace stray_dlss::mv {
  struct ResolveInputs {
      ID3D12Resource *depth = nullptr, *velocity = nullptr;
      std::uint32_t render_width = 0, render_height = 0;
      std::uint32_t buffer_width = 0, buffer_height = 0;
      std::uint32_t view_rect_min_x = 0, view_rect_min_y = 0;
      const ue4::ViewParams *view = nullptr;
  };
  bool initialise(ID3D12Device *device);
  void shutdown();
  ID3D12Resource *resolve(ID3D12GraphicsCommandList *cmd, const ResolveInputs &in);
  ID3D12Resource *output_texture();   // R16G16_FLOAT at render resolution
  }
  ```

- [ ] **Step 1: Write the shader**

Create `shaders/mv_resolve.hlsl`. This is NVIDIA's `VelocityCombine.usf` math, adapted to read
Stray's buffers directly. Every line that encodes a convention carries its citation.

```hlsl
// Dense motion vectors for DLSS, from UE 4.27's sparse velocity plus camera motion.
//
// Output: R16G16_FLOAT, render resolution, render-resolution PIXELS, [0,0] upper-left,
// pointing BACKWARD (current -> previous). NGX InMVScaleX/Y are therefore (1,1).
// docs/RESEARCH.md section 3.2.

cbuffer Params : register(b0)
{
    float4x4 ClipToPrevClip;      // row-major, jitter-free (NoAA) matrices
    float2   RenderSize;          // View.ViewSizeAndInvSize.xy
    float2   ViewRectMin;         // View.ViewRectMin.xy
    float2   BufferSize;          // View.BufferSizeAndInvSize.xy
    float2   Padding;
};

Texture2D<float>  SceneDepth   : register(t0);  // R32_FLOAT_X8X24_TYPELESS view, reversed-Z
Texture2D<float4> VelocityTex  : register(t1);  // R16G16B16A16_UNORM, BUFFER-sized
RWTexture2D<float2> OutMV      : register(u0);  // R16G16_FLOAT, render-res

// UE 4.27 Common.ush:1537-1570. The bias is 32767/65535, NOT 0.5, and both constants were
// located in Stray's own DXBC. CLAUDE.md section 2.5.
static const float kInvDiv     = 1.0f / (0.499f * 0.5f);
static const float kDecodeBias = (32767.0f / 65535.0f) * kInvDiv;

float2 DecodeVelocity(float4 encoded)
{
    return encoded.xy * kInvDiv - kDecodeBias;
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)RenderSize.x || tid.y >= (uint)RenderSize.y)
        return;

    // Absolute buffer coordinates: the velocity target is BUFFER-sized, not view-sized.
    // docs/RESEARCH.md section 4.8.
    const uint2 buffer_pos = uint2(ViewRectMin) + tid.xy;

    // ViewportUVToScreenPos: (2u - 1, 1 - 2v). Y-up NDC.
    const float2 uv = (float2(tid.xy) + 0.5f) / RenderSize;
    const float2 screen_pos = float2(2.0f * uv.x - 1.0f, 1.0f - 2.0f * uv.y);

    float2 velocity_ndc;

    const float4 encoded = VelocityTex.Load(int3(buffer_pos, 0));

    // UE4 clears velocity to 0 and its own TAA tests `EncodedVelocity.x > 0.0` — strict, red
    // channel only. A magnitude test would be wrong. CLAUDE.md section 2.5.
    if (encoded.x > 0.0f)
    {
        velocity_ndc = DecodeVelocity(encoded);
    }
    else
    {
        // Camera motion from reversed-Z depth. ClipToPrevClip is row-major and jitter-free,
        // so `mul(v, M)` (row-vector) is correct and no jitter compensation is needed.
        // docs/RESEARCH.md section 4.7.
        const float device_z = SceneDepth.Load(int3(buffer_pos, 0));
        const float4 this_clip = float4(screen_pos, device_z, 1.0f);
        const float4 prev_clip = mul(this_clip, ClipToPrevClip);

        if (prev_clip.w > 0.0f)
        {
            const float2 prev_screen = prev_clip.xy / prev_clip.w;
            velocity_ndc = screen_pos - prev_screen;
        }
        else
        {
            velocity_ndc = float2(0.0f, 0.0f); // behind the camera
        }
    }

    // NVIDIA's VelocityCombine.usf: scale to pixels, then negate so the vector points back to
    // the previous frame. Y is flipped because NDC is Y-up and DLSS wants Y-down.
    const float2 mv_pixels = velocity_ndc * float2(0.5f * RenderSize.x, -0.5f * RenderSize.y);
    OutMV[tid.xy] = -mv_pixels;
}
```

- [ ] **Step 2: Add build-time shader compilation**

Create `cmake/CompileShaders.cmake`:

```cmake
# HLSL is compiled to DXBC at build time and embedded as a byte array. D3DCompile is never
# called at runtime: d3dcompiler_47 is routinely absent from a Proton prefix, and that is the
# most common Linux failure mode for tools of this shape. (docs/RESEARCH.md section 1.4)
find_program(FXC_EXECUTABLE fxc
    HINTS "$ENV{WindowsSdkVerBinPath}x64" "$ENV{WindowsSdkDir}bin/x64"
    PATHS "C:/Program Files (x86)/Windows Kits/10/bin"
    PATH_SUFFIXES x64
    DOC "Legacy HLSL compiler for shader model 5.0")

if(NOT FXC_EXECUTABLE)
    message(FATAL_ERROR "fxc not found. It ships with the Windows SDK.")
endif()

# compile_shader(<target> <hlsl> <entry> <profile> <symbol>)
function(compile_shader TARGET SOURCE ENTRY PROFILE SYMBOL)
    get_filename_component(name ${SOURCE} NAME_WE)
    set(out "${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.h")
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
        COMMAND ${FXC_EXECUTABLE} /nologo /T ${PROFILE} /E ${ENTRY}
                /Fh ${out} /Vn ${SYMBOL} /O3 /WX ${SOURCE}
        DEPENDS ${SOURCE}
        COMMENT "fxc ${name}.hlsl -> ${name}.h"
        VERBATIM)
    target_sources(${TARGET} PRIVATE ${out})
    target_include_directories(${TARGET} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
endfunction()
```

In `CMakeLists.txt`, inside the `STRAY_DLSS_BUILD_ADDON` block:

```cmake
include(cmake/CompileShaders.cmake)
compile_shader(stray-dlss "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mv_resolve.hlsl"
               main cs_5_0 g_mv_resolve_dxbc)
```

- [ ] **Step 3: Verify the shader compiles in CI**

Push and watch. Expected: `fxc mv_resolve.hlsl -> mv_resolve.h` in the build log, and the
add-on still links. A shader that does not compile fails the build, which is exactly the
feedback we want given nobody can test it locally.

- [ ] **Step 4: Implement `d3d12_util` and `mv_resolve`**

`d3d12_util` provides: a shader-visible CBV/SRV/UAV descriptor heap owned by us, a helper to
create a committed texture with `ALLOW_UNORDERED_ACCESS`, an upload-heap constant buffer with a
per-frame ring, and a transition-barrier helper.

`mv_resolve` creates a root signature (one CBV, two SRVs, one UAV), a compute PSO from
`g_mv_resolve_dxbc`, and the `R16G16_FLOAT` output at render resolution; recreates them when
the resolution changes; and records the dispatch. Raw D3D12 throughout — we must save and
restore command-list state around NGX anyway, so owning the heap is simpler than interleaving
with ReShade's.

- [ ] **Step 5: Add a debug visualisation and run it in Phase B**

`intercept_dispatch` runs the resolve pass and still returns `false`. A config flag
`[STRAYDLSS] DebugView=1` makes a second tiny pass blit a false-colour view of the MV field
into u0 so the user can screenshot it.

- [ ] **Step 6: Commit and push**

```bash
git add -A && git commit -m "Resolve UE4's sparse velocity into the dense MV field DLSS needs"
```

- [ ] **Step 7: FIELD TEST — the motion-vector debug view**

Ask the user for a screenshot of the debug view while panning the camera and while a moving
object crosses the frame.

What correct looks like: a smooth gradient across static geometry that grows with camera speed
and reverses when the camera reverses; moving objects showing a *different* vector from the
background behind them; and no discontinuity at the boundary between the sparse-velocity region
and the reconstructed region. A hard seam there means the two branches disagree in sign or
scale, which is precisely the bug this phase exists to catch before DLSS ever sees it.

---

## Phase C — Replace

### Task 5: NGX feature creation and evaluation (DLAA)

**Files:**
- Modify: `src/ngx_backend.hpp`, `src/ngx_backend.cpp`
- Create: `src/core/dlss_settings.hpp`, `src/core/dlss_settings.cpp`
- Test: `tests/test_dlss_settings.cpp`
- Modify: `src/taa_hook.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace stray_dlss::dlss {
  struct OptimalSettings { unsigned int mode = 0, optimal_w = 0, optimal_h = 0,
                           min_w = 0, min_h = 0, max_w = 0, max_h = 0; };
  int select_quality_mode(const std::vector<OptimalSettings> &modes,
                          unsigned int render_w, unsigned int render_h,
                          unsigned int output_w, unsigned int output_h);
  }
  namespace stray_dlss::ngx {
  struct FeatureDesc { unsigned int render_w, render_h, output_w, output_h;
                       int perf_quality_mode; unsigned int preset; };
  bool ensure_feature(ID3D12GraphicsCommandList *cmd, const FeatureDesc &desc);
  struct EvalInputs { ID3D12Resource *color, *output, *depth, *motion_vectors;
                      float jitter_x, jitter_y, pre_exposure;
                      unsigned int render_w, render_h; bool reset; };
  bool evaluate(ID3D12GraphicsCommandList *cmd, const EvalInputs &in);
  }
  ```

Quality-mode selection is pure and unit-tested: loop every `NVSDK_NGX_PerfQuality_Value`, take
the mode whose optimal render resolution best matches the resolution actually in use, and fall
back to DLAA when render >= output. Feature flags are
`IsHDR | MVLowRes | DepthInverted | AutoExposure` (`0x4B`). All five preset hint keys are set
to K **before** `NGX_D3D12_CREATE_DLSS_EXT`. A rejected create is retried once with `Balanced`
and `Preset_Default`.

Detailed steps follow the same test-first shape as Tasks 1 and 2.

- [ ] **Step 1: Write the failing test for `select_quality_mode`**
- [ ] **Step 2: Run it, confirm it fails**
- [ ] **Step 3: Implement `dlss_settings`**
- [ ] **Step 4: Run tests, confirm they pass**
- [ ] **Step 5: Implement `ensure_feature` and `evaluate`**
- [ ] **Step 6: Commit**

---

### Task 6: State restoration and the u1 obligation

**Files:**
- Create: `shaders/downsample.hlsl`
- Modify: `src/mv_resolve.cpp` (add the downsample pass), `src/taa_hook.cpp`

NGX clobbers D3D12 command-list state and the game will not re-set it. D3D12 has no state
getters, so the state is tracked from ReShade's events with `state_block::capture()` and
re-applied with `apply()` after evaluation.

When `has_downsample_uav` is true, a second compute pass writes u1 as a half-resolution filter
of the DLSS output. Skipping this leaves downstream bloom and depth of field reading garbage.

Only after both are in place does `intercept_dispatch` return `true`.

- [ ] **Step 1: Write `shaders/downsample.hlsl`**
- [ ] **Step 2: Wire `state_block` capture and apply around the NGX evaluation**
- [ ] **Step 3: Switch `intercept_dispatch` to return true on a confirmed match**
- [ ] **Step 4: Verify CI is green**
- [ ] **Step 5: Commit**

- [ ] **Step 6: FIELD TEST — DLAA**

Ask for a screenshot plus `stray-dlss.log`, and for a run with
`DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSIndicator=1024` so the DLSS overlay proves DLSS is
actually executing rather than the add-on having silently fallen back.

Failure signatures worth naming in advance: smearing on static geometry while the camera moves
means the camera-motion branch is wrong; ghosting behind moving objects means the sparse branch
is wrong; ghosting plus brightness lag means pre-exposure is wrong; a black image means a
missing `ALLOW_UNORDERED_ACCESS` (NGX returns success and writes nothing); shimmer that never
resolves means the jitter sign is wrong — test that in-game with the SDK DLL's Ctrl+Alt+F9 and
Ctrl+Alt+F10 jitter-negation cycling rather than guessing in code.

---

### Task 7: Configuration, overlay and failure policy

**Files:** Modify `src/addon.cpp`, `src/taa_hook.cpp`

A `[STRAYDLSS]` section in ReShade.ini read at `init_device` and written from the settings
overlay: `Enabled`, `Preset`, `QualityMode`, `DebugView`, `LogVerbose`. The OSD overlay shows
one live status line. Every failure path logs once with a reason and returns `false` so the
engine's TAA runs — this is what makes the artifact safe to leave installed between iterations.

- [ ] **Step 1: Implement config get/set**
- [ ] **Step 2: Expand the settings and OSD overlays**
- [ ] **Step 3: Audit every early return for a one-shot log**
- [ ] **Step 4: Commit**

---

## Phase D — Upscale

### Task 8: Super resolution

**Files:** Modify `src/taa_hook.cpp`, `src/ngx_backend.cpp`, `README.md`; create `docs/ENGINE-INI.md`

Ship an `Engine.ini` `[SystemSettings]` snippet with `r.ScreenPercentage=<N>` and
`r.TemporalAA.Upsampling=1`, which makes UE4 render smaller, allocate a full-resolution output,
and expect its TAA pass to upscale — DLSS's contract exactly, plus plain Halton jitter, an
auto-scaled phase count and the correct mip bias for free.

Consequences to handle: the permutation and therefore the DXBC hash change, so
`0x1708ec956099e259` no longer appears and the structural matcher from Task 2 carries the
identification alone; input and output rects now differ, so `InRenderSubrectDimensions` and the
output size must come from the View CB rather than being assumed equal; and `r.TemporalAASamples`
must be raised for the higher-scaling modes.

Hard constraints: `r.TemporalAA.Algorithm` must stay 0, and
`r.TemporalAA.HistoryScreenPercentage` must stay 100.

- [ ] **Step 1: Re-derive the upsampling permutation's hash from a field log**
- [ ] **Step 2: Handle differing input and output rects**
- [ ] **Step 3: Expose quality presets in the overlay**
- [ ] **Step 4: Document the Engine.ini snippet**
- [ ] **Step 5: Commit**
- [ ] **Step 6: FIELD TEST — SR at each preset**

---

## Self-review notes

**Spec coverage.** Every section of the design spec maps to a task: §4.1 → Tasks 2 and 3,
§4.2 → Tasks 2 and 3, §4.3 → Tasks 1 and 3, §4.4 → Task 4, §4.5 → Task 5, §4.6 → Task 6,
§4.7 → Task 6, §4.8 → Task 7, phases → task ordering.

**Type consistency.** `TexFormat`, `BoundTexture`, `DispatchSignature`, `MatchResult` and
`MatchVerdict` are defined once in Task 2 and consumed unchanged by Tasks 3–8. `ViewParams`,
`Float2`, `Float4` and `Matrix4` are defined once in Task 1. `to_tex_format` is the single
bridge from `reshade::api::format`.

**Known thinness.** Tasks 5–8 are specified to the interface level rather than step-by-step
code. This is deliberate: their detail depends on what the Task 3 and Task 4 field tests
return, and writing precise code against unvalidated assumptions is how a plan turns into
confident nonsense. **Each of Tasks 5–8 gets expanded into full step-by-step form once the
preceding field test has come back.**
