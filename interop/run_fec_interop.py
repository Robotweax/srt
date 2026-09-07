#!/usr/bin/env python3
"""Clear and encrypted FEC interop against pinned Haivision SRT."""

from __future__ import annotations

import argparse
import json
import os
import secrets
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Callable, Protocol

from interop_common import (
    expected_live_packets,
    file_sha256,
    free_udp_port,
    nonnegative_statistic,
    parse_complete,
    reserved_udp_ports,
    resolve_program_path,
    terminate,
    write_deterministic_payload,
)
from run_live_timing_interop import TimingRendezvousTraceProxy
from srt_handshake_trace import (
    CallerListenerFaultProxy,
    CallerListenerSequenceProxy,
    RendezvousFault,
    RendezvousTraceProxy,
)


ROW_COLUMNS = 10
ROW_PACKET_FILTER = (
    f"fec,cols:{ROW_COLUMNS},rows:1,layout:even,arq:onreq"
)
COLUMN_COLUMNS = 10
COLUMN_ROWS = 5
COLUMN_PACKET_FILTER = (
    f"fec,cols:{COLUMN_COLUMNS},rows:-{COLUMN_ROWS},"
    "layout:even,arq:onreq"
)
MATRIX_COLUMNS = 10
MATRIX_ROWS = 5
MATRIX_PACKET_FILTER = (
    f"fec,cols:{MATRIX_COLUMNS},rows:{MATRIX_ROWS},"
    "layout:even,arq:onreq"
)
# Haivision v1.5.5 rebuilds a Rendezvous packet-filter string from an ordered
# parameter map after the first negotiation pass, then compares a repeated
# proposal byte-for-byte. Supplying that stable form up front prevents an
# otherwise equivalent repeated HSREQ from being rejected.
RENDEZVOUS_PACKET_FILTERS = {
    "row": (
        f"fec,arq:onreq,cols:{ROW_COLUMNS},layout:even,rows:1"
    ),
    "column": (
        f"fec,arq:onreq,cols:{COLUMN_COLUMNS},layout:even,"
        f"rows:-{COLUMN_ROWS}"
    ),
    "matrix": (
        f"fec,arq:onreq,cols:{MATRIX_COLUMNS},layout:even,"
        f"rows:{MATRIX_ROWS}"
    ),
}
# Seven 188-byte MPEG transport-stream packets are the canonical live SRT
# application message. This is an interoperability profile, not a Robotweax
# payload-size limit.
MPEG_TS_PACKET_SIZE = 188
MPEG_TS_PACKETS_PER_MESSAGE = 7
SOURCE_PACKET_SIZE = (
    MPEG_TS_PACKET_SIZE * MPEG_TS_PACKETS_PER_MESSAGE
)
# Keep receive capacity independent of the profiled application message. With
# IPv4 MSS 1500, the SRT data header and packet-filter header leave 1452 bytes.
FEC_RECEIVE_CAPACITY = 1_452
DROP_SOURCE_OCCURRENCE = 5
SEQUENCE_MODULUS = 1 << 31
SEQUENCE_MASK = SEQUENCE_MODULUS - 1
PASSPHRASE_ENVIRONMENT = "SRT_INTEROP_PASSPHRASE"
ENCRYPTED_FEC_GROUPS = 3
ENCRYPTED_FEC_KEY_LENGTH = 32
ENCRYPTED_FEC_KEY_REFRESH_RATE = 50
ENCRYPTED_FEC_KEY_PREANNOUNCEMENT = 20
ENCRYPTED_FEC_MINIMUM_KEY_TRANSITIONS = 2
ENCRYPTED_RENDEZVOUS_GEOMETRIES = frozenset(
    {
        "encrypted-row-rendezvous",
        "encrypted-column-rendezvous",
        "encrypted-matrix-rendezvous",
    }
)
# Haivision documents that the Rendezvous initiator may consider itself
# connected immediately after sending AGREEMENT while the responder is still
# completing its receive-path handoff (an "initial tearing" window). The
# general Rendezvous matrix deliberately exercises immediate DATA. This FEC
# matrix instead isolates parity recovery, so settle only a Robotweax sender
# whose reference receiver can still be in that handoff window.
REFERENCE_RENDEZVOUS_SETTLE_MILLISECONDS = 50
FEC_RECOVERY_ONLY = "fec-only"
ARQ_HANDOFF = "arq-handoff"
RECOVERY_POLICIES = frozenset({FEC_RECOVERY_ONLY, ARQ_HANDOFF})
ROLLOVER_TARGET_ISN = SEQUENCE_MASK - 5


@dataclass(frozen=True)
class Scenario:
    name: str
    caller: Path
    listener: Path
    seed: int
    geometry: str
    packet_filter: str
    source_packets_per_group: int
    control_packets_per_group: int
    fault_occurrences: tuple[int, ...] = (
        DROP_SOURCE_OCCURRENCE,
    )
    expected_sequence_offsets: tuple[int, ...] = (0,)
    expected_reconstructions: int = 1
    expected_filter_losses: int = 0
    recovery_policy: str = FEC_RECOVERY_ONLY
    expected_arq_sequence_offsets: tuple[int, ...] = ()
    accept_reference_row_arq_expansion: bool = False
    byte_count_multiplier: int = 1
    key_length: int = 0
    minimum_key_transitions: int = 0
    crypto_mode: str | None = None
    # Harness-only policy for the pinned reference Sender. Robotweax always
    # remains acknowledgement-before-use strict in production and interop.
    accept_reference_deferred_key_responses: bool = False
    translated_initial_sequence: int | None = None
    rollover: bool = False

    def __post_init__(self) -> None:
        if (
            not self.fault_occurrences
            or len(self.fault_occurrences)
                != len(self.expected_sequence_offsets)
            or self.expected_sequence_offsets[0] != 0
            or any(
                occurrence <= 0
                for occurrence in self.fault_occurrences
            )
            or self.source_packets_per_group <= 0
            or self.control_packets_per_group <= 0
            or self.expected_reconstructions < 0
            or self.expected_filter_losses < 0
            or self.recovery_policy not in RECOVERY_POLICIES
            or (
                self.recovery_policy == FEC_RECOVERY_ONLY
                and (
                    self.expected_reconstructions <= 0
                    or self.expected_filter_losses != 0
                    or self.expected_arq_sequence_offsets
                )
            )
            or (
                self.recovery_policy == ARQ_HANDOFF
                and (
                    self.expected_reconstructions != 0
                    or len(self.fault_occurrences) < 2
                    or not self.expected_arq_sequence_offsets
                    or self.expected_arq_sequence_offsets[0] != 0
                    or tuple(sorted(set(
                        self.expected_arq_sequence_offsets
                    ))) != self.expected_arq_sequence_offsets
                    or not set(self.expected_sequence_offsets).issubset(
                        self.expected_arq_sequence_offsets
                    )
                    or self.expected_filter_losses
                        != len(self.expected_arq_sequence_offsets)
                )
            )
            or (
                self.accept_reference_row_arq_expansion
                and (
                    self.recovery_policy != ARQ_HANDOFF
                    or self.geometry != "encrypted-row-rendezvous"
                    or self.expected_sequence_offsets != (0, 1)
                    or self.expected_arq_sequence_offsets != (0, 1, 2)
                    or self.key_length == 0
                )
            )
            or self.byte_count_multiplier <= 0
            or self.key_length not in (0, 16, 24, 32)
            or (
                self.key_length == 0
                and self.minimum_key_transitions != 0
            )
            or (
                self.key_length != 0
                and self.minimum_key_transitions <= 0
            )
            or self.crypto_mode not in (None, "ctr", "gcm")
            or (self.crypto_mode is not None and self.key_length == 0)
            or (
                self.accept_reference_deferred_key_responses
                and (
                    self.geometry not in ENCRYPTED_RENDEZVOUS_GEOMETRIES
                    or self.key_length == 0
                )
            )
            or (
                self.translated_initial_sequence is None
                and self.rollover
            )
            or (
                self.translated_initial_sequence is not None
                and (
                    not self.rollover
                    or not (
                        0
                        <= self.translated_initial_sequence
                        <= SEQUENCE_MASK
                    )
                )
            )
        ):
            raise ValueError("invalid FEC interoperability scenario")

    def expected_control_packets(self, source_packets: int) -> int:
        return (
            source_packets // self.source_packets_per_group
        ) * self.control_packets_per_group


@dataclass(frozen=True)
class RunOptions:
    byte_count: int = SOURCE_PACKET_SIZE * 50
    timeout_seconds: int = 30
    key_refresh_rate: int = ENCRYPTED_FEC_KEY_REFRESH_RATE
    key_preannouncement: int = (
        ENCRYPTED_FEC_KEY_PREANNOUNCEMENT
    )


