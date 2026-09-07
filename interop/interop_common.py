"""Shared helpers for black-box SRT interoperability runners."""

from __future__ import annotations

import hashlib
import ipaddress
import json
import socket
import subprocess
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path


# This keeps a 4 MiB loss-free loopback transfer below the default live
# TLPKTDROP window while preserving packet pacing and KM preannouncement.
DEFAULT_LOOPBACK_INPUT_BANDWIDTH = 8_000_000


def resolve_program_path(path: Path) -> Path:
    resolved = path.expanduser().resolve(strict=True)
    if not resolved.is_file():
        raise FileNotFoundError(f"program is not a regular file: {resolved}")
    return resolved


def free_udp_port(host: str = "127.0.0.1") -> int:
    address = ipaddress.ip_address(host)
    family = socket.AF_INET6 if address.version == 6 else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as candidate:
        candidate.bind((host, 0))
        return int(candidate.getsockname()[1])


@contextmanager
def reserved_udp_ports(
    count: int = 2, host: str = "127.0.0.1"
) -> Iterator[tuple[int, ...]]:
    """Reserve distinct UDP ports until dependent sockets are bound.

    Rendezvous relays need target ports before they bind their own ephemeral
    sockets.  Keeping the targets reserved during relay construction prevents
    the relay from accidentally acquiring a target port and forwarding a
    peer's datagrams back to itself.
    """
    if count <= 0:
        raise ValueError("reserved UDP port count must be positive")
    address = ipaddress.ip_address(host)
    family = socket.AF_INET6 if address.version == 6 else socket.AF_INET
    reservations: list[socket.socket] = []
    try:
        for _ in range(count):
            reservation = socket.socket(family, socket.SOCK_DGRAM)
            try:
                reservation.bind((str(address), 0))
            except OSError:
                reservation.close()
                raise
            reservations.append(reservation)
        yield tuple(
            int(reservation.getsockname()[1])
            for reservation in reservations
        )
    finally:
        for reservation in reservations:
            reservation.close()


def write_deterministic_payload(path: Path, size: int, seed: int) -> str:
    digest = hashlib.sha256()
    seed_bytes = seed.to_bytes(8, byteorder="big", signed=False)
    remaining = size
    counter = 0
    with path.open("wb") as output:
        while remaining != 0:
            block = hashlib.sha256(
                seed_bytes + counter.to_bytes(8, byteorder="big")
            ).digest()
            selected = block[:remaining]
            output.write(selected)
            digest.update(selected)
            remaining -= len(selected)
            counter += 1
    return digest.hexdigest()


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(64 * 1_024), b""):
            digest.update(block)
    return digest.hexdigest()


def describe_file_mismatch(
    expected_path: Path,
    actual_path: Path,
    packet_size: int,
    *,
    maximum_reported_packets: int = 16,
    preview_bytes: int = 16,
) -> str:
    if (
        packet_size <= 0
        or maximum_reported_packets <= 0
        or preview_bytes <= 0
    ):
        raise ValueError("mismatch diagnostic limits must be positive")

    first_offset: int | None = None
    mismatching_packets: list[int] = []
    compared_offset = 0
    with expected_path.open("rb") as expected, actual_path.open(
        "rb"
    ) as actual:
        while True:
            expected_block = expected.read(64 * 1_024)
            actual_block = actual.read(64 * 1_024)
            if not expected_block and not actual_block:
                break
            compared = max(len(expected_block), len(actual_block))
            for index in range(compared):
                expected_byte = (
                    expected_block[index]
                    if index < len(expected_block)
                    else None
                )
                actual_byte = (
                    actual_block[index]
                    if index < len(actual_block)
                    else None
                )
                if expected_byte == actual_byte:
                    continue
                offset = compared_offset + index
                if first_offset is None:
                    first_offset = offset
                occurrence = offset // packet_size + 1
                if (
                    len(mismatching_packets)
                    < maximum_reported_packets
                    and (
                        not mismatching_packets
                        or mismatching_packets[-1] != occurrence
                    )
                ):
                    mismatching_packets.append(occurrence)
            compared_offset += compared

    if first_offset is None:
        return "files are byte-identical"

    def preview(path: Path) -> str:
        with path.open("rb") as source:
            source.seek(first_offset)
            selected = source.read(preview_bytes)
        return selected.hex() if selected else "<eof>"

    return (
        f"first_mismatch_offset={first_offset} "
        f"packet_occurrence={first_offset // packet_size + 1} "
        f"packet_offset={first_offset % packet_size} "
        f"expected_size={expected_path.stat().st_size} "
        f"actual_size={actual_path.stat().st_size} "
        f"expected_preview={preview(expected_path)} "
        f"actual_preview={preview(actual_path)} "
        f"mismatching_packet_occurrences={mismatching_packets}"
    )


def parse_complete(output: str, expected_role: str) -> dict[str, object]:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (
            event.get("event") == "complete"
            and event.get("role") == expected_role
        ):
            return event
    raise RuntimeError(f"{expected_role} did not report completion")


def nonnegative_statistic(
    event: dict[str, object], name: str
) -> int | None:
    statistics = event.get("stats")
    if not isinstance(statistics, dict):
        return None
    value = statistics.get(name)
    return value if type(value) is int and value >= 0 else None


def expected_live_packets(byte_count: int, chunk_size: int) -> int:
    if byte_count <= 0 or chunk_size <= 0:
        raise ValueError("byte count and chunk size must be positive")
    return (byte_count + chunk_size - 1) // chunk_size


def validate_sender_statistics(
    event: dict[str, object], required_packets: int
) -> int:
    packets_sent = nonnegative_statistic(event, "pktSentTotal")
    packets_dropped = nonnegative_statistic(event, "pktSndDropTotal")
    if packets_sent is None or packets_dropped is None:
        raise RuntimeError("sender packet statistics are unavailable")
    if packets_dropped != 0:
        raise RuntimeError(
            f"sender reported {packets_dropped} dropped packets"
        )
    if packets_sent < required_packets:
        raise RuntimeError(
            f"only {packets_sent} packets were sent; "
            f"at least {required_packets} are required"
        )
    return packets_sent


def terminate(process: subprocess.Popen[str]) -> None:
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
