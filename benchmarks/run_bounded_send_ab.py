#!/usr/bin/env python3
"""Fixed plain ABBA qualification in three gated stages, at most 104 transfers."""
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

build, diag, sc = common.build, common.diag, common.sc
REVISIONS = {
    "baseline": "8e1bdebed836cb7b732db852f51ef6a7b212e925",
    "candidate": "36477473ad3f68e94b90573797fb5a45c04d7872",
}
PROFILES = ["robotweax-self", "robotweax-to-haivision"]
THRESHOLDS = {"minimum_rate_ratio": .95, "maximum_cpu_ratio": 1.05,
              "improved_rate_ratio": 1.05, "improved_cpu_ratio": .95,
              "maximum_control_spread": 1.15}


def plan() -> list[dict]:
    return [{"name": name, "connections": connections, "bytes_per_connection": size,
             "repetitions": repetitions, "warmups": warmups}
            for name, connections, size, repetitions, warmups in
            (("00-window-128m", 1, 128 * 1024**2, 2, 0),
             ("01-long-1g", 1, 1024**3, 3, 1),
             ("02-ten-streams", 10, 128 * 1024**2, 3, 1))]


def arguments(stage: dict) -> dict:
    return {**common.ARGUMENTS, **{k: v for k, v in stage.items() if k != "name"}}


def analyze_block(report: dict, manifest: dict, exit_code: int, stage: dict, *, capture: str = "none") -> dict:
    result = common.analyze_block(report, manifest, exit_code,
                                  arguments={**arguments(stage), "capture": capture}, profiles=PROFILES, capture=capture)
    for row, case in zip(result["cases"], report.get("runs", [])):
        raw = case.get("result", {})
        resources = raw.get("peer_process_resources", {})
        row["peer_process_resources"] = resources
        for role in ("sender", "receiver"):
            own = resources.get(role) or {}
            valid = common.number(own.get("pid"), integer=True, positive=True) and all(
                common.number(own.get(k), integer=True) for k in
                ("user_cpu_us", "system_cpu_us", "voluntary_context_switches", "involuntary_context_switches"))
            if not valid:
                row["errors"].append(f"missing/invalid {role} PID, CPU or context switches")
            byte_count = raw.get("bytes_per_connection")
            connections = raw.get("connections")
            valid_size = (common.number(byte_count, integer=True, positive=True)
                          and common.number(connections, integer=True, positive=True))
            seconds = (own["user_cpu_us"] + own["system_cpu_us"]) / 1e6 if valid else None
            row[f"{role}_cpu_seconds"] = seconds
            row[f"{role}_cpu_seconds_per_gib"] = (
                seconds * 1024**3 / (byte_count * connections) if seconds is not None and valid_size else None)
        row["total_cpu_seconds_per_gib"] = (
            row["sender_cpu_seconds_per_gib"] + row["receiver_cpu_seconds_per_gib"]
            if all(row[f"{role}_cpu_seconds_per_gib"] is not None for role in ("sender", "receiver")) else None)
        row["completion_distributions"] = {}
        if stage["connections"] > 1:
            for side in ("caller", "listener"):
                distribution = raw.get("timing", {}).get(f"{side}_completion_seconds", {})
                values = [distribution.get(k) for k in ("minimum", "p50", "p99", "p99_9", "maximum")]
                if (not all(common.number(v) for v in values) or values != sorted(values)
                        or not common.number(values[-1], positive=True)):
                    row["errors"].append(f"missing/invalid {side} per-connection completion distribution")
                row["completion_distributions"][side] = distribution
        result["errors"].extend(f"case {row['index']}: {error}" for error in row["errors"]
                                if f"case {row['index']}: {error}" not in result["errors"])
    for summary in result["summary"]:
        rows = [r for r in result["cases"] if r["kind"] == "measurement" and r["profile"] == summary["profile"]]
        summary["evidence_passes"] = sum(not row["errors"] for row in rows)
        for metric in ("sender_cpu_seconds_per_gib", "receiver_cpu_seconds_per_gib", "total_cpu_seconds_per_gib"):
            values = [r[metric] for r in rows if r["transfer_pass"] and r.get(metric) is not None]
            summary[metric] = {"samples": len(values), "median": statistics.median(values) if values else None}
    result["measurement_contract_pass"] = not result["errors"]
    return result