@dataclass(frozen=True)
class RendezvousScenario:
    profile: Scenario
    sender: Path
    receiver: Path
    robotweax_peer: Path
    start_order: str = "receiver-first"

    def __post_init__(self) -> None:
        if (
            self.profile.caller != self.sender
            or self.profile.listener != self.receiver
            or self.start_order not in (
                "sender-first",
                "receiver-first",
            )
            or self.robotweax_peer not in (
                self.sender,
                self.receiver,
            )
            or (
                self.profile.geometry in ENCRYPTED_RENDEZVOUS_GEOMETRIES
                and (
                    self.profile.accept_reference_deferred_key_responses
                    != (self.sender != self.robotweax_peer)
                )
            )
            or (
                self.profile.recovery_policy == ARQ_HANDOFF
                and (
                    self.profile.accept_reference_row_arq_expansion
                    != (self.receiver != self.robotweax_peer)
                )
            )
        ):
            raise ValueError("invalid FEC rendezvous scenario")


class FaultObservationSource(Protocol):
    def fault_observations(self) -> list[dict[str, object]]: ...


class ArqObservationSource(FaultObservationSource, Protocol):
    def loss_report_observations(self) -> list[dict[str, object]]: ...

    def retransmission_observations(
        self, direction: str
    ) -> list[dict[str, object]]: ...

    def arq_trace_complete(self) -> bool: ...


class RolloverObservationSource(
    FaultObservationSource,
    Protocol,
):
    def sequence_observation(self) -> dict[str, object]: ...


def scenario_matrix(robotweax: Path, reference: Path) -> list[Scenario]:
    return [
        Scenario(
            name="row-fec-robotweax-to-haivision-source-drop",
            caller=robotweax,
            listener=reference,
            seed=70_001,
            geometry="row",
            packet_filter=ROW_PACKET_FILTER,
            source_packets_per_group=ROW_COLUMNS,
            control_packets_per_group=1,
        ),
        Scenario(
            name="row-fec-haivision-to-robotweax-source-drop",
            caller=reference,
            listener=robotweax,
            seed=70_002,
            geometry="row",
            packet_filter=ROW_PACKET_FILTER,
            source_packets_per_group=ROW_COLUMNS,
            control_packets_per_group=1,
        ),
        Scenario(
            name="column-fec-robotweax-to-haivision-source-drop",
            caller=robotweax,
            listener=reference,
            seed=71_001,
            geometry="column",
            packet_filter=COLUMN_PACKET_FILTER,
            source_packets_per_group=COLUMN_COLUMNS * COLUMN_ROWS,
            control_packets_per_group=COLUMN_COLUMNS,
        ),
        Scenario(
            name="column-fec-haivision-to-robotweax-source-drop",
            caller=reference,
            listener=robotweax,
            seed=71_002,
            geometry="column",
            packet_filter=COLUMN_PACKET_FILTER,
            source_packets_per_group=COLUMN_COLUMNS * COLUMN_ROWS,
            control_packets_per_group=COLUMN_COLUMNS,
        ),
        Scenario(
            name="matrix-fec-robotweax-to-haivision-recursive-drop",
            caller=robotweax,
            listener=reference,
            seed=72_001,
            geometry="matrix",
            packet_filter=MATRIX_PACKET_FILTER,
            source_packets_per_group=MATRIX_COLUMNS * MATRIX_ROWS,
            control_packets_per_group=MATRIX_ROWS + MATRIX_COLUMNS,
            # Source positions (row, column): (0, 0), (0, 1), (1, 1).
            # The relay trace pins source position (1, 1) to the twelfth
            # original-source data-header datagram on the wire.
            fault_occurrences=(1, 2, 12),
            expected_sequence_offsets=(0, 1, MATRIX_COLUMNS + 1),
            expected_reconstructions=3,
        ),
        Scenario(
            name="matrix-fec-haivision-to-robotweax-recursive-drop",
            caller=reference,
            listener=robotweax,
            seed=72_002,
            geometry="matrix",
            packet_filter=MATRIX_PACKET_FILTER,
            source_packets_per_group=MATRIX_COLUMNS * MATRIX_ROWS,
            control_packets_per_group=MATRIX_ROWS + MATRIX_COLUMNS,
            fault_occurrences=(1, 2, 12),
            expected_sequence_offsets=(0, 1, MATRIX_COLUMNS + 1),
            expected_reconstructions=3,
        ),
    ]


def encrypted_matrix_scenarios(
    robotweax: Path,
    reference: Path,
) -> list[Scenario]:
    wire_packets_per_group = (
        MATRIX_COLUMNS * MATRIX_ROWS
        + MATRIX_ROWS
        + MATRIX_COLUMNS
    )
    group_fault_occurrences = (1, 2, 12)
    group_sequence_offsets = (0, 1, MATRIX_COLUMNS + 1)
    fault_occurrences = tuple(
        group * wire_packets_per_group + occurrence
        for group in range(ENCRYPTED_FEC_GROUPS)
        for occurrence in group_fault_occurrences
    )
    expected_sequence_offsets = tuple(
        group * wire_packets_per_group + offset
        for group in range(ENCRYPTED_FEC_GROUPS)
        for offset in group_sequence_offsets
    )

    def scenario(
        name: str,
        caller: Path,
        listener: Path,
        seed: int,
    ) -> Scenario:
        return Scenario(
            name=name,
            caller=caller,
            listener=listener,
            seed=seed,
            geometry="encrypted-matrix",
            packet_filter=MATRIX_PACKET_FILTER,
            source_packets_per_group=MATRIX_COLUMNS * MATRIX_ROWS,
            control_packets_per_group=MATRIX_ROWS + MATRIX_COLUMNS,
            fault_occurrences=fault_occurrences,
            expected_sequence_offsets=expected_sequence_offsets,
            expected_reconstructions=3 * ENCRYPTED_FEC_GROUPS,
            byte_count_multiplier=ENCRYPTED_FEC_GROUPS,
            key_length=ENCRYPTED_FEC_KEY_LENGTH,
            minimum_key_transitions=(
                ENCRYPTED_FEC_MINIMUM_KEY_TRANSITIONS
            ),
        )

    return [
        scenario(
            "aes256-matrix-fec-robotweax-to-haivision-rotation",
            robotweax,
            reference,
            73_001,
        ),
        scenario(
            "aes256-matrix-fec-haivision-to-robotweax-rotation",
            reference,
            robotweax,
            73_002,
        ),
    ]


def encrypted_rendezvous_scenarios(
    robotweax: Path,
    reference: Path,
) -> list[RendezvousScenario]:
    def scenario(
        geometry: str,
        source_packets_per_group: int,
        control_packets_per_group: int,
        sender: Path,
        receiver: Path,
        seed: int,
        start_order: str,
        *,
        burst: bool = False,
    ) -> RendezvousScenario:
        if (sender, receiver) == (robotweax, reference):
            direction = "robotweax-to-haivision"
        elif (sender, receiver) == (reference, robotweax):
            direction = "haivision-to-robotweax"
        else:
            raise ValueError("invalid encrypted FEC peer direction")
        fault_occurrences = (
            (DROP_SOURCE_OCCURRENCE, DROP_SOURCE_OCCURRENCE + 1)
            if geometry == "row" and burst
            else (
                (
                    DROP_SOURCE_OCCURRENCE,
                    DROP_SOURCE_OCCURRENCE + 1,
                    DROP_SOURCE_OCCURRENCE + 2,
                )
                if burst
                else (DROP_SOURCE_OCCURRENCE,)
            )
        )
        expected_reconstructions = (
            0 if geometry == "row" and burst
            else len(fault_occurrences)
        )
        reference_row_arq_expansion = (
            geometry == "row" and burst and receiver == reference
        )
        expected_arq_sequence_offsets = (
            tuple(range(3 if reference_row_arq_expansion else 2))
            if geometry == "row" and burst
            else ()
        )
        profile = Scenario(
            name=(
                f"aes256-{geometry}-fec-rendezvous-{direction}"
                + ("-burst" if burst else "")
            ),
            caller=sender,
            listener=receiver,
            seed=seed,
            geometry=f"encrypted-{geometry}-rendezvous",
            packet_filter=RENDEZVOUS_PACKET_FILTERS[geometry],
            source_packets_per_group=source_packets_per_group,
            control_packets_per_group=control_packets_per_group,
            fault_occurrences=fault_occurrences,
            expected_sequence_offsets=tuple(
                range(len(fault_occurrences))
            ),
            expected_reconstructions=expected_reconstructions,
            expected_filter_losses=(
                len(expected_arq_sequence_offsets)
                if geometry == "row" and burst
                else 0
            ),
            recovery_policy=(
                ARQ_HANDOFF
                if geometry == "row" and burst
                else FEC_RECOVERY_ONLY
            ),
            expected_arq_sequence_offsets=(
                expected_arq_sequence_offsets
            ),
            accept_reference_row_arq_expansion=(
                reference_row_arq_expansion
            ),
            byte_count_multiplier=ENCRYPTED_FEC_GROUPS,
            key_length=ENCRYPTED_FEC_KEY_LENGTH,
            minimum_key_transitions=(
                ENCRYPTED_FEC_MINIMUM_KEY_TRANSITIONS
            ),
            # At high send rates v1.5.5 can select preannounced material
            # before the corresponding KMRSP crosses the reverse relay path.
            accept_reference_deferred_key_responses=(
                sender == reference
            ),
        )
        return RendezvousScenario(
            profile=profile,
            sender=sender,
            receiver=receiver,
            robotweax_peer=robotweax,
            start_order=start_order,
        )

    geometries = (
        (
            "matrix",
            MATRIX_COLUMNS * MATRIX_ROWS,
            MATRIX_ROWS + MATRIX_COLUMNS,
            78_000,
        ),
        ("row", ROW_COLUMNS, 1, 78_100),
        (
            "column",
            COLUMN_COLUMNS * COLUMN_ROWS,
            COLUMN_COLUMNS,
            78_200,
        ),
    )
    scenarios: list[RendezvousScenario] = []
    for geometry, source_group, control_group, seed in geometries:
        scenarios.extend(
            (
                scenario(
                    geometry,
                    source_group,
                    control_group,
                    robotweax,
                    reference,
                    seed + 1,
                    "sender-first",
                ),
                scenario(
                    geometry,
                    source_group,
                    control_group,
                    reference,
                    robotweax,
                    seed + 2,
                    "receiver-first",
                ),
                scenario(
                    geometry,
                    source_group,
                    control_group,
                    robotweax,
                    reference,
                    seed + 11,
                    "receiver-first",
                    burst=True,
                ),
                scenario(
                    geometry,
                    source_group,
                    control_group,
                    reference,
                    robotweax,
                    seed + 12,
                    "sender-first",
                    burst=True,
                ),
            )
        )
    return scenarios


