#!/usr/bin/env python3
"""Measure real Robotweax SRT TSBPD release through a UDP handoff."""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
from contextlib import closing, contextmanager, nullcontext
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterator, Sequence

from interop_common import (
    free_udp_port,
    nonnegative_statistic,
    parse_complete,
    reserved_udp_ports,
    resolve_program_path,
    terminate,
)
from live_timing import (
    GeneratedMessage,
    MpegTsProfile,
    TimingSample,
    generate_binary_messages,
    generate_mpeg_ts_messages,
    parse_transport_packet,
    score_timing,
)
from srt_handshake_trace import (
    CallerListenerFaultProxy,
    HandshakeTraceProxy,
    MAX_RUNTIME_KEY_MATERIAL_EVENTS,
    RendezvousFault,
    RendezvousTraceProxy,
    describe_acknowledgement_datagram,
    describe_runtime_key_material_datagram,
)


PROFILE_PACKETS = {
    "ts-188": 1,
    "ts-376": 2,
    "ts-1316": 7,
}
FAULT_PROFILES = (
    "none",
    "loss-delay-reorder",
    "fec-source-drop",
    "fec-burst-drop",
    "fec-burst-delay-reorder",
)
CONNECTION_MODES = ("caller-listener", "rendezvous", "backup-group")
UDP_TIMESTAMP_SOURCES = ("userspace", "linux-software")
FAULT_DELAY_MILLISECONDS = 30
FEC_SOURCE_DROP_OCCURRENCE = 5
FEC_BURST_DROP_OCCURRENCES = (5, 6, 7)
FEC_COMPOUND_MINIMUM_MESSAGES = 32
FEC_MAXIMUM_SOURCE_BYTES = 1_452
FEC_COLUMNS = 10
FEC_ROWS = 5
# At 7.52 Mbit/s, the smallest compound-profile payload (1,200 bytes) emits at
# most eight source packets during the 10 ms ACK/SYN interval immediately
# before the relay begins its measured delay. Packets held after that point are
# counted exactly by the relay itself.
FEC_COMPOUND_MAXIMUM_PRE_DELAY_ACK_FLIGHT_PACKETS = 8
FEC_COMPOUND_MINIMUM_RTO_AGE_MICROSECONDS = 30_000
PASSPHRASE_ENVIRONMENT = "ROBOTWEAX_SRT_TIMING_PASSPHRASE"
SECURED_KEY_STATE = 2
MAXIMUM_KEY_TRANSITION_EVENTS = 64
MAXIMUM_GROUP_REPLAY_TRACE_EVENTS = 2_048
MAXIMUM_GROUP_STABILITY_MILLISECONDS = 5_000
REFERENCE_LIVE_MINIMUM_PAYLOAD_BYTES = 1_316
RECEIVER_SHUTDOWN_GRACE_MILLISECONDS = 250
GROUP_OUTAGE_TEARDOWN_MARGIN_MILLISECONDS = 250


@dataclass(frozen=True)
class FecGeometry:
    packet_filter: str
    row_source_packets: int = 0
    column_source_packets: int = 0
    column_control_packets: int = 0

    def expected_control_packets(self, source_packets: int) -> int:
        row_controls = (
            source_packets // self.row_source_packets
            if self.row_source_packets > 0
            else 0
        )
        column_controls = (
            source_packets
            // self.column_source_packets
            * self.column_control_packets
            if self.column_source_packets > 0
            else 0
        )
        return row_controls + column_controls


FEC_GEOMETRIES = {
    "row": FecGeometry(
        packet_filter=(
            f"fec,arq:onreq,cols:{FEC_COLUMNS},layout:even,rows:1"
        ),
        row_source_packets=FEC_COLUMNS,
    ),
    "column": FecGeometry(
        packet_filter=(
            f"fec,arq:onreq,cols:{FEC_COLUMNS},layout:even,rows:-{FEC_ROWS}"
        ),
        column_source_packets=FEC_COLUMNS * FEC_ROWS,
        column_control_packets=FEC_COLUMNS,
    ),
    "matrix": FecGeometry(
        packet_filter=(
            f"fec,arq:onreq,cols:{FEC_COLUMNS},layout:even,rows:{FEC_ROWS}"
        ),
        row_source_packets=FEC_COLUMNS,
        column_source_packets=FEC_COLUMNS * FEC_ROWS,
        column_control_packets=FEC_COLUMNS,
    ),
}
FEC_PROFILES = ("none", *FEC_GEOMETRIES)


@dataclass(frozen=True)
class SecurityProfile:
    key_length: int = 0
    key_refresh_rate: int = 0
    key_preannouncement: int = 0
    minimum_key_transitions: int = 0
    crypto_mode: str | None = None

    def __post_init__(self) -> None:
        if self.key_length == 0:
            if any(
                value != 0
                for value in (
                    self.key_refresh_rate,
                    self.key_preannouncement,
                    self.minimum_key_transitions,
                )
            ) or self.crypto_mode is not None:
                raise ValueError(
                    "clear timing cannot request key rotation"
                )
            return
        if (
            self.key_length not in (16, 24, 32)
            or self.crypto_mode not in (None, "ctr", "gcm")
            or self.key_refresh_rate < 2
            or self.key_preannouncement <= 0
            or self.key_preannouncement
            > (self.key_refresh_rate - 1) // 2
            or self.minimum_key_transitions <= 0
        ):
            raise ValueError("invalid encrypted timing profile")

    @property
    def encrypted(self) -> bool:
        return self.key_length != 0


@dataclass(frozen=True)
class FecProfile:
    name: str = "none"

    def __post_init__(self) -> None:
        if self.name not in FEC_PROFILES:
            raise ValueError(f"unknown timing FEC profile: {self.name}")

    @property
    def enabled(self) -> bool:
        return self.name != "none"

    @property
    def packet_filter(self) -> str:
        return FEC_GEOMETRIES[self.name].packet_filter if self.enabled else ""

    def expected_control_packets(self, source_packets: int) -> int:
        if not self.enabled:
            return 0
        return FEC_GEOMETRIES[self.name].expected_control_packets(
            source_packets
        )


def _original_data_key_transition(
    payload: bytes, direction: str, relay_ordinal: int
) -> dict[str, object] | None:
    if len(payload) < 16:
        return None
    first_word = int.from_bytes(payload[0:4], "big")
    message_word = int.from_bytes(payload[4:8], "big")
    message_number = message_word & 0x03FF_FFFF
    retransmitted = bool(message_word & 0x0400_0000)
    if first_word & 0x8000_0000 or message_number == 0 or retransmitted:
        return None
    return {
        "direction": direction,
        "relay_ordinal": relay_ordinal,
        "sequence": first_word & 0x7FFF_FFFF,
        "destination_socket_id": int.from_bytes(payload[12:16], "big"),
        "key_selection": (message_word >> 27) & 0x03,
    }


def _data_packet_observation(
    payload: bytes, direction: str, relay_ordinal: int
) -> dict[str, object] | None:
    if len(payload) < 16:
        return None
    first_word = int.from_bytes(payload[0:4], "big")
    if first_word & 0x8000_0000:
        return None
    message_word = int.from_bytes(payload[4:8], "big")
    message_number = message_word & 0x03FF_FFFF
    if message_number == 0:
        return None
    return {
        "direction": direction,
        "relay_ordinal": relay_ordinal,
        "sequence": first_word & 0x7FFF_FFFF,
        "message_number": message_number,
        "packet_position": (message_word >> 30) & 0x03,
        "in_order": bool(message_word & 0x2000_0000),
        "key_selection": (message_word >> 27) & 0x03,
        "retransmitted": bool(message_word & 0x0400_0000),
        "timestamp": int.from_bytes(payload[8:12], "big"),
        "destination_socket_id": int.from_bytes(payload[12:16], "big"),
        "payload_bytes": len(payload) - 16,
        "ciphertext_sha256": hashlib.sha256(payload[16:]).hexdigest(),
    }


def _render_key_transition_events(
    scenario: str,
    transitions: Sequence[dict[str, object]],
    omitted: int,
    *,
    include_first_data: bool,
) -> list[str]:
    events: list[str] = []
    if include_first_data and transitions:
        events.append(
            json.dumps(
                {
                    "event": "srt_first_data_trace",
                    "scenario": scenario,
                    **transitions[0],
                },
                sort_keys=True,
            )
        )
    events.extend(
        json.dumps(
            {
                "event": "srt_data_key_transition_trace",
                "scenario": scenario,
                **transition,
            },
            sort_keys=True,
        )
        for transition in transitions
    )
    if omitted != 0:
        events.append(
            json.dumps(
                {
                    "event": "srt_data_key_transition_trace_truncated",
                    "scenario": scenario,
                    "omitted": omitted,
                },
                sort_keys=True,
            )
        )
    return events


class TimingTraceProxy(HandshakeTraceProxy):
    """Record original-DATA key-selector changes without payload disclosure."""

    def __init__(
        self, target_port: int, *, host: str = "127.0.0.1"
    ) -> None:
        super().__init__(target_port, host=host)
        self._data_key_transitions: list[dict[str, object]] = []
        self._last_key_selection: dict[str, int] = {}
        self._data_key_transitions_omitted = 0
        self._data_packet_events: list[dict[str, object]] = []
        self._data_packet_events_omitted = 0

    def __enter__(self) -> "TimingTraceProxy":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def render(self, scenario: str) -> str:
        trace = super().render(scenario)
        with self._lock:
            transitions = [
                dict(event) for event in self._data_key_transitions
            ]
            omitted = self._data_key_transitions_omitted
        events = _render_key_transition_events(
            scenario,
            transitions,
            omitted,
            include_first_data=False,
        )
        if not events:
            return trace
        if trace == "<empty>":
            return "\n".join(events)
        return "\n".join((trace, *events))

    def _record_wire(self, payload: bytes, direction: str) -> None:
        super()._record_wire(payload, direction)
        packet = _data_packet_observation(
            payload, direction, self._relay_ordinal
        )
        if packet is not None:
            with self._lock:
                if (
                    len(self._data_packet_events)
                    < MAXIMUM_GROUP_REPLAY_TRACE_EVENTS
                ):
                    self._data_packet_events.append(packet)
                else:
                    self._data_packet_events_omitted += 1
        transition = _original_data_key_transition(
            payload, direction, self._relay_ordinal
        )
        if transition is None:
            return
        key_selection = int(transition["key_selection"])
        with self._lock:
            if self._last_key_selection.get(direction) == key_selection:
                return
            self._last_key_selection[direction] = key_selection
            if len(self._data_key_transitions) >= MAXIMUM_KEY_TRANSITION_EVENTS:
                self._data_key_transitions_omitted += 1
                return
            self._data_key_transitions.append(transition)

    def data_packet_observations(
        self, direction: str
    ) -> tuple[dict[str, object], ...]:
        with self._lock:
            if self._data_packet_events_omitted != 0:
                raise RuntimeError("Backup-group DATA trace is incomplete")
            return tuple(
                dict(event)
                for event in self._data_packet_events
                if event.get("direction") == direction
            )


class AckSuppressingTimingTraceProxy(TimingTraceProxy):
    """Drop primary runtime ACKs while forwarding every other wire class."""

    def __init__(
        self, target_port: int, *, host: str = "127.0.0.1"
    ) -> None:
        super().__init__(target_port, host=host)
        self._suppressed_acknowledgements: list[dict[str, object]] = []

    def acknowledgement_observation(self) -> dict[str, object]:
        with self._lock:
            acknowledgements = [
                dict(event) for event in self._suppressed_acknowledgements
            ]
        return {
            "suppressed": len(acknowledgements),
            "forwarded": 0,
            "acknowledgements": acknowledgements,
        }

    def _should_forward_wire(self, payload: bytes, direction: str) -> bool:
        acknowledgement = describe_acknowledgement_datagram(
            payload, direction
        )
        if direction != "listener_to_caller" or acknowledgement is None:
            return True
        with self._lock:
            self._suppressed_acknowledgements.append(
                {
                    **acknowledgement,
                    "relay_ordinal": self._relay_ordinal,
                }
            )
        return False


class GroupTimingTraceProxy:
    """Give each Backup member an independent secret-safe wire relay."""

    def __init__(
        self,
        target_port: int,
        *,
        host: str = "127.0.0.1",
        suppress_primary_acknowledgements: bool = False,
    ) -> None:
        self.primary = (
            AckSuppressingTimingTraceProxy(target_port, host=host)
            if suppress_primary_acknowledgements
            else TimingTraceProxy(target_port, host=host)
        )
        try:
            self.backup = TimingTraceProxy(target_port, host=host)
        except BaseException:
            self.primary.close()
            raise

    @property
    def primary_port(self) -> int:
        return self.primary.port

    @property
    def backup_port(self) -> int:
        return self.backup.port

    def start(self) -> None:
        self.primary.start()
        try:
            self.backup.start()
        except BaseException:
            self.primary.close()
            raise

    def close(self) -> None:
        self.backup.close()
        self.primary.close()

    def render(self, scenario: str) -> str:
        return "\n".join(
            (
                "--- primary member trace ---",
                self.primary.render(f"{scenario}-primary"),
                "--- backup member trace ---",
                self.backup.render(f"{scenario}-backup"),
            )
        )


class GroupPathOutageCoordinator:
    """Atomically black-hole one established Backup member after DATA."""

    def __init__(self, trigger_original_data_count: int) -> None:
        if trigger_original_data_count <= 0:
            raise ValueError("path outage trigger must be positive")
        self._trigger_original_data_count = trigger_original_data_count
        self._lock = threading.Lock()
        self._primary_original_data_count = 0
        self._outage_started_monotonic_ns: int | None = None
        self._outage_sequence: int | None = None
        self._dropped_primary_datagrams = 0
        self._dropped_primary_data_datagrams = 0
        self._first_backup_data_monotonic_ns: int | None = None
        self._first_backup_sequence: int | None = None

    @staticmethod
    def _original_forward_data(
        payload: bytes, direction: str
    ) -> dict[str, object] | None:
        if direction != "caller_to_listener":
            return None
        packet = _data_packet_observation(payload, direction, 0)
        if packet is None or packet.get("retransmitted") is not False:
            return None
        return packet

    def should_forward(
        self, path: str, payload: bytes, direction: str
    ) -> bool:
        packet = self._original_forward_data(payload, direction)
        now = time.monotonic_ns()
        with self._lock:
            if path == "primary":
                if self._outage_started_monotonic_ns is not None:
                    self._dropped_primary_datagrams += 1
                    if packet is not None:
                        self._dropped_primary_data_datagrams += 1
                    return False
                if packet is None:
                    return True
                self._primary_original_data_count += 1
                if (
                    self._primary_original_data_count
                    != self._trigger_original_data_count
                ):
                    return True
                self._outage_started_monotonic_ns = now
                self._outage_sequence = int(packet["sequence"])
                self._dropped_primary_datagrams = 1
                self._dropped_primary_data_datagrams = 1
                return False
            if path != "backup":
                raise ValueError(f"unknown Backup path: {path}")
            if (
                packet is not None
                and self._outage_started_monotonic_ns is not None
                and self._first_backup_data_monotonic_ns is None
            ):
                self._first_backup_data_monotonic_ns = now
                self._first_backup_sequence = int(packet["sequence"])
            return True

    def observation(self) -> dict[str, object]:
        with self._lock:
            started = self._outage_started_monotonic_ns
            replacement = self._first_backup_data_monotonic_ns
            result: dict[str, object] = {
                "trigger_original_data_count": (
                    self._trigger_original_data_count
                ),
                "primary_original_data_count": (
                    self._primary_original_data_count
                ),
                "dropped_primary_datagrams": (
                    self._dropped_primary_datagrams
                ),
                "dropped_primary_data_datagrams": (
                    self._dropped_primary_data_datagrams
                ),
            }
            if started is not None:
                result["outage_started_monotonic_ns"] = started
                result["outage_sequence"] = self._outage_sequence
            if replacement is not None:
                result["first_backup_data_monotonic_ns"] = replacement
                result["first_backup_sequence"] = self._first_backup_sequence
            if started is not None and replacement is not None:
                result["first_backup_data_delay_microseconds"] = (
                    replacement - started
                ) // 1_000
            return result


