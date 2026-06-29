# Tools

A small collection of **single-file C CLI tools** for Windows. Each tool is designed to be:

- Minimal (one `.c` file)
- Practical and forensic-friendly
- Easy to build with MSVC or clang-cl
- Documented with a matching `.md` file

## Tools

- `errnfo` — decode HRESULT/NTSTATUS/Win32 error codes; scan and dump message tables.
- `hdrinfo` — report Windows display HDR/output metadata, color encoding, and troubleshooting hints.
- `modsnap` — snapshot a process module list (base, size, name, path) with table/CSV/path/count output.
- `quuid` — GUID/COM discovery: parse, registry lookup, scan binaries, inspect type libraries.
- `rawwhp` — define multiple WHP guest memory areas, run code, and dump selected GPA ranges.
- `rpscan` — reparse-point scanner for paths (symlinks/junctions/mount points/etc.).
- `wchain` — inspect per-thread wait chains (WCT) to report blocked/waiting threads and wait cycles.

## Out Of Scope (Transient)

- `uwpchar` — Windows GUI icon-font browser/export utility. This is intentionally outside the CLI scope of this repo and is likely temporary; if similar tools accumulate, they should migrate to a separate repo.

## Build

Each tool is a single C file. Example (MSVC):

```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE <tool>.c
```

See each tool's `.md` for usage, examples, and build notes.

## Tool Tests

`rawwhp` includes a discovery matrix and strict integration suite:

```bat
powershell -ExecutionPolicy Bypass -File tests\rawwhp\discover.ps1
powershell -ExecutionPolicy Bypass -File tests\rawwhp\run.ps1
```

## Blog

This repo includes hype/adoption posts per tool:

- [Hype Blog Roll](./hype/)
- [rawwhp Hype Post](./hype/rawwhp.html)

GitHub Pages can serve this directly from the same repo (root source).

## Critical Reviews

Independent utility critiques and keep/improve/deprecate guidance:

- [Reviews Index](./reviews/)
- [rawwhp Review](./reviews/rawwhp.html)

## Releases

- `CHANGELOG.md` tracks release-level summaries.
- Initial tagged release target: `v1.0.0`.

## Contributing

Ideas and contributions are welcome.

- New tools should remain single-file C CLIs.
- Prefer low-level Windows-focused utilities that are easy to compile and reuse.
- Include a matching `.md` doc with build instructions and examples.

If you have a tool idea, open a PR with an implementation.
