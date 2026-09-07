#!/usr/bin/env python3
"""Bounded serial group diagnosis with retained failures, never retry-to-green."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import time

import run_group_interop as group
from interop_common import file_sha256, resolve_program_path


CASE_TIMEOUT_SECONDS = 40
SERIES_BUDGET_SECONDS = 600
SCENARIOS = {
    "group-receive-contract": ("broadcast", "reference-receive-contract"),
    "group-backup-baseline": ("backup", "baseline"),
}
DEFAULT_SCENARIO = "group-receive-contract"
REFERENCE_COMMIT = "899348d8318eb9a3c5a5b6ec43c4a1114288773a"


def invoke(command: list[str], directory: Path,
           timeout: int = CASE_TIMEOUT_SECONDS,
           environment: dict[str, str] | None = None) -> int:
    with (directory / "stdout.log").open("w") as stdout, \
            (directory / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                   start_new_session=True, env=environment)
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124
        finally:
            # Reap every worker's peers before starting the next measurement,
            # even if an unexpected exception bypassed harness cleanup.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()


def run_series(robotweax: Path, reference: Path, output: Path,
               repetitions: int = 60, scenario: str = DEFAULT_SCENARIO) -> int:
    if not 1 <= repetitions <= 60:
        raise ValueError("repetitions must be 1..60")
    policy, profile = SCENARIOS[scenario]
    if os.name != "posix":
        raise RuntimeError("diagnostics require POSIX process-group cleanup")
    robotweax = resolve_program_path(robotweax)
    reference = resolve_program_path(reference)
    if not all(os.access(p, os.X_OK) for p in (robotweax, reference)):
        raise RuntimeError("both freshly built peers must be executable")
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError("output directory must be empty; refusing stale evidence")
    (output / "environment.json").write_text(json.dumps({
        "platform": platform.platform(), "python": sys.version,
        "robotweax_revision": os.environ.get("GITHUB_SHA"),
        "reference_commit": REFERENCE_COMMIT,
        "robotweax_peer_sha256": file_sha256(robotweax),
        "reference_peer_sha256": file_sha256(reference),
        "scenario": scenario, "profile": profile, "policy": policy,
        "direction": "Robotweax caller -> Haivision listener",
        "requested_repetitions": repetitions,
        "case_timeout_seconds": CASE_TIMEOUT_SECONDS,
        "series_budget_seconds": SERIES_BUDGET_SECONDS,
    }, indent=2) + "\n", encoding="utf-8")
    started = time.monotonic()
    records = []
    summary = {"requested": repetitions, "completed": 0, "passed": False,
               "reason": "incomplete", "runs": records}

    def persist() -> None:
        (output / "summary.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")

    persist()
    for index in range(repetitions):
        if time.monotonic() - started + CASE_TIMEOUT_SECONDS > SERIES_BUDGET_SECONDS:
            summary["reason"] = "series-budget-exhausted"
            break
        directory = output / f"case-{index + 1:03d}"
        directory.mkdir()
        command = [sys.executable, str(Path(__file__).resolve()),
                   "--robotweax-peer", str(robotweax),
                   "--reference-peer", str(reference),
                   "--output-directory", str(directory / "peers"),
                   "--scenario", scenario, "--single-case"]
        summary["active_case"] = index + 1
        persist()
        case_started = time.monotonic()
        try:
            code = invoke(command, directory)
            record = {"index": index + 1, "exit_code": code,
                      "passed": code == 0}
            if code == 0 and not all((directory / "peers" / name).is_file()
                                    for name in ("case.json", "caller.stdout",
                                                 "caller.stderr", "listener.stdout",
                                                 "listener.stderr")):
                record.update(passed=False, error="missing-peer-evidence")
        except OSError as error:
            record = {"index": index + 1, "exit_code": None,
                      "passed": False, "error": str(error)}
        record["elapsed_seconds"] = time.monotonic() - case_started
        records.append(record)
        summary["completed"] = len(records)
        summary["active_case"] = None
        with (output / "runs.jsonl").open("a", encoding="utf-8") as log:
            log.write(json.dumps(record) + "\n")
        persist()
        print(json.dumps(record), flush=True)
    else:
        summary["passed"] = all(r["passed"] for r in records)
        summary["reason"] = "passed" if summary["passed"] else "case-failure"
    persist()
    return 0 if summary["passed"] else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=60)
    parser.add_argument("--scenario", choices=SCENARIOS, default=DEFAULT_SCENARIO)
    parser.add_argument("--single-case", action="store_true", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.single_case:
        group.PINNED_SRT_VERSION = group.parse_srt_version("1.5.7")
        policy, profile = SCENARIOS[args.scenario]
        group.run_case(resolve_program_path(args.reference_peer),
                       resolve_program_path(args.robotweax_peer),
                       f"isolated {args.scenario}", policy, True,
                       profile=profile, evidence_directory=args.output_directory)
        return 0
    return run_series(args.robotweax_peer, args.reference_peer,
                      args.output_directory, args.repetitions, args.scenario)


if __name__ == "__main__":
    raise SystemExit(main())
