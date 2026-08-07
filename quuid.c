// quuid.c — GUID/COM discovery CLI for Windows (gleamed)
//
// Build (MSVC x64):
//   cl /nologo /W4 /O2 /DUNICODE /D_UNICODE quuid.c ole32.lib oleaut32.lib advapi32.lib
//
// Commands:
//   quuid parse  <guid> [--one-line]
//   quuid find   <guid> [--wow32|--wow64] [--both-views]
//   quuid scan   <path> [--registry] [--both-views] [--binary] [--binary-loose] [--locate] [--one-line]
//   quuid server <clsid-guid> [--scan] [scan flags...]
//   quuid tlb    <file.tlb|.dll|.ocx>
//   quuid enum   clsid|iid|typelib|appid [--limit N] [--with-name]
//
// Global flags:
//   --verbose  (prints Win32 error details for non-fatal failures)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <stdio.h>
#include <wchar.h>
#include <limits.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A)/sizeof((A)[0]))
#endif

// ============================= options =============================

typedef struct OPTS {
    int verbose;
    DWORD reg_view;      // 0, KEY_WOW64_32KEY, KEY_WOW64_64KEY
    int both_views;      // query both (32 + 64) when meaningful
} OPTS;

static OPTS g_opt = { 0, 0, 0 };
static unsigned long g_runtime_errors = 0;

// ============================= errors =============================

static void print_error_code(const wchar_t* where, DWORD e) {
    wchar_t buf[512];
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, e, 0, buf, ARRAYSIZE(buf), NULL);
    if (!n) {
        fwprintf(stderr, L"%ls: error %lu\n", where, e);
    }
    else {
        while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n')) buf[--n] = 0;
        fwprintf(stderr, L"%ls: error %lu: %ls\n", where, e, buf);
    }
}

static void note_runtime_error_code(const wchar_t* where, DWORD e) {
    g_runtime_errors++;
    if (g_opt.verbose) print_error_code(where, e);
}

static void note_runtime_error_last(const wchar_t* where) {
    note_runtime_error_code(where, GetLastError());
}

static void note_runtime_error_text(const wchar_t* where, const wchar_t* detail) {
    g_runtime_errors++;
    if (g_opt.verbose) {
        fwprintf(stderr, L"%ls: %ls\n", where, detail ? detail : L"error");
    }
}

static void note_runtime_hresult(const wchar_t* where, HRESULT hr) {
    g_runtime_errors++;
    if (g_opt.verbose) fwprintf(stderr, L"%ls: HRESULT 0x%08lX\n", where, (unsigned long)hr);
}

static int finish_command(unsigned long errors_before) {
    unsigned long errors = g_runtime_errors - errors_before;
    if (!errors) return 0;
    fwprintf(stderr, L"quuid: operation incomplete (%lu error%ls)%ls\n",
        errors, errors == 1 ? L"" : L"s", g_opt.verbose ? L"" : L"; use --verbose for details");
    return 1;
}

// ============================= small utils =============================

static int is_hex_w(wchar_t c) {
    return (c >= L'0' && c <= L'9') ||
        (c >= L'a' && c <= L'f') ||
        (c >= L'A' && c <= L'F');
}

static int hex_val_w(wchar_t c) {
    if (c >= L'0' && c <= L'9') return (int)(c - L'0');
    if (c >= L'a' && c <= L'f') return 10 + (int)(c - L'a');
    if (c >= L'A' && c <= L'F') return 10 + (int)(c - L'A');
    return -1;
}

static void skip_space_w(const wchar_t** ps) {
    const wchar_t* s = *ps;
    while (*s == L' ' || *s == L'\t' || *s == L'\r' || *s == L'\n' ||
        *s == L'\v' || *s == L'\f') {
        s++;
    }
    *ps = s;
}

static int take_char_w(const wchar_t** ps, wchar_t expected) {
    skip_space_w(ps);
    if (**ps != expected) return 0;
    (*ps)++;
    return 1;
}

static int parse_c_hex_w(const wchar_t** ps, size_t digits, DWORD* out) {
    const wchar_t* s = *ps;
    DWORD value = 0;

    skip_space_w(&s);
    if (s[0] != L'0' || (s[1] != L'x' && s[1] != L'X')) return 0;
    s += 2;

    for (size_t i = 0; i < digits; i++) {
        int hv = hex_val_w(s[i]);
        if (hv < 0) return 0;
        value = (value << 4) | (DWORD)hv;
    }
    s += digits;

    *ps = s;
    *out = value;
    return 1;
}

// C GUID initializer:
// {0xdddddddd,0xdddd,0xdddd,{0xdd,0xdd,0xdd,0xdd,0xdd,0xdd,0xdd,0xdd}}
static int parse_guid_c_initializer(const wchar_t* s, GUID* out) {
    const wchar_t* p = s;
    GUID g = { 0 };
    DWORD value = 0;

    if (!take_char_w(&p, L'{')) return 0;
    if (!parse_c_hex_w(&p, 8, &value)) return 0;
    g.Data1 = value;
    if (!take_char_w(&p, L',')) return 0;

    if (!parse_c_hex_w(&p, 4, &value)) return 0;
    g.Data2 = (WORD)value;
    if (!take_char_w(&p, L',')) return 0;

    if (!parse_c_hex_w(&p, 4, &value)) return 0;
    g.Data3 = (WORD)value;
    if (!take_char_w(&p, L',')) return 0;
    if (!take_char_w(&p, L'{')) return 0;

    for (size_t i = 0; i < ARRAYSIZE(g.Data4); i++) {
        if (!parse_c_hex_w(&p, 2, &value)) return 0;
        g.Data4[i] = (BYTE)value;
        if (i + 1 < ARRAYSIZE(g.Data4) && !take_char_w(&p, L',')) return 0;
    }

    if (!take_char_w(&p, L'}')) return 0;
    if (!take_char_w(&p, L'}')) return 0;
    skip_space_w(&p);
    if (*p != L'\0') return 0;

    *out = g;
    return 1;
}

static int hex_val8(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
    return -1;
}

static int parse_u32_hex_n(const char* s, int n, unsigned long* out) {
    unsigned long v = 0;
    for (int i = 0; i < n; i++) {
        int hv = hex_val8(s[i]);
        if (hv < 0) return 0;
        v = (v << 4) | (unsigned long)hv;
    }
    *out = v;
    return 1;
}

static int parse_u16_hex_n(const char* s, int n, unsigned short* out) {
    unsigned long v = 0;
    if (!parse_u32_hex_n(s, n, &v)) return 0;
    *out = (unsigned short)v;
    return 1;
}

