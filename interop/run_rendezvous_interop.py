#!/usr/bin/env python3
"""Rendezvous interoperability against pinned Haivision SRT."""

from __future__ import annotations

import argparse
import ipaddress
import os
import secrets
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, replace
from pathlib import Path

from interop_common import (
    DEFAULT_LOOPBACK_INPUT_BANDWIDTH,
    describe_file_mismatch,
    expected_live_packets,
    file_sha256,
    nonnegative_statistic,
    parse_complete,
    reserved_udp_ports,
    resolve_program_path,
    terminate,
    validate_sender_statistics,
    write_deterministic_payload,
)
from srt_handshake_trace import (
    RendezvousFault,
    RendezvousTraceProxy,
)


PASSPHRASE_ENVIRONMENT = "SRT_INTEROP_PASSPHRASE"
# Haivision v1.5.5 prioritizes queued original messages over retransmissions.
# Recovery profiles therefore need enough TSBPD latency to drain the bounded
# original flight before a queued retransmission reaches the receiver. Keep
# this distinct from the short-latency profiles that intentionally measure
# belated delivery or receiver-side TLPKTDROP.
LOSS_RECOVERY_LATENCY_MILLISECONDS = 600
SUSTAINED_FAULT_LATENCY_MILLISECONDS = 800
SUSTAINED_KEY_REFRESH_RATE = 500
SUSTAINED_KEY_PREANNOUNCEMENT = 200
STATISTICS_REORDER_TOLERANCE_PACKETS = 4
REFERENCE_RECEIVER_REORDER_TAIL_DISTANCE_PACKETS = 32
ROBOTWEAX_RECEIVER_REORDER_TAIL_DISTANCE_PACKETS = 2
REORDER_LATENCY_MILLISECONDS = 300
BELATED_TAIL_FAULT_DISTANCE_PACKETS = 8
BELATED_LATENCY_MILLISECONDS = 100
BELATED_DUPLICATE_DELAY_MILLISECONDS = 250
SUSTAINED_FAULT_SCHEDULE = (
    ("drop", 495),
    ("delay", 505),
    ("reorder", 995),
    ("delay", 1_005),
    ("drop", 1_495),
    ("delay", 1_505),
    ("reorder", 1_995),
    ("delay", 2_005),
    ("drop", 2_495),
    ("delay", 2_505),
    ("reorder", 2_995),
    ("delay", 3_005),
)


@dataclass(frozen=True)
class Scenario:
    name: str
    sender: Path
    receiver: Path
    key_length: int
    seed: int
    start_order: str = "receiver-first"
    start_delay_milliseconds: int = 0
    latency_milliseconds: int | None = None
    trace_roles: bool = False
    robotweax_peer: Path | None = None
    faults: tuple[RendezvousFault, ...] = ()
    byte_count_multiplier: int = 1
    key_refresh_rate: int | None = None
    key_preannouncement: int | None = None
    minimum_fault_key_transitions: int = 0
    maximum_reorder_tolerance_packets: int | None = None
    tail_fault_distance_packets: int | None = None
    statistics_profile: str | None = None
    source_pacing: bool = False
    crypto_mode: str | None = None
    expected_crypto_mode: str | None = None
    minimum_data_key_transitions: int = 0

    def __post_init__(self) -> None:
        if self.byte_count_multiplier <= 0:
            raise ValueError(
                "scenario byte-count multiplier must be positive"
            )
        if self.minimum_fault_key_transitions < 0:
            raise ValueError(
                "minimum fault key transitions must not be negative"
            )
        if self.minimum_data_key_transitions < 0:
            raise ValueError(
                "minimum data key transitions must not be negative"
            )
        if (
            self.maximum_reorder_tolerance_packets is not None
            and self.maximum_reorder_tolerance_packets < 0
        ):
            raise ValueError(
                "maximum reorder tolerance must not be negative"
            )
        if (
            self.tail_fault_distance_packets is not None
            and self.tail_fault_distance_packets < 2
        ):
            raise ValueError(
                "tail fault distance must leave a partner packet"
            )
        if self.statistics_profile not in (None, "reorder", "belated"):
            raise ValueError("unsupported statistics profile")
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
        if self.statistics_profile is not None:
            expected_action = (
                "reorder"
                if self.statistics_profile == "reorder"
                else "late_duplicate"
            )
            if (
                len(self.faults) != 1
                or self.faults[0].action != expected_action
                or self.faults[0].packet_kind != "data"
                or self.faults[0].direction != "sender_to_receiver"
                or self.tail_fault_distance_packets is None
            ):
                raise ValueError(
                    "statistics profiles require one matching "
                    "tail data fault"
                )
        if (
            self.statistics_profile == "reorder"
            and (
                self.maximum_reorder_tolerance_packets is None
                or self.maximum_reorder_tolerance_packets == 0
            )
        ):
            raise ValueError(
                "reorder statistics require a positive tolerance"
            )
        if (self.key_refresh_rate is None) != (
            self.key_preannouncement is None
        ):
            raise ValueError(
                "scenario key-rotation overrides must be paired"
            )
        if self.key_refresh_rate is not None:
            if (
                self.key_refresh_rate <= 0
                or self.key_preannouncement is None
                or self.key_preannouncement <= 0
                or self.key_preannouncement
                > (self.key_refresh_rate - 1) // 2
            ):
                raise ValueError(
                    "invalid scenario key-rotation parameters"
                )


