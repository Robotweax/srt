#!/usr/bin/env python3
"""Encrypted FileCC Stream interoperability against pinned Haivision SRT."""

from __future__ import annotations

import argparse
import os
import secrets
import subprocess
import sys
import tempfile
import time
from dataclasses import replace
from pathlib import Path

from interop_common import (
    free_udp_port,
    resolve_program_path,
    terminate,
    write_deterministic_payload,
)
from run_file_interop import (
    FILE_PAYLOAD_SIZE,
    PASSPHRASE_ENVIRONMENT,
    RendezvousScenario,
    RunOptions,
    Scenario,
    parse_ready_port,
    peer_command,
    rendezvous_role_probe_scenario,
    render_failure,
    rollover_scenario_matrix as clear_rollover_scenario_matrix,
    run_rendezvous_scenario,
    run_scenario,
)
from srt_handshake_trace import CallerListenerFaultProxy, RendezvousFault


RENDEZVOUS_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND = 500_000
ROLLOVER_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND = 500_000
RENDEZVOUS_ROLE_PROBE_KEY_REFRESH_RATE = 64
RENDEZVOUS_ROLE_PROBE_KEY_PREANNOUNCEMENT = 20
RENDEZVOUS_ROLE_PROBE_MINIMUM_KEY_TRANSITIONS = 1
RENDEZVOUS_ROLE_PROBE_PACKETS = 256
PASSPHRASE_MISMATCH_BYTES = 64 * FILE_PAYLOAD_SIZE + 379
PASSPHRASE_REJECTION_REASON = 10
PASSPHRASE_REJECTION_LINE = (
    "caller setup failed: Connection setup failure: connection rejected; "
    f"reject_reason={PASSPHRASE_REJECTION_REASON}"
)
ENCRYPTED_FILE_PROFILES = (
    "all",
    "base",
    "rendezvous",
    "faults",
    "rollover",
)


def profile_sections(profile: str) -> tuple[str, ...]:
    if profile == "all":
        return ("base", "faults", "rollover", "rendezvous")
    if profile in ENCRYPTED_FILE_PROFILES[1:]:
        return (profile,)
    raise ValueError(f"unknown encrypted FileCC profile: {profile}")


def base_scenario_matrix(
    robotweax: Path,
    reference: Path,
    byte_count: int,
    key_refresh_rate: int,
    key_preannouncement: int,
    minimum_key_transitions: int,
) -> list[Scenario]:
    scenarios: list[Scenario] = []
    directions = (
        (
            "robotweax-to-haivision",
            robotweax,
            reference,
            61_000,
            32_768,
            997,
        ),
        (
            "haivision-to-robotweax",
            reference,
            robotweax,
            62_000,
            4_093,
            701,
        ),
    )
    for key_index, key_length in enumerate((16, 24, 32), start=1):
        for (
            label,
            caller,
            listener,
            seed_base,
            sender_chunk_size,
            receiver_size,
        ) in directions:
            security = {
                "caller": caller,
                "listener": listener,
                "sender_chunk_size": sender_chunk_size,
                "receiver_size": receiver_size,
                "key_length": key_length,
                "key_refresh_rate": key_refresh_rate,
                "key_preannouncement": key_preannouncement,
                "minimum_key_transitions": minimum_key_transitions,
            }
            scenarios.extend(
                (
                    Scenario(
                        name=(
                            f"filecc-aes{key_length * 8}-{label}-"
                            "rotation"
                        ),
                        seed=seed_base + key_index,
                        expect_eof=True,
                        **security,
                    ),
                    Scenario(
                        name=(
                            f"filecc-aes{key_length * 8}-{label}-"
                            "file-api-rotation"
                        ),
                        seed=seed_base + key_index + 10,
                        file_api=True,
                        file_size_to_eof=True,
                        allow_reference_file_api_statistics_lag=(
                            caller == reference
                        ),
                        **security,
                    ),
                )
            )
    return scenarios


