#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import argparse
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path


COPYRIGHT_YEAR = 2026
COPYRIGHT_OWNER = "fastsbc_hmr contributors"

HEADER_LINES = [
    (
        "fastsbc_hmr - The HMR DSL was inspired by Oracle's HMR language. "
        "It is a compiler based on the LLVM backend that generates a library for dynamic loading."
    ),
    f"Copyright (C) {COPYRIGHT_YEAR}  {COPYRIGHT_OWNER}",
    "",
    "This program is free software: you can redistribute it and/or modify",
    "it under the terms of the GNU General Public License as published by",
    "the Free Software Foundation, either version 3 of the License, or",
    "(at your option) any later version.",
    "",
    "This program is distributed in the hope that it will be useful,",
    "but WITHOUT ANY WARRANTY; without even the implied warranty of",
    "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the",
    "GNU General Public License for more details.",
    "",
    "You should have received a copy of the GNU General Public License",
    "along with this program.  If not, see <https://www.gnu.org/licenses/>.",
]

TARGET_SUFFIXES = {
    ".h",
    ".hpp",
    ".hxx",
    ".hh",
    ".cpp",
    ".cc",
    ".cxx",
    ".c++",
}
EXCLUDED_DIRS = {
    ".antlr",
    ".git",
    "subprojects",
    "third_party",
    "external",
    "vendor",
    "generated",
}
SKIP_MARKER = "NO_AUTO_LICENSE_UPDATE"
PROJECT_COPYRIGHT_RE = re.compile(
    r"Copyright\s*\(C\)\s+\d{4}(?:-\d{4})?\s+" + re.escape(COPYRIGHT_OWNER)
)
ANY_COPYRIGHT_RE = re.compile(
    r"Copyright\s*(?:\(C\)|\(c\)|\u00a9)\s*(?:\d{4}(?:-\d{4})?)?\s*[^\r\n]*",
    re.IGNORECASE,
)


@dataclass(frozen=True)
class FileResult:
    path: Path
    status: str
    reason: str = ""


@dataclass
class Summary:
    processed: int = 0
    added: int = 0
    updated: int = 0
    unchanged: int = 0
    skipped: int = 0

    def record(self, result: FileResult) -> None:
        self.processed += 1
        if result.status == "added":
            self.added += 1
        elif result.status == "updated":
            self.updated += 1
        elif result.status == "unchanged":
            self.unchanged += 1
        elif result.status == "skipped":
            self.skipped += 1
        else:
            raise ValueError(f"unknown result status: {result.status}")


def license_header() -> str:
    return "".join(f"// {line}\n" if line else "//\n" for line in HEADER_LINES) + "\n"


def detect_newline(data: bytes) -> str:
    crlf = data.count(b"\r\n")
    lf = data.count(b"\n")
    if crlf and crlf == lf:
        return "\r\n"
    return "\n"


def decode_source(data: bytes) -> tuple[str, str]:
    newline = detect_newline(data)
    text = data.decode("utf-8")
    return text.replace("\r\n", "\n").replace("\r", "\n"), newline


def encode_source(text: str, newline: str) -> bytes:
    if newline != "\n":
        text = text.replace("\n", newline)
    return text.encode("utf-8")


def is_excluded_dir(name: str) -> bool:
    return name in EXCLUDED_DIRS or name.startswith("build")


def iter_source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for current, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if not is_excluded_dir(d)]
        base = Path(current)
        for name in names:
            path = base / name
            if path.suffix in TARGET_SUFFIXES:
                files.append(path)
    return sorted(files)


def third_party_copyright(text: str) -> str | None:
    for match in ANY_COPYRIGHT_RE.finditer(text):
        line = match.group(0)
        if COPYRIGHT_OWNER not in line:
            return line
    return None


def strip_leading_legacy_spdx(text: str) -> tuple[str, bool]:
    lines = text.splitlines(keepends=True)
    if not lines:
        return text, False

    first = lines[0].strip()
    if first.startswith("// SPDX-License-Identifier:"):
        del lines[0]
        if lines and lines[0].strip() == "//":
            del lines[0]
        return "".join(lines).lstrip("\n"), True

    if re.match(r"/\*\s*SPDX-License-Identifier:", first):
        lines[0] = "/*\n"
        if len(lines) > 1 and re.fullmatch(r"\s*\*\s*", lines[1]):
            del lines[1]
        return "".join(lines), True

    return text, False