def rollover_scenarios(
    robotweax: Path,
    reference: Path,
) -> list[Scenario]:
    def scenario(
        name: str,
        caller: Path,
        listener: Path,
        seed: int,
        geometry: str,
        packet_filter: str,
        source_packets_per_group: int,
        control_packets_per_group: int,
        fault_occurrences: tuple[int, ...],
        expected_sequence_offsets: tuple[int, ...],
        expected_reconstructions: int,
    ) -> Scenario:
        return Scenario(
            name=name,
            caller=caller,
            listener=listener,
            seed=seed,
            geometry=geometry,
            packet_filter=packet_filter,
            source_packets_per_group=source_packets_per_group,
            control_packets_per_group=control_packets_per_group,
            fault_occurrences=fault_occurrences,
            expected_sequence_offsets=expected_sequence_offsets,
            expected_reconstructions=expected_reconstructions,
            translated_initial_sequence=ROLLOVER_TARGET_ISN,
            rollover=True,
        )

    scenarios: list[Scenario] = []
    directions = (
        ("robotweax-to-haivision", robotweax, reference, 74_001),
        ("haivision-to-robotweax", reference, robotweax, 74_002),
    )
    for label, caller, listener, seed in directions:
        scenarios.extend(
            (
                scenario(
                    f"row-fec-rollover-{label}",
                    caller,
                    listener,
                    seed,
                    "row-rollover",
                    ROW_PACKET_FILTER,
                    ROW_COLUMNS,
                    1,
                    (8,),
                    (0,),
                    1,
                ),
                scenario(
                    f"column-fec-rollover-{label}",
                    caller,
                    listener,
                    seed + 100,
                    "column-rollover",
                    COLUMN_PACKET_FILTER,
                    COLUMN_COLUMNS * COLUMN_ROWS,
                    COLUMN_COLUMNS,
                    (12,),
                    (0,),
                    1,
                ),
                scenario(
                    f"matrix-fec-rollover-{label}",
                    caller,
                    listener,
                    seed + 200,
                    "matrix-rollover",
                    MATRIX_PACKET_FILTER,
                    MATRIX_COLUMNS * MATRIX_ROWS,
                    MATRIX_ROWS + MATRIX_COLUMNS,
                    (1, 2, 12),
                    (0, 1, MATRIX_COLUMNS + 1),
                    3,
                ),
            )
        )
    return scenarios


def rendezvous_scenarios(
    robotweax: Path,
    reference: Path,
) -> list[RendezvousScenario]:
    scenarios: list[RendezvousScenario] = []
    for index, profile in enumerate(
        scenario_matrix(robotweax, reference)
    ):
        rendezvous_profile = replace(
            profile,
            name=profile.name.replace("-fec-", "-fec-rendezvous-"),
            seed=profile.seed + 5_000,
            packet_filter=RENDEZVOUS_PACKET_FILTERS[
                profile.geometry
            ],
        )
        scenarios.append(
            RendezvousScenario(
                profile=rendezvous_profile,
                sender=rendezvous_profile.caller,
                receiver=rendezvous_profile.listener,
                robotweax_peer=robotweax,
                start_order=(
                    "sender-first"
                    if index % 2 == 0
                    else "receiver-first"
                ),
            )
        )
    return scenarios


def rendezvous_role_probe_scenario(
    robotweax: Path,
    reference: Path,
    attempt: int,
) -> RendezvousScenario:
    if attempt < 0:
        raise ValueError("role-probe attempt must not be negative")
    combination = attempt % 4
    robotweax_sends = combination < 2
    sender = robotweax if robotweax_sends else reference
    receiver = reference if robotweax_sends else robotweax
    profile = Scenario(
        name=f"row-fec-rendezvous-cookie-role-probe-{attempt + 1}",
        caller=sender,
        listener=receiver,
        seed=76_000 + attempt,
        geometry="row-rendezvous-role-probe",
        packet_filter=RENDEZVOUS_PACKET_FILTERS["row"],
        source_packets_per_group=ROW_COLUMNS,
        control_packets_per_group=1,
    )
    return RendezvousScenario(
        profile=profile,
        sender=sender,
        receiver=receiver,
        robotweax_peer=robotweax,
        start_order=(
            "sender-first"
            if combination % 2 == 0
            else "receiver-first"
        ),
    )


def encrypted_rendezvous_role_probe_scenario(
    robotweax: Path,
    reference: Path,
    attempt: int,
) -> RendezvousScenario:
    if attempt < 0:
        raise ValueError("role-probe attempt must not be negative")
    combination = attempt % 4
    robotweax_sends = combination < 2
    sender = robotweax if robotweax_sends else reference
    receiver = reference if robotweax_sends else robotweax
    profile = replace(
        encrypted_rendezvous_scenarios(robotweax, reference)[0].profile,
        name=(
            "aes256-matrix-fec-rendezvous-cookie-role-probe-"
            f"{attempt + 1}"
        ),
        caller=sender,
        listener=receiver,
        seed=79_000 + attempt,
        accept_reference_deferred_key_responses=(
            sender == reference
        ),
    )
    return RendezvousScenario(
        profile=profile,
        sender=sender,
        receiver=receiver,
        robotweax_peer=robotweax,
        start_order=(
            "sender-first"
            if combination % 2 == 0
            else "receiver-first"
        ),
    )


def peer_command(
    scenario: Scenario,
    program: Path,
    role: str,
    port: int,
    payload_path: Path,
    options: RunOptions,
) -> list[str]:
    if role not in ("caller", "listener"):
        raise ValueError(f"unsupported role: {role}")
    path_option = "--input" if role == "caller" else "--output"
    command = [
        str(program),
        role,
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--bytes",
        str(options.byte_count),
        path_option,
        str(payload_path),
        "--transport",
        "live",
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
        "--chunk-size",
        str(SOURCE_PACKET_SIZE),
        "--receive-size",
        str(FEC_RECEIVE_CAPACITY),
        "--packet-filter",
        scenario.packet_filter,
        "--latency-ms",
        "120",
        "--shutdown-grace-ms",
        "250",
    ]
    if scenario.key_length != 0:
        command.extend(
            (
                "--passphrase-env",
                PASSPHRASE_ENVIRONMENT,
                "--pbkeylen",
                str(scenario.key_length),
                "--km-refresh-rate",
                str(options.key_refresh_rate),
                "--km-preannounce",
                str(options.key_preannouncement),
            )
        )
    if scenario.crypto_mode is not None:
        command.extend(
            (
                "--crypto-mode",
                scenario.crypto_mode,
                "--expect-crypto-mode",
                scenario.crypto_mode,
            )
        )
    return command


