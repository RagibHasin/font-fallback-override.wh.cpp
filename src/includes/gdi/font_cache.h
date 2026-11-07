#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>

#include "includes/utils.h"

// Replacement font cache.
//
// A replacement HFONT must have approximately the same metrics/style as the
// original font. We therefore derive LOGFONTW from the currently selected font
// and only replace lfFaceName.
//
// Cache key is LOGFONTW of the original HFONT + replacement family.
//
// The cache is intentionally simple. Font handles are generally long-lived in
// classic Win32 applications.

struct FontCacheKey {
    LOGFONTW originalFont;
    std::wstring family;

    bool operator==(const FontCacheKey& other) const {
        return family == other.family && originalFont.lfHeight == other.originalFont.lfHeight
               && originalFont.lfWidth == other.originalFont.lfWidth
               && originalFont.lfEscapement == other.originalFont.lfEscapement
               && originalFont.lfOrientation == other.originalFont.lfOrientation
               && originalFont.lfWeight == other.originalFont.lfWeight
               && originalFont.lfItalic == other.originalFont.lfItalic
               && originalFont.lfUnderline == other.originalFont.lfUnderline
               && originalFont.lfStrikeOut == other.originalFont.lfStrikeOut
               && originalFont.lfCharSet == other.originalFont.lfCharSet
               && originalFont.lfOutPrecision == other.originalFont.lfOutPrecision
               && originalFont.lfClipPrecision == other.originalFont.lfClipPrecision
               && originalFont.lfQuality == other.originalFont.lfQuality
               && originalFont.lfPitchAndFamily == other.originalFont.lfPitchAndFamily
               && std::wstring_view(static_cast<const wchar_t*>(originalFont.lfFaceName))
                      == std::wstring_view(
                          static_cast<const wchar_t*>(other.originalFont.lfFaceName)
                      );
    }
};

template <>
class std::hash<FontCacheKey> {
  public:
    static constexpr size_t operator()(const FontCacheKey& key) {
        auto hash = 0UZ;

        auto combine = [&](const auto& v) {
            // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers)
            hash ^= std::hash<std::remove_cvref_t<decltype(v)>>{}(v) + 0x9e3779b9 + (hash << 6)
                    + (hash >> 2);
            // NOLINTEND(cppcoreguidelines-avoid-magic-numbers)
        };

        combine(key.originalFont.lfHeight);
        combine(key.originalFont.lfWidth);
        combine(key.originalFont.lfEscapement);
        combine(key.originalFont.lfOrientation);
        combine(key.originalFont.lfWeight);
        combine(key.originalFont.lfItalic);
        combine(key.originalFont.lfUnderline);
        combine(key.originalFont.lfStrikeOut);
        combine(key.originalFont.lfCharSet);
        combine(key.originalFont.lfOutPrecision);
        combine(key.originalFont.lfClipPrecision);
        combine(key.originalFont.lfQuality);
        combine(key.originalFont.lfPitchAndFamily);
        combine(std::wstring_view(static_cast<const wchar_t*>(key.originalFont.lfFaceName)));
        combine(key.family);

        return hash;
    }
};

struct ManagedFontHandle {
  public:
    ManagedFontHandle(const HFONT font)
        : font(font) {
    }
    ~ManagedFontHandle() {
        DeleteObject(font);
    }

    ManagedFontHandle(const ManagedFontHandle&) = delete;
    ManagedFontHandle(ManagedFontHandle&&) = delete;
    ManagedFontHandle& operator=(const ManagedFontHandle&) = delete;
    ManagedFontHandle& operator=(ManagedFontHandle&&) = delete;

    HFONT operator*() const {
        return font;
    }

    bool operator==(const ManagedFontHandle& other) const {
        return font == other.font;
    }
    bool operator==(const HFONT other) const {
        return font == other;
    }

    bool operator!=(const ManagedFontHandle& other) const {
        return font != other.font;
    }
    bool operator!=(const HFONT other) const {
        return font != other;
    }

  private:
    HFONT font;
};

bool operator==(
    const HFONT a,
    const ManagedFontHandle& b
) {
    return b == a;
}
bool operator!=(
    const HFONT a,
    const ManagedFontHandle& b
) {
    return b != a;
}

using FontHandle = std::variant<std::shared_ptr<ManagedFontHandle>, HFONT>;

HFONT get(const FontHandle& handle) {
    if (std::holds_alternative<HFONT>(handle)) {
        return std::get<HFONT>(handle);
    }
    return **std::get<std::shared_ptr<ManagedFontHandle>>(handle);
}

static const Mutex<std::unordered_map<FontCacheKey, std::shared_ptr<ManagedFontHandle>>>
    g_fontCache;

static void ClearFontCache() {
    auto fontCache = g_fontCache.lock_mut();

    for (auto& [_, font] : *fontCache) {
        font.reset();
    }

    fontCache->clear();
}
