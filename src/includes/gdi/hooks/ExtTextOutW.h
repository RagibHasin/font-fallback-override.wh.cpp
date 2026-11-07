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
    Wh_Log(L">");

    if (options & ETO_GLYPH_INDEX) {
        return g_originalExtTextOutW(hdc, x, y, options, rect, text, count, dx);
    }

    Wh_Log(L">");

    const auto originalFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
    if (!originalFont) {
        return g_originalExtTextOutW(hdc, x, y, options, rect, text, count, dx);
    }

    Wh_Log(L">");

    const auto textView = std::wstring_view(text, count < 0 ? wcslen(text) : count);

    Wh_Log(L"text: \"%s\", count: %d, size: %d", text, count, textView.size());

    Wh_Log(L">");
    const auto runs = BuildRuns(hdc, originalFont, textView);
    if (!runs || !runs->second) {
        return g_originalExtTextOutW(hdc, x, y, options, rect, text, count, dx);
    }

    if (options & ETO_OPAQUE) {
        g_originalExtTextOutW(hdc, x, y, ETO_OPAQUE, rect, nullptr, 0, nullptr);
    }

    options &= ~ETO_OPAQUE;

    const size_t dxFactor = (options & ETO_PDY) ? 2 : 1;
    const auto dxSpan = dx ? std::span{dx, textView.size() * dxFactor} : std::span<INT>{};
    auto currentX = x;
    for (const auto& run : runs->first) {
        const SelectedObjectGuard fontGuard(hdc, run.font);
        if (!fontGuard) {
            continue;
        }

        const auto runDx = dx ? dxSpan.subspan(run.text.cbegin() - text, run.text.size() * dxFactor)
                              : std::span<INT>{};
        const auto result = g_originalExtTextOutW(
            hdc, currentX, y, options, rect, run.text.cbegin(), run.text.size(), runDx.data()
        );

        if (dx) {
            auto indices = std::views::iota(0UZ, runDx.size());
            auto runDxOnly = std::views::zip(indices, runDx) | std::views::filter([](auto p) {
                                 return std::get<0>(p) % 2 == 0;
                             })
                             | std::views::elements<1>;
            currentX += (options & ETO_PDY) ? std::ranges::fold_left(runDxOnly, 0, std::plus{})
                                            : std::ranges::fold_left(runDx, 0, std::plus{});
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