def rendezvous_peer_command(
    scenario: RendezvousScenario,
    program: Path,
    role: str,
    local_port: int,
    peer_port: int,
    payload_path: Path,
    options: RunOptions,
) -> list[str]:
    if role not in (
        "rendezvous-sender",
        "rendezvous-receiver",
    ):
        raise ValueError(f"unsupported rendezvous role: {role}")
    profile = scenario.profile
    path_option = (
        "--input" if role == "rendezvous-sender" else "--output"
    )
    command = [
        str(program),
        role,
        "--local-host",
        "127.0.0.1",
        "--local-port",
        str(local_port),
        "--host",
        "127.0.0.1",
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
        str(SOURCE_PACKET_SIZE),
        "--receive-size",
        str(FEC_RECEIVE_CAPACITY),
        "--packet-filter",
        profile.packet_filter,
        "--latency-ms",
        "120",
        "--shutdown-grace-ms",
        "250",
    ]
    if profile.key_length != 0:
        command.extend(
            (
                "--passphrase-env",
                PASSPHRASE_ENVIRONMENT,
                "--pbkeylen",
                str(profile.key_length),
                "--km-refresh-rate",
                str(options.key_refresh_rate),
                "--km-preannounce",
                str(options.key_preannouncement),
            )
        )
    if profile.crypto_mode is not None:
        command.extend(
            (
                "--crypto-mode",
                profile.crypto_mode,
                "--expect-crypto-mode",
                profile.crypto_mode,
            )
        )
    if (
        role == "rendezvous-sender"
        and scenario.receiver != scenario.robotweax_peer
    ):
        command.extend(
            (
                "--sender-start-delay-ms",
                str(
                    REFERENCE_RENDEZVOUS_SETTLE_MILLISECONDS
                ),
            )
        )
    return command


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
            and not isinstance(port, bool)
            and 0 < port <= 65_535
        ):
            return port
    return None


def filtercap_directions(trace: str) -> set[str]:
    directions: set[str] = set()
    for line in trace.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        extensions = event.get("extensions")
        direction = event.get("direction")
        if not isinstance(extensions, list) or not isinstance(direction, str):
            continue
        if any(
            isinstance(extension, dict)
            and extension.get("name") == "FILTER"
            for extension in extensions
        ):
            directions.add(direction)
    return directions


def runtime_key_material_names(trace: str) -> list[tuple[str, str]]:
    names: list[tuple[str, str]] = []
    for line in trace.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        name = event.get("name")
        direction = event.get("direction")
        if (
            event.get("event") == "srt_runtime_key_material_trace"
            and name in ("KMREQ", "KMRSP")
            and isinstance(direction, str)
        ):
            names.append((str(name), direction))
    return names


def trace_events(trace: str) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for line in trace.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events


def key_material_identity(
    event: dict[str, object],
) -> tuple[int, str] | None:
    material = event.get("key_material")
    if not isinstance(material, dict):
        return None
    selection = material.get("key_selection")
    digest = material.get("content_sha256")
    if (
        type(selection) is not int
        or selection not in (1, 2, 3)
        or not isinstance(digest, str)
        or len(digest) != 64
        or any(character not in "0123456789abcdef" for character in digest)
    ):
        return None
    return selection, digest


def validate_transition_trace(
    scenario: Scenario,
    trace: str,
) -> int | None:
    events = trace_events(trace)
    transitions = [
        event
        for event in events
        if event.get("event") == "srt_data_key_transition_trace"
    ]
    if not transitions:
        return None
    if any(
        event.get("event")
        in {
            "srt_control_trace_truncated",
            "srt_data_key_transition_trace_truncated",
            "srt_handshake_trace_error",
            "srt_runtime_key_material_trace_truncated",
        }
        for event in events
    ):
        raise RuntimeError(
            f"{scenario.name}: encrypted Rendezvous trace is incomplete"
        )

    selections = [event.get("key_selection") for event in transitions]
    directions = {event.get("direction") for event in transitions}
    ordinals = [event.get("relay_ordinal") for event in transitions]
    sequences = [event.get("sequence") for event in transitions]
    socket_ids = [
        event.get("destination_socket_id") for event in transitions
    ]
    if (
        len(transitions) < scenario.minimum_key_transitions + 1
        or directions != {"sender_to_receiver"}
        or any(type(value) is not int for value in selections)
        or any(value not in (1, 2) for value in selections)
        or any(type(value) is not int for value in ordinals)
        or any(
            type(value) is not int or not 0 <= value <= SEQUENCE_MASK
            for value in sequences
        )
        or any(type(value) is not int or value <= 0 for value in socket_ids)
        or len(set(socket_ids)) != 1
        or any(
            current <= previous
            for previous, current in zip(
                ordinals, ordinals[1:], strict=False
            )
        )
        or any(
            current == previous
            for previous, current in zip(
                selections, selections[1:], strict=False
            )
        )
    ):
        raise RuntimeError(
            f"{scenario.name}: DATA does not prove repeated key rotation"
        )

    first_data = [
        event
        for event in events
        if event.get("event") == "srt_first_data_trace"
        and event.get("direction") == "sender_to_receiver"
    ]
    if len(first_data) != 1 or any(
        first_data[0].get(field) != transitions[0].get(field)
        for field in (
            "relay_ordinal",
            "sequence",
            "destination_socket_id",
            "key_selection",
        )
    ):
        raise RuntimeError(
            f"{scenario.name}: selector trace misses first DATA"
        )

    key_events = [
        event
        for event in events
        if event.get("event") == "srt_runtime_key_material_trace"
    ]
    requests = [
        event
        for event in key_events
        if event.get("name") == "KMREQ"
        and event.get("direction") == "sender_to_receiver"
    ]
    responses = [
        event
        for event in key_events
        if event.get("name") == "KMRSP"
        and event.get("direction") == "receiver_to_sender"
    ]
    used_material: set[tuple[int, str]] = set()
    for previous, transition in zip(
        transitions, transitions[1:], strict=False
    ):
        previous_ordinal = int(previous["relay_ordinal"])
        data_ordinal = int(transition["relay_ordinal"])
        selection = int(transition["key_selection"])
        candidates: list[tuple[int, str, int]] = []
        for request in requests:
            identity = key_material_identity(request)
            request_ordinal = request.get("relay_ordinal")
            if (
                identity is None
                or identity in used_material
                or identity[0] not in (selection, 3)
                or type(request_ordinal) is not int
                or not previous_ordinal < request_ordinal < data_ordinal
            ):
                continue
            response_ordinals = [
                response.get("relay_ordinal")
                for response in responses
                if key_material_identity(response) == identity
                and type(response.get("relay_ordinal")) is int
                and request_ordinal
                < int(response["relay_ordinal"])
                and (
                    scenario.accept_reference_deferred_key_responses
                    or int(response["relay_ordinal"]) < data_ordinal
                )
            ]
            if response_ordinals:
                candidates.append(
                    (
                        identity[0],
                        identity[1],
                        min(int(value) for value in response_ordinals),
                    )
                )
        if not candidates:
            raise RuntimeError(
                f"{scenario.name}: DATA selector {selection} lacks a "
                "causal byte-identical KMREQ/KMRSP"
            )
        selected = max(candidates, key=lambda material: material[2])
        used_material.add((selected[0], selected[1]))

    return len(transitions) - 1


def validate_filter_statistics(
    scenario: Scenario,
    sender: dict[str, object],
    receiver: dict[str, object],
    source_packets: int,
) -> tuple[int, int]:
    expected_parity = scenario.expected_control_packets(
        source_packets
    )
    sender_extra = nonnegative_statistic(
        sender, "pktSndFilterExtraTotal"
    )
    receiver_extra = nonnegative_statistic(
        receiver, "pktRcvFilterExtraTotal"
    )
    receiver_supply = nonnegative_statistic(
        receiver, "pktRcvFilterSupplyTotal"
    )
    receiver_filter_loss = nonnegative_statistic(
        receiver, "pktRcvFilterLossTotal"
    )
    if sender_extra != expected_parity:
        raise RuntimeError(
            f"{scenario.name}: sender emitted {sender_extra} FEC control "
            f"packets, expected exactly {expected_parity}"
        )
    if receiver_extra != expected_parity:
        raise RuntimeError(
            f"{scenario.name}: receiver consumed {receiver_extra} FEC "
            f"control packets, expected exactly {expected_parity}"
        )
    if receiver_supply != scenario.expected_reconstructions:
        raise RuntimeError(
            f"{scenario.name}: receiver reported {receiver_supply} FEC "
            f"reconstructions, expected exactly "
            f"{scenario.expected_reconstructions}"
        )
    if receiver_filter_loss != scenario.expected_filter_losses:
        raise RuntimeError(
            f"{scenario.name}: receiver finalized "
            f"{receiver_filter_loss} unrecovered FEC source packets, "
            f"expected {scenario.expected_filter_losses}"
        )
    return sender_extra, receiver_supply


def validate_no_arq_statistics(
    scenario: Scenario,
    sender: dict[str, object],
) -> None:
    retransmissions = nonnegative_statistic(
        sender, "pktRetransTotal"
    )
    received_naks = nonnegative_statistic(
        sender, "pktRecvNAKTotal"
    )
    if retransmissions is None or received_naks is None:
        raise RuntimeError(
            f"{scenario.name}: sender ARQ statistics are unavailable"
        )
    if retransmissions != 0 or received_naks != 0:
        raise RuntimeError(
            f"{scenario.name}: FEC recovery used ARQ "
            f"(retransmissions={retransmissions}, "
            f"received_naks={received_naks})"
        )