class PathOutageTimingTraceProxy(TimingTraceProxy):
    """Trace one member and delegate its forwarding policy atomically."""

    def __init__(
        self,
        target_port: int,
        coordinator: GroupPathOutageCoordinator,
        path: str,
        *,
        host: str = "127.0.0.1",
    ) -> None:
        super().__init__(target_port, host=host)
        self._coordinator = coordinator
        self._path = path

    def _should_forward_wire(self, payload: bytes, direction: str) -> bool:
        return self._coordinator.should_forward(
            self._path, payload, direction
        )


class GroupPathOutageTraceProxy(GroupTimingTraceProxy):
    """Give each member a relay and black-hole the established primary."""

    def __init__(
        self,
        target_port: int,
        trigger_original_data_count: int,
        *,
        host: str = "127.0.0.1",
    ) -> None:
        self._coordinator = GroupPathOutageCoordinator(
            trigger_original_data_count
        )
        self.primary = PathOutageTimingTraceProxy(
            target_port, self._coordinator, "primary", host=host
        )
        try:
            self.backup = PathOutageTimingTraceProxy(
                target_port, self._coordinator, "backup", host=host
            )
        except BaseException:
            self.primary.close()
            raise

    def outage_observation(self) -> dict[str, object]:
        return self._coordinator.observation()


class TimingFaultTraceProxy(CallerListenerFaultProxy):
    """Combine deterministic faults with secret-safe key-transition traces."""

    def __init__(
        self,
        target_port: int,
        faults: tuple[RendezvousFault, ...],
        *,
        host: str = "127.0.0.1",
    ) -> None:
        super().__init__(target_port, faults, host=host)
        self._data_key_transitions: list[dict[str, object]] = []
        self._last_key_selection: dict[str, int] = {}
        self._data_key_transitions_omitted = 0

    def render(self, scenario: str) -> str:
        trace = super().render(scenario)
        with self._lock:
            transitions = [
                dict(event) for event in self._data_key_transitions
            ]
            omitted = self._data_key_transitions_omitted
        events = _render_key_transition_events(
            scenario,
            transitions,
            omitted,
            include_first_data=True,
        )
        if not events:
            return trace
        if trace == "<empty>":
            return "\n".join(events)
        return "\n".join((trace, *events))

    def _record(self, payload: bytes, direction: str) -> None:
        super()._record(payload, direction)
        transition = _original_data_key_transition(
            payload, direction, self._relay_ordinal + 1
        )
        if transition is None:
            return
        key_selection = int(transition["key_selection"])
        with self._lock:
            if self._last_key_selection.get(direction) == key_selection:
                return
            self._last_key_selection[direction] = key_selection
            if len(self._data_key_transitions) >= MAXIMUM_KEY_TRANSITION_EVENTS:
                self._data_key_transitions_omitted += 1
                return
            self._data_key_transitions.append(transition)

    def _record_runtime_key_material(
        self, payload: bytes, direction: str
    ) -> None:
        description = describe_runtime_key_material_datagram(
            payload, direction
        )
        if description is None:
            return
        with self._lock:
            if (
                len(self._runtime_key_material_events)
                < MAX_RUNTIME_KEY_MATERIAL_EVENTS
            ):
                self._runtime_key_material_events.append(
                    {
                        **description,
                        "relay_ordinal": self._relay_ordinal + 1,
                    }
                )
            else:
                self._runtime_key_material_events_omitted += 1


class TimingRendezvousTraceProxy(RendezvousTraceProxy):
    """Record Rendezvous roles and causal DATA key-selector changes."""

    def __init__(
        self,
        sender_target_port: int,
        receiver_target_port: int,
        faults: tuple[RendezvousFault, ...] = (),
        *,
        host: str = "127.0.0.1",
    ) -> None:
        super().__init__(
            sender_target_port,
            receiver_target_port,
            faults,
            host=host,
        )
        self._data_key_transitions: list[dict[str, object]] = []
        self._last_key_selection: dict[str, int] = {}
        self._data_key_transitions_omitted = 0

    def render(self, scenario: str) -> str:
        trace = super().render(scenario)
        with self._lock:
            transitions = [
                dict(event) for event in self._data_key_transitions
            ]
            omitted = self._data_key_transitions_omitted
        events = _render_key_transition_events(
            scenario,
            transitions,
            omitted,
            include_first_data=True,
        )
        if not events:
            return trace
        if trace == "<empty>":
            return "\n".join(events)
        return "\n".join((trace, *events))

    def _record(self, payload: bytes, direction: str) -> None:
        super()._record(payload, direction)
        transition = _original_data_key_transition(
            payload, direction, self._relay_ordinal + 1
        )
        if transition is None:
            return
        key_selection = int(transition["key_selection"])
        with self._lock:
            if self._last_key_selection.get(direction) == key_selection:
                return
            self._last_key_selection[direction] = key_selection
            if len(self._data_key_transitions) >= MAXIMUM_KEY_TRANSITION_EVENTS:
                self._data_key_transitions_omitted += 1
                return
            self._data_key_transitions.append(transition)

    def _record_runtime_key_material(
        self, payload: bytes, direction: str
    ) -> None:
        description = describe_runtime_key_material_datagram(
            payload, direction
        )
        if description is None:
            return
        with self._lock:
            if (
                len(self._runtime_key_material_events)
                < MAX_RUNTIME_KEY_MATERIAL_EVENTS
            ):
                self._runtime_key_material_events.append(
                    {
                        **description,
                        "relay_ordinal": self._relay_ordinal + 1,
                    }
                )
            else:
                self._runtime_key_material_events_omitted += 1


@dataclass(frozen=True)
class MeasurementResult:
    samples: tuple[TimingSample, ...]
    effective_receiver_latency_microseconds: int
    source_time_origin_microseconds: int
    receiver_output: str
    sender_output: str
    fault_observations: tuple[dict[str, object], ...] = ()
    security_observation: dict[str, object] | None = None
    fec_observation: dict[str, object] | None = None
    connection_observation: dict[str, object] | None = None
    udp_timestamp_observation: dict[str, object] | None = None
    phase_observations: tuple[dict[str, object], ...] = ()


class UdpCapture:
    def __init__(self, host: str, expected_messages: int) -> None:
        address = ipaddress.ip_address(host)
        family = socket.AF_INET6 if address.version == 6 else socket.AF_INET
        self._socket = socket.socket(family, socket.SOCK_DGRAM)
        try:
            self._socket.bind((host, 0))
            self._socket.settimeout(0.1)
        except BaseException:
            self._socket.close()
            raise
        self.port = int(self._socket.getsockname()[1])
        self.expected_messages = expected_messages
        self.payloads: list[bytes] = []
        self.error: BaseException | None = None
        self._stop = threading.Event()
        self._thread = threading.Thread(
            target=self._receive, name="robotweax-udp-capture", daemon=True
        )

    def _receive(self) -> None:
        try:
            while (
                len(self.payloads) < self.expected_messages
                and not self._stop.is_set()
            ):
                try:
                    payload, _ = self._socket.recvfrom(65_535)
                except TimeoutError:
                    continue
                self.payloads.append(payload)
        except BaseException as error:  # surfaced on the controlling thread
            self.error = error

    def start(self) -> None:
        self._thread.start()

    def wait(self, timeout_seconds: float) -> list[bytes]:
        self._thread.join(timeout_seconds)
        if self._thread.is_alive():
            raise RuntimeError(
                "UDP capture timed out after receiving "
                f"{len(self.payloads)}/{self.expected_messages} messages"
            )
        if self.error is not None:
            raise RuntimeError(f"UDP capture failed: {self.error}")
        if len(self.payloads) != self.expected_messages:
            raise RuntimeError(
                "UDP capture ended after receiving "
                f"{len(self.payloads)}/{self.expected_messages} messages"
            )
        return self.payloads

    def close(self) -> None:
        self._stop.set()
        self._thread.join(1.0)
        self._socket.close()

    def __enter__(self) -> "UdpCapture":
        self.start()
        return self

    def __exit__(self, *_: object) -> None:
        self.close()


def generated_messages(
    profile: str,
    message_count: int,
    bitrate_bits_per_second: int,
) -> list[GeneratedMessage]:
    if message_count <= 0 or bitrate_bits_per_second <= 0:
        raise ValueError("message count and bitrate must be positive")
    if profile == "binary-1200":
        return generate_binary_messages(
            1_200, message_count, bitrate_bits_per_second
        )
    try:
        packets_per_message = PROFILE_PACKETS[profile]
    except KeyError as error:
        raise ValueError(f"unknown timing profile: {profile}") from error
    return generate_mpeg_ts_messages(
        MpegTsProfile(
            packets_per_message=packets_per_message,
            message_count=message_count,
            bitrate_bits_per_second=bitrate_bits_per_second,
            pcr_interval_packets=packets_per_message,
        )
    )


def fault_plan(
    profile: str, message_count: int
) -> tuple[RendezvousFault, ...]:
    if profile == "none":
        return ()
    if profile == "fec-source-drop":
        return (
            RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=FEC_SOURCE_DROP_OCCURRENCE,
            ),
        )
    if profile == "fec-burst-drop":
        if message_count <= FEC_BURST_DROP_OCCURRENCES[-1]:
            raise ValueError(
                "fec-burst-drop requires at least eight messages"
            )
        return tuple(
            RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=occurrence,
            )
            for occurrence in FEC_BURST_DROP_OCCURRENCES
        )
    if profile == "fec-burst-delay-reorder":
        if message_count < FEC_COMPOUND_MINIMUM_MESSAGES:
            raise ValueError(
                "fec-burst-delay-reorder requires at least "
                f"{FEC_COMPOUND_MINIMUM_MESSAGES} messages"
            )
        return (
            *(
                RendezvousFault(
                    action="drop",
                    direction="sender_to_receiver",
                    occurrence=occurrence,
                )
                for occurrence in FEC_BURST_DROP_OCCURRENCES
            ),
            RendezvousFault(
                action="delay",
                direction="sender_to_receiver",
                occurrence=message_count // 3,
                delay_milliseconds=FAULT_DELAY_MILLISECONDS,
            ),
            RendezvousFault(
                action="reorder",
                direction="sender_to_receiver",
                occurrence=message_count * 2 // 3,
            ),
        )
    if profile != "loss-delay-reorder":
        raise ValueError(f"unknown timing fault profile: {profile}")
    if message_count < 8:
        raise ValueError(
            "loss-delay-reorder requires at least eight messages"
        )
    return (
        RendezvousFault(
            action="delay",
            direction="sender_to_receiver",
            occurrence=message_count // 4,
            delay_milliseconds=FAULT_DELAY_MILLISECONDS,
        ),
        RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=message_count // 2,
        ),
        RendezvousFault(
            action="reorder",
            direction="sender_to_receiver",
            occurrence=message_count * 3 // 4,
        ),
    )


def validate_fault_observations(
    plan: Sequence[RendezvousFault],
    relay: CallerListenerFaultProxy | TimingRendezvousTraceProxy | None,
) -> tuple[dict[str, object], ...]:
    if not plan:
        return ()
    if relay is None:
        raise RuntimeError("timing fault plan has no active relay")
    if relay.error() is not None:
        raise RuntimeError(f"timing fault relay failed: {relay.error()}")
    observations = relay.fault_observations()
    if len(observations) != len(plan):
        raise RuntimeError(
            f"only {len(observations)}/{len(plan)} timing faults were injected"
        )
    for index, (expected, observed) in enumerate(
        zip(plan, observations, strict=True)
    ):
        if (
            observed.get("plan_index") != index + 1
            or observed.get("action") != expected.action
            or observed.get("direction") != expected.direction
            or observed.get("occurrence") != expected.occurrence
            or observed.get("packet_kind") != expected.packet_kind
        ):
            raise RuntimeError(
                "timing fault observation does not match its plan"
            )
        if expected.action == "delay" and (
            observed.get("delay_milliseconds")
            != expected.delay_milliseconds
            or observed.get("delay_released") is not True
            or type(observed.get("delayed_datagrams")) is not int
            or int(observed["delayed_datagrams"]) <= 0
            or type(observed.get("delay_elapsed_milliseconds"))
            not in (int, float)
            or float(observed["delay_elapsed_milliseconds"])
            < expected.delay_milliseconds
        ):
            raise RuntimeError(
                "timing delay fault has no complete release evidence"
            )
        if expected.action == "drop" and (
            observed.get("later_data_observed_before_retransmission")
            is not True
            or observed.get("loss_report_observed") is not True
            or observed.get("retransmission_observed") is not True
            or observed.get("retransmission_flag") is not True
            or observed.get("retransmission_ciphertext_matches") is not True
            or observed.get("retransmission_key_selection")
            != observed.get("key_selection")
        ):
            raise RuntimeError(
                "timing drop fault has no exact causal recovery evidence"
            )
        if expected.action == "reorder" and (
            observed.get("reorder_released") is not True
            or type(observed.get("reorder_partner_sequence")) is not int
            or type(observed.get("sequence")) is not int
            or (
                (
                    int(observed["reorder_partner_sequence"])
                    - int(observed["sequence"])
                )
                & 0x7FFF_FFFF
            )
            != 1
            or observed.get("reorder_partner_retransmitted") is not False
            or observed.get("reorder_partner_forwarded_first") is not True
        ):
            raise RuntimeError(
                "timing reorder fault has no complete release evidence"
            )
    return tuple(observations)


def _required_integer(event: dict[str, object], name: str) -> int:
    value = event.get(name)
    if type(value) is not int:
        raise RuntimeError(f"timing peer field {name} is not an integer")
    return value


def _trace_events(trace: str) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for line in trace.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events


def _filtercap_directions(trace: str) -> set[str]:
    directions: set[str] = set()
    for event in _trace_events(trace):
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


