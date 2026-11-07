#pragma once

#include <algorithm>
#include <ranges>
#include <span>
#include <string_view>

#include "includes/gdi/text_run.h"
#include "includes/hooks.h"

/// ExtTextOutW renderer.
///
/// If lpDx is supplied, we preserve the caller's exact character advances.
/// This is especially important for controls because they frequently supply
/// explicit positioning.
///
/// For the no-lpDx case we calculate run advances from the fallback font.
///
/// ETO_GLYPH_INDEX is deliberately bypassed because lpString then contains
/// glyph IDs rather than Unicode characters.
static BOOL RenderExtTextOutRuns(
    HDC hdc,
    int x,
    int y,
    UINT options,
    const RECT* rect,
    LPCWSTR text,
    UINT count,
    const INT* dx
) {
    // TODO: measure and draw for non-left alignment
    const auto textAlign = GetTextAlign(hdc);
    if ((options & ETO_GLYPH_INDEX) || (textAlign & TA_RIGHT) || (textAlign & TA_CENTER)) {
        return g_originalExtTextOutW(hdc, x, y, options, rect, text, count, dx);
    }

    Wh_Log(L">");

    const auto originalFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
    if (!originalFont) {
        return g_originalExtTextOutW(hdc, x, y, options, rect, text, count, dx);
    }

    Wh_Log(L">");

    const std::wstring_view textView(text, count);

    Wh_Log(L"text: \"%.*s\", count: %u", count, text, count);

    const auto runs = BuildRuns(hdc, originalFont, textView);
    if (!runs || !runs->second) {
        return g_originalExtTextOutW(hdc, x, y, options, rect, text, count, dx);
    }

    if (options & ETO_OPAQUE) {
        g_originalExtTextOutW(hdc, x, y, ETO_OPAQUE, rect, nullptr, 0, nullptr);
    }

    options &= ~ETO_OPAQUE;

    const auto displacementsHaveY = options & ETO_PDY;
    const auto dxFactor = displacementsHaveY ? 2UZ : 1;
    const auto dxSpan = dx ? std::span{dx, textView.size() * dxFactor} : std::span<INT>{};
    auto currentX = x;
    auto currentY = y;
    for (const auto& run : runs->first) {
        const SelectedObjectGuard fontGuard(hdc, get(run.font));
        if (!fontGuard) {
            continue;
        }

        const auto runDx =
            dx ? dxSpan.subspan((run.text.cbegin() - text) * dxFactor, run.text.size() * dxFactor)
               : std::span<INT>{};
        const auto result = g_originalExtTextOutW(
            hdc, currentX, currentY, options, rect, run.text.cbegin(), run.text.size(), runDx.data()
        );

        if (dx) {
            auto enumerated = std::views::zip(std::views::iota(0UZ, runDx.size()), runDx);
            auto runDxOnly = enumerated | std::views::filter([=](auto p) {
                                 return !displacementsHaveY || std::get<0>(p) % 2 == 0;
                             })
                             | std::views::elements<1>;
            auto runDyOnly = enumerated | std::views::filter([=](auto p) {
                                 return displacementsHaveY && std::get<0>(p) % 2 != 0;
                             })
                             | std::views::elements<1>;
            currentX += std::ranges::fold_left(runDxOnly, 0, std::plus{});
            currentY += std::ranges::fold_left(runDyOnly, 0, std::plus{});
        } else {
            SIZE extent{};
            GetTextExtentPoint32W(
                hdc, run.text.cbegin(), static_cast<int>(run.text.size()), &extent
            );
            currentX += extent.cx;
        }

        if (!result) {
            return FALSE;
        }
    }

    return TRUE;
}

static BOOL WINAPI ExtTextOutW_Hook(
    HDC hdc,
    int x,
    int y,
    UINT options,
    const RECT* rect,
    LPCWSTR text,
    UINT count,
    const INT* dx
) {
    if (g_inHook || !text || count == 0) {
        return g_originalExtTextOutW(hdc, x, y, options, rect, text, count, dx);
    }

    const HookGuard _;

    return RenderExtTextOutRuns(hdc, x, y, options, rect, text, count, dx);
}
