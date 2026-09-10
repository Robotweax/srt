#!/usr/bin/env python3
"""Encrypted caller/listener interoperability against pinned Haivision SRT."""

from __future__ import annotations

import argparse
import ipaddress
import json
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
    expected_live_packets,
    file_sha256,
    free_udp_port,
    nonnegative_statistic,
    parse_complete,
    resolve_program_path,
    terminate,
    validate_sender_statistics,
    write_deterministic_payload,
)
from srt_handshake_trace import HandshakeTraceProxy


PASSPHRASE_ENVIRONMENT = "SRT_INTEROP_PASSPHRASE"


@dataclass(frozen=True)
class Scenario:
    name: str
    caller: Path
    listener: Path
    key_length: int
    seed: int


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
    crypto_mode: str | None = None
    expected_crypto_mode: str | None = None
    maximum_payload_size: int | None = None
    expected_maximum_payload_size: int | None = None


def peer_command(
    program: Path,
    role: str,
    port: int,
    payload_path: Path,
    options: RunOptions,
    key_length: int,
    host: str = "127.0.0.1",
) -> list[str]:
    path_option = "--output" if role == "listener" else "--input"
    command = [
        str(program),
        role,
        "--host",
        host,
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
        str(options.chunk_size),
        "--passphrase-env",
        PASSPHRASE_ENVIRONMENT,
        "--pbkeylen",
        str(key_length),
        "--km-refresh-rate",
        str(options.key_refresh_rate),
        "--km-preannounce",
        str(options.key_preannouncement),
        "--shutdown-grace-ms",
        str(options.shutdown_grace_milliseconds),
    ]
    if role == "caller":
        # SRT applies INPUTBW only when MAXBW selects relative-rate mode.
        command.extend(
            (
                "--input-bw",
                str(options.input_bandwidth_bytes_per_second),
                "--max-bw",
                "0",
            )
        )
    if options.crypto_mode is not None:
        command.extend(("--crypto-mode", options.crypto_mode))
    if options.expected_crypto_mode is not None:
        command.extend(
            ("--expect-crypto-mode", options.expected_crypto_mode)
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
    return command


def no_rotation_baselines(
    robotweax: Path,
    reference: Path,
    options: RunOptions,
    *,
    name_prefix: str = "",
) -> tuple[list[Scenario], RunOptions, int]:
    packet_count = (
        options.key_refresh_rate - options.key_preannouncement - 1
    )
    baselines = [
        Scenario(
            name=(
                f"{name_prefix}"
                "aes128-robotweax-to-haivision-no-rotation"
            ),
            caller=robotweax,
            listener=reference,
            key_length=16,
            seed=9_001,
        ),
        Scenario(
            name=(
                f"{name_prefix}"
                "aes128-haivision-to-robotweax-no-rotation"
            ),
            caller=reference,
            listener=robotweax,
            key_length=16,
            seed=9_002,
        ),
    ]
    return (
        baselines,
        replace(
            options,
            byte_count=packet_count * options.chunk_size,
        ),
        packet_count,
    )


def render_failure(
    scenario: Scenario,
    reason: str,
    output_path: Path,
    caller_stdout: str | bytes | None = None,
    caller_stderr: str | bytes | None = None,
    listener_stdout: str | bytes | None = None,
    listener_stderr: str | bytes | None = None,
    handshake_trace: str | None = None,
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
        + section("caller stdout", caller_stdout)
        + section("caller stderr", caller_stderr)
        + section("listener stdout", listener_stdout)
        + section("listener stderr", listener_stderr)
    )
    if handshake_trace is not None:
        report += section("secret-safe handshake trace", handshake_trace)
    return report


def wait_for_listener_ready(listener, output, port: int, timeout: float) -> None:
    """Wait for this peer's complete ready event, never merely a live process."""
    deadline = time.monotonic() + timeout
    while True:
        if listener.poll() is not None:
            raise RuntimeError("listener exited before readiness")
        stdout, _ = output()
        for line in stdout.splitlines():
            try:
                event = json.loads(line)
            except (ValueError, TypeError):
                continue
            if isinstance(event, dict) and event.get("event") == "ready" and event.get("port") == port:
                return
        if time.monotonic() >= deadline:
            raise RuntimeError("listener readiness timed out")
        time.sleep(0.01)


def run_scenario(
    scenario: Scenario,
    options: RunOptions,
    directory: Path,
    environment: dict[str, str],
    minimum_packets: int | None = None,
    trace_handshake: bool = False,
    host: str = "127.0.0.1",
) -> None:
    listener_port = free_udp_port(host)
    trace_proxy = (
        HandshakeTraceProxy(listener_port) if trace_handshake else None
    )
    caller_port = (
        trace_proxy.port if trace_proxy is not None else listener_port
    )
    input_path = directory / f"{scenario.name}.input"
    output_path = directory / f"{scenario.name}.output"
    expected_digest = write_deterministic_payload(
        input_path, options.byte_count, scenario.seed
    )

    listener_stdout_path = directory / f"{scenario.name}.listener.stdout"
    listener_stderr_path = directory / f"{scenario.name}.listener.stderr"
    with (
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
                options,
                scenario.key_length,
                host,
            ),
            stdout=listener_stdout_stream,
            stderr=listener_stderr_stream,
            text=True,
            env=environment,
        )
        if trace_proxy is not None:
            trace_proxy.start()

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

        def handshake_trace() -> str | None:
            return (
                trace_proxy.render(scenario.name)
                if trace_proxy is not None
                else None
            )

        try:
            try:
                wait_for_listener_ready(listener, listener_output, listener_port,
                                        options.timeout_seconds)
            except RuntimeError as error:
                listener_stdout, listener_stderr = listener_output()
                trace = handshake_trace()
                raise RuntimeError(
                    f"{scenario.name}: "
                    f"{error}\n"
                    + listener_stdout
                    + listener_stderr
                    + (
                        "\n--- secret-safe handshake trace ---\n"
                        + trace
                        if trace is not None
                        else ""
                    )
                ) from error

            try:
                caller = subprocess.run(
                    peer_command(
                        scenario.caller,
                        "caller",
                        caller_port,
                        input_path,
                        options,
                        scenario.key_length,
                        host,
                    ),
                    capture_output=True,
                    text=True,
                    timeout=options.timeout_seconds + 5,
                    check=False,
                    env=environment,
                )
            except subprocess.TimeoutExpired as error:
                terminate(listener)
                listener_stdout, listener_stderr = listener_output()
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "caller timed out",
                        output_path,
                        caller_stdout=error.stdout or "",
                        caller_stderr=error.stderr or "",
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        handshake_trace=handshake_trace(),
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
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        handshake_trace=handshake_trace(),
                    )
                ) from error
            if caller.returncode != 0 or listener.returncode != 0:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "encrypted transfer failed "
                        f"(caller={caller.returncode}, "
                        f"listener={listener.returncode})",
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        handshake_trace=handshake_trace(),
                    )
                )

            caller_complete = parse_complete(caller.stdout, "caller")
            listener_complete = parse_complete(listener_stdout, "listener")
            if (
                caller_complete.get("bytes") != options.byte_count
                or listener_complete.get("bytes") != options.byte_count
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
            required_packets = max(
                expected_live_packets(
                    options.byte_count, options.chunk_size
                ),
                options.key_refresh_rate * 3
                if minimum_packets is None
                else minimum_packets,
            )
            try:
                packets_sent = validate_sender_statistics(
                    caller_complete, required_packets
                )
            except RuntimeError as error:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        str(error),
                        output_path,
                        caller_stdout=caller.stdout,
                        caller_stderr=caller.stderr,
                        listener_stdout=listener_stdout,
                        listener_stderr=listener_stderr,
                        handshake_trace=handshake_trace(),
                    )
                ) from error
            packets_received = nonnegative_statistic(
                listener_complete, "pktRecvTotal"
            )
            if packets_received is None:
                raise RuntimeError(
                    f"{scenario.name}: receiver packet statistics "
                    "are unavailable"
                )
            if packets_received < required_packets:
                raise RuntimeError(
                    f"{scenario.name}: only {packets_received} packets were "
                    f"received; at least {required_packets} are required "
                    f"(sender reported {packets_sent})"
                )
            listener_stats = listener_complete.get("stats")
            undecryptable = (
                listener_stats.get("pktRcvUndecryptTotal", 0)
                if isinstance(listener_stats, dict)
                else 0
            )
            if undecryptable != 0:
                raise RuntimeError(
                    f"{scenario.name}: receiver reported "
                    f"{undecryptable} undecryptable packets"
                )
            if trace_proxy is not None:
                print(handshake_trace(), flush=True)
            print(
                f"PASS {scenario.name} sha256={expected_digest} "
                f"packets_sent={packets_sent} "
                f"packets_received={packets_received}",
                flush=True,
            )
        finally:
            if listener.poll() is None:
                terminate(listener)
            if trace_proxy is not None:
                trace_proxy.close()