def _validate_compound_fec_retransmissions(
    relay: CallerListenerFaultProxy | TimingRendezvousTraceProxy,
    observations: Sequence[dict[str, object]],
    sender_retransmissions: int,
    source_payload_bytes: int,
) -> tuple[dict[str, object], ...]:
    if relay.arq_trace_complete() is not True:
        raise RuntimeError(
            "timing compound FEC retransmission trace was truncated"
        )
    loss_reports = relay.loss_report_observations()
    if loss_reports:
        raise RuntimeError(
            "timing compound FEC observed an unexpected loss report: "
            f"{loss_reports[0]}"
        )
    retransmissions = relay.retransmission_observations(
        "sender_to_receiver"
    )
    if len(retransmissions) != sender_retransmissions:
        raise RuntimeError(
            "timing compound FEC retransmission counter/trace mismatch: "
            f"counter={sender_retransmissions}, "
            f"trace={len(retransmissions)}"
        )
    delayed_datagrams = sum(
        int(observation["delayed_datagrams"])
        for observation in observations
        if observation.get("action") == "delay"
        and type(observation.get("delayed_datagrams")) is int
    )
    maximum_retransmissions = (
        delayed_datagrams
        + FEC_COMPOUND_MAXIMUM_PRE_DELAY_ACK_FLIGHT_PACKETS
    )
    if len(retransmissions) > maximum_retransmissions:
        raise RuntimeError(
            "timing compound FEC non-drop retransmission batch exceeds "
            "its causal delay flight: "
            f"observed={len(retransmissions)}, "
            f"delayed_datagrams={delayed_datagrams}, "
            "maximum_pre_delay_ack_flight="
            f"{FEC_COMPOUND_MAXIMUM_PRE_DELAY_ACK_FLIGHT_PACKETS}"
        )

    planned_faults = {
        (observation["sequence"], observation["message_number"]): observation
        for observation in observations
    }
    source_drop_identities = {
        identity
        for identity, observation in planned_faults.items()
        if observation.get("action") == "drop"
    }
    retransmission_identities: set[tuple[object, object]] = set()
    observed_ordinals: set[int] = set()
    normalized: list[dict[str, object]] = []
    for retransmission in retransmissions:
        sequence = retransmission.get("sequence")
        message_number = retransmission.get("message_number")
        key_selection = retransmission.get("key_selection")
        payload_digest = retransmission.get("payload_sha256")
        relay_ordinal = retransmission.get("relay_ordinal")
        original_ordinal = retransmission.get("original_relay_ordinal")
        delay_microseconds = retransmission.get(
            "retransmission_delay_microseconds"
        )
        identity = (sequence, message_number)
        if identity in source_drop_identities:
            raise RuntimeError(
                "timing compound FEC source-drop identity used "
                f"retransmission fallback: sequence={sequence}, "
                f"message_number={message_number}"
            )
        if identity in retransmission_identities:
            raise RuntimeError(
                "timing compound FEC repeated an unrelated "
                f"retransmission identity: sequence={sequence}, "
                f"message_number={message_number}"
            )
        if (
            retransmission.get("direction") != "sender_to_receiver"
            or type(sequence) is not int
            or not 0 <= sequence <= 0x7FFF_FFFF
            or type(message_number) is not int
            or not 0 < message_number <= 0x03FF_FFFF
            or type(key_selection) is not int
            or key_selection not in (0, 1, 2)
            or retransmission.get("original_observed") is not True
            or retransmission.get("ciphertext_matches_original") is not True
            or retransmission.get("original_key_selection") != key_selection
            or retransmission.get("payload_bytes") != source_payload_bytes
            or retransmission.get("original_payload_bytes")
            != source_payload_bytes
            or not isinstance(payload_digest, str)
            or len(payload_digest) != 64
            or any(
                character not in "0123456789abcdef"
                for character in payload_digest
            )
            or retransmission.get("original_payload_sha256")
            != payload_digest
            or type(original_ordinal) is not int
            or type(relay_ordinal) is not int
            or not 0 < original_ordinal < relay_ordinal
            or type(delay_microseconds) is not int
            or delay_microseconds < 0
        ):
            raise RuntimeError(
                "timing compound FEC unrelated retransmission lacks "
                f"exact immutable original DATA evidence: {retransmission}"
            )
        planned_fault = planned_faults.get(identity)
        planned_companion_action: str | None = None
        if planned_fault is not None:
            action = planned_fault.get("action")
            if (
                action not in {"delay", "reorder"}
                or planned_fault.get("key_selection") != key_selection
                or planned_fault.get("payload_bytes")
                != source_payload_bytes
                or planned_fault.get("payload_sha256") != payload_digest
            ):
                raise RuntimeError(
                    "timing compound FEC companion-fault retry does not "
                    f"match its planned original DATA: {retransmission}"
                )
            planned_companion_action = str(action)
        if (
            original_ordinal in observed_ordinals
            or relay_ordinal in observed_ordinals
        ):
            raise RuntimeError(
                "timing compound FEC repeated an original or "
                f"retransmission relay ordinal: original={original_ordinal}, "
                f"retransmission={relay_ordinal}"
            )
        retransmission_identities.add(identity)
        observed_ordinals.update((original_ordinal, relay_ordinal))
        normalized.append(
            {
                "sequence": sequence,
                "message_number": message_number,
                "key_selection": key_selection,
                "payload_sha256": payload_digest,
                "retransmission_delay_microseconds": delay_microseconds,
                "planned_companion_action": planned_companion_action,
            }
        )
    if normalized and max(
        int(event["retransmission_delay_microseconds"])
        for event in normalized
    ) < FEC_COMPOUND_MINIMUM_RTO_AGE_MICROSECONDS:
        raise RuntimeError(
            "timing compound FEC unrelated retries do not prove a sender "
            f"RTO age of {FEC_COMPOUND_MINIMUM_RTO_AGE_MICROSECONDS} "
            "microseconds"
        )
    return tuple(normalized)


def validate_fec_evidence(
    fec: FecProfile,
    plan: Sequence[RendezvousFault],
    sender: dict[str, object],
    receiver: dict[str, object],
    relay: CallerListenerFaultProxy | TimingRendezvousTraceProxy | None,
    source_packets: int,
    source_payload_bytes: int,
) -> tuple[tuple[dict[str, object], ...], dict[str, object]]:
    if not fec.enabled or relay is None:
        raise RuntimeError("timing FEC has no active recovery relay")
    if relay.error() is not None:
        raise RuntimeError(f"timing FEC relay failed: {relay.error()}")
    observations = relay.fault_observations()
    if not plan or len(observations) != len(plan):
        raise RuntimeError(
            "timing FEC observed "
            f"{len(observations)}/{len(plan)} planned faults"
        )
    normalized_drops: list[dict[str, object]] = []
    normalized_companion_faults: list[dict[str, object]] = []
    observed_socket_ids: set[int] = set()
    for index, (expected, observed) in enumerate(
        zip(plan, observations, strict=True)
    ):
        sequence = observed.get("sequence")
        message_number = observed.get("message_number")
        destination_socket_id = observed.get("destination_socket_id")
        payload_digest = observed.get("payload_sha256")
        if (
            expected.direction != "sender_to_receiver"
            or expected.packet_kind != "data"
            or observed.get("plan_index") != index + 1
            or observed.get("action") != expected.action
            or observed.get("direction") != expected.direction
            or observed.get("occurrence") != expected.occurrence
            or observed.get("packet_kind") != expected.packet_kind
            or observed.get("filter_control") is not False
            or observed.get("retransmitted") is not False
            or observed.get("payload_bytes") != source_payload_bytes
            or type(sequence) is not int
            or not 0 <= sequence <= 0x7FFF_FFFF
            or type(message_number) is not int
            or not 0 < message_number <= 0x03FF_FFFF
            or observed.get("key_selection") not in (0, 1, 2)
            or type(destination_socket_id) is not int
            or destination_socket_id <= 0
            or not isinstance(payload_digest, str)
            or len(payload_digest) != 64
            or any(
                character not in "0123456789abcdef"
                for character in payload_digest
            )
        ):
            raise RuntimeError(
                "timing FEC fault lacks exact source DATA identity"
            )
        observed_socket_ids.add(destination_socket_id)
        if expected.action == "delay":
            if (
                observed.get("delay_milliseconds")
                != expected.delay_milliseconds
                or observed.get("delay_released") is not True
                or type(observed.get("delayed_datagrams")) is not int
                or int(observed["delayed_datagrams"]) <= 0
                or type(observed.get("delay_elapsed_milliseconds"))
                not in (int, float)
                or float(observed["delay_elapsed_milliseconds"])
                < expected.delay_milliseconds
                or "loss_report_observed" in observed
                or "retransmission_observed" in observed
            ):
                raise RuntimeError(
                    "timing FEC companion delay lacks complete release "
                    "evidence"
                )
            normalized_companion_faults.append(
                {
                    "plan_index": index + 1,
                    "action": expected.action,
                    "occurrence": expected.occurrence,
                    "sequence": sequence,
                    "message_number": message_number,
                    "delay_milliseconds": expected.delay_milliseconds,
                    "delayed_datagrams": observed["delayed_datagrams"],
                    "delay_elapsed_milliseconds": observed[
                        "delay_elapsed_milliseconds"
                    ],
                }
            )
            continue
        if expected.action == "reorder":
            if (
                observed.get("reorder_released") is not True
                or type(observed.get("reorder_partner_sequence")) is not int
                or (
                    (
                        int(observed["reorder_partner_sequence"])
                        - int(sequence)
                    )
                    & 0x7FFF_FFFF
                )
                != 1
                or observed.get("reorder_partner_retransmitted") is not False
                or observed.get("reorder_partner_forwarded_first") is not True
                or "loss_report_observed" in observed
                or "retransmission_observed" in observed
            ):
                raise RuntimeError(
                    "timing FEC companion reorder lacks complete release "
                    "evidence"
                )
            normalized_companion_faults.append(
                {
                    "plan_index": index + 1,
                    "action": expected.action,
                    "occurrence": expected.occurrence,
                    "sequence": sequence,
                    "message_number": message_number,
                    "reorder_partner_sequence": observed[
                        "reorder_partner_sequence"
                    ],
                }
            )
            continue
        if (
            expected.action != "drop"
            or observed.get("later_data_observed_before_retransmission")
            is not True
            or "loss_report_observed" in observed
            or "retransmission_observed" in observed
        ):
            raise RuntimeError(
                "timing FEC source drop lacks exact no-ARQ recovery evidence"
            )
        normalized_drops.append(
            {
                "sequence": sequence,
                "message_number": message_number,
                "key_selection": observed.get("key_selection"),
                "destination_socket_id": destination_socket_id,
                "payload_sha256": payload_digest,
            }
        )

    if not normalized_drops:
        raise RuntimeError("timing FEC plan contains no source drop")
    if len(normalized_drops) > 1:
        if fec.name not in {"column", "matrix"}:
            raise RuntimeError(
                "timing FEC burst requires Column or Matrix geometry"
            )
        destination_socket_ids = {
            int(dropped["destination_socket_id"])
            for dropped in normalized_drops
        }
        if len(destination_socket_ids) != 1:
            raise RuntimeError(
                "timing FEC burst crossed receiver socket identities"
            )
        for previous, current in zip(
            normalized_drops, normalized_drops[1:]
        ):
            if (
                (int(current["sequence"]) - int(previous["sequence"]))
                & 0x7FFF_FFFF
            ) != 1 or (
                (
                    int(current["message_number"])
                    - int(previous["message_number"])
                )
                & 0x03FF_FFFF
            ) != 1:
                raise RuntimeError(
                    "timing FEC burst source drops are not contiguous"
                )
    if len(observed_socket_ids) != 1:
        raise RuntimeError(
            "timing FEC faults crossed receiver socket identities"
        )

    expected_control = fec.expected_control_packets(source_packets)
    expected_reconstructions = len(normalized_drops)
    sender_extra = nonnegative_statistic(sender, "pktSndFilterExtraTotal")
    sender_retransmissions = nonnegative_statistic(sender, "pktRetransTotal")
    sender_received_naks = nonnegative_statistic(sender, "pktRecvNAKTotal")
    receiver_extra = _required_integer(receiver, "receiver_filter_extra_packets")
    receiver_supply = _required_integer(
        receiver, "receiver_filter_supply_packets"
    )
    receiver_loss = _required_integer(receiver, "receiver_filter_loss_packets")
    compound_faults = bool(normalized_companion_faults)
    if (
        expected_control <= 0
        or sender_extra is None
        or sender_extra != expected_control
        or receiver_extra != expected_control
        or receiver_supply != expected_reconstructions
        or receiver_loss != 0
        or sender_retransmissions is None
        or (not compound_faults and sender_retransmissions != 0)
        or sender_received_naks != 0
    ):
        raise RuntimeError(
            "timing FEC statistics do not prove reconstruction: "
            f"profile={fec.name}, expected_controls={expected_control}, "
            f"sender_controls={sender_extra}, receiver_controls="
            f"{receiver_extra}, receiver_supplies={receiver_supply}, "
            f"receiver_losses={receiver_loss}, sender_retransmissions="
            f"{sender_retransmissions}, sender_received_naks="
            f"{sender_received_naks}"
        )

    unrelated_retransmissions: tuple[dict[str, object], ...] = ()
    if compound_faults:
        unrelated_retransmissions = _validate_compound_fec_retransmissions(
            relay,
            observations,
            sender_retransmissions,
            source_payload_bytes,
        )

    trace = relay.render("live-timing-encrypted-fec")
    if _filtercap_directions(trace) != {
        "sender_to_receiver",
        "receiver_to_sender",
    }:
        raise RuntimeError(
            "timing FEC FILTERCAP was not observed in both directions"
        )
    evidence: dict[str, object] = {
        "profile": fec.name,
        "packet_filter": fec.packet_filter,
        "source_drops": normalized_drops,
        "companion_faults": normalized_companion_faults,
        "expected_reconstructed_packets": expected_reconstructions,
        "expected_control_packets": expected_control,
        "sender_control_packets": sender_extra,
        "receiver_control_packets": receiver_extra,
        "receiver_reconstructed_packets": receiver_supply,
        "receiver_unrecovered_packets": receiver_loss,
        "sender_retransmissions": sender_retransmissions,
        "compound_delay_flight_datagrams": sum(
            int(fault["delayed_datagrams"])
            for fault in normalized_companion_faults
            if fault.get("action") == "delay"
        ),
        "non_drop_sender_rto_retransmissions": unrelated_retransmissions,
        "sender_received_naks": sender_received_naks,
    }
    if len(normalized_drops) == 1:
        only_drop = normalized_drops[0]
        evidence.update(
            {
                "source_drop_sequence": only_drop["sequence"],
                "source_drop_message_number": only_drop["message_number"],
                "source_drop_key_selection": only_drop["key_selection"],
                "source_drop_payload_sha256": only_drop["payload_sha256"],
            }
        )
    return tuple(observations), evidence


