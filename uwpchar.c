#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <strsafe.h>
#include <stdint.h>
#include <string.h>
#include <richedit.h>
#include "uwpchar_names.h"

typedef struct GLYPH_LIST {
    uint32_t *codes;
    size_t count;
    size_t cap;
} GLYPH_LIST;


static const WCHAR *kFontA = L"Segoe MDL2 Assets";
static const WCHAR *kFontB = L"Segoe Fluent Icons";

static HWND g_hwnd = NULL;
static HWND g_view = NULL;
static HWND g_comboFont = NULL;
static HWND g_comboSize = NULL;
static HWND g_editDefines = NULL;
static HWND g_btnCopy = NULL;
static HWND g_status = NULL;
static HWND g_labelFont = NULL;
static HWND g_labelSize = NULL;
static HWND g_labelLang = NULL;
static HWND g_comboLang = NULL;

static WCHAR g_fontFace[LF_FACESIZE] = L"Segoe MDL2 Assets";
static WCHAR g_customFont[LF_FACESIZE] = L"";
static int g_customIndex = -1;
static int g_fontSize = 24;
static int g_lastFontSel = 0;

static HFONT g_glyphFont = NULL;
static HFONT g_uiFont = NULL;
static HFONT g_monoFont = NULL;

static GLYPH_LIST g_glyphs = {0};

static int g_cellW = 64;
static int g_cellH = 72;
static int g_labelH = 16;
static int g_scrollY = 0;
static int g_comboDropH = 220;
static BOOL g_inSizeMove = FALSE;

static void InsertDefineLine(uint32_t code);
static void FormatLine(WCHAR *out, size_t cap, const WCHAR *name, uint32_t code, const WCHAR *font);

static void ListClear(GLYPH_LIST *list) {
    if (list->codes) {
        HeapFree(GetProcessHeap(), 0, list->codes);
    }
    list->codes = NULL;
    list->count = 0;
    list->cap = 0;
}

static BOOL ListPush(GLYPH_LIST *list, uint32_t code) {
    if (list->count == list->cap) {
        size_t newCap = list->cap ? list->cap * 2 : 4096;
        uint32_t *p = NULL;
        if (list->codes) {
            p = (uint32_t *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, list->codes, newCap * sizeof(uint32_t));
        } else {
            p = (uint32_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, newCap * sizeof(uint32_t));
        }
        if (!p) return FALSE;
        list->codes = p;
        list->cap = newCap;
    }
    list->codes[list->count++] = code;
    return TRUE;
}

static BOOL BuildAsciiName(uint32_t code, char *out, size_t cap) {
    if (cap == 0) return FALSE;
    out[0] = '\0';

    const char **tokens = NULL;
    const uint32_t *offsets = NULL;
    const uint8_t *counts = NULL;
    const void *tokenIndex = NULL;
    BOOL isMdl2 = FALSE;
    uint32_t base = 0;
    uint32_t count = 0;
    if (lstrcmpW(g_fontFace, kFontA) == 0) {
        base = kUwpcharMdl2Base;
        count = kUwpcharMdl2Count;
        tokens = kUwpcharMdl2Tokens;
        offsets = kUwpcharMdl2NameTokenOffset;
        counts = kUwpcharMdl2NameTokenCount;
        tokenIndex = kUwpcharMdl2TokenIndex;
        isMdl2 = TRUE;
    } else if (lstrcmpW(g_fontFace, kFontB) == 0) {
        base = kUwpcharFluentBase;
        count = kUwpcharFluentCount;
        tokens = kUwpcharFluentTokens;
        offsets = kUwpcharFluentNameTokenOffset;
        counts = kUwpcharFluentNameTokenCount;
        tokenIndex = kUwpcharFluentTokenIndex;
    } else {
        return FALSE;
    }

    if (count == 0) return FALSE;
    if (code < base) return FALSE;
    uint32_t idx = code - base;
    if (idx >= count) return FALSE;

    uint32_t off = offsets[idx];
    uint32_t cnt = counts[idx];
    if (cnt == 0) return FALSE;

    size_t pos = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        uint32_t tindex;
        if (isMdl2) {
            tindex = ((const kUwpcharMdl2TokenIndexT *)tokenIndex)[off + i];
        } else {
            tindex = ((const kUwpcharFluentTokenIndexT *)tokenIndex)[off + i];
        }
        const char *tok = tokens[tindex];
        if (!tok) continue;
        size_t tlen = strlen(tok);
        if (pos + tlen + 1 >= cap) break;
        memcpy(out + pos, tok, tlen);
        pos += tlen;
        out[pos] = '\0';
    }
    return pos > 0;
}

