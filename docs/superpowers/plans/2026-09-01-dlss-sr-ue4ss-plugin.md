# DLSS Super Resolution as a UE4SS C++ Plugin — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the first DLSS Super Resolution build of stray-dlss that runs without ReShade — a UE4SS C++ plugin (`mods/StrayDLSS`) loading an own D3D12 vtable-hook layer — while the ReShade add-on keeps building and working at every intermediate commit.

**Architecture:** One framework-free interception seam (`src/intercept/`) with two backends: the existing ReShade add-on (moved behind the seam, no behaviour change) and a new native backend that vtable-patches vkd3d's real device, command list, queue and swapchain, shadows descriptors and root arguments itself, and never substitutes an object or mints a handle. The native backend is first run *beside* ReShade as a differential observer that diffs every resolved dispatch against ReShade's answer; only at zero disagreements does it take over the TAA hook, and only after its own screenshot channel is validated against ReShade's does ReShade leave the process. UE4SS is the loader and, later, the engine-side signal source; it contributes no graphics code.

**Tech Stack:** C++20 (C++23 in the one UE4SS glue file), CMake, MSVC via GitHub Actions only, NVIDIA NGX D3D12 SDK v310.7.0, MinHook (BSD-2, vendored) for at most three export detours, plain `VirtualProtect` vtable patching (already proven in `src/ext_unhook.cpp`), RE-UE4SS at SHA `68caddcf` for the glue, doctest + WARP harness for CI.

**Spec:** `docs/RESEARCH-UE4SS-MIGRATION.md` (the feasibility assessment this plan implements — its §1 risk order, §5 regressions and §9 stages are the argument; this plan is the execution). Facts: `CLAUDE.md`, `docs/RESEARCH.md`.

## Global Constraints

Copied from `CLAUDE.md`; every task inherits them.

- **You cannot test this yourself.** No game, no Windows box, no GPU. Every build happens in GitHub Actions with MSVC. The only feedback loop is the human running the artifact on their Linux/Stray machine and pasting back a log.
- **Prefer a loud failure to a quiet wrong image.** Every convention must be asserted and logged, never assumed.
- **Never claim something works because it compiled.** The only evidence DLSS is engaged is a log line from the user's machine, a screenshot, or the DLSS on-screen indicator.
- **Everything provable in CI must be tested in CI.** Keep the untestable NGX surface as thin as possible.
- **HARD / SOFT / UNCONFIRMED.** Label the provenance of every technical claim.
- **Pass plain `ID3D12Resource*` to NGX and nothing else.** Never touch `ID3D12DXVKInteropDevice`, `GetVulkanResourceInfo`, or any vkd3d interop API. Never write a D3D12→Vulkan bridge. Never use the Vulkan NGX path.
- **Initialise NGX with the original vkd3d `ID3D12Device`** (today `device::get_native()`), never a proxy.
- **Do not gate availability on `NVSDK_NGX_D3D12_GetFeatureRequirements`.**
- **`PROTON_ENABLE_NVAPI` does not exist.** Never set `PROTON_HIDE_NVIDIA_GPU=1`. Do not set `VKD3D_CONFIG=descriptor_heap`.
- **CMake + GitHub Actions, MSVC, x64 only.** No local build. Warnings are errors, `/W4`.
- **Precompile every HLSL shader to DXBC at build time. Never call `D3DCompile` at runtime.**
- **The add-on stays `/MT` + `nvsdk_ngx_s.lib`** (CMakeLists.txt, the msvcp140 `_Mtx_lock` trap). **A UE4SS mod must be `/MD`** (`mods/StrayDualSense/CMakeLists.txt`: "an ABI CONTRACT, not a style choice") and therefore `nvsdk_ngx_d.lib`. Both are true at once; the plan builds the shared code twice.
- **Never `CopyDescriptorsSimple` out of the game's bound heap** (D3D12 ERROR #654, reproduced in CI). **Never call `GetDesc()` on a resource you have not proven live.**
- **NGX is not thread-safe.** Teardown on one thread, GPU idle first: `ReleaseFeature` → `DestroyParameters` → `Shutdown1`.
- **Never commit `nvngx_dlss.dll`, `nvsdk_ngx_*.lib` or game assets.**
- Every constant from `CLAUDE.md` §2 gets a named symbol with a comment citing §2.x.
- **The ReShade build (`stray-dlss.addon64`) must be green and behaviourally identical at the end of every task through Stage 3.** A task that breaks it is not done.
- **Frame generation is out of scope** (`docs/RESEARCH-UE4SS-MIGRATION.md` §7, §8.5): removing ReShade does not unlock it. It must not be used to justify any task here.

---

## Part A — What can be abstracted: the seam

### A.1 What the add-on actually consumes from ReShade (HARD, counted from `src/`)

`grep -c "reshade::"` per file, 2026-09-01: `addon.cpp` 210 · `frame_state.cpp` 82 · `frame_state.hpp` 23 · `pass_finder.cpp` 17 · `taa_hook.cpp` 14 · `nr_hook.cpp` 11 · `pass_finder.hpp` 10 · `nr_hook.hpp` 7 · `nr_history.cpp` 7 · `gbuffer_finder.cpp` 7 · `gbuffer_finder.hpp` 5 · `taa_hook.hpp` 3 · `shader_dump.cpp` 1 · `nr_history.hpp` 1 · `log.cpp` 1. **Every other file in `src/` is zero** — `ngx_backend`, `ngx_nr`, `ngx_snippet`, `nr_codec_pass`, `mv_resolve`, `gbuffer_resolve`, `input_dump`, `ext_unhook`, `d3d12_restore`, `perf`, all of `src/core/`, all shaders.

Grouped by what they *do* (HARD, read from the call sites):

| Need | ReShade surface used today | Where |
|---|---|---|
| Framework events in | 26 `register_event<...>` in one function | `addon.cpp:1714-1914` `register_events()` |
| Pipeline identity | `init_pipeline` (DXBC bytes), `destroy_pipeline`, `bind_pipeline` (fires with `pipeline_stage::all`) | `addon.cpp`, `taa_hook.cpp:32` |
| Root descriptors / constants | `push_descriptors`, `push_constants` | `frame_state.cpp:245,617` |
| Descriptor-table contents | `descriptor_tracking` (vendored) + `device->get_descriptor_heap_offset` | `frame_state.cpp:368,439,523` |
| Which tables are bound | `state_tracking` (vendored) `cmd_list->get_private_data<state_tracking>()` | `frame_state.cpp:173,394,458,645` |
| Resource facts | `device->get_resource_desc`, `get_resource_from_view`, `get_resource_view_desc` | `taa_hook.cpp:873`, `nr_hook.cpp:635,661`, `pass_finder.cpp:357` |
| The View CB read | `device->map_buffer_region(..., read_only)` / `unmap_buffer_region` | `taa_hook.cpp:892-896` |
| Liveness | `init_resource` / `destroy_resource` → `g_live_resources` | `frame_state.cpp:779-801` |
| The one suppressible event | `dispatch` returning `bool` | `addon.cpp` `on_dispatch` |
| Frame boundary | `present(queue, swapchain, ...)` + `swapchain->get_current_back_buffer()` | `addon.cpp:937-1000` |
| Post-tonemap sites | `bind_render_targets_and_depth_stencil`, `init_swapchain`, `reshade_begin_effects` | `nr_hook.cpp:612-661` |
| Finder taps | `draw`, `draw_indexed`, `begin_render_pass`, `copy_resource`, `copy_texture_region`, `resolve_texture_region`, `execute_command_list` | `addon.cpp` `on_pf_*` |
| Native access | `get_native()` (16 sites) | `taa_hook.cpp` ×10, `nr_hook.cpp` ×2, `nr_history.cpp` ×2, `addon.cpp` ×3, `ngx_backend.hpp` ×1 |
| Config | `get_config_value` (55 keys, 79 sites, 74 in `addon.cpp`) | `addon.cpp`, `shader_dump.cpp:33` |
| Log | `reshade::log::message` | `log.cpp:72` |
| Overlay | `register_overlay` ×2, ~65 `ImGui::` calls | `addon.cpp:1912-1913` |

