// errnfo.c
// Windows error decoder + extensible message-table sources + message-table discovery/dump.
//
// Build (MSVC):
//   cl /nologo /W4 /O2 /DUNICODE /D_UNICODE errnfo.c
//
// Build (clang-cl):
//   clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE errnfo.c
//
// Build (clang in MSVC env):
//   clang -O2 -Wall -Wextra -DUNICODE -D_UNICODE errnfo.c -o errnfo.exe
//
// -----------------------------------------------------------------------------
// DECODE (terse, primary use):
//   errnfo [decode-options] <tag> <value>
//   errnfo [decode-options] <value>            (heuristic: show HRESULT + NTSTATUS + WIN32)
//
// Examples:
//   errnfo hr 0x8034001B
//   errnfo nt 0xC0000241
//   errnfo w32 5
//   errnfo 0xC0000241
//
// Decode options:
//   -m, --module <dll-or-path>    Add message-table module (repeatable)
//       --lang <id>              FormatMessage language id (default: 0 user default)
//       --no-common              Disable built-in common module list
//       --list-tags              List available tags
//   -h, --help                    Help
//
// -----------------------------------------------------------------------------
// SCAN (discovery, directory scope; no listing of strings):
//   errnfo scan <dir> [--recursive] [--paths] [--verbose]
//
// Output defaults to pasteable `-m "path"` lines (stdout).
//   --paths    output only paths (stdout)
//   --verbose  print scan diagnostics to stderr
//
// Examples:
//   errnfo scan C:\Windows\System32 --recursive > msgmods.txt
//   errnfo scan C:\Windows\System32 --paths > msgmods_paths.txt
//
// -----------------------------------------------------------------------------
// DUMP (inspection, single-module scope; optional entry listing):
//   errnfo dump <module-or-path> [--tables] [--langs] [--list]
//                                [--lang <id>]
//                                [--id-min <n>] [--id-max <n>]
//                                [--grep <substr>]
//                                [--max <n>]
//
// Defaults:
//   - if no --tables/--langs/--list is specified: behaves as --tables.
//   - --list prints message entries; use filters to avoid floods.
//
// Notes:
//   - In dump mode, --lang filters *resource language* (WORD). (Example: 0x409)
//   - In decode mode, --lang sets FormatMessage language id.
//
// Examples:
//   errnfo dump netmsg.dll --tables
//   errnfo dump wininet.dll --list --grep timeout --max 50
//   errnfo dump C:\Windows\System32\ntdll.dll --list --lang 0x409 --id-min 0xC0000000 --id-max 0xC0000300
//
// =============================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <wctype.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>

typedef LONG NTSTATUS;
typedef ULONG(WINAPI* PFN_RtlNtStatusToDosError)(NTSTATUS);

#ifndef LOAD_LIBRARY_AS_IMAGE_RESOURCE
#define LOAD_LIBRARY_AS_IMAGE_RESOURCE 0x00000020
#endif
#ifndef LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE
#define LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE 0x00000040
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif


// ============================
// I/O (UTF-16 console; UTF-8 when redirected)
// ============================

static bool g_stdout_is_console = false;
static bool g_stderr_is_console = false;

static bool is_console_handle(HANDLE h) {
    DWORD mode = 0;
    return (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0);
}

static void io_init(void) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    g_stdout_is_console = is_console_handle(hOut);
    g_stderr_is_console = is_console_handle(hErr);
    if (g_stdout_is_console || g_stderr_is_console) {
        SetConsoleOutputCP(CP_UTF8);
    }
}

static void write_w(HANDLE h, bool is_console, const wchar_t* s) {
    if (!s) return;

    if (is_console) {
        DWORD written = 0;
        WriteConsoleW(h, s, (DWORD)wcslen(s), &written, NULL);
        return;
    }

    int need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return;

    char* buf = (char*)malloc((size_t)need);
    if (!buf) return;

    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, need, NULL, NULL);

    DWORD written = 0;
    WriteFile(h, buf, (DWORD)(need - 1), &written, NULL);
    free(buf);
}

static void outw(const wchar_t* s) {
    write_w(GetStdHandle(STD_OUTPUT_HANDLE), g_stdout_is_console, s);
}

static void errw(const wchar_t* s) {
    write_w(GetStdHandle(STD_ERROR_HANDLE), g_stderr_is_console, s);
}

static void vfmt_to(HANDLE h, bool is_console, const wchar_t* fmt, va_list ap) {
    wchar_t stackbuf[2048];
#if defined(_MSC_VER)
    int n = _vsnwprintf_s(stackbuf, _countof(stackbuf), _TRUNCATE, fmt, ap);
#else
    int n = vswprintf(stackbuf, (int)(sizeof(stackbuf) / sizeof(stackbuf[0])), fmt, ap);
#endif
    if (n >= 0) {
        write_w(h, is_console, stackbuf);
        return;
    }

    size_t cap = 8192;
    wchar_t* dyn = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (!dyn) return;

    va_list ap2;
    va_copy(ap2, ap);
#if defined(_MSC_VER)
    _vsnwprintf_s(dyn, cap, _TRUNCATE, fmt, ap2);
#else
    vswprintf(dyn, (int)cap, fmt, ap2);
#endif
    va_end(ap2);

    write_w(h, is_console, dyn);
    free(dyn);
}

static void outf(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfmt_to(GetStdHandle(STD_OUTPUT_HANDLE), g_stdout_is_console, fmt, ap);
    va_end(ap);
}

static void errf(const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfmt_to(GetStdHandle(STD_ERROR_HANDLE), g_stderr_is_console, fmt, ap);
    va_end(ap);
}

// ============================
// General utilities
// ============================

