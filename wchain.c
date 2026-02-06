// wchain.c
// Wait-chain inspector for Windows threads.
//
// Build (MSVC):
//   cl /nologo /W4 /O2 /DUNICODE /D_UNICODE wchain.c advapi32.lib user32.lib
//
// Build (clang-cl):
//   clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE wchain.c advapi32.lib user32.lib
//
// Usage:
//   wchain [--pid <pid>|--self] [--all-threads] [--include-running] [--verbose]
//
// Examples:
//   wchain --self
//   wchain --pid 1234 --all-threads
//   wchain --pid 1234 --all-threads --include-running
//   wchain --self --all-threads --verbose

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <tlhelp32.h>
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4201) // nameless struct/union in SDK headers
#endif
#include <wct.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#include <wchar.h>
#include <wctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>

#ifndef WCT_MAX_NODE_COUNT
#define WCT_MAX_NODE_COUNT 16
#endif

#ifndef WCT_OBJNAME_LENGTH
#define WCT_OBJNAME_LENGTH 128
#endif

#ifndef WCT_OUT_OF_PROC_FLAG
#define WCT_OUT_OF_PROC_FLAG 0x00000001
#endif
#ifndef WCT_OUT_OF_PROC_COM_FLAG
#define WCT_OUT_OF_PROC_COM_FLAG 0x00000002
#endif
#ifndef WCT_OUT_OF_PROC_CS_FLAG
#define WCT_OUT_OF_PROC_CS_FLAG 0x00000004
#endif
#ifndef WCT_NETWORK_IO_FLAG
#define WCT_NETWORK_IO_FLAG 0x00000008
#endif
#ifndef WCTP_GETINFO_ALL_FLAGS
#define WCTP_GETINFO_ALL_FLAGS (WCT_OUT_OF_PROC_FLAG | WCT_OUT_OF_PROC_COM_FLAG | WCT_OUT_OF_PROC_CS_FLAG | WCT_NETWORK_IO_FLAG)
#endif

typedef enum {
    PARSE_OK = 0,
    PARSE_HELP = 1,
    PARSE_ERROR = 2
} PARSE_RESULT;

typedef struct {
    DWORD pid;
    bool gui_only;
    bool include_running;
    bool verbose;
} OPTIONS;

typedef struct {
    DWORD* ids;
    size_t count;
    size_t cap;
} THREAD_LIST;

typedef struct {
    DWORD total_threads;
    DWORD gui_threads;
    DWORD inspected_threads;
    DWORD displayed_threads;
    DWORD blocked_threads;
    DWORD failures;
} STATS;

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

    {
        int need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        if (need <= 0) return;

        char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need);
        if (!buf) return;

        WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, need, NULL, NULL);
        {
            DWORD written = 0;
            WriteFile(h, buf, (DWORD)(need - 1), &written, NULL);
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
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
        wchar_t* dyn = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
        if (!dyn) return;

        _vsnwprintf_s(dyn, cap, _TRUNCATE, fmt, ap);
        write_w(h, is_console, dyn);
        HeapFree(GetProcessHeap(), 0, dyn);
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
    {
        wchar_t* end = NULL;
        unsigned long long v = wcstoull(s, &end, 0);
        if (end == s || errno == ERANGE || v > 0xFFFFFFFFull) return false;

        while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') end++;
        if (*end != 0) return false;
        *out = (DWORD)v;
    }
    return true;
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

static void print_usage(void) {
    outw(L"wchain - wait-chain inspector for Windows threads\n");
    outw(L"\n");
    outw(L"Usage:\n");
    outw(L"  wchain [--pid <pid>|--self] [--all-threads] [--include-running] [--verbose]\n");
    outw(L"\n");
    outw(L"Defaults:\n");
    outw(L"  --self process, GUI threads only, blocked/waiting threads only\n");
    outw(L"\n");
    outw(L"Options:\n");
    outw(L"  --pid <pid>          Target process ID (decimal or 0x-prefixed hex)\n");
    outw(L"  --self               Use current process ID (default)\n");
    outw(L"  --all-threads        Inspect non-GUI threads too\n");
    outw(L"  --include-running    Also print threads that are not currently blocked\n");
    outw(L"  --verbose            Print additional Win32 error text\n");
    outw(L"  -h, --help           Show this help text\n");
    outw(L"\n");
    outw(L"Examples:\n");
    outw(L"  wchain --self\n");
    outw(L"  wchain --pid 1234 --all-threads\n");
    outw(L"  wchain --pid 1234 --all-threads --include-running\n");
    outw(L"  wchain --self --all-threads --verbose\n");
}

static PARSE_RESULT parse_args(int argc, wchar_t** argv, OPTIONS* opt) {
    int i = 0;
    if (!opt) return PARSE_ERROR;

    opt->pid = GetCurrentProcessId();
    opt->gui_only = true;
    opt->include_running = false;
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
        if (streqi(a, L"--all-threads")) {
            opt->gui_only = false;
            continue;
        }
        if (streqi(a, L"--include-running")) {
            opt->include_running = true;
            continue;
        }
        if (streqi(a, L"--verbose")) {
            opt->verbose = true;
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

        errf(L"unknown option: %ls\n", a);
        return PARSE_ERROR;
    }

    return PARSE_OK;
}

static bool thread_list_push(THREAD_LIST* list, DWORD tid) {
    if (!list) return false;
    if (list->count == list->cap) {
        size_t newCap = (list->cap == 0) ? 64 : (list->cap * 2);
        DWORD* p = NULL;
        if (list->ids) {
            p = (DWORD*)HeapReAlloc(GetProcessHeap(), 0, list->ids, newCap * sizeof(DWORD));
        }
        else {
            p = (DWORD*)HeapAlloc(GetProcessHeap(), 0, newCap * sizeof(DWORD));
        }
        if (!p) return false;
        list->ids = p;
        list->cap = newCap;
    }
    list->ids[list->count++] = tid;
    return true;
}

static void thread_list_free(THREAD_LIST* list) {
    if (!list) return;
    if (list->ids) {
        HeapFree(GetProcessHeap(), 0, list->ids);
    }
    list->ids = NULL;
    list->count = 0;
    list->cap = 0;
}

static bool collect_threads_for_pid(DWORD pid, THREAD_LIST* list, const OPTIONS* opt) {
    HANDLE snap = INVALID_HANDLE_VALUE;
    THREADENTRY32 te;

    if (!list) return false;
    ZeroMemory(&te, sizeof(te));

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        print_win32_error(L"CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD)", GetLastError(), opt->verbose);
        return false;
    }

    te.dwSize = sizeof(te);
    if (!Thread32First(snap, &te)) {
        DWORD e = GetLastError();
        CloseHandle(snap);
        print_win32_error(L"Thread32First", e, opt->verbose);
        return false;
    }

    do {
        if (te.th32OwnerProcessID == pid) {
            if (!thread_list_push(list, te.th32ThreadID)) {
                errw(L"out of memory while collecting thread IDs\n");
                CloseHandle(snap);
                return false;
            }
        }
        te.dwSize = sizeof(te);
    } while (Thread32Next(snap, &te));

    {
        DWORD e = GetLastError();
        CloseHandle(snap);
        if (e != ERROR_NO_MORE_FILES) {
            print_win32_error(L"Thread32Next", e, opt->verbose);
            return false;
        }
    }

    return true;
}

