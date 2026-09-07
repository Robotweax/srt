#!/usr/bin/env python3
"""Robotweax AES-GCM Connection Group interoperability for AEAD-11."""

from __future__ import annotations

import argparse
import os
import secrets
import sys
from pathlib import Path

from interop_common import resolve_program_path
from run_group_interop import run_case


PASSPHRASE_ENVIRONMENT = "ROBOTWEAX_SRT_GROUP_PASSPHRASE"
CRYPTO_MODE_ENVIRONMENT = "ROBOTWEAX_SRT_GROUP_CRYPTO_MODE"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    peer = resolve_program_path(arguments.robotweax_peer)
    environment = os.environ.copy()
    environment[PASSPHRASE_ENVIRONMENT] = secrets.token_hex(24)
    environment[CRYPTO_MODE_ENVIRONMENT] = "gcm"
    scenarios = (
        ("AES-GCM Broadcast baseline", "broadcast", "baseline", False),
        ("AES-GCM Broadcast late join", "broadcast", "late-join", True),
        ("AES-GCM Backup baseline", "backup", "baseline", False),
    )
    failures: list[str] = []
    for name, policy, profile, repeated_update in scenarios:
        try:
            run_case(
                peer,
                peer,
                name,
                policy,
                True,
                profile=profile,
                require_repeated_listener_update=repeated_update,
                environment=environment,
            )
            print(f"PASS {name}")
        except (OSError, RuntimeError) as error:
            failures.append(str(error))
    if failures:
        print(
            "AES-GCM Connection Group interoperability failures:\n\n"
            + "\n\n".join(failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
