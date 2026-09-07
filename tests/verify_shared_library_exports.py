#!/usr/bin/env python3

"""Verify that a Robotweax SRT shared library exports exactly its public C ABI."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


def read_manifest(path: pathlib.Path) -> list[str]:
    symbols = [
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    ]
    if symbols != sorted(set(symbols)):
        raise ValueError("the public symbol manifest must be sorted and unique")
    invalid = [symbol for symbol in symbols if not re.fullmatch(r"[A-Za-z_]\w*", symbol)]
    if invalid:
        raise ValueError(f"invalid public symbols: {', '.join(invalid)}")
    return symbols


def declared_exports(paths: list[pathlib.Path]) -> set[str]:
    declaration = re.compile(
        r"\b(?:ROBOTWEAX_SRT_API|SRT_API)\b"
        r"\s+(?:extern\s+)?[^;{}]*?"
        r"\b((?:robotweax_srt|srt)_[A-Za-z0-9_]+)"
        r"\s*(?=\(|\[|;)",
        re.DOTALL,
    )
    exports: set[str] = set()
    for path in paths:
        exports.update(declaration.findall(path.read_text(encoding="utf-8")))
    return exports


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command,
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return completed.stdout


def macho_exports(tool: str, library: pathlib.Path) -> set[str]:
    output = run([tool, "-gUj", str(library)])
    return {
        line.removeprefix("_")
        for line in output.splitlines()
        if line.strip()
    }


def elf_exports(tool: str, library: pathlib.Path) -> set[str]:
    output = run(
        [tool, "-D", "--defined-only", "--format=just-symbols", str(library)]
    )
    return {
        line.split("@", 1)[0]
        for line in output.splitlines()
        if line.strip()
    }


def pe_exports(tool: str, library: pathlib.Path) -> set[str]:
    output = run([tool, "/nologo", "/exports", str(library)])
    exports: set[str] = set()
    export_line = re.compile(
        r"^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)"
    )
    for line in output.splitlines():
        match = export_line.match(line)
        if match:
            exports.add(match.group(1))
    return exports


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", required=True, type=pathlib.Path)
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--header", action="append", required=True, type=pathlib.Path)
    parser.add_argument("--tool", required=True)
    args = parser.parse_args()

    expected = set(read_manifest(args.manifest))
    declared = declared_exports(args.header)
    if declared != expected:
        missing = sorted(declared - expected)
        stale = sorted(expected - declared)
        if missing:
            print("public header declarations missing from manifest:", file=sys.stderr)
            print("\n".join(f"  {symbol}" for symbol in missing), file=sys.stderr)
        if stale:
            print("manifest symbols absent from public headers:", file=sys.stderr)
            print("\n".join(f"  {symbol}" for symbol in stale), file=sys.stderr)
        return 1

    if sys.platform == "darwin":
        actual = macho_exports(args.tool, args.library)
    elif sys.platform == "win32":
        actual = pe_exports(args.tool, args.library)
    else:
        actual = elf_exports(args.tool, args.library)

    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        if missing:
            print("missing public exports:", file=sys.stderr)
            print("\n".join(f"  {symbol}" for symbol in missing), file=sys.stderr)
        if unexpected:
            print("unexpected shared-library exports:", file=sys.stderr)
            print("\n".join(f"  {symbol}" for symbol in unexpected), file=sys.stderr)
        return 1

    print(f"verified {len(actual)} public shared-library exports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