def validate_arq_handoff_statistics(
    scenario: Scenario,
    sender: dict[str, object],
) -> tuple[int, int]:
    if scenario.recovery_policy != ARQ_HANDOFF:
        raise ValueError("ARQ validation requires an ARQ-handoff scenario")
    retransmissions = nonnegative_statistic(
        sender, "pktRetransTotal"
    )
    received_naks = nonnegative_statistic(
        sender, "pktRecvNAKTotal"
    )
    if retransmissions is None or received_naks is None:
        raise RuntimeError(
            f"{scenario.name}: sender ARQ statistics are unavailable"
        )
    if (
        retransmissions < len(scenario.expected_arq_sequence_offsets)
        or received_naks < 1
    ):
        raise RuntimeError(
            f"{scenario.name}: FEC did not hand every loss to ARQ "
            f"(retransmissions={retransmissions}, "
            f"received_naks={received_naks})"
        )
    return retransmissions, received_naks


def validate_arq_wire_evidence(
    scenario: Scenario,
    relay: ArqObservationSource,
    first_dropped_sequence: int,
) -> tuple[int, int]:
    if scenario.recovery_policy != ARQ_HANDOFF:
        raise ValueError("wire ARQ validation requires an ARQ scenario")
    if not relay.arq_trace_complete():
        raise RuntimeError(
            f"{scenario.name}: bounded ARQ trace was truncated"
        )
    expected_sequences = {
        (first_dropped_sequence + offset) & SEQUENCE_MASK
        for offset in scenario.expected_arq_sequence_offsets
    }
    loss_ordinals: dict[int, list[int]] = {
        sequence: [] for sequence in expected_sequences
    }
    loss_reports = relay.loss_report_observations()
    for report in loss_reports:
        ranges = report.get("ranges")
        ordinal = report.get("relay_ordinal")
        if (
            report.get("direction") != "receiver_to_sender"
            or type(ordinal) is not int
            or ordinal <= 0
            or not isinstance(ranges, (list, tuple))
            or not ranges
        ):
            raise RuntimeError(
                f"{scenario.name}: invalid ARQ loss-report trace: "
                f"{report}"
            )
        for item in ranges:
            if (
                not isinstance(item, (list, tuple))
                or len(item) != 2
                or type(item[0]) is not int
                or type(item[1]) is not int
            ):
                raise RuntimeError(
                    f"{scenario.name}: invalid ARQ loss range: {item}"
                )
            start, end = int(item[0]), int(item[1])
            distance = (end - start) & SEQUENCE_MASK
            if distance >= len(expected_sequences):
                raise RuntimeError(
                    f"{scenario.name}: ARQ loss range is broader than "
                    f"the approved profile: {item}"
                )
            for offset in range(distance + 1):
                sequence = (start + offset) & SEQUENCE_MASK
                if sequence not in expected_sequences:
                    raise RuntimeError(
                        f"{scenario.name}: NAK included unexpected "
                        f"sequence {sequence}"
                    )
                loss_ordinals[sequence].append(ordinal)
    if not loss_reports or any(
        not ordinals for ordinals in loss_ordinals.values()
    ):
        raise RuntimeError(
            f"{scenario.name}: NAK coverage does not exactly include "
            f"{sorted(expected_sequences)}"
        )

    retransmission_sequences: set[int] = set()
    retransmissions = relay.retransmission_observations(
        "sender_to_receiver"
    )
    for retransmission in retransmissions:
        sequence = retransmission.get("sequence")
        ordinal = retransmission.get("relay_ordinal")
        original_ordinal = retransmission.get(
            "original_relay_ordinal"
        )
        if (
            type(sequence) is not int
            or sequence not in expected_sequences
            or type(ordinal) is not int
            or type(original_ordinal) is not int
            or not original_ordinal < ordinal
            or retransmission.get("original_observed") is not True
            or retransmission.get("ciphertext_matches_original")
                is not True
            or retransmission.get("key_selection")
                != retransmission.get("original_key_selection")
            or retransmission.get("payload_bytes")
                != retransmission.get("original_payload_bytes")
            or retransmission.get("payload_sha256")
                != retransmission.get("original_payload_sha256")
            or not any(
                loss_ordinal < ordinal
                for loss_ordinal in loss_ordinals.get(sequence, [])
            )
        ):
            raise RuntimeError(
                f"{scenario.name}: invalid or unexpected ARQ "
                f"retransmission trace: {retransmission}"
            )
        retransmission_sequences.add(sequence)
    if retransmission_sequences != expected_sequences:
        raise RuntimeError(
            f"{scenario.name}: retransmission sequences "
            f"{sorted(retransmission_sequences)} do not exactly match "
            f"{sorted(expected_sequences)}"
        )
    return len(loss_reports), len(retransmissions)


def validate_causal_recovery(
    scenario: Scenario,
    relay: FaultObservationSource,
) -> list[dict[str, object]]:
    observations = relay.fault_observations()
    if len(observations) != len(scenario.fault_occurrences):
        raise RuntimeError(
            f"{scenario.name}: expected "
            f"{len(scenario.fault_occurrences)} source-drop observations, "
            f"got {len(observations)}"
        )
    sequences: list[int] = []
    for expected_occurrence, observation in zip(
        scenario.fault_occurrences, observations, strict=True
    ):
        sequence = observation.get("sequence")
        if (
            observation.get("action") != "drop"
            or observation.get("direction") != "sender_to_receiver"
            or observation.get("occurrence") != expected_occurrence
            or observation.get("packet_kind") != "data"
            or observation.get("filter_control") is not False
            or observation.get("message_number") in (None, 0)
            or not isinstance(sequence, int)
        ):
            raise RuntimeError(
                f"{scenario.name}: injected fault was not the expected "
                f"original source packet: {observation}"
        )
        if scenario.recovery_policy == FEC_RECOVERY_ONLY:
            if "loss_report_observed" in observation:
                raise RuntimeError(
                    f"{scenario.name}: FEC-recovered source unexpectedly "
                    "caused a matching NAK"
                )
            if "retransmission_observed" in observation:
                raise RuntimeError(
                    f"{scenario.name}: FEC-recovered source unexpectedly "
                    "caused an ARQ retransmission"
                )
        else:
            loss_ordinal = observation.get("loss_report_relay_ordinal")
            retransmission_ordinal = observation.get(
                "retransmission_relay_ordinal"
            )
            original_ordinal = observation.get("relay_ordinal")
            key_selection = observation.get("key_selection")
            payload_bytes = observation.get("payload_bytes")
            payload_digest = observation.get("payload_sha256")
            if (
                observation.get("loss_report_observed") is not True
                or observation.get("retransmission_observed") is not True
                or observation.get("retransmission_flag") is not True
                or observation.get("retransmission_ciphertext_matches")
                    is not True
                or type(key_selection) is not int
                or key_selection not in (1, 2)
                or observation.get("retransmission_key_selection")
                    != key_selection
                or type(payload_bytes) is not int
                or payload_bytes <= 0
                or observation.get("retransmission_payload_bytes")
                    != payload_bytes
                or not isinstance(payload_digest, str)
                or len(payload_digest) != 64
                or observation.get("retransmission_payload_sha256")
                    != payload_digest
                or type(original_ordinal) is not int
                or type(loss_ordinal) is not int
                or type(retransmission_ordinal) is not int
                or not (
                    original_ordinal
                    < loss_ordinal
                    < retransmission_ordinal
                )
                or type(
                    observation.get("loss_report_delay_microseconds")
                ) is not int
                or observation["loss_report_delay_microseconds"] < 0
                or type(
                    observation.get(
                        "retransmission_delay_microseconds"
                    )
                ) is not int
                or observation["retransmission_delay_microseconds"] < 0
            ):
                raise RuntimeError(
                    f"{scenario.name}: Row-FEC loss lacks a causal, "
                    "byte-identical ARQ handoff: "
                    f"{observation}"
                )
        sequences.append(sequence)

    first_sequence = sequences[0]
    offsets = tuple(
        (sequence - first_sequence) & SEQUENCE_MASK
        for sequence in sequences
    )
    if offsets != scenario.expected_sequence_offsets:
        raise RuntimeError(
            f"{scenario.name}: dropped source offsets {offsets} do not "
            f"match {scenario.expected_sequence_offsets}"
        )
    if scenario.recovery_policy == ARQ_HANDOFF:
        validate_arq_wire_evidence(
            scenario,
            relay,  # type: ignore[arg-type]
            first_sequence,
        )
    return observations