def caller_listener_fault_scenario_matrix(
    robotweax: Path,
    reference: Path,
    byte_count: int,
    key_refresh_rate: int,
    key_preannouncement: int,
    minimum_key_transitions: int,
) -> list[Scenario]:
    scenarios: list[Scenario] = []
    directions = (
        (
            "robotweax-to-haivision",
            robotweax,
            reference,
            61_000,
            32_768,
            997,
        ),
        (
            "haivision-to-robotweax",
            reference,
            robotweax,
            62_000,
            4_093,
            701,
        ),
    )
    nak_occurrence = 2 * key_refresh_rate + 17
    rto_occurrence = (
        byte_count + FILE_PAYLOAD_SIZE - 1
    ) // FILE_PAYLOAD_SIZE
    for (
        label,
        caller,
        listener,
        seed_base,
        sender_chunk_size,
        receiver_size,
    ) in directions:
        security = {
            "caller": caller,
            "listener": listener,
            "receiver_size": receiver_size,
            "key_length": 32,
            "key_refresh_rate": key_refresh_rate,
            "key_preannouncement": key_preannouncement,
            "minimum_key_transitions": minimum_key_transitions,
            "expect_eof": True,
        }
        scenarios.extend(
            (
                Scenario(
                    name=f"filecc-aes256-{label}-nak-drop",
                    seed=seed_base + 101,
                    sender_chunk_size=sender_chunk_size,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=nak_occurrence,
                    ),
                    recovery="NAK",
                    **security,
                ),
                Scenario(
                    name=(
                        f"filecc-aes256-{label}-"
                        "rto-flight-tail-drop"
                    ),
                    seed=seed_base + 201,
                    sender_chunk_size=FILE_PAYLOAD_SIZE,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=rto_occurrence,
                    ),
                    recovery="RTO/LATEREXMIT",
                    **security,
                ),
            )
        )
    return scenarios


def scenario_matrix(
    robotweax: Path,
    reference: Path,
    byte_count: int,
    key_refresh_rate: int,
    key_preannouncement: int,
    minimum_key_transitions: int,
) -> list[Scenario]:
    """Return the historical combined Caller/Listener matrix."""

    return base_scenario_matrix(
        robotweax,
        reference,
        byte_count,
        key_refresh_rate,
        key_preannouncement,
        minimum_key_transitions,
    ) + caller_listener_fault_scenario_matrix(
        robotweax,
        reference,
        byte_count,
        key_refresh_rate,
        key_preannouncement,
        minimum_key_transitions,
    )


def passphrase_mismatch_scenario_matrix(
    robotweax: Path,
    reference: Path,
) -> list[Scenario]:
    return [
        Scenario(
            name=f"filecc-aes256-{label}-file-api-wrong-passphrase",
            caller=caller,
            listener=listener,
            seed=seed,
            sender_chunk_size=sender_chunk_size,
            receiver_size=receiver_size,
            byte_count=PASSPHRASE_MISMATCH_BYTES,
            key_length=32,
            key_refresh_rate=64,
            key_preannouncement=20,
            minimum_key_transitions=1,
            file_api=True,
            file_size_to_eof=True,
        )
        for (
            label,
            caller,
            listener,
            seed,
            sender_chunk_size,
            receiver_size,
        ) in (
            (
                "robotweax-to-haivision",
                robotweax,
                reference,
                63_001,
                32_768,
                997,
            ),
            (
                "haivision-to-robotweax",
                reference,
                robotweax,
                63_002,
                4_093,
                701,
            ),
        )
    ]


def validate_passphrase_mismatch(
    scenario: Scenario,
    caller_returncode: int,
    caller_stdout: str,
    caller_stderr: str,
    listener_was_waiting: bool,
    output_exists: bool,
    output_bytes: int,
    relay: CallerListenerFaultProxy,
    caller_is_reference: bool,
) -> None:
    lines = [line for line in caller_stderr.splitlines() if line]
    observation = relay.data_key_observation("sender_to_receiver")
    exact_empty_data_observation = {
        "packets": 0,
        "unencrypted_packets": 0,
        "transitions": 0,
        "selectors": [],
        "destination_socket_ids": [],
        "complete": True,
    }
    if (
        caller_returncode != 3
        or caller_stdout != ""
        or not listener_was_waiting
        or output_exists
        or output_bytes != 0
        or relay.error() is not None
        or observation != exact_empty_data_observation
        or not lines
        or lines[-1] != PASSPHRASE_REJECTION_LINE
        or lines.count(PASSPHRASE_REJECTION_LINE) != 1
        or any("Connection established" in line for line in lines[:-1])
        or (
            caller_is_reference
            and not any("ERROR:BADSECRET" in line for line in lines[:-1])
        )
        or (not caller_is_reference and len(lines) != 1)
    ):
        raise RuntimeError(
            f"{scenario.name}: wrong-passphrase rejection evidence "
            "is incomplete"
        )