static void MakeFallbackName(uint32_t code, WCHAR *out, size_t cap) {
    if (code >= 0xE000 && code <= 0xF8FF) {
        StringCchPrintfW(out, cap, L"ICON_%04X", code);
    } else {
        StringCchPrintfW(out, cap, L"U_%04X", code);
    }
}

static void MakeMacroName(const char *asciiName, uint32_t code, WCHAR *out, size_t cap) {
    if (asciiName && asciiName[0] && asciiName[0] != '<') {
        WCHAR tmp[256];
        size_t n = 0;
        for (const char *p = asciiName; *p && n + 1 < _countof(tmp); ++p) {
            unsigned char ch = (unsigned char)*p;
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                (ch >= '0' && ch <= '9') || ch == '_') {
                tmp[n++] = (WCHAR)ch;
            } else if (ch == ' ' || ch == '-') {
                tmp[n++] = L'_';
            }
        }
        tmp[n] = L'\0';
        if (n > 0 && tmp[0] >= L'0' && tmp[0] <= L'9') {
            WCHAR prefixed[260];
            StringCchPrintfW(prefixed, _countof(prefixed), L"U_%s", tmp);
            StringCchCopyW(out, cap, prefixed);
            return;
        }
        if (n > 0) {
            StringCchCopyW(out, cap, tmp);
            return;
        }
    }
    MakeFallbackName(code, out, cap);
}