// ASCII "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (36 chars)
static int parse_guid_ascii36(const unsigned char* p, GUID* g) {
    for (int i = 0; i < 36; i++) {
        unsigned char c = p[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return 0;
        }
        else {
            if (!((c >= '0' && c <= '9') ||
                (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F')))
                return 0;
        }
    }

    unsigned long d1 = 0;
    unsigned short d2 = 0, d3 = 0;

    if (!parse_u32_hex_n((const char*)p + 0, 8, &d1)) return 0;
    if (!parse_u16_hex_n((const char*)p + 9, 4, &d2)) return 0;
    if (!parse_u16_hex_n((const char*)p + 14, 4, &d3)) return 0;

    // group4: 4 hex chars (2 bytes)
    unsigned short d4_0 = 0;
    if (!parse_u16_hex_n((const char*)p + 19, 4, &d4_0)) return 0;

    // last 12 hex => 6 bytes
    unsigned char d4_rest[6];
    for (int i = 0; i < 6; i++) {
        int hi = hex_val8((char)p[24 + i * 2]);
        int lo = hex_val8((char)p[24 + i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        d4_rest[i] = (unsigned char)((hi << 4) | lo);
    }

    g->Data1 = (DWORD)d1;
    g->Data2 = (WORD)d2;
    g->Data3 = (WORD)d3;
    g->Data4[0] = (unsigned char)((d4_0 >> 8) & 0xFF);
    g->Data4[1] = (unsigned char)(d4_0 & 0xFF);
    for (int i = 0; i < 6; i++) g->Data4[2 + i] = d4_rest[i];

    return 1;
}

static int match_guid_ascii_at(const unsigned char* p, size_t remaining, size_t* consumed, GUID* out) {
    // {GUID} (38) or GUID (36)
    if (remaining >= 38 && p[0] == '{' && p[37] == '}') {
        if (parse_guid_ascii36(p + 1, out)) {
            *consumed = 38;
            return 1;
        }
        return 0;
    }
    if (remaining >= 36) {
        if (parse_guid_ascii36(p, out)) {
            *consumed = 36;
            return 1;
        }
    }
    return 0;
}

// Heuristic for GUID stored in memory layout (little-endian Data1/2/3)
// Variant bits live in byte[8]; version nibble lives in byte[7] high nibble.
static int looks_like_guid_memlayout_rfc4122(const unsigned char* b) {
    // Variant: 10xx xxxx
    if ((b[8] & 0xC0) != 0x80) return 0;
    // Version: 1..5 is common
    unsigned v = (unsigned)(b[7] >> 4);
    if (v < 1 || v > 5) return 0;
    return 1;
}

static int looks_like_guid_memlayout_loose(const unsigned char* b) {
    // Keep only variant constraint to reduce total noise a bit
    return ((b[8] & 0xC0) == 0x80);
}

// Accept:
// - {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
// - xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
// - xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx (32 hex)
// - {0xdddddddd,0xdddd,0xdddd,{0xdd,0xdd,0xdd,0xdd,0xdd,0xdd,0xdd,0xdd}}
static int parse_guid_any(const wchar_t* s, GUID* out) {
    if (!s || !*s) return 0;

    HRESULT hr = CLSIDFromString((LPOLESTR)s, out);
    if (SUCCEEDED(hr)) return 1;

    if (parse_guid_c_initializer(s, out)) return 1;

    size_t n = wcslen(s);
    if (n == 36) {
        wchar_t tmp[64];
        tmp[0] = L'{';
        wmemcpy(tmp + 1, s, 36);
        tmp[37] = L'}';
        tmp[38] = 0;
        hr = CLSIDFromString((LPOLESTR)tmp, out);
        if (SUCCEEDED(hr)) return 1;
    }

    if (n == 32) {
        for (size_t i = 0; i < 32; i++) if (!is_hex_w(s[i])) return 0;
        wchar_t tmp[64];
        swprintf(tmp, ARRAYSIZE(tmp),
            L"{%.*ls-%.*ls-%.*ls-%.*ls-%.*ls}",
            8, s, 4, s + 8, 4, s + 12, 4, s + 16, 12, s + 20);
        hr = CLSIDFromString((LPOLESTR)tmp, out);
        return SUCCEEDED(hr);
    }

    return 0;
}

static void guid_to_string_braced(const GUID* g, wchar_t* out, size_t cchOut) {
    if (!out || cchOut == 0) return;
    out[0] = 0;
    if (!StringFromGUID2(g, out, (int)cchOut)) {
        note_runtime_error_code(L"StringFromGUID2", ERROR_INSUFFICIENT_BUFFER);
    }
}

static void print_guid_forms(const GUID* g, int one_line) {
    wchar_t s[64];
    guid_to_string_braced(g, s, ARRAYSIZE(s));

    if (one_line) {
        wprintf(L"%ls\n", s);
        return;
    }

    wchar_t plain[64];
    wcsncpy_s(plain, ARRAYSIZE(plain), s, _TRUNCATE);
    size_t n = wcslen(plain);
    if (n >= 2 && plain[0] == L'{' && plain[n - 1] == L'}') {
        plain[n - 1] = 0;
        memmove(plain, plain + 1, (wcslen(plain) + 1) * sizeof(wchar_t));
    }

    wprintf(L"GUID:\n");
    wprintf(L"  braced : %ls\n", s);
    wprintf(L"  dashed : %ls\n", plain);

    wprintf(L"  fields : Data1=0x%08lX Data2=0x%04X Data3=0x%04X Data4=0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X\n",
        (unsigned long)g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);

    wprintf(L"  C init : {0x%08lX, 0x%04X, 0x%04X, {0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,0x%02X}}\n",
        (unsigned long)g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
        g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);

    unsigned char bytes[16];
    memcpy(bytes, g, 16);

    wprintf(L"  db     : ");
    for (int i = 0; i < 16; i++) {
        wprintf(L"0x%02X%s", bytes[i], (i == 15) ? L"" : L",");
    }
    wprintf(L"\n");

    wprintf(L"  ddq    : 0x");
    for (int i = 15; i >= 0; i--) wprintf(L"%02X", bytes[i]);
    wprintf(L"\n");
}

// ============================= GUID set (dedupe) =============================

typedef struct GUIDSET {
    GUID* items;
    unsigned char* used;
    size_t cap; // power of two
    size_t len;
} GUIDSET;

static unsigned long long fnv1a64(const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int guid_equal(const GUID* a, const GUID* b) {
    return memcmp(a, b, sizeof(GUID)) == 0;
}

static int guidset_init(GUIDSET* s, size_t initialCapPow2) {
    if (!s) return 0;
    size_t cap = 1;
    size_t want = (initialCapPow2 < 64) ? 64 : initialCapPow2;
    while (cap < want) cap <<= 1;
    s->cap = cap;
    s->len = 0;
    s->items = (GUID*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, s->cap * sizeof(GUID));
    s->used = (unsigned char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, s->cap);
    if (!s->items || !s->used) {
        if (s->items) HeapFree(GetProcessHeap(), 0, s->items);
        if (s->used) HeapFree(GetProcessHeap(), 0, s->used);
        ZeroMemory(s, sizeof(*s));
        return 0;
    }
    return 1;
}

static void guidset_free(GUIDSET* s) {
    if (!s) return;
    if (s->items) HeapFree(GetProcessHeap(), 0, s->items);
    if (s->used)  HeapFree(GetProcessHeap(), 0, s->used);
    ZeroMemory(s, sizeof(*s));
}

static int guidset_rehash(GUIDSET* s, size_t newCap) {
    GUIDSET ns;
    if (!guidset_init(&ns, newCap)) return 0;

    for (size_t i = 0; i < s->cap; i++) {
        if (!s->used[i]) continue;
        GUID g = s->items[i];

        unsigned long long h = fnv1a64(&g, sizeof(GUID));
        size_t mask = ns.cap - 1;
        size_t pos = (size_t)h & mask;
        for (;;) {
            if (!ns.used[pos]) {
                ns.used[pos] = 1;
                ns.items[pos] = g;
                ns.len++;
                break;
            }
            pos = (pos + 1) & mask;
        }
    }

    guidset_free(s);
    *s = ns;
    return 1;
}

static int guidset_add(GUIDSET* s, const GUID* g) {
    if (!s || !g) return 0;

    if ((s->len + 1) * 10 >= s->cap * 7) {
        if (!guidset_rehash(s, s->cap * 2)) return 0;
    }

    unsigned long long h = fnv1a64(g, sizeof(GUID));
    size_t mask = s->cap - 1;
    size_t pos = (size_t)h & mask;

    for (;;) {
        if (!s->used[pos]) {
            s->used[pos] = 1;
            s->items[pos] = *g;
            s->len++;
            return 1;
        }
        if (guid_equal(&s->items[pos], g)) return 1;
        pos = (pos + 1) & mask;
    }
}

static void guidset_foreach(const GUIDSET* s, void (*fn)(const GUID*, void*), void* ctx) {
    if (!s || !fn) return;
    for (size_t i = 0; i < s->cap; i++) {
        if (s->used[i]) fn(&s->items[i], ctx);
    }
}

// ============================= registry =============================

static REGSAM reg_sam_read(void) {
    return KEY_READ | g_opt.reg_view;
}

static int reg_error_is_absence(LONG r) {
    return r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND;
}

// Returns 1 for success, 0 for an absent key/value, and -1 for an operational error.
static int reg_open_checked(HKEY root, const wchar_t* subkey, HKEY* out) {
    LONG r = RegOpenKeyExW(root, subkey, 0, reg_sam_read(), out);
    if (r == ERROR_SUCCESS) return 1;
    if (reg_error_is_absence(r)) return 0;
    note_runtime_error_code(L"RegOpenKeyExW", (DWORD)r);
    return -1;
}

static int reg_open_hkcr(const wchar_t* subkey, HKEY* out) {
    return reg_open_checked(HKEY_CLASSES_ROOT, subkey, out);
}

static int reg_query_string_value(HKEY k, const wchar_t* name_or_null, wchar_t* out, DWORD cchOut, DWORD* outType) {
    if (!out || cchOut == 0) return 0;
    out[0] = 0;
    DWORD type = 0;
    DWORD cb = cchOut * sizeof(wchar_t);
    LONG r = RegQueryValueExW(k, name_or_null, NULL, &type, (BYTE*)out, &cb);
    if (r != ERROR_SUCCESS) {
        if (reg_error_is_absence(r)) return 0;
        note_runtime_error_code(L"RegQueryValueExW", (DWORD)r);
        return -1;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        note_runtime_error_text(L"RegQueryValueExW", L"value is not REG_SZ or REG_EXPAND_SZ");
        return -1;
    }
    if ((cb % sizeof(wchar_t)) != 0) {
        note_runtime_error_text(L"RegQueryValueExW", L"string value has an odd byte length");
        return -1;
    }

    size_t cch = cb / sizeof(wchar_t);
    if (cch > cchOut || (cch && out[cch - 1] != L'\0')) {
        out[cchOut - 1] = 0;
        note_runtime_error_text(L"RegQueryValueExW", L"string value is truncated or unterminated");
        return -1;
    }
    if (!cch) out[0] = 0;
    if (outType) *outType = type;
    return 1;
}

static int copy_registry_string(wchar_t* out, DWORD cchOut, const wchar_t* value) {
    size_t n = wcslen(value);
    if (n >= cchOut) {
        out[0] = 0;
        note_runtime_error_text(L"registry string", L"expanded value does not fit the destination buffer");
        return -1;
    }
    wmemcpy(out, value, n + 1);
    return 1;
}

static int reg_read_default_string_expanded(HKEY k, wchar_t* out, DWORD cchOut) {
    DWORD type = 0;
    wchar_t tmp[2048];
    int qr = reg_query_string_value(k, NULL, tmp, ARRAYSIZE(tmp), &type);
    if (qr != 1) return qr;

    if (type == REG_EXPAND_SZ) {
        DWORD n = ExpandEnvironmentStringsW(tmp, out, cchOut);
        if (n == 0 || n > cchOut) {
            note_runtime_error_code(L"ExpandEnvironmentStringsW", n == 0 ? GetLastError() : ERROR_MORE_DATA);
            out[0] = 0;
            return -1;
        }
        return 1;
    }

    return copy_registry_string(out, cchOut, tmp);
}

static int reg_read_named_string_expanded(HKEY k, const wchar_t* name, wchar_t* out, DWORD cchOut) {
    DWORD type = 0;
    wchar_t tmp[2048];
    int qr = reg_query_string_value(k, name, tmp, ARRAYSIZE(tmp), &type);
    if (qr != 1) return qr;

    if (type == REG_EXPAND_SZ) {
        DWORD n = ExpandEnvironmentStringsW(tmp, out, cchOut);
        if (n == 0 || n > cchOut) {
            note_runtime_error_code(L"ExpandEnvironmentStringsW", n == 0 ? GetLastError() : ERROR_MORE_DATA);
            out[0] = 0;
            return -1;
        }
        return 1;
    }

    return copy_registry_string(out, cchOut, tmp);
}

static void print_key_value_line(const wchar_t* label, const wchar_t* val) {
    if (val && *val) wprintf(L"    %-18ls %ls\n", label, val);
}

static void query_clsid_view(const GUID* g, DWORD viewFlag, const wchar_t* header) {
    DWORD saved = g_opt.reg_view;
    g_opt.reg_view = viewFlag;

    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t path[160];
    swprintf(path, ARRAYSIZE(path), L"CLSID\\%ls", gs);

    HKEY k = NULL;
    if (reg_open_hkcr(path, &k) != 1) { g_opt.reg_view = saved; return; }

    wprintf(L"  [CLSID%s] ", header);
    wchar_t name[512];
    if (reg_read_default_string_expanded(k, name, ARRAYSIZE(name)) == 1) wprintf(L"%ls\n", name);
    else wprintf(L"(no name)\n");

    // InprocServer32 / LocalServer32 + ThreadingModel
    const wchar_t* subkeys[] = { L"InprocServer32", L"LocalServer32" };
    for (int i = 0; i < (int)ARRAYSIZE(subkeys); i++) {
        HKEY sk = NULL;
        if (reg_open_checked(k, subkeys[i], &sk) == 1) {
            wchar_t v[1024];
            if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v)) == 1) {
                print_key_value_line(subkeys[i], v);
            }
            if (_wcsicmp(subkeys[i], L"InprocServer32") == 0) {
                wchar_t tm[64];
                if (reg_read_named_string_expanded(sk, L"ThreadingModel", tm, ARRAYSIZE(tm)) == 1) {
                    print_key_value_line(L"ThreadingModel", tm);
                }
            }
            RegCloseKey(sk);
        }
    }

    // ProgID / VIProgID / TreatAs
    const wchar_t* misc[] = { L"ProgID", L"VersionIndependentProgID", L"TreatAs" };
    for (int i = 0; i < (int)ARRAYSIZE(misc); i++) {
        HKEY sk = NULL;
        if (reg_open_checked(k, misc[i], &sk) == 1) {
            wchar_t v[1024];
            if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v)) == 1) {
                print_key_value_line(misc[i], v);
            }
            RegCloseKey(sk);
        }
    }

    wchar_t appid[256];
    if (reg_read_named_string_expanded(k, L"AppID", appid, ARRAYSIZE(appid)) == 1) {
        print_key_value_line(L"AppID", appid);
    }

    RegCloseKey(k);
    g_opt.reg_view = saved;
}

