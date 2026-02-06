# wchain — wait-chain inspector for Windows

`wchain` is a single-file Windows CLI that inspects thread wait chains using Wait Chain Traversal (WCT) APIs.

Default behavior is tuned for quick UI hang triage:
- target the current process
- inspect GUI threads only
- print only blocked/waiting threads

---

## Features

- Enumerates threads in a process and queries each thread with `GetThreadWaitChain`
- Reports wait-chain nodes (thread, critical section, mutex, COM, ALPC, etc.)
- Detects and displays wait cycles (`cycle=yes`) from WCT
- Supports focused filters:
  - GUI-only (default)
  - all threads (`--all-threads`)
  - include running/non-blocked threads (`--include-running`)

---

## Build

### Requirements
- Windows
- MSVC or LLVM toolchain in `PATH` (Developer Command Prompt or equivalent)

### Compile (x64)

```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE wchain.c advapi32.lib user32.lib
clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE wchain.c advapi32.lib user32.lib
```

---

## Usage

```text
wchain [--pid <pid>|--self] [--all-threads] [--include-running] [--verbose]
```

Options:

- `--pid <pid>` target process ID (decimal or `0x`-prefixed hex)
- `--self` use current process ID (default)
- `--all-threads` inspect non-GUI threads too
- `--include-running` also print threads that are not currently blocked
- `--verbose` print additional Win32 error text
- `-h`, `--help` show help

---

## Examples

Inspect blocked GUI threads in current process:

```bat
wchain --self
```

Inspect a known PID (replace `1234`):

```bat
wchain --pid 1234 --all-threads
```

Inspect a known PID and include running threads:

```bat
wchain --pid 1234 --all-threads --include-running
```

Get a running process PID (current PowerShell host) and inspect it:

```powershell
$targetPid = (Get-Process -Id $PID | Select-Object -ExpandProperty Id)
wchain --pid $targetPid --all-threads --include-running
```

Create a temporary process, inspect it, then clean up (PowerShell):

```powershell
$p = Start-Process powershell -ArgumentList '-NoLogo','-NoProfile','-Command','Start-Sleep -Seconds 20' -PassThru
try {
    wchain --pid $p.Id --all-threads --include-running
} finally {
    Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
}
```

Inspect with verbose API error details:

```bat
wchain --pid 1234 --all-threads --verbose
```

---

## Notes

- GUI-thread classification uses `GetGUIThreadInfo`; inaccessible threads may be treated as non-GUI.
- `--include-running` is useful for full snapshots; default output is intentionally terse for blocked-thread analysis.
- Access to some processes/threads can fail due to permissions or process protection level.

---

## Extending wchain

Natural next additions that fit the current architecture:

- `--json` output mode for automation
- `--tid <tid>` direct thread targeting
- optional deadlock-only mode (`cycle=yes` only)

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