@dataclass(frozen=True)
class RunOptions:
    byte_count: int
    timeout_seconds: int
    key_refresh_rate: int
    key_preannouncement: int
    chunk_size: int = 1_200
    input_bandwidth_bytes_per_second: int = (
        DEFAULT_LOOPBACK_INPUT_BANDWIDTH
    )
    shutdown_grace_milliseconds: int = 250
    maximum_payload_size: int | None = None
    expected_maximum_payload_size: int | None = None


def options_for_scenario(
    scenario: Scenario,
    options: RunOptions,
) -> RunOptions:
    return replace(
        options,
        byte_count=(
            options.byte_count * scenario.byte_count_multiplier
        ),
        key_refresh_rate=(
            scenario.key_refresh_rate
            if scenario.key_refresh_rate is not None
            else options.key_refresh_rate
        ),
        key_preannouncement=(
            scenario.key_preannouncement
            if scenario.key_preannouncement is not None
            else options.key_preannouncement
        ),
    )


def fault_plan_for_scenario(
    scenario: Scenario,
    options: RunOptions,
) -> tuple[RendezvousFault, ...]:
    if scenario.tail_fault_distance_packets is None:
        return scenario.faults
    packet_count = expected_live_packets(
        options.byte_count, options.chunk_size
    )
    if packet_count <= scenario.tail_fault_distance_packets:
        raise RuntimeError(
            f"{scenario.name}: transfer has only {packet_count} data "
            "packets, which cannot satisfy its tail fault distance"
        )
    return (
        replace(
            scenario.faults[0],
            occurrence=(
                packet_count
                - scenario.tail_fault_distance_packets
            ),
        ),
    )


def scenario_matrix(robotweax: Path, reference: Path) -> list[Scenario]:
    scenarios = [
        Scenario(
            name="clear-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            key_length=0,
            seed=30_001,
            robotweax_peer=robotweax,
        ),
        Scenario(
            name="clear-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            key_length=0,
            seed=30_002,
            robotweax_peer=robotweax,
        ),
    ]
    for index, key_length in enumerate((16, 24, 32), start=1):
        label = key_length * 8
        scenarios.extend(
            (
                Scenario(
                    name=f"aes{label}-robotweax-to-haivision",
                    sender=robotweax,
                    receiver=reference,
                    key_length=key_length,
                    seed=31_000 + index,
                    robotweax_peer=robotweax,
                ),
                Scenario(
                    name=f"aes{label}-haivision-to-robotweax",
                    sender=reference,
                    receiver=robotweax,
                    key_length=key_length,
                    seed=32_000 + index,
                    robotweax_peer=robotweax,
                ),
            )
        )
    return scenarios


def ipv6_scenario_matrix(
    robotweax: Path,
    reference: Path,
) -> list[Scenario]:
    return [
        Scenario(
            name="ipv6-clear-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            key_length=0,
            seed=35_001,
            trace_roles=True,
            robotweax_peer=robotweax,
        ),
        Scenario(
            name="ipv6-clear-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            key_length=0,
            seed=35_002,
            trace_roles=True,
            robotweax_peer=robotweax,
        ),
        Scenario(
            name="ipv6-aes256-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            key_length=32,
            seed=35_003,
            trace_roles=True,
            robotweax_peer=robotweax,
        ),
        Scenario(
            name="ipv6-aes256-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            key_length=32,
            seed=35_004,
            trace_roles=True,
            robotweax_peer=robotweax,
        ),
        Scenario(
            name="ipv6-aes256-data-drop-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            key_length=32,
            seed=35_005,
            trace_roles=True,
            robotweax_peer=robotweax,
            faults=(
                RendezvousFault(
                    action="drop",
                    direction="sender_to_receiver",
                    occurrence=128,
                ),
            ),
        ),
        Scenario(
            name="ipv6-aes256-data-reorder-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            key_length=32,
            seed=35_006,
            trace_roles=True,
            robotweax_peer=robotweax,
            faults=(
                RendezvousFault(
                    action="reorder",
                    direction="sender_to_receiver",
                    occurrence=256,
                ),
            ),
        ),
    ]


def timing_matrix(
    robotweax: Path,
    reference: Path,
    start_delay_milliseconds: int,
) -> list[Scenario]:
    return [
        Scenario(
            name="aes256-timing-robotweax-sender-first",
            sender=robotweax,
            receiver=reference,
            key_length=32,
            seed=41_001,
            start_order="sender-first",
            start_delay_milliseconds=start_delay_milliseconds,
            trace_roles=True,
            robotweax_peer=robotweax,
        ),
        Scenario(
            name="aes256-timing-haivision-receiver-first",
            sender=robotweax,
            receiver=reference,
            key_length=32,
            seed=41_002,
            start_order="receiver-first",
            start_delay_milliseconds=start_delay_milliseconds,
            trace_roles=True,
            robotweax_peer=robotweax,
        ),
        Scenario(
            name="aes256-timing-haivision-sender-first",
            sender=reference,
            receiver=robotweax,
            key_length=32,
            seed=42_001,
            start_order="sender-first",
            start_delay_milliseconds=start_delay_milliseconds,
            trace_roles=True,
            robotweax_peer=robotweax,
        ),
        Scenario(
            name="aes256-timing-robotweax-receiver-first",
            sender=reference,
            receiver=robotweax,
            key_length=32,
            seed=42_002,
            start_order="receiver-first",
            start_delay_milliseconds=start_delay_milliseconds,
            trace_roles=True,
            robotweax_peer=robotweax,
        ),
    ]


