#!/usr/bin/env python3

"""Validate the self-contained public documentation in an install prefix."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit


LINK_PATTERN = re.compile(r"(?<!!)\[[^\]]+\]\(([^)]+)\)")
EXTERNAL_SCHEMES = {"http", "https", "mailto"}
REQUIRED_PATHS = (
    "LICENSE",
    "README.md",
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "THIRD_PARTY.md",
    "docs/README.md",
    "docs/building.md",
    "docs/integration.md",
    "docs/limitations.md",
    "examples/README.md",
    "examples/installed-consumer/CMakeLists.txt",
    "examples/installed-consumer/main.cpp",
    "examples/installed-consumer/README.md",
    "compat/robotweax-0.2-aead-fixtures.json",
    "compat/robotweax-0.2-aead.json",
    "compat/srt-1.5.7-api.json",
)


def link_destination(raw_destination: str) -> str:
    destination = raw_destination.strip()
    if destination.startswith("<") and ">" in destination:
        return destination[1 : destination.index(">")]
    return destination.split(maxsplit=1)[0]


def validate(root: Path) -> list[str]:
    errors: list[str] = []
    root = root.resolve()

    for relative_path in REQUIRED_PATHS:
        if not (root / relative_path).is_file():
            errors.append(f"missing installed documentation file: {relative_path}")

    for manifest in sorted((root / "compat").glob("*.json")):
        try:
            parsed_manifest = json.loads(manifest.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            errors.append(
                f"invalid compatibility manifest {manifest.relative_to(root)}: "
                f"{error}"
            )
            continue
        if not isinstance(parsed_manifest, dict):
            errors.append(
                f"compatibility manifest must contain a JSON object: "
                f"{manifest.relative_to(root)}"
            )

    markdown_files = sorted(root.glob("*.md"))
    markdown_files.extend(sorted((root / "docs").rglob("*.md")))
    markdown_files.extend(sorted((root / "examples").rglob("*.md")))
    for markdown_file in markdown_files:
        content = markdown_file.read_text(encoding="utf-8")
        for match in LINK_PATTERN.finditer(content):
            destination = link_destination(match.group(1))
            parsed = urlsplit(destination)
            if parsed.scheme.lower() in EXTERNAL_SCHEMES or not parsed.path:
                continue
            if parsed.scheme or parsed.netloc or parsed.path.startswith("/"):
                line = content.count("\n", 0, match.start()) + 1
                errors.append(
                    f"{markdown_file.relative_to(root)}:{line}: "
                    f"unsupported local link: {destination}"
                )
                continue

            target = (markdown_file.parent / unquote(parsed.path)).resolve()
            try:
                target.relative_to(root)
            except ValueError:
                line = content.count("\n", 0, match.start()) + 1
                errors.append(
                    f"{markdown_file.relative_to(root)}:{line}: "
                    f"link escapes installed documentation: {destination}"
                )
                continue
            if not target.exists():
                line = content.count("\n", 0, match.start()) + 1
                errors.append(
                    f"{markdown_file.relative_to(root)}:{line}: "
                    f"missing local link target: {destination}"
                )

    documentation_index = root / "docs" / "README.md"
    if documentation_index.is_file():
        indexed_documents: set[Path] = set()
        index_content = documentation_index.read_text(encoding="utf-8")
        for match in LINK_PATTERN.finditer(index_content):
            destination = link_destination(match.group(1))
            parsed = urlsplit(destination)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            target = (documentation_index.parent / unquote(parsed.path)).resolve()
            if target.is_file():
                indexed_documents.add(target)
        for documentation_file in sorted((root / "docs").rglob("*.md")):
            if (
                documentation_file != documentation_index
                and documentation_file.resolve() not in indexed_documents
            ):
                errors.append(
                    "public documentation is not listed in docs/README.md: "
                    f"{documentation_file.relative_to(root)}"
                )

    return errors


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} DOCUMENTATION_ROOT", file=sys.stderr)
        return 2

    root = Path(sys.argv[1])
    if not root.is_dir():
        print(f"installed documentation directory not found: {root}", file=sys.stderr)
        return 1

    errors = validate(root)
    if errors:
        print("installed documentation validation failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1

    markdown_count = sum(1 for _ in root.glob("*.md"))
    markdown_count += sum(1 for _ in (root / "docs").rglob("*.md"))
    markdown_count += sum(1 for _ in (root / "examples").rglob("*.md"))
    print(f"validated {markdown_count} installed Markdown files under {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
