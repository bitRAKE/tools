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
- `/report <file>` writes machine-readable JSON for each run
- Mode support: `real`, `unreal`, `protected`, `long`
- Optional `/cpl 0|3` for `protected`/`long` guest privilege
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
rawwhp [/mode real|unreal|protected|long] [/cpl 0|3] [/ticks <hex>] [/pedantic]
       [/at <hex|seg:off>]
       [/report <file>]
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
- `/report <file>`
  - Writes a JSON report for automation.
  - Includes host capability flags, final exit reason/details, mapped segments, dumps, and elapsed microseconds.
- `/mode <name>`: `real`, `unreal`, `protected`, `long` (default `real`)
- `/cpl <0|3>`
  - Applies to `protected` and `long`.
  - `3` is default for those modes (user ring behavior).
  - `0` enables kernel-ring behavior for broader privileged exit topology.
  - In `real`/`unreal`, only `0` is valid.
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

Kernel-ring long mode probe:

```bat
rawwhp /mode long /cpl 0 /area 10000 100 rdmsr.bin /at 10000 /report run.json
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

JSON report:

```bat
rawwhp /area 10000 100 hlt.bin /at 10000 /report run.json
```

---

## Report Schema (`/report`)

Top-level keys:
- `mode`, `cpl`, `at`, `ticks`
- `areas[]`: includes `start`, `length`, optional `file`, `mapped_page_start`, `mapped_page_end`
- `capabilities`:
  - `extended_vm_exits_supported`, `extended_vm_exits_supported_mask`
  - `extended_vm_exits_requested`, `extended_vm_exits_requested_mask`
  - `extended_vm_exits_enabled`, `extended_vm_exits_enabled_mask`
  - `exception_bitmap_supported`, `exception_bitmap_supported_mask`
  - `exception_bitmap_requested`, `exception_bitmap_requested_mask`
  - `exception_bitmap_enabled`, `exception_bitmap_enabled_mask`
  - `msr_exit_bitmap_supported`, `msr_exit_bitmap_supported_mask`
  - `msr_exit_bitmap_requested`, `msr_exit_bitmap_requested_mask`
  - `msr_exit_bitmap_enabled`, `msr_exit_bitmap_enabled_mask`
- `run`: `result` (`success|vm_exit|setup_error`), `exit_reason`, `rip`, `details`
- `dumps[]`: `start`, `length`, `target`, optional `file`, `status` (`ok|error|not_run`)
- `timing.elapsed_us`
- `maps[]`: merged page-aligned mapped segments

`run.details` contains decoded payloads when available:
- `memory` (for `MemoryAccess`)
- `io` (for `X64IoPortAccess`)
- `msr` (for `X64MsrAccess`)
- `exception` (for `Exception`)

Capability semantics:
- `*_supported` and `*_supported_mask` come from `WHvGetCapability(...)` (host support).
- `*_requested_mask` is the final mask `rawwhp` asked WHP to use after compatibility filtering/probing.
- `*_enabled` means the corresponding `WHvSetPartitionProperty(...)` call succeeded for that requested mask.
- `extended_vm_exits_supported_mask` may be broader than `extended_vm_exits_requested_mask` on a given host/partition.

---

## Test Workflow

Run discovery matrix:

```bat
powershell -ExecutionPolicy Bypass -File tests\rawwhp\discover.ps1
```

Run integration suite:

```bat
powershell -ExecutionPolicy Bypass -File tests\rawwhp\run.ps1
```

Skip rebuild when iterating quickly:

```bat
powershell -ExecutionPolicy Bypass -File tests\rawwhp\discover.ps1 -SkipBuild
powershell -ExecutionPolicy Bypass -File tests\rawwhp\run.ps1 -SkipBuild
```

Suite behavior:
- Mapping and dump validation checks run on every host.
- Strict exit matrix uses `tests\rawwhp\expected\strict_exits.json`.
- If host does not report required WHP support (`WHvGetCapability`), strict matrix is skipped with explicit status.
- If host reports support but partition enable fails, the suite fails fast with a configuration error.
- Discovery runs `protected`/`long` probes at both `cpl=3` and `cpl=0`.

Probe sources:
- Generated probe binaries: `tests\rawwhp\probes\generated\`
- Optional fasm templates: `tests\rawwhp\probes\probe16.asm`, `probe32.asm`, `probe64.asm`

---

## Host/Guest Layering

`rawwhp` behavior comes from four layers:

1. Host capability layer (`WHvGetCapability`)
   - Reports what the platform can support (`*_supported`, `*_supported_mask`).
2. Partition configuration layer (`WHvSetPartitionProperty`, `WHvGetPartitionProperty`)
   - Applies the requested behavior for this partition (`*_requested*`, `*_enabled*`).
3. Guest construction layer (`/mode`, `/cpl`, `/area`, `/at`)
   - Controls privilege level, segmentation/paging shape, and mapped GPA ranges.
4. Probe/instruction layer (the bytes you execute)
   - Determines which exit reason actually occurs under the selected configuration.

Practical implication:
- Host support alone does not guarantee an exit.
- The partition property must be accepted.
- The guest must be configured so the instruction is legal/reachable for that mode/CPL.

---

## Targeted Workflow

Use this loop when you want to validate one specific exit/failure mode:

1. Discover host + baseline topology
   - `discover.ps1` and inspect `host_caps.json` + `matrix.jsonl`.
2. Pick target mode/CPL/exit
   - Example: MSR exits usually require `protected/long` with `/cpl 0`.
3. Build or edit a focused probe
   - Use `tests\rawwhp\probes\probe16.asm|probe32.asm|probe64.asm` with `C:\fasm\fasm2\fasm2.cmd`.
4. Reproduce with one direct `rawwhp` command
   - Always include `/report` and explicit `/ticks`.
5. Promote to strict suite
   - Add/adjust a case in `tests\rawwhp\expected\strict_exits.json`.
6. Re-run `run.ps1`
   - Confirms behavior and catches regressions on this host.

Useful direct command template:

```bat
_test\rawwhp.exe /mode long /cpl 0 /area 10000 100 probe.bin /at 10000 /ticks 400 /report run.json
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
- In `protected` and `long` mode, default is `cpl=3`; use `/cpl 0` for kernel-ring behavior.
- `rawwhp` aligns Extended VM exits to host support, excludes `GpaAccessFaultExit` by default (to preserve instruction progress), and probes a reduced mask if needed.
- If WHP property setup is partially unsupported on host, warnings are printed and execution continues with the best accepted configuration.
- GPA mapping is page-oriented in `rawwhp`: areas are rounded to 4 KiB and merged into contiguous map segments.
- Long-mode guest page tables are identity-mapped with 2 MiB pages over those mapped segments.
- Separate `/area` ranges that land in the same 4 KiB page will share that mapped page (the in-between bytes are still mapped).
- `rawwhp` unmaps all mapped segments at teardown; it does not currently do incremental unmap during a run.
- Sub-page carve-outs are not possible with the current approach; if you need hard holes, split regions on 4 KiB boundaries.

Common corner cases:
- In non-pedantic mode, a hidden runtime area may be auto-added for stack/GDT/page tables; this can change map topology.
- In pedantic mode, runtime scaffolding must fit inside user areas; fully packed areas can fail layout planning.
- In `protected`/`long` with `cpl=3`, privileged instructions often raise `#GP` (`Exception type=13`) instead of exiting as privileged operations.
- `extended_vm_exits_supported_mask` can be larger than the accepted/requested mask for a partition.

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
