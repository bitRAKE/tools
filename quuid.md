# quuid — GUID / COM discovery CLI for Windows

`quuid` is a single-file Windows command-line tool that helps you **parse, discover, and cross-reference** UUIDs/GUIDs/CLSIDs/IIDs commonly found in COM, type libraries, and Windows binaries.

It is designed to be:
- **Practical**: answers “what is this GUID?” quickly.
- **Forensic-friendly**: scans files for embedded GUID text.
- **COM-aware**: queries the Windows registry for CLSID/IID/TypeLib/AppID registrations.
- **Minimal**: one C source file, builds with MSVC.

## Features

### 1) Parse GUIDs into multiple forms
- Canonical braced/dashed strings
- C initializer form
- Raw in-memory byte layout (`db` list)

### 2) Registry discovery (COM cross-reference)
Given a GUID, `quuid` checks common COM registration locations under `HKCR`:
- `HKCR\CLSID\{...}` (class registrations)
- `HKCR\Interface\{...}` (interface registrations)
- `HKCR\TypeLib\{...}` (type library registrations)
- `HKCR\AppID\{...}` (application registrations)

It prints relevant subkeys/values when present (e.g., `InprocServer32`, `LocalServer32`, `ProxyStubClsid32`, etc.).

### 3) Scan files/directories for GUID text
Scans a file or directory tree for ASCII GUID patterns in the form:
- `{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}`
- `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`

Optionally cross-references each discovered GUID against the registry.

### 4) Enumerate TypeLib contents
Loads a type library (`.tlb`, or a `.dll`/`.ocx` containing an embedded type library) via `LoadTypeLibEx` and prints:
- LIBID (TypeLib GUID), version, LCID
- Types and their GUIDs (interfaces, coclasses, records, etc.)

### 5) Enumerate registry categories
Dumps GUID subkeys under:
- `HKCR\CLSID`
- `HKCR\Interface`
- `HKCR\TypeLib`
- `HKCR\AppID`

This is primarily for reconnaissance or indexing.

## Build

### Requirements
- Windows
- MSVC (Visual Studio Developer Command Prompt)

### Compile (x64)
```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE quuid.c ole32.lib oleaut32.lib advapi32.lib
````

Output: `quuid.exe`

## Usage

```text
quuid parse <guid>
quuid find  <guid>
quuid scan  <path> [--registry]
quuid tlb   <file.tlb|.dll|.ocx>
quuid enum  clsid|iid|typelib|appid [--limit N]
```

## Examples

### Parse a GUID and print representations

```bat
quuid parse 6F9619FF-8B86-D011-B42D-00C04FC964FF
```

### Find COM registry meaning for a GUID

```bat
quuid find {00021401-0000-0000-C000-000000000046}
```

### Scan a directory for GUID text and cross-reference registry hits

```bat
quuid scan C:\Windows\System32 --registry
```

### Enumerate a type library (TLB)

```bat
quuid tlb C:\Windows\System32\stdole2.tlb
```

### List first 50 CLSID registrations

```bat
quuid enum clsid --limit 50
```

## Output notes

### `parse`

Produces:

* Braced string (from `StringFromGUID2`)
* Dashed string
* Field breakdown (`Data1/Data2/Data3/Data4`)
* C initializer
* Raw bytes as stored in memory (little-endian fields)

This is useful for:

* Writing constants into C/C++
* Emitting `db` sequences for assemblers
* Debugging endianness mismatches

### `find`

Prints only categories that exist (CLSID / IID / TypeLib / AppID). If there are no hits:

```text
(no HKCR hits in CLSID/Interface/TypeLib/AppID)
```

### `scan`

Reports:

* total files scanned
* total bytes scanned
* raw matches found (may include duplicates)
* unique GUIDs (deduplicated)

Then prints unique GUIDs; with `--registry` it prints registry hits per GUID.

### `tlb`

Prints LIBID and then each type’s GUID, kind (interface/coclass/etc), and name if available.

## Limitations (current)

* Scanning detects only **ASCII textual GUIDs** (36-char dashed or 38-char braced).

  * It does **not** currently detect raw 16-byte GUID structures.
* Directory traversal uses Win32 `FindFirstFileW`/`FindNextFileW` and does not special-case symlink loops.
* Scanning uses memory-mapped I/O; extremely large files may be slow or pressure address space.

## Roadmap ideas

* Raw 16-byte GUID scanning with heuristics (RFC4122 version/variant filtering + context scoring)
* Scan “server path” binaries for additional GUIDs (CLSID → InprocServer32 → scan)
* WOW6432Node / HKLM deltas to compare 32-bit vs 64-bit registrations
* Output formatting modes suitable for macro ingestion (e.g., `define GUID.NAME {...}` or `db` blocks)
* JSON output for automation pipelines

## License

Unlicensed / public domain intent (adjust to your preferred license before publishing).

## Contributing

Keep it simple:

* single-file C source preferred
* add features behind new subcommands or flags
* avoid dependencies beyond Win32/OLE APIs
* test on clean VMs (registry state matters)

## Security / safety notes

* `scan` is read-only. It opens files for reading with sharing enabled.
* `--registry` reads COM registration data from HKCR; results are system-dependent.

## Why this exists

When doing low-level Windows/COM work you often see GUIDs in:

* registry dumps
* PE resources and strings
* type libraries
* debugger output
* COM interface declarations (IDL)

`quuid` gives you a fast way to turn “random GUID noise” into structured meaning and a path to the corresponding COM registration footprint.
