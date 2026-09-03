# StrayDualSense — a UE4SS C++ plugin restoring Stray's DualSense features on PC/Proton.
#
# This file plays TWO roles, on purpose:
#
#   standalone   `cmake -S mods/StrayDualSense -B build` builds only the portable pieces —
#                the pure-logic library and its tests. Runs anywhere, including a Linux
#                runner, and needs no UE4SS at all.
#
#   sub-project  When added from a parent that has already done `add_subdirectory(RE-UE4SS)`,
#                the `UE4SS` target exists and the plugin DLL is built too. That is the
#                layout RE-UE4SS documents for C++ mods (docs/guides/creating-a-c++-mod.md);
#                `.github/workflows/dualsense.yml` generates the parent.
#
# The DLL cannot be built or tested on the machine that wrote it — no Windows, no MSVC, no
# game, no pad (CLAUDE.md §0). CI is the only build; the user's box is the only test.
cmake_minimum_required(VERSION 3.22)

project(StrayDualSense
    VERSION 0.2.0
    DESCRIPTION "DualSense adaptive triggers, coil haptics and controller speaker for Stray"
    LANGUAGES CXX)

option(STRAY_DS_WERROR "Treat warnings as errors" ON)

if(MSVC)
    # /external:* keeps /W4 /WX honest without drowning in UE4SS, imgui and fmt warnings:
    # anything included with <angle brackets> is treated as external and silenced.
    set(SDS_WARN /W4 $<$<BOOL:${STRAY_DS_WERROR}>:/WX> /external:anglebrackets /external:W0)
else()
    set(SDS_WARN -Wall -Wextra $<$<BOOL:${STRAY_DS_WERROR}>:-Werror>)
endif()

# ---------------------------------------------------------------------------------------
# Pure logic. No Windows, no UE4SS, no pad. This is the only part CI can PROVE:
#   AssetName      "SoundWave /Game/.../X.X" -> "X"
#   TriggerEffect  the game<->Sony enum translation and the ScePadTriggerEffectParam layout
#   LoopList       the game's own bLooping flags, parsed from haptic_loops.txt / spk_loops.txt
#   Fade           the FadeInTime / FadeOutTime gain ramps
#   SubmixChoice   the submix spike's FAudioDevice acceptance ladder - the part that got it
#                  wrong in the game on 2026-09-02, and is pure, so CI can prove the fix
#   CoilOwner      WHO DRIVES THE COILS, from the facts - the sentence whose absence let a
#                  silent submix pass for a working one on 2026-09-03
#   SubmixDsp      the submix spike's signal path: channel fold, soft clip, resampler, the
#                  level meter that IS the proof, and the lock-free ring the audio render
#                  thread writes into
# ---------------------------------------------------------------------------------------
add_library(straydualsense_core STATIC
    src/AssetName.cpp
    src/AssetName.hpp
    src/CoilOwner.cpp
    src/CoilOwner.hpp
    src/Fade.hpp
    src/LoopList.cpp
    src/LoopList.hpp
    src/SubmixChoice.cpp
    src/SubmixChoice.hpp
    src/SubmixDsp.cpp
    src/SubmixDsp.hpp
    src/TriggerEffect.cpp
    src/TriggerEffect.hpp)
