#!/usr/bin/env python3
"""Gate Robotweax caller-side group membership against pinned Haivision."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tempfile
import time
from contextlib import ExitStack, contextmanager
from pathlib import Path

from interop_common import free_udp_port, resolve_program_path, terminate
from srt_handshake_trace import (
    PEER_ERROR_ACK_HOLD_MILLISECONDS,
    CallerListenerFaultProxy,
    CallerListenerPeerErrorProxy,
)
from run_live_timing_interop import GroupPathOutageTraceProxy
from reference_version import compatible_srt_version, parse_srt_version


PINNED_SRT_VERSION = compatible_srt_version()
PEER_ERROR_PHASE_TIMEOUT_SECONDS = 6.0
REPLACEMENT_SUFFIX_MESSAGE_COUNT = 14
REPLACEMENT_FIRST_MESSAGE_NUMBER = 3
REPLACEMENT_LAST_MESSAGE_NUMBER = 16
SRT_SEQUENCE_MASK = 0x7FFF_FFFF
PINNED_REPLACEMENT_RECEIVE_ERROR = (
    "peer-error surviving-path receive/reply failed: "
    "Connection was broken"
)
EXPECTED_REVERSE_LISTENER_TIMEOUT = (
    "peer-error surviving-path receive/reply failed: "
    "Non-blocking call failure: transmission timed out"
)
PINNED_REPLACEMENT_CALLER_ERROR_PREFIX = (
    "peer-error surviving-path send/reply failed: "
)
PINNED_REPLACEMENT_CALLER_STATE_SUFFIX = (
    "; update=0; isolated=0; group_state=5; failed_state=5; "
    "healthy_state=5; replacement_state=9; replacement=1; "
    "replacement_distinct=0; replacement_update_absent=0; "
    "replacement_connected=0"
)
PINNED_REPLACEMENT_CALLER_ERRORS = tuple(
    PINNED_REPLACEMENT_CALLER_ERROR_PREFIX
    + detail
    + PINNED_REPLACEMENT_CALLER_STATE_SUFFIX
    for detail in (
        "group member isolation/update invariant failed",
    )
)
PINNED_OPTION_WARNING_SUFFIXES = (
    "OPTION: #35 UNKNOWN",
    "OPTION: #59 UNKNOWN",
)
PATH_OUTAGE_TRIGGER_MESSAGE = 4
PATH_OUTAGE_MESSAGE_COUNT = 16
PATH_OUTAGE_PHASE_TIMEOUT_SECONDS = 15.0
PATH_OUTAGE_REFERENCE_SENDER_ERROR = (
    "path-outage failover invariant failed: "
    "backup path did not become active"
)
PATH_OUTAGE_REFERENCE_STALLED_PRIMARY = "stalled-primary"
PATH_OUTAGE_REFERENCE_SCHEDULER_COLLAPSE = "scheduler-collapse"
PINNED_REFERENCE_LOG_PATTERN = re.compile(
    r"^\d{2}:\d{2}:\d{2}\.\d{6}/\S+: (?P<message>.+)$"
)


def replacement_admission_message_count(
    expect_pinned_listener_failure: bool,
) -> int:
    """Keep the pinned listener alive until its exact suffix is on wire."""
    return (
        REPLACEMENT_SUFFIX_MESSAGE_COUNT
        if expect_pinned_listener_failure
        else 1
    )


def release_completion_barrier(process: subprocess.Popen[str]) -> None:
    """Release a peer kept alive until transport evidence is complete."""
    if process.poll() is not None:
        return
    if process.stdin is None:
        raise RuntimeError("peer completion barrier has no stdin")
    try:
        process.stdin.write("continue\n")
        process.stdin.flush()
    except BrokenPipeError:
        # Validation below reports an unexpectedly terminated peer with
        # its complete stdout/stderr context.
        pass
    finally:
        try:
            process.stdin.close()
        except BrokenPipeError:
            pass


def wait_for_forwarded_data(
    relay: CallerListenerFaultProxy,
    processes: tuple[subprocess.Popen[str], ...],
    timeout_seconds: float,
    minimum_count: int = 1,
) -> dict[str, object] | None:
    """Wait until replacement DATA reaches the receiver transport."""
    if minimum_count <= 0:
        raise ValueError("minimum forwarded DATA count must be positive")
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        observation = relay.forwarded_data_observation("sender_to_receiver")
        if (
            observation is not None
            and int(observation.get("count", 0)) >= minimum_count
        ):
            return observation
        if any(process.poll() is not None for process in processes):
            return None
        time.sleep(0.01)
    return None


def wait_for_acknowledged_forwarded_data(
    relay: CallerListenerFaultProxy,
    processes: tuple[subprocess.Popen[str], ...],
    timeout_seconds: float,
) -> dict[str, object] | None:
    """Wait until replacement DATA has been forwarded and acknowledged."""
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        observation = relay.forwarded_data_observation(
            "sender_to_receiver"
        )
        if has_acknowledged_forwarded_data(observation):
            return observation
        if any(process.poll() is not None for process in processes):
            return None
        time.sleep(0.01)
    return None


def has_acknowledged_forwarded_data(
    observation: dict[str, object] | None,
) -> bool:
    return (
        observation is not None
        and int(observation.get("count", 0)) > 0
        and observation.get("acknowledged") is True
    )


def has_complete_replacement_relay_data(
    observation: dict[str, object] | None,
    expected_destination_socket_id: int | None,
) -> bool:
    """Recognize the exact suffix independently of application delivery."""
    if observation is None:
        return False
    first_sequence = observation.get("first_sequence")
    last_sequence = observation.get("last_sequence")
    first_message_number = observation.get("first_message_number")
    last_message_number = observation.get("last_message_number")
    first_forwarded_at = observation.get("first_forwarded_monotonic_ns")
    last_forwarded_at = observation.get("last_forwarded_monotonic_ns")
    if (
        type(expected_destination_socket_id) is not int
        or int(expected_destination_socket_id) <= 0
        or type(observation.get("count")) is not int
        or type(first_sequence) is not int
        or type(last_sequence) is not int
        or type(first_message_number) is not int
        or type(last_message_number) is not int
        or type(first_forwarded_at) is not int
        or type(last_forwarded_at) is not int
    ):
        return False
    sequence_span = (
        int(last_sequence) - int(first_sequence)
    ) & SRT_SEQUENCE_MASK
    complete = (
        observation.get("count") == REPLACEMENT_SUFFIX_MESSAGE_COUNT
        and first_message_number == REPLACEMENT_FIRST_MESSAGE_NUMBER
        and last_message_number == REPLACEMENT_LAST_MESSAGE_NUMBER
        and observation.get("sequences_contiguous") is True
        and observation.get("message_numbers_contiguous") is True
        and type(observation.get("destination_socket_id")) is int
        and observation.get("destination_socket_id")
            == expected_destination_socket_id
        and observation.get("destination_socket_id_consistent") is True
        and sequence_span == REPLACEMENT_SUFFIX_MESSAGE_COUNT - 1
        and int(last_forwarded_at) >= int(first_forwarded_at)
    )
    if not complete or type(observation.get("acknowledged")) is not bool:
        return False
    if observation.get("acknowledged") is False:
        return (
            "acknowledgement_next_sequence" not in observation
            and "acknowledgement_relay_ordinal" not in observation
            and "acknowledgement_monotonic_ns" not in observation
            and "acknowledgement_forwarded_data_count" not in observation
        )
    acknowledged_sequence = observation.get(
        "acknowledgement_next_sequence"
    )
    acknowledged_ordinal = observation.get(
        "acknowledgement_relay_ordinal"
    )
    acknowledged_at = observation.get("acknowledgement_monotonic_ns")
    acknowledged_data_count = observation.get(
        "acknowledgement_forwarded_data_count"
    )
    first_ordinal = observation.get("first_relay_ordinal")
    if (
        type(acknowledged_sequence) is not int
        or type(acknowledged_ordinal) is not int
        or type(acknowledged_at) is not int
        or type(acknowledged_data_count) is not int
        or type(first_ordinal) is not int
    ):
        return False
    acknowledged_span = (
        int(acknowledged_sequence) - int(first_sequence)
    ) & SRT_SEQUENCE_MASK
    return (
        1 <= acknowledged_span <= REPLACEMENT_SUFFIX_MESSAGE_COUNT
        and 1 <= int(acknowledged_data_count) <= int(observation["count"])
        and acknowledged_span <= int(acknowledged_data_count)
        and int(acknowledged_ordinal) > int(first_ordinal)
        and int(acknowledged_at) >= int(first_forwarded_at)
    )


def has_empty_forwarded_data(
    observation: dict[str, object] | None,
) -> bool:
    return (
        observation is not None
        and set(observation) == {"count", "acknowledged"}
        and type(observation.get("count")) is int
        and observation.get("count") == 0
        and type(observation.get("acknowledged")) is bool
        and observation.get("acknowledged") is False
    )


def has_valid_diagnostic_peer_error_injection(
    injection: dict[str, object] | None,
    expected_destination_socket_id: object,
) -> bool:
    if (
        injection is None
        or type(expected_destination_socket_id) is not int
        or int(expected_destination_socket_id) <= 0
    ):
        return False
    injected_at = injection.get("relay_monotonic_ns")
    acknowledgement_released_at = injection.get(
        "ack_release_monotonic_ns"
    )
    first_data_sequence = injection.get("first_data_sequence")
    trigger_ack_next_sequence = injection.get(
        "trigger_ack_next_sequence"
    )
    return (
        type(injection.get("control_type")) is int
        and injection.get("control_type") == 8
        and type(injection.get("error_code")) is int
        and injection.get("error_code") == 4_000
        and type(injection.get("datagram_bytes")) is int
        and injection.get("datagram_bytes") == 20
        and type(injection.get("destination_socket_id")) is int
        and injection.get("destination_socket_id")
            == expected_destination_socket_id
        and type(injection.get("ack_hold_milliseconds")) is int
        and injection.get("ack_hold_milliseconds")
            == PEER_ERROR_ACK_HOLD_MILLISECONDS
        and type(injected_at) is int
        and type(acknowledgement_released_at) is int
        and type(first_data_sequence) is int
        and type(trigger_ack_next_sequence) is int
        and (
            int(trigger_ack_next_sequence) - int(first_data_sequence)
        ) & SRT_SEQUENCE_MASK == 1
        and int(acknowledgement_released_at) - int(injected_at)
        >= PEER_ERROR_ACK_HOLD_MILLISECONDS * 1_000_000
    )


def has_expected_pinned_stderr(
    error: str, terminal_lines: tuple[str, ...]
) -> bool:
    """Allow only known v1.5.5 option noise before one terminal error."""
    lines = [line.strip() for line in error.splitlines() if line.strip()]
    return (
        bool(lines)
        and lines[-1] in terminal_lines
        and sum(line in terminal_lines for line in lines) == 1
        and all(
            any(line.endswith(suffix) for suffix in PINNED_OPTION_WARNING_SUFFIXES)
            for line in lines[:-1]
        )
    )


def has_expected_pinned_listener_replacement_stderr(
    error: str,
    expected_destination_socket_id: object,
    expected_first_sequence: object,
) -> bool:
    """Bind optional v1.5.5 TSBPD-drop noise to replacement DATA."""
    if (
        type(expected_destination_socket_id) is not int
        or int(expected_destination_socket_id) <= 0
        or type(expected_first_sequence) is not int
        or not 0 <= int(expected_first_sequence) <= SRT_SEQUENCE_MASK
    ):
        return False
    lines = [line.strip() for line in error.splitlines() if line.strip()]
    if (
        not lines
        or lines[-1] != PINNED_REPLACEMENT_RECEIVE_ERROR
        or lines.count(PINNED_REPLACEMENT_RECEIVE_ERROR) != 1
    ):
        return False
    dropped = 0
    pattern = re.compile(
        r"^.+/SRT:TsbPd!W:SRT\.br: @(?P<socket>[1-9][0-9]*): "
        r"RCV-DROPPED 1 packet\(s\)\. Packet seqno %(?P<sequence>[0-9]+) "
        r"delayed for (?P<delay>[0-9]+(?:\.[0-9]+)?) ms$"
    )
    for line in lines[:-1]:
        if any(
            line.endswith(suffix) for suffix in PINNED_OPTION_WARNING_SUFFIXES
        ):
            continue
        match = pattern.fullmatch(line)
        if (
            match is None
            or dropped != 0
            or int(match.group("socket"))
                != expected_destination_socket_id
            or int(match.group("sequence")) != expected_first_sequence
            or not 0.0 < float(match.group("delay")) <= 10_000.0
        ):
            return False
        dropped += 1
    return True


def has_expected_pinned_listener_replacement_failure(
    observation: dict[str, object] | None,
    expected_destination_socket_id: int | None,
    injection: dict[str, object] | None,
    caller_output: str,
    caller_error: str,
    listener_output: str,
    listener_error: str,
    caller_alive: bool,
    listener_returncode: int | None,
    replacement_retransmissions: int,
) -> bool:
    """Classify only the pinned v1.5.5 terminal listener signature."""
    sender_ready = parse_event(
        caller_output, "peer_error_sender_ready", "caller"
    )
    expected_first_sequence = (
        None if observation is None else observation.get("first_sequence")
    )
    return (
        caller_alive
        and listener_returncode == 6
        and replacement_retransmissions == 0
        and caller_error.strip() == ""
        and has_complete_replacement_relay_data(
            observation, expected_destination_socket_id
        )
        and sender_ready is not None
        and type(sender_ready.get("healthy_member")) is int
        and sender_ready.get("healthy_member")
            != sender_ready.get("failed_member")
        and has_valid_diagnostic_peer_error_injection(
            injection, sender_ready.get("failed_member")
        )
        and has_event(caller_output, "peer_error_barrier", "caller")
        and has_event(caller_output, "peer_error_replacement_ready", "caller")
        and not has_event(caller_output, "complete", "caller")
        and has_event(listener_output, "ready")
        and has_event(
            listener_output, "peer_error_receiver_ready", "listener"
        )
        and has_event(
            listener_output, "peer_error_replacement_ready", "listener"
        )
        and not has_event(listener_output, "complete", "listener")
        and has_expected_pinned_listener_replacement_stderr(
            listener_error,
            expected_destination_socket_id,
            expected_first_sequence,
        )
    )


def has_expected_pinned_caller_replacement_failure(
    observation: dict[str, object] | None,
    injection: dict[str, object] | None,
    caller_output: str,
    caller_error: str,
    listener_output: str,
    listener_error: str,
    caller_returncode: int | None,
    listener_returncode: int | None,
    replacement_retransmissions: int,
) -> bool:
    """Classify v1.5.5 failing to isolate a PEERERROR group member."""
    sender_ready = parse_event(
        caller_output, "peer_error_sender_ready", "caller"
    )
    unavailable = parse_event(
        caller_output, "peer_error_replacement_unavailable", "caller"
    )
    listener_state_valid = (
        listener_returncode is None
        and (
            listener_error.strip() == ""
            or has_expected_pinned_stderr(
                listener_error, (EXPECTED_REVERSE_LISTENER_TIMEOUT,)
            )
        )
    ) or (
        listener_returncode == 6
        and has_expected_pinned_stderr(
            listener_error, (EXPECTED_REVERSE_LISTENER_TIMEOUT,)
        )
    )
    return (
        caller_returncode == 5
        and listener_state_valid
        and replacement_retransmissions == 0
        and has_empty_forwarded_data(observation)
        and sender_ready is not None
        and type(sender_ready.get("healthy_member")) is int
        and sender_ready.get("healthy_member")
            != sender_ready.get("failed_member")
        and has_valid_diagnostic_peer_error_injection(
            injection, sender_ready.get("failed_member")
        )
        and has_event(caller_output, "peer_error_barrier", "caller")
        and not has_event(
            caller_output, "peer_error_replacement_ready", "caller"
        )
        and not has_event(caller_output, "complete", "caller")
        and unavailable is not None
        and unavailable.get("post_error_send_succeeded") is True
        and unavailable.get("peer_error_group_update") is False
        and unavailable.get("peer_error_member_isolated") is False
        and unavailable.get("replacement_distinct") is False
        and unavailable.get("replacement_connected") is False
        and unavailable.get("group_state") == 5
        and unavailable.get("failed_state") == 5
        and unavailable.get("healthy_state") == 5
        and unavailable.get("replacement_state") == 9
        and has_event(listener_output, "ready")
        and has_event(
            listener_output, "peer_error_receiver_ready", "listener"
        )
        and not has_event(
            listener_output, "peer_error_replacement_ready", "listener"
        )
        and not has_event(listener_output, "complete", "listener")
        and has_expected_pinned_stderr(
            caller_error, PINNED_REPLACEMENT_CALLER_ERRORS
        )
    )


def has_event(output: str, name: str, role: str | None = None) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == name and (
            role is None or event.get("role") == role
        ):
            return True
    return False


def has_baseline_payload_barrier(
    caller_output: str, listener_output: str
) -> bool:
    return has_event(
        caller_output, "payload_sender_ready", "caller"
    ) and has_event(
        listener_output, "payload_receiver_ready", "listener"
    )


def parse_event(
    output: str, name: str, role: str | None = None
) -> dict[str, object] | None:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == name and (
            role is None or event.get("role") == role
        ):
            return event
    return None


def has_valid_group_callback(
    output: str, expected_tokens: list[int]
) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "complete" and event.get("role") == "caller":
            return (
                event.get("callback_valid") is True
                and event.get("callback_calls") == len(expected_tokens)
                and event.get("tokens") == expected_tokens
            )
    return False


def has_listener_update(output: str) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (
            event.get("event") == "complete"
            and event.get("role") == "listener"
        ):
            return event.get("listener_update") is True
    return False


def has_late_listener_update(output: str) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (
            event.get("event") == "complete"
            and event.get("role") == "listener"
        ):
            return event.get("late_listener_update") is True
    return False


def has_initial_listener_update(output: str) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (
            event.get("event") == "complete"
            and event.get("role") == "listener"
        ):
            return event.get("initial_listener_update") is True
    return False


def has_required_listener_updates(
    output: str, late_join: bool, require_repeated: bool
) -> bool:
    return (
        has_listener_update(output)
        and (not late_join or has_initial_listener_update(output))
        and (not require_repeated or has_late_listener_update(output))
    )


def has_bonded_listener(output: str) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (
            event.get("event") == "complete"
            and event.get("role") == "listener"
        ):
            return event.get("bonded_listeners") == 2
    return False


def has_valid_payload(output: str, role: str) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "complete" and event.get("role") == role:
            return event.get("payload_valid") is True
    return False


def has_invalid_control_size_diagnostic(error: str) -> bool:
    """Detect the reference rejection emitted for malformed controls."""

    return "INVALID SIZE" in error.upper()


def has_valid_receive_contract(
    output: str, *, pinned_reference: bool
) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") != "receive_contract":
            continue
        expected_profile = (
            "reference" if pinned_reference else "strict"
        )
        expected_timeout_error = 6_002 if pinned_reference else 6_003
        return (
            event.get("role") == "listener"
            and event.get("matched") is True
            and event.get("profile") == expected_profile
            and event.get("nonblocking_result") == -1
            and event.get("nonblocking_error") == 6_002
            and type(event.get("nonblocking_elapsed_us")) is int
            and 0 <= int(event["nonblocking_elapsed_us"]) < 500_000
            and event.get("timeout_result") == -1
            and event.get("timeout_error") == expected_timeout_error
            and event.get("timeout_configured_ms") == 150
            and type(event.get("timeout_elapsed_us")) is int
            and 50_000 <= int(event["timeout_elapsed_us"]) < 5_000_000
        )
    return False


def has_late_join(output: str, role: str) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "complete" and event.get("role") == role:
            return event.get("late_join") is True
    return False


def has_member_count(output: str, role: str, expected: int) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == "complete" and event.get("role") == role:
            return event.get("members") == expected
    return False


def has_valid_peer_metadata(
    output: str, role: str, expected_members: int
) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") != "complete" or event.get("role") != role:
            continue
        valid = (
            event.get("peer_version") == PINNED_SRT_VERSION
            and event.get("peer_version_valid") is True
        )
        if role == "listener":
            valid = (
                valid
                and event.get("listener_metadata_valid") is True
                and event.get("listener_callback_calls")
                == expected_members
            )
        return valid
    return False


def wait_ready(
    process: subprocess.Popen[str], output: Path, error: Path
) -> None:
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if has_event(output.read_text(errors="replace"), "ready"):
            return
        if process.poll() is not None:
            break
        time.sleep(0.02)
    raise RuntimeError(
        "reference group listener did not become ready\n"
        f"--- listener stdout ---\n{output.read_text(errors='replace')}\n"
        f"--- listener stderr ---\n{error.read_text(errors='replace')}\n"
    )


@contextmanager
def group_case_directory(evidence_directory: Path | None):
    if evidence_directory is None:
        with tempfile.TemporaryDirectory(prefix="robotweax-group-") as raw:
            yield Path(raw)
    else:
        evidence_directory.mkdir(parents=True, exist_ok=False)
        yield evidence_directory


def run_case(
    listener_program: Path,
    caller_program: Path,
    name: str,
    policy: str,
    require_group_callback: bool,
    bonded: bool = False,
    profile: str = "baseline",
    require_repeated_listener_update: bool = False,
    environment: dict[str, str] | None = None,
    evidence_directory: Path | None = None,
) -> None:
    if evidence_directory is not None and profile not in (
        "reference-receive-contract", "receive-contract", "baseline", "late-join",
    ):
        raise ValueError("persistent evidence requires a baseline matrix profile")
    if bonded and profile != "baseline":
        raise ValueError("fault profiles cannot be combined with bonding")
    port = free_udp_port()
    bonded_port = free_udp_port() if bonded else None
    while bonded_port == port:
        bonded_port = free_udp_port()

    with group_case_directory(evidence_directory) as directory:
        stdout_path = directory / "listener.stdout"
        stderr_path = directory / "listener.stderr"
        caller_stdout_path = directory / "caller.stdout"
        caller_stderr_path = directory / "caller.stderr"
        with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr, \
                caller_stdout_path.open("w") as caller_output, \
                caller_stderr_path.open("w") as caller_error:
            listener_command = [
                str(listener_program), "listener", "127.0.0.1",
                str(port), policy,
            ]
            if bonded_port is not None:
                listener_command.append(str(bonded_port))
            elif profile != "baseline":
                listener_command.append(profile)
            if evidence_directory is not None:
                (directory / "case.json").write_text(json.dumps({
                    "name": name, "policy": policy, "profile": profile,
                    "listener_command": listener_command,
                    "caller_program": str(caller_program),
                    "harness_pid": os.getpid(),
                    "started_unix_ns": time.time_ns(),
                }, indent=2) + "\n", encoding="utf-8")
            listener = subprocess.Popen(
                listener_command,
                stdin=subprocess.PIPE,
                stdout=stdout,
                stderr=stderr,
                text=True,
                env=environment,
            )
            if evidence_directory is not None:
                identity_path = directory / "listener-process.json"
                identity_path.write_text(json.dumps({
                    "pid": listener.pid, "parent_pid": os.getpid(),
                    "program": str(listener_program),
                }) + "\n")
            caller: subprocess.CompletedProcess[str] | None = None
            try:
                wait_ready(listener, stdout_path, stderr_path)
                caller_command = [
                    str(caller_program), "caller", "127.0.0.1",
                    str(port), policy,
                ]
                if bonded_port is not None:
                    caller_command.append(str(bonded_port))
                elif profile != "baseline":
                    caller_command.append(profile)
                if profile in (
                    "receive-contract",
                    "reference-receive-contract",
                ):
                    caller_process = subprocess.Popen(
                        caller_command,
                        stdin=subprocess.PIPE,
                        stdout=(caller_output if evidence_directory is not None
                                else subprocess.PIPE),
                        stderr=(caller_error if evidence_directory is not None
                                else subprocess.PIPE),
                        text=True,
                        env=environment,
                    )
                    try:
                        deadline = time.monotonic() + 5.0
                        while time.monotonic() < deadline:
                            if has_event(
                                stdout_path.read_text(errors="replace"),
                                "receive_contract",
                                "listener",
                            ):
                                break
                            if (
                                caller_process.poll() is not None
                                or listener.poll() is not None
                            ):
                                break
                            time.sleep(0.02)
                        else:
                            raise RuntimeError(
                                "group receive contract probe did not finish"
                            )
                        if (
                            caller_process.poll() is not None
                            or listener.poll() is not None
                            or caller_process.stdin is None
                        ):
                            raise RuntimeError(
                                "group receive contract endpoint stopped "
                                "before release"
                            )
                        caller_process.stdin.write("send\n")
                        caller_process.stdin.flush()
                        caller_stdout, caller_stderr = (
                            caller_process.communicate(timeout=12)
                        )
                        if evidence_directory is not None:
                            caller_stdout = caller_stdout_path.read_text(errors="replace")
                            caller_stderr = caller_stderr_path.read_text(errors="replace")
                        caller = subprocess.CompletedProcess(
                            caller_command,
                            caller_process.returncode,
                            caller_stdout,
                            caller_stderr,
                        )
                    finally:
                        terminate(caller_process)
                elif profile == "baseline":
                    caller_process = subprocess.Popen(
                        caller_command,
                        stdin=subprocess.PIPE,
                        stdout=(caller_output if evidence_directory is not None
                                else subprocess.PIPE),
                        stderr=(caller_error if evidence_directory is not None
                                else subprocess.PIPE),
                        text=True,
                        env=environment,
                    )
                    try:
                        deadline = time.monotonic() + 5.0
                        while time.monotonic() < deadline:
                            if has_event(
                                stdout_path.read_text(errors="replace"),
                                "payload_receiver_ready",
                                "listener",
                            ):
                                break
                            if (
                                caller_process.poll() is not None
                                or listener.poll() is not None
                            ):
                                break
                            time.sleep(0.02)
                        else:
                            raise RuntimeError(
                                "baseline group receiver did not become ready"
                            )
                        if (
                            caller_process.poll() is not None
                            or listener.poll() is not None
                            or caller_process.stdin is None
                        ):
                            raise RuntimeError(
                                "baseline group endpoint stopped before "
                                "payload release"
                            )
                        caller_process.stdin.write("send\n")
                        caller_process.stdin.flush()
                        caller_stdout, caller_stderr = (
                            caller_process.communicate(timeout=12)
                        )
                        if evidence_directory is not None:
                            caller_stdout = caller_stdout_path.read_text(errors="replace")
                            caller_stderr = caller_stderr_path.read_text(errors="replace")
                        caller = subprocess.CompletedProcess(
                            caller_command,
                            caller_process.returncode,
                            caller_stdout,
                            caller_stderr,
                        )
                    finally:
                        terminate(caller_process)
                else:
                    caller = subprocess.run(
                        caller_command,
                        stdout=(caller_output if evidence_directory is not None
                                else subprocess.PIPE),
                        stderr=(caller_error if evidence_directory is not None
                                else subprocess.PIPE),
                        text=True,
                        timeout=12,
                        check=False,
                        env=environment,
                    )
                    if evidence_directory is not None:
                        caller.stdout = caller_stdout_path.read_text(errors="replace")
                        caller.stderr = caller_stderr_path.read_text(errors="replace")
                release_completion_barrier(listener)
                listener.wait(timeout=12)
            finally:
                terminate(listener)
        listener_stdout = stdout_path.read_text(errors="replace")
        listener_stderr = stderr_path.read_text(errors="replace")
        expected_members = 3 if profile == "late-join" else 2
        expected_tokens = (
            [4001, 4002, 4003]
            if profile == "late-join"
            else [4001, 4002]
        )
        if (
            caller is None
            or caller.returncode != 0
            or listener.returncode != 0
            or not has_event(caller.stdout, "complete", "caller")
            or not has_event(listener_stdout, "complete", "listener")
            or (
                profile == "baseline"
                and not has_baseline_payload_barrier(
                    caller.stdout, listener_stdout
                )
            )
            or not has_required_listener_updates(
                listener_stdout,
                late_join=profile == "late-join",
                require_repeated=require_repeated_listener_update,
            )
            or not has_member_count(
                caller.stdout, "caller", expected_members
            )
            or not has_member_count(
                listener_stdout, "listener", expected_members
            )
            or not has_valid_peer_metadata(
                caller.stdout, "caller", expected_members
            )
            or not has_valid_peer_metadata(
                listener_stdout, "listener", expected_members
            )
            or (
                not has_valid_payload(caller.stdout, "caller")
                or not has_valid_payload(listener_stdout, "listener")
            )
            or (
                profile
                in ("receive-contract", "reference-receive-contract")
                and not has_valid_receive_contract(
                    listener_stdout,
                    pinned_reference=(
                        profile == "reference-receive-contract"
                    ),
                )
            )
            or (bonded and not has_bonded_listener(listener_stdout))
            or (
                profile == "late-join"
                and (
                    not has_late_join(caller.stdout, "caller")
                    or not has_late_join(listener_stdout, "listener")
                )
            )
            or (
                require_group_callback
                and not has_valid_group_callback(
                    caller.stdout, expected_tokens
                )
            )
            or has_invalid_control_size_diagnostic(caller.stderr)
            or has_invalid_control_size_diagnostic(listener_stderr)
        ):
            raise RuntimeError(
                f"connection-group interoperability failed: {name} "
                f"({policy}, {profile})\n"
                f"--- caller stdout ---\n{caller.stdout if caller else '<none>'}\n"
                f"--- caller stderr ---\n{caller.stderr if caller else '<none>'}\n"
                f"--- listener stdout ---\n{listener_stdout}\n"
                f"--- listener stderr ---\n{listener_stderr}\n"
            )
        print(f"PASS {name} ({policy}, {profile})")


def has_valid_peer_error_evidence(
    caller: dict[str, object] | None,
    listener: dict[str, object] | None,
    injection: dict[str, object] | None,
    replacement: bool = False,
    reference_listener: bool = False,
) -> bool:
    if caller is None or listener is None or injection is None:
        return False
    healthy_member = caller.get("healthy_member")
    failed_member = caller.get("failed_member")
    injected_at = injection.get("relay_monotonic_ns")
    acknowledgement_released_at = injection.get(
        "ack_release_monotonic_ns"
    )
    if (
        type(healthy_member) is not int
        or type(failed_member) is not int
        or healthy_member == failed_member
        or type(injected_at) is not int
        or type(acknowledgement_released_at) is not int
    ):
        return False
    replacement_member = caller.get("replacement_member")
    if replacement and (
        type(replacement_member) is not int
        or replacement_member in (healthy_member, failed_member)
    ):
        return False
    listener_peer_version = (
        listener.get("peer_version") == PINNED_SRT_VERSION
        and listener.get("peer_version_valid") is True
    ) or (
        reference_listener
        and (
            # Pinned Haivision v1.5.5 loses the aggregate SRTO_PEERVERSION
            # observation after one group member fails. The Robotweax caller
            # has already validated the same peer version before this
            # transition.
            listener.get("peer_version") == 0
            and listener.get("peer_version_valid") is False
        )
    )
    return (
        caller.get("payload_valid") is True
        and caller.get("callback_valid") is True
        and caller.get("peer_error") is True
        and caller.get("peer_error_member_isolated") is True
        and caller.get("peer_error_group_update") is True
        and caller.get("peer_error_replacement") is replacement
        and (
            not replacement
            or (
                caller.get("replacement_distinct") is True
                and caller.get("replacement_group_update_absent") is True
                and caller.get("replacement_connected") is True
            )
        )
        and caller.get("group_connected") is True
        and caller.get("members") == 2
        and caller.get("peer_version_valid") is True
        and listener.get("payload_valid") is True
        and listener.get("peer_error") is True
        and listener.get("peer_error_replacement") is replacement
        and listener.get("replacement_attached") is replacement
        and type(listener.get("transient_group_receive_errors")) is int
        and int(listener["transient_group_receive_errors"]) >= 0
        and (
            reference_listener
            or listener.get("transient_group_receive_errors") == 0
        )
        and listener.get("group_connected") is True
        and listener.get("bonded_listeners") == 2
        and listener.get("listener_callback_calls")
            == (3 if replacement else 2)
        and listener.get("listener_metadata_valid") is True
        and listener_peer_version
        and injection.get("control_type") == 8
        and injection.get("error_code") == 4_000
        and injection.get("datagram_bytes") == 20
        and injection.get("destination_socket_id") == failed_member
        and injection.get("ack_hold_milliseconds")
            == PEER_ERROR_ACK_HOLD_MILLISECONDS
        and acknowledgement_released_at - injected_at
            >= PEER_ERROR_ACK_HOLD_MILLISECONDS * 1_000_000
    )


def run_peer_error_case(
    caller_program: Path,
    listener_program: Path,
    caller_name: str,
    listener_name: str,
    replacement: bool = False,
    reference_listener: bool = False,
    expect_pinned_listener_failure: bool = False,
    expect_pinned_caller_failure: bool = False,
) -> None:
    if (
        expect_pinned_listener_failure or expect_pinned_caller_failure
    ) and not replacement:
        raise ValueError("pinned group failures apply only to replacement")
    if expect_pinned_listener_failure and not reference_listener:
        raise ValueError(
            "the pinned listener failure requires the reference listener"
        )
    if expect_pinned_listener_failure and expect_pinned_caller_failure:
        raise ValueError("only one pinned endpoint failure can be expected")
    if expect_pinned_caller_failure and reference_listener:
        raise ValueError(
            "the pinned caller failure requires the Robotweax listener"
        )
    listener_ports = [free_udp_port(), free_udp_port()]
    while listener_ports[1] == listener_ports[0]:
        listener_ports[1] = free_udp_port()

    with tempfile.TemporaryDirectory(
        prefix="robotweax-group-peer-error-"
    ) as raw:
        directory = Path(raw)
        paths = {
            name: directory / name
            for name in (
                "listener.stdout",
                "listener.stderr",
                "caller.stdout",
                "caller.stderr",
            )
        }
        with (
            paths["listener.stdout"].open("w") as listener_stdout,
            paths["listener.stderr"].open("w") as listener_stderr,
            paths["caller.stdout"].open("w") as caller_stdout,
            paths["caller.stderr"].open("w") as caller_stderr,
        ):
            listener = subprocess.Popen(
                [
                    str(listener_program),
                    "listener",
                    "127.0.0.1",
                    str(listener_ports[0]),
                    "broadcast",
                    str(listener_ports[1]),
                    (
                        "peer-error-replacement"
                        if replacement else "peer-error"
                    ),
                ],
                stdin=subprocess.PIPE,
                stdout=listener_stdout,
                stderr=listener_stderr,
                text=True,
            )
            caller: subprocess.Popen[str] | None = None
            healthy_relay = CallerListenerFaultProxy(listener_ports[0])
            affected_relay = CallerListenerPeerErrorProxy(
                listener_ports[1]
            )
            replacement_relay = (
                CallerListenerFaultProxy(listener_ports[1])
                if replacement else None
            )
            replacement_transfer: dict[str, object] | None = None
            replacement_failure_observation: dict[str, object] | None = None
            replacement_failure_destination_socket_id: int | None = None
            replacement_failure_caller_alive = False
            replacement_failure_listener_returncode: int | None = None
            replacement_failure_retransmissions = -1
            replacement_failure_caller_output = ""
            replacement_failure_caller_error = ""
            replacement_failure_listener_output = ""
            replacement_failure_listener_error = ""
            pinned_listener_failure_path = False
            pinned_caller_failure_path = False
            replacement_caller_failure_observation: (
                dict[str, object] | None
            ) = None
            replacement_caller_failure_returncode: int | None = None
            replacement_caller_failure_listener_returncode: int | None = None
            replacement_caller_failure_retransmissions = -1
            replacement_caller_failure_caller_output = ""
            replacement_caller_failure_caller_error = ""
            replacement_caller_failure_listener_output = ""
            replacement_caller_failure_listener_error = ""

            def replacement_diagnostics() -> str:
                if replacement_relay is None:
                    return "<not used>"
                trace = replacement_relay.render(
                    "group-peer-error-replacement"
                )
                forwarded = json.dumps(
                    {
                        "event": "srt_forwarded_data_observation",
                        "scenario": "group-peer-error-replacement",
                        **replacement_relay.forwarded_data_observation(
                            "sender_to_receiver"
                        ),
                    },
                    sort_keys=True,
                )
                return "\n".join((trace, forwarded))

            try:
                wait_ready(
                    listener,
                    paths["listener.stdout"],
                    paths["listener.stderr"],
                )
                with ExitStack() as relays:
                    relays.enter_context(healthy_relay)
                    relays.enter_context(affected_relay)
                    if replacement_relay is not None:
                        relays.enter_context(replacement_relay)
                    healthy_relay.start()
                    affected_relay.start()
                    if replacement_relay is not None:
                        replacement_relay.start()
                    caller_command = [
                        str(caller_program),
                        "caller",
                        "127.0.0.1",
                        str(healthy_relay.port),
                        "broadcast",
                        str(affected_relay.port),
                    ]
                    if replacement_relay is not None:
                        caller_command.append(str(replacement_relay.port))
                        caller_command.append("peer-error-replacement")
                    else:
                        caller_command.append("peer-error")
                    caller = subprocess.Popen(
                        caller_command,
                        stdin=subprocess.PIPE,
                        stdout=caller_stdout,
                        stderr=caller_stderr,
                        text=True,
                    )

                    def phase_output(name: str) -> str:
                        return paths[name].read_text(
                            errors="replace"
                        ) or "<empty>"

                    def phase_failure(reason: str) -> RuntimeError:
                        return RuntimeError(
                            f"{reason}; caller_returncode={caller.poll()} "
                            f"listener_returncode={listener.poll()}\n"
                            "--- caller stdout ---\n"
                            f"{phase_output('caller.stdout')}\n"
                            "--- caller stderr ---\n"
                            f"{phase_output('caller.stderr')}\n"
                            "--- listener stdout ---\n"
                            f"{phase_output('listener.stdout')}\n"
                            "--- listener stderr ---\n"
                            f"{phase_output('listener.stderr')}\n"
                            "--- healthy relay ---\n"
                            f"{healthy_relay.render('group-peer-error-healthy')}\n"
                            "--- affected relay ---\n"
                            f"{affected_relay.render('group-peer-error-affected')}\n"
                            "--- replacement relay ---\n"
                            f"{replacement_diagnostics()}\n"
                        )

                    readiness_deadline = (
                        time.monotonic()
                        + PEER_ERROR_PHASE_TIMEOUT_SECONDS
                    )
                    ready = False
                    while time.monotonic() < readiness_deadline:
                        caller_output = paths["caller.stdout"].read_text(
                            errors="replace"
                        )
                        listener_output = paths[
                            "listener.stdout"
                        ].read_text(errors="replace")
                        if (
                            has_event(
                                caller_output,
                                "peer_error_sender_ready",
                                "caller",
                            )
                            and has_event(
                                listener_output,
                                "peer_error_receiver_ready",
                                "listener",
                            )
                        ):
                            ready = True
                            break
                        if (
                            caller.poll() is not None
                            or listener.poll() is not None
                        ):
                            break
                        time.sleep(0.01)
                    if not ready or caller.stdin is None:
                        raise phase_failure(
                            "group PEERERROR endpoints did not reach the "
                            "coordinated transfer barrier"
                        )
                    caller.stdin.write("send\n")
                    caller.stdin.flush()

                    deadline = (
                        time.monotonic()
                        + PEER_ERROR_PHASE_TIMEOUT_SECONDS
                    )
                    injection: dict[str, object] | None = None
                    causal_ready = False
                    while time.monotonic() < deadline:
                        caller_output = paths["caller.stdout"].read_text(
                            errors="replace"
                        )
                        injection = affected_relay.injection_observation()
                        if (
                            has_event(
                                caller_output,
                                "peer_error_barrier",
                                "caller",
                            )
                            and injection is not None
                        ):
                            causal_ready = True
                            break
                        if (
                            caller.poll() is not None
                            or listener.poll() is not None
                        ):
                            break
                        time.sleep(0.01)
                    if not causal_ready:
                        raise phase_failure(
                            "group PEERERROR phase did not reach the "
                            "causal injection barrier"
                        )
                    if (
                        caller.poll() is not None
                        or listener.poll() is not None
                        or injection is None
                        or caller.stdin is None
                    ):
                        raise phase_failure(
                            "group PEERERROR endpoint exited before release"
                        )
                    caller.stdin.write("continue\n")
                    caller.stdin.flush()
                    if replacement:
                        replacement_deadline = (
                            time.monotonic()
                            + PEER_ERROR_PHASE_TIMEOUT_SECONDS
                        )
                        replacement_ready = False
                        while time.monotonic() < replacement_deadline:
                            caller_output = paths[
                                "caller.stdout"
                            ].read_text(errors="replace")
                            listener_output = paths[
                                "listener.stdout"
                            ].read_text(errors="replace")
                            if (
                                has_event(
                                    caller_output,
                                    "peer_error_replacement_ready",
                                    "caller",
                                )
                                and has_event(
                                    listener_output,
                                    "peer_error_replacement_ready",
                                    "listener",
                                )
                            ):
                                replacement_ready = True
                                break
                            if (
                                caller.poll() is not None
                                or listener.poll() is not None
                            ):
                                break
                            time.sleep(0.01)
                        if (
                            not replacement_ready
                            or caller.stdin is None
                            or listener.stdin is None
                        ):
                            if not expect_pinned_caller_failure:
                                raise phase_failure(
                                    "group PEERERROR replacement did not "
                                    "reach the coordinated payload barrier"
                                )
                            pinned_caller_failure_path = True
                        if (
                            not pinned_caller_failure_path
                            and replacement_relay is None
                        ):
                            raise phase_failure(
                                "group PEERERROR replacement relay is absent"
                            )
                        # Release the sender first, then use wire evidence to
                        # prove that replacement DATA has reached the receiver
                        # transport before its application receive path runs.
                        # This preserves the causal admission guarantee without
                        # relying on an arbitrary scheduling delay.
                        if not pinned_caller_failure_path:
                            assert replacement_relay is not None
                            caller.stdin.write("resume\n")
                            caller.stdin.flush()
                            replacement_admission = wait_for_forwarded_data(
                                replacement_relay,
                                (caller, listener),
                                PEER_ERROR_PHASE_TIMEOUT_SECONDS,
                                minimum_count=(
                                    replacement_admission_message_count(
                                        expect_pinned_listener_failure
                                    )
                                ),
                            )
                            if replacement_admission is None:
                                raise phase_failure(
                                    "group PEERERROR replacement suffix was "
                                    "not forwarded before listener release"
                                )
                            listener.stdin.write("resume\n")
                            listener.stdin.flush()
                    if replacement and not pinned_caller_failure_path:
                        assert replacement_relay is not None
                        replacement_transfer = (
                            wait_for_acknowledged_forwarded_data(
                                replacement_relay,
                                (caller, listener),
                                PEER_ERROR_PHASE_TIMEOUT_SECONDS,
                            )
                        )
                        if expect_pinned_listener_failure:
                            failure_deadline = (
                                time.monotonic()
                                + PEER_ERROR_PHASE_TIMEOUT_SECONDS
                            )
                            while (
                                listener.poll() is None
                                and caller.poll() is None
                                and time.monotonic() < failure_deadline
                            ):
                                time.sleep(0.01)
                            wait_for_forwarded_data(
                                replacement_relay,
                                (caller,),
                                PEER_ERROR_PHASE_TIMEOUT_SECONDS,
                                minimum_count=(
                                    REPLACEMENT_SUFFIX_MESSAGE_COUNT
                                ),
                            )
                            pinned_listener_failure_path = True
                        elif replacement_transfer is None:
                            raise phase_failure(
                                "group PEERERROR replacement suffix was not "
                                "forwarded and cumulatively acknowledged "
                                "before teardown"
                            )
                        else:
                            release_completion_barrier(caller)
                    else:
                        caller.stdin.close()
                    if not (
                        pinned_listener_failure_path
                        or pinned_caller_failure_path
                    ):
                        caller.wait(timeout=12)
                        release_completion_barrier(listener)
                        listener.wait(timeout=12)

                # ExitStack has stopped every relay. Capture all defect
                # evidence at this common point before the harness forcibly
                # terminates a peer that is intentionally kept alive.
                if pinned_listener_failure_path:
                    assert caller is not None
                    assert replacement_relay is not None
                    replacement_failure_observation = (
                        replacement_relay.forwarded_data_observation(
                            "sender_to_receiver"
                        )
                    )
                    replacement_failure_destination_socket_id = (
                        replacement_relay.conclusion_socket_id(
                            "receiver_to_sender"
                        )
                    )
                    replacement_failure_retransmissions = (
                        replacement_relay.forwarded_data_retransmissions(
                            "sender_to_receiver"
                        )
                    )
                    replacement_failure_caller_output = paths[
                        "caller.stdout"
                    ].read_text(errors="replace")
                    replacement_failure_caller_error = paths[
                        "caller.stderr"
                    ].read_text(errors="replace")
                    replacement_failure_listener_output = paths[
                        "listener.stdout"
                    ].read_text(errors="replace")
                    replacement_failure_listener_error = paths[
                        "listener.stderr"
                    ].read_text(errors="replace")
                    replacement_failure_caller_alive = caller.poll() is None
                    replacement_failure_listener_returncode = listener.poll()
                if pinned_caller_failure_path:
                    assert caller is not None
                    assert replacement_relay is not None
                    replacement_caller_failure_observation = (
                        replacement_relay.forwarded_data_observation(
                            "sender_to_receiver"
                        )
                    )
                    replacement_caller_failure_retransmissions = (
                        replacement_relay.forwarded_data_retransmissions(
                            "sender_to_receiver"
                        )
                    )
                    replacement_caller_failure_caller_output = paths[
                        "caller.stdout"
                    ].read_text(errors="replace")
                    replacement_caller_failure_caller_error = paths[
                        "caller.stderr"
                    ].read_text(errors="replace")
                    replacement_caller_failure_listener_output = paths[
                        "listener.stdout"
                    ].read_text(errors="replace")
                    replacement_caller_failure_listener_error = paths[
                        "listener.stderr"
                    ].read_text(errors="replace")
                    replacement_caller_failure_returncode = caller.poll()
                    replacement_caller_failure_listener_returncode = (
                        listener.poll()
                    )
            finally:
                if caller is not None:
                    terminate(caller)
                terminate(listener)

        caller_output = paths["caller.stdout"].read_text(errors="replace")
        caller_error = paths["caller.stderr"].read_text(errors="replace")
        listener_output = paths["listener.stdout"].read_text(
            errors="replace"
        )
        listener_error = paths["listener.stderr"].read_text(
            errors="replace"
        )
        injection = affected_relay.injection_observation()
        caller_complete = parse_event(caller_output, "complete", "caller")
        listener_complete = parse_event(
            listener_output, "complete", "listener"
        )
        relays_valid = (
            healthy_relay.error() is None
            and affected_relay.error() is None
            and (
                replacement_relay is None
                or replacement_relay.error() is None
            )
        )
        if expect_pinned_listener_failure:
            valid = (
                pinned_listener_failure_path
                and relays_valid
                and has_expected_pinned_listener_replacement_failure(
                    replacement_failure_observation,
                    replacement_failure_destination_socket_id,
                    injection,
                    replacement_failure_caller_output,
                    replacement_failure_caller_error,
                    replacement_failure_listener_output,
                    replacement_failure_listener_error,
                    replacement_failure_caller_alive,
                    replacement_failure_listener_returncode,
                    replacement_failure_retransmissions,
                )
                and caller_output == replacement_failure_caller_output
                and caller_error == replacement_failure_caller_error
                and listener_output == replacement_failure_listener_output
                and listener_error == replacement_failure_listener_error
            )
        elif expect_pinned_caller_failure:
            valid = (
                pinned_caller_failure_path
                and relays_valid
                and has_expected_pinned_caller_replacement_failure(
                    replacement_caller_failure_observation,
                    injection,
                    replacement_caller_failure_caller_output,
                    replacement_caller_failure_caller_error,
                    replacement_caller_failure_listener_output,
                    replacement_caller_failure_listener_error,
                    replacement_caller_failure_returncode,
                    replacement_caller_failure_listener_returncode,
                    replacement_caller_failure_retransmissions,
                )
                and caller_output
                    == replacement_caller_failure_caller_output
                and caller_error == replacement_caller_failure_caller_error
                and listener_output
                    == replacement_caller_failure_listener_output
                and listener_error
                    == replacement_caller_failure_listener_error
            )
        else:
            valid = (
                caller is not None
                and caller.returncode == 0
                and listener.returncode == 0
                and relays_valid
                and (
                    not replacement
                    or has_acknowledged_forwarded_data(
                        replacement_transfer
                    )
                )
                and has_valid_peer_error_evidence(
                    caller_complete,
                    listener_complete,
                    injection,
                    replacement=replacement,
                    reference_listener=reference_listener,
                )
            )
        if not valid:
            raise RuntimeError(
                "connection-group PEERERROR interoperability failed "
                f"({caller_name} caller -> {listener_name} listener)\n"
                f"--- caller stdout ---\n{caller_output or '<empty>'}\n"
                f"--- caller stderr ---\n{caller_error or '<empty>'}\n"
                f"--- listener stdout ---\n{listener_output or '<empty>'}\n"
                f"--- listener stderr ---\n{listener_error or '<empty>'}\n"
                "--- healthy relay ---\n"
                f"{healthy_relay.render('group-peer-error-healthy')}\n"
                "--- affected relay ---\n"
                f"{affected_relay.render('group-peer-error-affected')}\n"
                "--- replacement relay ---\n"
                f"{replacement_diagnostics()}\n"
            )
        if expect_pinned_listener_failure:
            transport_acknowledged = bool(
                replacement_failure_observation
                and replacement_failure_observation.get("acknowledged")
            )
            print(
                "PASS relay forwards the complete replacement suffix toward "
                "pinned Haivision v1.5.5, whose group receive latches closed "
                "before "
                "application delivery "
                f"(transport_ack={str(transport_acknowledged).lower()}, "
                "version-scoped defect diagnostic)"
            )
        elif expect_pinned_caller_failure:
            print(
                "PASS pinned Haivision v1.5.5 caller keeps the PEERERROR "
                "member connected and cannot initiate replacement "
                "(version-scoped defect diagnostic)"
            )
        elif replacement:
            print(
                f"PASS {caller_name} broadcast group replaces an injected "
                f"PEERERROR member through the {listener_name} listener"
            )
        else:
            print(
                f"PASS {caller_name} broadcast group isolates an injected "
                f"PEERERROR member and preserves the surviving "
                f"{listener_name} path"
            )


def parse_exact_json_events(
    output: str, expected_names: tuple[str, ...]
) -> tuple[dict[str, object], ...] | None:
    events: list[dict[str, object]] = []
    for line in output.splitlines():
        if not line.strip():
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            return None
        if not isinstance(event, dict):
            return None
        events.append(event)
    if tuple(event.get("event") for event in events) != expected_names:
        return None
    return tuple(events)


PATH_OUTAGE_DATA_FIELDS = {
    "direction",
    "relay_ordinal",
    "sequence",
    "message_number",
    "packet_position",
    "in_order",
    "key_selection",
    "retransmitted",
    "timestamp",
    "destination_socket_id",
    "payload_bytes",
    "ciphertext_sha256",
}


def has_valid_path_outage_data_event(
    event: dict[str, object], destination_socket_id: int
) -> bool:
    message_number = event.get("message_number")
    return (
        set(event) == PATH_OUTAGE_DATA_FIELDS
        and event.get("direction") == "caller_to_listener"
        and type(event.get("relay_ordinal")) is int
        and int(event["relay_ordinal"]) > 0
        and type(event.get("sequence")) is int
        and 0 <= int(event["sequence"]) <= SRT_SEQUENCE_MASK
        and type(message_number) is int
        and 1 <= int(message_number) <= PATH_OUTAGE_MESSAGE_COUNT
        and event.get("packet_position") == 3
        and event.get("in_order") is False
        and event.get("key_selection") == 0
        and type(event.get("retransmitted")) is bool
        and type(event.get("timestamp")) is int
        and 0 <= int(event["timestamp"]) <= 0xFFFF_FFFF
        and event.get("destination_socket_id")
            == destination_socket_id
        and event.get("payload_bytes")
            == (1_316 if int(message_number) % 2 == 0 else 188)
        and type(event.get("ciphertext_sha256")) is str
        and re.fullmatch(r"[0-9a-f]{64}", str(event["ciphertext_sha256"]))
            is not None
    )


def has_valid_path_outage_wire_evidence(
    outage: dict[str, object],
    primary_data: tuple[dict[str, object], ...],
    backup_data: tuple[dict[str, object], ...],
    primary_destination_socket_id: int | None,
    backup_destination_socket_id: int | None,
    *,
    reference_sender: bool,
    reference_sender_variant: str | None = None,
) -> bool:
    expected_outage_fields = {
        "trigger_original_data_count",
        "primary_original_data_count",
        "dropped_primary_datagrams",
        "dropped_primary_data_datagrams",
        "outage_started_monotonic_ns",
        "outage_sequence",
    }
    reference_backup_sequence_collapse = (
        reference_sender
        and reference_sender_variant
            == PATH_OUTAGE_REFERENCE_SCHEDULER_COLLAPSE
        and bool(backup_data)
    )
    if not reference_sender or reference_backup_sequence_collapse:
        expected_outage_fields.update(
            {
                "first_backup_data_monotonic_ns",
                "first_backup_sequence",
                "first_backup_data_delay_microseconds",
            }
        )
    if (
        set(outage) != expected_outage_fields
        or type(primary_destination_socket_id) is not int
        or type(backup_destination_socket_id) is not int
        or primary_destination_socket_id <= 0
        or backup_destination_socket_id <= 0
        or primary_destination_socket_id == backup_destination_socket_id
        or outage.get("trigger_original_data_count")
            != PATH_OUTAGE_TRIGGER_MESSAGE
        or outage.get("primary_original_data_count")
            != PATH_OUTAGE_TRIGGER_MESSAGE
        or type(outage.get("dropped_primary_datagrams")) is not int
        or type(outage.get("dropped_primary_data_datagrams")) is not int
        or int(outage["dropped_primary_data_datagrams"]) < 1
        or int(outage["dropped_primary_datagrams"])
            < int(outage["dropped_primary_data_datagrams"])
        or type(outage.get("outage_started_monotonic_ns")) is not int
        or (
            reference_sender
            and reference_sender_variant
                not in {
                    PATH_OUTAGE_REFERENCE_STALLED_PRIMARY,
                    PATH_OUTAGE_REFERENCE_SCHEDULER_COLLAPSE,
                }
        )
        or (not reference_sender and reference_sender_variant is not None)
    ):
        return False
    if reference_sender and not reference_backup_sequence_collapse:
        if backup_data or any(
            key.startswith("first_backup_") for key in outage
        ):
            return False
    elif (
        type(outage.get("first_backup_data_monotonic_ns")) is not int
        or int(outage["first_backup_data_monotonic_ns"])
            < int(outage["outage_started_monotonic_ns"])
        or type(outage.get("first_backup_data_delay_microseconds"))
            is not int
        or not 0
            <= int(outage["first_backup_data_delay_microseconds"])
            <= 5_000_000
    ):
        return False
    if not primary_data or (
        (not reference_sender or reference_backup_sequence_collapse)
        and not backup_data
    ):
        return False
    if not all(
        has_valid_path_outage_data_event(
            event, primary_destination_socket_id
        )
        for event in primary_data
    ) or (
        (not reference_sender or reference_backup_sequence_collapse)
        and not all(
            has_valid_path_outage_data_event(
                event, backup_destination_socket_id
            )
            for event in backup_data
        )
    ):
        return False
    primary_original = [
        event for event in primary_data
        if event.get("retransmitted") is False
    ]
    backup_original = [
        event for event in backup_data
        if event.get("retransmitted") is False
    ]
    if not primary_original or (
        (not reference_sender or reference_backup_sequence_collapse)
        and not backup_original
    ):
        return False
    primary_numbers = [
        int(event["message_number"]) for event in primary_original
    ]
    backup_numbers = [
        int(event["message_number"]) for event in backup_original
    ]
    if (
        not PATH_OUTAGE_TRIGGER_MESSAGE <= len(primary_original)
            <= PATH_OUTAGE_MESSAGE_COUNT
        or primary_numbers != list(range(1, len(primary_original) + 1))
    ):
        return False
    if reference_sender:
        emitted_messages = len(primary_numbers)
        if (
            emitted_messages
                not in {
                    PATH_OUTAGE_MESSAGE_COUNT - 1,
                    PATH_OUTAGE_MESSAGE_COUNT,
                }
            or outage.get("dropped_primary_data_datagrams")
                != emitted_messages - PATH_OUTAGE_TRIGGER_MESSAGE + 1
        ):
            return False
    elif backup_numbers != list(
        range(PATH_OUTAGE_TRIGGER_MESSAGE, PATH_OUTAGE_MESSAGE_COUNT + 1)
    ):
        return False
    primary_sequences = [int(event["sequence"]) for event in primary_original]
    backup_sequences = [int(event["sequence"]) for event in backup_original]
    if any(
        ((right - left) & SRT_SEQUENCE_MASK) != 1
        for left, right in zip(primary_sequences, primary_sequences[1:])
    ) or any(
        ((right - left) & SRT_SEQUENCE_MASK) != 1
        for left, right in zip(backup_sequences, backup_sequences[1:])
    ):
        return False
    trigger = primary_original[PATH_OUTAGE_TRIGGER_MESSAGE - 1]
    if outage.get("outage_sequence") != trigger.get("sequence"):
        return False
    if not reference_sender and (
        outage.get("first_backup_sequence") != trigger.get("sequence")
        or backup_original[0].get("sequence") != trigger.get("sequence")
        or backup_original[0].get("ciphertext_sha256")
            != trigger.get("ciphertext_sha256")
    ):
        return False
    primary_by_message = {
        int(event["message_number"]): event for event in primary_original
    }
    if reference_backup_sequence_collapse:
        first_backup_sequence = int(backup_original[0]["sequence"])
        first_backup_message = int(backup_original[0]["message_number"])
        matching_primary = primary_by_message.get(first_backup_message)
        sequence_jump = signed_srt_sequence_difference(
            first_backup_sequence, int(trigger["sequence"])
        )
        return (
            all(event.get("retransmitted") is False for event in backup_data)
            and backup_numbers
                == list(range(backup_numbers[0], backup_numbers[-1] + 1))
            and backup_numbers[0] >= PATH_OUTAGE_TRIGGER_MESSAGE
            and outage.get("first_backup_sequence")
                == first_backup_sequence
            and type(sequence_jump) is int
            and sequence_jump > 0
            and matching_primary is not None
            and matching_primary.get("ciphertext_sha256")
                == backup_original[0].get("ciphertext_sha256")
        )
    return reference_sender or all(
        message_number not in primary_by_message
        or (
            primary_by_message[message_number].get("sequence")
                == event.get("sequence")
            and primary_by_message[message_number].get("ciphertext_sha256")
                == event.get("ciphertext_sha256")
        )
        for message_number, event in (
            (int(event["message_number"]), event)
            for event in backup_original
        )
    )


def has_exact_path_outage_sender_events(
    output: str, *, reference_sender: bool
) -> tuple[dict[str, object], dict[str, object]] | None:
    terminal_event = (
        "path_outage_sender_failure"
        if reference_sender
        else "path_outage_sender_complete"
    )
    expected_names = (
        "path_outage_sender_ready",
        "path_outage_sender_released",
        terminal_event,
    )
    if not reference_sender:
        expected_names += ("complete",)
    events = parse_exact_json_events(
        output,
        expected_names,
    )
    if events is None:
        return None
    ready, released, terminal = events[:3]
    terminal_fields = {
        "event",
        "role",
        "sent_messages",
        "first_active_member",
        "final_active_member",
        "active_member_transitions",
        "group_state",
        "primary_state",
        "backup_state",
        "error_code",
        "srt_version",
    }
    if (
        set(ready)
        != {
            "event",
            "role",
            "primary_member",
            "backup_member",
            "srt_version",
        }
        or ready.get("role") != "caller"
        or ready.get("srt_version") != PINNED_SRT_VERSION
        or type(ready.get("primary_member")) is not int
        or type(ready.get("backup_member")) is not int
        or int(ready["primary_member"]) <= 0
        or int(ready["backup_member"]) <= 0
        or ready["primary_member"] == ready["backup_member"]
        or released
            != {"event": "path_outage_sender_released", "role": "caller"}
        or set(terminal) != terminal_fields
        or terminal.get("event") != terminal_event
        or terminal.get("role") != "caller"
        or terminal.get("srt_version") != PINNED_SRT_VERSION
        or terminal.get("sent_messages") != PATH_OUTAGE_MESSAGE_COUNT
        or terminal.get("first_active_member") != ready["primary_member"]
        or type(terminal.get("active_member_transitions")) is not int
        or type(terminal.get("final_active_member")) is not int
        or type(terminal.get("group_state")) is not int
        or type(terminal.get("primary_state")) is not int
        or type(terminal.get("backup_state")) is not int
        or type(terminal.get("error_code")) is not int
    ):
        return None
    if reference_sender:
        if reference_path_outage_sender_variant(ready, terminal) is None:
            return None
        return ready, terminal

    caller_complete = events[3]
    caller_complete_fields = {
        "event",
        "role",
        "group",
        "member",
        "members",
        "policy",
        "callback_calls",
        "callback_valid",
        "payload_valid",
        "late_join",
        "peer_error",
        "peer_error_replacement",
        "peer_error_member_isolated",
        "peer_error_group_update",
        "replacement_member",
        "replacement_distinct",
        "replacement_group_update_absent",
        "replacement_connected",
        "healthy_member",
        "failed_member",
        "group_connected",
        "peer_version",
        "peer_version_valid",
        "tokens",
    }
    if (
        terminal.get("final_active_member") != ready["backup_member"]
        or int(terminal["active_member_transitions"]) < 1
        or terminal.get("error_code") != 0
        or set(caller_complete) != caller_complete_fields
        or caller_complete.get("role") != "caller"
        or type(caller_complete.get("group")) is not int
        or int(caller_complete["group"]) <= 0
        or caller_complete.get("member")
            not in (ready["primary_member"], ready["backup_member"])
        or caller_complete.get("members") != 2
        or caller_complete.get("policy") != "backup"
        or caller_complete.get("callback_calls") != 2
        or caller_complete.get("callback_valid") is not True
        or caller_complete.get("payload_valid") is not True
        or caller_complete.get("late_join") is not False
        or caller_complete.get("peer_error") is not False
        or caller_complete.get("peer_error_replacement") is not False
        or caller_complete.get("peer_error_member_isolated") is not False
        or caller_complete.get("peer_error_group_update") is not False
        or caller_complete.get("replacement_member") != -1
        or caller_complete.get("replacement_distinct") is not False
        or caller_complete.get("replacement_group_update_absent") is not False
        or caller_complete.get("replacement_connected") is not False
        or caller_complete.get("healthy_member") != ready["primary_member"]
        or caller_complete.get("failed_member") != ready["backup_member"]
        or caller_complete.get("group_connected") is not True
        or caller_complete.get("peer_version") != PINNED_SRT_VERSION
        or caller_complete.get("peer_version_valid") is not True
        or caller_complete.get("tokens") != [4_001, 4_002]
    ):
        return None
    return ready, terminal


def reference_path_outage_sender_variant(
    ready: dict[str, object], terminal: dict[str, object]
) -> str | None:
    """Classify exact negative outcomes from the pinned v1.5.5 sender."""

    if (
        terminal.get("final_active_member") == ready.get("primary_member")
        and terminal.get("active_member_transitions") == 0
        and type(terminal.get("error_code")) is int
        and int(terminal["error_code"]) >= 0
    ):
        return PATH_OUTAGE_REFERENCE_STALLED_PRIMARY
    if (
        terminal.get("final_active_member") == -1
        and terminal.get("active_member_transitions") == 1
        and terminal.get("group_state") == 5
        and terminal.get("primary_state") == 5
        and terminal.get("backup_state") == 5
        and terminal.get("error_code") == 0
    ):
        return PATH_OUTAGE_REFERENCE_SCHEDULER_COLLAPSE
    return None


def signed_srt_sequence_difference(left: int, right: int) -> int | None:
    """Return the signed shortest distance in the 31-bit sequence space."""

    if (
        type(left) is not int
        or type(right) is not int
        or not 0 <= left <= SRT_SEQUENCE_MASK
        or not 0 <= right <= SRT_SEQUENCE_MASK
    ):
        return None
    distance = (left - right) & SRT_SEQUENCE_MASK
    if distance >= (SRT_SEQUENCE_MASK + 1) // 2:
        distance -= SRT_SEQUENCE_MASK + 1
    return distance


def has_exact_path_outage_receiver_events(
    output: str, *, reference_receiver: bool
) -> dict[str, object] | None:
    terminal_event = (
        "path_outage_receiver_complete"
        if reference_receiver
        else "path_outage_receiver_failure"
    )
    expected_names = ("ready", "path_outage_receiver_ready", terminal_event)
    if reference_receiver:
        expected_names += ("complete",)
    events = parse_exact_json_events(
        output,
        expected_names,
    )
    if events is None:
        return None
    ready, receiver_ready, terminal = events[:3]
    terminal_fields = {
        "event",
        "role",
        "received_messages",
        "group_state",
        "srt_version",
    }
    if not reference_receiver:
        terminal_fields.add("error_code")
    if (
        ready != {"event": "ready"}
        or receiver_ready
            != {
                "event": "path_outage_receiver_ready",
                "role": "listener",
                "srt_version": PINNED_SRT_VERSION,
            }
        or set(terminal) != terminal_fields
        or terminal.get("event") != terminal_event
        or terminal.get("role") != "listener"
        or terminal.get("srt_version") != PINNED_SRT_VERSION
        or type(terminal.get("received_messages")) is not int
        or type(terminal.get("group_state")) is not int
    ):
        return None
    if not reference_receiver:
        if (
            terminal.get("received_messages")
                != PATH_OUTAGE_TRIGGER_MESSAGE - 1
            or terminal.get("group_state") != 5
            or terminal.get("error_code") != 6_003
        ):
            return None
        return terminal

    listener_complete = events[3]
    listener_complete_fields = {
        "event",
        "role",
        "group",
        "members",
        "listener_update",
        "initial_listener_update",
        "late_listener_update",
        "bonded_listeners",
        "payload_valid",
        "late_join",
        "peer_error",
        "peer_error_replacement",
        "replacement_attached",
        "transient_group_receive_errors",
        "group_connected",
        "payload_hash",
        "peer_version",
        "peer_version_valid",
        "listener_callback_calls",
        "listener_metadata_valid",
    }
    if (
        terminal.get("received_messages") != PATH_OUTAGE_MESSAGE_COUNT
        or set(listener_complete) != listener_complete_fields
        or listener_complete.get("role") != "listener"
        or type(listener_complete.get("group")) is not int
        or int(listener_complete["group"]) <= 0
        or listener_complete.get("members") != 2
        or listener_complete.get("listener_update") is not True
        or type(listener_complete.get("initial_listener_update")) is not bool
        or listener_complete.get("late_listener_update") is not True
        or listener_complete.get("bonded_listeners") != 1
        or listener_complete.get("payload_valid") is not True
        or listener_complete.get("late_join") is not False
        or listener_complete.get("peer_error") is not False
        or listener_complete.get("peer_error_replacement") is not False
        or listener_complete.get("replacement_attached") is not False
        or listener_complete.get("transient_group_receive_errors") != 0
        or listener_complete.get("group_connected") is not True
        or type(listener_complete.get("payload_hash")) is not int
        or int(listener_complete["payload_hash"]) <= 0
        or listener_complete.get("peer_version") != PINNED_SRT_VERSION
        or listener_complete.get("peer_version_valid") is not True
        or listener_complete.get("listener_callback_calls") != 2
        or listener_complete.get("listener_metadata_valid") is not True
    ):
        return None
    return terminal


def has_expected_robotweax_path_outage_receiver_stderr(
    error: str, received_messages: int
) -> bool:
    return error.strip() in (
        "path-outage receive failed after "
        f"{received_messages} messages: Connection was broken",
        "path-outage receive failed after "
        f"{received_messages} messages: "
        "Non-blocking call failure: transmission timed out",
    )


def has_expected_reference_path_outage_listener_stderr(error: str) -> bool:
    lines = [line.strip() for line in error.splitlines() if line.strip()]
    counts = {
        suffix: sum(line.endswith(suffix) for line in lines)
        for suffix in PINNED_OPTION_WARNING_SUFFIXES
    }
    return all(count == 2 for count in counts.values()) and all(
        any(line.endswith(suffix) for suffix in PINNED_OPTION_WARNING_SUFFIXES)
        for line in lines
    )


def has_expected_reference_path_outage_sender_stderr(
    error: str,
    *,
    variant: str,
    primary_member: int,
    backup_member: int,
) -> bool:
    if variant == PATH_OUTAGE_REFERENCE_SCHEDULER_COLLAPSE:
        return has_expected_reference_scheduler_collapse_stderr(
            error,
            primary_member=primary_member,
            backup_member=backup_member,
        )
    if variant != PATH_OUTAGE_REFERENCE_STALLED_PRIMARY:
        return False
    lines = [line.strip() for line in error.splitlines() if line.strip()]
    required = {
        "activation": 0,
        "activated": 0,
        "lookup": 0,
        "ack_range": 0,
        "ttl_skip": 0,
        "all_links_broken": 0,
        "terminal": 0,
    }
    for line in lines:
        if any(
            line.endswith(suffix) for suffix in PINNED_OPTION_WARNING_SUFFIXES
        ):
            continue
        if "grp/sendBackup: trying to activate a stand-by link" in line:
            required["activation"] += 1
        elif "FRESH-ACTIVATED" in line:
            required["activated"] += 1
        elif "CSndBuffer::getMsgNoAt: IPE:" in line:
            required["lookup"] += 1
        elif "ATTACK/IPE: incoming ack seq" in line and (
            "exceeds current" in line
        ):
            required["ack_range"] += 1
        elif "CSndBuffer: skipping packet %0 #0 with TTL=0" in line:
            required["ttl_skip"] += 1
        elif line.endswith("grp/recv: ALL LINKS BROKEN, ABANDONING."):
            required["all_links_broken"] += 1
        elif line == PATH_OUTAGE_REFERENCE_SENDER_ERROR:
            required["terminal"] += 1
        else:
            return False
    return (
        required["activation"] == 1
        and required["activated"] >= 1
        and required["lookup"] >= 1
        and required["ack_range"] >= 1
        and required["all_links_broken"] <= 1
        and required["terminal"] == 1
    )


def has_expected_reference_scheduler_collapse_stderr(
    error: str, *, primary_member: int, backup_member: int
) -> bool:
    """Match the pinned sender's closed sequence-scheduler defect profile."""

    if (
        type(primary_member) is not int
        or type(backup_member) is not int
        or primary_member <= 0
        or backup_member <= 0
        or primary_member == backup_member
    ):
        return False
    activation_available: list[int] = []
    activated_members: list[int] = []
    lookups = 0
    ttl_skips = 0
    override_records: list[tuple[int, int, int]] = []
    scheduled_sequences: list[int] = []
    scheduled_record_positions: list[int] = []
    fixing_records: list[tuple[int, int]] = []
    fixing_record_positions: list[int] = []
    scheduler_record_kinds: list[str] = []
    override_predecessor_kinds: list[str | None] = []
    terminal_count = 0
    terminal_seen = False
    post_terminal_scheduled = False
    last_line_kind: str | None = None
    terminal_predecessor_kind: str | None = None
    for raw_line in error.splitlines():
        line = raw_line.strip()
        if not line:
            continue
        if line == PATH_OUTAGE_REFERENCE_SENDER_ERROR:
            if terminal_seen:
                return False
            terminal_count += 1
            terminal_seen = True
            terminal_predecessor_kind = last_line_kind
            continue
        if any(
            line.endswith(suffix) for suffix in PINNED_OPTION_WARNING_SUFFIXES
        ):
            last_line_kind = "option-warning"
            continue
        record = PINNED_REFERENCE_LOG_PATTERN.fullmatch(line)
        if record is None:
            return False
        message = record.group("message")
        activation = re.fullmatch(
            r"grp/sendBackup: trying to activate a stand-by link "
            r"\((\d+) available\)\. Reason: no stable links",
            message,
        )
        activated = re.fullmatch(r"@(\d+) FRESH-ACTIVATED", message)
        lookup = re.fullmatch(
            r"CSndBuffer::getMsgNoAt: IPE: offset=0 not found, "
            r"max offset=(-?\d+)",
            message,
        )
        override = re.fullmatch(
            r"@(\d+): IPE: Overriding with seq %(\d+) DISCREPANCY "
            r"against current %(\d+) and next sched %(\d+) - diff=(-\d+)",
            message,
        )
        scheduled = re.fullmatch(
            r"@(\d+): IPE: packData: SCHEDULING sequence (\d+) is "
            r"behind of EXTRACTION sequence (\d+), dropping this packet: "
            r"DIFF=(-\d+) STAMP=[0-9A-F]{8}",
            message,
        )
        fixing = re.fullmatch(
            r"grp/sendBackup: @(\d+): IPE: another running link seq "
            r"discrepancy: %(\d+) vs\. previous %(\d+) - fixing",
            message,
        )
        if terminal_seen:
            if post_terminal_scheduled or scheduled is None:
                return False
            member, sequence, extraction, difference = (
                int(value) for value in scheduled.groups()
            )
            if (
                member != backup_member
                or signed_srt_sequence_difference(sequence, extraction)
                    != difference
            ):
                return False
            scheduled_sequences.append(sequence)
            scheduled_record_positions.append(
                len(scheduler_record_kinds)
            )
            scheduler_record_kinds.append("scheduled")
            post_terminal_scheduled = True
            last_line_kind = "scheduled"
            continue
        if activation is not None:
            activation_available.append(int(activation.group(1)))
            last_line_kind = "activation"
        elif activated is not None:
            activated_members.append(int(activated.group(1)))
            last_line_kind = "activated"
        elif lookup is not None:
            if int(lookup.group(1)) > 0:
                return False
            lookups += 1
            last_line_kind = "lookup"
        elif message == "CSndBuffer: skipping packet %0 #0 with TTL=0":
            ttl_skips += 1
            last_line_kind = "ttl-skip"
        elif override is not None:
            member, sequence, current, scheduled_next, difference = (
                int(value) for value in override.groups()
            )
            if (
                member != backup_member
                or signed_srt_sequence_difference(sequence, current)
                    != difference
                or signed_srt_sequence_difference(
                    sequence, scheduled_next
                ) != 2
            ):
                return False
            override_predecessor_kinds.append(last_line_kind)
            override_records.append((sequence, current, scheduled_next))
            scheduler_record_kinds.append("override")
            last_line_kind = "override"
        elif scheduled is not None:
            member, sequence, extraction, difference = (
                int(value) for value in scheduled.groups()
            )
            if (
                member != backup_member
                or signed_srt_sequence_difference(sequence, extraction)
                    != difference
            ):
                return False
            scheduled_sequences.append(sequence)
            scheduled_record_positions.append(
                len(scheduler_record_kinds)
            )
            scheduler_record_kinds.append("scheduled")
            last_line_kind = "scheduled"
        elif fixing is not None:
            member, sequence, previous = (
                int(value) for value in fixing.groups()
            )
            if (
                member != backup_member
                or signed_srt_sequence_difference(sequence, previous) != -2
            ):
                return False
            fixing_records.append((sequence, previous))
            fixing_record_positions.append(
                len(scheduler_record_kinds)
            )
            scheduler_record_kinds.append("fixing")
            last_line_kind = "fixing"
        else:
            return False
    complete_scheduler_trace = (
        len(override_records) == len(scheduled_sequences)
        and len(fixing_records) == len(scheduled_sequences) - 1
    )
    terminal_override_trace = (
        len(scheduled_sequences) >= 1
        and len(override_records) == len(scheduled_sequences) + 1
        and len(fixing_records) == len(scheduled_sequences)
        and override_records[0][2] == scheduled_sequences[0]
        and scheduler_record_kinds[-2:] == ["fixing", "override"]
        and all(
            predecessor == "fixing"
            for predecessor in override_predecessor_kinds[1:]
        )
        and terminal_predecessor_kind == "override"
        and all(
            fixing_sequence == previous_override[2]
            and fixing_previous == previous_override[0]
            for (fixing_sequence, fixing_previous), previous_override in zip(
                fixing_records[1:], override_records[1:-1]
            )
        )
        and all(
            signed_srt_sequence_difference(
                following_override[0], fixing_previous
            )
            == 1
            and signed_srt_sequence_difference(
                following_override[2], fixing_sequence
            )
            == 1
            for (fixing_sequence, fixing_previous), following_override in zip(
                fixing_records, override_records[1:]
            )
        )
    )
    terminal_queue_drain_trace = (
        len(scheduled_sequences) >= 4
        and len(fixing_records) >= 1
        and len(override_records) == len(fixing_records) + 1
        and scheduler_record_kinds[0] == "override"
        and signed_srt_sequence_difference(
            override_records[0][2], scheduled_sequences[0]
        )
        == 0
        and signed_srt_sequence_difference(
            fixing_records[0][0], scheduled_sequences[-1]
        )
        in (0, 1)
        and (
            (
                not post_terminal_scheduled
                and scheduler_record_kinds[-2:] == ["fixing", "override"]
            )
            or (
                post_terminal_scheduled
                and scheduler_record_kinds[-3:]
                    == ["fixing", "override", "scheduled"]
            )
        )
        and all(
            predecessor == "fixing"
            for predecessor in override_predecessor_kinds[1:]
        )
        and terminal_predecessor_kind == "override"
        and all(
            fixing_sequence == previous_override[2]
            and fixing_previous == previous_override[0]
            for (fixing_sequence, fixing_previous), previous_override in zip(
                fixing_records[1:], override_records[1:-1]
            )
        )
        and all(
            signed_srt_sequence_difference(
                following_override[0], fixing_previous
            )
            == 1
            and signed_srt_sequence_difference(
                following_override[2], fixing_sequence
            )
            == 1
            for (fixing_sequence, fixing_previous), following_override in zip(
                fixing_records, override_records[1:]
            )
        )
    )
    interleaved_terminal_queue_tail_trace = (
        len(scheduled_sequences) >= 4
        and len(fixing_records) >= 2
        and len(override_records) == len(fixing_records) + 1
        and len(scheduled_record_positions) == len(scheduled_sequences)
        and len(fixing_record_positions) == len(fixing_records)
        and scheduler_record_kinds[0] == "override"
        and signed_srt_sequence_difference(
            override_records[0][2], scheduled_sequences[0]
        )
        == 0
        # The pinned queue worker can emit exactly one final contiguous
        # scheduled drop after the repair loop has already begun. Bind that
        # late tail explicitly instead of widening the numeric tolerance.
        and signed_srt_sequence_difference(
            fixing_records[0][0], scheduled_sequences[-1]
        )
        == -1
        and signed_srt_sequence_difference(
            fixing_records[0][0], scheduled_sequences[-2]
        )
        == 0
        and fixing_record_positions[0] < scheduled_record_positions[-1]
        and (
            (
                not post_terminal_scheduled
                and scheduler_record_kinds[-2:] == ["fixing", "override"]
            )
            or (
                post_terminal_scheduled
                and scheduler_record_kinds[-3:]
                == ["fixing", "override", "scheduled"]
            )
        )
        and all(
            predecessor == "fixing"
            for predecessor in override_predecessor_kinds[1:]
        )
        and terminal_predecessor_kind == "override"
        and all(
            fixing_sequence == previous_override[2]
            and fixing_previous == previous_override[0]
            for (fixing_sequence, fixing_previous), previous_override in zip(
                fixing_records[1:], override_records[1:-1]
            )
        )
        and all(
            signed_srt_sequence_difference(
                following_override[0], fixing_previous
            )
            == 1
            and signed_srt_sequence_difference(
                following_override[2], fixing_sequence
            )
            == 1
            for (fixing_sequence, fixing_previous), following_override in zip(
                fixing_records, override_records[1:]
            )
        )
    )
    interleaved_two_packet_queue_tail_trace = (
        len(scheduled_sequences) >= 6
        and len(fixing_records) >= 2
        and len(override_records) == len(fixing_records) + 1
        and len(scheduled_record_positions) == len(scheduled_sequences)
        and len(fixing_record_positions) == len(fixing_records)
        and scheduler_record_kinds[0] == "override"
        and signed_srt_sequence_difference(
            override_records[0][2], scheduled_sequences[0]
        )
        == 0
        # The pinned queue worker can publish the repair sequence and
        # exactly two following contiguous drops after the group thread has
        # already begun repairing that sequence. Bind both the sequence
        # depth and the observed interleaving instead of accepting an
        # arbitrary late queue tail.
        and signed_srt_sequence_difference(
            fixing_records[0][0], scheduled_sequences[-3]
        )
        == 0
        and fixing_record_positions[0] < scheduled_record_positions[-3]
        and signed_srt_sequence_difference(
            scheduled_sequences[-1], fixing_records[0][0]
        )
        == 2
        and not post_terminal_scheduled
        and scheduler_record_kinds[-3:]
            == ["scheduled", "fixing", "override"]
        and all(
            predecessor == "fixing"
            for predecessor in override_predecessor_kinds[1:]
        )
        and terminal_predecessor_kind == "override"
        and all(
            fixing_sequence == previous_override[2]
            and fixing_previous == previous_override[0]
            for (fixing_sequence, fixing_previous), previous_override in zip(
                fixing_records[1:], override_records[1:-1]
            )
        )
        and all(
            signed_srt_sequence_difference(
                following_override[0], fixing_previous
            )
            == 1
            and signed_srt_sequence_difference(
                following_override[2], fixing_sequence
            )
            == 1
            for (fixing_sequence, fixing_previous), following_override in zip(
                fixing_records, override_records[1:]
            )
        )
    )
    interleaved_complete_queue_drain_trace = (
        len(scheduled_sequences) >= 4
        and len(fixing_records) >= 2
        and len(override_records) == len(fixing_records) + 1
        and len(scheduled_record_positions) == len(scheduled_sequences)
        and len(fixing_record_positions) == len(fixing_records)
        and scheduler_record_kinds[0] == "override"
        and signed_srt_sequence_difference(
            override_records[0][2], scheduled_sequences[0]
        )
        == 0
        # A pinned queue worker can drain its complete contiguous scheduled
        # prefix while the group thread is already repairing later sequence
        # state. The first repair must begin exactly after that prefix.
        and signed_srt_sequence_difference(
            fixing_records[0][0], scheduled_sequences[-1]
        )
        == 1
        and fixing_record_positions[0] < scheduled_record_positions[-1]
        and not post_terminal_scheduled
        and scheduler_record_kinds[-3:]
            == ["fixing", "override", "scheduled"]
        and all(
            predecessor == "fixing"
            for predecessor in override_predecessor_kinds[1:]
        )
        and terminal_predecessor_kind == "scheduled"
        and all(
            fixing_sequence == previous_override[2]
            and fixing_previous == previous_override[0]
            for (fixing_sequence, fixing_previous), previous_override in zip(
                fixing_records[1:], override_records[1:-1]
            )
        )
        and all(
            signed_srt_sequence_difference(
                following_override[0], fixing_previous
            )
            == 1
            and signed_srt_sequence_difference(
                following_override[2], fixing_sequence
            )
            == 1
            for (fixing_sequence, fixing_previous), following_override in zip(
                fixing_records, override_records[1:]
            )
        )
    )
    return (
        activation_available == [2, 1]
        and activated_members == [primary_member, backup_member]
        and lookups >= 1
        and ttl_skips >= 1
        and len(scheduled_sequences) >= 1
        and all(
            signed_srt_sequence_difference(right, left) == 1
            for left, right in zip(
                scheduled_sequences, scheduled_sequences[1:]
            )
        )
        and (
            complete_scheduler_trace
            or terminal_override_trace
            or terminal_queue_drain_trace
            or interleaved_terminal_queue_tail_trace
            or interleaved_two_packet_queue_tail_trace
            or interleaved_complete_queue_drain_trace
        )
        and terminal_count == 1
    )


