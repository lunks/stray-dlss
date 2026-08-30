# Verified external research

Everything here was established by reading primary sources — ReShade's own headers at tag
`v6.8.0`, the NVIDIA DLSS SDK headers and Programming Guide, UE 4.27.2 source, vkd3d-proton and
dxvk-nvapi source and changelogs, Proton's launcher script, and NVIDIA's own DLSS-for-Unreal
plugin. Each claim was then handed to a separate adversarial verifier whose job was to refute it.

Confidence labels: **HARD** = read in a header, source file or official doc. **SOFT** = inferred
from consistent secondary evidence. **UNCONFIRMED** = not established; do not build on it.

Counts: 228 claims, 183 confirmed, 42 corrected, 4 refuted. The corrections are folded in below —
where a first-pass claim was wrong, only the corrected form appears.

---

## 1. Proton / vkd3d-proton — the feasibility question, resolved

**D3D12 NGX DLSS works under vkd3d-proton. This is a first-class, NVIDIA-authored path, not a
hack.** (HARD)

The chain, end to end:

```
our add-on
  → NVSDK_NGX_D3D12_*            (nvsdk_ngx_d.lib, statically linked into us)
  → nvngx.dll / _nvngx.dll       (NVIDIA driver, "NGX Core for Proton", PE, copied into system32)
  → nvngx_dlss.dll               (PE, shipped beside the game)
  → NvAPI_D3D12_CreateCubinComputeShaderWithName / LaunchCubinShader
    / GetCudaTextureObject / GetCudaSurfaceObject      (DXVK-NVAPI nvapi64.dll)
  → ID3D12DeviceExt / ID3D12GraphicsCommandListExt     (vkd3d-proton)
  → VK_NVX_binary_import + VK_NVX_image_view_handle    (NVIDIA Vulkan driver)
```

* vkd3d-proton CHANGELOG 2.5, section *NVIDIA DLSS*: "NVIDIA contributed integration APIs in
  vkd3d-proton which enables DLSS support in D3D12 titles in Proton." The IDL files carry
  `Copyright 2021 NVIDIA Corporation`. (HARD)
* **`ID3D12Resource` being really a `VkImage` is not a problem.** `nvngx_dlss.dll` never touches
  native handles — it creates ordinary D3D12 SRV/UAV descriptors on its own heaps and passes
  `D3D12_CPU_DESCRIPTOR_HANDLE` values to NvAPI, which vkd3d decodes to a `VkImageView`. **Pass
  plain `ID3D12Resource*` and nothing else.** Do not touch `ID3D12DXVKInteropDevice`,
  `GetVulkanResourceInfo`, or any vkd3d interop API — they are irrelevant here and add breakage
  surface. (HARD)
* **Do not write a D3D12→Vulkan NGX bridge and do not use the Vulkan NGX path.** (HARD)
* Empirical confirmation independent of the source argument: Cyberpunk 2077 in D3D12 with DLSS
  works under Proton, with logs showing `_nvngx.dll` then `nvngx_dlss.dll` loaded native. (HARD)

### 1.1 The one real hazard, and why it is already fixed

ReShade-with-add-on-support + D3D12 DLSS under vkd3d was a **known crash**. crosire's own
explanation: ReShade's add-on build rewrites descriptor handles for tracking; DLSS calls
`NvAPI_D3D12_GetCudaTextureObject` with a handle from the game; with real NVAPI that re-enters
ReShade's proxy and gets converted, but vkd3d's own implementation bypassed ReShade's hooks, so an
unconverted handle got through → invalid pointer access. (HARD)

Fixed in two commits, **both contained in tag `v6.8.0`**:

| Commit | Date | What |
|---|---|---|
| `9b2815e48e55393d478fda21f1b3f31d49336298` | 2026-01-10 | "Fix crash with vkd3d and DLSS" — adds `source/d3d12/d3d12_extensions.cpp`, hooks `ID3D12DeviceExt` vtable slots 7/8 (`GetCudaTextureObject`, `GetCudaSurfaceObject`) |
| `50319382618a53072a9e92ca3509caa2c7051f48` | 2026-03-02 | "Add support for ID3D12DeviceExt2 from vkd3d" — also hooks slots 14/15 |

**ReShade ≥ 6.8.0 is therefore a hard prerequisite.** 6.7.3.2148 was still reported broken on
2026-03-23. The user's 6.8.0.2155 qualifies. Log the ReShade version at startup and warn below
6.8.0. (HARD)

`ID3D12DeviceExt2` (UUID `e859c4ac-ba8f-41c4-8eac-1137fde6158d`) landed in vkd3d-proton
`23321a6b` (2025-02-04) — not in v2.14.1, present in v3.0/v3.0.1. The user's
`GE-Proton-dxvk301-…` is vkd3d 3.0.1-era, so ReShade 6.8.0 is exactly the minimum. (HARD)

### 1.2 Why initialising NGX with ReShade's native device is the safe design

`device::get_native()` returns the **original vkd3d `ID3D12Device`**, not ReShade's proxy —
`class device_impl : public api::api_object_impl<ID3D12Device *, api::device>` with
`get_native() { return (uint64_t)_orig; }`. NGX initialised with it allocates genuine vkd3d
descriptors and queries `ID3D12DeviceExt` off the raw device, **bypassing ReShade's descriptor
remapping entirely**. (HARD)

That matters because `device_impl::convert_to_original_cpu_descriptor_handle` has **no validation
in release builds** — it computes a heap index and indexes a vector behind a bare `assert`. Feed it
a handle it did not mint and you get an out-of-bounds read. Do **not** try to fish out ReShade's
proxy device via `GetPrivateData(__uuidof(D3D12Device))`. (HARD)

### 1.3 Runtime prerequisites on the user's machine

* `nvngx.dll` and `_nvngx.dll` must be in the prefix's `system32`. Proton's
  `find_nvidia_wine_dll_dir()` locates them next to `libGLX_nvidia.so.0` under `nvidia/wine/` and
  copies both in during `setup_prefix()`, exporting `NVIDIA_WINE_DLL_DIR`. Distro packaging must
  ship that directory (Arch `nvidia-utils`: `/usr/lib/nvidia/wine/`). (HARD)
* Registry key `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath` = `C:\Windows\system32`.
  dxvk-nvapi's wiki: its absence "might prevent NVNGX from initializing correctly." (HARD)
* `nvidia_uvm` kernel module loaded (`nvidia-modprobe -u -c=0`). Its absence shows up as
  `NVSDK_NGX_Result_FAIL_PlatformError`. (HARD)
