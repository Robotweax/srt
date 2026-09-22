#!/usr/bin/env python3
"""Qualify the installed Windows OBS Qt frontend with an isolated SRT stream."""

from __future__ import annotations

import argparse
from array import array
import ctypes
from ctypes import wintypes
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import time


spec = importlib.util.spec_from_file_location(
    "obs_windows_module_smoke", Path(__file__).with_name("run_windows_smoke.py")
)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def prepare_profile(prefix: Path, fixture: Path, destination: str) -> Path:
    """Use OBS portable mode only inside this fresh, isolated install prefix."""
    config = prefix / "config/obs-studio"
    profile = config / "basic/profiles/Robotweax"
    scenes = config / "basic/scenes"
    profile.mkdir(parents=True, exist_ok=False)
    scenes.mkdir(parents=True, exist_ok=False)
    (config / "global.ini").write_text("[General]\nEnableAutoUpdates=false\n")
    (config / "user.ini").write_text(
        "[General]\nFirstRun=true\nConfirmOnExit=false\n"
        "[Basic]\nProfile=Robotweax\n"
        "ProfileDir=Robotweax\nSceneCollection=Robotweax\n"
        "SceneCollectionFile=Robotweax\n[BasicWindow]\n"
        "WarnBeforeStartingStream=false\nWarnBeforeStoppingStream=false\n"
        "SysTrayEnabled=false\n"
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
        json.dumps({"type": "rtmp_custom", "settings": {"server": destination, "key": ""}})
    )
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
                        "settings": {
                            "is_local_file": True,
                            "local_file": str(fixture),
                            "looping": True,
                            "hw_decode": False,
                            "restart_on_activate": True,
                        },
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


def desktop_log(config: Path) -> str:
    logs = list((config / "logs").glob("*.txt"))
    if not logs:
        return ""
    return max(logs, key=lambda item: item.stat().st_mtime).read_text(
        errors="replace"
    )


def use_network_source(config: Path, source: str, destination: str) -> None:
    """Switch only the test-owned portable profile after the first OBS exit."""
    scene = config / "basic/scenes/Robotweax.json"
    data = json.loads(scene.read_text())
    media = next(item for item in data["sources"] if item["name"] == "Media")
    media["settings"] = {
        "is_local_file": False,
        "input": source,
        "input_format": "mpegts",
        "reconnect_delay_sec": 1,
        "buffering_mb": 0,
        "hw_decode": False,
        "restart_on_activate": True,
    }
    scene.write_text(json.dumps(data))
    service = config / "basic/profiles/Robotweax/service.json"
    data = json.loads(service.read_text())
    data["settings"]["server"] = destination
    service.write_text(json.dumps(data))


def visible_window(pid: int) -> int | None:
    """Find a real, visible top-level window belonging to this OBS process."""
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [callback_type, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]
    user32.GetWindowTextLengthW.argtypes = [wintypes.HWND]
    user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    found: list[int] = []

    @callback_type
    def inspect(hwnd, unused):
        process = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(process))
        if process.value == pid and user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)
            if length:
                title = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, title, length + 1)
                if "OBS" in title.value:
                    found.append(hwnd)
        return True

    user32.EnumWindows(inspect, 0)
    return found[0] if found else None


def wait_for(predicate, description: str, desktop: subprocess.Popen, timeout=40):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        if desktop.poll() is not None:
            raise RuntimeError(f"{description}: OBS exited with {desktop.returncode}")
        time.sleep(0.2)
    raise RuntimeError(f"{description}: timeout")


def decoded_audio(ffmpeg: Path, capture: Path, env: dict[str, str]) -> None:
    result = subprocess.run(
        [
            str(ffmpeg), "-hide_banner", "-nostdin", "-loglevel", "error",
            "-i", str(capture), "-map", "0:a:0", "-ac", "1", "-f", "f32le", "-",
        ],
        check=True,
        capture_output=True,
        timeout=60,
        env=env,
    )
    samples = array("f")
    samples.frombytes(result.stdout)
    if len(samples) < 4800 or not any(abs(value) > 0.001 for value in samples):
        raise RuntimeError("desktop capture has no audible decoded audio")


