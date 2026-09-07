#!/usr/bin/env python3
"""Pinned shared-binding and acquired-UDP lifecycle interoperability."""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from interop_common import (
    file_sha256,
    parse_complete,
    reserved_udp_ports,
    resolve_program_path,
    terminate,
    write_deterministic_payload,
)
from reference_version import compatible_srt_version, parse_srt_version


PINNED_SRT_VERSION = compatible_srt_version()


@dataclass(frozen=True)
class SharedScenario:
    name: str
    caller: Path
    listener: Path
    local_host: str
    listener_host: str
    peer_host: str
    ipv6_only: int | None
    seed: int


@dataclass(frozen=True)
class AcquiredScenario:
    name: str
    caller: Path
    listener: Path
    local_host: str
    peer_host: str
    ipv6_only: int | None
    seed: int


@dataclass(frozen=True)
class RunOptions:
    first_bytes: int = 98_765
    second_bytes: int = 101_113
    acquired_bytes: int = 97_531
    timeout_seconds: int = 15


@dataclass
class CapturedProcess:
    process: subprocess.Popen[str]
    stdout_path: Path
    stderr_path: Path
    stdout_stream: object
    stderr_stream: object

    def output(self) -> tuple[str, str]:
        self.stdout_stream.flush()
        self.stderr_stream.flush()
        return (
            self.stdout_path.read_text(
                encoding="utf-8", errors="replace"
            ),
            self.stderr_path.read_text(
                encoding="utf-8", errors="replace"
            ),
        )

    def close(self) -> None:
        self.stdout_stream.close()
        self.stderr_stream.close()


def shared_scenario_matrix(
    robotweax: Path, reference: Path
) -> list[SharedScenario]:
    profiles = (
        ("ipv4", "127.0.0.1", "127.0.0.1", "127.0.0.1", None),
        ("ipv6", "::1", "::1", "::1", 1),
        ("dual-stack", "::", "::", "::ffff:127.0.0.1", 0),
    )
    scenarios: list[SharedScenario] = []
    seed = 81_000
    for profile, local_host, listener_host, peer_host, ipv6_only in profiles:
        scenarios.extend(
            (
                SharedScenario(
                    name=f"shared-{profile}-robotweax-to-haivision",
                    caller=robotweax,
                    listener=reference,
                    local_host=local_host,
                    listener_host=listener_host,
                    peer_host=peer_host,
                    ipv6_only=ipv6_only,
                    seed=seed,
                ),
                SharedScenario(
                    name=f"shared-{profile}-haivision-to-robotweax",
                    caller=reference,
                    listener=robotweax,
                    local_host=local_host,
                    listener_host=listener_host,
                    peer_host=peer_host,
                    ipv6_only=ipv6_only,
                    seed=seed + 1,
                ),
            )
        )
        seed += 2
    return scenarios


def acquired_scenario_matrix(
    robotweax: Path, reference: Path
) -> list[AcquiredScenario]:
    return [
        AcquiredScenario(
            name="acquired-ipv4-robotweax-to-haivision",
            caller=robotweax,
            listener=reference,
            local_host="127.0.0.1",
            peer_host="127.0.0.1",
            ipv6_only=None,
            seed=82_000,
        ),
        AcquiredScenario(
            name="acquired-ipv4-haivision-to-robotweax",
            caller=reference,
            listener=robotweax,
            local_host="127.0.0.1",
            peer_host="127.0.0.1",
            ipv6_only=None,
            seed=82_001,
        ),
        AcquiredScenario(
            name="acquired-ipv6-robotweax-to-haivision",
            caller=robotweax,
            listener=reference,
            local_host="::1",
            peer_host="::1",
            ipv6_only=1,
            seed=82_002,
        ),
        AcquiredScenario(
            name="acquired-ipv6-haivision-to-robotweax",
            caller=reference,
            listener=robotweax,
            local_host="::1",
            peer_host="::1",
            ipv6_only=1,
            seed=82_003,
        ),
    ]


