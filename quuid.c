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
// Global flags (before command):
//   --verbose  (prints Win32 error details for non-fatal failures)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <stdio.h>
#include <wchar.h>

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

// ============================= errors =============================

static void print_last_error(const wchar_t* where) {
    DWORD e = GetLastError();
    wchar_t buf[512];
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, e, 0, buf, ARRAYSIZE(buf), NULL);
    if (!n) {
        wprintf(L"%ls: error %lu\n", where, e);
    }
    else {
        while (n && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n')) buf[--n] = 0;
        wprintf(L"%ls: error %lu: %ls\n", where, e, buf);
    }
}

static void verror(const wchar_t* where) {
    if (g_opt.verbose) print_last_error(where);
}

// ============================= small utils =============================

static int is_hex_w(wchar_t c) {
    return (c >= L'0' && c <= L'9') ||
        (c >= L'a' && c <= L'f') ||
        (c >= L'A' && c <= L'F');
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
static int parse_guid_any(const wchar_t* s, GUID* out) {
    if (!s || !*s) return 0;

    HRESULT hr = CLSIDFromString((LPOLESTR)s, out);
    if (SUCCEEDED(hr)) return 1;

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
            L"%.*ls-%.*ls-%.*ls-%.*ls-%.*ls",
            8, s, 4, s + 8, 4, s + 12, 4, s + 16, 12, s + 20);
        hr = CLSIDFromString((LPOLESTR)tmp, out);
        return SUCCEEDED(hr);
    }

    return 0;
}

