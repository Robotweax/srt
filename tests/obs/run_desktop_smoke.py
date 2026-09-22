#!/usr/bin/env python3
"""Run the installed Qt frontend with an isolated synthetic OBS user profile."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

spec = importlib.util.spec_from_file_location(
    "obs_module_smoke", Path(__file__).with_name("run_smoke.py")
)
media = importlib.util.module_from_spec(spec)
spec.loader.exec_module(media)
gst = media.gst


def prepare_profile(root, source, destination, *, network=False, reconnect=False):
    """Never read or overwrite a real user's settings; reject directory reuse."""
    root.mkdir(parents=True, exist_ok=False)
    config = root / "config/obs-studio"
    profile = config / "basic/profiles/Robotweax"
    scenes = config / "basic/scenes"
    profile.mkdir(parents=True)
    scenes.mkdir(parents=True)
    (root / "home").mkdir()
    (root / "runtime").mkdir(mode=0o700)
    (config / "global.ini").write_text("[General]\nEnableAutoUpdates=false\n")
    (config / "user.ini").write_text(
        "[General]\nFirstRun=true\n[Basic]\nProfile=Robotweax\n"
        "ProfileDir=Robotweax\nSceneCollection=Robotweax\n"
        "SceneCollectionFile=Robotweax\n[BasicWindow]\n"
        "WarnBeforeStartingStream=false\nWarnBeforeStoppingStream=false\n"
        "WarnBeforeStoppingRecord=false\nSysTrayEnabled=false\n"
    )
    (profile / "basic.ini").write_text(
        "[General]\nName=Robotweax\n[Video]\nBaseCX=320\nBaseCY=180\n"
        "OutputCX=320\nOutputCY=180\nFPSType=1\nFPSInt=25\n"
        "[Audio]\nSampleRate=48000\nChannelSetup=Stereo\n"
        f"[Output]\nMode=Simple\nReconnect={str(reconnect).lower()}\n"
        "RetryDelay=1\nMaxRetries=60\n"
        "[SimpleOutput]\nStreamEncoder=x264\nStreamAudioEncoder=aac\n"
        "VBitrate=500\nABitrate=128\nPreset=ultrafast\nRecFormat2=mkv\n"
        '[Hotkeys]\nOBSBasic.StartStreaming={"bindings":[{"key":"OBS_KEY_F9"}]}\n'
        'OBSBasic.StopStreaming={"bindings":[{"key":"OBS_KEY_F10"}]}\n'
    )
    (profile / "service.json").write_text(
        json.dumps(
            {"type": "rtmp_custom", "settings": {"server": destination, "key": ""}}
        )
    )
    settings = (
        {
            "is_local_file": False,
            "input": str(source),
            "input_format": "mpegts",
            "reconnect_delay_sec": 1,
            "buffering_mb": 0,
        }
        if network
        else {"is_local_file": True, "local_file": str(source), "looping": True}
    )
    settings.update({"hw_decode": False, "restart_on_activate": True})
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


def desktop_command(prefix, root):
    env = {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("LD_", "DYLD_", "QT_", "OBS_", "XDG_"))
    }
    env.update(
        HOME=str(root / "home"),
        XDG_CONFIG_HOME=str(root / "config"),
        XDG_CACHE_HOME=str(root / "cache"),
        XDG_DATA_HOME=str(root / "data"),
        XDG_RUNTIME_DIR=str(root / "runtime"),
        QT_QPA_PLATFORM="xcb",
        LIBGL_ALWAYS_SOFTWARE="1",
        LC_ALL="C",
    )
    return [
        str(prefix / "bin/obs"),
        "--disable-updater",
        "--disable-missing-files-check",
        "--profile",
        "Robotweax",
        "--collection",
        "Robotweax",
    ], env


def verify_maps(child, args, log):
    text = Path(f"/proc/{child.pid}/maps").read_text()
    log.write_text(text)
    paths = {
        Path(line.split()[-1]).resolve()
        for line in text.splitlines()
        if len(line.split()) >= 6 and line.split()[-1].startswith("/")
    }
    for name, directory in (
        ("librobotweax-srt.so", args.srt_prefix / "lib"),
        ("libavformat.so", args.ffmpeg_prefix / "lib"),
        ("libobs.so", args.obs_prefix / "lib"),
        ("obs-ffmpeg.so", args.obs_prefix / "lib/obs-plugins"),
        ("rtmp-services.so", args.obs_prefix / "lib/obs-plugins"),
    ):
        matches = {path for path in paths if path.name.startswith(name)}
        if len(matches) != 1 or next(iter(matches)).parent != directory.resolve():
            raise RuntimeError(f"unexpected desktop provider: {name}")
    if any(path.name.startswith("libsrt") for path in paths):
        raise RuntimeError("desktop loaded a competing SRT provider")