def verify_capture(ffmpeg: Path, capture: Path, snapshot: Path, env) -> None:
    with capture.open("rb") as stream:
        snapshot.write_bytes(stream.read(2 * 1024 * 1024))
    module.ts_packets(snapshot)
    module.inspect_captured_video(ffmpeg, snapshot, env)
    decoded_audio(ffmpeg, snapshot, env)


def close_desktop(desktop: subprocess.Popen, window: int) -> None:
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    if not user32.PostMessageW(window, 0x0010, 0, 0):
        raise RuntimeError("cannot send WM_CLOSE to OBS desktop")
    if desktop.wait(timeout=20) != 0:
        raise RuntimeError(f"OBS desktop exited with {desktop.returncode}")


def check_shutdown(log: str, label: str, baseline: int | None = None) -> int:
    if "Loaded scenes:" not in log or "Streaming Start" not in log \
            or "Streaming Stop" not in log or "Freeing OBS context data" not in log:
        raise RuntimeError(f"{label}: OBS frontend log lacks startup, streaming, stop or shutdown")
    leaks = re.findall(r"Number of memory leaks: (\d+)", log)
    if len(leaks) != 1 or int(leaks[0]) > 1 \
            or (baseline is not None and int(leaks[0]) != baseline):
        raise RuntimeError(f"{label}: OBS frontend allocation baseline regressed: {leaks}")
    return int(leaks[0])


def start_desktop(args, runtime: Path, env: dict[str, str], log_file):
    return subprocess.Popen(
        [str(args.obs_executable), "--portable", "--multi", "--disable-updater",
         "--disable-missing-files-check", "--profile", "Robotweax",
         "--collection", "Robotweax", "--startstreaming"],
        cwd=runtime, env=env, stdout=log_file, stderr=subprocess.STDOUT,
    )