static void query_iid_view(const GUID* g, DWORD viewFlag, const wchar_t* header) {
    DWORD saved = g_opt.reg_view;
    g_opt.reg_view = viewFlag;

    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t path[160];
    swprintf(path, ARRAYSIZE(path), L"Interface\\%ls", gs);

    HKEY k = NULL;
    if (reg_open_hkcr(path, &k) != 1) { g_opt.reg_view = saved; return; }

    wprintf(L"  [IID%s] ", header);
    wchar_t name[512];
    if (reg_read_default_string_expanded(k, name, ARRAYSIZE(name)) == 1) wprintf(L"%ls\n", name);
    else wprintf(L"(no name)\n");

    HKEY sk = NULL;
    if (reg_open_checked(k, L"ProxyStubClsid32", &sk) == 1) {
        wchar_t v[256];
        if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v)) == 1) {
            print_key_value_line(L"ProxyStubClsid32", v);
        }
        RegCloseKey(sk);
    }

    wchar_t typelib[256];
    if (reg_read_named_string_expanded(k, L"TypeLib", typelib, ARRAYSIZE(typelib)) == 1) {
        print_key_value_line(L"TypeLib", typelib);
    }
    wchar_t num[256];
    if (reg_read_named_string_expanded(k, L"NumMethods", num, ARRAYSIZE(num)) == 1) {
        print_key_value_line(L"NumMethods", num);
    }

    RegCloseKey(k);
    g_opt.reg_view = saved;
}

