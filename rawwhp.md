# rawwhp - execute a raw binary in a minimal WHP guest

`rawwhp` is a single-file Windows CLI that maps a small guest-physical-memory window, loads a raw binary payload, sets an initial CPU mode, and runs one WHP vCPU until a terminal VM-exit.

Design goals:
- Run tiny payloads quickly without full OS boot flow
- Keep mapping and setup minimal and explicit
- Provide detailed VM-exit context for host-side emulation/debugging

---

## Features

- Loads arbitrary binary bytes at `/to` guest physical address
- Starts execution at `/at` (defaults to `/to`)
- Supports startup modes: `real`, `unreal`, `protected`, `long`
- `real`: 16-bit
- `unreal`: 16-bit with expanded segment limits
- `protected`: 32-bit flat, ring-3
- `long`: 64-bit flat, ring-3 with minimal identity tables
- Accepts `/bytes` with compatibility aliases `/len` and `/size`
- Accepts flat hex and `seg:off` addresses (for `/to` and `/at`)
- Reports detailed VM-exit context and emulation points

---

## Build

### Requirements
- Windows with WHP available/enabled
- MSVC or LLVM toolchain in `PATH`

### Compile (x64)

```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rawwhp.c WinHvPlatform.lib
clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rawwhp.c WinHvPlatform.lib
```

---

## Usage

```text
rawwhp.exe [/to:<hex|seg:off>] [/at:<hex|seg:off>] [/bytes:<hex>]
           [/mode:real|unreal|protected|long] [/ticks:<hex>] file
```

Options:
- `/to <value>` load base address (default `0`)
- `/at <value>` execution address (default `/to`)
- `/bytes <value>` logical guest window size from `/to`
  aliases: `/len`, `/size`
- `/mode <name>` `real`, `unreal`, `protected`, `long` (default `real`)
- `/ticks <value>` max `WHvRunVirtualProcessor` iterations (default `0x100000`)

Input format:
- All numeric values are hexadecimal (`4000` means `0x4000`)
- `0x` prefix is accepted
- Addresses can also be `seg:off` (example: `80:100`)

---

## Examples

### Requested syntax examples

```bat
rawwhp /to 7C00 mymbr.bin
rawwhp /to 80:0 /at 80:100 /bytes 4000 test.com
rawwhp /to 800 /mode protected kernel32.bin
```

### Simple controlled fixtures

```bat
rawwhp /to 1000 /mode real hlt.bin
rawwhp /to 2000 /mode protected /ticks 10000 int3.bin
rawwhp /to 3000 /mode long /ticks 20000 vmcall.bin
```

---

## VM-exit behavior

Run loop behavior:
- Success exits: `WHvRunVpExitReasonX64Halt`, `WHvRunVpExitReasonHypercall`
- Timeout exit: returns code `3` when `/ticks` is exhausted
- Other exits: detailed context is printed and execution stops (exit code `4`)

Examples of printed detail:
- `MemoryAccess` with access type, GPA/GVA, instruction bytes
- `Exception` with exception type, error code, and bytes
- `IoPort`, `MsrAccess`, `Cpuid`, `UnsupportedFeature`, APIC traps

`rawwhp` v1 intentionally does not run a re-entrant emulation loop for exits such as MMIO/IO/MSR/CPUID; it stops and reports an emulation point for host handling.

---

## Notes

- Real/unreal mode currently requires payload/entry/stack below `0x100000`.
- `/bytes` defines the logical window from `/to`; runtime scaffolding (stack/GDT/tables as needed) must fit inside this window.
- Mapping is page-aligned internally, but logical validation uses requested addresses and `/bytes`.
- Privileged instructions or unavailable host features can trigger non-success exits.
- In `protected` and `long` modes, payloads are started in ring-3; instructions like `HLT` can fault and surface as non-success exits.
- Some hosts do not allow setting all extended-exit properties; this is reported as warnings and execution continues with platform defaults.
- Real-mode interrupts may hit low-memory vectors (IVT/BDA); those accesses are reported as emulation points when unmapped.

---

## Extending rawwhp

Natural next additions:
- Add a callback-based emulation loop for MMIO/IO/MSR/CPUID paths
- Add register preset import/export and initial register overrides
- Add JSON output mode for automation and CI diagnostics

---

## Exit codes

- `0` success (`HLT` or `Hypercall`)
- `1` runtime/WHP failure
- `2` usage/parse/validation failure
- `3` timeout (`/ticks` exhausted)
- `4` non-success VM-exit reported (emulation point / unsupported path)

---

## License

Unlicensed / public domain intent

- Initial implementation drafted with AI assistance
- ChatGPT 5.2 Thinking (OpenAI), < 5 min.
