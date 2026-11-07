#pragma once

#include <algorithm>
#include <concepts>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "includes/gdi/font_utils.h"
#include "includes/gdi/text_run.h"
#include "includes/hooks.h"

template <typename DrawRun>
using DrawRunResult = std::invoke_result_t<const DrawRun&, const TextRun&, RECT&, DWORD>;

// The themed text APIs accept a rectangle rather than an explicit origin.
// Render each fallback run into its own horizontally adjacent rectangle while
// retaining the caller's vertical alignment, clipping and themed options.
template <typename DrawRun>
    requires std::invocable<
        const DrawRun&,
        const TextRun&,
        RECT&,
        DWORD>
static std::optional<DrawRunResult<DrawRun>> RenderThemedTextRuns(
    HDC hdc,
    LPCWSTR text,
    int count,
    const RECT* rect,
    DWORD textFlags,
    DrawRun&& drawRun
) {
    if (!text || !rect) {
        return std::nullopt;
    }

    const std::wstring_view textView(text, count < 0 ? wcslen(text) : count);

    Wh_Log(L"text: \"%.*s\", count: %d, size: %zu", textView.size(), text, count, textView.size());

    const auto originalFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT));
    if (!originalFont) {
        return std::nullopt;
    }

    Wh_Log(L">");
    const auto runs = BuildRuns(hdc, originalFont, textView);
    if (!runs || !runs->second) {
        return std::nullopt;
    }

    std::vector<LONG> measuredWidths;
    measuredWidths.reserve(runs->first.size());

    int totalWidth = 0;
    for (const auto& run : runs->first) {
        const SelectedObjectGuard fontGuard(hdc, get(run.font));
        if (!fontGuard) {
            return std::nullopt;
        }

        SIZE extent{};
        if (!GetTextExtentPoint32W(
                hdc, run.text.data(), static_cast<int>(run.text.size()), &extent
            )) {
            return std::nullopt;
        }

        const auto width = std::max(0L, extent.cx);
        measuredWidths.push_back(width);
        totalWidth += width;
    }

    int currentX = rect->left;
    if (textFlags & DT_CENTER) {
        currentX = rect->left + ((rect->right - rect->left) - totalWidth) / 2;
    } else if (textFlags & DT_RIGHT) {
        currentX = rect->right - totalWidth;
    }

    const auto runFlags = textFlags & ~(DT_CENTER | DT_RIGHT);
    std::optional<DrawRunResult<DrawRun>> lastResult;
    for (const auto& [run, measuredWidth] : std::views::zip(runs->first, measuredWidths)) {
        RECT runRect = *rect;
        runRect.left = currentX;
        runRect.right = currentX + measuredWidth;

        const auto result = std::forward<DrawRun>(drawRun)(run, runRect, runFlags);
        if constexpr (std::is_same_v<DrawRunResult<DrawRun>, HRESULT>) {
            if (FAILED(result)) {
                return result;
            }
        } else {
            if (result <= 0) {
                return result;
            }
        }
        lastResult = result;
        currentX += measuredWidth;
    }

    return lastResult;
}

static HRESULT WINAPI DrawThemeText_Hook(
    HTHEME theme,
    HDC hdc,
    int partId,
    int stateId,
    LPCWSTR text,
    int count,
    DWORD textFlags,
    DWORD textFlags2,
    LPCRECT rect
) {
    if (g_inHook || !text || !rect || count < -1 || count == 0 || (textFlags & DT_CALCRECT)) {
        return g_originalDrawThemeText(
            theme, hdc, partId, stateId, text, count, textFlags, textFlags2, rect
        );
    }

    const HookGuard _;
    const auto result = RenderThemedTextRuns(
        hdc, text, count, rect, textFlags, [&](const TextRun& run, RECT& runRect, DWORD runFlags) {
            const SelectedObjectGuard fontGuard(hdc, get(run.font));
            if (!fontGuard) {
                return E_FAIL;
            }
            return g_originalDrawThemeText(
                theme,
                hdc,
                partId,
                stateId,
                run.text.data(),
                static_cast<int>(run.text.size()),
                runFlags,
                textFlags2,
                &runRect
            );
        }
    );

    if (result) {
        return *result;
    }
    return g_originalDrawThemeText(
        theme, hdc, partId, stateId, text, count, textFlags, textFlags2, rect
    );
}

static HRESULT WINAPI DrawThemeTextEx_Hook(
    HTHEME theme,
    HDC hdc,
    int partId,
    int stateId,
    LPCWSTR text,
    int count,
    DWORD textFlags,
    LPRECT rect,
    const DTTOPTS* options
) {
    if (g_inHook || !text || !rect || count < -1 || count == 0 || (textFlags & DT_CALCRECT)
        || (options && (options->dwFlags & DTT_CALCRECT))) {
        return g_originalDrawThemeTextEx(
            theme, hdc, partId, stateId, text, count, textFlags, rect, options
        );
    }

    const HookGuard _;

    const auto result = RenderThemedTextRuns(
        hdc, text, count, rect, textFlags, [&](const TextRun& run, RECT& runRect, DWORD runFlags) {
            const SelectedObjectGuard fontGuard(hdc, get(run.font));
            if (!fontGuard) {
                return E_FAIL;
            }
            return g_originalDrawThemeTextEx(
                theme,
                hdc,
                partId,
                stateId,
                run.text.data(),
                static_cast<int>(run.text.size()),
                runFlags,
                &runRect,
                options
            );
        }
    );

    if (result) {
        return *result;
    }
    return g_originalDrawThemeTextEx(
        theme, hdc, partId, stateId, text, count, textFlags, rect, options
    );
}

static int WINAPI DrawShadowText_Hook(
    HDC hdc,
    LPCWSTR text,
    UINT count,
    RECT* rect,
    DWORD flags,
    COLORREF textColor,
    COLORREF shadowColor,
    int shadowOffsetX,
    int shadowOffsetY
) {
    if (g_inHook || !text || !rect || count == 0 || count > INT_MAX) {
        return g_originalDrawShadowText(
            hdc, text, count, rect, flags, textColor, shadowColor, shadowOffsetX, shadowOffsetY
        );
    }

    const HookGuard _;
    const auto result = RenderThemedTextRuns(
        hdc,
        text,
        static_cast<int>(count),
        rect,
        flags,
        [&](const TextRun& run, RECT& runRect, DWORD runFlags) {
            const SelectedObjectGuard fontGuard(hdc, get(run.font));
            if (!fontGuard) {
                return 0;
            }
            return g_originalDrawShadowText(
                hdc,
                run.text.data(),
                static_cast<UINT>(run.text.size()),
                &runRect,
                runFlags,
                textColor,
                shadowColor,
                shadowOffsetX,
                shadowOffsetY
            );
        }
    );

    if (result) {
        return *result;
    }
    return g_originalDrawShadowText(
        hdc, text, count, rect, flags, textColor, shadowColor, shadowOffsetX, shadowOffsetY
    );
}
