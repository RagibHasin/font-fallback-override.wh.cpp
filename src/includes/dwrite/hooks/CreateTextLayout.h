#pragma once

#include <dwrite.h>
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <winrt/base.h>

using winrt::com_ptr;

using CreateTextLayout0_t = HRESULT(STDMETHODCALLTYPE*)(
    IDWriteFactory8* self,
    WCHAR const* string,
    UINT32 stringLength,
    IDWriteTextFormat* textFormat,
    FLOAT maxWidth,
    FLOAT maxHeight,
    IDWriteTextLayout** textLayout
);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
CreateTextLayout0_t g_originalCreateTextLayout0 = nullptr;

STDMETHODIMP CreateTextLayout0_Hook(
    IDWriteFactory8* self,
    WCHAR const* string,
    UINT32 stringLength,
    IDWriteTextFormat* textFormat,
    FLOAT maxWidth,
    FLOAT maxHeight,
    IDWriteTextLayout** textLayout
) {
    Wh_Log(L"IDWriteFactory::CreateTextLayout");
    auto hr = g_originalCreateTextLayout0(
        self, string, stringLength, textFormat, maxWidth, maxHeight, textLayout
    );
    if (SUCCEEDED(hr) && textLayout && *textLayout) {
        // Try to set custom fallback on the text layout
        com_ptr<IDWriteTextLayout> layout;
        layout.copy_from(*textLayout);

        auto layout4 = layout.try_as<IDWriteTextLayout4>();
        if (layout4) {
            com_ptr<IDWriteFontFallback> customFallback;
            self->GetSystemFontFallback(customFallback.put());
            layout4->SetFontFallback(customFallback.get());

            Wh_Log(L"CreateTextLayout: Injected fallback into text layout");
        }
    }
    return hr;
}
