#!/usr/bin/env python3
"""Validate one-shot PEERERROR handling through the public socket API."""

from __future__ import annotations

import argparse
import json
import subprocess
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
from srt_handshake_trace import (
    PEER_ERROR_ACK_HOLD_MILLISECONDS,
    SEQUENCE_MASK,
    CallerListenerPeerErrorProxy,
)


DEFAULT_BYTE_COUNT = 2 * 1_024 * 1_024 + 379
# Pinned v1.5.5 publishes PEERERROR only from a send that has to wait for
# buffer space. One IPv4 MSS payload unit makes that state deterministic:
# the relay injects before forwarding the first runtime ACK, so one DATA
# packet remains unacknowledged while the next send checks peer health.
DEFAULT_SEND_BUFFER_BYTES = 1_472
DEFAULT_MAXIMUM_BANDWIDTH = 2_000_000
DEFAULT_SHUTDOWN_GRACE_MILLISECONDS = 1_000


@dataclass(frozen=True)
class Scenario:
    name: str
    sender: Path
    receiver: Path
    transport: str
    congestion: str
    chunk_size: int
    seed: int
    # True requires one public error, False requires the known missing-error
    # outcome, and None admits either exact version-scoped reference outcome.
    expect_peer_error_observation: bool | None = True


def scenario_matrix(robotweax: Path, reference: Path) -> list[Scenario]:
    return [
        Scenario(
            "peer-error-message-robotweax-to-haivision",
            robotweax,
            reference,
            "live",
            "live",
            1_200,
            82_101,
        ),
        Scenario(
            "peer-error-message-haivision-to-robotweax",
            reference,
            robotweax,
            "live",
            "live",
            1_200,
            82_102,
        ),
        Scenario(
            "peer-error-stream-robotweax-to-haivision",
            robotweax,
            reference,
            "file",
            "file",
            4_096,
            82_103,
        ),
        Scenario(
            "peer-error-stream-haivision-to-robotweax",
            reference,
            robotweax,
            "file",
            "file",
            4_096,
            82_104,
            None,
        ),
    ]


def peer_command(
    scenario: Scenario,
    role: str,
    port: int,
    path: Path,
    byte_count: int,
    timeout_seconds: int,
) -> list[str]:
    if role not in ("caller", "listener"):
        raise ValueError(f"unsupported role: {role}")
    program = scenario.sender if role == "caller" else scenario.receiver
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
        scenario.transport,
        "--congestion",
        scenario.congestion,
        "--timeout-ms",
        str(timeout_seconds * 1_000),
        "--chunk-size",
        str(scenario.chunk_size),
        "--shutdown-grace-ms",
        str(DEFAULT_SHUTDOWN_GRACE_MILLISECONDS),
    ]
    if role == "caller":
        command.extend(
            (
                "--send-buffer",
                str(DEFAULT_SEND_BUFFER_BYTES),
                "--max-bw",
                str(DEFAULT_MAXIMUM_BANDWIDTH),
                "--expect-injected-peer-error",
            )
        )
        if scenario.expect_peer_error_observation is False:
            command.append("--require-missing-injected-peer-error")
        elif scenario.expect_peer_error_observation is None:
            command.append("--allow-missing-injected-peer-error")
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
    return port if type(port) is int and 0 < port <= 65_535 else None


