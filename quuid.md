# quuid — GUID / COM discovery CLI for Windows

`quuid` is a single-file Windows CLI for **parsing, discovering, and cross-referencing** UUIDs/GUIDs/CLSIDs/IIDs commonly encountered in COM, type libraries, and Windows binaries.

Design goals:
- **Fast answers** for “what is this GUID?”
- **Forensic utility** (scan files/directories for embedded GUIDs)
- **COM awareness** via registry lookups (CLSID / Interface / TypeLib / AppID)
- **Minimal footprint**: one C file, MSVC build, no third-party deps

---

## Features

### Parse GUIDs into useful forms
- Accepts canonical braced, dashed, 32-hex-digit, and C initializer input
- Braced canonical form (`{...}`)
- Dashed form
- Field breakdown (`Data1/Data2/Data3/Data4`)
- C initializer form
- Raw in-memory byte layout (`db` list)

### Find COM registry meaning for a GUID
Queries common COM registration loci under `HKCR`:
- `HKCR\CLSID\{...}`
- `HKCR\Interface\{...}`
- `HKCR\TypeLib\{...}`
- `HKCR\AppID\{...}`

Includes extra details such as `ThreadingModel`, `ProxyStubClsid32`, and TypeLib `win32/win64` paths when present.

Supports registry view selection:
- `--wow64` for 64-bit registry view
- `--wow32` for 32-bit registry view
- `--both-views` to print both (tagged `:64` / `:32`)

### Scan files and directory trees for GUIDs
Scans for:
- ASCII GUIDs (braced or dashed)
- Optional **binary GUIDs** (16-byte memory-layout GUIDs) with:
  - `--binary` (RFC4122-ish variant + version heuristic)
  - `--binary-loose` (variant-only heuristic; noisier)

Extra scan knobs:
- `--locate` prints `file:offset:kind:{guid}` per match (for forensic offset work)
- `--registry` cross-references each unique GUID against the registry
- `--both-views` cross-references against both 32/64 registry views
- Reparse points (symlinks/junctions) are skipped to avoid loops

### Pivot CLSID → server binary
`server` resolves `CLSID` → `InprocServer32` / `LocalServer32` (expanded) and can optionally scan the server binary.

### Enumerate registry categories
Lists GUID-named subkeys under:
- `HKCR\CLSID`
- `HKCR\Interface`
- `HKCR\TypeLib`
- `HKCR\AppID`

Optional:
- `--with-name` also prints the default value for each subkey when present.

### Enumerate TypeLib contents
Loads a type library (`.tlb`, or a `.dll`/`.ocx` with embedded TypeLib) and prints:
- LIBID (TypeLib GUID), version, LCID, SYSKIND
- Each type’s GUID, kind, and name when available

---

## Build

### Requirements
- Windows
- MSVC (Visual Studio Developer Command Prompt)

### Compile (x64)
```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE quuid.c ole32.lib oleaut32.lib advapi32.lib
```

---

## Usage

Global options:

* `--verbose` prints the underlying Win32, registry, and HRESULT details for incomplete operations.
* `--help` prints command usage.

Options are accepted in any unambiguous position before or after the command's required argument. Use `--` to treat the remaining tokens as positionals.

Commands:

```text
quuid parse  <guid> [--one-line]
quuid find   <guid> [--wow32|--wow64] [--both-views]
quuid scan   <path> [--registry] [--both-views] [--binary] [--binary-loose] [--locate] [--one-line]
quuid server <clsid-guid> [--scan] [scan flags...]
quuid tlb    <file.tlb|.dll|.ocx>
quuid enum   clsid|iid|typelib|appid [--limit N] [--with-name]
```

Registry-aware commands accept `--wow32`, `--wow64`, or `--both-views`. `scan` accepts `--registry`, `--binary`, `--binary-loose`, `--locate`, and `--one-line`. The same scan options are available to `server` when `--scan` is present. `enum` defaults to 100 entries; `--limit 0` means no limit.

Exit status is part of the command contract:

* `0`: the requested work completed. A valid lookup with no registry match is still complete.
* `1`: a runtime operation was incomplete or failed.
* `2`: the command line or GUID input was invalid.

Normal output stays terse. An incomplete operation always emits a summary; add `--verbose` for the individual API failures.

---

## Examples

### PowerShell note

PowerShell treats `{...}` as a script block. Quote braced GUIDs:

```bat
quuid find "{00021401-0000-0000-C000-000000000046}" --wow64
```

### Parse a GUID

```bat
quuid parse 6F9619FF-8B86-D011-B42D-00C04FC964FF
```

### Parse a C GUID initializer

Quote the initializer so PowerShell passes it as one argument:

```powershell
quuid parse '{0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}}'
```

### Parse (scripting mode)

```bat
quuid parse 6F9619FF-8B86-D011-B42D-00C04FC964FF --one-line
```

Options may also lead naturally:

```bat
quuid --one-line parse 6F9619FF-8B86-D011-B42D-00C04FC964FF
```

### Find COM registration (64-bit view)

```bat
quuid find {00021401-0000-0000-C000-000000000046} --wow64
```

### Find in both registry views

```bat
quuid find {00021401-0000-0000-C000-000000000046} --both-views
```

### Scan System32 for ASCII GUIDs and cross-reference registry

```bat
quuid scan C:\Windows\System32 --registry
```

### Scan with per-hit offsets (forensics)

```bat
quuid scan C:\Windows\System32 --locate
```

### Scan for binary GUIDs too (heuristic)

```bat
quuid scan C:\Windows\System32 --binary
```

### Pivot CLSID → server module, then scan the module

```bat
quuid server {00021401-0000-0000-C000-000000000046} --scan --binary --registry --both-views
```

### Enumerate first 50 CLSIDs (with names)

```bat
quuid enum clsid --limit 50 --with-name
```

### Enumerate TypeLib contents

```bat
quuid tlb C:\Windows\System32\stdole2.tlb
```

---

## Notes on accuracy vs noise

* ASCII scanning is typically high-signal.
* Binary scanning can be very noisy in general binaries; `--binary` uses a variant+version heuristic to keep the set smaller.
* If you need maximum recall, use `--binary-loose`; `--registry` adds COM registration context to the resulting GUIDs.
* Noisy results are intentional. Redirect or post-process them rather than expecting `quuid` to grow filtering and output-policy options.

---

## Why this exists

When doing low-level Windows/COM work you often see GUIDs in:

* registry dumps
* PE resources and strings
* type libraries
* debugger output
* COM interface declarations (IDL)

`quuid` gives you a fast way to turn “random GUID noise” into structured meaning and a path to the corresponding COM registration footprint.

---

## Testing

From an MSVC Developer Command Prompt:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests\quuid\run.ps1
```

The focused suite covers accepted GUID forms, flexible option placement, malformed input, exit codes, and exact ASCII/binary scan behavior across the 4 MiB chunk boundary.

## Scope

`quuid` is intentionally a narrow GUID/COM investigation tool. Prefer correctness improvements inside the existing parse, registry, scan, server, enumeration, and TypeLib workflows. Extension filters, result caps, sorting controls, JSON schemas, alternate scan policies, and reparse traversal are deliberately left to shell composition or other tools.

---

## License

Unlicensed / public domain intent

* Initial implementation drafted with AI assistance
* ChatGPT 5.2 Thinking (OpenAI), < 5 min.
