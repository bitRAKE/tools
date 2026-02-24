# Changelog

## v1.0.0 - 2026-02-24

First formal multi-tool release for this repository.

### Included tools

- `errnfo` - HRESULT/NTSTATUS/Win32 decode + message-table scan/dump
- `modsnap` - process module snapshot (table/path/csv/count)
- `quuid` - GUID/COM parse + registry/scan/type-lib tooling
- `rawwhp` - WHP sparse area mapping, execution, dumps, reports
- `rpscan` - reparse-point scanner for path safety
- `wchain` - wait-chain traversal CLI for blocked-thread triage
- `uwpchar` - GUI icon-font browser/export utility (transient/out-of-scope for CLI focus)

### Release highlights

- `rawwhp` area/dump refactor, host-aligned capability handling, and strict/discovery test suites
- Per-tool documentation parity and usage examples
- New hype-blog rollout under `hype/` with tool-specific workflow posts and a blog index

### Notes

- GitHub release publishing requires either:
  - GitHub web UI (Draft a new release), or
  - GitHub CLI (`gh`) if installed/authenticated.