def qualify(stage: dict) -> dict:
    blocks = stage["blocks"]
    contract = ([b["variant"] for b in blocks] == common.plan()
                and all(b["analysis"]["measurement_contract_pass"] for b in blocks))
    gate = {"measurement_contract_pass": contract, "qualified": False,
            "thresholds": THRESHOLDS, "comparisons": [], "errors": []}
    if not contract:
        gate["errors"].append("incomplete/failed measurement contract; no performance promotion")
        return gate
    controls = [r["mbps"] for b in blocks for r in b["analysis"]["cases"]
                if r["kind"] in ("control-before", "control-after")]
    if len(controls) != 8 or not all(common.number(v, positive=True) for v in controls):
        gate["errors"].append("missing/nonpositive Haivision controls")
    else:
        gate["control_spread"] = max(controls) / min(controls)
        if gate["control_spread"] > THRESHOLDS["maximum_control_spread"]:
            gate["errors"].append("Haivision control spread exceeds 15%")
    improved = False
    for profile in PROFILES:
        medians = {}
        for variant in REVISIONS:
            rows = [r for b in blocks if b["variant"] == variant for r in b["analysis"]["cases"]
                    if r["kind"] == "measurement" and r["profile"] == profile]
            metrics = ("mbps", "sender_cpu_seconds_per_gib", "total_cpu_seconds_per_gib")
            if len(rows) != 2 * stage["repetitions"] or any(
                    not common.number(r.get(k), positive=True) for r in rows for k in metrics):
                gate["errors"].append(f"missing/nonpositive {variant} {profile} measurement metrics")
                continue
            medians[variant] = {k: statistics.median(r[k] for r in rows) for k in metrics}
        if set(medians) != set(REVISIONS):
            continue
        ratios = {k: medians["candidate"][k] / medians["baseline"][k] for k in medians["baseline"]}
        gate["comparisons"].append({"profile": profile, "medians": medians, "candidate_over_baseline": ratios})
        if ratios["mbps"] < THRESHOLDS["minimum_rate_ratio"]:
            gate["errors"].append(f"{profile}: throughput regression exceeds 5%")
        if any(ratios[k] > THRESHOLDS["maximum_cpu_ratio"] for k in
               ("sender_cpu_seconds_per_gib", "total_cpu_seconds_per_gib")):
            gate["errors"].append(f"{profile}: sender/total CPU regression exceeds 5%")
        improved |= (ratios["mbps"] >= THRESHOLDS["improved_rate_ratio"]
                     or ratios["total_cpu_seconds_per_gib"] <= THRESHOLDS["improved_cpu_ratio"])
    if not improved:
        gate["errors"].append("no profile improves throughput or total CPU by at least 5%")
    gate["qualified"] = not gate["errors"]
    return gate


