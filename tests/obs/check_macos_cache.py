#!/usr/bin/env python3
"""Fail closed if OBS resolves prebuilt FFmpeg or SRT instead of Robotweax."""

import argparse
from pathlib import Path
import re


def check(cache: Path, ffmpeg: Path, srt: Path, qt: Path | None = None) -> None:
    values = dict(
        re.findall(
            r"^([^/#=\r\n][^:=\r\n]*):[^=\r\n]*=([^\r\n]*)$",
            cache.read_text(),
            re.M,
        )
    )
    srt_library = Path(values["Libsrt_LIBRARY"]).resolve()
    if srt_library.parent != (srt / "lib").resolve() or not srt_library.name.startswith(
        "librobotweax-srt"
    ):
        raise RuntimeError(f"OBS selected a non-Robotweax SRT library: {srt_library}")
    if Path(values["Libsrt_INCLUDE_DIR"]).resolve() != (srt / "include").resolve():
        raise RuntimeError("OBS selected unexpected SRT headers")
    for component in (
        "avcodec",
        "avdevice",
        "avfilter",
        "avformat",
        "avutil",
        "swscale",
        "swresample",
    ):
        key = f"FFmpeg_{component}_LIBRARY"
        library = Path(values[key]).resolve()
        if library.parent != (ffmpeg / "lib").resolve():
            raise RuntimeError(f"OBS selected unexpected {component}: {library}")
        include = Path(values[f"FFmpeg_{component}_INCLUDE_DIR"]).resolve()
        if include != (ffmpeg / "include").resolve():
            raise RuntimeError(
                f"OBS selected unexpected {component} headers: {include}"
            )
    if qt is not None:
        # The desktop profile replaces the Qt that OBS would download itself.
        packages = {
            key: Path(value).resolve()
            for key, value in values.items()
            if re.fullmatch(r"Qt6\w*_DIR", key)
        }
        expected = (qt / "lib/cmake").resolve()
        if "Qt6_DIR" not in packages or "Qt6Gui_DIR" not in packages:
            raise RuntimeError("OBS did not record the selected Qt packages")
        for key, package in sorted(packages.items()):
            if package.parent != expected:
                raise RuntimeError(f"OBS selected unexpected {key}: {package}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("cache", type=Path)
    parser.add_argument("ffmpeg", type=Path)
    parser.add_argument("srt", type=Path)
    parser.add_argument("--qt", type=Path)
    args = parser.parse_args()
    check(args.cache, args.ffmpeg, args.srt, args.qt)