def parse_receiver_output(
    output: str,
    expected_messages: int,
    expected_key_length: int = 0,
    expected_packet_filter: str = "",
    expected_group_members: int = 0,
    expected_crypto_mode: str | None = None,
) -> tuple[int, list[dict[str, object]], dict[str, object]]:
    connected_events: list[dict[str, object]] = []
    timing_events: list[dict[str, object]] = []
    complete_events: list[dict[str, object]] = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if not isinstance(event, dict):
            continue
        if event.get("event") == "connected":
            connected_events.append(event)
        elif event.get("event") == "timing":
            timing_events.append(event)
        elif event.get("event") == "complete":
            complete_events.append(event)
    if len(connected_events) != 1:
        raise RuntimeError(
            "timing peer reported "
            f"{len(connected_events)} connected events"
        )
    connected = connected_events[0]
    latency_microseconds = _required_integer(
        connected, "receiver_latency_microseconds"
    )
    if latency_microseconds is None or latency_microseconds < 0:
        raise RuntimeError("timing peer did not report effective latency")
    if expected_group_members != 0:
        if connected.get("group_members") != expected_group_members:
            raise RuntimeError(
                "timing receiver did not acquire the requested group"
            )
    elif "group_members" in connected:
        raise RuntimeError("non-group timing reported group membership")
    if expected_key_length != 0:
        key_length = _required_integer(connected, "receiver_key_length")
        key_state = _required_integer(connected, "receiver_key_state")
        if (
            key_length != expected_key_length
            or key_state != SECURED_KEY_STATE
        ):
            raise RuntimeError(
                "timing receiver did not establish the requested security"
            )
    elif (
        "receiver_key_length" in connected
        or "receiver_key_state" in connected
    ):
        raise RuntimeError("clear timing unexpectedly reported security state")
    if expected_crypto_mode is not None:
        expected_mode_value = {"ctr": 1, "gcm": 2}[expected_crypto_mode]
        if connected.get("receiver_crypto_mode") != expected_mode_value:
            raise RuntimeError(
                "timing receiver did not establish the requested crypto mode"
            )
    elif "receiver_crypto_mode" in connected:
        raise RuntimeError("timing unexpectedly reported a crypto mode")
    if expected_packet_filter:
        if (
            connected.get("receiver_packet_filter_matched") is not True
            or connected.get("receiver_packet_filter_bytes")
            != len(expected_packet_filter)
        ):
            raise RuntimeError(
                "timing receiver did not establish the requested FEC filter"
            )
    elif (
        "receiver_packet_filter_matched" in connected
        or "receiver_packet_filter_bytes" in connected
    ):
        raise RuntimeError("non-FEC timing unexpectedly reported a filter")
    complete_event: dict[str, object] = {}
    complete = len(complete_events) == 1
    if complete:
        complete_event = complete_events[0]
        complete = complete_event.get("messages") == expected_messages
        if expected_key_length != 0:
            undecryptable = _required_integer(
                complete_event, "receiver_undecryptable_packets"
            )
            if undecryptable != 0:
                raise RuntimeError(
                    "timing receiver reported undecryptable packets"
                )
        elif "receiver_undecryptable_packets" in complete_event:
            raise RuntimeError(
                "clear timing unexpectedly reported crypto statistics"
            )
        fec_statistic_names = (
            "receiver_filter_extra_packets",
            "receiver_filter_supply_packets",
            "receiver_filter_loss_packets",
        )
        if expected_packet_filter:
            for name in fec_statistic_names:
                if _required_integer(complete_event, name) < 0:
                    raise RuntimeError(
                        f"timing peer field {name} is negative"
                    )
        elif any(name in complete_event for name in fec_statistic_names):
            raise RuntimeError(
                "non-FEC timing unexpectedly reported filter statistics"
            )
    if len(timing_events) != expected_messages or not complete:
        raise RuntimeError(
            "timing peer reported "
            f"{len(timing_events)}/{expected_messages} observations"
        )
    return latency_microseconds, timing_events, complete_event


def _nearest_rank_integer(
    values: Sequence[int], numerator: int, denominator: int
) -> int:
    if not values or numerator <= 0 or numerator > denominator:
        raise ValueError("invalid nearest-rank input")
    ordered = sorted(values)
    rank = (len(ordered) * numerator + denominator - 1) // denominator
    return ordered[rank - 1]


def validate_udp_timestamp_evidence(
    timing_events: Sequence[dict[str, object]],
    complete_event: dict[str, object],
    requested_source: str,
) -> dict[str, object]:
    if requested_source not in UDP_TIMESTAMP_SOURCES:
        raise ValueError(f"unknown UDP timestamp source: {requested_source}")
    event_fields = {
        "udp_userspace_realtime_nanoseconds",
        "udp_kernel_software_tx_realtime_nanoseconds",
        "udp_kernel_timestamp_id",
    }
    complete_fields = {
        "udp_kernel_timestamp_source",
        "udp_kernel_timestamp_clock",
        "udp_kernel_timestamp_hardware",
        "udp_kernel_timestamps",
    }

    def timestamp_fields(event: dict[str, object]) -> set[str]:
        return {
            field
            for field in event
            if field.startswith("udp_kernel_")
            or field.startswith("udp_userspace_")
        }

    if requested_source == "userspace":
        if any(timestamp_fields(event) for event in timing_events) or any(
            field.startswith("udp_kernel_")
            or field.startswith("udp_userspace_")
            for field in complete_event
        ):
            raise RuntimeError(
                "userspace UDP timing unexpectedly reported kernel timestamps"
            )
        return {"requested": False}

    if timestamp_fields(complete_event) != complete_fields:
        raise RuntimeError("kernel UDP timestamp completion metadata differs")
    if (
        complete_event.get("udp_kernel_timestamp_source")
        != "linux-software-tx"
        or complete_event.get("udp_kernel_timestamp_clock")
        != "CLOCK_REALTIME"
        or complete_event.get("udp_kernel_timestamp_hardware") is not False
        or _required_integer(complete_event, "udp_kernel_timestamps")
        != len(timing_events)
    ):
        raise RuntimeError("kernel UDP timestamp completion evidence differs")

    if not timing_events:
        raise RuntimeError("kernel UDP timestamp evidence is empty")
    deltas: list[int] = []
    for index, event in enumerate(timing_events):
        if timestamp_fields(event) != event_fields:
            raise RuntimeError(
                f"kernel UDP timestamp event {index} metadata differs"
            )
        timestamp_id = _required_integer(event, "udp_kernel_timestamp_id")
        kernel_timestamp = _required_integer(
            event, "udp_kernel_software_tx_realtime_nanoseconds"
        )
        userspace_timestamp = _required_integer(
            event, "udp_userspace_realtime_nanoseconds"
        )
        if (
            timestamp_id != index
            or kernel_timestamp <= 0
            or userspace_timestamp <= 0
        ):
            raise RuntimeError(
                f"kernel UDP timestamp event {index} is not correlated"
            )
        deltas.append(kernel_timestamp - userspace_timestamp)

    return {
        "requested": True,
        "source": "linux-software-tx",
        "clock_domain": "CLOCK_REALTIME",
        "hardware": False,
        "correlated_messages": len(timing_events),
        "kernel_tx_minus_userspace_send_completion_nanoseconds": {
            "minimum": min(deltas),
            "p50": _nearest_rank_integer(deltas, 50, 100),
            "p99": _nearest_rank_integer(deltas, 99, 100),
            "maximum": max(deltas),
        },
    }


def _key_material_identity(
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


def validate_security_trace(
    security: SecurityProfile,
    relay: (
        TimingTraceProxy
        | TimingFaultTraceProxy
        | TimingRendezvousTraceProxy
        | None
    ),
) -> dict[str, object]:
    if not security.encrypted:
        if relay is not None:
            raise RuntimeError("clear timing unexpectedly used a crypto trace")
        return {
            "encrypted": False,
            "key_length": 0,
            "data_key_transitions": 0,
            "acknowledged_key_updates": 0,
        }
    if relay is None:
        raise RuntimeError("encrypted timing has no active wire trace")
    if relay.error() is not None:
        raise RuntimeError(f"encrypted timing trace failed: {relay.error()}")
    trace = relay.render("live-timing-encrypted-rotation")
    events = _trace_events(trace)
    if any(
        event.get("event") in {
            "srt_control_trace_truncated",
            "srt_data_key_transition_trace_truncated",
            "srt_handshake_trace_error",
            "srt_runtime_key_material_trace_truncated",
        }
        for event in events
    ):
        raise RuntimeError("encrypted timing trace is incomplete")

    transitions = [
        event
        for event in events
        if event.get("event") == "srt_data_key_transition_trace"
    ]
    transition_directions = {
        event.get("direction") for event in transitions
    }
    if transition_directions == {"caller_to_listener"}:
        forward_direction = "caller_to_listener"
        reverse_direction = "listener_to_caller"
    elif transition_directions == {"sender_to_receiver"}:
        forward_direction = "sender_to_receiver"
        reverse_direction = "receiver_to_sender"
    else:
        raise RuntimeError(
            "encrypted timing DATA direction is missing or inconsistent"
        )
    selections = [event.get("key_selection") for event in transitions]
    ordinals = [event.get("relay_ordinal") for event in transitions]
    sequences = [event.get("sequence") for event in transitions]
    destination_socket_ids = [
        event.get("destination_socket_id") for event in transitions
    ]
    required_selections = security.minimum_key_transitions + 1
    if (
        len(transitions) < required_selections
        or any(type(selection) is not int for selection in selections)
        or any(selection not in (1, 2) for selection in selections)
        or any(type(ordinal) is not int for ordinal in ordinals)
        or any(
            type(sequence) is not int
            or not 0 <= sequence <= 0x7FFF_FFFF
            for sequence in sequences
        )
        or any(
            type(socket_id) is not int or socket_id <= 0
            for socket_id in destination_socket_ids
        )
        or len(set(destination_socket_ids)) != 1
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
            "encrypted timing DATA does not prove repeated key rotation"
        )

    first_data = [
        event
        for event in events
        if event.get("event") == "srt_first_data_trace"
        and event.get("direction") == forward_direction
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
        raise RuntimeError("encrypted timing selector trace misses first DATA")

    key_events = [
        event
        for event in events
        if event.get("event") == "srt_runtime_key_material_trace"
    ]
    requests = [
        event
        for event in key_events
        if event.get("name") == "KMREQ"
        and event.get("direction") == forward_direction
    ]
    responses = [
        event
        for event in key_events
        if event.get("name") == "KMRSP"
        and event.get("direction") == reverse_direction
    ]
    used_material: set[tuple[int, str]] = set()
    for previous, transition in zip(
        transitions, transitions[1:], strict=False
    ):
        previous_ordinal = int(previous["relay_ordinal"])
        data_ordinal = int(transition["relay_ordinal"])
        selection = int(transition["key_selection"])
        candidates: list[tuple[int, str, int, int]] = []
        for request in requests:
            identity = _key_material_identity(request)
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
                if _key_material_identity(response) == identity
                and type(response.get("relay_ordinal")) is int
                and request_ordinal
                < int(response["relay_ordinal"])
                < data_ordinal
            ]
            if response_ordinals:
                candidates.append(
                    (
                        identity[0],
                        identity[1],
                        request_ordinal,
                        min(int(value) for value in response_ordinals),
                    )
                )
        if not candidates:
            raise RuntimeError(
                f"encrypted timing DATA selector {selection} lacks a "
                "causal byte-identical KMREQ/KMRSP"
            )
        selected = max(candidates, key=lambda material: material[3])
        used_material.add((selected[0], selected[1]))

    return {
        "encrypted": True,
        "crypto_mode": security.crypto_mode or "ctr",
        "key_length": security.key_length,
        "key_refresh_rate_packets": security.key_refresh_rate,
        "key_preannouncement_packets": security.key_preannouncement,
        "data_key_transitions": len(transitions) - 1,
        "acknowledged_key_updates": len(used_material),
    }


def validate_group_security_trace(
    security: SecurityProfile,
    relay: GroupTimingTraceProxy | None,
) -> dict[str, object]:
    if not security.encrypted:
        if relay is not None:
            raise RuntimeError(
                "clear Backup-group timing unexpectedly used crypto relays"
            )
        return validate_security_trace(security, None)
    if relay is None:
        raise RuntimeError(
            "encrypted Backup-group timing has no member wire traces"
        )
    per_member_security = SecurityProfile(
        key_length=security.key_length,
        key_refresh_rate=security.key_refresh_rate,
        key_preannouncement=security.key_preannouncement,
        minimum_key_transitions=1,
        crypto_mode=security.crypto_mode,
    )
    paths = {
        "primary": validate_security_trace(
            per_member_security, relay.primary
        ),
        "backup": validate_security_trace(
            per_member_security, relay.backup
        ),
    }
    destination_socket_ids: dict[str, int] = {}
    for name, path_relay in (
        ("primary", relay.primary),
        ("backup", relay.backup),
    ):
        transitions = [
            event
            for event in _trace_events(
                path_relay.render(f"live-timing-group-{name}")
            )
            if event.get("event") == "srt_data_key_transition_trace"
        ]
        socket_ids = {
            event.get("destination_socket_id") for event in transitions
        }
        if (
            len(socket_ids) != 1
            or type(next(iter(socket_ids))) is not int
            or int(next(iter(socket_ids))) <= 0
        ):
            raise RuntimeError(
                f"encrypted Backup-group {name} path has ambiguous DATA identity"
            )
        destination_socket_ids[name] = int(next(iter(socket_ids)))
    if destination_socket_ids["primary"] == destination_socket_ids["backup"]:
        raise RuntimeError(
            "encrypted Backup-group member traces share one receiver identity"
        )
    total_transitions = sum(
        int(path["data_key_transitions"]) for path in paths.values()
    )
    total_updates = sum(
        int(path["acknowledged_key_updates"]) for path in paths.values()
    )
    if total_transitions < security.minimum_key_transitions:
        raise RuntimeError(
            "encrypted Backup-group timing lacks aggregate key rotation"
        )
    return {
        "encrypted": True,
        "crypto_mode": security.crypto_mode or "ctr",
        "key_length": security.key_length,
        "key_refresh_rate_packets": security.key_refresh_rate,
        "key_preannouncement_packets": security.key_preannouncement,
        "data_key_transitions": total_transitions,
        "acknowledged_key_updates": total_updates,
        "member_paths": {
            name: {
                **path,
                "receiver_socket_id": destination_socket_ids[name],
            }
            for name, path in paths.items()
        },
    }


def validate_connection_trace(
    connection_mode: str,
    relay: TimingRendezvousTraceProxy | None,
) -> dict[str, object]:
    if connection_mode == "caller-listener":
        if relay is not None:
            raise RuntimeError(
                "caller/listener timing unexpectedly used a Rendezvous relay"
            )
        return {"mode": connection_mode}
    if connection_mode != "rendezvous":
        raise RuntimeError(f"unknown timing connection mode: {connection_mode}")
    if relay is None:
        raise RuntimeError("Rendezvous timing has no active wire trace")
    if relay.error() is not None:
        raise RuntimeError(f"Rendezvous timing trace failed: {relay.error()}")
    observation = relay.role_observation()
    if observation is None or {
        observation.get("sender_role"),
        observation.get("receiver_role"),
    } != {"initiator", "responder"}:
        raise RuntimeError(
            "Rendezvous timing did not prove a resolved cookie contest"
        )
    sender_socket_id = relay.conclusion_socket_id("sender_to_receiver")
    receiver_socket_id = relay.conclusion_socket_id("receiver_to_sender")
    if (
        type(sender_socket_id) is not int
        or sender_socket_id <= 0
        or type(receiver_socket_id) is not int
        or receiver_socket_id <= 0
    ):
        raise RuntimeError(
            "Rendezvous timing did not observe both positive CONCLUSION "
            "socket IDs"
        )
    events = _trace_events(relay.render("live-timing-rendezvous"))
    if any(
        event.get("event")
        in {
            "srt_control_trace_truncated",
            "srt_data_key_transition_trace_truncated",
            "srt_handshake_trace_error",
        }
        for event in events
    ):
        raise RuntimeError("Rendezvous timing trace is incomplete")
    first_data = [
        event
        for event in events
        if event.get("event") == "srt_first_data_trace"
        and event.get("direction") == "sender_to_receiver"
    ]
    if (
        len(first_data) != 1
        or first_data[0].get("destination_socket_id")
        != receiver_socket_id
        or type(first_data[0].get("sequence")) is not int
        or not 0 <= int(first_data[0]["sequence"]) <= 0x7FFF_FFFF
    ):
        raise RuntimeError(
            "Rendezvous timing DATA is not bound to its receiver CONCLUSION"
        )
    return {
        "mode": connection_mode,
        "sender_cookie_role": observation["sender_role"],
        "receiver_cookie_role": observation["receiver_role"],
        "sender_conclusion_socket_id": sender_socket_id,
        "receiver_conclusion_socket_id": receiver_socket_id,
        "first_data_sequence": first_data[0]["sequence"],
    }


def parse_sender_source_timeline(
    output: str, expected_bytes_per_second: int
) -> int:
    timelines: list[dict[str, object]] = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict) and event.get("event") == "source_timeline":
            timelines.append(event)
    if len(timelines) != 1:
        raise RuntimeError(
            "timing sender reported "
            f"{len(timelines)} explicit source timelines"
        )
    origin = _required_integer(timelines[0], "origin_microseconds")
    bytes_per_second = _required_integer(
        timelines[0], "bytes_per_second"
    )
    if origin <= 0:
        raise RuntimeError("timing sender source origin is not positive")
    if bytes_per_second != expected_bytes_per_second:
        raise RuntimeError(
            "timing sender source rate differs: "
            f"{bytes_per_second} != {expected_bytes_per_second} B/s"
        )
    return origin


