#!/usr/bin/env python3
"""Pinned plain ABBA for the absolute pacer-deadline candidate.

72 serial 1-GiB transfers on the original Linux ARM64 VM. No retries,
sysctl changes, instrumentation or automatic performance approval.
"""
from __future__ import annotations

import argparse
import contextlib
import json
import platform
import signal
import socket
import statistics
from pathlib import Path

import run_plain_capacity_ab as common

build, sc, diag = common.build, common.sc, common.diag
REVISIONS = {
    "baseline": "8e1bdebed836cb7b732db852f51ef6a7b212e925",
    "candidate": "11814d3a1ebc217dbe95613b679960ac3152ff59",
}
PROFILES = ["robotweax-self", "haivision-self", "robotweax-to-haivision", "haivision-to-robotweax"]
ARGUMENTS = {**common.ARGUMENTS, "bytes_per_connection": 1024**3}


def cases() -> list[dict]:
    return diag.case_plan(PROFILES, ARGUMENTS["repetitions"], ARGUMENTS["warmups"], True, "none")


def analyze_block(report: dict, manifest: dict, exit_code: int, *, arguments: dict = ARGUMENTS,
                  profiles: list = PROFILES, capture: str = "none") -> dict:
    analysis = common.analyze_block(report, manifest, exit_code, arguments=arguments, profiles=profiles, capture=capture)
    for row, entry in zip(analysis["cases"], report.get("runs", [])):
        resources = entry.get("result", {}).get("peer_process_resources", {})
        row["peer_process_resources"] = resources
        for role in ("sender", "receiver"):
            own = resources.get(role) or {}
            valid = common.number(own.get("pid"), integer=True, positive=True) and all(
                common.number(own.get(k), integer=True) for k in
                ("user_cpu_us", "system_cpu_us", "voluntary_context_switches", "involuntary_context_switches"))
            if not valid:
                error = f"missing/invalid {role} CPU, PID or context-switch evidence"
                row["errors"].append(error)
                analysis["errors"].append(f"case {row['index']}: {error}")
            seconds = (own["user_cpu_us"] + own["system_cpu_us"]) / 1e6 if valid else None
            row[f"{role}_cpu_seconds"] = seconds
            byte_count = entry.get("result", {}).get("bytes_per_connection")
            row[f"{role}_cpu_seconds_per_gib"] = (
                seconds * 1024**3 / byte_count
                if seconds is not None and common.number(byte_count, integer=True, positive=True) else None)
    for summary in analysis["summary"]:
        selected = [row for row in analysis["cases"]
                    if row["kind"] == "measurement" and row["profile"] == summary["profile"]]
        summary["evidence_passes"] = sum(not row["errors"] for row in selected)
        for metric in ("sender_cpu_seconds", "receiver_cpu_seconds",
                       "sender_cpu_seconds_per_gib", "receiver_cpu_seconds_per_gib"):
            values = [row[metric] for row in selected if row["transfer_pass"] and row.get(metric) is not None]
            summary[metric] = {"samples": len(values), "median": statistics.median(values) if values else None,
                               "mean": statistics.mean(values) if values else None,
                               "p95": sc.percentile(values, .95) if values else None}
    analysis["measurement_contract_pass"] = not analysis["errors"]
    return analysis


