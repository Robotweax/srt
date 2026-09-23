#!/usr/bin/env python3
"""Fail closed if OBS resolves prebuilt FFmpeg or SRT instead of Robotweax."""

import argparse
from pathlib import Path
import re


def check(cache: Path, ffmpeg: Path, srt: Path) -> None:
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("cache", type=Path)
    parser.add_argument("ffmpeg", type=Path)
    parser.add_argument("srt", type=Path)
    args = parser.parse_args()
    check(args.cache, args.ffmpeg, args.srt)
