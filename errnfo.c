#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

typedef LONG NTSTATUS;
typedef ULONG (WINAPI *PFN_RtlNtStatusToDosError)(NTSTATUS);

static bool g_stdout_is_console = false;

static bool is_console_handle(HANDLE h) {
    DWORD mode = 0;
    return (h != NULL && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode) != 0);
}

static void io_init(void) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_stdout_is_console = is_console_handle(hOut);
    if (g_stdout_is_console) {
        // Make UTF-8 output more reliable for redirected/legacy console settings.
        SetConsoleOutputCP(CP_UTF8);
    }
}

static void outw(const wchar_t *s) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_stdout_is_console) {
        DWORD written = 0;
        WriteConsoleW(hOut, s, (DWORD)wcslen(s), &written, NULL);
        return;
    }

    // Redirected: emit UTF-8 bytes.
    int need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return;
    char *buf = (char*)malloc((size_t)need);
    if (!buf) return;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, need, NULL, NULL);
    // need includes null terminator; don't write it.
    fwrite(buf, 1, (size_t)need - 1, stdout);
    free(buf);
}

static void outf(const wchar_t *fmt, ...) {
    wchar_t stackbuf[2048];
    va_list ap;
    va_start(ap, fmt);
#if defined(_MSC_VER)
    int n = _vsnwprintf_s(stackbuf, _countof(stackbuf), _TRUNCATE, fmt, ap);
#else
    int n = vswprintf(stackbuf, (int)(sizeof(stackbuf)/sizeof(stackbuf[0])), fmt, ap);
#endif
    va_end(ap);

    if (n >= 0) {
        outw(stackbuf);
        return;
    }

    // Fallback: allocate and retry.
    size_t cap = 8192;
    wchar_t *dyn = (wchar_t*)malloc(cap * sizeof(wchar_t));
    if (!dyn) return;

    va_start(ap, fmt);
#if defined(_MSC_VER)
    _vsnwprintf_s(dyn, cap, _TRUNCATE, fmt, ap);
#else
    vswprintf(dyn, (int)cap, fmt, ap);
#endif
    va_end(ap);

    outw(dyn);
    free(dyn);
}

static void trim_message_inplace(wchar_t *s) {
    if (!s) return;
    size_t n = wcslen(s);
    while (n > 0) {
        wchar_t c = s[n - 1];
        if (c == L'\r' || c == L'\n' || c == L' ' || c == L'\t') {
            s[n - 1] = 0;
            --n;
        } else {
            break;
        }
    }
}

static wchar_t* format_message_system(DWORD id) {
    wchar_t *buf = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, NULL, id, 0, (LPWSTR)&buf, 0, NULL);
    if (!len || !buf) return NULL;
    trim_message_inplace(buf);
    return buf; // LocalFree() when done
}

static wchar_t* format_message_module(HMODULE mod, DWORD id) {
    wchar_t *buf = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                  FORMAT_MESSAGE_FROM_HMODULE |
                  FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageW(flags, (LPCVOID)mod, id, 0, (LPWSTR)&buf, 0, NULL);
    if (!len || !buf) return NULL;
    trim_message_inplace(buf);
    return buf; // LocalFree() when done
}

typedef struct {
    uint16_t id;
    const wchar_t *name;
} FACNAME;

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
    { 13, L"FACILITY_MEDIASERVER" },
    { 14, L"FACILITY_MSMQ" },
    { 15, L"FACILITY_SETUPAPI" },
    { 16, L"FACILITY_SCARD" },
    { 17, L"FACILITY_COMPLUS" },
    { 18, L"FACILITY_AAF" },
    { 19, L"FACILITY_URT" },
    { 20, L"FACILITY_ACS" },
    { 21, L"FACILITY_DPLAY" },
    { 22, L"FACILITY_UMI" },
    { 23, L"FACILITY_SXS" },
    { 24, L"FACILITY_WINDOWS_CE" },
    { 25, L"FACILITY_HTTP" },
    { 26, L"FACILITY_USERMODE_COMMONLOG" },
    { 27, L"FACILITY_WER" },
    { 31, L"FACILITY_USERMODE_FILTER_MANAGER" },
    { 32, L"FACILITY_BACKGROUNDCOPY" },
    { 33, L"FACILITY_CONFIGURATION/WIA" },
    { 34, L"FACILITY_STATE_MANAGEMENT" },
    { 35, L"FACILITY_METADIRECTORY" },
    { 36, L"FACILITY_WINDOWSUPDATE" },
    { 37, L"FACILITY_DIRECTORYSERVICE" },
    { 38, L"FACILITY_GRAPHICS" },
    { 39, L"FACILITY_NAP/SHELL" },
    { 40, L"FACILITY_TPM_SERVICES" },
    { 41, L"FACILITY_TPM_SOFTWARE" },
    { 42, L"FACILITY_UI" },
    { 43, L"FACILITY_XAML" },
    { 44, L"FACILITY_ACTION_QUEUE" },
    { 48, L"FACILITY_PLA/WINDOWS_SETUP" },
    { 49, L"FACILITY_FVE" },
    { 50, L"FACILITY_FWP" },
    { 51, L"FACILITY_WINRM" },
    { 52, L"FACILITY_NDIS" },
    { 53, L"FACILITY_USERMODE_HYPERVISOR" },
};

