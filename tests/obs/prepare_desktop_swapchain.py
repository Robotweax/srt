#!/usr/bin/env python3
"""Apply a version-locked, independently authored OBS macOS swap-chain fix.

OBS defers each swap-chain framebuffer update to the main queue. The deferred
block creates the replacement texture first and adopts it only on its last
step. Every earlier exit drops it: in particular, an update that runs after
shutdown has deleted the framebuffer object fails to bind it and leaves the
texture allocated. Release whatever the swap chain did not adopt.
"""

import argparse
import hashlib
from pathlib import Path

TARGET = "libobs-opengl/gl-cocoa.m"
ORIGINAL_SHA256 = "b36aedb3e3a600af10c2b02c1242ffe9d8adf5cf2febf6108fd6cae3b5ceffba"
FUNCTION = b"void updateSwapchainFramebuffer("
ADOPT = (
    b"        currentSwapchain->wi->texture = framebufferTexture;\n"
    b"        gs_texture_destroy(priorFramebufferTexture);\n"
)
ADOPTED = ADOPT + b"        framebufferTexture = NULL;\n"
CLEANUP = b"updateSwapchainCleanup:\n"
RELEASE = CLEANUP + b"    gs_texture_destroy(framebufferTexture);\n"


def transform(original):
    before, separator, after = original.partition(FUNCTION)
    if not separator or after.count(ADOPT) != 1 or after.count(CLEANUP) != 1:
        raise RuntimeError("unrecognized OBS swap-chain framebuffer update")
    return before + separator + after.replace(ADOPT, ADOPTED, 1).replace(CLEANUP, RELEASE, 1)


def prepare(source):
    target = source / TARGET
    current = target.read_bytes()
    normalized = current.replace(b"\r\n", b"\n")
    # Undo only our exact additions for verification; unknown edits are rejected.
    restored = normalized.replace(RELEASE, CLEANUP, 1).replace(ADOPTED, ADOPT, 1)
    if hashlib.sha256(restored).hexdigest() != ORIGINAL_SHA256:
        raise RuntimeError("unrecognized OBS swap-chain source; use the pinned checkout")
    fixed = transform(restored)
    if normalized == fixed:
        return False
    if normalized != restored:
        raise RuntimeError("partially applied OBS swap-chain correction")
    target.write_bytes(fixed)
    return True


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    prepare(parser.parse_args().source)
