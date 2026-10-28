#pragma once

#include <dwrite.h>
#include <dwrite_2.h>
#include <dwrite_3.h>
#include <winrt/base.h>

using winrt::com_ptr;

using CreateTextFormat0_t = HRESULT(STDMETHODCALLTYPE*)(
    IDWriteFactory8* self,
    const WCHAR* family_name,
    IDWriteFontCollection* collection,
    DWRITE_FONT_WEIGHT weight,
    DWRITE_FONT_STYLE style,
    DWRITE_FONT_STRETCH stretch,
    FLOAT size,
    const WCHAR* locale,
    IDWriteTextFormat** format
);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
CreateTextFormat0_t g_originalCreateTextFormat0 = nullptr;

STDMETHODIMP CreateTextFormat0_Hook(
    IDWriteFactory8* self,
    WCHAR const* fontFamilyName,
    IDWriteFontCollection* fontCollection,
    DWRITE_FONT_WEIGHT fontWeight,
    DWRITE_FONT_STYLE fontStyle,
    DWRITE_FONT_STRETCH fontStretch,
    FLOAT fontSize,
    WCHAR const* localeName,
    IDWriteTextFormat** textFormat
) {
    Wh_Log(L"IDWriteFacotry::CreateTextFormat");
    auto hr = g_originalCreateTextFormat0(
        self,
        fontFamilyName,
        fontCollection,
        fontWeight,
        fontStyle,
        fontStretch,
        fontSize,
        localeName,
        textFormat
    );

    if (SUCCEEDED(hr) && textFormat && *textFormat) {
        // Try to set custom fallback on the text layout
        com_ptr<IDWriteTextFormat> format;
        format.copy_from(*textFormat);

        auto format3 = format.try_as<IDWriteTextFormat3>();
        if (format3) {
            com_ptr<IDWriteFontFallback> customFallback;
            self->GetSystemFontFallback(customFallback.put());

            format3->SetFontFallback(customFallback.get());

            Wh_Log(L"CreateTextFormat: Injected fallback into text format");
        }
    }
    return hr;
}

using CreateTextFormat6_t = HRESULT(STDMETHODCALLTYPE*)(
    IDWriteFactory8* self,
    const WCHAR* familyname,
    IDWriteFontCollection* collection,
    const DWRITE_FONT_AXIS_VALUE* axis_values,
    UINT32 num_axis,
    FLOAT fontsize,
    const WCHAR* localename,
    IDWriteTextFormat3** text_format
);

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
CreateTextFormat6_t g_originalCreateTextFormat6 = nullptr;

STDMETHODIMP CreateTextFormat6_Hook(
    IDWriteFactory8* self,
    const WCHAR* familyname,
    IDWriteFontCollection* collection,
    const DWRITE_FONT_AXIS_VALUE* axis_values,
    UINT32 num_axis,
    FLOAT fontsize,
    const WCHAR* localename,
    IDWriteTextFormat3** text_format
) {
    Wh_Log(L"IDWriteFactory6::CreateTextFormat");
    auto hr = g_originalCreateTextFormat6(
        self, familyname, collection, axis_values, num_axis, fontsize, localename, text_format
    );
    if (SUCCEEDED(hr) && text_format && *text_format) {
        // Try to set custom fallback on the text format
        com_ptr<IDWriteFontFallback> customFallback;
        self->GetSystemFontFallback(customFallback.put());
        (*text_format)->SetFontFallback(customFallback.get());

        Wh_Log(L"CreateTextFormat: Injected fallback into text format");
    }
    return hr;
}