static HFONT CreateGlyphFont(int pt, LPCWSTR face) {
    HDC hdc = GetDC(NULL);
    int height = -MulDiv(pt, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);
    return CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

static void UpdateGlyphFont(void) {
    if (g_glyphFont) DeleteObject(g_glyphFont);
    g_glyphFont = CreateGlyphFont(g_fontSize, g_fontFace);
}

static void UpdateCellMetrics(void) {
    g_labelH = 16;
    g_cellW = g_fontSize + 24;
    g_cellH = g_fontSize + 24 + g_labelH;
    if (g_cellW < 48) g_cellW = 48;
    if (g_cellH < 64) g_cellH = 64;
}

static void BuildGlyphList(void) {
    ListClear(&g_glyphs);
    UpdateGlyphFont();
    UpdateCellMetrics();

    HDC hdc = GetDC(NULL);
    HFONT old = (HFONT)SelectObject(hdc, g_glyphFont);

    const uint32_t start = 0x0020;
    const uint32_t end = 0xFFFD;
    WCHAR chars[512];
    WORD glyphs[512];

    uint32_t code = start;
    while (code <= end) {
        int count = 0;
        for (; count < (int)_countof(chars) && code <= end; ++code) {
            if (code >= 0xD800 && code <= 0xDFFF) continue;
            chars[count++] = (WCHAR)code;
        }
        if (count == 0) break;
        if (GetGlyphIndicesW(hdc, chars, count, glyphs, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR) {
            for (int i = 0; i < count; ++i) {
                if (glyphs[i] != 0xFFFF) {
                    ListPush(&g_glyphs, (uint32_t)chars[i]);
                }
            }
        }
    }

    SelectObject(hdc, old);
    ReleaseDC(NULL, hdc);
}

static void UpdateStatus(void) {
    WCHAR text[256];
    StringCchPrintfW(text, _countof(text), L"Glyphs: %zu | Names: %s", g_glyphs.count,
                     (lstrcmpW(g_fontFace, kFontA) == 0 || lstrcmpW(g_fontFace, kFontB) == 0) ? L"msdocs" : L"fallback");
    SetWindowTextW(g_status, text);
}

static void UpdateViewScroll(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int width = rc.right - rc.left;
    int cols = width / g_cellW;
    if (cols < 1) cols = 1;
    int rows = (int)((g_glyphs.count + (size_t)cols - 1) / (size_t)cols);
    int totalHeight = rows * g_cellH;

    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (totalHeight > 0 ? totalHeight - 1 : 0);
    si.nPage = (UINT)max(1, rc.bottom - rc.top);
    si.nPos = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

static void ClampScroll(HWND hwnd) {
    SCROLLINFO si = {0};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    GetScrollInfo(hwnd, SB_VERT, &si);

    int maxPos = si.nMax - (int)si.nPage + 1;
    if (maxPos < 0) maxPos = 0;
    if (g_scrollY < 0) g_scrollY = 0;
    if (g_scrollY > maxPos) g_scrollY = maxPos;
    si.nPos = g_scrollY;
    SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

static void ViewScrollBy(HWND hwnd, int delta) {
    g_scrollY += delta;
    ClampScroll(hwnd);
    InvalidateRect(hwnd, NULL, TRUE);
}

static void ViewOnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);

    HBRUSH bg = CreateSolidBrush(RGB(18, 18, 20));
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    SetBkMode(hdc, TRANSPARENT);

    int width = rc.right - rc.left;
    int cols = width / g_cellW;
    if (cols < 1) cols = 1;

    int startRow = max(0, g_scrollY / g_cellH);
    int endRow = (g_scrollY + (rc.bottom - rc.top)) / g_cellH + 1;

    HFONT old = (HFONT)SelectObject(hdc, g_glyphFont);

    for (int row = startRow; row <= endRow; ++row) {
        for (int col = 0; col < cols; ++col) {
            size_t idx = (size_t)row * (size_t)cols + (size_t)col;
            if (idx >= g_glyphs.count) break;

            int x = col * g_cellW + 12;
            int y = row * g_cellH + 8 - g_scrollY;

            WCHAR ch[2] = { (WCHAR)g_glyphs.codes[idx], 0 };
            SetTextColor(hdc, RGB(230, 230, 230));
            TextOutW(hdc, x, y, ch, 1);

            SelectObject(hdc, g_uiFont);
            WCHAR label[32];
            StringCchPrintfW(label, _countof(label), L"U+%04X", (unsigned)g_glyphs.codes[idx]);
            RECT tr = { x - 8, y + g_fontSize + 6, x + g_cellW - 16, y + g_fontSize + 6 + g_labelH };
            SetTextColor(hdc, RGB(150, 150, 160));
            DrawTextW(hdc, label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, g_glyphFont);
        }
    }

    SelectObject(hdc, old);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK ViewProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SIZE:
            if (!g_inSizeMove) {
                UpdateViewScroll(hwnd);
            }
            return 0;
        case WM_VSCROLL:
            switch (LOWORD(wParam)) {
                case SB_LINEUP: ViewScrollBy(hwnd, -g_cellH / 2); break;
                case SB_LINEDOWN: ViewScrollBy(hwnd, g_cellH / 2); break;
                case SB_PAGEUP: ViewScrollBy(hwnd, -g_cellH * 2); break;
                case SB_PAGEDOWN: ViewScrollBy(hwnd, g_cellH * 2); break;
                case SB_THUMBTRACK: g_scrollY = HIWORD(wParam); ClampScroll(hwnd); InvalidateRect(hwnd, NULL, TRUE); break;
            }
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ViewScrollBy(hwnd, -(delta / WHEEL_DELTA) * (g_cellH / 2));
            return 0;
        }
        case WM_LBUTTONDOWN: {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int width = rc.right - rc.left;
            int cols = width / g_cellW;
            if (cols < 1) cols = 1;

            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam) + g_scrollY;
            int col = x / g_cellW;
            int row = y / g_cellH;
            if (col < 0 || row < 0) return 0;
            size_t idx = (size_t)row * (size_t)cols + (size_t)col;
            if (idx < g_glyphs.count) {
                InsertDefineLine(g_glyphs.codes[idx]);
            }
            return 0;
        }
        case WM_PAINT:
            ViewOnPaint(hwnd);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static void UpdateAll(void) {
    BuildGlyphList();
    UpdateStatus();
    UpdateViewScroll(g_view);
    InvalidateRect(g_view, NULL, TRUE);
}

static void ChooseCustomFont(void) {
    CHOOSEFONTW cf = {0};
    LOGFONTW lf = {0};
    StringCchCopyW(lf.lfFaceName, LF_FACESIZE, g_fontFace);
    HDC hdc = GetDC(NULL);
    lf.lfHeight = -MulDiv(g_fontSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(NULL, hdc);

    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = g_hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST;

    if (ChooseFontW(&cf)) {
        StringCchCopyW(g_customFont, LF_FACESIZE, lf.lfFaceName);
        StringCchCopyW(g_fontFace, LF_FACESIZE, g_customFont);

        int count = (int)SendMessageW(g_comboFont, CB_GETCOUNT, 0, 0);
        int browseIndex = count - 1;
        if (g_customIndex != -1) {
            SendMessageW(g_comboFont, CB_DELETESTRING, g_customIndex, 0);
        }
        g_customIndex = (int)SendMessageW(g_comboFont, CB_INSERTSTRING, browseIndex, (LPARAM)g_customFont);
        SendMessageW(g_comboFont, CB_SETCURSEL, g_customIndex, 0);
        UpdateAll();
    } else {
        SendMessageW(g_comboFont, CB_SETCURSEL, g_lastFontSel, 0);
    }
}

static void OnFontComboChange(void) {
    int sel = (int)SendMessageW(g_comboFont, CB_GETCURSEL, 0, 0);
    int count = (int)SendMessageW(g_comboFont, CB_GETCOUNT, 0, 0);
    int browseIndex = count - 1;
    if (sel == browseIndex) {
        ChooseCustomFont();
        return;
    }

    WCHAR buf[LF_FACESIZE];
    SendMessageW(g_comboFont, CB_GETLBTEXT, sel, (LPARAM)buf);
    StringCchCopyW(g_fontFace, LF_FACESIZE, buf);
    g_lastFontSel = sel;
    UpdateAll();
}

static void OnSizeComboChange(void) {
    int sel = (int)SendMessageW(g_comboSize, CB_GETCURSEL, 0, 0);
    WCHAR buf[32];
    SendMessageW(g_comboSize, CB_GETLBTEXT, sel, (LPARAM)buf);
    int size = _wtoi(buf);
    if (size > 6 && size < 200) {
        g_fontSize = size;
        UpdateAll();
    }
}

static void SetDefaultUIFont(void) {
    NONCLIENTMETRICSW ncm = {0};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        g_uiFont = CreateFontIndirectW(&ncm.lfMessageFont);
    }
}

static void ApplyUIFont(void) {
    if (!g_uiFont) return;
    SendMessageW(g_comboFont, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_comboSize, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_editDefines, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_btnCopy, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_labelFont, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_labelSize, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_labelLang, WM_SETFONT, (WPARAM)g_uiFont, TRUE);
    SendMessageW(g_comboLang, WM_SETFONT, (WPARAM)g_uiFont, TRUE);

    if (!g_monoFont) {
        g_monoFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, FIXED_PITCH | FF_DONTCARE, L"Consolas");
    }
    if (g_monoFont) {
        SendMessageW(g_editDefines, WM_SETFONT, (WPARAM)g_monoFont, TRUE);
    }
}