static void guid_to_string_braced(const GUID* g, wchar_t* out, size_t cchOut) {
    if (!out || cchOut == 0) return;
    out[0] = 0;
    StringFromGUID2(g, out, (int)cchOut);
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

    wprintf(L"  fields : Data1=0x%08lX Data2=0x%04X Data3=0x%04X Data4=%02X%02X-%02X%02X%02X%02X%02X%02X\n",
        (unsigned long)g->Data1, g->Data2, g->Data3,
        g->Data4[0], g->Data4[1],
        g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);

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
    return s->items && s->used;
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

static LONG reg_open_hkcr(const wchar_t* subkey, HKEY* out) {
    return RegOpenKeyExW(HKEY_CLASSES_ROOT, subkey, 0, reg_sam_read(), out);
}

static int reg_query_string_value(HKEY k, const wchar_t* name_or_null, wchar_t* out, DWORD cchOut, DWORD* outType) {
    if (!out || cchOut == 0) return 0;
    out[0] = 0;
    DWORD type = 0;
    DWORD cb = cchOut * sizeof(wchar_t);
    LONG r = RegQueryValueExW(k, name_or_null, NULL, &type, (BYTE*)out, &cb);
    if (r != ERROR_SUCCESS) return 0;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return 0;
    out[cchOut - 1] = 0;
    if (outType) *outType = type;
    return 1;
}

static int reg_read_default_string_expanded(HKEY k, wchar_t* out, DWORD cchOut) {
    DWORD type = 0;
    wchar_t tmp[2048];
    if (!reg_query_string_value(k, NULL, tmp, ARRAYSIZE(tmp), &type)) return 0;

    if (type == REG_EXPAND_SZ) {
        DWORD n = ExpandEnvironmentStringsW(tmp, out, cchOut);
        if (n == 0 || n > cchOut) {
            // fallback to raw
            wcsncpy_s(out, cchOut, tmp, _TRUNCATE);
            return 1;
        }
        return 1;
    }

    wcsncpy_s(out, cchOut, tmp, _TRUNCATE);
    return 1;
}

static int reg_read_named_string_expanded(HKEY k, const wchar_t* name, wchar_t* out, DWORD cchOut) {
    DWORD type = 0;
    wchar_t tmp[2048];
    if (!reg_query_string_value(k, name, tmp, ARRAYSIZE(tmp), &type)) return 0;

    if (type == REG_EXPAND_SZ) {
        DWORD n = ExpandEnvironmentStringsW(tmp, out, cchOut);
        if (n == 0 || n > cchOut) {
            wcsncpy_s(out, cchOut, tmp, _TRUNCATE);
            return 1;
        }
        return 1;
    }

    wcsncpy_s(out, cchOut, tmp, _TRUNCATE);
    return 1;
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
    if (reg_open_hkcr(path, &k) != ERROR_SUCCESS) { g_opt.reg_view = saved; return; }

    wprintf(L"  [CLSID%s] ", header);
    wchar_t name[512];
    if (reg_read_default_string_expanded(k, name, ARRAYSIZE(name))) wprintf(L"%ls\n", name);
    else wprintf(L"(no name)\n");

    // InprocServer32 / LocalServer32 + ThreadingModel
    const wchar_t* subkeys[] = { L"InprocServer32", L"LocalServer32" };
    for (int i = 0; i < (int)ARRAYSIZE(subkeys); i++) {
        HKEY sk = NULL;
        if (RegOpenKeyExW(k, subkeys[i], 0, reg_sam_read(), &sk) == ERROR_SUCCESS) {
            wchar_t v[1024];
            if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v))) {
                print_key_value_line(subkeys[i], v);
            }
            if (_wcsicmp(subkeys[i], L"InprocServer32") == 0) {
                wchar_t tm[64];
                if (reg_read_named_string_expanded(sk, L"ThreadingModel", tm, ARRAYSIZE(tm))) {
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
        if (RegOpenKeyExW(k, misc[i], 0, reg_sam_read(), &sk) == ERROR_SUCCESS) {
            wchar_t v[1024];
            if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v))) {
                print_key_value_line(misc[i], v);
            }
            RegCloseKey(sk);
        }
    }

    wchar_t appid[256];
    if (reg_read_named_string_expanded(k, L"AppID", appid, ARRAYSIZE(appid))) {
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
    if (reg_open_hkcr(path, &k) != ERROR_SUCCESS) { g_opt.reg_view = saved; return; }

    wprintf(L"  [IID%s] ", header);
    wchar_t name[512];
    if (reg_read_default_string_expanded(k, name, ARRAYSIZE(name))) wprintf(L"%ls\n", name);
    else wprintf(L"(no name)\n");

    HKEY sk = NULL;
    if (RegOpenKeyExW(k, L"ProxyStubClsid32", 0, reg_sam_read(), &sk) == ERROR_SUCCESS) {
        wchar_t v[256];
        if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v))) {
            print_key_value_line(L"ProxyStubClsid32", v);
        }
        RegCloseKey(sk);
    }

    wchar_t typelib[256];
    if (reg_read_named_string_expanded(k, L"TypeLib", typelib, ARRAYSIZE(typelib))) {
        print_key_value_line(L"TypeLib", typelib);
    }
    wchar_t num[256];
    if (reg_read_named_string_expanded(k, L"NumMethods", num, ARRAYSIZE(num))) {
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
    if (reg_open_hkcr(path, &k) != ERROR_SUCCESS) { g_opt.reg_view = saved; return; }

    wprintf(L"  [TypeLib%s] ", header);
    wchar_t name[512];
    if (reg_read_default_string_expanded(k, name, ARRAYSIZE(name))) wprintf(L"%ls\n", name);
    else wprintf(L"(no name)\n");

    // Enumerate versions and show win32/win64 paths when present:
    DWORD idx = 0;
    wchar_t ver[256];
    DWORD cchVer = ARRAYSIZE(ver);

    while (RegEnumKeyExW(k, idx++, ver, &cchVer, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        wprintf(L"    version            %ls\n", ver);

        wchar_t sub0[512];
        swprintf(sub0, ARRAYSIZE(sub0), L"%ls\\0\\win32", ver);
        HKEY vk = NULL;
        if (RegOpenKeyExW(k, sub0, 0, reg_sam_read(), &vk) == ERROR_SUCCESS) {
            wchar_t pth[1024];
            if (reg_read_default_string_expanded(vk, pth, ARRAYSIZE(pth))) {
                print_key_value_line(L"win32", pth);
            }
            RegCloseKey(vk);
        }

        swprintf(sub0, ARRAYSIZE(sub0), L"%ls\\0\\win64", ver);
        if (RegOpenKeyExW(k, sub0, 0, reg_sam_read(), &vk) == ERROR_SUCCESS) {
            wchar_t pth[1024];
            if (reg_read_default_string_expanded(vk, pth, ARRAYSIZE(pth))) {
                print_key_value_line(L"win64", pth);
            }
            RegCloseKey(vk);
        }

        cchVer = ARRAYSIZE(ver);
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
    if (reg_open_hkcr(path, &k) != ERROR_SUCCESS) { g_opt.reg_view = saved; return; }

    wprintf(L"  [AppID%s] ", header);
    wchar_t name[512];
    if (reg_read_default_string_expanded(k, name, ARRAYSIZE(name))) wprintf(L"%ls\n", name);
    else wprintf(L"(no name)\n");

    wchar_t v[1024];
    if (reg_read_named_string_expanded(k, L"LocalService", v, ARRAYSIZE(v))) print_key_value_line(L"LocalService", v);
    if (reg_read_named_string_expanded(k, L"ServiceParameters", v, ARRAYSIZE(v))) print_key_value_line(L"ServiceParameters", v);
    if (reg_read_named_string_expanded(k, L"RunAs", v, ARRAYSIZE(v))) print_key_value_line(L"RunAs", v);
    if (reg_read_named_string_expanded(k, L"DllSurrogate", v, ARRAYSIZE(v))) print_key_value_line(L"DllSurrogate", v);

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
    if (!hit && reg_open_hkcr(p1, &k) == ERROR_SUCCESS) { hit = 1; RegCloseKey(k); }
    if (!hit && reg_open_hkcr(p2, &k) == ERROR_SUCCESS) { hit = 1; RegCloseKey(k); }
    if (!hit && reg_open_hkcr(p3, &k) == ERROR_SUCCESS) { hit = 1; RegCloseKey(k); }
    if (!hit && reg_open_hkcr(p4, &k) == ERROR_SUCCESS) { hit = 1; RegCloseKey(k); }

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
static int extract_primary_module_path(const wchar_t* cmdline, wchar_t* out, DWORD cchOut) {
    if (!cmdline || !*cmdline) return 0;
    while (*cmdline == L' ' || *cmdline == L'\t') cmdline++;

    if (*cmdline == L'"') {
        cmdline++;
        const wchar_t* end = wcschr(cmdline, L'"');
        if (!end) return 0;
        size_t n = (size_t)(end - cmdline);
        if (n + 1 > cchOut) n = cchOut - 1;
        wmemcpy(out, cmdline, n);
        out[n] = 0;
        return 1;
    }
    else {
        const wchar_t* end = cmdline;
        while (*end && *end != L' ' && *end != L'\t') end++;
        size_t n = (size_t)(end - cmdline);
        if (n + 1 > cchOut) n = cchOut - 1;
        wmemcpy(out, cmdline, n);
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
    if (reg_open_hkcr(path, &k) != ERROR_SUCCESS) return 0;

    int any = 0;

    HKEY sk = NULL;
    if (outInproc && RegOpenKeyExW(k, L"InprocServer32", 0, reg_sam_read(), &sk) == ERROR_SUCCESS) {
        wchar_t v[1024];
        if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v))) {
            wchar_t mod[1024];
            if (extract_primary_module_path(v, mod, ARRAYSIZE(mod))) {
                wcsncpy_s(outInproc, cchInproc, mod, _TRUNCATE);
                any = 1;
            }
        }
        RegCloseKey(sk);
    }

    if (outLocal && RegOpenKeyExW(k, L"LocalServer32", 0, reg_sam_read(), &sk) == ERROR_SUCCESS) {
        wchar_t v[1024];
        if (reg_read_default_string_expanded(sk, v, ARRAYSIZE(v))) {
            wchar_t mod[1024];
            if (extract_primary_module_path(v, mod, ARRAYSIZE(mod))) {
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

static void join_path(wchar_t* out, size_t cchOut, const wchar_t* a, const wchar_t* b) {
    size_t na = wcslen(a);
    int needSlash = (na > 0 && (a[na - 1] != L'\\' && a[na - 1] != L'/'));
    if (needSlash) swprintf(out, cchOut, L"%ls\\%ls", a, b);
    else swprintf(out, cchOut, L"%ls%ls", a, b);
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
    if (h == INVALID_HANDLE_VALUE) { verror(L"CreateFileW"); return 0; }

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        verror(L"GetFileSizeEx");
        CloseHandle(h);
        return 0;
    }

    st->files_scanned++;
    if (sz.QuadPart > 0) st->bytes_scanned += (unsigned long long)sz.QuadPart;

    // 4 MiB chunks with 64-byte overlap (enough for "{...}" and safety)
    const DWORD CHUNK = 4u * 1024u * 1024u;
    const DWORD OVERLAP = 64u;

    unsigned char* buf = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, CHUNK + OVERLAP);
    if (!buf) { CloseHandle(h); return 0; }

    DWORD keep = 0;
    unsigned long long base_off = 0;

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf + keep, CHUNK, &got, NULL)) {
            verror(L"ReadFile");
            break;
        }
        if (got == 0) break;

        DWORD avail = keep + got;

        // ASCII scan
        for (DWORD i = 0; i + 36 <= avail; i++) {
            unsigned char c = buf[i];
            if (!(c == '{' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                continue;

            GUID g;
            size_t consumed = 0;
            if (match_guid_ascii_at(buf + i, avail - i, &consumed, &g)) {
                guidset_add(set, &g);
                st->ascii_hits++;
                if (opt->locate) locate_hit(path, base_off + i, L"ascii", &g);
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
                guidset_add(set, &g);
                st->bin_hits++;
                if (opt->locate) locate_hit(path, base_off + i, opt->binary_loose ? L"bin-loose" : L"bin", &g);
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

static void scan_path_recursive(const wchar_t* path, GUIDSET* set, SCANSTATS* st, const SCANOPTS* opt) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) { verror(L"GetFileAttributesW"); return; }

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        wchar_t pat[32768];
        swprintf(pat, ARRAYSIZE(pat), L"%ls\\*", path);

        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) { verror(L"FindFirstFileW"); return; }

        do {
            if (is_dot_or_dotdot(fd.cFileName)) continue;

            // Skip reparse points by default (avoid junction/symlink loops)
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                continue;
            }

            wchar_t child[32768];
            join_path(child, ARRAYSIZE(child), path, fd.cFileName);

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                scan_path_recursive(child, set, st, opt);
            }
            else {
                scan_stream_for_guids(child, set, st, opt);
            }
        } while (FindNextFileW(f, &fd));

        FindClose(f);
    }
    else {
        scan_stream_for_guids(path, set, st, opt);
    }
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
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        wprintf(L"CoInitializeEx failed: 0x%08lX\n", (unsigned long)hr);
        return 1;
    }

    ITypeLib* tlb = NULL;
    hr = LoadTypeLibEx(file, REGKIND_NONE, &tlb);
    if (FAILED(hr) || !tlb) {
        wprintf(L"LoadTypeLibEx failed: 0x%08lX\n", (unsigned long)hr);
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

    UINT count = tlb->lpVtbl->GetTypeInfoCount(tlb);
    wprintf(L"  TYPES  : %u\n", (unsigned)count);

    for (UINT i = 0; i < count; i++) {
        ITypeInfo* ti = NULL;
        if (FAILED(tlb->lpVtbl->GetTypeInfo(tlb, i, &ti)) || !ti) continue;

        TYPEATTR* ta = NULL;
        if (SUCCEEDED(ti->lpVtbl->GetTypeAttr(ti, &ta)) && ta) {
            BSTR bname = NULL;
            ti->lpVtbl->GetDocumentation(ti, MEMBERID_NIL, &bname, NULL, NULL, NULL);

            wchar_t gs[64];
            guid_to_string_braced(&ta->guid, gs, ARRAYSIZE(gs));
            wprintf(L"  %ls  %-10ls  %ls\n",
                gs, typekind_name(ta->typekind),
                bname ? (const wchar_t*)bname : L"(noname)");

            if (bname) SysFreeString(bname);
            ti->lpVtbl->ReleaseTypeAttr(ti, ta);
        }

        ti->lpVtbl->Release(ti);
    }

    tlb->lpVtbl->Release(tlb);
    CoUninitialize();
    return 0;
}