static bool get_process_image(DWORD pid, wchar_t* out, size_t cchOut) {
    HANDLE h = NULL;
    DWORD cch = 0;

    if (!out || cchOut == 0) return false;
    out[0] = 0;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;

    cch = (DWORD)cchOut;
    if (!QueryFullProcessImageNameW(h, 0, out, &cch)) {
        out[0] = 0;
        CloseHandle(h);
        return false;
    }

    CloseHandle(h);
    return true;
}

static bool query_is_gui_thread(DWORD tid, bool* isGui, DWORD* errorCode) {
    GUITHREADINFO gti;
    if (!isGui || !errorCode) return false;

    ZeroMemory(&gti, sizeof(gti));
    gti.cbSize = sizeof(gti);

    if (GetGUIThreadInfo(tid, &gti)) {
        *isGui = true;
        *errorCode = 0;
        return true;
    }

    *isGui = false;
    *errorCode = GetLastError();
    return true;
}

static const wchar_t* object_type_name(int t) {
    switch (t) {
        case 1: return L"CriticalSection";
        case 2: return L"SendMessage";
        case 3: return L"Mutex";
        case 4: return L"ALPC";
        case 5: return L"COM";
        case 6: return L"ThreadWait";
        case 7: return L"ProcessWait";
        case 8: return L"Thread";
        case 9: return L"COMActivation";
        case 10: return L"Unknown";
        case 11: return L"SocketIo";
        case 12: return L"SmbIo";
        default: return L"Other";
    }
}

static const wchar_t* status_name(int s) {
    switch (s) {
        case 1: return L"NoAccess";
        case 2: return L"Running";
        case 3: return L"Blocked";
        case 4: return L"PidOnly";
        case 5: return L"PidOnlyRpcss";
        case 6: return L"Owned";
        case 7: return L"NotOwned";
        case 8: return L"Abandoned";
        case 9: return L"Unknown";
        case 10: return L"Error";
        default: return L"Other";
    }
}

static bool wait_chain_is_blocked(DWORD nodeCount, const WAITCHAIN_NODE_INFO* nodes, BOOL isCycle) {
    if (isCycle) return true;
    if (!nodes || nodeCount == 0) return false;

    if (nodeCount > 1) return true;
    if ((int)nodes[0].ObjectStatus == 3) return true; // Blocked

    return false;
}