static const wchar_t* facility_name(uint16_t id) {
    for (size_t i = 0; i < sizeof(g_facility_names)/sizeof(g_facility_names[0]); ++i) {
        if (g_facility_names[i].id == id) return g_facility_names[i].name;
    }
    return NULL;
}

static bool parse_u32(const wchar_t *s, uint32_t *out) {
    if (!s || !*s || !out) return false;

    errno = 0;
    wchar_t *end = NULL;

    // Accept signed or unsigned input.
    long long v = wcstoll(s, &end, 0);
    if (end == s || errno == ERANGE) return false;

    // Skip trailing whitespace.
    while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') end++;
    if (*end != 0) return false;

    *out = (uint32_t)v;
    return true;
}

static void print_message_line(const wchar_t *label, wchar_t *msg) {
    if (!msg) {
        outf(L"  %ls: (no message)\n", label);
        return;
    }
    outf(L"  %ls: %ls\n", label, msg);
}

static void print_win32(uint32_t e) {
    outf(L"WIN32\n");
    outf(L"  value: 0x%08X (%u, %d)\n", e, e, (int32_t)e);

    wchar_t *sys = format_message_system((DWORD)e);
    print_message_line(L"message", sys);
    if (sys) LocalFree(sys);
}

static const wchar_t* hresult_sev_name(uint32_t sbit) {
    return sbit ? L"failure" : L"success";
}

