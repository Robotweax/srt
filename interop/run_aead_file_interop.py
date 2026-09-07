#!/usr/bin/env python3
"""AES-GCM File/Stream gates for the Robotweax SRT 0.2 preview."""

from __future__ import annotations

import argparse
import ipaddress
import os
import secrets
import subprocess
import sys
import tempfile
from pathlib import Path

from interop_common import resolve_program_path
from run_file_interop import (
    PASSPHRASE_ENVIRONMENT,
    RendezvousScenario,
    RunOptions,
    Scenario,
    peer_command,
    run_rendezvous_scenario,
    run_scenario,
)
from srt_handshake_trace import RendezvousFault


GCM_FILE_IPV4_PAYLOAD_SIZE = 1_440
GCM_FILE_IPV6_PAYLOAD_SIZE = 1_420
REFERENCE_FILE_GCM_DIAGNOSTICS = (
    "Enable TSBPD to use AES GCM.",
    "setsockflag 62 failed",
)


def caller_listener_matrix(
    robotweax: Path,
    byte_count: int,
    key_refresh_rate: int,
    key_preannouncement: int,
    payload_size: int,
    name_prefix: str = "",
) -> list[Scenario]:
    scenarios: list[Scenario] = []
    profiles = (
        ("robotweax-large-write", 81_000, 32_768, 997),
        ("robotweax-small-write", 82_000, 4_093, 701),
    )
    for label, seed, sender_size, receiver_size in profiles:
        common = {
            "caller": robotweax,
            "listener": robotweax,
            "sender_chunk_size": sender_size,
            "receiver_size": receiver_size,
            "byte_count": byte_count,
            "key_length": 16,
            "key_refresh_rate": key_refresh_rate,
            "key_preannouncement": key_preannouncement,
            "minimum_key_transitions": 2,
            "crypto_mode": "gcm",
            "expected_crypto_mode": "gcm",
            "maximum_payload_size": payload_size,
            "expected_maximum_payload_size": payload_size,
        }
        scenarios.extend(
            (
                Scenario(
                    name=f"{name_prefix}gcm-file-stream-{label}",
                    seed=seed,
                    expect_eof=True,
                    **common,
                ),
                Scenario(
                    name=f"{name_prefix}gcm-file-helper-{label}",
                    seed=seed + 10,
                    file_api=True,
                    file_size_to_eof=True,
                    **common,
                ),
                Scenario(
                    name=f"{name_prefix}gcm-file-stream-{label}-nak-drop",
                    seed=seed + 20,
                    expect_eof=True,
                    fault=RendezvousFault(
                        action="drop",
                        direction="sender_to_receiver",
                        occurrence=key_refresh_rate + 17,
                    ),
                    recovery="NAK",
                    **common,
                ),
            )
        )
    return scenarios


def rendezvous_matrix(
    robotweax: Path,
    byte_count: int,
    key_refresh_rate: int,
    key_preannouncement: int,
    payload_size: int,
    name_prefix: str = "",
) -> list[RendezvousScenario]:
    common = {
        "robotweax_peer": robotweax,
        "sender_chunk_size": 4_093,
        "receiver_size": 701,
        "byte_count": byte_count,
        "key_length": 16,
        "key_refresh_rate": key_refresh_rate,
        "key_preannouncement": key_preannouncement,
        "minimum_key_transitions": 2,
        "expect_eof": True,
        "fault": RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=key_refresh_rate + 17,
        ),
        "recovery": "NAK",
        "crypto_mode": "gcm",
        "expected_crypto_mode": "gcm",
        "maximum_payload_size": payload_size,
        "expected_maximum_payload_size": payload_size,
    }
    return [
        RendezvousScenario(
            name=f"{name_prefix}gcm-file-rendezvous-sender-first",
            sender=robotweax,
            receiver=robotweax,
            seed=83_001,
            start_order="sender-first",
            start_delay_milliseconds=200,
            **common,
        ),
        RendezvousScenario(
            name=f"{name_prefix}gcm-file-rendezvous-receiver-first",
            sender=robotweax,
            receiver=robotweax,
            seed=83_002,
            start_order="receiver-first",
            start_delay_milliseconds=200,
            **common,
        ),
    ]


