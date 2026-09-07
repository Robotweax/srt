"""Payload-independent Live timing and MPEG-TS/PCR test primitives."""

from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Mapping, Sequence


MPEG_TS_PACKET_BYTES = 188
MPEG_TS_SYNC_BYTE = 0x47
PCR_CLOCK_HZ = 27_000_000
PCR_BASE_MODULUS = 1 << 33
PCR_EXTENSION_MODULUS = 300
PCR_TICK_MODULUS = PCR_BASE_MODULUS * PCR_EXTENSION_MODULUS


@dataclass(frozen=True)
class PcrObservation:
    packet_index: int
    ticks: int
    discontinuity: bool = False


@dataclass(frozen=True)
class TransportPacketInfo:
    pid: int
    continuity_counter: int
    payload_offset: int
    pcr_ticks: int | None
    discontinuity: bool


@dataclass(frozen=True)
class GeneratedMessage:
    index: int
    payload: bytes
    source_submission_microseconds: int
    pcr_observations: tuple[PcrObservation, ...] = ()

    @property
    def payload_sha256(self) -> str:
        return hashlib.sha256(self.payload).hexdigest()


@dataclass(frozen=True)
class MpegTsProfile:
    packets_per_message: int
    message_count: int
    bitrate_bits_per_second: int
    pid: int = 0x100
    pcr_pid: int = 0x100
    pcr_interval_packets: int = 7
    start_pcr_ticks: int = 0
    start_source_microseconds: int = 0
    discontinuity_packets: frozenset[int] = frozenset()

    def __post_init__(self) -> None:
        if self.packets_per_message <= 0:
            raise ValueError("packets per message must be positive")
        if self.message_count <= 0:
            raise ValueError("message count must be positive")
        if self.bitrate_bits_per_second <= 0:
            raise ValueError("bitrate must be positive")
        if not 0 <= self.pid <= 0x1FFF:
            raise ValueError("PID must fit 13 bits")
        if not 0 <= self.pcr_pid <= 0x1FFF:
            raise ValueError("PCR PID must fit 13 bits")
        if self.pcr_interval_packets <= 0:
            raise ValueError("PCR interval must be positive")
        if not 0 <= self.start_pcr_ticks < PCR_TICK_MODULUS:
            raise ValueError("initial PCR must be inside the PCR clock range")
        if self.start_source_microseconds < 0:
            raise ValueError("initial source time must not be negative")
        packet_count = self.packets_per_message * self.message_count
        if any(
            packet < 0 or packet >= packet_count
            for packet in self.discontinuity_packets
        ):
            raise ValueError("PCR discontinuity packet is outside the stream")