static bool streqi(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

static bool parse_u32(const wchar_t* s, uint32_t* out) {
    if (!s || !*s || !out) return false;

    errno = 0;
    wchar_t* end = NULL;
    long long v = wcstoll(s, &end, 0);
    if (end == s || errno == ERANGE) return false;

    while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') end++;
    if (*end != 0) return false;

    *out = (uint32_t)v;
    return true;
}

static bool parse_u64(const wchar_t* s, uint64_t* out) {
    if (!s || !*s || !out) return false;

    errno = 0;
    wchar_t* end = NULL;
    unsigned long long v = wcstoull(s, &end, 0);
    if (end == s || errno == ERANGE) return false;

    while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') end++;
    if (*end != 0) return false;

    *out = (uint64_t)v;
    return true;
}

static bool has_pathish(const wchar_t* s) {
    if (!s) return false;
    for (const wchar_t* p = s; *p; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':') return true;
    }
    return false;
}

static void path_join(wchar_t* dst, size_t dstCch, const wchar_t* a, const wchar_t* b) {
    size_t alen = wcslen(a);
    bool needSlash = (alen > 0 && a[alen - 1] != L'\\' && a[alen - 1] != L'/');
#if defined(_MSC_VER)
    _snwprintf_s(dst, dstCch, _TRUNCATE, needSlash ? L"%ls\\%ls" : L"%ls%ls", a, b);
#else
    swprintf(dst, (int)dstCch, needSlash ? L"%ls\\%ls" : L"%ls%ls", a, b);
#endif
}

static void trim_ws_tail_inplace(wchar_t* s) {
    if (!s) return;
    size_t n = wcslen(s);
    while (n) {
        wchar_t c = s[n - 1];
        if (c == L'\r' || c == L'\n' || c == L' ' || c == L'\t') {
            s[--n] = 0;
        }
        else break;
    }
}

static const wchar_t* wcsistr(const wchar_t* hay, const wchar_t* needle) {
    if (!hay || !needle) return NULL;
    if (!*needle) return hay;

    for (const wchar_t* h = hay; *h; ++h) {
        const wchar_t* a = h;
        const wchar_t* b = needle;
        while (*a && *b) {
            wchar_t ca = towlower(*a);
            wchar_t cb = towlower(*b);
            if (ca != cb) break;
            ++a;
            ++b;
        }
        if (!*b) return h;
    }
    return NULL;
}

// ============================
// Message formatting (system + module message tables)
// ============================

static wchar_t* format_message_system(DWORD id, DWORD langid) {
    wchar_t* buf = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, NULL, id, langid, (LPWSTR)&buf, 0, NULL);
    if (!len || !buf) return NULL;
    trim_ws_tail_inplace(buf);
    return buf; // LocalFree by caller
}

static wchar_t* format_message_module(HMODULE mod, DWORD id, DWORD langid) {
    wchar_t* buf = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_HMODULE |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, (LPCVOID)mod, id, langid, (LPWSTR)&buf, 0, NULL);
    if (!len || !buf) return NULL;
    trim_ws_tail_inplace(buf);
    return buf; // LocalFree by caller
}

// ============================
// Module lists (user/common/tag)
// ============================

typedef struct {
    wchar_t spec[MAX_PATH];
    wchar_t label[MAX_PATH];
    HMODULE h;
    bool tried;
} MSGMOD;

typedef struct {
    MSGMOD* v;
    size_t n;
    size_t cap;
} MODLIST;

static void modlist_free(MODLIST* ml) {
    if (!ml) return;
    for (size_t i = 0; i < ml->n; ++i) {
        if (ml->v[i].h) FreeLibrary(ml->v[i].h);
    }
    free(ml->v);
    ml->v = NULL;
    ml->n = ml->cap = 0;
}

static bool modlist_add(MODLIST* ml, const wchar_t* spec, const wchar_t* label) {
    if (!ml || !spec || !*spec) return false;

    for (size_t i = 0; i < ml->n; ++i) {
        if (_wcsicmp(ml->v[i].spec, spec) == 0) return true;
    }

    if (ml->n == ml->cap) {
        size_t ncap = (ml->cap == 0) ? 8 : (ml->cap * 2);
        MSGMOD* nv = (MSGMOD*)realloc(ml->v, ncap * sizeof(MSGMOD));
        if (!nv) return false;
        ml->v = nv;
        ml->cap = ncap;
    }

    MSGMOD* m = &ml->v[ml->n++];
    wcsncpy_s(m->spec, _countof(m->spec), spec, _TRUNCATE);
    if (label && *label) wcsncpy_s(m->label, _countof(m->label), label, _TRUNCATE);
    else wcsncpy_s(m->label, _countof(m->label), spec, _TRUNCATE);
    m->h = NULL;
    m->tried = false;
    return true;
}

