cmake_minimum_required(VERSION 3.21)

project(stray-dlss
    VERSION 0.1.0
    DESCRIPTION "DLSS Super Resolution ReShade add-on for Stray (UE 4.27.2, D3D12)"
    LANGUAGES CXX)

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
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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
    src/core/taa_signature.cpp
    src/core/taa_signature.hpp
    src/core/pass_walk.cpp
    src/core/pass_walk.hpp
    src/core/gbuffer_classify.cpp
    src/core/gbuffer_classify.hpp
    src/core/envbrdf.cpp
    src/core/envbrdf.hpp
    src/core/nr_codec.cpp
    src/core/nr_codec.hpp
    src/core/nr_hook_plan.cpp
    src/core/nr_hook_plan.hpp
    src/core/ring.hpp)

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
# The add-on
# ---------------------------------------------------------------------------------------
if(STRAY_DLSS_BUILD_ADDON)
    if(NOT WIN32)
        message(FATAL_ERROR "STRAY_DLSS_BUILD_ADDON requires Windows.")
    endif()

    if(STRAY_DLSS_ENABLE_NGX)
        # _s = static CRT, to match /MT above.
        find_library(NGX_LIBRARY
            NAMES nvsdk_ngx_s
            PATHS ${STRAY_DLSS_NGX_LIB_DIR}
            NO_DEFAULT_PATH)

        if(NOT NGX_LIBRARY)
            message(FATAL_ERROR
                "nvsdk_ngx_s.lib not found in '${STRAY_DLSS_NGX_LIB_DIR}'.\n"
                "Fetch it from https://github.com/NVIDIA/DLSS (lib/Windows_x86_64/x64/) or set "
                "-DSTRAY_DLSS_NGX_LIB_DIR=<dir>.")
        endif()
    else()
        set(NGX_LIBRARY "")
        message(STATUS "NGX disabled: building a no-DLSS diagnostic add-on")
    endif()

    add_library(stray-dlss SHARED
        src/addon.cpp
        src/log.cpp
        src/log.hpp
        src/ngx_backend.cpp
        src/ngx_backend.hpp
        src/ngx_nr.cpp
        src/ngx_nr.hpp
        src/ngx_snippet.cpp
        src/ngx_snippet.hpp
        src/shader_dump.cpp
        src/shader_dump.hpp
        src/frame_state.cpp
        src/frame_state.hpp
        src/taa_hook.cpp
        src/taa_hook.hpp
        src/pass_finder.cpp
        src/pass_finder.hpp
        src/perf.cpp
        src/perf.hpp
        src/gbuffer_finder.cpp
        src/gbuffer_finder.hpp
        src/gbuffer_resolve.cpp
        src/gbuffer_resolve.hpp
        src/nr_codec_pass.cpp
        src/nr_codec_pass.hpp
        src/nr_hook.cpp
        src/nr_hook.hpp
        src/ext_unhook.cpp
        src/input_dump.cpp
        src/mv_resolve.cpp
        src/mv_resolve.hpp
        src/d3d12_restore.cpp
        src/d3d12_restore.hpp
        src/version.rc)

    # ReShade discovers add-ons by extension, not by name.
    set_target_properties(stray-dlss PROPERTIES
        OUTPUT_NAME "stray-dlss"
        PREFIX ""
        SUFFIX ".addon64")

    target_link_libraries(stray-dlss PRIVATE
        stray_dlss_core
        reshade_sdk
        reshade_utils
        reshade_utils_headers
        imgui_headers
        ngx_sdk
        ${NGX_LIBRARY}
        d3d12
        dxgi
        d3dcompiler)  # for D3D12SerializeRootSignature only; D3DCompile is never called

    target_compile_definitions(stray-dlss PRIVATE
        STRAY_DLSS_ENABLE_NGX=$<BOOL:${STRAY_DLSS_ENABLE_NGX}>)

    target_compile_definitions(stray-dlss PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        # ReShade's add-on API is header-only. Defining RESHADE_API_LIBRARY would ask for an
        # import lib that does not exist. (docs/RESEARCH.md §2.1)
        STRAY_DLSS_VERSION="${PROJECT_VERSION}"
        # Belt and braces alongside /MT: keeps std::mutex on the pre-17.10 layout so even an
        # accidental /MD build stays loadable against an old msvcp140.dll.
        _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

    target_compile_options(stray-dlss PRIVATE
        /W4 /permissive- /Zi
        $<$<BOOL:${STRAY_DLSS_WERROR}>:/WX>)

    # Keep symbols for field crash triage, but do not pay for them in the binary.
    target_link_options(stray-dlss PRIVATE /DEBUG /OPT:REF /OPT:ICF)

    # Shaders are compiled to DXBC here and embedded, never compiled at runtime.
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/CompileShaders.cmake)
    compile_shader(stray-dlss "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mv_resolve.hlsl"
                   main cs_5_0 g_mv_resolve_dxbc)
    compile_shader(stray-dlss "${CMAKE_CURRENT_SOURCE_DIR}/shaders/gbuffer_resolve.hlsl"
                   main cs_5_0 g_gbuffer_resolve_dxbc)
    # The DLSS-NR HDR colour codec. Both halves include shaders/nr_codec.hlsli, which is named
    # as an extra dependency so editing the shared transfer functions rebuilds both.
    compile_shader(stray-dlss "${CMAKE_CURRENT_SOURCE_DIR}/shaders/nr_encode.hlsl"
                   main cs_5_0 g_nr_encode_dxbc
                   "${CMAKE_CURRENT_SOURCE_DIR}/shaders/nr_codec.hlsli")
    compile_shader(stray-dlss "${CMAKE_CURRENT_SOURCE_DIR}/shaders/nr_decode.hlsl"
                   main cs_5_0 g_nr_decode_dxbc
                   "${CMAKE_CURRENT_SOURCE_DIR}/shaders/nr_codec.hlsli")

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
        # The real frame_state, so the ReShade-facing half of the restore is the code under
        # test rather than a reimplementation of it.
        src/frame_state.cpp)

    compile_shader(stray_dlss_warp "${CMAKE_CURRENT_SOURCE_DIR}/shaders/mv_resolve.hlsl"
                   main cs_5_0 g_mv_resolve_dxbc)
    compile_shader(stray_dlss_warp "${CMAKE_CURRENT_SOURCE_DIR}/shaders/warp_probe.hlsl"
                   main cs_5_0 g_warp_probe_dxbc)

    target_include_directories(stray_dlss_warp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
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
        tests/test_taa_signature.cpp
        tests/test_pass_walk.cpp
        tests/test_gbuffer_classify.cpp
        tests/test_envbrdf.cpp
        tests/test_nr_codec.cpp
        tests/test_nr_hook_plan.cpp
        tests/test_ring.cpp)
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