static void query_typelib_view(const GUID* g, DWORD viewFlag, const wchar_t* header) {
    DWORD saved = g_opt.reg_view;
    g_opt.reg_view = viewFlag;

    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t path[160];
    swprintf(path, ARRAYSIZE(path), L"TypeLib\\%ls", gs);

    HKEY k = NULL;
    if (reg_open_hkcr(path, &k) != 1) { g_opt.reg_view = saved; return; }

    wprintf(L"  [TypeLib%s] ", header);
    wchar_t name[512];
    if (reg_read_default_string_expanded(k, name, ARRAYSIZE(name)) == 1) wprintf(L"%ls\n", name);
    else wprintf(L"(no name)\n");

    // Enumerate versions and show win32/win64 paths when present:
    DWORD idx = 0;
    wchar_t ver[256];

    for (;;) {
        DWORD cchVer = ARRAYSIZE(ver);
        LONG er = RegEnumKeyExW(k, idx, ver, &cchVer, NULL, NULL, NULL, NULL);
        if (er == ERROR_NO_MORE_ITEMS) break;
        if (er != ERROR_SUCCESS) {
            note_runtime_error_code(L"RegEnumKeyExW(TypeLib versions)", (DWORD)er);
            break;
        }
        idx++;
        wprintf(L"    version            %ls\n", ver);

        wchar_t sub0[512];
        swprintf(sub0, ARRAYSIZE(sub0), L"%ls\\0\\win32", ver);
        HKEY vk = NULL;
        if (reg_open_checked(k, sub0, &vk) == 1) {
            wchar_t pth[1024];
            if (reg_read_default_string_expanded(vk, pth, ARRAYSIZE(pth)) == 1) {
                print_key_value_line(L"win32", pth);
            }
            RegCloseKey(vk);
        }

        swprintf(sub0, ARRAYSIZE(sub0), L"%ls\\0\\win64", ver);
        if (reg_open_checked(k, sub0, &vk) == 1) {
            wchar_t pth[1024];
            if (reg_read_default_string_expanded(vk, pth, ARRAYSIZE(pth)) == 1) {
                print_key_value_line(L"win64", pth);
            }
            RegCloseKey(vk);
        }

    }

    RegCloseKey(k);
    g_opt.reg_view = saved;
}

static void query_appid_view(const GUID* g, DWORD viewFlag, const wchar_t* header) {
    DWORD saved = g_opt.reg_view;
    g_opt.reg_view = viewFlag;

    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t path[160];
    swprintf(path, ARRAYSIZE(path), L"AppID\\%ls", gs);

    HKEY k = NULL;
    if (reg_open_hkcr(path, &k) != 1) { g_opt.reg_view = saved; return; }

    wprintf(L"  [AppID%s] ", header);
    wchar_t name[512];
    if (reg_read_default_string_expanded(k, name, ARRAYSIZE(name)) == 1) wprintf(L"%ls\n", name);
    else wprintf(L"(no name)\n");

    wchar_t v[1024];
    if (reg_read_named_string_expanded(k, L"LocalService", v, ARRAYSIZE(v)) == 1) print_key_value_line(L"LocalService", v);
    if (reg_read_named_string_expanded(k, L"ServiceParameters", v, ARRAYSIZE(v)) == 1) print_key_value_line(L"ServiceParameters", v);
    if (reg_read_named_string_expanded(k, L"RunAs", v, ARRAYSIZE(v)) == 1) print_key_value_line(L"RunAs", v);
    if (reg_read_named_string_expanded(k, L"DllSurrogate", v, ARRAYSIZE(v)) == 1) print_key_value_line(L"DllSurrogate", v);

    RegCloseKey(k);
    g_opt.reg_view = saved;
}

static int any_registry_hit_view(const GUID* g, DWORD viewFlag) {
    DWORD saved = g_opt.reg_view;
    g_opt.reg_view = viewFlag;

    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t p1[160], p2[160], p3[160], p4[160];
    swprintf(p1, ARRAYSIZE(p1), L"CLSID\\%ls", gs);
    swprintf(p2, ARRAYSIZE(p2), L"Interface\\%ls", gs);
    swprintf(p3, ARRAYSIZE(p3), L"TypeLib\\%ls", gs);
    swprintf(p4, ARRAYSIZE(p4), L"AppID\\%ls", gs);

    HKEY k = NULL;
    int hit = 0;
    int rr = reg_open_hkcr(p1, &k);
    if (rr == 1) { hit = 1; RegCloseKey(k); }
    if (!hit) {
        rr = reg_open_hkcr(p2, &k);
        if (rr == 1) { hit = 1; RegCloseKey(k); }
    }
    if (!hit) {
        rr = reg_open_hkcr(p3, &k);
        if (rr == 1) { hit = 1; RegCloseKey(k); }
    }
    if (!hit) {
        rr = reg_open_hkcr(p4, &k);
        if (rr == 1) { hit = 1; RegCloseKey(k); }
    }

    g_opt.reg_view = saved;
    return hit;
}

static void query_all_categories(const GUID* g) {
    if (!g_opt.both_views) {
        query_clsid_view(g, g_opt.reg_view, L"");
        query_iid_view(g, g_opt.reg_view, L"");
        query_typelib_view(g, g_opt.reg_view, L"");
        query_appid_view(g, g_opt.reg_view, L"");
        return;
    }

    // Explicit dual-view output
    query_clsid_view(g, KEY_WOW64_64KEY, L":64");
    query_iid_view(g, KEY_WOW64_64KEY, L":64");
    query_typelib_view(g, KEY_WOW64_64KEY, L":64");
    query_appid_view(g, KEY_WOW64_64KEY, L":64");

    query_clsid_view(g, KEY_WOW64_32KEY, L":32");
    query_iid_view(g, KEY_WOW64_32KEY, L":32");
    query_typelib_view(g, KEY_WOW64_32KEY, L":32");
    query_appid_view(g, KEY_WOW64_32KEY, L":32");
}

// Resolve CLSID -> InprocServer32/LocalServer32 (expanded), return extracted primary module path.
static int extract_primary_module_path(const wchar_t* value, int is_command_line, wchar_t* out, DWORD cchOut) {
    if (!value || !*value) return 0;
    while (*value == L' ' || *value == L'\t') value++;
    if (!*value) {
        note_runtime_error_text(L"CLSID server path", L"registration contains only whitespace");
        return 0;
    }

    if (*value == L'"') {
        value++;
        const wchar_t* end = wcschr(value, L'"');
        if (!end) {
            note_runtime_error_text(L"CLSID server path", L"unterminated quote in registration");
            return 0;
        }
        size_t n = (size_t)(end - value);
        if (!n) {
            note_runtime_error_text(L"CLSID server path", L"registration contains an empty quoted path");
            return 0;
        }
        if (n + 1 > cchOut) {
            note_runtime_error_text(L"CLSID server path", L"module path is too long");
            return 0;
        }
        wmemcpy(out, value, n);
        out[n] = 0;
        return 1;
    }
    else {
        const wchar_t* end = value + wcslen(value);
        if (is_command_line) {
            end = value;
            while (*end && *end != L' ' && *end != L'\t') end++;
        }
        else {
            while (end > value && (end[-1] == L' ' || end[-1] == L'\t')) end--;
        }
        size_t n = (size_t)(end - value);
        if (n + 1 > cchOut) {
            note_runtime_error_text(L"CLSID server path", L"module path is too long");
            return 0;
        }
        wmemcpy(out, value, n);
        out[n] = 0;
        return 1;
    }
}

