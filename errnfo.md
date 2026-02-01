# errnfo

`errnfo` is a small Windows CLI for decoding and formatting Windows error values across the most common numbering spaces:

- **WIN32** (`GetLastError()` / `DWORD`)
- **NTSTATUS**
- **HRESULT**

It supports explicit “source tags” (recommended) and a heuristic mode when the input is ambiguous.

## Why this exists

Windows has multiple error ecosystems (WIN32, NTSTATUS, HRESULT, and many facility/subsystem families). A single 32-bit number can be valid in more than one space, so the tool treats the domain as a first-class concept.

`errnfo` is built around two ideas:

1. **Tagged interpretation**: `errnfo nt 0xC0000241` is unambiguous.
2. **Extensible message sources**: error text can live in the **system message table** or in **message-table resources embedded in DLLs** that you can load and query with `FormatMessage`. :contentReference[oaicite:0]{index=0}

---

## Quick start

### Build

**MSVC**
```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE errnfo.c
````

**clang-cl**

```bat
clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE errnfo.c
```

**clang (MSVC environment)**

```bat
clang -O2 -Wall -Wextra -DUNICODE -D_UNICODE errnfo.c -o errnfo.exe
```

> If you build with a MinGW toolchain and want wide entry points, you may need `-municode`.

### Use

```text
errnfo [options] <tag> <value>
errnfo [options] <value>          (heuristic: show HR + NT + W32)
```

Examples:

```bat
errnfo hr 0x8034001B
errnfo nt 0xC0000241
errnfo w32 5
errnfo 0xC0000241
```

---

## Tags (domains and environment presets)

Base domains:

* `hr` / `hresult`  → interpret as HRESULT
* `nt` / `ntstatus` → interpret as NTSTATUS
* `w32` / `win32`   → interpret as Win32 error (`DWORD`)

Environment tags (optional, user-tunable):

* `dx` → interpret as HRESULT, but also try DirectX-related DLLs as message sources (example preset)

List available tags:

```bat
errnfo --list-tags
```

---

## Options

### `-m <dll-or-path>` / `--module <dll-or-path>`

Add a message-table module to try (repeatable). This is the primary “make it your own” mechanism.

```bat
errnfo w32 2103 -m netmsg.dll
errnfo w32 12029 -m wininet.dll
errnfo hr  0x887A0005 -m dxgi.dll -m d3d12.dll
```

Why this matters: `FormatMessage` can format strings from the **system message table** or from a **message-table resource in a loaded module**. ([Microsoft Learn][1])

### `--lang <id>`

Use a specific language id for message lookup (e.g. `0x409` for en-US). `FormatMessage` selects messages by **message id + language id**. ([Microsoft Learn][1])

```bat
errnfo --lang 0x409 w32 5
```

### `--no-common`

Disable the built-in “common module list”.

### `-h` / `--help`

Show help.

---

## Message resolution policy

Message strings are retrieved via `FormatMessageW`. ([Microsoft Learn][1])

### WIN32 (`w32`)

1. System message table
2. Tag-default modules (if the tag defines any)
3. User `-m/--module` modules
4. Built-in common modules (unless `--no-common`)

### HRESULT (`hr`)

1. System message table for the HRESULT value
2. If it matches `HRESULT_FROM_WIN32` pattern (`0x80070000 | e`), also show the embedded Win32 message
3. Then: tag/user/common modules as additional sources (same ordering as above)

### NTSTATUS (`nt`)

1. `ntdll.dll` message table (many NTSTATUS values live here)
2. Then: tag/user/common modules
3. Convert to Win32 via `RtlNtStatusToDosError` and show the Win32 message

`RtlNtStatusToDosError` converts an NTSTATUS to the closest Win32 error (and can return `ERROR_MR_MID_NOT_FOUND` if there is no mapping). ([ntdoc.m417z.com][2])

---

## Bitfield breakdowns (what you’ll see)

### HRESULT

`errnfo` prints:

* severity bit (`S`)
* customer bit (`C`)
* NT marker bit (`N`)
* facility
* code

The layout and semantics match the published Windows error-numbering documentation. ([Microsoft Learn][3])

If the HRESULT has the NT marker set, `errnfo` also prints a derived NTSTATUS (by clearing `FACILITY_NT_BIT`).

The documented mapping is:

```c
HRESULT_FROM_NT(x) == (x | FACILITY_NT_BIT)
```

([Microsoft Learn][4])

### NTSTATUS

`errnfo` prints:

* severity (2-bit)
* customer bit
* facility
* code
* derived HRESULT (`HRESULT_FROM_NT`)
* derived Win32 (`RtlNtStatusToDosError`)

NTSTATUS/HRESULT numbering-space relationships are documented in the same reference. ([Microsoft Learn][3])

---

## Extensibility: adding your own tag

Tags are designed to be added by editing only a small, obvious section in the source:

1. **Define a per-tag default module list** (optional)
2. **Write a tag handler** (usually 5–10 lines)
3. **Register the tag** in `g_tags[]`

### Example: add a `myenv` tag that treats input as HRESULT and tries extra DLLs

1. Declare a module list:

```c
static MODLIST g_myenvMods;
```

2. Populate it in `init_tag_modules()`:

```c
modlist_add(&g_myenvMods, L"foo.dll", L"foo.dll");
modlist_add(&g_myenvMods, L"bar.dll", L"bar.dll");
```

3. Add the handler:

```c
static int tag_myenv(TAGCTX *t, int argc, wchar_t **argv) {
    if (argc < 1) return -1;
    uint32_t v = 0;
    if (!parse_u32(argv[0], &v)) return -2;
    print_hresult(t->ctx, v, &g_myenvMods);
    return 0;
}
```

4. Register it:

```c
{ L"myenv", L"Interpret as HRESULT + try foo/bar modules", tag_myenv },
```

That’s it. No other plumbing changes required.

---

## Customizing the “common module list”

The tool contains a small `g_common_module_specs[]` array intended to be edited to match a user’s environment (typical choices depend on what you work with: networking, setup/configuration, graphics, etc.).

* It is tried **after** system/tag/user sources.
* Load failures are warnings and do not stop execution.
* Disable with `--no-common`.

---

## Heuristic mode (when you omit the tag)

If you run:

```bat
errnfo 0xC0000241
```

`errnfo` will print three interpretations:

* as HRESULT
* as NTSTATUS
* as Win32

Heuristic mode is meant for quick exploration; it does not guess the “true” domain.

---

## Exit codes

* `0` success
* `2` usage / parse error

Module load failures are warnings; decoding continues.

---

## Notes

* Many error families do *not* have system-table messages; they live in component DLL message tables and require `-m/--module`. This is expected behavior. ([Microsoft Learn][1])
* NTSTATUS→Win32 conversion is inherently lossy; treat it as a convenience, not a perfect inverse mapping. ([ntdoc.m417z.com][2])

---

## License

Unlicensed / public domain intent

* Initial implementation drafted with AI assistance
* ChatGPT 5.2 Thinking (OpenAI), < 5 min.


[1]: https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-formatmessagew?utm_source=chatgpt.com "FormatMessageW function (winbase.h) - Win32 apps"
[2]: https://ntdoc.m417z.com/rtlntstatustodoserror?utm_source=chatgpt.com "RtlNtStatusToDosError - NtDoc"
[3]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/0642cb2f-2075-4469-918c-4441e69c548a?utm_source=chatgpt.com "[MS-ERREF]: HRESULT"
[4]: https://learn.microsoft.com/en-us/windows/win32/api/winerror/nf-winerror-hresult_from_nt?utm_source=chatgpt.com "HRESULT_FROM_NT macro (winerror.h) - Win32 apps"