static HMODULE load_msg_module_best_effort(const wchar_t* spec) {
    DWORD searchFlags = 0;
    if (!has_pathish(spec)) {
        searchFlags = LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    }

    HMODULE h = LoadLibraryExW(spec, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | searchFlags);
    if (!h) h = LoadLibraryExW(spec, NULL, LOAD_LIBRARY_AS_DATAFILE | searchFlags);

    if (!h && searchFlags) {
        h = LoadLibraryExW(spec, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (!h) h = LoadLibraryExW(spec, NULL, LOAD_LIBRARY_AS_DATAFILE);
    }
    return h;
}

static void modlist_ensure_loaded(MODLIST* ml) {
    if (!ml) return;
    for (size_t i = 0; i < ml->n; ++i) {
        MSGMOD* m = &ml->v[i];
        if (m->tried) continue;
        m->tried = true;
        m->h = load_msg_module_best_effort(m->spec);
    }
}

static wchar_t* format_message_from_list(MODLIST* ml, DWORD id, DWORD langid, const wchar_t** outSource) {
    if (!ml) return NULL;
    modlist_ensure_loaded(ml);

    for (size_t i = 0; i < ml->n; ++i) {
        MSGMOD* m = &ml->v[i];
        if (!m->h) continue;
        wchar_t* msg = format_message_module(m->h, id, langid);
        if (msg) {
            if (outSource) *outSource = m->label;
            return msg;
        }
    }
    return NULL;
}

// ============================
// Facilities (small display map)
// ============================

typedef struct { uint16_t id; const wchar_t* name; } FACNAME;

static const FACNAME g_facility_names[] = {
    { 0,  L"FACILITY_NULL" },
    { 1,  L"FACILITY_RPC" },
    { 2,  L"FACILITY_DISPATCH" },
    { 3,  L"FACILITY_STORAGE" },
    { 4,  L"FACILITY_ITF" },
    { 7,  L"FACILITY_WIN32" },
    { 8,  L"FACILITY_WINDOWS" },
    { 9,  L"FACILITY_SECURITY/SSPI" },
    { 10, L"FACILITY_CONTROL" },
    { 11, L"FACILITY_CERT" },
    { 12, L"FACILITY_INTERNET" },
    { 15, L"FACILITY_SETUPAPI" },
    { 19, L"FACILITY_URT" },
    { 23, L"FACILITY_SXS" },
    { 27, L"FACILITY_WER" },
    { 36, L"FACILITY_WINDOWSUPDATE" },
    { 38, L"FACILITY_GRAPHICS" },
    { 48, L"FACILITY_WINDOWS_SETUP" },
    { 49, L"FACILITY_FVE" },
    { 50, L"FACILITY_FWP" },
};

static const wchar_t* facility_name(uint16_t id) {
    for (size_t i = 0; i < sizeof(g_facility_names) / sizeof(g_facility_names[0]); ++i) {
        if (g_facility_names[i].id == id) return g_facility_names[i].name;
    }
    return NULL;
}

// ============================
// Decode context + resolution policy
// ============================

static HMODULE g_ntdll = NULL;
static PFN_RtlNtStatusToDosError g_pRtlNtStatusToDosError = NULL;

static void ensure_ntdll(void) {
    if (g_ntdll) return;
    g_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!g_ntdll) g_ntdll = LoadLibraryW(L"ntdll.dll");
    if (g_ntdll) {
        g_pRtlNtStatusToDosError = (PFN_RtlNtStatusToDosError)GetProcAddress(g_ntdll, "RtlNtStatusToDosError");
    }
}

typedef struct {
    DWORD langid;          // 0 = user default (FormatMessage)
    bool common_enabled;

    MODLIST userMods;      // -m/--module
    MODLIST commonMods;    // curated list
} CTX;

// Editable "common list" (load failures tolerated).
// Note: can update to language specific MUI.
static const wchar_t* g_common_module_specs[] = {
    L"netmsg.dll",
    L"wininet.dll",
    L"setupapi.dll",
};

static void ctx_init(CTX* c) {
    ZeroMemory(c, sizeof(*c));
    c->langid = 0;
    c->common_enabled = true;

    for (size_t i = 0; i < sizeof(g_common_module_specs) / sizeof(g_common_module_specs[0]); ++i) {
        modlist_add(&c->commonMods, g_common_module_specs[i], g_common_module_specs[i]);
    }
}

static void ctx_free(CTX* c) {
    if (!c) return;
    modlist_free(&c->userMods);
    modlist_free(&c->commonMods);
}

static wchar_t* resolve_message(
    CTX* c,
    DWORD id,
    bool trySystem,
    MODLIST* tagDefaults,
    const wchar_t** outSource
) {
    if (outSource) *outSource = NULL;

    if (trySystem) {
        wchar_t* sys = format_message_system(id, c->langid);
        if (sys) {
            if (outSource) *outSource = L"(system)";
            return sys;
        }
    }

    if (tagDefaults) {
        wchar_t* m = format_message_from_list(tagDefaults, id, c->langid, outSource);
        if (m) return m;
    }

    {
        wchar_t* m = format_message_from_list(&c->userMods, id, c->langid, outSource);
        if (m) return m;
    }

    if (c->common_enabled) {
        wchar_t* m = format_message_from_list(&c->commonMods, id, c->langid, outSource);
        if (m) return m;
    }

    return NULL;
}

static void print_message_line(const wchar_t* label, const wchar_t* source, wchar_t* msg) {
    if (!msg) {
        outf(L"  %ls: (no message)\n", label);
        return;
    }
    if (source) outf(L"  %ls: %ls  [%ls]\n", label, msg, source);
    else        outf(L"  %ls: %ls\n", label, msg);
}

static const wchar_t* hresult_sev_name(uint32_t sbit) { return sbit ? L"failure" : L"success"; }

static const wchar_t* nt_sev_name(uint32_t sev2) {
    switch (sev2 & 3u) {
    case 0: return L"success";
    case 1: return L"informational";
    case 2: return L"warning";
    case 3: return L"error";
    default: return L"(?)";
    }
}

static void print_win32(CTX* c, uint32_t e, MODLIST* tagDefaults) {
    outf(L"WIN32\n");
    outf(L"  value: 0x%08X\n", e);

    const wchar_t* src = NULL;
    wchar_t* msg = resolve_message(c, (DWORD)e, true, tagDefaults, &src);
    print_message_line(L"message", src, msg);
    if (msg) LocalFree(msg);
}

