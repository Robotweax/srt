#!/usr/bin/env python3
"""Gate individual Message and Stream receive contracts against Haivision."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from interop_common import (
    file_sha256,
    free_udp_port,
    parse_complete,
    resolve_program_path,
    terminate,
    write_deterministic_payload,
)
from reference_version import compatible_srt_version, parse_srt_version


SRT_ERROR = -1
SRTS_BROKEN = 6
SRT_ECONNLOST = 2_001
SRT_EINVALMSGAPI = 5_009
SRT_EASYNCRCV = 6_002
SRT_ETIMEOUT = 6_003
RECEIVE_TIMEOUT_MILLISECONDS = 150
SHORT_MESSAGE_BUFFER_BYTES = 188
PINNED_REFERENCE_VERSION = compatible_srt_version()


@dataclass(frozen=True)
class Scenario:
    name: str
    sender: Path
    receiver: Path
    message_api: bool
    byte_count: int
    chunk_size: int
    receive_size: int
    seed: int
    reference_short_buffer_loss: bool = False


@dataclass(frozen=True)
class RunOptions:
    host: str = "127.0.0.1"
    timeout_seconds: int = 15


def scenario_matrix(robotweax: Path, reference: Path) -> list[Scenario]:
    return [
        Scenario(
            name="message-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            message_api=True,
            byte_count=1_316,
            chunk_size=1_316,
            receive_size=1_500,
            seed=81_001,
            reference_short_buffer_loss=True,
        ),
        Scenario(
            name="message-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            message_api=True,
            byte_count=1_316,
            chunk_size=1_316,
            receive_size=1_500,
            seed=81_002,
        ),
        Scenario(
            name="stream-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            message_api=False,
            byte_count=4_267,
            chunk_size=1_316,
            receive_size=997,
            seed=81_003,
        ),
        Scenario(
            name="stream-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            message_api=False,
            byte_count=4_267,
            chunk_size=1_316,
            receive_size=997,
            seed=81_004,
        ),
    ]


def peer_command(
    scenario: Scenario,
    role: str,
    port: int,
    payload_path: Path,
    options: RunOptions,
) -> list[str]:
    if role not in ("caller", "listener"):
        raise ValueError(f"unsupported role: {role}")
    sender = role == "caller"
    transport = "live" if scenario.message_api else "file"
    command = [
        str(scenario.sender if sender else scenario.receiver),
        role,
        "--host",
        options.host,
        "--port",
        str(port),
        "--bytes",
        str(scenario.byte_count),
        "--input" if sender else "--output",
        str(payload_path),
        "--transport",
        transport,
        "--message-api",
        "true" if scenario.message_api else "false",
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
        "--chunk-size",
        str(scenario.chunk_size),
        "--receive-size",
        str(scenario.receive_size),
    ]
    if not scenario.message_api:
        command.extend(["--congestion", "file"])
    if sender:
        command.append("--receive-contract-sender")
    else:
        command.append("--receive-contract-receiver")
        command.append(
            "--expect-peer-shutdown"
            if scenario.message_api
            else "--expect-eof"
        )
    return command


def json_events(output: str) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events


def find_event(
    output: str, event_name: str, role: str | None = None
) -> dict[str, object] | None:
    for event in json_events(output):
        if event.get("event") == event_name and (
            role is None or event.get("role") == role
        ):
            return event
    return None


def has_valid_receive_contract(output: str, *, message_api: bool) -> bool:
    event = find_event(output, "receive_contract", "listener")
    return event is not None and all(
        (
            event.get("matched") is True,
            event.get("message_api") is message_api,
            event.get("nonblocking_result") == SRT_ERROR,
            event.get("nonblocking_error") == SRT_EASYNCRCV,
            type(event.get("nonblocking_elapsed_us")) is int,
            0 <= int(event["nonblocking_elapsed_us"]) < 500_000,
            event.get("timeout_result") == SRT_ERROR,
            event.get("timeout_error") == SRT_ETIMEOUT,
            type(event.get("timeout_elapsed_us")) is int,
            50_000 <= int(event["timeout_elapsed_us"]) < 5_000_000,
            event.get("timeout_configured_ms")
            == RECEIVE_TIMEOUT_MILLISECONDS,
        )
    )


def has_valid_terminal_drain(output: str, *, message_api: bool) -> bool:
    event = find_event(output, "terminal_drain_ready", "listener")
    if event is None:
        return False
    expected_result = SRT_ERROR if message_api else 0
    expected_error = SRT_EINVALMSGAPI if message_api else 0
    return all(
        (
            event.get("matched") is True,
            event.get("message_api") is message_api,
            event.get("socket_state") == SRTS_BROKEN,
            event.get("short_result") == expected_result,
            event.get("short_error") == expected_error,
            event.get("short_buffer_bytes")
            == SHORT_MESSAGE_BUFFER_BYTES,
        )
    )


def has_valid_terminal_result(
    output: str, *, message_api: bool, byte_count: int
) -> bool:
    if message_api:
        event = find_event(output, "peer_shutdown", "listener")
        return event is not None and all(
            (
                event.get("matched") is True,
                event.get("bytes") == byte_count,
                event.get("message_api") is True,
                event.get("terminal") == "connection-lost",
                event.get("result") == SRT_ERROR,
                event.get("error") == SRT_ECONNLOST,
                event.get("socket_state") == SRTS_BROKEN,
            )
        )
    event = find_event(output, "eof", "listener")
    return event is not None and event.get("bytes") == byte_count


def wait_for_event(
    process: subprocess.Popen[str],
    stdout_path: Path,
    stderr_path: Path,
    event_name: str,
    role: str | None = None,
    timeout_seconds: float = 5.0,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        output = stdout_path.read_text(encoding="utf-8", errors="replace")
        if find_event(output, event_name, role) is not None:
            return
        if process.poll() is not None:
            break
        time.sleep(0.02)
    stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
    stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
    raise RuntimeError(
        f"{role or 'peer'} did not report {event_name} "
        f"(exit={process.poll()})\n"
        f"--- stdout ---\n{stdout or '<empty>'}\n"
        f"--- stderr ---\n{stderr or '<empty>'}\n"
    )


def release(process: subprocess.Popen[str], command: str) -> None:
    if process.stdin is None:
        raise RuntimeError("receive-contract peer has no control input")
    process.stdin.write(f"{command}\n")
    process.stdin.flush()


def failure_report(
    scenario: Scenario,
    reason: str,
    sender: subprocess.Popen[str],
    receiver: subprocess.Popen[str],
    sender_stdout: str,
    sender_stderr: str,
    receiver_stdout: str,
    receiver_stderr: str,
) -> str:
    return (
        f"{scenario.name}: {reason} "
        f"(sender={sender.returncode}, receiver={receiver.returncode})\n"
        f"--- sender stdout ---\n{sender_stdout or '<empty>'}\n"
        f"--- sender stderr ---\n{sender_stderr or '<empty>'}\n"
        f"--- receiver stdout ---\n{receiver_stdout or '<empty>'}\n"
        f"--- receiver stderr ---\n{receiver_stderr or '<empty>'}\n"
    )


def has_expected_reference_short_buffer_loss(
    scenario: Scenario,
    *,
    sender_returncode: int | None,
    receiver_returncode: int | None,
    sender_stdout: str,
    receiver_stdout: str,
    receiver_stderr: str,
    output_bytes: int | None,
) -> bool:
    """Match only the pinned v1.5.5 lossy Live short-buffer behavior."""
    if not (
        scenario.reference_short_buffer_loss
        and scenario.name == "message-robotweax-to-haivision"
        and scenario.message_api
        and scenario.byte_count == 1_316
    ):
        return False
    try:
        sender_complete = parse_complete(sender_stdout, "caller")
    except RuntimeError:
        return False
    receiver_failure = find_event(receiver_stdout, "failure", "listener")
    receiver_failure_bytes = (
        receiver_failure.get("bytes")
        if receiver_failure is not None
        else None
    )
    receiver_version = (
        receiver_failure.get("srt_version")
        if receiver_failure is not None
        else None
    )
    return all(
        (
            sender_returncode == 0,
            receiver_returncode == 6,
            sender_complete.get("bytes") == scenario.byte_count,
            has_valid_receive_contract(receiver_stdout, message_api=True),
            has_valid_terminal_drain(receiver_stdout, message_api=True),
            receiver_failure_bytes == 0,
            receiver_version == PINNED_REFERENCE_VERSION,
            find_event(receiver_stdout, "complete", "listener") is None,
            find_event(receiver_stdout, "peer_shutdown", "listener") is None,
            output_bytes == 0,
            "receive failed after 0 bytes: Connection was broken"
            in receiver_stderr,
        )
    )


def run_scenario(
    scenario: Scenario, options: RunOptions, directory: Path
) -> None:
    port = free_udp_port(options.host)
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    expected_digest = write_deterministic_payload(
        input_path, scenario.byte_count, scenario.seed
    )
    sender_stdout_path = directory / f"{scenario.name}.sender.stdout"
    sender_stderr_path = directory / f"{scenario.name}.sender.stderr"
    receiver_stdout_path = directory / f"{scenario.name}.receiver.stdout"
    receiver_stderr_path = directory / f"{scenario.name}.receiver.stderr"

    with (
        sender_stdout_path.open("w", encoding="utf-8") as sender_stdout_file,
        sender_stderr_path.open("w", encoding="utf-8") as sender_stderr_file,
        receiver_stdout_path.open(
            "w", encoding="utf-8"
        ) as receiver_stdout_file,
        receiver_stderr_path.open(
            "w", encoding="utf-8"
        ) as receiver_stderr_file,
    ):
        receiver = subprocess.Popen(
            peer_command(
                scenario, "listener", port, output_path, options
            ),
            stdin=subprocess.PIPE,
            stdout=receiver_stdout_file,
            stderr=receiver_stderr_file,
            text=True,
        )
        sender: subprocess.Popen[str] | None = None
        try:
            wait_for_event(
                receiver,
                receiver_stdout_path,
                receiver_stderr_path,
                "ready",
            )
            sender = subprocess.Popen(
                peer_command(
                    scenario, "caller", port, input_path, options
                ),
                stdin=subprocess.PIPE,
                stdout=sender_stdout_file,
                stderr=sender_stderr_file,
                text=True,
            )
            wait_for_event(
                sender,
                sender_stdout_path,
                sender_stderr_path,
                "receive_contract_sender_ready",
                "caller",
            )
            wait_for_event(
                receiver,
                receiver_stdout_path,
                receiver_stderr_path,
                "receive_contract_receiver_ready",
                "listener",
            )
            release(sender, "send")
            sender.wait(timeout=options.timeout_seconds + 5)
            release(receiver, "receive")
            receiver.wait(timeout=options.timeout_seconds + 5)
        finally:
            if sender is not None:
                terminate(sender)
            terminate(receiver)

    sender_stdout = sender_stdout_path.read_text(
        encoding="utf-8", errors="replace"
    )
    sender_stderr = sender_stderr_path.read_text(
        encoding="utf-8", errors="replace"
    )
    receiver_stdout = receiver_stdout_path.read_text(
        encoding="utf-8", errors="replace"
    )
    receiver_stderr = receiver_stderr_path.read_text(
        encoding="utf-8", errors="replace"
    )
    if sender is None:
        raise RuntimeError(f"{scenario.name}: sender did not start")
    output_bytes = output_path.stat().st_size if output_path.is_file() else None
    if has_expected_reference_short_buffer_loss(
        scenario,
        sender_returncode=sender.returncode,
        receiver_returncode=receiver.returncode,
        sender_stdout=sender_stdout,
        receiver_stdout=receiver_stdout,
        receiver_stderr=receiver_stderr,
        output_bytes=output_bytes,
    ):
        print(
            f"PASS {scenario.name} "
            "known_reference_behavior="
            "v1.5.5-live-short-buffer-message-loss "
            f"short_error={SRT_EINVALMSGAPI} bytes_lost={scenario.byte_count}"
        )
        return
    try:
        sender_complete = parse_complete(sender_stdout, "caller")
        receiver_complete = parse_complete(receiver_stdout, "listener")
    except RuntimeError as error:
        raise RuntimeError(
            failure_report(
                scenario,
                str(error),
                sender,
                receiver,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
            )
        ) from error
    valid = all(
        (
            sender.returncode == 0,
            receiver.returncode == 0,
            sender_complete.get("bytes") == scenario.byte_count,
            receiver_complete.get("bytes") == scenario.byte_count,
            output_path.is_file(),
            output_path.is_file()
            and file_sha256(output_path) == expected_digest,
            has_valid_receive_contract(
                receiver_stdout, message_api=scenario.message_api
            ),
            has_valid_terminal_drain(
                receiver_stdout, message_api=scenario.message_api
            ),
            has_valid_terminal_result(
                receiver_stdout,
                message_api=scenario.message_api,
                byte_count=scenario.byte_count,
            ),
        )
    )
    if not valid:
        raise RuntimeError(
            failure_report(
                scenario,
                "receive contract validation failed",
                sender,
                receiver,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
            )
        )
    print(
        f"PASS {scenario.name} sha256={expected_digest} "
        f"bytes={scenario.byte_count}"
    )


def main() -> int:
    global PINNED_REFERENCE_VERSION
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument(
        "--expected-reference-version",
        type=parse_srt_version,
        default=compatible_srt_version(),
    )
    arguments = parser.parse_args()
    PINNED_REFERENCE_VERSION = arguments.expected_reference_version
    robotweax = resolve_program_path(arguments.robotweax_peer)
    reference = resolve_program_path(arguments.reference_peer)
    options = RunOptions()

    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="robotweax-receive-contract-"
    ) as raw_directory:
        directory = Path(raw_directory)
        for scenario in scenario_matrix(robotweax, reference):
            try:
                run_scenario(scenario, options, directory)
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                failures.append(str(error))
    if failures:
        print(
            "individual Message/Stream receive contract failures:\n\n"
            + "\n\n".join(failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
