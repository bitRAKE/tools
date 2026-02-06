// modsnap.c
// Module snapshot CLI for Windows processes.
//
// Build (MSVC):
//   cl /nologo /W4 /O2 /DUNICODE /D_UNICODE modsnap.c
//
// Build (clang-cl):
//   clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE modsnap.c
//
// Usage:
//   modsnap [--pid <pid>|--self] [--paths|--csv|--count] [--verbose]
//
// Examples:
//   modsnap --self
//   modsnap --self --paths
//   modsnap --self --csv
//   modsnap --self --count

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#include <wctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>

typedef enum {
    PARSE_OK = 0,
    PARSE_HELP = 1,
    PARSE_ERROR = 2
} PARSE_RESULT;

typedef struct {
    DWORD pid;
    bool paths_only;
    bool csv;
    bool count_only;
    bool verbose;
} OPTIONS;

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
    {
        DWORD written = 0;
        WriteFile(h, buf, (DWORD)(need - 1), &written, NULL);
    }
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
    int n = _vsnwprintf_s(stackbuf, _countof(stackbuf), _TRUNCATE, fmt, ap);
    if (n >= 0) {
        write_w(h, is_console, stackbuf);
        return;
    }

    {
        size_t cap = 8192;
        wchar_t* dyn = (wchar_t*)malloc(cap * sizeof(wchar_t));
        if (!dyn) return;

        _vsnwprintf_s(dyn, cap, _TRUNCATE, fmt, ap);
        write_w(h, is_console, dyn);
        free(dyn);
    }
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

