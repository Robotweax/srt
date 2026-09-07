#!/usr/bin/env python3
"""Cross-platform loopback smoke tests for the public Message API demo."""

from __future__ import annotations

import argparse
import os
import queue
import socket
import subprocess
import threading
from pathlib import Path


TIMEOUT_SECONDS = 10


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


def run_caller_listener(demo: Path, crypto: str) -> None:
    port = reserve_udp_port()
    passphrase_name = "ROBOTWEAX_SRT_DEMO_TEST_PASSPHRASE"
    environment = os.environ.copy()
    environment[passphrase_name] = "robotweax-message-demo-test"
    common = [
        "--passphrase-env",
        passphrase_name,
        "--pbkeylen",
        "32",
        "--crypto",
        crypto,
        "--packet-filter",
        "fec,cols:10,arq:onreq",
        "--timeout-ms",
        "5000",
        "--nonblocking",
    ]
    listener = subprocess.Popen(
        [
            str(demo),
            "listener",
            "--bind",
            "127.0.0.1",
            "--port",
            str(port),
            "--expect-stream-id",
            "demo/loopback",
            *common,
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )
    ready = wait_for_ready(listener)
    caller = subprocess.run(
        [
            str(demo),
            "caller",
            "--host",
            "127.0.0.1",
            "--port",
            str(port),
            "--stream-id",
            "demo/loopback",
            "--message",
            "caller-listener-smoke",
            *common,
        ],
        capture_output=True,
        text=True,
        timeout=TIMEOUT_SECONDS,
        env=environment,
        check=False,
    )
    listener_stdout, listener_stderr = command_output(listener)
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
    if "ECHO verified" not in caller.stdout or "STATS " not in caller.stdout:
        raise RuntimeError(f"caller output was incomplete:\n{caller.stdout}")
    if "ECHOED " not in listener_stdout or "STATS " not in listener_stdout:
        raise RuntimeError(
            f"listener output was incomplete:\n{listener_stdout}"
        )


def run_rendezvous(demo: Path) -> None:
    left_port = reserve_udp_port()
    right_port = reserve_udp_port()
    while right_port == left_port:
        right_port = reserve_udp_port()

    def arguments(
        local_port: int,
        remote_port: int,
        message: str,
        expected: str,
    ) -> list[str]:
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
            "--message",
            message,
            "--expect-message",
            expected,
            "--timeout-ms",
            "5000",
        ]

    left = subprocess.Popen(
        arguments(left_port, right_port, "left-peer", "right-peer"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    right = subprocess.Popen(
        arguments(right_port, left_port, "right-peer", "left-peer"),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    left_stdout, left_stderr = command_output(left)
    right_stdout, right_stderr = command_output(right)
    require_success("left Rendezvous peer", left.returncode, left_stdout, left_stderr)
    require_success(
        "right Rendezvous peer", right.returncode, right_stdout, right_stderr
    )
    for description, output in (("left", left_stdout), ("right", right_stdout)):
        if "RECEIVED " not in output or "STATS " not in output:
            raise RuntimeError(
                f"{description} Rendezvous output was incomplete:\n{output}"
            )


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
    if arguments.scenario == "caller-listener":
        run_caller_listener(arguments.demo, arguments.crypto)
    else:
        run_rendezvous(arguments.demo)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
