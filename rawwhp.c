// rawwhp.c
// Execute a raw binary inside a Windows Hypervisor Platform (WHP) guest.
//
// Build (MSVC):
//   cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rawwhp.c WinHvPlatform.lib
//
// Build (clang-cl):
//   clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rawwhp.c WinHvPlatform.lib
//
// Usage:
//   rawwhp.exe [/to:<hex|seg:off>] [/at:<hex|seg:off>] [/bytes:<hex>]
//              [/mode:real|unreal|protected|long] [/ticks:<hex>] file
//
// Examples:
//   rawwhp /to 7C00 mymbr.bin
//   rawwhp /to 80:0 /at 80:100 /bytes 4000 test.com
//   rawwhp /to 800 /mode protected kernel32.bin

#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <WinHvPlatform.h>
#include <wchar.h>
#include <wctype.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>

#define PAGE_SIZE_4K 0x1000ull
#define PAGE_SIZE_2M 0x200000ull
#define STACK_SIZE   0x4000ull
#define GDT_SIZE     0x1000ull

#define X64_CR0_PE (1ull << 0)
#define X64_CR0_ET (1ull << 4)
#define X64_CR0_NE (1ull << 5)
#define X64_CR0_PG (1ull << 31)
#define X64_CR4_PAE (1ull << 5)
#define X64_EFER_LME (1ull << 8)

#define PT_PRESENT (1ull << 0)
#define PT_WRITE   (1ull << 1)
#define PT_USER    (1ull << 2)
#define PT_PS      (1ull << 7)

#define SEL_CODE32_U 0x1B
#define SEL_DATA32_U 0x23
#define SEL_CODE64_U 0x2B
#define SEL_DATA64_U 0x33

typedef enum {
    MODE_REAL = 0,
    MODE_UNREAL = 1,
    MODE_PROTECTED = 2,
    MODE_LONG = 3
} GUEST_MODE;

typedef enum {
    PARSE_OK = 0,
    PARSE_HELP = 1,
    PARSE_ERROR = 2
} PARSE_RESULT;

typedef struct {
    uint64_t load_addr;
    uint64_t exec_addr;
    uint64_t bytes;
    uint64_t ticks;
    bool have_exec_addr;
    bool have_bytes;
    GUEST_MODE mode;
    const wchar_t* file_path;
} OPTIONS;

typedef struct {
    uint64_t logical_start;
    uint64_t logical_end;
    uint64_t map_start;
    uint64_t map_end;
    uint64_t map_size;
    uint64_t map2m_start;
    uint64_t map2m_end;
    uint64_t user_high;
    uint64_t stack_base;
    uint64_t stack_top;
    bool has_gdt;
    uint64_t gdt_gpa;
    bool has_tables;
    uint32_t table_pages;
    uint32_t pdpt_pages;
    uint32_t pd_pages;
    uint64_t tables_base;
    uint64_t pml4_gpa;
} GUEST_LAYOUT;

typedef struct {
    uint16_t pml4_index;
    uint64_t gpa;
    uint64_t* entries;
} PDPT_INFO;

typedef struct {
    uint16_t pml4_index;
    uint16_t pdpt_index;
    uint64_t gpa;
    uint64_t* entries;
} PD_INFO;

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

    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need);
    if (!buf) return;

    (void)WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, need, NULL, NULL);
    {
        DWORD written = 0;
        (void)WriteFile(h, buf, (DWORD)(need - 1), &written, NULL);
    }
    HeapFree(GetProcessHeap(), 0, buf);
}

