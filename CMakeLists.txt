cmake_minimum_required(VERSION 3.21)

project(stray-dlss
    VERSION 0.1.0
    DESCRIPTION "DLSS Super Resolution ReShade add-on for Stray (UE 4.27.2, D3D12)"
    LANGUAGES C CXX)

# The add-on itself is Windows/MSVC only and is never built locally — CI is the only build.
# The pure-logic core and its tests are deliberately platform-independent so they also build
# and run on a Linux runner, which is where the fast feedback comes from.
option(STRAY_DLSS_BUILD_ADDON "Build the ReShade add-on (requires Windows + MSVC)" ${WIN32})
option(STRAY_DLSS_BUILD_TESTS "Build the unit tests" ON)
option(STRAY_DLSS_WERROR "Treat warnings as errors" ON)
# Linking NVIDIA's static NGX library runs its global constructors during
# _DllMainCRTStartup, before our DllMain body. If that faults, Wine reports the whole
# LoadLibraryEx as error 998 (ERROR_NOACCESS) and nothing of ours ever runs — not even the
# first log line. Turning this OFF builds an otherwise identical add-on with NGX stubbed, to
# isolate that.
option(STRAY_DLSS_ENABLE_NGX "Link NVIDIA NGX (DLSS)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# STATIC CRT (/MT), deliberately, and it is not a style preference.
#
# A /MD build links the prefix's msvcp140.dll, and Proton prefixes routinely carry an ancient
# one (Stray's is dated Nov 2020, VS2019-era). Since MSVC 17.10 the STL gives std::mutex a
# constexpr constructor that merely zero-initialises it; the older msvcp140's _Mtx_lock then
# dereferences a field that was never set up. The result is an access violation at DLL entry,
# which Wine reports as LoadLibraryEx error 998 with none of our code having run — no log, no
# clue. This cost a full debugging round; see docs/RESEARCH.md §5.
#
# /MT makes the add-on self-contained and independent of whatever redist the user's prefix
# happens to have. It pairs with nvsdk_ngx_s.lib (the _s suffix means static CRT); mixing that
# up is an _ITERATOR_DEBUG_LEVEL link error, which is at least loud.
#
# ONE EXCEPTION, and it is the UE4SS plugin's ABI contract rather than a preference: a UE4SS
# C++ mod must be /MD to match UE4SS.dll (mods/StrayDLSS/CMakeLists.txt). Its build configures
# THIS tree with STRAY_DLSS_DYNAMIC_CRT=ON — one configure, one CRT, every stray_dlss_* target
# consistent — and pairs it with nvsdk_ngx_d.lib. The two are never mixed in one build: the
# add-on's CI asserts no MSVCP140 import, the plugin's asserts its presence.
option(STRAY_DLSS_DYNAMIC_CRT "Link the dynamic CRT (/MD): ONLY for the UE4SS plugin build" OFF)
if(MSVC)
    if(STRAY_DLSS_DYNAMIC_CRT)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    else()
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # std::mutex's constexpr constructor (MSVC STL >= 17.10) zero-initialises the mutex and
    # skips _Mtx_init_in_situ; the FIRST lock against an OLDER msvcp140.dll then dereferences an
    # uninitialised field and faults inside MSVCP140.dll. Stray's Proton prefix ships a Nov-2020
    # msvcp140 (measured), so a /MD build that links it crashes on its first std::mutex lock.
    # This macro keeps the pre-17.10 (init-in-situ) layout. It MUST cover EVERY target that
    # constructs a std::mutex, not just the native library: src/log.cpp (the file sink, whose
    # g_mutex init_file_sink locks as the first thing the UE4SS host does) lives in
    # stray_dlss_core, and a per-target definition on stray_dlss_native alone left core without
    # it — which crashed the plugin's start_mod in MSVCP140.dll +0x12eb0, before any log line
    # (measured 2026-09-02). Global, so no translation unit can miss it. Harmless under /MT.
    add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
endif()

# ---------------------------------------------------------------------------------------
# Pure logic. No Windows, no D3D, no ReShade. Everything here is unit-testable.
# ---------------------------------------------------------------------------------------
add_library(stray_dlss_core STATIC
    src/core/ue4_view.cpp
    src/core/ue4_view.hpp
    src/core/fnv1a.hpp
    src/core/view_params.cpp
    src/core/view_params.hpp
    src/core/dlss_quality.cpp
    src/core/taa_signature.cpp
    src/core/taa_signature.hpp
    src/core/engine_seam.cpp
    src/core/engine_seam.hpp
    src/core/pass_walk.cpp
    src/core/pass_walk.hpp
    src/core/gbuffer_classify.cpp
    src/core/gbuffer_classify.hpp
    src/core/envbrdf.cpp
    src/core/envbrdf.hpp
    src/core/nr_hook_plan.cpp
    src/core/nr_hook_plan.hpp
    src/core/nr_lifetime.cpp
    src/core/nr_lifetime.hpp
    src/core/nr_mask_plan.cpp
    src/core/nr_mask_plan.hpp
    src/core/nr_params.cpp
    src/core/nr_params.hpp
    src/core/present_plan.cpp
    src/core/present_plan.hpp
    src/core/fg_plan.cpp
    src/core/fg_plan.hpp
    src/core/exposure_plan.cpp
    src/core/exposure_plan.hpp
    src/core/fg_throttle.cpp
    src/core/fg_throttle.hpp
    src/core/ring.hpp
    src/host/ini.cpp
    src/host/ini.hpp
    src/host/config.cpp
    src/host/config.hpp
    src/core/dxgi_format.cpp
    src/core/dxgi_format.hpp
    src/intercept/types.hpp
    src/intercept/backend.hpp
    src/intercept/backend.cpp
    src/core/va_map.cpp
    src/core/va_map.hpp
    src/core/heap_math.cpp
    src/core/heap_math.hpp
    src/core/root_signature_walk.cpp
    src/core/root_signature_walk.hpp
    # The differential observer's comparison is pure; its live half is plain C++ too.
    src/app/diff_observer.cpp
    src/app/diff_observer.hpp
    # The file sink is plain stdio and lives here so the seam can log under every host. The
    # WARP harness links its own log_stub.cpp first, so the archive member is never pulled.
    src/log.cpp
    src/log.hpp)

target_include_directories(stray_dlss_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(stray_dlss_core PRIVATE /W4 /permissive- $<$<BOOL:${STRAY_DLSS_WERROR}>:/WX>)
else()
    target_compile_options(stray_dlss_core PRIVATE -Wall -Wextra $<$<BOOL:${STRAY_DLSS_WERROR}>:-Werror>)
endif()

# ---------------------------------------------------------------------------------------
# Vendored third-party headers, all version-pinned. See docs/RESEARCH.md for why each pin
# matters — a drift in any of the three silently breaks loading rather than the build.
#   ReShade  v6.8.0   RESHADE_API_VERSION 20; ReShade refuses anything newer than its own
#   Dear ImGui v1.92.5-docking  IMGUI_VERSION_NUM 19250, enforced by #error in
#            reshade_overlay.hpp. It MUST be the docking branch: ReShade's imgui function
#            table references ImGuiDockNodeFlags and ImGuiWindowClass, which exist only
#            there. The plain v1.92.5 tag reports the same version number and then fails to
#            compile, so the #error is not enough to catch this.
#   NGX      v310.7.0
# ---------------------------------------------------------------------------------------
add_library(reshade_sdk INTERFACE)
target_include_directories(reshade_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/include)

add_library(imgui_headers INTERFACE)
target_include_directories(imgui_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

add_library(reshade_utils_headers INTERFACE)
target_include_directories(reshade_utils_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade_utils)

# ReShade's own descriptor and state tracking, copied verbatim from its examples/utils at tag
# v6.8.0. Reimplementing these is how people get descriptor tracking subtly wrong, so they are
# kept pristine — which means they cannot be held to our /W4 /WX (they have unreferenced
# parameters). Isolating them in their own target keeps our code strict without touching theirs.
if(WIN32)
    add_library(reshade_utils STATIC
        third_party/reshade_utils/descriptor_tracking.cpp
        third_party/reshade_utils/state_tracking.cpp)
    target_link_libraries(reshade_utils PUBLIC reshade_sdk reshade_utils_headers imgui_headers)
    target_compile_definitions(reshade_utils PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(reshade_utils PRIVATE /W3 /wd4100)
    endif()
endif()

add_library(ngx_sdk INTERFACE)
target_include_directories(ngx_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/include)

# The NGX import library is ~5 MB and is fetched by CI rather than committed. Point this at
# a directory containing nvsdk_ngx_d.lib.
set(STRAY_DLSS_NGX_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ngx/lib"
    CACHE PATH "Directory containing nvsdk_ngx_d.lib")

# ---------------------------------------------------------------------------------------
# The native library: everything that is Windows/D3D12 but framework-free. The application
# (src/app), the NGX path, the resolve passes, the hooks and the shaders. Built ONCE per
# configure with whichever CRT this configure chose, and linked by both hosts: the ReShade
# add-on below (/MT) and the UE4SS plugin (mods/StrayDLSS, /MD, its own configure).
# ---------------------------------------------------------------------------------------
option(STRAY_DLSS_BUILD_NATIVE "Build the framework-free native library (Windows + MSVC)" ${WIN32})

if(STRAY_DLSS_BUILD_NATIVE)
    if(NOT WIN32)
        message(FATAL_ERROR "STRAY_DLSS_BUILD_NATIVE requires Windows.")
    endif()

    if(STRAY_DLSS_ENABLE_NGX)
        # _s = static CRT (/MT), _d = dynamic CRT (/MD). Mixing them up is an
        # _ITERATOR_DEBUG_LEVEL link error, which is at least loud.
        if(STRAY_DLSS_DYNAMIC_CRT)
            set(_ngx_lib_name nvsdk_ngx_d)
        else()
            set(_ngx_lib_name nvsdk_ngx_s)
        endif()
        find_library(NGX_LIBRARY
            NAMES ${_ngx_lib_name}
            PATHS ${STRAY_DLSS_NGX_LIB_DIR}
            NO_DEFAULT_PATH)

        if(NOT NGX_LIBRARY)
            message(FATAL_ERROR
                "${_ngx_lib_name}.lib not found in '${STRAY_DLSS_NGX_LIB_DIR}'.\n"
                "Fetch it from https://github.com/NVIDIA/DLSS (lib/Windows_x86_64/x64/) or set "
                "-DSTRAY_DLSS_NGX_LIB_DIR=<dir>.")
        endif()
    else()
        set(NGX_LIBRARY "")
        message(STATUS "NGX disabled: building a no-DLSS diagnostic build")
    endif()

    add_library(stray_dlss_native STATIC
        # The framework-free application, behind the seam.
        src/app/dlss_app.cpp
        src/app/dlss_app.hpp
        src/exposure_texture.cpp
        src/exposure_texture.hpp
        src/ngx_backend.cpp
        src/ngx_backend.hpp
        src/ngx_nr.cpp
        src/ngx_nr.hpp
        src/ngx_fg.cpp
        src/ngx_fg.hpp
        src/ngx_snippet.cpp
        src/ngx_snippet.hpp
        src/shader_dump.cpp
        src/shader_dump.hpp
        src/taa_hook.cpp
        src/taa_hook.hpp
        src/engine_seam_hook.cpp
        src/engine_seam_hook.hpp
        src/pass_finder.cpp
        src/pass_finder.hpp
        src/perf.cpp
        src/perf.hpp
        src/gbuffer_finder.cpp
        src/gbuffer_finder.hpp
        src/gbuffer_resolve.cpp
        src/gbuffer_resolve.hpp
        src/nr_hook.cpp
        src/nr_hook.hpp
        src/nr_stage.cpp
        src/nr_stage.hpp
        src/nr_mask.cpp
        src/nr_mask.hpp
        src/ext_unhook.cpp
        src/ext_unhook.hpp
        src/input_dump.cpp
        src/input_dump.hpp
        src/mv_resolve.cpp
        src/mv_resolve.hpp
        src/d3d12_restore.cpp
        src/d3d12_restore.hpp
        # The native backend: our own D3D12 vtable hooks, the descriptor and root shadows,
        # the liveness registry. Framework-free by construction.
        src/backend_native/native_backend.cpp
        src/backend_native/native_backend.hpp
        src/backend_native/d3d12_hooks.cpp
        src/backend_native/d3d12_hooks.hpp
        src/backend_native/descriptor_shadow.cpp
        src/backend_native/descriptor_shadow.hpp
        src/backend_native/resource_registry.cpp
        src/backend_native/resource_registry.hpp
        src/backend_native/root_shadow.cpp
        src/backend_native/root_shadow.hpp
        src/backend_native/vram_query_watch.cpp
        src/backend_native/vram_query_watch.hpp
        src/backend_native/vtable_patch.cpp
        src/backend_native/vtable_patch.hpp
        src/backend_native/present_owner.cpp
        src/backend_native/present_owner.hpp
        src/backend_native/fg_present.cpp
        src/backend_native/fg_present.hpp
        src/backend_native/fg_reflex.cpp
        src/backend_native/fg_reflex.hpp
        src/backend_native/fg_throttle.cpp
        src/backend_native/fg_throttle.hpp
        src/backend_native/vtable_slots.hpp
        # Compile-time proof of the slot numbers against the SDK's C Vtbl structs; links nothing.
        src/backend_native/vtable_slots_check.c)

    target_include_directories(stray_dlss_native PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

    target_link_libraries(stray_dlss_native PUBLIC
        stray_dlss_core
        ngx_sdk
        ${NGX_LIBRARY}
        d3d12
        dxgi
        d3dcompiler)  # for D3D12SerializeRootSignature only; D3DCompile is never called

    target_compile_definitions(stray_dlss_native PUBLIC
        STRAY_DLSS_ENABLE_NGX=$<BOOL:${STRAY_DLSS_ENABLE_NGX}>
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        STRAY_DLSS_VERSION="${PROJECT_VERSION}"
        # Belt and braces alongside /MT: keeps std::mutex on the pre-17.10 layout so even an
        # accidental /MD build stays loadable against an old msvcp140.dll.
        _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

    # /Z7, not /Zi: debug info goes into each .obj instead of a PDB shared by every object of
    # the target. The linker's /DEBUG still emits the target PDB, so nothing downstream
    # changes - but sccache (the plugin CI's compile cache, .github/workflows/dlss-plugin.yml)
    # refuses to cache any /Zi compile whose /Fd is shared ("shared pdb" in its msvc.rs), and
    # CMake+Ninja hands every object of a target the same /Fd.
    target_compile_options(stray_dlss_native PRIVATE
        /W4 /permissive- /Z7
        $<$<BOOL:${STRAY_DLSS_WERROR}>:/WX>)
    # The SDK's C interface (CINTERFACE) trips /W4 inside d3d12.h itself; the slot check is a
    # set of negative-array-size typedefs, which are ERRORS at any warning level, so /W3 there
    # loses nothing. (MSVC takes the last /W on the line.)
    set_source_files_properties(src/backend_native/vtable_slots_check.c PROPERTIES
        COMPILE_OPTIONS "/W3")

    # Shaders are compiled to DXBC here and embedded, never compiled at runtime.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake)
    compile_shader(stray_dlss_native "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mv_resolve.hlsl"
                   main cs_5_0 g_mv_resolve_dxbc)
    compile_shader(stray_dlss_native "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer_resolve.hlsl"
                   main cs_5_0 g_gbuffer_resolve_dxbc)
    compile_shader(stray_dlss_native "${CMAKE_CURRENT_SOURCE_DIR}/shaders/nr_mask.hlsl"
                   main cs_5_0 g_nr_mask_dxbc)
endif()

# ---------------------------------------------------------------------------------------
# The ReShade add-on: the native library plus the ReShade host.
# ---------------------------------------------------------------------------------------
if(STRAY_DLSS_BUILD_ADDON)
    if(NOT WIN32)
        message(FATAL_ERROR "STRAY_DLSS_BUILD_ADDON requires Windows.")
    endif()
    if(NOT STRAY_DLSS_BUILD_NATIVE)
        message(FATAL_ERROR "STRAY_DLSS_BUILD_ADDON requires STRAY_DLSS_BUILD_NATIVE.")
    endif()
    if(STRAY_DLSS_DYNAMIC_CRT)
        message(FATAL_ERROR
            "The ReShade add-on must be /MT (see the CRT note at the top of this file); "
            "STRAY_DLSS_DYNAMIC_CRT is for the UE4SS plugin build only.")
    endif()

    add_library(stray-dlss SHARED
        # The ReShade host: the ONLY directory that includes ReShade's headers (CI enforces).
        src/backend_reshade/addon_entry.cpp
        src/backend_reshade/reshade_all.hpp
        src/backend_reshade/reshade_backend.cpp
        src/backend_reshade/reshade_backend.hpp
        src/backend_reshade/reshade_bindings.cpp
        src/backend_reshade/reshade_bindings.hpp
        src/backend_reshade/reshade_config.cpp
        src/backend_reshade/reshade_log.cpp
        src/backend_reshade/reshade_host.hpp
        src/version.rc)

    # ReShade discovers add-ons by extension, not by name.
    set_target_properties(stray-dlss PROPERTIES
        OUTPUT_NAME "stray-dlss"
        PREFIX ""
        SUFFIX ".addon64")

    # reshade_all.hpp and the backend's own headers are found relative to their directory.
    target_include_directories(stray-dlss PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/backend_reshade)

    target_link_libraries(stray-dlss PRIVATE
        stray_dlss_native
        reshade_sdk
        reshade_utils
        reshade_utils_headers
        imgui_headers)

    # /Z7, not /Zi — same reasoning as stray_dlss_native above: sccache (now wired into
    # ci.yml's build-windows job) refuses to cache a /Zi compile whose /Fd is shared, and
    # CMake+Ninja gives every object of this target the same /Fd. /DEBUG below still emits
    # the full PDB from the embedded per-object records, so nothing downstream changes.
    target_compile_options(stray-dlss PRIVATE
        /W4 /permissive- /Z7
        $<$<BOOL:${STRAY_DLSS_WERROR}>:/WX>)

    # Keep symbols for field crash triage, but do not pay for them in the binary.
    target_link_options(stray-dlss PRIVATE /DEBUG /OPT:REF /OPT:ICF)

    install(TARGETS stray-dlss RUNTIME DESTINATION .)
    install(FILES $<TARGET_PDB_FILE:stray-dlss> DESTINATION . OPTIONAL)
endif()

# ---------------------------------------------------------------------------------------
# WARP harness
#
# Runs the real resolve pass against Microsoft's software D3D12 device with the debug layer on,
# so our D3D12 usage is judged on a CI machine with no GPU. This matters because the failures
# on the target machine are silent: vkd3d-proton validates root parameters by assert only in a
# release build, so illegal usage yields a wrong image or an unresponsive GPU rather than an
# error. It cannot cover vkd3d-specific behaviour, only correctness of what we ask for.
# ---------------------------------------------------------------------------------------
option(STRAY_DLSS_BUILD_WARP_TESTS "Build the WARP D3D12 harness (Windows only)" ${WIN32})

if(STRAY_DLSS_BUILD_WARP_TESTS AND WIN32)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake)

    add_executable(stray_dlss_warp
        tests/warp/warp_mv_resolve.cpp
        tests/warp/log_stub.cpp
        src/mv_resolve.cpp
        src/d3d12_restore.cpp
        # The NR present stage's D3D12 half: the staging texture and the two copies. NGX-free on
        # purpose (src/nr_stage.hpp), which is the only reason the harness can judge it at all.
        src/nr_stage.cpp
        # The ControlMask fill: one committed texture and one compute dispatch, NGX-free for the
        # same reason (src/nr_mask.hpp), so the debug layer can judge whether it stores as well
        # as records.
        src/nr_mask.cpp
        # present_owner.cpp now times its per-present work with perf::Scope (frame-pacing work,
        # 2026-09-02), so the harness that links present_owner must also link perf.
        src/perf.cpp
        # The real ReShade bindings, so the ReShade-facing half of the restore is the code
        # under test rather than a reimplementation of it.
        src/backend_reshade/reshade_bindings.cpp
        # The native backend, hooked onto the harness's own WARP device (warp_native_backend.inc).
        src/backend_native/native_backend.cpp
        src/backend_native/d3d12_hooks.cpp
        src/backend_native/descriptor_shadow.cpp
        src/backend_native/resource_registry.cpp
        src/backend_native/root_shadow.cpp
        src/backend_native/present_owner.cpp
        src/backend_native/fg_present.cpp
        src/backend_native/fg_reflex.cpp
        src/backend_native/fg_throttle.cpp
        src/backend_native/vram_query_watch.cpp
        src/backend_native/vtable_patch.cpp
        src/app/diff_observer.cpp)

    compile_shader(stray_dlss_warp "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mv_resolve.hlsl"
                   main cs_5_0 g_mv_resolve_dxbc)
    compile_shader(stray_dlss_warp "${CMAKE_CURRENT_SOURCE_DIR}/shaders/nr_mask.hlsl"
                   main cs_5_0 g_nr_mask_dxbc)
    compile_shader(stray_dlss_warp "${CMAKE_CURRENT_SOURCE_DIR}/shaders/warp_probe.hlsl"
                   main cs_5_0 g_warp_probe_dxbc)

    target_include_directories(stray_dlss_warp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/backend_reshade
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/warp)
    target_link_libraries(stray_dlss_warp PRIVATE
        stray_dlss_core
        reshade_sdk
        reshade_utils
        reshade_utils_headers
        d3d12 dxgi d3dcompiler)
    target_compile_definitions(stray_dlss_warp PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(stray_dlss_warp PRIVATE /W4 $<$<BOOL:${STRAY_DLSS_WERROR}>:/WX>)

    add_test(NAME warp COMMAND stray_dlss_warp)
endif()

# ---------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------
if(STRAY_DLSS_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.12
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(doctest)

    enable_testing()

    add_executable(stray_dlss_tests
        tests/test_main.cpp
        tests/test_core.cpp
        tests/test_view_params.cpp
        tests/test_dlss_quality.cpp
        tests/test_taa_signature.cpp
        tests/test_engine_seam.cpp
        tests/test_pass_walk.cpp
        tests/test_gbuffer_classify.cpp
        tests/test_envbrdf.cpp
        tests/test_nr_hook_plan.cpp
        tests/test_nr_lifetime.cpp
        tests/test_nr_mask_plan.cpp
        tests/test_nr_params.cpp
        tests/test_ring.cpp
        tests/test_ini.cpp
        tests/test_intercept_types.cpp
        tests/test_native_core.cpp
        tests/test_fg_plan.cpp
        tests/test_exposure_plan.cpp
        tests/test_fg_throttle.cpp)
    target_link_libraries(stray_dlss_tests PRIVATE stray_dlss_core doctest::doctest)

    if(MSVC)
        # C5285: doctest specialises std::tuple, which N5014 forbids. It is third-party
        # header noise, not something we can fix, so silence it rather than dropping /WX.
        target_compile_options(stray_dlss_tests PRIVATE /W4 /wd5285 $<$<BOOL:${STRAY_DLSS_WERROR}>:/WX>)
    else()
        target_compile_options(stray_dlss_tests PRIVATE -Wall -Wextra $<$<BOOL:${STRAY_DLSS_WERROR}>:-Werror>)
    endif()

    add_test(NAME core COMMAND stray_dlss_tests)
endif()