def role_probe_scenario(
    robotweax: Path,
    reference: Path,
    attempt: int,
    start_delay_milliseconds: int,
) -> Scenario:
    combination = attempt % 4
    robotweax_sends = combination < 2
    sender = robotweax if robotweax_sends else reference
    receiver = reference if robotweax_sends else robotweax
    start_order = (
        "sender-first" if combination % 2 == 0 else "receiver-first"
    )
    return Scenario(
        name=f"clear-cookie-role-probe-{attempt + 1}",
        sender=sender,
        receiver=receiver,
        key_length=0,
        seed=50_000 + attempt,
        start_order=start_order,
        start_delay_milliseconds=start_delay_milliseconds,
        trace_roles=True,
        robotweax_peer=robotweax,
    )


def sustained_fault_plan(
    delay_milliseconds: int,
) -> tuple[RendezvousFault, ...]:
    return tuple(
        RendezvousFault(
            action=action,
            direction="sender_to_receiver",
            occurrence=occurrence,
            delay_milliseconds=(
                delay_milliseconds
                if action == "delay"
                else 0
            ),
        )
        for action, occurrence in SUSTAINED_FAULT_SCHEDULE
    )


def fault_matrix(
    robotweax: Path,
    reference: Path,
    delay_milliseconds: int,
) -> list[Scenario]:
    scenarios: list[Scenario] = []
    handshake_actions = (
        ("drop", 0),
        ("duplicate", 0),
        ("delay", delay_milliseconds),
        ("reorder", 0),
    )
    for index, (action, delay) in enumerate(handshake_actions):
        robotweax_sends = index % 2 == 0
        scenarios.append(
            Scenario(
                name=f"clear-handshake-{action}",
                sender=robotweax if robotweax_sends else reference,
                receiver=reference if robotweax_sends else robotweax,
                key_length=0,
                seed=60_000 + index,
                trace_roles=True,
                robotweax_peer=robotweax,
                faults=(
                    RendezvousFault(
                        action=action,
                        direction="either",
                        occurrence=1,
                        packet_kind="handshake",
                        delay_milliseconds=delay,
                        handshake_request=-1,
                    ),
                ),
            )
        )

    data_occurrences = {
        "drop": 128,
        "duplicate": 256,
        "delay": 384,
        "reorder": 512,
    }
    for action_index, action in enumerate(
        ("drop", "duplicate", "delay", "reorder")
    ):
        data_latency_milliseconds = {
            "drop": LOSS_RECOVERY_LATENCY_MILLISECONDS,
            "reorder": REORDER_LATENCY_MILLISECONDS,
        }.get(action)
        for direction_index, (sender, receiver, label) in enumerate(
            (
                (robotweax, reference, "robotweax-to-haivision"),
                (reference, robotweax, "haivision-to-robotweax"),
            )
        ):
            reorder_tail_distance = (
                ROBOTWEAX_RECEIVER_REORDER_TAIL_DISTANCE_PACKETS
                if receiver == robotweax
                else REFERENCE_RECEIVER_REORDER_TAIL_DISTANCE_PACKETS
            )
            scenarios.append(
                Scenario(
                    name=f"aes256-data-{action}-{label}",
                    sender=sender,
                    receiver=receiver,
                    key_length=32,
                    seed=61_000 + action_index * 10 + direction_index,
                    latency_milliseconds=data_latency_milliseconds,
                    source_pacing=action == "reorder",
                    trace_roles=True,
                    robotweax_peer=robotweax,
                    faults=(
                        RendezvousFault(
                            action=action,
                            direction="sender_to_receiver",
                            occurrence=data_occurrences[action],
                            delay_milliseconds=(
                                delay_milliseconds
                                if action == "delay"
                                else 0
                            ),
                        ),
                    ),
                    maximum_reorder_tolerance_packets=(
                        STATISTICS_REORDER_TOLERANCE_PACKETS
                        if action == "reorder"
                        else None
                    ),
                    tail_fault_distance_packets=(
                        reorder_tail_distance
                        if action == "reorder"
                        else None
                    ),
                    statistics_profile=(
                        "reorder" if action == "reorder" else None
                    ),
                )
            )

    for direction_index, (sender, receiver, label) in enumerate(
        (
            (robotweax, reference, "robotweax-to-haivision"),
            (reference, robotweax, "haivision-to-robotweax"),
        )
    ):
        scenarios.append(
            Scenario(
                name=f"clear-data-belated-{label}",
                sender=sender,
                receiver=receiver,
                key_length=0,
                seed=61_100 + direction_index,
                latency_milliseconds=BELATED_LATENCY_MILLISECONDS,
                trace_roles=True,
                robotweax_peer=robotweax,
                faults=(
                    RendezvousFault(
                        action="late_duplicate",
                        direction="sender_to_receiver",
                        occurrence=1,
                        delay_milliseconds=(
                            BELATED_DUPLICATE_DELAY_MILLISECONDS
                        ),
                    ),
                ),
                tail_fault_distance_packets=(
                    BELATED_TAIL_FAULT_DISTANCE_PACKETS
                ),
                statistics_profile="belated",
            )
        )

    data_directions = (
        (robotweax, reference, "robotweax-to-haivision"),
        (reference, robotweax, "haivision-to-robotweax"),
    )
    for direction_index, (sender, receiver, label) in enumerate(
        data_directions
    ):
        scenarios.append(
            Scenario(
                name=f"aes256-data-burst-drop-{label}",
                sender=sender,
                receiver=receiver,
                key_length=32,
                seed=62_000 + direction_index,
                latency_milliseconds=(
                    LOSS_RECOVERY_LATENCY_MILLISECONDS
                ),
                trace_roles=True,
                robotweax_peer=robotweax,
                faults=tuple(
                    RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=occurrence,
                    )
                    for occurrence in (999, 1_000, 1_001)
                ),
            )
        )
        scenarios.append(
            Scenario(
                name=f"aes256-data-drop-reorder-rotation-{label}",
                sender=sender,
                receiver=receiver,
                key_length=32,
                seed=63_000 + direction_index,
                latency_milliseconds=(
                    LOSS_RECOVERY_LATENCY_MILLISECONDS
                ),
                trace_roles=True,
                robotweax_peer=robotweax,
                faults=(
                    RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=998,
                    ),
                    RendezvousFault(
                        action="reorder",
                        direction="sender_to_receiver",
                        occurrence=1_002,
                    ),
                ),
            )
        )
        scenarios.append(
            Scenario(
                name=f"aes256-sustained-faults-{label}",
                sender=sender,
                receiver=receiver,
                key_length=32,
                seed=64_000 + direction_index,
                latency_milliseconds=(
                    SUSTAINED_FAULT_LATENCY_MILLISECONDS
                ),
                trace_roles=True,
                robotweax_peer=robotweax,
                faults=sustained_fault_plan(
                    delay_milliseconds
                ),
                key_refresh_rate=SUSTAINED_KEY_REFRESH_RATE,
                key_preannouncement=SUSTAINED_KEY_PREANNOUNCEMENT,
                minimum_fault_key_transitions=5,
            )
        )
    return scenarios


