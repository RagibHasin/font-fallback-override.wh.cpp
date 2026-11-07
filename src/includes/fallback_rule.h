#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <windhawk_utils.h>

#include "includes/gdi/font_cache.h"
#include "includes/gdi/font_utils.h"
#include "includes/utils.h"

struct FallbackRule {
    char32_t startChar;
    char32_t endChar;
    std::wstring fontFamily;

  public:
    /// Create a fallback HFONT based on the current selected font.
    ///
    /// We intentionally preserve:
    ///
    ///   height
    ///   width
    ///   escapement
    ///   orientation
    ///   weight
    ///   italic
    ///   underline
    ///   strikeout
    ///   charset
    ///   output precision
    ///   clip precision
    ///   quality
    ///   pitch
    ///
    /// Only lfFaceName changes.
    ///
    /// This is important because blindly calling CreateFontW with only a family
    /// name would lose DPI-scaled size and visual characteristics of the
    /// control.
    FontHandle GetReplacementFont(
        HDC hdc,
        HFONT originalFont,
        const std::wstring_view text
    ) const {
        LOGFONTW logFont{};
        if (!originalFont || FontHasGlyph(hdc, originalFont, text) || fontFamily.empty()
            || GetObjectW(originalFont, sizeof(logFont), &logFont) != sizeof(logFont)) {
            return originalFont;
        }

        const FontCacheKey key{.originalFont = logFont, .family = fontFamily};

        {
            auto fontCache = g_fontCache.lock();

            auto it = fontCache->find(key);
            if (it != fontCache->end()) {
                return it->second;
            }
        }

        wcsncpy_s(
            static_cast<wchar_t*>(logFont.lfFaceName), LF_FACESIZE, fontFamily.c_str(), _TRUNCATE
        );

        const auto replacement = CreateFontIndirectW(&logFont);
        if (!replacement) {
            return originalFont;
        }

        if (!FontHasGlyph(hdc, replacement, text)) {
            DeleteObject(replacement);
            return originalFont;
        }

        auto handle = std::make_shared<ManagedFontHandle>(replacement);
        return g_fontCache.lock_mut()->try_emplace(key, handle).first->second;
    }
};

static const Mutex<std::shared_ptr<std::vector<FallbackRule>>> g_fallbackRules;

static void LoadRules() {
    auto fallbackRules = std::make_shared<std::vector<FallbackRule>>();

    using WindhawkUtils::StringSetting;

    for (int i = 0;; ++i) {
        const auto startChar = StringSetting::make(L"rules[%d].startChar", i);

        if (wcslen(startChar) == 0) {
            break;
        }

        const auto endChar = StringSetting::make(L"rules[%d].endChar", i);
        const auto fontFamily = StringSetting::make(L"rules[%d].fontFamily", i);

        const auto rule = fallbackRules->emplace_back(
            ParseHex(startChar), ParseHex(endChar), std::wstring(fontFamily)
        );

        if (rule.startChar > rule.endChar) {
            Wh_Log(L"Ignoring invalid rule %d: U+%04X > U+%04X", i, rule.startChar, rule.endChar);
            fallbackRules->pop_back();
            continue;
        }

        if (rule.fontFamily.empty()) {
            Wh_Log(L"Ignoring rule %d with empty font family", i);
            fallbackRules->pop_back();
            continue;
        }

        Wh_Log(
            L"Loaded rule %d: U+%04X-U+%04X -> %s",
            i,
            rule.startChar,
            rule.endChar,
            rule.fontFamily.c_str()
        );
    }

    Wh_Log(L"Loaded %zu fallback rules", fallbackRules->size());

    *g_fallbackRules.lock_mut() = fallbackRules;
}

/// Find the user rule that applies to a Unicode scalar.
///
/// Rules are evaluated in declaration order. The first matching rule wins,
/// matching the spirit of the supplied DirectWrite mod.
static std::shared_ptr<FallbackRule> FindRule(char32_t codePoint) {
    if (codePoint < hiSurrogateBase) {
        Wh_Log(
            L"codePoint: %lc (U+%X)",
            static_cast<wchar_t>(codePoint),
            static_cast<uint32_t>(codePoint)
        );
    } else {
        Wh_Log(L"codePoint: %x", static_cast<uint32_t>(codePoint));
    }

    auto fallbackRules = g_fallbackRules.lock();

    for (auto& rule : **fallbackRules) {
        if (rule.startChar <= codePoint && codePoint <= rule.endChar) {
            return {*fallbackRules, &rule};
        }
    }

    return nullptr;
}
