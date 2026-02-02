# rpscan

`rpscan` is a small Windows CLI that scans files/directories for **reparse points** (symlinks, junctions, mount points, cloud placeholders, etc.) and reports what it finds.

This is useful as a **pre-flight safety check** before tools that perform writes, deletes, or deployments.

---

## Quick start

Scan a directory (top-level only):

```bat
rpscan C:\work\repo
```

Recursive scan:

```bat
rpscan C:\work\repo --recursive
```

Paths only:

```bat
rpscan C:\work\repo --recursive --paths
```

Stats summary:

```bat
rpscan C:\work\repo --recursive --stats
```

---

## Usage

```text
rpscan <path> [--recursive] [--max-depth N]
             [--files] [--dirs] [--paths]
             [--stats] [--verbose]
```

### Notes

- Reparse points are reported; **directories that are reparse points are not traversed**.
- Without `--recursive`, only **immediate children** are scanned when `<path>` is a directory.

---

## Output

Default output includes:

- reparse tag name
- hex reparse tag
- path
- optional target (for symlink/mount point)

Example:

```text
SYMLINK 0xA000000C C:\work\repo\link -> C:\real\target
MOUNT_POINT 0xA0000003 C:\work\repo\junction -> \??\C:\data
```

---

## Build

```bat
cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rpscan.c
clang-cl /nologo /W4 /O2 /DUNICODE /D_UNICODE rpscan.c
clang -O2 -Wall -Wextra -DUNICODE -D_UNICODE rpscan.c -o rpscan.exe
```

---

## Exit codes

- `0` success
- `2` usage / parse error

---

## License

Unlicensed / public domain intent

* Initial implementation drafted with AI assistance
* ChatGPT 5.2 Thinking (OpenAI), < 5 min.
