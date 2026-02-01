
// errnfo.c -- error information tool

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>

typedef LONG NTSTATUS;
typedef ULONG(WINAPI* PFN_RtlNtStatusToDosError)(NTSTATUS);

#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif
#ifndef LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
#define LOAD_LIBRARY_SEARCH_DEFAULT_DIRS 0x00001000
#endif

// ----------------------------
// Output (UTF-16 console, UTF-8 when redirected)
// ----------------------------

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

// ----------------------------
// Parsing helpers
// ----------------------------

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

static bool has_pathish(const wchar_t* s) {
    if (!s) return false;
    for (const wchar_t* p = s; *p; ++p) {
        if (*p == L'\\' || *p == L'/' || *p == L':') return true;
    }
    return false;
}

// ----------------------------
// Message formatting (system + module message tables)
// ----------------------------

static void trim_message_inplace(wchar_t* s) {
    if (!s) return;
    size_t n = wcslen(s);
    while (n > 0) {
        wchar_t c = s[n - 1];
        if (c == L'\r' || c == L'\n' || c == L' ' || c == L'\t') {
            s[n - 1] = 0;
            --n;
        }
        else break;
    }
}

static wchar_t* format_message_system(DWORD id, DWORD langid) {
    wchar_t* buf = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, NULL, id, langid, (LPWSTR)&buf, 0, NULL);
    if (!len || !buf) return NULL;
    trim_message_inplace(buf);
    return buf; // LocalFree() by caller
}

static wchar_t* format_message_module(HMODULE mod, DWORD id, DWORD langid) {
    wchar_t* buf = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_HMODULE |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, (LPCVOID)mod, id, langid, (LPWSTR)&buf, 0, NULL);
    if (!len || !buf) return NULL;
    trim_message_inplace(buf);
    return buf; // LocalFree() by caller
}

// ----------------------------
// Module list and safe loading
// ----------------------------

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

    // De-dupe by spec (case-insensitive)
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

static HMODULE load_msg_module(const wchar_t* spec) {
    // Resource-only load first, to avoid executing DllMain.
    DWORD baseFlags = LOAD_LIBRARY_AS_IMAGE_RESOURCE;

    // If caller gave just a basename, prefer System32 search flags when supported.
    DWORD searchFlags = 0;
    if (!has_pathish(spec)) {
        searchFlags = LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
    }

    HMODULE h = LoadLibraryExW(spec, NULL, baseFlags | searchFlags);
    if (!h) {
        h = LoadLibraryExW(spec, NULL, LOAD_LIBRARY_AS_DATAFILE | searchFlags);
    }
    return h;
}

static void modlist_ensure_loaded(MODLIST* ml) {
    if (!ml) return;
    for (size_t i = 0; i < ml->n; ++i) {
        if (ml->v[i].tried) continue;
        ml->v[i].tried = true;
        ml->v[i].h = load_msg_module(ml->v[i].spec);
        if (!ml->v[i].h) {
            DWORD gle = GetLastError();
            errf(L"warning: could not load module '%ls' (GLE=%lu)\n", ml->v[i].spec, gle);
        }
    }
}

static wchar_t* format_message_from_list(MODLIST* ml, DWORD id, DWORD langid, const wchar_t** outSource) {
    if (!ml) return NULL;
    modlist_ensure_loaded(ml);
    for (size_t i = 0; i < ml->n; ++i) {
        if (!ml->v[i].h) continue;
        wchar_t* msg = format_message_module(ml->v[i].h, id, langid);
        if (msg) {
            if (outSource) *outSource = ml->v[i].label;
            return msg;
        }
    }
    return NULL;
}

// ----------------------------
// Facilities (small, editable map for display)
// ----------------------------

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

// ----------------------------
// Global context
// ----------------------------

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
    DWORD langid;          // 0 = user default (FormatMessage convention)
    bool common_enabled;

    MODLIST userMods;      // -m / --module
    MODLIST commonMods;    // curated, editable list
} CTX;