def listener_command(
    program: Path,
    host: str,
    port: int,
    output_path: Path,
    byte_count: int,
    options: RunOptions,
    ipv6_only: int | None,
) -> list[str]:
    command = [
        str(program),
        "listener",
        "--host",
        host,
        "--port",
        str(port),
        "--bytes",
        str(byte_count),
        "--output",
        str(output_path),
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
    ]
    if ipv6_only is not None:
        command.extend(("--ipv6-only", str(ipv6_only)))
    return command


def shared_caller_command(
    scenario: SharedScenario,
    source_port: int,
    first_port: int,
    second_port: int,
    first_input: Path,
    second_input: Path,
    options: RunOptions,
) -> list[str]:
    command = [
        str(scenario.caller),
        "shared-caller",
        "--local-host",
        scenario.local_host,
        "--local-port",
        str(source_port),
        "--host",
        scenario.peer_host,
        "--port",
        str(first_port),
        "--second-port",
        str(second_port),
        "--bytes",
        str(options.first_bytes),
        "--second-bytes",
        str(options.second_bytes),
        "--input",
        str(first_input),
        "--second-input",
        str(second_input),
        "--chunk-size",
        "1200",
        "--second-chunk-size",
        "1316",
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
    ]
    if scenario.ipv6_only is not None:
        command.extend(("--ipv6-only", str(scenario.ipv6_only)))
    return command


def acquired_caller_command(
    scenario: AcquiredScenario,
    port: int,
    input_path: Path,
    options: RunOptions,
) -> list[str]:
    command = [
        str(scenario.caller),
        "acquired-caller",
        "--local-host",
        scenario.local_host,
        "--host",
        scenario.peer_host,
        "--port",
        str(port),
        "--bytes",
        str(options.acquired_bytes),
        "--input",
        str(input_path),
        "--chunk-size",
        "1200",
        "--timeout-ms",
        str(options.timeout_seconds * 1_000),
    ]
    if scenario.ipv6_only is not None:
        command.extend(("--ipv6-only", str(scenario.ipv6_only)))
    return command


def start_listener(
    command: list[str], directory: Path, label: str
) -> CapturedProcess:
    stdout_path = directory / f"{label}.stdout"
    stderr_path = directory / f"{label}.stderr"
    stdout_stream = stdout_path.open("w", encoding="utf-8")
    stderr_stream = stderr_path.open("w", encoding="utf-8")
    process = subprocess.Popen(
        command,
        stdout=stdout_stream,
        stderr=stderr_stream,
        text=True,
    )
    return CapturedProcess(
        process,
        stdout_path,
        stderr_path,
        stdout_stream,
        stderr_stream,
    )


def parse_event(output: str, event_name: str) -> dict[str, object]:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") == event_name:
            return event
    raise RuntimeError(f"peer did not report {event_name}")


def process_failure(
    name: str,
    reason: str,
    caller: subprocess.CompletedProcess[str] | None,
    listeners: list[CapturedProcess],
) -> RuntimeError:
    details = [f"{name}: {reason}"]
    if caller is not None:
        details.extend(
            (
                "--- caller stdout ---",
                caller.stdout or "<empty>",
                "--- caller stderr ---",
                caller.stderr or "<empty>",
            )
        )
    for index, listener in enumerate(listeners, start=1):
        stdout, stderr = listener.output()
        details.extend(
            (
                f"--- listener {index} stdout ---",
                stdout or "<empty>",
                f"--- listener {index} stderr ---",
                stderr or "<empty>",
            )
        )
    return RuntimeError("\n".join(details))


def expected_family(host: str) -> int:
    return socket.AF_INET6 if ":" in host else socket.AF_INET


def validate_ipv6_only(
    event: dict[str, object], expected: int | None, name: str
) -> None:
    if expected is not None and event.get("ipv6_only") != expected:
        raise RuntimeError(
            f"{name}: SRTO_IPV6ONLY is {event.get('ipv6_only')}, "
            f"expected {expected}"
        )