def _first_pcr(payload: bytes) -> tuple[int | None, bool]:
    if len(payload) % 188 != 0:
        raise RuntimeError("captured TS message is not 188-byte aligned")
    for offset in range(0, len(payload), 188):
        packet = parse_transport_packet(payload[offset : offset + 188])
        if packet.pcr_ticks is not None:
            return packet.pcr_ticks, packet.discontinuity
    return None, False


def combine_observations(
    messages: Sequence[GeneratedMessage],
    receiver_events: Sequence[dict[str, object]],
    udp_payloads: Sequence[bytes],
    source_time_origin_microseconds: int,
    *,
    mpeg_ts: bool,
) -> list[TimingSample]:
    if source_time_origin_microseconds <= 0:
        raise ValueError("source time origin must be positive")
    if not (
        len(messages) == len(receiver_events) == len(udp_payloads)
    ):
        raise ValueError("message, timing, and UDP counts differ")
    samples: list[TimingSample] = []
    for expected_index, (message, event, payload) in enumerate(
        zip(messages, receiver_events, udp_payloads, strict=True)
    ):
        message_index = _required_integer(event, "message_index")
        if message_index != expected_index or message.index != expected_index:
            raise RuntimeError("timing observations are not ordered")
        deadline = _required_integer(
            event, "tsbpd_deadline_microseconds"
        )
        source_submission = (
            source_time_origin_microseconds
            + message.source_submission_microseconds
        )
        payload_bytes = _required_integer(event, "payload_bytes")
        if payload_bytes != len(payload):
            raise RuntimeError(
                f"UDP message {message_index} has {len(payload)} bytes, "
                f"timing peer reported {payload_bytes}"
            )
        if payload != message.payload:
            raise RuntimeError(
                f"UDP message {message_index} differs from its source payload"
            )
        pcr_ticks, discontinuity = (
            _first_pcr(payload) if mpeg_ts else (None, False)
        )
        samples.append(
            TimingSample(
                message_index=message_index,
                source_submission_microseconds=source_submission,
                tsbpd_deadline_microseconds=deadline,
                srt_release_microseconds=_required_integer(
                    event, "srt_release_microseconds"
                ),
                udp_egress_microseconds=_required_integer(
                    event, "udp_egress_microseconds"
                ),
                payload_bytes=payload_bytes,
                source_payload_sha256=message.payload_sha256,
                egress_payload_sha256=hashlib.sha256(payload).hexdigest(),
                pcr_ticks=pcr_ticks,
                pcr_discontinuity=discontinuity,
            )
        )
    return samples


def _write_payload(path: Path, messages: Sequence[GeneratedMessage]) -> None:
    with path.open("wb") as output:
        for message in messages:
            output.write(message.payload)


