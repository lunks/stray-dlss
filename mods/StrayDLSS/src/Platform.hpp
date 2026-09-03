// StrayDLSS — copied verbatim from mods/StrayDualSense/src/Platform.hpp: the handful of Win32 helpers every module used to carry its own copy of.
//
// No UE4SS types. Compiles under mingw as well as MSVC.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sds {

std::wstring Widen(const std::string& utf8);
std::string  Narrow(const std::wstring& wide);

uint64_t NowMs();   // GetTickCount64

bool DirectoryExists(const std::wstring& path);

// Whole file into memory. Returns false if it cannot be opened; an empty file is "true" with
// an empty vector, so the caller can tell the two apart.
bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out);

// The directory Stray's own binaries live in (`.../Hk_project/Binaries/Win64/`), derived from
// the running executable and never hardcoded. Empty on failure. Trailing separator included.
std::wstring GameBinariesDir();

// The directory this mod's DLL lives in. Trailing separator included.
std::wstring ModuleDir(const void* addressInsideThisModule);

// The module an address belongs to: its load address (an HMODULE IS the base address) and its
// full path. Both plugins in this repository are loaded by UE4SS, which hardcodes
// `Mods/<Name>/dlls/main.dll`, so a UE4 crash dump names every one of them `main` and cannot
// say which. The dump line carries the base (`main 0x00006ffff4720000 + 771f6`), so logging
// the base once makes the module identifiable by matching it. False (and both outputs left
// empty) if the address belongs to no loaded module.
bool ModuleIdentity(const void* addressInsideThisModule, const void*& base, std::wstring& path);

// A UFunction argument can name anything, and a name that reaches the filesystem must not be
// able to escape the asset directory. Refuse rather than sanitise, so a surprise is visible.
bool AssetNameIsSafe(const std::string& name);

} // namespace sds