def run_passphrase_mismatch_scenario(
    scenario: Scenario,
    reference: Path,
    options: RunOptions,
    directory: Path,
    base_environment: dict[str, str],
) -> None:
    listener_port = free_udp_port()
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    write_deterministic_payload(
        input_path, PASSPHRASE_MISMATCH_BYTES, scenario.seed
    )
    listener_stdout_path = directory / f"{scenario.name}.listener.stdout"
    listener_stderr_path = directory / f"{scenario.name}.listener.stderr"
    listener_environment = base_environment.copy()
    listener_environment[PASSPHRASE_ENVIRONMENT] = (
        "listener-passphrase-0001"
    )
    caller_environment = base_environment.copy()
    caller_environment[PASSPHRASE_ENVIRONMENT] = (
        "caller-passphrase-000002"
    )

    with (
        CallerListenerFaultProxy(listener_port, None) as relay,
        listener_stdout_path.open("w", encoding="utf-8")
        as listener_stdout_stream,
        listener_stderr_path.open("w", encoding="utf-8")
        as listener_stderr_stream,
    ):
        listener = subprocess.Popen(
            peer_command(
                scenario.listener,
                "listener",
                listener_port,
                output_path,
                scenario,
                options,
            ),
            stdout=listener_stdout_stream,
            stderr=listener_stderr_stream,
            text=True,
            env=listener_environment,
        )
        try:
            ready_deadline = time.monotonic() + min(
                5, options.timeout_seconds
            )
            while True:
                listener_stdout_stream.flush()
                listener_output = listener_stdout_path.read_text(
                    encoding="utf-8", errors="replace"
                )
                if parse_ready_port(listener_output) == listener_port:
                    break
                if listener.poll() is not None:
                    raise RuntimeError(
                        f"{scenario.name}: listener exited before "
                        "wrong-passphrase caller started"
                    )
                if time.monotonic() >= ready_deadline:
                    raise RuntimeError(
                        f"{scenario.name}: listener did not report "
                        "readiness"
                    )
                time.sleep(0.01)

            relay.start()
            caller = subprocess.run(
                peer_command(
                    scenario.caller,
                    "caller",
                    relay.port,
                    input_path,
                    scenario,
                    options,
                ),
                capture_output=True,
                text=True,
                env=caller_environment,
                timeout=options.timeout_seconds + 5,
                check=False,
            )
            listener_was_waiting = listener.poll() is None
            relay.close()
            validate_passphrase_mismatch(
                scenario,
                caller.returncode,
                caller.stdout,
                caller.stderr,
                listener_was_waiting,
                output_path.exists(),
                output_path.stat().st_size if output_path.exists() else 0,
                relay,
                scenario.caller == reference,
            )
            print(
                f"PASS {scenario.name} reject_reason="
                f"{PASSPHRASE_REJECTION_REASON} encrypted_data_packets=0",
                flush=True,
            )
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            listener_stdout_stream.flush()
            listener_stderr_stream.flush()
            raise RuntimeError(
                render_failure(
                    scenario,
                    str(error).removeprefix(f"{scenario.name}: "),
                    input_path,
                    output_path,
                    caller_stdout=(
                        caller.stdout if "caller" in locals() else None
                    ),
                    caller_stderr=(
                        caller.stderr if "caller" in locals() else None
                    ),
                    listener_stdout=listener_stdout_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    listener_stderr=listener_stderr_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    relay_trace=relay.render(scenario.name),
                )
            ) from error
        finally:
            if listener.poll() is None:
                terminate(listener)


def rendezvous_scenario_matrix(
    robotweax: Path,
    reference: Path,
    key_refresh_rate: int,
    key_preannouncement: int,
    minimum_key_transitions: int,
    maximum_bandwidth_bytes_per_second: int = (
        RENDEZVOUS_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND
    ),
) -> list[RendezvousScenario]:
    scenarios: list[RendezvousScenario] = []
    byte_count = (
        key_refresh_rate * minimum_key_transitions + 128
    ) * FILE_PAYLOAD_SIZE + 379
    directions = (
        (
            "robotweax-to-haivision",
            robotweax,
            reference,
            63_000,
            32_768,
            997,
        ),
        (
            "haivision-to-robotweax",
            reference,
            robotweax,
            64_000,
            4_093,
            701,
        ),
    )
    for key_index, key_length in enumerate((16, 24, 32), start=1):
        for (
            label,
            sender,
            receiver,
            seed_base,
            sender_chunk_size,
            receiver_size,
        ) in directions:
            scenarios.append(
                RendezvousScenario(
                    name=(
                        f"filecc-rendezvous-aes{key_length * 8}-"
                        f"{label}-rotation"
                    ),
                    sender=sender,
                    receiver=receiver,
                    robotweax_peer=robotweax,
                    seed=seed_base + key_index,
                    sender_chunk_size=sender_chunk_size,
                    receiver_size=receiver_size,
                    byte_count=byte_count,
                    maximum_bandwidth_bytes_per_second=(
                        maximum_bandwidth_bytes_per_second
                    ),
                    key_length=key_length,
                    key_refresh_rate=key_refresh_rate,
                    key_preannouncement=key_preannouncement,
                    minimum_key_transitions=minimum_key_transitions,
                    expect_eof=True,
                )
            )
    return scenarios


