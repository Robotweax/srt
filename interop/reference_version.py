#!/usr/bin/env python3
"""Version parameters shared by Robotweax reference-interoperability gates."""

from __future__ import annotations

import re
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
VERSION_HEADER = REPOSITORY_ROOT / "include" / "srt" / "version.h"


def make_srt_version(major: int, minor: int, patch: int) -> int:
    if any(component < 0 or component > 0xFF for component in (
        major,
        minor,
        patch,
    )):
        raise ValueError("SRT version components must fit in one byte")
    return (major << 16) | (minor << 8) | patch


def parse_srt_version(value: str) -> int:
    """Parse either a dotted SRT version or its integer wire value."""
    dotted = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", value)
    if dotted is not None:
        return make_srt_version(*(int(part) for part in dotted.groups()))
    try:
        parsed = int(value, 0)
    except ValueError as error:
        raise ValueError(
            f"invalid SRT version {value!r}; expected MAJOR.MINOR.PATCH or integer"
        ) from error
    if parsed < 0 or parsed > 0xFF_FF_FF:
        raise ValueError("SRT version integer must fit in 24 bits")
    return parsed


def compatible_srt_version(header: Path = VERSION_HEADER) -> int:
    """Read the expected primary reference version from the public header."""
    content = header.read_text(encoding="utf-8")
    components = []
    for name in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(
            rf"^#define SRT_VERSION_{name} (\d+)$",
            content,
            re.MULTILINE,
        )
        if match is None:
            raise RuntimeError(f"SRT_VERSION_{name} is missing from {header}")
        components.append(int(match.group(1)))
    return make_srt_version(*components)


def format_srt_version(value: int) -> str:
    return f"{value >> 16}.{(value >> 8) & 0xFF}.{value & 0xFF}"