static void vfmt_to(HANDLE h, bool is_console, const wchar_t* fmt, va_list ap) {
    wchar_t stackbuf[2048];
    va_list copy;
    va_copy(copy, ap);
    int n = _vsnwprintf_s(stackbuf, _countof(stackbuf), _TRUNCATE, fmt, copy);
    va_end(copy);
    if (n >= 0) {
        write_w(h, is_console, stackbuf);
        return;
    }

    size_t cap = 8192;
    wchar_t* dyn = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (!dyn) return;

    va_copy(copy, ap);
    (void)_vsnwprintf_s(dyn, cap, _TRUNCATE, fmt, copy);
    va_end(copy);
    write_w(h, is_console, dyn);
    HeapFree(GetProcessHeap(), 0, dyn);
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

static void outw(const wchar_t* s) {
    write_w(GetStdHandle(STD_OUTPUT_HANDLE), g_stdout_is_console, s);
}

static void errw(const wchar_t* s) {
    write_w(GetStdHandle(STD_ERROR_HANDLE), g_stderr_is_console, s);
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

static uint64_t u64_min(uint64_t a, uint64_t b) {
    return (a < b) ? a : b;
}

static uint64_t u64_max(uint64_t a, uint64_t b) {
    return (a > b) ? a : b;
}

static bool add_u64_checked(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out) return false;
    if (UINT64_MAX - a < b) return false;
    *out = a + b;
    return true;
}

static bool sub_u64_checked(uint64_t a, uint64_t b, uint64_t* out) {
    if (!out) return false;
    if (a < b) return false;
    *out = a - b;
    return true;
}

static uint64_t align_down_u64(uint64_t v, uint64_t align) {
    return v & ~(align - 1ull);
}

static bool align_up_u64_checked(uint64_t v, uint64_t align, uint64_t* out) {
    uint64_t add = align - 1ull;
    uint64_t tmp = 0;
    if (!add_u64_checked(v, add, &tmp)) return false;
    *out = tmp & ~add;
    return true;
}

static bool chunk_count(uint64_t start, uint64_t end_exclusive, unsigned shift, uint64_t* out) {
    if (!out) return false;
    if (end_exclusive <= start) {
        *out = 0;
        return true;
    }
    uint64_t first = start >> shift;
    uint64_t last = (end_exclusive - 1ull) >> shift;
    *out = (last - first) + 1ull;
    return true;
}

static bool parse_hex_u64_strict(const wchar_t* s, uint64_t* out) {
    if (!s || !*s || !out) return false;

    const wchar_t* p = s;
    if (starts_with_i(p, L"0x")) p += 2;
    if (!*p) return false;

    for (const wchar_t* t = p; *t; ++t) {
        if (!iswxdigit(*t)) return false;
    }

    errno = 0;
    wchar_t* end = NULL;
    unsigned long long v = wcstoull(p, &end, 16);
    if (end == p || errno == ERANGE) return false;
    if (*end != 0) return false;

    *out = (uint64_t)v;
    return true;
}

static bool parse_hex_u32_strict(const wchar_t* s, uint32_t* out) {
    uint64_t v = 0;
    if (!parse_hex_u64_strict(s, &v)) return false;
    if (v > 0xFFFFFFFFull) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_address_value(const wchar_t* s, uint64_t* out) {
    if (!s || !*s || !out) return false;
    const wchar_t* colon = wcschr(s, L':');
    if (!colon) {
        return parse_hex_u64_strict(s, out);
    }

    if (wcschr(colon + 1, L':') != NULL) return false;

    size_t left_len = (size_t)(colon - s);
    if (left_len == 0) return false;
    if (*(colon + 1) == 0) return false;
    if (left_len >= 64) return false;

    wchar_t left[64];
    wmemcpy(left, s, left_len);
    left[left_len] = 0;

    uint32_t seg = 0;
    uint32_t off = 0;
    if (!parse_hex_u32_strict(left, &seg)) return false;
    if (!parse_hex_u32_strict(colon + 1, &off)) return false;
    if (seg > 0xFFFFu) return false;

    *out = ((uint64_t)seg << 4) + (uint64_t)off;
    return true;
}

static const wchar_t* mode_name(GUEST_MODE mode) {
    switch (mode) {
    case MODE_REAL: return L"real";
    case MODE_UNREAL: return L"unreal";
    case MODE_PROTECTED: return L"protected";
    case MODE_LONG: return L"long";
    default: return L"unknown";
    }
}

static bool parse_mode_name(const wchar_t* s, GUEST_MODE* out) {
    if (!s || !out) return false;
    if (streqi(s, L"real")) {
        *out = MODE_REAL;
        return true;
    }
    if (streqi(s, L"unreal")) {
        *out = MODE_UNREAL;
        return true;
    }
    if (streqi(s, L"protected")) {
        *out = MODE_PROTECTED;
        return true;
    }
    if (streqi(s, L"long")) {
        *out = MODE_LONG;
        return true;
    }
    return false;
}

static void print_usage(void) {
    outw(
        L"Execute raw binary data file in WHP guest.\n"
        L"\n"
        L"rawwhp.exe [/to:<hex|seg:off>] [/at:<hex|seg:off>] [/bytes:<hex>]\n"
        L"           [/mode:real|unreal|protected|long] [/ticks:<hex>] file\n"
        L"\n"
        L"/to <value>       base absolute address to load binary data (default: 0)\n"
        L"/at <value>       execute at this absolute address (default: /to)\n"
        L"/bytes <value>    guest memory window size from /to (aliases: /len, /size)\n"
        L"/mode <name>      real, unreal, protected, long (default: real)\n"
        L"/ticks <value>    max WHvRunVirtualProcessor iterations (default: 0x100000)\n"
        L"\n"
        L"All numeric values are hexadecimal. Accepts plain hex and 0x-prefixed hex.\n"
        L"Addresses also accept seg:off form (e.g., 80:100).\n"
        L"\n"
        L"Examples:\n"
        L"  rawwhp /to 7C00 mymbr.bin\n"
        L"  rawwhp /to 80:0 /at 80:100 /bytes 4000 test.com\n"
        L"  rawwhp /to 800 /mode protected kernel32.bin\n"
    );
}

static void split_option_name(const wchar_t* arg, wchar_t* name, size_t cap, const wchar_t** inline_value) {
    const wchar_t* p = arg;
    while (*p == L'/' || *p == L'-') p++;

    const wchar_t* colon = wcschr(p, L':');
    if (!colon) {
        wcsncpy_s(name, cap, p, _TRUNCATE);
        *inline_value = NULL;
        return;
    }

    size_t len = (size_t)(colon - p);
    if (len >= cap) len = cap - 1;
    wmemcpy(name, p, len);
    name[len] = 0;
    *inline_value = colon + 1;
}

static PARSE_RESULT parse_args(int argc, wchar_t** argv, OPTIONS* o) {
    if (!o) return PARSE_ERROR;

    ZeroMemory(o, sizeof(*o));
    o->load_addr = 0;
    o->exec_addr = 0;
    o->ticks = 0x100000ull;
    o->mode = MODE_REAL;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!a || !*a) continue;

        if (streqi(a, L"/?") || streqi(a, L"-?") || streqi(a, L"/h") || streqi(a, L"-h") ||
            streqi(a, L"/help") || streqi(a, L"-help") || streqi(a, L"--help")) {
            return PARSE_HELP;
        }

        if (a[0] == L'/' || a[0] == L'-') {
            wchar_t key[32];
            const wchar_t* inline_value = NULL;
            split_option_name(a, key, _countof(key), &inline_value);

            const wchar_t* value = inline_value;
            if (!value) {
                if (i + 1 >= argc) {
                    errf(L"missing value for option: %ls\n", a);
                    return PARSE_ERROR;
                }
                value = argv[++i];
            }

            if (streqi(key, L"to")) {
                if (!parse_address_value(value, &o->load_addr)) {
                    errf(L"invalid /to value: %ls\n", value);
                    return PARSE_ERROR;
                }
                continue;
            }

            if (streqi(key, L"at")) {
                if (!parse_address_value(value, &o->exec_addr)) {
                    errf(L"invalid /at value: %ls\n", value);
                    return PARSE_ERROR;
                }
                o->have_exec_addr = true;
                continue;
            }

            if (streqi(key, L"bytes") || streqi(key, L"len") || streqi(key, L"size")) {
                if (!parse_hex_u64_strict(value, &o->bytes) || o->bytes == 0) {
                    errf(L"invalid /bytes value: %ls\n", value);
                    return PARSE_ERROR;
                }
                o->have_bytes = true;
                continue;
            }

            if (streqi(key, L"mode")) {
                if (!parse_mode_name(value, &o->mode)) {
                    errf(L"invalid /mode value: %ls\n", value);
                    return PARSE_ERROR;
                }
                continue;
            }

            if (streqi(key, L"ticks")) {
                if (!parse_hex_u64_strict(value, &o->ticks)) {
                    errf(L"invalid /ticks value: %ls\n", value);
                    return PARSE_ERROR;
                }
                continue;
            }

            errf(L"unknown option: %ls\n", a);
            return PARSE_ERROR;
        }

        if (o->file_path) {
            errf(L"multiple input files provided: %ls\n", a);
            return PARSE_ERROR;
        }
        o->file_path = a;
    }

    if (!o->file_path) {
        errw(L"missing input file\n");
        return PARSE_ERROR;
    }

    if (!o->have_exec_addr) {
        o->exec_addr = o->load_addr;
    }

    return PARSE_OK;
}

