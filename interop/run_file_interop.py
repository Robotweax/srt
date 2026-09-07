#!/usr/bin/env python3
"""FileCC caller/listener interoperability against pinned Haivision SRT."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from contextlib import nullcontext
from dataclasses import dataclass
from pathlib import Path

from interop_common import (
    describe_file_mismatch,
    file_sha256,
    free_udp_port,
    nonnegative_statistic,
    parse_complete,
    reserved_udp_ports,
    resolve_program_path,
    terminate,
    write_deterministic_payload,
)
from reference_version import compatible_srt_version, parse_srt_version
from srt_handshake_trace import (
    CallerListenerFaultProxy,
    CallerListenerSequenceProxy,
    FileRendezvousTraceProxy,
    RendezvousFault,
)


FILE_PAYLOAD_SIZE = 1_456
MINIMUM_RTO_RECOVERY_DELAY_MICROSECONDS = 10_000
MAX_NATIVE_ROLLOVER_INCIDENTAL_RETRANSMISSIONS = 8
MAX_NATIVE_ROLLOVER_STARTUP_DATA_PACKETS = 32
MAX_NATIVE_ROLLOVER_STARTUP_RELAY_ORDINAL = 128
MAX_NATIVE_ROLLOVER_ACK_RACE_RELAY_DATAGRAMS = 8
MINIMUM_NATIVE_ROLLOVER_ACK_RACE_DELAY_MICROSECONDS = 8_000
SEQUENCE_MODULUS = 1 << 31
SEQUENCE_MASK = SEQUENCE_MODULUS - 1
ROLLOVER_MINIMUM_ISN = SEQUENCE_MODULUS - 16_384
ROLLOVER_PACKET_COUNT = 17_500
ROLLOVER_NAK_OCCURRENCE = 17_000
TRANSLATED_ROLLOVER_ISN = SEQUENCE_MODULUS - 64
TRANSLATED_ROLLOVER_PACKET_COUNT = 256
TRANSLATED_ROLLOVER_NAK_OCCURRENCE = 128
TRANSLATED_ROLLOVER_RTO_ISN = SEQUENCE_MASK
TRANSLATED_ROLLOVER_RTO_BYTE_COUNT = FILE_PAYLOAD_SIZE + 997
TRANSLATED_ROLLOVER_RTO_OCCURRENCE = 2
DYNAMIC_MAXBW_FIRST_BYTES_PER_SECOND = 150_000
DYNAMIC_MAXBW_SECOND_BYTES_PER_SECOND = 600_000
DYNAMIC_MAXBW_PHASE_PACKETS = 256
RESILIENCE_TRANSFER_PACKETS = 1_024
ACK_DELAY_MILLISECONDS = 200
RESILIENCE_FLOW_WINDOW_PACKETS = 256
RENDEZVOUS_SINGLE_LOSS_OCCURRENCE = 69
RENDEZVOUS_BURST_LOSS_OCCURRENCES = (133, 134, 135)
PASSPHRASE_ENVIRONMENT = "SRT_INTEROP_PASSPHRASE"
PINNED_REFERENCE_SRT_VERSION = compatible_srt_version()
HAIVISION_1_5_5_VERSION = parse_srt_version("1.5.5")
HAIVISION_REFERENCE_FILE_API_STATISTICS_LAG_VERSIONS = frozenset(
    {
        HAIVISION_1_5_5_VERSION,
        parse_srt_version("1.5.7"),
    }
)
REFERENCE_FILE_API_STATISTICS_LAG_PACKETS = 3
FILE_API_RECEIVER_SHUTDOWN_GRACE_MILLISECONDS = 1_000

FaultPlan = RendezvousFault | tuple[RendezvousFault, ...]


@dataclass(frozen=True)
class Scenario:
    name: str
    caller: Path
    listener: Path
    seed: int
    sender_chunk_size: int
    receiver_size: int
    fault: FaultPlan | None = None
    byte_count: int | None = None
    recovery: str | None = None
    minimum_initial_sequence: int | None = None
    translated_initial_sequence: int | None = None
    maximum_bandwidth_bytes_per_second: int | None = None
    second_maximum_bandwidth_bytes_per_second: int | None = None
    flow_window_packets: int | None = None
    rollover: bool = False
    key_length: int = 0
    key_refresh_rate: int = 0
    key_preannouncement: int = 0
    minimum_key_transitions: int = 0
    expect_eof: bool = False
    file_api: bool = False
    file_size_to_eof: bool = False
    allow_reference_file_api_statistics_lag: bool = False
    crypto_mode: str | None = None
    expected_crypto_mode: str | None = None
    maximum_payload_size: int | None = None
    expected_maximum_payload_size: int | None = None

    def __post_init__(self) -> None:
        if self.allow_reference_file_api_statistics_lag and not (
            self.file_api
            and self.file_size_to_eof
            and self.key_length in (16, 24, 32)
            and self.fault is None
        ):
            raise ValueError(
                "the pinned reference file-API statistics profile "
                "requires an encrypted, loss-free, size-to-EOF helper "
                "scenario"
            )
        if self.crypto_mode not in (None, "auto", "ctr", "gcm"):
            raise ValueError("unsupported crypto mode")
        if self.expected_crypto_mode not in (None, "ctr", "gcm"):
            raise ValueError("unsupported expected crypto mode")
        if (self.crypto_mode is None) != (
            self.expected_crypto_mode is None
        ):
            raise ValueError(
                "scenario crypto mode and expectation must be paired"
            )
        if (self.maximum_payload_size is None) != (
            self.expected_maximum_payload_size is None
        ):
            raise ValueError(
                "scenario payload size and expectation must be paired"
            )


@dataclass(frozen=True)
class RendezvousScenario:
    name: str
    sender: Path
    receiver: Path
    robotweax_peer: Path
    seed: int
    sender_chunk_size: int
    receiver_size: int
    start_order: str = "receiver-first"
    start_delay_milliseconds: int = 0
    byte_count: int | None = None
    fault: FaultPlan | None = None
    recovery: str | None = None
    maximum_bandwidth_bytes_per_second: int | None = None
    second_maximum_bandwidth_bytes_per_second: int | None = None
    flow_window_packets: int | None = None
    expect_eof: bool = False
    rollover: bool = False
    key_length: int = 0
    key_refresh_rate: int = 0
    key_preannouncement: int = 0
    minimum_key_transitions: int = 0
    minimum_fault_key_selectors: int = 0
    crypto_mode: str | None = None
    expected_crypto_mode: str | None = None
    maximum_payload_size: int | None = None
    expected_maximum_payload_size: int | None = None

    def __post_init__(self) -> None:
        if self.crypto_mode not in (None, "auto", "ctr", "gcm"):
            raise ValueError("unsupported crypto mode")
        if self.expected_crypto_mode not in (None, "ctr", "gcm"):
            raise ValueError("unsupported expected crypto mode")
        if (self.crypto_mode is None) != (
            self.expected_crypto_mode is None
        ):
            raise ValueError(
                "scenario crypto mode and expectation must be paired"
            )
        if (self.maximum_payload_size is None) != (
            self.expected_maximum_payload_size is None
        ):
            raise ValueError(
                "scenario payload size and expectation must be paired"
            )


@dataclass(frozen=True)
class RunOptions:
    byte_count: int
    timeout_seconds: int
    maximum_bandwidth_bytes_per_second: int
    shutdown_grace_milliseconds: int
    host: str = "127.0.0.1"


def scenario_matrix(robotweax: Path, reference: Path) -> list[Scenario]:
    scenarios: list[Scenario] = []
    directions = (
        ("robotweax-to-haivision", robotweax, reference, 30_001, 32_768, 997),
        ("haivision-to-robotweax", reference, robotweax, 30_002, 4_093, 701),
    )
    for label, caller, listener, seed, sender_size, receiver_size in directions:
        common = {
            "caller": caller,
            "listener": listener,
            "sender_chunk_size": sender_size,
            "receiver_size": receiver_size,
        }
        scenarios.extend(
            (
                Scenario(
                    name=f"filecc-{label}",
                    seed=seed,
                    **common,
                ),
                Scenario(
                    name=f"filecc-{label}-file-api",
                    seed=seed + 300,
                    file_api=True,
                    file_size_to_eof=True,
                    **common,
                ),
                Scenario(
                    name=f"filecc-{label}-nak-drop",
                    seed=seed + 100,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=17,
                    ),
                    recovery="NAK",
                    **common,
                ),
                Scenario(
                    name=f"filecc-{label}-rto-flight-tail-drop",
                    seed=seed + 200,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=2,
                    ),
                    byte_count=FILE_PAYLOAD_SIZE + 997,
                    recovery="RTO/LATEREXMIT",
                    **common,
                ),
            )
        )
    return scenarios


def rollover_scenario_matrix(
    robotweax: Path, reference: Path
) -> list[Scenario]:
    robotweax_byte_count = (
        ROLLOVER_PACKET_COUNT * FILE_PAYLOAD_SIZE - 379
    )
    translated_byte_count = (
        TRANSLATED_ROLLOVER_PACKET_COUNT * FILE_PAYLOAD_SIZE
        - 379
    )
    return [
        Scenario(
            name="filecc-rollover-robotweax-to-haivision-nak",
            caller=robotweax,
            listener=reference,
            seed=35_001,
            # Keep the long native rollover flight packet-aligned. Large
            # application bursts can overrun the userspace fault relay before
            # its deliberately late post-rollover NAK target is reached.
            sender_chunk_size=FILE_PAYLOAD_SIZE,
            receiver_size=997,
            fault=RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=ROLLOVER_NAK_OCCURRENCE,
            ),
            byte_count=robotweax_byte_count,
            recovery="NAK",
            minimum_initial_sequence=ROLLOVER_MINIMUM_ISN,
            rollover=True,
        ),
        Scenario(
            name="filecc-rollover-robotweax-to-haivision-rto",
            caller=robotweax,
            listener=reference,
            seed=35_002,
            sender_chunk_size=FILE_PAYLOAD_SIZE,
            receiver_size=997,
            fault=RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=ROLLOVER_PACKET_COUNT,
            ),
            byte_count=robotweax_byte_count,
            recovery="RTO/LATEREXMIT",
            minimum_initial_sequence=ROLLOVER_MINIMUM_ISN,
            rollover=True,
        ),
        Scenario(
            name="filecc-rollover-haivision-to-robotweax-nak",
            caller=reference,
            listener=robotweax,
            seed=35_003,
            sender_chunk_size=4_093,
            receiver_size=701,
            fault=RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=TRANSLATED_ROLLOVER_NAK_OCCURRENCE,
            ),
            byte_count=translated_byte_count,
            recovery="NAK",
            translated_initial_sequence=TRANSLATED_ROLLOVER_ISN,
            rollover=True,
        ),
        Scenario(
            name="filecc-rollover-haivision-to-robotweax-rto",
            caller=reference,
            listener=robotweax,
            seed=35_004,
            sender_chunk_size=4_093,
            receiver_size=701,
            fault=RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=TRANSLATED_ROLLOVER_RTO_OCCURRENCE,
            ),
            byte_count=TRANSLATED_ROLLOVER_RTO_BYTE_COUNT,
            recovery="RTO/LATEREXMIT",
            translated_initial_sequence=TRANSLATED_ROLLOVER_RTO_ISN,
            rollover=True,
        ),
    ]


def resilience_scenario_matrix(
    robotweax: Path, reference: Path
) -> list[Scenario]:
    resilience_byte_count = (
        RESILIENCE_TRANSFER_PACKETS * FILE_PAYLOAD_SIZE + 379
    )
    burst = tuple(
        RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=occurrence,
        )
        for occurrence in (64, 65, 66)
    )
    delayed_ack = RendezvousFault(
        action="delay",
        direction="receiver_to_sender",
        occurrence=32,
        packet_kind="control",
        control_type=2,
        delay_milliseconds=ACK_DELAY_MILLISECONDS,
    )
    return [
        Scenario(
            name="filecc-dynamic-maxbw-robotweax-to-haivision",
            caller=robotweax,
            listener=reference,
            seed=36_001,
            sender_chunk_size=FILE_PAYLOAD_SIZE,
            receiver_size=FILE_PAYLOAD_SIZE,
            byte_count=(
                2 * DYNAMIC_MAXBW_PHASE_PACKETS * FILE_PAYLOAD_SIZE
            ),
            maximum_bandwidth_bytes_per_second=(
                DYNAMIC_MAXBW_FIRST_BYTES_PER_SECOND
            ),
            second_maximum_bandwidth_bytes_per_second=(
                DYNAMIC_MAXBW_SECOND_BYTES_PER_SECOND
            ),
        ),
        Scenario(
            name="filecc-burst-loss-robotweax-to-haivision",
            caller=robotweax,
            listener=reference,
            seed=36_002,
            sender_chunk_size=32_768,
            receiver_size=997,
            fault=burst,
            byte_count=resilience_byte_count,
            recovery="NAK",
        ),
        Scenario(
            name="filecc-burst-loss-haivision-to-robotweax",
            caller=reference,
            listener=robotweax,
            seed=36_003,
            sender_chunk_size=4_093,
            receiver_size=701,
            fault=burst,
            byte_count=resilience_byte_count,
            recovery="NAK",
        ),
        Scenario(
            name="filecc-ack-delay-robotweax-to-haivision",
            caller=robotweax,
            listener=reference,
            seed=36_004,
            sender_chunk_size=32_768,
            receiver_size=997,
            fault=delayed_ack,
            byte_count=resilience_byte_count,
            recovery="ACK-DELAY",
            flow_window_packets=RESILIENCE_FLOW_WINDOW_PACKETS,
        ),
        Scenario(
            name="filecc-ack-delay-haivision-to-robotweax",
            caller=reference,
            listener=robotweax,
            seed=36_005,
            sender_chunk_size=4_093,
            receiver_size=701,
            fault=delayed_ack,
            byte_count=resilience_byte_count,
            recovery="ACK-DELAY",
            flow_window_packets=RESILIENCE_FLOW_WINDOW_PACKETS,
        ),
    ]


def rendezvous_scenario_matrix(
    robotweax: Path,
    reference: Path,
) -> list[RendezvousScenario]:
    return [
        RendezvousScenario(
            name="filecc-rendezvous-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            robotweax_peer=robotweax,
            seed=33_001,
            sender_chunk_size=32_768,
            receiver_size=997,
        ),
        RendezvousScenario(
            name="filecc-rendezvous-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            robotweax_peer=robotweax,
            seed=33_002,
            sender_chunk_size=4_093,
            receiver_size=701,
        ),
    ]


def ipv6_rendezvous_scenario_matrix(
    robotweax: Path,
    reference: Path,
) -> list[RendezvousScenario]:
    transfer_bytes = RESILIENCE_TRANSFER_PACKETS * FILE_PAYLOAD_SIZE + 379
    common_fault = RendezvousFault(
        action="drop",
        direction="sender_to_receiver",
        occurrence=RENDEZVOUS_SINGLE_LOSS_OCCURRENCE,
    )
    return [
        RendezvousScenario(
            name="filecc-ipv6-rendezvous-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            robotweax_peer=robotweax,
            seed=38_001,
            sender_chunk_size=32_768,
            receiver_size=997,
        ),
        RendezvousScenario(
            name="filecc-ipv6-rendezvous-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            robotweax_peer=robotweax,
            seed=38_002,
            sender_chunk_size=4_093,
            receiver_size=701,
        ),
        RendezvousScenario(
            name=(
                "filecc-ipv6-rendezvous-nak-drop-"
                "robotweax-to-haivision"
            ),
            sender=robotweax,
            receiver=reference,
            robotweax_peer=robotweax,
            seed=38_003,
            sender_chunk_size=32_768,
            receiver_size=997,
            byte_count=transfer_bytes,
            fault=common_fault,
            recovery="NAK",
        ),
        RendezvousScenario(
            name=(
                "filecc-ipv6-rendezvous-loss-recovery-"
                "haivision-to-robotweax"
            ),
            sender=reference,
            receiver=robotweax,
            robotweax_peer=robotweax,
            seed=38_004,
            sender_chunk_size=4_093,
            receiver_size=701,
            byte_count=transfer_bytes,
            fault=common_fault,
            recovery="CAUSAL",
        ),
    ]


def rendezvous_resilience_scenario_matrix(
    robotweax: Path,
    reference: Path,
) -> list[RendezvousScenario]:
    transfer_bytes = RESILIENCE_TRANSFER_PACKETS * FILE_PAYLOAD_SIZE + 379
    eof_bytes = 256 * FILE_PAYLOAD_SIZE + 379
    burst = tuple(
        RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=occurrence,
        )
        for occurrence in RENDEZVOUS_BURST_LOSS_OCCURRENCES
    )
    directions = (
        ("robotweax-to-haivision", robotweax, reference, 37_010, 32_768, 997),
        ("haivision-to-robotweax", reference, robotweax, 37_020, 4_093, 701),
    )
    scenarios = [
        RendezvousScenario(
            name="filecc-rendezvous-dynamic-maxbw-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            robotweax_peer=robotweax,
            seed=37_001,
            sender_chunk_size=FILE_PAYLOAD_SIZE,
            receiver_size=FILE_PAYLOAD_SIZE,
            byte_count=(
                2 * DYNAMIC_MAXBW_PHASE_PACKETS * FILE_PAYLOAD_SIZE
            ),
            maximum_bandwidth_bytes_per_second=(
                DYNAMIC_MAXBW_FIRST_BYTES_PER_SECOND
            ),
            second_maximum_bandwidth_bytes_per_second=(
                DYNAMIC_MAXBW_SECOND_BYTES_PER_SECOND
            ),
        )
    ]
    for label, sender, receiver, seed, sender_size, receiver_size in directions:
        robotweax_sends = sender == robotweax
        single_name = (
            "nak-drop" if robotweax_sends else "loss-recovery"
        )
        burst_name = (
            "burst-loss"
            if robotweax_sends
            else "repeated-loss-recovery"
        )
        recovery = "NAK" if robotweax_sends else "CAUSAL"
        common = {
            "sender": sender,
            "receiver": receiver,
            "robotweax_peer": robotweax,
            "sender_chunk_size": sender_size,
            "receiver_size": receiver_size,
        }
        scenarios.extend(
            (
                RendezvousScenario(
                    name=f"filecc-rendezvous-{single_name}-{label}",
                    seed=seed,
                    byte_count=transfer_bytes,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=RENDEZVOUS_SINGLE_LOSS_OCCURRENCE,
                    ),
                    recovery=recovery,
                    **common,
                ),
                RendezvousScenario(
                    name=f"filecc-rendezvous-{burst_name}-{label}",
                    seed=seed + 1,
                    byte_count=transfer_bytes,
                    fault=burst,
                    recovery=recovery,
                    **common,
                ),
                RendezvousScenario(
                    name=f"filecc-rendezvous-rto-flight-tail-drop-{label}",
                    seed=seed + 2,
                    byte_count=FILE_PAYLOAD_SIZE + 997,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=2,
                    ),
                    recovery="RTO/LATEREXMIT",
                    **common,
                ),
                RendezvousScenario(
                    name=f"filecc-rendezvous-shutdown-eof-{label}",
                    seed=seed + 3,
                    byte_count=eof_bytes,
                    expect_eof=True,
                    **common,
                ),
            )
        )
    return scenarios


def rendezvous_role_probe_scenario(
    robotweax: Path,
    reference: Path,
    attempt: int,
    byte_count: int,
    start_delay_milliseconds: int,
    name_prefix: str = "filecc-rendezvous-cookie-role-probe",
) -> RendezvousScenario:
    combination = attempt % 4
    robotweax_sends = combination < 2
    return RendezvousScenario(
        name=f"{name_prefix}-{attempt + 1}",
        sender=robotweax if robotweax_sends else reference,
        receiver=reference if robotweax_sends else robotweax,
        robotweax_peer=robotweax,
        seed=34_000 + attempt,
        sender_chunk_size=32_768 if robotweax_sends else 4_093,
        receiver_size=997 if robotweax_sends else 701,
        start_order=(
            "sender-first"
            if combination % 2 == 0
            else "receiver-first"
        ),
        start_delay_milliseconds=start_delay_milliseconds,
        byte_count=byte_count,
    )


def scenario_byte_count(scenario: Scenario, options: RunOptions) -> int:
    return scenario.byte_count or options.byte_count


def append_encrypted_file_options(
    command: list[str],
    scenario: Scenario | RendezvousScenario,
) -> None:
    encrypted = scenario.key_length != 0
    if encrypted != (
        scenario.key_refresh_rate > 0
        and scenario.key_preannouncement > 0
        and scenario.minimum_key_transitions > 0
    ):
        raise ValueError(
            "encrypted FileCC scenarios require complete key-rotation policy"
        )
    if not encrypted:
        return
    if scenario.key_length not in (16, 24, 32):
        raise ValueError("unsupported encrypted FileCC key length")
    if scenario.key_preannouncement > (
        scenario.key_refresh_rate - 1
    ) // 2:
        raise ValueError("invalid encrypted FileCC preannouncement")
    command.extend(
        (
            "--passphrase-env",
            PASSPHRASE_ENVIRONMENT,
            "--pbkeylen",
            str(scenario.key_length),
            "--km-refresh-rate",
            str(scenario.key_refresh_rate),
            "--km-preannounce",
            str(scenario.key_preannouncement),
        )
    )
    if scenario.crypto_mode is not None:
        command.extend(("--crypto-mode", scenario.crypto_mode))
    if scenario.expected_crypto_mode is not None:
        command.extend(
            ("--expect-crypto-mode", scenario.expected_crypto_mode)
        )
    if scenario.maximum_payload_size is not None:
        command.extend(
            ("--payload-size", str(scenario.maximum_payload_size))
        )
    if scenario.expected_maximum_payload_size is not None:
        command.extend(
            (
                "--expect-payload-size",
                str(scenario.expected_maximum_payload_size),
            )
        )


def rendezvous_byte_count(
    scenario: RendezvousScenario,
    options: RunOptions,
) -> int:
    return scenario.byte_count or options.byte_count


def parse_ready_port(output: str) -> int | None:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        port = event.get("port")
        if (
            event.get("event") == "ready"
            and isinstance(port, int)
            and 0 < port <= 65_535
        ):
            return port
    return None


def parse_event(
    output: str, event_name: str
) -> dict[str, object] | None:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == event_name:
            return event
    return None


def peer_command(
    program: Path,
    role: str,
    port: int,
    payload_path: Path,
    scenario: Scenario,
    options: RunOptions,
) -> list[str]:
    if role not in ("caller", "listener"):
        raise ValueError(f"unsupported role: {role}")
    if scenario.file_size_to_eof and not scenario.file_api:
        raise ValueError("size-to-EOF requires the public file API")
    path_option = "--input" if role == "caller" else "--output"
    byte_count = scenario_byte_count(scenario, options)
    maximum_bandwidth = (
        scenario.maximum_bandwidth_bytes_per_second
        if scenario.maximum_bandwidth_bytes_per_second is not None
        else options.maximum_bandwidth_bytes_per_second
    )
    shutdown_grace_milliseconds = options.shutdown_grace_milliseconds
    if role == "listener" and scenario.file_api:
        shutdown_grace_milliseconds = max(
            shutdown_grace_milliseconds,
            FILE_API_RECEIVER_SHUTDOWN_GRACE_MILLISECONDS,
        )
    transfer_block_size = (
        scenario.receiver_size
        if role == "listener" and scenario.file_api
        else scenario.sender_chunk_size
    )
    command = [
        str(program),
        role,
        "--host",
        options.host,
        "--port",
        str(port),
        "--bytes",
        str(byte_count),
        path_option,
        str(payload_path),
        "--transport",
        "file",
        "--congestion",
        "file",
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
        "--chunk-size",
        str(transfer_block_size),
        "--receive-size",
        str(scenario.receiver_size),
        "--max-bw",
        str(maximum_bandwidth),
        "--shutdown-grace-ms",
        str(shutdown_grace_milliseconds),
    ]
    if scenario.file_api:
        command.append("--file-api")
    if role == "caller" and scenario.file_size_to_eof:
        command.append("--file-size-to-eof")
    if (
        role == "caller"
        and scenario.minimum_initial_sequence is not None
    ):
        command.extend(
            (
                "--minimum-isn",
                str(scenario.minimum_initial_sequence),
                "--isn-search-limit",
                "100000",
            )
        )
    if (
        role == "caller"
        and scenario.second_maximum_bandwidth_bytes_per_second
            is not None
    ):
        command.extend(
            (
                "--second-max-bw",
                str(
                    scenario
                    .second_maximum_bandwidth_bytes_per_second
                ),
            )
        )
    if scenario.flow_window_packets is not None:
        command.extend(
            ("--flow-window", str(scenario.flow_window_packets))
        )
    append_encrypted_file_options(command, scenario)
    if role == "listener" and scenario.expect_eof:
        command.append("--expect-eof")
    return command


def rendezvous_peer_command(
    program: Path,
    role: str,
    local_port: int,
    peer_port: int,
    payload_path: Path,
    scenario: RendezvousScenario,
    options: RunOptions,
    host: str = "127.0.0.1",
) -> list[str]:
    if role not in ("rendezvous-sender", "rendezvous-receiver"):
        raise ValueError(f"unsupported rendezvous role: {role}")
    path_option = (
        "--input" if role == "rendezvous-sender" else "--output"
    )
    maximum_bandwidth = (
        scenario.maximum_bandwidth_bytes_per_second
        if scenario.maximum_bandwidth_bytes_per_second is not None
        else options.maximum_bandwidth_bytes_per_second
    )
    command = [
        str(program),
        role,
        "--local-host",
        host,
        "--local-port",
        str(local_port),
        "--host",
        host,
        "--port",
        str(peer_port),
        "--bytes",
        str(rendezvous_byte_count(scenario, options)),
        path_option,
        str(payload_path),
        "--transport",
        "file",
        "--congestion",
        "file",
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
        "--chunk-size",
        str(scenario.sender_chunk_size),
        "--receive-size",
        str(scenario.receiver_size),
        "--max-bw",
        str(maximum_bandwidth),
        "--shutdown-grace-ms",
        str(options.shutdown_grace_milliseconds),
    ]
    if (
        role == "rendezvous-sender"
        and scenario.second_maximum_bandwidth_bytes_per_second is not None
    ):
        command.extend(
            (
                "--second-max-bw",
                str(scenario.second_maximum_bandwidth_bytes_per_second),
            )
        )
    if scenario.flow_window_packets is not None:
        command.extend(
            ("--flow-window", str(scenario.flow_window_packets))
        )
    append_encrypted_file_options(command, scenario)
    if role == "rendezvous-receiver" and scenario.expect_eof:
        command.append("--expect-eof")
    return command


def render_failure(
    scenario: Scenario,
    reason: str,
    input_path: Path,
    output_path: Path,
    caller_stdout: str | bytes | None = None,
    caller_stderr: str | bytes | None = None,
    listener_stdout: str | bytes | None = None,
    listener_stderr: str | bytes | None = None,
    relay_trace: str | None = None,
) -> str:
    partial_bytes = output_path.stat().st_size if output_path.exists() else 0

    def section(label: str, output: str | bytes | None) -> str:
        rendered = (
            output.decode(errors="replace")
            if isinstance(output, bytes)
            else output
        )
        return f"--- {label} ---\n{rendered if rendered else '<empty>'}\n"

    report = (
        f"{scenario.name}: {reason}; "
        f"partial_output_bytes={partial_bytes}\n"
    )
    if input_path.exists() and output_path.exists():
        report += (
            "--- payload diagnostic ---\n"
            + describe_file_mismatch(
                input_path, output_path, FILE_PAYLOAD_SIZE
            )
            + "\n"
        )
    rendered = (
        report
        + section("caller stdout", caller_stdout)
        + section("caller stderr", caller_stderr)
        + section("listener stdout", listener_stdout)
        + section("listener stderr", listener_stderr)
    )
    if relay_trace is not None:
        rendered += section("fault relay trace", relay_trace)
    return rendered


def validate_statistics(
    scenario: Scenario | RendezvousScenario,
    caller_complete: dict[str, object],
    listener_complete: dict[str, object],
    byte_count: int,
    fault_observations: list[dict[str, object]] | None = None,
) -> tuple[int, int]:
    packets_sent, packets_received = validate_loss_free_statistics(
        scenario.name,
        caller_complete,
        listener_complete,
        byte_count,
    )
    dropped_faults = [
        fault
        for fault in scenario_faults(scenario)
        if fault.action == "drop"
    ]
    if dropped_faults:
        retransmissions = nonnegative_statistic(
            caller_complete, "pktRetransTotal"
        )
        if (
            retransmissions is None
            or retransmissions < len(dropped_faults)
        ):
            raise RuntimeError(
                f"{scenario.name}: sender did not report all "
                f"{len(dropped_faults)} retransmissions"
            )
        required_nak_recoveries = nak_recovery_count(
            scenario, fault_observations
        )
        if required_nak_recoveries != 0:
            received_naks = nonnegative_statistic(
                caller_complete, "pktRecvNAKTotal"
            )
            sent_naks = nonnegative_statistic(
                listener_complete, "pktSentNAKTotal"
            )
            reported_losses = nonnegative_statistic(
                caller_complete, "pktSndLossTotal"
            )
            if (
                received_naks is None
                or sent_naks is None
                or reported_losses is None
            ):
                raise RuntimeError(
                    f"{scenario.name}: NAK statistics are unavailable"
                )
            if (
                received_naks < 1
                or sent_naks < 1
                or reported_losses < 1
            ):
                raise RuntimeError(
                    f"{scenario.name}: peers did not report NAK recovery"
                )
    return packets_sent, packets_received


def scenario_faults(
    scenario: Scenario | RendezvousScenario,
) -> tuple[RendezvousFault, ...]:
    if scenario.fault is None:
        return ()
    if isinstance(scenario.fault, tuple):
        return scenario.fault
    return (scenario.fault,)


def validate_encrypted_fault_key_selectors(
    scenario: Scenario | RendezvousScenario,
    fault_observations: list[dict[str, object]],
) -> None:
    """Prove that every encrypted DATA drop has usable key evidence."""
    planned_drops = sum(
        fault.action == "drop" for fault in scenario_faults(scenario)
    )
    selectors = [
        event.get("key_selection")
        for event in fault_observations
        if event.get("action") == "drop"
    ]
    valid_selectors = {
        selector
        for selector in selectors
        if type(selector) is int and selector in (1, 2)
    }
    if (
        len(selectors) != planned_drops
        or any(
            type(selector) is not int or selector not in (1, 2)
            for selector in selectors
        )
        or len(valid_selectors)
            < getattr(scenario, "minimum_fault_key_selectors", 0)
    ):
        raise RuntimeError(
            f"{scenario.name}: encrypted fault key-selector evidence "
            f"is incomplete: {list(map(repr, selectors))}"
        )


def nak_recovery_count(
    scenario: Scenario | RendezvousScenario,
    fault_observations: list[dict[str, object]] | None,
) -> int:
    """Count drops that the receiver had to expose as physical loss.

    Explicit NAK scenarios expose every configured drop. A CAUSAL scenario
    may mix sender-RTO recovery with NAK recovery; only a drop followed by
    later data exposes a receive gap. ``validate_fault_plan`` separately
    proves that every such exposed gap caused a matching NAK before retry.
    """
    dropped_faults = sum(
        fault.action == "drop"
        for fault in scenario_faults(scenario)
    )
    if scenario.recovery == "NAK":
        return dropped_faults
    if scenario.recovery != "CAUSAL" or fault_observations is None:
        return 0
    return sum(
        observation.get(
            "later_data_observed_before_retransmission"
        )
        is True
        for observation in fault_observations
        if observation.get("action", "drop") == "drop"
    )


def validate_loss_free_statistics(
    scenario_name: str,
    sender_complete: dict[str, object],
    receiver_complete: dict[str, object],
    byte_count: int,
) -> tuple[int, int]:
    required_packets = (byte_count + FILE_PAYLOAD_SIZE - 1) // (
        FILE_PAYLOAD_SIZE
    )
    packets_sent = nonnegative_statistic(sender_complete, "pktSentTotal")
    packets_dropped = nonnegative_statistic(
        sender_complete, "pktSndDropTotal"
    )
    packets_received = nonnegative_statistic(
        receiver_complete, "pktRecvTotal"
    )
    if packets_sent is None or packets_dropped is None:
        raise RuntimeError(
            f"{scenario_name}: sender packet statistics are unavailable"
        )
    if packets_received is None:
        raise RuntimeError(
            f"{scenario_name}: receiver packet statistics are unavailable"
        )
    if packets_dropped != 0:
        raise RuntimeError(
            f"{scenario_name}: sender reported "
            f"{packets_dropped} dropped packets"
        )
    if packets_sent < required_packets:
        raise RuntimeError(
            f"{scenario_name}: only {packets_sent} packets were sent; "
            f"at least {required_packets} are required"
        )
    if packets_received < required_packets:
        raise RuntimeError(
            f"{scenario_name}: only {packets_received} packets were "
            f"received; at least {required_packets} are required"
        )
    return packets_sent, packets_received


def validate_wire_statistics(
    scenario: Scenario | RendezvousScenario,
    sender_complete: dict[str, object],
    receiver_complete: dict[str, object],
    byte_count: int,
    fault_observations: list[dict[str, object]] | None = None,
) -> dict[str, int] | None:
    required_packets = (byte_count + FILE_PAYLOAD_SIZE - 1) // (
        FILE_PAYLOAD_SIZE
    )

    def required(
        event: dict[str, object],
        name: str,
        role: str,
    ) -> int:
        value = nonnegative_statistic(event, name)
        if value is None:
            raise RuntimeError(
                f"{scenario.name}: {role} {name} is unavailable"
            )
        return value

    packets_sent = required(
        sender_complete, "pktSentTotal", "sender"
    )
    packets_sent_unique = required(
        sender_complete, "pktSentUniqueTotal", "sender"
    )
    packets_retransmitted = required(
        sender_complete, "pktRetransTotal", "sender"
    )
    bytes_sent = required(
        sender_complete, "byteSentTotal", "sender"
    )
    bytes_sent_unique = required(
        sender_complete, "byteSentUniqueTotal", "sender"
    )
    bytes_retransmitted = required(
        sender_complete, "byteRetransTotal", "sender"
    )
    packets_received = required(
        receiver_complete, "pktRecvTotal", "receiver"
    )
    packets_received_unique = required(
        receiver_complete, "pktRecvUniqueTotal", "receiver"
    )
    bytes_received = required(
        receiver_complete, "byteRecvTotal", "receiver"
    )
    bytes_received_unique = required(
        receiver_complete, "byteRecvUniqueTotal", "receiver"
    )

    allow_reference_file_api_lag = (
        isinstance(scenario, Scenario)
        and scenario.allow_reference_file_api_statistics_lag
    )
    if allow_reference_file_api_lag and (
        sender_complete.get("srt_version")
        not in HAIVISION_REFERENCE_FILE_API_STATISTICS_LAG_VERSIONS
    ):
        raise RuntimeError(
            f"{scenario.name}: reference file-API statistics profile "
            "requires pinned SRT v1.5.5 or v1.5.7 sender evidence"
        )

    if (
        packets_sent_unique < required_packets
        or packets_sent_unique > packets_sent
    ):
        raise RuntimeError(
            f"{scenario.name}: invalid sender unique packet totals "
            f"(sent={packets_sent}, unique={packets_sent_unique}, "
            f"required={required_packets})"
        )
    if packets_sent != packets_sent_unique + packets_retransmitted:
        raise RuntimeError(
            f"{scenario.name}: sender packet total is not "
            "unique plus retransmitted "
            f"({packets_sent} != {packets_sent_unique} + "
            f"{packets_retransmitted})"
        )
    if bytes_sent != bytes_sent_unique + bytes_retransmitted:
        raise RuntimeError(
            f"{scenario.name}: sender byte total is not "
            "unique plus retransmitted "
            f"({bytes_sent} != {bytes_sent_unique} + "
            f"{bytes_retransmitted})"
        )
    minimum_unique_wire_bytes = (
        byte_count + packets_sent_unique * 44
    )
    reference_statistics_lag: dict[str, int] | None = None
    if bytes_sent_unique < minimum_unique_wire_bytes:
        packet_lag = packets_received_unique - packets_sent_unique
        accounted_payload_bytes = bytes_sent_unique - (
            packets_sent_unique * 44
        )
        payload_byte_lag = byte_count - accounted_payload_bytes
        if not (
            allow_reference_file_api_lag
            and 0 <= packet_lag
                <= REFERENCE_FILE_API_STATISTICS_LAG_PACKETS
            and 0 < accounted_payload_bytes <= byte_count
            and 0 < payload_byte_lag
                <= (packet_lag + 1) * FILE_PAYLOAD_SIZE
            and bytes_sent_unique <= bytes_received_unique
        ):
            raise RuntimeError(
                f"{scenario.name}: sender unique wire bytes "
                f"{bytes_sent_unique} are below "
                f"{minimum_unique_wire_bytes}"
            )
        reference_statistics_lag = {
            "packets": packet_lag,
            "payload_bytes": payload_byte_lag,
        }

    if (
        packets_received_unique < required_packets
        or packets_received_unique > packets_received
    ):
        raise RuntimeError(
            f"{scenario.name}: invalid receiver unique packet totals "
            f"(received={packets_received}, "
            f"unique={packets_received_unique}, "
            f"required={required_packets})"
        )
    minimum_received_wire_bytes = (
        byte_count + packets_received_unique * 44
    )
    if (
        bytes_received_unique < minimum_received_wire_bytes
        or bytes_received < bytes_received_unique
    ):
        raise RuntimeError(
            f"{scenario.name}: invalid receiver wire byte totals "
            f"(received={bytes_received}, "
            f"unique={bytes_received_unique}, "
            f"minimum={minimum_received_wire_bytes})"
        )

    dropped_faults = [
        fault
        for fault in scenario_faults(scenario)
        if fault.action == "drop"
    ]
    if not dropped_faults:
        return reference_statistics_lag
    if (
        packets_retransmitted < len(dropped_faults)
        or bytes_retransmitted <= 0
    ):
        raise RuntimeError(
            f"{scenario.name}: dropped packets lack retransmission "
            "packet/byte accounting"
        )

    required_nak_recoveries = nak_recovery_count(
        scenario, fault_observations
    )
    if required_nak_recoveries == 0:
        return reference_statistics_lag
    receiver_loss_packets = required(
        receiver_complete, "pktRcvLossTotal", "receiver"
    )
    receiver_loss_bytes = required(
        receiver_complete, "byteRcvLossTotal", "receiver"
    )
    if (
        receiver_loss_packets < required_nak_recoveries
        or receiver_loss_bytes <= 0
    ):
        raise RuntimeError(
            f"{scenario.name}: NAK recovery lacks receiver loss "
            "packet/byte accounting"
        )
    return reference_statistics_lag


def matching_loss_report_precedes_retransmission(
    observation: dict[str, object],
) -> bool:
    loss_report_ordinal = observation.get(
        "loss_report_relay_ordinal"
    )
    retransmission_ordinal = observation.get(
        "retransmission_relay_ordinal"
    )
    return (
        isinstance(loss_report_ordinal, int)
        and isinstance(retransmission_ordinal, int)
        and loss_report_ordinal < retransmission_ordinal
    )


def validate_fault_plan(
    scenario: Scenario | RendezvousScenario,
    relay: CallerListenerFaultProxy | FileRendezvousTraceProxy,
    byte_count: int,
) -> list[dict[str, object]]:
    if scenario.fault is None:
        raise ValueError("fault validation requires a fault scenario")
    if relay.error() is not None:
        raise RuntimeError(
            f"{scenario.name}: fault relay failed: {relay.error()}"
        )
    faults = scenario_faults(scenario)
    observations = relay.fault_observations()
    if len(observations) != len(faults):
        raise RuntimeError(
            f"{scenario.name}: only {len(observations)} of "
            f"{len(faults)} configured faults were injected"
        )
    for fault, observation in zip(faults, observations):
        if (
            observation.get("action") != fault.action
            or observation.get("direction") != fault.direction
            or observation.get("occurrence") != fault.occurrence
        ):
            raise RuntimeError(
                f"{scenario.name}: fault observation does not "
                "match its plan"
            )
        if fault.action == "delay":
            elapsed = observation.get(
                "delay_elapsed_milliseconds"
            )
            if (
                observation.get("delay_released") is not True
                or not isinstance(elapsed, (int, float))
                or elapsed < fault.delay_milliseconds * 0.9
                or not isinstance(
                    observation.get("delayed_datagrams"), int
                )
                or int(observation["delayed_datagrams"]) < 1
            ):
                raise RuntimeError(
                    f"{scenario.name}: delayed control window "
                    "was not observed completely"
                )
            continue
        if fault.action != "drop":
            continue
        if observation.get("retransmission_observed") is not True:
            raise RuntimeError(
                f"{scenario.name}: dropped packet was not retransmitted"
            )
        if (
            observation.get("retransmission_ciphertext_matches")
                is not True
            or observation.get("retransmission_payload_bytes")
                != observation.get("payload_bytes")
        ):
            raise RuntimeError(
                f"{scenario.name}: retransmission payload does not "
                "match the dropped packet"
            )
        loss_report_preceded_retransmission = (
            matching_loss_report_precedes_retransmission(observation)
        )
        if scenario.recovery == "NAK" and not (
            observation.get("loss_report_observed") is True
            and loss_report_preceded_retransmission
        ):
            raise RuntimeError(
                f"{scenario.name}: matching NAK was not observed "
                "before the retransmission"
            )
        if scenario.recovery == "CAUSAL":
            gap_exposed = observation.get(
                "later_data_observed_before_retransmission"
            )
            recovery_delay = observation.get(
                "retransmission_delay_microseconds"
            )
            if not isinstance(gap_exposed, bool):
                raise RuntimeError(
                    f"{scenario.name}: causal recovery evidence "
                    "is incomplete"
                )
            if gap_exposed:
                drop_ordinal = observation.get("relay_ordinal")
                later_ordinal = observation.get(
                    "first_later_data_relay_ordinal"
                )
                loss_ordinal = observation.get(
                    "loss_report_relay_ordinal"
                )
                retransmission_ordinal = observation.get(
                    "retransmission_relay_ordinal"
                )
                if (
                    observation.get("loss_report_observed") is not True
                    or not loss_report_preceded_retransmission
                    or not all(
                        isinstance(ordinal, int)
                        for ordinal in (
                            drop_ordinal,
                            later_ordinal,
                            loss_ordinal,
                            retransmission_ordinal,
                        )
                    )
                    or not (
                        int(drop_ordinal)
                        < int(later_ordinal)
                        < int(loss_ordinal)
                        < int(retransmission_ordinal)
                    )
                ):
                    raise RuntimeError(
                        f"{scenario.name}: an exposed sequence gap "
                        "did not recover through a matching NAK"
                    )
            elif (
                loss_report_preceded_retransmission
                or not isinstance(recovery_delay, int)
                or recovery_delay
                    < MINIMUM_RTO_RECOVERY_DELAY_MICROSECONDS
            ):
                raise RuntimeError(
                    f"{scenario.name}: an unexposed sequence gap "
                    "did not recover by delayed sender RTO"
                )
        if scenario.recovery == "RTO/LATEREXMIT":
            recovery_delay = observation.get(
                "retransmission_delay_microseconds"
            )
            required_packets = (
                byte_count + FILE_PAYLOAD_SIZE - 1
            ) // FILE_PAYLOAD_SIZE
            expected_tail_bytes = (
                byte_count
                - (required_packets - 1) * FILE_PAYLOAD_SIZE
            )
            targets_long_flight_tail = (
                scenario.sender_chunk_size == FILE_PAYLOAD_SIZE
                and fault.occurrence == required_packets
                and observation.get("payload_bytes")
                    == expected_tail_bytes
            )
            targets_short_flight_tail = (
                fault.occurrence == 2
                and byte_count > FILE_PAYLOAD_SIZE
                and byte_count <= 2 * FILE_PAYLOAD_SIZE
                and observation.get("payload_bytes")
                    == byte_count - FILE_PAYLOAD_SIZE
            )
            targets_flight_tail = (
                targets_long_flight_tail
                or targets_short_flight_tail
            )
            if (
                not targets_flight_tail
                or loss_report_preceded_retransmission
                or not isinstance(recovery_delay, int)
                or recovery_delay
                    < MINIMUM_RTO_RECOVERY_DELAY_MICROSECONDS
            ):
                raise RuntimeError(
                    f"{scenario.name}: tail-drop scenario did not "
                    "target the flight tail and recover by sender RTO"
                )
    return observations


def validate_fault_recovery(
    scenario: Scenario | RendezvousScenario,
    relay: CallerListenerFaultProxy | FileRendezvousTraceProxy,
    byte_count: int,
) -> dict[str, object]:
    observations = validate_fault_plan(scenario, relay, byte_count)
    if len(observations) != 1:
        raise RuntimeError(
            f"{scenario.name}: single-fault validation received "
            f"{len(observations)} observations"
        )
    return observations[0]


def validate_dynamic_maximum_bandwidth(
    scenario: Scenario | RendezvousScenario,
    caller_stdout: str,
) -> tuple[int, int]:
    first_limit = scenario.maximum_bandwidth_bytes_per_second
    second_limit = (
        scenario.second_maximum_bandwidth_bytes_per_second
    )
    if first_limit is None or second_limit is None:
        raise ValueError(
            "dynamic MAXBW validation requires two limits"
        )
    change = parse_event(caller_stdout, "rate_change")
    phases = parse_event(caller_stdout, "rate_phases")
    if change is None or phases is None:
        raise RuntimeError(
            f"{scenario.name}: dynamic MAXBW phase evidence "
            "is unavailable"
        )
    first_bytes = phases.get("first_bytes")
    second_bytes = phases.get("second_bytes")
    first_elapsed = phases.get("first_elapsed_us")
    second_elapsed = phases.get("second_elapsed_us")
    if scenario.byte_count is None:
        raise ValueError(
            "dynamic MAXBW scenario requires an explicit byte count"
        )
    expected_phase_bytes = scenario.byte_count // 2
    if (
        change.get("bytes") != expected_phase_bytes
        or change.get("old_max_bw") != first_limit
        or change.get("new_max_bw") != second_limit
        or first_bytes != expected_phase_bytes
        or second_bytes != expected_phase_bytes
        or phases.get("first_max_bw") != first_limit
        or phases.get("second_max_bw") != second_limit
        or not isinstance(first_elapsed, int)
        or isinstance(first_elapsed, bool)
        or first_elapsed <= 0
        or not isinstance(second_elapsed, int)
        or isinstance(second_elapsed, bool)
        or second_elapsed <= 0
    ):
        raise RuntimeError(
            f"{scenario.name}: dynamic MAXBW phase evidence "
            f"is inconsistent: change={change}, phases={phases}"
        )
    first_rate = first_bytes * 1_000_000 // first_elapsed
    second_rate = second_bytes * 1_000_000 // second_elapsed
    if (
        first_rate < first_limit * 65 // 100
        or first_rate > first_limit * 3 // 2
        or second_rate < second_limit * 65 // 100
        or second_rate > second_limit * 3 // 2
        or second_rate < first_rate * 2
    ):
        raise RuntimeError(
            f"{scenario.name}: dynamic MAXBW rates are outside "
            f"the expected corridors: first={first_rate}, "
            f"second={second_rate}"
        )
    return first_rate, second_rate


def event_initial_sequence(
    scenario_name: str,
    event: dict[str, object],
    role: str,
) -> int:
    initial_sequence = event.get("isn")
    if (
        not isinstance(initial_sequence, int)
        or isinstance(initial_sequence, bool)
        or not 0 <= initial_sequence <= SEQUENCE_MASK
    ):
        raise RuntimeError(
            f"{scenario_name}: {role} SRTO_ISN is unavailable"
        )
    return initial_sequence


def validate_rollover(
    scenario: Scenario,
    relay: CallerListenerFaultProxy,
    caller_complete: dict[str, object],
    listener_complete: dict[str, object],
    byte_count: int,
    fault_observation: dict[str, object],
) -> dict[str, object]:
    if not scenario.rollover:
        raise ValueError("rollover validation requires a rollover scenario")
    caller_isn = event_initial_sequence(
        scenario.name, caller_complete, "caller"
    )
    listener_isn_value = listener_complete.get("isn")
    listener_wire_isn = relay.conclusion_initial_sequence(
        "receiver_to_sender"
    )
    if listener_isn_value is None:
        listener_isn_value = listener_wire_isn
    listener_isn = event_initial_sequence(
        scenario.name, {"isn": listener_isn_value}, "listener"
    )
    if (
        scenario.translated_initial_sequence is None
        and listener_wire_isn is not None
        and listener_wire_isn != listener_isn
    ):
        raise RuntimeError(
            f"{scenario.name}: listener SRTO_ISN conflicts with "
            "HSRSP/CONCLUSION wire evidence"
        )
    required_packets = (
        byte_count + FILE_PAYLOAD_SIZE - 1
    ) // FILE_PAYLOAD_SIZE

    if scenario.translated_initial_sequence is not None:
        if not isinstance(relay, CallerListenerSequenceProxy):
            raise RuntimeError(
                f"{scenario.name}: sequence translation relay is missing"
            )
        sequence = relay.sequence_observation()
        if (
            caller_isn != sequence.get("source_initial_sequence")
            or listener_isn != scenario.translated_initial_sequence
            or sequence.get("target_initial_sequence")
                != scenario.translated_initial_sequence
            or sequence.get("data_wrap_observed") is not True
            or sequence.get("ack_wrap_observed") is not True
            or not isinstance(
                sequence.get("translated_data_packets"), int
            )
            or int(sequence["translated_data_packets"])
                < required_packets + 1
        ):
            raise RuntimeError(
                f"{scenario.name}: translated DATA/ACK rollover "
                f"evidence is incomplete: {sequence}"
            )
        wire_initial_sequence = scenario.translated_initial_sequence
    else:
        minimum = scenario.minimum_initial_sequence
        if (
            minimum is None
            or caller_isn < minimum
            or listener_isn != caller_isn
            or caller_isn + required_packets <= SEQUENCE_MODULUS
        ):
            raise RuntimeError(
                f"{scenario.name}: native sender did not cross the "
                "31-bit sequence boundary"
            )
        wire_initial_sequence = caller_isn

    fault_sequence = fault_observation.get("sequence")
    if (
        not isinstance(fault_sequence, int)
        or isinstance(fault_sequence, bool)
        or fault_sequence >= required_packets
    ):
        raise RuntimeError(
            f"{scenario.name}: injected recovery fault was not "
            "observed after sequence rollover"
        )
    return {
        "caller_isn": caller_isn,
        "listener_isn": listener_isn,
        "wire_isn": wire_initial_sequence,
        "end_sequence": (
            wire_initial_sequence + required_packets
        ) & SEQUENCE_MASK,
        "packets": required_packets,
    }


def trace_events(trace: str) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for line_number, line in enumerate(trace.splitlines(), start=1):
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(
                f"malformed relay trace line {line_number}"
            ) from error
        if not isinstance(event, dict):
            raise ValueError(
                f"non-object relay trace line {line_number}"
            )
        events.append(event)
    return events


def has_exact_reference_rendezvous_initial_recovery(
    scenario: Scenario | RendezvousScenario,
    caller_complete: dict[str, object],
    listener_complete: dict[str, object],
    relay: CallerListenerFaultProxy | FileRendezvousTraceProxy,
    events: list[dict[str, object]],
    expected_retransmissions: int,
    observed_retransmissions: int,
) -> bool:
    if (
        not isinstance(scenario, RendezvousScenario)
        or scenario.sender != scenario.robotweax_peer
        or scenario.receiver == scenario.robotweax_peer
        or scenario.key_length != 32
        or scenario.fault is None
        or caller_complete.get("srt_version")
            != HAIVISION_1_5_5_VERSION
        or listener_complete.get("srt_version")
            != HAIVISION_1_5_5_VERSION
        or observed_retransmissions != expected_retransmissions + 1
        or relay.arq_trace_complete() is not True
    ):
        return False
    role = relay.role_observation()
    if (
        role is None
        or (
            role.get("sender_role"),
            role.get("receiver_role"),
        )
        not in {
            ("initiator", "responder"),
            ("responder", "initiator"),
        }
    ):
        return False
    initial_sequences = [
        event.get("initial_sequence")
        for event in events
        if event.get("event") == "srt_handshake_trace"
        and event.get("direction") == "sender_to_receiver"
        and event.get("request") == 0
    ]
    if (
        len(initial_sequences) != 1
        or type(initial_sequences[0]) is not int
    ):
        return False
    initial_sequence = int(initial_sequences[0])
    fault_identities = {
        (event.get("sequence"), event.get("message_number"))
        for event in relay.fault_observations()
        if event.get("action") == "drop"
    }
    retransmissions = relay.retransmission_observations(
        "sender_to_receiver"
    )
    if len(retransmissions) != observed_retransmissions:
        return False
    extras = [
        event
        for event in retransmissions
        if (event.get("sequence"), event.get("message_number"))
        not in fault_identities
    ]
    if len(extras) != 1:
        return False
    extra = extras[0]
    retransmission_ordinal = extra.get("relay_ordinal")
    if (
        extra.get("sequence") != initial_sequence
        or extra.get("original_observed") is not True
        or extra.get("ciphertext_matches_original") is not True
        or type(extra.get("message_number")) is not int
        or type(extra.get("key_selection")) is not int
        or extra.get("key_selection") not in (1, 2)
        or extra.get("original_key_selection")
            != extra.get("key_selection")
        or type(extra.get("payload_bytes")) is not int
        or int(extra["payload_bytes"]) <= 0
        or extra.get("original_payload_bytes")
            != extra.get("payload_bytes")
        or extra.get("original_payload_sha256")
            != extra.get("payload_sha256")
        or type(extra.get("original_relay_ordinal")) is not int
        or type(retransmission_ordinal) is not int
        or int(extra["original_relay_ordinal"])
            >= int(retransmission_ordinal)
        or any(
            event.get("ciphertext_matches_original") is not True
            for event in retransmissions
        )
    ):
        return False
    matching_reports = [
        event
        for event in relay.loss_report_observations()
        if event.get("direction") == "receiver_to_sender"
        and event.get("ranges") == ((initial_sequence, initial_sequence),)
        and type(event.get("relay_ordinal")) is int
        and int(event["relay_ordinal"]) < int(retransmission_ordinal)
    ]
    return len(matching_reports) == 1


def has_exact_native_rollover_incidental_rtos(
    scenario: Scenario | RendezvousScenario,
    caller_complete: dict[str, object],
    listener_complete: dict[str, object],
    relay: CallerListenerFaultProxy | FileRendezvousTraceProxy,
    events: list[dict[str, object]],
    expected_retransmissions: int,
    observed_retransmissions: int,
) -> bool:
    faults = scenario_faults(scenario)
    # The bounded timer race is independent of whether the deliberately late
    # rollover fault recovers through NAK or sender RTO. The NAK branch below
    # still requires one exact, causal LOSSREPORT for the planned retry.
    expected_fault_occurrence = (
        ROLLOVER_NAK_OCCURRENCE
        if scenario.recovery == "NAK"
        else ROLLOVER_PACKET_COUNT
        if scenario.recovery == "RTO/LATEREXMIT"
        else None
    )
    if (
        not isinstance(scenario, Scenario)
        or scenario.rollover is not True
        or scenario.key_length != 32
        or expected_fault_occurrence is None
        or scenario.minimum_initial_sequence != ROLLOVER_MINIMUM_ISN
        or scenario.translated_initial_sequence is not None
        or len(faults) != 1
        or faults[0].action != "drop"
        or faults[0].direction != "sender_to_receiver"
        or faults[0].occurrence != expected_fault_occurrence
        or caller_complete.get("srt_version")
            != PINNED_REFERENCE_SRT_VERSION
        or listener_complete.get("srt_version")
            != PINNED_REFERENCE_SRT_VERSION
        or not (
            expected_retransmissions + 1
            <= observed_retransmissions
            <= expected_retransmissions
                + MAX_NATIVE_ROLLOVER_INCIDENTAL_RETRANSMISSIONS
        )
        or relay.arq_trace_complete() is not True
    ):
        return False
    initial_sequences = [
        event.get("initial_sequence")
        for event in events
        if event.get("event") == "srt_handshake_trace"
        and event.get("direction") == "sender_to_receiver"
        and event.get("request") == -1
    ]
    if (
        len(initial_sequences) != 1
        or type(initial_sequences[0]) is not int
    ):
        return False
    fault_identities = {
        (event.get("sequence"), event.get("message_number"))
        for event in relay.fault_observations()
        if event.get("action") == "drop"
    }
    retransmissions = relay.retransmission_observations(
        "sender_to_receiver"
    )
    loss_reports = relay.loss_report_observations()
    if (
        len(fault_identities) != expected_retransmissions
        or len(retransmissions) != observed_retransmissions
        or (
            scenario.recovery == "RTO/LATEREXMIT"
            and loss_reports
        )
    ):
        return False
    planned = [
        event
        for event in retransmissions
        if (event.get("sequence"), event.get("message_number"))
        in fault_identities
    ]
    extras = [
        event
        for event in retransmissions
        if (event.get("sequence"), event.get("message_number"))
        not in fault_identities
    ]
    if (
        len(planned) != expected_retransmissions
        or not 1 <= len(extras)
            <= MAX_NATIVE_ROLLOVER_INCIDENTAL_RETRANSMISSIONS
    ):
        return False
    if scenario.recovery == "NAK":
        if len(loss_reports) != len(planned):
            return False
        for retransmission in planned:
            sequence = retransmission.get("sequence")
            original_ordinal = retransmission.get("original_relay_ordinal")
            retransmission_ordinal = retransmission.get("relay_ordinal")
            matching_reports = []
            for report in loss_reports:
                ranges = report.get("ranges")
                report_ordinal = report.get("relay_ordinal")
                exact_range = (
                    isinstance(ranges, (list, tuple))
                    and len(ranges) == 1
                    and isinstance(ranges[0], (list, tuple))
                    and tuple(ranges[0]) == (sequence, sequence)
                )
                if (
                    report.get("direction") == "receiver_to_sender"
                    and exact_range
                    and type(original_ordinal) is int
                    and type(report_ordinal) is int
                    and type(retransmission_ordinal) is int
                    and original_ordinal
                        < report_ordinal
                        < retransmission_ordinal
                ):
                    matching_reports.append(report)
            if len(matching_reports) != 1:
                return False
    initial_sequence = int(initial_sequences[0])
    fault_ordinals = [
        event.get("relay_ordinal")
        for event in relay.fault_observations()
        if event.get("action") == "drop"
    ]
    if (
        any(type(ordinal) is not int for ordinal in fault_ordinals)
        or not fault_ordinals
    ):
        return False
    fault_ordinal = min(int(ordinal) for ordinal in fault_ordinals)
    startup: list[tuple[int, dict[str, object]]] = []
    unacknowledged: dict[
        tuple[int, int], list[tuple[int, dict[str, object]]]
    ] = {}
    identities: set[tuple[int, int]] = set()
    for event in extras:
        sequence = event.get("sequence")
        message_number = event.get("message_number")
        original_ordinal = event.get("original_relay_ordinal")
        retransmission_ordinal = event.get("relay_ordinal")
        delay = event.get("retransmission_delay_microseconds")
        prior_ack_next_sequence = event.get(
            "prior_cumulative_ack_next_sequence"
        )
        prior_ack_ordinal = event.get(
            "prior_cumulative_ack_relay_ordinal"
        )
        if (
            type(sequence) is not int
            or type(message_number) is not int
            or message_number <= 0
            or event.get("original_observed") is not True
            or event.get("ciphertext_matches_original") is not True
            or type(event.get("key_selection")) is not int
            or event.get("key_selection") not in (1, 2)
            or event.get("original_key_selection")
                != event.get("key_selection")
            or event.get("payload_bytes") != FILE_PAYLOAD_SIZE
            or event.get("original_payload_bytes")
                != event.get("payload_bytes")
            or event.get("original_payload_sha256")
                != event.get("payload_sha256")
            or type(original_ordinal) is not int
            or type(retransmission_ordinal) is not int
            or not (
                int(original_ordinal)
                < int(retransmission_ordinal)
                < fault_ordinal
            )
            or type(delay) is not int
            or int(delay) <= 0
        ):
            return False
        identity = (int(sequence), int(message_number))
        if identity in identities:
            return False
        identities.add(identity)

        if (
            prior_ack_next_sequence is None
            and prior_ack_ordinal is None
        ):
            startup_offset = (
                int(sequence) - initial_sequence
            ) & SEQUENCE_MASK
            if (
                startup_offset
                    >= MAX_NATIVE_ROLLOVER_STARTUP_DATA_PACKETS
                or int(retransmission_ordinal)
                    > MAX_NATIVE_ROLLOVER_STARTUP_RELAY_ORDINAL
            ):
                return False
            startup.append((startup_offset, event))
            continue

        if (
            type(prior_ack_next_sequence) is not int
            or type(prior_ack_ordinal) is not int
        ):
            return False
        acknowledgement_distance = (
            int(prior_ack_next_sequence) - int(sequence)
        ) & SEQUENCE_MASK
        if 0 < acknowledgement_distance < SEQUENCE_MODULUS // 2:
            if (
                not (
                    int(original_ordinal)
                    < int(prior_ack_ordinal)
                    < int(retransmission_ordinal)
                )
                or int(retransmission_ordinal)
                    - int(prior_ack_ordinal)
                    > MAX_NATIVE_ROLLOVER_ACK_RACE_RELAY_DATAGRAMS
                or int(delay)
                    < MINIMUM_NATIVE_ROLLOVER_ACK_RACE_DELAY_MICROSECONDS
            ):
                return False
            continue

        flight_offset = (
            int(sequence) - int(prior_ack_next_sequence)
        ) & SEQUENCE_MASK
        if (
            flight_offset
                >= MAX_NATIVE_ROLLOVER_INCIDENTAL_RETRANSMISSIONS
            or int(prior_ack_ordinal) >= int(original_ordinal)
        ):
            return False
        unacknowledged.setdefault(
            (
                int(prior_ack_next_sequence),
                int(prior_ack_ordinal),
            ),
            [],
        ).append((flight_offset, event))

    def contiguous_group(
        group: list[tuple[int, dict[str, object]]],
        *,
        require_zero_offset: bool,
    ) -> bool:
        group.sort(key=lambda item: item[0])
        first_offset = group[0][0]
        first = group[0][1]
        first_message_number = first.get("message_number")
        first_original_ordinal = first.get("original_relay_ordinal")
        first_retransmission_ordinal = first.get("relay_ordinal")
        first_delay = first.get("retransmission_delay_microseconds")
        if (
            (require_zero_offset and first_offset != 0)
            or type(first_message_number) is not int
            or type(first_original_ordinal) is not int
            or type(first_retransmission_ordinal) is not int
            or type(first_delay) is not int
            or int(first_delay)
                < MINIMUM_RTO_RECOVERY_DELAY_MICROSECONDS
        ):
            return False
        return all(
            offset == first_offset + index
            and event.get("message_number")
                == int(first_message_number) + index
            and event.get("original_relay_ordinal")
                == int(first_original_ordinal) + index
            and event.get("relay_ordinal")
                == int(first_retransmission_ordinal) + index
            for index, (offset, event) in enumerate(group)
        )

    if startup and not contiguous_group(
        startup, require_zero_offset=False
    ):
        return False
    if any(
        not contiguous_group(group, require_zero_offset=True)
        for group in unacknowledged.values()
    ):
        return False
    return all(
        event.get("ciphertext_matches_original") is True
        for event in retransmissions
    )


def validate_encrypted_stream(
    scenario: Scenario | RendezvousScenario,
    caller_complete: dict[str, object],
    listener_complete: dict[str, object],
    relay: CallerListenerFaultProxy | FileRendezvousTraceProxy,
    trace: str,
) -> dict[str, int]:
    if scenario.key_length not in (16, 24, 32):
        raise ValueError("encrypted stream validation requires an AES key")
    observation = relay.data_key_observation("sender_to_receiver")
    packets = observation.get("packets")
    transitions = observation.get("transitions")
    selectors = observation.get("selectors")
    socket_ids = observation.get("destination_socket_ids")
    if (
        observation.get("complete") is not True
        or type(packets) is not int
        or packets < scenario.key_refresh_rate
            * scenario.minimum_key_transitions
        or observation.get("unencrypted_packets") != 0
        or type(transitions) is not int
        or transitions < scenario.minimum_key_transitions
        or not isinstance(selectors, list)
        or len(selectors) != transitions + 1
        or any(type(selector) is not int or selector not in (1, 2)
               for selector in selectors)
        or not isinstance(socket_ids, list)
        or len(socket_ids) != 1
        or type(socket_ids[0]) is not int
        or socket_ids[0] <= 0
    ):
        raise RuntimeError(
            f"{scenario.name}: encrypted DATA-key evidence is "
            f"incomplete: {observation}"
        )

    undecryptable = nonnegative_statistic(
        listener_complete, "pktRcvUndecryptTotal"
    )
    if undecryptable != 0:
        raise RuntimeError(
            f"{scenario.name}: receiver reported {undecryptable} "
            "undecryptable packets"
        )

    try:
        events = trace_events(trace)
    except ValueError as error:
        raise RuntimeError(
            f"{scenario.name}: encrypted FileCC trace is malformed"
        ) from error

    uncontrolled = {
        "sender loss": nonnegative_statistic(
            caller_complete, "pktSndLossTotal"
        ),
        "sender retransmission": nonnegative_statistic(
            caller_complete, "pktRetransTotal"
        ),
        "receiver loss": nonnegative_statistic(
            listener_complete, "pktRcvLossTotal"
        ),
    }
    if scenario.fault is None and any(
        value != 0 for value in uncontrolled.values()
    ):
        raise RuntimeError(
            f"{scenario.name}: loss-free encrypted baseline observed "
            f"uncontrolled recovery: {uncontrolled}"
        )
    if scenario.fault is not None:
        fault_observations = relay.fault_observations()
        validate_encrypted_fault_key_selectors(
            scenario, fault_observations
        )
        expected_retransmissions = sum(
            fault.action == "drop"
            for fault in scenario_faults(scenario)
        )
        if (
            uncontrolled["sender retransmission"]
                != expected_retransmissions
            and not has_exact_reference_rendezvous_initial_recovery(
                scenario,
                caller_complete,
                listener_complete,
                relay,
                events,
                expected_retransmissions,
                uncontrolled["sender retransmission"],
            )
            and not has_exact_native_rollover_incidental_rtos(
                scenario,
                caller_complete,
                listener_complete,
                relay,
                events,
                expected_retransmissions,
                uncontrolled["sender retransmission"],
            )
        ):
            raise RuntimeError(
                f"{scenario.name}: encrypted fault profile observed "
                "unplanned retransmissions: "
                f"{uncontrolled['sender retransmission']} != "
                f"{expected_retransmissions}"
            )

    if any(
        event.get("event") in {
            "srt_handshake_trace_error",
            "srt_runtime_key_material_trace_truncated",
            "srt_arq_trace_truncated",
        }
        for event in events
    ):
        raise RuntimeError(
            f"{scenario.name}: encrypted FileCC trace is incomplete"
        )
    material_events = [
        event
        for event in events
        if event.get("event") == "srt_runtime_key_material_trace"
    ]
    requests: list[tuple[int, tuple[int, str]]] = []
    responses: list[tuple[int, tuple[int, str]]] = []
    reverse_requests: list[tuple[int, tuple[int, str]]] = []
    reverse_responses: list[tuple[int, tuple[int, str]]] = []
    for index, event in enumerate(material_events):
        name = event.get("name")
        direction = event.get("direction")
        material = event.get("key_material")
        key_words = (
            material.get("key_words")
            if isinstance(material, dict)
            else None
        )
        key_selection = (
            material.get("key_selection")
            if isinstance(material, dict)
            else None
        )
        digest = (
            material.get("content_sha256")
            if isinstance(material, dict)
            else None
        )
        if (
            not isinstance(material, dict)
            or material.get("content_length_matches") is not True
            or type(key_words) is not int
            or key_words != scenario.key_length // 4
            or type(key_selection) is not int
            or key_selection not in (1, 2, 3)
            or not isinstance(digest, str)
            or len(digest) != 64
            or any(character not in "0123456789abcdef"
                   for character in digest)
        ):
            raise RuntimeError(
                f"{scenario.name}: malformed runtime key material"
            )
        identity = (
            key_selection,
            digest,
        )
        if name == "KMREQ" and direction == "sender_to_receiver":
            requests.append((index, identity))
        elif name == "KMRSP" and direction == "receiver_to_sender":
            responses.append((index, identity))
        elif name == "KMREQ" and direction == "receiver_to_sender":
            reverse_requests.append((index, identity))
        elif name == "KMRSP" and direction == "sender_to_receiver":
            reverse_responses.append((index, identity))
        else:
            raise RuntimeError(
                f"{scenario.name}: unexpected runtime key-material direction"
            )
    # A bidirectional endpoint can confirm one independent initial reverse
    # key even when this fixture sends DATA only forward. Validate that
    # exchange separately: it cannot satisfy forward rotation coverage.
    reverse_material = {identity for _, identity in reverse_requests}
    if (
        len(reverse_material) > 1
        or any(identity[0] != 1 for identity in reverse_material)
        or reverse_material.intersection(identity for _, identity in requests)
        or any(
            not any(response_index > request_index and response == identity
                    for response_index, response in reverse_responses)
            for request_index, identity in reverse_requests
        )
        or any(
            not any(request_index < response_index and request == identity
                    for request_index, request in reverse_requests)
            for response_index, identity in reverse_responses
        )
    ):
        raise RuntimeError(
            f"{scenario.name}: invalid initial reverse key-material exchange"
        )
    acknowledged_material = {
        identity
        for request_index, identity in requests
        if any(
            response_index > request_index
            and response_identity == identity
            for response_index, response_identity in responses
        )
    }
    if (
        len(acknowledged_material) < scenario.minimum_key_transitions
        or any(
            not any(
                response_index > request_index
                and response_identity == identity
                for response_index, response_identity in responses
            )
            for request_index, identity in requests
        )
        or any(
            not any(
                request_index < response_index
                and request_identity == identity
                for request_index, request_identity in requests
            )
            for response_index, identity in responses
        )
    ):
        raise RuntimeError(
            f"{scenario.name}: observed only "
            f"{len(acknowledged_material)} acknowledged key-material "
            "exchanges"
        )
    return {
        "packets": packets,
        "transitions": transitions,
        "acknowledged_key_material": len(acknowledged_material),
    }


def validate_stream_eof(
    scenario: Scenario,
    listener_stdout: str,
    byte_count: int,
) -> None:
    if not scenario.expect_eof:
        return
    try:
        parsed_events = trace_events(listener_stdout)
    except ValueError as error:
        raise RuntimeError(
            f"{scenario.name}: File Stream EOF output is malformed"
        ) from error
    events = [
        event
        for event in parsed_events
        if event.get("event") == "eof"
    ]
    if (
        len(events) != 1
        or (event := events[0]).get("event") != "eof"
        or set(event) != {"event", "role", "bytes"}
        or event.get("role") != "listener"
        or event.get("bytes") != byte_count
    ):
        raise RuntimeError(
            f"{scenario.name}: exact zero-byte File Stream EOF evidence "
            "is unavailable"
        )


def run_scenario(
    scenario: Scenario,
    options: RunOptions,
    directory: Path,
    environment: dict[str, str] | None = None,
) -> None:
    listener_port = free_udp_port(options.host)
    byte_count = scenario_byte_count(scenario, options)
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    expected_digest = write_deterministic_payload(
        input_path, byte_count, scenario.seed
    )
    listener_stdout_path = directory / f"{scenario.name}.listener.stdout"
    listener_stderr_path = directory / f"{scenario.name}.listener.stderr"
    relay_context = (
        CallerListenerSequenceProxy(
            listener_port,
            scenario.translated_initial_sequence,
            scenario.fault,
        )
        if scenario.translated_initial_sequence is not None
        else CallerListenerFaultProxy(
            listener_port, scenario.fault, host=options.host
        )
        if scenario.fault is not None or scenario.key_length != 0
        else nullcontext()
    )

    with (
        relay_context as relay,
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
            env=environment,
        )

        def listener_output() -> tuple[str, str]:
            listener_stdout_stream.flush()
            listener_stderr_stream.flush()
            return (
                listener_stdout_path.read_text(
                    encoding="utf-8", errors="replace"
                ),
                listener_stderr_path.read_text(
                    encoding="utf-8", errors="replace"
                ),
            )

        try:
            ready_deadline = time.monotonic() + min(
                5, options.timeout_seconds
            )
            while True:
                listener_stdout, listener_stderr = listener_output()
                ready_port = parse_ready_port(listener_stdout)
                if ready_port == listener_port:
                    break
                if listener.poll() is not None:
                    raise RuntimeError(
                        render_failure(
                            scenario,
                            "listener exited before the caller started",
                            input_path,
                            output_path,
                            listener_stdout=listener_stdout,
                            listener_stderr=listener_stderr,
                        )
                    )
                if time.monotonic() >= ready_deadline:
                    raise RuntimeError(
                        render_failure(
                            scenario,
                            "listener did not report readiness",
                            input_path,
                            output_path,
                            listener_stdout=listener_stdout,
                            listener_stderr=listener_stderr,
                        )
                    )
                time.sleep(0.01)
            if listener.poll() is not None:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "listener exited before the caller started",
                        input_path,
                        output_path,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                    )
                )
            if relay is not None:
                relay.start()
            caller_port = (
                relay.port if relay is not None else listener_port
            )
            try:
                caller = subprocess.run(
                    peer_command(
                        scenario.caller,
                        "caller",
                        caller_port,
                        input_path,
                        scenario,
                        options,
                    ),
                    capture_output=True,
                    text=True,
                    env=environment,
                    timeout=options.timeout_seconds + 5,
                    check=False,
                )
            except subprocess.TimeoutExpired as error:
                terminate(listener)
                listener_stdout, listener_stderr = listener_output()
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "caller timed out",
                        input_path,
                        output_path,
                        caller_stdout=error.stdout,
                        caller_stderr=error.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        relay_trace=(
                            relay.render(scenario.name)
                            if relay is not None
                            else None
                        ),
                    )
                ) from error
            try:
                listener.wait(timeout=options.timeout_seconds + 5)
                listener_stdout, listener_stderr = listener_output()
            except subprocess.TimeoutExpired as error:
                terminate(listener)
                listener_stdout, listener_stderr = listener_output()
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "listener timed out",
                        input_path,
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        relay_trace=(
                            relay.render(scenario.name)
                            if relay is not None
                            else None
                        ),
                    )
                ) from error
            if scenario.key_length != 0 and relay is not None:
                # Both processes are terminal. Stop the relay before taking
                # the combined DATA/KM snapshot so no late control packet can
                # make the evidence internally inconsistent.
                relay.close()
            if caller.returncode != 0 or listener.returncode != 0:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "file transfer failed "
                        f"(caller={caller.returncode}, "
                        f"listener={listener.returncode})",
                        input_path,
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        relay_trace=(
                            relay.render(scenario.name)
                            if relay is not None
                            else None
                        ),
                    )
                )

            caller_complete = parse_complete(caller.stdout, "caller")
            listener_complete = parse_complete(
                listener_stdout, "listener"
            )
            if (
                caller_complete.get("bytes") != byte_count
                or listener_complete.get("bytes") != byte_count
            ):
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "peer byte counters do not match",
                        input_path,
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        relay_trace=(
                            relay.render(scenario.name)
                            if relay is not None
                            else None
                        ),
                    )
                )
            received_digest = file_sha256(output_path)
            if received_digest != expected_digest:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "SHA-256 mismatch "
                        f"{received_digest} != {expected_digest}",
                        input_path,
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        relay_trace=(
                            relay.render(scenario.name)
                            if relay is not None
                            else None
                        ),
                    )
                )
            fault_summary = ""
            dynamic_summary = ""
            rollover_summary = ""
            security_summary = ""
            try:
                observations = (
                    validate_fault_plan(
                        scenario, relay, byte_count
                    )
                    if relay is not None and scenario.fault is not None
                    else []
                )
                packets_sent, packets_received = validate_statistics(
                    scenario,
                    caller_complete,
                    listener_complete,
                    byte_count,
                    observations,
                )
                reference_statistics_lag = validate_wire_statistics(
                    scenario,
                    caller_complete,
                    listener_complete,
                    byte_count,
                    observations,
                )
                dynamic_rates = (
                    validate_dynamic_maximum_bandwidth(
                        scenario, caller.stdout
                    )
                    if scenario
                        .second_maximum_bandwidth_bytes_per_second
                        is not None
                    else None
                )
                observation = (
                    observations[0] if observations else None
                )
                rollover = (
                    validate_rollover(
                        scenario,
                        relay,
                        caller_complete,
                        listener_complete,
                        byte_count,
                        observation,
                    )
                    if scenario.rollover
                    and relay is not None
                    and observation is not None
                    else None
                )
                validate_stream_eof(
                    scenario, listener_stdout, byte_count
                )
                security = (
                    validate_encrypted_stream(
                        scenario,
                        caller_complete,
                        listener_complete,
                        relay,
                        relay.render(scenario.name),
                    )
                    if scenario.key_length != 0
                    and relay is not None
                    else None
                )
            except RuntimeError as error:
                reason = str(error)
                scenario_prefix = f"{scenario.name}: "
                if reason.startswith(scenario_prefix):
                    reason = reason[len(scenario_prefix):]
                raise RuntimeError(
                    render_failure(
                        scenario,
                        reason,
                        input_path,
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        relay_trace=(
                            relay.render(scenario.name)
                            if relay is not None
                            else None
                        ),
                    )
                ) from error
            if observations:
                caller_naks = nonnegative_statistic(
                    caller_complete, "pktRecvNAKTotal"
                )
                listener_naks = nonnegative_statistic(
                    listener_complete, "pktSentNAKTotal"
                )
                dropped = [
                    item
                    for item in observations
                    if item.get("action") == "drop"
                ]
                delayed = [
                    item
                    for item in observations
                    if item.get("action") == "delay"
                ]
                if dropped:
                    sequences = ",".join(
                        str(item["sequence"]) for item in dropped
                    )
                    matching_nak = all(
                        matching_loss_report_precedes_retransmission(
                            item
                        )
                        for item in dropped
                    )
                    maximum_delay = max(
                        int(
                            item[
                                "retransmission_delay_microseconds"
                            ]
                        )
                        for item in dropped
                    )
                    fault_summary += (
                        f" recovery={scenario.recovery} "
                        f"dropped_sequences={sequences} "
                        f"maximum_retransmission_delay_us="
                        f"{maximum_delay} "
                        "matching_nak_before_retransmission="
                        f"{matching_nak} "
                        f"caller_naks={caller_naks} "
                        f"listener_naks={listener_naks}"
                    )
                if delayed:
                    delay = delayed[0]
                    fault_summary += (
                        " ack_delay_ms="
                        f"{delay['delay_elapsed_milliseconds']:.1f} "
                        "delayed_ack_datagrams="
                        f"{delay['delayed_datagrams']} "
                        f"flow_window={scenario.flow_window_packets}"
                    )
            if dynamic_rates is not None:
                dynamic_summary = (
                    f" first_rate_bps={dynamic_rates[0]} "
                    f"second_rate_bps={dynamic_rates[1]}"
                )
            if rollover is not None:
                rollover_summary = (
                    f" caller_isn={rollover['caller_isn']} "
                    f"listener_isn={rollover['listener_isn']} "
                    f"wire_isn={rollover['wire_isn']} "
                    f"end_sequence={rollover['end_sequence']} "
                    f"rollover_packets={rollover['packets']}"
                )
            if security is not None:
                security_summary = (
                    f" aes_bits={scenario.key_length * 8} "
                    f"encrypted_packets={security['packets']} "
                    f"key_transitions={security['transitions']} "
                    "acknowledged_key_material="
                    f"{security['acknowledged_key_material']} "
                    + (
                        "file_api=true file_size_to_eof=true"
                        if scenario.file_api
                        else "stream_eof=true"
                    )
                )
            if reference_statistics_lag is not None:
                security_summary += (
                    " reference_sender_statistics_lag_packets="
                    f"{reference_statistics_lag['packets']}"
                    " reference_sender_statistics_lag_payload_bytes="
                    f"{reference_statistics_lag['payload_bytes']}"
                )
            print(
                f"PASS {scenario.name} sha256={expected_digest} "
                f"packets_sent={packets_sent} "
                f"packets_received={packets_received}"
                f"{fault_summary}"
                f"{dynamic_summary}"
                f"{rollover_summary}"
                f"{security_summary}",
                flush=True,
            )
        finally:
            if listener.poll() is None:
                terminate(listener)


def render_rendezvous_failure(
    scenario: RendezvousScenario,
    reason: str,
    input_path: Path,
    output_path: Path,
    sender_stdout: str = "",
    sender_stderr: str = "",
    receiver_stdout: str = "",
    receiver_stderr: str = "",
    handshake_trace: str | None = None,
) -> str:
    partial_bytes = output_path.stat().st_size if output_path.exists() else 0

    def section(label: str, output: str) -> str:
        return f"--- {label} ---\n{output if output else '<empty>'}\n"

    report = (
        f"{scenario.name}: {reason}; "
        f"partial_output_bytes={partial_bytes}\n"
    )
    if input_path.exists() and output_path.exists():
        report += (
            "--- payload diagnostic ---\n"
            + describe_file_mismatch(
                input_path, output_path, FILE_PAYLOAD_SIZE
            )
            + "\n"
        )
    report += (
        section("sender stdout", sender_stdout)
        + section("sender stderr", sender_stderr)
        + section("receiver stdout", receiver_stdout)
        + section("receiver stderr", receiver_stderr)
    )
    if handshake_trace is not None:
        report += section(
            "secret-safe FileCC rendezvous trace",
            handshake_trace,
        )
    return report


def run_rendezvous_scenario(
    scenario: RendezvousScenario,
    options: RunOptions,
    directory: Path,
    host: str = "127.0.0.1",
    environment: dict[str, str] | None = None,
) -> str:
    if scenario.start_order not in ("sender-first", "receiver-first"):
        raise RuntimeError(
            f"{scenario.name}: invalid start order "
            f"{scenario.start_order!r}"
        )
    byte_count = rendezvous_byte_count(scenario, options)
    with reserved_udp_ports(2, host) as endpoint_ports:
        sender_port, receiver_port = endpoint_ports
        trace_proxy = FileRendezvousTraceProxy(
            sender_port,
            receiver_port,
            scenario.fault,
            host=host,
        )
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    expected_digest = write_deterministic_payload(
        input_path, byte_count, scenario.seed
    )
    sender_stdout_path = directory / f"{scenario.name}.sender.stdout"
    sender_stderr_path = directory / f"{scenario.name}.sender.stderr"
    receiver_stdout_path = directory / f"{scenario.name}.receiver.stdout"
    receiver_stderr_path = directory / f"{scenario.name}.receiver.stderr"

    sender: subprocess.Popen[str] | None = None
    receiver: subprocess.Popen[str] | None = None
    processes: list[subprocess.Popen[str]] = []
    with (
        sender_stdout_path.open("w", encoding="utf-8") as sender_stdout_stream,
        sender_stderr_path.open("w", encoding="utf-8") as sender_stderr_stream,
        receiver_stdout_path.open(
            "w", encoding="utf-8"
        ) as receiver_stdout_stream,
        receiver_stderr_path.open(
            "w", encoding="utf-8"
        ) as receiver_stderr_stream,
    ):
        try:
            trace_proxy.start()
            process_specs = {
                "sender": (
                    rendezvous_peer_command(
                        scenario.sender,
                        "rendezvous-sender",
                        sender_port,
                        trace_proxy.sender_port,
                        input_path,
                        scenario,
                        options,
                        host,
                    ),
                    sender_stdout_stream,
                    sender_stderr_stream,
                ),
                "receiver": (
                    rendezvous_peer_command(
                        scenario.receiver,
                        "rendezvous-receiver",
                        receiver_port,
                        trace_proxy.receiver_port,
                        output_path,
                        scenario,
                        options,
                        host,
                    ),
                    receiver_stdout_stream,
                    receiver_stderr_stream,
                ),
            }
            start_sequence = (
                ("sender", "receiver")
                if scenario.start_order == "sender-first"
                else ("receiver", "sender")
            )
            for index, label in enumerate(start_sequence):
                command, stdout_stream, stderr_stream = process_specs[label]
                process = subprocess.Popen(
                    command,
                    stdout=stdout_stream,
                    stderr=stderr_stream,
                    text=True,
                    env=environment,
                )
                processes.append(process)
                if label == "sender":
                    sender = process
                else:
                    receiver = process
                if (
                    index == 0
                    and scenario.start_delay_milliseconds > 0
                ):
                    time.sleep(
                        scenario.start_delay_milliseconds / 1_000
                    )

            deadline = time.monotonic() + options.timeout_seconds + 5
            for process in processes:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(
                        process.args, options.timeout_seconds + 5
                    )
                process.wait(timeout=remaining)
        except subprocess.TimeoutExpired as error:
            for process in processes:
                terminate(process)
            for stream in (
                sender_stdout_stream,
                sender_stderr_stream,
                receiver_stdout_stream,
                receiver_stderr_stream,
            ):
                stream.flush()
            raise RuntimeError(
                render_rendezvous_failure(
                    scenario,
                    "FileCC rendezvous peers timed out",
                    input_path,
                    output_path,
                    sender_stdout_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    sender_stderr_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    receiver_stdout_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    receiver_stderr_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    trace_proxy.render(scenario.name),
                )
            ) from error
        finally:
            for process in processes:
                if process.poll() is None:
                    terminate(process)
            trace_proxy.close()

    if sender is None or receiver is None:
        raise RuntimeError(f"{scenario.name}: failed to start both peers")
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
    handshake_trace = trace_proxy.render(scenario.name)
    if sender.returncode != 0 or receiver.returncode != 0:
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                "FileCC rendezvous transfer failed "
                f"(sender={sender.returncode}, "
                f"receiver={receiver.returncode})",
                input_path,
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )
    if trace_proxy.error() is not None:
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                f"FileCC rendezvous relay failed: {trace_proxy.error()}",
                input_path,
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )

    sender_complete = parse_complete(
        sender_stdout, "rendezvous-sender"
    )
    receiver_complete = parse_complete(
        receiver_stdout, "rendezvous-receiver"
    )
    if (
        sender_complete.get("bytes") != byte_count
        or receiver_complete.get("bytes") != byte_count
    ):
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                "peer byte counters do not match",
                input_path,
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )
    received_digest = file_sha256(output_path)
    if received_digest != expected_digest:
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                f"SHA-256 mismatch {received_digest} "
                f"!= {expected_digest}",
                input_path,
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )
    try:
        fault_observations = (
            validate_fault_plan(
                scenario,
                trace_proxy,
                byte_count,
            )
            if scenario.fault is not None
            else []
        )
        packets_sent, packets_received = validate_statistics(
            scenario,
            sender_complete,
            receiver_complete,
            byte_count,
            fault_observations,
        )
        validate_wire_statistics(
            scenario,
            sender_complete,
            receiver_complete,
            byte_count,
            fault_observations,
        )
        dynamic_rates = (
            validate_dynamic_maximum_bandwidth(
                scenario,
                sender_stdout,
            )
            if scenario.second_maximum_bandwidth_bytes_per_second
                is not None
            else None
        )
        eof_event = (
            parse_event(receiver_stdout, "eof")
            if scenario.expect_eof
            else None
        )
        if scenario.expect_eof and (
            eof_event is None
            or eof_event.get("role") != "rendezvous-receiver"
            or eof_event.get("bytes") != byte_count
        ):
            raise RuntimeError(
                f"{scenario.name}: zero-byte stream EOF evidence "
                "is unavailable"
            )
        security = (
            validate_encrypted_stream(
                scenario,
                sender_complete,
                receiver_complete,
                trace_proxy,
                handshake_trace,
            )
            if scenario.key_length != 0
            else None
        )
    except RuntimeError as error:
        reason = str(error)
        prefix = f"{scenario.name}: "
        if reason.startswith(prefix):
            reason = reason[len(prefix):]
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                reason,
                input_path,
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        ) from error

    observation = trace_proxy.role_observation()
    if observation is None:
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                "could not observe both WAVEAHAND cookies",
                input_path,
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )
    if {
        observation["sender_role"],
        observation["receiver_role"],
    } != {"initiator", "responder"}:
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                f"invalid cookie role contest: {observation}",
                input_path,
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )
    if scenario.robotweax_peer == scenario.sender:
        robotweax_role = str(observation["sender_role"])
    elif scenario.robotweax_peer == scenario.receiver:
        robotweax_role = str(observation["receiver_role"])
    else:
        raise RuntimeError(
            f"{scenario.name}: Robotweax peer side is not identified"
        )

    recovery_summary = scenario.recovery
    if scenario.recovery == "CAUSAL":
        recovery_summary = ",".join(
            (
                "NAK"
                if item.get(
                    "later_data_observed_before_retransmission"
                )
                is True
                else "RTO"
            )
            for item in fault_observations
            if item.get("action") == "drop"
        )
    print(
        f"ROLE {scenario.name} robotweax={robotweax_role} "
        f"sender_cookie=0x{int(observation['sender_cookie']):08x} "
        f"receiver_cookie=0x"
        f"{int(observation['receiver_cookie']):08x}",
        flush=True,
    )
    print(
        f"PASS {scenario.name} sha256={expected_digest} "
        f"packets_sent={packets_sent} "
        f"packets_received={packets_received}"
        + (
            f" recovery={recovery_summary} dropped_sequences="
            + ",".join(
                str(item["sequence"])
                for item in fault_observations
                if item.get("action") == "drop"
            )
            if fault_observations
            else ""
        )
        + (
            f" first_rate_bps={dynamic_rates[0]} "
            f"second_rate_bps={dynamic_rates[1]}"
            if dynamic_rates is not None
            else ""
        )
        + (" eof=zero-byte" if eof_event is not None else "")
        + (
            f" aes_bits={scenario.key_length * 8} "
            f"encrypted_packets={security['packets']} "
            f"key_transitions={security['transitions']} "
            "acknowledged_key_material="
            f"{security['acknowledged_key_material']}"
            if security is not None
            else ""
        ),
        flush=True,
    )
    return robotweax_role


def run_rendezvous_role_matrix(
    scenarios: list[RendezvousScenario],
    robotweax: Path,
    reference: Path,
    options: RunOptions,
    directory: Path,
    role_probe_attempts: int,
    role_probe_bytes: int,
    role_probe_delay_milliseconds: int,
    *,
    host: str,
    coverage_name: str,
    probe_name_prefix: str,
) -> list[str]:
    robotweax_roles: set[str] = set()
    failures: list[str] = []
    for scenario in scenarios:
        try:
            robotweax_roles.add(
                run_rendezvous_scenario(
                    scenario,
                    options,
                    directory,
                    host,
                )
            )
        except (
            OSError,
            RuntimeError,
            subprocess.TimeoutExpired,
        ) as error:
            failures.append(str(error))

    required_roles = {"initiator", "responder"}
    if failures:
        return failures
    for attempt in range(role_probe_attempts):
        if robotweax_roles == required_roles:
            break
        try:
            robotweax_roles.add(
                run_rendezvous_scenario(
                    rendezvous_role_probe_scenario(
                        robotweax,
                        reference,
                        attempt,
                        role_probe_bytes,
                        role_probe_delay_milliseconds,
                        probe_name_prefix,
                    ),
                    options,
                    directory,
                    host,
                )
            )
        except (
            OSError,
            RuntimeError,
            subprocess.TimeoutExpired,
        ) as error:
            failures.append(str(error))
            return failures

    if robotweax_roles != required_roles:
        missing = sorted(required_roles - robotweax_roles)
        failures.append(
            f"{coverage_name} is incomplete after "
            f"{role_probe_attempts} probes; missing: "
            + ", ".join(missing)
        )
        return failures

    print(
        f"PASS {coverage_name} roles=initiator,responder",
        flush=True,
    )
    return failures


def main() -> int:
    global PINNED_REFERENCE_SRT_VERSION
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument(
        "--bytes", type=int, default=2 * 1_024 * 1_024 + 379
    )
    parser.add_argument("--timeout-seconds", type=int, default=45)
    parser.add_argument("--max-bw", type=int, default=20_000_000)
    parser.add_argument("--shutdown-grace-ms", type=int, default=250)
    parser.add_argument(
        "--expected-reference-version",
        type=parse_srt_version,
        default=compatible_srt_version(),
    )
    parser.add_argument(
        "--rendezvous-role-probe-attempts",
        type=int,
        default=16,
    )
    parser.add_argument(
        "--rendezvous-role-probe-bytes",
        type=int,
        default=128 * 1_024,
    )
    parser.add_argument(
        "--rendezvous-role-probe-delay-ms",
        type=int,
        default=100,
    )
    parser.add_argument("--skip-rollover", action="store_true")
    parser.add_argument("--rollover-only", action="store_true")
    parser.add_argument("--skip-resilience", action="store_true")
    parser.add_argument("--resilience-only", action="store_true")
    parser.add_argument(
        "--skip-rendezvous-resilience",
        action="store_true",
    )
    parser.add_argument(
        "--rendezvous-resilience-only",
        action="store_true",
    )
    parser.add_argument(
        "--skip-ipv6-rendezvous",
        action="store_true",
        help="skip the focused IPv6 FileCC rendezvous baseline and loss gate",
    )
    parser.add_argument(
        "--ipv6-rendezvous-only",
        action="store_true",
        help="run only the focused IPv6 FileCC rendezvous matrix",
    )
    arguments = parser.parse_args()
    PINNED_REFERENCE_SRT_VERSION = arguments.expected_reference_version
    if (
        arguments.bytes <= 0
        or arguments.timeout_seconds <= 0
        or arguments.max_bw <= 0
        or arguments.shutdown_grace_ms < 0
        or arguments.rendezvous_role_probe_attempts < 0
        or arguments.rendezvous_role_probe_bytes <= 0
        or arguments.rendezvous_role_probe_delay_ms < 0
        or (
            arguments.skip_rollover
            and arguments.rollover_only
        )
        or (
            arguments.skip_resilience
            and arguments.resilience_only
        )
        or (
            arguments.rollover_only
            and arguments.resilience_only
        )
        or (
            arguments.skip_rendezvous_resilience
            and arguments.rendezvous_resilience_only
        )
        or (
            arguments.rollover_only
            and arguments.rendezvous_resilience_only
        )
        or (
            arguments.resilience_only
            and arguments.rendezvous_resilience_only
        )
        or (
            arguments.ipv6_rendezvous_only
            and (
                arguments.skip_rollover
                or arguments.rollover_only
                or arguments.skip_resilience
                or arguments.resilience_only
                or arguments.skip_rendezvous_resilience
                or arguments.rendezvous_resilience_only
                or arguments.skip_ipv6_rendezvous
            )
        )
    ):
        parser.error("invalid file-transfer parameters")

    try:
        robotweax = resolve_program_path(arguments.robotweax_peer)
        reference = resolve_program_path(arguments.reference_peer)
        options = RunOptions(
            byte_count=arguments.bytes,
            timeout_seconds=arguments.timeout_seconds,
            maximum_bandwidth_bytes_per_second=arguments.max_bw,
            shutdown_grace_milliseconds=arguments.shutdown_grace_ms,
        )
        with tempfile.TemporaryDirectory(
            prefix="robotweax-srt-file-interop-"
        ) as directory:
            work = Path(directory)
            failures: list[str] = []
            run_baseline = (
                not arguments.rollover_only
                and not arguments.resilience_only
                and not arguments.rendezvous_resilience_only
                and not arguments.ipv6_rendezvous_only
            )
            if run_baseline:
                for scenario in scenario_matrix(robotweax, reference):
                    try:
                        run_scenario(scenario, options, work)
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if (
                not arguments.skip_rollover
                and not arguments.resilience_only
                and not arguments.rendezvous_resilience_only
                and not arguments.ipv6_rendezvous_only
            ):
                for scenario in rollover_scenario_matrix(
                    robotweax, reference
                ):
                    try:
                        run_scenario(scenario, options, work)
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if (
                not arguments.skip_resilience
                and not arguments.rollover_only
                and not arguments.rendezvous_resilience_only
                and not arguments.ipv6_rendezvous_only
            ):
                for scenario in resilience_scenario_matrix(
                    robotweax, reference
                ):
                    try:
                        run_scenario(scenario, options, work)
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if run_baseline:
                failures.extend(
                    run_rendezvous_role_matrix(
                        rendezvous_scenario_matrix(
                            robotweax,
                            reference,
                        ),
                        robotweax,
                        reference,
                        options,
                        work,
                        arguments.rendezvous_role_probe_attempts,
                        arguments.rendezvous_role_probe_bytes,
                        arguments.rendezvous_role_probe_delay_ms,
                        host="127.0.0.1",
                        coverage_name=(
                            "robotweax-filecc-rendezvous-cookie-role-"
                            "coverage"
                        ),
                        probe_name_prefix=(
                            "filecc-rendezvous-cookie-role-probe"
                        ),
                    )
                )
            if (
                (run_baseline or arguments.ipv6_rendezvous_only)
                and not arguments.skip_ipv6_rendezvous
            ):
                failures.extend(
                    run_rendezvous_role_matrix(
                        ipv6_rendezvous_scenario_matrix(
                            robotweax,
                            reference,
                        ),
                        robotweax,
                        reference,
                        options,
                        work,
                        arguments.rendezvous_role_probe_attempts,
                        arguments.rendezvous_role_probe_bytes,
                        arguments.rendezvous_role_probe_delay_ms,
                        host="::1",
                        coverage_name=(
                            "robotweax-filecc-ipv6-rendezvous-cookie-"
                            "role-coverage"
                        ),
                        probe_name_prefix=(
                            "filecc-ipv6-rendezvous-cookie-role-probe"
                        ),
                    )
                )
            if (
                not arguments.skip_rendezvous_resilience
                and not arguments.rollover_only
                and not arguments.resilience_only
                and not arguments.ipv6_rendezvous_only
            ):
                for scenario in rendezvous_resilience_scenario_matrix(
                    robotweax,
                    reference,
                ):
                    try:
                        run_rendezvous_scenario(
                            scenario,
                            options,
                            work,
                        )
                    except (
                        OSError,
                        RuntimeError,
                        subprocess.TimeoutExpired,
                    ) as error:
                        failures.append(str(error))
            if failures:
                raise RuntimeError(
                    "FileCC interoperability failures:\n\n"
                    + "\n\n".join(failures)
                )
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
