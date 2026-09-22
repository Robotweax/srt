#!/usr/bin/env python3
"""Prepare the pinned OBS checkout for a headless Windows build."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path


TARGET = Path("cmake/windows/buildspec.cmake")
ORIGINAL_SHA256 = "ae738576d0f7764ca91a4a85f94c2ac9cdca97c675b3c1b07c4012854b8b9540"
PREPARED_SHA256 = "dbd2c3c875186d48368dc45a12b1eb0f73363c3d9fa25cab5c631c6148f210ad"
ORIGINAL = b"  set(dependencies_list prebuilt qt6 cef)\n"
PREPARED = (
    b"  if(NOT DEFINED ENABLE_FRONTEND OR ENABLE_FRONTEND)\n"
    b"    set(dependencies_list prebuilt qt6 cef)\n"
    b"  else()\n"
    b"    set(dependencies_list prebuilt)\n"
    b"  endif()\n"
)
QT_ORIGINAL = (
    b"  if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL Win32)\n"
    b"    _handle_qt_cross_compile(${CMAKE_HOST_SYSTEM_PROCESSOR} DIRECTORY "
    b'"${CMAKE_CURRENT_SOURCE_DIR}/.deps/${qt6_destination}")\n'
    b"  endif()\n"
)
QT_PREPARED = QT_ORIGINAL.replace(
    b"if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL Win32)",
    b"if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL Win32 AND "
    b"(NOT DEFINED ENABLE_FRONTEND OR ENABLE_FRONTEND))",
)


def prepare(source: Path) -> bool:
    target = source / TARGET
    content = target.read_bytes()
    normalized = content.replace(b"\r\n", b"\n")
    digest = hashlib.sha256(normalized).hexdigest()
    if digest == PREPARED_SHA256:
        return False
    if digest != ORIGINAL_SHA256:
        raise RuntimeError(
            "unrecognized OBS Windows dependency selection; use the pinned "
            "unmodified checkout"
        )
    if normalized.count(ORIGINAL) != 1 or normalized.count(QT_ORIGINAL) != 1:
        raise RuntimeError("unrecognized OBS Windows dependency selection")
    target.write_bytes(
        normalized.replace(ORIGINAL, PREPARED).replace(QT_ORIGINAL, QT_PREPARED)
    )
    return True


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    prepare(args.source.resolve())


if __name__ == "__main__":
    main()