@dataclass(frozen=True)
class TimingSample:
    message_index: int
    source_submission_microseconds: int
    tsbpd_deadline_microseconds: int
    srt_release_microseconds: int
    udp_egress_microseconds: int
    payload_bytes: int
    source_payload_sha256: str | None = None
    egress_payload_sha256: str | None = None
    pcr_ticks: int | None = None
    pcr_discontinuity: bool = False

    def __post_init__(self) -> None:
        values = (
            self.message_index,
            self.source_submission_microseconds,
            self.tsbpd_deadline_microseconds,
            self.srt_release_microseconds,
            self.udp_egress_microseconds,
        )
        if any(value < 0 for value in values):
            raise ValueError("timing values must not be negative")
        if self.payload_bytes <= 0:
            raise ValueError("payload size must be positive")
        if self.tsbpd_deadline_microseconds < (
            self.source_submission_microseconds
        ):
            raise ValueError("TSBPD deadline precedes source submission")
        if self.udp_egress_microseconds < self.srt_release_microseconds:
            raise ValueError("UDP egress precedes SRT release")
        if self.pcr_ticks is not None and not (
            0 <= self.pcr_ticks < PCR_TICK_MODULUS
        ):
            raise ValueError("PCR value is outside the PCR clock range")
        if self.pcr_discontinuity and self.pcr_ticks is None:
            raise ValueError("PCR discontinuity requires a PCR observation")
        for digest in (
            self.source_payload_sha256,
            self.egress_payload_sha256,
        ):
            if digest is not None and (
                len(digest) != 64
                or any(character not in "0123456789abcdef" for character in digest)
            ):
                raise ValueError("payload digest must be lowercase SHA-256")

    @classmethod
    def from_mapping(cls, value: Mapping[str, object]) -> "TimingSample":
        def required_integer(name: str) -> int:
            result = value.get(name)
            if type(result) is not int:
                raise ValueError(f"{name} must be an integer")
            return result

        def optional_integer(name: str) -> int | None:
            result = value.get(name)
            if result is None:
                return None
            if type(result) is not int:
                raise ValueError(f"{name} must be an integer or null")
            return result

        def optional_string(name: str) -> str | None:
            result = value.get(name)
            if result is None:
                return None
            if not isinstance(result, str):
                raise ValueError(f"{name} must be a string or null")
            return result

        discontinuity = value.get("pcr_discontinuity", False)
        if type(discontinuity) is not bool:
            raise ValueError("pcr_discontinuity must be boolean")
        return cls(
            message_index=required_integer("message_index"),
            source_submission_microseconds=required_integer(
                "source_submission_microseconds"
            ),
            tsbpd_deadline_microseconds=required_integer(
                "tsbpd_deadline_microseconds"
            ),
            srt_release_microseconds=required_integer(
                "srt_release_microseconds"
            ),
            udp_egress_microseconds=required_integer(
                "udp_egress_microseconds"
            ),
            payload_bytes=required_integer("payload_bytes"),
            source_payload_sha256=optional_string(
                "source_payload_sha256"
            ),
            egress_payload_sha256=optional_string(
                "egress_payload_sha256"
            ),
            pcr_ticks=optional_integer("pcr_ticks"),
            pcr_discontinuity=discontinuity,
        )


def encode_pcr(ticks: int) -> bytes:
    if not 0 <= ticks < PCR_TICK_MODULUS:
        raise ValueError("PCR value is outside the PCR clock range")
    base, extension = divmod(ticks, PCR_EXTENSION_MODULUS)
    encoded = (base << 15) | (0x3F << 9) | extension
    return encoded.to_bytes(6, byteorder="big")


def decode_pcr(encoded: bytes) -> int:
    if len(encoded) != 6:
        raise ValueError("PCR encoding must be exactly six bytes")
    value = int.from_bytes(encoded, byteorder="big")
    base = value >> 15
    reserved = (value >> 9) & 0x3F
    extension = value & 0x1FF
    if reserved != 0x3F or extension >= PCR_EXTENSION_MODULUS:
        raise ValueError("invalid MPEG-TS PCR encoding")
    return base * PCR_EXTENSION_MODULUS + extension


def parse_transport_packet(packet: bytes) -> TransportPacketInfo:
    if len(packet) != MPEG_TS_PACKET_BYTES:
        raise ValueError("MPEG-TS packet must be exactly 188 bytes")
    if packet[0] != MPEG_TS_SYNC_BYTE:
        raise ValueError("MPEG-TS sync byte is missing")
    pid = ((packet[1] & 0x1F) << 8) | packet[2]
    adaptation_control = (packet[3] >> 4) & 0x03
    if adaptation_control == 0:
        raise ValueError("reserved adaptation-field control value")
    continuity_counter = packet[3] & 0x0F
    payload_offset = 4 if adaptation_control == 1 else MPEG_TS_PACKET_BYTES
    pcr_ticks: int | None = None
    discontinuity = False
    if adaptation_control in (2, 3):
        adaptation_length = packet[4]
        adaptation_end = 5 + adaptation_length
        if adaptation_end > MPEG_TS_PACKET_BYTES:
            raise ValueError("adaptation field exceeds MPEG-TS packet")
        if adaptation_length != 0:
            flags = packet[5]
            discontinuity = (flags & 0x80) != 0
            if (flags & 0x10) != 0:
                if adaptation_length < 7:
                    raise ValueError("PCR flag has no complete PCR value")
                pcr_ticks = decode_pcr(packet[6:12])
        if adaptation_control == 3:
            payload_offset = adaptation_end
    return TransportPacketInfo(
        pid=pid,
        continuity_counter=continuity_counter,
        payload_offset=payload_offset,
        pcr_ticks=pcr_ticks,
        discontinuity=discontinuity,
    )