def validate_compatibility_version(
    event: dict[str, object], name: str
) -> None:
    if event.get("srt_version") != PINNED_SRT_VERSION:
        raise RuntimeError(
            f"{name}: SRT compatibility version is "
            f"{event.get('srt_version')}, expected {PINNED_SRT_VERSION}"
        )


def valid_acquired_release_wait(event: dict[str, object]) -> bool:
    value = event.get("release_wait_us")
    return type(value) is int and 0 <= value <= 3_000_000


def run_shared_scenario(
    scenario: SharedScenario, options: RunOptions, directory: Path
) -> None:
    with reserved_udp_ports(3, scenario.listener_host) as ports:
        source_port, first_port, second_port = ports
    first_input = directory / f"{scenario.name}.first.input"
    second_input = directory / f"{scenario.name}.second.input"
    first_output = directory / f"{scenario.name}.first.output"
    second_output = directory / f"{scenario.name}.second.output"
    first_digest = write_deterministic_payload(
        first_input, options.first_bytes, scenario.seed
    )
    second_digest = write_deterministic_payload(
        second_input, options.second_bytes, scenario.seed + 100
    )

    listeners = [
        start_listener(
            listener_command(
                scenario.listener,
                scenario.listener_host,
                first_port,
                first_output,
                options.first_bytes,
                options,
                scenario.ipv6_only,
            ),
            directory,
            f"{scenario.name}.first-listener",
        ),
        start_listener(
            listener_command(
                scenario.listener,
                scenario.listener_host,
                second_port,
                second_output,
                options.second_bytes,
                options,
                scenario.ipv6_only,
            ),
            directory,
            f"{scenario.name}.second-listener",
        ),
    ]
    caller: subprocess.CompletedProcess[str] | None = None
    try:
        time.sleep(0.2)
        if any(listener.process.poll() is not None for listener in listeners):
            raise process_failure(
                scenario.name,
                "listener exited before caller startup",
                caller,
                listeners,
            )
        caller = subprocess.run(
            shared_caller_command(
                scenario,
                source_port,
                first_port,
                second_port,
                first_input,
                second_input,
                options,
            ),
            capture_output=True,
            text=True,
            timeout=options.timeout_seconds + 5,
            check=False,
        )
        for listener in listeners:
            listener.process.wait(timeout=options.timeout_seconds + 5)
        if caller.returncode != 0 or any(
            listener.process.returncode != 0 for listener in listeners
        ):
            raise process_failure(
                scenario.name,
                "shared transfer failed",
                caller,
                listeners,
            )

        caller_event = parse_event(caller.stdout, "shared_complete")
        if (
            caller_event.get("first_bytes") != options.first_bytes
            or caller_event.get("second_bytes") != options.second_bytes
            or caller_event.get("local_port") != source_port
            or caller_event.get("local_family")
            != expected_family(scenario.local_host)
            or caller_event.get("first_owner_closed") is not True
        ):
            raise process_failure(
                scenario.name,
                f"invalid shared completion event: {caller_event}",
                caller,
                listeners,
            )
        validate_ipv6_only(
            caller_event, scenario.ipv6_only, scenario.name
        )
        validate_compatibility_version(caller_event, scenario.name)
        for index, listener in enumerate(listeners):
            stdout, _ = listener.output()
            event = parse_complete(stdout, "listener")
            expected_bytes = (
                options.first_bytes if index == 0 else options.second_bytes
            )
            if event.get("bytes") != expected_bytes:
                raise RuntimeError(
                    f"{scenario.name}: listener {index + 1} byte mismatch"
                )
            validate_ipv6_only(event, scenario.ipv6_only, scenario.name)
            validate_compatibility_version(event, scenario.name)
        if file_sha256(first_output) != first_digest:
            raise RuntimeError(
                f"{scenario.name}: first payload SHA-256 mismatch"
            )
        if file_sha256(second_output) != second_digest:
            raise RuntimeError(
                f"{scenario.name}: second payload SHA-256 mismatch"
            )
        print(
            f"PASS {scenario.name} first_sha256={first_digest} "
            f"second_sha256={second_digest} source_port={source_port} "
            f"ipv6_only={scenario.ipv6_only} first_owner_closed=true"
        )
    finally:
        for listener in listeners:
            terminate(listener.process)
            listener.close()


