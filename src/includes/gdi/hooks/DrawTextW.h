#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <windhawk_api.h>

#include "includes/gdi/font_utils.h"
#include "includes/gdi/text_run.h"
#include "includes/hooks.h"
#include "includes/utils.h"

using namespace std::literals;

// DrawText support.
//
// The layout below follows the structure of DrawTextExW in text.c:
// paragraphs are split into logical lines, DT_WORDBREAK is applied at word
// boundaries, tabs are expanded to tab stops, prefixes are removed and
// underlined, and the ellipsis modes are handled before rendering.
//
// Unlike the native implementation, every text measurement and every text
// draw is performed through g_originalDrawTextW. The currently selected font
// is changed for each fallback run, so the layout uses the metrics of the
// font that will actually draw each character.

struct ParsedDrawText {
    std::wstring text;
    // For every UTF-16 code unit in text, the source offset consumed to produce
    // it. Both halves of a surrogate pair point to the end of that pair.
    std::vector<const wchar_t*> sourceEnds;
    std::vector<size_t> underlineOffsets;

    void AppendParsedCharacter(std::wstring_view text) {
        const auto [_, charLen] = DecodeCodePoint(text);
        const auto ch = text.substr(0, charLen);
        this->text.append(ch);
        sourceEnds.append_range(std::views::repeat(text.cend(), charLen));
    }

  public:
    ParsedDrawText() = default;

    ParsedDrawText(
        std::wstring_view text,
        const UINT format
    ) {
        this->text.reserve(text.size());
        sourceEnds.reserve(text.size());

        const auto noPrefix = (format & DT_NOPREFIX) != 0;
        const auto hidePrefix = (format & DT_HIDEPREFIX) != 0;

        while (!text.empty()) {
            if (!noPrefix && text[0] == L'\x1F') {
                // Win16 Katakana access-key marker: discard the marker and its
                // letter.
                text = text.substr(std::min(text.size(), 2UZ));
                continue;
            }

            if (!noPrefix && (text[0] == L'&' || text[0] == L'\x1E')) {
                if (text.size() == 1) {
                    // A trailing ampersand is literal text.
                    AppendParsedCharacter(text.substr(0, 1));
                    text = text.substr(1);
                    continue;
                }

                if (text.starts_with(L"&&")) {
                    // && becomes one literal ampersand.
                    this->text.push_back(L'&');
                    text = text.substr(2);
                    sourceEnds.push_back(text.cbegin());
                    continue;
                }

                const auto [_, charLen] = DecodeCodePoint(text.substr(1));
                const auto sourceEnd = charLen + 1;
                if (!hidePrefix) {
                    underlineOffsets.push_back(this->text.size());
                }
                AppendParsedCharacter(text.substr(1, charLen));
                text = text.substr(sourceEnd);
                continue;
            }

            const auto [_, charLen] = DecodeCodePoint(text);
            AppendParsedCharacter(text.substr(0, charLen));
            text = text.substr(charLen);
        }
    }
};

static bool IsLineBreakCharacter(wchar_t ch) {
    return ch == L'\r' || ch == L'\n';
}

static bool IsDrawTextBreakCharacter(wchar_t ch) {
    return ch == L' ' || ch == L'\t';
}

static size_t NextCodePointBoundary(
    const std::wstring_view text,
    const size_t position,
    const size_t end
) {
    if (position >= end) {
        return end;
    }

    const auto [_, charLen] = DecodeCodePoint(text.substr(position, end - position));
    return position + charLen;
}

struct DrawTextLine {
    size_t start = 0;
    size_t end = 0;
    size_t next = 0;
    SIZE size{};
    std::wstring renderedText;
    std::vector<size_t> underlineOffsets;
    bool ellipsified = false;
};

static constexpr auto ellipsis = L"..."sv;

struct Modification {
    size_t start;
    size_t length;
    std::wstring replacement;
};

struct Context {
    HDC hdc;
    std::wstring_view text;
    RECT* rect;
    UINT format;
    DRAWTEXTPARAMS* params;
    bool allowModifyString;
    HFONT originalFont;
    ParsedDrawText parsed;
    std::vector<TextRun> runs;

