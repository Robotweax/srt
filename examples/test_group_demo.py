#!/usr/bin/env python3
"""Cross-platform loopback smoke tests for the public Group API demo."""

from __future__ import annotations

import argparse
import os
import queue
import socket
import subprocess
import threading
from pathlib import Path


TIMEOUT_SECONDS = 25


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


def run_scenario(demo: Path, scenario: str, crypto: str) -> None:
    port = reserve_udp_port()
    policy = "broadcast" if scenario == "broadcast" else "backup"
    passphrase_name = "ROBOTWEAX_SRT_GROUP_DEMO_TEST_PASSPHRASE"
    environment = os.environ.copy()
    environment[passphrase_name] = "robotweax-group-demo-test"
    common = [
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--policy",
        policy,
        "--members",
        "2",
        "--messages",
        "4",
        "--message",
        "deterministic group payload",
        "--timeout-ms",
        "7000",
        "--passphrase-env",
        passphrase_name,
        "--pbkeylen",
        "32",
        "--crypto",
        crypto,
    ]
    if scenario == "backup-failover":
        common.extend(["--failover-after", "1"])

    listener = subprocess.Popen(
        [str(demo), "listener", "--bind", "127.0.0.1", *common],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=environment,
    )
    try:
        ready = wait_for_ready(listener)
        caller = subprocess.run(
            [str(demo), "caller", *common],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            env=environment,
            check=False,
        )
        if caller.returncode != 0:
            listener.kill()
            listener_stdout, listener_stderr = listener.communicate()
            raise RuntimeError(
                f"caller failed with exit code {caller.returncode}\n"
                f"caller stdout:\n{caller.stdout}\n"
                f"caller stderr:\n{caller.stderr}\n"
                f"listener stdout:\n{ready}{listener_stdout}\n"
                f"listener stderr:\n{listener_stderr}"
            )
        listener_stdout, listener_stderr = command_output(listener)
    except Exception:
        if listener.poll() is None:
            listener.kill()
            listener.communicate()
        raise

    listener_stdout = ready + listener_stdout
    if caller.returncode != 0 or listener.returncode != 0:
        raise RuntimeError(
            f"group demo failed\ncaller exit: {caller.returncode}\n"
            f"caller stdout:\n{caller.stdout}\ncaller stderr:\n{caller.stderr}\n"
            f"listener exit: {listener.returncode}\n"
            f"listener stdout:\n{listener_stdout}\n"
            f"listener stderr:\n{listener_stderr}"
        )

    expected_caller = f"COMPLETE role=caller policy={policy} messages=4 members=2"
    expected_listener = (
        f"COMPLETE role=listener policy={policy} messages=4 members=2"
    )
    if (
        expected_caller not in caller.stdout
        or expected_listener not in listener_stdout
    ):
        raise RuntimeError(
            f"completion output was incomplete\ncaller:\n{caller.stdout}\n"
            f"listener:\n{listener_stdout}"
        )
    if caller.stdout.count("SEND index=") != 4:
        raise RuntimeError(f"caller did not confirm four sends:\n{caller.stdout}")
    if listener_stdout.count("DELIVERY index=") != 4:
        raise RuntimeError(
            f"listener did not confirm four deliveries:\n{listener_stdout}"
        )
    if "MEMBERS phase=ready total=2 connected=2" not in caller.stdout:
        raise RuntimeError(f"caller group was not ready:\n{caller.stdout}")
    if (
        "MEMBER_STATS " not in caller.stdout
        or "MEMBER_STATS " not in listener_stdout
    ):
        raise RuntimeError("member statistics were not reported")
    if scenario == "backup-failover":
        if "FAILOVER closed_member=" not in listener_stdout:
            raise RuntimeError(f"listener did not close the primary:\n{listener_stdout}")
        if "UPDATE observed=true" not in caller.stdout:
            raise RuntimeError(f"caller did not observe group update:\n{caller.stdout}")
        if "active_transitions=1" not in caller.stdout:
            raise RuntimeError(f"caller did not switch paths once:\n{caller.stdout}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--demo", type=Path, required=True)
    parser.add_argument(
        "--scenario",
        choices=("broadcast", "backup-failover"),
        required=True,
    )
    parser.add_argument("--crypto", choices=("ctr", "gcm"), default="ctr")
    arguments = parser.parse_args()
    run_scenario(arguments.demo, arguments.scenario, arguments.crypto)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
