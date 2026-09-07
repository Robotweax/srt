#!/usr/bin/env python3
"""IPv6 MSS negotiation and wire-statistics interoperability."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

from interop_common import (
    expected_live_packets,
    file_sha256,
    free_udp_port,
    nonnegative_statistic,
    parse_complete,
    resolve_program_path,
    terminate,
    write_deterministic_payload,
)
from reference_version import compatible_srt_version, parse_srt_version


IPV6_SRT_PACKET_OVERHEAD = 64
PINNED_SRT_VERSION = compatible_srt_version()
SRT_EPOLL_IN = 0x1
SRT_EPOLL_OUT = 0x4
SRT_EPOLL_ERR = 0x8
SRT_EPOLL_IO_EVENTS = SRT_EPOLL_IN | SRT_EPOLL_OUT | SRT_EPOLL_ERR


@dataclass(frozen=True)
class Scenario:
    name: str
    caller: Path
    listener: Path
    seed: int
    stream_id: str
    caller_mss: int = 1_400
    listener_mss: int = 1_280
    caller_ipttl: int = 43
    listener_ipttl: int = 44
    caller_iptos: int = 0x88
    listener_iptos: int = 0x90

    @property
    def negotiated_mss(self) -> int:
        return min(self.caller_mss, self.listener_mss)


@dataclass(frozen=True)
class RunOptions:
    byte_count: int = 1_048_699
    timeout_seconds: int = 30
    chunk_size: int = 1_216
    host: str = "::1"


def scenario_matrix(robotweax: Path, reference: Path) -> list[Scenario]:
    return [
        Scenario(
            name="ipv6-mss-robotweax-to-haivision",
            caller=robotweax,
            listener=reference,
            seed=60_001,
            stream_id="#!::r=live/robotweax-ipv6,m=request",
        ),
        Scenario(
            name="ipv6-mss-haivision-to-robotweax",
            caller=reference,
            listener=robotweax,
            seed=60_002,
            stream_id="#!::r=live/haivision-ipv6,m=request",
        ),
    ]


def peer_command(
    program: Path,
    role: str,
    port: int,
    payload_path: Path,
    options: RunOptions,
    mss: int,
    stream_id: str,
    ipttl: int,
    iptos: int,
    *,
    require_connect_callback: bool = False,
) -> list[str]:
    path_option = "--output" if role == "listener" else "--input"
    command = [
        str(program),
        role,
        "--host",
        options.host,
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
        "--mss",
        str(mss),
        "--ipttl",
        str(ipttl),
        "--iptos",
        str(iptos),
        "--shutdown-grace-ms",
        "250",
        "--stream-id" if role == "caller" else "--expect-stream-id",
        stream_id,
    ]
    if role == "caller":
        command.append("--connect-callback")
        if require_connect_callback:
            command.append("--require-connect-callback")
    return command


def stream_id_was_verified(output: str, expected_size: int) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (
            event.get("event") == "stream_id_match"
            and event.get("bytes") == expected_size
        ):
            return True
    return False


def connect_callback_was_valid(output: str) -> bool:
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if event.get("event") != "connect_callback":
            continue
        return all(
            (
                event.get("valid") is True,
                event.get("error_code") == 0,
                event.get("peer_family") == 10,
                event.get("token") == -1,
                event.get("socket_state") == 5,
                event.get("peer_name_available") is True,
            )
        )
    return False


def connect_callback_meets_policy(output: str, *, required: bool) -> bool:
    """Keep Robotweax strict without inheriting a v1.5.5 callback defect."""
    return not required or connect_callback_was_valid(output)


def render_failure(
    scenario: Scenario,
    reason: str,
    output_path: Path,
    caller_stdout: str = "",
    caller_stderr: str = "",
    listener_stdout: str = "",
    listener_stderr: str = "",
) -> str:
    partial = output_path.stat().st_size if output_path.exists() else 0
    return (
        f"{scenario.name}: {reason}; partial_output_bytes={partial}\n"
        f"--- caller stdout ---\n{caller_stdout or '<empty>'}\n"
        f"--- caller stderr ---\n{caller_stderr or '<empty>'}\n"
        f"--- listener stdout ---\n{listener_stdout or '<empty>'}\n"
        f"--- listener stderr ---\n{listener_stderr or '<empty>'}\n"
    )


def validate_robotweax_statistics(
    event: dict[str, object],
    *,
    sender: bool,
    byte_count: int,
    packet_count: int,
    negotiated_mss: int,
) -> None:
    byte_mss = nonnegative_statistic(event, "byteMSS")
    if byte_mss != negotiated_mss:
        raise RuntimeError(
            f"Robotweax byteMSS is {byte_mss}, expected {negotiated_mss}"
        )

    packet_name = "pktSentTotal" if sender else "pktRecvTotal"
    byte_name = "byteSentTotal" if sender else "byteRecvTotal"
    unique_name = (
        "byteSentUniqueTotal" if sender else "byteRecvUniqueTotal"
    )
    observed_packets = nonnegative_statistic(event, packet_name)
    observed_bytes = nonnegative_statistic(event, byte_name)
    observed_unique_bytes = nonnegative_statistic(event, unique_name)
    if observed_packets != packet_count:
        raise RuntimeError(
            f"Robotweax {packet_name} is {observed_packets}, "
            f"expected {packet_count}"
        )
    expected_wire_bytes = (
        byte_count + packet_count * IPV6_SRT_PACKET_OVERHEAD
    )
    if observed_bytes != expected_wire_bytes:
        raise RuntimeError(
            f"Robotweax {byte_name} is {observed_bytes}, "
            f"expected IPv6 wire total {expected_wire_bytes}"
        )
    if observed_unique_bytes != expected_wire_bytes:
        raise RuntimeError(
            f"Robotweax {unique_name} is {observed_unique_bytes}, "
            f"expected IPv6 wire total {expected_wire_bytes}"
        )


def validate_completion_readiness(
    event: dict[str, object], *, peer_name: str
) -> None:
    readiness = event.get("readiness")
    if not isinstance(readiness, dict):
        raise RuntimeError(f"{peer_name} readiness options are unavailable")
    events = readiness.get("event")
    snddata = readiness.get("snddata")
    rcvdata = readiness.get("rcvdata")
    if not all(type(value) is int for value in (events, snddata, rcvdata)):
        raise RuntimeError(f"{peer_name} readiness options are not integers")
    events = int(events)
    snddata = int(snddata)
    rcvdata = int(rcvdata)
    if events & ~SRT_EPOLL_IO_EVENTS:
        raise RuntimeError(
            f"{peer_name} SRTO_EVENT contains unknown flags: {events}"
        )
    # A reference listener may observe the successful peer shutdown before it
    # captures the options, leaving ERR as the only terminal readiness flag.
    if (events & (SRT_EPOLL_OUT | SRT_EPOLL_ERR)) == 0:
        raise RuntimeError(
            f"{peer_name} completed socket is neither write-ready nor "
            f"terminal: {events}"
        )
    if snddata != 0 or rcvdata != 0:
        raise RuntimeError(
            f"{peer_name} completion buffers did not drain: "
            f"snddata={snddata}, rcvdata={rcvdata}"
        )


def validate_peer_version(
    event: dict[str, object], *, peer_name: str
) -> None:
    peer_version = event.get("peer_version")
    if type(peer_version) is not int:
        raise RuntimeError(f"{peer_name} SRTO_PEERVERSION is unavailable")
    if peer_version != PINNED_SRT_VERSION:
        raise RuntimeError(
            f"{peer_name} SRTO_PEERVERSION is {peer_version:#x}, "
            f"expected {PINNED_SRT_VERSION:#x}"
        )


def validate_network_options(
    event: dict[str, object], *, peer_name: str, ipttl: int, iptos: int
) -> None:
    network = event.get("network")
    if not isinstance(network, dict):
        raise RuntimeError(f"{peer_name} network options are unavailable")
    if network.get("ipttl") != ipttl or network.get("iptos") != iptos:
        raise RuntimeError(
            f"{peer_name} network options are {network}, expected "
            f"ipttl={ipttl}, iptos={iptos}"
        )


def validate_version_timing_options(
    event: dict[str, object], *, peer_name: str
) -> None:
    options = event.get("version_timing")
    if not isinstance(options, dict):
        raise RuntimeError(
            f"{peer_name} version/timing options are unavailable"
        )
    if options.get("drift_tracer") is not True:
        raise RuntimeError(f"{peer_name} drift tracer is not enabled")
    if options.get("minimum_input_bandwidth") != 0:
        raise RuntimeError(
            f"{peer_name} minimum input bandwidth is not zero"
        )
    if options.get("minimum_peer_version") != 0x0001_0000:
        raise RuntimeError(
            f"{peer_name} minimum peer version is not 1.0.0"
        )
    connection_time = options.get("connection_time")
    current_time = options.get("current_time")
    if (
        type(connection_time) is not int
        or type(current_time) is not int
        or connection_time < 0
        or connection_time > current_time
    ):
        raise RuntimeError(
            f"{peer_name} connection time is outside its monotonic epoch"
        )


def run_scenario(
    scenario: Scenario,
    options: RunOptions,
    directory: Path,
    robotweax: Path,
) -> None:
    port = free_udp_port(options.host)
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
                port,
                output_path,
                options,
                scenario.listener_mss,
                scenario.stream_id,
                scenario.listener_ipttl,
                scenario.listener_iptos,
            ),
            stdout=listener_stdout_stream,
            stderr=listener_stderr_stream,
            text=True,
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
            time.sleep(0.2)
            if listener.poll() is not None:
                stdout, stderr = listener_output()
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "listener exited before caller startup",
                        output_path,
                        listener_stdout=stdout,
                        listener_stderr=stderr,
                    )
                )
            caller = subprocess.run(
                peer_command(
                    scenario.caller,
                    "caller",
                    port,
                    input_path,
                    options,
                    scenario.caller_mss,
                    scenario.stream_id,
                    scenario.caller_ipttl,
                    scenario.caller_iptos,
                    require_connect_callback=scenario.caller == robotweax,
                ),
                capture_output=True,
                text=True,
                timeout=options.timeout_seconds + 5,
                check=False,
            )
            listener.wait(timeout=options.timeout_seconds + 5)
            listener_stdout, listener_stderr = listener_output()
            if caller.returncode != 0 or listener.returncode != 0:
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "transfer failed "
                        f"(caller={caller.returncode}, "
                        f"listener={listener.returncode})",
                        output_path,
                        caller.stdout,
                        caller.stderr,
                        listener_stdout,
                        listener_stderr,
                    )
                )

            if not stream_id_was_verified(
                listener_stdout, len(scenario.stream_id.encode("utf-8"))
            ):
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "listener did not verify the caller stream ID",
                        output_path,
                        caller.stdout,
                        caller.stderr,
                        listener_stdout,
                        listener_stderr,
                    )
                )
            if not connect_callback_meets_policy(
                caller.stdout, required=scenario.caller == robotweax
            ):
                raise RuntimeError(
                    render_failure(
                        scenario,
                        "caller did not verify the IPv6 connect callback",
                        output_path,
                        caller.stdout,
                        caller.stderr,
                        listener_stdout,
                        listener_stderr,
                    )
                )

            caller_event = parse_complete(caller.stdout, "caller")
            listener_event = parse_complete(listener_stdout, "listener")
            validate_completion_readiness(
                caller_event,
                peer_name=(
                    "Robotweax caller"
                    if scenario.caller == robotweax
                    else "Haivision caller"
                ),
            )
            validate_completion_readiness(
                listener_event,
                peer_name=(
                    "Robotweax listener"
                    if scenario.listener == robotweax
                    else "Haivision listener"
                ),
            )
            validate_peer_version(
                caller_event,
                peer_name=(
                    "Robotweax caller"
                    if scenario.caller == robotweax
                    else "Haivision caller"
                ),
            )
            validate_peer_version(
                listener_event,
                peer_name=(
                    "Robotweax listener"
                    if scenario.listener == robotweax
                    else "Haivision listener"
                ),
            )
            validate_network_options(
                caller_event,
                peer_name=(
                    "Robotweax caller"
                    if scenario.caller == robotweax
                    else "Haivision caller"
                ),
                ipttl=scenario.caller_ipttl,
                iptos=scenario.caller_iptos,
            )
            validate_network_options(
                listener_event,
                peer_name=(
                    "Robotweax listener"
                    if scenario.listener == robotweax
                    else "Haivision listener"
                ),
                ipttl=scenario.listener_ipttl,
                iptos=scenario.listener_iptos,
            )
            validate_version_timing_options(
                caller_event,
                peer_name=(
                    "Robotweax caller"
                    if scenario.caller == robotweax
                    else "Haivision caller"
                ),
            )
            validate_version_timing_options(
                listener_event,
                peer_name=(
                    "Robotweax listener"
                    if scenario.listener == robotweax
                    else "Haivision listener"
                ),
            )
            if (
                caller_event.get("bytes") != options.byte_count
                or listener_event.get("bytes") != options.byte_count
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

            packet_count = expected_live_packets(
                options.byte_count, options.chunk_size
            )
            robotweax_is_sender = scenario.caller == robotweax
            validate_robotweax_statistics(
                caller_event if robotweax_is_sender else listener_event,
                sender=robotweax_is_sender,
                byte_count=options.byte_count,
                packet_count=packet_count,
                negotiated_mss=scenario.negotiated_mss,
            )
            reference_event = (
                listener_event if robotweax_is_sender else caller_event
            )
            reference_mss = nonnegative_statistic(
                reference_event, "byteMSS"
            )
            if reference_mss != scenario.negotiated_mss:
                raise RuntimeError(
                    f"{scenario.name}: Haivision byteMSS is "
                    f"{reference_mss}, expected {scenario.negotiated_mss}"
                )

            print(
                f"PASS {scenario.name} sha256={received_digest} "
                f"mss={scenario.negotiated_mss} "
                f"caller_ipttl={scenario.caller_ipttl} "
                f"caller_iptos={scenario.caller_iptos} "
                f"listener_ipttl={scenario.listener_ipttl} "
                f"listener_iptos={scenario.listener_iptos} "
                f"packets={packet_count} "
                "robotweax_ipv6_header_bytes="
                f"{IPV6_SRT_PACKET_OVERHEAD}"
            )
        except subprocess.TimeoutExpired as error:
            terminate(listener)
            listener_stdout, listener_stderr = listener_output()
            raise RuntimeError(
                render_failure(
                    scenario,
                    "peer timed out",
                    output_path,
                    caller.stdout if caller is not None else "",
                    caller.stderr if caller is not None else "",
                    listener_stdout,
                    listener_stderr,
                )
            ) from error
        finally:
            if listener.poll() is None:
                terminate(listener)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--bytes", type=int, default=1_048_699)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument(
        "--expected-reference-version",
        type=parse_srt_version,
        default=compatible_srt_version(),
    )
    return parser.parse_args()


def main() -> int:
    global PINNED_SRT_VERSION
    arguments = parse_arguments()
    PINNED_SRT_VERSION = arguments.expected_reference_version
    if arguments.bytes <= 0 or arguments.timeout_seconds <= 0:
        raise SystemExit("byte and timeout values must be positive")
    robotweax = resolve_program_path(arguments.robotweax_peer)
    reference = resolve_program_path(arguments.reference_peer)
    options = RunOptions(
        byte_count=arguments.bytes,
        timeout_seconds=arguments.timeout_seconds,
    )
    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="robotweax-srt-ipv6-mtu-interop-"
    ) as temporary_directory:
        directory = Path(temporary_directory)
        for scenario in scenario_matrix(robotweax, reference):
            try:
                run_scenario(
                    scenario, options, directory, robotweax
                )
            except RuntimeError as error:
                failures.append(str(error))
    if failures:
        print(
            "IPv6 MSS/statistics interoperability failures:\n\n"
            + "\n\n".join(failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
