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
// full path. A UE4 crash dump line is `<module> 0x<base> + <offset>` and nothing more, so both
// halves are what makes a dump readable: the path's filename is the name the dump prints, and
// the base is what a dump's base must match for the crash to be ours. Logging the pair once at
// startup also catches the one silent failure of the naming scheme - a stale dlls/main.dll,
// which UE4SS loads in preference to dlls/<ModName>.dll. False (and both outputs left empty) if
// the address belongs to no loaded module.
bool ModuleIdentity(const void* addressInsideThisModule, const void*& base, std::wstring& path);

// A UFunction argument can name anything, and a name that reaches the filesystem must not be
// able to escape the asset directory. Refuse rather than sanitise, so a surprise is visible.
bool AssetNameIsSafe(const std::string& name);

} // namespace sds
