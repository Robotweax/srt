#!/usr/bin/env python3
"""Run IPv4 multicast dispatch cases; missing or failed cases fail, and only a
recorded host-environment skip of a non-required case is tolerated."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    "group_membership_admits_multicast_dispatch_only_by_policy",
    "multicast_dispatch_round_trips_ipv4_and_ipv6_announcements",
    "multicast_dispatch_encoder_rejects_invalid_announcements",
    "multicast_dispatch_decoder_rejects_malformed_wire_content",
    "hsv5_multicast_dispatch_negotiation_announces_the_group",
    "hsv5_multicast_dispatch_is_rejected_without_policy_or_announcement",
    "compat_multicast_dispatch_group_creation_and_receiver_shape",
    "compat_multicast_dispatch_sender_epoll_tracks_empty_and_backpressure",
    "compat_multicast_dispatch_backpressure_preserves_original_and_tail",
    "compat_multicast_dispatch_lost_or_failed_repair_uses_unicast_within_budget",
    "multicast_dispatch_repair_does_not_suppress_before_submission",
    "multicast_dispatch_repair_repeated_nak_uses_unicast",
    "multicast_dispatch_repair_completion_cannot_mutate_a_new_tally",
    "multicast_dispatch_repair_member_capacity_preserves_unicast_fallback",
    "multicast_dispatch_repair_threshold_counts_distinct_receivers_in_window",
    "multicast_dispatch_group_repeats_a_shared_loss_once_over_multicast",
    "multicast_dispatch_group_recovers_when_original_and_multicast_repair_are_lost",
    "multicast_dispatch_group_delivers_one_stream_to_several_receivers",
    "multicast_dispatch_group_authenticates_with_aes_gcm_group_key",
    "multicast_dispatch_group_encrypts_with_one_group_key_across_rotations",
    "multicast_dispatch_listener_rejects_joins_without_key_or_as_bonding",
    "multicast_dispatch_group_joins_with_a_full_accept_backlog",
}
# Cases whose precondition is a host property that hosted runners may lack.
# A skip is accepted only for these names with exactly this reason, is never
# accepted for a REQUIRED case, and is recorded in the evidence, not hidden.
ENVIRONMENT_SKIPS = {
    "multicast_dispatch_independent_source_ip_distributes_and_rejoins":
        "no second assigned IPv4 address available",
}
SKIP_LINE = re.compile(r"^\[SKIP\] (\w+): (.*)$", re.MULTILINE)
KEY_REQUIRED = {
    "crypto_shared_schedule_maps_group_sequence_to_generation_and_selector",
    "crypto_shared_schedule_keeps_one_key_per_sequence_across_wraparound",
    "crypto_shared_schedule_advances_unused_rotations_and_wraps",
    "crypto_shared_schedule_unused_advance_failure_preserves_keys_and_position",
}


def environment_skips(output: str, tolerated=None) -> dict[str, str]:
    """Return tolerated host-environment skips; any other skip is an error."""
    tolerated = tolerated or {}
    skips = dict(SKIP_LINE.findall(output))
    if len(re.findall(r"^\[SKIP\]", output, re.MULTILINE)) != len(skips):
        raise ValueError("malformed multicast skip report")
    for name, reason in skips.items():
        if tolerated.get(name) != reason:
            raise ValueError(f"multicast case skipped without a recorded host limitation: {name}: {reason}")
    return skips


def validate_native(output: str, required=REQUIRED, tolerated=None) -> list[str]:
    if re.search(r"\[FAIL\]", output):
        raise ValueError("multicast case failed")
    skipped = environment_skips(output, tolerated)
    if required & set(skipped):
        raise ValueError("required multicast case skipped: " + ", ".join(sorted(required & set(skipped))))
    passed = re.findall(r"^\[PASS\] (\w+)$", output, re.MULTILINE)
    started = re.findall(r"^\[RUN\] (\w+)$", output, re.MULTILINE)
    summaries = re.findall(r"^(\d+)/(\d+) tests passed(?: \((\d+) skipped\))?$", output, re.MULTILINE)
    if len(summaries) != 1:
        raise ValueError("missing or ambiguous native summary")
    good, selected, skip_count = summaries[0]
    if (int(skip_count or 0) != len(skipped) or int(good) + len(skipped) != int(selected)
            or int(good) != len(passed) or len(set(passed)) != len(passed)
            or sorted(started) != sorted(passed + list(skipped))):
        raise ValueError("incomplete or skipped native execution")
    if not required.issubset(passed):
        raise ValueError("missing required native cases: " + ", ".join(sorted(required - set(passed))))
    return passed


def binary(build: Path, directory: str, name: str, config: str) -> Path:
    suffix = ".exe" if os.name == "nt" else ""
    for parent in [build / directory / config, build / directory]:
        candidate = parent / (name + suffix)
        if candidate.is_file():
            return candidate.resolve()
    raise FileNotFoundError(name)


def execute(command: list[str], output: Path, timeout: int) -> str:
    with output.open("w", encoding="utf-8") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
    text = output.read_text(encoding="utf-8", errors="replace")
    if result.returncode:
        raise RuntimeError(f"exit {result.returncode}: {command}; see {output}")
    return text


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--backend", choices=["openssl", "mbedtls", "bcrypt"], required=True)
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    record = dict(status="failed", backend=args.backend, platform=platform.platform(),
                  python=platform.python_version(), configuration=args.config,
                  scope="IPv4 loopback dispatch and public MPEG-TS sample; no IPv6 dispatch or physical-network claim; "
                        "environment_skips lists host-limited non-required cases that did not run",
                  commit=os.environ.get("GITHUB_SHA") or subprocess.check_output(
                      ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip())
    try:
        cache = (args.build / "CMakeCache.txt").read_text()
        for expected in [f"ROBOTWEAX_SRT_CRYPTO_BACKEND:STRING={args.backend}",
                         "ENABLE_AEAD_API_PREVIEW:BOOL=ON"]:
            if expected not in cache.splitlines():
                raise ValueError("required build configuration missing: " + expected)
        native = binary(args.build, "", "robotweax_srt_tests", args.config)
        sample = binary(args.build, "examples/applications", "robotweax_srt_multicast", args.config)
        output = execute([str(native), "--include", "multicast_dispatch"], args.output / "native.log", 120)
        record["native_cases"] = validate_native(output, tolerated=ENVIRONMENT_SKIPS)
        record["environment_skips"] = environment_skips(output, ENVIRONMENT_SKIPS)
        output = execute([str(native), "--include", "crypto_shared_schedule"],
                         args.output / "key-schedule.log", 30)
        record["key_cases"] = validate_native(output, KEY_REQUIRED)
        record["sample_modes"] = []
        for mode in ["plain", "ctr", "gcm"]:
            output = execute([sys.executable, "-u", str(ROOT / "examples/applications/test_multicast.py"),
                              "--binary", str(sample), "--crypto", mode,
                              "--logs", str(args.output / f"sample-{mode}")],
                             args.output / f"sample-{mode}.log", 45)
            if not re.search(rf"^PASS {mode}: \d+ bytes, \d+ multicast originals, two exact receivers, datagram integrity$",
                             output, re.MULTILINE):
                raise ValueError("missing sample completion: " + mode)
            record["sample_modes"].append(mode)
        record["status"] = "passed"
    finally:
        (args.output / "qualification.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()
