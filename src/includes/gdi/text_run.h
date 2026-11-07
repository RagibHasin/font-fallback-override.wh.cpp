#pragma once

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "includes/fallback_rule.h"
#include "includes/gdi/font_cache.h"
#include "includes/utils.h"

/// Run representation.
///
/// A run is a contiguous UTF-16 range that should be rendered with one font.
struct TextRun {
    std::wstring_view text;
    FontHandle font;
};

/// Build runs.
///
/// Example:
///
///   original original original Bengali Bengali original
///
/// becomes:
///
///   [0,3] original
///   [3,5] Bengali
///   [8,...] original
///
/// We deliberately use character-level tests, rather than testing the whole
/// string, because GDI fallback is effectively a glyph/run problem.
static std::optional<std::pair<
    std::vector<TextRun>,
    bool>>
BuildRuns(
    HDC hdc,
    HFONT originalFont,
    std::wstring_view text
) {
    if (text.empty()) {
        return std::nullopt;
    }

    std::vector<TextRun> runs;
    auto replaced = false;

    while (!text.empty()) {
        const auto [codePoint, charLen] = DecodeCodePoint(text);

        if (charLen <= 0) {
            break;
        }

        const auto rule = FindRule(codePoint);
        const auto ch = text.substr(0, charLen);
        auto selectedFont = rule ? rule->GetReplacementFont(hdc, originalFont, ch) : originalFont;

        replaced |= get(selectedFont) != originalFont;

        if (!runs.empty() && runs.back().font == selectedFont
            && runs.back().text.cend() == text.cbegin()) {
            runs.back().text = {runs.back().text.cbegin(), ch.cend()};
        } else {
            runs.emplace_back(ch, selectedFont);
        }

        text = text.substr(charLen);
    }

    Wh_Log(L"> #runs: %zu", runs.size());

    return std::pair{runs, replaced};
}
