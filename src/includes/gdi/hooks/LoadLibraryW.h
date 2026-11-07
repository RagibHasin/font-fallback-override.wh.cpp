#include <windhawk_api.h>

using LoadLibraryExW_t = decltype(&LoadLibraryExW);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
LoadLibraryExW_t g_originalLoadLibraryExW;

extern bool InstallGDIHooks();

HMODULE WINAPI LoadLibraryExW_Hook(
    LPCWSTR name,
    HANDLE file,
    DWORD flags
) {
    HMODULE module = g_originalLoadLibraryExW(name, file, flags);
    if (module && InstallGDIHooks()) {
        Wh_ApplyHookOperations();
    }
    return module;
}