**Two ReShade behaviours the seam must NOT reproduce**, because the project already works around them (HARD, `src/frame_state.hpp:79-113`): `descriptor_tracking` keeps a dangling pointer for `descriptor_table_with_flags` (UE4's variant), and `state_tracking` has no `push_descriptors`/`push_constants` handler so `apply()` cannot replay root CBVs. The seam's `restore_game_compute_state` is a replay of *observed* root arguments, which `src/d3d12_restore.{hpp,cpp}` already implements natively and WARP-tests.

### A.2 The interface

One header of plain types, one `Backend` (what a consumer asks the framework), one `Sink` (what the framework tells a consumer). No ReShade type, no UE4SS type, no ImGui. `ID3D12*` forward declarations only.

```cpp
// src/intercept/types.hpp  — plain data, buildable on Linux (no d3d12.h)
namespace stray_dlss::icept {

using ResourceId   = std::uint64_t;  // ID3D12Resource* as an integer. IDENTITY ONLY.
using DescriptorId = std::uint64_t;  // D3D12_CPU_DESCRIPTOR_HANDLE::ptr. Always a REAL handle:
                                     // ReShade un-mangles before reporting (RESEARCH.md §2.4),
                                     // the native backend never mints one.
constexpr std::uint64_t kUnknownSize = ~0ull;

struct BufferRange { ResourceId buffer = 0; std::uint64_t offset = 0; std::uint64_t size = kUnknownSize; };

struct ResourceInfo {
    bool is_buffer = false; std::uint64_t buffer_size = 0;
    std::uint32_t width = 0, height = 0, mip_levels = 0, array_size = 0, sample_count = 0;
    TexFormat format = TexFormat::unknown;   // core/taa_signature.hpp
    bool upload_heap = false;                // Map() legal for a CPU read
    bool allow_uav = false;
};

struct DispatchBindings {                    // moved verbatim from frame_state.hpp, retyped
    std::vector<BoundTexture> srvs, uavs;    // BoundTexture: core/taa_signature.hpp:38 (unchanged)
    std::vector<std::pair<std::uint32_t, BufferRange>> constant_buffers;
    BufferRange view_cb{}; bool view_cb_valid = false; std::uint32_t view_cb_register = 0;
    ::ID3D12DescriptorHeap *heaps[2] = {}; unsigned heap_count = 0;
};

struct CommandContext {                      // one per GAME command list, keyed by `native`
    ::ID3D12GraphicsCommandList *native = nullptr;
    ::ID3D12Device *device = nullptr;        // the ORIGINAL vkd3d device, both backends
    std::uint64_t backend_cookie = 0;        // reshade::api::command_list* or a native record id
};

struct PresentContext {
    ::ID3D12CommandQueue *queue = nullptr; ::IDXGISwapChain3 *swapchain = nullptr;
    ::ID3D12Resource *back_buffer = nullptr; std::uint64_t frame = 0;
};
} // namespace stray_dlss::icept
```

```cpp
// src/intercept/backend.hpp
namespace stray_dlss::icept {
class Backend {
public:
    virtual ~Backend() = default;
    virtual const char *name() const = 0;                       // "reshade" | "native"
    virtual bool resolve_compute_bindings(const CommandContext &, DispatchBindings &out) = 0;
    virtual bool resolve_graphics_srvs(const CommandContext &, std::vector<BoundTexture> &out) = 0;
    virtual bool describe_resource(ResourceId, ResourceInfo &out) = 0;   // false if not KNOWN LIVE
    virtual bool resource_from_view(DescriptorId, ResourceId &out) = 0;  // false if unknown/dead
    virtual bool read_buffer(const BufferRange &, std::uint64_t bytes, void *out) = 0; // the View CB
    virtual bool is_resource_live(ResourceId) = 0;
    virtual void restore_game_compute_state(const CommandContext &) = 0;
};
struct Sink {                                // implemented by app::DlssApp
    virtual ~Sink() = default;
    virtual void on_device(::ID3D12Device *, bool created) = 0;
    virtual void on_pipeline(std::uint64_t pso, const void *cs_dxbc, std::size_t len, bool created) = 0;
    virtual void on_command_list_reset(const CommandContext &) = 0;
    virtual void on_bind_pipeline(const CommandContext &, std::uint64_t pso) = 0;
    virtual bool on_dispatch(const CommandContext &, std::uint32_t x, std::uint32_t y, std::uint32_t z) = 0; // true = suppress
    virtual void on_render_targets(const CommandContext &, std::uint32_t n, const DescriptorId *rtvs, DescriptorId dsv) = 0;
    virtual void on_draw(const CommandContext &, std::uint32_t vertex_or_index_count) = 0;
    virtual void on_copy(const CommandContext &, ResourceId src, ResourceId dst) = 0;
    virtual void on_execute(const CommandContext &) = 0;
    virtual void on_swapchain(::IDXGISwapChain3 *, bool created) = 0;
    virtual void on_present(const PresentContext &) = 0;
};
Backend *backend();  void set_backend(Backend *);   // exactly one DRIVER at a time
} // namespace stray_dlss::icept
```

`reshade_begin_effects` is deliberately **not** in `Sink`: it is a ReShade-only site with a documented inertness trap (CLAUDE.md §5: never fires with an empty preset). `NgxNRHook=present` stays a ReShade-backend-only mode and is refused loudly by the native backend.

### A.3 File map

| File | Stage 1 change | Reason |
|---|---|---|
| `src/reshade_all.hpp` | unchanged, but included **only** from `src/backend_reshade/` | the one cut point (HARD, 13 lines) |
| `src/addon.cpp` (2,043) | → `src/app/dlss_app.{hpp,cpp}` (framework-free `Sink`) + `src/backend_reshade/addon_entry.cpp` (`DllMain`, `register_addon`, `register_events()`, overlays) | all 26 registrations already sit in one function (HARD) |
| `src/frame_state.{hpp,cpp}` (956) | → `src/backend_reshade/reshade_bindings.{hpp,cpp}` (the trackers, the ReShade half of restore) + `src/intercept/types.hpp` (`DispatchBindings`) | 110 refs; the deepest coupling; it *is* the ReShade backend's resolver |
| `src/taa_hook.{hpp,cpp}` | retype `reshade::api::command_list*` → `const icept::CommandContext &`; `read_view_cb` → `backend()->read_buffer` | 17 refs, all at `:32,391,398,638,678,741,871-893,1087-1102,1243` |
| `src/nr_hook.{hpp,cpp}`, `src/nr_history.{hpp,cpp}`, `src/pass_finder.{hpp,cpp}`, `src/gbuffer_finder.{hpp,cpp}` | same retype; `get_resource_from_view` → `backend()->resource_from_view`; `get_resource_desc` → `describe_resource` | ≤17 refs each |
| `src/log.cpp:72` | `reshade::log::message` becomes an installable external sink | 1 ref |
| `src/shader_dump.cpp:33` + 74 sites in `addon.cpp` | `host::cfg::get_*` | config seam |
| `src/d3d12_restore.*`, `src/ngx_*`, `src/mv_resolve.*`, `src/gbuffer_resolve.*`, `src/nr_codec_pass.*`, `src/input_dump.*`, `src/ext_unhook.*`, `src/perf.*`, `src/core/*`, `shaders/*` | **unchanged** | zero ReShade references (HARD) |
| `third_party/reshade_utils/*` | unchanged, linked only into the ReShade backend | |
| `tests/warp/fake_reshade_command_list.hpp` | unchanged through Stage 3, deleted in Stage 4 | |

New directories: `src/intercept/` (seam), `src/host/` (config/log/ini, lifted from `mods/StrayDualSense/src/`), `src/backend_reshade/`, `src/backend_native/`, `src/app/`, `mods/StrayDLSS/` (UE4SS glue + CMake), `.github/workflows/dlss-plugin.yml`.

---

## Part B — Libraries: verdicts

Legend: **use** = link/vendor; **lift** = copy specific code with attribution; **reference** = read, do not copy; **avoid**.

| Library | Licence | Verdict | One line |
|---|---|---|---|
| NVIDIA DLSS UE plugin (4.27) | NVIDIA RTX SDKs licence (SDK); the plugin bundle's own text UNCONFIRMED from the unofficial mirror | **reference** (already lifted where it mattered) | `VelocityCombine.usf` math is already in `shaders/mv_resolve.hlsl:2` (HARD); the remaining gaps are integration logic we already have, not shader code |
| OptiScaler | **GPL-3.0** (HARD, fetched `LICENSE`) | **avoid as code; reference for Proton behaviour** | copyleft is incompatible with an NGX-linked MIT/BSD-style add-on; and as a runtime it silently swaps our params for FSR2 (CLAUDE.md §5) |
| MinHook | **BSD-2-Clause** (HARD, fetched) | **use** for ≤3 export detours | ReShade itself vendors it (`deps/minhook`, `source/hook.cpp`, HARD); C, four files, `/MT`-safe |
| Microsoft Detours | MIT (HARD, fetched) | avoid | heavier, C++ transactions API, nothing MinHook lacks for our three functions |
| PolyHook2 | MIT (HARD, fetched) | **avoid in the hook layer; available in the glue** | UE4SS links it `PUBLIC` at the pinned SHA (HARD, `UE4SS/CMakeLists.txt`), so the mod gets it for free — but the hook layer must build without UE4SS, and needs Zydis + C++17 headers |
| SafetyHook | Boost 1.0 (HARD, fetched) | avoid | newest, Zydis dependency, no advantage for three static exports |
| Streamline interposer | MIT with NVIDIA header (HARD, fetched `license.txt`) | **reference only** | its "own the device from creation" shape is what our early-attach path does; we never link it (FG out of scope) |
| Special K | GPL-3.0 (HARD, fetched) | avoid | copyleft; also a 17k-file monolith |
| RenoDX | MIT (HARD, fetched) | reference | ReShade add-on; its D3D12 state handling is ReShade's, not its own |
| Luma-Framework | custom MIT, commercial use needs permission (HARD, fetched), **DX11-only** (HARD) | reference | not applicable to D3D12 descriptor tracking |
| ReShade `descriptor_tracking.cpp` / `state_tracking.cpp` | BSD-3 OR MIT (HARD, SPDX header in the vendored files) | **keep for the ReShade backend; do NOT vendor into the native backend** | they are built from ReShade's *translated* events (`update_descriptor_tables`/`copy_descriptor_tables`), a surface the native backend does not have; the native shadow hooks the raw D3D12 calls and is simpler (assessment §1.1) |
| ReShade `d3d12_impl_device.cpp` (upstream, local checkout) | BSD-3 | **reference** for two mechanisms | `_buffer_gpu_addresses` registry at `register_resource` (`:2107-2124`, HARD) — the GPU-VA → resource map the View CB read needs; and `d3d12_extensions.cpp` as the cautionary tale |

### B.1 NVIDIA DLSS UE plugin — what to lift, what is already lifted (HARD from `src/`)

| Plugin piece | Status here |
|---|---|
| `VelocityCombine.usf` dense-MV math, sign, `ClipToPrevClip` branch | **done**: `shaders/mv_resolve.hlsl` ("NVIDIA's own VelocityCombine.usf math, adapted"), CI-tested camera-branch orientation (`warp_mv_resolve.cpp:846`) |
| Jitter pass-through (`TemporalAAParams.zw` unmodified) | **done**: `ngx_backend.hpp` `EvaluateInputs::jitter_x/y`, CLAUDE.md §2.7 |
| Exposure pair (`AutoExposure` flag vs texture; `PreExposure` unmodified in both modes) | **done**: `ngx_backend.hpp:63-98` cites `NGXRHI.cpp:537-546`, `DLSSUpscaler.cpp:1085-1089` |
| Feature create/recreate on size change, quality from ratio | **done**: `ensure_feature`, `core/dlss_quality.hpp` |
| Preset hints before create | **done**: `set_preset` |
| Reflex, DLSS-G, `IDXGISwapChainProvider` | **out of scope** (not in UE 4.27 anyway — assessment §7.4) |

**Nothing remains to lift for SR.** The plugin's remaining value is as the citation for conventions, which `ngx_backend.hpp` already carries line-by-line. Its shader/plugin source is under NVIDIA's RTX SDKs licence at best; we copy math, not files.

### B.2 OptiScaler — reusable pieces versus a runtime to fight

GPL-3.0 (HARD) settles the code question: nothing is lifted. Its hooking engine could not be determined from the tree (`OptiScaler/hooks/Hook_Utils.h` holds only signature-check templates; `external/` has no Detours/MinHook/PolyHook entry) — **UNCONFIRMED**, irrelevant given the licence. What it *teaches* (all HARD from CLAUDE.md §5): a process-wide suffix-matched `LoadLibrary` hook intercepts *our* NGX calls; `FGOutput=dlssg` needs Streamline; the FG swapchain wrapper dies at a fixed frame; and `Dx12Upscaler=dlss` can silently become FSR2. Its `wrapped_swapchain.{h,cpp}` is the shape of the one object substitution FG would ever require (assessment §7.4) — reference, not code, and not in this plan.

### B.3 Hooking engine — vtable patch versus inline detour, and vkd3d's static vtables

* **COM vtable patching needs no library.** `src/ext_unhook.cpp:105-118` already does the `VirtualProtect` → write → restore dance on this exact target (HARD). A 40-line `vtable_patch.hpp` (Task 9) is the whole engine for ~30 slots across four interfaces.
* **vkd3d-proton uses a single static vtable for `ID3D12DeviceExt`** — measured on the RTX 4090 (`warp_mv_resolve.cpp:1347-1400`, HARD). That the main interfaces (`ID3D12Device`, `ID3D12GraphicsCommandList`, `ID3D12CommandQueue`, `ID3D12Resource`, the DXGI swapchain) are likewise static is **SOFT** (vkd3d-proton is C with `CONST_VTBL` structs) and is Task 1's first measurement. Consequence when true: one patch reaches every existing and future object, so **late attach costs nothing for interception** (assessment §6.2) — and our own dummy objects share the vtable, so hooks fire for our calls too (Task 12's guards).
* **Inline detours are needed only for early attach**: `D3D12CreateDevice` and `CreateDXGIFactory1/2`, so the game's device, queue and swapchain are caught at creation. **MinHook** (BSD-2) — the same engine ReShade uses (HARD), so two MinHook copies coexist in-process during Stages 2–3 exactly as ReShade + any MinHook-based overlay already do (SOFT: each DLL has its own `MH_Initialize` state and they hook different functions).
* **Contested slots:** during Stages 2–3 ReShade's proxies sit *above* our patches (the game calls ReShade's proxy, which forwards to the real object, whose vtable we patched), so our hooks see real handles after ReShade's conversion — the property the differential observer depends on (SOFT, from ReShade's proxy design; verified in Task 15 under the real-ReShade CI lane). We save the original slot, always call it, and **never restore** (assessment §1.2).

### B.4 ReShade's trackers — vendor or not

Keep them exactly where they are (ReShade backend). The native shadow (Task 11–12) is not a port of them: ReShade's `descriptor_tracking` cannot see the game's heap and is fed by ReShade's own translated updates; ours hooks `CreateShaderResourceView`/`CopyDescriptors` directly and keys by real CPU handle, which removes the need to identify offline heaps at all (assessment §1.1, SOFT). `state_tracking`'s missing `push_descriptors` is why `d3d12_restore.cpp` exists; hooking `SetComputeRoot*` gives the complete argument list (assessment §3.3, HARD).

---

## Part C — What is missing, ordered by risk

