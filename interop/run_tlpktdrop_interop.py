#!/usr/bin/env python3
"""Validate receiver-side TLPKTDROP and cumulative fake ACK interop."""

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
    file_sha256,
    free_udp_port,
    nonnegative_statistic,
    parse_complete,
    resolve_program_path,
)
from live_timing import (
    GeneratedMessage,
    MpegTsProfile,
    generate_binary_messages,
    generate_mpeg_ts_messages,
)
from srt_handshake_trace import (
    SEQUENCE_MASK,
    CallerListenerFaultProxy,
    CallerListenerSequenceProxy,
    RendezvousFault,
)


PROFILE_MESSAGE_SIZE = {
    "binary-1200": 1_200,
    "binary-max-1456": 1_456,
    "ts-1316": 1_316,
}
SRT_LIVE_MAX_PAYLOAD_SIZE = 1_456
MESSAGE_COUNT = 3
BITRATE_BITS_PER_SECOND = 7_520_000
LATENCY_MILLISECONDS = 300
TIMEOUT_MILLISECONDS = 3_000
DISABLED_TIMEOUT_MILLISECONDS = 900


@dataclass(frozen=True)
class Scenario:
    name: str
    sender: Path
    receiver: Path
    profile: str
    tlpktdrop: bool = True
    rollover: bool = False


def scenario_matrix(
    robotweax_peer: Path, reference_peer: Path
) -> tuple[Scenario, ...]:
    directions = (
        ("robotweax-to-haivision", robotweax_peer, reference_peer),
        ("haivision-to-robotweax", reference_peer, robotweax_peer),
    )
    scenarios: list[Scenario] = []
    for direction, sender, receiver in directions:
        for profile in (
            "binary-1200",
            "binary-max-1456",
            "ts-1316",
        ):
            scenarios.append(
                Scenario(
                    name=f"{direction}-{profile}",
                    sender=sender,
                    receiver=receiver,
                    profile=profile,
                )
            )
        scenarios.append(
            Scenario(
                name=f"{direction}-ts-1316-rollover",
                sender=sender,
                receiver=receiver,
                profile="ts-1316",
                rollover=True,
            )
        )
        scenarios.append(
            Scenario(
                name=f"{direction}-tlpktdrop-disabled",
                sender=sender,
                receiver=receiver,
                profile="binary-1200",
                tlpktdrop=False,
            )
        )
    return tuple(scenarios)


def generated_messages(profile: str) -> list[GeneratedMessage]:
    try:
        message_size = PROFILE_MESSAGE_SIZE[profile]
    except KeyError as error:
        raise ValueError(f"unknown TLPKTDROP profile: {profile}") from error
    if profile == "ts-1316":
        return generate_mpeg_ts_messages(
            MpegTsProfile(
                packets_per_message=7,
                message_count=MESSAGE_COUNT,
                bitrate_bits_per_second=BITRATE_BITS_PER_SECOND,
                pcr_interval_packets=7,
            )
        )
    return generate_binary_messages(
        message_size,
        MESSAGE_COUNT,
        BITRATE_BITS_PER_SECOND,
    )


def write_messages(
    path: Path, messages: list[GeneratedMessage], start: int = 0
) -> None:
    with path.open("wb") as output:
        for message in messages[start:]:
            output.write(message.payload)


