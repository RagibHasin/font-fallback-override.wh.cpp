#include <algorithm>
#include <array>
#include <string_view>

#include <winnt.h>

#include <windhawk_api.h>

using LoadLibraryExW_t = decltype(&LoadLibraryExW);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
LoadLibraryExW_t g_originalLoadLibraryExW;

HMODULE WINAPI LoadLibraryExW_Hook(
    LPCWSTR lpLibFileName,
    HANDLE hFile,
    DWORD dwFlags
) {
    constexpr std::array hookedDLLs{L"gdi32.dll", L"user32.dll", L"comctl32.dll", L"uxtheme.dll"};

    HMODULE module = g_originalLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (module
        && std::ranges::find(hookedDLLs, std::wstring_view{lpLibFileName}) != hookedDLLs.cend()) {
        Wh_ApplyHookOperations();
    }

    return module;
}