def reference_file_gcm_rejection_command(
    reference: Path,
    host: str,
    payload_path: Path,
    payload_size: int,
    key_refresh_rate: int,
    key_preannouncement: int,
) -> list[str]:
    scenario = Scenario(
        name="pinned-haivision-file-gcm-boundary",
        caller=reference,
        listener=reference,
        seed=84_000,
        sender_chunk_size=1,
        receiver_size=1,
        byte_count=1,
        key_length=16,
        key_refresh_rate=key_refresh_rate,
        key_preannouncement=key_preannouncement,
        minimum_key_transitions=1,
        crypto_mode="gcm",
        expected_crypto_mode="gcm",
        maximum_payload_size=payload_size,
        expected_maximum_payload_size=payload_size,
    )
    options = RunOptions(
        byte_count=1,
        timeout_seconds=1,
        maximum_bandwidth_bytes_per_second=1_000_000,
        shutdown_grace_milliseconds=0,
        host=host,
    )
    return peer_command(
        reference,
        "caller",
        9,
        payload_path,
        scenario,
        options,
    )


def verify_reference_file_gcm_boundary(
    reference: Path,
    host: str,
    work: Path,
    environment: dict[str, str],
    payload_size: int,
    key_refresh_rate: int,
    key_preannouncement: int,
) -> None:
    payload_path = work / "reference-file-gcm-boundary.bin"
    payload_path.write_bytes(b"\0")
    command = reference_file_gcm_rejection_command(
        reference,
        host,
        payload_path,
        payload_size,
        key_refresh_rate,
        key_preannouncement,
    )
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            check=False,
            env=environment,
            text=True,
            timeout=5,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(
            "pinned Haivision File/Stream AES-GCM capability probe "
            "did not fail during option setup"
        ) from error
    if completed.returncode == 0 or any(
        diagnostic not in completed.stderr
        for diagnostic in REFERENCE_FILE_GCM_DIAGNOSTICS
    ):
        raise RuntimeError(
            "pinned Haivision File/Stream AES-GCM capability boundary "
            "changed unexpectedly: "
            f"returncode={completed.returncode}, "
            f"stderr={completed.stderr!r}"
        )
    print(
        "PASS pinned-haivision-file-gcm-boundary "
        "reason=requires-tsbpd",
        flush=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=45)
    parser.add_argument("--km-refresh-rate", type=int, default=128)
    parser.add_argument("--km-preannounce", type=int, default=32)
    parser.add_argument("--host", default="127.0.0.1")
    arguments = parser.parse_args()
    try:
        address = ipaddress.ip_address(arguments.host)
    except ValueError as error:
        parser.error(str(error))
    if (
        arguments.timeout_seconds <= 0
        or arguments.km_refresh_rate < 2
        or arguments.km_preannounce <= 0
        or arguments.km_preannounce
        > (arguments.km_refresh_rate - 1) // 2
    ):
        parser.error("invalid AES-GCM File/Stream parameters")

    try:
        robotweax = resolve_program_path(arguments.robotweax_peer)
        reference = resolve_program_path(arguments.reference_peer)
        payload_size = (
            GCM_FILE_IPV6_PAYLOAD_SIZE
            if address.version == 6
            else GCM_FILE_IPV4_PAYLOAD_SIZE
        )
        name_prefix = "ipv6-" if address.version == 6 else ""
        byte_count = (
            arguments.km_refresh_rate * 3 * payload_size + 379
        )
        options = RunOptions(
            byte_count=byte_count,
            timeout_seconds=arguments.timeout_seconds,
            maximum_bandwidth_bytes_per_second=1_000_000,
            shutdown_grace_milliseconds=250,
            host=arguments.host,
        )
        environment = os.environ.copy()
        environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)
        failures: list[str] = []
        with tempfile.TemporaryDirectory(
            prefix="robotweax-srt-aead-file-interop-"
        ) as directory:
            work = Path(directory)
            verify_reference_file_gcm_boundary(
                reference,
                arguments.host,
                work,
                environment,
                payload_size,
                arguments.km_refresh_rate,
                arguments.km_preannounce,
            )
            for scenario in caller_listener_matrix(
                robotweax,
                byte_count,
                arguments.km_refresh_rate,
                arguments.km_preannounce,
                payload_size,
                name_prefix,
            ):
                try:
                    run_scenario(scenario, options, work, environment)
                except (
                    OSError,
                    RuntimeError,
                    subprocess.TimeoutExpired,
                ) as error:
                    failures.append(str(error))
            for scenario in rendezvous_matrix(
                robotweax,
                byte_count,
                arguments.km_refresh_rate,
                arguments.km_preannounce,
                payload_size,
                name_prefix,
            ):
                try:
                    run_rendezvous_scenario(
                        scenario,
                        options,
                        work,
                        host=arguments.host,
                        environment=environment,
                    )
                except (
                    OSError,
                    RuntimeError,
                    subprocess.TimeoutExpired,
                ) as error:
                    failures.append(str(error))
        if failures:
            raise RuntimeError(
                "AES-GCM File/Stream interoperability failures:\n\n"
                + "\n\n".join(failures)
            )
    except (OSError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