def has_expected_reference_listener_path_outage_success(
    caller_output: str,
    caller_error: str,
    listener_output: str,
    listener_error: str,
    caller_returncode: int | None,
    listener_returncode: int | None,
) -> bool:
    return (
        caller_returncode == 0
        and listener_returncode == 0
        and has_exact_path_outage_sender_events(
            caller_output, reference_sender=False
        )
        is not None
        and has_exact_path_outage_receiver_events(
            listener_output, reference_receiver=True
        )
        is not None
        and caller_error.strip() == ""
        and has_expected_reference_path_outage_listener_stderr(
            listener_error
        )
    )


def has_expected_reference_sender_path_outage_failure(
    caller_output: str,
    caller_error: str,
    listener_output: str,
    listener_error: str,
    caller_returncode: int | None,
    listener_returncode: int | None,
) -> bool:
    sender_failure = has_exact_path_outage_sender_events(
        caller_output, reference_sender=True
    )
    receiver_failure = has_exact_path_outage_receiver_events(
        listener_output, reference_receiver=False
    )
    if sender_failure is None or receiver_failure is None:
        return False
    ready, terminal = sender_failure
    variant = reference_path_outage_sender_variant(ready, terminal)
    if variant is None:
        return False
    return (
        caller_returncode == 5
        and listener_returncode == 6
        and has_expected_reference_path_outage_sender_stderr(
            caller_error,
            variant=variant,
            primary_member=int(ready["primary_member"]),
            backup_member=int(ready["backup_member"]),
        )
        and has_expected_robotweax_path_outage_receiver_stderr(
            listener_error, int(receiver_failure["received_messages"])
        )
    )


