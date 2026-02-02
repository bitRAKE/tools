# errnfo

`errnfo` is a small Windows CLI that helps you **decode error numbers quickly** and **extend message lookup** using additional message-table modules (`RT_MESSAGETABLE`). It is designed for the common workflow:

- you have a number (HRESULT / NTSTATUS / Win32 error),
- you usually know the “source class”,
- you want an immediate explanation (and useful structure like facility/severity),
- and when the system table doesn’t have the text, you want to pull messages from the relevant DLLs without building a whole debugger pipeline.

`errnfo` supports three modes:

- **decode** (default): interpret a number with a tag (`hr`, `nt`, `w32`, etc.), or heuristically show several interpretations.
- **scan**: discover binaries that contain message tables (to build your own module list).
- **dump**: inspect a single module’s message-table contents (including listing message IDs/strings with filters).

---

## Quick start

### Decode (the main feature)

Terse “tag + number” form:

```bat
errnfo hr 0x8034001B
errnfo nt 0xC0000241
errnfo w32 5
````

Heuristic “number only” form:

```bat
errnfo 0xC0000241
```

This prints blocks for **HRESULT**, **NTSTATUS**, and **WIN32** so you can quickly see which one matches your context.

### Add message modules (better text coverage)

When a message isn’t in the system message table, it may exist in the **module that defines it** (or a related subsystem DLL). Add one or more modules:

```bat
errnfo w32 12029 -m wininet.dll
errnfo hr  0x80070005 -m netmsg.dll
errnfo hr  0x887A0005 -m dxgi.dll -m d3d12.dll
```

`-m/--module` is repeatable. You can keep a personal list in a script or by pasting from `scan` output.

---

## Why this tool exists

Windows errors are represented in several common formats:

* **Win32** error codes (e.g. from `GetLastError`)
* **HRESULT** (COM, DirectX, many APIs)
* **NTSTATUS** (native/NT layer, kernel/user boundary)

Even when the numeric value is known, message text may be stored in different places:

* the **system** message table
* a **module’s message-table resource** (`RT_MESSAGETABLE`)
* or nowhere (in which case only structure and derived forms help)

`errnfo` focuses on:

1. **fast decode**, 2) **message lookup**, 3) **making your environment reusable**.

---

## Mode 1: decode (default)

### Syntax

```text
errnfo [decode-options] <tag> <value>
errnfo [decode-options] <value>
```

### Tags

Common tags:

* `hr`, `hresult` — interpret as HRESULT
* `nt`, `ntstatus` — interpret as NTSTATUS
* `w32`, `win32` — interpret as Win32 error code
* `dx` — interpret as HRESULT and also try common DirectX-related modules for message text

List all tags:

```bat
errnfo --list-tags
```

### Output structure

Decode output is intentionally structured:

* **value** in hex/decimal/signed
* **severity** and **facility** breakdown (for HRESULT/NTSTATUS)
* **message text** (if available)
* **derived forms** where useful:

  * `HRESULT_FROM_WIN32` embedded Win32 message
  * NTSTATUS-derived HRESULT and Win32 (via `RtlNtStatusToDosError` when available)

This makes it practical even when message text is missing.

### Decode options

```text
-m, --module <dll-or-path>    Add message-table module (repeatable)
    --lang <id>               Language ID for FormatMessage (decode). Default: 0 (user default)
    --no-common               Disable built-in common module list
    --list-tags               List tags