def validate_encrypted_rotation(
    scenario: Scenario,
    receiver: dict[str, object],
    observations: list[dict[str, object]],
    trace: str,
    *,
    data_key_observation: dict[str, object] | None = None,
) -> int:
    if scenario.key_length == 0:
        raise ValueError("encrypted validation requires a key length")
    undecryptable = nonnegative_statistic(
        receiver, "pktRcvUndecryptTotal"
    )
    if undecryptable is None:
        raise RuntimeError(
            f"{scenario.name}: receiver undecryptable-packet "
            "statistics are unavailable"
        )
    if undecryptable != 0:
        raise RuntimeError(
            f"{scenario.name}: receiver reported {undecryptable} "
            "undecryptable packets"
        )

    selectors = [
        observation.get("key_selection")
        for observation in observations
    ]
    if any(selector not in (1, 2) for selector in selectors):
        raise RuntimeError(
            f"{scenario.name}: FEC source faults were not all encrypted: "
            f"{selectors}"
        )
    traced_transitions = validate_transition_trace(scenario, trace)
    if traced_transitions is not None:
        transitions = traced_transitions
    elif data_key_observation is not None:
        packets = data_key_observation.get("packets")
        unencrypted = data_key_observation.get("unencrypted_packets")
        transitions = data_key_observation.get("transitions")
        traced_selectors = data_key_observation.get("selectors")
        socket_ids = data_key_observation.get("destination_socket_ids")
        if (
            data_key_observation.get("complete") is not True
            or type(packets) is not int
            or packets <= 0
            or type(unencrypted) is not int
            or unencrypted != 0
            or type(transitions) is not int
            or not isinstance(traced_selectors, list)
            or len(traced_selectors) != transitions + 1
            or any(
                type(selector) is not int or selector not in (1, 2)
                for selector in traced_selectors
            )
            or any(
                current == previous
                for previous, current in zip(
                    traced_selectors, traced_selectors[1:], strict=False
                )
            )
            or not isinstance(socket_ids, list)
            or len(socket_ids) != 1
            or type(socket_ids[0]) is not int
            or socket_ids[0] <= 0
        ):
            raise RuntimeError(
                f"{scenario.name}: DATA key-selection trace is incomplete"
            )
    else:
        transitions = sum(
            previous != current
            for previous, current in zip(
                selectors, selectors[1:], strict=False
            )
        )
    if transitions < scenario.minimum_key_transitions:
        raise RuntimeError(
            f"{scenario.name}: observed only {transitions} data-key "
            f"transitions, expected at least "
            f"{scenario.minimum_key_transitions}"
        )

    if traced_transitions is None:
        key_material = runtime_key_material_names(trace)
        requests = sum(
            name == "KMREQ" and direction == "sender_to_receiver"
            for name, direction in key_material
        )
        responses = sum(
            name == "KMRSP" and direction == "receiver_to_sender"
            for name, direction in key_material
        )
        if (
            requests < scenario.minimum_key_transitions
            or responses < scenario.minimum_key_transitions
        ):
            raise RuntimeError(
                f"{scenario.name}: observed {requests} KMREQ and "
                f"{responses} KMRSP runtime messages, expected at least "
                f"{scenario.minimum_key_transitions} of each"
            )
    return transitions


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
    relay: RolloverObservationSource,
    caller: dict[str, object],
    listener: dict[str, object],
    observations: list[dict[str, object]],
    source_packets: int,
) -> dict[str, int]:
    target = scenario.translated_initial_sequence
    if not scenario.rollover or target is None:
        raise ValueError(
            "rollover validation requires a translated scenario"
        )
    caller_isn = event_initial_sequence(
        scenario.name, caller, "caller"
    )
    listener_isn = event_initial_sequence(
        scenario.name, listener, "listener"
    )
    expected_wire_packets = (
        source_packets
        + scenario.expected_control_packets(source_packets)
    )
    sequence = relay.sequence_observation()
    translated_packets = sequence.get("translated_data_packets")
    translated_acks = sequence.get("translated_acknowledgements")
    if (
        listener_isn != target
        or sequence.get("source_initial_sequence") != caller_isn
        or sequence.get("target_initial_sequence") != target
        or sequence.get("first_translated_sequence") != target
        or sequence.get("data_wrap_observed") is not True
        or sequence.get("ack_wrap_observed") is not True
        or not isinstance(translated_packets, int)
        or isinstance(translated_packets, bool)
        or translated_packets != expected_wire_packets
        or not isinstance(translated_acks, int)
        or isinstance(translated_acks, bool)
        or translated_acks <= 0
        or not isinstance(
            sequence.get("last_translated_sequence"), int
        )
        or int(sequence["last_translated_sequence"]) >= target
        or target + expected_wire_packets <= SEQUENCE_MODULUS
    ):
        raise RuntimeError(
            f"{scenario.name}: DATA/ACK rollover evidence is "
            f"incomplete: {sequence}"
        )
    fault_sequences = [
        observation.get("sequence")
        for observation in observations
    ]
    if not any(
        isinstance(value, int)
        and not isinstance(value, bool)
        and value < target
        for value in fault_sequences
    ):
        raise RuntimeError(
            f"{scenario.name}: no reconstructed source was observed "
            f"after rollover: {fault_sequences}"
        )
    return {
        "caller_isn": caller_isn,
        "listener_isn": listener_isn,
        "wire_isn": target,
        "end_sequence": int(sequence["last_translated_sequence"]),
        "wire_packets": translated_packets,
    }


def render_failure(
    scenario: Scenario,
    reason: str,
    output_path: Path,
    *,
    caller_stdout: str = "",
    caller_stderr: str = "",
    listener_stdout: str = "",
    listener_stderr: str = "",
    relay_trace: str = "",
) -> str:
    partial = output_path.stat().st_size if output_path.exists() else 0
    return (
        f"{scenario.name}: {reason}; partial_output_bytes={partial}\n"
        f"--- caller stdout ---\n{caller_stdout or '<empty>'}\n"
        f"--- caller stderr ---\n{caller_stderr or '<empty>'}\n"
        f"--- listener stdout ---\n{listener_stdout or '<empty>'}\n"
        f"--- listener stderr ---\n{listener_stderr or '<empty>'}\n"
        f"--- relay trace ---\n{relay_trace or '<empty>'}\n"
    )


def run_scenario(
    scenario: Scenario,
    options: RunOptions,
    directory: Path,
    environment: dict[str, str],
) -> None:
    scenario_options = replace(
        options,
        byte_count=(
            options.byte_count * scenario.byte_count_multiplier
        ),
    )
    listener_port = free_udp_port()
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    expected_digest = write_deterministic_payload(
        input_path, scenario_options.byte_count, scenario.seed
    )
    listener_stdout_path = directory / f"{scenario.name}.listener.stdout"
    listener_stderr_path = directory / f"{scenario.name}.listener.stderr"
    faults = tuple(
        RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=occurrence,
        )
        for occurrence in scenario.fault_occurrences
    )

    relay_context = (
        CallerListenerSequenceProxy(
            listener_port,
            scenario.translated_initial_sequence,
            faults,
        )
        if scenario.translated_initial_sequence is not None
        else CallerListenerFaultProxy(listener_port, faults)
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
                scenario,
                scenario.listener,
                "listener",
                listener_port,
                output_path,
                scenario_options,
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

        caller: subprocess.CompletedProcess[str] | None = None
        try:
            ready_deadline = time.monotonic() + min(
                5, scenario_options.timeout_seconds
            )
            while True:
                listener_stdout, listener_stderr = listener_output()
                if parse_ready_port(listener_stdout) == listener_port:
                    break
                if listener.poll() is not None:
                    raise RuntimeError(
                        render_failure(
                            scenario,
                            "listener exited before caller startup",
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
                            output_path,
                            listener_stdout=listener_stdout,
                            listener_stderr=listener_stderr,
                        )
                    )
                time.sleep(0.01)

            relay.start()
            caller = subprocess.run(
                peer_command(
                    scenario,
                    scenario.caller,
                    "caller",
                    relay.port,
                    input_path,
                    scenario_options,
                ),
                capture_output=True,
                text=True,
                timeout=scenario_options.timeout_seconds + 5,
                check=False,
                env=environment,
            )
            listener.wait(
                timeout=scenario_options.timeout_seconds + 5
            )
            listener_stdout, listener_stderr = listener_output()
            trace = relay.render(scenario.name)
            if caller.returncode != 0 or listener.returncode != 0:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "transfer failed "
                        f"(caller={caller.returncode}, "
                        f"listener={listener.returncode})",
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        relay_trace=trace,
                    )
                )

            caller_event = parse_complete(caller.stdout, "caller")
            listener_event = parse_complete(listener_stdout, "listener")
            if (
                caller_event.get("bytes")
                    != scenario_options.byte_count
                or listener_event.get("bytes")
                    != scenario_options.byte_count
            ):
                raise RuntimeError(
                    f"{scenario.name}: peer byte counters do not match"
                )
            received_digest = file_sha256(output_path)
            if received_digest != expected_digest:
                raise RuntimeError(
                    f"{scenario.name}: SHA-256 mismatch "
                    f"{received_digest} != {expected_digest}"
                )

            source_packets = expected_live_packets(
                scenario_options.byte_count, SOURCE_PACKET_SIZE
            )
            sender_extra, receiver_supply = validate_filter_statistics(
                scenario,
                caller_event,
                listener_event,
                source_packets,
            )
            validate_no_arq_statistics(scenario, caller_event)
            observations = validate_causal_recovery(scenario, relay)
            rollover = None
            if scenario.rollover:
                if not isinstance(relay, CallerListenerSequenceProxy):
                    raise RuntimeError(
                        f"{scenario.name}: sequence relay was not installed"
                    )
                rollover = validate_rollover(
                    scenario,
                    relay,
                    caller_event,
                    listener_event,
                    observations,
                    source_packets,
                )
            key_transitions = (
                validate_encrypted_rotation(
                    scenario,
                    listener_event,
                    observations,
                    trace,
                    data_key_observation=relay.data_key_observation(
                        "sender_to_receiver"
                    ),
                )
                if scenario.key_length != 0
                else 0
            )
            directions = filtercap_directions(trace)
            if directions != {
                "sender_to_receiver",
                "receiver_to_sender",
            }:
                raise RuntimeError(
                    f"{scenario.name}: FILTERCAP was not observed in "
                    f"both handshake directions: {sorted(directions)}"
                )
            dropped_sequences = ",".join(
                str(observation.get("sequence"))
                for observation in observations
            )
            rollover_summary = (
                f" wire_isn={rollover['wire_isn']}"
                f" end_sequence={rollover['end_sequence']}"
                f" wire_packets={rollover['wire_packets']}"
                if rollover is not None
                else ""
            )
            print(
                f"PASS {scenario.name} sha256={received_digest} "
                f"geometry={scenario.geometry} "
                f"dropped_sequences={dropped_sequences} "
                f"source_packets={source_packets} "
                f"fec_control_packets={sender_extra} "
                f"fec_reconstructed={receiver_supply} "
                f"key_transitions={key_transitions}"
                f"{rollover_summary} "
                "matching_nak=False retransmission=False"
            )
        except RuntimeError as error:
            if "--- caller stdout ---" in str(error):
                raise
            listener_stdout, listener_stderr = listener_output()
            raise RuntimeError(
                render_failure(
                    scenario,
                    str(error).removeprefix(f"{scenario.name}: "),
                    output_path,
                    caller_stdout=(
                        caller.stdout if caller is not None else ""
                    ),
                    caller_stderr=(
                        caller.stderr if caller is not None else ""
                    ),
                    listener_stdout=listener_stdout,
                    listener_stderr=listener_stderr,
                    relay_trace=relay.render(scenario.name),
                )
            ) from error
        except subprocess.TimeoutExpired as error:
            terminate(listener)
            listener_stdout, listener_stderr = listener_output()
            raise RuntimeError(
                render_failure(
                    scenario,
                    "peer timed out",
                    output_path,
                    caller_stdout=(
                        caller.stdout if caller is not None else ""
                    ),
                    caller_stderr=(
                        caller.stderr if caller is not None else ""
                    ),
                    listener_stdout=listener_stdout,
                    listener_stderr=listener_stderr,
                    relay_trace=relay.render(scenario.name),
                )
            ) from error
        finally:
            if listener.poll() is None:
                terminate(listener)