static bool read_entire_file(const wchar_t* path, uint8_t** out_data, uint64_t* out_size) {
    if (!path || !out_data || !out_size) return false;
    *out_data = NULL;
    *out_size = 0;

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errf(L"CreateFileW failed for %ls: %lu\n", path, (unsigned long)GetLastError());
        return false;
    }

    LARGE_INTEGER sizeLi;
    if (!GetFileSizeEx(h, &sizeLi)) {
        errf(L"GetFileSizeEx failed: %lu\n", (unsigned long)GetLastError());
        CloseHandle(h);
        return false;
    }
    if (sizeLi.QuadPart < 0) {
        errw(L"invalid negative file size\n");
        CloseHandle(h);
        return false;
    }

    uint64_t file_size = (uint64_t)sizeLi.QuadPart;
    if (file_size > (uint64_t)SIZE_MAX) {
        errw(L"file too large for process address space\n");
        CloseHandle(h);
        return false;
    }

    SIZE_T alloc_size = (SIZE_T)((file_size == 0) ? 1 : file_size);
    uint8_t* data = (uint8_t*)HeapAlloc(GetProcessHeap(), 0, alloc_size);
    if (!data) {
        errw(L"HeapAlloc failed while reading input file\n");
        CloseHandle(h);
        return false;
    }

    uint64_t done = 0;
    while (done < file_size) {
        DWORD chunk = (DWORD)u64_min(1u << 20, file_size - done);
        DWORD got = 0;
        if (!ReadFile(h, data + (SIZE_T)done, chunk, &got, NULL)) {
            errf(L"ReadFile failed: %lu\n", (unsigned long)GetLastError());
            HeapFree(GetProcessHeap(), 0, data);
            CloseHandle(h);
            return false;
        }
        if (got == 0) {
            errw(L"short read while reading input file\n");
            HeapFree(GetProcessHeap(), 0, data);
            CloseHandle(h);
            return false;
        }
        done += got;
    }

    CloseHandle(h);
    *out_data = data;
    *out_size = file_size;
    return true;
}

static bool check_realmode_range(const OPTIONS* o, uint64_t file_size, const GUEST_LAYOUT* l) {
    if (o->mode != MODE_REAL && o->mode != MODE_UNREAL) return true;

    uint64_t payload_end = 0;
    if (!add_u64_checked(o->load_addr, file_size, &payload_end)) return false;
    if (payload_end > 0x100000ull) return false;
    if (o->exec_addr >= 0x100000ull) return false;
    if (l->stack_top >= 0x100000ull) return false;
    return true;
}

static bool compute_layout(const OPTIONS* o, uint64_t file_size, GUEST_LAYOUT* l, wchar_t* err_msg, size_t err_cap) {
    if (!o || !l || !err_msg || err_cap == 0) return false;
    ZeroMemory(l, sizeof(*l));
    err_msg[0] = 0;

    uint64_t payload_end = 0;
    uint64_t exec_end = 0;
    if (!add_u64_checked(o->load_addr, file_size, &payload_end)) {
        wcsncpy_s(err_msg, err_cap, L"overflow while computing payload end", _TRUNCATE);
        return false;
    }
    if (!add_u64_checked(o->exec_addr, 1, &exec_end)) {
        wcsncpy_s(err_msg, err_cap, L"overflow while computing execution address end", _TRUNCATE);
        return false;
    }

    l->user_high = u64_max(payload_end, exec_end);
    l->logical_start = u64_min(o->load_addr, o->exec_addr);
    l->has_gdt = (o->mode != MODE_REAL);
    l->has_tables = (o->mode == MODE_LONG);

    if (o->have_bytes) {
        if (o->exec_addr < o->load_addr) {
            wcsncpy_s(err_msg, err_cap, L"/at must be >= /to when /bytes is specified", _TRUNCATE);
            return false;
        }
        l->logical_start = o->load_addr;
        if (!add_u64_checked(o->load_addr, o->bytes, &l->logical_end)) {
            wcsncpy_s(err_msg, err_cap, L"overflow while computing /bytes region end", _TRUNCATE);
            return false;
        }
        if (payload_end > l->logical_end) {
            wcsncpy_s(err_msg, err_cap, L"/bytes is smaller than input file payload", _TRUNCATE);
            return false;
        }
        if (o->exec_addr >= l->logical_end) {
            wcsncpy_s(err_msg, err_cap, L"/at is outside /bytes region", _TRUNCATE);
            return false;
        }
    }

    if (!align_up_u64_checked(l->logical_end, PAGE_SIZE_4K, &l->logical_end)) {
        wcsncpy_s(err_msg, err_cap, L"overflow while aligning logical end", _TRUNCATE);
        return false;
    }
    l->map_start = align_down_u64(l->logical_start, PAGE_SIZE_4K);

    uint64_t base_user_end = 0;
    if (!align_up_u64_checked(l->user_high, PAGE_SIZE_4K, &base_user_end)) {
        wcsncpy_s(err_msg, err_cap, L"overflow while aligning user end", _TRUNCATE);
        return false;
    }

    uint64_t pdpt_count = 0;
    uint64_t pd_count = 0;
    uint64_t table_pages = 0;
    uint64_t overhead = 0;

    if (!o->have_bytes) {
        l->logical_end = base_user_end;
    }

    if (l->has_tables) {
        table_pages = 1;
        for (int iter = 0; iter < 8; ++iter) {
            overhead = STACK_SIZE + GDT_SIZE + (table_pages * PAGE_SIZE_4K);

            if (!o->have_bytes) {
                if (!add_u64_checked(base_user_end, overhead, &l->logical_end)) {
                    wcsncpy_s(err_msg, err_cap, L"overflow while sizing long-mode layout", _TRUNCATE);
                    return false;
                }
            }

            if (!align_up_u64_checked(l->logical_end, PAGE_SIZE_4K, &l->map_end)) {
                wcsncpy_s(err_msg, err_cap, L"overflow while aligning map end", _TRUNCATE);
                return false;
            }

            l->map2m_start = align_down_u64(l->map_start, PAGE_SIZE_2M);
            if (!align_up_u64_checked(l->map_end, PAGE_SIZE_2M, &l->map2m_end)) {
                wcsncpy_s(err_msg, err_cap, L"overflow while aligning long-mode map range", _TRUNCATE);
                return false;
            }

            if (!chunk_count(l->map2m_start, l->map2m_end, 39, &pdpt_count) ||
                !chunk_count(l->map2m_start, l->map2m_end, 30, &pd_count)) {
                wcsncpy_s(err_msg, err_cap, L"failed counting long-mode page-table chunks", _TRUNCATE);
                return false;
            }

            uint64_t new_table_pages = 1ull + pdpt_count + pd_count;
            if (new_table_pages == table_pages) break;
            table_pages = new_table_pages;
        }

        l->pdpt_pages = (uint32_t)pdpt_count;
        l->pd_pages = (uint32_t)pd_count;
        l->table_pages = (uint32_t)table_pages;
    }
    else {
        overhead = STACK_SIZE + (l->has_gdt ? GDT_SIZE : 0);
        if (!o->have_bytes) {
            if (!add_u64_checked(base_user_end, overhead, &l->logical_end)) {
                wcsncpy_s(err_msg, err_cap, L"overflow while sizing layout", _TRUNCATE);
                return false;
            }
        }
        if (!align_up_u64_checked(l->logical_end, PAGE_SIZE_4K, &l->map_end)) {
            wcsncpy_s(err_msg, err_cap, L"overflow while aligning map end", _TRUNCATE);
            return false;
        }
    }

    if (!o->have_bytes && l->has_tables) {
        overhead = STACK_SIZE + GDT_SIZE + ((uint64_t)l->table_pages * PAGE_SIZE_4K);
        if (!add_u64_checked(base_user_end, overhead, &l->logical_end)) {
            wcsncpy_s(err_msg, err_cap, L"overflow while finalizing long-mode layout", _TRUNCATE);
            return false;
        }
        if (!align_up_u64_checked(l->logical_end, PAGE_SIZE_4K, &l->map_end)) {
            wcsncpy_s(err_msg, err_cap, L"overflow while aligning final map end", _TRUNCATE);
            return false;
        }
        l->map2m_start = align_down_u64(l->map_start, PAGE_SIZE_2M);
        if (!align_up_u64_checked(l->map_end, PAGE_SIZE_2M, &l->map2m_end)) {
            wcsncpy_s(err_msg, err_cap, L"overflow while aligning final long-mode map range", _TRUNCATE);
            return false;
        }
    }

    if (l->map_end <= l->map_start) {
        wcsncpy_s(err_msg, err_cap, L"invalid mapping window", _TRUNCATE);
        return false;
    }
    l->map_size = l->map_end - l->map_start;

    uint64_t cursor = align_down_u64(l->logical_end, PAGE_SIZE_4K);

    if (l->has_tables) {
        uint64_t tables_bytes = (uint64_t)l->table_pages * PAGE_SIZE_4K;
        if (!sub_u64_checked(cursor, tables_bytes, &cursor)) {
            wcsncpy_s(err_msg, err_cap, L"not enough room for page tables", _TRUNCATE);
            return false;
        }
        l->tables_base = cursor;
        l->pml4_gpa = l->tables_base;
    }

    if (l->has_gdt) {
        if (!sub_u64_checked(cursor, GDT_SIZE, &cursor)) {
            wcsncpy_s(err_msg, err_cap, L"not enough room for GDT", _TRUNCATE);
            return false;
        }
        l->gdt_gpa = cursor;
    }

    if (!sub_u64_checked(cursor, STACK_SIZE, &cursor)) {
        wcsncpy_s(err_msg, err_cap, L"not enough room for stack", _TRUNCATE);
        return false;
    }
    l->stack_base = cursor;
    l->stack_top = l->stack_base + STACK_SIZE - 16ull;

    if (l->stack_base < l->user_high) {
        wcsncpy_s(err_msg, err_cap, L"/bytes region is too small for payload + runtime scaffolding", _TRUNCATE);
        return false;
    }
    if (l->stack_base < l->logical_start) {
        wcsncpy_s(err_msg, err_cap, L"runtime scaffolding would underflow the logical region", _TRUNCATE);
        return false;
    }
    if (!check_realmode_range(o, file_size, l)) {
        wcsncpy_s(err_msg, err_cap, L"real/unreal mode currently requires payload, entry, and stack below 0x100000", _TRUNCATE);
        return false;
    }

    return true;
}

