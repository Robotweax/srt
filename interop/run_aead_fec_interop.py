#!/usr/bin/env python3
"""Pinned AES-GCM FEC interoperability for Robotweax SRT 0.2."""

from __future__ import annotations

import argparse
import os
import secrets
import sys
import tempfile
from dataclasses import replace
from pathlib import Path

from interop_common import resolve_program_path
from run_fec_interop import (
    ENCRYPTED_FEC_GROUPS,
    ENCRYPTED_FEC_KEY_PREANNOUNCEMENT,
    ENCRYPTED_FEC_KEY_REFRESH_RATE,
    ENCRYPTED_FEC_MINIMUM_KEY_TRANSITIONS,
    PASSPHRASE_ENVIRONMENT,
    RunOptions,
    Scenario,
    run_scenario,
    scenario_matrix,
)


def aead_fec_scenarios(
    robotweax: Path,
    reference: Path,
) -> list[Scenario]:
    """Return the positive matrix allowed by the pinned reference.

    Haivision's selected AEAD revision reconstructs an FEC DATA header with a
    constant message number of one. That authenticates only the first source
    message. The Robotweax-to-reference case therefore proves protected-byte
    generation with a first-source Row loss. All later-message and recursive
    recovery cases deliberately use Robotweax as receiver, where the exact
    message number is inferred from a surviving source before GCM open.
    """

    clear = scenario_matrix(robotweax, reference)
    selected = [
        replace(
            clear[0],
            name="gcm-row-fec-robotweax-to-haivision-first-source-drop",
            fault_occurrences=(1,),
            expected_sequence_offsets=(0,),
        ),
        replace(
            clear[1],
            name="gcm-row-fec-haivision-to-robotweax-source-drop",
        ),
        replace(
            clear[3],
            name="gcm-column-fec-haivision-to-robotweax-source-drop",
        ),
        replace(
            clear[5],
            name="gcm-matrix-fec-haivision-to-robotweax-recursive-drop",
        ),
    ]
    return [
        replace(
            scenario,
            geometry=f"gcm-{scenario.geometry}",
            byte_count_multiplier=ENCRYPTED_FEC_GROUPS,
            key_length=16,
            minimum_key_transitions=(
                ENCRYPTED_FEC_MINIMUM_KEY_TRANSITIONS
            ),
            crypto_mode="gcm",
        )
        for scenario in selected
    ]


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=int, default=30)
    parser.add_argument(
        "--km-refresh-rate",
        type=int,
        default=ENCRYPTED_FEC_KEY_REFRESH_RATE,
    )
    parser.add_argument(
        "--km-preannounce",
        type=int,
        default=ENCRYPTED_FEC_KEY_PREANNOUNCEMENT,
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    if (
        arguments.timeout_seconds <= 0
        or arguments.km_refresh_rate <= 0
        or arguments.km_preannounce <= 0
        or arguments.km_preannounce
        > (arguments.km_refresh_rate - 1) // 2
    ):
        raise SystemExit("invalid timeout or key-rotation parameters")

    robotweax = resolve_program_path(arguments.robotweax_peer)
    reference = resolve_program_path(arguments.reference_peer)
    options = RunOptions(
        timeout_seconds=arguments.timeout_seconds,
        key_refresh_rate=arguments.km_refresh_rate,
        key_preannouncement=arguments.km_preannounce,
    )
    environment = os.environ.copy()
    environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)
    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="robotweax-srt-aead-fec-interop-"
    ) as temporary_directory:
        directory = Path(temporary_directory)
        for scenario in aead_fec_scenarios(robotweax, reference):
            try:
                run_scenario(scenario, options, directory, environment)
            except (OSError, RuntimeError) as error:
                failures.append(str(error))

    if failures:
        print(
            "AES-GCM FEC interoperability failures:\n\n"
            + "\n\n".join(failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