static void print_hresult(CTX* c, uint32_t hr, MODLIST* tagDefaults) {
    uint32_t S = (hr >> 31) & 1u;
    uint32_t R = (hr >> 30) & 1u;
    uint32_t Cb = (hr >> 29) & 1u;
    uint32_t N = (hr >> 28) & 1u;      // FACILITY_NT_BIT marker in HRESULT layout
    uint32_t X = (hr >> 27) & 1u;
    uint16_t fac = (uint16_t)((hr >> 16) & 0x07FFu);
    uint16_t code = (uint16_t)(hr & 0xFFFFu);

    outf(L"HRESULT\n");
    outf(L"  value: 0x%08X\n", hr);
    outf(L"  S(severity): %ls (%u)\n", hresult_sev_name(S), S);
    outf(L"  R(reserved): %u\n", R);
    outf(L"  C(customer): %u\n", Cb);
    outf(L"  N(nt-bit): %u\n", N);
    outf(L"  X(reserved): %u\n", X);

    {
        const wchar_t* fname = facility_name(fac);
        if (fname) outf(L"  facility: 0x%03X (%u) %ls\n", fac, fac, fname);
        else       outf(L"  facility: 0x%03X (%u)\n", fac, fac);
    }
    outf(L"  code: 0x%04X (%u)\n", code, code);

    const wchar_t* src = NULL;
    wchar_t* msg = resolve_message(c, (DWORD)hr, true, tagDefaults, &src);
    if (msg) {
        print_message_line(L"message", src, msg);
        LocalFree(msg);
    }
    else {
        outf(L"  message: (no message)\n");
    }

    if ((hr & 0xFFFF0000u) == 0x80070000u) { // common HRESULT_FROM_WIN32 encoding
        uint32_t w32 = (uint32_t)(hr & 0xFFFFu);
        const wchar_t* src2 = NULL;
        wchar_t* wmsg = resolve_message(c, (DWORD)w32, true, NULL, &src2);
        if (wmsg) {
            outf(L"  message(win32-embedded): %ls  [%ls]\n", wmsg, src2 ? src2 : L"(?)");
            LocalFree(wmsg);
        }
        outf(L"  derived win32: %u (0x%X)\n", w32, w32);
    }

    if (N) {
        uint32_t nt = (hr & ~0x10000000u); // inverse of HRESULT_FROM_NT (best-effort)
        outf(L"  derived ntstatus: 0x%08X\n", nt);
    }
}

static void print_ntstatus(CTX* c, uint32_t st, MODLIST* tagDefaults) {
    uint32_t Sev = (st >> 30) & 3u;
    uint32_t Cb = (st >> 29) & 1u;
    uint32_t N = (st >> 28) & 1u;     // reserved in NTSTATUS
    uint16_t fac = (uint16_t)((st >> 16) & 0x0FFFu);
    uint16_t code = (uint16_t)(st & 0xFFFFu);

    outf(L"NTSTATUS\n");
    outf(L"  value: 0x%08X\n", st);
    outf(L"  Sev: %ls (%u)\n", nt_sev_name(Sev), Sev);
    outf(L"  C(customer): %u\n", Cb);
    outf(L"  N(reserved): %u\n", N);

    {
        const wchar_t* fname = facility_name(fac);
        if (fname) outf(L"  facility: 0x%03X (%u) %ls\n", fac, fac, fname);
        else       outf(L"  facility: 0x%03X (%u)\n", fac, fac);
    }
    outf(L"  code: 0x%04X (%u)\n", code, code);

    ensure_ntdll();

    const wchar_t* src = NULL;
    wchar_t* msg = NULL;

    if (g_ntdll) {
        msg = format_message_module(g_ntdll, (DWORD)st, c->langid);
        if (msg) src = L"ntdll.dll";
    }
    if (!msg) {
        msg = resolve_message(c, (DWORD)st, false, tagDefaults, &src);
    }

    if (msg) {
        print_message_line(L"message", src, msg);
        LocalFree(msg);
    }
    else {
        outf(L"  message: (no message)\n");
    }

    uint32_t hr_from_nt = (st | 0x10000000u); // HRESULT_FROM_NT
    outf(L"  derived hresult: 0x%08X\n", hr_from_nt);

    if (g_pRtlNtStatusToDosError) {
        uint32_t w32 = (uint32_t)g_pRtlNtStatusToDosError((NTSTATUS)st);
        outf(L"  derived win32: %u (0x%X)\n", w32, w32);

        const wchar_t* src2 = NULL;
        wchar_t* wmsg = resolve_message(c, (DWORD)w32, true, NULL, &src2);
        if (wmsg) {
            outf(L"  message(win32-derived): %ls  [%ls]\n", wmsg, src2 ? src2 : L"(?)");
            LocalFree(wmsg);
        }
    }
    else {
        outf(L"  derived win32: (RtlNtStatusToDosError unavailable)\n");
    }
}

static void print_all(CTX* c, uint32_t v) {
    outf(L"Input: 0x%08X\n\n", v);
    print_hresult(c, v, NULL);
    outw(L"\n");
    print_ntstatus(c, v, NULL);
    outw(L"\n");
    print_win32(c, v, NULL);
}

// ============================
// Tag system (easy to extend)
// ============================

typedef struct TAGCTX TAGCTX;
typedef int (*TAGRUN)(TAGCTX* t, int argc, wchar_t** argv);

typedef struct {
    const wchar_t* tag;
    const wchar_t* help;
    TAGRUN run;
} TAGDEF;

struct TAGCTX {
    CTX* ctx;
};

// Example environment tag defaults
static MODLIST g_dxMods;

static void init_tag_modules(void) {
    // note: found through scan (check your system)
    modlist_add(&g_dxMods, L"dxgi.dll", L"dxgi.dll");
    modlist_add(&g_dxMods, L"DXGIDebug.dll", L"DXGIDebug.dll");
    modlist_add(&g_dxMods, L"dxgwdi.dll", L"dxgwdi.dll");
    modlist_add(&g_dxMods, L"d3d9.dll", L"d3d9.dll");
    modlist_add(&g_dxMods, L"d3d10core.dll", L"d3d10core.dll");
    modlist_add(&g_dxMods, L"d3d10level9.dll", L"d3d10level9.dll");
    modlist_add(&g_dxMods, L"d3d10_1core.dll", L"d3d10_1core.dll");
    modlist_add(&g_dxMods, L"d3d11.dll", L"d3d11.dll");
    modlist_add(&g_dxMods, L"D3D12Core.dll", L"D3D12Core.dll");
    modlist_add(&g_dxMods, L"D3DSCache.dll", L"D3DSCache.dll");
}

