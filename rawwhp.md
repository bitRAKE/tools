# rawwhp - run WHP guest code with explicit memory areas and dumps

`rawwhp` is a single-file Windows CLI that executes guest code in WHP using one or more explicit guest-physical memory areas, then optionally dumps memory ranges before teardown.

Design goals:
- Support sparse, non-overlapping guest memory setup
- Allow optional per-area file initialization
- Support post-run round-trip workflows via configurable dumps

---

## Features

- Repeatable `/area <start> <length> [file]` declarations
- Repeatable `/dump <start> <length> [file]` declarations
- Default entry at first area start (unless `/pedantic` is used)
- `/at` accepts flat hex and `seg:off`
- Mode support: `real`, `unreal`, `protected`, `long`
- Strict-mode policy toggle via `/pedantic`
- Pretty hexdump output to screen when `/dump` omits a file

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
rawwhp [/mode real|unreal|protected|long] [/ticks <hex>] [/pedantic]
       [/at <hex|seg:off>]
       /area <start> <length> [file]...
       [/dump <start> <length> [file]]...
```

Options:
- `/area <start> <length> [file]`
  - Adds one non-overlapping GPA range.
  - If `file` is present, bytes are copied from file start.
  - If file is shorter than length: remaining bytes stay zero.
  - If file is longer than length: file is truncated to area length.
- `/dump <start> <length> [file]`
  - Dumps GPA bytes after the VCPU run loop exits.
  - Without `file`, output is a formatted hex+ASCII dump to stdout.
  - With `file`, bytes are written to that file.
- `/at <hex|seg:off>`
  - Entry point.
  - `real`/`unreal`: `seg:off` is used as explicit CS:RIP; flat values are converted.
  - `protected`/`long`: interpreted as linear RIP.
- `/mode <name>`: `real`, `unreal`, `protected`, `long` (default `real`)
- `/ticks <hex>`: max run-loop iterations (default `0x100000`)
- `/pedantic`: strict mode

Removed options:
- `/to`, `/bytes`, `/len`, `/size` are no longer supported.

Input format:
- All numeric values are hexadecimal (`1000` means `0x1000`)
- `0x` prefix is accepted
- Address fields can use `seg:off`

---

## Strict Mode (`/pedantic`)

`/pedantic` changes behavior:
- `/at` is required.
- Dump target files must not already exist.
- Area/file length mismatch emits warnings.
- Runtime scaffolding must fit user-provided areas (no hidden runtime area is auto-added).

Without `/pedantic`:
- `/at` defaults to first `/area` start.
- Hidden runtime area may be auto-added for stack/GDT/page tables.
- Dump files are overwritten in command order.

---

## Examples

Single area, explicit entry:

```bat
rawwhp /area 7C00 200 mymbr.bin /at 7C00
```

Multiple areas:

```bat
rawwhp /area 8000 2000 code.bin /area 20000 1000 data.bin /mode protected /at 8000
```

Round-trip dump to file:

```bat
rawwhp /area 10000 4000 code.bin /at 10000 /dump 10000 20 output.bin
```

Dump to screen:

```bat
rawwhp /area 1000 100 mymbr.bin /at 1000 /dump 1000 20
```

Pedantic mode:

```bat
rawwhp /pedantic /area 10000 8000 input.bin /at 10000 /dump 10000 10 out.bin
```

---

## VM-exit behavior

Run loop termination:
- Success exits: `WHvRunVpExitReasonX64Halt`, `WHvRunVpExitReasonHypercall`
- Timeout: `/ticks` exhausted
- Other exits: detailed context is printed and execution stops

---

## Notes

- Real/unreal currently require entry/stack below `0x100000`.
- `/area` ranges must not overlap.
- `/dump` ranges must be fully covered by mapped GPA ranges.
- In `protected` and `long` mode, guest runs at ring-3 defaults; privileged instructions may fault.
- If WHP property setup is partially unsupported on host, warnings are printed and execution continues with defaults.

---

## Exit codes

- `0` success (`HLT` or `Hypercall`)
- `1` runtime/WHP failure
- `2` usage/parse/validation failure
- `3` timeout (`/ticks` exhausted)
- `4` non-success VM-exit reported

---

## License

Unlicensed / public domain intent

- Initial implementation drafted with AI assistance
- ChatGPT 5.2 Thinking (OpenAI), < 5 min.
