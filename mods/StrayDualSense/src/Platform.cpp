#include "Platform.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

namespace sds {

std::wstring Widen(const std::string& utf8)
{
    if (utf8.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& wide)
{
    if (wide.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string a(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, a.data(), n, nullptr, nullptr);
    return a;
}

uint64_t NowMs()
{
    return static_cast<uint64_t>(::GetTickCount64());
}

bool MainModuleRange(const void*& base, size_t& size)
{
    base = nullptr;
    size = 0;
    HMODULE exe = ::GetModuleHandleW(nullptr);
    if (exe == nullptr)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const unsigned char*>(exe) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    base = exe;
    size = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
    return size != 0;
}

bool DirectoryExists(const std::wstring& path)
{
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0)
    {
        out.resize(static_cast<size_t>(n));
        const size_t got = std::fread(out.data(), 1, out.size(), f);
        out.resize(got);
    }
    std::fclose(f);
    return true;
}

namespace {

std::wstring DirOfModule(HMODULE module)
{
    wchar_t path[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(module, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return {};
    std::wstring s(path, n);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return {};
    return s.substr(0, slash + 1);
}

} // namespace

std::wstring GameBinariesDir()
{
    return DirOfModule(nullptr);
}

std::wstring ModuleDir(const void* addressInsideThisModule)
{
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(addressInsideThisModule), &self))
        return {};
    return DirOfModule(self);
}

} // namespace sds