def run_reference_backup_path_outage_case(
    caller_program: Path,
    listener_program: Path,
    *,
    reference_sender: bool,
) -> None:
    listener_port = free_udp_port()
    with tempfile.TemporaryDirectory(
        prefix="robotweax-group-path-outage-"
    ) as raw:
        directory = Path(raw)
        paths = {
            name: directory / name
            for name in (
                "listener.stdout",
                "listener.stderr",
                "caller.stdout",
                "caller.stderr",
            )
        }
        relay = GroupPathOutageTraceProxy(
            listener_port, PATH_OUTAGE_TRIGGER_MESSAGE
        )
        caller: subprocess.Popen[str] | None = None
        listener: subprocess.Popen[str] | None = None
        endpoints_ready = False
        both_terminal = False
        with (
            paths["listener.stdout"].open("w") as listener_stdout,
            paths["listener.stderr"].open("w") as listener_stderr,
            paths["caller.stdout"].open("w") as caller_stdout,
            paths["caller.stderr"].open("w") as caller_stderr,
        ):
            try:
                listener = subprocess.Popen(
                    [
                        str(listener_program),
                        "listener",
                        "127.0.0.1",
                        str(listener_port),
                        "backup",
                        "path-outage",
                    ],
                    stdin=subprocess.PIPE,
                    stdout=listener_stdout,
                    stderr=listener_stderr,
                    text=True,
                )
                wait_ready(
                    listener,
                    paths["listener.stdout"],
                    paths["listener.stderr"],
                )
                relay.start()
                caller = subprocess.Popen(
                    [
                        str(caller_program),
                        "caller",
                        "127.0.0.1",
                        str(relay.primary_port),
                        "backup",
                        str(relay.backup_port),
                        "path-outage",
                    ],
                    stdin=subprocess.PIPE,
                    stdout=caller_stdout,
                    stderr=caller_stderr,
                    text=True,
                )
                deadline = (
                    time.monotonic() + PATH_OUTAGE_PHASE_TIMEOUT_SECONDS
                )
                while time.monotonic() < deadline:
                    caller_output = paths["caller.stdout"].read_text(
                        errors="replace"
                    )
                    listener_output = paths["listener.stdout"].read_text(
                        errors="replace"
                    )
                    if (
                        has_event(
                            caller_output,
                            "path_outage_sender_ready",
                            "caller",
                        )
                        and has_event(
                            listener_output,
                            "path_outage_receiver_ready",
                            "listener",
                        )
                    ):
                        endpoints_ready = True
                        break
                    if caller.poll() is not None or listener.poll() is not None:
                        break
                    time.sleep(0.01)
                if (
                    endpoints_ready
                    and caller.stdin is not None
                    and caller.poll() is None
                    and listener.poll() is None
                ):
                    caller.stdin.write("send\n")
                    caller.stdin.flush()
                deadline = (
                    time.monotonic() + PATH_OUTAGE_PHASE_TIMEOUT_SECONDS
                )
                endpoints_released = False
                while time.monotonic() < deadline:
                    if (
                        not endpoints_released
                        and listener.poll() is None
                        and has_event(
                            paths["listener.stdout"].read_text(
                                errors="replace"
                            ),
                            "complete",
                            "listener",
                        )
                    ):
                        release_completion_barrier(listener)
                        release_completion_barrier(caller)
                        endpoints_released = True
                    if caller.poll() is not None and listener.poll() is not None:
                        both_terminal = True
                        break
                    time.sleep(0.01)
            finally:
                relay.close()
                if caller is not None:
                    terminate(caller)
                if listener is not None:
                    terminate(listener)

        caller_output = paths["caller.stdout"].read_text(errors="replace")
        caller_error = paths["caller.stderr"].read_text(errors="replace")
        listener_output = paths["listener.stdout"].read_text(
            errors="replace"
        )
        listener_error = paths["listener.stderr"].read_text(
            errors="replace"
        )
        primary_data = relay.primary.data_packet_observations(
            "caller_to_listener"
        )
        backup_data = relay.backup.data_packet_observations(
            "caller_to_listener"
        )
        reference_sender_variant: str | None = None
        if reference_sender:
            reference_sender_events = has_exact_path_outage_sender_events(
                caller_output, reference_sender=True
            )
            if reference_sender_events is not None:
                reference_sender_variant = (
                    reference_path_outage_sender_variant(
                        *reference_sender_events
                    )
                )
        wire_valid = has_valid_path_outage_wire_evidence(
            relay.outage_observation(),
            primary_data,
            backup_data,
            relay.primary.conclusion_socket_id("listener_to_caller"),
            relay.backup.conclusion_socket_id("listener_to_caller"),
            reference_sender=reference_sender,
            reference_sender_variant=reference_sender_variant,
        )
        process_valid = (
            caller is not None
            and listener is not None
            and endpoints_ready
            and both_terminal
            and relay.primary.error() is None
            and relay.backup.error() is None
            and (
                has_expected_reference_sender_path_outage_failure(
                    caller_output,
                    caller_error,
                    listener_output,
                    listener_error,
                    caller.returncode,
                    listener.returncode,
                )
                if reference_sender
                else has_expected_reference_listener_path_outage_success(
                    caller_output,
                    caller_error,
                    listener_output,
                    listener_error,
                    caller.returncode,
                    listener.returncode,
                )
            )
        )
        if not wire_valid or not process_valid:
            raise RuntimeError(
                "pinned Haivision v1.5.5 Backup path-outage classifier "
                f"failed ({'sender' if reference_sender else 'receiver'})\n"
                f"--- caller stdout ---\n{caller_output or '<empty>'}\n"
                f"--- caller stderr ---\n{caller_error or '<empty>'}\n"
                f"--- listener stdout ---\n{listener_output or '<empty>'}\n"
                f"--- listener stderr ---\n{listener_error or '<empty>'}\n"
                "--- relay trace ---\n"
                f"{relay.render('group-reference-path-outage')}\n"
                "--- outage observation ---\n"
                f"{json.dumps(relay.outage_observation(), sort_keys=True)}\n"
            )
        if reference_sender:
            sender_failure = has_exact_path_outage_sender_events(
                caller_output, reference_sender=True
            )
            if sender_failure is None:
                raise RuntimeError(
                    "validated reference sender events disappeared"
                )
            sender_variant = reference_path_outage_sender_variant(
                *sender_failure
            )
            if sender_variant is None:
                raise RuntimeError(
                    "validated reference sender variant disappeared"
                )
            standby_result = (
                "emits only noncontiguous standby DATA"
                if backup_data
                else "emits no standby DATA"
            )
            print(
                "PASS pinned Haivision v1.5.5 Backup sender "
                f"{standby_result} after a causal primary-path outage "
                f"(variant={sender_variant}; version-scoped defect "
                "diagnostic)"
            )
        else:
            print(
                "PASS Robotweax Backup sender transfers the complete "
                "payload through pinned Haivision v1.5.5 after a causal "
                "primary-path outage"
            )