def peer_command(
    peer: Path,
    role: str,
    port: int,
    path: Path,
    byte_count: int,
    message_size: int,
    *,
    tlpktdrop: bool,
) -> list[str]:
    timeout = (
        TIMEOUT_MILLISECONDS
        if tlpktdrop
        else DISABLED_TIMEOUT_MILLISECONDS
    )
    command = [
        str(peer),
        role,
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--bytes",
        str(byte_count),
        "--transport",
        "live",
        "--latency-ms",
        str(LATENCY_MILLISECONDS),
        "--payload-size",
        str(message_size),
        "--tlpktdrop",
        "on" if tlpktdrop else "off",
        "--timeout-ms",
        str(timeout),
        "--shutdown-grace-ms",
        "350",
    ]
    if role == "caller":
        command.extend(
            (
                "--input",
                str(path),
                "--chunk-size",
                str(message_size),
                "--input-bw",
                str(BITRATE_BITS_PER_SECOND // 8),
                "--source-pacing",
                "--explicit-source-time",
            )
        )
    else:
        command.extend(("--output", str(path)))
    return command


def parse_source_timeline(output: str) -> dict[str, object]:
    events: list[dict[str, object]] = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(event, dict) and event.get("event") == "source_timeline":
            events.append(event)
    if len(events) != 1:
        raise RuntimeError(
            f"sender reported {len(events)} explicit source timelines"
        )
    if events[0].get("bytes_per_second") != BITRATE_BITS_PER_SECOND // 8:
        raise RuntimeError("sender source-time rate differs from the profile")
    return events[0]


def validate_fault_observation(
    scenario: Scenario,
    relay: CallerListenerFaultProxy,
) -> dict[str, object]:
    if relay.error() is not None:
        raise RuntimeError(f"fault relay failed: {relay.error()}")
    observations = relay.fault_observations()
    if len(observations) != 1:
        raise RuntimeError(
            f"{scenario.name}: observed {len(observations)} DATA drops"
        )
    observation = observations[0]
    if (
        observation.get("suppress_retransmissions") is not True
        or observation.get("direction") != "sender_to_receiver"
        or observation.get("packet_kind") != "data"
    ):
        raise RuntimeError(
            f"{scenario.name}: persistent DATA drop was not observed"
        )
    suppressed = observation.get("suppressed_retransmissions")
    if type(suppressed) is not int or suppressed <= 0:
        raise RuntimeError(
            f"{scenario.name}: sender did not attempt a retransmission"
        )
    if scenario.tlpktdrop:
        if observation.get("cumulative_ack_observed") is not True:
            raise RuntimeError(
                f"{scenario.name}: receiver did not cumulatively ACK "
                "the permanently missing packet"
            )
        dropped = observation.get("sequence")
        acknowledged = observation.get("cumulative_ack_next_sequence")
        if type(dropped) is not int or type(acknowledged) is not int:
            raise RuntimeError(f"{scenario.name}: ACK sequence evidence is absent")
        distance = (acknowledged - dropped) & SEQUENCE_MASK
        if not 0 < distance < ((SEQUENCE_MASK + 1) // 2):
            raise RuntimeError(
                f"{scenario.name}: cumulative ACK does not cross the drop"
            )
    elif observation.get("cumulative_ack_observed") is True:
        raise RuntimeError(
            f"{scenario.name}: disabled TLPKTDROP advanced across the gap"
        )
    return observation


def diagnostic(
    scenario: Scenario,
    sender_stdout: str,
    sender_stderr: str,
    receiver_stdout: str,
    receiver_stderr: str,
    relay: CallerListenerFaultProxy,
) -> str:
    return (
        f"{scenario.name}\n"
        f"--- sender stdout ---\n{sender_stdout or '<empty>'}\n"
        f"--- sender stderr ---\n{sender_stderr or '<empty>'}\n"
        f"--- receiver stdout ---\n{receiver_stdout or '<empty>'}\n"
        f"--- receiver stderr ---\n{receiver_stderr or '<empty>'}\n"
        f"--- relay trace ---\n{relay.render(scenario.name)}"
    )


def run_scenario(scenario: Scenario, directory: Path) -> None:
    messages = generated_messages(scenario.profile)
    message_size = len(messages[0].payload)
    input_path = directory / f"{scenario.name}.input"
    expected_path = directory / f"{scenario.name}.expected"
    output_path = directory / f"{scenario.name}.output"
    write_messages(input_path, messages)
    write_messages(expected_path, messages, 1)
    listener_port = free_udp_port()
    fault = RendezvousFault(
        action="drop",
        direction="sender_to_receiver",
        occurrence=1,
        suppress_retransmissions=True,
    )
    relay: CallerListenerFaultProxy = (
        CallerListenerSequenceProxy(
            listener_port, SEQUENCE_MASK - 1, fault
        )
        if scenario.rollover
        else CallerListenerFaultProxy(listener_port, fault)
    )
    receiver_bytes = sum(len(message.payload) for message in messages[1:])
    receiver = subprocess.Popen(
        peer_command(
            scenario.receiver,
            "listener",
            listener_port,
            output_path,
            receiver_bytes,
            message_size,
            tlpktdrop=scenario.tlpktdrop,
        ),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    sender: subprocess.Popen[str] | None = None
    sender_stdout = ""
    sender_stderr = ""
    receiver_stdout = ""
    receiver_stderr = ""
    try:
        time.sleep(0.2)
        if receiver.poll() is not None:
            receiver_stdout, receiver_stderr = receiver.communicate()
            raise RuntimeError("receiver exited before relay startup")
        relay.start()
        sender = subprocess.Popen(
            peer_command(
                scenario.sender,
                "caller",
                relay.port,
                input_path,
                input_path.stat().st_size,
                message_size,
                tlpktdrop=scenario.tlpktdrop,
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        timeout_seconds = (
            TIMEOUT_MILLISECONDS / 1_000 + 5
            if scenario.tlpktdrop
            else DISABLED_TIMEOUT_MILLISECONDS / 1_000 + 5
        )
        sender_stdout, sender_stderr = sender.communicate(
            timeout=timeout_seconds
        )
        receiver_stdout, receiver_stderr = receiver.communicate(
            timeout=timeout_seconds
        )
    except (subprocess.TimeoutExpired, RuntimeError) as error:
        for process in (sender, receiver):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
        raise RuntimeError(
            f"{error}\n"
            + diagnostic(
                scenario,
                sender_stdout,
                sender_stderr,
                receiver_stdout,
                receiver_stderr,
                relay,
            )
        ) from error
    finally:
        relay.close()

    details = diagnostic(
        scenario,
        sender_stdout,
        sender_stderr,
        receiver_stdout,
        receiver_stderr,
        relay,
    )
    try:
        observation = validate_fault_observation(scenario, relay)
    except RuntimeError as error:
        raise RuntimeError(f"{error}\n{details}") from error
    if scenario.tlpktdrop:
        if sender.returncode != 0 or receiver.returncode != 0:
            raise RuntimeError("TLPKTDROP transfer failed\n" + details)
        sender_complete = parse_complete(sender_stdout, "caller")
        receiver_complete = parse_complete(receiver_stdout, "listener")
        parse_source_timeline(sender_stdout)
        if (
            sender_complete.get("bytes") != input_path.stat().st_size
            or receiver_complete.get("bytes") != receiver_bytes
            or file_sha256(output_path) != file_sha256(expected_path)
        ):
            raise RuntimeError("TLPKTDROP payload mismatch\n" + details)
        receiver_drops = nonnegative_statistic(
            receiver_complete, "pktRcvDropTotal"
        )
        sender_retransmissions = nonnegative_statistic(
            sender_complete, "pktRetransTotal"
        )
        if receiver_drops is None or receiver_drops <= 0:
            raise RuntimeError("receiver drop statistics are absent\n" + details)
        if sender_retransmissions is None or sender_retransmissions <= 0:
            raise RuntimeError(
                "sender retransmission statistics are absent\n" + details
            )
        elapsed = receiver_complete.get("elapsed_us")
        if type(elapsed) is not int or elapsed < LATENCY_MILLISECONDS * 700:
            raise RuntimeError("receiver bypassed the TSBPD deadline\n" + details)
        print(
            f"PASS {scenario.name} profile={scenario.profile} "
            f"dropped_sequence={observation['sequence']} "
            f"fake_ack_next={observation['cumulative_ack_next_sequence']} "
            f"suppressed_retransmissions="
            f"{observation['suppressed_retransmissions']}"
        )
        return

    if sender.returncode == 0 or receiver.returncode == 0:
        raise RuntimeError("disabled TLPKTDROP unexpectedly completed\n" + details)
    if output_path.exists() and output_path.stat().st_size != 0:
        raise RuntimeError("disabled TLPKTDROP released later data\n" + details)
    print(
        f"PASS {scenario.name} preserved_gap=true "
        f"suppressed_retransmissions="
        f"{observation['suppressed_retransmissions']}"
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        robotweax_peer = resolve_program_path(arguments.robotweax_peer)
        reference_peer = resolve_program_path(arguments.reference_peer)
        with tempfile.TemporaryDirectory(
            prefix="robotweax-tlpktdrop-interop-"
        ) as temporary:
            directory = Path(temporary)
            for scenario in scenario_matrix(
                robotweax_peer, reference_peer
            ):
                run_scenario(scenario, directory)
    except (OSError, RuntimeError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
