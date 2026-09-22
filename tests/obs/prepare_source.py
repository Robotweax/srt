#!/usr/bin/env python3
"""Select production media plugins in a dedicated, pinned OBS checkout."""

import argparse
import hashlib
from pathlib import Path

ORIGINAL_SHA256 = "5575f37b78a70597d1951ff9b25e54c08865e58ac430b23c601e2c6ad5e956ed"
PROFILE = b"""# Independently authored Robotweax headless qualification profile.
# The OBS module implementation and module CMakeLists remain unmodified.
cmake_minimum_required(VERSION 3.28)
add_subdirectory(obs-ffmpeg)
add_subdirectory(obs-x264)
"""


DESKTOP_PROFILE = PROFILE.replace(b"headless", b"desktop").replace(
    b"# The OBS module implementation and module CMakeLists remain unmodified.",
    b"# Includes the version-locked MPEG-TS lifecycle correction; see obs-desktop.md.",
) + (b"add_subdirectory(rtmp-services)\nadd_subdirectory(obs-transitions)\n")


def prepare(source, profile="headless"):
    profiles = {"headless": PROFILE, "desktop": DESKTOP_PROFILE}
    selected = profiles[profile]
    target = source / "plugins/CMakeLists.txt"
    current = target.read_bytes()
    normalized = current.replace(b"\r\n", b"\n")
    if normalized == selected:
        return False
    if hashlib.sha256(normalized).hexdigest() != ORIGINAL_SHA256:
        raise RuntimeError(
            "unrecognized OBS plugin selection; use a dedicated pinned checkout"
        )
    target.write_bytes(selected)
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument(
        "--profile", choices=("headless", "desktop"), default="headless"
    )
    arguments = parser.parse_args()
    prepare(arguments.source, arguments.profile)