static void* gpa_to_host(uint8_t* mapped, const GUEST_LAYOUT* l, uint64_t gpa) {
    if (!mapped || !l) return NULL;
    if (gpa < l->map_start || gpa >= l->map_end) return NULL;
    uint64_t off = gpa - l->map_start;
    if (off > (uint64_t)SIZE_MAX) return NULL;
    return mapped + (SIZE_T)off;
}

static void write_gdt_descriptor(uint8_t* gdt, size_t index, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    uint64_t desc = 0;
    desc |= (uint64_t)(limit & 0xFFFFu);
    desc |= (uint64_t)(base & 0x00FFFFFFu) << 16;
    desc |= (uint64_t)access << 40;
    desc |= (uint64_t)((limit >> 16) & 0xFu) << 48;
    desc |= (uint64_t)(flags & 0x0Fu) << 52;
    desc |= (uint64_t)((base >> 24) & 0xFFu) << 56;
    ((uint64_t*)gdt)[index] = desc;
}

static void build_gdt(uint8_t* gdt) {
    ZeroMemory(gdt, (SIZE_T)GDT_SIZE);

    // Index 0: null descriptor.
    write_gdt_descriptor(gdt, 0, 0, 0, 0, 0);

    // Index 1/2: 16-bit ring-0 code/data.
    write_gdt_descriptor(gdt, 1, 0, 0xFFFFu, 0x9Au, 0x0u);
    write_gdt_descriptor(gdt, 2, 0, 0xFFFFu, 0x92u, 0x0u);

    // Index 3/4: 32-bit ring-3 code/data.
    write_gdt_descriptor(gdt, 3, 0, 0xFFFFFu, 0xFAu, 0xCu);
    write_gdt_descriptor(gdt, 4, 0, 0xFFFFFu, 0xF2u, 0xCu);

    // Index 5/6: 64-bit ring-3 code/data.
    write_gdt_descriptor(gdt, 5, 0, 0xFFFFFu, 0xFAu, 0xAu);
    write_gdt_descriptor(gdt, 6, 0, 0xFFFFFu, 0xF2u, 0xCu);
}

