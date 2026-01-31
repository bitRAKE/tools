/*
Engineer a UUID/GUID/CLSID/IID discovery CLI program (quuid.exe).

## quuid.exe goals

* Parse GUID text in multiple common formats and print canonical/struct/memory forms.
* Query the registry for COM-related registrations (CLSID / Interface / TypeLib / AppID).
* Scan files or directory trees for GUID-looking ASCII patterns and optionally cross-reference the registry.
* Load and enumerate GUIDs from a type library file via `LoadTypeLibEx`.

## Build

Single-file C program; link against OLE + registry libraries.

MSVC (x64 Developer Command Prompt):

```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE quuid.c ole32.lib oleaut32.lib advapi32.lib
```
or

```bat
clang -Wall -O3 -march=native -DUNICODE -D_UNICODE quuid.c -lole32.lib -loleaut32.lib -ladvapi32.lib
```

## Usage

```text
quuid parse <guid>
quuid find  <guid>
quuid scan  <path> [--registry]
quuid tlb   <file.tlb|.dll|.ocx> 
quuid enum  clsid|iid|typelib|appid [--limit N]
```

Examples:

```bat
quuid parse 6F9619FF-8B86-D011-B42D-00C04FC964FF
quuid find  {00021401-0000-0000-C000-000000000046}
quuid scan  C:\Windows\System32 --registry
quuid tlb   C:\Windows\System32\stdole2.tlb
quuid enum  clsid --limit 50
```

## Notes for extension

* Add “binary GUID” scanning (raw 16-byte sequences) with heuristic validation (e.g., version/variant bits for RFC4122) and a “confirm by nearby ASCII/registry” strategy.
* Add a mode to resolve CLSID → server path and then scan that server binary for more GUIDs.
* Add a mode to enumerate `HKLM\Software\Microsoft\Ole` / `WOW6432Node` deltas to compare 32-bit vs 64-bit registrations.
* Add output formats suitable for fasmg/fasm2 macro ingestion (e.g., `define GUID.xxx ...`, `db`, `dq` layouts).

Mentions of COM registry structures and the type library loader rely on standard Microsoft component models and APIs.

*/

// quuid.c - GUID/COM registry discovery + file scanner + typelib enumerator
// Build: cl /nologo /W4 /O2 /DUNICODE /D_UNICODE quuid.c ole32.lib oleaut32.lib advapi32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <stdio.h>
#include <wchar.h>

#ifndef ARRAYSIZE
#define ARRAYSIZE(A) (sizeof(A)/sizeof((A)[0]))
#endif

// ----------------------------- utilities -----------------------------

void print_last_error(const wchar_t* where) {
    DWORD e = GetLastError();
    wchar_t buf[512];
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, e, 0, buf, ARRAYSIZE(buf), NULL);
    if (!n) {
        wprintf(L"%ls: error %lu\n", where, e);
    } else {
        // Trim trailing CRLF
        while (n && (buf[n-1] == L'\r' || buf[n-1] == L'\n')) buf[--n] = 0;
        wprintf(L"%ls: error %lu: %ls\n", where, e, buf);
    }
}

int is_hex_w(wchar_t c) {
    return (c >= L'0' && c <= L'9') ||
           (c >= L'a' && c <= L'f') ||
           (c >= L'A' && c <= L'F');
}

int hex_val8(char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
    return -1;
}

int parse_u32_hex_n(const char* s, int n, unsigned long* out) {
    unsigned long v = 0;
    for (int i = 0; i < n; i++) {
        int hv = hex_val8(s[i]);
        if (hv < 0) return 0;
        v = (v << 4) | (unsigned long)hv;
    }
    *out = v;
    return 1;
}

int parse_u16_hex_n(const char* s, int n, unsigned short* out) {
    unsigned long v = 0;
    if (!parse_u32_hex_n(s, n, &v)) return 0;
    *out = (unsigned short)v;
    return 1;
}

