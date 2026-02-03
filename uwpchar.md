# uwpchar — icon font browser for Windows

`uwpchar` is a single-file Windows GUI for browsing **Segoe MDL2 Assets** and **Segoe Fluent Icons** at different sizes and exporting codepoint defines for UWP-style icon usage.

Design goals:
- **Fast visual browsing** of icon fonts
- **Size-aware preview** for UI tuning
- **Minimal footprint**: one C file, MSVC/LLVM build, no third-party deps
- **Win32-first**: use Win32 APIs over the C stdlib when practical

---

## Features

- Font dropdown with:
  - `Segoe MDL2 Assets`
  - `Segoe Fluent Icons`
  - `...` to browse and select any installed font
- Size dropdown with common UI sizes (default: 24)
- Grid view showing **only glyphs that exist** in the selected font
- Export panel that emits:
  - `#define <name> 0xXXXX // <font>`
  - Lines are added by clicking glyphs in the grid (inserts below the current cursor line).
- Copy button to send the export list to the clipboard

---

## Build

### Requirements
- Windows
- MSVC or LLVM toolchain in `PATH` (Developer Command Prompt or equivalent)

### Compile (x64)

```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE uwpchar.c user32.lib gdi32.lib comdlg32.lib
clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE uwpchar.c user32.lib gdi32.lib comdlg32.lib
```

---

## Usage

Launch the GUI:

```text
uwpchar
```

---

## Workflow

1. Pick a font (`Segoe MDL2 Assets` or `Segoe Fluent Icons`, or `...` to browse).
2. Pick a size for preview.
3. Pick a format template (C/C++/ASM/C#/JSON/Text).
4. Click glyphs to append lines into the editor.
5. Copy the results to the clipboard.

---

## Examples

### Browse MDL2 icons at 24px

1. Launch `uwpchar`
2. Ensure the font dropdown is `Segoe MDL2 Assets`
3. Select size `24`
4. Click glyphs to add `#define` lines, then copy

### Switch to Fluent icons or any font

1. Choose `Segoe Fluent Icons` in the font dropdown
2. Or select `...` and pick a custom font

---

## Notes

- Glyph enumeration scans a broad BMP range (U+0020 to U+FFFD) and filters to glyphs that exist in the font.
- Icon names are compiled in from Microsoft Docs tables via `uwpchar_names.h`.
- If no name is available, a fallback name is generated (`ICON_XXXX` for private-use codepoints, otherwise `U_XXXX`).

---

## Exit codes

- `0` success
- `1` initialization failure

---

## License

Unlicensed / public domain intent

- Initial implementation drafted with AI assistance
- ChatGPT 5.2 Thinking (OpenAI), < 5 min.

---

## Updating icon names

Rebuild the compiled name mapping from the Microsoft Docs markdown files:

```bat
python uwpchar_extract_names.py --md segoe-ui-symbol-font.md --md segoe-fluent-icons-font.md --out uwpchar_names.h
```

---

## Web alternative

A static HTML version is available at:

- `uwpchar.html`

Notes:

- Loads the Microsoft Docs markdown tables directly from GitHub at runtime.
- Requires a network connection and a browser that allows `fetch()` from `raw.githubusercontent.com`.
- Defaults to `Segoe Fluent Icons` and named-only filtering.