static int resolve_clsid_server_paths(const GUID* clsid, wchar_t* outInproc, DWORD cchInproc, wchar_t* outLocal, DWORD cchLocal) {
    if (outInproc && cchInproc) outInproc[0] = 0;
    if (outLocal && cchLocal) outLocal[0] = 0;

    wchar_t gs[64];
    guid_to_string_braced(clsid, gs, ARRAYSIZE(gs));

    wchar_t path[160];
    swprintf(path, ARRAYSIZE(path), L"CLSID\\%ls", gs);

    HKEY k = NULL;
    if (reg_open_hkcr(path, &k) != 1) return 0;

    int any = 0;

    HKEY sk = NULL;
    if (outInproc && reg_open_checked(k, L"InprocServer32", &sk) == 1) {
        wchar_t v[1024];
        if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v)) == 1) {
            wchar_t mod[1024];
            if (extract_primary_module_path(v, 0, mod, ARRAYSIZE(mod))) {
                wcsncpy_s(outInproc, cchInproc, mod, _TRUNCATE);
                any = 1;
            }
        }
        RegCloseKey(sk);
    }

    if (outLocal && reg_open_checked(k, L"LocalServer32", &sk) == 1) {
        wchar_t v[1024];
        if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v)) == 1) {
            wchar_t mod[1024];
            if (extract_primary_module_path(v, 1, mod, ARRAYSIZE(mod))) {
                wcsncpy_s(outLocal, cchLocal, mod, _TRUNCATE);
                any = 1;
            }
        }
        RegCloseKey(sk);
    }

    RegCloseKey(k);
    return any;
}

// ============================= scanning =============================

typedef struct SCANOPTS {
    int with_registry;
    int binary_scan;
    int binary_loose;
    int locate;       // print per-hit with file offsets
    int one_line;     // when printing unique set, print only GUID strings
} SCANOPTS;

typedef struct SCANSTATS {
    unsigned long long files_scanned;
    unsigned long long bytes_scanned;
    unsigned long long ascii_hits;
    unsigned long long bin_hits;
} SCANSTATS;

static int is_dot_or_dotdot(const wchar_t* name) {
    return (name[0] == L'.' && name[1] == 0) || (name[0] == L'.' && name[1] == L'.' && name[2] == 0);
}

static int join_path(wchar_t* out, size_t cchOut, const wchar_t* a, const wchar_t* b) {
    size_t na = wcslen(a);
    int needSlash = (na > 0 && (a[na - 1] != L'\\' && a[na - 1] != L'/'));
    int n = needSlash
        ? swprintf(out, cchOut, L"%ls\\%ls", a, b)
        : swprintf(out, cchOut, L"%ls%ls", a, b);
    return n >= 0 && (size_t)n < cchOut;
}

static void locate_hit(const wchar_t* path, unsigned long long off, const wchar_t* kind, const GUID* g) {
    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));
    wprintf(L"%ls:%llu:%ls:%ls\n", path, off, kind, gs);
}

static int scan_stream_for_guids(
    const wchar_t* path,
    GUIDSET* set,
    SCANSTATS* st,
    const SCANOPTS* opt)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { note_runtime_error_last(L"CreateFileW"); return 0; }

    // 4 MiB chunks with 64-byte overlap (enough for "{...}" and safety)
    const DWORD CHUNK = 4u * 1024u * 1024u;
    const DWORD OVERLAP = 64u;

    unsigned char* buf = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, CHUNK + OVERLAP);
    if (!buf) {
        note_runtime_error_code(L"HeapAlloc(scan buffer)", ERROR_OUTOFMEMORY);
        CloseHandle(h);
        return 0;
    }

    st->files_scanned++;

    DWORD keep = 0;
    unsigned long long base_off = 0;

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf + keep, CHUNK, &got, NULL)) {
            note_runtime_error_last(L"ReadFile");
            HeapFree(GetProcessHeap(), 0, buf);
            CloseHandle(h);
            return 0;
        }
        if (got == 0) break;

        DWORD prefix = keep;
        DWORD avail = keep + got;
        st->bytes_scanned += (unsigned long long)got;

        // ASCII scan
        for (DWORD i = 0; i + 36 <= avail; i++) {
            unsigned char c = buf[i];
            if (!(c == '{' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                continue;

            GUID g;
            size_t consumed = 0;
            if (match_guid_ascii_at(buf + i, avail - i, &consumed, &g)) {
                // A hit wholly inside the retained prefix was already scanned in the prior chunk.
                if ((size_t)i + consumed > prefix) {
                    if (!guidset_add(set, &g)) {
                        note_runtime_error_code(L"HeapAlloc(GUID set)", ERROR_OUTOFMEMORY);
                        HeapFree(GetProcessHeap(), 0, buf);
                        CloseHandle(h);
                        return 0;
                    }
                    st->ascii_hits++;
                    if (opt->locate) locate_hit(path, base_off + i, L"ascii", &g);
                }
                i += (DWORD)(consumed ? (consumed - 1) : 0);
            }
        }

        // Binary scan (16-byte windows)
        if (opt->binary_scan) {
            for (DWORD i = 0; i + 16 <= avail; i++) {
                const unsigned char* b = buf + i;
                int ok = opt->binary_loose ? looks_like_guid_memlayout_loose(b)
                    : looks_like_guid_memlayout_rfc4122(b);
                if (!ok) continue;

                GUID g;
                memcpy(&g, b, 16);
                if (i + 16 > prefix) {
                    if (!guidset_add(set, &g)) {
                        note_runtime_error_code(L"HeapAlloc(GUID set)", ERROR_OUTOFMEMORY);
                        HeapFree(GetProcessHeap(), 0, buf);
                        CloseHandle(h);
                        return 0;
                    }
                    st->bin_hits++;
                    if (opt->locate) locate_hit(path, base_off + i, opt->binary_loose ? L"bin-loose" : L"bin", &g);
                }
            }
        }

        // prepare overlap for next read
        if (avail >= OVERLAP) {
            memmove(buf, buf + (avail - OVERLAP), OVERLAP);
            keep = OVERLAP;
            base_off += (unsigned long long)(avail - OVERLAP);
        }
        else {
            memmove(buf, buf, avail);
            keep = avail;
            // base_off unchanged
        }
    }

    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(h);
    return 1;
}

static int scan_path_recursive(const wchar_t* path, GUIDSET* set, SCANSTATS* st, const SCANOPTS* opt) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) { note_runtime_error_last(L"GetFileAttributesW"); return 0; }

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return 1;

        const size_t path_cch = 32768;
        wchar_t* path_buffers = (wchar_t*)HeapAlloc(
            GetProcessHeap(), 0, path_cch * 2 * sizeof(wchar_t));
        if (!path_buffers) {
            note_runtime_error_code(L"HeapAlloc(scan paths)", ERROR_OUTOFMEMORY);
            return 0;
        }
        wchar_t* pat = path_buffers;
        wchar_t* child = path_buffers + path_cch;

        if (swprintf(pat, path_cch, L"%ls\\*", path) < 0) {
            note_runtime_error_code(L"scan path", ERROR_FILENAME_EXCED_RANGE);
            HeapFree(GetProcessHeap(), 0, path_buffers);
            return 0;
        }

        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) {
            DWORD find_error = GetLastError();
            HeapFree(GetProcessHeap(), 0, path_buffers);
            if (find_error == ERROR_FILE_NOT_FOUND || find_error == ERROR_NO_MORE_FILES) return 1;
            note_runtime_error_code(L"FindFirstFileW", find_error);
            return 0;
        }

        int complete = 1;

        do {
            if (is_dot_or_dotdot(fd.cFileName)) continue;

            // Skip reparse points by default (avoid junction/symlink loops)
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }

            if (!join_path(child, path_cch, path, fd.cFileName)) {
                note_runtime_error_code(L"scan path", ERROR_FILENAME_EXCED_RANGE);
                complete = 0;
                continue;
            }

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!scan_path_recursive(child, set, st, opt)) complete = 0;
            }
            else {
                if (!scan_stream_for_guids(child, set, st, opt)) complete = 0;
            }
        } while (FindNextFileW(f, &fd));

        DWORD enum_error = GetLastError();
        if (enum_error != ERROR_NO_MORE_FILES) {
            note_runtime_error_code(L"FindNextFileW", enum_error);
            complete = 0;
        }

        if (!FindClose(f)) {
            note_runtime_error_last(L"FindClose");
            complete = 0;
        }
        HeapFree(GetProcessHeap(), 0, path_buffers);
        return complete;
    }

    return scan_stream_for_guids(path, set, st, opt);
}