* **`PROTON_ENABLE_NVAPI` does not exist.** Proton exposes only `PROTON_DISABLE_NVAPI` →
  `disablenvapi` and `PROTON_FORCE_NVAPI` → `forcenvapi`; **NVAPI is on by default** and Proton
  sets `DXVK_ENABLE_NVAPI=1` itself. Never set `PROTON_HIDE_NVIDIA_GPU=1`. (HARD — this corrects
  the older guidance in our own notes.)
* **Do not set `VKD3D_CONFIG=descriptor_heap` / `PROTON_VKD3D_HEAP=1`.** That path routes
  `GetCudaTextureObject` through `vkGetDeviceCombinedImageSamplerIndexNVX` and fails with
  `E_INVALIDARG` unless the CPU handles point at shader-visible heaps. It is opt-in today; treat a
  future default flip as a forward-compat time bomb needing revalidation. (HARD)
* `NVSDK_NGX_D3D12_GetFeatureRequirements` is **not implemented** by the Proton-side `_nvngx.dll`
  and reports unsupported even when DLSS works. **Do not gate availability on it.** Gate on
  `NVSDK_NGX_D3D12_Init` succeeding plus `NVSDK_NGX_Parameter_SuperSampling_Available`. (HARD)
* `PROTON_ENABLE_NGX_UPDATER` is consumed by the driver's `_nvngx.dll`, not by Proton. **Leave it
  off** — the updater has a history of access violations inside `nvngx.dll`. Ship a pinned,
  known-good `nvngx_dlss.dll`. (HARD)

### 1.4 Free diagnostics

* **vkd3d-proton detection**: `QueryInterface(IID_ID3D12GraphicsCommandListExt,
  77a86b09-2bea-4801-b89a-37648e104af1)` on the native command list. ReShade itself uses this as
  its vkd3d detector. Log "vkd3d-proton" vs "native D3D12" at startup. (HARD)
* `DXVK_NVAPI_LOG_LEVEL=info` + `DXVK_NVAPI_LOG_PATH=<dir>` → `nvapi64.log`. (HARD)
* `PROTON_LOG=1` → every `loaddll:build_module` line, i.e. exactly which `nvngx*.dll` loaded from
  where. (HARD)
* `DXVK_NVAPI_SET_NGX_DEBUG_OPTIONS=DLSSIndicator=1024` → the on-screen DLSS indicator. **1024 is
  required for release builds of DLSS; 1 only works for develop/debug builds.** This is the
  fastest possible visual proof that DLSS is actually executing rather than silently falling back.
  Warning: it permanently modifies the registry. (HARD)
* `VKD3D_DEBUG=warn` + `VKD3D_LOG_FILE=<path>`. (HARD)

### 1.5 gamescope and HDR

Orthogonal. The R10G10B10A2 HDR swapchain is downstream of where DLSS runs, and reporters in the
vkd3d ReShade+DLSS thread confirm results are identical with and without gamescope. Known
gamescope HDR bugs are WSI/swapchain-level and independent. Spend no effort here. (SOFT)

### 1.6 Fallback ranking, if it ever comes to that

1. **D3D12 NGX DLSS** — confirmed, one caveat already fixed. Commit to it.
2. **FSR2/FSR3 via the FidelityFX SDK D3D12 backend** — pure D3D12 compute, no driver dependency,
   shaders offline-compiled into headers so no runtime compiler needed. Cheapest insurance, and
   doubles as a way to validate the depth/velocity/jitter plumbing independently of NGX.
3. **XeSS** — also pure D3D12, Linux-proven, but no quality win over DLSS on a 4090.
4. **Vulkan NGX via vkd3d interop** — enormous complexity, zero benefit. Do not.
5. **Windows-only** — unnecessary.

---

## 2. ReShade add-on API (v6.8.0)

**Pin the headers to tag `v6.8.0`.** `RESHADE_API_VERSION` is **20**. ReShade rejects an add-on
whose version is greater than its own:

```cpp
if (api_version == 0 || api_version > RESHADE_API_VERSION ||
    (api_version / 10000) != (RESHADE_API_VERSION / 10000))
    // logs: Failed to register add-on, because the requested API version (%u) is not supported (%u)!
```

Vendoring `main` risks a bump to 21 that silently prevents loading. Older is accepted (forward
compatible); newer is not. (HARD)

Version history: 6.0.0=10, 6.1.0=11, 6.2.0=13, 6.3.0=14, 6.4.0=16, 6.5.x=17, 6.6.0/6.7.x=18,
**6.8.0=20**.

### 2.1 Loading and registration

* A plain DLL renamed `.addon64`. Header-only — **do not define `RESHADE_API_LIBRARY`**; there is
  no import lib to link.
* `reshade::register_addon(hModule)` in `DllMain`/`DLL_PROCESS_ATTACH`. It finds ReShade via
  `reshade::internal::get_reshade_module_handle()`, which enumerates process modules for one
  exporting both `ReShadeRegisterAddon` and `ReShadeUnregisterAddon`.
* Optional exports `AddonInit(HMODULE addon, HMODULE reshade)` / `AddonUninit(...)`. **Do NGX init
  in `init_device`, never in `DllMain`** (loader lock).
* Optional string exports `NAME`, `DESCRIPTION`, `WEBSITE` as `const char *const *`. Embed a
  VERSIONINFO resource too — the version appears in the success log line.
* Discovery scans for `*.addon64` and loads with
  `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`, so **a `nvngx_dlss.dll`
  placed in the same directory resolves** — a supported way to bundle it.
* Success line to grep for: `Registered add-on "..." vX.Y.Z.W using ReShade API version 20.`

### 2.2 The critical build gate

`push_descriptors`, `bind_descriptor_tables`, `bind_pipeline`, `create_pipeline`/`init_pipeline`,
`copy_descriptor_tables`, `update_descriptor_tables`, `copy_buffer_region` are all compiled under
**`#if RESHADE_ADDON >= 2`** in the D3D12 backend — i.e. they exist only in the **full** add-on
build. Only `dispatch`, `draw*`, `init_resource`, `create_resource`, `init_resource_view` are at
level 1. A limited build logs *"Skipped loading add-on ... because this build of ReShade has only
limited add-on functionality."* and does not load `.addon64` at all. (HARD)

**Assert at startup that we saw at least one `bind_pipeline`/`push_descriptors` in the first N
frames.** Without that check, "wrong ReShade build" and "UE4 binds differently" are
indistinguishable from a log.

### 2.3 Events

Only **`bool`-returning** events can skip the underlying call. On the TAA path that is
**`dispatch` alone**:

```cpp
bool on_dispatch(api::command_list *cmd, uint32_t gx, uint32_t gy, uint32_t gz);
// fires BEFORE ID3D12GraphicsCommandList::Dispatch; return true suppresses it
```