def run_acquired_scenario(
    scenario: AcquiredScenario, options: RunOptions, directory: Path
) -> None:
    with reserved_udp_ports(1, scenario.peer_host) as ports:
        (port,) = ports
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    digest = write_deterministic_payload(
        input_path, options.acquired_bytes, scenario.seed
    )
    listener = start_listener(
        listener_command(
            scenario.listener,
            scenario.peer_host,
            port,
            output_path,
            options.acquired_bytes,
            options,
            scenario.ipv6_only,
        ),
        directory,
        f"{scenario.name}.listener",
    )
    caller: subprocess.CompletedProcess[str] | None = None
    try:
        time.sleep(0.2)
        if listener.process.poll() is not None:
            raise process_failure(
                scenario.name,
                "listener exited before acquired caller startup",
                caller,
                [listener],
            )
        caller = subprocess.run(
            acquired_caller_command(
                scenario, port, input_path, options
            ),
            capture_output=True,
            text=True,
            timeout=options.timeout_seconds + 5,
            check=False,
        )
        listener.process.wait(timeout=options.timeout_seconds + 5)
        if caller.returncode != 0 or listener.process.returncode != 0:
            raise process_failure(
                scenario.name,
                "acquired transfer failed",
                caller,
                [listener],
            )
        event = parse_event(caller.stdout, "acquired_complete")
        release_wait = event.get("release_wait_us")
        if (
            event.get("bytes") != options.acquired_bytes
            or event.get("local_port", 0) <= 0
            or event.get("local_family")
            != expected_family(scenario.local_host)
            or event.get("native_closed") is not True
            or event.get("port_rebound") is not True
            or not valid_acquired_release_wait(event)
        ):
            raise process_failure(
                scenario.name,
                f"invalid acquired completion event: {event}",
                caller,
                [listener],
            )
        validate_ipv6_only(event, scenario.ipv6_only, scenario.name)
        validate_compatibility_version(event, scenario.name)
        listener_stdout, _ = listener.output()
        listener_event = parse_complete(listener_stdout, "listener")
        if listener_event.get("bytes") != options.acquired_bytes:
            raise RuntimeError(
                f"{scenario.name}: listener byte counter mismatch"
            )
        validate_compatibility_version(listener_event, scenario.name)
        if file_sha256(output_path) != digest:
            raise RuntimeError(f"{scenario.name}: SHA-256 mismatch")
        print(
            f"PASS {scenario.name} sha256={digest} "
            f"local_port={event['local_port']} "
            "native_closed=true port_rebound=true "
            f"release_wait_us={release_wait}"
        )
    finally:
        terminate(listener.process)
        listener.close()


def main() -> int:
    global PINNED_SRT_VERSION
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=15)
    parser.add_argument(
        "--expected-reference-version",
        type=parse_srt_version,
        default=compatible_srt_version(),
    )
    arguments = parser.parse_args()
    PINNED_SRT_VERSION = arguments.expected_reference_version
    if arguments.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be positive")

    robotweax = resolve_program_path(arguments.robotweax_peer)
    reference = resolve_program_path(arguments.reference_peer)
    options = RunOptions(timeout_seconds=arguments.timeout_seconds)
    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="robotweax-srt-binding-interop-"
    ) as temporary:
        directory = Path(temporary)
        for scenario in shared_scenario_matrix(robotweax, reference):
            try:
                run_shared_scenario(scenario, options, directory)
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                failures.append(f"{scenario.name}: {error}")
        for scenario in acquired_scenario_matrix(robotweax, reference):
            try:
                run_acquired_scenario(scenario, options, directory)
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                failures.append(f"{scenario.name}: {error}")

    if failures:
        print(
            "shared binding and acquired-socket interoperability failures:\n\n"
            + "\n\n".join(failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