int parse_guid_ascii36(const unsigned char* p, GUID* g) {
    // format: 8-4-4-4-12 (36 chars), no braces
    // indices: 8,13,18,23 are '-'
    for (int i = 0; i < 36; i++) {
        unsigned char c = p[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return 0;
        } else {
            if (!( (c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F') ))
                return 0;
        }
    }

    unsigned long d1 = 0;
    unsigned short d2 = 0, d3 = 0;
    unsigned long tmp = 0;

    if (!parse_u32_hex_n((const char*)p + 0, 8, &d1)) return 0;
    if (!parse_u16_hex_n((const char*)p + 9, 4, &d2)) return 0;
    if (!parse_u16_hex_n((const char*)p + 14, 4, &d3)) return 0;

    // Data4: 2 bytes (positions 19-22 excluding dash at 23?) actually group4 is 4 hex at 19-22
    unsigned short d4_0 = 0;
    if (!parse_u16_hex_n((const char*)p + 19, 4, &d4_0)) return 0;

    // last 12 hex => 6 bytes
    unsigned char d4_rest[6];
    for (int i = 0; i < 6; i++) {
        int hi = hex_val8((char)p[24 + i*2]);
        int lo = hex_val8((char)p[24 + i*2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        d4_rest[i] = (unsigned char)((hi << 4) | lo);
    }

    g->Data1 = (DWORD)d1;
    g->Data2 = (WORD)d2;
    g->Data3 = (WORD)d3;
    g->Data4[0] = (unsigned char)((d4_0 >> 8) & 0xFF);
    g->Data4[1] = (unsigned char)(d4_0 & 0xFF);
    for (int i = 0; i < 6; i++) g->Data4[2+i] = d4_rest[i];

    (void)tmp;
    return 1;
}

int match_guid_at(const unsigned char* p, size_t remaining, size_t* consumed, GUID* out) {
    // Detect {GUID} (38) or GUID (36), ASCII only.
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

// Accept:
// - {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}
// - xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
// - xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx (32 hex)
int parse_guid_any(const wchar_t* s, GUID* out) {
    if (!s || !*s) return 0;

    // Try CLSIDFromString first (tolerates braces and often tolerates no braces).
    HRESULT hr = CLSIDFromString((LPOLESTR)s, out);
    if (SUCCEEDED(hr)) return 1;

    // Try adding braces if missing.
    size_t n = wcslen(s);
    if (n == 36) {
        wchar_t tmp[64];
        if (ARRAYSIZE(tmp) >= 40) {
            tmp[0] = L'{';
            wmemcpy(tmp + 1, s, 36);
            tmp[37] = L'}';
            tmp[38] = 0;
            hr = CLSIDFromString((LPOLESTR)tmp, out);
            if (SUCCEEDED(hr)) return 1;
        }
    }

    // Try 32-hex form -> dashed.
    if (n == 32) {
        for (size_t i = 0; i < 32; i++) if (!is_hex_w(s[i])) return 0;
        wchar_t tmp[64];
        // 8-4-4-4-12
        swprintf(tmp, ARRAYSIZE(tmp),
            L"%.*ls-%.*ls-%.*ls-%.*ls-%.*ls",
            8, s, 4, s + 8, 4, s + 12, 4, s + 16, 12, s + 20);
        hr = CLSIDFromString((LPOLESTR)tmp, out);
        return SUCCEEDED(hr);
    }

    return 0;
}

void guid_to_string_braced(const GUID* g, wchar_t* out, size_t cchOut) {
    if (!out || cchOut == 0) return;
    out[0] = 0;
    StringFromGUID2(g, out, (int)cchOut);
}

void print_guid_forms(const GUID* g) {
    wchar_t s[64];
    guid_to_string_braced(g, s, ARRAYSIZE(s));

    // s from StringFromGUID2 is "{...}"
    wchar_t plain[64];
    wcsncpy_s(plain, ARRAYSIZE(plain), s, _TRUNCATE);
    size_t n = wcslen(plain);
    if (n >= 2 && plain[0] == L'{' && plain[n-1] == L'}') {
        plain[n-1] = 0;
        // shift left to remove '{'
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

    // memory layout as stored in GUID struct (little endian for Data1/2/3)
    unsigned char bytes[16];
    memcpy(bytes, g, 16);
    wprintf(L"  db     : ");
    for (int i = 0; i < 16; i++) {
        wprintf(L"0x%02X%s", bytes[i], (i == 15) ? L"" : L",");
    }
    wprintf(L"\n");
}

// ----------------------------- GUID set -----------------------------

typedef struct GUIDSET {
    GUID* items;
    unsigned char* used;
    size_t cap;
    size_t len;
} GUIDSET;

unsigned long long fnv1a64(const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= (unsigned long long)p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int guid_equal(const GUID* a, const GUID* b) {
    return memcmp(a, b, sizeof(GUID)) == 0;
}

int guidset_init(GUIDSET* s, size_t initialCapPow2) {
    if (!s) return 0;
    s->cap = (initialCapPow2 < 64) ? 64 : initialCapPow2;
    // cap must be power of two
    size_t cap = 1;
    while (cap < s->cap) cap <<= 1;
    s->cap = cap;
    s->len = 0;
    s->items = (GUID*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, s->cap * sizeof(GUID));
    s->used  = (unsigned char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, s->cap);
    return s->items && s->used;
}

void guidset_free(GUIDSET* s) {
    if (!s) return;
    if (s->items) HeapFree(GetProcessHeap(), 0, s->items);
    if (s->used)  HeapFree(GetProcessHeap(), 0, s->used);
    ZeroMemory(s, sizeof(*s));
}

int guidset_rehash(GUIDSET* s, size_t newCap) {
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

int guidset_add(GUIDSET* s, const GUID* g) {
    if (!s || !g) return 0;

    // Grow at ~70% load.
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
        if (guid_equal(&s->items[pos], g)) return 1; // already present
        pos = (pos + 1) & mask;
    }
}

void guidset_foreach(const GUIDSET* s, void (*fn)(const GUID*, void*), void* ctx) {
    if (!s || !fn) return;
    for (size_t i = 0; i < s->cap; i++) {
        if (s->used[i]) fn(&s->items[i], ctx);
    }
}

// ----------------------------- registry -----------------------------

int reg_read_default_string(HKEY k, wchar_t* out, DWORD cchOut) {
    if (!out || cchOut == 0) return 0;
    out[0] = 0;
    DWORD type = 0;
    DWORD cb = cchOut * sizeof(wchar_t);
    LONG r = RegQueryValueExW(k, NULL, NULL, &type, (BYTE*)out, &cb);
    if (r != ERROR_SUCCESS) return 0;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return 0;
    out[cchOut - 1] = 0;
    return 1;
}

int reg_read_named_string(HKEY k, const wchar_t* name, wchar_t* out, DWORD cchOut) {
    if (!out || cchOut == 0) return 0;
    out[0] = 0;
    DWORD type = 0;
    DWORD cb = cchOut * sizeof(wchar_t);
    LONG r = RegQueryValueExW(k, name, NULL, &type, (BYTE*)out, &cb);
    if (r != ERROR_SUCCESS) return 0;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return 0;
    out[cchOut - 1] = 0;
    return 1;
}

void print_key_value_line(const wchar_t* label, const wchar_t* val) {
    if (val && *val) wprintf(L"    %-18ls %ls\n", label, val);
}

void query_clsid(const GUID* g) {
    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t path[128];
    swprintf(path, ARRAYSIZE(path), L"CLSID\\%ls", gs);

    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;

    wchar_t name[512];
    if (reg_read_default_string(k, name, ARRAYSIZE(name))) {
        wprintf(L"  [CLSID] %ls\n", name);
    } else {
        wprintf(L"  [CLSID]\n");
    }

    // InprocServer32 / LocalServer32 / ProgID / VIProgID / TreatAs / AppID
    const wchar_t* subkeys[] = { L"InprocServer32", L"LocalServer32", L"ProgID", L"VersionIndependentProgID", L"TreatAs" };
    for (int i = 0; i < (int)ARRAYSIZE(subkeys); i++) {
        HKEY sk = NULL;
        if (RegOpenKeyExW(k, subkeys[i], 0, KEY_READ, &sk) == ERROR_SUCCESS) {
            wchar_t v[1024];
            if (reg_read_default_string(sk, v, ARRAYSIZE(v))) {
                wchar_t label[64];
                wcsncpy_s(label, ARRAYSIZE(label), subkeys[i], _TRUNCATE);
                print_key_value_line(label, v);
            }
            RegCloseKey(sk);
        }
    }

    wchar_t appid[256];
    if (reg_read_named_string(k, L"AppID", appid, ARRAYSIZE(appid))) {
        print_key_value_line(L"AppID", appid);
    }

    RegCloseKey(k);
}

void query_iid(const GUID* g) {
    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t path[128];
    swprintf(path, ARRAYSIZE(path), L"Interface\\%ls", gs);

    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;

    wchar_t name[512];
    if (reg_read_default_string(k, name, ARRAYSIZE(name))) {
        wprintf(L"  [IID] %ls\n", name);
    } else {
        wprintf(L"  [IID]\n");
    }

    HKEY sk = NULL;
    if (RegOpenKeyExW(k, L"ProxyStubClsid32", 0, KEY_READ, &sk) == ERROR_SUCCESS) {
        wchar_t v[256];
        if (reg_read_default_string(sk, v, ARRAYSIZE(v))) {
            print_key_value_line(L"ProxyStubClsid32", v);
        }
        RegCloseKey(sk);
    }

    wchar_t typelib[256];
    if (reg_read_named_string(k, L"TypeLib", typelib, ARRAYSIZE(typelib))) {
        print_key_value_line(L"TypeLib", typelib);
    }
    wchar_t num[256];
    if (reg_read_named_string(k, L"NumMethods", num, ARRAYSIZE(num))) {
        print_key_value_line(L"NumMethods", num);
    }

    RegCloseKey(k);
}

void query_typelib(const GUID* g) {
    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t path[128];
    swprintf(path, ARRAYSIZE(path), L"TypeLib\\%ls", gs);

    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;

    wchar_t name[512];
    if (reg_read_default_string(k, name, ARRAYSIZE(name))) {
        wprintf(L"  [TypeLib] %ls\n", name);
    } else {
        wprintf(L"  [TypeLib]\n");
    }

    // enumerate versions (subkeys)
    DWORD idx = 0;
    wchar_t sub[256];
    DWORD cchSub = ARRAYSIZE(sub);
    while (RegEnumKeyExW(k, idx++, sub, &cchSub, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        wprintf(L"    version            %ls\n", sub);
        cchSub = ARRAYSIZE(sub);
    }

    RegCloseKey(k);
}

void query_appid(const GUID* g) {
    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t path[128];
    swprintf(path, ARRAYSIZE(path), L"AppID\\%ls", gs);

    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return;

    wchar_t name[512];
    if (reg_read_default_string(k, name, ARRAYSIZE(name))) {
        wprintf(L"  [AppID] %ls\n", name);
    } else {
        wprintf(L"  [AppID]\n");
    }

    wchar_t svc[256];
    if (reg_read_named_string(k, L"LocalService", svc, ARRAYSIZE(svc))) {
        print_key_value_line(L"LocalService", svc);
    }
    wchar_t runas[256];
    if (reg_read_named_string(k, L"RunAs", runas, ARRAYSIZE(runas))) {
        print_key_value_line(L"RunAs", runas);
    }

    RegCloseKey(k);
}

void query_all_categories(const GUID* g) {
    // Print only categories that exist.
    query_clsid(g);
    query_iid(g);
    query_typelib(g);
    query_appid(g);
}

int key_exists_under_hkcr(const wchar_t* relPath) {
    HKEY k = NULL;
    LONG r = RegOpenKeyExW(HKEY_CLASSES_ROOT, relPath, 0, KEY_READ, &k);
    if (r == ERROR_SUCCESS) {
        RegCloseKey(k);
        return 1;
    }
    return 0;
}

int any_registry_hit(const GUID* g) {
    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));

    wchar_t p1[128], p2[128], p3[128], p4[128];
    swprintf(p1, ARRAYSIZE(p1), L"CLSID\\%ls", gs);
    swprintf(p2, ARRAYSIZE(p2), L"Interface\\%ls", gs);
    swprintf(p3, ARRAYSIZE(p3), L"TypeLib\\%ls", gs);
    swprintf(p4, ARRAYSIZE(p4), L"AppID\\%ls", gs);

    return key_exists_under_hkcr(p1) || key_exists_under_hkcr(p2) || key_exists_under_hkcr(p3) || key_exists_under_hkcr(p4);
}

// ----------------------------- scanning -----------------------------

typedef struct SCANSTATS {
    unsigned long long files_scanned;
    unsigned long long bytes_scanned;
    unsigned long long matches;
} SCANSTATS;

int scan_file_for_guids(const wchar_t* path, GUIDSET* set, SCANSTATS* st) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        return 0;
    }
    if (sz.QuadPart <= 0) {
        CloseHandle(h);
        return 1;
    }

    // Optional guard (avoid pathological sizes)
    // if (sz.QuadPart > (LONGLONG)(1024ULL*1024ULL*1024ULL)) { CloseHandle(h); return 1; }

    HANDLE map = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!map) {
        CloseHandle(h);
        return 0;
    }

    unsigned char* base = (unsigned char*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!base) {
        CloseHandle(map);
        CloseHandle(h);
        return 0;
    }

    size_t n = (size_t)sz.QuadPart; // on x64, size_t is large enough for typical files
    st->files_scanned++;
    st->bytes_scanned += (unsigned long long)n;

    for (size_t i = 0; i + 36 <= n; i++) {
        // Quick filter: likely starts with '{' or hex
        unsigned char c = base[i];
        if (!(c == '{' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            continue;

        GUID g;
        size_t consumed = 0;
        if (match_guid_at(base + i, n - i, &consumed, &g)) {
            guidset_add(set, &g);
            st->matches++;
            i += consumed ? (consumed - 1) : 0;
        }
    }

    UnmapViewOfFile(base);
    CloseHandle(map);
    CloseHandle(h);
    return 1;
}

int is_dot_or_dotdot(const wchar_t* name) {
    return (name[0] == L'.' && name[1] == 0) || (name[0] == L'.' && name[1] == L'.' && name[2] == 0);
}

void join_path(wchar_t* out, size_t cchOut, const wchar_t* a, const wchar_t* b) {
    size_t na = wcslen(a);
    int needSlash = (na > 0 && (a[na-1] != L'\\' && a[na-1] != L'/'));
    if (needSlash) {
        swprintf(out, cchOut, L"%ls\\%ls", a, b);
    } else {
        swprintf(out, cchOut, L"%ls%ls", a, b);
    }
}

void scan_path_recursive(const wchar_t* path, GUIDSET* set, SCANSTATS* st) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return;

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        wchar_t pat[MAX_PATH * 4];
        swprintf(pat, ARRAYSIZE(pat), L"%ls\\*", path);

        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) return;

        do {
            if (is_dot_or_dotdot(fd.cFileName)) continue;

            wchar_t child[MAX_PATH * 4];
            join_path(child, ARRAYSIZE(child), path, fd.cFileName);

            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                scan_path_recursive(child, set, st);
            } else {
                scan_file_for_guids(child, set, st);
            }
        } while (FindNextFileW(f, &fd));

        FindClose(f);
    } else {
        scan_file_for_guids(path, set, st);
    }
}

// ----------------------------- typelib enumeration -----------------------------

const wchar_t* typekind_name(TYPEKIND k) {
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

int cmd_tlb(const wchar_t* file) {
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
            gs, (unsigned)la->lcid, (int)la->syskind, (unsigned)la->wMajorVerNum, (unsigned)la->wMinorVerNum);
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

// ----------------------------- enum registry -----------------------------

int cmd_enum_root(const wchar_t* which, unsigned long limit) {
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
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, root, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        wprintf(L"Failed to open HKCR\\%ls\n", root);
        return 1;
    }

    DWORD idx = 0;
    wchar_t sub[256];
    DWORD cchSub = ARRAYSIZE(sub);

    unsigned long printed = 0;
    while (RegEnumKeyExW(k, idx++, sub, &cchSub, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        wprintf(L"[%ls] %ls\n", label, sub);
        printed++;
        if (limit && printed >= limit) break;
        cchSub = ARRAYSIZE(sub);
    }

    RegCloseKey(k);
    return 0;
}

// ----------------------------- commands -----------------------------

void usage(void) {
    wprintf(L"quuid - GUID/COM discovery CLI\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  quuid parse <guid>\n");
    wprintf(L"  quuid find  <guid>\n");
    wprintf(L"  quuid scan  <path> [--registry]\n");
    wprintf(L"  quuid tlb   <file.tlb|.dll|.ocx>\n");
    wprintf(L"  quuid enum  clsid|iid|typelib|appid [--limit N]\n");
}

int cmd_parse(const wchar_t* s) {
    GUID g;
    if (!parse_guid_any(s, &g)) {
        wprintf(L"Failed to parse GUID: %ls\n", s);
        return 1;
    }
    print_guid_forms(&g);
    return 0;
}

int cmd_find(const wchar_t* s) {
    GUID g;
    if (!parse_guid_any(s, &g)) {
        wprintf(L"Failed to parse GUID: %ls\n", s);
        return 1;
    }

    wchar_t gs[64];
    guid_to_string_braced(&g, gs, ARRAYSIZE(gs));
    wprintf(L"%ls\n", gs);

    if (!any_registry_hit(&g)) {
        wprintf(L"  (no HKCR hits in CLSID/Interface/TypeLib/AppID)\n");
        return 0;
    }

    query_all_categories(&g);
    return 0;
}

typedef struct PRINTCTX {
    int withRegistry;
    unsigned long long printed;
} PRINTCTX;

void print_guid_line_cb(const GUID* g, void* vctx) {
    PRINTCTX* ctx = (PRINTCTX*)vctx;
    wchar_t gs[64];
    guid_to_string_braced(g, gs, ARRAYSIZE(gs));
    wprintf(L"%ls\n", gs);
    ctx->printed++;
    if (ctx->withRegistry) {
        query_all_categories(g);
    }
}

int cmd_scan(const wchar_t* path, int withRegistry) {
    GUIDSET set;
    if (!guidset_init(&set, 256)) {
        wprintf(L"Out of memory.\n");
        return 1;
    }

    SCANSTATS st = {0};
    scan_path_recursive(path, &set, &st);

    wprintf(L"Scan:\n");
    wprintf(L"  files  : %llu\n", st.files_scanned);
    wprintf(L"  bytes  : %llu\n", st.bytes_scanned);
    wprintf(L"  hits   : %llu (raw matches)\n", st.matches);
    wprintf(L"  unique : %llu\n", (unsigned long long)set.len);

    PRINTCTX ctx;
    ctx.withRegistry = withRegistry;
    ctx.printed = 0;

    guidset_foreach(&set, print_guid_line_cb, &ctx);

    guidset_free(&set);
    return 0;
}

int parse_u32_dec(const wchar_t* s, unsigned long* out) {
    if (!s || !*s) return 0;
    unsigned long v = 0;
    for (const wchar_t* p = s; *p; p++) {
        if (*p < L'0' || *p > L'9') return 0;
        v = v * 10 + (unsigned long)(*p - L'0');
    }
    *out = v;
    return 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }

    const wchar_t* cmd = argv[1];

    if (_wcsicmp(cmd, L"parse") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_parse(argv[2]);
    }

    if (_wcsicmp(cmd, L"find") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_find(argv[2]);
    }

    if (_wcsicmp(cmd, L"scan") == 0) {
        if (argc < 3) { usage(); return 1; }
        int withRegistry = 0;
        if (argc >= 4 && _wcsicmp(argv[3], L"--registry") == 0) withRegistry = 1;
        return cmd_scan(argv[2], withRegistry);
    }

    if (_wcsicmp(cmd, L"tlb") == 0) {
        if (argc < 3) { usage(); return 1; }
        return cmd_tlb(argv[2]);
    }

    if (_wcsicmp(cmd, L"enum") == 0) {
        if (argc < 3) { usage(); return 1; }
        unsigned long limit = 100;
        for (int i = 3; i + 1 < argc; i++) {
            if (_wcsicmp(argv[i], L"--limit") == 0) {
                unsigned long v = 0;
                if (parse_u32_dec(argv[i+1], &v)) limit = v;
            }
        }
        return cmd_enum_root(argv[2], limit);
    }

    usage();
    return 1;
}