def sender_command(
    sender_peer: Path,
    host: str,
    port: int,
    input_path: Path,
    messages: Sequence[GeneratedMessage],
    bitrate_bits_per_second: int,
    latency_milliseconds: int,
    timeout_seconds: int,
    *,
    connection_mode: str = "caller-listener",
    local_port: int | None = None,
    security: SecurityProfile = SecurityProfile(),
    fec: FecProfile = FecProfile(),
) -> list[str]:
    message_size = len(messages[0].payload)
    if connection_mode not in ("caller-listener", "rendezvous"):
        raise ValueError(f"unknown timing connection mode: {connection_mode}")
    if (connection_mode == "rendezvous") != (local_port is not None):
        raise ValueError(
            "Rendezvous timing sender requires exactly one local port"
        )
    command = [
        str(sender_peer),
        "rendezvous-sender" if connection_mode == "rendezvous" else "caller",
    ]
    if local_port is not None:
        command.extend(
            (
                "--local-host",
                host,
                "--local-port",
                str(local_port),
            )
        )
    command.extend(
        (
            "--host",
            host,
            "--port",
            str(port),
            "--bytes",
            str(message_size * len(messages)),
            "--input",
            str(input_path),
            "--transport",
            "live",
            "--chunk-size",
            str(message_size),
            "--latency-ms",
            str(latency_milliseconds),
            "--timeout-ms",
            str(timeout_seconds * 1_000),
            "--input-bw",
            str(bitrate_bits_per_second // 8),
            "--source-pacing",
            "--explicit-source-time",
            "--shutdown-grace-ms",
            "250",
        )
    )
    if security.encrypted:
        command.extend(
            (
                "--passphrase-env",
                PASSPHRASE_ENVIRONMENT,
                "--pbkeylen",
                str(security.key_length),
                "--km-refresh-rate",
                str(security.key_refresh_rate),
                "--km-preannounce",
                str(security.key_preannouncement),
            )
        )
        if security.crypto_mode is not None:
            command.extend(("--crypto-mode", security.crypto_mode))
    if fec.enabled:
        command.extend(("--packet-filter", fec.packet_filter))
    return command


def group_sender_command(
    sender_peer: Path,
    host: str,
    port: int,
    input_path: Path,
    messages: Sequence[GeneratedMessage],
    bitrate_bits_per_second: int,
    timeout_seconds: int,
    failover_after: int,
    *,
    backup_port: int | None = None,
    security: SecurityProfile = SecurityProfile(),
    unacknowledged_replay: bool = False,
    external_path_outage: bool = False,
    shutdown_grace_milliseconds: int = 250,
    peer_idle_timeout_milliseconds: int = 0,
) -> list[str]:
    ipaddress.ip_address(host)
    if not 0 < failover_after < len(messages):
        raise ValueError("Backup-group timing requires an interior failover")
    message_size = len(messages[0].payload)
    if unacknowledged_replay and external_path_outage:
        raise ValueError("Backup-group fault profiles are mutually exclusive")
    if shutdown_grace_milliseconds < 0:
        raise ValueError("Backup-group shutdown grace must be nonnegative")
    if external_path_outage != (peer_idle_timeout_milliseconds > 0):
        raise ValueError(
            "external Backup outage requires one positive peer idle timeout"
        )
    command = [
        str(sender_peer),
        "--host",
        host,
        "--primary-port",
        str(port),
        "--backup-port",
        str(port if backup_port is None else backup_port),
        "--input",
        str(input_path),
        "--messages",
        str(len(messages)),
        "--message-size",
        str(message_size),
        "--input-bw",
        str(bitrate_bits_per_second // 8),
        "--failover-after",
        str(failover_after),
        "--minimum-stability-ms",
        str(
            60
            if external_path_outage
            else min(
                timeout_seconds * 1_000,
                MAXIMUM_GROUP_STABILITY_MILLISECONDS,
            )
        ),
        "--timeout-ms",
        str(timeout_seconds * 1_000),
        "--shutdown-grace-ms",
        str(shutdown_grace_milliseconds),
    ]
    if security.encrypted:
        command.extend(
            (
                "--passphrase-env",
                PASSPHRASE_ENVIRONMENT,
                "--pbkeylen",
                str(security.key_length),
                "--km-refresh-rate",
                str(security.key_refresh_rate),
                "--km-preannounce",
                str(security.key_preannouncement),
            )
        )
        if security.crypto_mode is not None:
            command.extend(("--crypto-mode", security.crypto_mode))
    if unacknowledged_replay:
        command.append("--undrained-failover")
    if external_path_outage:
        command.extend(
            (
                "--external-path-outage",
                "--peer-idle-timeout-ms",
                str(peer_idle_timeout_milliseconds),
            )
        )
    return command


def require_successful_process(
    process: subprocess.CompletedProcess[str], role: str
) -> None:
    if process.returncode != 0:
        raise RuntimeError(
            f"live timing {role} exited with status {process.returncode}"
        )


def group_outage_sender_shutdown_grace(
    latency_milliseconds: int,
) -> int:
    if latency_milliseconds < 0:
        raise ValueError("Backup path outage latency must be nonnegative")
    # Sender drain can complete before TSBPD releases the final message. Keep
    # both member transports alive until after that release, the receiver's
    # own post-measurement grace, and one additional scheduling margin. This
    # prevents receiver statistics from racing the sender's group teardown.
    return (
        latency_milliseconds
        + RECEIVER_SHUTDOWN_GRACE_MILLISECONDS
        + GROUP_OUTAGE_TEARDOWN_MARGIN_MILLISECONDS
    )


def timing_receiver_command(
    timing_peer: Path,
    host: str,
    port: int,
    udp_host: str,
    udp_port: int,
    message_count: int,
    message_size: int,
    latency_milliseconds: int,
    timeout_seconds: int,
    *,
    connection_mode: str = "caller-listener",
    peer_port: int | None = None,
    shutdown_grace_milliseconds: int = 250,
    security: SecurityProfile = SecurityProfile(),
    fec: FecProfile = FecProfile(),
    udp_timestamp_source: str = "userspace",
    phase_timing: bool = False,
) -> list[str]:
    if connection_mode not in CONNECTION_MODES:
        raise ValueError(f"unknown timing connection mode: {connection_mode}")
    if (connection_mode == "rendezvous") != (peer_port is not None):
        raise ValueError(
            "Rendezvous timing receiver requires exactly one peer port"
        )
    if udp_timestamp_source not in UDP_TIMESTAMP_SOURCES:
        raise ValueError(
            f"unknown UDP timestamp source: {udp_timestamp_source}"
        )
    command = [
        str(timing_peer),
        "--host",
        host,
        "--port",
        str(port),
        "--udp-host",
        udp_host,
        "--udp-port",
        str(udp_port),
        "--messages",
        str(message_count),
        "--max-message-size",
        str(max(message_size, REFERENCE_LIVE_MINIMUM_PAYLOAD_BYTES)),
        "--latency-ms",
        str(latency_milliseconds),
        "--timeout-ms",
        str(timeout_seconds * 1_000),
        "--shutdown-grace-ms",
        str(shutdown_grace_milliseconds),
    ]
    if peer_port is not None:
        command.extend(
            (
                "--rendezvous",
                "--peer-host",
                host,
                "--peer-port",
                str(peer_port),
            )
        )
    if connection_mode == "backup-group":
        command.extend(("--group-members", "2"))
    if security.encrypted:
        command.extend(
            (
                "--passphrase-env",
                PASSPHRASE_ENVIRONMENT,
                "--pbkeylen",
                str(security.key_length),
            )
        )
        if security.crypto_mode is not None:
            command.extend(("--crypto-mode", security.crypto_mode))
    if fec.enabled:
        command.extend(("--packet-filter", fec.packet_filter))
    if udp_timestamp_source != "userspace":
        command.extend(
            ("--udp-timestamp-source", udp_timestamp_source)
        )
    if phase_timing:
        command.append("--phase-timing")
    return command


def validate_unacknowledged_group_replay(
    relay: GroupTimingTraceProxy | None,
    failover_after: int,
    message_count: int,
) -> dict[str, object]:
    """Require the exact unacknowledged prefix on both encrypted members."""
    acknowledgement_observation = (
        None
        if relay is None
        else getattr(relay.primary, "acknowledgement_observation", None)
    )
    if (
        relay is None
        or not callable(acknowledgement_observation)
        or relay.primary.error() is not None
        or relay.backup.error() is not None
        or not 0 < failover_after < message_count
    ):
        raise RuntimeError("Backup-group replay has no complete relay evidence")

    acknowledgement = acknowledgement_observation()
    suppressed = acknowledgement.get("acknowledgements")
    if (
        set(acknowledgement)
        != {"suppressed", "forwarded", "acknowledgements"}
        or type(acknowledgement.get("suppressed")) is not int
        or int(acknowledgement["suppressed"]) <= 0
        or type(acknowledgement.get("forwarded")) is not int
        or acknowledgement.get("forwarded") != 0
        or not isinstance(suppressed, list)
        or len(suppressed) != acknowledgement["suppressed"]
    ):
        raise RuntimeError("primary runtime ACK suppression is incomplete")
    for event in suppressed:
        if (
            not isinstance(event, dict)
            or set(event)
            != {
                "direction",
                "acknowledgement_number",
                "next_sequence",
                "payload_bytes",
                "relay_ordinal",
            }
            or event.get("direction") != "listener_to_caller"
            or type(event.get("acknowledgement_number")) is not int
            or type(event.get("next_sequence")) is not int
            or type(event.get("payload_bytes")) is not int
            or int(event["payload_bytes"]) < 4
            or int(event["payload_bytes"]) % 4 != 0
            or type(event.get("relay_ordinal")) is not int
            or int(event["relay_ordinal"]) <= 0
        ):
            raise RuntimeError("suppressed primary ACK evidence is malformed")

    primary_packets = relay.primary.data_packet_observations(
        "caller_to_listener"
    )
    backup_packets = relay.backup.data_packet_observations(
        "caller_to_listener"
    )
    primary_original = tuple(
        event
        for event in primary_packets
        if event.get("retransmitted") is False
    )
    backup_original = tuple(
        event
        for event in backup_packets
        if event.get("retransmitted") is False
    )
    expected_fields = {
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
    if (
        not 0 < len(primary_original) <= failover_after
        or not 0 < len(backup_original) <= message_count
        or any(set(event) != expected_fields for event in primary_packets)
        or any(set(event) != expected_fields for event in backup_packets)
    ):
        raise RuntimeError(
            "Backup-group replay DATA cardinality is invalid: "
            f"primary={len(primary_original)}/{len(primary_packets)} "
            f"maximum={failover_after}, "
            f"backup={len(backup_original)}/{len(backup_packets)} "
            f"maximum={message_count}, "
            f"suppressed_acks={len(suppressed)}"
        )

    def validate_path(
        packets: Sequence[dict[str, object]],
        path_name: str,
        *,
        require_contiguous: bool,
    ) -> int:
        destination_ids = {
            event.get("destination_socket_id") for event in packets
        }
        if (
            len(destination_ids) != 1
            or type(next(iter(destination_ids), None)) is not int
            or int(next(iter(destination_ids))) <= 0
        ):
            raise RuntimeError("Backup-group replay DATA identity is ambiguous")
        for index, event in enumerate(packets):
            digest = event.get("ciphertext_sha256")
            if (
                event.get("direction") != "caller_to_listener"
                or event.get("packet_position") != 3
                or event.get("in_order") is not True
                or event.get("key_selection") not in (1, 2)
                or event.get("retransmitted") is not False
                or type(event.get("sequence")) is not int
                or type(event.get("message_number")) is not int
                or type(event.get("timestamp")) is not int
                or type(event.get("payload_bytes")) is not int
                or int(event["payload_bytes"]) <= 0
                or not isinstance(digest, str)
                or len(digest) != 64
                or any(
                    character not in "0123456789abcdef"
                    for character in digest
                )
            ):
                raise RuntimeError("Backup-group replay DATA evidence is malformed")
            if index == 0:
                continue
            previous = packets[index - 1]
            sequence_distance = (
                int(event["sequence"]) - int(previous["sequence"])
            ) & 0x7FFF_FFFF
            message_distance = (
                int(event["message_number"])
                - int(previous["message_number"])
            ) % 0x03FF_FFFF
            timestamp_distance = (
                int(event["timestamp"]) - int(previous["timestamp"])
            ) & 0xFFFF_FFFF
            if (
                not 0 < sequence_distance < 0x4000_0000
                or sequence_distance != message_distance
                or not 0 < timestamp_distance < 0x8000_0000
                or (require_contiguous and sequence_distance != 1)
            ):
                raise RuntimeError(
                    f"Backup-group {path_name} DATA order is invalid: "
                    f"previous=({previous.get('sequence')},"
                    f"{previous.get('message_number')}), "
                    f"current=({event.get('sequence')},"
                    f"{event.get('message_number')})"
                )
        return int(next(iter(destination_ids)))

    primary_destination = validate_path(
        primary_original, "primary", require_contiguous=True
    )
    backup_destination = validate_path(
        backup_original, "backup", require_contiguous=False
    )
    if primary_destination == backup_destination:
        raise RuntimeError("Backup-group replay reused one crypto endpoint")

    identity_fields = (
        "sequence",
        "message_number",
        "packet_position",
        "in_order",
        "payload_bytes",
    )
    primary_by_sequence = {
        int(event["sequence"]): event for event in primary_original
    }
    backup_by_sequence = {
        int(event["sequence"]): event for event in backup_original
    }
    if (
        len(primary_by_sequence) != len(primary_original)
        or len(backup_by_sequence) != len(backup_original)
    ):
        raise RuntimeError("Backup-group replay DATA identities repeat on one path")
    replayed_sequences = sorted(
        set(primary_by_sequence) & set(backup_by_sequence)
    )
    if len(replayed_sequences) < min(16, len(primary_original)):
        raise RuntimeError("Backup-group outstanding replay overlap is too small")
    timestamp_rebases: set[int] = set()
    for sequence in replayed_sequences:
        primary = primary_by_sequence[sequence]
        replay = backup_by_sequence[sequence]
        timestamp_rebases.add(
            (int(replay["timestamp"]) - int(primary["timestamp"]))
            & 0xFFFF_FFFF
        )
        if (
            any(primary[field] != replay[field] for field in identity_fields)
            or primary["ciphertext_sha256"] == replay["ciphertext_sha256"]
        ):
            raise RuntimeError(
                "Backup-group replay did not preserve metadata and re-encrypt"
            )
    if len(timestamp_rebases) != 1:
        raise RuntimeError(
            "Backup-group replay did not preserve one member-local timebase"
        )
    timestamp_rebase = next(iter(timestamp_rebases))
    signed_timestamp_rebase = (
        timestamp_rebase - 0x1_0000_0000
        if timestamp_rebase >= 0x8000_0000
        else timestamp_rebase
    )

    first_sequence = int(primary_original[0]["sequence"])
    first_message = int(primary_original[0]["message_number"])
    logical_packets = {**primary_by_sequence, **backup_by_sequence}
    offsets = {
        (sequence - first_sequence) & 0x7FFF_FFFF
        for sequence in logical_packets
    }
    if offsets != set(range(message_count)):
        raise RuntimeError("Backup-group replay DATA union is not exact")
    for sequence, event in logical_packets.items():
        offset = (sequence - first_sequence) & 0x7FFF_FFFF
        expected_message = (
            (first_message - 1 + offset) % 0x03FF_FFFF
        ) + 1
        if event.get("message_number") != expected_message:
            raise RuntimeError("Backup-group replay message identity diverged")
    if not any(
        0
        < (int(event["next_sequence"]) - first_sequence) & 0x7FFF_FFFF
        < 0x4000_0000
        for event in suppressed
    ):
        raise RuntimeError("suppressed ACKs do not causally cover replay DATA")
    return {
        "unacknowledged_replay": True,
        "replayed_messages": len(replayed_sequences),
        "primary_original_packets": len(primary_original),
        "backup_original_packets": len(backup_original),
        "suppressed_primary_acknowledgements": len(suppressed),
        "primary_retransmissions": sum(
            event.get("retransmitted") is True for event in primary_packets
        ),
        "metadata_preserved": True,
        "member_local_timestamp_rebase_microseconds": signed_timestamp_rebase,
        "replacement_ciphertext_distinct": True,
        "primary_destination_socket_id": primary_destination,
        "backup_destination_socket_id": backup_destination,
    }


def validate_group_replay_receiver_binding(
    connection_observation: dict[str, object],
    replay_observation: dict[str, object],
) -> None:
    receiver_members = connection_observation.get(
        "receiver_observed_members"
    )
    primary_destination = replay_observation.get(
        "primary_destination_socket_id"
    )
    backup_destination = replay_observation.get(
        "backup_destination_socket_id"
    )
    if (
        not isinstance(receiver_members, list)
        or not receiver_members
        or any(type(member) is not int for member in receiver_members)
        or type(primary_destination) is not int
        or primary_destination <= 0
        or type(backup_destination) is not int
        or backup_destination <= 0
        or primary_destination == backup_destination
        or not set(receiver_members).issubset(
            {primary_destination, backup_destination}
        )
    ):
        raise RuntimeError(
            "Backup replay receiver status is not bound to traced members"
        )


def validate_backup_group_timing(
    sender_output: str,
    sender_complete: dict[str, object],
    receiver_events: Sequence[dict[str, object]],
    failover_after: int,
    source_time_origin_microseconds: int,
    message_size: int,
    bytes_per_second: int,
    *,
    unacknowledged_replay: bool = False,
) -> dict[str, object]:
    events = _trace_events(sender_output)
    connected = [
        event for event in events if event.get("event") == "group_connected"
    ]
    failovers = [
        event for event in events if event.get("event") == "group_failover"
    ]
    sends = [
        event for event in events if event.get("event") == "group_send"
    ]
    if len(connected) != 1 or set(connected[0]) != {
        "event",
        "primary_member",
        "backup_member",
    }:
        raise RuntimeError("group timing lacks exact connection evidence")
    primary = _required_integer(connected[0], "primary_member")
    backup = _required_integer(connected[0], "backup_member")
    if primary <= 0 or backup <= 0 or primary == backup:
        raise RuntimeError("group timing sender members are not distinct")
    if (
        len(failovers) != 1
        or set(failovers[0])
        != {
            "event",
            "after_messages",
            "closed_member",
            "replacement_member",
            "primary_drained",
        }
        or failovers[0].get("after_messages") != failover_after
        or failovers[0].get("closed_member") != primary
        or failovers[0].get("replacement_member") != backup
        or failovers[0].get("primary_drained")
            is unacknowledged_replay
    ):
        raise RuntimeError("group timing lacks exact failover evidence")
    if (
        sender_complete.get("messages") != len(receiver_events)
        or sender_complete.get("primary_closed") is not True
        or sender_complete.get("primary_drained")
            is unacknowledged_replay
        or not 0 < failover_after < len(receiver_events)
        or len(sends) != len(receiver_events)
    ):
        raise RuntimeError("group timing completion is inconsistent")

    for index, event in enumerate(sends):
        expected_time = source_time_origin_microseconds + (
            index * message_size * 1_000_000
        ) // bytes_per_second
        expected_member = primary if index < failover_after else backup
        if (
            set(event)
            != {
                "event",
                "message_index",
                "source_time_microseconds",
                "running_member",
                "member_count",
            }
            or event.get("message_index") != index
            or event.get("source_time_microseconds") != expected_time
            or event.get("running_member") != expected_member
            or type(event.get("member_count")) is not int
            or not 1 <= int(event["member_count"]) <= 2
        ):
            raise RuntimeError(
                "group timing sender transition is invalid at "
                f"{index}: source_time="
                f"{event.get('source_time_microseconds')} expected="
                f"{expected_time}, member={event.get('running_member')} "
                f"expected={expected_member}, count="
                f"{event.get('member_count')}"
            )

    receiver_members: list[int] = []
    for index, event in enumerate(receiver_events):
        member = event.get("group_running_member")
        member_count = event.get("group_member_count")
        if (
            type(member) is not int
            or member <= 0
            or type(member_count) is not int
            or not 1 <= member_count <= 2
        ):
            raise RuntimeError(
                f"group timing receiver evidence is invalid at {index}"
            )
        receiver_members.append(member)
    receiver_primary = receiver_members[0]
    transition_indices = [
        index
        for index in range(1, len(receiver_members))
        if receiver_members[index] != receiver_members[index - 1]
    ]
    receiver_transition = (
        transition_indices[-1] if transition_indices else -1
    )
    receiver_backup = receiver_members[-1]
    observed_receiver_members = sorted(set(receiver_members))
    transition_valid = (
        len(observed_receiver_members) <= 2
        if unacknowledged_replay
        else (
            len(observed_receiver_members) == 2
            and receiver_primary != receiver_backup
            and bool(transition_indices)
            # grpdata is the group status at application receive time, not
            # provenance for an already-buffered message. Concurrent member
            # state publication may therefore alternate immediately before
            # the controlled close, but it must settle on the backup no later
            # than the exact sender failover boundary.
            and receiver_transition <= failover_after
            and not any(
                member == receiver_primary
                for member in receiver_members[failover_after:]
            )
        )
    )
    if not transition_valid:
        raise RuntimeError(
            "group timing receiver transition is invalid: "
            f"indices={transition_indices}, sender_failover={failover_after}, "
            f"unacknowledged_replay={unacknowledged_replay}"
        )
    return {
        "mode": "backup-group",
        "failure_injection": (
            "controlled-primary-srt-close-with-unacknowledged-data"
            if unacknowledged_replay
            else "controlled-primary-srt-close-after-drain"
        ),
        "failover_after_messages": failover_after,
        "sender_primary_member": primary,
        "sender_backup_member": backup,
        "receiver_primary_member": receiver_primary,
        "receiver_backup_member": receiver_backup,
        "receiver_observed_members": observed_receiver_members,
        "member_transitions": 1,
        "receiver_member_transitions": len(transition_indices),
        "receiver_transition_after_messages": (
            receiver_transition if receiver_transition >= 0 else 0
        ),
    }


def validate_backup_group_path_outage(
    sender_output: str,
    sender_complete: dict[str, object],
    receiver_events: Sequence[dict[str, object]],
    relay: GroupPathOutageTraceProxy | None,
    outage_after: int,
    source_time_origin_microseconds: int,
    message_size: int,
    bytes_per_second: int,
    maximum_failover_delay_milliseconds: int,
) -> dict[str, object]:
    if relay is None or maximum_failover_delay_milliseconds <= 0:
        raise RuntimeError("Backup path outage lacks a bounded relay")
    if relay.primary.error() is not None or relay.backup.error() is not None:
        raise RuntimeError("Backup path outage relay failed")

    events = _trace_events(sender_output)
    connected = [
        event for event in events if event.get("event") == "group_connected"
    ]
    failovers = [
        event for event in events if event.get("event") == "group_failover"
    ]
    sends = [
        event for event in events if event.get("event") == "group_send"
    ]
    if len(connected) != 1 or set(connected[0]) != {
        "event",
        "primary_member",
        "backup_member",
    }:
        raise RuntimeError("Backup path outage lacks exact connection evidence")
    primary = _required_integer(connected[0], "primary_member")
    backup = _required_integer(connected[0], "backup_member")
    if primary <= 0 or backup <= 0 or primary == backup:
        raise RuntimeError("Backup path outage sender members are ambiguous")
    expected_failover_fields = {
        "event",
        "after_messages",
        "outage_after_messages",
        "closed_member",
        "replacement_member",
        "failure_injection",
        "primary_closed",
        "primary_drained",
    }
    if len(failovers) != 1 or set(failovers[0]) != expected_failover_fields:
        raise RuntimeError("Backup path outage lacks exact failover evidence")
    transition = _required_integer(failovers[0], "after_messages")
    if (
        not outage_after < transition < len(receiver_events)
        or failovers[0].get("outage_after_messages") != outage_after
        or failovers[0].get("closed_member") != primary
        or failovers[0].get("replacement_member") != backup
        or failovers[0].get("failure_injection") != "external-path-outage"
        or failovers[0].get("primary_closed") is not False
        or failovers[0].get("primary_drained") is not False
        or set(sender_complete)
        != {
            "event",
            "role",
            "bytes",
            "messages",
            "primary_closed",
            "primary_drained",
            "external_path_outage",
        }
        or sender_complete.get("event") != "complete"
        or sender_complete.get("role") != "group-timing-sender"
        or sender_complete.get("bytes")
        != len(receiver_events) * message_size
        or sender_complete.get("messages") != len(receiver_events)
        or sender_complete.get("primary_closed") is not False
        or sender_complete.get("primary_drained") is not False
        or sender_complete.get("external_path_outage") is not True
        or len(sends) != len(receiver_events)
    ):
        raise RuntimeError("Backup path outage completion is inconsistent")

    for index, event in enumerate(sends):
        expected_time = source_time_origin_microseconds + (
            index * message_size * 1_000_000
        ) // bytes_per_second
        expected_member = primary if index < transition else backup
        if (
            set(event)
            != {
                "event",
                "message_index",
                "source_time_microseconds",
                "running_member",
                "member_count",
            }
            or event.get("message_index") != index
            or event.get("source_time_microseconds") != expected_time
            or event.get("running_member") != expected_member
            or event.get("member_count") != 2
        ):
            raise RuntimeError(
                f"Backup path outage sender evidence diverged at {index}"
            )

    outage = relay.outage_observation()
    expected_outage_fields = {
        "trigger_original_data_count",
        "primary_original_data_count",
        "dropped_primary_datagrams",
        "dropped_primary_data_datagrams",
        "outage_started_monotonic_ns",
        "outage_sequence",
        "first_backup_data_monotonic_ns",
        "first_backup_sequence",
        "first_backup_data_delay_microseconds",
    }
    first_backup_delay = outage.get(
        "first_backup_data_delay_microseconds"
    )
    started = outage.get("outage_started_monotonic_ns")
    replacement = outage.get("first_backup_data_monotonic_ns")
    if (
        set(outage) != expected_outage_fields
        or outage.get("trigger_original_data_count") != outage_after
        or outage.get("primary_original_data_count") != outage_after
        or type(outage.get("dropped_primary_datagrams")) is not int
        or type(outage.get("dropped_primary_data_datagrams")) is not int
        or int(outage["dropped_primary_datagrams"])
        < int(outage["dropped_primary_data_datagrams"])
        or int(outage["dropped_primary_data_datagrams"]) <= 0
        or type(started) is not int
        or type(replacement) is not int
        or replacement < started
        or type(first_backup_delay) is not int
        or first_backup_delay != (replacement - started) // 1_000
        or not 0 <= first_backup_delay
        <= maximum_failover_delay_milliseconds * 1_000
        or type(outage.get("outage_sequence")) is not int
        or not 0 <= int(outage["outage_sequence"]) <= 0x7FFF_FFFF
        or type(outage.get("first_backup_sequence")) is not int
        or not 0 <= int(outage["first_backup_sequence"]) <= 0x7FFF_FFFF
    ):
        raise RuntimeError("Backup path outage relay evidence is incomplete")

    outage_source_time = _required_integer(
        sends[outage_after - 1], "source_time_microseconds"
    )
    transition_source_time = _required_integer(
        sends[transition], "source_time_microseconds"
    )
    sender_transition_delay = transition_source_time - outage_source_time
    if not 0 < sender_transition_delay <= (
        maximum_failover_delay_milliseconds * 1_000
    ):
        raise RuntimeError(
            "Backup path outage sender transition exceeds its bound"
        )

    primary_packets = relay.primary.data_packet_observations(
        "caller_to_listener"
    )
    backup_packets = relay.backup.data_packet_observations(
        "caller_to_listener"
    )
    primary_destinations = {
        packet.get("destination_socket_id") for packet in primary_packets
    }
    backup_destinations = {
        packet.get("destination_socket_id") for packet in backup_packets
    }
    # Match the coordinator's trigger: retransmissions are recorded on the
    # wire, but do not advance its original-DATA counter. Still validate the
    # destination identity of every packet, including retransmissions.
    primary_original_packets = tuple(
        packet for packet in primary_packets
        if packet.get("retransmitted") is False
    )
    if (
        len(primary_original_packets) < outage_after
        or primary_original_packets[outage_after - 1].get("sequence")
        != outage["outage_sequence"]
        or not any(
            packet.get("sequence") == outage["first_backup_sequence"]
            and packet.get("retransmitted") is False
            for packet in backup_packets
        )
        or len(primary_destinations) != 1
        or len(backup_destinations) != 1
        or any(
            type(destination) is not int or destination <= 0
            for destination in primary_destinations | backup_destinations
        )
        or primary_destinations == backup_destinations
    ):
        raise RuntimeError(
            "Backup path outage DATA identities are ambiguous: "
            + json.dumps({
                "primary_packet_count": len(primary_packets),
                "primary_original_packet_count": len(primary_original_packets),
                "expected_outage_sequence": outage["outage_sequence"],
                "observed_trigger_sequence": (
                    primary_original_packets[outage_after - 1].get("sequence")
                    if len(primary_original_packets) >= outage_after else None
                ),
                "primary_destinations": list(primary_destinations),
                "backup_destinations": list(backup_destinations),
                "expected_first_backup_sequence": outage["first_backup_sequence"],
            }, sort_keys=True)
        )
    receiver_members: list[int] = []
    receiver_running_counts: list[int] = []
    for index, event in enumerate(receiver_events):
        member = event.get("group_running_member")
        running_count = event.get("group_running_member_count")
        if (
            type(running_count) is not int
            or not 1 <= running_count <= 2
            or type(member) is not int
            or event.get("message_index") != index
            or (running_count == 1 and member <= 0)
            or (running_count > 1 and member != 0)
            or event.get("group_member_count") != 2
        ):
            raise RuntimeError(
                f"Backup path outage receiver evidence diverged at {index}"
            )
        receiver_members.append(member)
        receiver_running_counts.append(running_count)
    receiver_destinations = primary_destinations | backup_destinations
    receiver_primary = next(iter(primary_destinations))
    receiver_backup = next(iter(backup_destinations))
    ambiguous_receiver_membership = any(
        count > 1 for count in receiver_running_counts
    )
    precise_members = [
        member for member in receiver_members if member != 0
    ]
    receiver_transitions: list[int] = []
    first_backup = -1
    settled_transition = -1
    if not ambiguous_receiver_membership:
        receiver_transitions = [
            index
            for index in range(1, len(receiver_members))
            if receiver_members[index] != receiver_members[index - 1]
        ]
        first_backup = next(
            (
                index
                for index, member in enumerate(receiver_members)
                if member == receiver_backup
            ),
            -1,
        )
        settled_transition = (
            receiver_transitions[-1] if receiver_transitions else -1
        )
    precise_evidence_valid = (
        set(precise_members).issubset(receiver_destinations)
        and (
            ambiguous_receiver_membership
            or (
                bool(receiver_transitions)
                # grpdata is a status snapshot, not message provenance.  It
                # may alternate while both members are being ACK-qualified,
                # but every such transition must precede settlement at the
                # externally injected outage boundary.
                and all(
                    index <= outage_after
                    for index in receiver_transitions
                )
                and set(receiver_members) == receiver_destinations
                and receiver_members[0] == receiver_primary
                and receiver_members[-1] == receiver_backup
                and first_backup >= 0
                and settled_transition <= outage_after
                and not any(
                    member == receiver_primary
                    for member in receiver_members[outage_after:]
                )
            )
        )
    )
    if not precise_evidence_valid:
        raise RuntimeError(
            "Backup path outage receiver status is inconsistent: "
            f"members={receiver_members}, "
            f"transitions={receiver_transitions}, "
            f"wire_destinations={sorted(receiver_destinations)}"
        )

    return {
        "mode": "backup-group",
        "failure_injection": "external-primary-path-black-hole",
        "outage_after_messages": outage_after,
        "sender_transition_after_messages": transition,
        "sender_primary_member": primary,
        "sender_backup_member": backup,
        "receiver_observed_members": sorted(receiver_destinations),
        "receiver_member_source_attribution": (
            "not-exposed-by-peer-api"
            if ambiguous_receiver_membership
            else "exact"
        ),
        "receiver_ambiguous_member_observations": sum(
            count > 1 for count in receiver_running_counts
        ),
        "receiver_member_transitions": (
            None
            if ambiguous_receiver_membership
            else len(receiver_transitions)
        ),
        "receiver_first_backup_after_messages": (
            None if ambiguous_receiver_membership else first_backup
        ),
        "receiver_transition_after_messages": (
            None if ambiguous_receiver_membership else settled_transition
        ),
        "first_backup_data_delay_microseconds": first_backup_delay,
        "sender_active_transition_delay_microseconds": (
            sender_transition_delay
        ),
        "maximum_failover_delay_microseconds": (
            maximum_failover_delay_milliseconds * 1_000
        ),
        "dropped_primary_datagrams": outage[
            "dropped_primary_datagrams"
        ],
        "dropped_primary_data_datagrams": outage[
            "dropped_primary_data_datagrams"
        ],
        "outage_sequence": outage["outage_sequence"],
        "first_backup_sequence": outage["first_backup_sequence"],
    }


def phase_observations(
    events: Sequence[dict[str, object]],
) -> tuple[dict[str, object], ...]:
    """Wall time boundaries only; blocking and descheduling are included."""
    result: list[dict[str, object]] = []
    previous_egress: int | None = None
    for index, event in enumerate(events):
        start = _required_integer(event, "receive_start_microseconds")
        release = _required_integer(event, "srt_release_microseconds")
        send = _required_integer(event, "udp_send_start_microseconds")
        egress = _required_integer(event, "udp_egress_microseconds")
        if (
            _required_integer(event, "message_index") != index
            or not 0 < start <= release <= send <= egress
            or (previous_egress is not None and start < previous_egress)
        ):
            raise RuntimeError("invalid phase timing boundary order")
        result.append({
            "message_index": index,
            "receive_start_microseconds": start,
            "srt_release_microseconds": release,
            "udp_send_start_microseconds": send,
            "udp_egress_microseconds": egress,
            "tsbpd_deadline_microseconds": _required_integer(
                event, "tsbpd_deadline_microseconds"
            ),
            "receive_call_microseconds": release - start,
            "post_receive_microseconds": send - release,
            "udp_send_call_microseconds": egress - send,
            "between_receives_microseconds": (
                None if previous_egress is None else start - previous_egress
            ),
        })
        previous_egress = egress
    return tuple(result)


@contextmanager
def preserve_measurement_evidence(
    directory: Path, receiver_log: Path | None
) -> Iterator[None]:
    """Retain opt-in raw evidence even if validation rejects the transfer."""
    try:
        yield
    finally:
        if receiver_log is not None:
            for source, destination in (
                (directory / "timing-listener.stdout", receiver_log),
                (directory / "path-outage-evidence.json",
                 receiver_log.with_name(receiver_log.name + ".path-outage.json")),
            ):
                if source.is_file():
                    destination.write_bytes(source.read_bytes())


def snapshot_path_outage_evidence(
    relay: GroupPathOutageTraceProxy,
) -> dict[str, object]:
    evidence: dict[str, object] = {"outage": relay.outage_observation()}
    for name, path in (("primary", relay.primary), ("backup", relay.backup)):
        try:
            evidence[name] = path.data_packet_observations("caller_to_listener")
        except RuntimeError as error:
            # Preserve the incomplete-trace reason without treating it as valid
            # evidence. The normal validator still rejects incomplete traces.
            evidence[name] = {"error": str(error)}
    return evidence


def run_measurement(
    timing_peer: Path,
    sender_peer: Path,
    profile: str,
    message_count: int,
    bitrate_bits_per_second: int,
    latency_milliseconds: int,
    timeout_seconds: int,
    host: str,
    udp_host: str,
    directory: Path,
    *,
    connection_mode: str = "caller-listener",
    group_failover_after: int = 0,
    group_unacknowledged_replay: bool = False,
    group_path_outage: bool = False,
    group_maximum_failover_delay_milliseconds: int = 0,
    fault_profile: str = "none",
    security: SecurityProfile = SecurityProfile(),
    fec: FecProfile = FecProfile(),
    udp_timestamp_source: str = "userspace",
    phase_timing: bool = False,
) -> MeasurementResult:
    if connection_mode not in CONNECTION_MODES:
        raise ValueError(f"unknown timing connection mode: {connection_mode}")
    if udp_timestamp_source not in UDP_TIMESTAMP_SOURCES:
        raise ValueError(
            f"unknown UDP timestamp source: {udp_timestamp_source}"
        )
    if bitrate_bits_per_second % 8 != 0:
        raise ValueError("bitrate must be exactly representable in bytes/second")
    faults = fault_plan(fault_profile, message_count)
    fec_fault = fault_profile in {
        "fec-source-drop",
        "fec-burst-drop",
        "fec-burst-delay-reorder",
    }
    if fec.enabled != fec_fault:
        raise ValueError(
            "timing FEC requires exactly one supported FEC fault profile"
        )
    if fault_profile in {
        "fec-burst-drop",
        "fec-burst-delay-reorder",
    } and fec.name not in {
        "column",
        "matrix",
    }:
        raise ValueError(
            "timing FEC burst requires Column or Matrix geometry"
        )
    if faults and not ipaddress.ip_address(host).is_loopback:
        raise ValueError(
            "timing fault profiles require a loopback SRT endpoint"
        )
    if connection_mode == "backup-group":
        if (
            faults
            or fec.enabled
            or not 0 < group_failover_after < message_count
        ):
            raise ValueError(
                "Backup-group timing requires no packet fault or FEC profile "
                "and an interior failover"
            )
        if group_unacknowledged_replay and not security.encrypted:
            raise ValueError(
                "unacknowledged Backup replay requires encryption"
            )
        if group_path_outage and not security.encrypted:
            raise ValueError("Backup path outage requires encryption")
        if group_unacknowledged_replay and group_path_outage:
            raise ValueError("Backup-group faults are mutually exclusive")
        if group_path_outage != (
            group_maximum_failover_delay_milliseconds > 0
        ):
            raise ValueError(
                "Backup path outage requires exactly one positive delay bound"
            )
    elif group_failover_after != 0:
        raise ValueError("Only Backup-group timing accepts a failover point")
    elif group_unacknowledged_replay:
        raise ValueError("Only Backup-group timing accepts replay injection")
    elif group_path_outage:
        raise ValueError("Only Backup-group timing accepts path outage")
    elif group_maximum_failover_delay_milliseconds != 0:
        raise ValueError("Only Backup path outage accepts a delay bound")
    messages = generated_messages(
        profile, message_count, bitrate_bits_per_second
    )
    message_size = len(messages[0].payload)
    if fec.enabled and message_size > FEC_MAXIMUM_SOURCE_BYTES:
        raise ValueError("timing FEC source message exceeds its negotiated MSS")
    input_path = directory / "timing.input"
    _write_payload(input_path, messages)
    listener_stdout_path = directory / "timing-listener.stdout"
    listener_stderr_path = directory / "timing-listener.stderr"
    sender_local_port: int | None = None
    if connection_mode == "rendezvous":
        with reserved_udp_ports(2, host) as endpoint_ports:
            sender_local_port, srt_port = endpoint_ports
            rendezvous_relay = TimingRendezvousTraceProxy(
                sender_local_port,
                srt_port,
                faults,
                host=host,
            )
    else:
        srt_port = free_udp_port(host)
    if connection_mode == "rendezvous":
        relay_context = closing(rendezvous_relay)
    elif connection_mode == "backup-group" and group_path_outage:
        relay_context = closing(
            GroupPathOutageTraceProxy(
                srt_port,
                group_failover_after,
                host=host,
            )
        )
    elif connection_mode == "backup-group" and security.encrypted:
        relay_context = closing(
            GroupTimingTraceProxy(
                srt_port,
                host=host,
                suppress_primary_acknowledgements=(
                    group_unacknowledged_replay
                ),
            )
        )
    elif security.encrypted:
        relay_context = closing(
            TimingFaultTraceProxy(srt_port, faults, host=host)
            if faults
            else TimingTraceProxy(srt_port, host=host)
        )
    elif faults:
        relay_context = closing(
            CallerListenerFaultProxy(srt_port, faults, host=host)
        )
    else:
        relay_context = nullcontext(None)
    environment = os.environ.copy()
    if security.encrypted:
        environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)

    with (
        UdpCapture(udp_host, message_count) as udp_capture,
        relay_context as relay,
        listener_stdout_path.open("w", encoding="utf-8")
        as listener_stdout,
        listener_stderr_path.open("w", encoding="utf-8")
        as listener_stderr,
    ):
        if relay is not None:
            relay.start()
        receiver_peer_port = (
            relay.receiver_port
            if isinstance(relay, TimingRendezvousTraceProxy)
            else None
        )
        listener = subprocess.Popen(
            timing_receiver_command(
                timing_peer,
                host,
                srt_port,
                udp_host,
                udp_capture.port,
                message_count,
                message_size,
                latency_milliseconds,
                timeout_seconds,
                connection_mode=connection_mode,
                peer_port=receiver_peer_port,
                security=security,
                fec=fec,
                udp_timestamp_source=udp_timestamp_source,
                phase_timing=phase_timing,
            ),
            stdout=listener_stdout,
            stderr=listener_stderr,
            text=True,
            env=environment,
        )
        sender: subprocess.CompletedProcess[str] | None = None
        try:
            time.sleep(0.2)
            if listener.poll() is not None:
                raise RuntimeError("timing receiver exited before sender startup")
            backup_sender_port: int | None = None
            if isinstance(relay, TimingRendezvousTraceProxy):
                sender_port = relay.sender_port
            elif isinstance(relay, GroupTimingTraceProxy):
                sender_port = relay.primary_port
                backup_sender_port = relay.backup_port
            else:
                sender_port = relay.port if relay is not None else srt_port
            command = (
                group_sender_command(
                    sender_peer,
                    host,
                    sender_port,
                    input_path,
                    messages,
                    bitrate_bits_per_second,
                    timeout_seconds,
                    group_failover_after,
                    backup_port=backup_sender_port,
                    security=security,
                    unacknowledged_replay=(
                        group_unacknowledged_replay
                    ),
                    external_path_outage=group_path_outage,
                    shutdown_grace_milliseconds=(
                        group_outage_sender_shutdown_grace(
                            latency_milliseconds
                        )
                        if group_path_outage
                        else RECEIVER_SHUTDOWN_GRACE_MILLISECONDS
                    ),
                    peer_idle_timeout_milliseconds=(
                        min(
                            1_000,
                            group_maximum_failover_delay_milliseconds,
                        )
                        if group_path_outage
                        else 0
                    ),
                )
                if connection_mode == "backup-group"
                else sender_command(
                    sender_peer,
                    host,
                    sender_port,
                    input_path,
                    messages,
                    bitrate_bits_per_second,
                    latency_milliseconds,
                    timeout_seconds,
                    connection_mode=connection_mode,
                    local_port=sender_local_port,
                    security=security,
                    fec=fec,
                )
            )
            sender = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=timeout_seconds + 5,
                check=False,
                env=environment,
            )
            # Do not spend a second full timeout waiting for the receiver
            # after the producer has already failed. In particular, this keeps
            # CTest's outer timeout from hiding the sender's stderr.
            require_successful_process(sender, "sender")
            listener.wait(timeout=timeout_seconds + 5)
            udp_payloads = udp_capture.wait(timeout_seconds)
        except (subprocess.TimeoutExpired, RuntimeError) as error:
            terminate(listener)
            listener_stdout.flush()
            listener_stderr.flush()
            listener_output = listener_stdout_path.read_text(
                encoding="utf-8", errors="replace"
            )
            listener_errors = listener_stderr_path.read_text(
                encoding="utf-8", errors="replace"
            )
            raise RuntimeError(
                f"{error}\n"
                f"--- sender stdout ---\n"
                f"{sender.stdout if sender else '<empty>'}\n"
                f"--- sender stderr ---\n"
                f"{sender.stderr if sender else '<empty>'}\n"
                f"--- timing stdout ---\n{listener_output or '<empty>'}\n"
                f"--- timing stderr ---\n{listener_errors or '<empty>'}\n"
                f"--- UDP capture progress ---\n"
                f"{len(udp_capture.payloads)}/{message_count} messages\n"
                f"--- fault trace ---\n"
                f"{relay.render('live-timing') if relay else '<none>'}"
            ) from error
        finally:
            listener_stdout.flush()
            listener_stderr.flush()

    # Snapshot and validate only after both the UDP capture and relay threads
    # have stopped. This makes late relay errors and final wire events part of
    # the same fail-closed evidence boundary as the completed transfer.
    listener_output = listener_stdout_path.read_text(
        encoding="utf-8", errors="replace"
    )
    listener_errors = listener_stderr_path.read_text(
        encoding="utf-8", errors="replace"
    )
    if isinstance(relay, GroupPathOutageTraceProxy):
        (directory / "path-outage-evidence.json").write_text(
            json.dumps(snapshot_path_outage_evidence(relay), sort_keys=True) + "\n",
            encoding="utf-8",
        )
    if sender is None or sender.returncode != 0 or listener.returncode != 0:
        raise RuntimeError(
            "live timing transfer failed\n"
            f"--- sender stdout ---\n{sender.stdout if sender else '<empty>'}\n"
            f"--- sender stderr ---\n{sender.stderr if sender else '<empty>'}\n"
            f"--- timing stdout ---\n{listener_output or '<empty>'}\n"
            f"--- timing stderr ---\n{listener_errors or '<empty>'}\n"
            f"--- fault trace ---\n"
            f"{relay.render('live-timing') if relay else '<none>'}"
        )
    sender_role = (
        "group-timing-sender"
        if connection_mode == "backup-group"
        else (
            "rendezvous-sender"
            if connection_mode == "rendezvous"
            else "caller"
        )
    )
    sender_complete = parse_complete(sender.stdout, sender_role)
    expected_bytes = message_size * message_count
    if sender_complete.get("bytes") != expected_bytes:
        raise RuntimeError("sender completion byte count differs")
    source_time_origin = parse_sender_source_timeline(
        sender.stdout, bitrate_bits_per_second // 8
    )
    effective_latency, receiver_events, receiver_complete = (
        parse_receiver_output(
            listener_output,
            message_count,
            security.key_length,
            fec.packet_filter,
            2 if connection_mode == "backup-group" else 0,
            security.crypto_mode,
        )
    )
    udp_timestamp_observation = validate_udp_timestamp_evidence(
        receiver_events, receiver_complete, udp_timestamp_source
    )
    samples = combine_observations(
        messages,
        receiver_events,
        udp_payloads,
        source_time_origin,
        mpeg_ts=profile in PROFILE_PACKETS,
    )
    if fec.enabled:
        fault_observations, fec_observation = validate_fec_evidence(
            fec,
            faults,
            sender_complete,
            receiver_complete,
            relay,
            message_count,
            message_size,
        )
    else:
        fault_observations = validate_fault_observations(faults, relay)
        fec_observation = None
    security_observation = (
        validate_group_security_trace(
            security,
            relay if isinstance(relay, GroupTimingTraceProxy) else None,
        )
        if connection_mode == "backup-group"
        else validate_security_trace(
            security,
            relay
            if isinstance(
                relay,
                (
                    TimingTraceProxy,
                    TimingFaultTraceProxy,
                    TimingRendezvousTraceProxy,
                ),
            )
            and security.encrypted
            else None,
        )
    )
    if group_path_outage:
        connection_observation = validate_backup_group_path_outage(
            sender.stdout,
            sender_complete,
            receiver_events,
            relay
            if isinstance(relay, GroupPathOutageTraceProxy)
            else None,
            group_failover_after,
            source_time_origin,
            message_size,
            bitrate_bits_per_second // 8,
            group_maximum_failover_delay_milliseconds,
        )
    elif connection_mode == "backup-group":
        connection_observation = validate_backup_group_timing(
            sender.stdout,
            sender_complete,
            receiver_events,
            group_failover_after,
            source_time_origin,
            message_size,
            bitrate_bits_per_second // 8,
            unacknowledged_replay=group_unacknowledged_replay,
        )
    else:
        connection_observation = validate_connection_trace(
            connection_mode,
            relay if isinstance(relay, TimingRendezvousTraceProxy) else None,
        )
    if group_unacknowledged_replay:
        if not isinstance(relay, GroupTimingTraceProxy):
            raise RuntimeError("Backup replay relay type is unavailable")
        replay_observation = validate_unacknowledged_group_replay(
            relay, group_failover_after, message_count
        )
        validate_group_replay_receiver_binding(
            connection_observation, replay_observation
        )
        connection_observation.update(replay_observation)
    return MeasurementResult(
        samples=tuple(samples),
        effective_receiver_latency_microseconds=effective_latency,
        source_time_origin_microseconds=source_time_origin,
        receiver_output=listener_output,
        sender_output=sender.stdout,
        fault_observations=fault_observations,
        security_observation=security_observation,
        fec_observation=fec_observation,
        connection_observation=connection_observation,
        udp_timestamp_observation=udp_timestamp_observation,
        phase_observations=(
            phase_observations(receiver_events) if phase_timing else ()
        ),
    )


def _metric_value(
    scorecard: dict[str, object], metric: str, statistic: str
) -> float:
    summary = scorecard.get(metric)
    if not isinstance(summary, dict):
        raise RuntimeError(f"timing metric {metric} is unavailable")
    value = summary.get(statistic)
    if not isinstance(value, (int, float)):
        raise RuntimeError(
            f"timing metric {metric}.{statistic} is unavailable"
        )
    return float(value)


def enforce_limits(
    scorecard: dict[str, object],
    *,
    maximum_egress_p99_9_microseconds: int | None,
    maximum_tsbpd_phase_range_microseconds: int | None,
    maximum_burst_depth: int | None,
    maximum_pcr_span_rate_error_ppm: float | None,
) -> None:
    if maximum_egress_p99_9_microseconds is not None:
        observed = _metric_value(
            scorecard, "egress_deadline_error_microseconds", "p99_9"
        )
        if observed > maximum_egress_p99_9_microseconds:
            raise RuntimeError(
                "UDP egress p99.9 deadline error exceeds limit: "
                f"{observed} > {maximum_egress_p99_9_microseconds} us"
            )
    if maximum_tsbpd_phase_range_microseconds is not None:
        maximum_latency = _metric_value(
            scorecard, "mapped_tsbpd_latency_microseconds", "maximum"
        )
        minimum_latency = _metric_value(
            scorecard, "mapped_tsbpd_latency_microseconds", "minimum"
        )
        observed_phase_range = maximum_latency - minimum_latency
        if observed_phase_range > maximum_tsbpd_phase_range_microseconds:
            raise RuntimeError(
                "TSBPD phase range exceeds limit: "
                f"{observed_phase_range} > "
                f"{maximum_tsbpd_phase_range_microseconds} us"
            )
    if maximum_burst_depth is not None:
        observed_burst = scorecard.get("maximum_burst_depth")
        if not isinstance(observed_burst, int):
            raise RuntimeError("maximum burst-depth metric is unavailable")
        if observed_burst > maximum_burst_depth:
            raise RuntimeError(
                "UDP egress burst depth exceeds limit: "
                f"{observed_burst} > {maximum_burst_depth}"
            )
    if maximum_pcr_span_rate_error_ppm is not None:
        observed_rate = _metric_value(
            scorecard, "pcr_absolute_span_rate_error_ppm", "maximum"
        )
        if observed_rate > maximum_pcr_span_rate_error_ppm:
            raise RuntimeError(
                "PCR span rate error exceeds limit: "
                f"{observed_rate} > {maximum_pcr_span_rate_error_ppm} ppm"
            )


def write_scorecard(
    scorecard: dict[str, object], output: Path | None
) -> None:
    rendered = json.dumps(scorecard, indent=2, sort_keys=True) + "\n"
    if output is None:
        print(rendered, end="", flush=True)
    else:
        output.write_text(rendered, encoding="utf-8")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timing-peer", type=Path, required=True)
    parser.add_argument("--sender-peer", type=Path, required=True)
    parser.add_argument(
        "--profile",
        choices=(*PROFILE_PACKETS, "binary-1200"),
        default="ts-1316",
    )
    parser.add_argument("--messages", type=int, default=100)
    parser.add_argument("--bitrate-bps", type=int, default=15_040_000)
    parser.add_argument("--latency-ms", type=int, default=120)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--udp-host", default="127.0.0.1")
    parser.add_argument(
        "--udp-timestamp-source",
        choices=UDP_TIMESTAMP_SOURCES,
        default="userspace",
    )
    parser.add_argument(
        "--connection-mode",
        choices=CONNECTION_MODES,
        default="caller-listener",
    )
    parser.add_argument("--group-failover-after", type=int, default=0)
    parser.add_argument(
        "--group-unacknowledged-replay", action="store_true"
    )
    parser.add_argument("--group-path-outage", action="store_true")
    parser.add_argument(
        "--group-maximum-failover-delay-ms", type=int, default=0
    )
    parser.add_argument("--burst-window-us", type=int, default=1_000)
    parser.add_argument(
        "--fault-profile", choices=FAULT_PROFILES, default="none"
    )
    parser.add_argument("--fec-profile", choices=FEC_PROFILES, default="none")
    parser.add_argument("--pbkeylen", type=int, default=0)
    parser.add_argument("--km-refresh-rate", type=int, default=0)
    parser.add_argument("--km-preannounce", type=int, default=0)
    parser.add_argument("--minimum-key-transitions", type=int, default=0)
    parser.add_argument("--crypto-mode", choices=("ctr", "gcm"))
    parser.add_argument("--maximum-egress-p99-9-us", type=int)
    parser.add_argument("--maximum-tsbpd-phase-range-us", type=int)
    parser.add_argument("--maximum-burst-depth", type=int)
    parser.add_argument("--maximum-pcr-span-rate-error-ppm", type=float)
    parser.add_argument("--write-events", type=Path)
    parser.add_argument("--write-receiver-log", type=Path)
    parser.add_argument(
        "--write-phase-events", type=Path,
        help="Opt in to extra receiver clock reads; write wall-time boundaries",
    )
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if (
        arguments.messages <= 0
        or arguments.bitrate_bps <= 0
        or arguments.latency_ms < 0
        or arguments.timeout_seconds <= 0
        or arguments.group_maximum_failover_delay_ms < 0
        or (
            arguments.maximum_egress_p99_9_us is not None
            and arguments.maximum_egress_p99_9_us < 0
        )
        or (
            arguments.maximum_tsbpd_phase_range_us is not None
            and arguments.maximum_tsbpd_phase_range_us < 0
        )
        or (
            arguments.maximum_burst_depth is not None
            and arguments.maximum_burst_depth <= 0
        )
        or (
            arguments.maximum_pcr_span_rate_error_ppm is not None
            and arguments.maximum_pcr_span_rate_error_ppm < 0
        )
    ):
        raise ValueError("measurement values are outside their valid range")
    timing_peer = resolve_program_path(arguments.timing_peer)
    sender_peer = resolve_program_path(arguments.sender_peer)
    security = SecurityProfile(
        key_length=arguments.pbkeylen,
        key_refresh_rate=arguments.km_refresh_rate,
        key_preannouncement=arguments.km_preannounce,
        minimum_key_transitions=arguments.minimum_key_transitions,
        crypto_mode=arguments.crypto_mode,
    )
    fec = FecProfile(arguments.fec_profile)
    with (
        tempfile.TemporaryDirectory(prefix="robotweax-srt-live-timing-")
        as temporary_directory,
        preserve_measurement_evidence(
            Path(temporary_directory), arguments.write_receiver_log
        ),
    ):
        result = run_measurement(
            timing_peer,
            sender_peer,
            arguments.profile,
            arguments.messages,
            arguments.bitrate_bps,
            arguments.latency_ms,
            arguments.timeout_seconds,
            arguments.host,
            arguments.udp_host,
            Path(temporary_directory),
            connection_mode=arguments.connection_mode,
            group_failover_after=arguments.group_failover_after,
            group_unacknowledged_replay=(
                arguments.group_unacknowledged_replay
            ),
            group_path_outage=arguments.group_path_outage,
            group_maximum_failover_delay_milliseconds=(
                arguments.group_maximum_failover_delay_ms
            ),
            fault_profile=arguments.fault_profile,
            security=security,
            fec=fec,
            udp_timestamp_source=arguments.udp_timestamp_source,
            phase_timing=arguments.write_phase_events is not None,
        )
    if arguments.write_phase_events is not None:
        with arguments.write_phase_events.open("w", encoding="utf-8") as output:
            for event in result.phase_observations:
                output.write(json.dumps(event, sort_keys=True) + "\n")
    if arguments.write_events is not None:
        with arguments.write_events.open("w", encoding="utf-8") as output:
            for sample in result.samples:
                output.write(json.dumps(asdict(sample), sort_keys=True) + "\n")
    scorecard = score_timing(
        result.samples, burst_window_microseconds=arguments.burst_window_us
    )
    scorecard["measurement"] = {
        "transport": "robotweax-srt-live",
        "connection": result.connection_observation,
        "srt_address_family": (
            f"ipv{ipaddress.ip_address(arguments.host).version}"
        ),
        "udp_address_family": (
            f"ipv{ipaddress.ip_address(arguments.udp_host).version}"
        ),
        "udp_observation": "userspace-send-completion",
        "phase_timing_enabled": arguments.write_phase_events is not None,
        "udp_kernel_timestamp": result.udp_timestamp_observation,
        "payload_capture": "downstream-loopback-receiver",
        "profile": arguments.profile,
        "fault_profile": arguments.fault_profile,
        "fault_observations": list(result.fault_observations),
        "security": result.security_observation,
        "fec": result.fec_observation,
        "effective_receiver_latency_microseconds": (
            result.effective_receiver_latency_microseconds
        ),
        "source_time_origin_microseconds": (
            result.source_time_origin_microseconds
        ),
        "source_time_mode": "explicit-srt-msgctrl",
    }
    # Preserve the complete measurement even when a quality gate below fails.
    # This avoids another remote run merely to reveal the causal timeline.
    write_scorecard(scorecard, arguments.output)
    if scorecard["payload_integrity_failures"] != 0:
        raise RuntimeError("downstream UDP payload integrity check failed")
    if scorecard["early_srt_release_count"] != 0:
        raise RuntimeError("SRT released one or more messages before TSBPD")
    enforce_limits(
        scorecard,
        maximum_egress_p99_9_microseconds=(
            arguments.maximum_egress_p99_9_us
        ),
        maximum_tsbpd_phase_range_microseconds=(
            arguments.maximum_tsbpd_phase_range_us
        ),
        maximum_burst_depth=arguments.maximum_burst_depth,
        maximum_pcr_span_rate_error_ppm=(
            arguments.maximum_pcr_span_rate_error_ppm
        ),
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1) from error
