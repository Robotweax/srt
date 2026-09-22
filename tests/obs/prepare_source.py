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


def prepare(source):
    target = source / "plugins/CMakeLists.txt"
    current = target.read_bytes()
    if current == PROFILE:
        return False
    if hashlib.sha256(current).hexdigest() != ORIGINAL_SHA256:
        raise RuntimeError(
            "unrecognized OBS plugin selection; use a dedicated pinned checkout"
        )
    target.write_bytes(PROFILE)
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    prepare(parser.parse_args().source)
