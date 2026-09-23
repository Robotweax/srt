#!/usr/bin/env python3
"""Exercise the pinned OBS macOS Qt application with private test settings."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import time

spec = importlib.util.spec_from_file_location(
    "obs_macos_module_smoke", Path(__file__).with_name("run_macos_smoke.py")
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def macho_payload_sha256(path: Path) -> str:
    """Hash the signed Mach-O payload while ignoring only signing metadata.

    Xcode re-signs dylibs copied into OBS.app, changing the signature and the
    __LINKEDIT size fields even when all executable and library bytes agree.
    """
    data = bytearray(path.read_bytes())
    if len(data) < 32 or data[:4] != b"\xcf\xfa\xed\xfe":
        raise RuntimeError(f"not a 64-bit Mach-O dylib: {path}")
    count, command_bytes = struct.unpack_from("<II", data, 16)
    end = 32 + command_bytes
    if count > 1024 or end > len(data):
        raise RuntimeError(f"invalid Mach-O load commands: {path}")
    offset = 32
    signature_offset = None
    linkedit = False
    for _ in range(count):
        if offset + 8 > end:
            raise RuntimeError(f"truncated Mach-O load command: {path}")
        command, size = struct.unpack_from("<II", data, offset)
        if size < 8 or offset + size > end:
            raise RuntimeError(f"invalid Mach-O load command size: {path}")
        if command == 0x19 and size >= 72:  # LC_SEGMENT_64
            if data[offset + 8 : offset + 24].rstrip(b"\0") == b"__LINKEDIT":
                if linkedit:
                    raise RuntimeError(f"duplicate Mach-O __LINKEDIT: {path}")
                linkedit = True
                struct.pack_into("<Q", data, offset + 32, 0)  # vmsize
                struct.pack_into("<Q", data, offset + 48, 0)  # filesize
        elif command == 0x1D and size == 16:  # LC_CODE_SIGNATURE
            if signature_offset is not None:
                raise RuntimeError(f"duplicate Mach-O signature: {path}")
            signature_offset, signature_size = struct.unpack_from(
                "<II", data, offset + 8
            )
            if (
                not signature_size
                or signature_offset < end
                or signature_offset + signature_size > len(data)
            ):
                raise RuntimeError(f"invalid Mach-O signature range: {path}")
            struct.pack_into("<I", data, offset + 12, 0)  # signature size
        offset += size
    if offset != end or not linkedit or signature_offset is None:
        raise RuntimeError(f"missing Mach-O signing metadata: {path}")
    return hashlib.sha256(data[:signature_offset]).hexdigest()


def one_bundle_file(app: Path, pattern: str) -> Path:
    matches = {path.resolve() for path in app.rglob(pattern) if path.is_file()}
    if len(matches) != 1:
        raise RuntimeError(f"OBS.app must contain one {pattern}: {sorted(matches)}")
    return matches.pop()


def bundle_contract(args: argparse.Namespace) -> Path:
    app = args.obs_build / "frontend/Release/OBS.app"
    executable = app / "Contents/MacOS/OBS"
    if not executable.is_file():
        raise RuntimeError(f"missing OBS Qt application: {executable}")
    if not (
        app / "Contents/PlugIns/obs-ffmpeg.plugin/Contents/MacOS/obs-ffmpeg"
    ).is_file():
        raise RuntimeError("OBS.app lacks the production obs-ffmpeg module")
    if not (
        app / "Contents/PlugIns/rtmp-services.plugin/Contents/MacOS/rtmp-services"
    ).is_file():
        raise RuntimeError("OBS.app lacks the custom SRT service module")
    if list(app.rglob("libsrt*.dylib")):
        raise RuntimeError("OBS.app contains a competing SRT provider")
    robotweax = one_bundle_file(app / "Contents/Frameworks", "librobotweax-srt*.dylib")
    avformat = one_bundle_file(app / "Contents/Frameworks", "libavformat*.dylib")
    expected_srt = one_bundle_file(args.srt_prefix / "lib", "librobotweax-srt*.dylib")
    expected_avformat = one_bundle_file(
        args.ffmpeg_prefix / "lib", "libavformat*.dylib"
    )
    if macho_payload_sha256(robotweax) != macho_payload_sha256(expected_srt):
        raise RuntimeError("OBS.app embeds a non-Robotweax SRT binary")
    if macho_payload_sha256(avformat) != macho_payload_sha256(expected_avformat):
        raise RuntimeError("OBS.app embeds a different FFmpeg libavformat")
    return executable


def uri(number: int) -> str:
    return (
        f"srt://127.0.0.1:{number}?mode=caller&transtype=live"
        f"&passphrase={module.SECRET}&pbkeylen=16"
    )


def prepare_profile(
    root: Path, fixture: Path, destination: str, *, source: str | None = None
) -> Path:
    """Create a fresh Cocoa Application Support tree outside the real HOME."""
    root.mkdir(parents=True, exist_ok=False)
    home = root / "home"
    home.mkdir()
    config = home / "Library/Application Support/obs-studio"
    profile = config / "basic/profiles/Robotweax"
    scenes = config / "basic/scenes"
    profile.mkdir(parents=True)
    scenes.mkdir(parents=True)
    (config / "global.ini").write_text("[General]\nEnableAutoUpdates=false\n")
    (config / "user.ini").write_text(
        "[General]\nFirstRun=true\nConfirmOnExit=false\n"
        "[Basic]\nProfile=Robotweax\nProfileDir=Robotweax\n"
        "SceneCollection=Robotweax\nSceneCollectionFile=Robotweax\n"
        "[BasicWindow]\nWarnBeforeStartingStream=false\n"
        "WarnBeforeStoppingStream=false\nSysTrayEnabled=false\n"
    )
    (profile / "basic.ini").write_text(
        "[General]\nName=Robotweax\n[Video]\nBaseCX=320\nBaseCY=180\n"
        "OutputCX=320\nOutputCY=180\nFPSType=1\nFPSInt=25\n"
        "[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n"
        "[Output]\nMode=Simple\nReconnect=true\nRetryDelay=1\nMaxRetries=60\n"
        "[SimpleOutput]\nStreamEncoder=x264\nStreamAudioEncoder=aac\n"
        "VBitrate=500\nABitrate=128\nPreset=ultrafast\n"
    )
    (profile / "service.json").write_text(
        json.dumps(
            {"type": "rtmp_custom", "settings": {"server": destination, "key": ""}}
        )
    )
    settings = (
        {
            "is_local_file": False,
            "input": source,
            "input_format": "mpegts",
            "reconnect_delay_sec": 1,
            "buffering_mb": 0,
        }
        if source
        else {"is_local_file": True, "local_file": str(fixture), "looping": True}
    )
    settings.update(hw_decode=False, restart_on_activate=True)
    (scenes / "Robotweax.json").write_text(
        json.dumps(
            {
                "name": "Robotweax",
                "current_scene": "Scene",
                "current_program_scene": "Scene",
                "scene_order": [{"name": "Scene"}],
                "current_transition": "Cut",
                "sources": [
                    {
                        "name": "Media",
                        "id": "ffmpeg_source",
                        "versioned_id": "ffmpeg_source",
                        "settings": settings,
                        "volume": 1.0,
                        "mixers": 1,
                        "enabled": True,
                    },
                    {
                        "name": "Scene",
                        "id": "scene",
                        "versioned_id": "scene",
                        "settings": {
                            "items": [
                                {
                                    "name": "Media",
                                    "visible": True,
                                    "pos": {"x": 0.0, "y": 0.0},
                                    "scale": {"x": 1.0, "y": 1.0},
                                    "id": 1,
                                }
                            ]
                        },
                    },
                ],
            }
        )
    )
    return config


def desktop_command(executable: Path, config: Path, *, stream: bool):
    home = config.parents[2]
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("DYLD_", "OBS_", "QT_"))
    }
    env.update(HOME=str(home), CFFIXED_USER_HOME=str(home), LC_ALL="C")
    command = [
        str(executable),
        "--multi",
        "--disable-updater",
        "--disable-missing-files-check",
        "--profile",
        "Robotweax",
        "--collection",
        "Robotweax",
    ]
    if stream:
        command.append("--startstreaming")
    return command, env


def obs_log(config: Path, destination: Path) -> str:
    logs = list((config / "logs").glob("*.txt"))
    if not logs:
        raise RuntimeError(f"OBS did not log inside its isolated profile: {config}")
    text = max(logs, key=lambda path: path.stat().st_mtime).read_text(errors="replace")
    destination.write_text(text)
    return text


def wait_for(child: subprocess.Popen, condition, label: str, timeout: int = 40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if condition():
            return
        if child.poll() is not None:
            raise RuntimeError(f"{label}: OBS exited with {child.returncode}")
        time.sleep(0.2)
    raise RuntimeError(f"{label}: timeout")


def visible_window(child: subprocess.Popen, probe: Path, evidence: Path) -> bool:
    result = subprocess.run(
        [str(probe), str(child.pid)], capture_output=True, text=True, timeout=5
    )
    evidence.write_text(result.stdout + result.stderr)
    if result.returncode not in (0, 1):
        raise RuntimeError(f"macOS window inspection failed: {result.stderr}")
    return result.returncode == 0


def verify_maps(child: subprocess.Popen, args: argparse.Namespace, evidence: Path):
    result = subprocess.run(
        ["vmmap", "-w", str(child.pid)], capture_output=True, text=True, timeout=20
    )
    if result.returncode:
        raise RuntimeError(f"cannot inspect OBS runtime mappings: {result.stderr}")
    lines = [
        line
        for line in result.stdout.splitlines()
        if re.search(r"librobotweax-srt|libavformat|libsrt|obs-ffmpeg", line)
    ]
    evidence.write_text("\n".join(lines) + "\n")
    paths = {
        Path(path).resolve()
        for line in lines
        for path in re.findall(
            r"/[^\s]*(?:librobotweax-srt|libavformat|libsrt|obs-ffmpeg)[^\s]*", line
        )
        if Path(path).is_file()
    }
    if any(path.name.startswith("libsrt") for path in paths):
        raise RuntimeError("OBS desktop mapped a competing SRT provider")
    for name, prefix in (
        ("librobotweax-srt", args.srt_prefix / "lib"),
        ("libavformat", args.ffmpeg_prefix / "lib"),
    ):
        matches = {path for path in paths if path.name.startswith(name)}
        expected = one_bundle_file(prefix, f"{name}*.dylib")
        if len(matches) != 1 or macho_payload_sha256(
            next(iter(matches))
        ) != macho_payload_sha256(expected):
            raise RuntimeError(f"OBS desktop mapped unexpected {name}: {matches}")
    if not any(path.name == "obs-ffmpeg" for path in paths):
        raise RuntimeError("OBS desktop did not load the production FFmpeg module")


def check_shutdown(
    status: int, log: str, stdout: str, baseline=None, *, streamed=False
) -> int:
    combined = log + "\n" + stdout
    if (
        status
        or "Loaded scenes:" not in log
        or "Freeing OBS context data" not in combined
    ):
        raise RuntimeError(f"OBS desktop did not shut down normally: status={status}")
    if streamed and not all(
        marker in log
        for marker in (
            "Streaming Start",
            "==== Shutting down",
            "Output 'simple_stream': stopping",
        )
    ):
        raise RuntimeError("OBS desktop lacks normal streaming shutdown evidence")
    counts = [
        int(value) for value in re.findall(r"Number of memory leaks: (\d+)", combined)
    ]
    if not counts or any(value != counts[0] for value in counts):
        raise RuntimeError(f"OBS desktop lacks a consistent allocation count: {counts}")
    if counts[0] > 1 or (baseline is not None and counts[0] != baseline):
        raise RuntimeError(f"OBS desktop allocation regression: {counts[0]}")
    return counts[0]


def stop_desktop(
    child: subprocess.Popen,
    config: Path,
    stdout: Path,
    artifacts: Path,
    label: str,
    baseline=None,
    *,
    streamed=False,
):
    child.send_signal(signal.SIGINT)
    try:
        status = child.wait(timeout=15)
    except subprocess.TimeoutExpired:
        module.sample_process(child, artifacts / f"{label}-stacks.txt")
        raise
    text = obs_log(config, artifacts / f"{label}-obs.log")
    return check_shutdown(
        status, text, stdout.read_text(errors="replace"), baseline, streamed=streamed
    )


def wait_decoded(
    child: subprocess.Popen,
    receiver: subprocess.Popen,
    ffmpeg: Path,
    capture: Path,
    artifacts: Path,
    label: str,
):
    snapshot = artifacts / f"{label}-snapshot.ts"
    error = "no media yet"
    deadline = time.monotonic() + 40
    while time.monotonic() < deadline:
        if child.poll() is not None or receiver.poll() is not None:
            raise RuntimeError(f"{label}: endpoint exited before decoded media")
        if capture.is_file() and capture.stat().st_size >= 500000:
            with capture.open("rb") as stream:
                snapshot.write_bytes(stream.read(2 * 1024 * 1024))
            try:
                module.decoded(ffmpeg, snapshot, artifacts, label)
            except (RuntimeError, subprocess.CalledProcessError) as failure:
                error = str(failure)
            else:
                return
        time.sleep(0.5)
    raise RuntimeError(f"{label}: decoded video/audio deadline: {error}")


def receiver_command(args: argparse.Namespace, number: int, output: Path):
    return [
        str(args.reference_peer),
        "receive",
        "listener",
        str(number),
        module.SECRET,
        str(output),
        str(args.reference_prefix / "lib/libsrt.dylib"),
        "50000000",
    ]


def reference_environment(prefix: Path):
    env = {
        key: value for key, value in os.environ.items() if not key.startswith("DYLD_")
    }
    env["DYLD_LIBRARY_PATH"] = str(prefix / "lib")
    return env


def run_desktop(args, executable: Path, config: Path, label: str, *, stream: bool):
    command, env = desktop_command(executable, config, stream=stream)
    stdout = args.artifacts / f"{label}-stdout.log"
    return command, env, stdout


def qualify(args: argparse.Namespace) -> None:
    args.artifacts.mkdir(parents=True, exist_ok=False)
    executable = bundle_contract(args)
    probe = args.artifacts / "macos-window-probe"
    module.run(
        [
            "cc",
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            str(Path(__file__).with_name("macos_window_probe.c")),
            "-framework",
            "CoreGraphics",
            "-framework",
            "CoreFoundation",
            "-o",
            str(probe),
        ]
    )
    reference_env = reference_environment(args.reference_prefix)
    ffmpeg = args.ffmpeg_prefix / "bin/ffmpeg"
    if not args.fixture.is_file() or not args.reference_peer.is_file():
        raise RuntimeError(
            "the module qualification did not produce a fixture and reference peer"
        )

    idle_config = prepare_profile(
        args.artifacts / "idle", args.fixture, uri(module.port())
    )
    command, env, stdout = run_desktop(
        args, executable, idle_config, "idle", stream=False
    )
    with module.process(command, stdout, env, cwd=executable.parent) as child:
        wait_for(
            child,
            lambda: visible_window(child, probe, args.artifacts / "idle-window.txt"),
            "visible idle OBS window",
        )
        baseline = stop_desktop(child, idle_config, stdout, args.artifacts, "idle")
    print(
        f"PASS macOS Qt frontend and idle shutdown: allocations={baseline}", flush=True
    )

    number = module.port()
    output = args.artifacts / "native.ts"
    config = prepare_profile(args.artifacts / "native", args.fixture, uri(number))
    listener_log = args.artifacts / "native-reference.log"
    with module.process(
        receiver_command(args, number, output), listener_log, reference_env
    ) as receiver:
        module.until(receiver, listener_log, "LISTENING")
        command, env, stdout = run_desktop(
            args, executable, config, "native", stream=True
        )
        with module.process(command, stdout, env, cwd=executable.parent) as child:
            wait_for(
                child,
                lambda: visible_window(
                    child, probe, args.artifacts / "native-window.txt"
                ),
                "visible streaming OBS window",
            )
            wait_decoded(child, receiver, ffmpeg, output, args.artifacts, "native")
            verify_maps(child, args, args.artifacts / "native-maps.txt")
            print(
                "PASS macOS desktop encrypted output with decoded video and audio",
                flush=True,
            )
            receiver.terminate()
            receiver.wait(timeout=10)
            time.sleep(8)
            resumed = args.artifacts / "reconnected.ts"
            resumed_log = args.artifacts / "reconnected-reference.log"
            with module.process(
                receiver_command(args, number, resumed), resumed_log, reference_env
            ) as new_receiver:
                module.until(new_receiver, resumed_log, "LISTENING")
                wait_decoded(
                    child, new_receiver, ffmpeg, resumed, args.artifacts, "reconnected"
                )
                if "Streaming Stop" in obs_log(
                    config, args.artifacts / "native-running-obs.log"
                ):
                    raise RuntimeError("OBS stopped streaming instead of reconnecting")
                print("PASS macOS desktop automatic encrypted reconnect", flush=True)
                stop_desktop(
                    child,
                    config,
                    stdout,
                    args.artifacts,
                    "native",
                    baseline,
                    streamed=True,
                )
                if new_receiver.wait(timeout=10):
                    raise RuntimeError("reconnected reference receiver failed")

    incoming, outgoing = module.port(), module.port()
    while incoming == outgoing:
        outgoing = module.port()
    config = prepare_profile(
        args.artifacts / "source", args.fixture, uri(outgoing), source=uri(incoming)
    )
    output = args.artifacts / "source-output.ts"
    receiver_log = args.artifacts / "source-output-reference.log"
    sender_log = args.artifacts / "source-input-reference.log"
    sender = [
        str(args.reference_peer),
        "send",
        "listener",
        str(incoming),
        module.SECRET,
        str(args.fixture),
        str(args.reference_prefix / "lib/libsrt.dylib"),
    ]
    with module.process(
        receiver_command(args, outgoing, output), receiver_log, reference_env
    ) as receiver:
        module.until(receiver, receiver_log, "LISTENING")
        with module.process(sender, sender_log, reference_env) as source:
            module.until(source, sender_log, "LISTENING")
            command, env, stdout = run_desktop(
                args, executable, config, "source", stream=True
            )
            with module.process(command, stdout, env, cwd=executable.parent) as child:
                wait_for(
                    child,
                    lambda: visible_window(
                        child, probe, args.artifacts / "source-window.txt"
                    ),
                    "visible OBS source window",
                )
                module.until(source, sender_log, "QUEUED", 35)
                wait_decoded(
                    child, receiver, ffmpeg, output, args.artifacts, "source-output"
                )
                verify_maps(child, args, args.artifacts / "source-maps.txt")
                module.tell(source, "quit")
                if source.wait(timeout=10):
                    raise RuntimeError("encrypted reference sender failed")
                stop_desktop(
                    child,
                    config,
                    stdout,
                    args.artifacts,
                    "source",
                    baseline,
                    streamed=True,
                )
                if receiver.wait(timeout=10):
                    raise RuntimeError("source-output reference receiver failed")
    print("PASS macOS desktop encrypted FFmpeg input and native A/V output", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for name in (
        "obs-build",
        "ffmpeg-prefix",
        "srt-prefix",
        "reference-prefix",
        "fixture",
        "reference-peer",
        "artifacts",
    ):
        parser.add_argument("--" + name, required=True, type=Path)
    arguments = parser.parse_args()
    for name, value in vars(arguments).items():
        setattr(arguments, name, value.resolve())
    qualify(arguments)