// ============================= commands =============================

static void usage(void) {
    wprintf(L"quuid — GUID/COM discovery CLI\n\n");
    wprintf(L"Global flags:\n");
    wprintf(L"  --verbose\n\n");
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
        v = v * 10 + (unsigned long)(*p - L'0');
    }
    *out = v;
    return 1;
}

static int cmd_parse(const wchar_t* s, int one_line) {
    GUID g;
    if (!parse_guid_any(s, &g)) {
        wprintf(L"Failed to parse GUID: %ls\n", s);
        return 1;
    }
    print_guid_forms(&g, one_line);
    return 0;
}

static int cmd_find(const wchar_t* s) {
    GUID g;
    if (!parse_guid_any(s, &g)) {
        wprintf(L"Failed to parse GUID: %ls\n", s);
        return 1;
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
        return 0;
    }

    query_all_categories(&g);
    return 0;
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

static int cmd_scan(const wchar_t* path, const SCANOPTS* opt) {
    GUIDSET set;
    if (!guidset_init(&set, 256)) {
        wprintf(L"Out of memory.\n");
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
    return 0;
}

static int cmd_enum_root(const wchar_t* which, unsigned long limit, int with_name) {
    const wchar_t* root = NULL;
    const wchar_t* label = NULL;

    if (_wcsicmp(which, L"clsid") == 0) { root = L"CLSID"; label = L"CLSID"; }
    else if (_wcsicmp(which, L"iid") == 0) { root = L"Interface"; label = L"IID"; }
    else if (_wcsicmp(which, L"typelib") == 0) { root = L"TypeLib"; label = L"TypeLib"; }
    else if (_wcsicmp(which, L"appid") == 0) { root = L"AppID"; label = L"AppID"; }
    else {
        wprintf(L"Unknown enum category: %ls\n", which);
        return 1;
    }

    HKEY k = NULL;
    if (reg_open_hkcr(root, &k) != ERROR_SUCCESS) {
        wprintf(L"Failed to open HKCR\\%ls\n", root);
        return 1;
    }

    DWORD idx = 0;
    wchar_t sub[256];
    DWORD cchSub = ARRAYSIZE(sub);

    unsigned long printed = 0;
    while (RegEnumKeyExW(k, idx++, sub, &cchSub, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        if (!with_name) {
            wprintf(L"[%ls] %ls\n", label, sub);
        }
        else {
            // open and read default
            HKEY sk = NULL;
            if (RegOpenKeyExW(k, sub, 0, reg_sam_read(), &sk) == ERROR_SUCCESS) {
                wchar_t name[512];
                if (reg_read_default_string_expanded(sk, name, ARRAYSIZE(name))) {
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
        cchSub = ARRAYSIZE(sub);
    }

    RegCloseKey(k);
    return 0;
}

static int cmd_server(const wchar_t* s, int do_scan, const SCANOPTS* scanopt) {
    GUID g;
    if (!parse_guid_any(s, &g)) {
        wprintf(L"Failed to parse CLSID: %ls\n", s);
        return 1;
    }

    wchar_t inproc[1024], local[1024];
    inproc[0] = local[0] = 0;

    if (!resolve_clsid_server_paths(&g, inproc, ARRAYSIZE(inproc), local, ARRAYSIZE(local))) {
        wprintf(L"No server registrations found for CLSID.\n");
        return 0;
    }

    if (inproc[0]) wprintf(L"InprocServer32: %ls\n", inproc);
    if (local[0])  wprintf(L"LocalServer32 : %ls\n", local);

    if (do_scan) {
        if (inproc[0]) cmd_scan(inproc, scanopt);
        if (local[0])  cmd_scan(local, scanopt);
    }
    return 0;
}

static int is_flag(const wchar_t* s, const wchar_t* flag) {
    return s && flag && _wcsicmp(s, flag) == 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 1; }

    // Global flags (must come before command)
    int argi = 1;
    while (argi < argc && wcsncmp(argv[argi], L"--", 2) == 0) {
        if (is_flag(argv[argi], L"--verbose")) {
            g_opt.verbose = 1;
        }
        else if (is_flag(argv[argi], L"--wow32")) {
            g_opt.reg_view = KEY_WOW64_32KEY;
        }
        else if (is_flag(argv[argi], L"--wow64")) {
            g_opt.reg_view = KEY_WOW64_64KEY;
        }
        else if (is_flag(argv[argi], L"--help")) {
            usage();
            return 0;
        }
        else {
            // unknown global flag; stop (treat as command)
            break;
        }
        argi++;
    }

    if (argi >= argc) { usage(); return 1; }

    const wchar_t* cmd = argv[argi++];

    // parse
    if (_wcsicmp(cmd, L"parse") == 0) {
        if (argi >= argc) { usage(); return 1; }
        int one_line = 0;
        const wchar_t* guid = argv[argi++];
        while (argi < argc) {
            if (is_flag(argv[argi], L"--one-line")) one_line = 1;
            argi++;
        }
        return cmd_parse(guid, one_line);
    }

    // find
    if (_wcsicmp(cmd, L"find") == 0) {
        if (argi >= argc) { usage(); return 1; }
        const wchar_t* guid = argv[argi++];

        // per-command flags
        while (argi < argc) {
            if (is_flag(argv[argi], L"--wow32")) g_opt.reg_view = KEY_WOW64_32KEY;
            else if (is_flag(argv[argi], L"--wow64")) g_opt.reg_view = KEY_WOW64_64KEY;
            else if (is_flag(argv[argi], L"--both-views")) g_opt.both_views = 1;
            argi++;
        }

        return cmd_find(guid);
    }

    // scan
    if (_wcsicmp(cmd, L"scan") == 0) {
        if (argi >= argc) { usage(); return 1; }
        const wchar_t* path = argv[argi++];

        SCANOPTS opt;
        ZeroMemory(&opt, sizeof(opt));

        while (argi < argc) {
            if (is_flag(argv[argi], L"--registry")) opt.with_registry = 1;
            else if (is_flag(argv[argi], L"--both-views")) g_opt.both_views = 1;
            else if (is_flag(argv[argi], L"--binary")) opt.binary_scan = 1;
            else if (is_flag(argv[argi], L"--binary-loose")) { opt.binary_scan = 1; opt.binary_loose = 1; }
            else if (is_flag(argv[argi], L"--locate")) opt.locate = 1;
            else if (is_flag(argv[argi], L"--one-line")) opt.one_line = 1;
            argi++;
        }

        return cmd_scan(path, &opt);
    }

    // server
    if (_wcsicmp(cmd, L"server") == 0) {
        if (argi >= argc) { usage(); return 1; }
        const wchar_t* clsid = argv[argi++];

        int do_scan = 0;
        SCANOPTS opt;
        ZeroMemory(&opt, sizeof(opt));

        while (argi < argc) {
            if (is_flag(argv[argi], L"--scan")) do_scan = 1;
            else if (is_flag(argv[argi], L"--registry")) opt.with_registry = 1;
            else if (is_flag(argv[argi], L"--both-views")) g_opt.both_views = 1;
            else if (is_flag(argv[argi], L"--binary")) opt.binary_scan = 1;
            else if (is_flag(argv[argi], L"--binary-loose")) { opt.binary_scan = 1; opt.binary_loose = 1; }
            else if (is_flag(argv[argi], L"--locate")) opt.locate = 1;
            else if (is_flag(argv[argi], L"--one-line")) opt.one_line = 1;
            argi++;
        }

        return cmd_server(clsid, do_scan, &opt);
    }

    // tlb
    if (_wcsicmp(cmd, L"tlb") == 0) {
        if (argi >= argc) { usage(); return 1; }
        return cmd_tlb(argv[argi]);
    }

    // enum
    if (_wcsicmp(cmd, L"enum") == 0) {
        if (argi >= argc) { usage(); return 1; }
        const wchar_t* which = argv[argi++];

        unsigned long limit = 100;
        int with_name = 0;

        while (argi < argc) {
            if (is_flag(argv[argi], L"--limit") && (argi + 1 < argc)) {
                unsigned long v = 0;
                if (parse_u32_dec(argv[argi + 1], &v)) limit = v;
                argi += 2;
                continue;
            }
            else if (is_flag(argv[argi], L"--with-name")) {
                with_name = 1;
            }
            else if (is_flag(argv[argi], L"--both-views")) {
                g_opt.both_views = 1; // for completeness, though enum uses current view
            }
            else if (is_flag(argv[argi], L"--wow32")) {
                g_opt.reg_view = KEY_WOW64_32KEY;
            }
            else if (is_flag(argv[argi], L"--wow64")) {
                g_opt.reg_view = KEY_WOW64_64KEY;
            }
            argi++;
        }

        // If both_views is requested for enum, do two passes.
        if (g_opt.both_views) {
            DWORD saved = g_opt.reg_view;
            g_opt.reg_view = KEY_WOW64_64KEY;
            wprintf(L"== 64-bit view ==\n");
            cmd_enum_root(which, limit, with_name);
            g_opt.reg_view = KEY_WOW64_32KEY;
            wprintf(L"== 32-bit view ==\n");
            cmd_enum_root(which, limit, with_name);
            g_opt.reg_view = saved;
            return 0;
        }

        return cmd_enum_root(which, limit, with_name);
    }

    usage();
    return 1;
}
