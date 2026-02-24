// rawwhp.c
// Execute guest code in a Windows Hypervisor Platform (WHP) partition with
// explicit guest physical memory areas and optional post-run dumps.
//
// Build (MSVC):
//   cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rawwhp.c WinHvPlatform.lib
//
// Build (clang-cl):
//   clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rawwhp.c WinHvPlatform.lib
//
// Usage:
//   rawwhp [/mode real|unreal|protected|long] [/cpl 0|3] [/ticks <hex>] [/pedantic]
//          [/at <hex|seg:off>]
//          [/report <file>]
//          /area <start> <length> [file]...
//          [/dump <start> <length> [file]]...
//
// Notes:
//   - /to, /bytes, /len, /size are intentionally removed.
//   - All numeric values are hexadecimal.

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
#define EXT_VM_EXIT_GPA_ACCESS_FAULT_MASK (1ull << 14)

#define PT_PRESENT (1ull << 0)
#define PT_WRITE   (1ull << 1)
#define PT_USER    (1ull << 2)
#define PT_PS      (1ull << 7)

#define SEL_CODE32_K 0x08
#define SEL_DATA32_K 0x10
#define SEL_CODE32_U 0x1B
#define SEL_DATA32_U 0x23
#define SEL_CODE64_U 0x2B
#define SEL_DATA64_U 0x33
#define SEL_CODE64_K 0x38

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
    uint64_t start;
    uint64_t length;
    bool has_file;
    const wchar_t* file_path;
    uint8_t* file_data;
    uint64_t file_size;
    uint64_t init_size;
} AREA_SPEC;

typedef struct {
    uint64_t start;
    uint64_t length;
    bool has_file;
    const wchar_t* file_path;
} DUMP_SPEC;

typedef struct {
    AREA_SPEC* items;
    size_t count;
    size_t cap;
} AREA_LIST;

typedef struct {
    DUMP_SPEC* items;
    size_t count;
    size_t cap;
} DUMP_LIST;

typedef struct {
    uint64_t start;
    uint64_t end;
} INTERVAL;

typedef struct {
    INTERVAL* items;
    size_t count;
    size_t cap;
} INTERVAL_LIST;

typedef struct {
    uint64_t map_start;
    uint64_t map_end;
    uint64_t map_size;
    uint8_t* host;
} MAP_SEGMENT;

typedef struct {
    MAP_SEGMENT* items;
    size_t count;
    size_t cap;
} MAP_LIST;

typedef struct {
    uint64_t start;
    uint64_t end;
} FREE_RANGE;

typedef struct {
    FREE_RANGE* items;
    size_t count;
    size_t cap;
} FREE_LIST;

typedef struct {
    bool enabled;
    uint16_t seg;
    uint16_t off;
    bool explicit_seg_off;
    uint64_t linear;
} ENTRY_POINT;

typedef struct {
    ENTRY_POINT entry;
    GUEST_MODE mode;
    bool cpl_explicit;
    uint8_t cpl;
    uint64_t ticks;
    bool pedantic;
    const wchar_t* report_path;
    AREA_LIST areas;
    DUMP_LIST dumps;
} OPTIONS;

typedef struct {
    bool has_runtime_area;
    uint64_t runtime_start;
    uint64_t runtime_length;
    bool has_gdt;
    bool has_tables;
    uint32_t table_pages;
    uint32_t pdpt_pages;
    uint32_t pd_pages;
    uint64_t stack_base;
    uint64_t stack_top;
    uint64_t gdt_gpa;
    uint64_t tables_base;
    uint64_t pml4_gpa;
} RUNTIME_LAYOUT;

typedef struct {
    bool extended_vm_exits_supported;
    uint64_t extended_vm_exits_supported_mask;
    bool extended_vm_exits_requested;
    uint64_t extended_vm_exits_requested_mask;
    bool extended_vm_exits_enabled;
    uint64_t extended_vm_exits_enabled_mask;
    bool exception_bitmap_supported;
    uint64_t exception_bitmap_supported_mask;
    bool exception_bitmap_requested;
    uint64_t exception_bitmap_requested_mask;
    bool exception_bitmap_enabled;
    uint64_t exception_bitmap_enabled_mask;
    bool msr_exit_bitmap_supported;
    uint64_t msr_exit_bitmap_supported_mask;
    bool msr_exit_bitmap_requested;
    uint64_t msr_exit_bitmap_requested_mask;
    bool msr_exit_bitmap_enabled;
    uint64_t msr_exit_bitmap_enabled_mask;
} PARTITION_CAPS;

typedef enum {
    RUN_STATUS_SETUP_ERROR = 0,
    RUN_STATUS_SUCCESS = 1,
    RUN_STATUS_VM_EXIT = 2
} RUN_STATUS;

typedef struct {
    RUN_STATUS status;
    bool has_exit_reason;
    WHV_RUN_VP_EXIT_REASON exit_reason;
    bool timed_out;
    bool has_run_hresult;
    HRESULT run_hresult;
    uint64_t ticks_executed;
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint8_t cpl;
    bool has_exit_context;
    WHV_RUN_VP_EXIT_CONTEXT exit_context;
} RUN_REPORT;

typedef struct {
    bool attempted;
    bool ok;
} DUMP_RESULT;

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
static bool g_vt_enabled = false;

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

    if (g_stdout_is_console) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            DWORD want = mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            if (SetConsoleMode(hOut, want)) {
                g_vt_enabled = true;
            }
        }
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