def render_rendezvous_failure(
    scenario: RendezvousScenario,
    reason: str,
    output_path: Path,
    *,
    sender_stdout: str = "",
    sender_stderr: str = "",
    receiver_stdout: str = "",
    receiver_stderr: str = "",
    relay_trace: str = "",
) -> str:
    partial = output_path.stat().st_size if output_path.exists() else 0
    return (
        f"{scenario.profile.name}: {reason}; "
        f"partial_output_bytes={partial}\n"
        f"--- sender stdout ---\n{sender_stdout or '<empty>'}\n"
        f"--- sender stderr ---\n{sender_stderr or '<empty>'}\n"
        f"--- receiver stdout ---\n{receiver_stdout or '<empty>'}\n"
        f"--- receiver stderr ---\n{receiver_stderr or '<empty>'}\n"
        f"--- relay trace ---\n{relay_trace or '<empty>'}\n"
    )


def validate_rendezvous_role(
    scenario: RendezvousScenario,
    relay: RendezvousTraceProxy,
) -> tuple[str, dict[str, object]]:
    observation = relay.role_observation()
    if observation is None:
        raise RuntimeError(
            f"{scenario.profile.name}: could not observe both "
            "WAVEAHAND cookies"
        )
    if {
        observation.get("sender_role"),
        observation.get("receiver_role"),
    } != {"initiator", "responder"}:
        raise RuntimeError(
            f"{scenario.profile.name}: invalid rendezvous cookie "
            f"contest: {observation}"
        )
    if scenario.robotweax_peer == scenario.sender:
        robotweax_role = observation.get("sender_role")
    else:
        robotweax_role = observation.get("receiver_role")
    if robotweax_role not in ("initiator", "responder"):
        raise RuntimeError(
            f"{scenario.profile.name}: Robotweax rendezvous role is "
            "unavailable"
        )
    return str(robotweax_role), observation


def validate_encrypted_rendezvous_binding(
    scenario: RendezvousScenario,
    relay: TimingRendezvousTraceProxy,
    trace: str,
) -> tuple[int, int]:
    sender_socket_id = relay.conclusion_socket_id("sender_to_receiver")
    receiver_socket_id = relay.conclusion_socket_id("receiver_to_sender")
    events = trace_events(trace)
    first_data = [
        event
        for event in events
        if event.get("event") == "srt_first_data_trace"
        and event.get("direction") == "sender_to_receiver"
    ]
    if (
        type(sender_socket_id) is not int
        or sender_socket_id <= 0
        or type(receiver_socket_id) is not int
        or receiver_socket_id <= 0
        or len(first_data) != 1
        or first_data[0].get("destination_socket_id")
        != receiver_socket_id
        or type(first_data[0].get("sequence")) is not int
        or not 0 <= int(first_data[0]["sequence"]) <= SEQUENCE_MASK
    ):
        raise RuntimeError(
            f"{scenario.profile.name}: encrypted Rendezvous DATA is not "
            "bound to both positive CONCLUSION identities"
        )
    return sender_socket_id, receiver_socket_id


