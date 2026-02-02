// rpscan.c
// Reparse-point scanner for Windows paths (files/directories).
//
// Build (MSVC):
//   cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rpscan.c
//
// Build (clang-cl):
//   clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rpscan.c
//
// Build (clang in MSVC env):
//   clang -O2 -Wall -Wextra -DUNICODE -D_UNICODE rpscan.c -o rpscan.exe
//
// Usage:
//   rpscan <path> [--recursive] [--max-depth N]
//                  [--files] [--dirs] [--paths]
//                  [--stats] [--verbose]
//
// Default: scan for reparse points under <path>. If <path> is a file, it is checked.
// If <path> is a directory, immediate children are scanned; use --recursive for deep scan.

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <winioctl.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <wctype.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>

#ifndef IO_REPARSE_TAG_LX_SYMLINK
#define IO_REPARSE_TAG_LX_SYMLINK (0xA000001D)
#endif
#ifndef IO_REPARSE_TAG_AF_UNIX
#define IO_REPARSE_TAG_AF_UNIX (0x80000023)
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
// Utilities
// ============================

static bool streqi(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

static bool parse_u32(const wchar_t* s, uint32_t* out) {
    if (!s || !*s || !out) return false;

    errno = 0;
    wchar_t* end = NULL;
    unsigned long long v = wcstoull(s, &end, 0);
    if (end == s || errno == ERANGE) return false;

    while (*end == L' ' || *end == L'\t' || *end == L'\r' || *end == L'\n') end++;
    if (*end != 0) return false;

    *out = (uint32_t)v;
    return true;
}

static wchar_t* path_join_alloc(const wchar_t* a, const wchar_t* b) {
    size_t alen = wcslen(a);
    size_t blen = wcslen(b);
    bool needSlash = (alen > 0 && a[alen - 1] != L'\\' && a[alen - 1] != L'/');
    size_t total = alen + (needSlash ? 1 : 0) + blen + 1;
    wchar_t* out = (wchar_t*)malloc(total * sizeof(wchar_t));
    if (!out) return NULL;

    if (needSlash) {
#if defined(_MSC_VER)
        _snwprintf_s(out, total, _TRUNCATE, L"%ls\\%ls", a, b);
#else
        swprintf(out, (int)total, L"%ls\\%ls", a, b);
#endif
    }
    else {
#if defined(_MSC_VER)
        _snwprintf_s(out, total, _TRUNCATE, L"%ls%ls", a, b);
#else
        swprintf(out, (int)total, L"%ls%ls", a, b);
#endif
    }

    return out;
}

// ============================
// Reparse inspection
// ============================

typedef struct {
    DWORD tag;
    wchar_t* target; // optional; caller frees
} REPARSE_INFO;

// Minimal reparse buffer definitions (avoid header availability differences).
typedef struct _RPS_SYMLINK_REPARSE_BUFFER {
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    ULONG Flags;
    WCHAR PathBuffer[1];
} RPS_SYMLINK_REPARSE_BUFFER;

typedef struct _RPS_MOUNT_POINT_REPARSE_BUFFER {
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    WCHAR PathBuffer[1];
} RPS_MOUNT_POINT_REPARSE_BUFFER;

typedef struct _RPS_REPARSE_DATA_BUFFER {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
    union {
        RPS_SYMLINK_REPARSE_BUFFER SymbolicLinkReparseBuffer;
        RPS_MOUNT_POINT_REPARSE_BUFFER MountPointReparseBuffer;
        struct {
            UCHAR DataBuffer[1];
        } GenericReparseBuffer;
    };
} RPS_REPARSE_DATA_BUFFER;

static const wchar_t* reparse_tag_name(DWORD tag) {
    switch (tag) {
    case IO_REPARSE_TAG_SYMLINK: return L"SYMLINK";
    case IO_REPARSE_TAG_MOUNT_POINT: return L"MOUNT_POINT";
    case IO_REPARSE_TAG_APPEXECLINK: return L"APPEXECLINK";
    case IO_REPARSE_TAG_WIM: return L"WIM";
    case IO_REPARSE_TAG_WCI: return L"WCI";
    case IO_REPARSE_TAG_WCI_1: return L"WCI_1";
    case IO_REPARSE_TAG_WCI_TOMBSTONE: return L"WCI_TOMBSTONE";
    case IO_REPARSE_TAG_CLOUD: return L"CLOUD";
    case IO_REPARSE_TAG_CLOUD_1: return L"CLOUD_1";
    case IO_REPARSE_TAG_CLOUD_2: return L"CLOUD_2";
    case IO_REPARSE_TAG_CLOUD_3: return L"CLOUD_3";
    case IO_REPARSE_TAG_CLOUD_4: return L"CLOUD_4";
    case IO_REPARSE_TAG_CLOUD_5: return L"CLOUD_5";
    case IO_REPARSE_TAG_CLOUD_6: return L"CLOUD_6";
    case IO_REPARSE_TAG_CLOUD_7: return L"CLOUD_7";
    case IO_REPARSE_TAG_CLOUD_8: return L"CLOUD_8";
    case IO_REPARSE_TAG_CLOUD_9: return L"CLOUD_9";
    case IO_REPARSE_TAG_CLOUD_A: return L"CLOUD_A";
    case IO_REPARSE_TAG_CLOUD_B: return L"CLOUD_B";
    case IO_REPARSE_TAG_CLOUD_C: return L"CLOUD_C";
    case IO_REPARSE_TAG_CLOUD_D: return L"CLOUD_D";
    case IO_REPARSE_TAG_CLOUD_E: return L"CLOUD_E";
    case IO_REPARSE_TAG_CLOUD_F: return L"CLOUD_F";
    case IO_REPARSE_TAG_NFS: return L"NFS";
    case IO_REPARSE_TAG_LX_SYMLINK: return L"LX_SYMLINK";
    case IO_REPARSE_TAG_AF_UNIX: return L"AF_UNIX";
    default: return NULL;
    }
}

static wchar_t* dup_wstr_range(const wchar_t* p, size_t cch) {
    wchar_t* out = (wchar_t*)malloc((cch + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    memcpy(out, p, cch * sizeof(wchar_t));
    out[cch] = 0;
    return out;
}

static wchar_t* extract_reparse_target(const RPS_REPARSE_DATA_BUFFER* rdb) {
    if (!rdb) return NULL;

    if (rdb->ReparseTag == IO_REPARSE_TAG_SYMLINK) {
        const RPS_SYMLINK_REPARSE_BUFFER* b = &rdb->SymbolicLinkReparseBuffer;
        const wchar_t* buf = b->PathBuffer;
        USHORT prnOff = b->PrintNameOffset;
        USHORT prnLen = b->PrintNameLength;
        USHORT subOff = b->SubstituteNameOffset;
        USHORT subLen = b->SubstituteNameLength;
        if (prnLen) return dup_wstr_range((const wchar_t*)((const BYTE*)buf + prnOff), prnLen / sizeof(wchar_t));
        if (subLen) return dup_wstr_range((const wchar_t*)((const BYTE*)buf + subOff), subLen / sizeof(wchar_t));
        return NULL;
    }

    if (rdb->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT) {
        const RPS_MOUNT_POINT_REPARSE_BUFFER* b = &rdb->MountPointReparseBuffer;
        const wchar_t* buf = b->PathBuffer;
        USHORT prnOff = b->PrintNameOffset;
        USHORT prnLen = b->PrintNameLength;
        USHORT subOff = b->SubstituteNameOffset;
        USHORT subLen = b->SubstituteNameLength;
        if (prnLen) return dup_wstr_range((const wchar_t*)((const BYTE*)buf + prnOff), prnLen / sizeof(wchar_t));
        if (subLen) return dup_wstr_range((const wchar_t*)((const BYTE*)buf + subOff), subLen / sizeof(wchar_t));
        return NULL;
    }

    return NULL;
}

static bool get_reparse_info(const wchar_t* path, bool is_dir, REPARSE_INFO* out, DWORD* out_err) {
    if (out_err) *out_err = 0;
    if (!out) return false;
    ZeroMemory(out, sizeof(*out));

    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT;
    if (is_dir) flags |= FILE_FLAG_BACKUP_SEMANTICS;

    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, flags, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (out_err) *out_err = GetLastError();
        return false;
    }

    DWORD bufSize = 16 * 1024;
    BYTE* buf = (BYTE*)malloc(bufSize);
    if (!buf) {
        CloseHandle(h);
        if (out_err) *out_err = ERROR_OUTOFMEMORY;
        return false;
    }

    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, NULL, 0, buf, bufSize, &bytes, NULL);
    if (!ok) {
        if (out_err) *out_err = GetLastError();
        free(buf);
        CloseHandle(h);
        return false;
    }

    RPS_REPARSE_DATA_BUFFER* rdb = (RPS_REPARSE_DATA_BUFFER*)buf;
    out->tag = rdb->ReparseTag;
    out->target = extract_reparse_target(rdb);

    free(buf);
    CloseHandle(h);
    return true;
}

// ============================
// Scan
// ============================

typedef struct {
    bool recursive;
    bool want_files;
    bool want_dirs;
    bool paths_only;
    bool stats;
    bool verbose;
    bool limit_depth;
    uint32_t max_depth;
} OPT;

typedef struct {
    uint64_t scanned;
    uint64_t found;
} STATS;

static void print_reparse(const wchar_t* path, const REPARSE_INFO* info, const OPT* opt) {
    if (opt->paths_only) {
        outf(L"%ls\n", path);
        return;
    }

    const wchar_t* name = reparse_tag_name(info->tag);
    if (!name) name = L"(unknown)";

    if (info->target) {
        outf(L"%ls 0x%08lX %ls -> %ls\n", name, info->tag, path, info->target);
    }
    else {
        outf(L"%ls 0x%08lX %ls\n", name, info->tag, path);
    }
}

static void scan_entry(const wchar_t* full, const WIN32_FIND_DATAW* fd, const OPT* opt, STATS* st) {
    bool is_dir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool is_reparse = (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    if (!opt->want_dirs && is_dir) return;
    if (!opt->want_files && !is_dir) return;

    st->scanned++;

    if (!is_reparse) return;

    REPARSE_INFO info;
    DWORD err = 0;
    if (!get_reparse_info(full, is_dir, &info, &err)) {
        if (opt->verbose) errf(L"rpscan: failed to query reparse info for '%ls' (GLE=%lu)\n", full, err);
        return;
    }

    st->found++;
    print_reparse(full, &info, opt);
    if (info.target) free(info.target);
}

static void scan_dir(const wchar_t* dir, const OPT* opt, STATS* st, uint32_t depth) {
    wchar_t* pat = path_join_alloc(dir, L"*");
    if (!pat) return;

    WIN32_FIND_DATAW fd;
    HANDLE hf = FindFirstFileW(pat, &fd);
    free(pat);

    if (hf == INVALID_HANDLE_VALUE) {
        if (opt->verbose) errf(L"rpscan: cannot enumerate '%ls' (GLE=%lu)\n", dir, GetLastError());
        return;
    }

    do {
        const wchar_t* name = fd.cFileName;
        if (wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0) continue;

        wchar_t* full = path_join_alloc(dir, name);
        if (!full) continue;

        scan_entry(full, &fd, opt, st);

        bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool is_reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        if (opt->recursive && is_dir && !is_reparse) {
            if (!opt->limit_depth || depth + 1 <= opt->max_depth) {
                scan_dir(full, opt, st, depth + 1);
            }
        }

        free(full);

    } while (FindNextFileW(hf, &fd));

    FindClose(hf);
}

static void scan_path(const wchar_t* path, const OPT* opt, STATS* st) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        errf(L"rpscan: cannot access '%ls' (GLE=%lu)\n", path, GetLastError());
        return;
    }

    bool is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool is_reparse = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    if (!is_dir) {
        if (opt->want_files) {
            st->scanned++;
            if (is_reparse) {
                REPARSE_INFO info;
                DWORD err = 0;
                if (get_reparse_info(path, false, &info, &err)) {
                    st->found++;
                    print_reparse(path, &info, opt);
                    if (info.target) free(info.target);
                }
                else if (opt->verbose) {
                    errf(L"rpscan: failed to query reparse info for '%ls' (GLE=%lu)\n", path, err);
                }
            }
        }
        return;
    }

    if (opt->want_dirs) {
        st->scanned++;
        if (is_reparse) {
            REPARSE_INFO info;
            DWORD err = 0;
            if (get_reparse_info(path, true, &info, &err)) {
                st->found++;
                print_reparse(path, &info, opt);
                if (info.target) free(info.target);
            }
            else if (opt->verbose) {
                errf(L"rpscan: failed to query reparse info for '%ls' (GLE=%lu)\n", path, err);
            }
        }
    }

    // Scan children. Root itself being a reparse point is never traversed.
    if (!is_reparse) {
        scan_dir(path, opt, st, 0);
    }
}