static void print_hresult(uint32_t hr) {
    // HRESULT layout (per MS-ERREF): S,R,C,N,X, Facility(11), Code(16)
    // S:31 R:30 C:29 N:28 X:27 Facility:16..26 Code:0..15
    uint32_t S = (hr >> 31) & 1u;
    uint32_t R = (hr >> 30) & 1u;
    uint32_t C = (hr >> 29) & 1u;
    uint32_t N = (hr >> 28) & 1u; // FACILITY_NT_BIT marker
    uint32_t X = (hr >> 27) & 1u;
    uint16_t fac = (uint16_t)((hr >> 16) & 0x07FFu);
    uint16_t code = (uint16_t)(hr & 0xFFFFu);

    outf(L"HRESULT\n");
    outf(L"  value: 0x%08X (%u, %d)\n", hr, hr, (int32_t)hr);
    outf(L"  S(severity): %ls (%u)\n", hresult_sev_name(S), S);
    outf(L"  R(reserved): %u\n", R);
    outf(L"  C(customer): %u\n", C);
    outf(L"  N(nt-bit): %u\n", N);
    outf(L"  X(reserved): %u\n", X);

    const wchar_t *fname = facility_name(fac);
    if (fname) outf(L"  facility: 0x%03X (%u) %ls\n", fac, fac, fname);
    else       outf(L"  facility: 0x%03X (%u)\n", fac, fac);

    outf(L"  code: 0x%04X (%u)\n", code, code);

    // Message resolution: try system message for the HRESULT itself.
    wchar_t *sys = format_message_system((DWORD)hr);
    if (sys) {
        print_message_line(L"message", sys);
        LocalFree(sys);
    } else {
        // If this looks like HRESULT_FROM_WIN32, also show the embedded Win32 message.
        if (hr == 0) {
            wchar_t *wmsg0 = format_message_system(0);
            print_message_line(L"message", wmsg0);
            if (wmsg0) LocalFree(wmsg0);
        } else if ((hr & 0xFFFF0000u) == 0x80070000u) {
            uint32_t w32 = (uint32_t)(hr & 0xFFFFu);
            wchar_t *wmsg = format_message_system((DWORD)w32);
            if (wmsg) {
                outf(L"  message(win32-embedded): %ls\n", wmsg);
                LocalFree(wmsg);
            } else {
                outf(L"  message: (no message)\n");
            }
        } else {
            outf(L"  message: (no message)\n");
        }
    }

    // Derived interpretations
    if (hr == 0) {
        outf(L"  derived win32: 0\n");
    } else if ((hr & 0xFFFF0000u) == 0x80070000u) {
        outf(L"  derived win32: %u (0x%X)\n", (uint32_t)(hr & 0xFFFFu), (uint32_t)(hr & 0xFFFFu));
    }

    if (N) {
        // HRESULT_FROM_NT(x) is x | 0x10000000, so invert by clearing the bit.
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

static PFN_RtlNtStatusToDosError g_pRtlNtStatusToDosError = NULL;
static HMODULE g_ntdll = NULL;

static void ensure_ntdll(void) {
    if (g_ntdll) return;
    g_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!g_ntdll) g_ntdll = LoadLibraryW(L"ntdll.dll");
    if (g_ntdll) {
        g_pRtlNtStatusToDosError = (PFN_RtlNtStatusToDosError)GetProcAddress(g_ntdll, "RtlNtStatusToDosError");
    }
}

static void print_ntstatus(uint32_t st) {
    // NTSTATUS layout (per MS-ERREF): Sev(2), C(1), N(1), Facility(12), Code(16)
    uint32_t Sev = (st >> 30) & 3u;
    uint32_t C   = (st >> 29) & 1u;
    uint32_t N   = (st >> 28) & 1u; // reserved (should be 0); set when mapping to HRESULT by setting this bit
    uint16_t fac = (uint16_t)((st >> 16) & 0x0FFFu);
    uint16_t code= (uint16_t)(st & 0xFFFFu);

    outf(L"NTSTATUS\n");
    outf(L"  value: 0x%08X (%u, %d)\n", st, st, (int32_t)st);
    outf(L"  Sev: %ls (%u)\n", nt_sev_name(Sev), Sev);
    outf(L"  C(customer): %u\n", C);
    outf(L"  N(reserved): %u\n", N);

    const wchar_t *fname = facility_name(fac);
    if (fname) outf(L"  facility: 0x%03X (%u) %ls\n", fac, fac, fname);
    else       outf(L"  facility: 0x%03X (%u)\n", fac, fac);

    outf(L"  code: 0x%04X (%u)\n", code, code);

    // Message resolution: try ntdll message table first.
    ensure_ntdll();
    wchar_t *ntmsg = NULL;
    if (g_ntdll) ntmsg = format_message_module(g_ntdll, (DWORD)st);

    if (ntmsg) {
        print_message_line(L"message", ntmsg);
        LocalFree(ntmsg);
    } else {
        outf(L"  message: (no message)\n");
    }

    // Derived conversions
    // HRESULT_FROM_NT(x) == (x | FACILITY_NT_BIT), FACILITY_NT_BIT == 0x10000000
    uint32_t hr_from_nt = (st | 0x10000000u);
    outf(L"  derived hresult: 0x%08X\n", hr_from_nt);

    if (g_pRtlNtStatusToDosError) {
        uint32_t w32 = (uint32_t)g_pRtlNtStatusToDosError((NTSTATUS)st);
        outf(L"  derived win32: %u (0x%X)\n", w32, w32);
        wchar_t *wmsg = format_message_system((DWORD)w32);
        if (wmsg) {
            outf(L"  message(win32-derived): %ls\n", wmsg);
            LocalFree(wmsg);
        }
    } else {
        outf(L"  derived win32: (RtlNtStatusToDosError unavailable)\n");
    }
}

static void print_usage(void) {
    outw(L"errinfo - Windows error decoder\n\n");
    outw(L"Usage:\n");
    outw(L"  errinfo hr  <value>   Interpret as HRESULT\n");
    outw(L"  errinfo nt  <value>   Interpret as NTSTATUS\n");
    outw(L"  errinfo w32 <value>   Interpret as Win32 error (GetLastError)\n");
    outw(L"  errinfo <value>       Heuristic: show all interpretations\n\n");
    outw(L"Value formats: decimal, hex (0x...), or signed decimal.\n");
}

static void print_all(uint32_t v) {
    outf(L"Input: 0x%08X (%u, %d)\n\n", v, v, (int32_t)v);
    print_hresult(v);
    outw(L"\n");
    print_ntstatus(v);
    outw(L"\n");
    print_win32(v);
}

static bool is_tag(const wchar_t *s, const wchar_t *tag) {
    return (_wcsicmp(s, tag) == 0);
}

int wmain(int argc, wchar_t **argv) {
    io_init();

    if (argc < 2) {
        print_usage();
        return 2;
    }

    if (is_tag(argv[1], L"-h") || is_tag(argv[1], L"--help") || is_tag(argv[1], L"/?")) {
        print_usage();
        return 0;
    }

    // Tagged mode: errinfo <tag> <value>
    if (argc >= 3) {
        if (is_tag(argv[1], L"hr") || is_tag(argv[1], L"hresult")) {
            uint32_t v = 0;
            if (!parse_u32(argv[2], &v)) { outw(L"Parse error.\n"); return 2; }
            print_hresult(v);
            return 0;
        }
        if (is_tag(argv[1], L"nt") || is_tag(argv[1], L"ntstatus")) {
            uint32_t v = 0;
            if (!parse_u32(argv[2], &v)) { outw(L"Parse error.\n"); return 2; }
            print_ntstatus(v);
            return 0;
        }
        if (is_tag(argv[1], L"w32") || is_tag(argv[1], L"win32") || is_tag(argv[1], L"dos")) {
            uint32_t v = 0;
            if (!parse_u32(argv[2], &v)) { outw(L"Parse error.\n"); return 2; }
            print_win32(v);
            return 0;
        }
    }

    // Heuristic mode: errinfo <value>
    {
        uint32_t v = 0;
        if (!parse_u32(argv[1], &v)) { outw(L"Parse error.\n"); return 2; }
        print_all(v);
    }

    return 0;
}