static const wchar_t* col_reset(void) {
    return g_vt_enabled ? L"\x1b[0m" : L"";
}
static const wchar_t* col_addr(void) {
    return g_vt_enabled ? L"\x1b[36m" : L"";
}
static const wchar_t* col_hex(void) {
    return g_vt_enabled ? L"\x1b[32m" : L"";
}
static const wchar_t* col_ascii(void) {
    return g_vt_enabled ? L"\x1b[33m" : L"";
}
static const wchar_t* col_title(void) {
    return g_vt_enabled ? L"\x1b[1;34m" : L"";
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

static bool is_option_token(const wchar_t* s) {
    if (!s || !*s) return false;
    return (s[0] == L'/' || s[0] == L'-');
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

static uint64_t align_down_nonzero(uint64_t v, uint64_t align) {
    return v & ~(align - 1ull);
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

typedef struct {
    bool had_colon;
    uint16_t seg;
    uint16_t off;
    uint64_t linear;
} PARSED_ADDR;

static bool parse_address_value(const wchar_t* s, PARSED_ADDR* out) {
    if (!s || !*s || !out) return false;
    ZeroMemory(out, sizeof(*out));

    const wchar_t* colon = wcschr(s, L':');
    if (!colon) {
        if (!parse_hex_u64_strict(s, &out->linear)) return false;
        return true;
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

    out->had_colon = true;
    out->seg = (uint16_t)seg;
    out->off = (uint16_t)(off & 0xFFFFu);
    out->linear = ((uint64_t)seg << 4) + (uint64_t)off;
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

static bool parse_cpl_value(const wchar_t* s, uint8_t* out) {
    if (!s || !out) return false;
    uint64_t v = 0;
    if (!parse_hex_u64_strict(s, &v)) return false;
    if (v != 0ull && v != 3ull) return false;
    *out = (uint8_t)v;
    return true;
}

static void print_usage(void) {
    outw(
        L"Execute guest code in a WHP partition using explicit GPA areas.\n"
        L"\n"
        L"rawwhp [/mode real|unreal|protected|long] [/cpl 0|3] [/ticks <hex>] [/pedantic]\n"
        L"       [/at <hex|seg:off>]\n"
        L"       [/report <file>]\n"
        L"       /area <start> <length> [file]...\n"
        L"       [/dump <start> <length> [file]]...\n"
        L"\n"
        L"/area <start> <len> [file]  map GPA area; optionally initialize from file\n"
        L"/dump <start> <len> [file]  dump GPA bytes after run (stdout or file)\n"
        L"/at <value>                 entry address (default: first /area start)\n"
        L"/report <file>              write run report JSON to file\n"
        L"/mode <name>                real | unreal | protected | long (default: real)\n"
        L"/cpl <0|3>                  protected/long privilege level (default: 3)\n"
        L"/ticks <hex>                max run-loop iterations (default: 0x100000)\n"
        L"/pedantic                   strict validation behavior\n"
        L"\n"
        L"All numeric values are hexadecimal.\n"
        L"Address forms: plain hex / 0x-prefixed hex / seg:off\n"
        L"\n"
        L"Examples:\n"
        L"  rawwhp /area 7C00 200 mymbr.bin /at 7C00\n"
        L"  rawwhp /area 8000 2000 code.bin /area 20000 1000 data.bin /mode protected /at 8000\n"
        L"  rawwhp /area 10000 4000 input.bin /dump 10000 4000 output.bin\n"
        L"  rawwhp /area 1000 100 /dump 1000 100\n"
    );
}

static bool area_list_push(AREA_LIST* l, const AREA_SPEC* a) {
    if (!l || !a) return false;
    if (l->count == l->cap) {
        size_t new_cap = (l->cap == 0) ? 8 : (l->cap * 2);
        AREA_SPEC* nv = (AREA_SPEC*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l->items, new_cap * sizeof(AREA_SPEC));
        if (!nv) {
            if (l->cap == 0) {
                nv = (AREA_SPEC*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_cap * sizeof(AREA_SPEC));
                if (!nv) return false;
            }
            else {
                return false;
            }
        }
        l->items = nv;
        l->cap = new_cap;
    }
    l->items[l->count++] = *a;
    return true;
}

static bool dump_list_push(DUMP_LIST* l, const DUMP_SPEC* d) {
    if (!l || !d) return false;
    if (l->count == l->cap) {
        size_t new_cap = (l->cap == 0) ? 8 : (l->cap * 2);
        DUMP_SPEC* nv = (DUMP_SPEC*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l->items, new_cap * sizeof(DUMP_SPEC));
        if (!nv) {
            if (l->cap == 0) {
                nv = (DUMP_SPEC*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_cap * sizeof(DUMP_SPEC));
                if (!nv) return false;
            }
            else {
                return false;
            }
        }
        l->items = nv;
        l->cap = new_cap;
    }
    l->items[l->count++] = *d;
    return true;
}

static bool interval_list_push(INTERVAL_LIST* l, uint64_t s, uint64_t e) {
    if (!l) return false;
    if (e <= s) return true;
    if (l->count == l->cap) {
        size_t new_cap = (l->cap == 0) ? 8 : (l->cap * 2);
        INTERVAL* nv = (INTERVAL*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l->items, new_cap * sizeof(INTERVAL));
        if (!nv) {
            if (l->cap == 0) {
                nv = (INTERVAL*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_cap * sizeof(INTERVAL));
                if (!nv) return false;
            }
            else {
                return false;
            }
        }
        l->items = nv;
        l->cap = new_cap;
    }
    l->items[l->count].start = s;
    l->items[l->count].end = e;
    l->count++;
    return true;
}

static bool map_list_push(MAP_LIST* l, const MAP_SEGMENT* m) {
    if (!l || !m) return false;
    if (l->count == l->cap) {
        size_t new_cap = (l->cap == 0) ? 8 : (l->cap * 2);
        MAP_SEGMENT* nv = (MAP_SEGMENT*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l->items, new_cap * sizeof(MAP_SEGMENT));
        if (!nv) {
            if (l->cap == 0) {
                nv = (MAP_SEGMENT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_cap * sizeof(MAP_SEGMENT));
                if (!nv) return false;
            }
            else {
                return false;
            }
        }
        l->items = nv;
        l->cap = new_cap;
    }
    l->items[l->count++] = *m;
    return true;
}

static bool free_list_push(FREE_LIST* l, uint64_t s, uint64_t e) {
    if (!l) return false;
    if (e <= s) return true;
    if (l->count == l->cap) {
        size_t new_cap = (l->cap == 0) ? 8 : (l->cap * 2);
        FREE_RANGE* nv = (FREE_RANGE*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, l->items, new_cap * sizeof(FREE_RANGE));
        if (!nv) {
            if (l->cap == 0) {
                nv = (FREE_RANGE*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, new_cap * sizeof(FREE_RANGE));
                if (!nv) return false;
            }
            else {
                return false;
            }
        }
        l->items = nv;
        l->cap = new_cap;
    }
    l->items[l->count].start = s;
    l->items[l->count].end = e;
    l->count++;
    return true;
}

static int cmp_interval_start(const void* a, const void* b) {
    const INTERVAL* ia = (const INTERVAL*)a;
    const INTERVAL* ib = (const INTERVAL*)b;
    if (ia->start < ib->start) return -1;
    if (ia->start > ib->start) return 1;
    if (ia->end < ib->end) return -1;
    if (ia->end > ib->end) return 1;
    return 0;
}

static int cmp_area_start(const void* a, const void* b) {
    const AREA_SPEC* ia = (const AREA_SPEC*)a;
    const AREA_SPEC* ib = (const AREA_SPEC*)b;
    if (ia->start < ib->start) return -1;
    if (ia->start > ib->start) return 1;
    if (ia->length < ib->length) return -1;
    if (ia->length > ib->length) return 1;
    return 0;
}

static int cmp_free_desc(const void* a, const void* b) {
    const FREE_RANGE* fa = (const FREE_RANGE*)a;
    const FREE_RANGE* fb = (const FREE_RANGE*)b;
    if (fa->end > fb->end) return -1;
    if (fa->end < fb->end) return 1;
    if (fa->start > fb->start) return -1;
    if (fa->start < fb->start) return 1;
    return 0;
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

typedef struct {
    uint64_t start;
    uint64_t length;
    bool has_file;
    const wchar_t* file_path;
} REGION_PARSE_RESULT;

static bool parse_region_tuple(int argc, wchar_t** argv, int* idx, REGION_PARSE_RESULT* out) {
    if (!idx || !out) return false;
    if (*idx + 2 >= argc) return false;

    const wchar_t* s_tok = argv[*idx + 1];
    const wchar_t* l_tok = argv[*idx + 2];
    uint64_t s = 0;
    uint64_t len = 0;
    if (!parse_hex_u64_strict(s_tok, &s)) return false;
    if (!parse_hex_u64_strict(l_tok, &len)) return false;
    if (len == 0) return false;

    uint64_t end = 0;
    if (!add_u64_checked(s, len, &end) || end <= s) return false;

    out->start = s;
    out->length = len;
    out->has_file = false;
    out->file_path = NULL;

    *idx += 2;
    if (*idx + 1 < argc) {
        const wchar_t* maybe_file = argv[*idx + 1];
        if (!is_option_token(maybe_file)) {
            out->has_file = true;
            out->file_path = maybe_file;
            *idx += 1;
        }
    }

    return true;
}

static PARSE_RESULT parse_args(int argc, wchar_t** argv, OPTIONS* o) {
    if (!o) return PARSE_ERROR;
    ZeroMemory(o, sizeof(*o));
    o->mode = MODE_REAL;
    o->cpl_explicit = false;
    o->cpl = 0;
    o->ticks = 0x100000ull;

    for (int i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if (!a || !*a) continue;

        if (streqi(a, L"/?") || streqi(a, L"-?") || streqi(a, L"/h") || streqi(a, L"-h") ||
            streqi(a, L"/help") || streqi(a, L"-help") || streqi(a, L"--help")) {
            return PARSE_HELP;
        }

        if (!is_option_token(a)) {
            return PARSE_ERROR;
        }

        wchar_t key[32];
        const wchar_t* inline_value = NULL;
        split_option_name(a, key, _countof(key), &inline_value);

        if (streqi(key, L"area")) {
            REGION_PARSE_RESULT r;
            if (!parse_region_tuple(argc, argv, &i, &r)) {
                return PARSE_ERROR;
            }
            AREA_SPEC area;
            ZeroMemory(&area, sizeof(area));
            area.start = r.start;
            area.length = r.length;
            area.has_file = r.has_file;
            area.file_path = r.file_path;
            if (!area_list_push(&o->areas, &area)) {
                errw(L"out of memory while parsing /area\n");
                return PARSE_ERROR;
            }
            continue;
        }

        if (streqi(key, L"dump")) {
            REGION_PARSE_RESULT r;
            if (!parse_region_tuple(argc, argv, &i, &r)) {
                return PARSE_ERROR;
            }
            DUMP_SPEC dump;
            ZeroMemory(&dump, sizeof(dump));
            dump.start = r.start;
            dump.length = r.length;
            dump.has_file = r.has_file;
            dump.file_path = r.file_path;
            if (!dump_list_push(&o->dumps, &dump)) {
                errw(L"out of memory while parsing /dump\n");
                return PARSE_ERROR;
            }
            continue;
        }

        if (streqi(key, L"mode")) {
            const wchar_t* v = inline_value;
            if (!v) {
                if (i + 1 >= argc) return PARSE_ERROR;
                v = argv[++i];
            }
            if (!parse_mode_name(v, &o->mode)) return PARSE_ERROR;
            continue;
        }

        if (streqi(key, L"ticks")) {
            const wchar_t* v = inline_value;
            if (!v) {
                if (i + 1 >= argc) return PARSE_ERROR;
                v = argv[++i];
            }
            if (!parse_hex_u64_strict(v, &o->ticks)) return PARSE_ERROR;
            continue;
        }

        if (streqi(key, L"cpl")) {
            const wchar_t* v = inline_value;
            if (!v) {
                if (i + 1 >= argc) return PARSE_ERROR;
                v = argv[++i];
            }
            uint8_t cpl = 0;
            if (!parse_cpl_value(v, &cpl)) return PARSE_ERROR;
            o->cpl = cpl;
            o->cpl_explicit = true;
            continue;
        }

        if (streqi(key, L"pedantic")) {
            o->pedantic = true;
            continue;
        }

        if (streqi(key, L"at")) {
            const wchar_t* v = inline_value;
            if (!v) {
                if (i + 1 >= argc) return PARSE_ERROR;
                v = argv[++i];
            }
            PARSED_ADDR pa;
            if (!parse_address_value(v, &pa)) return PARSE_ERROR;
            o->entry.enabled = true;
            o->entry.linear = pa.linear;
            o->entry.explicit_seg_off = pa.had_colon;
            o->entry.seg = pa.seg;
            o->entry.off = pa.off;
            continue;
        }

        if (streqi(key, L"report")) {
            const wchar_t* v = inline_value;
            if (!v) {
                if (i + 1 >= argc) return PARSE_ERROR;
                v = argv[++i];
            }
            if (!v || !*v) return PARSE_ERROR;
            o->report_path = v;
            continue;
        }

        return PARSE_ERROR;
    }

    if (o->areas.count == 0) return PARSE_ERROR;

    if (!o->entry.enabled) {
        if (o->pedantic) {
            return PARSE_ERROR;
        }
        o->entry.enabled = true;
        o->entry.linear = o->areas.items[0].start;
        o->entry.explicit_seg_off = false;
        o->entry.seg = 0;
        o->entry.off = 0;
    }

    if (!o->cpl_explicit) {
        o->cpl = (o->mode == MODE_PROTECTED || o->mode == MODE_LONG) ? 3u : 0u;
    }
    if ((o->mode == MODE_REAL || o->mode == MODE_UNREAL) && o->cpl != 0u) {
        return PARSE_ERROR;
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

static bool load_area_files(OPTIONS* o) {
    if (!o) return false;

    for (size_t i = 0; i < o->areas.count; ++i) {
        AREA_SPEC* a = &o->areas.items[i];
        if (!a->has_file) {
            a->file_data = NULL;
            a->file_size = 0;
            a->init_size = 0;
            continue;
        }

        if (!read_entire_file(a->file_path, &a->file_data, &a->file_size)) {
            return false;
        }

        a->init_size = u64_min(a->file_size, a->length);
        if (o->pedantic && a->file_size != a->length) {
            errf(L"warning: /area file-size mismatch at start=0x%llx len=0x%llx file=0x%llx\n",
                (unsigned long long)a->start,
                (unsigned long long)a->length,
                (unsigned long long)a->file_size);
        }
    }

    return true;
}

static void free_area_files(OPTIONS* o) {
    if (!o) return;
    for (size_t i = 0; i < o->areas.count; ++i) {
        if (o->areas.items[i].file_data) {
            HeapFree(GetProcessHeap(), 0, o->areas.items[i].file_data);
            o->areas.items[i].file_data = NULL;
        }
    }
}

static bool validate_area_non_overlapping(const OPTIONS* o) {
    if (!o || o->areas.count == 0) return false;

    AREA_SPEC* sorted = (AREA_SPEC*)HeapAlloc(GetProcessHeap(), 0, o->areas.count * sizeof(AREA_SPEC));
    if (!sorted) return false;

    CopyMemory(sorted, o->areas.items, o->areas.count * sizeof(AREA_SPEC));
    qsort(sorted, o->areas.count, sizeof(AREA_SPEC), cmp_area_start);

    bool ok = true;
    for (size_t i = 1; i < o->areas.count; ++i) {
        uint64_t prev_end = 0;
        if (!add_u64_checked(sorted[i - 1].start, sorted[i - 1].length, &prev_end)) {
            ok = false;
            break;
        }
        if (sorted[i].start < prev_end) {
            ok = false;
            break;
        }
    }

    HeapFree(GetProcessHeap(), 0, sorted);
    return ok;
}

static bool build_user_intervals(const OPTIONS* o, INTERVAL_LIST* out, uint64_t* out_high_end) {
    if (!o || !out || !out_high_end) return false;
    *out_high_end = 0;

    for (size_t i = 0; i < o->areas.count; ++i) {
        uint64_t end = 0;
        if (!add_u64_checked(o->areas.items[i].start, o->areas.items[i].length, &end)) {
            return false;
        }
        if (!interval_list_push(out, o->areas.items[i].start, end)) {
            return false;
        }
        *out_high_end = u64_max(*out_high_end, end);
    }

    return true;
}

static bool build_map_segments_from_intervals(const INTERVAL_LIST* src, MAP_LIST* out) {
    if (!src || !out) return false;
    if (src->count == 0) return true;

    INTERVAL* tmp = (INTERVAL*)HeapAlloc(GetProcessHeap(), 0, src->count * sizeof(INTERVAL));
    if (!tmp) return false;

    size_t tcount = 0;
    for (size_t i = 0; i < src->count; ++i) {
        uint64_t rs = align_down_u64(src->items[i].start, PAGE_SIZE_4K);
        uint64_t re = 0;
        if (!align_up_u64_checked(src->items[i].end, PAGE_SIZE_4K, &re)) {
            HeapFree(GetProcessHeap(), 0, tmp);
            return false;
        }
        if (re <= rs) continue;
        tmp[tcount].start = rs;
        tmp[tcount].end = re;
        tcount++;
    }

    if (tcount == 0) {
        HeapFree(GetProcessHeap(), 0, tmp);
        return true;
    }

    qsort(tmp, tcount, sizeof(INTERVAL), cmp_interval_start);

    uint64_t cur_s = tmp[0].start;
    uint64_t cur_e = tmp[0].end;
    for (size_t i = 1; i < tcount; ++i) {
        if (tmp[i].start <= cur_e) {
            if (tmp[i].end > cur_e) cur_e = tmp[i].end;
        }
        else {
            MAP_SEGMENT m;
            ZeroMemory(&m, sizeof(m));
            m.map_start = cur_s;
            m.map_end = cur_e;
            m.map_size = cur_e - cur_s;
            if (!map_list_push(out, &m)) {
                HeapFree(GetProcessHeap(), 0, tmp);
                return false;
            }
            cur_s = tmp[i].start;
            cur_e = tmp[i].end;
        }
    }

    {
        MAP_SEGMENT m;
        ZeroMemory(&m, sizeof(m));
        m.map_start = cur_s;
        m.map_end = cur_e;
        m.map_size = cur_e - cur_s;
        if (!map_list_push(out, &m)) {
            HeapFree(GetProcessHeap(), 0, tmp);
            return false;
        }
    }

    HeapFree(GetProcessHeap(), 0, tmp);
    return true;
}

static bool map_lookup(const MAP_LIST* maps, uint64_t gpa, MAP_SEGMENT** seg_out, uint64_t* max_end_out) {
    if (!maps) return false;
    for (size_t i = 0; i < maps->count; ++i) {
        MAP_SEGMENT* m = &maps->items[i];
        if (gpa >= m->map_start && gpa < m->map_end) {
            if (seg_out) *seg_out = m;
            if (max_end_out) *max_end_out = m->map_end;
            return true;
        }
    }
    return false;
}

static bool gpa_to_host(const MAP_LIST* maps, uint64_t gpa, uint8_t** out_ptr, uint64_t* chunk_end) {
    if (!out_ptr) return false;
    MAP_SEGMENT* m = NULL;
    uint64_t max_end = 0;
    if (!map_lookup(maps, gpa, &m, &max_end)) return false;
    uint64_t off = gpa - m->map_start;
    if (off > (uint64_t)SIZE_MAX) return false;
    *out_ptr = m->host + (SIZE_T)off;
    if (chunk_end) *chunk_end = max_end;
    return true;
}

static bool range_covered_by_maps(const MAP_LIST* maps, uint64_t start, uint64_t length) {
    if (!maps) return false;
    if (length == 0) return true;

    uint64_t end = 0;
    if (!add_u64_checked(start, length, &end) || end <= start) return false;

    uint64_t cur = start;
    while (cur < end) {
        uint64_t chunk_end = 0;
        if (!map_lookup(maps, cur, NULL, &chunk_end)) return false;
        if (chunk_end <= cur) return false;
        cur = (chunk_end < end) ? chunk_end : end;
    }

    return true;
}

static bool copy_buffer_to_guest(const MAP_LIST* maps, uint64_t dst_gpa, const uint8_t* src, uint64_t len) {
    if (len == 0) return true;
    if (!maps || !src) return false;

    uint64_t cur = dst_gpa;
    uint64_t done = 0;
    while (done < len) {
        uint8_t* ptr = NULL;
        uint64_t chunk_end = 0;
        if (!gpa_to_host(maps, cur, &ptr, &chunk_end)) return false;
        uint64_t avail = chunk_end - cur;
        uint64_t need = len - done;
        uint64_t take = u64_min(avail, need);
        CopyMemory(ptr, src + (SIZE_T)done, (SIZE_T)take);
        cur += take;
        done += take;
    }

    return true;
}

static bool read_guest_to_handle(const MAP_LIST* maps, uint64_t src_gpa, uint64_t len, HANDLE h) {
    uint64_t cur = src_gpa;
    uint64_t done = 0;

    while (done < len) {
        uint8_t* ptr = NULL;
        uint64_t chunk_end = 0;
        if (!gpa_to_host(maps, cur, &ptr, &chunk_end)) return false;

        uint64_t avail = chunk_end - cur;
        uint64_t need = len - done;
        uint64_t take = u64_min(avail, need);

        uint64_t wrote_total = 0;
        while (wrote_total < take) {
            DWORD chunk = (DWORD)u64_min(1u << 20, take - wrote_total);
            DWORD wrote = 0;
            if (!WriteFile(h, ptr + wrote_total, chunk, &wrote, NULL)) {
                return false;
            }
            if (wrote == 0) {
                return false;
            }
            wrote_total += wrote;
        }

        cur += take;
        done += take;
    }

    return true;
}

static bool collect_chunk_pairs(const MAP_LIST* maps, uint16_t** pml4_list, size_t* pml4_count,
    uint32_t** pd_pairs, size_t* pd_pair_count) {
    if (!maps || !pml4_list || !pml4_count || !pd_pairs || !pd_pair_count) return false;
    *pml4_list = NULL;
    *pml4_count = 0;
    *pd_pairs = NULL;
    *pd_pair_count = 0;

    size_t pml4_cap = 0;
    size_t pd_cap = 0;

    for (size_t i = 0; i < maps->count; ++i) {
        uint64_t s2m = align_down_u64(maps->items[i].map_start, PAGE_SIZE_2M);
        uint64_t e2m = 0;
        if (!align_up_u64_checked(maps->items[i].map_end, PAGE_SIZE_2M, &e2m)) return false;

        for (uint64_t g = s2m; g < e2m; g += PAGE_SIZE_2M) {
            uint16_t i4 = (uint16_t)((g >> 39) & 0x1FFu);
            uint16_t i3 = (uint16_t)((g >> 30) & 0x1FFu);

            bool has_i4 = false;
            for (size_t k = 0; k < *pml4_count; ++k) {
                if ((*pml4_list)[k] == i4) {
                    has_i4 = true;
                    break;
                }
            }
            if (!has_i4) {
                if (*pml4_count == pml4_cap) {
                    size_t nc = (pml4_cap == 0) ? 8 : (pml4_cap * 2);
                    uint16_t* nv = (uint16_t*)HeapReAlloc(GetProcessHeap(), 0, *pml4_list, nc * sizeof(uint16_t));
                    if (!nv) {
                        if (pml4_cap == 0) {
                            nv = (uint16_t*)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(uint16_t));
                            if (!nv) return false;
                        }
                        else return false;
                    }
                    *pml4_list = nv;
                    pml4_cap = nc;
                }
                (*pml4_list)[(*pml4_count)++] = i4;
            }

            uint32_t key = ((uint32_t)i4 << 16) | (uint32_t)i3;
            bool has_pd = false;
            for (size_t k = 0; k < *pd_pair_count; ++k) {
                if ((*pd_pairs)[k] == key) {
                    has_pd = true;
                    break;
                }
            }
            if (!has_pd) {
                if (*pd_pair_count == pd_cap) {
                    size_t nc = (pd_cap == 0) ? 16 : (pd_cap * 2);
                    uint32_t* nv = (uint32_t*)HeapReAlloc(GetProcessHeap(), 0, *pd_pairs, nc * sizeof(uint32_t));
                    if (!nv) {
                        if (pd_cap == 0) {
                            nv = (uint32_t*)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(uint32_t));
                            if (!nv) return false;
                        }
                        else return false;
                    }
                    *pd_pairs = nv;
                    pd_cap = nc;
                }
                (*pd_pairs)[(*pd_pair_count)++] = key;
            }
        }
    }

    return true;
}

static bool compute_long_table_pages(const MAP_LIST* maps, uint32_t* pdpt_pages, uint32_t* pd_pages, uint32_t* total_pages) {
    if (!maps || !pdpt_pages || !pd_pages || !total_pages) return false;

    uint16_t* pml4 = NULL;
    uint32_t* pdpairs = NULL;
    size_t c_pml4 = 0;
    size_t c_pd = 0;

    if (!collect_chunk_pairs(maps, &pml4, &c_pml4, &pdpairs, &c_pd)) {
        HeapFree(GetProcessHeap(), 0, pml4);
        HeapFree(GetProcessHeap(), 0, pdpairs);
        return false;
    }

    *pdpt_pages = (uint32_t)c_pml4;
    *pd_pages = (uint32_t)c_pd;

    uint64_t total = 1ull + (uint64_t)(*pdpt_pages) + (uint64_t)(*pd_pages);
    if (total > 0xFFFFFFFFull) {
        HeapFree(GetProcessHeap(), 0, pml4);
        HeapFree(GetProcessHeap(), 0, pdpairs);
        return false;
    }
    *total_pages = (uint32_t)total;

    HeapFree(GetProcessHeap(), 0, pml4);
    HeapFree(GetProcessHeap(), 0, pdpairs);
    return true;
}

static bool alloc_from_free_ranges(FREE_LIST* free_list, uint64_t size, uint64_t align, uint64_t* out_addr) {
    if (!free_list || !out_addr || size == 0 || align == 0 || (align & (align - 1ull)) != 0) return false;

    qsort(free_list->items, free_list->count, sizeof(FREE_RANGE), cmp_free_desc);

    for (size_t i = 0; i < free_list->count; ++i) {
        FREE_RANGE* r = &free_list->items[i];
        if (r->end <= r->start) continue;

        uint64_t top = align_down_nonzero(r->end, align);
        if (top < r->start) continue;

        uint64_t candidate = 0;
        if (!sub_u64_checked(top, size, &candidate)) continue;
        candidate = align_down_nonzero(candidate, align);

        uint64_t end = 0;
        if (!add_u64_checked(candidate, size, &end)) continue;
        if (candidate < r->start || end > r->end) continue;

        r->end = candidate;
        *out_addr = candidate;
        return true;
    }

    return false;
}

static bool check_dump_targets(const OPTIONS* o) {
    if (!o) return false;
    if (!o->pedantic) return true;

    for (size_t i = 0; i < o->dumps.count; ++i) {
        if (!o->dumps.items[i].has_file) continue;
        DWORD attrs = GetFileAttributesW(o->dumps.items[i].file_path);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            return false;
        }
    }

    return true;
}

static bool plan_runtime_layout(const OPTIONS* o, const INTERVAL_LIST* user_intervals, uint64_t user_high_end,
    RUNTIME_LAYOUT* rt, INTERVAL_LIST* all_intervals, MAP_LIST* map_segments) {
    if (!o || !user_intervals || !rt || !all_intervals || !map_segments) return false;
    ZeroMemory(rt, sizeof(*rt));

    rt->has_gdt = (o->mode == MODE_PROTECTED || o->mode == MODE_LONG);
    rt->has_tables = (o->mode == MODE_LONG);

    for (size_t i = 0; i < user_intervals->count; ++i) {
        if (!interval_list_push(all_intervals, user_intervals->items[i].start, user_intervals->items[i].end)) {
            return false;
        }
    }

    if (!rt->has_gdt && !rt->has_tables && STACK_SIZE == 0) {
        if (!build_map_segments_from_intervals(all_intervals, map_segments)) return false;
        return true;
    }

    uint64_t base_runtime = STACK_SIZE + (rt->has_gdt ? GDT_SIZE : 0);

    if (!o->pedantic) {
        uint64_t runtime_size = base_runtime;
        if (!align_up_u64_checked(runtime_size, PAGE_SIZE_4K, &runtime_size)) return false;
        if (runtime_size == 0) runtime_size = PAGE_SIZE_4K;

        uint64_t runtime_start = 0;
        if (!align_up_u64_checked(user_high_end, PAGE_SIZE_4K, &runtime_start)) return false;

        for (int iter = 0; iter < 8; ++iter) {
            INTERVAL_LIST trial_intervals;
            ZeroMemory(&trial_intervals, sizeof(trial_intervals));
            for (size_t i = 0; i < user_intervals->count; ++i) {
                if (!interval_list_push(&trial_intervals, user_intervals->items[i].start, user_intervals->items[i].end)) {
                    HeapFree(GetProcessHeap(), 0, trial_intervals.items);
                    return false;
                }
            }
            if (!interval_list_push(&trial_intervals, runtime_start, runtime_start + runtime_size)) {
                HeapFree(GetProcessHeap(), 0, trial_intervals.items);
                return false;
            }

            MAP_LIST trial_maps;
            ZeroMemory(&trial_maps, sizeof(trial_maps));
            if (!build_map_segments_from_intervals(&trial_intervals, &trial_maps)) {
                HeapFree(GetProcessHeap(), 0, trial_intervals.items);
                HeapFree(GetProcessHeap(), 0, trial_maps.items);
                return false;
            }

            uint32_t pdpt = 0, pd = 0, total = 0;
            if (rt->has_tables) {
                if (!compute_long_table_pages(&trial_maps, &pdpt, &pd, &total)) {
                    HeapFree(GetProcessHeap(), 0, trial_intervals.items);
                    HeapFree(GetProcessHeap(), 0, trial_maps.items);
                    return false;
                }
            }
            else {
                total = 0;
                pdpt = 0;
                pd = 0;
            }

            uint64_t needed = base_runtime + ((uint64_t)total * PAGE_SIZE_4K);
            if (!align_up_u64_checked(needed, PAGE_SIZE_4K, &needed)) {
                HeapFree(GetProcessHeap(), 0, trial_intervals.items);
                HeapFree(GetProcessHeap(), 0, trial_maps.items);
                return false;
            }

            HeapFree(GetProcessHeap(), 0, trial_intervals.items);
            HeapFree(GetProcessHeap(), 0, trial_maps.items);

            if (needed == runtime_size) {
                rt->table_pages = total;
                rt->pdpt_pages = pdpt;
                rt->pd_pages = pd;
                break;
            }
            runtime_size = needed;
            if (iter == 7) {
                rt->table_pages = total;
                rt->pdpt_pages = pdpt;
                rt->pd_pages = pd;
            }
        }

        rt->has_runtime_area = true;
        rt->runtime_start = runtime_start;
        rt->runtime_length = runtime_size;

        if (!interval_list_push(all_intervals, rt->runtime_start, rt->runtime_start + rt->runtime_length)) {
            return false;
        }

        if (!build_map_segments_from_intervals(all_intervals, map_segments)) return false;

        uint64_t cursor = rt->runtime_start + rt->runtime_length;

        if (rt->has_tables) {
            uint64_t tables_bytes = (uint64_t)rt->table_pages * PAGE_SIZE_4K;
            if (!sub_u64_checked(cursor, tables_bytes, &cursor)) return false;
            cursor = align_down_nonzero(cursor, PAGE_SIZE_4K);
            rt->tables_base = cursor;
            rt->pml4_gpa = rt->tables_base;
        }

        if (rt->has_gdt) {
            if (!sub_u64_checked(cursor, GDT_SIZE, &cursor)) return false;
            cursor = align_down_nonzero(cursor, 16);
            rt->gdt_gpa = cursor;
        }

        if (!sub_u64_checked(cursor, STACK_SIZE, &cursor)) return false;
        cursor = align_down_nonzero(cursor, 16);
        rt->stack_base = cursor;
        rt->stack_top = rt->stack_base + STACK_SIZE - 16ull;

        if (rt->stack_base < rt->runtime_start) return false;
    }
    else {
        FREE_LIST free_list;
        ZeroMemory(&free_list, sizeof(free_list));

        for (size_t i = 0; i < o->areas.count; ++i) {
            uint64_t area_end = o->areas.items[i].start + o->areas.items[i].length;
            uint64_t free_start = o->areas.items[i].start + o->areas.items[i].init_size;
            if (!free_list_push(&free_list, free_start, area_end)) {
                HeapFree(GetProcessHeap(), 0, free_list.items);
                return false;
            }
        }

        if (!build_map_segments_from_intervals(all_intervals, map_segments)) {
            HeapFree(GetProcessHeap(), 0, free_list.items);
            return false;
        }

        if (rt->has_tables) {
            if (!compute_long_table_pages(map_segments, &rt->pdpt_pages, &rt->pd_pages, &rt->table_pages)) {
                HeapFree(GetProcessHeap(), 0, free_list.items);
                return false;
            }
            uint64_t tables_bytes = (uint64_t)rt->table_pages * PAGE_SIZE_4K;
            if (!alloc_from_free_ranges(&free_list, tables_bytes, PAGE_SIZE_4K, &rt->tables_base)) {
                HeapFree(GetProcessHeap(), 0, free_list.items);
                return false;
            }
            rt->pml4_gpa = rt->tables_base;
        }

        if (rt->has_gdt) {
            if (!alloc_from_free_ranges(&free_list, GDT_SIZE, 16, &rt->gdt_gpa)) {
                HeapFree(GetProcessHeap(), 0, free_list.items);
                return false;
            }
        }

        if (!alloc_from_free_ranges(&free_list, STACK_SIZE, 16, &rt->stack_base)) {
            HeapFree(GetProcessHeap(), 0, free_list.items);
            return false;
        }
        rt->stack_top = rt->stack_base + STACK_SIZE - 16ull;

        HeapFree(GetProcessHeap(), 0, free_list.items);
    }

    if ((o->mode == MODE_REAL || o->mode == MODE_UNREAL)) {
        if (o->entry.linear >= 0x100000ull) return false;
        if (rt->stack_top >= 0x100000ull) return false;
    }

    if (!range_covered_by_maps(map_segments, o->entry.linear, 1)) return false;
    if (!range_covered_by_maps(map_segments, rt->stack_base, STACK_SIZE)) return false;

    for (size_t i = 0; i < o->dumps.count; ++i) {
        if (!range_covered_by_maps(map_segments, o->dumps.items[i].start, o->dumps.items[i].length)) {
            return false;
        }
    }

    return true;
}

static bool allocate_and_map_segments(WHV_PARTITION_HANDLE partition, MAP_LIST* maps) {
    if (!partition || !maps) return false;

    for (size_t i = 0; i < maps->count; ++i) {
        MAP_SEGMENT* m = &maps->items[i];
        if (m->map_size > (uint64_t)SIZE_MAX) {
            errw(L"map segment too large for local address space\n");
            return false;
        }

        m->host = (uint8_t*)VirtualAlloc(NULL, (SIZE_T)m->map_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!m->host) {
            errf(L"VirtualAlloc failed for map segment: %lu\n", (unsigned long)GetLastError());
            return false;
        }
        ZeroMemory(m->host, (SIZE_T)m->map_size);

        HRESULT hr = WHvMapGpaRange(
            partition,
            m->host,
            m->map_start,
            m->map_size,
            WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
        if (FAILED(hr)) {
            errf(L"WHvMapGpaRange failed for [0x%llx..0x%llx): 0x%08lX\n",
                (unsigned long long)m->map_start,
                (unsigned long long)m->map_end,
                (unsigned long)hr);
            return false;
        }
    }

    return true;
}

static void unmap_and_free_segments(WHV_PARTITION_HANDLE partition, MAP_LIST* maps, bool partition_setup) {
    if (!maps) return;

    for (size_t i = 0; i < maps->count; ++i) {
        MAP_SEGMENT* m = &maps->items[i];
        if (partition && partition_setup) {
            HRESULT hr = WHvUnmapGpaRange(partition, m->map_start, m->map_size);
            if (FAILED(hr)) {
                errf(L"warning: WHvUnmapGpaRange failed for [0x%llx..0x%llx): 0x%08lX\n",
                    (unsigned long long)m->map_start,
                    (unsigned long long)m->map_end,
                    (unsigned long)hr);
            }
        }

        if (m->host) {
            VirtualFree(m->host, 0, MEM_RELEASE);
            m->host = NULL;
        }
    }
}

static bool initialize_areas(const OPTIONS* o, const MAP_LIST* maps) {
    if (!o || !maps) return false;

    for (size_t i = 0; i < o->areas.count; ++i) {
        const AREA_SPEC* a = &o->areas.items[i];
        if (a->init_size == 0) continue;
        if (!copy_buffer_to_guest(maps, a->start, a->file_data, a->init_size)) {
            errf(L"failed to copy area init bytes at start=0x%llx\n", (unsigned long long)a->start);
            return false;
        }
    }

    return true;
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

    write_gdt_descriptor(gdt, 0, 0, 0, 0, 0);
    write_gdt_descriptor(gdt, 1, 0, 0xFFFFFu, 0x9Au, 0xCu);
    write_gdt_descriptor(gdt, 2, 0, 0xFFFFFu, 0x92u, 0xCu);
    write_gdt_descriptor(gdt, 3, 0, 0xFFFFFu, 0xFAu, 0xCu);
    write_gdt_descriptor(gdt, 4, 0, 0xFFFFFu, 0xF2u, 0xCu);
    write_gdt_descriptor(gdt, 5, 0, 0xFFFFFu, 0xFAu, 0xAu);
    write_gdt_descriptor(gdt, 6, 0, 0xFFFFFu, 0xF2u, 0xCu);
    write_gdt_descriptor(gdt, 7, 0, 0xFFFFFu, 0x9Au, 0xAu);
}

static bool build_identity_2m_page_tables(const MAP_LIST* maps, const RUNTIME_LAYOUT* rt) {
    if (!maps || !rt || !rt->has_tables) return false;

    uint16_t* pml4_keys = NULL;
    uint32_t* pd_keys = NULL;
    size_t pml4_count = 0;
    size_t pd_count = 0;

    if (!collect_chunk_pairs(maps, &pml4_keys, &pml4_count, &pd_keys, &pd_count)) {
        HeapFree(GetProcessHeap(), 0, pml4_keys);
        HeapFree(GetProcessHeap(), 0, pd_keys);
        return false;
    }

    if ((uint32_t)pml4_count != rt->pdpt_pages || (uint32_t)pd_count != rt->pd_pages) {
        errw(L"internal mismatch in long-mode page table planning\n");
        HeapFree(GetProcessHeap(), 0, pml4_keys);
        HeapFree(GetProcessHeap(), 0, pd_keys);
        return false;
    }

    uint64_t next = rt->tables_base;
    uint64_t expected_end = rt->tables_base + ((uint64_t)rt->table_pages * PAGE_SIZE_4K);

    uint8_t* pml4_host_u8 = NULL;
    if (!gpa_to_host(maps, next, &pml4_host_u8, NULL)) {
        errw(L"failed to resolve PML4 host pointer\n");
        HeapFree(GetProcessHeap(), 0, pml4_keys);
        HeapFree(GetProcessHeap(), 0, pd_keys);
        return false;
    }
    uint64_t* pml4 = (uint64_t*)pml4_host_u8;
    ZeroMemory(pml4, (SIZE_T)PAGE_SIZE_4K);
    next += PAGE_SIZE_4K;

    PDPT_INFO* pdpt_infos = NULL;
    PD_INFO* pd_infos = NULL;

    if (pml4_count) {
        pdpt_infos = (PDPT_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pml4_count * sizeof(PDPT_INFO));
        if (!pdpt_infos) {
            HeapFree(GetProcessHeap(), 0, pml4_keys);
            HeapFree(GetProcessHeap(), 0, pd_keys);
            return false;
        }
    }
    if (pd_count) {
        pd_infos = (PD_INFO*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, pd_count * sizeof(PD_INFO));
        if (!pd_infos) {
            HeapFree(GetProcessHeap(), 0, pdpt_infos);
            HeapFree(GetProcessHeap(), 0, pml4_keys);
            HeapFree(GetProcessHeap(), 0, pd_keys);
            return false;
        }
    }

    for (size_t i = 0; i < pml4_count; ++i) {
        uint8_t* pdpt_u8 = NULL;
        if (!gpa_to_host(maps, next, &pdpt_u8, NULL)) {
            errw(L"failed to resolve PDPT host pointer\n");
            HeapFree(GetProcessHeap(), 0, pd_infos);
            HeapFree(GetProcessHeap(), 0, pdpt_infos);
            HeapFree(GetProcessHeap(), 0, pml4_keys);
            HeapFree(GetProcessHeap(), 0, pd_keys);
            return false;
        }
        uint64_t* pdpt = (uint64_t*)pdpt_u8;
        ZeroMemory(pdpt, (SIZE_T)PAGE_SIZE_4K);

        pdpt_infos[i].pml4_index = pml4_keys[i];
        pdpt_infos[i].gpa = next;
        pdpt_infos[i].entries = pdpt;
        pml4[pml4_keys[i]] = next | PT_PRESENT | PT_WRITE | PT_USER;
        next += PAGE_SIZE_4K;
    }

    for (size_t i = 0; i < pd_count; ++i) {
        uint16_t i4 = (uint16_t)((pd_keys[i] >> 16) & 0xFFFFu);
        uint16_t i3 = (uint16_t)(pd_keys[i] & 0xFFFFu);

        uint8_t* pd_u8 = NULL;
        if (!gpa_to_host(maps, next, &pd_u8, NULL)) {
            errw(L"failed to resolve PD host pointer\n");
            HeapFree(GetProcessHeap(), 0, pd_infos);
            HeapFree(GetProcessHeap(), 0, pdpt_infos);
            HeapFree(GetProcessHeap(), 0, pml4_keys);
            HeapFree(GetProcessHeap(), 0, pd_keys);
            return false;
        }
        uint64_t* pd = (uint64_t*)pd_u8;
        ZeroMemory(pd, (SIZE_T)PAGE_SIZE_4K);

        pd_infos[i].pml4_index = i4;
        pd_infos[i].pdpt_index = i3;
        pd_infos[i].gpa = next;
        pd_infos[i].entries = pd;

        for (size_t j = 0; j < pml4_count; ++j) {
            if (pdpt_infos[j].pml4_index == i4) {
                pdpt_infos[j].entries[i3] = next | PT_PRESENT | PT_WRITE | PT_USER;
                break;
            }
        }

        next += PAGE_SIZE_4K;
    }

    for (size_t mi = 0; mi < maps->count; ++mi) {
        uint64_t s2m = align_down_u64(maps->items[mi].map_start, PAGE_SIZE_2M);
        uint64_t e2m = 0;
        if (!align_up_u64_checked(maps->items[mi].map_end, PAGE_SIZE_2M, &e2m)) {
            HeapFree(GetProcessHeap(), 0, pd_infos);
            HeapFree(GetProcessHeap(), 0, pdpt_infos);
            HeapFree(GetProcessHeap(), 0, pml4_keys);
            HeapFree(GetProcessHeap(), 0, pd_keys);
            return false;
        }

        for (uint64_t g = s2m; g < e2m; g += PAGE_SIZE_2M) {
            uint16_t i4 = (uint16_t)((g >> 39) & 0x1FFu);
            uint16_t i3 = (uint16_t)((g >> 30) & 0x1FFu);
            uint16_t i2 = (uint16_t)((g >> 21) & 0x1FFu);

            for (size_t pi = 0; pi < pd_count; ++pi) {
                if (pd_infos[pi].pml4_index == i4 && pd_infos[pi].pdpt_index == i3) {
                    pd_infos[pi].entries[i2] = (g & ~(PAGE_SIZE_2M - 1ull)) | PT_PRESENT | PT_WRITE | PT_USER | PT_PS;
                    break;
                }
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, pd_infos);
    HeapFree(GetProcessHeap(), 0, pdpt_infos);
    HeapFree(GetProcessHeap(), 0, pml4_keys);
    HeapFree(GetProcessHeap(), 0, pd_keys);

    if (next != expected_end) {
        errw(L"internal long-mode table allocator mismatch\n");
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

static void configure_extended_vm_exits_mask(
    WHV_PARTITION_HANDLE partition,
    uint64_t supported_mask,
    uint64_t* requested_mask_out,
    uint64_t* enabled_mask_out,
    bool* enabled_out) {
    if (!requested_mask_out || !enabled_mask_out || !enabled_out) {
        return;
    }

    *requested_mask_out = 0;
    *enabled_mask_out = 0;
    *enabled_out = false;

    if (supported_mask == 0) {
        return;
    }

    uint64_t desired_mask = supported_mask & ~EXT_VM_EXIT_GPA_ACCESS_FAULT_MASK;

    WHV_EXTENDED_VM_EXITS request;
    ZeroMemory(&request, sizeof(request));
    request.AsUINT64 = desired_mask;

    HRESULT hr = WHvSetPartitionProperty(
        partition,
        WHvPartitionPropertyCodeExtendedVmExits,
        &request,
        (UINT32)sizeof(request));
    if (FAILED(hr)) {
        uint64_t accepted = 0;
        for (UINT32 bit = 0; bit < 64; ++bit) {
            uint64_t test_bit = 1ull << bit;
            if ((desired_mask & test_bit) == 0) {
                continue;
            }

            WHV_EXTENDED_VM_EXITS candidate;
            ZeroMemory(&candidate, sizeof(candidate));
            candidate.AsUINT64 = accepted | test_bit;

            hr = WHvSetPartitionProperty(
                partition,
                WHvPartitionPropertyCodeExtendedVmExits,
                &candidate,
                (UINT32)sizeof(candidate));
            if (SUCCEEDED(hr)) {
                accepted |= test_bit;
            }
        }

        request.AsUINT64 = accepted;
        if (accepted != 0) {
            hr = WHvSetPartitionProperty(
                partition,
                WHvPartitionPropertyCodeExtendedVmExits,
                &request,
                (UINT32)sizeof(request));
            if (FAILED(hr)) {
                errf(L"warning: failed to set reduced ExtendedVmExits mask=0x%llx (hr=0x%08lX); continuing with defaults.\n",
                    (unsigned long long)accepted,
                    (unsigned long)hr);
                request.AsUINT64 = 0;
            }
        }
        else {
            errw(L"warning: no ExtendedVmExits bits were accepted for this partition.\n");
        }
    }

    WHV_EXTENDED_VM_EXITS active;
    ZeroMemory(&active, sizeof(active));
    UINT32 active_written = 0;
    HRESULT ghr = WHvGetPartitionProperty(
        partition,
        WHvPartitionPropertyCodeExtendedVmExits,
        &active,
        (UINT32)sizeof(active),
        &active_written);
    if (SUCCEEDED(ghr)) {
        uint64_t req = request.AsUINT64;
        uint64_t act = active.AsUINT64;
        if (req != 0 && ((act & req) != req)) {
            errf(L"warning: ExtendedVmExits readback adjusted requested=0x%llx active=0x%llx; adopting active mask.\n",
                (unsigned long long)req,
                (unsigned long long)act);
            req = act;
        }
        *requested_mask_out = req;
        *enabled_mask_out = act;
        if (req != 0) {
            *enabled_out = ((act & req) == req);
        }
        else {
            *enabled_out = (act != 0);
        }
        return;
    }

    errf(L"warning: WHvGetPartitionProperty(ExtendedVmExits) failed (hr=0x%08lX); assuming configured mask is active.\n",
        (unsigned long)ghr);
    *requested_mask_out = request.AsUINT64;
    *enabled_mask_out = request.AsUINT64;
    *enabled_out = (request.AsUINT64 != 0);
}

static bool set_partition_properties(WHV_PARTITION_HANDLE partition, PARTITION_CAPS* caps_out) {
    PARTITION_CAPS caps_state;
    ZeroMemory(&caps_state, sizeof(caps_state));

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
        WHV_EXTENDED_VM_EXITS supported = caps.ExtendedVmExits;
        caps_state.extended_vm_exits_supported = (supported.AsUINT64 != 0);
        caps_state.extended_vm_exits_supported_mask = supported.AsUINT64;
        if (supported.AsUINT64 != 0) {
            caps_state.extended_vm_exits_requested = true;
            configure_extended_vm_exits_mask(
                partition,
                supported.AsUINT64,
                &caps_state.extended_vm_exits_requested_mask,
                &caps_state.extended_vm_exits_enabled_mask,
                &caps_state.extended_vm_exits_enabled);
        }
    }
    else {
        errf(L"warning: WHvGetCapability(ExtendedVmExits) failed (hr=0x%08lX).\n", (unsigned long)hr);
    }

#if defined(_AMD64_)
    WHV_CAPABILITY ex_caps;
    written = 0;
    ZeroMemory(&ex_caps, sizeof(ex_caps));
    hr = WHvGetCapability(WHvCapabilityCodeExceptionExitBitmap, &ex_caps, (UINT32)sizeof(ex_caps), &written);
    if (SUCCEEDED(hr)) {
        UINT64 bitmap = ex_caps.ExceptionExitBitmap;
        caps_state.exception_bitmap_supported = (bitmap != 0);
        caps_state.exception_bitmap_supported_mask = bitmap;
        if (bitmap != 0) {
            caps_state.exception_bitmap_requested = true;
            caps_state.exception_bitmap_requested_mask = bitmap;
            hr = WHvSetPartitionProperty(
                partition,
                WHvPartitionPropertyCodeExceptionExitBitmap,
                &bitmap,
                (UINT32)sizeof(bitmap));
            if (FAILED(hr)) {
                errf(L"warning: failed to set ExceptionExitBitmap (mask=0x%llx, hr=0x%08lX); exception exits may be limited.\n",
                    (unsigned long long)bitmap,
                    (unsigned long)hr);
            }
            else {
                UINT64 active_bitmap = 0;
                UINT32 active_written = 0;
                HRESULT ghr = WHvGetPartitionProperty(
                    partition,
                    WHvPartitionPropertyCodeExceptionExitBitmap,
                    &active_bitmap,
                    (UINT32)sizeof(active_bitmap),
                    &active_written);
                if (SUCCEEDED(ghr)) {
                    caps_state.exception_bitmap_enabled_mask = active_bitmap;
                    caps_state.exception_bitmap_enabled =
                        ((active_bitmap & bitmap) == bitmap);
                    if (!caps_state.exception_bitmap_enabled) {
                        errf(L"warning: ExceptionExitBitmap readback mismatch requested=0x%llx active=0x%llx\n",
                            (unsigned long long)bitmap,
                            (unsigned long long)active_bitmap);
                    }
                }
                else {
                    errf(L"warning: WHvGetPartitionProperty(ExceptionExitBitmap) failed (hr=0x%08lX); assuming enable succeeded.\n",
                        (unsigned long)ghr);
                    caps_state.exception_bitmap_enabled = true;
                    caps_state.exception_bitmap_enabled_mask = bitmap;
                }
            }
        }
    }
    else {
        errf(L"warning: WHvGetCapability(ExceptionExitBitmap) failed (hr=0x%08lX).\n", (unsigned long)hr);
    }

    WHV_CAPABILITY msr_caps;
    written = 0;
    ZeroMemory(&msr_caps, sizeof(msr_caps));
    hr = WHvGetCapability(WHvCapabilityCodeX64MsrExitBitmap, &msr_caps, (UINT32)sizeof(msr_caps), &written);
    if (SUCCEEDED(hr)) {
        WHV_X64_MSR_EXIT_BITMAP msr_bitmap = msr_caps.X64MsrExitBitmap;
        caps_state.msr_exit_bitmap_supported = (msr_bitmap.AsUINT64 != 0);
        caps_state.msr_exit_bitmap_supported_mask = msr_bitmap.AsUINT64;
        if (msr_bitmap.AsUINT64 != 0) {
            caps_state.msr_exit_bitmap_requested = true;
            caps_state.msr_exit_bitmap_requested_mask = msr_bitmap.AsUINT64;
            hr = WHvSetPartitionProperty(
                partition,
                WHvPartitionPropertyCodeX64MsrExitBitmap,
                &msr_bitmap,
                (UINT32)sizeof(msr_bitmap));
            if (FAILED(hr)) {
                errf(L"warning: failed to set X64MsrExitBitmap (mask=0x%llx, hr=0x%08lX).\n",
                    (unsigned long long)msr_bitmap.AsUINT64,
                    (unsigned long)hr);
            }
            else {
                WHV_X64_MSR_EXIT_BITMAP active_msr;
                ZeroMemory(&active_msr, sizeof(active_msr));
                UINT32 active_written = 0;
                HRESULT ghr = WHvGetPartitionProperty(
                    partition,
                    WHvPartitionPropertyCodeX64MsrExitBitmap,
                    &active_msr,
                    (UINT32)sizeof(active_msr),
                    &active_written);
                if (SUCCEEDED(ghr)) {
                    caps_state.msr_exit_bitmap_enabled_mask = active_msr.AsUINT64;
                    caps_state.msr_exit_bitmap_enabled =
                        ((active_msr.AsUINT64 & msr_bitmap.AsUINT64) == msr_bitmap.AsUINT64);
                    if (!caps_state.msr_exit_bitmap_enabled) {
                        errf(L"warning: X64MsrExitBitmap readback mismatch requested=0x%llx active=0x%llx\n",
                            (unsigned long long)msr_bitmap.AsUINT64,
                            (unsigned long long)active_msr.AsUINT64);
                    }
                }
                else {
                    errf(L"warning: WHvGetPartitionProperty(X64MsrExitBitmap) failed (hr=0x%08lX); assuming enable succeeded.\n",
                        (unsigned long)ghr);
                    caps_state.msr_exit_bitmap_enabled = true;
                    caps_state.msr_exit_bitmap_enabled_mask = msr_bitmap.AsUINT64;
                }
            }
        }
    }
    else {
        errf(L"warning: WHvGetCapability(X64MsrExitBitmap) failed (hr=0x%08lX).\n", (unsigned long)hr);
    }
#endif

    if (caps_out) {
        *caps_out = caps_state;
    }

    return true;
}

static bool set_guest_registers(WHV_PARTITION_HANDLE partition, const OPTIONS* o, const RUNTIME_LAYOUT* rt) {
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

    uint64_t rip = o->entry.linear;
    uint64_t rsp = rt->stack_top;
    uint64_t rflags = 0x2ull;
    uint64_t cr0 = X64_CR0_ET;
    uint64_t cr3 = 0;
    uint64_t cr4 = 0;
    uint64_t efer = 0;

    if (o->mode == MODE_REAL || o->mode == MODE_UNREAL) {
        bool unreal = (o->mode == MODE_UNREAL);
        uint32_t seg_limit = unreal ? 0xFFFFFFFFu : 0xFFFFu;
        uint16_t seg_gran = unreal ? 1 : 0;

        uint16_t cs_sel = 0;
        uint64_t cs_base = 0;

        if (o->entry.explicit_seg_off) {
            cs_sel = o->entry.seg;
            cs_base = (uint64_t)cs_sel << 4;
            rip = o->entry.off;
        }
        else {
            cs_sel = (uint16_t)((o->entry.linear >> 4) & 0xFFFFu);
            cs_base = (uint64_t)cs_sel << 4;
            rip = o->entry.linear - cs_base;
        }

        uint16_t data_sel = o->entry.explicit_seg_off ? o->entry.seg : 0;
        uint64_t data_base = (uint64_t)data_sel << 4;

        uint16_t ss_sel = (uint16_t)((rt->stack_base >> 4) & 0xFFFFu);
        uint64_t ss_base = (uint64_t)ss_sel << 4;
        rsp = rt->stack_top - ss_base;

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
        bool kernel = (o->cpl == 0);
        uint16_t cs_sel = kernel ? SEL_CODE32_K : SEL_CODE32_U;
        uint16_t ds_sel = kernel ? SEL_DATA32_K : SEL_DATA32_U;
        uint16_t dpl = kernel ? 0 : 3;

        cs = make_segment(cs_sel, 0, 0xFFFFFFFFu, 0xB, dpl, 0, 1, 1);
        ds = make_segment(ds_sel, 0, 0xFFFFFFFFu, 0x3, dpl, 0, 1, 1);
        es = ds;
        fs = ds;
        gs = ds;
        ss = ds;

        cr0 = X64_CR0_PE | X64_CR0_ET | X64_CR0_NE;
        rip = o->entry.linear;
    }
    else {
        bool kernel = (o->cpl == 0);
        uint16_t cs_sel = kernel ? SEL_CODE64_K : SEL_CODE64_U;
        uint16_t ds_sel = kernel ? SEL_DATA32_K : SEL_DATA64_U;
        uint16_t dpl = kernel ? 0 : 3;

        cs = make_segment(cs_sel, 0, 0xFFFFFFFFu, 0xB, dpl, 1, 0, 1);
        ds = make_segment(ds_sel, 0, 0xFFFFFFFFu, 0x3, dpl, 0, 1, 1);
        es = ds;
        fs = ds;
        gs = ds;
        ss = ds;

        cr0 = X64_CR0_PE | X64_CR0_ET | X64_CR0_NE | X64_CR0_PG;
        cr3 = rt->pml4_gpa;
        cr4 = X64_CR4_PAE;
        efer = X64_EFER_LME;
        rip = o->entry.linear;
    }

    if (rt->has_gdt) {
        gdtr.Base = rt->gdt_gpa;
        gdtr.Limit = 0x3F;
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
        break;
    }
    case WHvRunVpExitReasonX64MsrAccess: {
        const WHV_X64_MSR_ACCESS_CONTEXT* c = &e->MsrAccess;
        outf(L"  msr-access is-write=%u msr=0x%X rax=0x%llx rdx=0x%llx\n",
            (unsigned)c->AccessInfo.IsWrite,
            (unsigned)c->MsrNumber,
            (unsigned long long)c->Rax,
            (unsigned long long)c->Rdx);
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
        break;
    }
    default:
        outw(L"  no additional context decoder for this exit reason in v2\n");
        break;
    }
}

static void init_run_report(RUN_REPORT* r) {
    if (!r) return;
    ZeroMemory(r, sizeof(*r));
    r->status = RUN_STATUS_SETUP_ERROR;
    r->has_exit_reason = true;
    r->exit_reason = WHvRunVpExitReasonNone;
}

static const wchar_t* run_status_name(RUN_STATUS s) {
    switch (s) {
    case RUN_STATUS_SUCCESS: return L"success";
    case RUN_STATUS_VM_EXIT: return L"vm_exit";
    case RUN_STATUS_SETUP_ERROR:
    default:
        return L"setup_error";
    }
}

static bool report_write_w(HANDLE h, const wchar_t* s) {
    if (!s) return true;

    int need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (need <= 0) return false;

    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)need);
    if (!buf) return false;

    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, need, NULL, NULL) <= 0) {
        HeapFree(GetProcessHeap(), 0, buf);
        return false;
    }

    DWORD want = (DWORD)(need - 1);
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, buf, want, &wrote, NULL);
    HeapFree(GetProcessHeap(), 0, buf);
    return ok && wrote == want;
}

static bool report_vwritef(HANDLE h, const wchar_t* fmt, va_list ap) {
    wchar_t stackbuf[2048];
    va_list copy;
    va_copy(copy, ap);
    int n = _vsnwprintf_s(stackbuf, _countof(stackbuf), _TRUNCATE, fmt, copy);
    va_end(copy);
    if (n >= 0) {
        return report_write_w(h, stackbuf);
    }

    size_t cap = 8192;
    wchar_t* dyn = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (!dyn) return false;

    va_copy(copy, ap);
    (void)_vsnwprintf_s(dyn, cap, _TRUNCATE, fmt, copy);
    va_end(copy);
    bool ok = report_write_w(h, dyn);
    HeapFree(GetProcessHeap(), 0, dyn);
    return ok;
}

static bool report_writef(HANDLE h, const wchar_t* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    bool ok = report_vwritef(h, fmt, ap);
    va_end(ap);
    return ok;
}

static bool report_write_json_string(HANDLE h, const wchar_t* s) {
    if (!report_write_w(h, L"\"")) return false;
    if (!s) {
        return report_write_w(h, L"\"");
    }

    for (const wchar_t* p = s; *p; ++p) {
        wchar_t tmp[2] = { *p, 0 };
        switch (*p) {
        case L'\"':
            if (!report_write_w(h, L"\\\"")) return false;
            break;
        case L'\\':
            if (!report_write_w(h, L"\\\\")) return false;
            break;
        case L'\b':
            if (!report_write_w(h, L"\\b")) return false;
            break;
        case L'\f':
            if (!report_write_w(h, L"\\f")) return false;
            break;
        case L'\n':
            if (!report_write_w(h, L"\\n")) return false;
            break;
        case L'\r':
            if (!report_write_w(h, L"\\r")) return false;
            break;
        case L'\t':
            if (!report_write_w(h, L"\\t")) return false;
            break;
        default:
            if ((unsigned)*p < 0x20u) {
                if (!report_writef(h, L"\\u%04X", (unsigned)*p)) return false;
            }
            else {
                if (!report_write_w(h, tmp)) return false;
            }
            break;
        }
    }

    return report_write_w(h, L"\"");
}

static bool report_write_json_hex_u64(HANDLE h, uint64_t v) {
    return report_writef(h, L"\"0x%llx\"", (unsigned long long)v);
}

static bool report_write_json_bool(HANDLE h, bool v) {
    return report_write_w(h, v ? L"true" : L"false");
}

static bool report_write_instruction_array(HANDLE h, const UINT8* bytes, UINT8 count) {
    if (!report_write_w(h, L"[")) return false;
    UINT8 n = count;
    if (n > 16) n = 16;
    for (UINT8 i = 0; i < n; ++i) {
        if (i != 0) {
            if (!report_write_w(h, L",")) return false;
        }
        if (!report_writef(h, L"%u", (unsigned)bytes[i])) return false;
    }
    return report_write_w(h, L"]");
}

static bool report_write_exit_details_json(HANDLE h, const RUN_REPORT* run) {
    if (!report_write_w(h, L"{")) return false;
    if (!run || !run->has_exit_context || run->timed_out) {
        return report_write_w(h, L"}");
    }

    switch (run->exit_reason) {
    case WHvRunVpExitReasonMemoryAccess: {
        const WHV_MEMORY_ACCESS_CONTEXT* c = &run->exit_context.MemoryAccess;
        const wchar_t* atype = L"unknown";
        if (c->AccessInfo.AccessType == WHvMemoryAccessRead) atype = L"read";
        else if (c->AccessInfo.AccessType == WHvMemoryAccessWrite) atype = L"write";
        else if (c->AccessInfo.AccessType == WHvMemoryAccessExecute) atype = L"execute";
        if (!report_write_w(h, L"\"memory\":{")) return false;
        if (!report_write_w(h, L"\"access_type\":")) return false;
        if (!report_write_json_string(h, atype)) return false;
        if (!report_write_w(h, L",\"gpa\":")) return false;
        if (!report_write_json_hex_u64(h, c->Gpa)) return false;
        if (!report_write_w(h, L",\"gva\":")) return false;
        if (!report_write_json_hex_u64(h, c->Gva)) return false;
        if (!report_write_w(h, L",\"gva_valid\":")) return false;
        if (!report_write_json_bool(h, c->AccessInfo.GvaValid != 0)) return false;
        if (!report_write_w(h, L",\"gpa_unmapped\":")) return false;
        if (!report_write_json_bool(h, c->AccessInfo.GpaUnmapped != 0)) return false;
        if (!report_write_w(h, L",\"instruction\":")) return false;
        if (!report_write_instruction_array(h, c->InstructionBytes, c->InstructionByteCount)) return false;
        if (!report_write_w(h, L"}")) return false;
        break;
    }
    case WHvRunVpExitReasonX64IoPortAccess: {
        const WHV_X64_IO_PORT_ACCESS_CONTEXT* c = &run->exit_context.IoPortAccess;
        if (!report_write_w(h, L"\"io\":{")) return false;
        if (!report_write_w(h, L"\"is_write\":")) return false;
        if (!report_write_json_bool(h, c->AccessInfo.IsWrite != 0)) return false;
        if (!report_write_w(h, L",\"size\":")) return false;
        if (!report_writef(h, L"%u", (unsigned)c->AccessInfo.AccessSize)) return false;
        if (!report_write_w(h, L",\"port\":")) return false;
        if (!report_writef(h, L"\"0x%X\"", (unsigned)c->PortNumber)) return false;
        if (!report_write_w(h, L",\"instruction\":")) return false;
        if (!report_write_instruction_array(h, c->InstructionBytes, c->InstructionByteCount)) return false;
        if (!report_write_w(h, L"}")) return false;
        break;
    }
    case WHvRunVpExitReasonX64MsrAccess: {
        const WHV_X64_MSR_ACCESS_CONTEXT* c = &run->exit_context.MsrAccess;
        if (!report_write_w(h, L"\"msr\":{")) return false;
        if (!report_write_w(h, L"\"is_write\":")) return false;
        if (!report_write_json_bool(h, c->AccessInfo.IsWrite != 0)) return false;
        if (!report_write_w(h, L",\"msr\":")) return false;
        if (!report_writef(h, L"\"0x%X\"", (unsigned)c->MsrNumber)) return false;
        if (!report_write_w(h, L",\"rax\":")) return false;
        if (!report_write_json_hex_u64(h, c->Rax)) return false;
        if (!report_write_w(h, L",\"rdx\":")) return false;
        if (!report_write_json_hex_u64(h, c->Rdx)) return false;
        if (!report_write_w(h, L"}")) return false;
        break;
    }
    case WHvRunVpExitReasonException: {
        const WHV_VP_EXCEPTION_CONTEXT* c = &run->exit_context.VpException;
        if (!report_write_w(h, L"\"exception\":{")) return false;
        if (!report_write_w(h, L"\"type\":")) return false;
        if (!report_writef(h, L"%u", (unsigned)c->ExceptionType)) return false;
        if (!report_write_w(h, L",\"software\":")) return false;
        if (!report_write_json_bool(h, c->ExceptionInfo.SoftwareException != 0)) return false;
        if (!report_write_w(h, L",\"error_valid\":")) return false;
        if (!report_write_json_bool(h, c->ExceptionInfo.ErrorCodeValid != 0)) return false;
        if (!report_write_w(h, L",\"error\":")) return false;
        if (!report_writef(h, L"\"0x%X\"", (unsigned)c->ErrorCode)) return false;
        if (!report_write_w(h, L",\"param\":")) return false;
        if (!report_write_json_hex_u64(h, c->ExceptionParameter)) return false;
        if (!report_write_w(h, L",\"instruction\":")) return false;
        if (!report_write_instruction_array(h, c->InstructionBytes, c->InstructionByteCount)) return false;
        if (!report_write_w(h, L"}")) return false;
        break;
    }
    default:
        break;
    }

    return report_write_w(h, L"}");
}

static bool write_report_json_file(const OPTIONS* o,
    const MAP_LIST* maps,
    const PARTITION_CAPS* caps,
    const RUN_REPORT* run,
    const DUMP_RESULT* dump_results,
    uint64_t elapsed_us) {
    if (!o || !o->report_path) return true;

    HANDLE h = CreateFileW(o->report_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errf(L"CreateFileW failed for report file %ls: %lu\n", o->report_path, (unsigned long)GetLastError());
        return false;
    }

    PARTITION_CAPS zero_caps;
    ZeroMemory(&zero_caps, sizeof(zero_caps));
    if (!caps) caps = &zero_caps;

    RUN_REPORT zero_run;
    if (!run) {
        init_run_report(&zero_run);
        run = &zero_run;
    }

    bool ok = true;
    ok = ok && report_write_w(h, L"{\n");
    ok = ok && report_write_w(h, L"  \"mode\":");
    ok = ok && report_write_json_string(h, mode_name(o->mode));
    ok = ok && report_write_w(h, L",\n  \"cpl\":");
    ok = ok && report_writef(h, L"%u", (unsigned)o->cpl);
    ok = ok && report_write_w(h, L",\n  \"at\":");
    ok = ok && report_write_json_hex_u64(h, o->entry.linear);
    ok = ok && report_write_w(h, L",\n  \"ticks\":");
    ok = ok && report_writef(h, L"%llu", (unsigned long long)o->ticks);

    ok = ok && report_write_w(h, L",\n  \"areas\":[");
    for (size_t i = 0; ok && i < o->areas.count; ++i) {
        const AREA_SPEC* a = &o->areas.items[i];
        uint64_t end = 0;
        uint64_t page_end = 0;
        bool range_ok = add_u64_checked(a->start, a->length, &end) &&
            align_up_u64_checked(end, PAGE_SIZE_4K, &page_end);
        if (i != 0) ok = ok && report_write_w(h, L",");
        ok = ok && report_write_w(h, L"\n    {\"start\":");
        ok = ok && report_write_json_hex_u64(h, a->start);
        ok = ok && report_write_w(h, L",\"length\":");
        ok = ok && report_write_json_hex_u64(h, a->length);
        if (a->has_file) {
            ok = ok && report_write_w(h, L",\"file\":");
            ok = ok && report_write_json_string(h, a->file_path);
        }
        ok = ok && report_write_w(h, L",\"mapped_page_start\":");
        ok = ok && report_write_json_hex_u64(h, align_down_u64(a->start, PAGE_SIZE_4K));
        ok = ok && report_write_w(h, L",\"mapped_page_end\":");
        ok = ok && report_write_json_hex_u64(h, range_ok ? page_end : align_down_u64(a->start, PAGE_SIZE_4K));
        ok = ok && report_write_w(h, L"}");
    }
    ok = ok && report_write_w(h, (o->areas.count > 0) ? L"\n  ]" : L"]");

    ok = ok && report_write_w(h, L",\n  \"capabilities\":{");
    ok = ok && report_write_w(h, L"\"extended_vm_exits_supported\":");
    ok = ok && report_write_json_bool(h, caps->extended_vm_exits_supported);
    ok = ok && report_write_w(h, L",\"extended_vm_exits_supported_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->extended_vm_exits_supported_mask);
    ok = ok && report_write_w(h, L",\"extended_vm_exits_requested\":");
    ok = ok && report_write_json_bool(h, caps->extended_vm_exits_requested);
    ok = ok && report_write_w(h, L",\"extended_vm_exits_requested_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->extended_vm_exits_requested_mask);
    ok = ok && report_write_w(h, L",\"extended_vm_exits_enabled\":");
    ok = ok && report_write_json_bool(h, caps->extended_vm_exits_enabled);
    ok = ok && report_write_w(h, L",\"extended_vm_exits_enabled_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->extended_vm_exits_enabled_mask);
    ok = ok && report_write_w(h, L",\"exception_bitmap_supported\":");
    ok = ok && report_write_json_bool(h, caps->exception_bitmap_supported);
    ok = ok && report_write_w(h, L",\"exception_bitmap_supported_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->exception_bitmap_supported_mask);
    ok = ok && report_write_w(h, L",\"exception_bitmap_requested\":");
    ok = ok && report_write_json_bool(h, caps->exception_bitmap_requested);
    ok = ok && report_write_w(h, L",\"exception_bitmap_requested_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->exception_bitmap_requested_mask);
    ok = ok && report_write_w(h, L",\"exception_bitmap_enabled\":");
    ok = ok && report_write_json_bool(h, caps->exception_bitmap_enabled);
    ok = ok && report_write_w(h, L",\"exception_bitmap_enabled_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->exception_bitmap_enabled_mask);
    ok = ok && report_write_w(h, L",\"msr_exit_bitmap_supported\":");
    ok = ok && report_write_json_bool(h, caps->msr_exit_bitmap_supported);
    ok = ok && report_write_w(h, L",\"msr_exit_bitmap_supported_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->msr_exit_bitmap_supported_mask);
    ok = ok && report_write_w(h, L",\"msr_exit_bitmap_requested\":");
    ok = ok && report_write_json_bool(h, caps->msr_exit_bitmap_requested);
    ok = ok && report_write_w(h, L",\"msr_exit_bitmap_requested_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->msr_exit_bitmap_requested_mask);
    ok = ok && report_write_w(h, L",\"msr_exit_bitmap_enabled\":");
    ok = ok && report_write_json_bool(h, caps->msr_exit_bitmap_enabled);
    ok = ok && report_write_w(h, L",\"msr_exit_bitmap_enabled_mask\":");
    ok = ok && report_write_json_hex_u64(h, caps->msr_exit_bitmap_enabled_mask);
    ok = ok && report_write_w(h, L"}");

    const wchar_t* exit_name = run->timed_out ? L"Timeout" :
        (run->has_exit_reason ? exit_reason_name(run->exit_reason) : L"None");
    ok = ok && report_write_w(h, L",\n  \"run\":{");
    ok = ok && report_write_w(h, L"\"result\":");
    ok = ok && report_write_json_string(h, run_status_name(run->status));
    ok = ok && report_write_w(h, L",\"exit_reason\":");
    ok = ok && report_write_json_string(h, exit_name);
    ok = ok && report_write_w(h, L",\"rip\":");
    ok = ok && report_write_json_hex_u64(h, run->rip);
    ok = ok && report_write_w(h, L",\"details\":");
    ok = ok && report_write_exit_details_json(h, run);
    ok = ok && report_write_w(h, L"}");

    ok = ok && report_write_w(h, L",\n  \"dumps\":[");
    for (size_t i = 0; ok && i < o->dumps.count; ++i) {
        const DUMP_SPEC* d = &o->dumps.items[i];
        const wchar_t* status = L"not_run";
        if (dump_results && dump_results[i].attempted) {
            status = dump_results[i].ok ? L"ok" : L"error";
        }
        if (i != 0) ok = ok && report_write_w(h, L",");
        ok = ok && report_write_w(h, L"\n    {\"start\":");
        ok = ok && report_write_json_hex_u64(h, d->start);
        ok = ok && report_write_w(h, L",\"length\":");
        ok = ok && report_write_json_hex_u64(h, d->length);
        ok = ok && report_write_w(h, L",\"target\":");
        ok = ok && report_write_json_string(h, d->has_file ? L"file" : L"stdout");
        if (d->has_file) {
            ok = ok && report_write_w(h, L",\"file\":");
            ok = ok && report_write_json_string(h, d->file_path);
        }
        ok = ok && report_write_w(h, L",\"status\":");
        ok = ok && report_write_json_string(h, status);
        ok = ok && report_write_w(h, L"}");
    }
    ok = ok && report_write_w(h, (o->dumps.count > 0) ? L"\n  ]" : L"]");

    ok = ok && report_write_w(h, L",\n  \"timing\":{");
    ok = ok && report_write_w(h, L"\"elapsed_us\":");
    ok = ok && report_writef(h, L"%llu", (unsigned long long)elapsed_us);
    ok = ok && report_write_w(h, L"}");

    if (maps) {
        ok = ok && report_write_w(h, L",\n  \"maps\":[");
        for (size_t i = 0; ok && i < maps->count; ++i) {
            const MAP_SEGMENT* m = &maps->items[i];
            if (i != 0) ok = ok && report_write_w(h, L",");
            ok = ok && report_write_w(h, L"\n    {\"start\":");
            ok = ok && report_write_json_hex_u64(h, m->map_start);
            ok = ok && report_write_w(h, L",\"end\":");
            ok = ok && report_write_json_hex_u64(h, m->map_end);
            ok = ok && report_write_w(h, L",\"size\":");
            ok = ok && report_write_json_hex_u64(h, m->map_size);
            ok = ok && report_write_w(h, L"}");
        }
        ok = ok && report_write_w(h, (maps->count > 0) ? L"\n  ]" : L"]");
    }

    ok = ok && report_write_w(h, L"\n}\n");

    CloseHandle(h);
    if (!ok) {
        errf(L"failed while writing report file %ls\n", o->report_path);
    }
    return ok;
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

static int run_vcpu_loop(WHV_PARTITION_HANDLE partition, const OPTIONS* o, RUN_REPORT* run) {
    WHV_RUN_VP_EXIT_CONTEXT exit_ctx;
    ZeroMemory(&exit_ctx, sizeof(exit_ctx));
    if (run) {
        init_run_report(run);
    }

    if (o->ticks == 0) {
        errw(L"timeout: /ticks was set to 0\n");
        if (run) {
            run->status = RUN_STATUS_VM_EXIT;
            run->timed_out = true;
        }
        return 3;
    }

    for (uint64_t iter = 0; iter < o->ticks; ++iter) {
        HRESULT hr = WHvRunVirtualProcessor(partition, 0, &exit_ctx, (UINT32)sizeof(exit_ctx));
        if (FAILED(hr)) {
            print_hresult(L"WHvRunVirtualProcessor", hr);
            if (run) {
                run->status = RUN_STATUS_SETUP_ERROR;
                run->has_run_hresult = true;
                run->run_hresult = hr;
                run->ticks_executed = iter;
            }
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

        if (run) {
            run->ticks_executed = iter + 1ull;
            run->has_exit_reason = true;
            run->exit_reason = exit_ctx.ExitReason;
            run->rip = exit_ctx.VpContext.Rip;
            run->rsp = rsp;
            run->rflags = exit_ctx.VpContext.Rflags;
            run->cpl = (uint8_t)exit_ctx.VpContext.ExecutionState.Cpl;
            run->has_exit_context = (exit_ctx.ExitReason != WHvRunVpExitReasonNone);
            if (run->has_exit_context) {
                run->exit_context = exit_ctx;
            }
        }

        if (exit_ctx.ExitReason == WHvRunVpExitReasonX64Halt ||
            exit_ctx.ExitReason == WHvRunVpExitReasonHypercall) {
            if (run) {
                run->status = RUN_STATUS_SUCCESS;
            }
            return 0;
        }

        if (exit_ctx.ExitReason == WHvRunVpExitReasonNone) {
            continue;
        }

        if (run) {
            run->status = RUN_STATUS_VM_EXIT;
        }
        return 4;
    }

    errf(L"timeout: exceeded /ticks limit (0x%llx)\n", (unsigned long long)o->ticks);
    if (run) {
        run->status = RUN_STATUS_VM_EXIT;
        run->timed_out = true;
        run->ticks_executed = o->ticks;
    }
    return 3;
}

static bool dump_to_stdout_pretty(const MAP_LIST* maps, uint64_t start, uint64_t len) {
    outf(L"%lsDUMP%ls start=0x%llx len=0x%llx -> stdout\n",
        col_title(), col_reset(), (unsigned long long)start, (unsigned long long)len);
    outf(L"%lsLegend%ls: %lsADDR%ls | %lsHEX%ls | %lsASCII%ls\n",
        col_title(), col_reset(), col_addr(), col_reset(), col_hex(), col_reset(), col_ascii(), col_reset());

    uint64_t end = start + len;
    for (uint64_t row = start; row < end; row += 16ull) {
        wchar_t ascii[17];
        ascii[16] = 0;

        outf(L"%ls%08llX%ls  ", col_addr(), (unsigned long long)row, col_reset());

        for (uint64_t i = 0; i < 16; ++i) {
            uint64_t addr = row + i;
            if (addr < end) {
                uint8_t* p = NULL;
                if (!gpa_to_host(maps, addr, &p, NULL)) return false;
                unsigned b = (unsigned)(*p);
                outf(L"%ls%02X%ls", col_hex(), b, col_reset());
                if (b >= 0x20 && b <= 0x7E) ascii[i] = (wchar_t)b;
                else ascii[i] = L'.';
            }
            else {
                outw(L"  ");
                ascii[i] = L' ';
            }
            outw(L" ");
        }

        outf(L" %ls%ls%ls\n", col_ascii(), ascii, col_reset());
    }

    return true;
}

static bool perform_dumps(const OPTIONS* o, const MAP_LIST* maps, DUMP_RESULT* results, size_t results_count) {
    if (!o || !maps) return false;

    for (size_t i = 0; i < o->dumps.count; ++i) {
        const DUMP_SPEC* d = &o->dumps.items[i];
        DUMP_RESULT* r = NULL;
        if (results && i < results_count) {
            r = &results[i];
            r->attempted = true;
            r->ok = false;
        }

        if (!d->has_file) {
            if (!dump_to_stdout_pretty(maps, d->start, d->length)) {
                errf(L"dump to stdout failed at [0x%llx..0x%llx)\n",
                    (unsigned long long)d->start,
                    (unsigned long long)(d->start + d->length));
                return false;
            }
            if (r) {
                r->ok = true;
            }
            continue;
        }

        DWORD disposition = o->pedantic ? CREATE_NEW : CREATE_ALWAYS;
        HANDLE h = CreateFileW(d->file_path, GENERIC_WRITE, 0, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            errf(L"CreateFileW failed for dump file %ls: %lu\n", d->file_path, (unsigned long)GetLastError());
            return false;
        }

        bool ok = read_guest_to_handle(maps, d->start, d->length, h);
        CloseHandle(h);
        if (!ok) {
            errf(L"failed writing dump bytes to %ls\n", d->file_path);
            return false;
        }

        if (r) {
            r->ok = true;
        }

        outf(L"dump [0x%llx..0x%llx) -> %ls\n",
            (unsigned long long)d->start,
            (unsigned long long)(d->start + d->length),
            d->file_path);
    }

    return true;
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

    LARGE_INTEGER qpf = { 0 };
    LARGE_INTEGER qpc_start = { 0 };
    LARGE_INTEGER qpc_end = { 0 };
    bool timing_ready = (QueryPerformanceFrequency(&qpf) != 0) &&
        (QueryPerformanceCounter(&qpc_start) != 0);
    uint64_t elapsed_us = 0;

    PARTITION_CAPS caps;
    ZeroMemory(&caps, sizeof(caps));

    RUN_REPORT run_report;
    init_run_report(&run_report);

    INTERVAL_LIST user_intervals;
    INTERVAL_LIST all_intervals;
    MAP_LIST map_segments;
    RUNTIME_LAYOUT rt;
    ZeroMemory(&user_intervals, sizeof(user_intervals));
    ZeroMemory(&all_intervals, sizeof(all_intervals));
    ZeroMemory(&map_segments, sizeof(map_segments));
    ZeroMemory(&rt, sizeof(rt));

    DUMP_RESULT* dump_results = NULL;
    if (opt.dumps.count > 0) {
        dump_results = (DUMP_RESULT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, opt.dumps.count * sizeof(DUMP_RESULT));
        if (!dump_results) {
            errw(L"out of memory allocating dump result tracking\n");
            HeapFree(GetProcessHeap(), 0, opt.areas.items);
            HeapFree(GetProcessHeap(), 0, opt.dumps.items);
            return 1;
        }
    }

    WHV_PARTITION_HANDLE partition = NULL;
    bool partition_setup = false;
    bool vp_created = false;
    bool mapped = false;
    int exit_code = 1;

    if (!validate_area_non_overlapping(&opt)) {
        errw(L"invalid area layout\n");
        print_usage();
        exit_code = 2;
        goto cleanup;
    }

    if (!check_dump_targets(&opt)) {
        errw(L"invalid dump target in pedantic mode\n");
        print_usage();
        exit_code = 2;
        goto cleanup;
    }

    if (!load_area_files(&opt)) {
        exit_code = 1;
        goto cleanup;
    }

    uint64_t user_high_end = 0;
    if (!build_user_intervals(&opt, &user_intervals, &user_high_end)) {
        errw(L"failed to build user intervals\n");
        exit_code = 2;
        goto cleanup;
    }

    if (!plan_runtime_layout(&opt, &user_intervals, user_high_end, &rt, &all_intervals, &map_segments)) {
        errw(L"failed runtime/memory layout planning\n");
        exit_code = 2;
        goto cleanup;
    }

    outf(L"mode=%ls cpl=%u at=0x%llx ticks=0x%llx areas=%llu dumps=%llu pedantic=%ls maps=%llu\n",
        mode_name(opt.mode),
        (unsigned)opt.cpl,
        (unsigned long long)opt.entry.linear,
        (unsigned long long)opt.ticks,
        (unsigned long long)opt.areas.count,
        (unsigned long long)opt.dumps.count,
        opt.pedantic ? L"yes" : L"no",
        (unsigned long long)map_segments.count);

    for (size_t i = 0; i < opt.areas.count; ++i) {
        const AREA_SPEC* a = &opt.areas.items[i];
        outf(L"  area[%llu] [0x%llx..0x%llx) init=%ls\n",
            (unsigned long long)i,
            (unsigned long long)a->start,
            (unsigned long long)(a->start + a->length),
            a->has_file ? a->file_path : L"<zero>");
    }

    if (rt.has_runtime_area) {
        outf(L"  runtime-area [0x%llx..0x%llx)\n",
            (unsigned long long)rt.runtime_start,
            (unsigned long long)(rt.runtime_start + rt.runtime_length));
    }

    if (!check_hypervisor_present()) {
        exit_code = 1;
        goto cleanup;
    }

    HRESULT hr = WHvCreatePartition(&partition);
    if (FAILED(hr)) {
        print_hresult(L"WHvCreatePartition", hr);
        goto cleanup;
    }

    if (!set_partition_properties(partition, &caps)) {
        goto cleanup;
    }

    hr = WHvSetupPartition(partition);
    if (FAILED(hr)) {
        print_hresult(L"WHvSetupPartition", hr);
        goto cleanup;
    }
    partition_setup = true;

    if (!allocate_and_map_segments(partition, &map_segments)) {
        goto cleanup;
    }
    mapped = true;

    if (!initialize_areas(&opt, &map_segments)) {
        goto cleanup;
    }

    if (rt.has_gdt) {
        uint8_t* gdt_ptr = NULL;
        if (!gpa_to_host(&map_segments, rt.gdt_gpa, &gdt_ptr, NULL)) {
            errw(L"failed to resolve gdt host pointer\n");
            goto cleanup;
        }
        build_gdt(gdt_ptr);
    }

    if (rt.has_tables) {
        if (!build_identity_2m_page_tables(&map_segments, &rt)) {
            goto cleanup;
        }
    }

    hr = WHvCreateVirtualProcessor(partition, 0, 0);
    if (FAILED(hr)) {
        print_hresult(L"WHvCreateVirtualProcessor", hr);
        goto cleanup;
    }
    vp_created = true;

    if (!set_guest_registers(partition, &opt, &rt)) {
        goto cleanup;
    }

    exit_code = run_vcpu_loop(partition, &opt, &run_report);

    if (!perform_dumps(&opt, &map_segments, dump_results, opt.dumps.count)) {
        if (exit_code == 0) exit_code = 1;
    }

cleanup:
    if (vp_created) {
        HRESULT dhr = WHvDeleteVirtualProcessor(partition, 0);
        if (FAILED(dhr)) {
            errw(L"warning: WHvDeleteVirtualProcessor failed during cleanup.\n");
        }
    }

    if (mapped) {
        unmap_and_free_segments(partition, &map_segments, partition_setup);
    }

    if (partition) {
        HRESULT phr = WHvDeletePartition(partition);
        if (FAILED(phr)) {
            errw(L"warning: WHvDeletePartition failed during cleanup.\n");
        }
    }

    if (timing_ready && QueryPerformanceCounter(&qpc_end)) {
        LONGLONG delta = qpc_end.QuadPart - qpc_start.QuadPart;
        if (delta < 0) delta = 0;
        if (qpf.QuadPart > 0) {
            elapsed_us = (uint64_t)(((uint64_t)delta * 1000000ull) / (uint64_t)qpf.QuadPart);
        }
    }

    if (opt.report_path) {
        if (!write_report_json_file(&opt, &map_segments, &caps, &run_report, dump_results, elapsed_us)) {
            if (exit_code == 0) {
                exit_code = 1;
            }
        }
    }

    HeapFree(GetProcessHeap(), 0, map_segments.items);
    HeapFree(GetProcessHeap(), 0, all_intervals.items);
    HeapFree(GetProcessHeap(), 0, user_intervals.items);
    free_area_files(&opt);
    HeapFree(GetProcessHeap(), 0, opt.areas.items);
    HeapFree(GetProcessHeap(), 0, opt.dumps.items);
    HeapFree(GetProcessHeap(), 0, dump_results);

    return exit_code;
}
