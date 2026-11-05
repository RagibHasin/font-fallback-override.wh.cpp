// ==WindhawkMod==
// @id              dwrite-font-fallback-override
// @name            Font Fallback Override
// @description     Intercepts DirectWrite font fallback and GDI text draw routines to replace fonts for specific characters
// @version         2.0
// @license         MIT
// @author          Muhammad Ragib Hasin
// @github          https://github.com/RagibHasin
// @include         *
// @compilerOptions -luser32 -lgdi32 -ldwrite -lole32 -lwindowsapp
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Font Fallback Override

Hooks DirectWrite's font fallback system to override font selection for specific
Unicode character ranges across all applications.

## Features

  - Per-Unicode-range fallback rules.
  - Works with...
    - classic GDI APIs: `DrawTextW`, `DrawTextExW`, `DrawThemeText`,
      `DrawThemeTextEx`, `DrawShadowText`, `TextOutW` and `ExtTextOutW`.
    - all DirectWrite APIs.
  - Preserves the original font's size, weight, italic and charset.
  - Detects whether the currently selected font contains a glyph and only
    replaces the font if it does not.

## Example

Configure:

U+0980-U+09FF -> Noto Sans Bengali
U+4E00-U+9FFF -> Microsoft YaHei

A string such as:

    "Hello বাংলা 世界"

can therefore be rendered as:

    original font -> Bengali fallback -> original font -> CJK fallback

*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- rules:
  - - startChar: "0980"
      $name: Start Character (Unicode hex, e.g., 4E00)
    - endChar: "09FF"
      $name: End Character (Unicode hex, e.g., 9FFF)
    - fontFamily: "Noto Sans Bengali"
      $name: Font Family Name
  $name: Fallback Rules
  $description: Define custom font fallback rules for character ranges
*/
// ==/WindhawkModSettings==

#include <dwrite.h>
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <unknwn.h>
#include <winrt/base.h>

#include <windhawk_api.h>
#include <windhawk_utils.h>

#include "includes/dwrite/hooks/CreateTextFormat.h"
#include "includes/dwrite/hooks/CreateTextLayout.h"
#include "includes/dwrite/hooks/GetSystemFontFallback.h"
#include "includes/fallback_rule.h"
#include "includes/gdi/font_cache.h"
#include "includes/gdi/hooks/DrawTextW.h"
#include "includes/gdi/hooks/DrawThemeText.h"
#include "includes/gdi/hooks/ExtTextOutW.h"
#include "includes/gdi/hooks/TextOutW.h"
#include "includes/hooks.h"

static bool InstallDWriteHooks() {
    IUnknown* factory = nullptr;
    auto hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &factory);
    if (FAILED(hr)) {
        Wh_Log(L"Failed to create first DirectWrite factory");
        return false;
    }

    // IUnknown        :  3 :  0
    // IDWriteFactory  : 21 :  3
    // IDWriteFactory1 :  2 : 24
    // IDWriteFactory2 :  5 : 26
    // IDWriteFactory3 :  9 : 31
    // IDWriteFactory4 :  3 : 40
    // IDWriteFactory5 :  5 : 43
    // IDWriteFactory6 :  7 : 48

    constexpr auto offsetGetSystemFontFallback = 26UZ; // 1st of IDWriteFactory2
    constexpr auto offsetCreateTextFormat0 = 15UZ;     // 13th of IDWriteFactory
    constexpr auto offsetCreateTextLayout0 = 18UZ;     // 16th of IDWriteFactory
    constexpr auto offsetCreateTextFormat6 = 54UZ;     // 7th of IDWriteFactory6

    const winrt::com_ptr<IDWriteFactory> factory0(factory, winrt::take_ownership_from_abi);
    const auto supportsFactory6 = static_cast<bool>(factory0.try_as<IDWriteFactory6>());

    using WindhawkUtils::SetFunctionHook;

    if (!HookFunctionInVtable(
            factory,
            offsetGetSystemFontFallback,
            GetSystemFontFallback_Hook,
            &g_originalGetSystemFontFallback
        )) {
        Wh_Log(L"Failed to hook GetSystemFontFallback");
        return false;
    }
    Wh_Log(L"Successfully hooked GetSystemFontFallback");

    if (!HookFunctionInVtable(
            factory, offsetCreateTextFormat0, CreateTextFormat0_Hook, &g_originalCreateTextFormat0
        )) {
        Wh_Log(L"Failed to find CreateTextFormat0");
        return false;
    }
    Wh_Log(L"Successfully hooked CreateTextFormat0");

    if (!HookFunctionInVtable(
            factory, offsetCreateTextLayout0, CreateTextLayout0_Hook, &g_originalCreateTextLayout0
        )) {
        Wh_Log(L"Failed to find CreateTextLayout0");
        return false;
    }
    Wh_Log(L"Successfully hooked CreateTextLayout0");

    if (!supportsFactory6) {
        Wh_Log(L"System does not support IDWriteFactory6");
        Wh_Log(L"Did not attempt hooking CreateTextFormat6");
        return true;
    }
    if (!HookFunctionInVtable(
            factory, offsetCreateTextFormat6, CreateTextFormat6_Hook, &g_originalCreateTextFormat6
        )) {
        Wh_Log(L"Failed to find CreateTextFormat6");
        return false;
    }
    Wh_Log(L"Successfully hooked CreateTextFormat6");

    return true;
}

BOOL Wh_ModInit() {
    Wh_Log(L"Font Fallback Override - Initializing");

    LoadRules();

    const auto successDWrite = InstallDWriteHooks();
    if (!successDWrite) {
        Wh_Log(L"Failed to hook DirectWrite font fallback machinery");
    }

    const auto successGDI =
        HookFunctionFromDLL(L"gdi32.dll", "TextOutW", TextOutW_Hook, &g_originalTextOutW)
        && HookFunctionFromDLL(
            L"gdi32.dll", "ExtTextOutW", ExtTextOutW_Hook, &g_originalExtTextOutW
        )
        && HookFunctionFromDLL(L"user32.dll", "DrawTextW", DrawTextW_Hook, &g_originalDrawTextW)
        && HookFunctionFromDLL(
            L"user32.dll", "DrawTextExW", DrawTextExW_Hook, &g_originalDrawTextExW
        )
        && HookFunctionFromDLL(
            L"uxtheme.dll", "DrawThemeText", DrawThemeText_Hook, &g_originalDrawThemeText
        )
        && HookFunctionFromDLL(
            L"uxtheme.dll", "DrawThemeTextEx", DrawThemeTextEx_Hook, &g_originalDrawThemeTextEx
        )
        && HookFunctionFromDLL(
            L"comctl32.dll", "DrawShadowText", DrawShadowText_Hook, &g_originalDrawShadowText
        );

    if (!successGDI) {
        Wh_Log(L"Failed to hook one or more GDI routines");
    }

    return successDWrite || successGDI;
}

void Wh_ModUninit() {
    Wh_Log(L"Font Fallback Override - Uninitializing");

    ClearFontCache();
}

void Wh_ModSettingsChanged() {
    Wh_Log(L"Settings changed, reloading...");

    LoadRules();

    *g_fallback.lock_mut() = nullptr;

    ClearFontCache();
}
