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
````

---

## Usage

Global flags (before the command):

* `--verbose` prints Win32 error messages for non-fatal failures (missing files, access denied, etc.)

Commands:

```text
quuid parse  <guid> [--one-line]
quuid find   <guid> [--wow32|--wow64] [--both-views]
quuid scan   <path> [--registry] [--both-views] [--binary] [--binary-loose] [--locate] [--one-line]
quuid server <clsid-guid> [--scan] [scan flags...]
quuid tlb    <file.tlb|.dll|.ocx>
quuid enum   clsid|iid|typelib|appid [--limit N] [--with-name]
```

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

### Parse (scripting mode)

```bat
quuid parse 6F9619FF-8B86-D011-B42D-00C04FC964FF --one-line
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
* If you need maximum recall, use `--binary-loose` and pair it with `--registry` to filter hits that actually correspond to COM registrations.

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

## Extending quuid

Natural next additions that fit the current architecture:

* Output formats tailored for assembly macro ingestion (`define GUID.NAME ...` / `db` / `dq`)
* Raw GUID scanning for alternate byte orders (wire/network order) as an explicit mode
* Optional reparse traversal with loop detection
* JSON output mode for toolchain integration

---

## License

Unlicensed / public domain intent

* Initial implementation drafted with AI assistance
* ChatGPT 5.2 Thinking (OpenAI), < 5 min.