static void InsertDefineLine(uint32_t code) {
    char asciiName[256];
    const char *uname = NULL;
    if (BuildAsciiName(code, asciiName, sizeof(asciiName))) {
        uname = asciiName;
    }
    WCHAR nameBuf[256];
    MakeMacroName(uname, code, nameBuf, _countof(nameBuf));

    WCHAR line[512];
    FormatLine(line, _countof(line), nameBuf, code, g_fontFace);

    DWORD selStart = 0, selEnd = 0;
    SendMessageW(g_editDefines, EM_GETSEL, (WPARAM)&selStart, (LPARAM)&selEnd);
    int lineIndex = (int)SendMessageW(g_editDefines, EM_LINEFROMCHAR, selStart, 0);
    int lineStart = (int)SendMessageW(g_editDefines, EM_LINEINDEX, lineIndex, 0);
    int lineLen = (int)SendMessageW(g_editDefines, EM_LINELENGTH, lineStart, 0);
    int insertPos = lineStart + lineLen;

    WCHAR insertBuf[600];
    StringCchPrintfW(insertBuf, _countof(insertBuf), L"\r\n%s", line);
    SendMessageW(g_editDefines, EM_SETSEL, insertPos, insertPos);
    SendMessageW(g_editDefines, EM_REPLACESEL, TRUE, (LPARAM)insertBuf);
}