def rendezvous_fault_scenario_matrix(
    robotweax: Path,
    reference: Path,
    key_refresh_rate: int,
    key_preannouncement: int,
    minimum_key_transitions: int,
    maximum_bandwidth_bytes_per_second: int = (
        RENDEZVOUS_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND
    ),
) -> list[RendezvousScenario]:
    byte_count = (
        key_refresh_rate * minimum_key_transitions + 128
    ) * FILE_PAYLOAD_SIZE + 379
    nak_occurrence = 2 * key_refresh_rate + 17
    tail_occurrence = (
        byte_count + FILE_PAYLOAD_SIZE - 1
    ) // FILE_PAYLOAD_SIZE
    burst_start = 2 * key_refresh_rate + 31
    burst = tuple(
        RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=burst_start + offset,
        )
        for offset in range(3)
    )
    sustained_start = max(2, key_refresh_rate // 2)
    sustained_interval = max(8, key_refresh_rate // 4)
    sustained = tuple(
        RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=sustained_start + sustained_interval * index,
        )
        for index in range(9)
    )
    directions = (
        (
            "robotweax-to-haivision",
            robotweax,
            reference,
            65_000,
            32_768,
            997,
            "NAK",
        ),
        (
            "haivision-to-robotweax",
            reference,
            robotweax,
            66_000,
            4_093,
            701,
            "CAUSAL",
        ),
    )
    scenarios: list[RendezvousScenario] = []
    for (
        label,
        sender,
        receiver,
        seed_base,
        sender_chunk_size,
        receiver_size,
        middle_recovery,
    ) in directions:
        common = {
            "sender": sender,
            "receiver": receiver,
            "robotweax_peer": robotweax,
            "receiver_size": receiver_size,
            "byte_count": byte_count,
            "maximum_bandwidth_bytes_per_second": (
                maximum_bandwidth_bytes_per_second
            ),
            "key_length": 32,
            "key_refresh_rate": key_refresh_rate,
            "key_preannouncement": key_preannouncement,
            "minimum_key_transitions": minimum_key_transitions,
            "expect_eof": True,
        }
        scenarios.extend(
            (
                RendezvousScenario(
                    name=(
                        "filecc-rendezvous-aes256-"
                        f"{label}-rotation-loss-recovery"
                    ),
                    seed=seed_base + 1,
                    sender_chunk_size=sender_chunk_size,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=nak_occurrence,
                    ),
                    recovery=middle_recovery,
                    **common,
                ),
                RendezvousScenario(
                    name=(
                        "filecc-rendezvous-aes256-"
                        f"{label}-rotation-rto-flight-tail-drop"
                    ),
                    seed=seed_base + 2,
                    sender_chunk_size=FILE_PAYLOAD_SIZE,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=tail_occurrence,
                    ),
                    recovery="RTO/LATEREXMIT",
                    **common,
                ),
                RendezvousScenario(
                    name=(
                        "filecc-rendezvous-aes256-"
                        f"{label}-rotation-burst-loss"
                    ),
                    seed=seed_base + 3,
                    sender_chunk_size=sender_chunk_size,
                    fault=burst,
                    recovery=middle_recovery,
                    minimum_fault_key_selectors=1,
                    **common,
                ),
                RendezvousScenario(
                    name=(
                        "filecc-rendezvous-aes256-"
                        f"{label}-rotation-sustained-loss"
                    ),
                    seed=seed_base + 4,
                    sender_chunk_size=sender_chunk_size,
                    fault=sustained,
                    recovery=middle_recovery,
                    minimum_fault_key_selectors=2,
                    **common,
                ),
            )
        )
    return scenarios


