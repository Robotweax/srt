#!/usr/bin/env python3
"""Pinned AES-GCM preview interoperability for the smallest 0.2 profile."""

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
from run_encrypted_interop import (
    PASSPHRASE_ENVIRONMENT,
    RunOptions,
    Scenario,
    no_rotation_baselines,
    run_no_rotation_baselines,
)
from run_rendezvous_interop import (
    LOSS_RECOVERY_LATENCY_MILLISECONDS,
    RunOptions as RendezvousRunOptions,
    Scenario as RendezvousScenario,
    run_scenario as run_rendezvous_scenario,
)
from srt_handshake_trace import RendezvousFault


AEAD_IPV4_CHUNK_SIZE = 1_200
AEAD_IPV6_CHUNK_SIZE = 1_420


def aead_baselines(
    robotweax: Path,
    reference: Path,
    options: RunOptions,
    name_prefix: str = "",
) -> tuple[list[Scenario], RunOptions, int]:
    return no_rotation_baselines(
        robotweax,
        reference,
        options,
        name_prefix=f"{name_prefix}gcm-",
    )


def aead_rendezvous_matrix(
    robotweax: Path,
    reference: Path,
    key_refresh_rate: int,
    name_prefix: str = "",
) -> list[RendezvousScenario]:
    common = {
        "key_length": 16,
        "trace_roles": True,
        "robotweax_peer": robotweax,
        "crypto_mode": "gcm",
        "expected_crypto_mode": "gcm",
        "minimum_data_key_transitions": 2,
    }
    return [
        RendezvousScenario(
            name=(
                f"{name_prefix}gcm-rendezvous-"
                "robotweax-to-haivision-sender-first"
            ),
            sender=robotweax,
            receiver=reference,
            seed=71_001,
            start_order="sender-first",
            start_delay_milliseconds=200,
            **common,
        ),
        RendezvousScenario(
            name=(
                f"{name_prefix}gcm-rendezvous-"
                "robotweax-to-haivision-receiver-first"
            ),
            sender=robotweax,
            receiver=reference,
            seed=71_002,
            start_order="receiver-first",
            start_delay_milliseconds=200,
            **common,
        ),
        RendezvousScenario(
            name=(
                f"{name_prefix}gcm-rendezvous-"
                "haivision-to-robotweax-sender-first"
            ),
            sender=reference,
            receiver=robotweax,
            seed=72_001,
            start_order="sender-first",
            start_delay_milliseconds=200,
            **common,
        ),
        RendezvousScenario(
            name=(
                f"{name_prefix}gcm-rendezvous-"
                "haivision-to-robotweax-receiver-first"
            ),
            sender=reference,
            receiver=robotweax,
            seed=72_002,
            start_order="receiver-first",
            start_delay_milliseconds=200,
            **common,
        ),
        RendezvousScenario(
            name=f"{name_prefix}gcm-rendezvous-drop-robotweax-to-haivision",
            sender=robotweax,
            receiver=reference,
            seed=73_001,
            latency_milliseconds=LOSS_RECOVERY_LATENCY_MILLISECONDS,
            faults=(
                RendezvousFault(
                    action="drop",
                    direction="sender_to_receiver",
                    occurrence=key_refresh_rate + 1,
                ),
            ),
            **common,
        ),
        RendezvousScenario(
            name=f"{name_prefix}gcm-rendezvous-duplicate-haivision-to-robotweax",
            sender=reference,
            receiver=robotweax,
            seed=73_002,
            faults=(
                RendezvousFault(
                    action="duplicate",
                    direction="sender_to_receiver",
                    occurrence=key_refresh_rate * 2 - 1,
                ),
            ),
            **common,
        ),
    ]


def aead_rendezvous_role_probe(
    robotweax: Path,
    reference: Path,
    attempt: int,
    name_prefix: str = "",
) -> RendezvousScenario:
    combination = attempt % 4
    robotweax_sends = combination < 2
    return RendezvousScenario(
        name=(
            f"{name_prefix}gcm-rendezvous-cookie-role-probe-{attempt + 1}"
        ),
        sender=robotweax if robotweax_sends else reference,
        receiver=reference if robotweax_sends else robotweax,
        key_length=16,
        seed=74_000 + attempt,
        start_order=(
            "sender-first" if combination % 2 == 0 else "receiver-first"
        ),
        start_delay_milliseconds=200,
        trace_roles=True,
        robotweax_peer=robotweax,
        crypto_mode="gcm",
        expected_crypto_mode="gcm",
        minimum_data_key_transitions=2,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument("--km-refresh-rate", type=int, default=256)
    parser.add_argument("--km-preannounce", type=int, default=64)
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
        parser.error("invalid transfer or key-rotation parameters")

    try:
        chunk_size = (
            AEAD_IPV6_CHUNK_SIZE
            if address.version == 6
            else AEAD_IPV4_CHUNK_SIZE
        )
        name_prefix = "ipv6-" if address.version == 6 else ""
        robotweax = resolve_program_path(arguments.robotweax_peer)
        reference = resolve_program_path(arguments.reference_peer)
        options = RunOptions(
            byte_count=1,
            timeout_seconds=arguments.timeout_seconds,
            key_refresh_rate=arguments.km_refresh_rate,
            key_preannouncement=arguments.km_preannounce,
            chunk_size=chunk_size,
            crypto_mode="gcm",
            expected_crypto_mode="gcm",
            maximum_payload_size=chunk_size,
            expected_maximum_payload_size=chunk_size,
        )
        scenarios, baseline, minimum_packets = aead_baselines(
            robotweax, reference, options, name_prefix
        )
        environment = os.environ.copy()
        environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)
        with tempfile.TemporaryDirectory(
            prefix="robotweax-srt-aead-interop-"
        ) as directory:
            work = Path(directory)
            run_no_rotation_baselines(
                scenarios,
                baseline,
                work,
                environment,
                minimum_packets,
                host=arguments.host,
            )
            rendezvous_options = RendezvousRunOptions(
                byte_count=(
                    arguments.km_refresh_rate
                    * 3
                    * chunk_size
                ),
                timeout_seconds=arguments.timeout_seconds,
                key_refresh_rate=arguments.km_refresh_rate,
                key_preannouncement=arguments.km_preannounce,
                chunk_size=chunk_size,
                maximum_payload_size=chunk_size,
                expected_maximum_payload_size=chunk_size,
            )
            roles: set[str] = set()
            for scenario in aead_rendezvous_matrix(
                robotweax,
                reference,
                arguments.km_refresh_rate,
                name_prefix,
            ):
                role = run_rendezvous_scenario(
                    scenario,
                    rendezvous_options,
                    work,
                    environment,
                    host=arguments.host,
                )
                if role is not None:
                    roles.add(role)
            required_roles = {"initiator", "responder"}
            for attempt in range(16):
                if roles == required_roles:
                    break
                role = run_rendezvous_scenario(
                    aead_rendezvous_role_probe(
                        robotweax,
                        reference,
                        attempt,
                        name_prefix,
                    ),
                    rendezvous_options,
                    work,
                    environment,
                    host=arguments.host,
                )
                if role is not None:
                    roles.add(role)
            if roles != required_roles:
                missing = sorted(required_roles - roles)
                raise RuntimeError(
                    "AES-GCM Robotweax rendezvous cookie-role coverage is "
                    "incomplete; missing: " + ", ".join(missing)
                )
            print(
                "PASS gcm-rendezvous-cookie-role-coverage "
                "roles=initiator,responder",
                flush=True,
            )
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
