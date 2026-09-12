#!/usr/bin/env python3
"""Fixed 48-case comparison separating dispatch policy from bounded send work."""
from __future__ import annotations

import argparse
import contextlib
import json
import math
import platform
import signal
import socket
import statistics
from pathlib import Path

import run_bounded_send_ab as budget

common, build, diag, sc = budget.common, budget.build, budget.diag, budget.sc
REVISIONS = {
    "a": "8e1bdebed836cb7b732db852f51ef6a7b212e925",
    "b": "11814d3a1ebc217dbe95613b679960ac3152ff59",
    "c": "36477473ad3f68e94b90573797fb5a45c04d7872",
    "d": "52d48ee9439d190b920692bd2981729365416b02",
}
FACTORS = {
    "a": {"dispatch": "baseline", "send_budget": False},
    "b": {"dispatch": "absolute-deadline", "send_budget": False},
    "c": {"dispatch": "absolute-deadline", "send_budget": True},
    "d": {"dispatch": "baseline", "send_budget": True},
}
STAGE = {"connections": 1, "bytes_per_connection": 128 * 1024**2, "repetitions": 2, "warmups": 0}
PROFILES = budget.PROFILES
THRESHOLDS = budget.THRESHOLDS
CORE_METRICS = ("mbps", "sender_cpu_seconds_per_gib", "receiver_cpu_seconds_per_gib", "total_cpu_seconds_per_gib")
RESOURCE_METRICS = ("user_cpu_us", "system_cpu_us", "voluntary_context_switches", "involuntary_context_switches")
EXTRA_METRICS = tuple(f"{role}_{metric}_per_gib" for role in ("sender", "receiver") for metric in RESOURCE_METRICS)


def plan() -> list[str]:
    return ["a", "b", "c", "d", "d", "c", "b", "a"]


def analyze_block(report: dict, manifest: dict, exit_code: int) -> dict:
    result = budget.analyze_block(report, manifest, exit_code, STAGE)
    actual_bytes = math.ceil(STAGE["bytes_per_connection"] / 1316) * 1316
    for row in result["cases"]:
        for role in ("sender", "receiver"):
            own = row["peer_process_resources"].get(role) or {}
            for key in RESOURCE_METRICS:
                value = own.get(key)
                row[f"{role}_{key}_per_gib"] = (
                    value * 1024**3 / actual_bytes if common.number(value, integer=True) else None)
    return result


def analyze_experiment(report: dict) -> dict:
    blocks = report["blocks"]
    contract = (report.get("complete") is True and not report.get("interrupted")
                and [b["variant"] for b in blocks] == plan()
                and all(b["analysis"]["measurement_contract_pass"] for b in blocks))
    result = {"measurement_contract_pass": contract, "comparison_valid": False,
              "thresholds": THRESHOLDS, "profiles": [], "candidate_gates": {},
              "followup_candidates": [], "long_transfer_qualification_performed": False, "errors": []}
    if not contract:
        result["errors"].append("incomplete/failed 48-case contract; no component comparison")
        return result
    controls = [row["mbps"] for b in blocks for row in b["analysis"]["cases"]
                if row["kind"] in ("control-before", "control-after")]
    controls_valid = len(controls) == 16 and all(common.number(x, positive=True) for x in controls)
    result["haivision_controls"] = controls
    result["control_spread"] = max(controls) / min(controls) if controls_valid else None
    if not controls_valid or result["control_spread"] > THRESHOLDS["maximum_control_spread"]:
        result["errors"].append("missing/nonpositive H controls or spread exceeds 15%")
    for profile in PROFILES:
        medians = {}
        for variant in REVISIONS:
            rows = [row for b in blocks if b["variant"] == variant for row in b["analysis"]["cases"]
                    if row["kind"] == "measurement" and row["profile"] == profile]
            if len(rows) != 4 or any(not common.number(row.get(k), positive=k in CORE_METRICS)
                                     for row in rows for k in (*CORE_METRICS, *EXTRA_METRICS)):
                result["errors"].append(f"missing/invalid {variant}/{profile} metrics")
                result["measurement_contract_pass"] = False
                continue
            medians[variant] = {k: statistics.median(row[k] for row in rows) for k in (*CORE_METRICS, *EXTRA_METRICS)}
        if set(medians) != set(REVISIONS):
            continue

        def ratio(numerator, denominator):
            return {k: medians[numerator][k] / medians[denominator][k]
                    if medians[denominator][k] > 0 else None for k in (*CORE_METRICS, *EXTRA_METRICS)}

        contrasts = {f"{n}_over_{d}": ratio(n, d) for n, d in (("d", "a"), ("c", "b"), ("b", "a"), ("c", "d"))}
        interaction = {k: contrasts["c_over_b"][k] / contrasts["d_over_a"][k]
                       if contrasts["c_over_b"][k] is not None and contrasts["d_over_a"][k] not in (None, 0)
                       else None for k in (*CORE_METRICS, *EXTRA_METRICS)}
        result["profiles"].append({"profile": profile, "samples_per_variant": 4, "medians": medians,
                                   "contrasts": contrasts, "budget_interaction_c_over_b_div_d_over_a": interaction,
                                   "versus_a": {v: ratio(v, "a") for v in ("b", "c", "d")}})
    result["comparison_valid"] = not result["errors"]
    for variant in ("b", "c", "d"):
        reasons, improved = [], False
        if not result["comparison_valid"]:
            reasons.append("comparison invalid; no follow-up promotion")
        for profile in result["profiles"]:
            ratios = profile["versus_a"][variant]
            if ratios["mbps"] < THRESHOLDS["minimum_rate_ratio"]:
                reasons.append(f"{profile['profile']}: throughput regression exceeds 5%")
            if any(ratios[k] > THRESHOLDS["maximum_cpu_ratio"] for k in
                   ("sender_cpu_seconds_per_gib", "total_cpu_seconds_per_gib")):
                reasons.append(f"{profile['profile']}: sender/total CPU regression exceeds 5%")
            improved |= (ratios["mbps"] >= THRESHOLDS["improved_rate_ratio"]
                         or ratios["total_cpu_seconds_per_gib"] <= THRESHOLDS["improved_cpu_ratio"])
        if not improved:
            reasons.append("no profile improves throughput or total CPU by at least 5%")
        result["candidate_gates"][variant] = {"eligible_for_long_transfer_followup": not reasons, "reasons": reasons}
        if not reasons:
            result["followup_candidates"].append(variant)
    return result