def validate_evidence(
    scenario: Scenario,
    sender_output: str,
    receiver_output: str,
    injection: dict[str, object] | None,
    byte_count: int,
) -> int:
    sender = parse_event(sender_output, "peer_error")
    sender_complete = parse_complete(sender_output, "caller")
    receiver_complete = parse_complete(receiver_output, "listener")
    offset = sender.get("offset") if sender is not None else None
    first_data_sequence = (
        injection.get("first_data_sequence")
        if injection is not None
        else None
    )
    trigger_ack_next_sequence = (
        injection.get("trigger_ack_next_sequence")
        if injection is not None
        else None
    )
    acknowledgement_distance = (
        (trigger_ack_next_sequence - first_data_sequence)
        & SEQUENCE_MASK
        if (
            type(first_data_sequence) is int
            and type(trigger_ack_next_sequence) is int
        )
        else 0
    )
    expected_missing = scenario.expect_peer_error_observation is False
    missing_allowed = scenario.expect_peer_error_observation is None
    observed_count = sender.get("count") if sender is not None else None
    recovered = sender.get("recovered") if sender is not None else None
    observed_offset = sender.get("offset") if sender is not None else None
    observed_once = (
        observed_count == 1
        and recovered is True
        and type(observed_offset) is int
        and 0 < observed_offset < byte_count
    )
    observed_missing = (
        observed_count == 0
        and recovered is False
        and observed_offset == 0
    )
    observation_matches = (
        sender is not None
        and sender.get("matched") is True
        and sender.get("expected_missing") is expected_missing
        and sender.get("missing_allowed") is missing_allowed
        and (
            observed_missing
            if expected_missing
            else observed_once or (missing_allowed and observed_missing)
        )
    )
    if (
        not observation_matches
        or sender_complete.get("bytes") != byte_count
        or receiver_complete.get("bytes") != byte_count
        or injection is None
        or injection.get("control_type") != 8
        or injection.get("error_code") != 4_000
        or injection.get("datagram_bytes") != 20
        or type(injection.get("destination_socket_id")) is not int
        or int(injection["destination_socket_id"]) == 0
        or type(
            injection.get("trigger_acknowledgement_number")
        ) is not int
        or type(injection.get("trigger_ack_payload_bytes")) is not int
        or int(injection["trigger_ack_payload_bytes"]) < 4
        or not 0 < acknowledgement_distance
            < ((SEQUENCE_MASK + 1) // 2)
        or injection.get("ack_hold_milliseconds")
            != PEER_ERROR_ACK_HOLD_MILLISECONDS
        or type(injection.get("relay_monotonic_ns")) is not int
        or type(injection.get("ack_release_monotonic_ns")) is not int
        or int(injection["ack_release_monotonic_ns"])
            - int(injection["relay_monotonic_ns"])
            < PEER_ERROR_ACK_HOLD_MILLISECONDS * 1_000_000
    ):
        raise RuntimeError(
            f"{scenario.name}: inconsistent PEERERROR evidence: "
            f"sender={sender}, injection={injection}"
        )
    return int(offset) if type(offset) is int else 0


def render_failure(
    scenario: Scenario,
    reason: str,
    sender_stdout: str = "",
    sender_stderr: str = "",
    receiver_stdout: str = "",
    receiver_stderr: str = "",
    relay_trace: str = "",
) -> str:
    return (
        f"{scenario.name}: {reason}\n"
        f"--- sender stdout ---\n{sender_stdout or '<empty>'}\n"
        f"--- sender stderr ---\n{sender_stderr or '<empty>'}\n"
        f"--- receiver stdout ---\n{receiver_stdout or '<empty>'}\n"
        f"--- receiver stderr ---\n{receiver_stderr or '<empty>'}\n"
        f"--- relay trace ---\n{relay_trace or '<empty>'}\n"
    )


def run_scenario(
    scenario: Scenario,
    directory: Path,
    byte_count: int,
    timeout_seconds: int,
) -> None:
    listener_port = free_udp_port()
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    expected_digest = write_deterministic_payload(
        input_path, byte_count, scenario.seed
    )
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
                scenario,
                "listener",
                listener_port,
                output_path,
                byte_count,
                timeout_seconds,
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

        sender_stdout = ""
        sender_stderr = ""
        relay_trace = ""
        try:
            deadline = time.monotonic() + min(5, timeout_seconds)
            while True:
                receiver_stdout, receiver_stderr = receiver_output()
                if parse_ready_port(receiver_stdout) == listener_port:
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

            with CallerListenerPeerErrorProxy(listener_port) as relay:
                relay.start()
                sender = subprocess.run(
                    peer_command(
                        scenario,
                        "caller",
                        relay.port,
                        input_path,
                        byte_count,
                        timeout_seconds,
                    ),
                    capture_output=True,
                    text=True,
                    timeout=timeout_seconds + 5,
                    check=False,
                )
                sender_stdout = sender.stdout
                sender_stderr = sender.stderr
                try:
                    receiver.wait(timeout=timeout_seconds + 5)
                except subprocess.TimeoutExpired as error:
                    terminate(receiver)
                    receiver_stdout, receiver_stderr = receiver_output()
                    relay_trace = relay.render(scenario.name)
                    raise RuntimeError(
                        render_failure(
                            scenario,
                            "receiver timed out",
                            sender_stdout,
                            sender_stderr,
                            receiver_stdout,
                            receiver_stderr,
                            relay_trace,
                        )
                    ) from error
                receiver_stdout, receiver_stderr = receiver_output()
                relay_trace = relay.render(scenario.name)
                injection = relay.injection_observation()
                relay_error = relay.error()

            if sender.returncode != 0 or receiver.returncode != 0:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "peer process failed "
                        f"(sender={sender.returncode}, "
                        f"receiver={receiver.returncode})",
                        sender_stdout,
                        sender_stderr,
                        receiver_stdout,
                        receiver_stderr,
                        relay_trace,
                    )
                )
            if relay_error is not None:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        f"relay failed: {relay_error}",
                        sender_stdout,
                        sender_stderr,
                        receiver_stdout,
                        receiver_stderr,
                        relay_trace,
                    )
                )
            offset = validate_evidence(
                scenario,
                sender_stdout,
                receiver_stdout,
                injection,
                byte_count,
            )
            actual_digest = file_sha256(output_path)
            if actual_digest != expected_digest:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "payload digest mismatch: "
                        f"expected={expected_digest} actual={actual_digest}",
                        sender_stdout,
                        sender_stderr,
                        receiver_stdout,
                        receiver_stderr,
                        relay_trace,
                    )
                )
            print(
                f"PASS {scenario.name} sha256={actual_digest} "
                + (
                    f"peer_error_offset={offset}"
                    if scenario.expect_peer_error_observation is True
                    else "reference_gap=stream-peer-error-unobserved"
                    if scenario.expect_peer_error_observation is False
                    else (
                        f"reference_observation=reported "
                        f"peer_error_offset={offset}"
                        if offset != 0
                        else "reference_observation=missing"
                    )
                ),
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
    arguments = parser.parse_args()
    if arguments.bytes <= DEFAULT_SEND_BUFFER_BYTES:
        parser.error("byte count must exceed the bounded send buffer")
    if arguments.timeout_seconds <= 0:
        parser.error("timeout must be positive")

    robotweax = resolve_program_path(arguments.robotweax_peer)
    reference = resolve_program_path(arguments.reference_peer)
    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="robotweax-srt-peer-error-wire-interop-"
    ) as directory:
        work = Path(directory)
        for scenario in scenario_matrix(robotweax, reference):
            try:
                run_scenario(
                    scenario,
                    work,
                    arguments.bytes,
                    arguments.timeout_seconds,
                )
            except (
                OSError,
                RuntimeError,
                subprocess.TimeoutExpired,
            ) as error:
                failures.append(str(error))
    if failures:
        print("PEERERROR wire-injection failures:\n", flush=True)
        print("\n\n".join(failures), flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