def _nominal_offset(
    byte_count: int,
    bitrate_bits_per_second: int,
    clock_hz: int,
) -> int:
    return (
        byte_count * 8 * clock_hz
        // bitrate_bits_per_second
    )


def _transport_packet(
    profile: MpegTsProfile,
    packet_index: int,
    continuity_counter: int,
) -> tuple[bytes, PcrObservation | None]:
    has_pcr = (
        packet_index % profile.pcr_interval_packets == 0
        or packet_index in profile.discontinuity_packets
    )
    packet_pid = profile.pcr_pid if has_pcr else profile.pid
    discontinuity = packet_index in profile.discontinuity_packets
    adaptation_control = 3 if has_pcr or discontinuity else 1
    packet = bytearray(MPEG_TS_PACKET_BYTES)
    packet[0] = MPEG_TS_SYNC_BYTE
    packet[1] = (packet_pid >> 8) & 0x1F
    packet[2] = packet_pid & 0xFF
    packet[3] = adaptation_control << 4 | continuity_counter
    payload_offset = 4
    observation: PcrObservation | None = None
    if adaptation_control == 3:
        packet[4] = 7 if has_pcr else 1
        packet[5] = (0x80 if discontinuity else 0) | (
            0x10 if has_pcr else 0
        )
        payload_offset = 6
        if has_pcr:
            ticks = (
                profile.start_pcr_ticks
                + _nominal_offset(
                    packet_index * MPEG_TS_PACKET_BYTES,
                    profile.bitrate_bits_per_second,
                    PCR_CLOCK_HZ,
                )
            ) % PCR_TICK_MODULUS
            packet[6:12] = encode_pcr(ticks)
            payload_offset = 12
            observation = PcrObservation(
                packet_index=packet_index,
                ticks=ticks,
                discontinuity=discontinuity,
            )
    for offset in range(payload_offset, MPEG_TS_PACKET_BYTES):
        packet[offset] = (packet_index * 17 + offset) & 0xFF
    return bytes(packet), observation


def generate_mpeg_ts_messages(
    profile: MpegTsProfile,
) -> list[GeneratedMessage]:
    messages: list[GeneratedMessage] = []
    continuity_counters: dict[int, int] = {}
    packet_index = 0
    for message_index in range(profile.message_count):
        payload = bytearray()
        observations: list[PcrObservation] = []
        for _ in range(profile.packets_per_message):
            has_pcr = (
                packet_index % profile.pcr_interval_packets == 0
                or packet_index in profile.discontinuity_packets
            )
            packet_pid = profile.pcr_pid if has_pcr else profile.pid
            continuity_counter = continuity_counters.get(packet_pid, 0)
            packet, observation = _transport_packet(
                profile, packet_index, continuity_counter
            )
            payload.extend(packet)
            if observation is not None:
                observations.append(observation)
            continuity_counters[packet_pid] = (
                continuity_counter + 1
            ) & 0x0F
            packet_index += 1
        first_packet = message_index * profile.packets_per_message
        source_time = (
            profile.start_source_microseconds
            + _nominal_offset(
                first_packet * MPEG_TS_PACKET_BYTES,
                profile.bitrate_bits_per_second,
                1_000_000,
            )
        )
        messages.append(
            GeneratedMessage(
                index=message_index,
                payload=bytes(payload),
                source_submission_microseconds=source_time,
                pcr_observations=tuple(observations),
            )
        )
    return messages


