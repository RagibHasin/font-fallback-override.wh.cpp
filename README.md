# Font Fallback Override

Hooks DirectWrite's font fallback system to override font selection for specific
Unicode character ranges across all applications.

## Features

- Per-Unicode-range fallback rules.
- Works with...
  - classic GDI APIs: `DrawTextW`, `DrawTextExW`, `DrawThemeText`,
      `DrawThemeTextEx`, `DrawShadowText`, `TextOutW` and `ExtTextOutW`.
  - all DirectWrite APIs.
- Preserves the original font's size, weight, italic and charset.
- Detects whether the currently selected font contains a glyph and only
    replaces the font if it does not.

## Example

Configure:

U+0980-U+09FF -> Noto Sans Bengali
U+4E00-U+9FFF -> Microsoft YaHei

A string such as:

    "Hello বাংলা 世界"

can therefore be rendered as:

    original font -> Bengali fallback -> original font -> CJK fallback

| Before            | After           |
| ----------------- | --------------- |
| ![Before][before] | ![After][after] |

## Notice

This mod only works on Windows 8.1 or later.

[before]: ./assets/before.png
[after]: ./assets/after.png