static bool build_identity_2m_page_tables(uint8_t* mapped, const GUEST_LAYOUT* l) {
    if (!mapped || !l || !l->has_tables) return false;

    PDPT_INFO* pdpt = NULL;
    PD_INFO* pd = NULL;
    if (l->pdpt_pages) {
        pdpt = (PDPT_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)l->pdpt_pages * sizeof(PDPT_INFO));
        if (!pdpt) {
            errw(L"HeapAlloc failed for PDPT metadata\n");
            return false;
        }
    }
    if (l->pd_pages) {
        pd = (PD_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)l->pd_pages * sizeof(PD_INFO));
        if (!pd) {
            errw(L"HeapAlloc failed for PD metadata\n");
            HeapFree(GetProcessHeap(), 0, pdpt);
            return false;
        }
    }

    uint64_t next = l->tables_base;
    uint64_t expected_end = l->tables_base + ((uint64_t)l->table_pages * PAGE_SIZE_4K);

    uint64_t* pml4 = (uint64_t*)gpa_to_host(mapped, l, next);
    if (!pml4) {
        errw(L"failed to map PML4 host pointer\n");
        HeapFree(GetProcessHeap(), 0, pd);
        HeapFree(GetProcessHeap(), 0, pdpt);
        return false;
    }
    ZeroMemory(pml4, (SIZE_T)PAGE_SIZE_4K);
    next += PAGE_SIZE_4K;

    uint32_t pdpt_used = 0;
    uint32_t pd_used = 0;

    for (uint64_t gpa = l->map2m_start; gpa < l->map2m_end; gpa += PAGE_SIZE_2M) {
        uint16_t i4 = (uint16_t)((gpa >> 39) & 0x1FFu);
        uint16_t i3 = (uint16_t)((gpa >> 30) & 0x1FFu);
        uint16_t i2 = (uint16_t)((gpa >> 21) & 0x1FFu);

        int pdpt_idx = -1;
        for (uint32_t i = 0; i < pdpt_used; ++i) {
            if (pdpt[i].pml4_index == i4) {
                pdpt_idx = (int)i;
                break;
            }
        }
        if (pdpt_idx < 0) {
            if (pdpt_used >= l->pdpt_pages) {
                errw(L"internal error: PDPT page count mismatch\n");
                HeapFree(GetProcessHeap(), 0, pd);
                HeapFree(GetProcessHeap(), 0, pdpt);
                return false;
            }
            uint64_t* pdpt_entries = (uint64_t*)gpa_to_host(mapped, l, next);
            if (!pdpt_entries) {
                errw(L"failed to map PDPT host pointer\n");
                HeapFree(GetProcessHeap(), 0, pd);
                HeapFree(GetProcessHeap(), 0, pdpt);
                return false;
            }
            ZeroMemory(pdpt_entries, (SIZE_T)PAGE_SIZE_4K);
            pdpt[pdpt_used].pml4_index = i4;
            pdpt[pdpt_used].gpa = next;
            pdpt[pdpt_used].entries = pdpt_entries;
            pml4[i4] = next | PT_PRESENT | PT_WRITE | PT_USER;
            pdpt_idx = (int)pdpt_used;
            pdpt_used++;
            next += PAGE_SIZE_4K;
        }

        int pd_idx = -1;
        for (uint32_t i = 0; i < pd_used; ++i) {
            if (pd[i].pml4_index == i4 && pd[i].pdpt_index == i3) {
                pd_idx = (int)i;
                break;
            }
        }
        if (pd_idx < 0) {
            if (pd_used >= l->pd_pages) {
                errw(L"internal error: PD page count mismatch\n");
                HeapFree(GetProcessHeap(), 0, pd);
                HeapFree(GetProcessHeap(), 0, pdpt);
                return false;
            }
            uint64_t* pd_entries = (uint64_t*)gpa_to_host(mapped, l, next);
            if (!pd_entries) {
                errw(L"failed to map PD host pointer\n");
                HeapFree(GetProcessHeap(), 0, pd);
                HeapFree(GetProcessHeap(), 0, pdpt);
                return false;
            }
            ZeroMemory(pd_entries, (SIZE_T)PAGE_SIZE_4K);
            pd[pd_used].pml4_index = i4;
            pd[pd_used].pdpt_index = i3;
            pd[pd_used].gpa = next;
            pd[pd_used].entries = pd_entries;
            pdpt[pdpt_idx].entries[i3] = next | PT_PRESENT | PT_WRITE | PT_USER;
            pd_idx = (int)pd_used;
            pd_used++;
            next += PAGE_SIZE_4K;
        }

        pd[pd_idx].entries[i2] = (gpa & ~(PAGE_SIZE_2M - 1ull)) | PT_PRESENT | PT_WRITE | PT_USER | PT_PS;
    }

    HeapFree(GetProcessHeap(), 0, pd);
    HeapFree(GetProcessHeap(), 0, pdpt);

    if (next != expected_end) {
        errf(L"internal table allocator mismatch (next=0x%llx expected=0x%llx)\n",
            (unsigned long long)next, (unsigned long long)expected_end);
        return false;
    }

    return true;
}

static WHV_X64_SEGMENT_REGISTER make_segment(uint16_t selector, uint64_t base, uint32_t limit,
    uint16_t seg_type, uint16_t dpl, uint16_t long_mode, uint16_t default_size, uint16_t granularity) {
    WHV_X64_SEGMENT_REGISTER s;
    ZeroMemory(&s, sizeof(s));
    s.Selector = selector;
    s.Base = base;
    s.Limit = limit;
    s.SegmentType = seg_type;
    s.NonSystemSegment = 1;
    s.DescriptorPrivilegeLevel = dpl;
    s.Present = 1;
    s.Available = 0;
    s.Long = long_mode;
    s.Default = default_size;
    s.Granularity = granularity;
    return s;
}

static void print_hresult(const wchar_t* where, HRESULT hr) {
    errf(L"%ls failed: HRESULT=0x%08lX\n", where, (unsigned long)hr);
}

static bool check_hypervisor_present(void) {
    WHV_CAPABILITY cap;
    ZeroMemory(&cap, sizeof(cap));
    UINT32 written = 0;
    HRESULT hr = WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap, (UINT32)sizeof(cap), &written);
    if (FAILED(hr)) {
        print_hresult(L"WHvGetCapability(WHvCapabilityCodeHypervisorPresent)", hr);
        return false;
    }
    if (!cap.HypervisorPresent) {
        errw(L"WHP hypervisor is not present/enabled on this system.\n");
        return false;
    }
    return true;
}

static bool set_partition_properties(WHV_PARTITION_HANDLE partition) {
    WHV_PARTITION_PROPERTY prop;
    ZeroMemory(&prop, sizeof(prop));
    prop.ProcessorCount = 1;

    HRESULT hr = WHvSetPartitionProperty(
        partition,
        WHvPartitionPropertyCodeProcessorCount,
        &prop.ProcessorCount,
        (UINT32)sizeof(prop.ProcessorCount));
    if (FAILED(hr)) {
        print_hresult(L"WHvSetPartitionProperty(ProcessorCount)", hr);
        return false;
    }

    WHV_CAPABILITY caps;
    UINT32 written = 0;
    ZeroMemory(&caps, sizeof(caps));
    hr = WHvGetCapability(WHvCapabilityCodeExtendedVmExits, &caps, (UINT32)sizeof(caps), &written);
    if (SUCCEEDED(hr)) {
        WHV_EXTENDED_VM_EXITS exits = caps.ExtendedVmExits;
        hr = WHvSetPartitionProperty(
            partition,
            WHvPartitionPropertyCodeExtendedVmExits,
            &exits,
            (UINT32)sizeof(exits));
        if (FAILED(hr)) {
            errw(L"warning: failed to set ExtendedVmExits; continuing with defaults.\n");
        }
    }

#if defined(_AMD64_)
    UINT64 all_exceptions = ~0ull;
    hr = WHvSetPartitionProperty(
        partition,
        WHvPartitionPropertyCodeExceptionExitBitmap,
        &all_exceptions,
        (UINT32)sizeof(all_exceptions));
    if (FAILED(hr)) {
        errw(L"warning: failed to set ExceptionExitBitmap; exception exits may be limited.\n");
    }
#endif

    return true;
}

