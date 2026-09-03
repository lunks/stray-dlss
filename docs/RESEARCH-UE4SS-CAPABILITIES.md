# What UE4SS offers that this project is not using

Assessment written 2026-09-03. Read-only research; no plugin code touched.

**Question:** we built a lot by hand — signature/shape scanning, vtable patching, D3D12 hooks, a
descriptor shadow, our own ini parser, our own file logger. Some of that might duplicate something
UE4SS already provides; some of what UE4SS provides might not fit us anyway. This is the audit.

Provenance labels follow CLAUDE.md's convention: **HARD** = read directly from source at the
pinned commit; **SOFT** = reasoned from HARD facts; **[derived]** = a consequence I worked out,
not stated anywhere; **UNCONFIRMED** = plausible but not established here.

## Method and a provenance caveat, stated once

`RE-UE4SS` was fetched as GitHub's tarball export of commit `68caddcf36804b7405424e03e911e6b278324f47`
(`curl -sL https://github.com/UE4SS-RE/RE-UE4SS/archive/68caddcf.tar.gz`), which reproduces the
tree exactly but — like any tarball export — omits submodule content. Two paths are submodules
(`.gitmodules`, HARD): `deps/first/patternsleuth` (public) and **`deps/first/Unreal`**, which is
the private, Epic-membership-gated `Re-UE4SS/UEPseudo` — the tree that defines `UObject`,
`UObjectGlobals`, every `FProperty` subclass, and everything else under the `Unreal/` include
prefix that our own mods `#include`.

Everything cited under `deps/first/Unreal/**` in this document therefore comes from the same
**public mirror this project's own CI already uses as a fallback** —
`.github/workflows/dlss-plugin.yml:67-74`: `FottenSC/HorseMod@e6313df89272949081e298aab1579aeaf2d5866a`,
subdirectory `RE-UE4SS/deps/first/Unreal`. Per that workflow's own comments (`:277-290`), this
mirror is dated **~2026-07-01** and is missing exactly two things relative to `68caddcf`:
`Unreal/ScriptContainerLayout.hpp` and `FSoftObjectPath::GetSubPathString`. None of my citations
touch either. I mark every citation into this tree with **(mirror)** so it is never confused with
a citation into the primary tarball, which is the real `68caddcf` byte-for-byte.

## Verdict

**We are using UE4SS about right.** The two biggest structural questions this project has
carried — "does UE4SS give us a way to hook a non-reflected native engine function" and "can its
SDK tooling generate correct layouts for Renderer-private types" — both resolve to **no**, cleanly
and confirmably, closing them rather than opening new work. The `ConsoleManagerSingleton` loose
end resolves to the same answer: UE4SS scans for the address and then does nothing with it.