  public:
    // Measure one string using a specific font. DT_CALCRECT is deliberately sent
    // to the trampoline, not to the hooked DrawTextW entry point.
    [[nodiscard]] std::optional<SIZE> MeasureTextWithFont(const TextRun& run) const {
        if (run.text.empty()) {
            return SIZE{0, 0};
        }

        const SelectedObjectGuard fontGuard(hdc, get(run.font));
        if (!fontGuard) {
            return std::nullopt;
        }

        RECT measure{0, 0, INT_MAX / 4, INT_MAX / 4};
        const auto measureFormat =
            DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX | (format & DT_RTLREADING);
        const auto result = g_originalDrawTextW(
            hdc, run.text.cbegin(), static_cast<int>(run.text.size()), &measure, measureFormat
        );
        if (result <= 0) {
            return std::nullopt;
        }

        SIZE measured{
            std::max(0L, measure.right - measure.left), std::max(0L, measure.bottom - measure.top)
        };
        if (!measured.cy) {
            measured.cy = result;
        }
        return measured;
    }

    /// Measure a run sequence with each run's actual font.
    ///
    /// This is the central measurement primitive used by wrapping, alignment, tabs,
    /// and underlines.
    [[nodiscard]] std::optional<SIZE> MeasureTextWithFallback(const std::wstring_view text) const {
        if (text.empty()) {
            return SIZE{0, 0};
        }

        Wh_Log(L">");
        const auto runs = BuildRuns(hdc, originalFont, text);
        if (!runs) {
            return std::nullopt;
        }

        SIZE measured{0, 0};
        for (const auto& run : runs->first) {
            const auto runSize = MeasureTextWithFont(run);
            if (!runSize) {
                return std::nullopt;
            }

            measured.cx += runSize->cx;
            measured.cy = std::max(measured.cy, runSize->cy);
        }

        return measured;
    }

    // Measure a line with expanded tabs. Tab stops are relative to the line's
    // left edge, as they are in DrawTextExW.
    [[nodiscard]] std::optional<SIZE> MeasureDrawTextRange(
        const std::wstring_view text,
        const size_t start,
        const size_t end,
        const int tabWidth
    ) const {
        if (end <= start) {
            return SIZE{0, 0};
        }

        if (!(format & DT_EXPANDTABS)) {
            return MeasureTextWithFallback(text.substr(start, end - start));
        }

        const auto safeTabWidth = std::max(1, tabWidth);

        SIZE measured{0, 0};
        auto segmentStart = start;
        for (auto i = start; i <= end; ++i) {
            if (i != end && text[i] != L'\t') {
                continue;
            }

            if (i > segmentStart) {
                const auto segmentSize =
                    MeasureTextWithFallback(text.substr(segmentStart, i - segmentStart));
                if (!segmentSize) {
                    return std::nullopt;
                }

                measured.cx += segmentSize->cx;
                measured.cy = std::max(measured.cy, segmentSize->cy);
            }

            if (i < end) {
                measured.cx = ((measured.cx / safeTabWidth) + 1) * safeTabWidth;
                segmentStart = i + 1;
            }
        }

        return measured;
    }

    // Find the longest UTF-16-safe prefix whose fallback-font width fits.
    [[nodiscard]] size_t FindFittingPrefix(
        const std::wstring_view text,
        const size_t start,
        const size_t end,
        const int width,
        const int tabWidth
    ) const {
        std::vector<size_t> boundaries;
        boundaries.push_back(start);
        for (auto i = start; i < end;) {
            i = NextCodePointBoundary(text, i, end);
            boundaries.push_back(i);
        }

        auto low = 0UZ;
        auto high = boundaries.size() - 1;
        while (low < high) {
            const auto middle = low + (high - low + 1) / 2;
            const auto measured = MeasureDrawTextRange(text, start, boundaries[middle], tabWidth);
            if (!measured) {
                high = middle - 1;
                continue;
            }

            if (measured->cx <= width) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }

        return boundaries[low];
    }

