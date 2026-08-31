# Upstream ReShade bug: the vkd3d ext hook converts descriptor handles it never minted

Status: **stage 1 (reproduce) and stage 2 (fix) both complete, with CI evidence.**
This is the upstream counterpart to our own `src/ext_unhook.{hpp,cpp}` workaround.

Fork and branches: <https://github.com/lunks/reshade>

| Branch | Contents | CI run | `test` | `test-msvc` | `build` (full ReShade) |
|---|---|---|---|---|---|
| `repro/d3d12-vkd3d-ext-descriptor-handle` | testable extraction of the real conversion + the failing test | [33447001336](https://github.com/lunks/reshade/actions/runs/33447001336) | **failure** | **failure** | success |
| `fix/d3d12-vkd3d-ext-descriptor-handle` | the same test + the fix | [33447082863](https://github.com/lunks/reshade/actions/runs/33447082863) | **success** | **success** | success |

The `build` job is upstream's own `msbuild ReShade.sln` for both 32-bit and 64-bit. It passes on
**both** branches, so the extraction and the fix each compile into the real DLL; only the test job
distinguishes them.

Both branch from upstream `main` at `ec0346e035b7d1c267103ea0d7c231b3945fc2b1`
("Add support for ID3D12DeviceExt3/4/5 from vkd3d", 2026-08-31).

---

## 1. The bug, verified against ReShade's own source

### 1.1 Where the hook is installed

`source/d3d12/d3d12_device.cpp:125-146`, inside `D3D12Device::QueryInterface`. When anything asks
ReShade's proxy device for `IID_ID3D12DeviceExt` / `Ext1` … `Ext5`, it forwards to the original
device and then patches the returned interface's vtable — slots 7/8, plus 14/15 for `Ext2` and up.

### 1.2 What the hook does

`source/d3d12/d3d12_extensions.cpp` — all four entry points have the same shape:

```cpp
com_ptr<ID3D12Device> device;
device_ext->QueryInterface(IID_PPV_ARGS(&device));

if (const auto device_proxy = get_private_pointer_d3dx<D3D12Device>(device.get()))
{
    srv_handle = device_proxy->convert_to_original_cpu_descriptor_handle(srv_handle);
    ...
}
```

### 1.3 Two independent reasons a foreign handle reaches that conversion

Both read directly out of ReShade's source; either alone is sufficient.

1. **The vtable is shared.** vkd3d-proton uses a single static vtable for `ID3D12DeviceExt`, so
   patching it from ReShade's proxy patches it for *every* holder of that interface in the process,
   including one obtained straight from the original device. (Measured independently on the RTX 4090
   by our own `test_vkd3d_ext_hook_reachability`: `before=…C645AD0 after=…D5680D0`, the patched entry
   belonging to ReShade's DLL.)

2. **The proxy lookup succeeds regardless of which device the caller holds.** `D3D12Device`'s
   constructor does `_orig->SetPrivateData(__uuidof(D3D12Device), sizeof(device_proxy), &device_proxy);`
   (`source/d3d12/d3d12_device.cpp:34`) — it registers itself on the **original** device. So the
   `QueryInterface` + `get_private_pointer_d3dx` dance in the hook finds the proxy even when the
   caller only ever touched the native device. This one is a property of ReShade alone and would
   still bite even if vkd3d used per-instance vtables.

Under Proton the caller that matters is NVIDIA's `nvngx_dlss.dll`, which makes its own descriptors on
whichever device it was initialised with. Initialise NGX with `device::get_native()` — which §1 of
`CLAUDE.md` requires, and which is the only configuration that does not crash vkd3d — and every
handle it passes is a real vkd3d handle.

### 1.4 The conversion has no release-build validation

`source/d3d12/d3d12_impl_device.cpp:2325-2331`:

```cpp
const size_t heap_index = (handle.ptr >> heap_index_start) & 0xFFFFFFF;
assert(heap_index < _descriptor_heaps.size() && _descriptor_heaps[heap_index] != nullptr);

return { _descriptor_heaps[heap_index]->_orig_base_cpu_handle.ptr + (handle.ptr & (((1ull << heap_index_start) - 1) ^ 0x7)) };
```

`assert` is a no-op under `NDEBUG`, and ReShade ships Release. The handle layout it assumes is minted
in `D3D12DescriptorHeap::initialize_descriptor_base_handle` (`d3d12_impl_device.cpp:2274`):

```
Bit  0 -  1: Heap type
Bit  2 -  2: Heap flags
Bit  3 - 27: Descriptor offset in bytes
Bit 28 - 55: Heap index          (bit 24+ on 32-bit builds)
Bit 56 - 63: Extra data
```

A real vkd3d CPU descriptor handle is an ordinary host pointer, so `heap_index` comes out as garbage.
Two distinct failure modes follow, and the test covers both:

* **Out of range** — `_descriptor_heaps` is subscripted past its end. Measured decodes from the test:
  `0x00007F9C4A3B2000` → heap index **522692**, `0x000001A2B3C40000` → **6699**, against a handful of
  registered heaps.
* **Accidentally in range** — e.g. `0x0000000000410000` → heap index **0**. No out-of-bounds access
  at all, just a valid-looking handle for a completely unrelated descriptor, returned silently.

### 1.5 Observed consequence on the target

Measured on the user's machine (RTX 4090, vkd3d-proton 3.1.0, DXVK-NVAPI 0.9.2, ReShade 6.8.0.2155):
`NvAPI_D3D12_GetCudaMergedTextureSamplerObject` and `GetCudaIndependentDescriptorObject` fail with
**nvapi status -5** (`NVAPI_INVALID_ARGUMENT`) on every evaluate, while
`NVSDK_NGX_D3D12_EvaluateFeature` still reports success and the DLSS output resource is never written
— proven by `NgxDumpInputs=1`: changing colour/depth inputs, byte-identical all-zero 66 MB output
across 300 evaluates.

So on this stack the failure is a loud rejection rather than a silently sampled wrong texture. The
silent variant (§1.4, "accidentally in range") remains possible and is the more dangerous one.

### 1.6 Introducing commits — verified against the real history

| Commit | Date | Author | Subject |
|---|---|---|---|
| `9b2815e48e55393d478fda21f1b3f31d49336298` | 2026-01-10 | crosire | Fix crash with vkd3d and DLSS |
| `d0c3cd6f5c305bf1e90343db9c40c5e0b3b45850` | 2026-01-11 | crosire | Reduce overhead of D3D12 CPU descriptor handle conversion |
| `50319382618a53072a9e92ca3509caa2c7051f48` | 2026-03-02 | crosire | Add support for "ID3D12DeviceExt2" from vkd3d |

The two commits cited in `docs/RESEARCH.md` §1.1 are confirmed exactly. `d0c3cd6f` is a third,
previously unrecorded one: it factored the conversion into the single-argument overload that exists
today. **No version of this code ever had a release-build bounds check** — `git show 9b2815e4` shows
the original went through `convert_to_descriptor_table` + the table overload, which had the same bare
`assert`. `d3d12_extensions.cpp` has exactly three commits in its history; all three are listed above.

---

## 2. The reproduction

The bug is pure pointer arithmetic, so it needs no game, no GPU and no vkd3d. The problem was making
a test that exercises *ReShade's real code* rather than a copy of it, given that upstream has no test
harness and the logic was buried inside `device_impl`.

Solution, in two commits:

1. **`Move D3D12 CPU descriptor handle encoding into a separate header`** — a behaviour-preserving
   extraction. The bit layout, the heap-index decode and the offset math move into the new
   `source/d3d12/d3d12_descriptor_handle.hpp`, which depends only on the standard library.
   `device_impl::convert_to_original_cpu_descriptor_handle` is rewritten to call it, keeping the
   unchecked `_descriptor_heaps` subscript exactly as it was. `D3D12DescriptorHeap` also starts
   recording its descriptor size and total size, which the fix needs.

   One substantive detail: upstream selects the heap-index bit position with `#ifndef _WIN64`. The
   header uses `#if SIZE_MAX > 0xFFFFFFFFull` instead so it also compiles off-Windows, with a
   `static_assert(heap_index_start == (sizeof(size_t) > 4 ? 28 : 24))` guarding the equivalence.
   CI confirms both: 64-bit MSVC reports `heap_index_start = 28`, 32-bit MSVC reports `24`.

2. **`Add failing test for foreign handles in the vkd3d extension hooks`** —
   `tests/d3d12_descriptor_handle_test.cpp` plus a workflow. The test builds a heap table the way
   ReShade builds one, then drives the **real** `convert_internal_cpu_descriptor_handle` from the
   header. It is compiled with `NDEBUG`, because release behaviour is precisely what is under test.

   The test file is **byte-identical on both branches** — `git diff repro fix` does not list it.

What it asserts:

* `test_minted_handles_round_trip` — handles ReShade did mint still convert correctly. This is the
  guard against the fix breaking the thing the original commits were added to do.
* `test_heap_index_is_extracted_correctly` — the decode matches the documented layout.
* `test_foreign_handles_are_not_converted` — the reproduction. Five realistic vkd3d/Wine/Windows heap
  pointers must be reported as unconvertible so the caller can pass them through untouched.
* `test_foreign_handle_with_in_range_heap_index` — the silent case: a foreign handle whose bits happen
  to decode to a registered heap index must also be rejected.

### Failing on the repro branch, in CI

Run <https://github.com/lunks/reshade/actions/runs/33447001336> — `test` **failure**, `test-msvc`
**failure**, 12 checks failing in each. Verbatim from the Linux job:

```
handle 0x00007f9c4a3b2000 -> heap index 522692 (of 4 registered heaps) (would be an out-of-bounds read of _descriptor_heaps)
  FAIL tests/d3d12_descriptor_handle_test.cpp:182: !did_convert
  FAIL tests/d3d12_descriptor_handle_test.cpp:185: converted == 0
handle 0x00007f1234567890 -> heap index 520483 (of 4 registered heaps) (would be an out-of-bounds read of _descriptor_heaps)
  ...
handle 0x0000000000410000 -> heap index 0 (of 4 registered heaps)
  FAIL tests/d3d12_descriptor_handle_test.cpp:182: !did_convert
  FAIL tests/d3d12_descriptor_handle_test.cpp:185: converted == 0
12 check(s) FAILED
```

Note the last one: heap index 0 is *in range*, so nothing reads out of bounds — the conversion just
silently returns a handle for the wrong descriptor. That is the failure mode that produces a wrong
image with no diagnostic.

---

## 3. The fix

Commit `Only convert descriptor handles that ReShade generated`. This is option (a) from the brief —
recognise our own synthetic handles, pass everything else through — plus the release-build bounds
check. The wrapper approach (option b) was not needed.

**1. A validating predicate** (`d3d12_descriptor_handle.hpp`):

```cpp
inline bool is_internal_cpu_descriptor_handle(uint64_t handle, const descriptor_heap_handle_info &heap)
{
    if (handle < heap.internal_base_handle)
        return false;

    const uint64_t offset = handle - heap.internal_base_handle;

    // The heap index, heap type and heap flag bits all have to match exactly, only the descriptor offset bits may differ
    if ((offset & ~descriptor_offset_mask) != 0)
        return false;

    // And the descriptor has to actually exist in the heap
    if (heap.handle_size == 0 || offset >= heap.size_in_bytes || (offset % heap.handle_size) != 0)
        return false;

    return true;
}
```

Subtracting the heap's own internal base handle checks the heap-index, heap-type and heap-flag bits
in one go; the remaining offset then has to be a real descriptor offset inside that heap — in range
and a whole multiple of the descriptor size. That is what rejects the dangerous in-range case, which
a bounds check alone would let through.

**2. A bounds-checked lookup** and a pass-through entry point (`d3d12_impl_device.cpp`):

```cpp
bool reshade::d3d12::device_impl::try_convert_to_original_cpu_descriptor_handle(D3D12_CPU_DESCRIPTOR_HANDLE *handle) const
```

The existing value-returning overload now delegates to it and, on failure, returns the handle
**unchanged** instead of reading out of bounds — a strict improvement at every one of its call sites.
It keeps an `assert(false)`, because reaching it with an unconvertible handle from any caller other
than the extension hooks really is a bug.

**3. The extension hooks** use `try_convert_...` and leave unrecognised handles alone:

```cpp
device_proxy->try_convert_to_original_cpu_descriptor_handle(&srv_handle);
device_proxy->try_convert_to_original_cpu_descriptor_handle(&sampler_handle);
```

### Why this does not regress the original DLSS fix

The case `9b2815e4` was added for is a game whose descriptors were created through ReShade's proxy
heap. Those handles satisfy every clause of the predicate by construction, so they still convert
exactly as before — asserted by `test_minted_handles_round_trip`.

### Why false positives are not a concern at the other call sites

Checked against the source: `D3D12Device::CreateDescriptorHeap` only wraps heaps with
`Type <= D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER`, so RTV and DSV heaps hand out real handles — and
correspondingly `CreateRenderTargetView` / `CreateDepthStencilView` never call the conversion. Every
existing call site is on a CBV/SRV/UAV or sampler path, i.e. a wrapped heap. And
`register_descriptor_heap` always calls `initialize_descriptor_base_handle` immediately after
`push_back`, so the new size fields are never left zero for a registered heap.

### Green on the fix branch, in CI

Run <https://github.com/lunks/reshade/actions/runs/33447082863> — `test` **success**, `test-msvc`
**success**:

```
heap_index_start = 28, descriptor_offset_mask = 0x000000000ffffff8
all checks passed
heap_index_start = 24, descriptor_offset_mask = 0x0000000000fffff8
all checks passed
```

(64-bit then 32-bit MSVC.)

The `build` job on the same run compiles ReShade itself, 32-bit and 64-bit, with the fix in place.

---

## 4. Scope note — what was deliberately left alone

Two other places decode the same bit layout with the same bare `assert` and no release-build check:

* `device_impl::get_descriptor_heap_offset` (`d3d12_impl_device.cpp:1640`)
* `device_impl::convert_to_original_gpu_descriptor_handle` (`d3d12_impl_device.cpp:2396`)

Both operate on `api::descriptor_table` handles that come from ReShade's own add-on API rather than
from the vkd3d extension hooks, so neither is on this bug's path. They were left untouched to keep
the diff minimal, and are worth mentioning to a maintainer as follow-up hardening.

---

## 5. Is this upstreamable? Yes — but submit the minimal branch

All figures below were verified against the GitHub API and the repo, not assumed.

### The repo does merge outside work

| | |
|---|---|
| Total PRs | 406 |
| Merged | **276** |
| Merged that were authored by crosire himself | **1** |
| Median open → merge latency | **2 days** (45% within 1 day, 77% within 7) |
| Merges in 2026 so far | 20 |

So 275 of 276 merged PRs came from outside contributors. Not a dormant or closed repo.

* **GitHub Issues are hard-disabled** (`has_issues: false`; the API returns HTTP 410 *"Issues are
  disabled for this repo"*). The README points bug reports at the
  [ReShade forum](https://reshade.me/forum) and Discord. **A PR body is therefore the bug report.**
* **No CONTRIBUTING.md, no CLA, no bot, no PR/issue templates.** `.github/` contains only
  `workflows/build.yml`. The README's entire policy is *"Any contributions to the project are
  welcomed, it's recommended to use GitHub pull requests."* Licence is BSD-3-Clause; inbound=outbound
  is implicit via GitHub ToS, and file headers stay `Copyright (C) <year> Patrick Mours`, so editing
  `d3d12_extensions.cpp` needs no header change.
* **Style: `.editorconfig` exists** — `indent_style = tab`, Allman braces, `IUnknown *device_ext`
  pointer alignment. No `.clang-format`, no formatting CI. `snake_case` functions/locals, `_leading`
  underscore members, `Interface_Method` hook thunks. Our diff matches all of this.
* **Commit subjects**: imperative, capitalised, **no trailing period** (0/660 since 2025), **no
  conventional-commit prefixes** (0/660), identifiers wrapped in double quotes (134/660). crosire
  squash-merges, keeps the contributor as author, and rewrites the subject himself with `(#NNN)`
  appended.

### The decisive constraint: do not bundle the tests

**Upstream has no test infrastructure at all.** No `tests/` directory, no test project in
`ReShade.sln`, no test target in CMake, and `.github/workflows/build.yml` is the only workflow — it
runs `msbuild` and uploads artifacts, and runs no tests. The one thing that exists is
`source/dll_main_test_app.cpp`, an interactive smoke-test EXE behind `RESHADE_TEST_APPLICATION`.

Merged PRs are consistently 1–15 lines. Out-of-scope PRs get closed fast and silently — [#434](https://github.com/crosire/reshade/pull/434)
was opened and closed **two minutes later with no comment**, on 2026-08-31.

So the `repro` and `fix` branches in §2–§3, which add a `tests/` directory and a second workflow, are
the right shape for *proving the bug to ourselves* and the wrong shape for a PR.

### The branch to actually submit

`pr/d3d12-vkd3d-ext-descriptor-handle` — branched clean from upstream `main`, **one commit, 4 files,
+48/−9**, no new header, no tests, no workflow:

```
source/d3d12/d3d12_descriptor_heap.hpp |  2 ++
source/d3d12/d3d12_extensions.cpp      | 17 ++++++++++------
source/d3d12/d3d12_impl_device.cpp     | 36 +++++++++++++++++++++++++++++++---
source/d3d12/d3d12_impl_device.hpp     |  2 ++
```

Same fix, validation inlined into `device_impl` instead of the extracted header. `ci/pr-minimal-verify`
is that branch plus a build-only workflow, purely so the compile can be verified in Actions:
[run 33447938388](https://github.com/lunks/reshade/actions/runs/33447938388) — **success**, 32-bit and
64-bit. The inlined transcription was additionally checked locally against the same handle set as the
unit test, to confirm it behaves identically to the CI-tested version on the `fix` branch.

### Timing is unusually good

`ec0346e0` — *"Add support for "ID3D12DeviceExt3/4/5" from vkd3d"* — landed **2026-08-31**, the same
day this work was done, touching this exact subsystem. crosire also engaged substantively with the
adjacent crash on a third-party tracker ([vkd3d-proton#2785](https://github.com/HansKristian-Work/vkd3d-proton/issues/2785)),
so the context is loaded.

### This bug is unreported

Searched: `repo:crosire/reshade vkd3d` → 0 results; `convert_to_original_cpu_descriptor_handle`
across all of GitHub issues → 0; `"d3d12_extensions"` → 0. The code search's 51 hits are all ReShade
itself or vendored copies — plus `lunks/stray-dlss`, i.e. our own repo.

What *is* reported is the **crash** variant, and it is already fixed. crosire's own diagnosis on
vkd3d-proton#2785, verbatim:

> "DLSS is calling `NvAPI_D3D12_GetCudaTextureObject`/`NvAPI_D3D12_GetCudaSurfaceObject` **with a
> descriptor handle from the game**. With normal NVAPI this works, since under the hood it actually
> calls into the ReShade proxy device again and the descriptor handles thus get converted properly."

*"a descriptor handle from the game"* is precisely the assumption the code encodes. An add-on that
creates descriptors on `device::get_native()` and hands them to NGX violates it — and the guard only
asks "does a ReShade proxy exist for this device?", never "did ReShade mint this handle?".

Corroborating prior art: [NIGos/dlss5-bridge](https://github.com/NIGos/dlss5-bridge), the leading
NGX-calling ReShade add-on, states in its release notes that *"The D3D12 device handed to NGX is
ReShade's underlying device, not its proxy."* — deliberately choosing the configuration that breaks
under vkd3d, with the add-on scoped to Windows. Nobody has hit and diagnosed this yet.

### Before filing

* **Strip the `Claude-Session:` trailer** from the commit message — it is a local convention of this
  repo, not something to send upstream.
* Lead the PR body with §1.3 (both source-level reasons, citable to crosire's own line numbers), then
  §1.5 (the measurement), then the fix. crosire cannot reproduce this himself — it needs Linux,
  vkd3d-proton and an NVIDIA GPU — so the body has to carry the whole argument.
* The review's whole question will be *"does this regress the game-minted path `9b2815e4` protects?"*.
  Answer it up front: it does not, and the `fix` branch's `test_minted_handles_round_trip` is the
  evidence, offered separately if wanted.
* Consider cross-posting to [vkd3d-proton#2797](https://github.com/HansKristian-Work/vkd3d-proton/issues/2797)
  or the forum — that is where the earlier instance of this bug actually got his attention.

**No upstream PR has been opened.** That is a judgement call for a human, so the branches are pushed
and ready but nothing has been filed against `crosire/reshade`.

---

## 6. Consequence for stray-dlss

`src/ext_unhook.{hpp,cpp}` restores the pristine vtable slots before every NGX call. It stays
necessary for every shipping ReShade build, since even a merged fix would only appear in a future
release. If the fix lands, `ext_unhook` can be gated on the ReShade version rather than removed —
and the repair is already a no-op when it finds the slots unpatched, so it costs nothing to keep.

This bug is also **exactly why the "native device" rule in §1 of `CLAUDE.md` is not sufficient on its
own**: native-device NGX and the ext hook are individually fine and jointly broken.
