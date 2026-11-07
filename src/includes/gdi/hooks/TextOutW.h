#pragma once

#include <string_view>

#include "includes/gdi/text_run.h"
#include "includes/hooks.h"

/// Render a TextOutW-style string.
///
/// `TextOutW` has no explicit per-character advance array, so when a fallback
/// run has different metrics we calculate the advance of the run using
/// GetTextExtentPoint32W and move the origin for the next run.
///
/// This is intentionally a "visual run" implementation rather than trying to
/// reproduce win32k's private fallback implementation.
static BOOL RenderTextOutRuns(
    HDC hdc,
    int x,
    int y,
    LPCWSTR text,
    int count
) {
    Wh_Log(L">");

    const auto originalFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
    if (!originalFont) {
        return g_originalTextOutW(hdc, x, y, text, count);
    }

    const std::wstring_view textView(text, count < 0 ? wcslen(text) : count);

    Wh_Log(L"text: \"%.s\", count: %d, size: %d", textView.size(), text, count, textView.size());

    Wh_Log(L">");
    const auto runs = BuildRuns(hdc, originalFont, textView);
    if (!runs || !runs->second) {
        return g_originalTextOutW(hdc, x, y, text, count);
    }

    int currentX = x;

    for (const auto& run : runs->first) {
        const SelectedObjectGuard fontGuard(hdc, get(run.font));
        if (!fontGuard) {
            continue;
        }

        const auto result = g_originalTextOutW(
            hdc, currentX, y, run.text.cbegin(), static_cast<int>(run.text.size())
        );
        if (!result) {
            return FALSE;
        }

        SIZE extent{};
        GetTextExtentPoint32W(hdc, run.text.cbegin(), static_cast<int>(run.text.size()), &extent);

        currentX += extent.cx;
    }

    return TRUE;
}

static BOOL WINAPI TextOutW_Hook(
    HDC hdc,
    int x,
    int y,
    LPCWSTR text,
    int count
) {
    if (g_inHook || !text || count <= 0) {
        return g_originalTextOutW(hdc, x, y, text, count);
    }

    const HookGuard _;

    return RenderTextOutRuns(hdc, x, y, text, count);
}