`bind_pipeline`, `push_descriptors`, `bind_descriptor_tables`, `push_constants`, `barrier`,
`init_*` all return `void` — observe only. (HARD)

**TRAP: in D3D12, `bind_pipeline` is invoked with `reshade::api::pipeline_stage::all`, NOT
`compute_shader`. Filtering on the compute stage silently misses every event.** (HARD)

**TRAP: `reshade::log_message(...)` no longer exists** (removed in 6.3.0). It is now
`reshade::log::message(reshade::log::level::info, buf)` with
`enum class level { error = 1, warning = 2, info = 3, debug = 4 }`, and it takes **no printf
arguments** — format into a buffer yourself. (HARD)

**Breaking 6.7→6.8**: `bind_descriptor_tables` gained `uint32_t dynamic_offset_count,
const uint32_t *dynamic_offsets`; `begin_render_pass`/`end_render_pass` changed `void`→`bool`.
Any callback copied from a 6.5–6.7-era add-on will not compile. (HARD)

### 2.4 Native handle mapping (D3D12)

`get_native()` is `virtual uint64_t get_native() const` — **it returns `uint64_t`, reinterpret_cast
it.** All handles are `typedef struct { uint64_t handle; } name;`.

| ReShade | D3D12 |
|---|---|
| `device::get_native()` | `ID3D12Device *` (the **original**, unwrapped) |
| `command_list::get_native()` | `ID3D12GraphicsCommandList *` (original) |
| `command_queue::get_native()` | `ID3D12CommandQueue *` |
| `swapchain::get_native()` | `IDXGISwapChain3 *` |
| `resource.handle` | `ID3D12Resource *` |
| `resource_view.handle` | a **real** `D3D12_CPU_DESCRIPTOR_HANDLE.ptr` (ReShade un-mangles before reporting) |
| `pipeline.handle` | `ID3D12PipelineState *` |
| `pipeline_layout.handle` | `ID3D12RootSignature *` |
| `descriptor_heap.handle` | `ID3D12DescriptorHeap *` (original) |
| `fence.handle` | `ID3D12Fence *` |

`device::get_api() == api::device_api::d3d12` (`0xc000`).

Recording on the unwrapped command list bypasses ReShade's hooks — which is what we want — but
means ReShade's `_current_root_signature` / `_current_descriptor_heaps` shadow state goes **stale**
after NGX clobbers them. (HARD)

### 2.5 Identifying the shader

Bytecode **is** exposed. On D3D12, `CreateComputePipelineState` produces
`pipeline_subobject{ pipeline_subobject_type::compute_shader, 1, &cs_desc }` where
`shader_desc::code = pShaderBytecode` and `code_size = BytecodeLength` (`entry_point` is nullptr).
Hash it with fnv1a64 in `create_pipeline` or `init_pipeline`; you need `init_pipeline` for the
handle. Register `destroy_pipeline` to evict — `ID3D12PipelineState` pointers get recycled. (HARD)

**SIDE-EFFECT WARNING**: merely registering `create_pipeline`/`init_pipeline`/`destroy_pipeline`
makes ReShade redirect every PSO creation through `ID3D12Device2::CreatePipelineState` with a
rebuilt subobject stream, **and deliberately drop the CachedPSO blob** (its comment cites Need for
Speed: Unbound erroring out otherwise). Under vkd3d-proton expect longer first-run shader hitching
and a hard dependency on vkd3d implementing `CreatePipelineState` correctly. If that regresses, the
alternative is to identify the pass structurally (root signature + exact SRV/UAV format and
dimension set + dispatch size) and never register the pipeline events at all. (HARD)

### 2.6 Reading the UE4 View constant buffer at b1

Two paths, both observable — **register both and log which one fires**:

* **Path A, root CBV.** `SetComputeRootConstantBufferView` → `push_descriptors` with
  `descriptor_type::constant_buffer` and a `buffer_range` whose `.buffer` is the resolved
  `ID3D12Resource*` and `.offset` the byte offset. Note `.size` is `UINT64_MAX` here, not the real
  size — use our own known layout for bounds.
* **Path B, descriptor table.** `CreateConstantBufferView` → `update_descriptor_tables`;
  `CopyDescriptorsSimple` → `copy_descriptor_tables`; `SetComputeRootDescriptorTable` →
  `bind_descriptor_tables`. UE 4.27's D3D12 RHI writes CBVs into offline heaps, copies them into
  the online heap, then sets the table — every step emits an event.

**ReShade ships the exact utility for path B**: `examples/utils/descriptor_tracking.{hpp,cpp}`
(class `descriptor_tracking`, uuid `f02b21ba-f5dd-44af-9e90-c54535a98ce3`) with
`get_buffer_range(heap, offset)`. **Copy it verbatim — do not reimplement.** Pair with
`examples/utils/state_tracking.cpp`. Both use MSVC-only concurrent containers. (HARD)

**Register descriptor tracking from the very first frame.** Attach late and you miss the
`CreateConstantBufferView`/`CopyDescriptors` that populated the heap; `get_buffer_range` returns
`{0}` and there is **no way to recover retroactively**. (HARD)

Read the bytes with `device::map_buffer_region(res, offset, size, map_access::read_only, &p)` —
on D3D12 a straight `ID3D12Resource::Map`. UE 4.27's `FD3D12FastConstantAllocator` sub-allocates
from `D3D12_HEAP_TYPE_UPLOAD` pages, which are CPU-mappable. (HARD)

**Read the CB inside the `dispatch` callback, never deferred to `present`.** At command-recording
time, on the thread that just issued the `SetComputeRoot*` for this pass, the CPU writer has not
yet advanced past this frame's allocation. Reads from write-combine upload memory are slow — copy
the needed rows contiguously in one pass and unmap immediately. Verify with
`get_resource_desc(buf).heap == memory_heap::upload`; if it ever comes back `default_`, Map fails
and the readback fallback would make jitter one frame stale, which is unacceptable — treat that as
a hard error. (HARD)

### 2.7 Our own resources and passes

`device::create_resource(desc, initial_data, initial_state, &out)`. The header asserts
`(desc.usage & initial_state) == initial_state`, so **declare every state you will ever transition
to**. Flags: `unordered_access = 0x8`, `shader_resource = 0xC0`, `copy_dest = 0x400`,
`copy_source = 0x800`.

`create_resource` is a **bool-returning** event — modify `desc` and return true to widen usage
flags on the game's own targets, exactly as ReShade's bundled generic_depth add-on does. Do it
narrowly (match width/height/format/usage of the exact TAA targets); blanket widening costs DCC on
D3D12 and regresses performance. (HARD)

`device::get_resource_from_view(view)` bridges "the SRV bound at t0/t2/t5" to the
`ID3D12Resource*`; `get_resource_view_desc(view)` gives the format so we can **assert**
`r16g16b16a16_float` / `r32_float_x8_uint` before feeding DLSS. (HARD)

