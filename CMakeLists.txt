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

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# NGX ships a static lib that exposes C++ std types across the ABI, so the CRT choice is not
# free: /MD pairs with nvsdk_ngx_d.lib, /MT with nvsdk_ngx_s.lib, and a mismatched
# _ITERATOR_DEBUG_LEVEL is a link error. We use /MD + nvsdk_ngx_d.lib, which is also the
# ReShade add-on convention. (docs/RESEARCH.md §3.9)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------------------------
# Pure logic. No Windows, no D3D, no ReShade. Everything here is unit-testable.
# ---------------------------------------------------------------------------------------
add_library(stray_dlss_core STATIC
    src/core/ue4_view.cpp
    src/core/ue4_view.hpp
    src/core/fnv1a.hpp)

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
#   Dear ImGui v1.92.5 IMGUI_VERSION_NUM 19250; enforced by #error in reshade_overlay.hpp
#   NGX      v310.7.0
# ---------------------------------------------------------------------------------------
add_library(reshade_sdk INTERFACE)
target_include_directories(reshade_sdk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/reshade/include)

add_library(imgui_headers INTERFACE)
target_include_directories(imgui_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui)

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

    find_library(NGX_LIBRARY
        NAMES nvsdk_ngx_d
        PATHS ${STRAY_DLSS_NGX_LIB_DIR}
        NO_DEFAULT_PATH)

    if(NOT NGX_LIBRARY)
        message(FATAL_ERROR
            "nvsdk_ngx_d.lib not found in '${STRAY_DLSS_NGX_LIB_DIR}'.\n"
            "Fetch it from https://github.com/NVIDIA/DLSS (lib/Windows_x86_64/x64/) or set "
            "-DSTRAY_DLSS_NGX_LIB_DIR=<dir>.")
    endif()

    add_library(stray-dlss SHARED
        src/addon.cpp
        src/log.cpp
        src/log.hpp
        src/ngx_backend.cpp
        src/ngx_backend.hpp
        src/version.rc)

    # ReShade discovers add-ons by extension, not by name.
    set_target_properties(stray-dlss PROPERTIES
        OUTPUT_NAME "stray-dlss"
        PREFIX ""
        SUFFIX ".addon64")

    target_link_libraries(stray-dlss PRIVATE
        stray_dlss_core
        reshade_sdk
        imgui_headers
        ngx_sdk
        ${NGX_LIBRARY}
        d3d12
        dxgi)

    target_compile_definitions(stray-dlss PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        # ReShade's add-on API is header-only. Defining RESHADE_API_LIBRARY would ask for an
        # import lib that does not exist. (docs/RESEARCH.md §2.1)
        STRAY_DLSS_VERSION="${PROJECT_VERSION}")

    target_compile_options(stray-dlss PRIVATE
        /W4 /permissive- /Zi
        $<$<BOOL:${STRAY_DLSS_WERROR}>:/WX>)

    # Keep symbols for field crash triage, but do not pay for them in the binary.
    target_link_options(stray-dlss PRIVATE /DEBUG /OPT:REF /OPT:ICF)

    install(TARGETS stray-dlss RUNTIME DESTINATION .)
    install(FILES $<TARGET_PDB_FILE:stray-dlss> DESTINATION . OPTIONAL)
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

    add_executable(stray_dlss_tests tests/test_core.cpp)
    target_link_libraries(stray_dlss_tests PRIVATE stray_dlss_core doctest::doctest)

    if(MSVC)
        target_compile_options(stray_dlss_tests PRIVATE /W4 $<$<BOOL:${STRAY_DLSS_WERROR}>:/WX>)
    else()
        target_compile_options(stray_dlss_tests PRIVATE -Wall -Wextra $<$<BOOL:${STRAY_DLSS_WERROR}>:-Werror>)
    endif()

    add_test(NAME core COMMAND stray_dlss_tests)
endif()