target_include_directories(straydualsense_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(straydualsense_core PUBLIC cxx_std_20)
target_compile_options(straydualsense_core PRIVATE ${SDS_WARN})

enable_testing()
foreach(t asset_name trigger_effect loop_list fade submix_dsp submix_choice coil_owner)
    add_executable(straydualsense_test_${t} tests/test_${t}.cpp)
    target_link_libraries(straydualsense_test_${t} PRIVATE straydualsense_core)
    target_compile_options(straydualsense_test_${t} PRIVATE ${SDS_WARN})
    add_test(NAME ${t} COMMAND straydualsense_test_${t})
endforeach()

# ---------------------------------------------------------------------------------------
# The plugin DLL. Only when a parent has already brought in RE-UE4SS.
# ---------------------------------------------------------------------------------------
if(NOT TARGET UE4SS)
    message(STATUS
        "StrayDualSense: the UE4SS target is absent, so only the portable tests are built. "
        "To build the plugin, configure a parent project that does add_subdirectory(RE-UE4SS) "
        "before add_subdirectory(StrayDualSense) — see .github/workflows/dualsense.yml.")
    return()
endif()

if(NOT WIN32)
    message(FATAL_ERROR
        "StrayDualSense: the plugin is Windows/MSVC only. UE4SS passes std:: types across the "
        "DLL boundary, so the mod must be built by the same toolchain and CRT as UE4SS itself.")
endif()

add_library(StrayDualSense SHARED
    src/AssetName.cpp
    src/AssetName.hpp
    src/AudioPlayer.cpp
    src/AudioPlayer.hpp
    src/CoilOwner.cpp
    src/CoilOwner.hpp
    src/Config.cpp
    src/Config.hpp
    src/Fade.hpp
    src/HidMode.cpp
    src/HidMode.hpp
    src/Log.cpp
    src/Log.hpp
    src/LoopList.cpp
    src/LoopList.hpp
    src/Mod.cpp
    src/Platform.cpp
    src/Platform.hpp
    src/Runtime.cpp
    src/Runtime.hpp
    src/ScePad.cpp
    src/ScePad.hpp
    src/SubmixChoice.cpp
    src/SubmixChoice.hpp
    src/SubmixDiscovery.cpp
    src/SubmixDiscovery.hpp
    src/SubmixDsp.cpp
    src/SubmixDsp.hpp
    src/SubmixSink.cpp
    src/SubmixSink.hpp
    src/SubmixTap.cpp
    src/SubmixTap.hpp
    src/TriggerEffect.cpp
    src/TriggerEffect.hpp
    src/Triggers.cpp
    src/Triggers.hpp
    src/Version.hpp
    src/Wasapi.cpp
    src/Wasapi.hpp)

target_include_directories(StrayDualSense PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# C++23 is not a preference: RE-UE4SS's root CMakeLists sets CMAKE_CXX_STANDARD 23 and UE4SS
# declares `target_compile_features(UE4SS PUBLIC cxx_std_23)`. /std:c++20 does not compile the
# headers. Stated here as well so a standalone reader is not misled.
target_compile_features(StrayDualSense PRIVATE cxx_std_23)

# Linking the UE4SS target brings the import library, the include directories and the
# Unreal/File/DynamicOutput/fmt/imgui transitives with it. Never name UE4SS.lib directly.
target_link_libraries(StrayDualSense PRIVATE UE4SS)

# ole32    : CoCreateInstance / PropVariantClear / CoTaskMemFree for the WASAPI paths
# setupapi : SetupDiGetClassDevs / EnumDeviceInterfaces / GetDeviceInterfaceDetail (HidMode)
# hid      : HidD_GetHidGuid / HidD_GetAttributes / HidD_GetPreparsedData / HidP_GetCaps
target_link_libraries(StrayDualSense PRIVATE ole32 setupapi hid)

target_compile_options(StrayDualSense PRIVATE ${SDS_WARN})
target_compile_definitions(StrayDualSense PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# DYNAMIC CRT (/MD), and it is an ABI CONTRACT, not a style choice — the opposite of the
# ReShade add-on in this repo, which is /MT for its own good reasons.
#
# CppUserModBase.hpp says it outright: "C++ mods will break if UE4SS and the mod don't use the
# same C Runtime library version. This includes them being compiled in different
# configurations (Debug/Release)." UE4SS itself is built /MD, and StringType (std::wstring) and
# UnrealScriptFunctionCallable (std::function) cross the DLL boundary in both directions. A /MT
# mod would allocate in its own heap and free in UE4SS's.
set_property(TARGET StrayDualSense PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# UE4SS loads <Mods>/<ModName>/dlls/main.dll, falling back to <ModName>.dll
# (UE4SS/src/Mod/CppMod.cpp). CI stages it under the former name.
set_target_properties(StrayDualSense PROPERTIES OUTPUT_NAME "StrayDualSense")

if(MSVC)
    target_compile_options(StrayDualSense PRIVATE /Zi)
    target_link_options(StrayDualSense PRIVATE /DEBUG /OPT:REF /OPT:ICF)
endif()
