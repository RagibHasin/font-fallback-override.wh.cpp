#pragma once

#include <dwrite.h>
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <winrt/base.h>

#include "includes/fallback_rule.h"
#include "includes/utils.h"

using winrt::com_ptr;

static const Mutex<com_ptr<IDWriteFontFallback1>> g_fallback;

using GetSystemFontFallback_t = HRESULT(STDMETHODCALLTYPE*)(
    IDWriteFactory8* self,
    IDWriteFontFallback1** fallback
);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
static GetSystemFontFallback_t g_originalGetSystemFontFallback = nullptr;

STDMETHODIMP GetSystemFontFallback_Hook(
    IDWriteFactory8* self,
    IDWriteFontFallback1** fallback
) {
    Wh_Log(L"GetSystemFontFallback");
    auto fallbackLock = g_fallback.lock_mut();

    if (*fallbackLock == nullptr) {
        com_ptr<IDWriteFontFallback1> fallback0;
        auto hr = g_originalGetSystemFontFallback(self, fallback0.put());

        com_ptr<IDWriteFontFallbackBuilder> builder;
        if (FAILED(self->CreateFontFallbackBuilder(builder.put()))) {
            *fallback = fallback0.detach();
            return hr;
        };

        for (const auto& rule : *g_fallbackRules.lock()) {
            const DWRITE_UNICODE_RANGE range{rule.startChar, rule.endChar};
            auto fontFamily = rule.fontFamily.c_str();
            if (FAILED(builder->AddMapping(&range, 1, &fontFamily, 1))) {
                *fallback = fallback0.detach();
                return hr;
            };

            Wh_Log(
                L"FontFallback: U+%04X - U+%04X -> %s", rule.startChar, rule.endChar, fontFamily
            );
        }

        if (FAILED(builder->AddMappings(fallback0.get()))) {
            *fallback = fallback0.detach();
            return hr;
        };

        com_ptr<IDWriteFontFallback> fallback1;
        if (FAILED(builder->CreateFontFallback(fallback1.put()))) {
            *fallback = fallback0.detach();
            return hr;
        };
        fallback1.as(*fallbackLock);

        Wh_Log(L"Injected overrides");
    }

    fallbackLock->copy_to(fallback);
    return S_OK;
}
