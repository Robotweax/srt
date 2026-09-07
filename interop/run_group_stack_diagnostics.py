#!/usr/bin/env python3
"""Bounded Linux group matrix with one stalled-reference stack snapshot."""
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
import time

from interop_common import file_sha256, resolve_program_path
from run_group_diagnostics import invoke, REFERENCE_COMMIT
from run_group_matrix_diagnostics import validate_phases


def stalled_receive(path: Path, now_us: int) -> dict | None:
    pending = None
    for line in path.read_text(errors="replace").splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if not isinstance(event, dict) or event.get("event") != "group_phase":
            continue
        if event.get("operation") == "receive-payload":
            pending = event if event.get("edge") == "begin" else None
    if pending and now_us - pending["monotonic_us"] >= 750_000:
        return pending
    return None


def verified_target(identity: dict, reference: Path, harness_pid: int) -> bool:
    """Only our current harness's child with the exact executable may be attached."""
    pid = identity.get("pid")
    if type(pid) is not int or pid <= 1 or identity.get("parent_pid") != harness_pid:
        return False
    try:
        proc = Path("/proc") / str(pid)
        parent = int((proc / "stat").read_text().rsplit(")", 1)[1].split()[1])
        return parent == harness_pid and (proc / "exe").resolve(strict=True) == reference
    except (OSError, ValueError, IndexError):
        return False


def snapshot_command(pid: int) -> list[str]:
    return ["sudo", "-n", "timeout", "--signal=TERM", "--kill-after=1s", "2s",
            "gdb", "-nx", "-nh", "--batch",
            "-iex", "set auto-load off", "-iex", "set debuginfod enabled off",
            "-iex", "set print frame-arguments none", "-p", str(pid),
            "-x", str(Path(__file__).with_name("group_stack.gdb"))]


def capture(command: list[str], reference: Path, directory: Path, environment: dict) -> dict:
    record = {"snapshot_attempted": False, "coverage": "no-stall-observed"}
    with (directory / "stdout.log").open("w") as stdout, (directory / "stderr.log").open("w") as stderr:
        process = subprocess.Popen(command, stdout=stdout, stderr=stderr,
                                   env=environment, start_new_session=True)
        deadline = time.monotonic() + 120
        try:
            while process.poll() is None and time.monotonic() < deadline:
                if not record["snapshot_attempted"]:
                    for path in sorted((directory / "matrix").glob("case-*/peers/listener.stderr")):
                        identity_path = path.with_name("listener-process.json")
                        if not identity_path.exists():
                            continue
                        try:
                            identity = json.loads(identity_path.read_text())
                            event = stalled_receive(path, time.monotonic_ns() // 1000)
                        except (OSError, ValueError, KeyError):
                            continue
                        if event and verified_target(identity, reference, process.pid):
                            record.update(snapshot_attempted=True, coverage="attempted",
                                          target=identity, phase=event, case=path.parent.parent.name,
                                          snapshot_started_unix_ns=time.time_ns())
                            (directory / "snapshot.json").write_text(json.dumps(record, indent=2) + "\n")
                            with (directory / "stack.log").open("w") as stack:
                                try:
                                    result = subprocess.run(snapshot_command(identity["pid"]),
                                        stdout=stack, stderr=subprocess.STDOUT, timeout=5, check=False)
                                    record["snapshot_exit_code"] = result.returncode
                                except subprocess.TimeoutExpired:
                                    record["snapshot_exit_code"] = 124
                            # A killed debugger must not leave our own child stopped.
                            if verified_target(identity, reference, process.pid):
                                os.kill(identity["pid"], signal.SIGCONT)
                            text = (directory / "stack.log").read_text()
                            record["coverage"] = ("stack-captured" if record["snapshot_exit_code"] == 0
                                and "GROUP_STACK_BEGIN" in text and "GROUP_STACK_END" in text
                                and "#0 " in text else "capture-failed")
                            record["snapshot_finished_unix_ns"] = time.time_ns()
                            (directory / "snapshot.json").write_text(json.dumps(record, indent=2) + "\n")
                            break
                time.sleep(0.05)
            record["exit_code"] = process.poll() if process.poll() is not None else 124
        finally:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
    return record


def run(robotweax: Path, reference: Path, output: Path) -> int:
    if platform.system() != "Linux" or not shutil.which("gdb"):
        raise RuntimeError("Linux with gdb required; no silent fallback")
    robotweax, reference = map(resolve_program_path, (robotweax, reference))
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "environment.json").write_text(json.dumps({
        "revision": os.environ.get("GITHUB_SHA"), "reference_commit": REFERENCE_COMMIT,
        "robotweax_sha256": file_sha256(robotweax), "reference_sha256": file_sha256(reference),
        "platform": platform.platform(), "python": sys.version,
        "note": "One control matrix and one independent stack-observed matrix. No strace."}, indent=2) + "\n")
    results = []
    for mode in ("matrix-control", "matrix-stack"):
        directory = output / mode
        directory.mkdir()
        environment = dict(os.environ, ROBOTWEAX_SRT_GROUP_PHASE_TRACE="0" if mode == "matrix-control" else "1")
        command = [sys.executable, str(Path(__file__).with_name("run_group_interop.py")),
            "--robotweax-peer", str(robotweax), "--reference-peer", str(reference),
            "--baseline-only", "--expected-reference-version", "1.5.7",
            "--evidence-directory", str(directory / "matrix")]
        record = {"mode": mode, "status": "running", "passed": False, "command": command}
        results.append(record)
        (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        if mode == "matrix-control":
            record.update(exit_code=invoke(command, directory, timeout=120, environment=environment))
        else:
            record.update(capture(command, reference, directory, environment))
        cases = [json.loads(p.read_text()) for p in (directory / "matrix").glob("case-*/result.json")]
        record.update(status="completed", completed_cases=len(cases),
                      passed=record["exit_code"] == 0 and len(cases) == 15
                      and all(c["passed"] for c in cases) and record.get("coverage") != "capture-failed")
        if record["passed"]:
            try:
                roots = list((directory / "matrix").glob("case-*/peers"))
                if len(roots) != 14:
                    raise RuntimeError("missing peer evidence")
                for root in roots:
                    for role in ("caller", "listener"):
                        validate_phases(root / f"{role}.stderr", mode == "matrix-stack")
            except (OSError, ValueError, RuntimeError):
                record.update(passed=False, error="missing or invalid phase evidence")
        (output / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
        print(json.dumps(record), flush=True)
    return 0 if all(r["passed"] for r in results) else 1


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--robotweax-peer", type=Path, required=True)
    parser.add_argument("--reference-peer", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(run(args.robotweax_peer, args.reference_peer, args.output_directory))