def run_no_rotation_baselines(
    scenarios: list[Scenario],
    options: RunOptions,
    directory: Path,
    environment: dict[str, str],
    minimum_packets: int,
    *,
    trace_handshake: bool = False,
    host: str = "127.0.0.1",
) -> None:
    failures: list[str] = []
    for scenario in scenarios:
        try:
            run_scenario(
                scenario,
                options,
                directory,
                environment,
                minimum_packets=minimum_packets,
                trace_handshake=trace_handshake,
                host=host,
            )
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            failures.append(str(error))

    if failures:
        raise RuntimeError(
            "encrypted no-rotation baseline failures:\n\n"
            + "\n\n".join(failures)
        )


def scenario_matrix(
    robotweax: Path,
    reference: Path,
    *,
    name_prefix: str = "",
) -> list[Scenario]:
    scenarios: list[Scenario] = []
    for index, key_length in enumerate((16, 24, 32), start=1):
        label = key_length * 8
        scenarios.extend(
            (
                Scenario(
                    name=(
                        f"{name_prefix}aes{label}-"
                        "robotweax-to-haivision"
                    ),
                    caller=robotweax,
                    listener=reference,
                    key_length=key_length,
                    seed=10_000 + index,
                ),
                Scenario(
                    name=(
                        f"{name_prefix}aes{label}-"
                        "haivision-to-robotweax"
                    ),
                    caller=reference,
                    listener=robotweax,
                    key_length=key_length,
                    seed=20_000 + index,
                ),
            )
        )
    return scenarios


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
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
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument(
        "--baseline-only",
        action="store_true",
        help="run only the two no-rotation baseline directions",
    )
    parser.add_argument(
        "--trace-baseline-handshake",
        action="store_true",
        help=(
            "route no-rotation baselines through the diagnostic handshake "
            "proxy; do not use this for loss-free payload validation"
        ),
    )
    arguments = parser.parse_args()

    try:
        host_address = ipaddress.ip_address(arguments.host)
    except ValueError as error:
        parser.error(str(error))
    if (
        arguments.bytes <= 0
        or arguments.timeout_seconds <= 0
        or arguments.km_refresh_rate <= 0
        or arguments.km_preannounce <= 0
        or arguments.input_bw <= 0
        or arguments.shutdown_grace_ms < 0
        or arguments.km_preannounce
        > (arguments.km_refresh_rate - 1) // 2
        or (
            arguments.trace_baseline_handshake
            and host_address.version != 4
        )
    ):
        parser.error("invalid transfer or key-rotation parameters")

    try:
        robotweax = resolve_program_path(arguments.robotweax_peer)
        reference = resolve_program_path(arguments.reference_peer)
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
        name_prefix = "ipv6-" if host_address.version == 6 else ""
        with tempfile.TemporaryDirectory(
            prefix="robotweax-srt-encrypted-interop-"
        ) as directory:
            work = Path(directory)
            baselines, baseline_options, baseline_packets = (
                no_rotation_baselines(
                    robotweax,
                    reference,
                    options,
                    name_prefix=name_prefix,
                )
            )
            run_no_rotation_baselines(
                baselines,
                baseline_options,
                work,
                environment,
                baseline_packets,
                trace_handshake=arguments.trace_baseline_handshake,
                host=arguments.host,
            )
            if not arguments.baseline_only:
                for scenario in scenario_matrix(
                    robotweax,
                    reference,
                    name_prefix=name_prefix,
                ):
                    run_scenario(
                        scenario, options, work, environment,
                        host=arguments.host,
                    )
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