Barriers: `command_list::barrier(count, resources, old_states, new_states)` — the parameters are
`old_states`/`new_states` of type `const resource_usage *`. When both old and new are
`unordered_access` a UAV barrier is emitted instead of a transition. Mixing ReShade `barrier` with
raw `ResourceBarrier` on the same list is fine (both reach `_orig`) but pick one. (HARD)

### 2.8 UI and config

* `reshade::register_overlay(nullptr, cb)` registers a **settings** overlay under our entry in the
  add-on list. `register_overlay("OSD", cb)` appends to the always-visible on-screen display —
  **register both.** The OSD is the fastest remote diagnostic when the user can only send a
  screenshot. ReShade calls `ImGui::Begin`/`End` around named overlays; do not call them yourself.
* `reshade::get_config_value` / `set_config_value` (pass `nullptr` runtime for the global
  ReShade.ini). Every knob goes in a `[STRAYDLSS]` section — the only way the user changes
  behaviour without a rebuild.
* **imgui is pinned exactly.** `reshade_overlay.hpp` v6.8.0 contains
  `#if IMGUI_VERSION_NUM != 19250 #error ...`. **19250 == Dear ImGui v1.92.5.** Vendor
  `imgui.h`/`imgui_internal.h` from tag `v1.92.5`, headers only — we do not compile imgui's .cpp;
  reshade.hpp routes every `ImGui::` call through a function table. **Include `imgui.h` BEFORE
  `reshade.hpp`.** A mismatch makes `register_addon` return **false** and the add-on never loads —
  silent and hard to diagnose, so log to a file before `register_addon` too.
  (`static_assert(sizeof(ImTextureID) == 8)` — 1.92.5 already defaults to `ImU64`.)

---

## 3. NVIDIA DLSS / NGX (D3D12)

### 3.1 Feature flags

```
_IsHDR = 1<<0   _MVLowRes = 1<<1   _MVJittered = 1<<2   _DepthInverted = 1<<3
_DoSharpening = 1<<5 [[deprecated]]   _AutoExposure = 1<<6   _AlphaUpscaling = 1<<7
```

**For Stray: `IsHDR | MVLowRes | DepthInverted | AutoExposure` = `0x4B`.** (HARD)

* `IsHDR = 1` — Stray's TAA input is pre-tonemap linear FP16. Set 0 only for 0..1 perceptually
  encoded colour.
* `MVLowRes = 1` — MVs at render resolution; DLSS dilates them internally, which the guide says is
  preferred over app-side dilation.
* `MVJittered = 0` — UE4 velocities do not carry jitter.
* `DepthInverted = 1` — reversed-Z.
* **Never set `DoSharpening`** — deprecated, does nothing, emits a compiler warning.

### 3.2 Motion vectors

**Format must be `RG16_FLOAT` or `RG32_FLOAT`, in render-resolution PIXELS, [0,0] upper-left, and
must point BACKWARD (current→previous).** Guide §8.1.1.a: "if the game is running at 1080p and the
object has moved from the left edge of the screen to the right edge, then the x value of the
motion vector for that pixel on the right edge will be **-1080.0**." (HARD)

**Stray's velocity at t2 is `r16g16b16a16_unorm` — not an accepted format — so a resolve pass into
a fresh RG16_FLOAT UAV is mandatory regardless of the sparseness problem.** (HARD)

The Programming Guide ships a **UE4-specific dense MV resolve shader** (§3.6.1.1) that solves
exactly our problem, and NVIDIA's DLSS-for-Unreal plugin ships the same thing as
`VelocityCombine.usf`. Net transform:

```
MV_pixels = (PrevScreen - ThisScreen) * float2(0.5 * RenderW, -0.5 * RenderH)
```

output `R16G16_FLOAT` at render resolution, and **`InMVScaleX/Y = (1, 1)`**. Guard with
`if (PrevClipPos.w > 0)` (zero MV behind the camera). (HARD)

Two valid designs: **(a)** bake the whole transform into the resolve shader and use scale (1,1) —
recommended, matches NVIDIA's own shader and is what the debug overlay's MV visualiser expects; or
**(b)** emit raw clip deltas with `InMVScaleX = -0.5*W`, `InMVScaleY = +0.5*H`. Never 0.0.

### 3.3 Jitter

`InJitterOffsetX/Y` are in **render-resolution pixels**, must lie in `[-0.5, +0.5]`, and use the
same coordinate system as motion vectors (X right, Y down; the "X right Y down" wording is derived,
not quoted). (HARD for range/space, SOFT for the axis wording.)

**Pass `View.TemporalAAParams.zw` straight across with NO sign flip.** NVIDIA's own UE plugin does
`const FVector2D JitterOffset = View.TemporalJitterPixels; DLSSArguments.JitterOffset = JitterOffset;`
and donut's reference wrapper is algebraically identical. (HARD)

**Phase count** should be `8 * (target / render)^2`. Documented minimums: DLAA 8, Quality 18,
Balanced 24, Performance 32, UltraPerformance 72. UE4's default `r.TemporalAASamples` is 8 —
sufficient for DLAA, **not** for Performance. Raise it when offering SR modes. (HARD)

**In-game A/B for the sign**: the SDK DLL exposes jitter-negation cycling on **Ctrl+Alt+F9** (per
§3.18.1.1) or **Ctrl+Alt+F10** (per §3.18.1 — the guide contradicts itself, try both). 20 configs;
config 9 negates Y, config 11 negates both. This converts an untestable code guess into a single
in-game test. Note these hotkeys exist **only in the SDK/development DLL**, not the release one.

### 3.4 Exposure

UE4 pre-multiplies scene colour by `View.PreExposure`. Two options: **(a)** set `AutoExposure` and
leave `InPreExposure` at 0 (the helper coerces 0→1.0) — low-risk first shot; or **(b)** supply
`View.PreExposure` via `InPreExposure`, which is what NVIDIA's plugin does. Getting it wrong looks
like ghosting plus brightness lag. Exposure texture is 1×1, first channel only, R16F preferred, and
is **only supported by presets J and K** — preset L always uses auto-exposure. Auto-exposure costs
~0.02 ms. (HARD)

**Write the DLSS output back into u0 still pre-exposed** — everything downstream (bloom,
tonemapper) expects pre-exposed scene colour.

### 3.5 D3D12 resource states and state clobbering

Inputs (colour, MVs, depth, exposure) must be in `D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE`;
the output must be in UAV state and created with `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`.
Missing that flag yields `FAIL_RWFlagMissing` (0xBAD00009) — but §8.1.7 warns the symptom of wrong
usage flags can instead be **"the output may be black without further indication."** (HARD)