// Editable "common list" users can tune to their environment.
// Load failures are non-fatal.
static const wchar_t* g_common_module_specs[] = {
    L"netmsg.dll",   // NetAPI NERR_* text
    L"wininet.dll",  // WinINet error text
    L"setupapi.dll",
    L"cfgmgr32.dll",
    L"ntdsbmsg.dll", // some directory-service / cert-service related message tables
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
    MODLIST* tagDefaults,          // may be NULL
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

// ----------------------------
// Printers
// ----------------------------

static void print_message_line(const wchar_t* label, const wchar_t* source, wchar_t* msg) {
    if (!msg) {
        outf(L"  %ls: (no message)\n", label);
        return;
    }
    if (source) outf(L"  %ls: %ls  [%ls]\n", label, msg, source);
    else        outf(L"  %ls: %ls\n", label, msg);
}

static const wchar_t* hresult_sev_name(uint32_t sbit) { return sbit ? L"failure" : L"success"; }

static void print_win32(CTX* c, uint32_t e, MODLIST* tagDefaults) {
    outf(L"WIN32\n");
    outf(L"  value: 0x%08X (%u, %d)\n", e, e, (int32_t)e);

    const wchar_t* src = NULL;
    wchar_t* msg = resolve_message(c, (DWORD)e, true, tagDefaults, &src);
    print_message_line(L"message", src, msg);
    if (msg) LocalFree(msg);
}

static void print_hresult(CTX* c, uint32_t hr, MODLIST* tagDefaults) {
    uint32_t S = (hr >> 31) & 1u;
    uint32_t R = (hr >> 30) & 1u;
    uint32_t Cb = (hr >> 29) & 1u;
    uint32_t N = (hr >> 28) & 1u;      // FACILITY_NT_BIT marker in the HRESULT layout
    uint32_t X = (hr >> 27) & 1u;
    uint16_t fac = (uint16_t)((hr >> 16) & 0x07FFu);
    uint16_t code = (uint16_t)(hr & 0xFFFFu);

    outf(L"HRESULT\n");
    outf(L"  value: 0x%08X (%u, %d)\n", hr, hr, (int32_t)hr);
    outf(L"  S(severity): %ls (%u)\n", hresult_sev_name(S), S);
    outf(L"  R(reserved): %u\n", R);
    outf(L"  C(customer): %u\n", Cb);
    outf(L"  N(nt-bit): %u\n", N);
    outf(L"  X(reserved): %u\n", X);

    const wchar_t* fname = facility_name(fac);
    if (fname) outf(L"  facility: 0x%03X (%u) %ls\n", fac, fac, fname);
    else       outf(L"  facility: 0x%03X (%u)\n", fac, fac);

    outf(L"  code: 0x%04X (%u)\n", code, code);

    // Message policy:
    // 1) Try system message for the HRESULT itself
    // 2) If HRESULT_FROM_WIN32 pattern, also show embedded Win32 message
    // 3) If still missing, try modules (tag/user/common) for the HRESULT id
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
        uint32_t nt = (hr & ~0x10000000u);
        outf(L"  derived ntstatus: 0x%08X\n", nt);
    }
}

static const wchar_t* nt_sev_name(uint32_t sev2) {
    switch (sev2 & 3u) {
    case 0: return L"success";
    case 1: return L"informational";
    case 2: return L"warning";
    case 3: return L"error";
    default: return L"(?)";
    }
}