// ============================= TypeLib enumeration =============================

static const wchar_t* typekind_name(TYPEKIND k) {
    switch (k) {
    case TKIND_ENUM: return L"enum";
    case TKIND_RECORD: return L"record";
    case TKIND_MODULE: return L"module";
    case TKIND_INTERFACE: return L"interface";
    case TKIND_DISPATCH: return L"dispatch";
    case TKIND_COCLASS: return L"coclass";
    case TKIND_ALIAS: return L"alias";
    case TKIND_UNION: return L"union";
    default: return L"unknown";
    }
}

static int cmd_tlb(const wchar_t* file) {
    unsigned long errors_before = g_runtime_errors;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        fwprintf(stderr, L"CoInitializeEx failed: 0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    ITypeLib* tlb = NULL;
    hr = LoadTypeLibEx(file, REGKIND_NONE, &tlb);
    if (FAILED(hr) || !tlb) {
        fwprintf(stderr, L"LoadTypeLibEx failed: 0x%08lX\n", (unsigned long)hr);
        CoUninitialize();
        return 1;
    }

    TLIBATTR* la = NULL;
    hr = tlb->lpVtbl->GetLibAttr(tlb, &la);
    if (SUCCEEDED(hr) && la) {
        wchar_t gs[64];
        guid_to_string_braced(&la->guid, gs, ARRAYSIZE(gs));
        wprintf(L"TypeLib:\n  LIBID  : %ls\n  LCID   : 0x%04X\n  SYSKIND: %d\n  VER    : %u.%u\n",
            gs, (unsigned)la->lcid, (int)la->syskind,
            (unsigned)la->wMajorVerNum, (unsigned)la->wMinorVerNum);
        tlb->lpVtbl->ReleaseTLibAttr(tlb, la);
    }
    else {
        if (FAILED(hr)) note_runtime_hresult(L"ITypeLib::GetLibAttr", hr);
        else note_runtime_error_text(L"ITypeLib::GetLibAttr", L"returned no library attributes");
    }

    UINT count = tlb->lpVtbl->GetTypeInfoCount(tlb);
    wprintf(L"  TYPES  : %u\n", (unsigned)count);

    for (UINT i = 0; i < count; i++) {
        ITypeInfo* ti = NULL;
        hr = tlb->lpVtbl->GetTypeInfo(tlb, i, &ti);
        if (FAILED(hr) || !ti) {
            if (FAILED(hr)) note_runtime_hresult(L"ITypeLib::GetTypeInfo", hr);
            else note_runtime_error_text(L"ITypeLib::GetTypeInfo", L"returned no type information");
            continue;
        }

        TYPEATTR* ta = NULL;
        hr = ti->lpVtbl->GetTypeAttr(ti, &ta);
        if (SUCCEEDED(hr) && ta) {
            BSTR bname = NULL;
            HRESULT doc_hr = ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL, &bname, NULL, NULL, NULL);
            if (FAILED(doc_hr)) note_runtime_hresult(L"ITypeInfo::GetDocumentation", doc_hr);

            wchar_t gs[64];
            guid_to_string_braced(&ta->guid, gs, ARRAYSIZE(gs));
            wprintf(L"  %ls  %-10ls  %ls\n",
                gs, typekind_name(ta->typekind),
                bname ? (const wchar_t*)bname : L"(noname)");

            if (bname) SysFreeString(bname);
            ti->lpVtbl->ReleaseTypeAttr(ti, ta);
        }
        else {
            if (FAILED(hr)) note_runtime_hresult(L"ITypeInfo::GetTypeAttr", hr);
            else note_runtime_error_text(L"ITypeInfo::GetTypeAttr", L"returned no type attributes");
        }

        ti->lpVtbl->Release(ti);
    }

    tlb->lpVtbl->Release(tlb);
    CoUninitialize();
    return finish_command(errors_before);
}

// ============================= commands =============================

static void usage(void) {
    wprintf(L"quuid — GUID/COM discovery CLI\n\n");
    wprintf(L"Options may appear before or after the command and its argument.\n\n");
    wprintf(L"Global options:\n");
    wprintf(L"  --verbose  --help\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  quuid parse  <guid> [--one-line]\n");
    wprintf(L"  quuid find   <guid> [--wow32|--wow64] [--both-views]\n");
    wprintf(L"  quuid scan   <path> [--registry] [--both-views] [--binary] [--binary-loose] [--locate] [--one-line]\n");
    wprintf(L"  quuid server <clsid-guid> [--scan] [scan flags...]\n");
    wprintf(L"  quuid tlb    <file.tlb|.dll|.ocx>\n");
    wprintf(L"  quuid enum   clsid|iid|typelib|appid [--limit N] [--with-name]\n");
}

static int parse_u32_dec(const wchar_t* s, unsigned long* out) {
    if (!s || !*s) return 0;
    unsigned long v = 0;
    for (const wchar_t* p = s; *p; p++) {
        if (*p < L'0' || *p > L'9') return 0;
        unsigned long digit = (unsigned long)(*p - L'0');
        if (v > (ULONG_MAX - digit) / 10) return 0;
        v = v * 10 + digit;
    }
    *out = v;
    return 1;
}

static int cmd_parse(const wchar_t* s, int one_line) {
    GUID g;
    if (!parse_guid_any(s, &g)) {
        fwprintf(stderr, L"quuid: failed to parse GUID: %ls\n", s);
        return 2;
    }
    print_guid_forms(&g, one_line);
    return 0;
}

static int cmd_find(const wchar_t* s) {
    unsigned long errors_before = g_runtime_errors;
    GUID g;
    if (!parse_guid_any(s, &g)) {
        fwprintf(stderr, L"quuid: failed to parse GUID: %ls\n", s);
        return 2;
    }

    wchar_t gs[64];
    guid_to_string_braced(&g, gs, ARRAYSIZE(gs));
    wprintf(L"%ls\n", gs);

    int hit = 0;
    if (!g_opt.both_views) {
        hit = any_registry_hit_view(&g, g_opt.reg_view);
    }
    else {
        hit = any_registry_hit_view(&g, KEY_WOW64_64KEY) || any_registry_hit_view(&g, KEY_WOW64_32KEY);
    }

    if (!hit) {
        wprintf(L"  (no HKCR hits in CLSID/Interface/TypeLib/AppID)\n");
        return finish_command(errors_before);
    }

    query_all_categories(&g);
    return finish_command(errors_before);
}

typedef struct PRINTCTX {
    const SCANOPTS* opt;
} PRINTCTX;

static void print_guid_cb(const GUID* g, void* vctx) {
    PRINTCTX* ctx = (PRINTCTX*)vctx;
    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    if (ctx->opt->one_line) {
        wprintf(L"%ls\n", gs);
    }
    else {
        wprintf(L"%ls\n", gs);
    }

    if (ctx->opt->with_registry) {
        // If both_views is enabled, query_all_categories already prints :64/:32 headings.
        query_all_categories(g);
    }
}