static bool streqi(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

static bool starts_with_i(const wchar_t* s, const wchar_t* prefix) {
    while (*prefix) {
        if (*s == 0) return false;
        if (towlower(*s) != towlower(*prefix)) return false;
        s++;
        prefix++;
    }
    return true;
}

static bool parse_u32(const wchar_t* s, DWORD* out) {
    if (!s || !*s || !out) return false;

    errno = 0;
    wchar_t* end = NULL;
    unsigned long long v = wcstoull(s, &end, 0);
    if (end == s || errno == ERANGE) return false;

    while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') end++;
    if (*end != 0) return false;
    if (v > 0xFFFFFFFFull) return false;

    *out = (DWORD)v;
    return true;
}

static void print_usage(void) {
    outw(L"modsnap - list loaded modules for a process\n");
    outw(L"\n");
    outw(L"Usage:\n");
    outw(L"  modsnap [--pid <pid>|--self] [--paths|--csv|--count] [--verbose]\n");
    outw(L"\n");
    outw(L"Options:\n");
    outw(L"  --pid <pid>   Target process ID (decimal or 0x-prefixed hex)\n");
    outw(L"  --self        Use current process ID (default)\n");
    outw(L"  --paths       Output module paths only (one per line)\n");
    outw(L"  --csv         Output CSV: pid,base,size,module,path\n");
    outw(L"  --count       Output only module count\n");
    outw(L"  --verbose     Print verbose Win32 error text\n");
    outw(L"  -h, --help    Show this help text\n");
    outw(L"\n");
    outw(L"Examples:\n");
    outw(L"  modsnap --self\n");
    outw(L"  modsnap --self --paths\n");
    outw(L"  modsnap --self --csv\n");
    outw(L"  modsnap --self --count\n");
}

static void print_win32_error(const wchar_t* where, DWORD code, bool verbose) {
    if (!verbose) {
        errf(L"%ls failed: %lu\n", where, (unsigned long)code);
        return;
    }

    {
        wchar_t buf[512];
        DWORD n = FormatMessageW(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL, code, 0, buf, (DWORD)_countof(buf), NULL);
        if (!n) {
            errf(L"%ls failed: %lu\n", where, (unsigned long)code);
            return;
        }

        while (n > 0 && (buf[n - 1] == L'\r' || buf[n - 1] == L'\n')) {
            buf[--n] = 0;
        }
        errf(L"%ls failed: %lu (%ls)\n", where, (unsigned long)code, buf);
    }
}

static PARSE_RESULT parse_args(int argc, wchar_t** argv, OPTIONS* opt) {
    int i = 0;
    if (!opt) return PARSE_ERROR;

    opt->pid = GetCurrentProcessId();
    opt->paths_only = false;
    opt->csv = false;
    opt->count_only = false;
    opt->verbose = false;

    for (i = 1; i < argc; i++) {
        const wchar_t* a = argv[i];

        if (streqi(a, L"-h") || streqi(a, L"--help")) {
            return PARSE_HELP;
        }
        if (streqi(a, L"--self")) {
            opt->pid = GetCurrentProcessId();
            continue;
        }
        if (streqi(a, L"--pid")) {
            DWORD pid = 0;
            if (i + 1 >= argc) {
                errw(L"--pid requires a value\n");
                return PARSE_ERROR;
            }
            if (!parse_u32(argv[++i], &pid)) {
                errf(L"invalid pid value: %ls\n", argv[i]);
                return PARSE_ERROR;
            }
            opt->pid = pid;
            continue;
        }
        if (starts_with_i(a, L"--pid=")) {
            DWORD pid = 0;
            const wchar_t* val = a + 6;
            if (!parse_u32(val, &pid)) {
                errf(L"invalid pid value: %ls\n", val);
                return PARSE_ERROR;
            }
            opt->pid = pid;
            continue;
        }
        if (streqi(a, L"--paths")) {
            opt->paths_only = true;
            continue;
        }
        if (streqi(a, L"--csv")) {
            opt->csv = true;
            continue;
        }
        if (streqi(a, L"--count")) {
            opt->count_only = true;
            continue;
        }
        if (streqi(a, L"--verbose")) {
            opt->verbose = true;
            continue;
        }

        errf(L"unknown option: %ls\n", a);
        return PARSE_ERROR;
    }

    if ((opt->paths_only && opt->csv) ||
        (opt->count_only && opt->csv) ||
        (opt->count_only && opt->paths_only)) {
        errw(L"--paths, --csv, and --count are mutually exclusive\n");
        return PARSE_ERROR;
    }

    return PARSE_OK;
}

static void format_base_hex(ULONG_PTR base, wchar_t* out, size_t cchOut) {
    if (!out || cchOut == 0) return;
#if defined(_WIN64)
    _snwprintf_s(out, cchOut, _TRUNCATE, L"0x%016llX", (unsigned long long)base);
#else
    _snwprintf_s(out, cchOut, _TRUNCATE, L"0x%08lX", (unsigned long)base);
#endif
}

static void csv_write_field(const wchar_t* s) {
    const wchar_t* p = s ? s : L"";
    wchar_t tmp[2];
    tmp[1] = 0;

    outw(L"\"");
    while (*p) {
        if (*p == L'"') {
            outw(L"\"\"");
        }
        else {
            tmp[0] = *p;
            outw(tmp);
        }
        p++;
    }
    outw(L"\"");
}

static void print_header(const OPTIONS* opt) {
    if (opt->count_only || opt->paths_only) return;

    if (opt->csv) {
        outw(L"pid,base,size,module,path\n");
        return;
    }

    outf(L"PID %lu\n", (unsigned long)opt->pid);
    outw(L"BASE               SIZE       MODULE                   PATH\n");
}

static void print_module_row(const OPTIONS* opt, const MODULEENTRY32W* me) {
    wchar_t baseHex[32];
    format_base_hex((ULONG_PTR)me->modBaseAddr, baseHex, _countof(baseHex));

    if (opt->paths_only) {
        outf(L"%ls\n", me->szExePath);
        return;
    }

    if (opt->csv) {
        outf(L"%lu,", (unsigned long)opt->pid);
        csv_write_field(baseHex);
        outf(L",%lu,", (unsigned long)me->modBaseSize);
        csv_write_field(me->szModule);
        outw(L",");
        csv_write_field(me->szExePath);
        outw(L"\n");
        return;
    }

    outf(L"%-18ls %-10lu %-24ls %ls\n",
         baseHex,
         (unsigned long)me->modBaseSize,
         me->szModule,
         me->szExePath);
}

static int run_snapshot(const OPTIONS* opt) {
    HANDLE snap = INVALID_HANDLE_VALUE;
    MODULEENTRY32W me;
    DWORD count = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, opt->pid);
    if (snap == INVALID_HANDLE_VALUE) {
        print_win32_error(L"CreateToolhelp32Snapshot", GetLastError(), opt->verbose);
        return 1;
    }

    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);

    if (!Module32FirstW(snap, &me)) {
        DWORD e = GetLastError();
        CloseHandle(snap);
        if (e == ERROR_NO_MORE_FILES) {
            if (opt->count_only) {
                outw(L"0\n");
            }
            else if (opt->csv) {
                outw(L"pid,base,size,module,path\n");
            }
            return 0;
        }
        print_win32_error(L"Module32FirstW", e, opt->verbose);
        return 1;
    }

    print_header(opt);
    do {
        count++;
        if (!opt->count_only) {
            print_module_row(opt, &me);
        }
        me.dwSize = sizeof(me);
    } while (Module32NextW(snap, &me));

    {
        DWORD e = GetLastError();
        CloseHandle(snap);
        if (e != ERROR_NO_MORE_FILES) {
            print_win32_error(L"Module32NextW", e, opt->verbose);
            return 1;
        }
    }

    if (opt->count_only) {
        outf(L"%lu\n", (unsigned long)count);
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    OPTIONS opt;
    PARSE_RESULT pr;

    io_init();
    pr = parse_args(argc, argv, &opt);
    if (pr == PARSE_HELP) {
        print_usage();
        return 0;
    }
    if (pr == PARSE_ERROR) {
        errw(L"\n");
        print_usage();
        return 2;
    }

    return run_snapshot(&opt);
}