def encrypted_drop_retransmission_failure(
    fault_plan: tuple[RendezvousFault, ...],
    fault_observations: list[dict[str, object]],
) -> str | None:
    for plan_index, (fault, observation) in enumerate(
        zip(fault_plan, fault_observations, strict=True),
        start=1,
    ):
        if fault.packet_kind != "data" or fault.action != "drop":
            continue
        sequence = observation.get("sequence")
        if observation.get("retransmission_observed") is not True:
            return (
                f"encrypted dropped data packet in plan entry {plan_index} "
                f"(sequence {sequence}) had no observed retransmission"
            )
        if (
            observation.get("retransmission_key_selection")
            != observation.get("key_selection")
        ):
            return (
                f"encrypted retransmission in plan entry {plan_index} "
                f"(sequence {sequence}) changed key selector"
            )
        if (
            observation.get("retransmission_ciphertext_matches")
            is not True
        ):
            return (
                f"encrypted retransmission in plan entry {plan_index} "
                f"(sequence {sequence}) changed ciphertext"
            )
    return None


def observed_fault_key_transitions(
    fault_observations: list[dict[str, object]],
) -> int:
    selectors = [
        int(selection)
        for observation in fault_observations
        if observation.get("packet_kind") == "data"
        and (selection := observation.get("key_selection"))
        in (1, 2)
    ]
    return sum(
        previous != current
        for previous, current in zip(
            selectors, selectors[1:], strict=False
        )
    )


def validate_receiver_statistics_profile(
    scenario: Scenario,
    receiver_complete: dict[str, object],
    fault_observation: dict[str, object],
    forwarded_retransmissions: int = 0,
) -> dict[str, int | float]:
    statistics = receiver_complete.get("stats")
    if not isinstance(statistics, dict):
        raise RuntimeError(
            f"{scenario.name}: receiver statistics are unavailable"
        )

    def required_integer(name: str) -> int:
        value = statistics.get(name)
        if type(value) is not int or value < 0:
            raise RuntimeError(
                f"{scenario.name}: receiver {name} is unavailable"
            )
        return value

    if scenario.statistics_profile == "reorder":
        fault_sequence = fault_observation.get("sequence")
        partner_sequence = fault_observation.get(
            "reorder_partner_sequence"
        )
        adjacent_originals = (
            type(fault_sequence) is int
            and type(partner_sequence) is int
            and partner_sequence
            == ((fault_sequence + 1) & 0x7FFF_FFFF)
        )
        if (
            fault_observation.get("action") != "reorder"
            or fault_observation.get("reorder_released") is not True
            or fault_observation.get("retransmitted") is not False
            or fault_observation.get(
                "reorder_partner_retransmitted"
            ) is not False
            or fault_observation.get(
                "reorder_partner_forwarded_first"
            ) is not True
            or not adjacent_originals
        ):
            raise RuntimeError(
                f"{scenario.name}: relay did not prove one adjacent "
                "original-packet exchange"
            )
        distance = required_integer("pktReorderDistance")
        tolerance = required_integer("pktReorderTolerance")
        belated = required_integer("pktRcvBelated")
        packets_received = required_integer("pktRecvTotal")
        packets_received_unique = required_integer(
            "pktRecvUniqueTotal"
        )
        duplicate_packets = (
            packets_received - packets_received_unique
        )
        if duplicate_packets < 0:
            raise RuntimeError(
                f"{scenario.name}: unique receive packets exceed "
                "physical receive packets"
            )
        configured_tolerance = (
            scenario.maximum_reorder_tolerance_packets
        )
        if (
            distance < 1
            or configured_tolerance is None
            or tolerance < 1
            or tolerance > configured_tolerance
        ):
            raise RuntimeError(
                f"{scenario.name}: adjacent reorder produced invalid "
                "distance/tolerance statistics "
                f"(distance={distance}, tolerance={tolerance}, "
                f"configured={configured_tolerance})"
            )
        if (
            belated > duplicate_packets
            or belated > forwarded_retransmissions
        ):
            raise RuntimeError(
                f"{scenario.name}: {belated} belated packet(s) are not "
                "fully explained by physical duplicates and observed "
                "retransmissions "
                f"(duplicates={duplicate_packets}, "
                f"retransmissions={forwarded_retransmissions})"
            )
        return {
            "reorder_distance": distance,
            "reorder_tolerance": tolerance,
            "belated_packets": belated,
            "duplicate_packets": duplicate_packets,
            "retransmission_candidates": forwarded_retransmissions,
        }

    if scenario.statistics_profile == "belated":
        if fault_observation.get("late_duplicate_released") is not True:
            raise RuntimeError(
                f"{scenario.name}: delayed duplicate was not released"
            )
        configured_delay = scenario.faults[0].delay_milliseconds
        observed_delay = fault_observation.get(
            "delay_elapsed_milliseconds"
        )
        if (
            type(observed_delay) not in (int, float)
            or observed_delay < configured_delay
        ):
            raise RuntimeError(
                f"{scenario.name}: delayed duplicate release was too "
                f"early ({observed_delay} ms < {configured_delay} ms)"
            )
        belated = required_integer("pktRcvBelated")
        packets_received = required_integer("pktRecvTotal")
        packets_received_unique = required_integer(
            "pktRecvUniqueTotal"
        )
        average_delay = statistics.get("pktRcvAvgBelatedTime")
        if (
            belated < 1
            or type(average_delay) not in (int, float)
            or average_delay <= 0
        ):
            raise RuntimeError(
                f"{scenario.name}: delayed duplicate lacks belated "
                "count/delay statistics "
                f"(packets={belated}, average_ms={average_delay})"
            )
        if packets_received < packets_received_unique + 1:
            raise RuntimeError(
                f"{scenario.name}: delayed duplicate is missing from "
                "physical versus unique receive totals "
                f"({packets_received} < "
                f"{packets_received_unique} + 1)"
            )
        return {
            "belated_packets": belated,
            "average_belated_milliseconds": average_delay,
            "received_packets": packets_received,
            "received_unique_packets": packets_received_unique,
        }

    return {}