static void free_tag_modules(void) {
    modlist_free(&g_dxMods);
}

static int tag_hr(TAGCTX* t, int argc, wchar_t** argv) {
    if (argc < 1) return -1;
    uint32_t v = 0;
    if (!parse_u32(argv[0], &v)) return -2;
    print_hresult(t->ctx, v, NULL);
    return 0;
}

static int tag_nt(TAGCTX* t, int argc, wchar_t** argv) {
    if (argc < 1) return -1;
    uint32_t v = 0;
    if (!parse_u32(argv[0], &v)) return -2;
    print_ntstatus(t->ctx, v, NULL);
    return 0;
}

static int tag_w32(TAGCTX* t, int argc, wchar_t** argv) {
    if (argc < 1) return -1;
    uint32_t v = 0;
    if (!parse_u32(argv[0], &v)) return -2;
    print_win32(t->ctx, v, NULL);
    return 0;
}

static int tag_dx(TAGCTX* t, int argc, wchar_t** argv) {
    if (argc < 1) return -1;
    uint32_t v = 0;
    if (!parse_u32(argv[0], &v)) return -2;
    print_hresult(t->ctx, v, &g_dxMods);
    return 0;
}

static const TAGDEF g_tags[] = {
    { L"hr",       L"Interpret as HRESULT", tag_hr },
    { L"hresult",  L"Interpret as HRESULT", tag_hr },
    { L"nt",       L"Interpret as NTSTATUS", tag_nt },
    { L"ntstatus", L"Interpret as NTSTATUS", tag_nt },
    { L"w32",      L"Interpret as Win32 error (GetLastError)", tag_w32 },
    { L"win32",    L"Interpret as Win32 error (GetLastError)", tag_w32 },
    { L"dx",       L"Interpret as HRESULT + try DX modules for message text", tag_dx },
};

static const TAGDEF* find_tag(const wchar_t* s) {
    for (size_t i = 0; i < sizeof(g_tags) / sizeof(g_tags[0]); ++i) {
        if (streqi(s, g_tags[i].tag)) return &g_tags[i];
    }
    return NULL;
}

static void list_tags(void) {
    outw(L"Tags:\n");
    for (size_t i = 0; i < sizeof(g_tags) / sizeof(g_tags[0]); ++i) {
        outf(L"  %-10ls  %ls\n", g_tags[i].tag, g_tags[i].help);
    }
}

// ============================
// SCAN subcommand (directory scope; discovery only)
// ============================

static bool file_has_ext_ci(const wchar_t* path, const wchar_t* ext) {
    size_t n = wcslen(path), m = wcslen(ext);
    if (n < m) return false;
    return _wcsicmp(path + (n - m), ext) == 0;
}

static bool scan_ext_allowed(const wchar_t* path) {
    static const wchar_t* kExts[] = {
        L".dll", L".exe", L".mui", L".sys", L".ocx", L".cpl", L".acm", L".drv"
    };
    for (size_t i = 0; i < sizeof(kExts) / sizeof(kExts[0]); ++i) {
        if (file_has_ext_ci(path, kExts[i])) return true;
    }
    return false;
}

static HMODULE load_for_resource_scan(const wchar_t* path) {
    // Resource/data mapping (avoid running DllMain).
    HMODULE h = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!h) h = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE);
    if (!h) h = LoadLibraryExW(path, NULL, LOAD_LIBRARY_AS_DATAFILE);
    return h;
}

typedef struct {
    bool found;
} SCAN_FOUND_CTX;

static BOOL CALLBACK scan_enum_name_cb(HMODULE h, LPCWSTR type, LPWSTR name, LONG_PTR lparam) {
    (void)h; (void)type; (void)name;
    SCAN_FOUND_CTX* c = (SCAN_FOUND_CTX*)lparam;
    c->found = true;
    return FALSE; // stop early; discovery only
}

static bool file_has_msgtable(const wchar_t* path) {
    HMODULE h = load_for_resource_scan(path);
    if (!h) return false;

    SCAN_FOUND_CTX c = { 0 };
    EnumResourceNamesW(h, RT_MESSAGETABLE, scan_enum_name_cb, (LONG_PTR)&c);
    FreeLibrary(h);
    return c.found;
}

typedef struct {
    bool recursive;
    bool paths_only;
    bool verbose;
} SCANOPT;

static void scan_dir(const wchar_t* dir, const SCANOPT* opt) {
    wchar_t pat[MAX_PATH];
#if defined(_MSC_VER)
    _snwprintf_s(pat, _countof(pat), _TRUNCATE, L"%ls\\*", dir);
#else
    swprintf(pat, _countof(pat), L"%ls\\*", dir);
#endif

    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pat, &fd);
    if (hf == INVALID_HANDLE_VALUE) {
        if (opt->verbose) errf(L"scan: cannot enumerate '%ls' (GLE=%lu)\n", dir, GetLastError());
        return;
    }

    do {
        const wchar_t* name = fd.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;

        wchar_t full[MAX_PATH];
        path_join(full, _countof(full), dir, name);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (opt->recursive) scan_dir(full, opt);
            continue;
        }

        if (!scan_ext_allowed(full)) continue;

        if (file_has_msgtable(full)) {
            if (opt->paths_only) outf(L"%ls\n", full);
            else outf(L"-m \"%ls\"\n", full);
        }

    } while (FindNextFileW(hf, &fd));

    FindClose(hf);
}