def run_blocks(paths: dict, manifests: dict, out: Path, report: dict) -> int:
    out.mkdir(parents=True, exist_ok=False)
    report.update(schema_version=1, complete=False, interrupted=False, qualified=False,
                  plan=plan(), thresholds=THRESHOLDS, stages=[])
    watched = [getattr(signal, name) for name in ("SIGINT", "SIGTERM", "SIGHUP") if hasattr(signal, name)]
    saved = {sig: signal.getsignal(sig) for sig in watched}
    active = None

    def interrupt(signum, _frame):
        for sig in watched:
            signal.signal(sig, signal.SIG_IGN)
        report["interruption_signal"] = signum
        raise KeyboardInterrupt

    for sig in watched:
        signal.signal(sig, interrupt)
    try:
        for stage_index, spec in enumerate(plan()):
            stage = {**spec, "blocks": [], "status": "running"}
            report["stages"].append(stage)
            stage_root = out / spec["name"]
            stage_root.mkdir()
            for index, variant in enumerate(common.plan()):
                name = f"{index:02}-{variant}-plain"
                argv = ["--build-manifest", str(paths[variant]), "--output-directory", str(stage_root / name)]
                for key, value in arguments(spec).items():
                    argv += ["--" + key.replace("_", "-"), str(value)]
                for profile in PROFILES:
                    argv += ["--profile", profile]
                active = {"name": name, "variant": variant, "exit_code": None, "driver_arguments": argv}
                stage["blocks"].append(active)
                sc.write_report(out / "qualification-report.json", report)
                with (stage_root / f"{name}.log").open("x") as log:
                    try:
                        with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                            active["exit_code"] = diag.main(argv)
                    except SystemExit as error:
                        active["exit_code"] = error.code if type(error.code) is int else 1
                    except Exception as error:
                        active["exit_code"] = 1
                        active["error"] = repr(error)
                        if report.get("interruption_signal") is not None:
                            report["interruption_cleanup_error"] = repr(error)
                            raise KeyboardInterrupt from error
                if report.get("interruption_signal") is not None:
                    raise KeyboardInterrupt
                try:
                    raw = json.loads((stage_root / name / "report.json").read_text())
                    active["analysis"] = analyze_block(raw, manifests[variant], active["exit_code"], spec)
                except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
                    active["analysis"] = {"measurement_contract_pass": False, "cases": [], "errors": [f"unusable report: {error}"]}
                print(json.dumps({"stage": spec["name"], "block": name, "exit_code": active["exit_code"],
                                  "measurement_contract_pass": active["analysis"]["measurement_contract_pass"]}), flush=True)
                sc.write_report(out / "qualification-report.json", report)
            stage["status"] = "completed"
            stage["gate"] = qualify(stage)
            if not stage["gate"]["qualified"]:
                report["stages"] += [{**later, "status": "skipped", "reason": f"gate failed: {spec['name']}"}
                                     for later in plan()[stage_index + 1:]]
                report["complete"] = True  # Conditional plan executed, not candidate approval.
                return 1 if not stage["gate"]["measurement_contract_pass"] else 2
        report.update(complete=True, qualified=True)
        return 0
    except KeyboardInterrupt:
        report["interrupted"] = True
        code = 128 + report.get("interruption_signal", signal.SIGINT)
        report["interruption_exit_code"] = code
        if active is not None and "analysis" not in active:
            if active["exit_code"] is not None:
                active["driver_exit_code_before_interruption"] = active["exit_code"]
            active["exit_code"] = code
        return code
    finally:
        try:
            sc.write_report(out / "qualification-report.json", report)
        finally:
            for sig, handler in saved.items():
                signal.signal(sig, handler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in REVISIONS:
        parser.add_argument("--" + variant + "-plain", required=True, type=Path)
    parser.add_argument("--output-directory", required=True, type=Path)
    args = parser.parse_args(argv)
    if (platform.system() != "Linux" or platform.machine() not in ("arm64", "aarch64")
            or socket.gethostname() != "lima-srt-network-lab-runtime"):
        parser.error("use the original dedicated srt-network-lab-runtime Linux ARM64 VM")
    environment = diag.host_metadata()
    if environment["cpu_count"] != 4:
        parser.error("retain the original four-vCPU configuration")
    paths = {v: getattr(args, v + "_plain").resolve() for v in REVISIONS}
    manifests = {v: json.loads(p.read_text()) for v, p in paths.items()}
    harness = build.source_identity(Path(__file__).resolve().parents[1])
    common.validate_manifests(manifests, harness, environment, revisions=REVISIONS)
    signatures = common.validate_build_files(paths, manifests)
    if any(int(environment["sysctls"].get(f"net/core/{key}_max") or 0) < common.ARGUMENTS["udp_buffer"]
           for key in ("rmem", "wmem")):
        parser.error("operator must configure and restore sufficient UDP maxima")
    report = {"harness_checkout": harness, "environment": environment, "manifests": manifests,
              "toolchain_signatures": signatures, "runner": sc.program_identity(Path(__file__)),
              "harness_files": [sc.program_identity(Path(m.__file__)) for m in (common, diag, sc, build)]}
    return run_blocks(paths, manifests, args.output_directory.resolve(), report)


if __name__ == "__main__":
    raise SystemExit(main())