def peer_command(
    program: Path,
    role: str,
    local_port: int,
    peer_port: int,
    payload_path: Path,
    options: RunOptions,
    key_length: int,
    latency_milliseconds: int | None = None,
    maximum_reorder_tolerance_packets: int | None = None,
    shutdown_grace_extra_milliseconds: int = 0,
    host: str = "127.0.0.1",
    source_pacing: bool = False,
    crypto_mode: str | None = None,
    expected_crypto_mode: str | None = None,
) -> list[str]:
    if shutdown_grace_extra_milliseconds < 0:
        raise ValueError("shutdown grace extra must not be negative")
    path_option = (
        "--input" if role == "rendezvous-sender" else "--output"
    )
    # A drained sender buffer only proves peer acknowledgement. Keep the
    # connection alive for the configured close margin after TSBPD can release
    # the final packet to the receiving application.
    effective_shutdown_grace_milliseconds = (
        options.shutdown_grace_milliseconds
        + (latency_milliseconds or 0)
        + shutdown_grace_extra_milliseconds
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
        str(options.byte_count),
        path_option,
        str(payload_path),
        "--transport",
        "live",
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
        "--chunk-size",
        str(options.chunk_size),
        "--shutdown-grace-ms",
        str(effective_shutdown_grace_milliseconds),
    ]
    if key_length != 0:
        command.extend(
            (
                "--passphrase-env",
                PASSPHRASE_ENVIRONMENT,
                "--pbkeylen",
                str(key_length),
                "--km-refresh-rate",
                str(options.key_refresh_rate),
                "--km-preannounce",
                str(options.key_preannouncement),
            )
        )
    if crypto_mode is not None:
        if expected_crypto_mode is None:
            raise ValueError("crypto mode requires an expected mode")
        command.extend(
            (
                "--crypto-mode",
                crypto_mode,
                "--expect-crypto-mode",
                expected_crypto_mode,
            )
        )
    if options.maximum_payload_size is not None:
        command.extend(
            ("--payload-size", str(options.maximum_payload_size))
        )
    if options.expected_maximum_payload_size is not None:
        command.extend(
            (
                "--expect-payload-size",
                str(options.expected_maximum_payload_size),
            )
        )
    if latency_milliseconds is not None:
        command.extend(
            ("--latency-ms", str(latency_milliseconds))
        )
    if maximum_reorder_tolerance_packets is not None:
        command.extend(
            (
                "--loss-max-ttl",
                str(maximum_reorder_tolerance_packets),
            )
        )
    if role == "rendezvous-sender":
        command.extend(
            (
                "--input-bw",
                str(options.input_bandwidth_bytes_per_second),
                "--max-bw",
                "0",
            )
        )
        if source_pacing:
            command.append("--source-pacing")
    elif source_pacing:
        raise ValueError("source pacing requires a sender role")
    return command


def render_failure(
    scenario: Scenario,
    reason: str,
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
        + section("sender stdout", sender_stdout)
        + section("sender stderr", sender_stderr)
        + section("receiver stdout", receiver_stdout)
        + section("receiver stderr", receiver_stderr)
    )
    if handshake_trace is not None:
        report += section(
            "secret-safe rendezvous handshake trace",
            handshake_trace,
        )
    return report


