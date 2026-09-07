#!/usr/bin/env python3
"""Compare isolated Backup with the unchanged CI group matrix and a separate trace."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import sys
import time

from interop_common import file_sha256, resolve_program_path
from run_group_diagnostics import invoke, REFERENCE_COMMIT


def validate_phases(path: Path, enabled: bool) -> None:
    events = []
    for line in path.read_text().splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if isinstance(event, dict) and event.get("event") == "group_phase":
            events.append(event)
    if bool(events) != enabled:
        raise RuntimeError("missing phase evidence or instrumented control")
    pending = set()
    previous = -1
    for event in events:
        if any(type(event.get(key)) is not int for key in
               ("monotonic_us", "unix_us", "index", "result")):
            raise RuntimeError("invalid phase timestamps or result")
        if event["monotonic_us"] < previous:
            raise RuntimeError("non-monotonic phase evidence")
        previous = event["monotonic_us"]
        key = (event.get("operation"), event["index"])
        if event.get("edge") == "begin" and key not in pending:
            pending.add(key)
        elif event.get("edge") == "end" and key in pending:
            pending.remove(key)
        else:
            raise RuntimeError("unpaired phase evidence")
    if pending:
        raise RuntimeError("incomplete phase evidence")


def trace_command(command: list[str], path: Path, stacks: bool = False) -> list[str]:
    # Follow only this harness and its descendants. Never decode payload,
    # socket-option buffers or keys. No system-wide capture.
    return ["strace", *(["-k", "--stack-trace-frame-limit=24"] if stacks else []),
            "-f", "-ttt", "-T", "-e",
            "trace=process,network,futex,poll,ppoll,epoll_wait,clock_nanosleep",
            "-e", "raw=sendto,recvfrom,sendmsg,recvmsg,sendmmsg,recvmmsg,setsockopt,getsockopt",
            "-o", str(path), *command]


def run(robotweax: Path, reference: Path, output: Path, trace: bool = True,
        stack_trace: bool = False) -> int:
    if os.name != "posix":
        raise RuntimeError("POSIX process-group cleanup required")
    if stack_trace and not trace:
        raise RuntimeError("stack tracing requires Linux strace")
    if trace and (platform.system() != "Linux" or not shutil.which("strace")):
        raise RuntimeError("Linux and strace required; use --without-strace explicitly on macOS")
    robotweax, reference = map(resolve_program_path, (robotweax, reference))
    if not all(os.access(path, os.X_OK) for path in (robotweax, reference)):
        raise RuntimeError("both peers must be executable")
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    schedule = ["isolated-before", "matrix-control", "isolated-after", "matrix-phase"]
    if trace:
        schedule.append("matrix-strace")
    if stack_trace:
        schedule = ["matrix-control", "matrix-strace", "matrix-strace-stack"]
    metadata = {"schedule": schedule, "platform": platform.platform(),
                "revision": os.environ.get("GITHUB_SHA"), "python": sys.version,
                "reference_commit": REFERENCE_COMMIT,
                "robotweax_peer_sha256": file_sha256(robotweax),
                "reference_peer_sha256": file_sha256(reference),
                "note": "Matrix retains CI order and fails at first failing case; traced run is not a control."}
    (output / "environment.json").write_text(json.dumps(metadata, indent=2) + "\n")
    results = []
    base = Path(__file__).parent
    for mode in schedule:
        directory = output / mode
        directory.mkdir()
        is_matrix = mode.startswith("matrix-")
        phase_enabled = mode in ("matrix-phase", "matrix-strace", "matrix-strace-stack")
        environment = dict(os.environ,
                           ROBOTWEAX_SRT_GROUP_PHASE_TRACE="1" if phase_enabled else "0")
        command = [sys.executable, str(base / (
            "run_group_interop.py" if is_matrix else "run_group_diagnostics.py")),
            "--robotweax-peer", str(robotweax), "--reference-peer", str(reference)]
        if is_matrix:
            command += ["--baseline-only", "--expected-reference-version", "1.5.7",
                        "--evidence-directory", str(directory / "matrix")]
        else:
            command += ["--single-case", "--scenario", "group-backup-baseline",
                        "--output-directory", str(directory / "peers")]
        if mode.startswith("matrix-strace"):
            command = trace_command(command, directory / "syscalls.log",
                                    stacks=mode == "matrix-strace-stack")
        record = {"mode": mode, "passed": False, "status": "running",
                  "phase_enabled": phase_enabled,
                  "started_unix_ns": time.time_ns(), "timeout_seconds": 120 if is_matrix else 40,
                  "load_average": os.getloadavg(), "command": command}
        results.append(record)
        (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        started = time.monotonic()
        try:
            code = invoke(command, directory, timeout=record["timeout_seconds"],
                          environment=environment)
            record.update(exit_code=code, passed=code == 0, status="completed")
            if mode == "matrix-strace-stack":
                path = directory / "syscalls.log"
                with path.open(errors="replace") as stream:
                    record["stack_frame_lines"] = sum(line.lstrip().startswith("> ") for line in stream)
                record["stack_coverage"] = ("frames-present" if record["stack_frame_lines"] else "missing")
                if not record["stack_frame_lines"]:
                    raise RuntimeError("missing strace stack frames")
            if is_matrix and code == 0:
                cases = [json.loads(p.read_text()) for p in sorted(
                    (directory / "matrix").glob("case-*/result.json"))]
                record["completed_cases"] = len(cases)
                if len(cases) != 15 or not all(case["passed"] for case in cases):
                    raise RuntimeError("incomplete matrix evidence")
            peer_roots = (list((directory / "matrix").glob("case-*/peers"))
                          if is_matrix else [directory / "peers"])
            if code == 0 and (len(peer_roots) != (14 if is_matrix else 1) or not all(
                (root / name).is_file() for root in peer_roots for name in
                ("case.json", "caller.stdout", "caller.stderr", "listener.stdout", "listener.stderr"))):
                raise RuntimeError("incomplete peer evidence")
            if code == 0:
                for root in peer_roots:
                    for role in ("caller", "listener"):
                        validate_phases(root / f"{role}.stderr", phase_enabled)
            if mode.startswith("matrix-strace") and code == 0 and (
                not (directory / "syscalls.log").is_file()
                or (directory / "syscalls.log").stat().st_size == 0):
                raise RuntimeError("missing syscall trace")
        except (OSError, ValueError, KeyError, RuntimeError) as error:
            record.update(passed=False, status="failed", error=str(error))
        record["finished_unix_ns"] = time.time_ns()
        record["elapsed_seconds"] = time.monotonic() - started
        (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        print(json.dumps(record), flush=True)
    return 0 if all(record["passed"] for record in results) else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--without-strace", action="store_true")
    parser.add_argument("--stack-trace-comparison", action="store_true")
    args = parser.parse_args()
    return run(args.robotweax_peer, args.reference_peer, args.output_directory,
               not args.without_strace, args.stack_trace_comparison)


if __name__ == "__main__":
    raise SystemExit(main())
