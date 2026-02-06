# modsnap — process module snapshot CLI for Windows

`modsnap` is a single-file Windows CLI for listing loaded modules (DLL/EXE images) in a target process.

Design goals:
- Fast module visibility for a PID
- Script-friendly output modes (`--paths`, `--csv`, `--count`)
- Minimal footprint: one C file, MSVC/LLVM build, no third-party deps
- Win32-first implementation via Tool Help APIs

---

## Features

- Snapshot loaded modules for a process (`CreateToolhelp32Snapshot` + `Module32First/Next`)
- Show base address, image size, module name, and full path
- Output modes:
  - default table view
  - paths only
  - CSV (`pid,base,size,module,path`)
  - count only

---

## Build

### Requirements
- Windows
- MSVC or LLVM toolchain in `PATH` (Developer Command Prompt or equivalent)

### Compile (x64)

```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE modsnap.c
clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE modsnap.c
```

---

## Usage

```text
modsnap [--pid <pid>|--self] [--paths|--csv|--count] [--verbose]
```

Options:

- `--pid <pid>` target process ID (decimal or `0x` hex)
- `--self` use current process ID (default)
- `--paths` print only module paths
- `--csv` print CSV rows
- `--count` print only module count
- `--verbose` include Win32 error text on failures
- `-h`, `--help` show help

---

## Examples

List current process modules in table form:

```bat
modsnap --self
```

Print only module paths:

```bat
modsnap --self --paths
```

Emit CSV:

```bat
modsnap --self --csv
```

Print only module count:

```bat
modsnap --self --count
```

---

## Notes

- `--paths`, `--csv`, and `--count` are mutually exclusive.
- Access to some PIDs can fail due to permissions, protected processes, or cross-bitness limitations.

---

## Extending modsnap

Natural next additions that fit the current architecture:

- `--sort <base|name|path>` output ordering controls
- `--json` machine-readable output mode
- optional module filtering by substring or wildcard

---

## Exit codes

- `0` success
- `1` runtime/API failure
- `2` usage / parse error

---

## License

Unlicensed / public domain intent

- Initial implementation drafted with AI assistance
- ChatGPT 5.2 Thinking (OpenAI), < 5 min.