What is left is small: one real process improvement worth adopting (a startup cross-check against
`UE4SSRuntime`'s own hook-availability flags), one optional tool worth knowing about for
interactive local debugging (UE4SS's own ImGui tab system) that does **not** solve the problem it
might look like it solves, and a short list of libraries that exist, are technically reachable,
and are correctly not worth the churn given this project's own SDK-free-core design rule.

---

## 1. What we could adopt

Ranked by value. Nothing here is large.

### 1.1 Cross-check our hooks against `UE4SSRuntime`'s own availability flags — S effort, no risk, real value

`UE4SS/include/UE4SSRuntime.hpp:33,43,51,58` (HARD) exposes four static queries a C++ mod can call
at any time:

```cpp
static auto IsEngineTickAvailable() -> bool;          // UEngine::Tick AOB found
static auto IsProcessEventAvailable() -> bool;         // UObject::ProcessEvent AOB found
static auto IsFUObjectHashTablesAvailable() -> bool;    // hash-table object lookup usable
static auto ShouldUseHashTableIteration() -> bool;      // whether FindFirstOf/ForEachUObject use it
```

The header's own comment (`:39-42`) says `IsProcessEventAvailable()` is a **precondition for
`RegisterHook` working at all**. We already depend on `RegisterHook` for every native and
Blueprint hook in `StrayDualSense` (`mods/StrayDualSense/src/Mod.cpp:917-943`), and diagnose a
missing hook today only from its own `RegisterHook threw` catch block (`:943`) — which fires only
if the specific `UFunction*` could not be found, not if the whole `ProcessEvent` hook substrate
failed to install on this game build. Logging `IsProcessEventAvailable()` once at `on_unreal_init`
turns "which of N hooks is silently missing" into "is the hook substrate even up," for the cost of
one log line. Cheap, low-risk, and it is exactly the kind of loud-failure-over-silent-gap this
project's CLAUDE.md prime directive 2 asks for.

### 1.2 `register_tab` / `render_tab` — a live ImGui panel, useful for a different job than it looks like

`UE4SS/include/Mod/CppUserModBase.hpp:140,212` (HARD): every `CppUserModBase` can call
`register_tab(name, RenderFunctionType)` and get a tab in UE4SS's own debug GUI, rendered from
`render_tab()`/the registered callback. `cppmods/EventViewerMod/src/EventViewer.cpp:22-32` (HARD)
is a working example: a plain ImGui callback disabled until Unreal is initialised.

**This is not the in-game overlay `docs/RESEARCH-UE4SS-MIGRATION.md` §5.1 is short one of.** UE4SS's
GUI is its own window, built on its own D3D11 or GLFW/OpenGL3 backend and its own Win32 window
(`UE4SS/src/GUI/DX11.cpp`, `UE4SS/src/GUI/GLFW3_OpenGL3.cpp`, `UE4SS/src/GUI/Windows.cpp` — HARD,
confirmed by grepping for swapchain/window creation in `UE4SS/src/GUI/`). It is **not** composited
onto the game's D3D12 back buffer, so it will not appear in a `gamescopectl screenshot` or any
other capture of the game's own presented frame — the exact channel this project's remote,
no-display workflow depends on (CLAUDE.md §2.11, `tools/screenshot-gamescope.sh`). Its value is
real but narrower: a live, interactive view of our own status counters and config toggles **for a
human sitting at the box with a display attached**, which this project's workflow mostly is not.
Worth building only if that workflow changes; not a fix for the screenshot-channel regression.

### 1.3 `on_cpp_mods_loaded()` / `on_dll_load()` — available, currently unused, no clear need yet

`CppUserModBase.hpp:136,207` (HARD). `on_dll_load(dll_name)` fires for every DLL UE4SS observes
loading into the process — a free way to log whether ReShade's `dxgi.dll` is present this session,
which today we only infer indirectly. `on_cpp_mods_loaded()` fires once every C++ mod has started,
useful only if `StrayDLSS` and `StrayDualSense` ever need to coordinate startup order. Neither
solves an open problem today; both are one-line overrides if a need shows up. Noted so nobody
re-derives their existence later.

### 1.4 Everything else technically reachable, correctly not adopted

`UE4SS/CMakeLists.txt:137-146` links `SinglePassSigScanner`, `Function`, `IniParser`, `JSON`,
`Helpers`, and third-party `PolyHook_2` **PUBLIC** into `UE4SS` — so a C++ mod that links
`UE4SS.lib` gets all of them for free, no extra gate beyond the one the mod already pays. Each is
real and each was checked against something we hand-rolled:

* **`Helpers::ScanTargetArray` / `WIN_MODULEINFO`** (`deps/first/Helpers/include/Helpers/UETargetModules.hpp:17-24,170-176`,
  HARD) gives a module's base address and size by name. Could shave a handful of
  `GetModuleHandle`/`GetModuleInformation` lines out of `src/engine_seam_hook.cpp`'s discovery
  path. Not worth a dependency for five lines.
* **`SinglePassSigScanner`** (`deps/first/SinglePassSigScanner/include/SigScanner/SinglePassSigScanner.hpp:1-80`,
  HARD) is a real, generic wildcard/mask byte-pattern scanner — not tied to Unreal at all. It could
  in principle have found `ITemporalUpscaler::AddPasses`'s vtable slot, **if we had a byte pattern
  for it**. We do not, and getting one against a licensee shipping build (no debug symbols, no
  guaranteed byte stability across their build vs. a public engine build) is exactly the kind of
  fragile input our own `src/core/engine_seam.hpp` structural **shape** scan (PE section
  enumeration + debug-name/layout matching, not byte-exact patterns — `engine_seam_hook.cpp:53-70`)
  was built to avoid. Same category of tool, worse fit for a licensee build with no known-good AOB.
* **`PolyHook_2`** (linked at `UE4SS/CMakeLists.txt:142`, HARD; usage confirmed only for IAT
  hooking in `UE4SS/src/UE4SSProgram.cpp:67` and `UE4SS/src/CrashDumper.cpp:8`) ships a vtable-swap
  hook class that could replace the ~30-line hand-rolled `VirtualProtect` dance in
  `src/backend_native/vtable_patch.cpp:25-57` and `src/engine_seam_hook.cpp:322-330`. Small,
  already-tested code either way — not worth the churn on its own.
* **`IniParser`** (`deps/first/IniParser/include/IniParser/Ini.hpp:15-49`, HARD) is a real
  section/key ini parser with typed getters (`get_string`/`get_int64`/`get_float`/`get_bool`,
  each with a default-value overload) — functionally close to our own config readers. No
  compelling reason to switch: our files are flat `[STRAYDLSS]`/`[STRAYDUALSENSE]` key sets with
  project-specific hot-reload logging per key, and `IniParser`'s heavier token/section machinery
  buys nothing we do not already have.
* **`RC::Output` + `FileDevice`** (`deps/first/DynamicOutput/include/DynamicOutput/FileDevice.hpp:1-90`,
  HARD) is a real pluggable logging framework, already used for the one line our own `Mod.cpp`
  logs directly (`mods/StrayDLSS/src/Mod.cpp` `Say()`, via `RC::Output::send`). It is **not** a
  replacement for our own file logger — see §2.3, this is a documented gap, not a style choice.

None of the four is wrong to have; none earns its keep against what already exists and is tested.

---

## 2. What we correctly built ourselves, and why UE4SS's version would not do

### 2.1 The entire D3D12 hook layer — not a duplicate, the only option

`grep -rln "dxgi\|D3D12\|ID3D12" UE4SS/src UE4SS/include` (run against the full `68caddcf` tarball,
not the mirror) returns **nothing**. UE4SS contains zero D3D12 or DXGI code — confirmed
independently of `docs/RESEARCH-UE4SS-MIGRATION.md`'s own binary-level finding, this time from the
source tree itself. `src/backend_native/{vtable_patch,d3d12_hooks,descriptor_shadow,
resource_registry,native_backend,present_owner}.*` is not reinventing anything UE4SS offers; there
is nothing there to reinvent from.

### 2.2 `src/engine_seam_hook.cpp`'s shape scan for `ITemporalUpscaler::AddPasses` — necessary, not duplicated

Two facts, both HARD, together close this definitively:

1. **`RegisterHook` requires a `UFunction*`.** `deps/first/Unreal/include/Unreal/UObjectGlobals.hpp:267-270`
   (mirror) — both overloads take a `UFunction*` or a fully-qualified `UFunction` name string.
   `ITemporalUpscaler::AddPasses` is a plain virtual C++ method on a Renderer-private interface
   with **no** `UFunction`, `UClass`, or any other reflection object behind it — there is nothing
   for `RegisterHook` to be given.
2. **UE4SS's own signature-scanning machinery is a fixed list of eleven specific engine globals**,
   not a general "find me any function" tool. `deps/first/patternsleuth_bind/src/lib.rs:22-36`
   (HARD, primary tarball — not gated) enumerates exactly what UE4SS's Rust scanner resolves:
   `GUObjectArray`, `FName::ToString`, `FName::FName(wchar)`, `GMalloc`,
   `StaticConstructObject_Internal`, `FText::FString`, engine version, `FUObjectHashTables::Get`,
   `GNatives`, `ConsoleManagerSingleton`, `UGameEngine::Tick`. Adding a twelfth target (our vtable)
   would mean writing and maintaining a new Rust `patternsleuth` resolver — a heavier, less
   testable path than the ~1,100-line C++ structural scan we already have in
   `src/core/engine_seam.hpp`/`src/engine_seam_hook.cpp`, which needed no new build toolchain and
   is exercised in this project's own CI.

`RegisterHook` and patternsleuth are both real tools; neither reaches this target. The same
argument applies verbatim to `mods/StrayDualSense/src/SubmixDiscovery.hpp`'s brute-force vtable
slot discovery for `FAudioDevice::RegisterSoundSubmix`/`RegisterSubmixBufferListener` — both are
non-reflected native engine methods outside the eleven-item list.

### 2.3 `Log.cpp`'s flush-per-line file logger — a documented gap in UE4SS's own logger, not a style choice

`deps/first/File/src/FileType/WinFile.cpp:388-` (HARD, primary tarball) implements
`WinFile::write_string_to_file` as a plain `WriteFile` call — grepping the whole `File` library for
`FlushFileBuffers`, `fflush`, or `_commit` returns nothing. UE4SS's own log file is therefore not
guaranteed flushed to disk after every line; a crash mid-session can lose whatever the OS had not
yet paged out. This project's own crash-diagnosis workflow (CLAUDE.md prime directive 1: the only
feedback loop is a user pasting back a log after a session that may have ended in an unclean
D3D12/GSP crash — §2.10, §5's GSP-crash section) makes flush-per-line a **hard requirement**, which
is exactly what `mods/StrayDualSense/src/Log.cpp`'s mutex-guarded, flush-every-line design exists
to satisfy, and `RC::Output`/`FileDevice` does not provide. Not a duplicate; a fix for a gap.

### 2.4 The descriptor + root-argument shadow — no equivalent exists to duplicate

Nothing in UE4SS shadows D3D12 descriptor heaps, root signatures, or bound root arguments — see
§2.1; there is no D3D12 code of any kind to compare against. `src/backend_native/descriptor_shadow.cpp`
stands alone, as `docs/RESEARCH-UE4SS-MIGRATION.md` §1.1 already established for the ReShade
comparison; UE4SS changes nothing about that conclusion.

---

## 3. What does not exist in UE4SS at `68caddcf` — so nobody looks again

### 3.1 No `IConsoleManager` / CVar API — resolves the "9 cvar hits" loose end

Case-sensitive search for `CVar`, `ConsoleVariable`, `IConsoleManager`, `ConsoleManager` across the
entire primary tarball (`UE4SS/`, `deps/`, `tools/`) returns exactly **seven** hits: four are the
Rust resolver registration already cited in §2.2 (`deps/first/patternsleuth_bind/src/lib.rs:17,35,207-208`,
HARD — the name `ConsoleManagerSingleton` and its Lua-override filename), and the remaining three
are a single inert stub in `UE4SS/src/Signatures.cpp:315-331` (HARD):

```cpp
auto lua_consolemanager_scan_script = working_directory / "UE4SS_Signatures/ConsoleManager.lua";
if (std::filesystem::exists(lua_consolemanager_scan_script))
{
    config.ScanOverrides.console_manager_singleton = [...](...) {
        scan_from_lua_script(lua_consolemanager_scan_script, signature_containers,
            [](void* address) {
                Output::send(STR("ConsoleManagerSingleton address: {} <- Lua Script\n"), address);
                return DidLuaScanSucceed::Yes;   // <- the address is logged and discarded
            }, ...);
    };
}
```

The found address is **never assigned to anything** — no global, no wrapper object, nothing
downstream reads it. `deps/first/patternsleuth_bind/src/lib.rs:17,35,101,206-207` (HARD) confirms
UE4SS's Rust scanner *can* find the engine's `ConsoleManagerSingleton` by AOB (it is one of the
eleven targets in §2.2), but grepping the whole tree — `UE4SS/`, `deps/`, and the mirrored
`deps/first/Unreal` — for `IConsoleManager`, `IConsoleVariable`, or `IConsoleObject` returns
**zero** hits anywhere. There is no C++ or Lua API to read or write a cvar through UE4SS at this
SHA; the address is found and thrown away.

This closes the original search's "9 hits for cvar": a case-insensitive grep for `cvar` matches
`vcvars` (`tools/xmakescripts/rules/version_rules.lua`) and `locvars`/`LocVar` (Lua's own bundled
`deps/first/LuaRaw/include/{lparser,lobject}.h`, debug-info fields of the vendored Lua VM) —
confirmed false positives, not gated content that a rate limit hid. There is nothing else to find.
If this project ever wants to read or set an engine cvar from the plugin, it has to build the same
kind of scan-and-wrap machinery it already builds for everything else in §2 — UE4SS supplies
neither the address (usably) nor the interface.

### 3.2 No RHI, RDG, or scene-view types anywhere

`grep -rln "FRHITexture\|FRDGResource\|FRDGBuilder\|ITemporalUpscaler\|FSceneView\b\|FViewInfo\b"`
across the full primary tarball **and** the mirrored `deps/first/Unreal` returns nothing. UE4SS's
reflection layer only ever sees `UObject`-rooted types; the Renderer module's internal C++ classes
are invisible to it by construction (see §3.3). This independently confirms CLAUDE.md's own
finding from a different angle — not the game's binary, but UE4SS's own source.

### 3.3 The SDK generator cannot produce a layout for a non-reflected type — closes the "could a tool have done this" question

`UE4SS/src/SDKGenerator/Generator.cpp:494,614` (HARD) walks
`UObjectGlobals::ForEachUObject(...)` over every **live `UObject`** in the running process and
emits headers from each one's `UClass`/`UScriptStruct`/`UEnum`/`UFunction`/`FProperty` reflection
data (`Generator.cpp:17-38` for the type aliases it operates on). The property-access helpers a mod
can call directly — `GetValuePtrByPropertyName<T>(name)`
(`deps/first/Unreal/include/Unreal/UObject.hpp:343-372`, mirror) — resolve a name to an offset the
**same way**: by walking `FProperty` metadata attached to a `UStruct`.

`FRDGResource`, `FRHITexture`, `FRDGBuilder`, `ITemporalUpscaler`, and `FViewInfo` are plain C++
classes in the Renderer module with **no `UClass` or `UStruct` at runtime** (§3.2) — there is no
reflection object for the generator to walk or for `GetValuePtrByPropertyName` to look an offset up
against. Neither the SDK generator, the property-access API, nor UEPseudo's own hand-ported headers
(confirmed absent by the same grep as §3.2, run against the mirror) can produce, check, or
cross-reference `FRDGResource::ResourceRHI @16` or `FRHITexture::GetNativeResource`'s vtable slot.
Deriving both by hand, from the shipped DXBC and measured behaviour, was not a shortcut around a
tool that existed — no such tool exists for a non-reflected type, by the reflection system's own
design. This closes the question the task raised as its concrete motivating example.

### 3.4 No C++-facing game-thread scheduler — `ExecuteInGameThread`/`LoopAsync` are Lua-only

`grep -rn "ExecuteInGameThread\|LoopAsync" UE4SS/include UE4SS/src` finds both only inside
`UE4SS/src/Mod/LuaMod.cpp:3096,4315` and as `static inline` members of the `LuaMod` class itself
(`UE4SS/include/Mod/LuaMod.hpp:68,158,162,167`, HARD) — there is no equivalent registered for
`CppUserModBase`. The nearest C++-facing primitive is `UE4SSProgram::queue_event`
(`UE4SS/include/UE4SSProgram.hpp:268-270`, HARD, explicitly commented `// Legacy overload for
backward compatibility with C++ mods`), which queues onto **UE4SS's own event-loop thread**
(`is_event_loop_thread()`/`get_event_loop_thread_id()`, `:276-283`) — the same thread `on_update`
fires on, confirmed by our own `mods/StrayDLSS/src/Mod.cpp` comment that `on_update` is "NOT the
game thread." There is no C++ primitive that reaches the game thread outside of a `UFunction` hook
callback firing there synchronously.

Checked against our own code: `grep -rn "ExecuteInGameThread\|LoopAsync" mods/` finds them **only**
in the Lua mods (`StrayFur`, `StrayProbe`, `StrayConsole`, `StrayDebugMenu`, `StrayAudioProbe`) —
zero hits in `mods/StrayDLSS/src/` or `mods/StrayDualSense/src/`. The task's framing assumed the
C++ plugins use these; they do not, and structurally cannot — the only game-thread entry point a
C++ mod has is a `RegisterHook` callback already running there, which is exactly what
`mods/StrayDualSense/src/Mod.cpp` already uses for every game-thread operation. There is nothing to
adopt here; the current design is the only one available to C++ mods.

### 3.5 `FindFirstOf` is a linear scan — our own caching discipline is correctly justified

`deps/first/Unreal/src/UObjectGlobals.cpp:354-382` (mirror, HARD): `FindFirstOf` is implemented as
a single `ForEachUObject` walk over the **entire live object array**, checking each object's class
and superclass chain by name. It is not a hash-table lookup even when
`UE4SSRuntime::ShouldUseHashTableIteration()` is true (`UE4SS/include/UE4SSRuntime.hpp:47-58`) —
that flag changes how the walk sources its objects, not whether a specific class name is looked up
in O(1). `mods/StrayDualSense/src/Mod.cpp:406` already documents calling it only "on rare events,"
which this confirms as the correct discipline rather than an unnecessary precaution.

---

## Summary table

| Area | Verdict | Section |
|---|---|---|
| D3D12/descriptor hooking | UE4SS has none; nothing to adopt | §2.1, §2.4 |
| Native (non-UFunction) hooking | `RegisterHook` cannot reach it; our shape scan was necessary | §2.2, §3.4* |
| Signature/AOB scanning | Generic scanner exists (`SinglePassSigScanner`) but needs a byte pattern we don't have and can't safely get; fixed 11-target Rust scanner doesn't cover us | §1.4, §2.2 |
| Config (ini) | `IniParser` exists, no compelling reason to switch | §1.4 |
| Logging | `RC::Output`/`FileDevice` exists but is not flush-per-line; ours fixes a real gap | §1.4, §2.3 |
| UObject access (`FindFirstOf`, `RegisterHook`, property helpers) | Already used correctly; `FindFirstOf` confirmed O(n), our caching discipline is justified | §3.5 |
| `IConsoleManager` / CVars | Address is scanned, never wired to an API — dead code, not a gap in our search | §3.1 |
| RHI / RDG / `FSceneView` | Zero references anywhere in UE4SS | §3.2 |
| SDK generator / UEPseudo for struct layouts | Reflection-only; cannot help with `FRDGResource`/`FRHITexture` | §3.3 |
| Threading (`ExecuteInGameThread`/`LoopAsync`) | Lua-only; no C++ equivalent; we don't and can't use them from C++ | §3.4 |
| Mod lifecycle (`CppUserModBase`) | Full surface reviewed; two callbacks worth knowing about, none urgent | §1.2, §1.3 |
| Startup self-check | `UE4SSRuntime::IsProcessEventAvailable()` etc. — worth adding | §1.1 |