    [[nodiscard]] std::wstring MakeEndEllipsis(
        const std::wstring_view text,
        const int width,
        const int tabWidth,
        const bool wordEllipsis
    ) const {
        auto bestEnd = 0UZ;
        std::wstring candidate(ellipsis);
        const auto size = MeasureDrawTextRange(candidate, 0, candidate.size(), tabWidth);
        if (!size) {
            return candidate;
        }

        for (auto end = text.size(); end > 0;) {
            std::wstring test{text.substr(0, end)};
            test += ellipsis;
            const auto size = MeasureDrawTextRange(test, 0, test.size(), tabWidth);
            if (size && size->cx <= width) {
                bestEnd = end;
                candidate = std::move(test);
                break;
            }
            auto newEnd = end - 1;
            if (newEnd > 0 && DecodeCodePoint(text.substr(newEnd - 1)).second == 2) {
                --newEnd;
            }

            end = newEnd;
        }

        if (wordEllipsis && bestEnd > 0 && bestEnd < text.size()) {
            auto wordEnd = bestEnd;
            while (wordEnd > 0 && !IsDrawTextBreakCharacter(text[wordEnd - 1])) {
                --wordEnd;
            }
            if (wordEnd > 0) {
                std::wstring wordCandidate{text.substr(0, wordEnd)};
                wordCandidate += ellipsis;
                const auto size =
                    MeasureDrawTextRange(wordCandidate, 0, wordCandidate.size(), tabWidth);
                if (size && size->cx <= width) {
                    candidate = std::move(wordCandidate);
                }
            }
        }

        // Per text.c, the ellipsis itself is returned even when it cannot fit.
        return candidate;
    }

    [[nodiscard]] std::wstring MakePathEllipsis(
        const std::wstring_view text,
        const int width,
        const int tabWidth
    ) const {
        const auto slash = text.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            return MakeEndEllipsis(text, width, tabWidth, false);
        }

        auto prefix = text.substr(0, slash);
        const auto suffix = text.substr(slash);
        std::wstring candidate;

        for (;;) {
            candidate.clear();
            candidate.append(prefix);
            candidate.append(ellipsis);
            candidate.append(suffix);

            const auto size = MeasureDrawTextRange(candidate, 0, candidate.size(), tabWidth);
            if (size && size->cx <= width) {
                return candidate;
            }

            if (prefix.empty()) {
                break;
            }

            auto newLength = prefix.size() - 1;
            if (newLength > 0 && static_cast<uint16_t>(prefix[newLength]) >= loSurrogateBase
                && static_cast<uint16_t>(prefix[newLength]) < postSurrogateBase) {
                --newLength;
            }

            prefix = prefix.substr(0, std::max(0UZ, newLength));
        }

        std::wstring output{ellipsis};
        output += suffix;