static int cmd_scan(const wchar_t* path, const SCANOPTS* opt, int report_errors) {
    unsigned long errors_before = g_runtime_errors;
    GUIDSET set;
    if (!guidset_init(&set, 256)) {
        note_runtime_error_code(L"HeapAlloc(GUID set)", ERROR_OUTOFMEMORY);
        if (report_errors) finish_command(errors_before);
        return 1;
    }

    SCANSTATS st = { 0 };
    scan_path_recursive(path, &set, &st, opt);

    if (!opt->one_line && !opt->locate) {
        wprintf(L"Scan:\n");
        wprintf(L"  files      : %llu\n", st.files_scanned);
        wprintf(L"  bytes      : %llu\n", st.bytes_scanned);
        wprintf(L"  ascii_hits : %llu\n", st.ascii_hits);
        wprintf(L"  bin_hits   : %llu\n", st.bin_hits);
        wprintf(L"  unique     : %llu\n", (unsigned long long)set.len);
    }

    PRINTCTX ctx;
    ctx.opt = opt;
    guidset_foreach(&set, print_guid_cb, &ctx);

    guidset_free(&set);
    if (report_errors) return finish_command(errors_before);
    return g_runtime_errors == errors_before ? 0 : 1;
}

static int cmd_enum_root(const wchar_t* which, unsigned long limit, int with_name) {
    unsigned long errors_before = g_runtime_errors;
    const wchar_t* root = NULL;
    const wchar_t* label = NULL;

    if (_wcsicmp(which, L"clsid") == 0) { root = L"CLSID"; label = L"CLSID"; }
    else if (_wcsicmp(which, L"iid") == 0) { root = L"Interface"; label = L"IID"; }
    else if (_wcsicmp(which, L"typelib") == 0) { root = L"TypeLib"; label = L"TypeLib"; }
    else if (_wcsicmp(which, L"appid") == 0) { root = L"AppID"; label = L"AppID"; }
    else {
        fwprintf(stderr, L"quuid: unknown enum category: %ls\n", which);
        return 2;
    }

    HKEY k = NULL;
    int root_result = reg_open_hkcr(root, &k);
    if (root_result != 1) {
        if (root_result == 0) fwprintf(stderr, L"Failed to open HKCR\\%ls: key not found\n", root);
        return root_result == 0 ? 1 : finish_command(errors_before);
    }

    DWORD idx = 0;
    wchar_t sub[256];

    unsigned long printed = 0;
    for (;;) {
        DWORD cchSub = ARRAYSIZE(sub);
        LONG er = RegEnumKeyExW(k, idx, sub, &cchSub, NULL, NULL, NULL, NULL);
        if (er == ERROR_NO_MORE_ITEMS) break;
        if (er != ERROR_SUCCESS) {
            note_runtime_error_code(L"RegEnumKeyExW", (DWORD)er);
            break;
        }
        idx++;
        if (!with_name) {
            wprintf(L"[%ls] %ls\n", label, sub);
        }
        else {
            // open and read default
            HKEY sk = NULL;
            if (reg_open_checked(k, sub, &sk) == 1) {
                wchar_t name[512];
                if (reg_read_default_string_expanded(sk, name, ARRAYSIZE(name)) == 1) {
                    wprintf(L"[%ls] %ls  %ls\n", label, sub, name);
                }
                else {
                    wprintf(L"[%ls] %ls\n", label, sub);
                }
                RegCloseKey(sk);
            }
            else {
                wprintf(L"[%ls] %ls\n", label, sub);
            }
        }

        printed++;
        if (limit && printed >= limit) break;
    }

    RegCloseKey(k);
    return finish_command(errors_before);
}

static int cmd_server_view(const GUID* g, DWORD view, const wchar_t* heading,
    int do_scan, const SCANOPTS* scanopt, int* found_any) {
    DWORD saved = g_opt.reg_view;
    int rc = 0;
    g_opt.reg_view = view;

    if (heading) wprintf(L"== %ls ==\n", heading);

    wchar_t inproc[1024], local[1024];
    inproc[0] = local[0] = 0;

    if (!resolve_clsid_server_paths(g, inproc, ARRAYSIZE(inproc), local, ARRAYSIZE(local))) {
        if (heading) wprintf(L"  (no server registrations)\n");
        g_opt.reg_view = saved;
        return 0;
    }

    *found_any = 1;
    if (inproc[0]) wprintf(L"InprocServer32: %ls\n", inproc);
    if (local[0])  wprintf(L"LocalServer32 : %ls\n", local);

    if (do_scan) {
        if (inproc[0] && cmd_scan(inproc, scanopt, 0) != 0) rc = 1;
        if (local[0] && cmd_scan(local, scanopt, 0) != 0) rc = 1;
    }

    g_opt.reg_view = saved;
    return rc;
}

static int cmd_server(const wchar_t* s, int do_scan, const SCANOPTS* scanopt) {
    unsigned long errors_before = g_runtime_errors;
    GUID g;
    if (!parse_guid_any(s, &g)) {
        fwprintf(stderr, L"quuid: failed to parse CLSID: %ls\n", s);
        return 2;
    }

    int found_any = 0;
    int rc = 0;
    if (g_opt.both_views) {
        if (cmd_server_view(&g, KEY_WOW64_64KEY, L"64-bit view", do_scan, scanopt, &found_any) != 0) rc = 1;
        if (cmd_server_view(&g, KEY_WOW64_32KEY, L"32-bit view", do_scan, scanopt, &found_any) != 0) rc = 1;
    }
    else {
        if (cmd_server_view(&g, g_opt.reg_view, NULL, do_scan, scanopt, &found_any) != 0) rc = 1;
    }

    if (!found_any && !g_opt.both_views) wprintf(L"No server registrations found for CLSID.\n");
    if (finish_command(errors_before) != 0) rc = 1;
    return rc;
}

static int is_flag(const wchar_t* s, const wchar_t* flag) {
    return s && flag && _wcsicmp(s, flag) == 0;
}

typedef enum COMMAND_KIND {
    COMMAND_NONE,
    COMMAND_PARSE,
    COMMAND_FIND,
    COMMAND_SCAN,
    COMMAND_SERVER,
    COMMAND_TLB,
    COMMAND_ENUM
} COMMAND_KIND;

enum CLI_FLAG {
    CLI_VERBOSE      = 1u << 0,
    CLI_WOW32        = 1u << 1,
    CLI_WOW64        = 1u << 2,
    CLI_BOTH_VIEWS   = 1u << 3,
    CLI_ONE_LINE     = 1u << 4,
    CLI_REGISTRY     = 1u << 5,
    CLI_BINARY       = 1u << 6,
    CLI_BINARY_LOOSE = 1u << 7,
    CLI_LOCATE       = 1u << 8,
    CLI_SCAN         = 1u << 9,
    CLI_LIMIT        = 1u << 10,
    CLI_WITH_NAME    = 1u << 11
};

typedef struct CLI_ARGS {
    COMMAND_KIND command;
    const wchar_t* command_text;
    const wchar_t* argument;
    unsigned flags;
    unsigned long limit;
} CLI_ARGS;

static COMMAND_KIND command_kind(const wchar_t* s) {
    if (_wcsicmp(s, L"parse") == 0) return COMMAND_PARSE;
    if (_wcsicmp(s, L"find") == 0) return COMMAND_FIND;
    if (_wcsicmp(s, L"scan") == 0) return COMMAND_SCAN;
    if (_wcsicmp(s, L"server") == 0) return COMMAND_SERVER;
    if (_wcsicmp(s, L"tlb") == 0) return COMMAND_TLB;
    if (_wcsicmp(s, L"enum") == 0) return COMMAND_ENUM;
    return COMMAND_NONE;
}

static int cli_error(const wchar_t* format, const wchar_t* value) {
    fwprintf(stderr, L"quuid: ");
    fwprintf(stderr, format, value);
    fwprintf(stderr, L"\nTry 'quuid --help' for usage.\n");
    return 2;
}