static int cmd_scan(int argc, wchar_t** argv) {
    // argv[0] == "scan"
    SCANOPT opt;
    ZeroMemory(&opt, sizeof(opt));

    const wchar_t* dir = NULL;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!a || !*a) continue;

        if (streqi(a, L"--recursive")) { opt.recursive = true; continue; }
        if (streqi(a, L"--paths")) { opt.paths_only = true; continue; }
        if (streqi(a, L"--verbose")) { opt.verbose = true; continue; }

        if (a[0] == L'-') {
            errf(L"scan: unknown option '%ls'\n", a);
            return 2;
        }

        if (!dir) { dir = a; continue; }
        errw(L"scan: too many positional arguments\n");
        return 2;
    }

    if (!dir) {
        errw(L"scan: missing <dir>\n");
        return 2;
    }

    scan_dir(dir, &opt);
    return 0;
}

// ============================
// DUMP subcommand (single-module scope; optional entry listing)
// ============================

typedef struct {
    bool tables;
    bool langs;
    bool list;

    bool filter_lang;
    WORD lang;

    bool filter_id_min;
    bool filter_id_max;
    uint32_t id_min;
    uint32_t id_max;

    const wchar_t* grep;   // optional substring (case-insensitive)
    bool limit;
    uint64_t max_print;

    bool verbose;          // extra summaries (stderr)
} DUMPOPT;

// Message table resource binary format
#pragma pack(push, 1)
typedef struct _MSG_RESOURCE_ENTRY {
    WORD Length; // bytes including header
    WORD Flags;  // 0=ANSI, 1=Unicode
    BYTE Text[1];
} MSG_RESOURCE_ENTRY;

typedef struct _MSG_RESOURCE_BLOCK {
    DWORD LowId;
    DWORD HighId;
    DWORD OffsetToEntries; // from start of MSG_RESOURCE_DATA
} MSG_RESOURCE_BLOCK;

typedef struct _MSG_RESOURCE_DATA {
    DWORD NumberOfBlocks;
    MSG_RESOURCE_BLOCK Blocks[1];
} MSG_RESOURCE_DATA;
#pragma pack(pop)

static wchar_t* ansi_to_wide_trim(const char* p, size_t cb) {
    size_t n = 0;
    while (n < cb && p[n] != 0) n++;

    int need = MultiByteToWideChar(CP_ACP, 0, p, (int)n, NULL, 0);
    if (need <= 0) return NULL;

    wchar_t* w = (wchar_t*)malloc(((size_t)need + 1) * sizeof(wchar_t));
    if (!w) return NULL;

    MultiByteToWideChar(CP_ACP, 0, p, (int)n, w, need);
    w[need] = 0;
    trim_ws_tail_inplace(w);
    return w;
}

static wchar_t* unicode_to_wide_trim(const wchar_t* p, size_t cch) {
    size_t n = 0;
    while (n < cch && p[n] != 0) n++;

    wchar_t* w = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
    if (!w) return NULL;

    memcpy(w, p, n * sizeof(wchar_t));
    w[n] = 0;
    trim_ws_tail_inplace(w);
    return w;
}

typedef struct {
    WORD langs[512];
    size_t n;
} LANGSET;

static void langset_add(LANGSET* s, WORD lang) {
    if (!s) return;
    for (size_t i = 0; i < s->n; ++i) if (s->langs[i] == lang) return;
    if (s->n < sizeof(s->langs) / sizeof(s->langs[0])) s->langs[s->n++] = lang;
}

typedef struct {
    const wchar_t* path;
    const DUMPOPT* opt;
    LANGSET* langset;     // for --langs
    uint64_t printed;     // for --max
} DUMPCTX;

static bool want_id(const DUMPOPT* o, uint32_t id) {
    if (o->filter_id_min && id < o->id_min) return false;
    if (o->filter_id_max && id > o->id_max) return false;
    return true;
}

static bool want_text(const DUMPOPT* o, const wchar_t* text) {
    if (!o->grep || !*o->grep) return true;
    return wcsistr(text, o->grep) != NULL;
}

static void dump_table_blob(
    DUMPCTX* dc,
    const BYTE* base,
    size_t cb,
    LPCWSTR name,
    WORD lang
) {
    const DUMPOPT* opt = dc->opt;

    if (cb < sizeof(DWORD)) return;
    const MSG_RESOURCE_DATA* rd = (const MSG_RESOURCE_DATA*)base;
    DWORD nb = rd->NumberOfBlocks;

    size_t blocks_off = sizeof(DWORD);
    size_t blocks_need = blocks_off + (size_t)nb * sizeof(MSG_RESOURCE_BLOCK);
    if (nb == 0 || blocks_need > cb) return;

    bool name_is_int = IS_INTRESOURCE(name) ? true : false;
    DWORD name_int = name_is_int ? (DWORD)(ULONG_PTR)name : 0;
    const wchar_t* name_str = name_is_int ? L"" : name;

    if (opt->tables || opt->list) {
        if (name_is_int) outf(L"table #%lu  lang 0x%04X  blocks %lu\n", name_int, lang, nb);
        else            outf(L"table \"%ls\"  lang 0x%04X  blocks %lu\n", name_str, lang, nb);
    }
    if (opt->verbose) {
        if (name_is_int) errf(L"dump: %ls table #%lu lang 0x%04X blocks %lu\n", dc->path, name_int, lang, nb);
        else            errf(L"dump: %ls table \"%ls\" lang 0x%04X blocks %lu\n", dc->path, name_str, lang, nb);
    }

    if (!opt->list) return;

    for (DWORD bi = 0; bi < nb; ++bi) {
        const MSG_RESOURCE_BLOCK* b = &rd->Blocks[bi];
        if (b->LowId > b->HighId) continue;
        if ((size_t)b->OffsetToEntries >= cb) continue;

        const BYTE* p = base + b->OffsetToEntries;
        const BYTE* end = base + cb;

        outf(L"  block[%lu]: 0x%08lX..0x%08lX (%lu)\n",
            bi, b->LowId, b->HighId, (b->HighId - b->LowId) + 1);

        for (DWORD id = b->LowId; id <= b->HighId; ++id) {
            if (p + 4 > end) break;

            const MSG_RESOURCE_ENTRY* e = (const MSG_RESOURCE_ENTRY*)p;
            WORD elen = e->Length;
            WORD flags = e->Flags;

            if (elen < 4) break;
            if (p + elen > end) break;

            const BYTE* text = e->Text;
            size_t text_cb = (size_t)elen - 4;

            bool show = want_id(opt, id);

            wchar_t* w = NULL;
            if (show) {
                if (flags == 1) {
                    size_t cch = text_cb / 2;
                    w = unicode_to_wide_trim((const wchar_t*)text, cch);
                }
                else {
                    w = ansi_to_wide_trim((const char*)text, text_cb);
                }

                if (w) {
                    if (!want_text(opt, w)) show = false;
                }
                else {
                    // If decode fails and grep is requested, treat as non-match.
                    if (opt->grep && *opt->grep) show = false;
                }
            }

            if (show) {
                if (opt->limit && dc->printed >= opt->max_print) {
                    if (w) free(w);
                    return;
                }

                if (w) {
                    outf(L"    0x%08lX: %ls\n", id, w);
                    free(w);
                }
                else {
                    outf(L"    0x%08lX: (decode failed)\n", id);
                }

                dc->printed++;
            }
            else {
                if (w) free(w);
            }

            p += elen;
            if (id == b->HighId) break;
        }
    }
}