def check_shutdown(status, text, baseline=None):
    counts = re.findall(r"Number of memory leaks: (\d+)", text)
    if status != 0 or "Freeing OBS context data" not in text or len(counts) != 1:
        raise RuntimeError("desktop did not complete normal shutdown")
    count = int(counts[0])
    # The pinned Qt frontend reports one allocation even without a stream.
    # Bound that known baseline and reject any additional streaming allocation;
    # do not mislabel frontend startup bookkeeping as a leak-free application.
    if (baseline is None and count > 1) or (baseline is not None and count != baseline):
        raise RuntimeError(
            f"desktop allocation regression: {count}, baseline={baseline}"
        )
    return count


def stop_desktop(child, log, baseline=None):
    child.send_signal(signal.SIGINT)
    return check_shutdown(child.wait(timeout=15), log.read_text(), baseline)


def hotkey(key):
    subprocess.run(["xdotool", "keydown", key], check=True, timeout=5)
    try:
        # libobs polls X11 keys every 25 ms. Hold the key like a human press;
        # an instantaneous injected down/up pair can be missed completely.
        time.sleep(0.2)
    finally:
        subprocess.run(["xdotool", "keyup", key], check=True, timeout=5)


def wait_decoded(desktop, receiver, ffmpeg, output, artifacts, label, offset=0):
    """Wait for media, not padded MPEG-TS bytes emitted before input is ready."""
    deadline = time.monotonic() + 15
    snapshot = artifacts / f"{label}-snapshot.ts"
    last_error = "no media yet"
    while time.monotonic() < deadline:
        if desktop.poll() is not None or receiver.poll() is not None:
            raise RuntimeError(f"{label}: endpoint exited before decoded media")
        if output.exists() and output.stat().st_size - offset >= 500000:
            with output.open("rb") as stream:
                stream.seek(offset)
                snapshot.write_bytes(stream.read(2 * 1024 * 1024))
            try:
                media.decoded_file(ffmpeg, snapshot, artifacts, f"{label}-probe")
            except (RuntimeError, subprocess.CalledProcessError) as error:
                last_error = str(error)
            else:
                return
        time.sleep(0.5)
    raise RuntimeError(f"{label}: decoded-media deadline: {last_error}")


def soak(desktop, receiver, ffmpeg, output, artifacts, seconds):
    """Each checkpoint must decode newly received bytes, never old good media."""
    started = time.monotonic()
    deadline = started + seconds
    with (artifacts / "desktop-soak-memory.jsonl").open("w") as evidence:
        while time.monotonic() < deadline:
            offset = output.stat().st_size
            wait_decoded(
                desktop, receiver, ffmpeg, output, artifacts, "desktop-soak", offset
            )
            status = Path(f"/proc/{desktop.pid}/status").read_text()
            rss = re.search(r"^VmRSS:\s+(\d+) kB$", status, re.M)
            if rss is None:
                raise RuntimeError("missing OBS resident-memory evidence")
            evidence.write(
                json.dumps(
                    {
                        "seconds": time.monotonic() - started,
                        "rss_kib": int(rss[1]),
                        "received_bytes": output.stat().st_size,
                    }
                )
                + "\n"
            )
            evidence.flush()
    print(f"PASS desktop soak: {time.monotonic() - started:.1f} seconds", flush=True)