        return output;
    }

    bool DrawTextLineWithFallback(
        const std::wstring_view text,
        const int x,
        const int y,
        const RECT* const clipRect,
        const int tabWidth
    ) const {
        const auto drawFormat =
            (format
             & ~(DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_RIGHT | DT_VCENTER | DT_BOTTOM
                 | DT_EXPANDTABS | DT_TABSTOP | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS
                 | DT_WORD_ELLIPSIS | DT_MODIFYSTRING | DT_HIDEPREFIX))
            | DT_SINGLELINE | DT_NOPREFIX;

        auto currentX = x;
        auto segmentStart = 0UZ;
        const auto safeTabWidth = std::max(1, tabWidth);

        auto drawSegment = [&](const std::wstring_view text) -> bool {
            if (text.empty()) {
                return true;
            }

            Wh_Log(L">");
            const auto runs = BuildRuns(hdc, originalFont, text);
            if (!runs) {
                return false;
            }

            for (const auto& run : runs->first) {
                const auto runSize = MeasureTextWithFont(run);
                if (!runSize) {
                    return false;
                }

                const SelectedObjectGuard fontGuard(hdc, get(run.font));
                if (!fontGuard) {
                    return false;
                }

                auto drawRect = clipRect ? *clipRect : RECT{currentX, y, INT_MAX / 4, INT_MAX / 4};
                drawRect.left = currentX;
                drawRect.top = y;

                const auto result = g_originalDrawTextW(
                    hdc, run.text.cbegin(), static_cast<int>(run.text.size()), &drawRect, drawFormat
                );
                if (result <= 0) {
                    return false;
                }

                currentX += runSize->cx;
            }

            return true;
        };

        if (!(format & DT_EXPANDTABS)) {
            return drawSegment(text);
        }

        for (auto i = 0UZ; i <= text.size(); ++i) {
            if (i != text.size() && text[i] != L'\t') {
                continue;
            }

            if (!drawSegment(text.substr(segmentStart, i - segmentStart))) {
                return false;
            }

            if (i < text.size()) {
                const auto lineOffset = currentX - x;
                currentX = x + ((lineOffset / safeTabWidth) + 1) * safeTabWidth;
                segmentStart = i + 1;
            }
        }

        return true;
    }

    void DrawDrawTextUnderline(
        const std::wstring_view lineText,
        const size_t offset,
        const int x,
        const int y,
        const RECT* const clipRect,
        const int tabWidth
    ) const {
        if (offset < 0 || offset >= lineText.size()) {
            return;
        }

        const auto before = MeasureDrawTextRange(lineText, 0, offset, tabWidth);
        if (!before) {
            return;
        }
        const auto through = MeasureDrawTextRange(lineText, 0, offset + 1, tabWidth);
        if (!through) {
            return;
        }

        auto startX = x + before->cx;
        auto endX = x + through->cx - 1;
        const auto lineY = y;
        auto underlineStartX = startX;
        auto underlineEndX = endX;

        if (clipRect) {
            if (startX > clipRect->right || endX < clipRect->left || lineY < clipRect->top
                || lineY > clipRect->bottom) {
                return;
            }

            underlineStartX = std::max(underlineStartX, clipRect->left);
            underlineEndX = std::min(underlineEndX, clipRect->right);
        }

        if (underlineEndX < underlineStartX) {
            return;
        }

        const HPEN pen = CreatePen(PS_SOLID, 1, GetTextColor(hdc));
        if (!pen) {
            return;
        }

        {
            const SelectedObjectGuard penGuard(hdc, pen);
            if (penGuard) {
                MoveToEx(hdc, static_cast<int>(underlineStartX), lineY, nullptr);
                LineTo(hdc, static_cast<int>(underlineEndX + 1), lineY);
            }
        }
        DeleteObject(pen);
    }

    [[nodiscard]] std::optional<DrawTextLine> BuildNextDrawTextLine(
        const size_t position,
        const int availableWidth,
        const int tabWidth,
        const bool lastLine
    ) const {
        const std::wstring_view text(parsed.text);

        const auto paragraphEnd =
            (format & DT_SINGLELINE)
                ? text.size()
                : (std::ranges::find_if(text.substr(position), &IsLineBreakCharacter)
                   - text.cbegin());

        DrawTextLine line{
            .start = position, .end = paragraphEnd, .next = paragraphEnd, .ellipsified = false
        };

        Wh_Log(L"line.next: %zu", line.next);

        // DrawText's word wrapping is opt-in. Without DT_WORDBREAK, a paragraph is
        // one line and is only clipped/ellipsified according to the requested
        // flags.
        if ((format & DT_WORDBREAK) && !(format & DT_SINGLELINE) && paragraphEnd > position) {
            const auto fullSize = MeasureDrawTextRange(text, position, paragraphEnd, tabWidth);
            if (!fullSize) {
                return std::nullopt;
            }

            if (fullSize->cx > availableWidth) {
                const auto fitEnd =
                    FindFittingPrefix(text, position, paragraphEnd, availableWidth, tabWidth);

                Wh_Log(
                    L"fullWidth: %ld, availableWidth: %ld, fitEnd: %zu",
                    fullSize->cx,
                    availableWidth,
                    fitEnd
                );

                const auto lastBreakRange = std::ranges::find_last_if(
                    text.substr(position, fitEnd - position), &IsDrawTextBreakCharacter
                );
                const auto lastBreak =
                    lastBreakRange.empty()
                        ? fitEnd
                        : static_cast<size_t>(lastBreakRange.begin() - text.cbegin());

                Wh_Log(L"lastBreak: %zu", lastBreak);

                if (lastBreak >= position) {
                    // A left-aligned line may retain the whitespace that fitted
                    // after the previous word. Center/right alignment breaks before
                    // it.
                    if (!(format & (DT_RIGHT | DT_CENTER))) {
                        line.end = line.next = lastBreak + 1;
                    } else {
                        line.end = lastBreak;
                        line.next = lastBreak + 1;
                    }
                    while (line.next < paragraphEnd && IsDrawTextBreakCharacter(text[line.next])) {
                        ++line.next;
                    }
                } else if (
                    (format & DT_EDITCONTROL) && !(format & (DT_WORD_ELLIPSIS | DT_PATH_ELLIPSIS))
                ) {
                    line.end = fitEnd;
                    if (line.end <= position) {
                        line.end = NextCodePointBoundary(text, position, paragraphEnd);
                    }
                    line.next = line.end;
                } else {
                    // text.c keeps an unbreakable word intact unless DT_EDITCONTROL
                    // permits breaking within words. It can therefore extend past
                    // width.
                    auto wordEnd = fitEnd;
                    while (wordEnd < paragraphEnd && !IsDrawTextBreakCharacter(text[wordEnd])) {
                        // NOLINTNEXTLINE(readability-suspicious-call-argument)
                        wordEnd = NextCodePointBoundary(text, wordEnd, paragraphEnd);
                    }
                    if (wordEnd <= position) {
                        wordEnd = NextCodePointBoundary(text, position, paragraphEnd);
                    }
                    line.end = line.next = wordEnd;
                    while (line.next < paragraphEnd && IsDrawTextBreakCharacter(text[line.next])) {
                        ++line.next;
                    }
                }
                Wh_Log(L"line.next: %zu", line.next);
            }
        }

        // Consume CR/LF as a single line separator, including CRLF and LFCR.
        if (line.next == paragraphEnd && paragraphEnd < text.size()) {
            const auto first = text[paragraphEnd];
            ++line.next;
            if (line.next < text.size() && IsLineBreakCharacter(text[line.next])
                && text[line.next] != first) {
                ++line.next;
            }
        }
        Wh_Log(L"line.next: %zu", line.next);

        // Empty paragraphs still represent a logical line.
        line.end = std::max(line.end, line.start);

        line.renderedText.assign(text, line.start, line.end - line.start);
        const auto lineSize =
            MeasureDrawTextRange(line.renderedText, 0, line.renderedText.size(), tabWidth);
        if (!lineSize) {
            return std::nullopt;
        }
        line.size = *lineSize;

        const auto tooWide = line.size.cx > availableWidth;
        const auto requestPathEllipsis = (format & DT_PATH_ELLIPSIS) && tooWide;
        const auto requestWordEllipsis = (format & DT_WORD_ELLIPSIS) && tooWide;
        const auto remainderIsOnlyLineBreak = line.end == paragraphEnd && line.next >= text.size();
        const auto requestEndEllipsis =
            (format & DT_END_ELLIPSIS)
            && ((lastLine && line.next < text.size()) || (remainderIsOnlyLineBreak && tooWide));

        if (requestPathEllipsis || requestWordEllipsis || requestEndEllipsis) {
            if (requestPathEllipsis) {
                line.renderedText = MakePathEllipsis(line.renderedText, availableWidth, tabWidth);
            }

            if (requestWordEllipsis || requestEndEllipsis) {
                // text.c can apply end/word ellipsis after path ellipsis as well.
                line.renderedText = MakeEndEllipsis(
                    line.renderedText, availableWidth, tabWidth, requestWordEllipsis
                );
            }

            line.ellipsified = true;
            const auto lineSize =
                MeasureDrawTextRange(line.renderedText, 0, line.renderedText.size(), tabWidth);
            if (!lineSize) {
                return std::nullopt;
            }
            line.size = *lineSize;
        }

        return line;
    }

    int execute() {
        TEXTMETRICW tm{};
        if (!GetTextMetricsW(hdc, &tm)) {
            return 0;
        }

        const auto originalRect = *rect;
        POINT origin{rect->left, rect->top};
        auto width = rect->right - rect->left;
        const auto lineHeight =
            (format & DT_EXTERNALLEADING) ? tm.tmHeight + tm.tmExternalLeading : tm.tmHeight;
        const auto safeLineHeight = std::max(lineHeight, 1L);

        auto leftMargin = 0;
        auto rightMargin = 0;
        if (params) {
            leftMargin = params->iLeftMargin;
            rightMargin = params->iRightMargin;
            width -= leftMargin + rightMargin;
            if (!(format & (DT_CENTER | DT_RIGHT))) {
                origin.x += leftMargin;
            }
            params->uiLengthDrawn = 0;
        }

        auto tabWidth = 0;
        if (format & DT_EXPANDTABS) {
            const auto tabStop =
                ((format & DT_TABSTOP) && params && params->iTabLength) ? params->iTabLength : 8;
            tabWidth = std::max(1, static_cast<int>(tm.tmAveCharWidth * tabStop));
        }

        if (format & DT_CALCRECT) {
            format |= DT_NOCLIP;
        }

        auto invertY = false;
        if (GetGraphicsMode(hdc) == GM_COMPATIBLE) {
            SIZE windowExt{};
            SIZE viewportExt{};
            if (GetWindowExtEx(hdc, &windowExt) && GetViewportExtEx(hdc, &viewportExt)
                && ((windowExt.cy > 0) != (viewportExt.cy > 0))) {
                invertY = true;
            }
        }

        auto maxWidth = 0L;
        const wchar_t* processedSource = nullptr;
        auto position = 0UZ;
        std::vector<Modification> modifications;

        for (;;) {
            Wh_Log(L"position: %zu", position);
            const auto offsetY =
                ((format & DT_EDITCONTROL) ? 2 * safeLineHeight - 1 : safeLineHeight);
            const auto lastLine = !(format & DT_NOCLIP)
                                  && (invertY ? origin.y - offsetY < rect->bottom
                                              : origin.y + offsetY > rect->bottom);

            auto line = BuildNextDrawTextLine(position, width, tabWidth, lastLine);
            if (!line) {
                // The fallback runs were already identified, so do not redraw the
                // text through the native function after a partial render.
                return 0;
            }

            Wh_Log(L"renderedText: %s", line->renderedText.c_str());

            for (auto prefixOffset : parsed.underlineOffsets) {
                if (prefixOffset >= line->start && prefixOffset < line->end) {
                    line->underlineOffsets.push_back(prefixOffset - line->start);
                }
            }

            if (line->ellipsified) {
                line->underlineOffsets.clear();
            }

            if (params) {
                const auto* const sourceEnd =
                    line->next > 0 ? parsed.sourceEnds[line->next - 1] : processedSource;
                params->uiLengthDrawn +=
                    static_cast<UINT>(std::max(0Z, sourceEnd - processedSource));
                processedSource = std::max(processedSource, sourceEnd);
            }

            auto lineX = origin.x;
            if (format & DT_CENTER) {
                lineX = (rect->left + leftMargin + rect->right - rightMargin - line->size.cx) / 2;
            } else if (format & DT_RIGHT) {
                lineX = rect->right - line->size.cx - rightMargin;
            }

            auto lineY = origin.y;
            if (format & DT_SINGLELINE) {
                if (format & DT_VCENTER) {
                    lineY = rect->top + (rect->bottom - rect->top) / 2 - line->size.cy / 2;
                } else if (format & DT_BOTTOM) {
                    lineY = rect->bottom - line->size.cy;
                }
            }

            if (!(format & DT_CALCRECT)) {
                RECT clipRect = *rect;
                const auto* clip = (format & DT_NOCLIP) ? nullptr : &clipRect;

                if (!DrawTextLineWithFallback(line->renderedText, lineX, lineY, clip, tabWidth)) {
                    return 0;
                }

                if (!(format & DT_HIDEPREFIX)) {
                    for (auto underlineOffset : line->underlineOffsets) {
                        DrawDrawTextUnderline(
                            line->renderedText,
                            underlineOffset,
                            lineX,
                            lineY + tm.tmAscent + 1,
                            clip,
                            tabWidth
                        );
                    }
                }
            } else {
                maxWidth = std::max(maxWidth, line->size.cx);
            }

            if (allowModifyString && (format & DT_MODIFYSTRING) && line->ellipsified
                && line->end > line->start) {
                const auto sourceStart =
                    line->start > 0 ? parsed.sourceEnds[line->start - 1] : text.cbegin();
                const auto sourceEnd = parsed.sourceEnds[line->end - 1];
                if (sourceEnd >= sourceStart) {
                    modifications.emplace_back(
                        sourceStart - text.cbegin(),
                        sourceEnd - sourceStart,
                        std::move(line->renderedText)
                    );
                }
            }

            if (invertY) {
                origin.y -= safeLineHeight;
            } else {
                origin.y += safeLineHeight;
            }

            position = line->next;
            if (position >= parsed.text.size() || lastLine) {
                break;
            }
        }

        if (format & DT_CALCRECT) {
            rect->right = rect->left + maxWidth;
            rect->bottom = origin.y;
            if (params) {
                rect->right += leftMargin + rightMargin;
            }
        }

        if (allowModifyString && (format & DT_MODIFYSTRING) && !modifications.empty()) {
            std::wstring modified(text);
            std::ranges::sort(modifications, std::ranges::greater{}, &Modification::start);
            for (const auto& [start, length, replacement] : modifications) {
                if (start <= modified.size() && start + length <= modified.size()) {
                    modified.replace(start, length, replacement);
                }
            }

            // DrawTextExW requires room for four extra WCHARs when DT_MODIFYSTRING
            // is specified. Preserve that contract while always terminating the
            // result.
            const auto copyCount = std::min(modified.size() + 1, text.size() + 4);
            if (copyCount) {
                memcpy(
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                    const_cast<wchar_t*>(text.data()),
                    modified.c_str(),
                    copyCount * sizeof(wchar_t)
                );
            }
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast,cppcoreguidelines-pro-bounds-pointer-arithmetic)
            (const_cast<wchar_t*>(text.end()))[3] = L'\0';
        }

        const auto result = origin.y - originalRect.top;
        return result == 0 ? 1 : result;
    }

    static std::optional<Context> Make(
        HDC hdc,
        LPWSTR text,
        int count,
        LPRECT rect,
        UINT format,
        LPDRAWTEXTPARAMS params,
        bool allowModifyString
    ) {
        if (!text || !rect || count < -1 || !g_originalDrawTextW) {
            return std::nullopt;
        }

        Context ctx{
            .hdc = hdc,
            .text = std::wstring_view(text, count < 0 ? wcslen(text) : count),
            .rect = rect,
            .format = format,
            .params = params,
            .allowModifyString = allowModifyString,
            .originalFont = static_cast<HFONT>(GetCurrentObject(hdc, OBJ_FONT))
        };

        Wh_Log(
            L"text: \"%.*s\", count: %d, size: %zu", ctx.text.size(), text, count, ctx.text.size()
        );

        if (ctx.text.size() > INT_MAX / 4) {
            return std::nullopt;
        }

        if (params && params->cbSize != sizeof(DRAWTEXTPARAMS)) {
            return std::nullopt;
        }

        if (!ctx.originalFont) {
            return std::nullopt;
        }

        if (ctx.format & DT_SINGLELINE) {
            ctx.format &= ~DT_WORDBREAK;
        }

        ctx.parsed = ParsedDrawText(ctx.text, format);
        if (ctx.parsed.text.empty() || ctx.parsed.text.size() > static_cast<size_t>(INT_MAX)) {
            return std::nullopt;
        }

        auto runs = BuildRuns(hdc, ctx.originalFont, ctx.parsed.text);
        if (!runs || !runs->second) {
            return std::nullopt;
        }

        ctx.runs = std::move(runs->first);

        return ctx;
    }
};

static int WINAPI DrawTextW_Hook(
    HDC hdc,
    LPCWSTR text,
    int count,
    LPRECT rect,
    UINT format
) {
    if (g_inHook || !text || !rect) {
        return g_originalDrawTextW(hdc, text, count, rect, format);
    }

    const HookGuard _;

    const auto result = Context::Make(
                            hdc,
                            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
                            const_cast<LPWSTR>(text),
                            count,
                            rect,
                            format,
                            nullptr,
                            false
    )
                            .transform(&Context::execute);

    return result ? *result : g_originalDrawTextW(hdc, text, count, rect, format);
}

static int WINAPI DrawTextExW_Hook(
    HDC hdc,
    LPWSTR text,
    int count,
    LPRECT rect,
    UINT format,
    LPDRAWTEXTPARAMS params
) {
    if (g_inHook || !text || !rect) {
        return g_originalDrawTextExW(hdc, text, count, rect, format, params);
    }

    const HookGuard _;

    const auto result =
        Context::Make(hdc, text, count, rect, format, params, true).transform(&Context::execute);
    return result ? *result : g_originalDrawTextExW(hdc, text, count, rect, format, params);
}