def run_blocks(paths: dict, manifests: dict, out: Path, report: dict) -> int:
    out.mkdir(parents=True, exist_ok=False)
    report.update(complete=False, interrupted=False, plan=common.plan(), blocks=[],
                  performance_decision_requires_review=True)
    watched = [getattr(signal, name) for name in ("SIGINT", "SIGTERM", "SIGHUP") if hasattr(signal, name)]
    previous = {sig: signal.getsignal(sig) for sig in watched}

    def interrupt(signum, _frame):
        # Let run_bounded terminate and reap its tracked case group. Repeated
        # signals must not interrupt that cleanup or the report's finally path.
        for sig in watched:
            signal.signal(sig, signal.SIG_IGN)
        report["interruption_signal"] = signum
        raise KeyboardInterrupt

    for sig in watched:
        signal.signal(sig, interrupt)
    try:
        for index, variant in enumerate(common.plan()):
            name = f"{index:02}-{variant}-plain"
            arguments = ["--build-manifest", str(paths[variant]), "--output-directory", str(out / name)]
            for key, value in ARGUMENTS.items():
                arguments += ["--" + key.replace("_", "-"), str(value)]
            for profile in PROFILES:
                arguments += ["--profile", profile]
            block = {"name": name, "variant": variant, "driver_arguments": arguments, "exit_code": None}
            report["blocks"].append(block)
            sc.write_report(out / "ab-report.json", report)
            # Run in this process: diag.run_bounded owns the single nested case
            # session and its peers. An extra block subprocess would obscure
            # those groups from the outer operator during interruption.
            with (out / f"{name}.log").open("x") as log:
                try:
                    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                        block["exit_code"] = diag.main(arguments)
                except SystemExit as error:
                    block["exit_code"] = error.code if type(error.code) is int else 1
                except Exception as error:
                    block["exit_code"] = 1
                    block["error"] = repr(error)
            try:
                diagnostic = json.loads((out / name / "report.json").read_text())
                block["analysis"] = analyze_block(diagnostic, manifests[variant], block["exit_code"])
            except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
                block["analysis"] = {"measurement_contract_pass": False, "errors": [f"unusable report: {error}"]}
            print(json.dumps({"name": name, "exit_code": block["exit_code"],
                              "measurement_contract_pass": block["analysis"]["measurement_contract_pass"]}), flush=True)
            sc.write_report(out / "ab-report.json", report)
        report["complete"] = True
        report["measurement_contract_pass"] = all(b["analysis"]["measurement_contract_pass"] for b in report["blocks"])
        return 0 if report["measurement_contract_pass"] else 1
    except KeyboardInterrupt:
        report["interrupted"] = True
        code = 128 + report.get("interruption_signal", signal.SIGINT)
        report["interruption_exit_code"] = code
        if report["blocks"] and report["blocks"][-1]["exit_code"] is None:
            report["blocks"][-1]["exit_code"] = code
        return code
    finally:
        try:
            sc.write_report(out / "ab-report.json", report)
        finally:
            for sig, handler in previous.items():
                signal.signal(sig, handler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in REVISIONS:
        parser.add_argument(f"--{variant}-plain", type=Path, required=True)
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args(argv)
    if (platform.system() != "Linux" or platform.machine() not in ("aarch64", "arm64")
            or socket.gethostname() != "lima-srt-network-lab-runtime"):
        parser.error("use the original dedicated srt-network-lab-runtime Linux ARM64 VM")
    environment = diag.host_metadata()
    if environment["cpu_count"] != 4:
        parser.error("retain the original four-vCPU configuration")
    paths = {variant: getattr(args, f"{variant}_plain").resolve() for variant in REVISIONS}
    manifests = {variant: json.loads(path.read_text()) for variant, path in paths.items()}
    harness = build.source_identity(Path(__file__).resolve().parents[1])
    common.validate_manifests(manifests, harness, environment, revisions=REVISIONS)
    signatures = common.validate_build_files(paths, manifests)
    if any(int(environment["sysctls"].get(f"net/core/{key}_max") or 0) < ARGUMENTS["udp_buffer"] for key in ("rmem", "wmem")):
        parser.error("operator must record/configure/restore UDP maxima of at least 8 MiB")
    report = {"schema_version": 1, "environment": environment, "manifests": manifests,
              "toolchain_signatures": signatures, "harness_checkout": harness,
              "harness_files": [sc.program_identity(Path(m.__file__).resolve()) for m in (common, diag, sc, build)],
              "runner": sc.program_identity(Path(__file__).resolve()),
              "scope": "plain IPv4 loopback; one connection; library pins distinct from the common harness pin"}
    return run_blocks(paths, manifests, args.output_directory.resolve(), report)


if __name__ == "__main__":
    raise SystemExit(main())
