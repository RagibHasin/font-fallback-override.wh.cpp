#pragma once

#include <uxtheme.h>

#include <windhawk_utils.h>

// Original function pointers.

using TextOutW_t = decltype(&TextOutW);
using ExtTextOutW_t = decltype(&ExtTextOutW);
using DrawTextW_t = decltype(&DrawTextW);
using DrawTextExW_t = decltype(&DrawTextExW);
using DrawThemeText_t = decltype(&DrawThemeText);
using DrawThemeTextEx_t = decltype(&DrawThemeTextEx);
using DrawShadowText_t = decltype(&DrawShadowText);

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)

static TextOutW_t g_originalTextOutW = nullptr;
static ExtTextOutW_t g_originalExtTextOutW = nullptr;
static DrawTextW_t g_originalDrawTextW = nullptr;
static DrawTextExW_t g_originalDrawTextExW = nullptr;
static DrawThemeText_t g_originalDrawThemeText = nullptr;
static DrawThemeTextEx_t g_originalDrawThemeTextEx = nullptr;
static DrawShadowText_t g_originalDrawShadowText = nullptr;

// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

/// Thread-local recursion guard.
///
/// Our implementation itself calls GDI APIs such as SelectObject,
/// GetGlyphIndicesW and GetTextExtentPoint32W. Those calls must not recursively
/// enter our hooks.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static thread_local bool g_inHook = false;

struct HookGuard {
    bool previous;

    HookGuard()
        : previous(g_inHook) {
        g_inHook = true;
    }
    ~HookGuard() {
        g_inHook = previous;
    }

    HookGuard(const HookGuard&) = delete;
    const HookGuard& operator=(const HookGuard&) = delete;

    HookGuard(HookGuard&&) = delete;
    HookGuard&& operator=(HookGuard&&) = delete;
};

// Resolves and hooks exported GDI functions.
//
// Uses GetModuleHandle/GetProcAddress rather than hardcoding addresses.
template <typename Prototype>
static bool HookFunctionFromDLL(
    const wchar_t* moduleName,
    const char* functionName,
    Prototype* hook,
    Prototype** original
) {
    const auto module = GetModuleHandleW(moduleName);

    if (!module) {
        Wh_Log(L"GetModuleHandleW(%s) failed", moduleName);

        return false;
    }

    const auto target =
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        reinterpret_cast<Prototype*>(GetProcAddress(module, functionName));

    if (!target) {
        Wh_Log(L"GetProcAddress(%S) failed", functionName);

        return false;
    }

    if (!WindhawkUtils::SetFunctionHook(target, hook, original)) {
        Wh_Log(L"SetFunctionHook(%S) failed", functionName);

        return false;
    }

    Wh_Log(L"Hooked %LS!%S", moduleName, functionName);

    return true;
}

template <typename Prototype>
static bool HookFunctionInVtable(
    IUnknown* vtable,
    uint32_t idx,
    Prototype* hook,
    Prototype** original
) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto target = reinterpret_cast<Prototype*>((*reinterpret_cast<void***>(vtable))[idx]);

    if (!target) {
        Wh_Log(L"Failed to find entry #%d of the Vtable", idx);

        return false;
    }

    if (!WindhawkUtils::SetFunctionHook(target, hook, original)) {
        Wh_Log(L"SetFunctionHook failed for entry #%d", idx);

        return false;
    }

    Wh_Log(L"Hooked entry #%d", idx);

    return true;
}
