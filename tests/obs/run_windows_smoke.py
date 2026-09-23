#!/usr/bin/env python3
"""Exercise native OBS SRT output and FFmpeg input on Windows."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import socket
import subprocess
import time
from urllib.parse import urlencode


SECRET = "robotweax-windows-test"  # Public synthetic fixture, not a credential.
MEDIA = re.compile(
    r"^MEDIA video=(\d+) changed=(\d+) audio=(\d+) audible=(\d+) bytes=(\d+)$",
    re.MULTILINE,
)


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserved:
        reserved.bind(("127.0.0.1", 0))
        return reserved.getsockname()[1]


def read(log: Path) -> str:
    return log.read_text(errors="replace") if log.exists() else ""


class Child:
    def __init__(
        self,
        command: list[str],
        log: Path,
        environment: dict[str, str],
        cwd: Path | None = None,
        evidence: Path | None = None,
    ):
        self.log_path = log
        if evidence is not None:
            if evidence.resolve() == log.resolve():
                raise ValueError("OBS evidence must be separate from runtime logs")
            evidence.write_text("")
            environment = dict(
                environment, ROBOTWEAX_OBS_EVIDENCE=str(evidence.resolve())
            )
        self.log_file = log.open("wb")
        self.process = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=self.log_file,
            stderr=subprocess.STDOUT,
            env=environment,
            cwd=cwd,
        )

    def wait_for(self, predicate, description: str, timeout: float = 20) -> None:
        deadline = time.monotonic() + timeout
        while not predicate():
            if self.process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError(
                    f"{self.log_path.name}: {description}; "
                    f"process={self.process.poll()}\n{read(self.log_path)}"
                )
            time.sleep(0.05)

    def command(self, value: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write((value + "\n").encode())
        self.process.stdin.flush()

    def finish(self, timeout: float = 12) -> None:
        status = self.process.wait(timeout=timeout)
        self.log_file.close()
        if status != 0:
            raise RuntimeError(
                f"{self.log_path.name}: process failed ({status})\n"
                f"{read(self.log_path)}"
            )

    def stop(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.log_file.close()


def environment(directory: Path) -> dict[str, str]:
    result = {
        key: value
        for key, value in os.environ.items()
        if key.upper() not in {"ROBOTWEAX_OBS_PASSPHRASE"}
    }
    result["PATH"] = str(directory) + os.pathsep + result.get("PATH", "")
    return result


def obs_working_directory(prefix: Path) -> Path:
    # The installed Windows libobs runtime resolves its effects relative to this directory.
    runtime = prefix / "bin/64bit"
    if not (runtime / "../../data/libobs/default.effect").is_file():
        raise RuntimeError("OBS runtime is missing its installed libobs effects")
    return runtime


def require_obs_peer_directory(peer: Path, runtime: Path) -> None:
    if peer.resolve().parent != runtime.resolve():
        raise RuntimeError("OBS peer must run from the installed OBS binary directory")


def uri(port: int, mode: str, encrypted: bool) -> str:
    query = {"mode": mode, "transtype": "live", "latency": "120"}
    if encrypted:
        query.update(passphrase=SECRET, pbkeylen="16")
    return f"srt://127.0.0.1:{port}?{urlencode(query)}"


def ts_packets(path: Path) -> tuple[int, float]:
    data = path.read_bytes()
    if len(data) < 188 * 100:
        raise RuntimeError("native OBS output is too small to qualify MPEG-TS")
    best_count = 0
    best_ratio = 0.0
    for offset in range(min(188, len(data))):
        samples = (len(data) - offset) // 188
        if not samples:
            continue
        matches = sum(data[index] == 0x47 for index in range(offset, len(data), 188))
        ratio = matches / samples
        if ratio > best_ratio:
            best_count, best_ratio = samples, ratio
    if best_count < 100 or best_ratio < 0.95:
        raise RuntimeError(
            f"native OBS output is not coherent MPEG-TS: "
            f"packets={best_count} sync_ratio={best_ratio:.3f}"
        )
    return best_count, best_ratio


def decoded_frame_hashes(output: str) -> tuple[int, int]:
    hashes = []
    for line in output.splitlines():
        columns = [column.strip() for column in line.split(",")]
        if len(columns) == 6 and re.fullmatch(r"[0-9a-fA-F]{32}", columns[-1]):
            hashes.append(columns[-1].lower())
    if not hashes:
        raise RuntimeError("captured native MPEG-TS has no decoded video frames")
    return len(hashes), len(set(hashes))


def inspect_captured_video(ffmpeg: Path, capture: Path, env: dict[str, str]) -> None:
    result = subprocess.run(
        [
            str(ffmpeg),
            "-hide_banner",
            "-nostdin",
            "-loglevel",
            "error",
            "-i",
            str(capture),
            "-map",
            "0:v:0",
            "-an",
            "-f",
            "framemd5",
            "-",
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=60,
        env=env,
    )
    frames, unique = decoded_frame_hashes(result.stdout)
    print(f"FRAME_HASHES decoded={frames} unique={unique}")
    if frames < 20 or unique < 10:
        raise RuntimeError("native OBS capture has insufficient decoded moving video")


def require_media(log: Path) -> None:
    samples = MEDIA.findall(read(log))
    if not samples:
        raise RuntimeError(f"{log.name}: no decoded media observations")
    video, changed, audio, audible, _ = map(int, samples[-1])
    if video < 20 or changed < 10 or audio < 20 or audible < 10:
        raise RuntimeError(f"{log.name}: insufficient decoded moving A/V")


def require_video_motion(log: Path) -> None:
    samples = MEDIA.findall(read(log))
    if not samples:
        raise RuntimeError(f"{log.name}: no native video observations")
    video, changed = map(int, samples[-1][:2])
    if video < 20 or changed < 10:
        raise RuntimeError(f"{log.name}: synthetic OBS video did not move")


def canonical(path: str | Path) -> str:
    return os.path.normcase(os.path.normpath(os.path.abspath(path)))


def require_provider(log: Path, expected: Path) -> None:
    text = read(log)
    bindings = re.findall(r"^BINDING (.+)$", text, re.MULTILINE)
    if bindings != [str(expected), str(expected)]:
        raise RuntimeError(f"{log.name}: unexpected SRT bindings: {bindings}")
    modules = re.findall(r"^MODULE (.+)$", text, re.MULTILINE)
    srt_modules = {canonical(value) for value in modules if value.lower().endswith("srt.dll")}
    if srt_modules != {canonical(expected)}:
        raise RuntimeError(f"{log.name}: competing SRT provider: {srt_modules}")
    if not any("obs-ffmpeg.dll" in value.lower() for value in modules):
        raise RuntimeError(f"{log.name}: production OBS FFmpeg module is absent")
    if not any("avformat-" in value.lower() for value in modules):
        raise RuntimeError(f"{log.name}: production FFmpeg library is absent")
    if text.count("SHUTDOWN\n") != 1:
        raise RuntimeError(f"{log.name}: unclean OBS shutdown")


def dumpbin(path: Path, option: str) -> str:
    return subprocess.run(
        ["dumpbin", option, str(path)],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    ).stdout


def require_binary_contract(obs_prefix: Path, reference_srt: Path) -> None:
    obs_srt = obs_prefix / "bin/64bit/srt.dll"
    if hashlib.sha256(obs_srt.read_bytes()).digest() == hashlib.sha256(
        reference_srt.read_bytes()
    ).digest():
        raise RuntimeError("OBS and reference peers use the same SRT binary")
    binaries = (
        obs_prefix / "obs-plugins/64bit/obs-ffmpeg.dll",
        obs_prefix / "bin/64bit/avformat-62.dll",
    )
    for binary in binaries:
        dependencies = dumpbin(binary, "/dependents").lower()
        if dependencies.count("srt.dll") != 1:
            raise RuntimeError(f"{binary.name}: expected one dynamic srt.dll dependency")
    exports = dumpbin(obs_srt, "/exports").lower()
    if "srt_startup" not in exports:
        raise RuntimeError("Robotweax compatibility DLL does not export srt_startup")


def qualify(args: argparse.Namespace) -> None:
    args.artifacts.mkdir(parents=True)
    obs_runtime = obs_working_directory(args.obs_prefix)
    require_obs_peer_directory(args.obs_peer, obs_runtime)
    reference_runtime = args.reference_srt.parent
    obs_provider = (obs_runtime / "srt.dll").resolve()
    reference_provider = args.reference_srt.resolve()
    require_binary_contract(args.obs_prefix, reference_provider)

    # Native output: Robotweax OBS caller to the separately loaded reference listener.
    port = reserve_port()
    capture = args.artifacts / "native-output.ts"
    reference_log = args.artifacts / "native-reference.log"
    observer_log = args.artifacts / "native-obs.log"
    receiver = Child(
        [
            str(args.reference_peer),
            "receive",
            "listener",
            str(port),
            "-",
            str(capture),
            str(reference_provider),
        ],
        reference_log,
        environment(reference_runtime),
    )
    sender = None
    try:
        receiver.wait_for(lambda: "LISTENING" in read(reference_log), "listener startup")
        sender = Child(
            [
                str(args.obs_peer),
                str(args.obs_prefix),
                "send",
                uri(port, "caller", False),
                str(obs_provider),
            ],
            observer_log.with_name("native-obs-runtime.log"),
            environment(obs_runtime),
            cwd=obs_runtime,
            evidence=observer_log,
        )
        sender.wait_for(lambda: "READY" in read(observer_log), "OBS output startup")
        sender.wait_for(
            lambda: capture.exists() and capture.stat().st_size >= 500_000,
            "native media delivery",
        )
        sender.command("quit")
        sender.finish()
        receiver.finish()
    finally:
        if sender:
            sender.stop()
        receiver.stop()
    packets, ratio = ts_packets(capture)
    require_video_motion(observer_log)
    inspect_captured_video(args.ffmpeg_cli, capture, environment(reference_runtime))
    require_provider(observer_log, obs_provider)
    if canonical(reference_provider) not in {
        canonical(value)
        for value in re.findall(r"^PROVIDER (.+)$", read(reference_log), re.MULTILINE)
    }:
        raise RuntimeError("reference receiver loaded an unexpected SRT provider")
    print(f"PASS Windows native output ({packets} TS packets, sync {ratio:.3f})")

    # FFmpeg source: reference caller sends the capture through encrypted SRT
    # to an OBS listener, which must decode moving video and audible audio.
    port = reserve_port()
    observer_log = args.artifacts / "source-obs.log"
    reference_log = args.artifacts / "source-reference.log"
    receiver = Child(
        [
            str(args.obs_peer),
            str(args.obs_prefix),
            "receive",
            uri(port, "listener", True),
            str(obs_provider),
        ],
        observer_log.with_name("source-obs-runtime.log"),
        environment(obs_runtime),
        cwd=obs_runtime,
        evidence=observer_log,
    )
    sender = None
    try:
        receiver.wait_for(lambda: "READY" in read(observer_log), "OBS source startup")
        sender = Child(
            [
                str(args.reference_peer),
                "send",
                "caller",
                str(port),
                SECRET,
                str(capture),
                str(reference_provider),
            ],
            reference_log,
            environment(reference_runtime),
        )
        sender.wait_for(
            lambda: "QUEUED" in read(reference_log), "reference live replay"
        )
        receiver.wait_for(
            lambda: bool(MEDIA.findall(read(observer_log)))
            and min(map(int, MEDIA.findall(read(observer_log))[-1][:4])) >= 20,
            "decoded encrypted A/V",
        )
        sender.command("quit")
        sender.finish()
        receiver.command("quit")
        receiver.finish()
    finally:
        if sender:
            sender.stop()
        receiver.stop()
    require_media(observer_log)
    require_provider(observer_log, obs_provider)
    print("PASS Windows encrypted FFmpeg source")


def main() -> None:
    parser = argparse.ArgumentParser()
    for name in (
        "obs-prefix",
        "obs-peer",
        "reference-peer",
        "reference-srt",
        "ffmpeg-cli",
        "artifacts",
    ):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    for name, value in vars(args).items():
        setattr(args, name, value.resolve())
    qualify(args)


if __name__ == "__main__":
    main()