def find_existing_project_header(text: str) -> tuple[int, int] | None:
    marker = PROJECT_COPYRIGHT_RE.search(text)
    if not marker:
        return None

    line_starts = [0]
    for match in re.finditer("\n", text):
        line_starts.append(match.end())

    line_index = 0
    for i, start in enumerate(line_starts):
        if start <= marker.start():
            line_index = i
        else:
            break

    lines = text.splitlines(keepends=True)
    start_line = line_index
    while start_line > 0 and lines[start_line - 1].lstrip().startswith("//"):
        start_line -= 1

    end_line = line_index + 1
    while end_line < len(lines) and lines[end_line].lstrip().startswith("//"):
        end_line += 1
    while end_line < len(lines) and lines[end_line].strip() == "":
        end_line += 1

    start = sum(len(line) for line in lines[:start_line])
    end = sum(len(line) for line in lines[:end_line])
    return start, end


def first_line(text: str) -> str:
    return text.split("\n", 1)[0].strip()


def insertion_prefix(text: str) -> int:
    lines = text.splitlines(keepends=True)
    if not lines:
        return 0

    if first_line(text) == "#pragma once":
        offset = len(lines[0])
    elif (
        len(lines) >= 2
        and lines[0].lstrip().startswith("#ifndef ")
        and lines[1].lstrip().startswith("#define ")
    ):
        offset = len(lines[0]) + len(lines[1])
    else:
        return 0

    while offset < len(text) and text[offset] == "\n":
        offset += 1
    return offset


def ensure_blank_after_prefix(prefix: str) -> str:
    if not prefix:
        return prefix
    return prefix.rstrip("\n") + "\n\n"


def trim_leading_blank_lines(text: str) -> str:
    while text.startswith("\n"):
        text = text[1:]
    return text


def update_text(text: str) -> tuple[str, str, str]:
    if SKIP_MARKER in text:
        return text, "skipped", SKIP_MARKER

    third_party = third_party_copyright(text)
    if third_party:
        return text, "skipped", f"third-party copyright: {third_party}"

    header = license_header()
    existing = find_existing_project_header(text)
    if existing:
        start, end = existing
        updated = text[:start] + header + text[end:]
        if updated == text:
            return text, "unchanged", ""
        return updated, "updated", ""

    text, _ = strip_leading_legacy_spdx(text)
    offset = insertion_prefix(text)
    prefix = ensure_blank_after_prefix(text[:offset])
    rest = trim_leading_blank_lines(text[offset:])
    updated = prefix + header + rest
    return updated, "added", ""


def process_file(path: Path, dry_run: bool) -> FileResult:
    data = path.read_bytes()
    try:
        text, newline = decode_source(data)
    except UnicodeDecodeError as exc:
        return FileResult(path, "skipped", f"not UTF-8: {exc}")

    updated, status, reason = update_text(text)
    if status in {"added", "updated"} and not dry_run:
        path.write_bytes(encode_source(updated, newline))
    return FileResult(path, status, reason)


def process_tree(root: Path, dry_run: bool) -> tuple[Summary, list[FileResult]]:
    summary = Summary()
    results: list[FileResult] = []
    for path in iter_source_files(root):
        result = process_file(path, dry_run)
        summary.record(result)
        results.append(result)
    return summary, results


def print_report(summary: Summary, results: list[FileResult], dry_run: bool) -> None:
    for result in results:
        if result.status in {"added", "updated"}:
            if dry_run:
                action = "would add" if result.status == "added" else "would update"
            else:
                action = result.status
            print(f"{action}: {result.path}")
        elif result.status == "skipped" and result.reason:
            print(f"warning: skipped {result.path}: {result.reason}", file=sys.stderr)

    print(
        "summary: "
        f"processed={summary.processed} "
        f"added={summary.added} "
        f"updated={summary.updated} "
        f"unchanged={summary.unchanged} "
        f"skipped={summary.skipped}"
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Add or refresh the fastsbc_hmr GPLv3 license header on C/C++ source files."
        )
    )
    parser.add_argument("root", nargs="?", default=".", help="source tree root")
    parser.add_argument(
        "--dry-run", action="store_true", help="report changes without writing files"
    )
    args = parser.parse_args(argv)

    root = Path(args.root).resolve()
    summary, results = process_tree(root, args.dry_run)
    print_report(summary, results, args.dry_run)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
