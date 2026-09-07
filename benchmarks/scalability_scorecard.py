#!/usr/bin/env python3
"""Reproducible public-API scalability scorecard for SRT implementations.

Both topologies use identical helper source linked against each library.  The
process-isolated baseline uses one endpoint process per connection.  The
many-socket topology uses one epoll-driven process per side, exposing library
thread and memory scaling without per-connection process overhead.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import IO, Iterable

try:
    import resource
except ImportError:  # pragma: no cover - unavailable on Windows
    resource = None  # type: ignore[assignment]


SCHEMA_VERSION = 2
DEFAULT_HOST = "127.0.0.1"
MANY_SOCKET_BURST_MESSAGES = 64
MANY_SOCKET_ROUND_MESSAGES = 128
MANY_SOCKET_PENDING_PACKETS_PER_SOCKET = 64
PROFILE_IMPLEMENTATIONS = {
    "robotweax-self": ("robotweax", "robotweax"),
    "haivision-self": ("haivision", "haivision"),
    "robotweax-to-haivision": ("robotweax", "haivision"),
    "haivision-to-robotweax": ("haivision", "robotweax"),
}


@dataclass(frozen=True)
class RunOptions:
    host: str
    connections: int
    bytes_per_connection: int
    message_size: int
    timeout_seconds: int
    latency_milliseconds: int
    shutdown_grace_milliseconds: int
    sampling_interval_seconds: float


@dataclass
class PeerProcess:
    process: subprocess.Popen[bytes]
    role: str
    implementation: str
    connection_index: int
    stdout_path: Path
    stderr_path: Path
    stdout_stream: IO[bytes]
    stderr_stream: IO[bytes]
    output_path: Path | None = None

    def close_streams(self) -> None:
        self.stdout_stream.close()
        self.stderr_stream.close()


@dataclass
class ResourcePeaks:
    aggregate_rss_bytes: int = 0
    aggregate_threads: int = 0
    samples: int = 0

    def observe(self, sample: tuple[int, int] | None) -> None:
        if sample is None:
            return
        rss_bytes, threads = sample
        self.aggregate_rss_bytes = max(self.aggregate_rss_bytes, rss_bytes)
        self.aggregate_threads = max(self.aggregate_threads, threads)
        self.samples += 1


class ScorecardFailure(RuntimeError):
    pass


def parse_positive_list(text: str, *, maximum: int) -> list[int]:
    values: list[int] = []
    for item in text.split(","):
        try:
            value = int(item)
        except ValueError as error:
            raise argparse.ArgumentTypeError(
                f"invalid integer list: {text}"
            ) from error
        if value <= 0 or value > maximum:
            raise argparse.ArgumentTypeError(
                f"values must be between 1 and {maximum}: {text}"
            )
        if value not in values:
            values.append(value)
    if not values:
        raise argparse.ArgumentTypeError("at least one value is required")
    return values


def percentile(values: Iterable[float], quantile: float) -> float:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile requires at least one sample")
    if not 0.0 <= quantile <= 1.0:
        raise ValueError("quantile must be between zero and one")
    position = (len(ordered) - 1) * quantile
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    fraction = position - lower
    return float(
        ordered[lower]
        + (ordered[upper] - ordered[lower]) * fraction
    )


def distribution(values: Iterable[float]) -> dict[str, float]:
    selected = list(values)
    if not selected:
        raise ValueError("distribution requires at least one sample")
    return {
        "minimum": min(selected),
        "p50": percentile(selected, 0.50),
        "p99": percentile(selected, 0.99),
        "p99_9": percentile(selected, 0.999),
        "maximum": max(selected),
    }


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_payload(path: Path, size: int) -> str:
    digest = hashlib.sha256()
    remaining = size
    counter = 0
    with path.open("wb") as output:
        while remaining:
            block = hashlib.sha256(
                b"robotweax-scalability-v1"
                + counter.to_bytes(8, byteorder="big")
            ).digest()
            selected = block[:remaining]
            output.write(selected)
            digest.update(selected)
            remaining -= len(selected)
            counter += 1
    return digest.hexdigest()


def free_udp_port(host: str) -> int:
    family = socket.AF_INET6 if ":" in host else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as candidate:
        candidate.bind((host, 0))
        return int(candidate.getsockname()[1])


def parse_json_events(output: str) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict):
            events.append(event)
    return events


def complete_event(output: str, role: str) -> dict[str, object] | None:
    for event in parse_json_events(output):
        if event.get("event") == "complete" and event.get("role") == role:
            return event
    return None


def peer_command(
    program: Path,
    role: str,
    port: int,
    payload_path: Path,
    options: RunOptions,
) -> list[str]:
    if role not in ("caller", "listener"):
        raise ValueError(f"unsupported role: {role}")
    command = [
        str(program),
        role,
        "--host",
        options.host,
        "--port",
        str(port),
        "--bytes",
        str(options.bytes_per_connection),
        "--input" if role == "caller" else "--output",
        str(payload_path),
        "--transport",
        "live",
        "--message-api",
        "true",
        "--timeout-ms",
        str(options.timeout_seconds * 1000),
        "--chunk-size",
        str(options.message_size),
        "--receive-size",
        str(max(options.message_size, 1500)),
        "--flow-window",
        "1024",
        "--send-buffer",
        str(4 * 1024 * 1024),
        "--mss",
        "1500",
        "--payload-size",
        str(options.message_size),
        "--latency-ms",
        str(options.latency_milliseconds),
        "--max-bw",
        "-1",
        "--shutdown-grace-ms",
        str(options.shutdown_grace_milliseconds),
    ]
    return command


def many_socket_message_count(options: RunOptions) -> int:
    return (
        options.bytes_per_connection + options.message_size - 1
    ) // options.message_size


def many_socket_peer_command(
    program: Path,
    role: str,
    port: int,
    options: RunOptions,
) -> list[str]:
    if role not in ("caller", "listener"):
        raise ValueError(f"unsupported role: {role}")
    return [
        str(program),
        role,
        "--host",
        options.host,
        "--port",
        str(port),
        "--connections",
        str(options.connections),
        "--messages",
        str(many_socket_message_count(options)),
        "--message-size",
        str(options.message_size),
        "--timeout-ms",
        str(options.timeout_seconds * 1000),
        "--latency-ms",
        str(options.latency_milliseconds),
        "--shutdown-grace-ms",
        str(options.shutdown_grace_milliseconds),
    ]


def child_usage() -> dict[str, float] | None:
    if resource is None:
        return None
    usage = resource.getrusage(resource.RUSAGE_CHILDREN)
    return {
        "user_cpu_seconds": float(usage.ru_utime),
        "system_cpu_seconds": float(usage.ru_stime),
        "voluntary_context_switches": float(usage.ru_nvcsw),
        "involuntary_context_switches": float(usage.ru_nivcsw),
    }


def usage_delta(
    before: dict[str, float] | None,
    after: dict[str, float] | None,
) -> dict[str, float] | None:
    if before is None or after is None:
        return None
    return {
        key: max(0.0, after[key] - before[key])
        for key in before
    }


def parse_linux_status(text: str) -> tuple[int, int]:
    values: dict[str, int] = {}
    for line in text.splitlines():
        name, separator, remainder = line.partition(":")
        if not separator or name not in {"VmRSS", "Threads"}:
            continue
        token = remainder.strip().split()[0]
        values[name] = int(token)
    return values.get("VmRSS", 0) * 1024, values.get("Threads", 0)


def sample_linux_processes(processes: Iterable[PeerProcess]) -> tuple[int, int] | None:
    if not Path("/proc/self/status").is_file():
        return None
    rss_bytes = 0
    threads = 0
    observed = False
    for peer in processes:
        status = Path(f"/proc/{peer.process.pid}/status")
        try:
            current_rss, current_threads = parse_linux_status(
                status.read_text(encoding="utf-8")
            )
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
        rss_bytes += current_rss
        threads += current_threads
        observed = True
    return (rss_bytes, threads) if observed else None


def spawn_peer(
    program: Path,
    implementation: str,
    role: str,
    connection_index: int,
    port: int,
    payload_path: Path,
    options: RunOptions,
    directory: Path,
) -> PeerProcess:
    prefix = f"connection-{connection_index:04d}-{role}"
    stdout_path = directory / f"{prefix}.stdout"
    stderr_path = directory / f"{prefix}.stderr"
    stdout_stream = stdout_path.open("wb")
    stderr_stream = stderr_path.open("wb")
    try:
        process = subprocess.Popen(
            peer_command(program, role, port, payload_path, options),
            stdout=stdout_stream,
            stderr=stderr_stream,
        )
    except Exception:
        stdout_stream.close()
        stderr_stream.close()
        raise
    return PeerProcess(
        process=process,
        role=role,
        implementation=implementation,
        connection_index=connection_index,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        stdout_stream=stdout_stream,
        stderr_stream=stderr_stream,
        output_path=payload_path if role == "listener" else None,
    )


def spawn_command_peer(
    command: list[str],
    implementation: str,
    role: str,
    directory: Path,
) -> PeerProcess:
    prefix = f"many-socket-{role}"
    stdout_path = directory / f"{prefix}.stdout"
    stderr_path = directory / f"{prefix}.stderr"
    stdout_stream = stdout_path.open("wb")
    stderr_stream = stderr_path.open("wb")
    try:
        process = subprocess.Popen(
            command,
            stdout=stdout_stream,
            stderr=stderr_stream,
        )
    except Exception:
        stdout_stream.close()
        stderr_stream.close()
        raise
    return PeerProcess(
        process=process,
        role=role,
        implementation=implementation,
        connection_index=-1,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        stdout_stream=stdout_stream,
        stderr_stream=stderr_stream,
    )


def read_output(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def render_process_failure(peer: PeerProcess) -> str:
    return (
        f"{peer.implementation} {peer.role} connection "
        f"{peer.connection_index} exited with {peer.process.returncode}\n"
        f"--- stdout ---\n{read_output(peer.stdout_path) or '<empty>'}\n"
        f"--- stderr ---\n{read_output(peer.stderr_path) or '<empty>'}\n"
    )


def wait_for_listeners(
    listeners: list[PeerProcess], timeout_seconds: int
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        ready = 0
        for listener in listeners:
            if listener.process.poll() is not None:
                raise ScorecardFailure(render_process_failure(listener))
            if any(
                event.get("event") == "ready"
                for event in parse_json_events(
                    read_output(listener.stdout_path)
                )
            ):
                ready += 1
        if ready == len(listeners):
            return
        time.sleep(0.01)
    raise ScorecardFailure(
        f"only {ready} of {len(listeners)} listeners became ready"
    )


def terminate_all(processes: Iterable[PeerProcess]) -> None:
    selected = list(processes)
    for peer in selected:
        if peer.process.poll() is None:
            peer.process.terminate()
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline and any(
        peer.process.poll() is None for peer in selected
    ):
        time.sleep(0.01)
    for peer in selected:
        if peer.process.poll() is None:
            peer.process.kill()
    for peer in selected:
        try:
            peer.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            pass


def monitor_processes(
    processes: list[PeerProcess],
    timeout_seconds: int,
    sampling_interval_seconds: float,
) -> tuple[float, float, ResourcePeaks]:
    deadline = time.monotonic() + timeout_seconds
    complete_observed_at: float | None = None
    peaks = ResourcePeaks()
    while True:
        now = time.monotonic()
        peaks.observe(sample_linux_processes(processes))
        completion = {
            peer.process.pid: complete_event(
                read_output(peer.stdout_path), peer.role
            )
            for peer in processes
        }
        for peer in processes:
            if (
                peer.process.poll() is not None
                and completion[peer.process.pid] is None
            ):
                terminate_all(processes)
                diagnostics = "\n".join(
                    render_process_failure(candidate)
                    for candidate in processes
                )
                raise ScorecardFailure(
                    "a peer exited before transfer completion\n"
                    f"{diagnostics}"
                )
        all_complete = all(event is not None for event in completion.values())
        if all_complete and complete_observed_at is None:
            complete_observed_at = now
        if all(peer.process.poll() is not None for peer in processes):
            exited_at = time.monotonic()
            return complete_observed_at or exited_at, exited_at, peaks
        if now >= deadline:
            terminate_all(processes)
            diagnostics = "\n".join(
                render_process_failure(peer) for peer in processes
            )
            raise ScorecardFailure(
                f"peer processes exceeded {timeout_seconds} seconds\n"
                f"{diagnostics}"
            )
        time.sleep(sampling_interval_seconds)


def validate_peer(
    peer: PeerProcess, expected_bytes: int
) -> dict[str, object]:
    if peer.process.returncode != 0:
        raise ScorecardFailure(render_process_failure(peer))
    event = complete_event(read_output(peer.stdout_path), peer.role)
    if event is None:
        raise ScorecardFailure(render_process_failure(peer))
    if event.get("bytes") != expected_bytes:
        raise ScorecardFailure(
            f"{peer.implementation} {peer.role} connection "
            f"{peer.connection_index} reported {event.get('bytes')} bytes; "
            f"expected {expected_bytes}"
        )
    elapsed = event.get("elapsed_us")
    if type(elapsed) is not int or elapsed <= 0:
        raise ScorecardFailure("peer completion elapsed_us is unavailable")
    return event


def aggregate_statistic(
    events: Iterable[dict[str, object]], name: str
) -> int | None:
    values: list[int] = []
    for event in events:
        statistics = event.get("stats")
        if not isinstance(statistics, dict):
            return None
        value = statistics.get(name)
        if type(value) is not int or value < 0:
            return None
        values.append(value)
    return sum(values)


def implementation_programs(
    robotweax_peer: Path,
    reference_peer: Path | None,
) -> dict[str, Path]:
    programs = {"robotweax": robotweax_peer}
    if reference_peer is not None:
        programs["haivision"] = reference_peer
    return programs


def run_profile(
    profile_name: str,
    programs: dict[str, Path],
    options: RunOptions,
    directory: Path,
    expected_digest: str,
    *,
    iteration: int,
    warmup: bool,
) -> dict[str, object]:
    sender_name, receiver_name = PROFILE_IMPLEMENTATIONS[profile_name]
    try:
        sender_program = programs[sender_name]
        receiver_program = programs[receiver_name]
    except KeyError as error:
        raise ScorecardFailure(
            f"profile {profile_name} requires --reference-peer"
        ) from error

    input_path = directory / "payload.input"
    ports: list[int] = []
    attempts = 0
    while len(ports) < options.connections:
        attempts += 1
        if attempts > options.connections * 100:
            raise ScorecardFailure("could not allocate distinct UDP ports")
        candidate = free_udp_port(options.host)
        if candidate not in ports:
            ports.append(candidate)
    peers: list[PeerProcess] = []
    listeners: list[PeerProcess] = []
    callers: list[PeerProcess] = []
    usage_before = child_usage()
    run_started = time.monotonic()
    try:
        for index, port in enumerate(ports):
            output_path = directory / f"connection-{index:04d}.output"
            listener = spawn_peer(
                receiver_program,
                receiver_name,
                "listener",
                index,
                port,
                output_path,
                options,
                directory,
            )
            listeners.append(listener)
            peers.append(listener)
        wait_for_listeners(listeners, options.timeout_seconds)
        transfer_started = time.monotonic()
        for index, port in enumerate(ports):
            caller = spawn_peer(
                sender_program,
                sender_name,
                "caller",
                index,
                port,
                input_path,
                options,
                directory,
            )
            callers.append(caller)
            peers.append(caller)
        complete_at, exited_at, peaks = monitor_processes(
            peers,
            options.timeout_seconds,
            options.sampling_interval_seconds,
        )
        for peer in peers:
            peer.process.wait()
    except Exception:
        terminate_all(peers)
        raise
    finally:
        for peer in peers:
            peer.close_streams()

    usage = usage_delta(usage_before, child_usage())
    caller_events = [
        validate_peer(peer, options.bytes_per_connection)
        for peer in callers
    ]
    listener_events = [
        validate_peer(peer, options.bytes_per_connection)
        for peer in listeners
    ]
    for listener in listeners:
        if listener.output_path is None:
            raise ScorecardFailure("listener output path is unavailable")
        observed_digest = file_sha256(listener.output_path)
        if observed_digest != expected_digest:
            raise ScorecardFailure(
                f"connection {listener.connection_index} SHA-256 mismatch: "
                f"expected {expected_digest}, observed {observed_digest}"
            )

    transfer_seconds = max(complete_at - transfer_started, 1e-9)
    total_seconds = max(exited_at - run_started, transfer_seconds)
    useful_bytes = options.bytes_per_connection * options.connections
    useful_gigabits = useful_bytes * 8.0 / 1_000_000_000.0
    expected_messages = (
        options.bytes_per_connection + options.message_size - 1
    ) // options.message_size
    useful_messages = expected_messages * options.connections
    cpu_seconds = None
    if usage is not None:
        cpu_seconds = (
            usage["user_cpu_seconds"] + usage["system_cpu_seconds"]
        )

    return {
        "profile": profile_name,
        "sender_implementation": sender_name,
        "receiver_implementation": receiver_name,
        "connections": options.connections,
        "message_size_bytes": options.message_size,
        "bytes_per_connection": options.bytes_per_connection,
        "iteration": iteration,
        "warmup": warmup,
        "integrity": {
            "sha256": expected_digest,
            "verified_connections": options.connections,
        },
        "timing": {
            "setup_seconds": transfer_started - run_started,
            "transfer_seconds": transfer_seconds,
            "process_exit_tail_seconds": max(0.0, exited_at - complete_at),
            "total_seconds": total_seconds,
            "caller_completion_seconds": distribution(
                float(event["elapsed_us"]) / 1_000_000.0
                for event in caller_events
            ),
            "listener_completion_seconds": distribution(
                float(event["elapsed_us"]) / 1_000_000.0
                for event in listener_events
            ),
        },
        "rates": {
            "useful_bits_per_second": useful_bytes * 8.0 / transfer_seconds,
            "useful_messages_per_second": useful_messages / transfer_seconds,
            "cpu_seconds_per_useful_gigabit": (
                cpu_seconds / useful_gigabits
                if cpu_seconds is not None and useful_gigabits > 0.0
                else None
            ),
        },
        "wire_statistics": {
            "sender_packets_unique": aggregate_statistic(
                caller_events, "pktSentUniqueTotal"
            ),
            "sender_packets_total": aggregate_statistic(
                caller_events, "pktSentTotal"
            ),
            "receiver_packets_unique": aggregate_statistic(
                listener_events, "pktRecvUniqueTotal"
            ),
            "retransmitted_packets": aggregate_statistic(
                caller_events, "pktRetransTotal"
            ),
        },
        "process_resources": {
            "usage_available": usage is not None,
            "linux_sampling_available": peaks.samples > 0,
            "user_cpu_seconds": (
                usage["user_cpu_seconds"] if usage is not None else None
            ),
            "system_cpu_seconds": (
                usage["system_cpu_seconds"] if usage is not None else None
            ),
            "voluntary_context_switches": (
                int(usage["voluntary_context_switches"])
                if usage is not None
                else None
            ),
            "involuntary_context_switches": (
                int(usage["involuntary_context_switches"])
                if usage is not None
                else None
            ),
            "peak_aggregate_rss_bytes": (
                peaks.aggregate_rss_bytes if peaks.samples else None
            ),
            "peak_aggregate_threads": (
                peaks.aggregate_threads if peaks.samples else None
            ),
            "linux_sample_count": peaks.samples,
        },
        "versions": {
            "sender": sorted(
                {event.get("srt_version") for event in caller_events}
            ),
            "receiver": sorted(
                {event.get("srt_version") for event in listener_events}
            ),
        },
    }


def completion_distribution_seconds(
    event: dict[str, object],
) -> dict[str, float]:
    raw = event.get("connection_completion_us")
    if not isinstance(raw, dict):
        raise ScorecardFailure(
            "many-socket peer completion distribution is unavailable"
        )
    result: dict[str, float] = {}
    for name in ("minimum", "p50", "p99", "p99_9", "maximum"):
        value = raw.get(name)
        if type(value) not in (int, float) or float(value) < 0.0:
            raise ScorecardFailure(
                f"many-socket completion field {name} is invalid"
            )
        result[name] = float(value) / 1_000_000.0
    return result


def validate_many_socket_peer(
    peer: PeerProcess,
    expected_bytes: int,
    expected_connections: int,
) -> dict[str, object]:
    event = validate_peer(peer, expected_bytes)
    if event.get("integrity") is not True:
        raise ScorecardFailure(
            f"{peer.implementation} {peer.role} did not verify integrity"
        )
    if event.get("connections") != expected_connections:
        raise ScorecardFailure(
            f"{peer.implementation} {peer.role} reported "
            f"{event.get('connections')} connections; "
            f"expected {expected_connections}"
        )
    completion_distribution_seconds(event)
    return event


def run_many_socket_profile(
    profile_name: str,
    programs: dict[str, Path],
    options: RunOptions,
    directory: Path,
    *,
    iteration: int,
    warmup: bool,
) -> dict[str, object]:
    sender_name, receiver_name = PROFILE_IMPLEMENTATIONS[profile_name]
    try:
        sender_program = programs[sender_name]
        receiver_program = programs[receiver_name]
    except KeyError as error:
        raise ScorecardFailure(
            f"profile {profile_name} requires --reference-peer"
        ) from error

    port = free_udp_port(options.host)
    peers: list[PeerProcess] = []
    usage_before = child_usage()
    run_started = time.monotonic()
    try:
        listener = spawn_command_peer(
            many_socket_peer_command(
                receiver_program, "listener", port, options
            ),
            receiver_name,
            "listener",
            directory,
        )
        peers.append(listener)
        wait_for_listeners([listener], options.timeout_seconds)
        transfer_started = time.monotonic()
        caller = spawn_command_peer(
            many_socket_peer_command(
                sender_program, "caller", port, options
            ),
            sender_name,
            "caller",
            directory,
        )
        peers.append(caller)
        complete_at, exited_at, peaks = monitor_processes(
            peers,
            options.timeout_seconds + 2,
            options.sampling_interval_seconds,
        )
        for peer in peers:
            peer.process.wait()
    except Exception:
        terminate_all(peers)
        raise
    finally:
        for peer in peers:
            peer.close_streams()

    messages_per_connection = many_socket_message_count(options)
    effective_bytes_per_connection = (
        messages_per_connection * options.message_size
    )
    useful_messages = messages_per_connection * options.connections
    useful_bytes = effective_bytes_per_connection * options.connections
    caller_event = validate_many_socket_peer(
        caller, useful_bytes, options.connections
    )
    listener_event = validate_many_socket_peer(
        listener, useful_bytes, options.connections
    )
    usage = usage_delta(usage_before, child_usage())
    cpu_seconds = None
    if usage is not None:
        cpu_seconds = (
            usage["user_cpu_seconds"] + usage["system_cpu_seconds"]
        )
    useful_gigabits = useful_bytes * 8.0 / 1_000_000_000.0
    transfer_seconds = max(complete_at - transfer_started, 1e-9)
    total_seconds = max(exited_at - run_started, transfer_seconds)

    return {
        "profile": profile_name,
        "sender_implementation": sender_name,
        "receiver_implementation": receiver_name,
        "connections": options.connections,
        "message_size_bytes": options.message_size,
        "requested_bytes_per_connection": options.bytes_per_connection,
        "bytes_per_connection": effective_bytes_per_connection,
        "messages_per_connection": messages_per_connection,
        "iteration": iteration,
        "warmup": warmup,
        "integrity": {
            "deterministic_payload_verified": True,
            "verified_connections": options.connections,
        },
        "timing": {
            "setup_seconds": transfer_started - run_started,
            "transfer_seconds": transfer_seconds,
            "process_exit_tail_seconds": max(0.0, exited_at - complete_at),
            "total_seconds": total_seconds,
            "caller_establishment_seconds": (
                float(caller_event["establishment_elapsed_us"])
                / 1_000_000.0
            ),
            "listener_establishment_seconds": (
                float(listener_event["establishment_elapsed_us"])
                / 1_000_000.0
            ),
            "caller_completion_seconds": completion_distribution_seconds(
                caller_event
            ),
            "listener_completion_seconds": completion_distribution_seconds(
                listener_event
            ),
        },
        "rates": {
            "useful_bits_per_second": useful_bytes * 8.0 / transfer_seconds,
            "useful_messages_per_second": useful_messages / transfer_seconds,
            "cpu_seconds_per_useful_gigabit": (
                cpu_seconds / useful_gigabits
                if cpu_seconds is not None and useful_gigabits > 0.0
                else None
            ),
        },
        "wire_statistics": {
            "sender_packets_unique": aggregate_statistic(
                [caller_event], "pktSentUniqueTotal"
            ),
            "sender_packets_total": aggregate_statistic(
                [caller_event], "pktSentTotal"
            ),
            "receiver_packets_unique": aggregate_statistic(
                [listener_event], "pktRecvUniqueTotal"
            ),
            "retransmitted_packets": aggregate_statistic(
                [caller_event], "pktRetransTotal"
            ),
        },
        "process_resources": {
            "usage_available": usage is not None,
            "linux_sampling_available": peaks.samples > 0,
            "user_cpu_seconds": (
                usage["user_cpu_seconds"] if usage is not None else None
            ),
            "system_cpu_seconds": (
                usage["system_cpu_seconds"] if usage is not None else None
            ),
            "voluntary_context_switches": (
                int(usage["voluntary_context_switches"])
                if usage is not None
                else None
            ),
            "involuntary_context_switches": (
                int(usage["involuntary_context_switches"])
                if usage is not None
                else None
            ),
            "peak_aggregate_rss_bytes": (
                peaks.aggregate_rss_bytes if peaks.samples else None
            ),
            "peak_aggregate_threads": (
                peaks.aggregate_threads if peaks.samples else None
            ),
            "linux_sample_count": peaks.samples,
        },
        "versions": {
            "sender": [caller_event.get("srt_version")],
            "receiver": [listener_event.get("srt_version")],
        },
    }


def median(values: Iterable[float]) -> float:
    return percentile(values, 0.5)


def summarize(runs: list[dict[str, object]]) -> list[dict[str, object]]:
    groups: dict[tuple[str, int, int], list[dict[str, object]]] = {}
    for run in runs:
        if run.get("warmup") is True:
            continue
        key = (
            str(run["profile"]),
            int(run["connections"]),
            int(run["message_size_bytes"]),
        )
        groups.setdefault(key, []).append(run)

    summaries: list[dict[str, object]] = []
    for (profile, connections, message_size), selected in sorted(groups.items()):
        rates = [run["rates"] for run in selected]
        timings = [run["timing"] for run in selected]
        resources = [run["process_resources"] for run in selected]

        def numeric(source: list[object], name: str) -> list[float]:
            return [
                float(item[name])
                for item in source
                if isinstance(item, dict)
                and type(item.get(name)) in (int, float)
            ]

        throughput = numeric(rates, "useful_bits_per_second")
        message_rate = numeric(rates, "useful_messages_per_second")
        cpu_per_gigabit = numeric(
            rates, "cpu_seconds_per_useful_gigabit"
        )
        rss = numeric(resources, "peak_aggregate_rss_bytes")
        threads = numeric(resources, "peak_aggregate_threads")
        summaries.append(
            {
                "profile": profile,
                "connections": connections,
                "message_size_bytes": message_size,
                "iterations": len(selected),
                "median_useful_bits_per_second": median(throughput),
                "median_useful_messages_per_second": median(message_rate),
                "median_cpu_seconds_per_useful_gigabit": (
                    median(cpu_per_gigabit) if cpu_per_gigabit else None
                ),
                "median_peak_aggregate_rss_bytes": (
                    median(rss) if rss else None
                ),
                "median_peak_aggregate_threads": (
                    median(threads) if threads else None
                ),
                "median_transfer_seconds": median(
                    numeric(timings, "transfer_seconds")
                ),
            }
        )
    return summaries


def cpu_model() -> str:
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            if line.lower().startswith("model name"):
                return line.partition(":")[2].strip()
    return platform.processor() or "unknown"


def program_identity(path: Path) -> dict[str, object]:
    resolved = path.expanduser().resolve(strict=True)
    if not resolved.is_file():
        raise FileNotFoundError(f"not a regular file: {resolved}")
    return {
        "path": str(resolved),
        "sha256": file_sha256(resolved),
        "size_bytes": resolved.stat().st_size,
    }


def write_report(path: Path, report: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--topology",
        choices=("process-isolated", "many-socket"),
        default="process-isolated",
    )
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--profile",
        action="append",
        choices=sorted(PROFILE_IMPLEMENTATIONS),
        dest="profiles",
    )
    parser.add_argument("--connections", default="1,8")
    parser.add_argument("--message-sizes", default="188,1316")
    parser.add_argument(
        "--bytes-per-connection", type=int, default=4 * 1024 * 1024
    )
    parser.add_argument("--iterations", type=int, default=2)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--timeout-seconds", type=int, default=60)
    parser.add_argument("--latency-ms", type=int)
    parser.add_argument("--shutdown-grace-ms", type=int)
    parser.add_argument("--sampling-interval-ms", type=float, default=10.0)
    parser.add_argument("--robotweax-revision", default="unspecified")
    parser.add_argument("--reference-revision", default="unspecified")
    arguments = parser.parse_args(argv)
    arguments.connections = parse_positive_list(
        arguments.connections, maximum=1024
    )
    arguments.message_sizes = parse_positive_list(
        arguments.message_sizes, maximum=1456
    )
    if arguments.latency_ms is None:
        arguments.latency_ms = (
            120 if arguments.topology == "many-socket" else 20
        )
    if arguments.shutdown_grace_ms is None:
        arguments.shutdown_grace_ms = (
            500 if arguments.topology == "many-socket" else 50
        )
    if arguments.topology == "many-socket" and any(
        size < 16 for size in arguments.message_sizes
    ):
        parser.error("many-socket message sizes must be at least 16 bytes")
    if arguments.bytes_per_connection <= 0:
        parser.error("--bytes-per-connection must be positive")
    if arguments.iterations <= 0 or arguments.warmup < 0:
        parser.error("iterations must be positive and warmup nonnegative")
    if arguments.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")
    if arguments.latency_ms < 0 or arguments.shutdown_grace_ms < 0:
        parser.error("latency and shutdown grace must be nonnegative")
    if arguments.sampling_interval_ms <= 0:
        parser.error("--sampling-interval-ms must be positive")
    if arguments.profiles is None:
        arguments.profiles = list(PROFILE_IMPLEMENTATIONS)
    if (
        arguments.reference_peer is None
        and any(
            "haivision" in PROFILE_IMPLEMENTATIONS[profile]
            for profile in arguments.profiles
        )
    ):
        parser.error("selected profiles require --reference-peer")
    return arguments


def main(argv: list[str] | None = None) -> int:
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    robotweax_peer = arguments.robotweax_peer.expanduser().resolve(strict=True)
    reference_peer = (
        arguments.reference_peer.expanduser().resolve(strict=True)
        if arguments.reference_peer is not None
        else None
    )
    programs = implementation_programs(robotweax_peer, reference_peer)
    report: dict[str, object] = {
        "schema_version": SCHEMA_VERSION,
        "status": "running",
        "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
        "topology": arguments.topology,
        "methodology": {
            "transport": "live",
            "message_api": True,
            "mss_bytes": 1500,
            "flow_window_packets": 1024,
            "send_buffer_bytes": 4 * 1024 * 1024,
            "maximum_bandwidth": -1,
            "too_late_packet_drop": False,
            "latency_milliseconds": arguments.latency_ms,
            "shutdown_grace_milliseconds": arguments.shutdown_grace_ms,
            "connections": arguments.connections,
            "message_sizes_bytes": arguments.message_sizes,
            "bytes_per_connection": arguments.bytes_per_connection,
            "iterations": arguments.iterations,
            "warmup_iterations": arguments.warmup,
            **(
                {
                    "application_burst_messages": (
                        MANY_SOCKET_BURST_MESSAGES
                    ),
                    "application_round_messages": (
                        MANY_SOCKET_ROUND_MESSAGES
                    ),
                    "application_pending_packets_per_socket": (
                        MANY_SOCKET_PENDING_PACKETS_PER_SOCKET
                    ),
                }
                if arguments.topology == "many-socket"
                else {}
            ),
        },
        "environment": {
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_model": cpu_model(),
            "logical_cpu_count": os.cpu_count(),
        },
        "implementations": {
            "robotweax": {
                **program_identity(robotweax_peer),
                "revision": arguments.robotweax_revision,
            },
            **(
                {
                    "haivision": {
                        **program_identity(reference_peer),
                        "revision": arguments.reference_revision,
                    }
                }
                if reference_peer is not None
                else {}
            ),
        },
        "runs": [],
        "summary": [],
        "limitations": [
            "completion percentiles are not one-way packet latency",
            "Linux /proc sampling provides aggregate RSS and thread peaks",
            "syscall, wakeup, and hardware-counter collection is deferred",
            "results are evidence for optimization, not protocol semantics",
        ]
        + (
            ["one helper process is used for each connection endpoint"]
            if arguments.topology == "process-isolated"
            else [
                "one single-threaded epoll application process is used per side",
                "the requested byte target is rounded up to a whole message",
            ]
        ),
    }
    write_report(arguments.output, report)

    runs = report["runs"]
    if not isinstance(runs, list):
        raise AssertionError("report run storage is invalid")
    try:
        with tempfile.TemporaryDirectory(
            prefix="robotweax-srt-scalability-"
        ) as temporary:
            root = Path(temporary)
            for message_size in arguments.message_sizes:
                for connections in arguments.connections:
                    payload_directory = root / (
                        f"payload-{message_size}-{connections}"
                    )
                    payload_directory.mkdir()
                    payload_path = payload_directory / "payload.input"
                    expected_digest = None
                    if arguments.topology == "process-isolated":
                        expected_digest = write_payload(
                            payload_path, arguments.bytes_per_connection
                        )
                    for profile in arguments.profiles:
                        total_iterations = (
                            arguments.warmup + arguments.iterations
                        )
                        for ordinal in range(total_iterations):
                            run_directory = payload_directory / (
                                f"{profile}-{ordinal}"
                            )
                            run_directory.mkdir()
                            if arguments.topology == "process-isolated":
                                target_input = (
                                    run_directory / "payload.input"
                                )
                                try:
                                    os.link(payload_path, target_input)
                                except OSError:
                                    target_input.write_bytes(
                                        payload_path.read_bytes()
                                    )
                            options = RunOptions(
                                host=DEFAULT_HOST,
                                connections=connections,
                                bytes_per_connection=(
                                    arguments.bytes_per_connection
                                ),
                                message_size=message_size,
                                timeout_seconds=arguments.timeout_seconds,
                                latency_milliseconds=arguments.latency_ms,
                                shutdown_grace_milliseconds=(
                                    arguments.shutdown_grace_ms
                                ),
                                sampling_interval_seconds=(
                                    arguments.sampling_interval_ms / 1000.0
                                ),
                            )
                            try:
                                if arguments.topology == "many-socket":
                                    result = run_many_socket_profile(
                                        profile,
                                        programs,
                                        options,
                                        run_directory,
                                        iteration=ordinal,
                                        warmup=ordinal < arguments.warmup,
                                    )
                                else:
                                    if expected_digest is None:
                                        raise AssertionError(
                                            "payload digest is unavailable"
                                        )
                                    result = run_profile(
                                        profile,
                                        programs,
                                        options,
                                        run_directory,
                                        expected_digest,
                                        iteration=ordinal,
                                        warmup=ordinal < arguments.warmup,
                                    )
                            except Exception as error:
                                raise ScorecardFailure(
                                    f"profile={profile} connections="
                                    f"{connections} message_size="
                                    f"{message_size} iteration={ordinal}: "
                                    f"{error}"
                                ) from error
                            runs.append(result)
                            report["summary"] = summarize(runs)
                            write_report(arguments.output, report)
    except Exception as error:
        report["status"] = "failure"
        report["error"] = str(error)
        report["summary"] = summarize(runs)
        write_report(arguments.output, report)
        print(f"scalability scorecard failed: {error}", file=sys.stderr)
        return 1

    report["status"] = "complete"
    report["summary"] = summarize(runs)
    write_report(arguments.output, report)
    print(
        json.dumps(
            {
                "status": report["status"],
                "output": str(arguments.output),
                "run_count": len(runs),
                "summary_count": len(report["summary"]),
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