typedef struct {
    DUMPCTX* dc;
    HMODULE h;
    LPCWSTR type;
    LPCWSTR name;
} DUMPNAMECTX;

static BOOL CALLBACK dump_enum_lang_cb(HMODULE h, LPCWSTR type, LPCWSTR name, WORD lang, LONG_PTR lparam) {
    DUMPCTX* dc = (DUMPCTX*)lparam;
    const DUMPOPT* opt = dc->opt;

    langset_add(dc->langset, lang);

    if (opt->filter_lang && lang != opt->lang) {
        return TRUE;
    }

    // If only collecting languages, don't load resources.
    if (opt->langs && !opt->tables && !opt->list) {
        return TRUE;
    }

    HRSRC r = FindResourceExW(h, type, name, lang);
    if (!r) return TRUE;

    DWORD cb = SizeofResource(h, r);
    if (!cb) return TRUE;

    HGLOBAL hg = LoadResource(h, r);
    if (!hg) return TRUE;

    const BYTE* p = (const BYTE*)LockResource(hg);
    if (!p) return TRUE;

    dump_table_blob(dc, p, (size_t)cb, name, lang);
    return TRUE;
}

static BOOL CALLBACK dump_enum_name_cb(HMODULE h, LPCWSTR type, LPWSTR name, LONG_PTR lparam) {
    DUMPCTX* dc = (DUMPCTX*)lparam;
    EnumResourceLanguagesW(h, type, name, dump_enum_lang_cb, (LONG_PTR)dc);
    return TRUE;
}

static int cmd_dump(int argc, wchar_t** argv) {
    // argv[0] == "dump"
    DUMPOPT opt;
    ZeroMemory(&opt, sizeof(opt));

    const wchar_t* module = NULL;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!a || !*a) continue;

        if (streqi(a, L"--tables")) { opt.tables = true; continue; }
        if (streqi(a, L"--langs")) { opt.langs = true; continue; }
        if (streqi(a, L"--list")) { opt.list = true; continue; }
        if (streqi(a, L"--verbose")) { opt.verbose = true; continue; }

        if (streqi(a, L"--lang")) {
            if (i + 1 >= argc) { errw(L"dump: --lang requires a value\n"); return 2; }
            ++i;
            uint32_t v = 0;
            if (!parse_u32(argv[i], &v)) { errw(L"dump: invalid --lang\n"); return 2; }
            opt.filter_lang = true;
            opt.lang = (WORD)v;
            continue;
        }

        if (streqi(a, L"--id-min")) {
            if (i + 1 >= argc) { errw(L"dump: --id-min requires a value\n"); return 2; }
            ++i;
            uint32_t v = 0;
            if (!parse_u32(argv[i], &v)) { errw(L"dump: invalid --id-min\n"); return 2; }
            opt.filter_id_min = true;
            opt.id_min = v;
            continue;
        }

        if (streqi(a, L"--id-max")) {
            if (i + 1 >= argc) { errw(L"dump: --id-max requires a value\n"); return 2; }
            ++i;
            uint32_t v = 0;
            if (!parse_u32(argv[i], &v)) { errw(L"dump: invalid --id-max\n"); return 2; }
            opt.filter_id_max = true;
            opt.id_max = v;
            continue;
        }

        if (streqi(a, L"--grep")) {
            if (i + 1 >= argc) { errw(L"dump: --grep requires a value\n"); return 2; }
            ++i;
            opt.grep = argv[i];
            continue;
        }

        if (streqi(a, L"--max")) {
            if (i + 1 >= argc) { errw(L"dump: --max requires a value\n"); return 2; }
            ++i;
            uint64_t v = 0;
            if (!parse_u64(argv[i], &v)) { errw(L"dump: invalid --max\n"); return 2; }
            opt.limit = true;
            opt.max_print = v;
            continue;
        }

        if (a[0] == L'-') {
            errf(L"dump: unknown option '%ls'\n", a);
            return 2;
        }

        if (!module) { module = a; continue; }
        errw(L"dump: too many positional arguments\n");
        return 2;
    }

    if (!module) {
        errw(L"dump: missing <module-or-path>\n");
        return 2;
    }

    // Default behavior: tables summary.
    if (!opt.tables && !opt.langs && !opt.list) opt.tables = true;

    // Load module for resource inspection.
    // If a basename is provided, attempt System32 search via LoadLibraryEx flags.
    HMODULE h = NULL;
    if (!has_pathish(module)) {
        DWORD searchFlags = LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        h = LoadLibraryExW(module, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE | searchFlags);
        if (!h) h = LoadLibraryExW(module, NULL, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | searchFlags);
        if (!h) h = LoadLibraryExW(module, NULL, LOAD_LIBRARY_AS_DATAFILE | searchFlags);
    }
    if (!h) {
        h = load_for_resource_scan(module);
    }
    if (!h) {
        errf(L"dump: could not load '%ls' (GLE=%lu)\n", module, GetLastError());
        return 2;
    }

    LANGSET langset;
    ZeroMemory(&langset, sizeof(langset));

    DUMPCTX dc;
    ZeroMemory(&dc, sizeof(dc));
    dc.path = module;
    dc.opt = &opt;
    dc.langset = &langset;

    // Enumerate message tables and languages; print as requested.
    EnumResourceNamesW(h, RT_MESSAGETABLE, dump_enum_name_cb, (LONG_PTR)&dc);

    if (opt.langs) {
        outf(L"languages:");
        for (size_t i = 0; i < langset.n; ++i) {
            outf(L" 0x%04X", langset.langs[i]);
        }
        outf(L"\n");
    }

    FreeLibrary(h);
    return 0;
}

