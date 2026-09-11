#!/usr/bin/env python3
"""Serial, cleartext capacity controls and separate Linux perf captures.

Uses the public-API scalability peer and its existing integrity validation.
Never adjusts sysctls, installs tools, retries a failed transfer, or treats
instrumented throughput as an uninstrumented performance result.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import time
from dataclasses import asdict
from pathlib import Path

import scalability_scorecard as sc

try:
    import resource
except ImportError:  # importable for portable harness tests; runner is POSIX only
    resource = None

ROOT = Path(__file__).resolve().parents[1]
PROFILES = tuple(sc.PROFILE_IMPLEMENTATIONS)


def bounded_int(low: int, high: int):
    def parse(value: str) -> int:
        result = int(value)
        if not low <= result <= high:
            raise argparse.ArgumentTypeError(f"must be in {low}..{high}")
        return result
    return parse


def host_metadata() -> dict:
    values = {}
    for key in ("net/core/rmem_max", "net/core/wmem_max", "kernel/perf_event_paranoid",
                "kernel/kptr_restrict"):
        path = Path("/proc/sys") / key
        values[key] = path.read_text().strip() if path.is_file() else None
    return {"platform": platform.platform(), "architecture": platform.machine(),
            "cpu_count": os.cpu_count(), "cpu_model": sc.cpu_model(), "sysctls": values,
            "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
            "load_average": os.getloadavg() if hasattr(os, "getloadavg") else None,
            "python": sys.version}


def udp_snapshot() -> dict:
    path = Path("/proc/net/snmp")
    if not path.is_file():
        return {}
    lines = path.read_text().splitlines()
    for index, line in enumerate(lines[:-1]):
        if line.startswith("Udp:"):
            return dict(zip(line.split()[1:], map(int, lines[index + 1].split()[1:])))
    return {}


def peer_arguments(args) -> tuple[str, ...]:
    return ("--pending-packets", str(args.pending_packets), "--flow-window", "16384",
            "--send-buffer", "33554432", "--receive-buffer", "33554432",
            "--udp-buffer", str(args.udp_buffer), "--max-bandwidth", "1250000000",
            "--target-bps", str(args.target_bps))


def case_plan(profiles: list[str], repetitions: int, warmups: int,
              reference: bool, capture: str) -> list[dict]:
    plan = []
    if reference and capture == "none":
        plan.append({"profile": "haivision-self", "kind": "control-before"})
    for profile in profiles:
        for _ in range(warmups if capture == "none" else 0):
            plan.append({"profile": profile, "kind": "warmup"})
        for _ in range(repetitions):
            plan.append({"profile": profile, "kind": "measurement" if capture == "none" else capture})
    if reference and capture == "none":
        plan.append({"profile": "haivision-self", "kind": "control-after"})
    return plan


def perf_command(capture: str, data: Path, command: list[str], perf: str) -> list[str]:
    if capture == "none":
        return command
    if capture == "cpu":
        # Software clock works without a virtualized hardware PMU. DWARF avoids
        # silently losing stacks when the unchanged Release build omits FP.
        return [perf, "record", "-o", str(data), "-e", "cpu-clock", "-F", "199",
                "--call-graph", "dwarf,8192", "--", *command]
    if capture == "scheduler":
        # perf sched records system-wide scheduler tracepoints. Explicit opt-in.
        return [perf, "sched", "record", "-o", str(data), "--", *command]
    raise ValueError("unknown capture mode")


def limit_trace_size() -> None:
    if resource is None:
        raise RuntimeError("POSIX resource limits are unavailable")
    limit = 128 * 1024 * 1024
    _, hard = resource.getrlimit(resource.RLIMIT_FSIZE)
    resource.setrlimit(resource.RLIMIT_FSIZE, (min(limit, hard) if hard >= 0 else limit, hard))


def run_bounded(command: list[str], directory: Path, timeout: int) -> int:
    with (directory / "command.log").open("w") as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True, preexec_fn=limit_trace_size)
        try:
            return process.wait(timeout=timeout)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
            raise
        finally:
            # A profiler can fail before its workload (e.g. file-size limit).
            # Do not leave that workload competing with the next serial case.
            try:
                os.killpg(process.pid, signal.SIGTERM)
                time.sleep(.1)
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass


def run_case(path: Path) -> int:
    request = json.loads(path.read_text())
    directory = path.parent
    entry = {"transfer_pass": False, "started_unix_ns": time.time_ns()}
    before = udp_snapshot()
    try:
        result = sc.run_many_socket_profile(
            request["profile"], {k: Path(v) for k, v in request["programs"].items()},
            sc.RunOptions(**request["options"]), directory,
            iteration=request["index"], warmup=request["kind"] == "warmup",
            peer_arguments=tuple(request["peer_arguments"]))
        # The completion contract already validates both complete payloads and
        # both exit statuses. Retain API buffer readbacks alongside the result.
        result["capacity_options"] = {
            role: [event for event in sc.parse_json_events(sc.read_output(directory / f"many-socket-{role}.stdout"))
                   if event.get("event") == "capacity-options"]
            for role in ("caller", "listener")
        }
        if not all(result["capacity_options"].values()):
            raise sc.ScorecardFailure("peer lacks capacity-option readbacks; rebuild both peers")
        entry.update(transfer_pass=True, result=result,
                     rate_pass=(request["target_bps"] == 0 or result["rates"]["useful_bits_per_second"] >= .95 * request["target_bps"]))
        return 0
    except Exception as error:
        entry["error"] = str(error)
        return 1
    finally:
        after = udp_snapshot()
        entry["system_udp_delta"] = {k: after[k] - v for k, v in before.items() if k in after}
        entry["finished_unix_ns"] = time.time_ns()
        sc.write_report(directory / "case.json", entry)


def sample_counts(text: str, pids: list[int]) -> dict[int, int]:
    counts = dict.fromkeys(pids, 0)
    for line in text.splitlines():
        fields = line.split()
        # perf's field selection is not an output-order guarantee. Versions
        # commonly render COMM then PID even for '-F pid,comm'.
        if len(fields) == 2:
            for field in fields:
                if field.isdigit() and int(field) in counts:
                    counts[int(field)] += 1
                    break
    return counts


def render_capture(capture: str, directory: Path, result: dict, perf: str) -> dict:
    data = directory / "perf.data"
    pids = [(result.get("peer_process_resources", {}).get(role) or {}).get("pid")
            for role in ("sender", "receiver")]
    if not data.is_file() or not all(isinstance(pid, int) for pid in pids):
        return {"valid": False, "error": "missing perf data or peer PID evidence"}
    commands = [([perf, "report", "--stdio", "--no-children", "--sort", "comm,pid,dso,symbol",
                 "-i", str(data)], "perf-report.txt"),
                ([perf, "script", "-i", str(data)], "perf-stacks.txt")]
    if capture == "scheduler":
        commands = [([perf, "sched", "timehist", "-i", str(data), "--summary", "--pid",
                      ",".join(map(str, pids))], "perf-scheduler.txt")]
    for command, name in commands:
        with (directory / name).open("w") as output:
            completed = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                       timeout=30, preexec_fn=limit_trace_size)
        if completed.returncode:
            return {"valid": False, "error": f"render failed: {name}", "command": command}
    if capture == "cpu":
        completed = subprocess.run([perf, "script", "-i", str(data), "-F", "pid,comm"],
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
        counts = sample_counts(completed.stdout, pids)
        sc.write_report(directory / "sample-coverage.json", counts)
        return {"valid": completed.returncode == 0 and all(counts.values()),
                "samples_by_peer_pid": counts,
                "call_stack_quality_requires_review": True}
    # A successful renderer is not proof of a usable per-peer schedule trace.
    content = (directory / "perf-scheduler.txt").read_text()
    found = all(re.search(rf"(?<!\d){pid}(?!\d)", content) for pid in pids)
    return {"valid": found, "peer_pids": pids, "trace_coverage_requires_review": True}


def summarize(entries: list[dict]) -> list[dict]:
    summaries = []
    for profile in sorted({entry["profile"] for entry in entries if entry["kind"] == "measurement"}):
        selected = [e for e in entries if e["profile"] == profile and e["kind"] == "measurement"]
        values = [e["result"]["rates"]["useful_bits_per_second"] / 1e6 for e in selected if e.get("transfer_pass")]
        summaries.append({"profile": profile, "attempts": len(selected), "complete": len(values),
                          "failed": len(selected) - len(values), "conditional_on_complete_transfers": True,
                          "median_mbps": statistics.median(values) if values else None,
                          "p95_mbps": sc.percentile(values, .95) if values else None,
                          "mean_mbps": statistics.mean(values) if values else None})
    return summaries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-manifest", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    parser.add_argument("--profile", action="append", choices=PROFILES)
    parser.add_argument("--capture", choices=("none", "cpu", "scheduler"), default="none")
    parser.add_argument("--repetitions", type=bounded_int(1, 10))
    parser.add_argument("--warmups", type=bounded_int(0, 2), default=1)
    parser.add_argument("--bytes-per-connection", type=bounded_int(1316, 1024**3), default=128 * 1024**2)
    parser.add_argument("--target-bps", type=bounded_int(0, 100_000_000_000), default=0)
    parser.add_argument("--pending-packets", type=bounded_int(1, 65536), default=8192)
    parser.add_argument("--udp-buffer", type=bounded_int(65536, 32 * 1024**2), default=8 * 1024**2)
    parser.add_argument("--timeout-seconds", type=bounded_int(10, 90), default=45)
    parser.add_argument("--cooldown-seconds", type=bounded_int(0, 60), default=10)
    parser.add_argument("--allow-small-udp-buffers", action="store_true")
    parser.add_argument("--allow-system-wide", action="store_true")
    args = parser.parse_args(argv)
    if platform.system() not in ("Linux", "Darwin"):
        parser.error("this process-group diagnostic runner requires Linux or macOS")
    if args.capture != "none" and platform.system() != "Linux":
        parser.error("perf capture requires Linux; use Instruments separately on macOS")
    if args.capture == "scheduler" and not args.allow_system_wide:
        parser.error("scheduler capture requires --allow-system-wide on a dedicated test VM")
    perf = shutil.which("perf") or "perf"
    if args.capture != "none" and not shutil.which("perf"):
        parser.error("perf is not installed; no fallback to an unprofiled success")
    manifest = json.loads(args.build_manifest.read_text())
    if manifest.get("complete") is not True:
        parser.error("build manifest is incomplete")
    for identity in [*manifest["programs"].values(), *manifest["libraries"].values()]:
        if sc.file_sha256(Path(identity["path"])) != identity["sha256"]:
            parser.error("a peer or library changed since the recorded build")
    programs = {name: value["path"] for name, value in manifest["programs"].items()}
    profiles = args.profile or ["robotweax-self", "robotweax-to-haivision"]
    if any("haivision" in sc.PROFILE_IMPLEMENTATIONS[p] for p in profiles) and "haivision" not in programs:
        parser.error("selected profile needs the reference peer")
    environment = host_metadata()
    limits = [environment["sysctls"][f"net/core/{key}_max"] for key in ("rmem", "wmem")]
    adequate = all(value is not None and int(value) >= args.udp_buffer for value in limits)
    if platform.system() == "Linux" and not adequate and not args.allow_small_udp_buffers:
        parser.error("UDP kernel limits are below requested buffers; see docs/throughput-profiling.md (no sysctl was changed)")
    repetitions = args.repetitions or (5 if args.capture == "none" else 1)
    out = args.output_directory.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"schema_version": 1, "finished": False, "capture": args.capture,
              "environment": environment, "udp_capacity_controlled": adequate,
              "build_manifest": manifest, "arguments": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
              "runs": [], "scope": "cleartext IPv4 loopback; TSBPD 120ms; TLPKTDROP=false; no deadline qualification",
              "profiling_scope": "driver and descendant peers, including connection setup and drain; filter by peer PID",
              "started_unix_ns": time.time_ns()}
    report["harness_files"] = [sc.program_identity(path) for path in
                               (Path(__file__).resolve(), ROOT / "benchmarks/scalability_scorecard.py")]
    sc.write_report(out / "report.json", report)
    options = sc.RunOptions("127.0.0.1", 1, args.bytes_per_connection, 1316,
                            args.timeout_seconds, 120, 500, .02)
    try:
        time.sleep(args.cooldown_seconds)
        for index, item in enumerate(case_plan(profiles, repetitions, args.warmups, "haivision" in programs, args.capture)):
            directory = out / f"{index:02}-{item['profile']}-{item['kind']}"
            directory.mkdir()
            request = {**item, "index": index, "programs": programs, "options": asdict(options),
                       "target_bps": args.target_bps, "peer_arguments": peer_arguments(args)}
            sc.write_report(directory / "request.json", request)
            command = perf_command(args.capture, directory / "perf.data",
                                   [sys.executable, str(Path(__file__).resolve()), "--case-file", str(directory / "request.json")], perf)
            entry = {**item, "index": index, "command": command, "transfer_pass": False}
            try:
                entry["exit_code"] = run_bounded(command, directory, 2 * args.timeout_seconds + 15)
                if (directory / "case.json").is_file():
                    entry.update(json.loads((directory / "case.json").read_text()))
                else:
                    entry["error"] = "capture/driver exited without a case report; see command.log"
                if args.capture != "none":
                    entry["profiling"] = render_capture(args.capture, directory, entry.get("result", {}), perf)
            except Exception as error:
                entry["error"] = str(error)
            report["runs"].append(entry)
            report["summary"] = summarize(report["runs"])
            sc.write_report(out / "report.json", report)
            print(json.dumps({"index": index, **item, "transfer_pass": entry["transfer_pass"],
                              "profiling": entry.get("profiling")}), flush=True)
        report["finished"] = True
        report["all_transfers_pass"] = all(e["transfer_pass"] and e.get("exit_code") == 0 for e in report["runs"])
        report["all_captures_usable"] = args.capture == "none" or all(e.get("profiling", {}).get("valid") for e in report["runs"])
        return 0 if report["all_transfers_pass"] and report["all_captures_usable"] else 1
    finally:
        report["finished_unix_ns"] = time.time_ns()
        sc.write_report(out / "report.json", report)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--case-file":
        raise SystemExit(run_case(Path(sys.argv[2])))
    raise SystemExit(main())