static int option_flag(const wchar_t* s, unsigned* flag) {
    struct OPTION_NAME { const wchar_t* name; unsigned flag; };
    static const struct OPTION_NAME names[] = {
        { L"--verbose", CLI_VERBOSE }, { L"--wow32", CLI_WOW32 },
        { L"--wow64", CLI_WOW64 }, { L"--both-views", CLI_BOTH_VIEWS },
        { L"--one-line", CLI_ONE_LINE }, { L"--registry", CLI_REGISTRY },
        { L"--binary", CLI_BINARY }, { L"--binary-loose", CLI_BINARY_LOOSE },
        { L"--locate", CLI_LOCATE }, { L"--scan", CLI_SCAN },
        { L"--with-name", CLI_WITH_NAME }
    };
    for (size_t i = 0; i < ARRAYSIZE(names); i++) {
        if (is_flag(s, names[i].name)) {
            *flag = names[i].flag;
            return 1;
        }
    }
    return 0;
}

static int parse_cli(int argc, wchar_t** argv, CLI_ARGS* cli) {
    int options = 1;
    ZeroMemory(cli, sizeof(*cli));
    cli->limit = 100;

    // Help is authoritative wherever an option would be recognized.
    for (int i = 1; i < argc; i++) {
        if (is_flag(argv[i], L"--")) break;
        if (is_flag(argv[i], L"--help")) {
            usage();
            return 0;
        }
    }

    for (int i = 1; i < argc; i++) {
        if (options && is_flag(argv[i], L"--")) {
            options = 0;
            continue;
        }

        if (options && wcsncmp(argv[i], L"--", 2) == 0) {
            unsigned flag = 0;
            if (is_flag(argv[i], L"--limit")) {
                if (++i >= argc) return cli_error(L"option requires a value: %ls", L"--limit");
                if (!parse_u32_dec(argv[i], &cli->limit)) return cli_error(L"invalid --limit value: %ls", argv[i]);
                cli->flags |= CLI_LIMIT;
                continue;
            }
            if (_wcsnicmp(argv[i], L"--limit=", 8) == 0) {
                if (!parse_u32_dec(argv[i] + 8, &cli->limit)) return cli_error(L"invalid --limit value: %ls", argv[i] + 8);
                cli->flags |= CLI_LIMIT;
                continue;
            }
            if (!option_flag(argv[i], &flag)) return cli_error(L"unknown option: %ls", argv[i]);
            cli->flags |= flag;
            continue;
        }

        if (!cli->command_text) {
            cli->command_text = argv[i];
            cli->command = command_kind(argv[i]);
        }
        else if (!cli->argument) {
            cli->argument = argv[i];
        }
        else {
            return cli_error(L"unexpected argument: %ls", argv[i]);
        }
    }

    if (!cli->command_text) return cli_error(L"missing command%ls", L"");
    if (cli->command == COMMAND_NONE) return cli_error(L"unknown command: %ls", cli->command_text);
    if (!cli->argument) return cli_error(L"missing command argument for: %ls", cli->command_text);

    if ((cli->flags & CLI_WOW32) && (cli->flags & CLI_WOW64))
        return cli_error(L"conflicting options: %ls", L"--wow32 and --wow64");
    if ((cli->flags & CLI_BOTH_VIEWS) && (cli->flags & (CLI_WOW32 | CLI_WOW64)))
        return cli_error(L"conflicting options: %ls", L"--both-views and a single registry view");

    unsigned allowed = CLI_VERBOSE;
    switch (cli->command) {
    case COMMAND_PARSE:
        allowed |= CLI_ONE_LINE;
        break;
    case COMMAND_FIND:
        allowed |= CLI_WOW32 | CLI_WOW64 | CLI_BOTH_VIEWS;
        break;
    case COMMAND_SCAN:
        allowed |= CLI_WOW32 | CLI_WOW64 | CLI_BOTH_VIEWS | CLI_ONE_LINE |
            CLI_REGISTRY | CLI_BINARY | CLI_BINARY_LOOSE | CLI_LOCATE;
        break;
    case COMMAND_SERVER:
        allowed |= CLI_WOW32 | CLI_WOW64 | CLI_BOTH_VIEWS | CLI_SCAN |
            CLI_ONE_LINE | CLI_REGISTRY | CLI_BINARY | CLI_BINARY_LOOSE | CLI_LOCATE;
        break;
    case COMMAND_TLB:
        break;
    case COMMAND_ENUM:
        allowed |= CLI_WOW32 | CLI_WOW64 | CLI_BOTH_VIEWS | CLI_LIMIT | CLI_WITH_NAME;
        break;
    default:
        break;
    }

    unsigned invalid = cli->flags & ~allowed;
    if (invalid) return cli_error(L"option is not valid for command: %ls", cli->command_text);

    if (cli->command == COMMAND_SERVER && !(cli->flags & CLI_SCAN) &&
        (cli->flags & (CLI_ONE_LINE | CLI_REGISTRY | CLI_BINARY | CLI_BINARY_LOOSE | CLI_LOCATE))) {
        return cli_error(L"server scan options require: %ls", L"--scan");
    }

    if (cli->command == COMMAND_SCAN && !(cli->flags & CLI_REGISTRY) &&
        (cli->flags & (CLI_WOW32 | CLI_WOW64 | CLI_BOTH_VIEWS))) {
        return cli_error(L"scan registry-view options require: %ls", L"--registry");
    }

    if (cli->command == COMMAND_ENUM &&
        _wcsicmp(cli->argument, L"clsid") != 0 && _wcsicmp(cli->argument, L"iid") != 0 &&
        _wcsicmp(cli->argument, L"typelib") != 0 && _wcsicmp(cli->argument, L"appid") != 0) {
        return cli_error(L"unknown enum category: %ls", cli->argument);
    }

    return -1;
}

int wmain(int argc, wchar_t** argv) {
    CLI_ARGS cli;
    int parse_result = parse_cli(argc, argv, &cli);
    if (parse_result >= 0) return parse_result;

    g_opt.verbose = (cli.flags & CLI_VERBOSE) != 0;
    g_opt.both_views = (cli.flags & CLI_BOTH_VIEWS) != 0;
    if (cli.flags & CLI_WOW32) g_opt.reg_view = KEY_WOW64_32KEY;
    if (cli.flags & CLI_WOW64) g_opt.reg_view = KEY_WOW64_64KEY;

    SCANOPTS scanopt;
    ZeroMemory(&scanopt, sizeof(scanopt));
    scanopt.with_registry = (cli.flags & CLI_REGISTRY) != 0;
    scanopt.binary_scan = (cli.flags & (CLI_BINARY | CLI_BINARY_LOOSE)) != 0;
    scanopt.binary_loose = (cli.flags & CLI_BINARY_LOOSE) != 0;
    scanopt.locate = (cli.flags & CLI_LOCATE) != 0;
    scanopt.one_line = (cli.flags & CLI_ONE_LINE) != 0;

    switch (cli.command) {
    case COMMAND_PARSE:
        return cmd_parse(cli.argument, scanopt.one_line);
    case COMMAND_FIND:
        return cmd_find(cli.argument);
    case COMMAND_SCAN:
        return cmd_scan(cli.argument, &scanopt, 1);
    case COMMAND_SERVER:
        return cmd_server(cli.argument, (cli.flags & CLI_SCAN) != 0, &scanopt);
    case COMMAND_TLB:
        return cmd_tlb(cli.argument);
    case COMMAND_ENUM:
        if (g_opt.both_views) {
            int rc = 0;
            g_opt.reg_view = KEY_WOW64_64KEY;
            wprintf(L"== 64-bit view ==\n");
            if (cmd_enum_root(cli.argument, cli.limit, (cli.flags & CLI_WITH_NAME) != 0) != 0) rc = 1;
            g_opt.reg_view = KEY_WOW64_32KEY;
            wprintf(L"== 32-bit view ==\n");
            if (cmd_enum_root(cli.argument, cli.limit, (cli.flags & CLI_WITH_NAME) != 0) != 0) rc = 1;
            return rc;
        }
        return cmd_enum_root(cli.argument, cli.limit, (cli.flags & CLI_WITH_NAME) != 0);
    default:
        return 2;
    }
}