**NGX clobbers D3D12 command-list state.** Guide §5.4, verbatim: *"IMPORTANT: NGX modifies the
Vulkan and D3D12 command list states. The calling process must save and restore its own Vulkan or
D3D12 state before and after making the NGX evaluate feature calls."* §5.2.5 adds that D3D11's
state preservation does **not** apply to D3D12. The reference sample calls `ClearState()`
immediately after evaluate. (HARD)

**This is the number one corruption risk for a ReShade add-on injecting into the game's own command
list mid-frame.** After Evaluate we must restore descriptor heaps, root signature, PSO, root
parameters, topology, viewports/scissors and RTVs — the game will not re-set them, and ReShade does
not do it for us. Heaps are restorable via
`device->get_descriptor_heap_offset(table, 0, 0, &heap, &offset)` on a table seen in
`bind_descriptor_tables`, then `SetDescriptorHeaps` with the original heap pointer.

Command list must be **open and recording**, with a node mask including the Init device, and
executed on a **non-copy** queue. Stray's TAA dispatch is on the direct queue — fine. (HARD)

### 3.6 Presets

**Presets A–D are removed from the current header; E and F are deprecated; G/H/I/N/O revert to
default.** The only usable values are `_Default = 0`, `_J = 10`, `_K = 11`, `_L = 12`, `_M = 13`.
Numeric values have been stable since 310.1.0. (HARD)

* **K** — "Default preset for DLAA/Balanced/Quality modes that is transformer based. Best image
  quality preset at a higher performance cost." **Use K for DLAA/Quality/Balanced on a 4090.**
* **M** — default for Performance. **L** — default for Ultra Performance.
* "DLSS 4 transformer" means J/K/L/M, never a letter ≤ F. Validate any config integer against
  `{0, 10, 11, 12, 13}`.

**Presets must be set on the parameter map BEFORE `NGX_D3D12_CREATE_DLSS_EXT`** — the helper never
touches the preset keys. Set all five hint keys to the same value. Setting them after create has no
effect; changing one requires release + recreate. (HARD)

RTX 40-series cost at 3840×2160: presets J/K = 307 MB, 1.06 ms; L = 448 MB, 1.67 ms; M = 456 MB,
1.39 ms.

### 3.7 Lifecycle

* `NVSDK_NGX_D3D12_Init_with_ProjectID(projectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, version, path,
  device, featureCommonInfo, sdkVersion)` — **a self-generated GUID works; no NVIDIA whitelist is
  needed.** A malformed ID returns `FAIL_InvalidParameter` (0xBAD00005). (HARD)
* Pass an `NVSDK_NGX_FeatureCommonInfo` with a **`LoggingCallback`** forwarding into
  `reshade::log::message`. Signature
  `void NVSDK_CONV (*)(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)`. This
  directly serves the "must be diagnosable from a log" constraint. Also settable:
  `HKLM\...\NGXCore\LogLevel`. (HARD)
* `PathListInfo` adds DLL search paths beyond the application folder — pass our own directory so
  both install layouts work.
* Quality mode should be **derived, not chosen**: loop every `NVSDK_NGX_PerfQuality_Value`, call
  `NGX_DLSS_GET_OPTIMAL_SETTINGS`, and take the mode whose optimal render resolution best matches
  the resolution actually in use. Retry a rejected create with `Balanced` + `Preset_Default`.
* **Output resolution change requires full `ReleaseFeature` + `CreateFeature`.** Stray switches
  between 3840×2160 and 2560×1440. Guard Evaluate to no-op when observed sizes differ from creation
  sizes rather than passing mismatched ones. Minimum target is 64×32 (output floor 32×32).
* Teardown order, all on one thread, **NGX is explicitly not thread-safe**: fence-wait for GPU idle
  → `ReleaseFeature` → `DestroyParameters` → `Shutdown1(device)`. All before ReShade releases the
  device. Set `NVSDK_NGX_Parameter_FreeMemOnReleaseFeature = 1` before releasing on a resolution
  change to avoid holding ~300–450 MB of stale cache.
* **NGX does not hold references to our resources** — we must keep strong refs while any Evaluate
  referencing them is in flight.
* Result codes are a **bitmask** over `0xBAD00000`. Use `NVSDK_NGX_SUCCEED` / `NVSDK_NGX_FAILED`,
  never `== Success`. Log `%08x` plus `GetNGXResultAsString` (returns `wchar_t*`, use `%ls`).

### 3.8 Eval params

`memset(&params, 0, sizeof(params))` is safe and idiomatic — the `NGX_D3D12_EVALUATE_DLSS_EXT`
helper coerces `MVScale 0→1.0f` and `PreExposure/ExposureScale 0→1.0f`. Strictly required:
`pInColor`, `pInOutput`, `pInDepth`, `pInMotionVectors`, `InJitterOffsetX/Y`,
`InRenderSubrectDimensions` (this last one is **above** the `/*** OPTIONAL ***/` marker). Stray
renders full-viewport, so all subrect bases are `{0,0}` and `InEnableOutputSubrects = false`.
(HARD)

### 3.9 Placement, and licensing

Guide §8.1.3, directly validating our plan: *"Disabling TAA often changes the way the engine renders
(jitter, depth, motion etc.) which can consequently break DLSS. When integrating DLSS, the TAA
rendering pass should be replaced with DLSS but everything else in the rendering pipeline that is
activated when TAA is turned on, must still be executed as if TAA is still active."* DLSS must run
in post-processing **before tone mapping**, as close to the start of post as possible. Stray's TAA
is exactly there. **Never disable TAA via `r.DefaultFeature.AntiAliasing`.** (HARD)

Link `nvsdk_ngx_d.lib` for `/MD` or `nvsdk_ngx_s.lib` for `/MT`, from
`lib/Windows_x86_64/x64/`. These are **real static libraries**, not import libs, and they expose
C++ std types across the ABI — mismatched `_ITERATOR_DEBUG_LEVEL` produces link errors in CI. Build
Release `/MD` with plain `nvsdk_ngx_d.lib`. (HARD — this corrects our earlier `/MT` preference.)

Redistribution of `lib/<plat>_<arch>/rel/nvngx_dlss.dll` is permitted only **as part of an
application with material additional functionality** (§2(a)) and **never as a stand-alone product**
(§4(b)). §4 requires notifying NVIDIA before commercial release, including for plug-ins. We are
non-commercial, but the cleanest posture is to have CI fetch it from the official NVIDIA/DLSS
release rather than commit it. (HARD)

---

## 4. UE 4.27.2 — the TAA pass in detail

Verified against a real UE 4.27.2 tree (`Build.version`: 4/27/2, `++UE4+Release-4.27`).