// ============================
// Help
// ============================

static void print_usage(void) {
    outw(L"rpscan - reparse-point scanner for Windows paths\n\n");
    outw(L"Usage:\n");
    outw(L"  rpscan <path> [--recursive] [--max-depth N]\n");
    outw(L"               [--files] [--dirs] [--paths]\n");
    outw(L"               [--stats] [--verbose]\n\n");

    outw(L"Notes:\n");
    outw(L"  - Reparse points are reported; directories that are reparse points are not traversed.\n");
    outw(L"  - Without --recursive, only immediate children are scanned when <path> is a directory.\n\n");

    outw(L"Examples:\n");
    outw(L"  rpscan C:\\work\\repo --recursive\n");
    outw(L"  rpscan C:\\work\\repo --recursive --paths\n");
    outw(L"  rpscan C:\\work\\repo --max-depth 1 --stats\n");
}

// ============================
// Main
// ============================

int wmain(int argc, wchar_t** argv) {
    io_init();

    OPT opt;
    ZeroMemory(&opt, sizeof(opt));
    opt.want_files = true;
    opt.want_dirs = true;

    const wchar_t* path = NULL;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!a || !*a) continue;

        if (streqi(a, L"-h") || streqi(a, L"--help") || streqi(a, L"/?")) {
            print_usage();
            return 0;
        }

        if (streqi(a, L"--recursive")) { opt.recursive = true; continue; }
        if (streqi(a, L"--files")) { opt.want_files = true; opt.want_dirs = false; continue; }
        if (streqi(a, L"--dirs")) { opt.want_dirs = true; opt.want_files = false; continue; }
        if (streqi(a, L"--paths")) { opt.paths_only = true; continue; }
        if (streqi(a, L"--stats")) { opt.stats = true; continue; }
        if (streqi(a, L"--verbose")) { opt.verbose = true; continue; }

        if (streqi(a, L"--max-depth")) {
            if (i + 1 >= argc) { errw(L"rpscan: --max-depth requires a value\n"); return 2; }
            ++i;
            uint32_t v = 0;
            if (!parse_u32(argv[i], &v)) { errw(L"rpscan: invalid --max-depth\n"); return 2; }
            opt.limit_depth = true;
            opt.max_depth = v;
            continue;
        }

        if (a[0] == L'-') {
            errf(L"rpscan: unknown option '%ls'\n", a);
            return 2;
        }

        if (!path) { path = a; continue; }
        errw(L"rpscan: too many positional arguments\n");
        return 2;
    }

    if (!path) {
        print_usage();
        return 2;
    }

    STATS st;
    ZeroMemory(&st, sizeof(st));

    scan_path(path, &opt, &st);

    if (opt.stats) {
        outf(L"scanned: %llu\n", (unsigned long long)st.scanned);
        outf(L"reparse: %llu\n", (unsigned long long)st.found);
    }

    return 0;
}