static void FormatLine(WCHAR *out, size_t cap, const WCHAR *name, uint32_t code, const WCHAR *font) {
    WCHAR fmt[16] = L"C";
    int sel = (int)SendMessageW(g_comboLang, CB_GETCURSEL, 0, 0);
    if (sel != CB_ERR) {
        SendMessageW(g_comboLang, CB_GETLBTEXT, sel, (LPARAM)fmt);
    }

    if (lstrcmpiW(fmt, L"C++") == 0) {
        StringCchPrintfW(out, cap, L"constexpr uint32_t %s = 0x%04X; // %s", name, code, font);
    } else if (lstrcmpiW(fmt, L"ASM") == 0) {
        StringCchPrintfW(out, cap, L"%s EQU 0%04Xh ; %s", name, code, font);
    } else if (lstrcmpiW(fmt, L"C#") == 0) {
        StringCchPrintfW(out, cap, L"public const int %s = 0x%04X; // %s", name, code, font);
    } else if (lstrcmpiW(fmt, L"JSON") == 0) {
        StringCchPrintfW(out, cap, L"\"%s\": \"0x%04X\"", name, code);
    } else if (lstrcmpiW(fmt, L"Text") == 0) {
        StringCchPrintfW(out, cap, L"%s 0x%04X // %s", name, code, font);
    } else {
        StringCchPrintfW(out, cap, L"#define %s 0x%04X // %s", name, code, font);
    }
}

static void OnCopyDefines(void) {
    int len = GetWindowTextLengthW(g_editDefines);
    if (len <= 0) return;

    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (len + 1) * sizeof(WCHAR));
    if (!h) return;
    WCHAR *buf = (WCHAR *)GlobalLock(h);
    if (!buf) {
        GlobalFree(h);
        return;
    }
    GetWindowTextW(g_editDefines, buf, len + 1);
    GlobalUnlock(h);

    if (OpenClipboard(g_hwnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, h);
        CloseClipboard();
    } else {
        GlobalFree(h);
    }
}

