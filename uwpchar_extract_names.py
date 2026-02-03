#!/usr/bin/env python3
"""
Extract Segoe MDL2 / Fluent icon name mappings from Microsoft Docs markdown tables.

Example (TSV):
  python uwpchar_extract_names.py ^
    --md segoe-ui-symbol-font.md ^
    --md segoe-fluent-icons-font.md ^
    --out uwpchar_names.tsv

Example (C header):
  python uwpchar_extract_names.py ^
    --md segoe-ui-symbol-font.md ^
    --md segoe-fluent-icons-font.md ^
    --out uwpchar_names.h
"""

from __future__ import annotations

import argparse
import re
from collections import Counter
from pathlib import Path


ROW_RE = re.compile(r"^\s*\|(.+)\|\s*$")
NAME_RE = re.compile(r':::no-loc\s+text="([^"]+)":::')
HEX_RE = re.compile(r"\b[0-9a-fA-F]{4,6}\b")


def guess_font(path: Path) -> str:
    name = path.name.lower()
    if "fluent" in name:
        return "Segoe Fluent Icons"
    return "Segoe MDL2 Assets"


def parse_md(path: Path, font: str) -> list[tuple[int, str, str]]:
    items: list[tuple[int, str, str]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        cols = [c.strip() for c in m.group(1).split("|")]
        if len(cols) < 3:
            continue
        if cols[0].lower().startswith("glyph") or cols[1].lower().startswith("unicode"):
            continue
        hexm = HEX_RE.search(cols[1])
        namem = NAME_RE.search(cols[2])
        if not hexm or not namem:
            continue
        code = int(hexm.group(0), 16)
        name = namem.group(1).strip()
        if not name:
            continue
        items.append((code, name, font))
    return items


def escape_wide(s: str) -> str:
    return s.replace("\\", "\\\\").replace('"', '\\"')


def write_tsv(out_path: Path, items: list[tuple[int, str, str]]) -> None:
    with out_path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("font\tcodepoint\tname\n")
        for code, name, font in items:
            f.write(f"{font}\t{code:04X}\t{name}\n")


def split_name(name: str) -> list[str]:
    # CamelCase + digits tokenizer with acronym handling.
    return re.findall(r"[A-Z]+(?=[A-Z][a-z]|[0-9]|$)|[A-Z]?[a-z]+|[0-9]+", name)


def build_token_tables(items: list[tuple[int, str]]):
    tokens_per_name: list[list[str]] = []
    counts = Counter()
    for _, name in items:
        toks = split_name(name)
        tokens_per_name.append(toks)
        counts.update(toks)

    # Stable order: frequency desc, then alpha.
    token_list = sorted(counts.keys(), key=lambda t: (-counts[t], t))
    token_index = {t: i for i, t in enumerate(token_list)}
    return tokens_per_name, token_list, token_index


def write_dense_table(f, items: list[tuple[int, str]], prefix: str) -> None:
    if not items:
        f.write(f"static const uint32_t {prefix}Base = 0;\n")
        f.write(f"static const uint32_t {prefix}Count = 0;\n")
        f.write(f"#define {prefix}TokenIndexU8 1\n")
        f.write(f"typedef uint8_t {prefix}TokenIndexT;\n")
        f.write(f"static const uint32_t {prefix}NameTokenOffset[] = {{ 0 }};\n")
        f.write(f"static const uint8_t {prefix}NameTokenCount[] = {{ 0 }};\n")
        f.write(f"static const {prefix}TokenIndexT {prefix}TokenIndex[] = {{ 0 }};\n")
        f.write(f"static const char *{prefix}Tokens[] = {{ 0 }};\n\n")
        return

    codes = sorted(code for code, _ in items)
    base = codes[0]
    last = codes[-1]
    count = last - base + 1
    table = {code: name for code, name in items}
    tokens_per_name, token_list, token_index = build_token_tables(items)

    # Build per-name token sequences in code order.
    name_tokens: dict[int, list[int]] = {}
    for (code, name), toks in zip(items, tokens_per_name):
        name_tokens[code] = [token_index[t] for t in toks]

    f.write(f"static const uint32_t {prefix}Base = 0x{base:04X};\n")
    f.write(f"static const uint32_t {prefix}Count = 0x{count:04X};\n")

    # Emit token string table.
    f.write(f"static const char *{prefix}Tokens[] = {{\n")
    for t in token_list:
        f.write(f"    \"{escape_wide(t)}\",\n")
    f.write("};\n")
    f.write(f"static const uint16_t {prefix}TokenCount = {len(token_list)};\n\n")

    # Emit token index stream and per-code offsets/counts.
    token_stream: list[int] = []
    offsets: list[int] = []
    counts_list: list[int] = []
    for code in range(base, last + 1):
        toks = name_tokens.get(code)
        if not toks:
            offsets.append(0)
            counts_list.append(0)
            continue
        offsets.append(len(token_stream))
        counts_list.append(len(toks))
        token_stream.extend(toks)

    use_u8 = len(token_list) <= 255
    index_type = "uint8_t" if use_u8 else "uint16_t"

    f.write(f"#define {prefix}TokenIndexU8 {1 if use_u8 else 0}\n")
    f.write(f"typedef {index_type} {prefix}TokenIndexT;\n")

    f.write(f"static const uint32_t {prefix}NameTokenOffset[] = {{\n")
    for off in offsets:
        f.write(f"    {off},\n")
    f.write("};\n")
    f.write(f"static const uint8_t {prefix}NameTokenCount[] = {{\n")
    for cnt in counts_list:
        f.write(f"    {cnt},\n")
    f.write("};\n")
    f.write(f"static const {prefix}TokenIndexT {prefix}TokenIndex[] = {{\n")
    for v in token_stream:
        f.write(f"    {v},\n")
    f.write("};\n\n")


def write_header(out_path: Path, items: list[tuple[int, str, str]]) -> None:
    mdl2 = [(c, n) for (c, n, f) in items if f == "Segoe MDL2 Assets"]
    fluent = [(c, n) for (c, n, f) in items if f == "Segoe Fluent Icons"]

    with out_path.open("w", encoding="utf-8", newline="\n") as f:
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        write_dense_table(f, mdl2, "kUwpcharMdl2")
        write_dense_table(f, fluent, "kUwpcharFluent")


def main() -> int:
    ap = argparse.ArgumentParser(description="Extract glyph name mappings from Microsoft Docs markdown tables.")
    ap.add_argument("--md", action="append", required=True, help="Path to a markdown file.")
    ap.add_argument("--out", required=True, help="Output TSV file.")
    ap.add_argument("--font", action="append", help="Optional font override per --md entry.")
    args = ap.parse_args()

    md_paths = [Path(p) for p in args.md]
    font_overrides = args.font or []
    if font_overrides and len(font_overrides) != len(md_paths):
        ap.error("--font must be provided for each --md when used")

    items: list[tuple[int, str, str]] = []
    for i, p in enumerate(md_paths):
        if not p.exists():
            raise SystemExit(f"Missing file: {p}")
        font = font_overrides[i] if font_overrides else guess_font(p)
        items.extend(parse_md(p, font))

    # de-dup by (code, font); keep first
    seen: set[tuple[int, str]] = set()
    deduped: list[tuple[int, str, str]] = []
    for code, name, font in items:
        key = (code, font)
        if key in seen:
            continue
        seen.add(key)
        deduped.append((code, name, font))

    deduped.sort(key=lambda t: (t[2], t[0]))

    out_path = Path(args.out)
    if out_path.suffix.lower() == ".h":
        write_header(out_path, deduped)
    else:
        write_tsv(out_path, deduped)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
