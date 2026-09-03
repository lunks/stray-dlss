// StrayDualSense — the handful of Win32 helpers every module used to carry its own copy of.
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

// The game executable's loaded image: base address and SizeOfImage, read from its own PE
// headers. Used to decide whether a pointer is a function in the game (a vtable entry) or a
// heap object, which is what makes the FAudioDevice search safe (SubmixDiscovery.hpp).
//
// Deliberately Win32 rather than UE4SS's SigScannerStaticData::m_modules_info: the answer is
// the same, and this compiles on the mingw lane where the SDK does not exist.
bool MainModuleRange(const void*& base, size_t& size);

} // namespace sds