def run_scenario(
    scenario: Scenario,
    options: RunOptions,
    directory: Path,
    environment: dict[str, str],
    host: str = "127.0.0.1",
) -> str | None:
    run_options = options_for_scenario(scenario, options)
    fault_plan = fault_plan_for_scenario(scenario, run_options)
    with reserved_udp_ports(2, host) as endpoint_ports:
        sender_port, receiver_port = endpoint_ports
        trace_proxy = (
            RendezvousTraceProxy(
                sender_port,
                receiver_port,
                fault_plan,
                host=host,
            )
            if scenario.trace_roles or fault_plan
            else None
        )
    sender_peer_port = (
        trace_proxy.sender_port
        if trace_proxy is not None
        else receiver_port
    )
    receiver_peer_port = (
        trace_proxy.receiver_port
        if trace_proxy is not None
        else sender_port
    )
    if scenario.start_order not in ("sender-first", "receiver-first"):
        raise RuntimeError(
            f"{scenario.name}: invalid start order {scenario.start_order!r}"
        )
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    expected_digest = write_deterministic_payload(
        input_path, run_options.byte_count, scenario.seed
    )
    sender_stdout_path = directory / f"{scenario.name}.sender.stdout"
    sender_stderr_path = directory / f"{scenario.name}.sender.stderr"
    receiver_stdout_path = directory / f"{scenario.name}.receiver.stdout"
    receiver_stderr_path = directory / f"{scenario.name}.receiver.stderr"

    processes: list[subprocess.Popen[str]] = []
    sender: subprocess.Popen[str] | None = None
    receiver: subprocess.Popen[str] | None = None
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
            if trace_proxy is not None:
                trace_proxy.start()
            process_specs = {
                "sender": (
                    peer_command(
                        scenario.sender,
                        "rendezvous-sender",
                        sender_port,
                        sender_peer_port,
                        input_path,
                        run_options,
                        scenario.key_length,
                        scenario.latency_milliseconds,
                        scenario.maximum_reorder_tolerance_packets,
                        (
                            scenario.faults[0].delay_milliseconds
                            if scenario.statistics_profile == "belated"
                            else 0
                        ),
                        host=host,
                        source_pacing=scenario.source_pacing,
                        crypto_mode=scenario.crypto_mode,
                        expected_crypto_mode=(
                            scenario.expected_crypto_mode
                        ),
                    ),
                    sender_stdout_stream,
                    sender_stderr_stream,
                ),
                "receiver": (
                    peer_command(
                        scenario.receiver,
                        "rendezvous-receiver",
                        receiver_port,
                        receiver_peer_port,
                        output_path,
                        run_options,
                        scenario.key_length,
                        scenario.latency_milliseconds,
                        scenario.maximum_reorder_tolerance_packets,
                        0,
                        host=host,
                        crypto_mode=scenario.crypto_mode,
                        expected_crypto_mode=(
                            scenario.expected_crypto_mode
                        ),
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
                command, stdout_stream, stderr_stream = process_specs[
                    label
                ]
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

            deadline = (
                time.monotonic()
                + run_options.timeout_seconds
                + 5
            )
            for process in processes:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(
                        process.args,
                        run_options.timeout_seconds + 5,
                    )
                process.wait(timeout=remaining)
        except subprocess.TimeoutExpired as error:
            for process in processes:
                terminate(process)
            sender_stdout_stream.flush()
            sender_stderr_stream.flush()
            receiver_stdout_stream.flush()
            receiver_stderr_stream.flush()
            raise RuntimeError(
                render_failure(
                    scenario,
                    "rendezvous peers timed out",
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
                    trace_proxy.render(scenario.name)
                    if trace_proxy is not None
                    else None,
                )
            ) from error
        finally:
            for process in processes:
                if process.poll() is None:
                    terminate(process)
            if trace_proxy is not None:
                trace_proxy.close()

    if sender is None or receiver is None:
        raise RuntimeError(f"{scenario.name}: failed to start both peers")
    handshake_trace = (
        trace_proxy.render(scenario.name)
        if trace_proxy is not None
        else None
    )
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
    if sender.returncode != 0 or receiver.returncode != 0:
        raise RuntimeError(
            render_failure(
                scenario,
                "rendezvous transfer failed "
                f"(sender={sender.returncode}, "
                f"receiver={receiver.returncode})",
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )
    if trace_proxy is not None and trace_proxy.error() is not None:
        raise RuntimeError(
            render_failure(
                scenario,
                f"rendezvous relay failed: {trace_proxy.error()}",
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
        sender_complete.get("bytes") != run_options.byte_count
        or receiver_complete.get("bytes") != run_options.byte_count
    ):
        raise RuntimeError(
            render_failure(
                scenario,
                "peer byte counters do not match",
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
        mismatch = describe_file_mismatch(
            input_path, output_path, run_options.chunk_size
        )
        raise RuntimeError(
            render_failure(
                scenario,
                f"SHA-256 mismatch {received_digest} "
                f"!= {expected_digest}; {mismatch}",
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )

    minimum_packets = max(
        expected_live_packets(
            run_options.byte_count, run_options.chunk_size
        ),
        run_options.key_refresh_rate * 3
        if scenario.key_length != 0
        else 0,
    )
    try:
        packets_sent = validate_sender_statistics(
            sender_complete, minimum_packets
        )
    except RuntimeError as error:
        raise RuntimeError(
            render_failure(
                scenario,
                str(error),
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        ) from error
    packets_received = nonnegative_statistic(
        receiver_complete, "pktRecvTotal"
    )
    if packets_received is None or packets_received < minimum_packets:
        raise RuntimeError(
            render_failure(
                scenario,
                f"only {packets_received} packets were received; "
                f"at least {minimum_packets} are required "
                f"(sender reported {packets_sent})",
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )
    undecryptable = nonnegative_statistic(
        receiver_complete, "pktRcvUndecryptTotal"
    )
    if undecryptable not in (None, 0):
        raise RuntimeError(
            render_failure(
                scenario,
                f"receiver reported {undecryptable} undecryptable packets",
                output_path,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                handshake_trace,
            )
        )

    robotweax_role: str | None = None
    fault_observations: list[dict[str, object]] = []
    if fault_plan:
        if trace_proxy is None:
            raise RuntimeError(
                f"{scenario.name}: fault relay was not created"
            )
        fault_observations = trace_proxy.fault_observations()
        if len(fault_observations) != len(fault_plan):
            observed_indices = {
                int(observation["plan_index"])
                for observation in fault_observations
            }
            missing_indices = [
                str(index)
                for index in range(1, len(fault_plan) + 1)
                if index not in observed_indices
            ]
            raise RuntimeError(
                render_failure(
                    scenario,
                    "configured rendezvous fault(s) were not injected; "
                    "missing plan indices: "
                    + ", ".join(missing_indices),
                    output_path,
                    sender_stdout,
                    sender_stderr,
                    receiver_stdout,
                    receiver_stderr,
                    handshake_trace,
                )
            )
        for plan_index, (fault, observation) in enumerate(
            zip(fault_plan, fault_observations, strict=True),
            start=1,
        ):
            if (
                observation.get("plan_index") != plan_index
                or observation.get("action") != fault.action
                or observation.get("occurrence") != fault.occurrence
            ):
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "fault observation does not match configured "
                        f"plan entry {plan_index}",
                        output_path,
                        sender_stdout,
                        sender_stderr,
                        receiver_stdout,
                        receiver_stderr,
                        handshake_trace,
                    )
                )
        observed_key_transitions = (
            observed_fault_key_transitions(
                fault_observations
            )
        )
        if (
            observed_key_transitions
            < scenario.minimum_fault_key_transitions
        ):
            raise RuntimeError(
                render_failure(
                    scenario,
                    "fault schedule crossed only "
                    f"{observed_key_transitions} observed key "
                    "transition(s); at least "
                    f"{scenario.minimum_fault_key_transitions} "
                    "are required",
                    output_path,
                    sender_stdout,
                    sender_stderr,
                    receiver_stdout,
                    receiver_stderr,
                    handshake_trace,
                )
            )
        dropped_data_packets = sum(
            fault.packet_kind == "data" and fault.action == "drop"
            for fault in fault_plan
        )
        if dropped_data_packets:
            retransmissions = nonnegative_statistic(
                sender_complete, "pktRetransTotal"
            )
            if (
                retransmissions is None
                or retransmissions < dropped_data_packets
            ):
                raise RuntimeError(
                    render_failure(
                        scenario,
                        f"{dropped_data_packets} dropped data packet(s) "
                        "produced only "
                        f"{retransmissions} reported retransmission(s)",
                        output_path,
                        sender_stdout,
                        sender_stderr,
                        receiver_stdout,
                        receiver_stderr,
                        handshake_trace,
                    )
                )
            if scenario.key_length != 0:
                retransmission_failure = (
                    encrypted_drop_retransmission_failure(
                        fault_plan, fault_observations
                    )
                )
                if retransmission_failure is not None:
                    raise RuntimeError(
                        render_failure(
                            scenario,
                            retransmission_failure,
                            output_path,
                            sender_stdout,
                            sender_stderr,
                            receiver_stdout,
                            receiver_stderr,
                            handshake_trace,
                        )
                    )
        for fault_observation in fault_observations:
            print(
                f"FAULT {scenario.name} "
                f"plan_index={fault_observation['plan_index']} "
                f"action={fault_observation['action']} "
                f"direction={fault_observation['direction']} "
                f"packet_kind={fault_observation['packet_kind']} "
                f"occurrence={fault_observation['occurrence']}",
                flush=True,
            )

    statistics_summary: dict[str, int | float] = {}
    if scenario.statistics_profile is not None:
        forwarded_retransmissions = (
            trace_proxy.forwarded_data_retransmissions(
                "sender_to_receiver"
            )
            if trace_proxy is not None
            else 0
        )
        statistics_summary = validate_receiver_statistics_profile(
            scenario,
            receiver_complete,
            fault_observations[0],
            forwarded_retransmissions,
        )
        print(
            f"STATISTICS {scenario.name} "
            + " ".join(
                f"{name}={value}"
                for name, value in statistics_summary.items()
            ),
            flush=True,
        )

    if scenario.trace_roles:
        if trace_proxy is None:
            raise RuntimeError(
                f"{scenario.name}: role trace relay was not created"
            )
        observation = trace_proxy.role_observation()
        if observation is None:
            raise RuntimeError(
                render_failure(
                    scenario,
                    "could not observe both WAVEAHAND cookies",
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
                render_failure(
                    scenario,
                    f"invalid cookie role contest: {observation}",
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
        print(
            f"ROLE {scenario.name} robotweax={robotweax_role} "
            f"sender_cookie=0x{int(observation['sender_cookie']):08x} "
            f"receiver_cookie=0x"
            f"{int(observation['receiver_cookie']):08x}",
            flush=True,
        )
    if scenario.minimum_data_key_transitions > 0:
        if trace_proxy is None:
            raise RuntimeError(
                f"{scenario.name}: key-transition trace relay was not created"
            )
        key_observation = trace_proxy.data_key_observation(
            "sender_to_receiver"
        )
        transitions = key_observation.get("transitions")
        if (
            key_observation.get("complete") is not True
            or key_observation.get("unencrypted_packets") != 0
            or type(transitions) is not int
            or transitions < scenario.minimum_data_key_transitions
        ):
            raise RuntimeError(
                render_failure(
                    scenario,
                    "DATA-key trace did not prove authenticated rotation: "
                    f"{key_observation}",
                    output_path,
                    sender_stdout,
                    sender_stderr,
                    receiver_stdout,
                    receiver_stderr,
                    handshake_trace,
                )
            )
        print(
            f"KEYS {scenario.name} transitions={transitions} "
            f"selectors={key_observation['selectors']}",
            flush=True,
        )
    print(
        f"PASS {scenario.name} sha256={expected_digest} "
        f"packets_sent={packets_sent} packets_received={packets_received}",
        flush=True,
    )
    return robotweax_role


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="numeric loopback address used by both rendezvous peers",
    )
    parser.add_argument(
        "--ipv6-matrix-only",
        action="store_true",
        help="run the focused IPv6 baseline and fault matrix only",
    )
    parser.add_argument("--bytes", type=int, default=4 * 1_024 * 1_024)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument("--km-refresh-rate", type=int, default=1_000)
    parser.add_argument("--km-preannounce", type=int, default=400)
    parser.add_argument(
        "--input-bw",
        type=int,
        default=DEFAULT_LOOPBACK_INPUT_BANDWIDTH,
    )
    parser.add_argument("--shutdown-grace-ms", type=int, default=250)
    parser.add_argument("--timing-delay-ms", type=int, default=350)
    parser.add_argument("--fault-delay-ms", type=int, default=40)
    parser.add_argument("--role-probe-attempts", type=int, default=16)
    parser.add_argument(
        "--role-probe-bytes",
        type=int,
        default=128 * 1_024,
    )
    parser.add_argument(
        "--scenario",
        action="append",
        default=[],
        help="run only the named scenario; may be repeated",
    )
    arguments = parser.parse_args()

    try:
        host_address = ipaddress.ip_address(arguments.host)
    except ValueError as error:
        parser.error(str(error))
    if arguments.ipv6_matrix_only and host_address.version != 6:
        parser.error("--ipv6-matrix-only requires an IPv6 --host")

    if (
        arguments.bytes <= 0
        or arguments.timeout_seconds <= 0
        or arguments.km_refresh_rate <= 0
        or arguments.km_preannounce <= 0
        or arguments.input_bw <= 0
        or arguments.shutdown_grace_ms < 0
        or arguments.timing_delay_ms < 0
        or arguments.fault_delay_ms <= 0
        or arguments.role_probe_attempts < 0
        or arguments.role_probe_bytes <= 0
        or arguments.km_preannounce
        > (arguments.km_refresh_rate - 1) // 2
    ):
        parser.error("invalid transfer or key-rotation parameters")

    try:
        robotweax = resolve_program_path(arguments.robotweax_peer)
        reference = resolve_program_path(arguments.reference_peer)
        scenarios = (
            ipv6_scenario_matrix(robotweax, reference)
            if arguments.ipv6_matrix_only
            else (
                scenario_matrix(robotweax, reference)
                + timing_matrix(
                    robotweax,
                    reference,
                    arguments.timing_delay_ms,
                )
                + fault_matrix(
                    robotweax,
                    reference,
                    arguments.fault_delay_ms,
                )
            )
        )
        selected_run = bool(arguments.scenario)
        if arguments.scenario:
            selected = set(arguments.scenario)
            known = {scenario.name for scenario in scenarios}
            unknown = sorted(selected - known)
            if unknown:
                raise RuntimeError(
                    "unknown rendezvous scenario(s): " + ", ".join(unknown)
                )
            scenarios = [
                scenario
                for scenario in scenarios
                if scenario.name in selected
            ]
        options = RunOptions(
            byte_count=arguments.bytes,
            timeout_seconds=arguments.timeout_seconds,
            key_refresh_rate=arguments.km_refresh_rate,
            key_preannouncement=arguments.km_preannounce,
            input_bandwidth_bytes_per_second=arguments.input_bw,
            shutdown_grace_milliseconds=arguments.shutdown_grace_ms,
        )
        environment = os.environ.copy()
        environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)
        with tempfile.TemporaryDirectory(
            prefix="robotweax-srt-rendezvous-interop-"
        ) as directory:
            work = Path(directory)
            robotweax_roles: set[str] = set()
            for scenario in scenarios:
                role = run_scenario(
                    scenario,
                    options,
                    work,
                    environment,
                    arguments.host,
                )
                if role is not None:
                    robotweax_roles.add(role)

            required_roles = {"initiator", "responder"}
            if not selected_run:
                probe_options = replace(
                    options,
                    byte_count=arguments.role_probe_bytes,
                )
                for attempt in range(arguments.role_probe_attempts):
                    if robotweax_roles == required_roles:
                        break
                    role = run_scenario(
                        role_probe_scenario(
                            robotweax,
                            reference,
                            attempt,
                            arguments.timing_delay_ms,
                        ),
                        probe_options,
                        work,
                        environment,
                        arguments.host,
                    )
                    if role is not None:
                        robotweax_roles.add(role)
                if robotweax_roles != required_roles:
                    missing = sorted(required_roles - robotweax_roles)
                    raise RuntimeError(
                        "Robotweax rendezvous cookie-role coverage is "
                        "incomplete after "
                        f"{arguments.role_probe_attempts} probes; "
                        "missing: "
                        + ", ".join(missing)
                    )
                print(
                    "PASS robotweax-cookie-role-coverage "
                    "roles=initiator,responder",
                    flush=True,
                )
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