static void Layout(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    int pad = 8;
    int top = 8;
    int rowH = 28;
    int sizeW = 80;
    int statusW = 200;
    int leftPanelW = 320;

    int leftLeft = pad;
    int leftTop = top;
    int leftInnerW = leftPanelW - pad * 2;

    HDWP dwp = BeginDeferWindowPos(10);
    if (!dwp) return;

    dwp = DeferWindowPos(dwp, g_labelFont, NULL, leftLeft, leftTop + 6, 48, rowH, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, g_comboFont, NULL, leftLeft + 48, leftTop, leftInnerW - 48, rowH + g_comboDropH, SWP_NOZORDER);
    leftTop += rowH + pad;

    dwp = DeferWindowPos(dwp, g_labelSize, NULL, leftLeft, leftTop + 6, 48, rowH, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, g_comboSize, NULL, leftLeft + 48, leftTop, sizeW, rowH + g_comboDropH, SWP_NOZORDER);
    leftTop += rowH + pad;

    dwp = DeferWindowPos(dwp, g_labelLang, NULL, leftLeft, leftTop + 6, 60, rowH, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, g_comboLang, NULL, leftLeft + 60, leftTop, leftInnerW - 60, rowH + g_comboDropH, SWP_NOZORDER);
    leftTop += rowH + pad;

    dwp = DeferWindowPos(dwp, g_btnCopy, NULL, leftLeft, leftTop, 80, rowH, SWP_NOZORDER);
    dwp = DeferWindowPos(dwp, g_status, NULL, leftLeft + 90, leftTop + 4, statusW, rowH, SWP_NOZORDER);
    leftTop += rowH + pad;

    int viewTop = top;
    int viewH = h - viewTop - pad;
    int viewW = w - leftPanelW - pad * 2;
    if (viewW < 100) viewW = 100;
    dwp = DeferWindowPos(dwp, g_view, NULL, leftPanelW + pad, viewTop, viewW, viewH, SWP_NOZORDER);

    int editTop = leftTop;
    int editH = h - editTop - pad;
    if (editH < 120) editH = 120;
    dwp = DeferWindowPos(dwp, g_editDefines, NULL, leftLeft, editTop, leftInnerW, editH, SWP_NOZORDER);
    EndDeferWindowPos(dwp);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_ENTERSIZEMOVE:
            g_inSizeMove = TRUE;
            SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
            return 0;
        case WM_EXITSIZEMOVE:
            g_inSizeMove = FALSE;
            SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
            Layout(hwnd);
            UpdateViewScroll(g_view);
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE);
            return 0;
        case WM_CREATE: {
            SetDefaultUIFont();

            LoadLibraryW(L"Msftedit.dll");

            g_comboFont = CreateWindowExW(0, L"COMBOBOX", NULL,
                                          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                          0, 0, 0, 0, hwnd, (HMENU)1001, NULL, NULL);
            g_comboSize = CreateWindowExW(0, L"COMBOBOX", NULL,
                                          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                          0, 0, 0, 0, hwnd, (HMENU)1002, NULL, NULL);
            g_labelFont = CreateWindowExW(0, L"STATIC", L"Font:",
                                          WS_CHILD | WS_VISIBLE,
                                          0, 0, 0, 0, hwnd, (HMENU)1007, NULL, NULL);
            g_labelSize = CreateWindowExW(0, L"STATIC", L"Size:",
                                          WS_CHILD | WS_VISIBLE,
                                          0, 0, 0, 0, hwnd, (HMENU)1008, NULL, NULL);
            g_labelLang = CreateWindowExW(0, L"STATIC", L"Format:",
                                          WS_CHILD | WS_VISIBLE,
                                          0, 0, 0, 0, hwnd, (HMENU)1009, NULL, NULL);
            g_comboLang = CreateWindowExW(0, L"COMBOBOX", NULL,
                                          WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                          0, 0, 0, 0, hwnd, (HMENU)1010, NULL, NULL);
            g_btnCopy = CreateWindowExW(0, L"BUTTON", L"Copy",
                                        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                        0, 0, 0, 0, hwnd, (HMENU)1003, NULL, NULL);
            g_status = CreateWindowExW(0, L"STATIC", L"",
                                       WS_CHILD | WS_VISIBLE,
                                       0, 0, 0, 0, hwnd, (HMENU)1004, NULL, NULL);

            g_view = CreateWindowExW(WS_EX_CLIENTEDGE, L"UwpGlyphView", NULL,
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                     0, 0, 0, 0, hwnd, (HMENU)1005, NULL, NULL);

            g_editDefines = CreateWindowExW(WS_EX_CLIENTEDGE, L"RICHEDIT50W", NULL,
                                            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | ES_NOHIDESEL,
                                            0, 0, 0, 0, hwnd, (HMENU)1006, NULL, NULL);
            SendMessageW(g_editDefines, EM_SETBKGNDCOLOR, 0, RGB(250, 250, 250));
            SendMessageW(g_editDefines, EM_SETLIMITTEXT, 0, 0);
            SendMessageW(g_editDefines, EM_SETREADONLY, TRUE, 0);

            SendMessageW(g_comboFont, CB_ADDSTRING, 0, (LPARAM)kFontA);
            SendMessageW(g_comboFont, CB_ADDSTRING, 0, (LPARAM)kFontB);
            SendMessageW(g_comboFont, CB_ADDSTRING, 0, (LPARAM)L"...");
            SendMessageW(g_comboFont, CB_SETCURSEL, 0, 0);
            g_lastFontSel = 0;

            const int sizes[] = {16, 20, 24, 32, 40, 48, 64};
            for (int i = 0; i < (int)_countof(sizes); ++i) {
                WCHAR tmp[16];
                StringCchPrintfW(tmp, _countof(tmp), L"%d", sizes[i]);
                SendMessageW(g_comboSize, CB_ADDSTRING, 0, (LPARAM)tmp);
                if (sizes[i] == 24) {
                    SendMessageW(g_comboSize, CB_SETCURSEL, i, 0);
                }
            }

            SendMessageW(g_comboLang, CB_ADDSTRING, 0, (LPARAM)L"C");
            SendMessageW(g_comboLang, CB_ADDSTRING, 0, (LPARAM)L"C++");
            SendMessageW(g_comboLang, CB_ADDSTRING, 0, (LPARAM)L"ASM");
            SendMessageW(g_comboLang, CB_ADDSTRING, 0, (LPARAM)L"C#");
            SendMessageW(g_comboLang, CB_ADDSTRING, 0, (LPARAM)L"JSON");
            SendMessageW(g_comboLang, CB_ADDSTRING, 0, (LPARAM)L"Text");
            SendMessageW(g_comboLang, CB_SETCURSEL, 0, 0);

            ApplyUIFont();
            UpdateAll();
            Layout(hwnd);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case 1001:
                    if (HIWORD(wParam) == CBN_SELCHANGE) OnFontComboChange();
                    return 0;
                case 1002:
                    if (HIWORD(wParam) == CBN_SELCHANGE) OnSizeComboChange();
                    return 0;
                case 1003:
                    OnCopyDefines();
                    return 0;
            }
            return 0;
        case WM_SIZE:
            Layout(hwnd);
            if (!g_inSizeMove) {
                UpdateViewScroll(g_view);
                InvalidateRect(g_view, NULL, TRUE);
                InvalidateRect(g_editDefines, NULL, TRUE);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        case WM_DESTROY:
            ListClear(&g_glyphs);
            if (g_glyphFont) DeleteObject(g_glyphFont);
            if (g_uiFont) DeleteObject(g_uiFont);
            if (g_monoFont) DeleteObject(g_monoFont);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmdLine, int nCmdShow) {
    (void)hPrev;
    (void)cmdLine;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"UwpCharMain";
    if (!RegisterClassW(&wc)) return 1;

    WNDCLASSW vc = {0};
    vc.lpfnWndProc = ViewProc;
    vc.hInstance = hInst;
    vc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    vc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    vc.lpszClassName = L"UwpGlyphView";
    if (!RegisterClassW(&vc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"uwpchar - icon font browser",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g_hwnd = hwnd;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