static bool set_guest_registers(WHV_PARTITION_HANDLE partition, const OPTIONS* o, const GUEST_LAYOUT* l) {
    WHV_REGISTER_NAME names[32];
    WHV_REGISTER_VALUE values[32];
    UINT32 n = 0;

    WHV_X64_SEGMENT_REGISTER cs;
    WHV_X64_SEGMENT_REGISTER ss;
    WHV_X64_SEGMENT_REGISTER ds;
    WHV_X64_SEGMENT_REGISTER es;
    WHV_X64_SEGMENT_REGISTER fs;
    WHV_X64_SEGMENT_REGISTER gs;
    WHV_X64_TABLE_REGISTER gdtr;
    WHV_X64_TABLE_REGISTER idtr;
    ZeroMemory(&gdtr, sizeof(gdtr));
    ZeroMemory(&idtr, sizeof(idtr));

    uint64_t rip = o->exec_addr;
    uint64_t rsp = l->stack_top;
    uint64_t rflags = 0x2ull;
    uint64_t cr0 = X64_CR0_ET;
    uint64_t cr3 = 0;
    uint64_t cr4 = 0;
    uint64_t efer = 0;

    if (o->mode == MODE_REAL || o->mode == MODE_UNREAL) {
        bool unreal = (o->mode == MODE_UNREAL);
        uint32_t seg_limit = unreal ? 0xFFFFFFFFu : 0xFFFFu;
        uint16_t seg_gran = unreal ? 1 : 0;

        uint16_t cs_sel = (uint16_t)((o->exec_addr >> 4) & 0xFFFFu);
        uint64_t cs_base = (uint64_t)cs_sel << 4;
        rip = o->exec_addr - cs_base;

        uint16_t data_sel = (uint16_t)((o->load_addr >> 4) & 0xFFFFu);
        uint64_t data_base = (uint64_t)data_sel << 4;

        uint16_t ss_sel = (uint16_t)((l->stack_base >> 4) & 0xFFFFu);
        uint64_t ss_base = (uint64_t)ss_sel << 4;
        rsp = l->stack_top - ss_base;

        cs = make_segment(cs_sel, cs_base, seg_limit, 0xB, 0, 0, 0, seg_gran);
        ds = make_segment(data_sel, data_base, seg_limit, 0x3, 0, 0, 0, seg_gran);
        es = ds;
        fs = ds;
        gs = ds;
        ss = make_segment(ss_sel, ss_base, seg_limit, 0x3, 0, 0, 0, seg_gran);

        cr0 = X64_CR0_ET;
        idtr.Base = 0;
        idtr.Limit = 0x03FF;
    }
    else if (o->mode == MODE_PROTECTED) {
        cs = make_segment(SEL_CODE32_U, 0, 0xFFFFFFFFu, 0xB, 3, 0, 1, 1);
        ds = make_segment(SEL_DATA32_U, 0, 0xFFFFFFFFu, 0x3, 3, 0, 1, 1);
        es = ds;
        fs = ds;
        gs = ds;
        ss = ds;

        cr0 = X64_CR0_PE | X64_CR0_ET | X64_CR0_NE;
    }
    else {
        cs = make_segment(SEL_CODE64_U, 0, 0xFFFFFFFFu, 0xB, 3, 1, 0, 1);
        ds = make_segment(SEL_DATA64_U, 0, 0xFFFFFFFFu, 0x3, 3, 0, 1, 1);
        es = ds;
        fs = ds;
        gs = ds;
        ss = ds;

        cr0 = X64_CR0_PE | X64_CR0_ET | X64_CR0_NE | X64_CR0_PG;
        cr3 = l->pml4_gpa;
        cr4 = X64_CR4_PAE;
        efer = X64_EFER_LME;
    }

    if (l->has_gdt) {
        gdtr.Base = l->gdt_gpa;
        gdtr.Limit = 0x37;
    }
    else {
        gdtr.Base = 0;
        gdtr.Limit = 0;
    }

#define ADD_REG64(name_, v_) do { \
    names[n] = (name_); \
    ZeroMemory(&values[n], sizeof(values[n])); \
    values[n].Reg64 = (uint64_t)(v_); \
    ++n; \
} while (0)

#define ADD_SEG(name_, seg_) do { \
    names[n] = (name_); \
    ZeroMemory(&values[n], sizeof(values[n])); \
    values[n].Segment = (seg_); \
    ++n; \
} while (0)

#define ADD_TABLE(name_, t_) do { \
    names[n] = (name_); \
    ZeroMemory(&values[n], sizeof(values[n])); \
    values[n].Table = (t_); \
    ++n; \
} while (0)

    ADD_REG64(WHvX64RegisterRip, rip);
    ADD_REG64(WHvX64RegisterRsp, rsp);
    ADD_REG64(WHvX64RegisterRflags, rflags);
    ADD_REG64(WHvX64RegisterCr0, cr0);
    ADD_REG64(WHvX64RegisterCr3, cr3);
    ADD_REG64(WHvX64RegisterCr4, cr4);
    ADD_REG64(WHvX64RegisterEfer, efer);
    ADD_SEG(WHvX64RegisterCs, cs);
    ADD_SEG(WHvX64RegisterSs, ss);
    ADD_SEG(WHvX64RegisterDs, ds);
    ADD_SEG(WHvX64RegisterEs, es);
    ADD_SEG(WHvX64RegisterFs, fs);
    ADD_SEG(WHvX64RegisterGs, gs);
    ADD_TABLE(WHvX64RegisterGdtr, gdtr);
    ADD_TABLE(WHvX64RegisterIdtr, idtr);

#undef ADD_REG64
#undef ADD_SEG
#undef ADD_TABLE

    HRESULT hr = WHvSetVirtualProcessorRegisters(partition, 0, names, n, values);
    if (FAILED(hr)) {
        print_hresult(L"WHvSetVirtualProcessorRegisters", hr);
        return false;
    }

    return true;
}

static const wchar_t* exit_reason_name(WHV_RUN_VP_EXIT_REASON reason) {
    switch (reason) {
    case WHvRunVpExitReasonNone: return L"None";
    case WHvRunVpExitReasonMemoryAccess: return L"MemoryAccess";
    case WHvRunVpExitReasonX64IoPortAccess: return L"X64IoPortAccess";
    case WHvRunVpExitReasonUnrecoverableException: return L"UnrecoverableException";
    case WHvRunVpExitReasonInvalidVpRegisterValue: return L"InvalidVpRegisterValue";
    case WHvRunVpExitReasonUnsupportedFeature: return L"UnsupportedFeature";
    case WHvRunVpExitReasonX64InterruptWindow: return L"X64InterruptWindow";
    case WHvRunVpExitReasonX64Halt: return L"X64Halt";
    case WHvRunVpExitReasonX64ApicEoi: return L"X64ApicEoi";
    case WHvRunVpExitReasonSynicSintDeliverable: return L"SynicSintDeliverable";
    case WHvRunVpExitReasonX64MsrAccess: return L"X64MsrAccess";
    case WHvRunVpExitReasonX64Cpuid: return L"X64Cpuid";
    case WHvRunVpExitReasonException: return L"Exception";
    case WHvRunVpExitReasonX64Rdtsc: return L"X64Rdtsc";
    case WHvRunVpExitReasonX64ApicSmiTrap: return L"X64ApicSmiTrap";
    case WHvRunVpExitReasonHypercall: return L"Hypercall";
    case WHvRunVpExitReasonX64ApicInitSipiTrap: return L"X64ApicInitSipiTrap";
    case WHvRunVpExitReasonX64ApicWriteTrap: return L"X64ApicWriteTrap";
    case WHvRunVpExitReasonCanceled: return L"Canceled";
    default: return L"Unknown";
    }
}

