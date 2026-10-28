#pragma once

#include <string>
#include <unordered_map>

#include "includes/utils.h"

// Replacement font cache.
//
// A replacement HFONT must have approximately the same metrics/style as the
// original font. We therefore derive LOGFONTW from the currently selected font
// and only replace lfFaceName.
//
// Cache key is original HFONT + replacement family.
//
// The cache is intentionally simple. Font handles are generally long-lived in
// classic Win32 applications.

struct FontCacheKey {
    HFONT originalFont;
    std::wstring family;

    bool operator==(const FontCacheKey& other) const {
        return originalFont == other.originalFont && family == other.family;
    }
};

template <>
class std::hash<FontCacheKey> {
  public:
    static constexpr std::size_t operator()(const FontCacheKey& key) {
        const size_t h1 = std::hash<void*>{}(static_cast<void*>(key.originalFont));
        const size_t h2 = std::hash<std::wstring>{}(key.family);

        // NOLINTNEXTLINE(cppcoreguidelines-avoid-magic-numbers)
        return h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
    }
};

static const Mutex<std::unordered_map<FontCacheKey, HFONT>> g_fontCache;

static void ClearFontCache() {
    auto fontCache = g_fontCache.lock_mut();

    for (auto& [_, font] : *fontCache) {
        if (font) {
            DeleteObject(font);
        }
    }

    fontCache->clear();
}
