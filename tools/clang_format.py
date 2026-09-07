#!/usr/bin/env python3
"""Fail-closed clang-format frontend for Robotweax SRT.

The default check modes never modify files. Write access must be selected
explicitly. Changed-file modes format only added or modified line ranges so
that introducing the formatter cannot rewrite unrelated legacy code.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path, PurePosixPath
from typing import Sequence


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
EXPECTED_VERSION = "22.1.8"
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
EXCLUDED_DIRECTORIES = {
    "dist",
    "interop-artifacts",
    "reference-build",
    "reference-srt",
    "third_party",
    "vendor",
}
HUNK_HEADER = re.compile(
    r"^@@ -\d+(?:,\d+)? \+(?P<start>\d+)(?:,(?P<count>\d+))? @@"
)
VERSION_PATTERN = re.compile(r"clang-format version (?P<version>\d+\.\d+\.\d+)")
FORMAT_ENABLED_PATTERN = re.compile(
    r"^DisableFormat:\s+false\s*$",
    re.MULTILINE,
)


class FormatError(RuntimeError):
    """A configuration, selection, or formatter error."""


def run_command(
    arguments: Sequence[str],
    *,
    capture_output: bool = False,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(arguments),
        cwd=REPOSITORY_ROOT,
        check=False,
        text=True,
        capture_output=capture_output,
    )


def formatter_binary() -> str:
    requested = os.environ.get("CLANG_FORMAT", "clang-format")
    resolved = shutil.which(requested)
    if resolved is None:
        raise FormatError(
            f"{requested!r} was not found; install clang-format "
            f"{EXPECTED_VERSION} or set CLANG_FORMAT"
        )
    version = run_command([resolved, "--version"], capture_output=True)
    match = VERSION_PATTERN.search(version.stdout)
    if version.returncode != 0 or match is None:
        output = version.stdout.strip()
        raise FormatError(f"cannot identify formatter version: {output}")
    actual = match.group("version")
    if actual != EXPECTED_VERSION:
        raise FormatError(
            f"clang-format {EXPECTED_VERSION} is required; found {actual} at {resolved}"
        )
    return resolved


def validate_configuration(formatter: str) -> None:
    configuration = REPOSITORY_ROOT / ".clang-format"
    if not configuration.is_file():
        raise FormatError("repository .clang-format is missing")
    dumped = run_command(
        [
            formatter,
            "--style=file",
            "--fallback-style=none",
            "--dump-config",
        ],
        capture_output=True,
    )
    if dumped.returncode != 0:
        detail = dumped.stderr.strip() or dumped.stdout.strip()
        raise FormatError(f"invalid .clang-format: {detail}")
    if FORMAT_ENABLED_PATTERN.search(dumped.stdout) is None:
        raise FormatError("repository formatting is disabled")


def is_cpp_path(path: str) -> bool:
    normalized = PurePosixPath(path.replace("\\", "/"))
    if normalized.suffix.lower() not in CPP_SUFFIXES:
        return False
    for part in normalized.parts[:-1]:
        if part in EXCLUDED_DIRECTORIES or part.startswith("build"):
            return False
    return True


def repository_path(raw_path: str) -> tuple[str, Path]:
    candidate = Path(raw_path)
    absolute = candidate if candidate.is_absolute() else REPOSITORY_ROOT / candidate
    try:
        relative = absolute.resolve().relative_to(REPOSITORY_ROOT)
    except ValueError as error:
        raise FormatError(f"path escapes the repository: {raw_path}") from error
    posix = relative.as_posix()
    if not is_cpp_path(posix):
        raise FormatError(f"not a selected C/C++ source path: {raw_path}")
    if not absolute.is_file():
        raise FormatError(f"source file does not exist: {raw_path}")
    return posix, absolute.resolve()


def parse_changed_ranges(diff: str) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    for line in diff.splitlines():
        match = HUNK_HEADER.match(line)
        if match is None:
            continue
        start = int(match.group("start"))
        count_text = match.group("count")
        count = 1 if count_text is None else int(count_text)
        if count != 0:
            ranges.append((start, start + count - 1))
    return merge_ranges(ranges)


def merge_ranges(ranges: Sequence[tuple[int, int]]) -> list[tuple[int, int]]:
    merged: list[tuple[int, int]] = []
    for start, end in sorted(ranges):
        if start <= 0 or end < start:
            raise FormatError(f"invalid line range: {start}:{end}")
        if merged and start <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def git_output(arguments: Sequence[str]) -> str:
    result = run_command(["git", *arguments], capture_output=True)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise FormatError(f"git {' '.join(arguments)} failed: {detail}")
    return result.stdout


def verify_checked_out_revision(head: str) -> None:
    current = git_output(["rev-parse", "HEAD"]).strip()
    requested = git_output(["rev-parse", head]).strip()
    if current != requested:
        detail = (
            f"changed-line formatting requires checked-out {requested}; "
            f"HEAD is {current}"
        )
        raise FormatError(
            detail
        )


def changed_sources(
    base: str,
    head: str,
) -> list[tuple[str, list[tuple[int, int]]]]:
    verify_checked_out_revision(head)
    git_output(["rev-parse", "--verify", f"{base}^{{commit}}"])
    names = git_output(
        ["diff", "--name-only", "--diff-filter=ACMR", base, head]
    ).splitlines()
    selected: list[tuple[str, list[tuple[int, int]]]] = []
    for path in sorted(set(names)):
        if not is_cpp_path(path):
            continue
        _, absolute = repository_path(path)
        diff = git_output(
            [
                "diff",
                "--no-ext-diff",
                "--unified=0",
                "--diff-filter=ACMR",
                base,
                head,
                "--",
                path,
            ]
        )
        ranges = parse_changed_ranges(diff)
        if ranges:
            selected.append((absolute.as_posix(), ranges))
    return selected


def explicit_sources(paths: Sequence[str]) -> list[tuple[str, list[tuple[int, int]]]]:
    selected: list[tuple[str, list[tuple[int, int]]]] = []
    seen: set[str] = set()
    for raw_path in paths:
        _, absolute = repository_path(raw_path)
        normalized = absolute.as_posix()
        if normalized not in seen:
            selected.append((normalized, []))
            seen.add(normalized)
    return selected


def format_sources(
    formatter: str,
    sources: Sequence[tuple[str, Sequence[tuple[int, int]]]],
    *,
    write: bool,
) -> None:
    failed = False
    for path, ranges in sources:
        command = [
            formatter,
            "--style=file",
            "--fallback-style=none",
        ]
        command.extend(f"--lines={start}:{end}" for start, end in ranges)
        command.extend(["-i"] if write else ["--dry-run", "--Werror"])
        command.append(path)
        result = run_command(command)
        failed = failed or result.returncode != 0
    if failed:
        action = "formatting" if write else "format check"
        raise FormatError(f"{action} failed")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("validate", help="validate tool version and configuration")
    for name in ("check", "write"):
        command = subparsers.add_parser(name)
        command.add_argument("files", nargs="+")
    for name in ("check-changed", "write-changed"):
        command = subparsers.add_parser(name)
        command.add_argument("base")
        command.add_argument("head", nargs="?", default="HEAD")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        formatter = formatter_binary()
        validate_configuration(formatter)
        if arguments.command == "validate":
            print(f"clang-format {EXPECTED_VERSION}: configuration valid")
            return 0
        write = arguments.command.startswith("write")
        if arguments.command.endswith("changed"):
            sources = changed_sources(arguments.base, arguments.head)
        else:
            sources = explicit_sources(arguments.files)
        format_sources(formatter, sources, write=write)
        action = "formatted" if write else "checked"
        print(f"clang-format {EXPECTED_VERSION}: {action} {len(sources)} file(s)")
        return 0
    except FormatError as error:
        print(f"clang-format guard: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