// ============================
// Help / usage
// ============================

static void print_usage(void) {
    outw(L"errnfo - Windows error decoder + message-table tooling\n\n");

    outw(L"Decode:\n");
    outw(L"  errnfo [decode-options] <tag> <value>\n");
    outw(L"  errnfo [decode-options] <value>\n\n");

    outw(L"Scan:\n");
    outw(L"  errnfo scan <dir> [--recursive] [--paths] [--verbose]\n\n");

    outw(L"Dump:\n");
    outw(L"  errnfo dump <module-or-path> [--tables] [--langs] [--list]\n");
    outw(L"                             [--lang <id>] [--id-min <n>] [--id-max <n>]\n");
    outw(L"                             [--grep <substr>] [--max <n>] [--verbose]\n\n");

    outw(L"Decode options:\n");
    outw(L"  -m, --module <dll-or-path>    Add message-table module (repeatable)\n");
    outw(L"      --lang <id>               FormatMessage language id (decode). Default: 0\n");
    outw(L"      --no-common               Disable built-in common module list\n");
    outw(L"      --list-tags               List tags\n");
    outw(L"  -h, --help                    Help\n\n");

    outw(L"Examples:\n");
    outw(L"  errnfo hr 0x8034001B\n");
    outw(L"  errnfo nt 0xC0000241\n");
    outw(L"  errnfo w32 12029 -m wininet.dll\n");
    outw(L"  errnfo scan C:\\Windows\\System32 --recursive > msgmods.txt\n");
    outw(L"  errnfo dump netmsg.dll --tables\n");
    outw(L"  errnfo dump wininet.dll --list --grep connection --max 50\n");
}

// ============================
// Main: global decode-options + subcommand dispatch
// ============================

int wmain(int argc, wchar_t** argv) {
    io_init();

    CTX ctx;
    ctx_init(&ctx);
    init_tag_modules();

    // Parse global options (decode options + generic help/tag listing).
    // Policy: options must precede the first non-option token (subcommand/tag/value).
    int i = 1;
    for (; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!a || !*a) continue;

        if (streqi(a, L"-h") || streqi(a, L"--help") || streqi(a, L"/?")) {
            print_usage();
            ctx_free(&ctx);
            free_tag_modules();
            return 0;
        }

        if (streqi(a, L"--list-tags")) {
            list_tags();
            ctx_free(&ctx);
            free_tag_modules();
            return 0;
        }

        if (streqi(a, L"--no-common")) {
            ctx.common_enabled = false;
            continue;
        }

        if (streqi(a, L"-m") || streqi(a, L"--module")) {
            if (i + 1 >= argc) { errw(L"error: -m/--module requires a value\n"); break; }
            ++i;
            const wchar_t* spec = argv[i];
            modlist_add(&ctx.userMods, spec, spec);
            continue;
        }

        if (streqi(a, L"--lang")) {
            if (i + 1 >= argc) { errw(L"error: --lang requires a value\n"); break; }
            ++i;
            uint32_t lang = 0;
            if (!parse_u32(argv[i], &lang)) { errw(L"error: invalid --lang\n"); break; }
            ctx.langid = (DWORD)lang;
            continue;
        }

        if (a[0] == L'-') {
            errf(L"error: unknown option '%ls'\n", a);
            break;
        }

        // first non-option token
        break;
    }

    int remaining = argc - i;
    if (remaining <= 0) {
        print_usage();
        ctx_free(&ctx);
        free_tag_modules();
        return 2;
    }

    // Subcommand dispatch
    const wchar_t* cmd = argv[i];
    if (streqi(cmd, L"scan")) {
        int rc = cmd_scan(remaining, &argv[i]);
        ctx_free(&ctx);
        free_tag_modules();
        return rc;
    }
    if (streqi(cmd, L"dump")) {
        int rc = cmd_dump(remaining, &argv[i]);
        ctx_free(&ctx);
        free_tag_modules();
        return rc;
    }

    // Decode path (tag/value or heuristic).
    TAGCTX tc = { &ctx };

    if (remaining >= 2) {
        const TAGDEF* tag = find_tag(argv[i]);
        if (tag) {
            int rc = tag->run(&tc, remaining - 1, &argv[i + 1]);
            if (rc == -1) errf(L"error: tag '%ls' needs an argument\n", tag->tag);
            else if (rc == -2) errw(L"error: parse error\n");

            ctx_free(&ctx);
            free_tag_modules();
            return (rc == 0) ? 0 : 2;
        }
    }

    // Heuristic mode: single value
    {
        uint32_t v = 0;
        if (!parse_u32(argv[i], &v)) {
            errw(L"error: parse error\n");
            print_usage();
            ctx_free(&ctx);
            free_tag_modules();
            return 2;
        }
        print_all(&ctx, v);
    }

    ctx_free(&ctx);
    free_tag_modules();
    return 0;
}