def run_rendezvous_scenario(
    scenario: RendezvousScenario,
    options: RunOptions,
    directory: Path,
    environment: dict[str, str],
) -> str:
    profile = scenario.profile
    scenario_options = replace(
        options,
        byte_count=(
            options.byte_count * profile.byte_count_multiplier
        ),
    )
    faults = tuple(
        RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=occurrence,
        )
        for occurrence in profile.fault_occurrences
    )
    with reserved_udp_ports() as endpoint_ports:
        sender_port, receiver_port = endpoint_ports
        relay = (
            TimingRendezvousTraceProxy(
                sender_port,
                receiver_port,
                faults,
            )
            if profile.key_length != 0
            else RendezvousTraceProxy(
                sender_port,
                receiver_port,
                faults,
            )
        )
    input_path = directory / f"{profile.name}.input"
    output_path = directory / f"{profile.name}.output"
    expected_digest = write_deterministic_payload(
        input_path,
        scenario_options.byte_count,
        profile.seed,
    )
    sender_stdout_path = directory / f"{profile.name}.sender.stdout"
    sender_stderr_path = directory / f"{profile.name}.sender.stderr"
    receiver_stdout_path = directory / f"{profile.name}.receiver.stdout"
    receiver_stderr_path = directory / f"{profile.name}.receiver.stderr"
    processes: list[subprocess.Popen[str]] = []
    sender: subprocess.Popen[str] | None = None
    receiver: subprocess.Popen[str] | None = None

    with (
        sender_stdout_path.open("w", encoding="utf-8")
        as sender_stdout_stream,
        sender_stderr_path.open("w", encoding="utf-8")
        as sender_stderr_stream,
        receiver_stdout_path.open("w", encoding="utf-8")
        as receiver_stdout_stream,
        receiver_stderr_path.open("w", encoding="utf-8")
        as receiver_stderr_stream,
    ):
        try:
            relay.start()
            process_specs = {
                "sender": (
                    rendezvous_peer_command(
                        scenario,
                        scenario.sender,
                        "rendezvous-sender",
                        sender_port,
                        relay.sender_port,
                        input_path,
                        scenario_options,
                    ),
                    sender_stdout_stream,
                    sender_stderr_stream,
                ),
                "receiver": (
                    rendezvous_peer_command(
                        scenario,
                        scenario.receiver,
                        "rendezvous-receiver",
                        receiver_port,
                        relay.receiver_port,
                        output_path,
                        scenario_options,
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
            for label in start_sequence:
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

            deadline = time.monotonic() + scenario_options.timeout_seconds + 5
            for process in processes:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise subprocess.TimeoutExpired(
                        process.args,
                        scenario_options.timeout_seconds + 5,
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
                    "rendezvous peers timed out",
                    output_path,
                    sender_stdout=sender_stdout_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    sender_stderr=sender_stderr_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    receiver_stdout=receiver_stdout_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    receiver_stderr=receiver_stderr_path.read_text(
                        encoding="utf-8", errors="replace"
                    ),
                    relay_trace=relay.render(profile.name),
                )
            ) from error
        finally:
            for process in processes:
                if process.poll() is None:
                    terminate(process)
            relay.close()

    if sender is None or receiver is None:
        raise RuntimeError(
            f"{profile.name}: failed to start both rendezvous peers"
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
    trace = relay.render(profile.name)
    if sender.returncode != 0 or receiver.returncode != 0:
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                "rendezvous transfer failed "
                f"(sender={sender.returncode}, "
                f"receiver={receiver.returncode})",
                output_path,
                sender_stdout=sender_stdout,
                sender_stderr=sender_stderr,
                receiver_stdout=receiver_stdout,
                receiver_stderr=receiver_stderr,
                relay_trace=trace,
            )
        )
    if relay.error() is not None:
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                f"rendezvous relay failed: {relay.error()}",
                output_path,
                sender_stdout=sender_stdout,
                sender_stderr=sender_stderr,
                receiver_stdout=receiver_stdout,
                receiver_stderr=receiver_stderr,
                relay_trace=trace,
            )
        )

    try:
        sender_event = parse_complete(
            sender_stdout, "rendezvous-sender"
        )
        receiver_event = parse_complete(
            receiver_stdout, "rendezvous-receiver"
        )
        if (
            sender_event.get("bytes") != scenario_options.byte_count
            or receiver_event.get("bytes") != scenario_options.byte_count
        ):
            raise RuntimeError(
                f"{profile.name}: peer byte counters do not match"
            )
        received_digest = file_sha256(output_path)
        if received_digest != expected_digest:
            raise RuntimeError(
                f"{profile.name}: SHA-256 mismatch "
                f"{received_digest} != {expected_digest}"
            )
        source_packets = expected_live_packets(
            scenario_options.byte_count,
            SOURCE_PACKET_SIZE,
        )
        sender_extra, receiver_supply = validate_filter_statistics(
            profile,
            sender_event,
            receiver_event,
            source_packets,
        )
        if profile.recovery_policy == FEC_RECOVERY_ONLY:
            validate_no_arq_statistics(profile, sender_event)
            arq_retransmissions = 0
            arq_naks = 0
        else:
            arq_retransmissions, arq_naks = (
                validate_arq_handoff_statistics(
                    profile, sender_event
                )
            )
        observations = validate_causal_recovery(profile, relay)
        key_transitions = (
            validate_encrypted_rotation(
                profile,
                receiver_event,
                observations,
                trace,
            )
            if profile.key_length != 0
            else 0
        )
        directions = filtercap_directions(trace)
        if directions != {
            "sender_to_receiver",
            "receiver_to_sender",
        }:
            raise RuntimeError(
                f"{profile.name}: FILTERCAP was not observed in both "
                f"rendezvous directions: {sorted(directions)}"
            )
        robotweax_role, role_observation = validate_rendezvous_role(
            scenario,
            relay,
        )
        connection_ids = (
            validate_encrypted_rendezvous_binding(
                scenario,
                relay,
                trace,
            )
            if isinstance(relay, TimingRendezvousTraceProxy)
            else None
        )
    except RuntimeError as error:
        if "--- sender stdout ---" in str(error):
            raise
        raise RuntimeError(
            render_rendezvous_failure(
                scenario,
                str(error).removeprefix(f"{profile.name}: "),
                output_path,
                sender_stdout=sender_stdout,
                sender_stderr=sender_stderr,
                receiver_stdout=receiver_stdout,
                receiver_stderr=receiver_stderr,
                relay_trace=trace,
            )
        ) from error

    dropped_sequences = ",".join(
        str(observation.get("sequence"))
        for observation in observations
    )
    print(
        f"ROLE {profile.name} robotweax={robotweax_role} "
        f"sender_cookie=0x"
        f"{int(role_observation['sender_cookie']):08x} "
        f"receiver_cookie=0x"
        f"{int(role_observation['receiver_cookie']):08x}",
        flush=True,
    )
    print(
        (
            f"PASS {profile.name} sha256={received_digest} "
            f"geometry={profile.geometry} "
            f"dropped_sequences={dropped_sequences} "
            f"source_packets={source_packets} "
            f"fec_control_packets={sender_extra} "
            f"fec_reconstructed={receiver_supply} "
            f"key_transitions={key_transitions} "
        )
        + (
            f"sender_socket_id={connection_ids[0]} "
            f"receiver_socket_id={connection_ids[1]} "
            if connection_ids is not None
            else ""
        )
        + (
            "matching_nak=False retransmission=False"
            if profile.recovery_policy == FEC_RECOVERY_ONLY
            else (
                f"matching_nak=True retransmission=True "
                f"arq_naks={arq_naks} "
                f"arq_retransmissions={arq_retransmissions}"
            )
        ),
        flush=True,
    )
    return robotweax_role


def run_rendezvous_matrix(
    scenarios: list[RendezvousScenario],
    robotweax: Path,
    reference: Path,
    options: RunOptions,
    directory: Path,
    environment: dict[str, str],
    role_probe_attempts: int,
    *,
    role_probe_factory: Callable[
        [Path, Path, int], RendezvousScenario
    ] = rendezvous_role_probe_scenario,
    coverage_name: str = "FEC Rendezvous",
) -> list[str]:
    roles: set[str] = set()
    failures: list[str] = []
    for scenario in scenarios:
        try:
            roles.add(
                run_rendezvous_scenario(
                    scenario,
                    options,
                    directory,
                    environment,
                )
            )
        except (OSError, RuntimeError) as error:
            failures.append(str(error))
    if failures:
        return failures

    required_roles = {"initiator", "responder"}
    for attempt in range(role_probe_attempts):
        if roles == required_roles:
            break
        try:
            roles.add(
                run_rendezvous_scenario(
                    role_probe_factory(
                        robotweax,
                        reference,
                        attempt,
                    ),
                    options,
                    directory,
                    environment,
                )
            )
        except (OSError, RuntimeError) as error:
            failures.append(str(error))
            return failures
    if roles != required_roles:
        missing = sorted(required_roles - roles)
        failures.append(
            f"Robotweax {coverage_name} cookie-role coverage is "
            f"incomplete after {role_probe_attempts} probes; missing: "
            + ", ".join(missing)
        )
        return failures
    print(
        "PASS robotweax-"
        + coverage_name.lower().replace(" ", "-")
        + "-cookie-role-coverage "
        "roles=initiator,responder",
        flush=True,
    )
    return failures


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument(
        "--bytes", type=int, default=SOURCE_PACKET_SIZE * 50
    )
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument(
        "--rendezvous-role-probe-attempts",
        type=int,
        default=16,
    )
    parser.add_argument(
        "--km-refresh-rate",
        type=int,
        default=ENCRYPTED_FEC_KEY_REFRESH_RATE,
    )
    parser.add_argument(
        "--km-preannounce",
        type=int,
        default=ENCRYPTED_FEC_KEY_PREANNOUNCEMENT,
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if (
        arguments.bytes <= 0
        or arguments.timeout_seconds <= 0
        or arguments.rendezvous_role_probe_attempts < 0
        or arguments.km_refresh_rate <= 0
        or arguments.km_preannounce <= 0
        or arguments.km_preannounce
            > (arguments.km_refresh_rate - 1) // 2
    ):
        raise SystemExit(
            "invalid byte, timeout, role-probe, or key-rotation parameters"
        )
    robotweax = resolve_program_path(arguments.robotweax_peer)
    reference = resolve_program_path(arguments.reference_peer)
    options = RunOptions(
        byte_count=arguments.bytes,
        timeout_seconds=arguments.timeout_seconds,
        key_refresh_rate=arguments.km_refresh_rate,
        key_preannouncement=arguments.km_preannounce,
    )
    environment = os.environ.copy()
    environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)
    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="robotweax-srt-fec-interop-"
    ) as temporary_directory:
        directory = Path(temporary_directory)
        scenarios = [
            *scenario_matrix(robotweax, reference),
            *encrypted_matrix_scenarios(robotweax, reference),
            *rollover_scenarios(robotweax, reference),
        ]
        for scenario in scenarios:
            try:
                run_scenario(
                    scenario, options, directory, environment
                )
            except RuntimeError as error:
                failures.append(str(error))
        failures.extend(
            run_rendezvous_matrix(
                rendezvous_scenarios(robotweax, reference),
                robotweax,
                reference,
                options,
                directory,
                environment,
                arguments.rendezvous_role_probe_attempts,
            )
        )
        failures.extend(
            run_rendezvous_matrix(
                encrypted_rendezvous_scenarios(robotweax, reference),
                robotweax,
                reference,
                options,
                directory,
                environment,
                arguments.rendezvous_role_probe_attempts,
                role_probe_factory=(
                    encrypted_rendezvous_role_probe_scenario
                ),
                coverage_name="encrypted FEC Rendezvous",
            )
        )
    if failures:
        print(
            "FEC interoperability failures:\n\n"
            + "\n\n".join(failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
