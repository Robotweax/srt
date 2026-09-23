#!/usr/bin/env python3
"""Qualify native OBS SRT output and FFmpeg input on Apple Silicon."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import re
import socket
import subprocess
import time

SECRET = "robotweax-macos-fixture"  # Public synthetic test value.
MEDIA = re.compile(
    r"^MEDIA video=(\d+) changed=(\d+) audio=(\d+) audible=(\d+) bytes=(\d+)$", re.M
)


def runtime_paths(build: Path) -> tuple[Path, Path, Path, Path]:
    """Select Xcode products, excluding its EagerLinkingTBDs stub frameworks."""
    framework = build / "libobs/Release/libobs.framework"
    ffmpeg_plugin = build / "plugins/obs-ffmpeg/Release/obs-ffmpeg.plugin"
    x264_plugin = build / "plugins/obs-x264/Release/obs-x264.plugin"
    graphics = build / "libobs-opengl/Release/libobs-opengl.dylib"
    for binary in (
        framework / "Versions/A/libobs",
        ffmpeg_plugin / "Contents/MacOS/obs-ffmpeg",
        x264_plugin / "Contents/MacOS/obs-x264",
        graphics,
    ):
        if not binary.is_file():
            raise RuntimeError(f"missing OBS runtime binary: {binary}")
    return framework, ffmpeg_plugin, x264_plugin, graphics


def run(command: list[str], *, env: dict[str, str] | None = None) -> str:
    return subprocess.run(
        command, check=True, capture_output=True, text=True, env=env
    ).stdout


def port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


@contextmanager
def process(command: list[str], log: Path, env: dict[str, str]):
    with log.open("w+") as stream:
        child = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=stream,
            stderr=subprocess.STDOUT,
            env=env,
        )
        try:
            yield child
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)


def until(child: subprocess.Popen, log: Path, pattern: str, seconds: int = 35) -> None:
    deadline = time.monotonic() + seconds
    while pattern not in log.read_text(errors="replace"):
        if child.poll() is not None or time.monotonic() >= deadline:
            tail = log.read_text(errors="replace")[-4000:]
            raise RuntimeError(
                f"{log.name}: waiting for {pattern}; exit={child.poll()}\n{tail}"
            )
        time.sleep(0.1)


def tell(child: subprocess.Popen, message: str) -> None:
    assert child.stdin is not None
    child.stdin.write((message + "\n").encode())
    child.stdin.flush()


def require_media(log: Path) -> None:
    matches = MEDIA.findall(log.read_text(errors="replace"))
    if not matches:
        raise RuntimeError(f"{log.name}: no OBS media observations")
    video, changed, audio, audible, _ = map(int, matches[-1])
    if video < 20 or changed < 10 or audio < 20 or audible < 10:
        raise RuntimeError(
            f"{log.name}: insufficient moving video/non-silent audio: {matches[-1]}"
        )


def require_provider(log: Path, srt: Path, ffmpeg: Path, plugin: Path) -> None:
    text = log.read_text(errors="replace")
    paths = [Path(path).resolve() for path in re.findall(r"^MAP (/.+)$", text, re.M)]
    expected = (
        ("librobotweax-srt", srt / "lib"),
        ("libavformat", ffmpeg / "lib"),
        ("obs-ffmpeg", plugin / "Contents/MacOS"),
    )
    for name, directory in expected:
        matches = [path for path in paths if path.name.startswith(name)]
        if len(matches) != 1 or matches[0].parent != directory.resolve():
            raise RuntimeError(f"{log.name}: unexpected loaded {name}: {matches}")
    if any(path.name.startswith("libsrt") for path in paths):
        raise RuntimeError(f"{log.name}: competing SRT provider is loaded")
    bindings = re.findall(r"^BINDING (native|ffmpeg) (/.+)$", text, re.M)
    expected_srt = next(
        path for path in paths if path.name.startswith("librobotweax-srt")
    )
    if {label for label, _ in bindings} != {"native", "ffmpeg"} or any(
        Path(path).resolve() != expected_srt for _, path in bindings
    ):
        raise RuntimeError(f"{log.name}: native/FFmpeg binding differs: {bindings}")


def decoded(ffmpeg: Path, media: Path, artifacts: Path, label: str) -> None:
    hashes = run(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-i",
            str(media),
            "-map",
            "0:v:0",
            "-f",
            "framehash",
            "-",
        ]
    )
    (artifacts / f"{label}.frames").write_text(hashes)
    values = re.findall(r"^0,.*?, ([0-9a-f]{64})$", hashes, re.M)
    if len(values) < 20 or len(set(values)) < 10:
        raise RuntimeError(f"{label}: video does not decode to moving frames")
    audio = subprocess.run(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-i",
            str(media),
            "-map",
            "0:a:0",
            "-f",
            "s16le",
            "-",
        ],
        check=True,
        capture_output=True,
        timeout=30,
    ).stdout
    if len(audio) < 48000 or sum(byte != 0 for byte in audio) < 1000:
        raise RuntimeError(f"{label}: no decoded audible audio")


def qualify(args: argparse.Namespace) -> None:
    artifacts = args.artifacts
    artifacts.mkdir(parents=True, exist_ok=True)
    build = args.obs_build
    framework, ffmpeg_plugin, x264_plugin, graphics = runtime_paths(build)
    ffmpeg = args.ffmpeg_prefix / "bin/ffmpeg"
    avformat = args.ffmpeg_prefix / "lib/libavformat.dylib"
    if not avformat.exists():
        raise RuntimeError(f"missing custom FFmpeg library: {avformat}")
    # Framework and module bundles are in the build tree; this never
    # assembles or uploads an OBS application/ZIP.
    env = {
        key: value for key, value in os.environ.items() if not key.startswith("DYLD_")
    }
    env.update(
        DYLD_LIBRARY_PATH=":".join(
            map(
                str,
                [
                    framework / "Versions/A",
                    graphics.parent,
                    ffmpeg_plugin / "Contents/MacOS",
                    x264_plugin / "Contents/MacOS",
                    args.ffmpeg_prefix / "lib",
                    args.srt_prefix / "lib",
                    args.reference_prefix / "lib",
                ],
            )
        ),
        ROBOTWEAX_OBS_FFMPEG_PLUGIN=str(ffmpeg_plugin),
        ROBOTWEAX_OBS_X264_PLUGIN=str(x264_plugin),
        ROBOTWEAX_OBS_GRAPHICS=str(graphics),
        ROBOTWEAX_OBS_AVFORMAT=str(avformat),
        ROBOTWEAX_OBS_PASSPHRASE=SECRET,
    )
    peer = artifacts / "macos-obs-peer"
    run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
        "-I",
        str(args.obs_source / "libobs"),
        "-I",
        str(build / "libobs"),
        "-I",
        str(build / "config"),
        "-I",
        str(args.reference_prefix / "include"),
            str(Path(__file__).with_name("peer.c")),
            "-o",
            str(peer),
            "-F",
            str(framework.parent),
            "-framework",
            "libobs",
            "-Wl,-rpath," + str(framework.parent),
        ]
    )
    reference = artifacts / "macos-reference-peer"
    run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-I",
            str(args.reference_prefix / "include"),
            str(Path(__file__).with_name("macos_reference_peer.c")),
            "-o",
            str(reference),
            "-L",
            str(args.reference_prefix / "lib"),
            "-lsrt",
            "-Wl,-rpath," + str(args.reference_prefix / "lib"),
        ]
    )
    reference_dylib = (args.reference_prefix / "lib/libsrt.dylib").resolve()
    reference_env = {
        key: value for key, value in os.environ.items() if not key.startswith("DYLD_")
    }
    reference_env["DYLD_LIBRARY_PATH"] = str(args.reference_prefix / "lib")
    fixture = artifacts / "fixture.ts"
    run(
        [
            str(ffmpeg),
            "-v",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=320x180:rate=25",
            "-f",
            "lavfi",
            "-i",
            "sine=frequency=997:sample_rate=48000",
            "-t",
            "8",
            "-c:v",
            "mpeg2video",
            "-b:v",
            "500k",
            "-c:a",
            "aac",
            "-b:a",
            "128k",
            "-f",
            "mpegts",
            str(fixture),
        ],
        env=env,
    )
    decoded(ffmpeg, fixture, artifacts, "fixture")

    # Native OBS SRT output -> independent Haivision listener.
    number = port()
    output = artifacts / "native.ts"
    listener_log = artifacts / "native-reference.log"
    listener = [
        str(reference),
        "receive",
        "listener",
        str(number),
        SECRET,
        str(output),
        str(reference_dylib),
    ]
    with process(listener, listener_log, reference_env) as receiver:
        until(receiver, listener_log, "LISTENING")
        obs_log = artifacts / "native-obs.log"
        obs = [
            str(peer),
            str(build),
            str(fixture),
            f"srt://127.0.0.1:{number}?mode=caller",
            "local",
        ]
        with process(obs, obs_log, env) as child:
            until(receiver, listener_log, "BYTES", 40)
            tell(child, "quit")
            if child.wait(timeout=10) or "SHUTDOWN" not in obs_log.read_text():
                raise RuntimeError("native OBS output did not shut down cleanly")
        if receiver.wait(timeout=5):
            raise RuntimeError("reference listener failed")
    require_provider(obs_log, args.srt_prefix, args.ffmpeg_prefix, ffmpeg_plugin)
    require_media(obs_log)
    decoded(ffmpeg, output, artifacts, "native")
    print("PASS native encrypted OBS output to independent SRT peer", flush=True)

    # Independent Haivision sender -> OBS FFmpeg media source.
    number = port()
    sender_log = artifacts / "source-reference.log"
    sender = [
        str(reference),
        "send",
        "listener",
        str(number),
        SECRET,
        str(fixture),
        str(reference_dylib),
    ]
    with process(sender, sender_log, reference_env) as source:
        until(source, sender_log, "LISTENING")
        obs_log = artifacts / "source-obs.log"
        url = (
            f"srt://127.0.0.1:{number}?mode=caller&transtype=live"
            f"&passphrase={SECRET}&pbkeylen=16"
        )
        with process(
            [str(peer), str(build), url, "-", "network"], obs_log, env
        ) as child:
            until(source, sender_log, "QUEUED", 35)
            deadline = time.monotonic() + 20
            while True:
                try:
                    require_media(obs_log)
                    break
                except RuntimeError:
                    if child.poll() is not None or time.monotonic() >= deadline:
                        raise
                    time.sleep(0.1)
            tell(source, "quit")
            if source.wait(timeout=10):
                raise RuntimeError("reference sender failed")
            tell(child, "quit")
            if child.wait(timeout=10) or "SHUTDOWN" not in obs_log.read_text():
                raise RuntimeError("OBS FFmpeg source did not shut down cleanly")
    require_provider(obs_log, args.srt_prefix, args.ffmpeg_prefix, ffmpeg_plugin)
    require_media(obs_log)
    print("PASS independent encrypted SRT peer into OBS FFmpeg source", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in (
        "obs-source",
        "obs-build",
        "ffmpeg-prefix",
        "srt-prefix",
        "reference-prefix",
        "artifacts",
    ):
        parser.add_argument("--" + name, required=True, type=Path)
    arguments = parser.parse_args()
    for name, value in vars(arguments).items():
        setattr(arguments, name, value.resolve())
    qualify(arguments)
