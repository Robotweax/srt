#!/usr/bin/env python3
"""Cross-platform loopback smoke tests for the public File API demo."""

from __future__ import annotations

import argparse
import os
import queue
import socket
import subprocess
import tempfile
import threading
from pathlib import Path


TIMEOUT_SECONDS = 20
PAYLOAD_SIZE = 192 * 1024 + 317


def reserve_udp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserved:
        reserved.bind(("127.0.0.1", 0))
        return int(reserved.getsockname()[1])


def command_output(process: subprocess.Popen[str]) -> tuple[str, str]:
    try:
        return process.communicate(timeout=TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        raise RuntimeError(
            f"process timed out\nstdout:\n{stdout}\nstderr:\n{stderr}"
        ) from None


def require_success(
    description: str,
    returncode: int | None,
    stdout: str,
    stderr: str,
) -> None:
    if returncode != 0:
        raise RuntimeError(
            f"{description} failed with exit code {returncode}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )


def wait_for_ready(process: subprocess.Popen[str]) -> str:
    assert process.stdout is not None
    lines: queue.Queue[str] = queue.Queue()

    def read_line() -> None:
        lines.put(process.stdout.readline())

    reader = threading.Thread(target=read_line, daemon=True)
    reader.start()
    try:
        line = lines.get(timeout=TIMEOUT_SECONDS)
    except queue.Empty:
        process.kill()
        stdout, stderr = command_output(process)
        raise RuntimeError(
            f"listener did not become ready\nstdout:\n{stdout}\n"
            f"stderr:\n{stderr}"
        ) from None
    if not line.startswith("READY role=listener"):
        process.kill()
        stdout, stderr = command_output(process)
        raise RuntimeError(
            f"unexpected listener readiness line: {line!r}\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )
    return line


def write_payload(path: Path) -> bytes:
    payload = bytes((index * 31 + 7) % 256 for index in range(PAYLOAD_SIZE))
    path.write_bytes(payload)
    return payload


def verify_transfer(
    sender_stdout: str,
    receiver_stdout: str,
    expected: bytes,
    output_path: Path,
) -> None:
    if output_path.read_bytes() != expected:
        raise RuntimeError("received file does not match the source bytes")
    if "TRANSFER sent_bytes=" not in sender_stdout or "STATS " not in sender_stdout:
        raise RuntimeError(f"sender output was incomplete:\n{sender_stdout}")
    if (
        "TRANSFER received_bytes=" not in receiver_stdout
        or "STATS " not in receiver_stdout
    ):
        raise RuntimeError(f"receiver output was incomplete:\n{receiver_stdout}")


def run_caller_listener(demo: Path, crypto: str, directory: Path) -> None:
    port = reserve_udp_port()
    source_path = directory / "caller-listener-source.bin"
    output_path = directory / "caller-listener-output.bin"
    expected = write_payload(source_path)
    passphrase_name = "ROBOTWEAX_SRT_FILE_DEMO_TEST_PASSPHRASE"
    environment = os.environ.copy()
    environment[passphrase_name] = "robotweax-file-demo-test"
    common = [
        "--passphrase-env",
        passphrase_name,
        "--pbkeylen",
        "32",
        "--crypto",
        crypto,
        "--timeout-ms",
        "7000",
        "--block-size",
        "4093",
    ]
    listener = subprocess.Popen(
        [
            str(demo),
            "listener",
            "--bind",
            "127.0.0.1",
            "--port",
            str(port),
            "--output",
            str(output_path),
            "--max-bytes",
            str(PAYLOAD_SIZE + 1),
            "--expect-stream-id",
            "demo/file-loopback",
            *common,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )
    try:
        ready = wait_for_ready(listener)
        caller = subprocess.run(
            [
                str(demo),
                "caller",
                "--host",
                "127.0.0.1",
                "--port",
                str(port),
                "--input",
                str(source_path),
                "--stream-id",
                "demo/file-loopback",
                *common,
            ],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            env=environment,
            check=False,
        )
        listener_stdout, listener_stderr = command_output(listener)
    except Exception:
        if listener.poll() is None:
            listener.kill()
            listener.communicate()
        raise
    listener_stdout = ready + listener_stdout
    if caller.returncode != 0:
        raise RuntimeError(
            f"caller failed with exit code {caller.returncode}\n"
            f"caller stdout:\n{caller.stdout}\n"
            f"caller stderr:\n{caller.stderr}\n"
            f"listener stdout:\n{listener_stdout}\n"
            f"listener stderr:\n{listener_stderr}"
        )
    require_success(
        "listener", listener.returncode, listener_stdout, listener_stderr
    )
    verify_transfer(caller.stdout, listener_stdout, expected, output_path)


def run_rendezvous(demo: Path, directory: Path) -> None:
    sender_port = reserve_udp_port()
    receiver_port = reserve_udp_port()
    while receiver_port == sender_port:
        receiver_port = reserve_udp_port()
    source_path = directory / "rendezvous-source.bin"
    output_path = directory / "rendezvous-output.bin"
    expected = write_payload(source_path)

    def common(local_port: int, remote_port: int) -> list[str]:
        return [
            str(demo),
            "rendezvous",
            "--bind",
            "127.0.0.1",
            "--local-port",
            str(local_port),
            "--host",
            "127.0.0.1",
            "--port",
            str(remote_port),
            "--timeout-ms",
            "7000",
            "--block-size",
            "5003",
        ]

    receiver = subprocess.Popen(
        [
            *common(receiver_port, sender_port),
            "--output",
            str(output_path),
            "--max-bytes",
            str(PAYLOAD_SIZE + 1),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    sender = subprocess.Popen(
        [
            *common(sender_port, receiver_port),
            "--input",
            str(source_path),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        sender_stdout, sender_stderr = command_output(sender)
        receiver_stdout, receiver_stderr = command_output(receiver)
    except Exception:
        sender.kill()
        receiver.kill()
        sender.communicate()
        receiver.communicate()
        raise
    require_success(
        "Rendezvous sender", sender.returncode, sender_stdout, sender_stderr
    )
    require_success(
        "Rendezvous receiver",
        receiver.returncode,
        receiver_stdout,
        receiver_stderr,
    )
    verify_transfer(sender_stdout, receiver_stdout, expected, output_path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", type=Path, required=True)
    parser.add_argument(
        "--scenario",
        choices=("caller-listener", "rendezvous"),
        required=True,
    )
    parser.add_argument("--crypto", choices=("ctr", "gcm"), default="ctr")
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="robotweax-srt-file-demo-") as value:
        directory = Path(value)
        if arguments.scenario == "caller-listener":
            run_caller_listener(arguments.demo, arguments.crypto, directory)
        else:
            run_rendezvous(arguments.demo, directory)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