S ≈ a day, M ≈ a few days, L ≈ a week with a real feedback loop (assessment's calibration).

| # | Work | Size | Risk | How it is tested |
|---|---|---|---|---|
| C1 | **Descriptor + root-argument shadow** (Tasks 11–12) | L | no offline oracle for "what UE4 bound at t3" | unit: handle arithmetic, VA lookup, root-signature walk on plain structs · WARP: synthetic UE4-shaped frame, exact register reconstruction · **differential observer vs ReShade** in CI (real-ReShade lane) and on the box |
| C2 | **Hook installation timing** — UE4SS loads after the device may exist (Task 2, 13) | M | if late attach cannot see pre-existing views, C1 is incomplete | Stage 0 probe logs whether `D3D12CreateDevice` fires after `start_mod()`; the `unknown-slot` counter at every matched TAA dispatch; fallback = proxy-DLL loader (Task 13 early path) |
| C3 | **Build split**: `/MT`+`nvsdk_ngx_s.lib` add-on vs `/MD`+`nvsdk_ngx_d.lib` UE4SS mod, UE4SS ABI pinned to `68caddcf`, Epic-gated submodule | M | wrong CRT = heap corruption, not a load error | `dumpbin /DEPENDENTS` assertions lifted from `dualsense.yml`; `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` on both |
| C4 | **Present ownership** — own allocator/list/fence on the presenting queue (Task 18) | M | wrong queue or wrong back-buffer state = hang | WARP: debug layer on our present-time list; box: `dmesg` clean, fps unchanged |
| C5 | **Screenshot channel** — back-buffer readback on a command file (Task 19) | M | losing the project's only visual instrument mid-migration | validated **against ReShade's PNG of the same frame** before ReShade is removed |
| C6 | **Resource liveness without `init_resource`/`destroy_resource`** (Task 10) | S | sentinel semantics under vkd3d | Stage 0 probe (c); WARP test; Release-hook fallback |
| C7 | **Config + log** (Task 4–5) | S | none — lifted from `mods/StrayDualSense` | unit tests on the INI parser; behaviour identical under ReShade |
| C8 | **Overlay replacement** (Task 20) | S now, L if faithful | the live-checkbox A/B becomes an INI edit | hot-reload INI + `stray-dlss-status.txt`; OSD deferred (assessment §5.1) |
| C9 | **CI lane replacing real-ReShade** (Task 14–15) | M | losing "runs through a real proxy" evidence | native-hooks-on-WARP lane; real-ReShade lane kept and *extended* with the differential observer while both backends exist |
| C10 | **UE4SS glue** (Task 16) | S code, L build | Epic-gated `UEPseudo`, rolling ABI | the `dualsense.yml` recipe, byte-for-byte |

---

## Stage 0 — Measurements that decide the architecture (2 tasks)

No architecture is written until these have run on the box. Each is one launch.

### Task 1: Static-vtable and private-data probes in the WARP/hardware harness

**Files:**
- Modify: `tests/warp/warp_mv_resolve.cpp` (add two tests beside `test_vkd3d_ext_hook_reachability` at `:1347`)
- Modify: `tests/warp/warp_mv_resolve.cpp:1504` `main` — register both

**Interfaces:**
- Produces: log lines `vtable-static: list=%d queue=%d resource=%d device=%d` and `private-data-release: fired=%d` which Task 9 and Task 10 cite.

- [ ] **Step 1: Write the static-vtable test**

```cpp
// Whether vkd3d-proton (and WARP) share ONE vtable across objects of a class. The whole
// native backend patches vtable SLOTS, so a per-object vtable would need a different
// installation strategy (assessment §1.2, §8.3). Measured, never assumed.
bool test_static_vtables(Gpu &gpu)
{
	std::printf("\n[test] whether vtables are shared across objects\n");
	const auto vt = [](IUnknown *o) { return *reinterpret_cast<void ***>(o); };

	ComPtr<ID3D12CommandAllocator> alloc;
	ComPtr<ID3D12GraphicsCommandList> list_a, list_b;
	if (FAILED(gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
		FAILED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list_a))) ||
		FAILED(gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list_b))))
	{
		std::printf("  FAIL: could not create two command lists\n");
		return false;
	}
	list_a->Close(); list_b->Close();

	D3D12_COMMAND_QUEUE_DESC qd = {};
	ComPtr<ID3D12CommandQueue> queue_b;
	gpu.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_b));

	ComPtr<ID3D12Resource> res_a = create_buffer(gpu, 256), res_b = create_buffer(gpu, 256);

	const bool list_static  = vt(list_a.Get()) == vt(list_b.Get());
	const bool queue_static = vt(gpu.queue.Get()) == vt(queue_b.Get());
	const bool res_static   = vt(res_a.Get()) == vt(res_b.Get());
	std::printf("  vtable-static: list=%d queue=%d resource=%d\n", list_static, queue_static, res_static);
	// Not an assertion on the box: the answer is the measurement. On WARP the debug layer may
	// wrap objects, so only report there too.
	return true;
}
```

`create_buffer` exists in the harness already (used by the resolve tests); if it does not, add a 12-line `CreateCommittedResource` helper on the default heap.

- [ ] **Step 2: Write the private-data destruction test**

```cpp
// Whether ID3D12Object::SetPrivateDataInterface releases the interface when the object dies.
// That release is the destruction callback Task 10 keys resource liveness on (assessment
// §1.4). Documented D3D12 behaviour on Microsoft's runtime; vkd3d-proton is the question.
namespace {
struct Sentinel : IUnknown
{
	std::atomic<ULONG> refs{ 1 };
	bool *fired;
	explicit Sentinel(bool *f) : fired(f) {}
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
	{
		if (riid == __uuidof(IUnknown)) { *out = this; AddRef(); return S_OK; }
		*out = nullptr; return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG r = --refs;
		if (r == 0) { *fired = true; delete this; }
		return r;
	}
};
}

bool test_private_data_release_on_destroy(Gpu &gpu)
{
	std::printf("\n[test] whether private-data interfaces are released at object destruction\n");
	static const GUID kTag = { 0x5d1e9c30, 0x7a4b, 0x4e02, { 0x9b, 0x1f, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 } };
	bool fired = false;
	{
		ComPtr<ID3D12Resource> res = create_buffer(gpu, 256);
		Sentinel *s = new Sentinel(&fired);
		res->SetPrivateDataInterface(kTag, s);
		s->Release();            // the object now holds the only reference
	}                            // res destroyed here
	std::printf("  private-data-release: fired=%d\n", fired ? 1 : 0);
	return true;
}
```

- [ ] **Step 3: Register both in `main`, build in CI, read the WARP numbers**

Run: push; CI job `WARP harness (NGX=ON only)`. Expected: both lines printed; on WARP `fired=1` (documented), vtable-static values reported.

- [ ] **Step 4: Run on the box through `tools/run-harness-proton.sh`**

Expected log lines from vkd3d-proton 3.1.0. **Record the answers in `docs/STRAY-RENDERING-FACTS.md`** under a new "vkd3d object model" heading: `list=1 queue=1 resource=1` makes Task 9's slot-patch strategy HARD; `fired=1` makes Task 10's sentinel HARD. Any 0 selects the fallback named in that task.

- [ ] **Step 5: Commit**

```bash
git add tests/warp/warp_mv_resolve.cpp docs/STRAY-RENDERING-FACTS.md
git commit -m "test(warp): measure vtable sharing and private-data release under vkd3d"
```

### Task 2: The `StrayDLSS` UE4SS skeleton — timing probe and build proof

**Files:**
- Create: `mods/StrayDLSS/CMakeLists.txt`, `mods/StrayDLSS/src/Mod.cpp`, `mods/StrayDLSS/src/Probe.{hpp,cpp}`, `mods/StrayDLSS/StrayDLSS.ini`, `mods/StrayDLSS/README.md`
- Create: `third_party/minhook/` (vendored MinHook: `MinHook.h`, `buffer.c/.h`, `hook.c`, `trampoline.c/.h`, `hde/` — pinned to the same commit ReShade vendors; record the SHA in `third_party/minhook/VERSION`)
- Create: `.github/workflows/dlss-plugin.yml` (copy of `dualsense.yml` with names changed; see below)

**Interfaces:**
- Produces: `stray-dlss-plugin.log` lines `probe: start_mod at t=%llu ms; d3d12.dll loaded=%d dxgi.dll loaded=%d`, `probe: D3D12CreateDevice HOOK FIRED at t=%llu ms (device=%p)` or, at frame N, `probe: hook never fired — device pre-existed`, and `probe: hidden window + dummy swapchain: hr=0x%08x, game responsive=?`.

- [ ] **Step 1: Copy the scaffolding exactly as the DualSense plugin has it**

From `mods/StrayDualSense/`: `CMakeLists.txt` (the two-role file: standalone builds only portable pieces, sub-project builds the DLL when `TARGET UE4SS` exists; `/MD` `MSVC_RUNTIME_LIBRARY`; `/external:anglebrackets /external:W0`; `cxx_std_23` on the DLL only), `src/Log.{hpp,cpp}` (mutex-guarded, flush-per-line, mirror queue), `src/Config.{hpp,cpp}` (INI + mtime hot reload — struct fields replaced in Task 4), `src/Platform.{hpp,cpp}` (`GameBinariesDir`, `ModuleDir`, `Widen/Narrow`). Rename namespace `sds` → `stray_dlss::host` only when they move under `src/host/` in Task 4; for the probe, use them verbatim.

From `.github/workflows/dualsense.yml`: everything — the `UE4SS_SHA: 68caddcf...` pin and its "pin the SHA, never the tag" comment, the `UEPSEUDO_PAT`/mirror fallback, the parent-project layout, `--target StrayDLSS`, the `dumpbin /EXPORTS` check for `start_mod`/`uninstall_mod`, the `dumpbin /DEPENDENTS` **MSVCP140 must be present** check, staging to `dist/StrayDLSS/dlls/main.dll`. Paths filter: `mods/StrayDLSS/**`, `src/**`, `.github/workflows/dlss-plugin.yml`.

- [ ] **Step 2: Write the probe (plain Win32, no UE4SS types)**

```cpp
// mods/StrayDLSS/src/Probe.cpp — decides assessment §6.2: is start_mod() before or after
// the game's D3D12CreateDevice? Also whether a dummy device and a hidden-window swapchain
// are tolerated (the late-attach vtable discovery Task 13 would use).
#include "Probe.hpp"
#include "Log.hpp"
#include <MinHook.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>   // Microsoft::WRL::ComPtr
#include <windows.h>

namespace stray_dlss::probe {
namespace {
using PFN_D3D12CreateDevice = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
PFN_D3D12CreateDevice g_orig = nullptr;
ULONGLONG g_t0 = 0;

HRESULT WINAPI hook_create_device(IUnknown *adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void **out)
{
	const HRESULT hr = g_orig(adapter, fl, riid, out);
	SDS_LOG_WARN("probe: D3D12CreateDevice HOOK FIRED at t=%llu ms hr=0x%08lx device=%p",
		GetTickCount64() - g_t0, static_cast<unsigned long>(hr), out ? *out : nullptr);
	return hr;
}
}

void Start()
{
	g_t0 = GetTickCount64();
	const HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
	const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
	SDS_LOG_WARN("probe: start_mod at t=0 ms; d3d12.dll loaded=%d dxgi.dll loaded=%d",
		d3d12 != nullptr, dxgi != nullptr);
	if (MH_Initialize() != MH_OK) { SDS_LOG_ERROR("probe: MH_Initialize failed"); return; }
	void *target = d3d12 ? reinterpret_cast<void *>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
	if (target == nullptr) { SDS_LOG_ERROR("probe: D3D12CreateDevice not resolvable yet"); return; }
	if (MH_CreateHook(target, reinterpret_cast<void *>(&hook_create_device),
			reinterpret_cast<void **>(&g_orig)) != MH_OK || MH_EnableHook(target) != MH_OK)
	{
		SDS_LOG_ERROR("probe: MH_CreateHook/EnableHook failed");
		return;
	}
	SDS_LOG_INFO("probe: D3D12CreateDevice hooked; if the game already has a device this never fires");
}

// Called from on_update ~200 Hz; runs the dummy-object probe ONCE, 30 s in, so it lands in
// gameplay where a focus steal would be visible (CLAUDE.md §6: a Wine window can freeze the
// game under gamescope; this is the test for exactly that).
void Tick()
{
	static bool done = false;
	if (done || GetTickCount64() - g_t0 < 30000) return;
	done = true;
	ComPtr<ID3D12Device> dev;
	HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
	SDS_LOG_WARN("probe: dummy D3D12CreateDevice hr=0x%08lx", static_cast<unsigned long>(hr));
	if (FAILED(hr)) return;
	// Hidden top-level window, never shown. Whether Wine maps an X window for it — and
	// whether gamescope then focuses it — is UNCONFIRMED; that is what this measures.
	WNDCLASSW wc = {}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"StrayDLSSProbe"; RegisterClassW(&wc);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
	ComPtr<IDXGIFactory4> factory; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
	D3D12_COMMAND_QUEUE_DESC qd = {}; ComPtr<ID3D12CommandQueue> q; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
	DXGI_SWAP_CHAIN_DESC1 sd = {}; sd.BufferCount = 2; sd.Width = 8; sd.Height = 8;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	ComPtr<IDXGISwapChain1> sc;
	hr = factory ? factory->CreateSwapChainForHwnd(q.Get(), hwnd, &sd, nullptr, nullptr, &sc) : E_FAIL;
	SDS_LOG_WARN("probe: hidden window + dummy swapchain: hr=0x%08lx swapchain_vtable=%p",
		static_cast<unsigned long>(hr), sc ? *reinterpret_cast<void **>(sc.Get()) : nullptr);
	sc.Reset(); q.Reset(); DestroyWindow(hwnd); dev.Reset();
	SDS_LOG_WARN("probe: dummy objects released");
}
} // namespace
```

`Mod.cpp` is the DualSense `Mod.cpp` with the hook table removed: constructor sets `ModName = STR("StrayDLSS")`, opens the log at `<GameBinariesDir>/stray-dlss-plugin.log`, calls `probe::Start()`; `on_update` calls `probe::Tick()` and drains the log mirror. Exports `start_mod`/`uninstall_mod` unchanged.

- [ ] **Step 3: Build in CI, confirm exports and `/MD`**

Run: push; workflow `StrayDLSS plugin`. Expected: `start_mod`, `uninstall_mod` exported; `MSVCP140.dll` and `UE4SS.dll` in `/DEPENDENTS`.

- [ ] **Step 4: Install on the box and read three lines**

`ue4ss/Mods/StrayDLSS/dlls/main.dll`, `StrayDLSS : 1` in `mods.txt` above Keybinds. Launch options must **merge**: `WINEDLLOVERRIDES="dwmapi,dxgi=n,b"` (assessment §0; the current line is `WINEDLLOVERRIDES="dxgi=n,b" %command% -dx12`, CLAUDE.md §5). Read `stray-dlss-plugin.log`:

| Line | Consequence |
|---|---|
| `HOOK FIRED` | early attach works: Task 13 uses the MinHook export path and needs **no** dummy objects |
| never fires, but `d3d12.dll loaded=1` at start | device pre-existed: Task 13 uses late attach; the `unknown-slot` counter (Task 15) decides whether the shadow is complete |
| dummy swapchain `hr=0` and the game stayed responsive (user confirms) | late attach may discover vtables from a dummy swapchain |
| game froze at ~30 s | late attach must not create a window; Present hooking needs the early path or the proxy-DLL loader |

Record the outcome in `docs/STRAY-RENDERING-FACTS.md` ("UE4SS attach timing") and in this plan's Task 13 header.

- [ ] **Step 5: Commit**

```bash
git add mods/StrayDLSS third_party/minhook .github/workflows/dlss-plugin.yml docs/STRAY-RENDERING-FACTS.md
git commit -m "feat(plugin): StrayDLSS UE4SS skeleton with the attach-timing probe"
```

---

## Stage 1 — The seam, ReShade the only backend, zero behaviour change (5 tasks)

### Task 3: Move the ReShade headers behind one directory

**Files:**
- Move: `src/reshade_all.hpp` → `src/backend_reshade/reshade_all.hpp`
- Modify: `CMakeLists.txt` — `reshade_sdk`, `reshade_utils`, `imgui_headers` linked to the `stray-dlss` target only via a new `stray_dlss_backend_reshade` object library

- [ ] **Step 1: Add a CI guard that fails if any file outside `src/backend_reshade/` includes ReShade**

Add to `.github/workflows/ci.yml` `tests-linux` job, before Configure:

```yaml
      - name: No ReShade include outside the ReShade backend
        run: |
          set -e
          bad=$(grep -rln --include='*.cpp' --include='*.hpp' -E '#include *[<"](reshade|imgui|reshade_all|descriptor_tracking|state_tracking)' src | grep -v '^src/backend_reshade/' || true)
          if [ -n "$bad" ]; then echo "ReShade leaked outside src/backend_reshade/:"; echo "$bad"; exit 1; fi
```

- [ ] **Step 2: Run it and watch it fail** (every file that includes `reshade_all.hpp` is listed). Expected: FAIL listing `src/addon.cpp src/frame_state.hpp src/taa_hook.hpp src/nr_hook.hpp src/nr_history.hpp src/pass_finder.hpp src/gbuffer_finder.hpp src/log.cpp src/shader_dump.cpp`.

This failing check is the to-do list for Tasks 4–7. It stays red until Task 7 and must be green at the end of Stage 1.

- [ ] **Step 3: Commit the guard (red is expected; mark the job `continue-on-error: true` until Task 7 removes that flag)**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: guard that ReShade headers stay inside src/backend_reshade (red until stage 1 lands)"
```

### Task 4: The config seam — `host::cfg`, lifted from the DualSense INI

**Files:**
- Create: `src/host/config.hpp`, `src/host/config.cpp`, `src/host/ini.hpp`, `src/host/ini.cpp`
- Create: `src/backend_reshade/reshade_config.cpp`
- Create: `tests/test_ini.cpp`; Modify: `CMakeLists.txt` (add `src/host/ini.cpp` to `stray_dlss_core`, add the test)
- Modify: every `reshade::get_config_value(nullptr, "STRAYDLSS", ...)` site (74 in `addon.cpp`, 1 in `shader_dump.cpp:33`)

**Interfaces:**
- Produces:
  ```cpp
  namespace stray_dlss::host::cfg {
  struct Source { virtual ~Source() = default;
                  virtual bool get(const char *key, char *buf, std::size_t *size) = 0; };  // section is always STRAYDLSS
  void set_source(Source *);
  bool  get_bool (const char *key, bool  fallback);
  int   get_int  (const char *key, int   fallback);
  float get_float(const char *key, float fallback);
  // Copies at most `size-1` chars, NUL-terminates, returns true if the key existed.
  bool  get_string(const char *key, char *buf, std::size_t size);
  }
  namespace stray_dlss::host {
  class IniFile {                      // lifted from mods/StrayDualSense/src/Config.cpp: Trim,
  public:                              // Lower, ParseBool, ParseFloat, FileWriteTime, mtime reload
      bool load(const std::string &utf8_path);
      bool reload_if_changed();        // true when re-read
      bool get(const char *section, const char *key, std::string &out) const;
      void set_for_test(const char *section, const char *key, const char *value);
  };
  }
  ```

- [ ] **Step 1: Write the failing INI test**

```cpp
// tests/test_ini.cpp
#include "host/ini.hpp"
#include <doctest/doctest.h>
#include <cstdio>

TEST_CASE("IniFile parses sections, comments, and keeps unknown keys")
{
	const char *path = "test_ini_tmp.ini";
	std::FILE *f = std::fopen(path, "w");
	std::fputs("; comment\n[STRAYDLSS]\nNgxRR = 2\nNgxNRHook=preui\nHashShaders=false\n[OTHER]\nNgxRR=9\n", f);
	std::fclose(f);

	stray_dlss::host::IniFile ini;
	REQUIRE(ini.load(path));
	std::string v;
	CHECK(ini.get("STRAYDLSS", "NgxRR", v)); CHECK(v == "2");
	CHECK(ini.get("STRAYDLSS", "NgxNRHook", v)); CHECK(v == "preui");
	CHECK(ini.get("STRAYDLSS", "HashShaders", v)); CHECK(v == "false");
	CHECK_FALSE(ini.get("STRAYDLSS", "Missing", v));
	CHECK(ini.get("OTHER", "NgxRR", v)); CHECK(v == "9");
	std::remove(path);
}

TEST_CASE("cfg getters coerce like ReShade did: bool accepts 0/1/true/false, int via strtol, float via strtod")
{
	struct Src : stray_dlss::host::cfg::Source {
		bool get(const char *key, char *buf, std::size_t *size) override {
			std::string k = key, v;
			if (k == "B") v = "true"; else if (k == "I") v = "11"; else if (k == "F") v = "0.056"; else return false;
			if (*size <= v.size()) { *size = v.size() + 1; return false; }
			std::snprintf(buf, *size, "%s", v.c_str()); *size = v.size() + 1; return true;
		}
	} src;
	stray_dlss::host::cfg::set_source(&src);
	CHECK(stray_dlss::host::cfg::get_bool("B", false) == true);
	CHECK(stray_dlss::host::cfg::get_int("I", 0) == 11);
	CHECK(stray_dlss::host::cfg::get_float("F", 1.0f) == doctest::Approx(0.056f));
	CHECK(stray_dlss::host::cfg::get_int("nope", 7) == 7);
	char buf[8]; CHECK_FALSE(stray_dlss::host::cfg::get_string("nope", buf, sizeof buf));
	stray_dlss::host::cfg::set_source(nullptr);
}
```

- [ ] **Step 2: Run it, expect a compile failure** (`host/ini.hpp` missing). Run: `cmake --build build && ctest --test-dir build -R core`.

- [ ] **Step 3: Implement `ini.cpp` by lifting `mods/StrayDualSense/src/Config.cpp:15-75`** (`Trim`, `Lower`, `ParseBool`, `ParseFloat`, `FileWriteTime`) into `host::IniFile`; `Load` becomes a `[section]`-aware line parser storing `std::map<std::string, std::map<std::string, std::string>>`. Keep the mtime reload exactly as `Config::ReloadIfChanged` does it. Implement `cfg::get_*` in `config.cpp` as thin coercions over `Source::get` with a 256-byte stack buffer.

- [ ] **Step 4: Implement the ReShade source**

```cpp
// src/backend_reshade/reshade_config.cpp
#include "reshade_all.hpp"
#include "host/config.hpp"
namespace stray_dlss::rsb {
struct ReshadeConfigSource final : host::cfg::Source {
	bool get(const char *key, char *buf, std::size_t *size) override {
		// The char overload. NOTE the documented trap (CLAUDE.md §5): comma lists come back as
		// element 0 only — unchanged here; lists still go through sidecar files.
		return reshade::get_config_value(nullptr, "STRAYDLSS", key, buf, size);
	}
};
host::cfg::Source *reshade_config_source() { static ReshadeConfigSource s; return &s; }
}
```

- [ ] **Step 5: Replace the 75 call sites mechanically**, e.g. `reshade::get_config_value(nullptr, "STRAYDLSS", "NgxRR", ngx_rr)` → `ngx_rr = host::cfg::get_int("NgxRR", ngx_rr)`; the two char-buffer sites (`NgxNRHook`, `NgxSnippetPath`) → `get_string`. Install the source in `DllMain` **before** any read: `host::cfg::set_source(rsb::reshade_config_source())`.

- [ ] **Step 6: Tests green on Linux and Windows; the add-on's startup log must print the same key values as before** (compare against the previous artifact's log on the box — same session, same ini).

- [ ] **Step 7: Commit**

```bash
git add src/host src/backend_reshade/reshade_config.cpp tests/test_ini.cpp CMakeLists.txt src/addon.cpp src/shader_dump.cpp
git commit -m "refactor: config seam (host::cfg) with the ReShade ini as the first source"
```

### Task 5: The log seam

**Files:**
- Modify: `src/log.hpp`, `src/log.cpp:72`
- Create: `src/backend_reshade/reshade_log.cpp`

**Interfaces:**
- Produces: `void stray_dlss::log::set_external_sink(void (*sink)(Level, const char *))` replacing `enable_reshade_sink()`.

- [ ] **Step 1: Replace `enable_reshade_sink()` with `set_external_sink(fn)`**; `write()` calls the sink if non-null after the file write. `reshade_log.cpp` provides `void reshade_sink(Level l, const char *m) { reshade::log::message(static_cast<reshade::log::level>(l), m); }` and `addon_entry` installs it after `register_addon` succeeds — exactly where `enable_reshade_sink()` was called.
- [ ] **Step 2: `tests/warp/log_stub.cpp` gains the same symbol** (a no-op sink). Build all lanes. Expected: green; `stray-dlss.log` and `ReShade.log` both still carry the banner (the real-ReShade lane greps `register_addon succeeded`).
- [ ] **Step 3: Commit** — `git commit -m "refactor: log sink is installable; ReShade's is one implementation"`

### Task 6: The interception types and `Backend`/`Sink` interfaces

**Files:**
- Create: `src/intercept/types.hpp`, `src/intercept/backend.hpp`, `src/intercept/backend.cpp` (the `backend()`/`set_backend()` singleton + a null backend that logs once and refuses everything)
- Create: `tests/test_intercept_types.cpp` (Linux: the types compile without `d3d12.h` and `DispatchBindings` is layout-equivalent to today's)
- Modify: `CMakeLists.txt` — `src/intercept/*` in `stray_dlss_core`

- [ ] **Step 1: Write the failing test** — `DispatchBindings b; b.srvs.push_back(BoundTexture{ 2, 0x1234, TexFormat::r32_float_x8x24, 1920, 1080, 0xabc, false }); CHECK(b.srvs[0].slot == 2);` plus `static_assert(std::is_abstract_v<icept::Backend>)` and a `NullBackend` behaviour test: `resolve_compute_bindings` returns false; `is_resource_live(0)` false.
- [ ] **Step 2: Fail (header missing). Step 3: Implement exactly the A.2 listing.** `types.hpp` includes `core/taa_signature.hpp` for `BoundTexture`/`TexFormat` and forward-declares the four `ID3D12*`/`IDXGISwapChain3` structs at global scope (the `struct ID3D12DescriptorHeap;` global-scope trap is documented in `frame_state.hpp:19-21` — keep that comment).
- [ ] **Step 4: Green on Linux. Commit** — `git commit -m "feat(intercept): framework-free seam types, Backend and Sink"`

### Task 7: Put the ReShade add-on behind the seam

**Files:**
- Create: `src/backend_reshade/reshade_backend.{hpp,cpp}` (implements `icept::Backend`; owns the `reshade::api::command_list*` ↔ `CommandContext` map keyed by `get_native()`)
- Move: `src/frame_state.{hpp,cpp}` → `src/backend_reshade/reshade_bindings.{hpp,cpp}` (unchanged bodies; the public functions become `ReshadeBackend` methods; `DispatchBindings`/`to_tex_format`/`format_name` move to `intercept/types.hpp` + `core/`)
- Create: `src/app/dlss_app.{hpp,cpp}` — `class DlssApp final : icept::Sink`; the bodies of `on_init_device`, `on_init_pipeline`, `on_bind_pipeline`, `on_dispatch`, `on_present`, `on_pf_*`, `on_nr_*` move here **verbatim** with ReShade types replaced by `CommandContext`
- Create: `src/backend_reshade/addon_entry.cpp` — `DllMain`, `register_addon`, `register_events()` (the 26 registrations, each a 3-line lambda translating ReShade args into `Sink` calls), `unregister_events()`, both overlays and their ImGui bodies (they read `DlssApp` state through a small `app::Status` struct)
- Modify: `src/taa_hook.{hpp,cpp}`, `src/nr_hook.{hpp,cpp}`, `src/nr_history.{hpp,cpp}`, `src/pass_finder.{hpp,cpp}`, `src/gbuffer_finder.{hpp,cpp}` — retype; `read_view_cb` becomes:

```cpp
bool read_view_cb(const icept::BufferRange &cb, ue4::ViewParams &out)
{
	icept::ResourceInfo ri{};
	if (!icept::backend()->describe_resource(cb.buffer, ri) || !ri.is_buffer || !ri.upload_heap)
		return false;
	// The bounds check is NOT a formality — its absence was an access violation (taa_hook.cpp).
	if (cb.size != icept::kUnknownSize && cb.size < ue4::kViewPrefixBytes) return false;
	if (ri.buffer_size < cb.offset + ue4::kViewPrefixBytes) return false;
	unsigned char copy[ue4::kViewPrefixBytes];
	if (!icept::backend()->read_buffer(cb, sizeof(copy), copy)) return false;
	return ue4::parse_view_params(copy, sizeof(copy), out);
}
```
- Modify: `tests/warp/fake_reshade_command_list.hpp` + `warp_mv_resolve.cpp` — the restore test (`:1237`) constructs a `ReshadeBackend` around the fake list and calls `restore_game_compute_state(ctx)`; the ReShade-side assertions are unchanged
- Modify: `CMakeLists.txt` — `stray-dlss` = `app/*` + `backend_reshade/*` + everything else; remove `continue-on-error` from Task 3's guard

**Interfaces:**
- Consumes: Task 6's `Backend`/`Sink`, Task 4's `cfg`, Task 5's log sink.
- Produces: `app::DlssApp &app::instance()`; `rsb::ReshadeBackend`.

- [ ] **Step 1: Move `frame_state` first, compile, run the WARP restore test** (it links `frame_state.cpp` directly — update the path). Expected: green, nothing else changed.
- [ ] **Step 2: Create `DlssApp` with every handler body moved, compile the add-on**. The ReShade `map_buffer_region` call now lives in `ReshadeBackend::read_buffer` (`device->map_buffer_region(cb.buffer, cb.offset, bytes, read_only, &p); memcpy; unmap`).
- [ ] **Step 3: `register_events()` in `addon_entry.cpp` — one lambda per event**, e.g.

```cpp
reshade::register_event<reshade::addon_event::dispatch>(
	[](reshade::api::command_list *cl, uint32_t x, uint32_t y, uint32_t z) -> bool {
		return app::instance().on_dispatch(rsb::backend().context_for(cl), x, y, z);
	});
```

`context_for` looks up (or creates) the `CommandContext` keyed by `cl->get_native()`; `init_command_list`/`reset_command_list` call `on_command_list_reset`; `destroy_command_list` erases it. **Register `init_command_list` from the very first frame** as today (CLAUDE.md §5: "attach late and there is no retroactive recovery").
- [ ] **Step 4: Task 3's guard goes green**; Linux, WARP and real-ReShade lanes green; artifact size within ±5%.
- [ ] **Step 5: Box verification — the only acceptance test for Stage 1.** Deploy the artifact; same ini; one session to gameplay. `diff` the new `stray-dlss.log` against the previous build's, ignoring timestamps and pointers: the banner, every `[STRAYDLSS]` key echo, `DLSS evaluate OK` cadence, the census line and the `NR HISTORY` counters must match. **Any new WARN/ERROR line is a Stage 1 defect.**
- [ ] **Step 6: Commit**

```bash
git add src tests CMakeLists.txt .github/workflows/ci.yml
git commit -m "refactor: ReShade add-on moved behind icept::Backend/Sink; DlssApp is framework-free"
```

---

## Stage 2 — The native backend as a differential observer (8 tasks)

Nothing in this stage changes the image. The ReShade backend drives; the native backend watches and disagrees out loud.

### Task 8: Build layout for the native backend (compiled twice: `/MT` and `/MD`)

**Files:**
- Modify: `CMakeLists.txt` — new function `stray_dlss_add_native_lib(name crt)` creating a STATIC lib from `src/backend_native/*.cpp` + `src/app/*.cpp` + `src/host/*.cpp` + the D3D12-only sources, with `MSVC_RUNTIME_LIBRARY` set per call; `stray_dlss_native_mt` links into `stray-dlss.addon64`; `stray_dlss_native_md` is what `mods/StrayDLSS` links
- Modify: `mods/StrayDLSS/CMakeLists.txt` — `add_subdirectory(${STRAY_DLSS_ROOT} stray_dlss_root)` with `-DSTRAY_DLSS_BUILD_ADDON=OFF -DSTRAY_DLSS_BUILD_NATIVE_MD=ON`; link `stray_dlss_native_md` + `nvsdk_ngx_d.lib`
- Modify: `.github/workflows/dlss-plugin.yml` — fetch `nvsdk_ngx_d.lib` (the `_d` variant — `/MD`), pass `-DSTRAY_DLSS_ROOT=<repo>`; add `_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR` to the mod target (the msvcp140 trap applies to the mod too, SOFT — UE4SS loads in that prefix, so its toolchain's CRT is present, but ours may be newer)

- [ ] **Step 1: Add the function and the two targets with an empty `src/backend_native/native_backend.cpp`** (a `NativeBackend` that refuses everything and logs `native backend: not installed`). Build both CI workflows. Expected: `stray-dlss.addon64` still `/MT`-only (existing `Verify no dynamic CRT dependency` step); `main.dll` imports `MSVCP140` (existing check).
- [ ] **Step 2: Add a link-time assertion that the two never mix:** in `stray_dlss_add_native_lib`, `target_compile_definitions(... STRAY_DLSS_CRT_${crt}=1)` and a `static_assert` in `native_backend.cpp` that exactly one is defined.
- [ ] **Step 3: Commit** — `git commit -m "build: native backend library, built once per CRT"`

### Task 9: `vtable_patch` — the whole hooking engine for COM slots

**Files:**
- Create: `src/backend_native/vtable_patch.{hpp,cpp}`
- Test: `tests/warp/warp_mv_resolve.cpp` — `test_vtable_patch_roundtrip`

**Interfaces:**
- Produces:
  ```cpp
  namespace stray_dlss::native {
  struct Patch { void **slot = nullptr; void *original = nullptr; void *replacement = nullptr; };
  // Installs `replacement` at `vtable[index]`; returns the original. Idempotent: a second call
  // with the same replacement returns the stored original without re-writing. NEVER restores.
  void *patch_slot(void *object, unsigned index, void *replacement, const char *name);
  unsigned patch_count();
  }
  ```

- [ ] **Step 1: Write the WARP test**

```cpp
namespace {
using PFN_GetDesc = D3D12_RESOURCE_DESC *(STDMETHODCALLTYPE *)(ID3D12Resource *, D3D12_RESOURCE_DESC *);
PFN_GetDesc g_orig_getdesc = nullptr;
std::atomic<int> g_getdesc_calls{ 0 };
D3D12_RESOURCE_DESC *STDMETHODCALLTYPE hooked_getdesc(ID3D12Resource *self, D3D12_RESOURCE_DESC *out)
{ ++g_getdesc_calls; return g_orig_getdesc(self, out); }
}
bool test_vtable_patch_roundtrip(Gpu &gpu)
{
	std::printf("\n[test] vtable_patch installs once and chains to the original\n");
	ComPtr<ID3D12Resource> a = create_buffer(gpu, 256), b = create_buffer(gpu, 256);
	// ID3D12Resource::GetDesc is slot 10 (IUnknown 0-2, ID3D12Object 3-6, ID3D12DeviceChild::GetDevice 7, Map 8, Unmap 9, GetDesc 10 — d3d12.h vtable order).
	g_orig_getdesc = reinterpret_cast<PFN_GetDesc>(
		stray_dlss::native::patch_slot(a.Get(), 10, reinterpret_cast<void *>(&hooked_getdesc), "ID3D12Resource::GetDesc"));
	void *again = stray_dlss::native::patch_slot(a.Get(), 10, reinterpret_cast<void *>(&hooked_getdesc), "ID3D12Resource::GetDesc");
	const D3D12_RESOURCE_DESC da = a->GetDesc(), db = b->GetDesc();
	std::printf("  calls=%d idempotent=%d desc_ok=%d shared_vtable_reached_b=%d\n",
		g_getdesc_calls.load(), again == reinterpret_cast<void *>(g_orig_getdesc), da.Width == 256 && db.Width == 256,
		g_getdesc_calls.load() == 2);
	return g_getdesc_calls.load() >= 1 && again == reinterpret_cast<void *>(g_orig_getdesc) && da.Width == 256;
}
```

- [ ] **Step 2: Fail (unresolved `patch_slot`). Step 3: Implement** — lift the `VirtualProtect(PAGE_READWRITE)` → write → restore sequence from `src/ext_unhook.cpp:105-118`; keep a `std::vector<Patch>` under a mutex; log `vtable_patch: %s slot %u %p -> %p` once per install.
- [ ] **Step 4: Green on WARP; run on the box via `run-harness-proton.sh`** — `shared_vtable_reached_b=1` there is the second confirmation of Task 1's `resource=1`.
- [ ] **Step 5: Commit** — `git commit -m "feat(native): vtable_patch, the slot-patch primitive"`

### Task 10: `resource_registry` — liveness, resource facts, GPU-VA → buffer

**Files:**
- Create: `src/backend_native/resource_registry.{hpp,cpp}`
- Create: `src/core/va_map.{hpp,cpp}` (pure: interval map `start → {size, id}` with `upper_bound` lookup — the same shape as ReShade's `_buffer_gpu_addresses`, `d3d12_impl_device.cpp:2211`, HARD)
- Test: `tests/test_va_map.cpp` (Linux), `tests/warp/warp_mv_resolve.cpp` — `test_registry_liveness`

**Interfaces:**
- Produces:
  ```cpp
  namespace stray_dlss::native::registry {
  void note_created(ID3D12Resource *);            // snapshots GetDesc + heap props + GPU VA (buffers), attaches the sentinel
  bool is_live(icept::ResourceId);
  bool describe(icept::ResourceId, icept::ResourceInfo &);
  bool buffer_for_va(std::uint64_t gpu_va, icept::BufferRange &);  // root CBV → buffer+offset
  void on_destroyed(icept::ResourceId);          // from the sentinel; erases every shadow slot (Task 11)
  }
  namespace stray_dlss::core { class VaMap { public: void insert(uint64_t start, uint64_t size, uint64_t id); void erase(uint64_t id);
                                            bool find(uint64_t va, uint64_t &id, uint64_t &offset) const; }; }
  ```

- [ ] **Step 1: Failing Linux test for `VaMap`**: insert `[0x1000,+0x800)` id 1 and `[0x4000,+0x100)` id 2; `find(0x1234)` → id 1 offset 0x234; `find(0x1800)` → false (end exclusive); `find(0x4000)` → id 2 offset 0; placed-resource overwrite: insert `[0x1000,+0x10)` id 3 replaces id 1 at that start (`insert_or_assign` semantics, as ReShade).
- [ ] **Step 2: Implement `VaMap` (std::map + upper_bound). Green.**
- [ ] **Step 3: Failing WARP test `test_registry_liveness`**: `note_created(buf)`; `is_live` true; `describe` reports `is_buffer`, `buffer_size==256`, `upload_heap` per the heap; `buffer_for_va(buf->GetGPUVirtualAddress()+16)` → offset 16; release the last ref → `is_live` false **without any explicit destroy call** (the sentinel from Task 1, whose WARP result was `fired=1`).
- [ ] **Step 4: Implement** — sentinel = Task 1's `Sentinel` with `on_destroyed(id)` in `Release()`; the registry never calls `GetDesc()` after creation (assessment §1.1 decision 2 — the direct fix for CLAUDE.md §5 hazard 2). **Fallback if the box measured `fired=0`:** patch `ID3D12Resource::Release` (slot 2) and treat a returned refcount of 0 as destruction — noted in the header, selected by `#if` on a build option set from the Task 1 result.
- [ ] **Step 5: Green. Commit** — `git commit -m "feat(native): resource registry — liveness sentinel, creation-time facts, GPU-VA map"`

### Task 11: `descriptor_shadow` — views, copies, heaps

**Files:**
- Create: `src/backend_native/descriptor_shadow.{hpp,cpp}`
- Create: `src/core/heap_math.{hpp,cpp}` (pure: `gpu_to_cpu(gpu, gpu_base, cpu_base, increment)`)
- Test: `tests/test_heap_math.cpp`, `tests/warp/warp_mv_resolve.cpp` — `test_descriptor_shadow_copy_chain`

**Interfaces:**
- Produces:
  ```cpp
  namespace stray_dlss::native::shadow {
  enum class ViewKind : std::uint8_t { srv, uav, cbv, rtv, dsv };
  struct ViewEntry { ViewKind kind; icept::ResourceId resource; TexFormat format; std::uint32_t width, height; bool is_3d; std::uint64_t buffer_offset, buffer_size; };
  void note_view(icept::DescriptorId cpu, const ViewEntry &);      // Create*View hooks
  void note_copy(icept::DescriptorId dst, icept::DescriptorId src); // CopyDescriptors(Simple), per descriptor
  void note_heap_bound(ID3D12DescriptorHeap *);                    // SetDescriptorHeaps: records cpu/gpu base + increment
  bool lookup(icept::DescriptorId cpu, ViewEntry &);
  bool gpu_to_cpu(std::uint64_t gpu, icept::DescriptorId &cpu);    // via the bound heaps
  void forget_resource(icept::ResourceId);                         // called by registry::on_destroyed
  std::uint64_t unknown_lookups();                                 // the §6.2 counter
  }
  ```

- [ ] **Step 1: Failing Linux test for `heap_math`**: `gpu_to_cpu(0x9000 + 3*32, 0x9000, 0x100, 32) == 0x100 + 3*32`; out-of-range GPU handle → false.
- [ ] **Step 2: Implement (two lines). Green.**
- [ ] **Step 3: Failing WARP test `test_descriptor_shadow_copy_chain`**: with the hooks from Task 12 *not yet installed*, drive the shadow API directly: create an offline heap (`D3D12_DESCRIPTOR_HEAP_FLAG_NONE`) and a shader-visible heap; `CreateShaderResourceView(tex, ..., offline[0])` and call `note_view(offline[0].ptr, {srv, tex, r16g16b16a16_float, 64, 64})`; `CopyDescriptorsSimple(1, online[5], offline[0], CBV_SRV_UAV)` and `note_copy(online[5].ptr, offline[0].ptr)`; `note_heap_bound(online)`; then `gpu_to_cpu(online_gpu_base + 5*inc)` → `online[5].ptr` and `lookup` → the SRV entry. Release `tex` → after `registry::on_destroyed`, `lookup` is false and `unknown_lookups()` incremented.
- [ ] **Step 4: Implement** — `std::unordered_map<DescriptorId, ViewEntry>` + reverse index `resource → vector<DescriptorId>` for `forget_resource`; a small vector of bound heaps `{heap, cpu_base, gpu_base, increment, count}` (from `GetCPUDescriptorHandleForHeapStart`, `GetGPUDescriptorHandleForHeapStart`, `GetDescriptorHandleIncrementSize`, `GetDesc().NumDescriptors` — all HARD D3D12 semantics). Keying by raw `ptr` is SOFT on handle uniqueness across heaps; log a WARN if `note_view` ever sees the same `ptr` with a different heap identity.
- [ ] **Step 5: Green. Commit** — `git commit -m "feat(native): descriptor shadow keyed by real CPU handle"`

### Task 12: `d3d12_hooks` — device and command-list slots, root shadow, dispatch, guards

**Files:**
- Create: `src/backend_native/d3d12_hooks.{hpp,cpp}` (device + list + queue slots), `src/backend_native/root_shadow.{hpp,cpp}` (per-list state), `src/core/root_signature_walk.{hpp,cpp}` (pure: walks a plain `RootParam[]` into `(register, kind, table range)` — Linux-testable; the D3D12 deserializer output is converted into it in `d3d12_hooks.cpp`)
- Modify: `src/backend_native/native_backend.cpp` — implements `icept::Backend` over registry + shadow + root_shadow; `restore_game_compute_state` builds a `NativeComputeState` (`src/d3d12_restore.hpp`) from the root shadow and calls the existing `restore_native_compute_state`
- Test: `tests/test_root_signature_walk.cpp`, `tests/warp/warp_mv_resolve.cpp` — `test_native_hooks_ue4_shaped_frame`, `test_native_dispatch_suppression`, `test_native_hooks_ignore_our_own_lists`

**Slot table (HARD from `d3d12.h` vtable order; verified by the test's first call landing in the right hook — a wrong index would show as a crash in CI, not on the box):**

| Interface | Methods hooked |
|---|---|
| `ID3D12Device` | `CreateCommittedResource`, `CreatePlacedResource`, `CreateReservedResource` (→ `registry::note_created`), `CreateShaderResourceView`, `CreateUnorderedAccessView`, `CreateConstantBufferView`, `CreateRenderTargetView`, `CreateDepthStencilView`, `CopyDescriptors`, `CopyDescriptorsSimple`, `CreateRootSignature` (deserialize with `D3D12CreateVersionedRootSignatureDeserializer`, store per `ID3D12RootSignature*`), `CreateComputePipelineState`, `CreateGraphicsPipelineState` (hash `CS.pShaderBytecode` **then call the original with the caller's unmodified desc**), `CreateCommandList` (register the list), `CreateCommandQueue` |
| `ID3D12Device2` | `CreatePipelineState` (walk the subobject stream for the CS) |
| `ID3D12GraphicsCommandList` | `Reset`, `Close`, `SetDescriptorHeaps`, `SetPipelineState`, `SetComputeRootSignature`, `SetComputeRootDescriptorTable`, `SetComputeRootConstantBufferView/ShaderResourceView/UnorderedAccessView`, `SetComputeRoot32BitConstant(s)`, the four `SetGraphicsRoot*` (tables + CBV only — the finders need SRVs by table), `OMSetRenderTargets`, `RSSetViewports`, `RSSetScissorRects`, `Dispatch`, `DrawInstanced`, `DrawIndexedInstanced`, `CopyResource`, `CopyTextureRegion`, `ResolveSubresource` |
| `ID3D12GraphicsCommandList4` | `BeginRenderPass` |
| `ID3D12CommandQueue` | `ExecuteCommandLists` (→ `Sink::on_execute`; also records "the queue this list executed on") |

**Guards (all three from the first build — assessment §8.3):**
- `thread_local int t_in_our_code` incremented by `DlssApp` around every native recording of ours (resolve, NGX evaluate, codec passes, history restore); hooks pass straight through when non-zero.
- A list is *tracked* only after `SetPipelineState` binds a PSO we hashed on the game's device; untracked lists (our own, ReShade's immediate list, OptiScaler's) pass straight through.
- A device is *the game's* if it is the first device on which a compute PSO was created; `note_created` etc. ignore others.

- [ ] **Step 1: Failing Linux test for the walk**: a layout `[0]=table{SRV t0-5}, [1]=root CBV b1, [2]=table{UAV u0-1}, [3]=32-bit constants b0×4` walks to exactly `{t0..t5 → param 0 offsets 0..5}`, `{b1 → root cbv param 1}`, `{u0,u1 → param 2}`, and `[3]` is reported as constants. An unbounded range (`count == UINT32_MAX`) is reported as `unbounded` and not expanded (`frame_state.cpp:512-513` behaviour).
- [ ] **Step 2: Implement the walk. Green.**
- [ ] **Step 3: Failing WARP test `test_native_hooks_ue4_shaped_frame`**: install the hooks on the WARP device (`native::install(device)`); then, through the *public* D3D12 API only: create a root signature shaped like UE4's compute layout (param 0 table `SRV t0-t5`, param 1 root CBV, param 2 table `UAV u0`), create SRVs for a depth `R32G8X24_TYPELESS` texture as `R32_FLOAT_X8X24` **and** `X32_TYPELESS_G8X24_UINT` into an offline heap, a velocity `R16G16B16A16_UNORM`, a colour `R16G16B16A16_FLOAT`, a history; `CopyDescriptors` into the online heap; bind heaps/root sig/tables/root CBV (an upload buffer of 2448 bytes with a fabricated View prefix whose row 135 is `(denormal, 0.452, 1/0.452, 0)`); `SetPipelineState(warp_probe PSO)`; `Dispatch(320,180,1)`. A test `Sink` records what `on_dispatch` received and calls `backend()->resolve_compute_bindings`. Assert: `srvs` has t2 and t4 with the **same** `resource` and formats `r32_float_x8x24` / `x32_g8x24_uint` (CLAUDE.md §2.3's discriminator), `uavs[0].slot==0`, `constant_buffers` holds `(1, {buffer, 0, kUnknownSize})`, `read_buffer` returns the 2448 bytes and `ue4::parse_view_params` accepts them with `pre_exposure*one_over_pre_exposure == 1.0`. **Then release the depth texture and dispatch again**: t2/t4 must be absent and `unknown_lookups()` must have grown by 2 — that is the liveness contract working through the whole chain. Debug layer: zero messages.
- [ ] **Step 4: Failing WARP test `test_native_dispatch_suppression`**: the `Sink` returns `true` from `on_dispatch`; a UAV cleared to 0 that the probe shader would write stays 0 after execution; returning `false` → written. `test_native_hooks_ignore_our_own_lists`: a second list that never binds a hashed PSO records a `Dispatch` and the `Sink` is **not** called.
- [ ] **Step 5: Implement `d3d12_hooks.cpp` and `root_shadow.cpp`.** Per-list record: `{root_sig, tables[param] (gpu handle), root_cbv[param] (va), root_srv/uav, constants[param], pso, heaps[2], rtvs, dsv, viewports, scissors, tracked}`; `Reset` clears it (this is the `on_command_list_reset` event). `resolve_compute_bindings` = walk the root signature; for each table param, `gpu_to_cpu` then `lookup` per range slot; for each root CBV, `registry::buffer_for_va`. `is_hdr_colour` etc. unchanged. **Every failed lookup increments `unknown_lookups()`.**
- [ ] **Step 6: All three green with the debug layer clean. Run on the box via `run-harness-proton.sh`** (the hardware lane cannot validate, but it can crash — that is the point). **Commit** — `git commit -m "feat(native): D3D12 device/list/queue hooks, root shadow, dispatch suppression"`

### Task 13: `attach` — early (export detours) and late (existing-object discovery)

**Files:**
- Create: `src/backend_native/attach.{hpp,cpp}`, `src/backend_native/dxgi_hooks.{hpp,cpp}`
- Test: `tests/warp/warp_mv_resolve.cpp` — `test_attach_early_catches_device` (hook `D3D12CreateDevice` with MinHook, create a device, assert `native::game_device()` is it)

**Interfaces:**
- Produces:
  ```cpp
  namespace stray_dlss::native {
  enum class AttachMode { early, late };
  // early: MinHook on D3D12CreateDevice + CreateDXGIFactory1/2 → hooks installed inside the
  //        first successful creation, swapchain slots patched at IDXGIFactory2::CreateSwapChainForHwnd.
  // late:  the device is discovered from the first hooked CreateComputePipelineState /
  //        ExecuteCommandLists call after slot patches are installed via a THROWAWAY device
  //        (Task 2 measured whether that is tolerated). The swapchain is discovered ONLY if
  //        Task 2 measured the hidden-window probe as harmless; otherwise Present hooking is
  //        deferred to Stage 4's proxy-DLL loader and `on_present` is driven by the ReShade
  //        backend while it exists.
  bool attach(AttachMode);
  ID3D12Device *game_device();
  const char *attach_report();   // one line for the log: mode, what was found, what was not
  }
  ```

Decision rule, filled in from Task 2's log: `HOOK FIRED` → `early` is the default; otherwise `late`.

- [ ] **Step 1: Failing WARP test** (early path on the harness: `attach(early)` before `D3D12CreateDevice`; assert `game_device()` equals the created device and that `patch_count() > 0`).
- [ ] **Step 2: Implement early** — three `MH_CreateHook`s; inside the `D3D12CreateDevice` hook call the original, then `install(device)`; inside `CreateSwapChainForHwnd`/`CreateSwapChain` hooks call the original then `dxgi::install(swapchain)` (patches `Present`, `Present1`, `ResizeBuffers`; records the `ID3D12CommandQueue*` the swapchain was created with — that argument is the queue, HARD from the DXGI signature). **Late**: `D3D12CreateDevice` on a throwaway device → `install(dummy)` (slot patches are process-wide by Task 1's measurement) → release it; `game_device()` resolves on the first tracked call.
- [ ] **Step 3: Green. Commit** — `git commit -m "feat(native): early and late attach"`

### Task 14: CI — the native-hooks-on-WARP lane

**Files:**
- Modify: `.github/workflows/ci.yml` — the existing `WARP harness` step already runs the whole binary; add a *second* invocation `stray_dlss_warp.exe --native-hooks` that installs the native backend first and re-runs every existing resolve/restore test through it (the tests call `icept::backend()`; only the backend differs)
- Modify: `tests/warp/warp_mv_resolve.cpp:1504` — parse `--native-hooks`

- [ ] **Step 1: Add the flag and the second invocation. Expected: every existing test passes under both backends; the debug layer reports nothing under `--native-hooks`.**
- [ ] **Step 2: Commit** — `git commit -m "ci: run the WARP harness through the native backend as well"`

### Task 15: The differential observer

**Files:**
- Create: `src/app/diff_observer.{hpp,cpp}`
- Modify: `src/app/dlss_app.cpp` — in `on_dispatch`, after the driver's resolve, `diff::publish_expected(ctx.native, bindings)`; the native backend, when configured `NativeMode=observe`, calls `diff::compare(ctx.native, native_bindings)` from its `Dispatch` hook (which fires *inside* ReShade's forward of the same call, so correlation is by native list pointer + nesting, exact — SOFT until the CI lane in Step 3 confirms it)
- Modify: `.github/workflows/ci.yml` — the real-ReShade lane also runs `--native-hooks --expect-reshade` and **requires** `diff: disagreements=0` in the harness output
- Modify: `src/addon_entry.cpp` — `[STRAYDLSS] NativeMode = off | observe` (default `off`); `observe` calls `native::attach(early)` from `DllMain` (in the add-on, early is always available: `DllMain` runs before the game creates its device, which is how ReShade itself attaches — HARD, `ci.yml` comment on `d3d12.cpp:40`)

**Interfaces:**
- Produces: log lines `DIFF frame=%llu hash=%016llx ok` / `DIFF ... MISMATCH srv t%u: reshade=(res %p fmt %s %ux%u) native=(...)` / `DIFF ... native UNKNOWN slot t%u`, a periodic `DIFF SUMMARY dispatches=%llu agree=%llu mismatch=%llu unknown=%llu` every 600 presents, and the same four numbers in `stray-dlss-status.txt`.

- [ ] **Step 1: Failing Linux test** for the pure comparison: two `DispatchBindings` that differ only in `srvs[1].format` produce exactly one mismatch naming `t1`; a missing entry on the native side is reported as `unknown`, not `mismatch`; identical inputs report `agree`.
- [ ] **Step 2: Implement `compare` (pure) and the thread-local expected slot.** Green.
- [ ] **Step 3: CI — real-ReShade lane with both backends.** The harness under ReShade's proxy now has ReShade resolving through its trackers and the native backend resolving beneath the proxy (real handles, after conversion). Expected output: `diff: disagreements=0` for the UE4-shaped frame from Task 12. **This is the differential machinery validating itself before it ever sees the game.**
- [ ] **Step 4: Box run — Stage 2's acceptance test.** `NativeMode=observe`, one full session menu → gameplay → 10 minutes. Acceptance: `DIFF SUMMARY ... mismatch=0 unknown=0` over the last 5,000 dispatches, **and** zero `unknown` at every dispatch whose hash is in `core/taa_hashes.hpp`. Any `unknown` in the first minute that clears later is the late-attach warmup (assessment §6.2) and is logged as such; `unknown` that persists into gameplay selects the early attach path (or the proxy loader) — do not proceed to Stage 3 with it.
- [ ] **Step 5: Commit** — `git commit -m "feat: differential observer — native backend diffs every dispatch against ReShade"`

### Task 16: The UE4SS mod loads the native backend in observer mode

**Files:**
- Modify: `mods/StrayDLSS/src/Mod.cpp` — replace the probe: constructor opens the log, reads `StrayDLSS.ini` (`host::IniFile` as the `cfg::Source`), calls `native::attach(<mode from Task 2>)`, `app::instance().start()`; `on_update` only drains the log mirror and polls `ini.reload_if_changed()`
- Modify: `mods/StrayDLSS/StrayDLSS.ini` — every `[STRAYDLSS]` key with today's defaults, `NativeMode=observe`
- Modify: `.github/workflows/dlss-plugin.yml` — stage `StrayDLSS.ini` beside `main.dll`

The observer needs *some* driver to publish expectations; with ReShade still loaded as `dxgi.dll` and `stray-dlss.addon64` still installed, the add-on drives and the mod's native backend observes. **Two native backends in one process** (the add-on's `NativeMode=observe` and the mod's) would double-patch: the ini for this run sets the add-on to `NativeMode=off` and the mod to `observe`. Log the process-wide `patch_count()` and refuse to install if another `StrayDLSS` patch marker is already present in the slot (a marker page tag on our replacement functions).

- [ ] **Step 1: Build; CI checks as Task 2.**
- [ ] **Step 2: Box run: same acceptance as Task 15 Step 4, from the mod.** This answers assessment §6.2 for real: the `unknown` counter from a UE4SS-loaded backend.
- [ ] **Step 3: Commit** — `git commit -m "feat(plugin): StrayDLSS loads the native backend as a differential observer"`

---

## Stage 3 — The native backend drives the TAA hook; ReShade stays loaded (1 task)

### Task 17: `NativeMode=drive`

**Files:**
- Modify: `src/app/dlss_app.cpp` — `set_backend(native)` when `NativeMode=drive`; the ReShade backend then registers **only** `init_command_list`, `present` and the overlays (so ReShade's frame boundary, screenshot and OSD keep working) and its `dispatch` lambda returns `false` unconditionally
- Modify: `src/backend_native/d3d12_hooks.cpp` — in `drive` mode the `Dispatch` hook calls `Sink::on_dispatch` and **does not call the original** when it returns true
- `src/ext_unhook` **stays ON** — ReShade is still in the process (assessment §3.1 caveat)

**Interfaces:**
- Consumes: everything above. `restore_game_compute_state` now replays the root shadow — including root CBVs and 32-bit constants that ReShade's `state_block` never had (assessment §3.3).

- [ ] **Step 1: WARP test `test_drive_mode_restore_is_complete`**: bind a root CBV + constants + tables, run a fake "our pass" that clobbers root sig/PSO/heaps, `restore_game_compute_state`, then dispatch the probe shader and assert its output matches the un-clobbered run (the existing `test_restore_preserves_game_state` pattern at `:1115`, now through the native backend).
- [ ] **Step 2: Green. Box run — Stage 3 acceptance:** `NativeMode=drive`. Required: `DLSS evaluate OK` cadence ≥ 99% of matched dispatches (the camera-cut rule, CLAUDE.md §5), the DLSS indicator present, `dmesg` clean, `PerfLog` fps within 5% of the Stage 1 baseline, a ReShade screenshot (`tools/screenshot-stray.sh`) judged by the user against a Stage 1 screenshot of the same spot. **Rollback is `NativeMode=off` in the ini.**
- [ ] **Step 3: Commit** — `git commit -m "feat: native backend drives the TAA hook (NativeMode=drive)"`

---

## Stage 4 — Own the frame: present, capture, config, then remove ReShade (4 tasks)

### Task 18: `present_owner` — our command list on the presenting queue

**Files:**
- Create: `src/backend_native/present_owner.{hpp,cpp}`
- Modify: `src/backend_native/dxgi_hooks.cpp` — the `Present`/`Present1` hooks call `present_owner::before_present(swapchain)` then the original; `ResizeBuffers` → `Sink::on_swapchain(created=false/true)` around the original
- Modify: `src/nr_history.cpp:519-610` — `on_present` takes a `PresentContext`; its barriers and copy record onto `present_owner::list()` directly (the `_has_commands` workaround at `:560-580` is deleted with a comment saying why it could be)
- Test: `tests/warp/warp_mv_resolve.cpp` — `test_present_owner_submits_in_order` (a swapchain-less variant: `before_present` on a queue with a fence; assert our list executed after the game's list submitted earlier, by reading a UAV both wrote)

**Interfaces:**
- Produces: `namespace present_owner { ID3D12GraphicsCommandList *list(); ID3D12CommandQueue *queue(); ID3D12Resource *back_buffer(); std::uint64_t frame(); }` — one allocator per in-flight frame (3, matching `BufferCount = 3`, CLAUDE.md §2.1), a fence, `ExecuteCommandLists` on the swapchain's queue **before** forwarding `Present`.

- [ ] **Step 1: Failing WARP test. Step 2: Implement.** The queue is the one captured at swapchain creation (early) or the one that executed the tracked TAA list (late; `on_execute` records it — HARD that the TAA list runs on the graphics queue that presents, since UE4's `FD3D12Viewport::Present` uses the same direct queue; SOFT for this title).
- [ ] **Step 3: Green. Commit** — `git commit -m "feat(native): present ownership — our list on the presenting queue"`

### Task 19: The screenshot channel — validated against ReShade's before ReShade leaves

**Files:**
- Create: `src/backend_native/capture.{hpp,cpp}` (re-points `src/input_dump.cpp`'s readback pattern — copy on our present list, map 5 presents later, HARD as measured under vkd3d)
- Modify: `tools/rawdump2png.py` — add `r10g10b10a2` (the swapchain format, CLAUDE.md §2.1)
- Create: `tools/capture-stray.sh` — writes `stray-dlss-capture.cmd` into the game dir (`frames=1` or `frames=2` for a consecutive pair), waits for `straydlss_capture_<frame>.bin` + `.json`, copies both back, converts

**Interfaces:**
- Produces: on `Present`, if `<gamedir>/stray-dlss-capture.cmd` exists: read `frames=N`, delete the file, barrier back buffer `PRESENT → COPY_SOURCE → PRESENT` (the back buffer **must** be in `PRESENT` at Present, HARD D3D12 rule), copy to a `READBACK` buffer, fence; write `straydlss_capture_<frame>.bin` and `.json {width,height,format,pitch,frame,phase}` when the fence passes. `phase` is the `DryRunAlternate` phase so the ALT-PHASE correlation becomes a field instead of a timestamp match.

- [ ] **Step 1: WARP test `test_capture_reads_back_the_render_target`**: render a known gradient to a texture in `PRESENT`-equivalent state, capture, assert bytes.
- [ ] **Step 2: Implement. Green.**
- [ ] **Step 3: Box validation — Stage 4's gate:** with ReShade still loaded, trigger ours and `tools/screenshot-stray.sh` within the same second, camera still. Compare PNGs: `magick compare -metric RMSE` must be within what two consecutive still frames measure (CLAUDE.md §1's temporal-stability method — take five pairs). Record the number in `docs/STRAY-RENDERING-FACTS.md`. Then take a **consecutive pair** (`frames=2`) and confirm `tools/measure-alternating.py` can consume the `.json` phase field.
- [ ] **Step 4: Commit** — `git commit -m "feat: own screenshot channel, validated against ReShade's on the same frame"`

### Task 20: Hot-reload config and the status file replace the overlay

**Files:**
- Modify: `mods/StrayDLSS/src/Mod.cpp` — `on_update` polls `ini.reload_if_changed()` every `ConfigReloadSeconds` (lifted knob) and pushes changed **live** keys (`NgxNRRestoreHistory`, `NgxNRPaperWhiteScale`, `NgxNRColorStrength`, `NgxNRTransferStrength`, `NgxNRTrackExposure`, `DryRunAlternate`, `NgxDryRun`, `RestoreState`) through the same setters the overlay's checkboxes call today; every other key logs `needs a relaunch`
- Modify: `src/app/dlss_app.cpp` — the status-file writer (`addon.cpp:990-1000` today) gains every number the OSD showed: evaluate counts, RR counters, NR history counters, the `DIFF SUMMARY` numbers, `attach_report()`

- [ ] **Step 1: Linux test**: `IniFile::reload_if_changed` returns true only when mtime moved; a changed live key is reported by a pure `diff_live_keys(old, new)` returning the list of changed names.
- [ ] **Step 2: Implement. Box: edit `NgxNRTransferStrength` over ssh mid-session; the log must show the setter firing within `ConfigReloadSeconds` and the image must change (the exact-bypass A/B from CLAUDE.md).**
- [ ] **Step 3: Commit** — `git commit -m "feat(plugin): hot-reload INI drives the live knobs; status file replaces the OSD"`

### Task 21: Remove ReShade from the box, keep the code

**Files:**
- Modify: `mods/StrayDLSS/README.md` — install: `WINEDLLOVERRIDES="dwmapi=n,b"`, no `dxgi` override, no `.addon64`; `nvngx_dlss.dll` beside the exe as before
- Modify: `src/ngx_backend.cpp`, `src/ngx_nr.cpp` — `ext_unhook::repair()` becomes a no-op when `ext_unhook::capture()` found the slots pristine **and** `GetModuleHandleW(L"dxgi.dll")` is Wine's builtin (no ReShade) — logged as `ext_unhook: ReShade absent, repair idle`. The code stays until a full session confirms the vtable never changes.
- Modify: `src/app/dlss_app.cpp` — NGX is initialised with `native::game_device()`; the `NgxDevice` knob and the frame-120 decision log a deprecation line and are removed one release later

- [ ] **Step 1: Box run with ReShade removed — the acceptance test of the whole plan.** Required, all from `stray-dlss-plugin.log` and `dmesg`: `attach_report` shows the device, queue and swapchain found; `ext_unhook: ReShade absent`; `DLSS evaluate OK` cadence ≥ 99%; indicator present; fps within 5% of Stage 1; `dmesg` clean over 20 minutes; `tools/capture-stray.sh` returns a PNG the user judges correct; `NgxNRHook=preui` census lines show the back-buffer bind ordinal (the boundary is now ours to define — assessment §3.4).
- [ ] **Step 2: Update `CLAUDE.md` §1 and §6:** the truth table's "native, patched" row is unreachable without ReShade; the real-ReShade CI lane's status; the new launch line. **In the same commit**, per house rule 5.
- [ ] **Step 3: Commit** — `git commit -m "docs: first DLSS SR build without ReShade — measured on the target"`

---

## Stage 5 — UE4SS engine-side signals (optional for the first build; 1 task kept small)

### Task 22: Game-thread frame marker and a console-command channel

**Files:**
- Modify: `mods/StrayDLSS/src/Mod.cpp` — `on_unreal_init`: `Hook::RegisterEngineTickPreCallback` (HARD: exported, on by default — assessment §6.1.6) publishes a game-thread frame counter to `app`; a `stray-dlss-exec.cmd` file in the game dir is consumed once per tick and passed to `UKismetSystemLibrary::ExecuteConsoleCommand` via `UObjectGlobals::StaticFindObject` + `ProcessEvent` (HARD that the UFunction exists; UNCONFIRMED that Stray's shipping build honours `r.ScreenPercentage` from it — verify by the DLSS create log showing the new rect)
- This is the seed for assessment §6.1's deterministic-camera work; that is its own plan.

- [ ] **Step 1: Build. Box: write `r.ScreenPercentage 70` to the command file; expect `DLSS feature created 2688x1512 -> 3840x2160` in the log without a relaunch.**
- [ ] **Step 2: Commit** — `git commit -m "feat(plugin): game-thread tick marker and a console-command file"`

---

## Self-review against the spec

**Spec coverage** (assessment §1, §4, §5, §9): §1.1 shadow → Tasks 10–12, 15; §1.2 installation/lifetime/coexistence → Tasks 9, 12 guards, 13; §1.3 present → Task 18; §1.4 liveness → Task 10 (+ Task 1 probe); §1.5 config/log → Tasks 4–5, 20; §1.6 visual channel → Task 19, gated before Task 21; §1.7 glue/build → Tasks 2, 8, 16; §1.8 overlay → Task 20 (deferred faithful OSD, as recommended); §5.2 CI lanes → Tasks 14–15 (real-ReShade lane kept and extended); §6.2 timing → Task 2 decides, Task 13 implements both, Task 15/16 measure; §9 stages 0–5 → Stages 0–5 above. **Not covered, by design:** §7 frame generation (excluded by Global Constraints); §6.1's deterministic camera (Task 22 is only the seed).

**Placeholder scan:** every code step has code; every box step names the exact log line or number that decides it.

**Type consistency:** `icept::CommandContext`, `icept::DispatchBindings`, `icept::BufferRange`, `icept::ResourceId`, `icept::DescriptorId`, `host::cfg::get_*`, `host::IniFile`, `native::patch_slot`, `native::registry::*`, `native::shadow::*`, `native::attach`, `native::game_device`, `present_owner::*`, `app::instance()` are used with the same names and signatures throughout.

**The three riskiest tasks, and why:**

1. **Task 12 (descriptor + root shadow).** The only work with no offline oracle; a wrong answer is *plausible*, which is the failure class `CLAUDE.md` §0.2 exists to prevent. Mitigation is structural, not hopeful: Task 15's differential observer validates it in CI under real ReShade and then on the game with ReShade's known-good trackers as the oracle, before it ever drives.
2. **Task 13 (attach timing / late attach).** Whether UE4SS's `start_mod()` precedes `D3D12CreateDevice` is a property of Stray's startup nobody has measured (UNCONFIRMED); the late path's swapchain discovery needs a hidden window under gamescope, which has a documented freeze mode. Task 2 spends one launch to settle both, and the proxy-DLL loader is the known-good fallback.
3. **Task 8 (the build split).** `/MT`+`nvsdk_ngx_s.lib` for the add-on and `/MD`+`nvsdk_ngx_d.lib` for the mod, against a UE4SS whose ABI is bound to one main-branch SHA and whose Unreal submodule is Epic-gated. A mistake here is heap corruption at runtime, not a link error — the `dumpbin` assertions lifted from `dualsense.yml` are the only tripwire.