def generate_binary_messages(
    payload_bytes: int,
    message_count: int,
    bitrate_bits_per_second: int,
    *,
    seed: int = 1,
    start_source_microseconds: int = 0,
) -> list[GeneratedMessage]:
    if payload_bytes <= 0 or message_count <= 0:
        raise ValueError("payload size and message count must be positive")
    if bitrate_bits_per_second <= 0:
        raise ValueError("bitrate must be positive")
    if seed < 0 or start_source_microseconds < 0:
        raise ValueError("seed and initial source time must not be negative")
    messages: list[GeneratedMessage] = []
    seed_bytes = seed.to_bytes(8, byteorder="big")
    for message_index in range(message_count):
        payload = bytearray()
        block_index = 0
        while len(payload) < payload_bytes:
            payload.extend(
                hashlib.sha256(
                    seed_bytes
                    + message_index.to_bytes(8, byteorder="big")
                    + block_index.to_bytes(8, byteorder="big")
                ).digest()
            )
            block_index += 1
        source_time = start_source_microseconds + _nominal_offset(
            message_index * payload_bytes,
            bitrate_bits_per_second,
            1_000_000,
        )
        messages.append(
            GeneratedMessage(
                index=message_index,
                payload=bytes(payload[:payload_bytes]),
                source_submission_microseconds=source_time,
            )
        )
    return messages


def synthetic_timing_samples(
    messages: Sequence[GeneratedMessage],
    latency_microseconds: int,
    *,
    release_offset_microseconds: int = 0,
    egress_delay_microseconds: int = 0,
) -> list[TimingSample]:
    if latency_microseconds < 0 or egress_delay_microseconds < 0:
        raise ValueError("latency and egress delay must not be negative")
    samples: list[TimingSample] = []
    for message in messages:
        deadline = message.source_submission_microseconds + latency_microseconds
        release = deadline + release_offset_microseconds
        if release < 0:
            raise ValueError("release offset produces a negative timestamp")
        first_pcr = (
            message.pcr_observations[0]
            if message.pcr_observations
            else None
        )
        digest = message.payload_sha256
        samples.append(
            TimingSample(
                message_index=message.index,
                source_submission_microseconds=(
                    message.source_submission_microseconds
                ),
                tsbpd_deadline_microseconds=deadline,
                srt_release_microseconds=release,
                udp_egress_microseconds=(
                    release + egress_delay_microseconds
                ),
                payload_bytes=len(message.payload),
                source_payload_sha256=digest,
                egress_payload_sha256=digest,
                pcr_ticks=first_pcr.ticks if first_pcr else None,
                pcr_discontinuity=(
                    first_pcr.discontinuity if first_pcr else False
                ),
            )
        )
    return samples


def load_timing_samples(path: Path) -> list[TimingSample]:
    samples: list[TimingSample] = []
    with path.open("r", encoding="utf-8") as source:
        for line_number, line in enumerate(source, start=1):
            if not line.strip():
                continue
            try:
                value = json.loads(line)
                if not isinstance(value, dict):
                    raise ValueError("event must be a JSON object")
                samples.append(TimingSample.from_mapping(value))
            except (json.JSONDecodeError, ValueError) as error:
                raise ValueError(
                    f"invalid timing event at line {line_number}: {error}"
                ) from error
    if not samples:
        raise ValueError("timing event file is empty")
    return samples


def _nearest_rank(values: Sequence[float], percentile: float) -> float:
    if not values:
        raise ValueError("percentile input is empty")
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[rank - 1]


def _metric_summary(values: Sequence[float]) -> dict[str, float]:
    if not values:
        return {}
    return {
        "minimum": min(values),
        "maximum": max(values),
        "mean": sum(values) / len(values),
        "p50": _nearest_rank(values, 0.50),
        "p99": _nearest_rank(values, 0.99),
        "p99_9": _nearest_rank(values, 0.999),
    }


def _maximum_burst_depth(
    egress_times: Sequence[int],
    window_microseconds: int,
) -> int:
    ordered = sorted(egress_times)
    first = 0
    maximum = 0
    for last, timestamp in enumerate(ordered):
        while timestamp - ordered[first] >= window_microseconds:
            first += 1
        maximum = max(maximum, last - first + 1)
    return maximum