static void print_instruction_bytes(const UINT8* bytes, UINT8 count) {
    outw(L"instr=");
    if (!bytes || count == 0 || count > 16) {
        outw(L"<none>");
        return;
    }
    for (UINT8 i = 0; i < count; ++i) {
        outf(L"%02X", (unsigned)bytes[i]);
        if (i + 1 != count) outw(L" ");
    }
}

static void print_exit_details(const WHV_RUN_VP_EXIT_CONTEXT* e) {
    switch (e->ExitReason) {
    case WHvRunVpExitReasonMemoryAccess: {
        const WHV_MEMORY_ACCESS_CONTEXT* c = &e->MemoryAccess;
        const wchar_t* atype = L"unknown";
        if (c->AccessInfo.AccessType == WHvMemoryAccessRead) atype = L"read";
        else if (c->AccessInfo.AccessType == WHvMemoryAccessWrite) atype = L"write";
        else if (c->AccessInfo.AccessType == WHvMemoryAccessExecute) atype = L"execute";
        outf(L"  memory-access type=%ls gpa=0x%llx gva=0x%llx gva-valid=%u unmapped=%u ",
            atype,
            (unsigned long long)c->Gpa,
            (unsigned long long)c->Gva,
            (unsigned)c->AccessInfo.GvaValid,
            (unsigned)c->AccessInfo.GpaUnmapped);
        print_instruction_bytes(c->InstructionBytes, c->InstructionByteCount);
        outw(L"\n");
        if (c->Gpa < 0x400ull) {
            outw(L"  host-emulation-point: low memory (possible IVT/BDA access)\n");
        }
        else {
            outw(L"  host-emulation-point: guest memory access needs backing/emulation\n");
        }
        break;
    }
    case WHvRunVpExitReasonX64IoPortAccess: {
        const WHV_X64_IO_PORT_ACCESS_CONTEXT* c = &e->IoPortAccess;
        outf(L"  io-port is-write=%u size=%u port=0x%X ",
            (unsigned)c->AccessInfo.IsWrite,
            (unsigned)c->AccessInfo.AccessSize,
            (unsigned)c->PortNumber);
        print_instruction_bytes(c->InstructionBytes, c->InstructionByteCount);
        outw(L"\n");
        outw(L"  host-emulation-point: I/O port emulation required\n");
        break;
    }
    case WHvRunVpExitReasonX64MsrAccess: {
        const WHV_X64_MSR_ACCESS_CONTEXT* c = &e->MsrAccess;
        outf(L"  msr-access is-write=%u msr=0x%X rax=0x%llx rdx=0x%llx\n",
            (unsigned)c->AccessInfo.IsWrite,
            (unsigned)c->MsrNumber,
            (unsigned long long)c->Rax,
            (unsigned long long)c->Rdx);
        outw(L"  host-emulation-point: MSR emulation required\n");
        break;
    }
    case WHvRunVpExitReasonX64Cpuid: {
        const WHV_X64_CPUID_ACCESS_CONTEXT* c = &e->CpuidAccess;
        outf(L"  cpuid in: eax=0x%llx ecx=0x%llx\n",
            (unsigned long long)c->Rax,
            (unsigned long long)c->Rcx);
        outw(L"  host-emulation-point: CPUID handling required\n");
        break;
    }
    case WHvRunVpExitReasonException: {
        const WHV_VP_EXCEPTION_CONTEXT* c = &e->VpException;
        outf(L"  exception type=%u software=%u error-valid=%u error=0x%X param=0x%llx ",
            (unsigned)c->ExceptionType,
            (unsigned)c->ExceptionInfo.SoftwareException,
            (unsigned)c->ExceptionInfo.ErrorCodeValid,
            (unsigned)c->ErrorCode,
            (unsigned long long)c->ExceptionParameter);
        print_instruction_bytes(c->InstructionBytes, c->InstructionByteCount);
        outw(L"\n");
        outw(L"  host-emulation-point: exception injection/handling path\n");
        break;
    }
    case WHvRunVpExitReasonUnsupportedFeature: {
        const WHV_X64_UNSUPPORTED_FEATURE_CONTEXT* c = &e->UnsupportedFeature;
        outf(L"  unsupported-feature code=%u param=0x%llx\n",
            (unsigned)c->FeatureCode, (unsigned long long)c->FeatureParameter);
        break;
    }
    case WHvRunVpExitReasonX64Rdtsc: {
        const WHV_X64_RDTSC_CONTEXT* c = &e->ReadTsc;
        outf(L"  rdtsc tsc=0x%llx aux=0x%llx is-rdtscp=%u\n",
            (unsigned long long)c->Tsc,
            (unsigned long long)c->TscAux,
            (unsigned)c->RdtscInfo.IsRdtscp);
        break;
    }
    case WHvRunVpExitReasonHypercall: {
        const WHV_HYPERCALL_CONTEXT* c = &e->Hypercall;
        outf(L"  hypercall rax=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx\n",
            (unsigned long long)c->Rax,
            (unsigned long long)c->Rcx,
            (unsigned long long)c->Rdx,
            (unsigned long long)c->R8);
        break;
    }
    case WHvRunVpExitReasonCanceled: {
        outf(L"  canceled reason=%u\n", (unsigned)e->CancelReason.CancelReason);
        break;
    }
    case WHvRunVpExitReasonX64ApicWriteTrap: {
        outf(L"  apic-write type=0x%X value=0x%llx\n",
            (unsigned)e->ApicWrite.Type,
            (unsigned long long)e->ApicWrite.WriteValue);
        break;
    }
    case WHvRunVpExitReasonX64ApicEoi: {
        outf(L"  apic-eoi vector=%u\n", (unsigned)e->ApicEoi.InterruptVector);
        break;
    }
    case WHvRunVpExitReasonX64InterruptWindow: {
        outf(L"  interrupt-window deliverable-type=%u\n", (unsigned)e->InterruptWindow.DeliverableType);
        break;
    }
    case WHvRunVpExitReasonSynicSintDeliverable: {
        outf(L"  synic deliverable-sints=0x%X\n", (unsigned)e->SynicSintDeliverable.DeliverableSints);
        break;
    }
    default:
        outw(L"  no additional context decoder for this exit reason in v1\n");
        break;
    }
}

static bool query_reg64(WHV_PARTITION_HANDLE partition, WHV_REGISTER_NAME reg, uint64_t* out) {
    if (!out) return false;
    WHV_REGISTER_VALUE val;
    ZeroMemory(&val, sizeof(val));
    HRESULT hr = WHvGetVirtualProcessorRegisters(partition, 0, &reg, 1, &val);
    if (FAILED(hr)) return false;
    *out = val.Reg64;
    return true;
}