-h, --help                    Help
```

`--lang` uses Windows LANGID values such as `0x409` (en-US).

---

## Mode 2: scan (discovery)

`scan` finds files that contain `RT_MESSAGETABLE` resources. This is for building a personal or project-specific list of modules you commonly care about.

### Syntax

```text
errnfo scan <dir> [--recursive] [--paths] [--verbose]
```

### Examples

Create a pasteable module list:

```bat
errnfo scan C:\Windows\System32 --recursive > msgmods.txt
```

Output looks like:

```text
-m "C:\Windows\System32\netmsg.dll"
-m "C:\Windows\System32\wininet.dll"
...
```

Paths only:

```bat
errnfo scan C:\Windows\System32 --paths > msgmods_paths.txt
```

### Notes

* `scan` is discovery only: it does **not** dump message strings (that belongs in `dump`).
* Windows contains more message-table modules than most people expect (languages, MUIs, side-by-side components). `scan` helps you find what matters in *your* environment.

---

## Mode 3: dump (module inspection)

`dump` is a **single-module** inspection tool for message tables. Use it when you want to see:

* which message tables exist,
* which languages they contain,
* the ID ranges stored in blocks,
* and optionally the messages themselves.

This mode is designed to avoid output floods by supporting filters.

### Syntax

```text
errnfo dump <module-or-path> [--tables] [--langs] [--list]
                         [--lang <id>]
                         [--id-min <n>] [--id-max <n>]
                         [--grep <substr>]
                         [--max <n>]
                         [--verbose]
```

### Defaults

If you don’t specify `--tables/--langs/--list`, `dump` defaults to **`--tables`**.

### Examples

Show message tables and their block counts:

```bat
errnfo dump netmsg.dll --tables
```

Show resource languages present in the module:

```bat
errnfo dump wininet.dll --langs
```

List messages but filter aggressively:

```bat
errnfo dump wininet.dll --list --grep timeout --max 50
```

Only dump one resource language (resource language filtering):

```bat
errnfo dump ntdll.dll --list --lang 0x409 --id-min 0xC0000000 --id-max 0xC0000300
```

### Filters (recommended when using `--list`)

* `--lang 0x409` limits output to one **resource language**
* `--id-min / --id-max` limits the ID range
* `--grep` substring filter on message text (case-insensitive)
* `--max` caps the number of printed entries

---

## Extensibility and customization

### Add your own tags

Tags are implemented as a small table:

* a tag name (string)
* a help string
* a handler function

Add a new tag by:

1. defining a handler (similar to `tag_dx`)
2. optionally creating a `MODLIST` for tag-specific default modules
3. adding an entry to the `g_tags[]` array

This makes it easy to create environment-specific workflows:

* graphics (`dx`)
* networking (`net`)
* setup/driver (`setup`)
* security (`sec`)
* etc.

### Build your own module list

The recommended workflow:

1. Run scan:

```bat
errnfo scan C:\Windows\System32 --recursive > msgmods.txt
```

2. Prune the list to what matters to you (keep it small).

3. Use it in your shell scripts or build environments:

```bat
for /f "usebackq delims=" %L in ("msgmods.txt") do @echo %L
```

Or paste the relevant `-m` lines into your own wrapper script.

---

## Practical coverage expectations

`errnfo` does well when:

* the error class is known (HRESULT/NTSTATUS/WIN32),
* and message text exists in either:

  * the system message table, or
  * a module message table you include via `-m`.

It will still provide value when message text does not exist:

* facility/severity/code breakdown
* derived forms (e.g., NTSTATUS → Win32)

Some systems return richer context out-of-band (e.g., COM `IErrorInfo`, protocol-specific “last response” text). Those are intentionally out of scope for this tool’s first version.

---

## Build notes

`errnfo.c` is a single-file build with Win32 APIs.

Examples:

```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE errnfo.c
clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE errnfo.c
clang -march=native -O3 -DUNICODE -D_UNICODE errnfo.c
```

---

## Help

```bat
errnfo --help
```

---

## Suggested workflow patterns

### 1) Quick decode while debugging

```bat
errnfo hr %ERRORLEVEL%
```

### 2) Add one module when you know the subsystem

```bat
errnfo w32 12029 -m wininet.dll
```

### 3) Build an environment list once

```bat
errnfo scan C:\Windows\System32 --recursive > msgmods.txt
```

### 4) Inspect a module before adding it

```bat
errnfo dump wininet.dll --tables
errnfo dump wininet.dll --list --grep timeout --max 20
```

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
