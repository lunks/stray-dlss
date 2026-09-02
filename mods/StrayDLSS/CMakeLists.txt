# StrayDLSS — DLSS Super Resolution for Stray as a UE4SS C++ plugin.
#
# This file plays TWO roles, on purpose (the layout is mods/StrayDualSense/CMakeLists.txt's):
#
#   standalone   `cmake -S mods/StrayDLSS -B build` is a configure check only; the pure logic
#                lives in the root's stray_dlss_core and is tested there, and the plain-Win32
#                host (Host.cpp) is compiled under mingw by the workflow.
#
#   sub-project  When added from a parent that has already done `add_subdirectory(RE-UE4SS)`,
#                the `UE4SS` target exists and the plugin DLL is built too. That is the
#                layout RE-UE4SS documents for C++ mods (docs/guides/creating-a-c++-mod.md);
#                `.github/workflows/dlss-plugin.yml` generates the parent.
#
# The DLL cannot be built locally — no MSVC (CLAUDE.md §0, §6). CI is the only build; the
# user's box is the only test. The plain-Win32 half (everything but Mod.cpp) is additionally
# compiled and linked under mingw by the workflow, which catches the common failures fast.
cmake_minimum_required(VERSION 3.22)

project(StrayDLSS
    VERSION 0.1.0
    DESCRIPTION "DLSS Super Resolution for Stray, loaded by UE4SS"
    LANGUAGES C CXX)

option(STRAY_DLSS_PLUGIN_WERROR "Treat warnings as errors" ON)

if(MSVC)
    # /external:* keeps /W4 /WX honest without drowning in UE4SS, imgui and fmt warnings:
    # anything included with <angle brackets> is treated as external and silenced.
    set(SDL_WARN /W4 $<$<BOOL:${STRAY_DLSS_PLUGIN_WERROR}>:/WX> /external:anglebrackets /external:W0)
else()
    set(SDL_WARN -Wall -Wextra $<$<BOOL:${STRAY_DLSS_PLUGIN_WERROR}>:-Werror>)
endif()

# The repository root, for third_party/. Overridable so the CI parent layout can point at
# the checkout wherever it landed.
set(STRAY_DLSS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "stray-dlss repository root")
get_filename_component(STRAY_DLSS_ROOT "${STRAY_DLSS_ROOT}" ABSOLUTE)

# ---------------------------------------------------------------------------------------
# MinHook (BSD-2), vendored at the commit ReShade v6.8.0 pins — third_party/minhook/VERSION.
# The only inline-detour engine in the plugin, used for at most three exports (plan §B.3).
# A STATIC library of C, so it needs no CRT choice of its own beyond the target's.
# ---------------------------------------------------------------------------------------
add_library(stray_dlss_minhook STATIC
    ${STRAY_DLSS_ROOT}/third_party/minhook/src/buffer.c
    ${STRAY_DLSS_ROOT}/third_party/minhook/src/hook.c
    ${STRAY_DLSS_ROOT}/third_party/minhook/src/trampoline.c
    ${STRAY_DLSS_ROOT}/third_party/minhook/src/hde/hde64.c)
target_include_directories(stray_dlss_minhook PUBLIC ${STRAY_DLSS_ROOT}/third_party/minhook/include)
if(MSVC)
    # Third-party C: not held to our /W4 /WX.
    target_compile_options(stray_dlss_minhook PRIVATE /W3)
    set_property(TARGET stray_dlss_minhook PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------------------------
# The plugin DLL. Only when a parent has already brought in RE-UE4SS.
# ---------------------------------------------------------------------------------------
if(NOT TARGET UE4SS)
    message(STATUS
        "StrayDLSS: the UE4SS target is absent, so the plugin DLL is not built. To build it, "
        "configure a parent project that does add_subdirectory(RE-UE4SS) before "
        "add_subdirectory(StrayDLSS) — see .github/workflows/dlss-plugin.yml.")
    return()
endif()

if(NOT WIN32)
    message(FATAL_ERROR
        "StrayDLSS: the plugin is Windows/MSVC only. UE4SS passes std:: types across the "
        "DLL boundary, so the mod must be built by the same toolchain and CRT as UE4SS itself.")
endif()

# The shared application + native backend, from the repository root, configured ONCE with the
# dynamic CRT (the UE4SS ABI contract below) and with nothing else the add-on builds.
set(STRAY_DLSS_BUILD_ADDON OFF CACHE BOOL "" FORCE)
set(STRAY_DLSS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STRAY_DLSS_BUILD_WARP_TESTS OFF CACHE BOOL "" FORCE)
set(STRAY_DLSS_BUILD_NATIVE ON CACHE BOOL "" FORCE)
set(STRAY_DLSS_DYNAMIC_CRT ON CACHE BOOL "" FORCE)
add_subdirectory(${STRAY_DLSS_ROOT} stray_dlss_root)

add_library(StrayDLSS SHARED
    src/Host.cpp
    src/Host.hpp
    src/Mod.cpp
    src/Platform.cpp
    src/Platform.hpp
    src/Version.hpp)

target_include_directories(StrayDLSS PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# C++23 is not a preference: RE-UE4SS's root CMakeLists sets CMAKE_CXX_STANDARD 23 and UE4SS
# declares `target_compile_features(UE4SS PUBLIC cxx_std_23)`. /std:c++20 does not compile the
# headers.
target_compile_features(StrayDLSS PRIVATE cxx_std_23)

# Linking the UE4SS target brings the import library, the include directories and the
# Unreal/File/DynamicOutput/fmt/imgui transitives with it. Never name UE4SS.lib directly.
target_link_libraries(StrayDLSS PRIVATE UE4SS stray_dlss_native stray_dlss_minhook d3d12 dxgi)

target_compile_options(StrayDLSS PRIVATE ${SDL_WARN})
target_compile_definitions(StrayDLSS PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    # Belt and braces: keeps std::mutex on the pre-17.10 layout so the DLL stays loadable
    # against an older msvcp140.dll in the prefix (the trap CMakeLists.txt at the root
    # documents for the add-on; UE4SS's own msvcp140 is whatever its build shipped).
    _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)

# DYNAMIC CRT (/MD), and it is an ABI CONTRACT, not a style choice — the opposite of the
# ReShade add-on in this repo, which is /MT for its own good reasons.
#
# CppUserModBase.hpp says it outright: "C++ mods will break if UE4SS and the mod don't use the
# same C Runtime library version. This includes them being compiled in different
# configurations (Debug/Release)." UE4SS itself is built /MD, and StringType (std::wstring)
# crosses the DLL boundary in both directions. A /MT mod would allocate in its own heap and
# free in UE4SS's.
set_property(TARGET StrayDLSS PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# UE4SS loads <Mods>/<ModName>/dlls/main.dll, falling back to <ModName>.dll
# (UE4SS/src/Mod/CppMod.cpp). CI stages it under the former name.
set_target_properties(StrayDLSS PROPERTIES OUTPUT_NAME "StrayDLSS")

if(MSVC)
    target_compile_options(StrayDLSS PRIVATE /Zi)
    target_link_options(StrayDLSS PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