def _require_monotonic(
    name: str, values: Sequence[int], message_indices: Sequence[int]
) -> None:
    for previous_index, current_index in zip(
        range(len(values) - 1), range(1, len(values)), strict=True
    ):
        previous = values[previous_index]
        current = values[current_index]
        if current < previous:
            raise ValueError(
                f"{name} must be monotonic; message "
                f"{message_indices[current_index]} regressed by "
                f"{previous - current} us ({previous} -> {current})"
            )


def _timeline_regressions(values: Sequence[int]) -> list[float]:
    return [
        float(previous - current)
        for previous, current in zip(values, values[1:], strict=False)
        if current < previous
    ]


def _pcr_rate_errors(samples: Sequence[TimingSample]) -> list[float]:
    errors: list[float] = []
    previous: TimingSample | None = None
    for sample in samples:
        if sample.pcr_ticks is None:
            continue
        if previous is not None and not sample.pcr_discontinuity:
            tick_delta = (
                sample.pcr_ticks - previous.pcr_ticks
            ) % PCR_TICK_MODULUS
            wall_delta = (
                sample.udp_egress_microseconds
                - previous.udp_egress_microseconds
            )
            if tick_delta != 0 and wall_delta > 0:
                expected_microseconds = (
                    tick_delta * 1_000_000 / PCR_CLOCK_HZ
                )
                errors.append(
                    (expected_microseconds / wall_delta - 1.0)
                    * 1_000_000
                )
        previous = sample
    return errors


def _pcr_span_rate_errors(samples: Sequence[TimingSample]) -> list[float]:
    """Fit each continuous PCR span against its UDP egress timeline."""
    errors: list[float] = []
    segment: list[TimingSample] = []

    def append_segment() -> None:
        if len(segment) < 2:
            return
        expected_times = [0.0]
        accumulated_ticks = 0
        for previous, current in zip(segment, segment[1:], strict=False):
            accumulated_ticks += (
                current.pcr_ticks - previous.pcr_ticks
            ) % PCR_TICK_MODULUS
            expected_times.append(
                accumulated_ticks * 1_000_000 / PCR_CLOCK_HZ
            )
        egress_origin = segment[0].udp_egress_microseconds
        egress_times = [
            float(sample.udp_egress_microseconds - egress_origin)
            for sample in segment
        ]
        expected_mean = sum(expected_times) / len(expected_times)
        egress_mean = sum(egress_times) / len(egress_times)
        expected_variance = sum(
            (value - expected_mean) ** 2 for value in expected_times
        )
        if expected_variance == 0:
            return
        wall_per_expected = sum(
            (expected - expected_mean) * (egress - egress_mean)
            for expected, egress in zip(
                expected_times, egress_times, strict=True
            )
        ) / expected_variance
        if wall_per_expected <= 0:
            return
        errors.append(
            (1.0 / wall_per_expected - 1.0) * 1_000_000
        )

    for sample in samples:
        if sample.pcr_ticks is None:
            continue
        if sample.pcr_discontinuity:
            append_segment()
            segment = []
        segment.append(sample)
    append_segment()
    return errors