static int run_vcpu_loop(WHV_PARTITION_HANDLE partition, const OPTIONS* o) {
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx;
    ZeroMemory(&exit_ctx, sizeof(exit_ctx));

    if (o->ticks == 0) {
        errw(L"timeout: /ticks was set to 0\n");
        return 3;
    }

    for (uint64_t iter = 0; iter < o->ticks; ++iter) {
        HRESULT hr = WHvRunVirtualProcessor(partition, 0, &exit_ctx, (UINT32)sizeof(exit_ctx));
        if (FAILED(hr)) {
            print_hresult(L"WHvRunVirtualProcessor", hr);
            return 1;
        }

        uint64_t rsp = 0;
        if (!query_reg64(partition, WHvX64RegisterRsp, &rsp)) {
            rsp = 0;
        }
        if (o->mode == MODE_REAL || o->mode == MODE_UNREAL) {
            rsp &= 0xFFFFull;
        }

        outf(
            L"[tick 0x%llx] exit=%ls(0x%08X) rip=0x%llx rsp=0x%llx rflags=0x%llx cpl=%u\n",
            (unsigned long long)(iter + 1ull),
            exit_reason_name(exit_ctx.ExitReason),
            (unsigned)exit_ctx.ExitReason,
            (unsigned long long)exit_ctx.VpContext.Rip,
            (unsigned long long)rsp,
            (unsigned long long)exit_ctx.VpContext.Rflags,
            (unsigned)exit_ctx.VpContext.ExecutionState.Cpl);

        print_exit_details(&exit_ctx);

        if (exit_ctx.ExitReason == WHvRunVpExitReasonX64Halt ||
            exit_ctx.ExitReason == WHvRunVpExitReasonHypercall) {
            return 0;
        }

        if (exit_ctx.ExitReason == WHvRunVpExitReasonNone) {
            continue;
        }

        // v1 stops on first non-success exit and reports it for host-side emulation handling.
        return 4;
    }

    errf(L"timeout: exceeded /ticks limit (0x%llx)\n", (unsigned long long)o->ticks);
    return 3;
}

int wmain(int argc, wchar_t** argv) {
    io_init();

    OPTIONS opt;
    PARSE_RESULT pr = parse_args(argc, argv, &opt);
    if (pr == PARSE_HELP) {
        print_usage();
        return 0;
    }
    if (pr != PARSE_OK) {
        print_usage();
        return 2;
    }

    uint8_t* file_data = NULL;
    uint64_t file_size = 0;
    if (!read_entire_file(opt.file_path, &file_data, &file_size)) {
        return 1;
    }

    GUEST_LAYOUT layout;
    wchar_t layout_err[256];
    if (!compute_layout(&opt, file_size, &layout, layout_err, _countof(layout_err))) {
        errf(L"layout error: %ls\n", layout_err);
        HeapFree(GetProcessHeap(), 0, file_data);
        return 2;
    }

    outf(L"mode=%ls load=0x%llx exec=0x%llx file=0x%llx map=[0x%llx..0x%llx) size=0x%llx\n",
        mode_name(opt.mode),
        (unsigned long long)opt.load_addr,
        (unsigned long long)opt.exec_addr,
        (unsigned long long)file_size,
        (unsigned long long)layout.map_start,
        (unsigned long long)layout.map_end,
        (unsigned long long)layout.map_size);

    if (!check_hypervisor_present()) {
        HeapFree(GetProcessHeap(), 0, file_data);
        return 1;
    }

    if (layout.map_size > (uint64_t)SIZE_MAX) {
        errw(L"mapping size exceeds local addressable size_t\n");
        HeapFree(GetProcessHeap(), 0, file_data);
        return 1;
    }

    uint8_t* mapped = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)layout.map_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mapped) {
        errf(L"VirtualAlloc failed: %lu\n", (unsigned long)GetLastError());
        HeapFree(GetProcessHeap(), 0, file_data);
        return 1;
    }
    ZeroMemory(mapped, (SIZE_T)layout.map_size);

    void* payload_ptr = gpa_to_host(mapped, &layout, opt.load_addr);
    if (!payload_ptr) {
        errw(L"internal error: payload pointer out of mapping\n");
        VirtualFree(mapped, 0, MEM_RELEASE);
        HeapFree(GetProcessHeap(), 0, file_data);
        return 1;
    }
    if (file_size > 0) {
        CopyMemory(payload_ptr, file_data, (SIZE_T)file_size);
    }

    if (layout.has_gdt) {
        uint8_t* gdt_ptr = (uint8_t*)gpa_to_host(mapped, &layout, layout.gdt_gpa);
        if (!gdt_ptr) {
            errw(L"internal error: gdt pointer out of mapping\n");
            VirtualFree(mapped, 0, MEM_RELEASE);
            HeapFree(GetProcessHeap(), 0, file_data);
            return 1;
        }
        build_gdt(gdt_ptr);
    }

    if (layout.has_tables) {
        if (!build_identity_2m_page_tables(mapped, &layout)) {
            VirtualFree(mapped, 0, MEM_RELEASE);
            HeapFree(GetProcessHeap(), 0, file_data);
            return 1;
        }
    }

    WHV_PARTITION_HANDLE partition = NULL;
    bool vp_created = false;
    bool partition_setup = false;
    int exit_code = 1;

    HRESULT hr = WHvCreatePartition(&partition);
    if (FAILED(hr)) {
        print_hresult(L"WHvCreatePartition", hr);
        goto cleanup;
    }

    if (!set_partition_properties(partition)) {
        goto cleanup;
    }

    hr = WHvSetupPartition(partition);
    if (FAILED(hr)) {
        print_hresult(L"WHvSetupPartition", hr);
        goto cleanup;
    }
    partition_setup = true;

    hr = WHvMapGpaRange(
        partition,
        mapped,
        layout.map_start,
        layout.map_size,
        WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
    if (FAILED(hr)) {
        print_hresult(L"WHvMapGpaRange", hr);
        goto cleanup;
    }

    hr = WHvCreateVirtualProcessor(partition, 0, 0);
    if (FAILED(hr)) {
        print_hresult(L"WHvCreateVirtualProcessor", hr);
        goto cleanup;
    }
    vp_created = true;

    if (!set_guest_registers(partition, &opt, &layout)) {
        goto cleanup;
    }

    exit_code = run_vcpu_loop(partition, &opt);

cleanup:
    if (vp_created) {
        HRESULT dhr = WHvDeleteVirtualProcessor(partition, 0);
        if (FAILED(dhr)) {
            errw(L"warning: WHvDeleteVirtualProcessor failed during cleanup.\n");
        }
    }

    if (partition_setup) {
        HRESULT uhr = WHvUnmapGpaRange(partition, layout.map_start, layout.map_size);
        if (FAILED(uhr)) {
            errw(L"warning: WHvUnmapGpaRange failed during cleanup.\n");
        }
    }

    if (partition) {
        HRESULT phr = WHvDeletePartition(partition);
        if (FAILED(phr)) {
            errw(L"warning: WHvDeletePartition failed during cleanup.\n");
        }
    }

    VirtualFree(mapped, 0, MEM_RELEASE);
    HeapFree(GetProcessHeap(), 0, file_data);
    return exit_code;
}
