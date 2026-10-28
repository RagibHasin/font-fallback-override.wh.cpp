#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

struct SelectedObjectGuard {
    HDC hdc;
    HGDIOBJ previous;
    bool valid;

    SelectedObjectGuard(
        HDC dc,
        HGDIOBJ obj
    )
        : hdc(dc)
        , previous(SelectObject(
              dc,
              obj
          ))
        , valid(previous && previous != HGDI_ERROR) {
    }

    ~SelectedObjectGuard() {
        if (valid) {
            SelectObject(hdc, previous);
        }
    }

    SelectedObjectGuard(const SelectedObjectGuard&) = delete;
    SelectedObjectGuard& operator=(const SelectedObjectGuard&) = delete;

    SelectedObjectGuard(SelectedObjectGuard&&) = delete;
    SelectedObjectGuard&& operator=(SelectedObjectGuard&&) = delete;

    operator bool() const {
        return valid;
    }
};

/// Check whether the selected HFONT contains a glyph.
///
/// GetGlyphIndicesW returns 0xFFFF for an unavailable glyph.
///
/// Important:
///
/// This checks the actual selected GDI font, not merely the font family name.
/// Consequently it also handles fonts whose Unicode coverage is different
/// between faces.
static bool FontHasGlyph(
    HDC hdc,
    HFONT font,
    const std::wstring_view text
) {
    if (text.empty()) {
        return false;
    }

    const SelectedObjectGuard fontGuard(hdc, font);
    if (!fontGuard) {
        return false;
    }

    std::vector<WORD> glyphs(text.size());
    const auto result = GetGlyphIndicesW(
        hdc,
        text.cbegin(),
        static_cast<int>(text.size()),
        glyphs.data(),
        GGI_MARK_NONEXISTING_GLYPHS
    );
    if (result == GDI_ERROR) {
        return false;
    }

    constexpr auto invalid_glyph_id = 0xFFFFU;
    return std::ranges::all_of(glyphs, [&](auto glyph) {
        return glyph != invalid_glyph_id;
    });
}