### 4.1 The shader is not where you'd expect

**`FTAAStandaloneCS` compiles `/Engine/Private/TemporalAA/TAAStandalone.usf`, entry `MainCS`.**
`PostProcessTemporalAA.usf` **does not exist in 4.27**. Threadgroup is 8×8; dispatch is
`ceil(W/8) × ceil(H/8)` over the TAA output rect — a cheap, reflection-free sanity check that an
intercepted dispatch really is the main TAA. (HARD)

### 4.2 Binding layout, resolved against our measurements

HLSL declaration order in `TAAStandalone.usf`: `InputSceneColor` (460), `InputSceneMetadata` (462),
`SceneDepthTexture` (465), `GBufferVelocityTexture` (468), `StencilTexture` as `Texture2D<uint2>`
(471), `HistoryBuffer_0` (473), `HistoryBuffer_1` (474); UAVs `OutComputeTex_0` (485),
`OutComputeTexDownsampled` (490, `#if TAA_DOWNSAMPLE`), `OutComputeTex_1` (495). `EyeAdaptationTexture`
comes in earlier via `EyeAdaptationCommon.ush`. (HARD)

For `ETAAPassConfig::Main` on deferred SM5, **exactly six SRVs survive dead-code elimination**:
`InputSceneColor`, `SceneDepthTexture`, `GBufferVelocityTexture`, `StencilTexture`,
`HistoryBuffer_0`, `EyeAdaptationTexture`. Dead: `InputSceneMetadata` (DOF configs only),
`HistoryBuffer_1`, `EyeAdaptationBuffer` (mobile). `DebugOutput` has no HLSL declaration at all and
is always stripped. (HARD)

**So our unknown t1/t3/t4 must come from `{StencilTexture, EyeAdaptationTexture}` plus a gap. No
other SRV can be bound by this shader.**