class MatrixEvidence:
    """Opt-in case boundaries without altering order, assertions or retries."""

    def __init__(self, root: Path | None):
        self.root = root
        self.index = 0
        if root is not None:
            root.mkdir(parents=True, exist_ok=False)

    def run(self, function, *args, **kwargs):
        if self.root is None:
            return function(*args, **kwargs)
        self.index += 1
        directory = self.root / f"case-{self.index:03d}"
        directory.mkdir()
        record = {
            "index": self.index, "function": function.__name__,
            "arguments": [str(value) for value in args],
            "options": {key: str(value) for key, value in kwargs.items()},
            "harness_pid": os.getpid(), "started_unix_ns": time.time_ns(),
            "passed": False, "status": "running",
        }
        path = directory / "result.json"
        path.write_text(json.dumps(record, indent=2) + "\n")
        started = time.monotonic()
        if function is run_case:
            kwargs["evidence_directory"] = directory / "peers"
        try:
            result = function(*args, **kwargs)
            record.update(passed=True, status="passed")
            return result
        except Exception as error:
            record.update(status="failed", error=str(error))
            raise
        finally:
            record["finished_unix_ns"] = time.time_ns()
            record["elapsed_seconds"] = time.monotonic() - started
            path.write_text(json.dumps(record, indent=2) + "\n")