static void print_chain_node(DWORD index, const WAITCHAIN_NODE_INFO* node) {
    if (!node) return;

    if ((int)node->ObjectType == 8) {
        outf(L"  [%lu] %ls status=%ls pid=%lu tid=%lu waitMs=%lu ctxSwitch=%lu\n",
             (unsigned long)index,
             object_type_name((int)node->ObjectType),
             status_name((int)node->ObjectStatus),
             (unsigned long)node->ThreadObject.ProcessId,
             (unsigned long)node->ThreadObject.ThreadId,
             (unsigned long)node->ThreadObject.WaitTime,
             (unsigned long)node->ThreadObject.ContextSwitches);
    }
    else {
        wchar_t objName[WCT_OBJNAME_LENGTH + 1];
        objName[0] = 0;
        wcsncpy_s(objName, _countof(objName), node->LockObject.ObjectName, _TRUNCATE);

        outf(L"  [%lu] %ls status=%ls name=\"%ls\" alertable=%ls\n",
             (unsigned long)index,
             object_type_name((int)node->ObjectType),
             status_name((int)node->ObjectStatus),
             (objName[0] != 0) ? objName : L"-",
             node->LockObject.Alertable ? L"yes" : L"no");
    }
}

static int run_inspector(const OPTIONS* opt) {
    THREAD_LIST list;
    HWCT session = NULL;
    STATS stats;
    wchar_t imagePath[2048];
    size_t i = 0;

    ZeroMemory(&list, sizeof(list));
    ZeroMemory(&stats, sizeof(stats));
    imagePath[0] = 0;

    if (!collect_threads_for_pid(opt->pid, &list, opt)) {
        thread_list_free(&list);
        return 1;
    }

    session = OpenThreadWaitChainSession(0, NULL);
    if (!session) {
        print_win32_error(L"OpenThreadWaitChainSession", GetLastError(), opt->verbose);
        thread_list_free(&list);
        return 1;
    }

    get_process_image(opt->pid, imagePath, _countof(imagePath));
    outf(L"PID %lu", (unsigned long)opt->pid);
    if (imagePath[0] != 0) {
        outf(L" (%ls)", imagePath);
    }
    outw(L"\n");
    outf(L"Filter: %ls, %ls\n\n",
         opt->gui_only ? L"GUI threads only" : L"all threads",
         opt->include_running ? L"include running threads" : L"blocked/waiting only");

    for (i = 0; i < list.count; i++) {
        DWORD tid = list.ids[i];
        bool isGui = false;
        DWORD guiErr = 0;
        WAITCHAIN_NODE_INFO nodes[WCT_MAX_NODE_COUNT];
        DWORD nodeCount = WCT_MAX_NODE_COUNT;
        BOOL isCycle = FALSE;
        bool blocked = false;
        DWORD j = 0;

        stats.total_threads++;
        query_is_gui_thread(tid, &isGui, &guiErr);
        if (isGui) stats.gui_threads++;

        if (opt->gui_only && !isGui) {
            continue;
        }
        stats.inspected_threads++;

        ZeroMemory(nodes, sizeof(nodes));
        if (!GetThreadWaitChain(session, 0, WCTP_GETINFO_ALL_FLAGS, tid, &nodeCount, nodes, &isCycle)) {
            stats.failures++;
            if (opt->verbose) {
                print_win32_error(L"GetThreadWaitChain", GetLastError(), true);
                if (opt->gui_only && guiErr != 0) {
                    errf(L"  (GetGUIThreadInfo tid=%lu returned %lu)\n",
                         (unsigned long)tid, (unsigned long)guiErr);
                }
            }
            continue;
        }

        blocked = wait_chain_is_blocked(nodeCount, nodes, isCycle);
        if (blocked) stats.blocked_threads++;

        if (!opt->include_running && !blocked) {
            continue;
        }

        stats.displayed_threads++;
        outf(L"TID %lu [%ls] [%ls] nodes=%lu cycle=%ls\n",
             (unsigned long)tid,
             isGui ? L"GUI" : L"non-GUI",
             blocked ? L"blocked" : L"running",
             (unsigned long)nodeCount,
             isCycle ? L"yes" : L"no");

        for (j = 0; j < nodeCount; j++) {
            print_chain_node(j, &nodes[j]);
        }
        outw(L"\n");
    }

    if (stats.displayed_threads == 0) {
        outw(L"No matching threads found.\n\n");
    }

    outf(L"Summary: total=%lu gui=%lu inspected=%lu displayed=%lu blocked=%lu failures=%lu\n",
         (unsigned long)stats.total_threads,
         (unsigned long)stats.gui_threads,
         (unsigned long)stats.inspected_threads,
         (unsigned long)stats.displayed_threads,
         (unsigned long)stats.blocked_threads,
         (unsigned long)stats.failures);

    CloseThreadWaitChainSession(session);
    thread_list_free(&list);
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

    return run_inspector(&opt);
}
