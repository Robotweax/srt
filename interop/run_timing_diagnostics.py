#!/usr/bin/env python3
"""Bounded serial timing evidence; failures remain failures, never retried."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import subprocess
import sys

import run_aead_timing_interop as aead
import run_live_timing_interop as timing


def schedule(repetitions: int, trace_repetitions: int) -> list[tuple[str, int]]:
    if not 1 <= repetitions <= 20 or not 0 <= trace_repetitions <= 5:
        raise ValueError("repetitions must be 1..20; trace repetitions 0..5")
    return [(mode, index) for index in range(repetitions)
            for mode in ("control", "phase")] + [
                ("strace", index) for index in range(trace_repetitions)]


def validate_receiver_evidence(text: str, phase_enabled: bool) -> None:
    events = [json.loads(line) for line in text.splitlines() if line.strip()]
    anchors = [e for e in events if e.get("event") in ("connected", "complete")]
    if len(anchors) != 2 or [e["event"] for e in anchors] != ["connected", "complete"]:
        raise RuntimeError("receiver clock anchors are incomplete")
    keys = ("phase_clock_monotonic_before_microseconds",
            "phase_clock_realtime_microseconds",
            "phase_clock_monotonic_after_microseconds")
    for event in anchors:
        if not phase_enabled:
            if any(key in event for key in keys):
                raise RuntimeError("control unexpectedly contains phase clock anchors")
            continue
        if any(type(event.get(key)) is not int or event[key] <= 0 for key in keys):
            raise RuntimeError("invalid receiver clock anchor")
        if event[keys[0]] > event[keys[2]]:
            raise RuntimeError("receiver clock anchor is reversed")
    messages = [e for e in events if e.get("event") == "timing"]
    if phase_enabled:
        timing.phase_observations(messages)
        if anchors[0][keys[2]] > anchors[1][keys[0]]:
            raise RuntimeError("receiver clock anchors are reversed")
    elif any("receive_start_microseconds" in e or "udp_send_start_microseconds" in e
             for e in messages):
        raise RuntimeError("control unexpectedly contains phase observations")


def invoke(command: list[str], directory: Path, environment: dict[str, str]) -> int:
    # A timed-out harness and its peers must not overlap the next measurement.
    with (directory / "stdout.log").open("w") as stdout, (
        directory / "stderr.log"
    ).open("w") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                   env=environment, start_new_session=True)
        try:
            return process.wait(timeout=90)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            return 124


def run_series(build: Path, output: Path, repetitions: int,
               trace_repetitions: int) -> int:
    planned = schedule(repetitions, trace_repetitions)
    if trace_repetitions and (platform.system() != "Linux" or not shutil.which("strace")):
        raise RuntimeError("traced runs require Linux and strace; no silent fallback")
    build = build.resolve()
    output = output.resolve()
    peers = [build / f"robotweax_srt_{name}" for name in
             ("timing_peer", "interop_peer", "group_timing_sender")]
    if any(not path.is_file() or not os.access(path, os.X_OK) for path in peers):
        raise RuntimeError("all three freshly built Robotweax peers are required")
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError("output directory must be empty to prevent stale evidence")
    scenario = next(s for s in aead.timing_scenarios()
                    if s.name == "ipv4-backup-group-path-outage-binary-1200")
    (output / "environment.json").write_text(json.dumps({
        "platform": platform.platform(), "python": sys.version,
        "revision": os.environ.get("GITHUB_SHA"),
        "schedule": planned, "scenario": scenario.name,
        "build_directory": str(build),
        "limits": {"egress_p99_9_us": aead.MAXIMUM_EGRESS_P99_9_MICROSECONDS,
                   "maximum_burst_depth": aead.MAXIMUM_BURST_DEPTH},
        "note": "Elapsed time including blocking/descheduling; tracing perturbs execution",
    }, indent=2) + "\n", encoding="utf-8")
    records = []
    for mode, index in planned:
        directory = output / f"{mode}-{index:02d}"
        directory.mkdir()
        score_path = directory / "scorecard.json"
        phase_path = directory / "phases.jsonl"
        receiver_path = directory / "receiver.jsonl"
        trace_path = directory / "syscalls.log"
        phase_enabled = mode != "control"
        receiver = (Path(__file__).with_name("trace_timing_peer.sh")
                    if mode == "strace" else peers[0])
        command = aead.scenario_command(scenario, receiver, peers[1], peers[2], score_path)
        command += ["--write-events", str(directory / "events.jsonl"),
                    "--write-receiver-log", str(receiver_path)]
        if phase_enabled:
            command += ["--write-phase-events", str(phase_path)]
        (directory / "command.json").write_text(json.dumps(command) + "\n", encoding="utf-8")
        environment = dict(os.environ, SRT_TIMING_PEER=str(peers[0]),
                           SRT_TIMING_TRACE_FILE=str(trace_path))
        record: dict[str, object] = {"mode": mode, "index": index, "passed": False}
        try:
            code = invoke(command, directory, environment)
            record["exit_code"] = code
            score = json.loads(score_path.read_text(encoding="utf-8"))
            record["egress_p99_9_us"] = score["egress_deadline_error_microseconds"]["p99_9"]
            record["release_p99_9_us"] = score["release_deadline_error_microseconds"]["p99_9"]
            record["phase_timing_enabled"] = score["measurement"]["phase_timing_enabled"]
            if record["phase_timing_enabled"] is not phase_enabled:
                raise RuntimeError("receiver instrumentation differs from requested mode")
            receiver_text = receiver_path.read_text(encoding="utf-8")
            validate_receiver_evidence(receiver_text, phase_enabled)
            if phase_enabled:
                phases = [json.loads(line) for line in phase_path.read_text().splitlines()]
                raw = [json.loads(line) for line in receiver_text.splitlines() if line.strip()]
                expected = timing.phase_observations(
                    [event for event in raw if event.get("event") == "timing"])
                if len(phases) != scenario.messages or tuple(phases) != expected:
                    raise RuntimeError("incomplete phase evidence")
            if mode == "strace" and (not trace_path.is_file() or trace_path.stat().st_size == 0):
                raise RuntimeError("syscall trace is missing")
            aead.validate_scorecard(scenario, score)
            if code:
                raise RuntimeError(f"measurement process exited with {code}")
            record["passed"] = True
        except (OSError, ValueError, KeyError, RuntimeError) as error:
            record["error"] = str(error)
        records.append(record)
        with (output / "runs.jsonl").open("a", encoding="utf-8") as log:
            log.write(json.dumps(record, sort_keys=True) + "\n")
        print(json.dumps(record, sort_keys=True), flush=True)
    (output / "summary.json").write_text(json.dumps(records, indent=2) + "\n", encoding="utf-8")
    return 0 if all(record["passed"] for record in records) else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-directory", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=10)
    parser.add_argument("--trace-repetitions", type=int, default=3)
    args = parser.parse_args()
    return run_series(args.build_directory, args.output_directory,
                      args.repetitions, args.trace_repetitions)


if __name__ == "__main__":
    raise SystemExit(main())