def rollover_scenario_matrix(
    robotweax: Path,
    reference: Path,
    key_refresh_rate: int,
    key_preannouncement: int,
    minimum_key_transitions: int,
) -> list[Scenario]:
    # The clear reference-sender rollover profiles translate sequence numbers
    # in the relay. Encrypted DATA cannot use that shortcut because the wire
    # sequence participates in AES-CTR counter derivation. Keep this gate on
    # the authentic Robotweax-sender wire path until the pinned reference
    # exposes a deterministic initial-sequence control surface.
    return [
        replace(
            scenario,
            name=scenario.name.replace(
                "filecc-rollover-",
                "filecc-aes256-rollover-",
                1,
            ),
            key_length=32,
            key_refresh_rate=key_refresh_rate,
            key_preannouncement=key_preannouncement,
            minimum_key_transitions=minimum_key_transitions,
            maximum_bandwidth_bytes_per_second=(
                ROLLOVER_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND
            ),
            expect_eof=True,
        )
        for scenario in clear_rollover_scenario_matrix(
            robotweax, reference
        )
        if scenario.caller == robotweax
        and scenario.listener == reference
        and scenario.translated_initial_sequence is None
    ]


def rendezvous_role_probe(
    robotweax: Path,
    reference: Path,
    attempt: int,
    start_delay_milliseconds: int,
    maximum_bandwidth_bytes_per_second: int,
) -> RendezvousScenario:
    clear_probe = rendezvous_role_probe_scenario(
        robotweax,
        reference,
        attempt,
        RENDEZVOUS_ROLE_PROBE_PACKETS * FILE_PAYLOAD_SIZE + 379,
        start_delay_milliseconds,
        "filecc-encrypted-rendezvous-cookie-role-probe",
    )
    return replace(
        clear_probe,
        maximum_bandwidth_bytes_per_second=(
            maximum_bandwidth_bytes_per_second
        ),
        key_length=32,
        key_refresh_rate=RENDEZVOUS_ROLE_PROBE_KEY_REFRESH_RATE,
        key_preannouncement=(
            RENDEZVOUS_ROLE_PROBE_KEY_PREANNOUNCEMENT
        ),
        minimum_key_transitions=(
            RENDEZVOUS_ROLE_PROBE_MINIMUM_KEY_TRANSITIONS
        ),
        expect_eof=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument(
        "--profile",
        choices=ENCRYPTED_FILE_PROFILES,
        default="all",
        help=(
            "encrypted FileCC subset to run; 'all' preserves the historical "
            "complete matrix"
        ),
    )
    parser.add_argument(
        "--bytes", type=int, default=4 * 1_024 * 1_024 + 379
    )
    parser.add_argument("--timeout-seconds", type=int, default=75)
    # The fail-closed trace relay decodes every DATA and KM packet. Keep the
    # loss-free baseline below the throughput at which a shared CI runner can
    # overflow its userspace UDP receive queue; controlled impairment belongs
    # in the separate fault matrix.
    parser.add_argument("--max-bw", type=int, default=1_000_000)
    parser.add_argument("--shutdown-grace-ms", type=int, default=250)
    parser.add_argument("--km-refresh-rate", type=int, default=512)
    parser.add_argument("--km-preannounce", type=int, default=200)
    parser.add_argument("--minimum-key-transitions", type=int, default=3)
    parser.add_argument(
        "--rendezvous-max-bw",
        type=int,
        default=RENDEZVOUS_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND,
    )
    parser.add_argument(
        "--rendezvous-role-probe-attempts",
        type=int,
        default=16,
    )
    parser.add_argument(
        "--rendezvous-role-probe-delay-ms",
        type=int,
        default=100,
    )
    arguments = parser.parse_args()
    sections = set(profile_sections(arguments.profile))

    required_packets = (
        arguments.km_refresh_rate * arguments.minimum_key_transitions
    )
    available_packets = (
        arguments.bytes + FILE_PAYLOAD_SIZE - 1
    ) // FILE_PAYLOAD_SIZE
    if (
        arguments.bytes <= 0
        or arguments.timeout_seconds <= 0
        or arguments.max_bw <= 0
        or arguments.shutdown_grace_ms < 0
        or arguments.km_refresh_rate < 2
        or arguments.km_preannounce <= 0
        or arguments.km_preannounce
            > (arguments.km_refresh_rate - 1) // 2
        or arguments.minimum_key_transitions <= 0
        or arguments.rendezvous_max_bw <= 0
        or arguments.rendezvous_role_probe_attempts < 0
        or arguments.rendezvous_role_probe_delay_ms < 0
        or available_packets < required_packets
    ):
        parser.error("invalid encrypted FileCC parameters")

    try:
        robotweax = resolve_program_path(arguments.robotweax_peer)
        reference = resolve_program_path(arguments.reference_peer)
        options = RunOptions(
            byte_count=arguments.bytes,
            timeout_seconds=arguments.timeout_seconds,
            maximum_bandwidth_bytes_per_second=arguments.max_bw,
            shutdown_grace_milliseconds=arguments.shutdown_grace_ms,
        )
        environment = os.environ.copy()
        environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)
        failures: list[str] = []
        with tempfile.TemporaryDirectory(
            prefix="robotweax-srt-encrypted-filecc-"
        ) as directory:
            work = Path(directory)
            robotweax_roles: set[str] = set()
            if "base" in sections:
                for scenario in base_scenario_matrix(
                    robotweax,
                    reference,
                    arguments.bytes,
                    arguments.km_refresh_rate,
                    arguments.km_preannounce,
                    arguments.minimum_key_transitions,
                ):
                    try:
                        run_scenario(
                            scenario,
                            options,
                            work,
                            environment,
                        )
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if "faults" in sections:
                for scenario in caller_listener_fault_scenario_matrix(
                    robotweax,
                    reference,
                    arguments.bytes,
                    arguments.km_refresh_rate,
                    arguments.km_preannounce,
                    arguments.minimum_key_transitions,
                ):
                    try:
                        run_scenario(
                            scenario,
                            options,
                            work,
                            environment,
                        )
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if "base" in sections:
                for scenario in passphrase_mismatch_scenario_matrix(
                    robotweax, reference
                ):
                    try:
                        run_passphrase_mismatch_scenario(
                            scenario,
                            reference,
                            options,
                            work,
                            environment,
                        )
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if "rollover" in sections:
                for scenario in rollover_scenario_matrix(
                    robotweax,
                    reference,
                    arguments.km_refresh_rate,
                    arguments.km_preannounce,
                    arguments.minimum_key_transitions,
                ):
                    try:
                        run_scenario(
                            scenario,
                            options,
                            work,
                            environment,
                        )
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if "rendezvous" in sections:
                for scenario in rendezvous_scenario_matrix(
                    robotweax,
                    reference,
                    arguments.km_refresh_rate,
                    arguments.km_preannounce,
                    arguments.minimum_key_transitions,
                    arguments.rendezvous_max_bw,
                ):
                    try:
                        robotweax_roles.add(
                            run_rendezvous_scenario(
                                scenario,
                                options,
                                work,
                                environment=environment,
                            )
                        )
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if "faults" in sections:
                for scenario in rendezvous_fault_scenario_matrix(
                    robotweax,
                    reference,
                    arguments.km_refresh_rate,
                    arguments.km_preannounce,
                    arguments.minimum_key_transitions,
                    arguments.rendezvous_max_bw,
                ):
                    try:
                        robotweax_roles.add(
                            run_rendezvous_scenario(
                                scenario,
                                options,
                                work,
                                environment=environment,
                            )
                        )
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            required_roles = {"initiator", "responder"}
            if "rendezvous" in sections and not failures:
                for attempt in range(
                    arguments.rendezvous_role_probe_attempts
                ):
                    if robotweax_roles == required_roles:
                        break
                    try:
                        robotweax_roles.add(
                            run_rendezvous_scenario(
                                rendezvous_role_probe(
                                    robotweax,
                                    reference,
                                    attempt,
                                    arguments.rendezvous_role_probe_delay_ms,
                                    arguments.rendezvous_max_bw,
                                ),
                                options,
                                work,
                                environment=environment,
                            )
                        )
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
                        break
            if (
                "rendezvous" in sections
                and not failures
                and robotweax_roles != required_roles
            ):
                missing = sorted(required_roles - robotweax_roles)
                failures.append(
                    "Encrypted FileCC Rendezvous cookie-role coverage "
                    "is incomplete; missing: " + ", ".join(missing)
                )
            elif "rendezvous" in sections and not failures:
                print(
                    "PASS robotweax-encrypted-filecc-rendezvous-"
                    "cookie-role-coverage roles=initiator,responder",
                    flush=True,
                )
        if failures:
            raise RuntimeError(
                "Encrypted FileCC interoperability failures:\n\n"
                + "\n\n".join(failures)
            )
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