def qualify(args) -> None:
    args.artifacts.mkdir(parents=True, exist_ok=False)
    runtime = module.obs_working_directory(args.obs_prefix)
    if args.obs_executable.parent != runtime or not args.qt_platform.is_file():
        raise RuntimeError("installed OBS desktop or Qt platform module is missing")
    if not args.services.is_file():
        raise RuntimeError("installed OBS SRT service module is missing")
    module.require_binary_contract(args.obs_prefix, args.reference_srt)
    module.inspect_captured_video(
        args.ffmpeg_cli, args.fixture, module.environment(args.reference_srt.parent)
    )
    port = module.reserve_port()
    destination = module.uri(port, "caller", True)
    config = prepare_profile(args.obs_prefix, args.fixture, destination)
    reference_env = module.environment(args.reference_srt.parent)
    receiver = module.Child(
        [str(args.reference_peer), "receive", "listener", str(port), module.SECRET,
         str(args.artifacts / "initial.ts"), str(args.reference_srt)],
        args.artifacts / "initial-reference.log", reference_env,
    )
    desktop = None
    source_sender = None
    source_receiver = None
    desktop_log_file = (args.artifacts / "desktop-stdout.log").open("wb")
    try:
        receiver.wait_for(lambda: "LISTENING" in module.read(receiver.log_path), "desktop listener")
        env = module.environment(runtime)
        for key in tuple(env):
            if key.upper().startswith(("OBS_", "QT_")):
                del env[key]
        desktop = start_desktop(args, runtime, env, desktop_log_file)
        wait_for(lambda: visible_window(desktop.pid), "visible OBS Qt window", desktop)
        window = visible_window(desktop.pid)
        wait_for(
            lambda: (args.artifacts / "initial.ts").exists()
            and (args.artifacts / "initial.ts").stat().st_size >= 500_000,
            "desktop encrypted SRT output", desktop,
        )
        verify_capture(args.ffmpeg_cli, args.artifacts / "initial.ts",
                       args.artifacts / "initial-snapshot.ts", reference_env)
        print("PASS Windows desktop encrypted output and visible Qt window", flush=True)
        receiver.stop()
        time.sleep(8)
        receiver = module.Child(
            [str(args.reference_peer), "receive", "listener", str(port), module.SECRET,
             str(args.artifacts / "reconnected.ts"), str(args.reference_srt)],
            args.artifacts / "reconnected-reference.log", reference_env,
        )
        receiver.wait_for(lambda: "LISTENING" in module.read(receiver.log_path), "reconnect listener")
        wait_for(
            lambda: (args.artifacts / "reconnected.ts").exists()
            and (args.artifacts / "reconnected.ts").stat().st_size >= 500_000,
            "automatic desktop SRT reconnect", desktop, timeout=25,
        )
        verify_capture(args.ffmpeg_cli, args.artifacts / "reconnected.ts",
                       args.artifacts / "reconnected-snapshot.ts", reference_env)
        print("PASS Windows desktop automatic encrypted reconnect", flush=True)
        close_desktop(desktop, window)
        receiver.finish()
        log = desktop_log(config)
        (args.artifacts / "desktop-obs.log").write_text(log)
        baseline = check_shutdown(log, "desktop output")
        print(f"PASS Windows desktop normal shutdown: allocations={baseline}", flush=True)

        source_port = module.reserve_port()
        output_port = module.reserve_port()
        while output_port == source_port:
            output_port = module.reserve_port()
        use_network_source(
            config, module.uri(source_port, "listener", True),
            module.uri(output_port, "caller", True),
        )
        source_receiver = module.Child(
            [str(args.reference_peer), "receive", "listener", str(output_port),
             module.SECRET, str(args.artifacts / "source-output.ts"),
             str(args.reference_srt)],
            args.artifacts / "source-output-reference.log", reference_env,
        )
        source_receiver.wait_for(
            lambda: "LISTENING" in module.read(source_receiver.log_path),
            "source output listener",
        )
        desktop_log_file.close()
        desktop_log_file = (args.artifacts / "source-desktop-stdout.log").open("wb")
        desktop = start_desktop(args, runtime, env, desktop_log_file)
        wait_for(lambda: visible_window(desktop.pid), "visible OBS source window", desktop)
        window = visible_window(desktop.pid)
        source_sender = module.Child(
            [str(args.reference_peer), "send", "caller", str(source_port),
             module.SECRET, str(args.fixture), str(args.reference_srt)],
            args.artifacts / "source-input-reference.log", reference_env,
        )
        source_sender.wait_for(
            lambda: "QUEUED" in module.read(source_sender.log_path),
            "encrypted source replay",
        )
        wait_for(
            lambda: (args.artifacts / "source-output.ts").exists()
            and (args.artifacts / "source-output.ts").stat().st_size >= 500_000,
            "desktop encrypted SRT source and output", desktop, timeout=40,
        )
        verify_capture(args.ffmpeg_cli, args.artifacts / "source-output.ts",
                       args.artifacts / "source-output-snapshot.ts", reference_env)
        source_sender.command("quit")
        source_sender.finish()
        close_desktop(desktop, window)
        source_receiver.finish()
        log = desktop_log(config)
        (args.artifacts / "source-desktop-obs.log").write_text(log)
        check_shutdown(log, "desktop source", baseline)
        print("PASS Windows desktop encrypted FFmpeg source and native output", flush=True)
    finally:
        receiver.stop()
        if source_sender:
            source_sender.stop()
        if source_receiver:
            source_receiver.stop()
        if desktop and desktop.poll() is None:
            desktop.terminate()
            desktop.wait(timeout=10)
        (args.artifacts / "latest-desktop-obs.log").write_text(desktop_log(config))
        desktop_log_file.close()


def main():
    parser = argparse.ArgumentParser()
    for name in (
        "obs-prefix", "obs-executable", "qt-platform", "services", "reference-peer",
        "reference-srt", "ffmpeg-cli", "fixture", "artifacts",
    ):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    for name, value in vars(args).items():
        setattr(args, name, value.resolve())
    qualify(args)


if __name__ == "__main__":
    main()
