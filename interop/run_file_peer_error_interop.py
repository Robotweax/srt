#!/usr/bin/env python3
"""Bidirectional public-API PEERERROR interop against pinned SRT."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from interop_common import (
    free_udp_port,
    resolve_program_path,
    terminate,
    write_deterministic_payload,
)


DEFAULT_BYTE_COUNT = 16 * 1_024 * 1_024 + 379
DEFAULT_BLOCK_SIZE = 65_536
DEFAULT_RECEIVER_HOLD_MILLISECONDS = 2_000


@dataclass(frozen=True)
class Scenario:
    name: str
    sender: Path
    receiver: Path
    seed: int


def scenario_matrix(robotweax: Path, reference: Path) -> list[Scenario]:
    return [
        Scenario(
            name="peer-error-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            seed=81_001,
        ),
        Scenario(
            name="peer-error-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            seed=81_002,
        ),
    ]


def peer_command(
    program: Path,
    role: str,
    port: int,
    path: Path,
    byte_count: int,
    timeout_seconds: int,
    receiver_hold_milliseconds: int,
) -> list[str]:
    if role not in ("caller", "listener"):
        raise ValueError(f"unsupported role: {role}")
    command = [
        str(program),
        role,
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--bytes",
        str(byte_count),
        "--input" if role == "caller" else "--output",
        str(path),
        "--transport",
        "file",
        "--congestion",
        "file",
        "--timeout-ms",
        str(timeout_seconds * 1_000),
        "--chunk-size",
        str(DEFAULT_BLOCK_SIZE),
        "--file-api",
    ]
    if role == "caller":
        command.append("--expect-peer-error")
    else:
        command.extend(
            (
                "--expect-file-write-error",
                "--shutdown-grace-ms",
                str(receiver_hold_milliseconds),
            )
        )
    return command


def parse_event(output: str, name: str) -> dict[str, object] | None:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == name:
            return event
    return None


def parse_ready_port(output: str) -> int | None:
    event = parse_event(output, "ready")
    port = event.get("port") if event is not None else None
    return port if isinstance(port, int) and 0 < port <= 65_535 else None


def render_failure(
    scenario: Scenario,
    reason: str,
    sender_stdout: str = "",
    sender_stderr: str = "",
    receiver_stdout: str = "",
    receiver_stderr: str = "",
) -> str:
    return (
        f"{scenario.name}: {reason}\n"
        f"--- sender stdout ---\n{sender_stdout or '<empty>'}\n"
        f"--- sender stderr ---\n{sender_stderr or '<empty>'}\n"
        f"--- receiver stdout ---\n{receiver_stdout or '<empty>'}\n"
        f"--- receiver stderr ---\n{receiver_stderr or '<empty>'}\n"
    )


def validate_events(
    scenario: Scenario,
    sender_output: str,
    receiver_output: str,
    byte_count: int,
) -> tuple[int, int]:
    sender = parse_event(sender_output, "peer_error")
    receiver = parse_event(receiver_output, "file_write_error")
    if sender is None or receiver is None:
        raise RuntimeError(
            f"{scenario.name}: PEERERROR evidence is incomplete"
        )
    sender_offset = sender.get("offset")
    receiver_offset = receiver.get("offset")
    if (
        sender.get("matched") is not True
        or receiver.get("matched") is not True
        or not isinstance(sender_offset, int)
        or isinstance(sender_offset, bool)
        or not 0 <= sender_offset < byte_count
        or not isinstance(receiver_offset, int)
        or isinstance(receiver_offset, bool)
        or not 0 <= receiver_offset < byte_count
    ):
        raise RuntimeError(
            f"{scenario.name}: inconsistent PEERERROR evidence: "
            f"sender={sender}, receiver={receiver}"
        )
    return sender_offset, receiver_offset


def run_scenario(
    scenario: Scenario,
    directory: Path,
    byte_count: int,
    timeout_seconds: int,
    receiver_hold_milliseconds: int,
    failure_path: Path,
) -> None:
    port = free_udp_port()
    input_path = directory / f"{scenario.name}.input"
    write_deterministic_payload(input_path, byte_count, scenario.seed)
    receiver_stdout_path = directory / f"{scenario.name}.receiver.stdout"
    receiver_stderr_path = directory / f"{scenario.name}.receiver.stderr"

    with (
        receiver_stdout_path.open("w", encoding="utf-8")
        as receiver_stdout_stream,
        receiver_stderr_path.open("w", encoding="utf-8")
        as receiver_stderr_stream,
    ):
        receiver = subprocess.Popen(
            peer_command(
                scenario.receiver,
                "listener",
                port,
                failure_path,
                byte_count,
                timeout_seconds,
                receiver_hold_milliseconds,
            ),
            stdout=receiver_stdout_stream,
            stderr=receiver_stderr_stream,
            text=True,
        )

        def receiver_output() -> tuple[str, str]:
            receiver_stdout_stream.flush()
            receiver_stderr_stream.flush()
            return (
                receiver_stdout_path.read_text(
                    encoding="utf-8", errors="replace"
                ),
                receiver_stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                ),
            )

        try:
            deadline = time.monotonic() + min(5, timeout_seconds)
            while True:
                receiver_stdout, receiver_stderr = receiver_output()
                if parse_ready_port(receiver_stdout) == port:
                    break
                if receiver.poll() is not None:
                    raise RuntimeError(
                        render_failure(
                            scenario,
                            "receiver exited before caller start",
                            receiver_stdout=receiver_stdout,
                            receiver_stderr=receiver_stderr,
                        )
                    )
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        render_failure(
                            scenario,
                            "receiver did not report readiness",
                            receiver_stdout=receiver_stdout,
                            receiver_stderr=receiver_stderr,
                        )
                    )
                time.sleep(0.01)

            sender = subprocess.run(
                peer_command(
                    scenario.sender,
                    "caller",
                    port,
                    input_path,
                    byte_count,
                    timeout_seconds,
                    receiver_hold_milliseconds,
                ),
                capture_output=True,
                text=True,
                timeout=timeout_seconds + 5,
                check=False,
            )
            try:
                receiver.wait(timeout=timeout_seconds + 5)
            except subprocess.TimeoutExpired as error:
                terminate(receiver)
                receiver_stdout, receiver_stderr = receiver_output()
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "receiver timed out",
                        sender.stdout,
                        sender.stderr,
                        receiver_stdout,
                        receiver_stderr,
                    )
                ) from error
            receiver_stdout, receiver_stderr = receiver_output()
            if sender.returncode != 0 or receiver.returncode != 0:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "peer process failed "
                        f"(sender={sender.returncode}, "
                        f"receiver={receiver.returncode})",
                        sender.stdout,
                        sender.stderr,
                        receiver_stdout,
                        receiver_stderr,
                    )
                )
            sender_offset, receiver_offset = validate_events(
                scenario, sender.stdout, receiver_stdout, byte_count
            )
            print(
                f"PASS {scenario.name} sender_offset={sender_offset} "
                f"receiver_offset={receiver_offset}",
                flush=True,
            )
        finally:
            if receiver.poll() is None:
                terminate(receiver)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--bytes", type=int, default=DEFAULT_BYTE_COUNT)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument(
        "--receiver-hold-ms",
        type=int,
        default=DEFAULT_RECEIVER_HOLD_MILLISECONDS,
    )
    parser.add_argument(
        "--failure-path", type=Path, default=Path("/dev/full")
    )
    arguments = parser.parse_args()
    if (
        arguments.bytes <= DEFAULT_BLOCK_SIZE
        or arguments.timeout_seconds <= 0
        or arguments.receiver_hold_ms < 0
        or not arguments.failure_path.exists()
    ):
        parser.error("invalid PEERERROR interop parameters")

    robotweax = resolve_program_path(arguments.robotweax_peer)
    reference = resolve_program_path(arguments.reference_peer)
    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="robotweax-srt-peer-error-interop-"
    ) as directory:
        work = Path(directory)
        for scenario in scenario_matrix(robotweax, reference):
            try:
                run_scenario(
                    scenario,
                    work,
                    arguments.bytes,
                    arguments.timeout_seconds,
                    arguments.receiver_hold_ms,
                    arguments.failure_path,
                )
            except (
                OSError,
                RuntimeError,
                subprocess.TimeoutExpired,
            ) as error:
                failures.append(str(error))
    if failures:
        print("PEERERROR interoperability failures:\n", flush=True)
        print("\n\n".join(failures), flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