def run_blocks(paths: dict, manifests: dict, out: Path, report: dict) -> int:
    out.mkdir(parents=True, exist_ok=False)
    report.update(schema_version=1, complete=False, interrupted=False, plan=plan(), factors=FACTORS,
                  stage=STAGE, thresholds=THRESHOLDS, blocks=[], analysis=None,
                  long_transfer_qualification_performed=False)
    watched = [getattr(signal, n) for n in ("SIGINT", "SIGTERM", "SIGHUP") if hasattr(signal, n)]
    saved = {sig: signal.getsignal(sig) for sig in watched}
    active = None

    def interrupted(signum, _frame):
        for sig in watched:
            signal.signal(sig, signal.SIG_IGN)
        report["interruption_signal"] = signum
        raise KeyboardInterrupt

    for sig in watched:
        signal.signal(sig, interrupted)
    try:
        for index, variant in enumerate(plan()):
            name = f"{index:02}-{variant}-plain"
            argv = ["--build-manifest", str(paths[variant]), "--output-directory", str(out / name)]
            for key, value in budget.arguments(STAGE).items():
                argv += ["--" + key.replace("_", "-"), str(value)]
            for profile in PROFILES:
                argv += ["--profile", profile]
            active = {"name": name, "variant": variant, "exit_code": None, "driver_arguments": argv}
            report["blocks"].append(active)
            sc.write_report(out / "factorial-report.json", report)
            with (out / f"{name}.log").open("x") as log:
                try:
                    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                        active["exit_code"] = diag.main(argv)
                except SystemExit as error:
                    active["exit_code"] = error.code if type(error.code) is int else 1
                except Exception as error:
                    active["exit_code"], active["error"] = 1, repr(error)
            try:
                raw = json.loads((out / name / "report.json").read_text())
                active["analysis"] = analyze_block(raw, manifests[variant], active["exit_code"])
            except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
                active["analysis"] = {"measurement_contract_pass": False, "cases": [], "errors": [f"unusable report: {error}"]}
            sc.write_report(out / "factorial-report.json", report)
            print(json.dumps({"block": name, "exit_code": active["exit_code"],
                              "measurement_contract_pass": active["analysis"]["measurement_contract_pass"]}), flush=True)
        report["complete"] = True
        report["analysis"] = analyze_experiment(report)
        if not report["analysis"]["measurement_contract_pass"]:
            report["runner_exit_code"] = 1
        else:
            report["runner_exit_code"] = 0 if report["analysis"]["comparison_valid"] else 2
        return report["runner_exit_code"]
    except KeyboardInterrupt:
        report["interrupted"] = True
        code = 128 + report.get("interruption_signal", signal.SIGINT)
        report["interruption_exit_code"] = report["runner_exit_code"] = code
        if active is not None and active["exit_code"] is None:
            active["exit_code"] = code
        return code
    finally:
        try:
            sc.write_report(out / "factorial-report.json", report)
        finally:
            for sig, handler in saved.items():
                signal.signal(sig, handler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in REVISIONS:
        parser.add_argument(f"--{variant}-plain", required=True, type=Path)
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
    if any(int(environment["sysctls"].get(f"net/core/{k}_max") or 0) < common.ARGUMENTS["udp_buffer"] for k in ("rmem", "wmem")):
        parser.error("operator must configure and restore sufficient UDP maxima")
    report = {"harness_checkout": harness, "environment": environment, "manifests": manifests,
              "toolchain_signatures": signatures, "runner": sc.program_identity(Path(__file__)),
              "harness_files": [sc.program_identity(Path(m.__file__)) for m in (budget, common, diag, sc, build)]}
    return run_blocks(paths, manifests, args.output_directory.resolve(), report)


if __name__ == "__main__":
    raise SystemExit(main())