def qualify(args):
    artifacts = args.artifacts
    artifacts.mkdir(parents=True, exist_ok=False)
    reference = gst.provider(
        args.gst_prefix, args.reference_srt_prefix, artifacts, "desktop-reference"
    )
    if args.soak_seconds:
        reference = (
            reference[0],
            dict(
                reference[1],
                ROBOTWEAX_TEST_PEER_TIMEOUT_SECONDS=str(args.soak_seconds + 60),
            ),
        )
    ffmpeg = args.ffmpeg_prefix / "bin/ffmpeg"
    fixture = artifacts / "fixture.ts"
    gst.run(
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
            "12",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-tune",
            "zerolatency",
            "-g",
            "25",
            "-b:v",
            "500k",
            "-c:a",
            "aac",
            "-b:a",
            "128k",
            "-f",
            "mpegts",
            str(fixture),
        ]
    )
    idle = artifacts / "desktop-idle"
    prepare_profile(idle, fixture, gst.uri(gst.port(), "caller"))
    idle_log = artifacts / "desktop-idle.log"
    with gst.process(*desktop_command(args.obs_prefix, idle), idle_log) as desktop:
        media.wait_for(
            desktop,
            idle_log,
            lambda: "Loaded scenes:" in idle_log.read_text(),
            "idle Qt frontend startup",
            timeout=40,
        )
        baseline = stop_desktop(desktop, idle_log)
    print(f"PASS desktop idle shutdown: allocation baseline={baseline}", flush=True)
    for encrypted, network in ((False, False), (True, False), (True, True)):
        label = "desktop-aes" if encrypted else "desktop-clear"
        if network:
            label += "-duplex"
        number = gst.port()
        incoming = gst.port()
        while incoming == number:
            incoming = gst.port()
        root = artifacts / label
        prepare_profile(
            root,
            (
                gst.uri(incoming, "listener", encrypted, ffmpeg=True)
                if network
                else fixture
            ),
            gst.uri(number, "caller", encrypted, ffmpeg=True),
            network=network,
            reconnect=encrypted and not network and args.reconnect_cycles > 0,
        )
        output = artifacts / f"{label}.ts"
        sink = gst.gst_command(
            reference, False, gst.uri(number, "listener", encrypted), output, 0
        )
        log = artifacts / f"{label}.log"
        with gst.process(*sink, artifacts / f"{label}-receiver.log") as receiver:
            gst.wait_listener(receiver, number)
            with gst.process(
                *desktop_command(args.obs_prefix, root), log
            ) as desktop, ExitStack() as senders:
                media.wait_for(
                    desktop,
                    log,
                    lambda: "Loaded scenes:" in log.read_text(),
                    "Qt frontend startup",
                    timeout=40,
                )
                verify_maps(desktop, args, artifacts / f"{label}-maps.txt")
                windows = subprocess.run(
                    [
                        "xdotool",
                        "search",
                        "--sync",
                        "--onlyvisible",
                        "--pid",
                        str(desktop.pid),
                        "--name",
                        "OBS",
                    ],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=10,
                ).stdout.strip()
                if not windows:
                    raise RuntimeError("no visible OBS desktop window")
                source = None
                if network:
                    gst.wait_listener(desktop, incoming)
                    source = senders.enter_context(
                        gst.process(
                            *gst.gst_command(
                                reference,
                                True,
                                gst.uri(incoming, "caller", encrypted),
                                fixture,
                            ),
                            artifacts / f"{label}-source.log",
                        )
                    )
                # Exercise real frontend hotkeys, not a substitute libobs driver.
                hotkey("F9")
                wait_decoded(desktop, receiver, ffmpeg, output, artifacts, label)
                if encrypted and not network:
                    for cycle in range(args.reconnect_cycles):
                        receiver.terminate()
                        receiver.wait(timeout=10)
                        # Keep the listener absent long enough for peer timeout;
                        # no F9/manual start is issued during recovery.
                        time.sleep(8)
                        output = artifacts / f"{label}-reconnect-{cycle + 1}.ts"
                        receiver = senders.enter_context(
                            gst.process(
                                *gst.gst_command(
                                    reference,
                                    False,
                                    gst.uri(number, "listener", True),
                                    output,
                                    0,
                                ),
                                artifacts / f"{label}-reconnect-{cycle + 1}.log",
                            )
                        )
                        gst.wait_listener(receiver, number)
                        wait_decoded(
                            desktop,
                            receiver,
                            ffmpeg,
                            output,
                            artifacts,
                            f"{label}-reconnect-{cycle + 1}",
                        )
                        print(
                            f"PASS desktop automatic reconnect {cycle + 1}", flush=True
                        )
                    if args.soak_seconds:
                        soak(
                            desktop,
                            receiver,
                            ffmpeg,
                            output,
                            artifacts,
                            args.soak_seconds,
                        )
                if receiver.poll() is not None or "Streaming Stop" in log.read_text():
                    raise RuntimeError(
                        "stream stopped before the frontend stop command"
                    )
                hotkey("F10")
                media.wait_for(
                    desktop,
                    log,
                    lambda: "Streaming Stop" in log.read_text(),
                    "frontend stream stop",
                )
                gst.require_success(receiver, label)
                if source is not None:
                    gst.release_sender(source)
                    gst.require_success(source, f"{label} source")
                stop_desktop(desktop, log, baseline)
        # Long captures are validated in bounded, fresh windows above.
        if not (encrypted and not network and args.soak_seconds):
            media.decoded_file(ffmpeg, output, artifacts, label)
        print(
            f"PASS {label}: Qt frontend start/stop, decoded moving video and audio",
            flush=True,
        )


def main():
    parser = argparse.ArgumentParser()
    for name in (
        "obs-prefix",
        "srt-prefix",
        "ffmpeg-prefix",
        "gst-prefix",
        "reference-srt-prefix",
        "artifacts",
    ):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--soak-seconds", type=int, default=0)
    parser.add_argument("--reconnect-cycles", type=int, default=0)
    args = parser.parse_args()
    if not 0 <= args.soak_seconds <= 86400 or not 0 <= args.reconnect_cycles <= 100:
        parser.error("soak seconds must be 0..86400 and reconnect cycles 0..100")
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    qualify(args)


if __name__ == "__main__":
    main()