def score_timing(
    samples: Iterable[TimingSample],
    *,
    burst_window_microseconds: int = 1_000,
) -> dict[str, object]:
    selected = list(samples)
    if not selected:
        raise ValueError("at least one timing sample is required")
    if burst_window_microseconds <= 0:
        raise ValueError("burst window must be positive")
    indices = [sample.message_index for sample in selected]
    if indices != sorted(indices) or len(indices) != len(set(indices)):
        raise ValueError("message indices must be unique and ordered")
    source_times = [
        sample.source_submission_microseconds for sample in selected
    ]
    deadline_times = [
        sample.tsbpd_deadline_microseconds for sample in selected
    ]
    release_times = [
        sample.srt_release_microseconds for sample in selected
    ]
    egress_times = [sample.udp_egress_microseconds for sample in selected]
    _require_monotonic("explicit source times", source_times, indices)
    _require_monotonic("UDP egress times", egress_times, indices)

    release_errors = [
        float(
            sample.srt_release_microseconds
            - sample.tsbpd_deadline_microseconds
        )
        for sample in selected
    ]
    egress_errors = [
        float(
            sample.udp_egress_microseconds
            - sample.tsbpd_deadline_microseconds
        )
        for sample in selected
    ]
    release_to_egress = [
        float(
            sample.udp_egress_microseconds
            - sample.srt_release_microseconds
        )
        for sample in selected
    ]
    mapped_latencies = [
        float(
            sample.tsbpd_deadline_microseconds
            - sample.source_submission_microseconds
        )
        for sample in selected
    ]
    interval_errors = [
        float(
            (current.udp_egress_microseconds
             - previous.udp_egress_microseconds)
            - (current.source_submission_microseconds
               - previous.source_submission_microseconds)
        )
        for previous, current in zip(selected, selected[1:], strict=False)
    ]
    pcr_rate_errors = _pcr_rate_errors(selected)
    pcr_span_rate_errors = _pcr_span_rate_errors(selected)
    deadline_regressions = _timeline_regressions(deadline_times)
    release_regressions = _timeline_regressions(release_times)
    comparable_hashes = [
        sample
        for sample in selected
        if sample.source_payload_sha256 is not None
        and sample.egress_payload_sha256 is not None
    ]
    payload_sizes = sorted({sample.payload_bytes for sample in selected})

    return {
        "sample_count": len(selected),
        "payload_sizes": payload_sizes,
        "payload_integrity_compared": len(comparable_hashes),
        "payload_integrity_uncompared": (
            len(selected) - len(comparable_hashes)
        ),
        "payload_integrity_failures": sum(
            sample.source_payload_sha256
            != sample.egress_payload_sha256
            for sample in comparable_hashes
        ),
        "early_srt_release_count": sum(
            error < 0 for error in release_errors
        ),
        "early_udp_egress_count": sum(
            error < 0 for error in egress_errors
        ),
        "release_deadline_error_microseconds": _metric_summary(
            release_errors
        ),
        "egress_deadline_error_microseconds": _metric_summary(
            egress_errors
        ),
        "release_to_egress_microseconds": _metric_summary(
            release_to_egress
        ),
        "mapped_tsbpd_latency_microseconds": _metric_summary(
            mapped_latencies
        ),
        "tsbpd_deadline_regression_count": len(deadline_regressions),
        "tsbpd_deadline_backward_step_microseconds": _metric_summary(
            deadline_regressions
        ),
        "srt_release_regression_count": len(release_regressions),
        "srt_release_backward_step_microseconds": _metric_summary(
            release_regressions
        ),
        "egress_interval_error_microseconds": _metric_summary(
            interval_errors
        ),
        "absolute_egress_interval_error_microseconds": _metric_summary(
            [abs(error) for error in interval_errors]
        ),
        "measurement_span_microseconds": {
            "source": source_times[-1] - source_times[0],
            "udp_egress": egress_times[-1] - egress_times[0],
            "error": (
                (egress_times[-1] - egress_times[0])
                - (source_times[-1] - source_times[0])
            ),
        },
        "burst_window_microseconds": burst_window_microseconds,
        "maximum_burst_depth": _maximum_burst_depth(
            egress_times, burst_window_microseconds
        ),
        "pcr_observation_count": sum(
            sample.pcr_ticks is not None for sample in selected
        ),
        "pcr_rate_interval_count": len(pcr_rate_errors),
        "pcr_rate_error_ppm": _metric_summary(pcr_rate_errors),
        "pcr_absolute_rate_error_ppm": _metric_summary(
            [abs(error) for error in pcr_rate_errors]
        ),
        "pcr_span_count": len(pcr_span_rate_errors),
        "pcr_span_rate_error_ppm": _metric_summary(
            pcr_span_rate_errors
        ),
        "pcr_absolute_span_rate_error_ppm": _metric_summary(
            [abs(error) for error in pcr_span_rate_errors]
        ),
    }