def main() -> int:
    global PINNED_SRT_VERSION
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--late-join-only", action="store_true")
    parser.add_argument("--receive-contract-only", action="store_true")
    parser.add_argument(
        "--baseline-only",
        action="store_true",
        help=(
            "run successful admission, payload, receive-contract, "
            "bonding, late-join, and PEERERROR-isolation paths only"
        ),
    )
    parser.add_argument(
        "--expected-reference-version",
        type=parse_srt_version,
        default=compatible_srt_version(),
        help="expected reference srt_version/peer_version (default: public header)",
    )
    parser.add_argument("--evidence-directory", type=Path)
    options = parser.parse_args()
    PINNED_SRT_VERSION = options.expected_reference_version
    robotweax = resolve_program_path(options.robotweax_peer)
    reference = resolve_program_path(options.reference_peer)

    recorder = MatrixEvidence(options.evidence_directory)

    selected_modes = sum(
        (
            options.late_join_only,
            options.receive_contract_only,
            options.baseline_only,
        )
    )
    if selected_modes > 1:
        parser.error(
            "--late-join-only, --receive-contract-only, and "
            "--baseline-only are mutually exclusive"
        )

    if options.receive_contract_only:
        for policy in ("broadcast", "backup"):
            recorder.run(run_case,
                reference,
                robotweax,
                "Robotweax group caller -> Haivision receive contract",
                policy,
                True,
                profile="reference-receive-contract",
            )
            recorder.run(run_case,
                robotweax,
                reference,
                "Haivision group caller -> Robotweax receive contract",
                policy,
                False,
                profile="receive-contract",
            )
        return 0

    if not options.late_join_only:
        for policy in ("broadcast", "backup"):
            recorder.run(run_case,
                reference,
                robotweax,
                "Robotweax group caller -> Haivision mirror group",
                policy,
                True,
            )
            recorder.run(run_case,
                robotweax,
                reference,
                "Haivision group caller -> Robotweax mirror group",
                policy,
                False,
            )
            recorder.run(run_case,
                reference,
                robotweax,
                "Robotweax group caller -> Haivision bonded listeners",
                policy,
                True,
                True,
            )
            recorder.run(run_case,
                robotweax,
                reference,
                "Haivision group caller -> Robotweax bonded listeners",
                policy,
                False,
                True,
            )
        for policy in ("broadcast", "backup"):
            recorder.run(run_case,
                reference,
                robotweax,
                "Robotweax group caller -> Haivision receive contract",
                policy,
                True,
                profile="reference-receive-contract",
            )
            recorder.run(run_case,
                robotweax,
                reference,
                "Haivision group caller -> Robotweax receive contract",
                policy,
                False,
                profile="receive-contract",
            )
    recorder.run(run_case,
        reference,
        robotweax,
        "Robotweax late-join caller -> Haivision mirror group",
        "broadcast",
        True,
        profile="late-join",
    )
    recorder.run(run_case,
        robotweax,
        reference,
        "Haivision late-join caller -> Robotweax mirror group",
        "broadcast",
        False,
        profile="late-join",
        require_repeated_listener_update=True,
    )
    if options.late_join_only:
        return 0
    recorder.run(run_peer_error_case,
        robotweax,
        reference,
        "Robotweax",
        "Haivision",
        reference_listener=True,
    )
    if options.baseline_only:
        return 0
    recorder.run(run_peer_error_case,
        robotweax,
        reference,
        "Robotweax",
        "Haivision",
        replacement=True,
        reference_listener=True,
        expect_pinned_listener_failure=True,
    )
    recorder.run(run_peer_error_case,
        reference,
        robotweax,
        "Haivision",
        "Robotweax",
        replacement=True,
        expect_pinned_caller_failure=True,
    )
    run_reference_backup_path_outage_case(
        robotweax,
        reference,
        reference_sender=False,
    )
    run_reference_backup_path_outage_case(
        reference,
        robotweax,
        reference_sender=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