static void print_ntstatus(CTX* c, uint32_t st, MODLIST* tagDefaults) {
    uint32_t Sev = (st >> 30) & 3u;
    uint32_t Cb = (st >> 29) & 1u;
    uint32_t N = (st >> 28) & 1u;     // reserved in NTSTATUS (kept 0 to allow mapping to HRESULT)
    uint16_t fac = (uint16_t)((st >> 16) & 0x0FFFu);
    uint16_t code = (uint16_t)(st & 0xFFFFu);

    outf(L"NTSTATUS\n");
    outf(L"  value: 0x%08X (%u, %d)\n", st, st, (int32_t)st);
    outf(L"  Sev: %ls (%u)\n", nt_sev_name(Sev), Sev);
    outf(L"  C(customer): %u\n", Cb);
    outf(L"  N(reserved): %u\n", N);

    const wchar_t* fname = facility_name(fac);
    if (fname) outf(L"  facility: 0x%03X (%u) %ls\n", fac, fac, fname);
    else       outf(L"  facility: 0x%03X (%u)\n", fac, fac);

    outf(L"  code: 0x%04X (%u)\n", code, code);

    // Message policy:
    // 1) Try ntdll message table for the NTSTATUS id (many have text there)
    // 2) If missing, try tag/user/common modules for the NTSTATUS id
    // 3) Convert to Win32 via RtlNtStatusToDosError and show Win32 message
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

    // Derived forms
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

// ----------------------------
// Tag system (easy to extend)
// ----------------------------

typedef struct TAGCTX TAGCTX;
typedef int (*TAGRUN)(TAGCTX* t, int argc, wchar_t** argv);

typedef struct {
    const wchar_t* tag;        // name used on cmdline
    const wchar_t* help;       // one-liner
    TAGRUN run;                // handler
} TAGDEF;

struct TAGCTX {
    CTX* ctx;
};

// Tag-specific default module lists are simply MODLISTs.
// This keeps tags user-editable without altering core resolution policy.
static MODLIST g_dxMods;

static void init_tag_modules(void) {
    // Example tag: dx (edit this to match your environment)
    // These may or may not contain message tables for your codes; missing loads are tolerated.
    modlist_add(&g_dxMods, L"dxgi.dll", L"dxgi.dll");
    modlist_add(&g_dxMods, L"d3d12.dll", L"d3d12.dll");
    modlist_add(&g_dxMods, L"d3d11.dll", L"d3d11.dll");
    modlist_add(&g_dxMods, L"d2d1.dll", L"d2d1.dll");
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

// Example of “environment tag”: dx => interpret value as HRESULT, but try DX modules for message text.
static int tag_dx(TAGCTX* t, int argc, wchar_t** argv) {
    if (argc < 1) return -1;
    uint32_t v = 0;
    if (!parse_u32(argv[0], &v)) return -2;
    print_hresult(t->ctx, v, &g_dxMods);
    return 0;
}

static const TAGDEF g_tags[] = {
    { L"hr",  L"Interpret as HRESULT",  tag_hr  },
    { L"hresult", L"Interpret as HRESULT", tag_hr },
    { L"nt",  L"Interpret as NTSTATUS", tag_nt  },
    { L"ntstatus", L"Interpret as NTSTATUS", tag_nt },
    { L"w32", L"Interpret as Win32 error (GetLastError)", tag_w32 },
    { L"win32", L"Interpret as Win32 error (GetLastError)", tag_w32 },
    { L"dx",  L"Interpret as HRESULT + try DX modules for message text", tag_dx },
};

static const TAGDEF* find_tag(const wchar_t* s) {
    for (size_t i = 0; i < sizeof(g_tags) / sizeof(g_tags[0]); ++i) {
        if (streqi(s, g_tags[i].tag)) return &g_tags[i];
    }
    return NULL;
}

// ----------------------------
// CLI
// ----------------------------

static void print_usage(void) {
    outw(L"errnfo - Windows error decoder (extensible)\n\n");
    outw(L"Usage:\n");
    outw(L"  errnfo [options] <tag> <value>\n");
    outw(L"  errnfo [options] <value>         (heuristic: show HRESULT + NTSTATUS + WIN32)\n\n");
    outw(L"Options:\n");
    outw(L"  -m, --module <dll-or-path>       Add message-table module (repeatable)\n");
    outw(L"      --lang <id>                  Language id (e.g. 0x409). Default: 0 (user default)\n");
    outw(L"      --no-common                  Disable built-in common module list\n");
    outw(L"      --list-tags                  Show available tags\n");
    outw(L"  -h, --help                       Show help\n\n");
    outw(L"Examples:\n");
    outw(L"  errnfo hr 0x8034001B\n");
    outw(L"  errnfo nt 0xC0000241\n");
    outw(L"  errnfo w32 12157 -m wininet.dll\n");
    outw(L"  errnfo dx 0x887A0005\n");
}

static void list_tags(void) {
    outw(L"Tags:\n");
    for (size_t i = 0; i < sizeof(g_tags) / sizeof(g_tags[0]); ++i) {
        outf(L"  %-10ls  %ls\n", g_tags[i].tag, g_tags[i].help);
    }
}

static void print_all(CTX* c, uint32_t v) {
    outf(L"Input: 0x%08X (%u, %d)\n\n", v, v, (int32_t)v);
    print_hresult(c, v, NULL);
    outw(L"\n");
    print_ntstatus(c, v, NULL);
    outw(L"\n");
    print_win32(c, v, NULL);
}

int wmain(int argc, wchar_t** argv) {
    io_init();

    CTX ctx;
    ctx_init(&ctx);
    init_tag_modules();

    // Parse options
    int i = 1;
    for (; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!a || !*a) continue;

        if (streqi(a, L"-h") || streqi(a, L"--help") || streqi(a, L"/?")) {
            print_usage();
            ctx_free(&ctx);
            modlist_free(&g_dxMods);
            return 0;
        }
        if (streqi(a, L"--list-tags")) {
            list_tags();
            ctx_free(&ctx);
            modlist_free(&g_dxMods);
            return 0;
        }
        if (streqi(a, L"--no-common")) {
            ctx.common_enabled = false;
            continue;
        }
        if (streqi(a, L"-m") || streqi(a, L"--module")) {
            if (i + 1 >= argc) { errw(L"error: -m/--module requires a value\n"); break; }
            const wchar_t* spec = argv[++i];
            modlist_add(&ctx.userMods, spec, spec);
            continue;
        }
        if (streqi(a, L"--lang")) {
            if (i + 1 >= argc) { errw(L"error: --lang requires a value\n"); break; }
            uint32_t lang = 0;
            if (!parse_u32(argv[++i], &lang)) { errw(L"error: invalid --lang\n"); break; }
            ctx.langid = (DWORD)lang;
            continue;
        }
        if (a[0] == L'-') {
            errf(L"error: unknown option '%ls'\n", a);
            break;
        }
        // First non-option
        break;
    }

    int remaining = argc - i;
    if (remaining <= 0) {
        print_usage();
        ctx_free(&ctx);
        modlist_free(&g_dxMods);
        return 2;
    }

    TAGCTX tc = { &ctx };

    // Tagged mode: <tag> <value>
    if (remaining >= 2) {
        const TAGDEF* tag = find_tag(argv[i]);
        if (tag) {
            int rc = tag->run(&tc, remaining - 1, &argv[i + 1]);
            if (rc == -1) errf(L"error: tag '%ls' needs an argument\n", tag->tag);
            else if (rc == -2) errw(L"error: parse error\n");
            ctx_free(&ctx);
            modlist_free(&g_dxMods);
            return (rc == 0) ? 0 : 2;
        }
    }

    // Heuristic mode: <value>
    {
        uint32_t v = 0;
        if (!parse_u32(argv[i], &v)) {
            errw(L"error: parse error\n");
            print_usage();
            ctx_free(&ctx);
            modlist_free(&g_dxMods);
            return 2;
        }
        print_all(&ctx, v);
    }

    ctx_free(&ctx);
    modlist_free(&g_dxMods);
    return 0;
}