* **t1 is `StencilTexture`**: an SRV over the **same `ID3D12Resource`** as the depth SRV. With
  `r.D3D12.Depth24Bit = 0` (Stray's default) the resource is `R32G8X24_TYPELESS`, the depth view is
  `R32_FLOAT_X8X24_TYPELESS` and the stencil view is `X32_TYPELESS_G8X24_UINT`. That is exactly the
  `r32_g8_typeless` we measured. **Bulletproof runtime test: the two SRVs whose underlying resource
  pointers are equal; the `R32_FLOAT_X8X24_TYPELESS` one is depth (feed to `pInDepth`), the
  `X32_TYPELESS_G8X24_UINT` one is stencil (ignore).** (HARD)
* t3 or t4 is `EyeAdaptationTexture` — a 1×1 `PF_A32B32G32R32F`. (HARD)

### 4.3 Which colour SRV is the history — do not guess

Our first-pass claim that "the earlier of t5/t6 is `InputSceneColor`" was **REFUTED** as merely
plausible. Register order is a fxc implementation detail. (SOFT at best.)

**The decisive runtime test** (HARD): `HistoryBuffer[0]` is literally the texture that was
`OutComputeTex[0]` (u0) in the **previous** frame's TAA dispatch, round-tripped through
`FTemporalAAHistory::RT[0]` via `QueueTextureExtraction`.

> Cache the `ID3D12Resource*` bound to u0 each frame. Next frame, the `R16G16B16A16_FLOAT` SRV
> whose resource equals that cached pointer is `HistoryBuffer_0`; **the other one is
> `InputSceneColor`, and that is what goes to `pInColor`.**

On a camera cut or first frame, `HistoryBuffer_0` is instead `GSystemTextures.BlackDummy` — a **1×1**
texture — and `GBufferVelocityTexture` is swapped to BlackDummy too. Seeing a 1×1 texture where
history or velocity should be is a reliable reset signal.

### 4.4 Which permutation Stray runs — now provable

Permutation domain: `<FTAAPassConfigDim, FTAAFastDim, FTAAScreenPercentageDim,
FTAAUpsampleFilteredDim, FTAADownsampleDim>`. `FTAAResponsiveDim` is **not** in the domain.
`ETAAPassConfig`: Main=0, MainUpsampling=1, MainSuperSampling=2, ScreenSpaceReflections=3,
LightShaft=4, DiaphragmDOF=5, DiaphragmDOFUpsampling=6. (HARD)

**Our measured shader binds u1 = `OutComputeTexDownsampled`, which exists only under
`TAA_DOWNSAMPLE`, which requires `bDownsample` → `bUseFast` → `GetPostProcessAAQuality() ==
Medium`. Therefore Stray is running `r.PostProcessAAQuality == 3`, and `0x1708ec956099e259` is the
Main / Fast / Downsample permutation.** (HARD)

Two consequences, both important:

1. **Stray's in-game AA/post quality setting changes the TAA shader hash.** A user raising the
   setting silently breaks a hash-only hook. **Match on more than the hash** — bound-resource
   signature plus dispatch size.
2. **If we skip the dispatch we MUST still produce u1** (a half-res box filter of our output), or
   downstream bloom/DOF read garbage. This is a hard correctness requirement, not a nicety.

The Main config has at most **three** legal permutations — (Fast=0,DS=0), (Fast=1,DS=0),
(Fast=1,DS=1) — so at most three hashes to recognise for non-upsampled TAA. Setting
`r.TemporalAA.AllowDownsampling=0` or `r.PostProcessAAQuality=4` in `Engine.ini` pins a simpler
permutation with no u1, and is worth doing to shrink the state space.

**The second hash `0x52101a15e1a0c5cc` is almost certainly NOT TAA** and should not be hooked.
`FTAAStandaloneCS` has no `R16G16_FLOAT` input in any main config — its only optional extra input,
`InputSceneMetadata`, is `PF_R16F` (single channel) and only bound for DiaphragmDOF. Most likely a
motion-blur pass. Disambiguate by dispatch size and by whether its u0 reappears as an SRV next
frame — only the TAA history does that. (SOFT)

### 4.5 View constant buffer — extended and independently reproduced

The researcher recomputed the float4-row layout from `SceneView.h` and **reproduced all seven of
our measured anchors exactly**, which validates the layout model. Additional verified rows:

| Field | Row | Byte |
|---|---|---|
| `ViewToClip` | 28 | 448 |
| `ViewToClipNoAA` | 32 | 512 |
| `InvDeviceZToWorldZTransform` | 65 | 1040 |
| `ClipToPrevClip` | 122 | 1952 |
| `TemporalAAJitter` | 126 | 2016 |
| `ViewRectMin` | 129 | 2064 |
| `ViewSizeAndInvSize` | 130 | 2080 |
| `BufferSizeAndInvSize` | **132** | **2112** |
| `PreExposure` | **135.y** | **2164** |
| `OneOverPreExposure` | **135.z** | **2168** |
| `NearPlane` | **142.x** | **2272** |
| `DeltaTime` | **143.x** | **2288** |
| `CameraCut` | **145.x** | **2320** |
| `TemporalAAParams` | 152 | 2432 |

Everything we need is in a single **2448-byte prefix**. Rows beyond 152 depend on
`GMaxGlobalDistanceFieldClipmaps` / `MAX_PHYSICS_FIELD_TARGETS` and were **not** verified — do not
use them. (HARD)

**Why we must read b1 and never b0**: UE4 strips the DXBC reflection chunk from shipped shaders
(`D3DCOMPILER_STRIP_REFLECTION_DATA`), so `D3DReflect` is impossible; and loose parameters land in
fxc's implicit `$Globals` at b0, which fxc **compacts by removing unused globals** — e.g.
`MaxViewportUVAndSvPositionToViewportUV` is only read under `AA_LOWER_RESOLUTION` and will be gone
in Main, shifting everything after it. **b0 offsets are unpredictable from source. b1 is an
explicit `cbuffer View` with a layout fixed by `FShaderParametersMetadata`.** (HARD)

### 4.6 Jitter — sign resolved

`View.TemporalAAJitter` is `(CurrentX, CurrentY, PrevX, PrevY)` in **clip/NDC** offset units — the
values added to `ProjectionMatrix.M[2][0]` and `M[2][1]`. `View.TemporalAAParams` is
`(JitterIndex, SequenceLength, TemporalJitterPixels.X, TemporalJitterPixels.Y)`. (HARD)

```
TemporalAAJitter.x = SampleX *  2.0 / ViewRect.Width
TemporalAAJitter.y = SampleY * -2.0 / ViewRect.Height
```

so our note that deriving pixels from `TemporalAAJitter` carries a negative Y is **correct** —
but there is no need to derive anything:

> **`InJitterOffsetX = TemporalAAParams.z` (byte 2440), `InJitterOffsetY = TemporalAAParams.w`
> (byte 2444). Read them and assign straight across. No negation, no scaling.**

`TemporalAAParams.zw` **is** `TemporalJitterPixels`, already in render-resolution pixels, and
NVIDIA's own UE plugin passes it to NGX unmodified.

### 4.7 ClipToPrevClip and camera-motion reconstruction

`ClipToPrevClip = InvViewProj * PrevViewProj`, built from **jitter-free (NoAA)** projection matrices
and including the pre-view-translation delta. It maps **unjittered** current clip to **unjittered**
previous clip, so camera MVs derived from it are jitter-free — exactly what DLSS wants. **No jitter
compensation needed.** (HARD)

UE4 compiles with `D3D10_SHADER_PACK_MATRIX_ROW_MAJOR`, so cb row *r* at byte `1952 + 16r` is
`M[r][0..3]` and HLSL `mul(v, M)` is the **row-vector** convention. Reading it into a
column-vector-convention C++/GLM matrix requires a transpose. (HARD)

UE4's own reconstruction, which our resolve pass must reproduce for pixels with no object velocity:

```hlsl
PosN.xy   = ViewportUVToScreenPos(ViewportUV);   // (2u-1, 1-2v)
PosN.z    = dilated depth (max over a 3x3 cross, because reversed-Z)
ThisClip  = float4(PosN.xy, PosN.z, 1);
PrevClip  = mul(ThisClip, View.ClipToPrevClip);
PrevScreen= PrevClip.xy / PrevClip.w;
BackN     = PosN.xy - PrevScreen;                // NDC delta, viewport width = 2, Y-up
```

### 4.8 Velocity

* Format is `PF_A16B16G16R16` = `R16G16B16A16_UNORM`, at the **full scene-buffer extent**
  (`BufferSizeAndInvSize`, row 132), **not** the view size. Index it with absolute buffer coords
  (`ViewRectMin + local`). (HARD)
* Cleared to 0 (`FClearValueBinding::Transparent`). **The validity test in the TAA shader is
  literally `EncodedVelocity.x > 0.0` — strict, on the RED channel only.** NVIDIA's guide snippet
  uses the two-component `all(Velocity.xy > 0)`; the DLSS-4-era plugin narrowed to `.x > 0.0f`.
  **Reproduce the exact test; do not use a magnitude threshold.** (HARD)
* Stored value is the screen-space delta `ScreenPos - PrevScreenPos` in NDC with **both** frames'
  jitter removed — directly comparable to the `ClipToPrevClip`-derived `BackN`. Both branches are
  jitter-free and both are current-minus-previous, so they get negated and scaled identically.
  (HARD)
* Decode constants confirmed: `InvDiv = 1/(0.499*0.5) = 4.008016032064128`, bias
  `= (32767/65535) * InvDiv`. **Compute the bias in float from the expression; do not hardcode a
  rounded constant.** Note `DecodeVelocityFromTexture` takes a `float4` and returns a `float3`.

### 4.9 Depth and pre-exposure

Reversed-Z with an **infinite far plane**: `HAS_INVERTED_Z_BUFFER = 1`, near = 1.0, far = 0.0,
`M[2][2] == 0`, `M[3][2] == GNearClippingPlane`, `SceneDepth = Near / DeviceZ`. The TAA shader
hard-`#error`s on non-inverted Z. Read `View.NearPlane` from row 142.x rather than assuming 10.0
uu; treat "far" as effectively infinite. (HARD)

`r.UsePreExposure` defaults to 1 and is `ECVF_ReadOnly`. Scene colour entering TAA **carries
pre-exposure**, and the TAA output (and thus the history) stays in pre-exposed space. Feed
`View.PreExposure` (row 135.y) to `InPreExposure` and write the DLSS output back still pre-exposed.
(HARD)

### 4.10 Camera cut

`const bool bCameraCut = !InputHistory.IsValid() || View.bCameraCut;` — **the history-invalid case
is NOT reflected in the View uniform buffer.** So OR three signals for `InReset`:

1. `View.CameraCut != 0` (row 145.x, byte 2320);
2. `TemporalAAJitter.zw == TemporalAAJitter.xy` (our own measured heuristic);
3. **the history or velocity SRV is a 1×1 texture** — the most reliable, because it directly
   reflects `!InputHistory.IsValid()`.

(HARD)

### 4.11 Console variables for the SR phase

All settable from `Engine.ini` `[SystemSettings]`, which is applied with
`ECVF_SetBySystemSettingsIni` at startup — **the only way to set the `ECVF_ReadOnly` ones**
(`r.UsePreExposure`, `r.D3D12.Depth24Bit`). Command line does not work on this title. (HARD)

| CVar | Default | Note |
|---|---|---|
| `r.ScreenPercentage` | 100.0 | |
| `r.TemporalAA.Upsampling` | 0 | **1 → `ETAAPassConfig::MainUpsampling`** |
| `r.SecondaryScreenPercentage.GameViewport` | 0 | |
| `r.TemporalAA.Upscaler` | 1 | |
| `r.TemporalAA.HistoryScreenPercentage` | 100.0 | **>100 switches to MainSuperSampling — leave at 100** |
| `r.TemporalAA.AllowDownsampling` | 1 | 0 removes the u1 output |
| `r.TemporalAA.Algorithm` | 0 | **must stay 0** |
| `r.TemporalAASamples` | 8 | raise for SR modes (Perf needs 32) |
| `r.PostProcessAAQuality` | — | Stray runs 3 (Medium) |

**`r.TemporalAA.Algorithm` must stay 0.** Setting it to 1 (Gen5) replaces the single
`FTAAStandaloneCS` dispatch with a chain of six shaders (`TAAClearPrevTextures`,
`TAADilateVelocity`, `TAADecimateHistory`, `TAAFilterFrequencies`, `TAACompareHistory`,
`TAADilateRejection`, `TAAUpdateHistory`) and **breaks the hook entirely**. (HARD)

### 4.12 The right way to do super resolution

Set `r.ScreenPercentage=<N>` **and** `r.TemporalAA.Upsampling=1` in `Engine.ini [SystemSettings]`.
Then:

* `PrimaryScreenPercentageMethod` becomes `TemporalUpscale`, and TemporalAA.cpp selects
  `MainUpsampling`.
* `InputViewRect` = render res, `OutputViewRect` = `(0,0)..GetSecondaryViewRectSize()` = display
  res, and `FTAAPassParameters::GetOutputExtent()` allocates a **full-res output texture**. The
  engine now *expects* the TAA pass to upscale — **exactly DLSS's contract** — and every downstream
  pass already uses the upscaled rect.
* **Bonus 1**: the jitter sequence switches to **plain Halton in [-0.5, 0.5]** with no Box-Muller
  warp (`SceneVisibility.cpp:3241-3242`) — which is what DLSS was trained on. In the shipped
  non-upsampling mode UE4 warps Halton through Box-Muller with `sigma = 0.47 *
  r.TemporalAAFilterSize`, a distribution DLSS was **not** trained on.
* **Bonus 2**: the same branch auto-scales phase count by `1/fraction²`, reproducing NVIDIA's
  `Base * (Target/Render)²` formula for free.
* **Bonus 3**: `View.MaterialTextureMipBias = -max(-log2(fraction), 0) + r.MinAutomaticViewMipBiasOffset`
  — correct texture LOD bias for free.
* **Cost**: this changes the permutation and therefore the DXBC hash. `0x1708ec956099e259` will no
  longer appear. Plan to re-derive it or match structurally.

(HARD)

In `Main` (the shipped config) input and output rects are **identical** and equal to
`View.ViewRect`; `FTAAPassParameters::SetupViewRect` sets them equal and `Validate()` asserts it.
`InputViewMin`/`InputViewSize`/`TemporalJitterPixels`/`ScreenPercentage`/`UpscaleFactor` exist in
the shader **only under `#if AA_UPSAMPLE`** — they are not in Stray's current shader at all. Get the
render rect from View CB rows 129/130 instead. UE4 shifts all view rects to the buffer top-left
after screen-percentage resolution, so `ViewRectMin` is almost always (0,0) in a single-view game —
**but read it, do not assume it.** (HARD)

---

## 5. Build and CI

* `windows-latest` / `windows-2025` / `windows-2025-vs2026` all now resolve to **Windows Server
  2025 + Visual Studio Enterprise 2026 (18.9.12112.369)**, CMake 4.4.2, Ninja 1.13.2, LLVM 20.1.8,
  Windows 11 SDK 10.0.26100.0. Migration ran 2026-06-08 → 2026-06-15. **Trap**: a stale
  `Windows2025-Readme.md` still exists in the runner-images repo showing VS 2022 — cite the README
  label table, not that file. (HARD)
* The image is **not** locked to the newest toolset: `Microsoft.VisualStudio.Component.VC.14.44.17.14.x86.x64`
  is installed "for compatibility", so **MSVC v143 (14.44) is available** via
  `-T v143,version=14.44`. (HARD)
* **Build Release `/MD` and link plain `nvsdk_ngx_d.lib`.** The NGX libs are real static libraries
  exposing C++ std types across the ABI; a mismatched `_ITERATOR_DEBUG_LEVEL` produces link errors
  on the runner. (This corrects the earlier `/MT` preference — `/MT` would require
  `nvsdk_ngx_s.lib`, and ReShade add-ons are conventionally `/MD`.)
* Precompile every HLSL shader to DXBC at build time on the runner. **Never call `D3DCompile` at
  runtime** — OptiScaler's most common Linux failure is exactly this, because `d3dcompiler_47` is
  often absent from the prefix.
* Ship PDBs as artifacts (`/Zi /DEBUG /OPT:REF /OPT:ICF`) so a field crash can be symbolised.

---

## 6. Prior art worth copying from

* **Luma-Framework** (`Filoppi/Luma-Framework`) — a ReShade add-on framework that injects DLSS into
  DX11 games. `Source/Core/includes/super_resolution.h` (the `SR::SuperResolutionImpl` interface),
  `Source/Core/dlss/DLSS.cpp` (full NGX lifecycle), `Source/Core/fsr/FSR.cpp` (same interface, FSR
  backend), `.github/workflows/build_and_release.yml`. **DX11 only** — the NGX entry points,
  resource states and descriptor handling all differ for us.
* **NVIDIA's DLSS-for-Unreal plugin** — the canonical UE conventions. `VelocityCombine.usf` is the
  authoritative dense-MV resolve. Public mirrors exist; the DLSS-4-era NGX headers live at
  `Plugins/DLSS/Source/ThirdParty/NGX/Include/`.
* **OptiScaler** (`optiscaler/OptiScaler`) — deepest well of Proton-specific upscaler knowledge, and
  a fallback distribution shape (inject as `d3d12.dll`) if the ReShade route ever proves untenable.
  Version-pin any claim about it; its working-mode list changes between releases.
* **ReShade's own `examples/utils/`** — `descriptor_tracking.{hpp,cpp}` and `state_tracking.cpp`.
  Copy verbatim.

Two things checked and found **not** to be useful prior art: `RankFTW/RHI` is a deployment tool, not
a DLSS injector; and PureDark's Skyrim Upscaler's ReShade-addon code is 5.x-era and will not compile
against 6.8 (it uses the removed `reshade::log_message` and treats `generic_depth_data` as swapchain
private data, which moved to the effect runtime in 6.x).
