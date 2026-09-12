#!/usr/bin/env python3
"""Fixed-pin, uninstrumented Linux ARM64 ABBA after the Lite-ACK correction.

Forty serial transfers, no builds, sysctl changes, retries or automatic
performance/release approval. Preserve failed blocks and their raw evidence.
"""
from __future__ import annotations

import argparse
import json
import math
import platform
import statistics
import subprocess
import sys
from pathlib import Path

import prepare_throughput as build
import scalability_scorecard as sc
import throughput_diagnostics as diag

REVISIONS = {
    "baseline": "a5c428b725b5373651041b0e30678406b2d5c38c",
    "candidate": "09c852b40374d21e60e68b8aadaaabb5aa7294b8",
}
PEER_SHA256 = "faf487b34f5661e4dc602130628ed216b8054a2ab364abcc275bdda1d826d2ae"
PROFILES = ["robotweax-self", "robotweax-to-haivision"]
ARGUMENTS = {
    "capture": "none", "repetitions": 3, "warmups": 1,
    "bytes_per_connection": 128 * 1024**2, "target_bps": 0,
    "pacing_burst_packets": 0, "pending_packets": 8192,
    "udp_buffer": 8 * 1024**2, "timeout_seconds": 45, "cooldown_seconds": 30,
}
UDP_ERRORS = ("InErrors", "RcvbufErrors", "SndbufErrors", "InCsumErrors")


def plan() -> list[str]:
    return ["baseline", "candidate", "candidate", "baseline"]


def cases() -> list[dict]:
    return diag.case_plan(PROFILES, 3, 1, True, "none")


def validate_manifests(manifests: dict, harness: dict, environment: dict) -> None:
    if set(manifests) != set(REVISIONS):
        raise ValueError("exactly two plain build manifests are required")
    if harness.get("dirty") is not False:
        raise ValueError("run from a clean committed harness checkout")
    for variant, manifest in manifests.items():
        if manifest.get("complete") is not True:
            raise ValueError("incomplete build")
        for library, revision in (("robotweax", REVISIONS[variant]),
                                  ("haivision", build.REFERENCE_REVISION)):
            source = manifest["sources"][library]
            if source.get("dirty") is not False or source.get("revision") != revision:
                raise ValueError(f"unexpected/dirty {variant} {library} source")
        if manifest.get("transport_trace", {}).get("enabled", False) is not False:
            raise ValueError("instrumented builds cannot produce plain capacity evidence")
        if manifest.get("crypto") != "openssl" or manifest.get("linkage") != "static":
            raise ValueError("this follow-up requires the original static OpenSSL build profile")
        if any(manifest.get(key) != environment[key] for key in ("platform", "architecture")):
            raise ValueError("build and measurement platform mismatch")
        checkout = manifest["harness_checkout"]
        if checkout.get("dirty") is not False or checkout.get("revision") != harness["revision"]:
            raise ValueError("both builds and runner must use the same clean harness revision")
        if manifest["harness_source"]["sha256"] != PEER_SHA256:
            raise ValueError("public peer differs from the previously measured harness")
        if any(set(manifest[key]) != {"robotweax", "haivision"} for key in ("programs", "libraries")):
            raise ValueError("both peers and libraries are required")


def validate_build_files(paths: dict, manifests: dict) -> dict:
    """Check binaries now (diagnostics rechecks per block) and toolchain evidence."""
    signatures = {}
    for variant, path in paths.items():
        manifest = manifests[variant]
        for identity in [*manifest["programs"].values(), *manifest["libraries"].values()]:
            if sc.file_sha256(Path(identity["path"])) != identity["sha256"]:
                raise ValueError("peer/library changed since build")
        root = path.parent
        signatures[variant] = {name: sc.file_sha256(root / name) for name in
                               ("compiler.log", "cmake.log", "openssl.log", "crypto-flags.log")}
        for library in ("robotweax", "haivision"):
            cache = {}
            for line in (root / f"{library}-cache.txt").read_text().splitlines():
                if "=" in line and ":" in line and not line.startswith(("//", "#")):
                    key, value = line.split("=", 1)
                    cache[key.split(":", 1)[0]] = value
            expected = {"CMAKE_BUILD_TYPE": "Release", "CMAKE_C_FLAGS": "-g", "CMAKE_CXX_FLAGS": "-g",
                        "CMAKE_C_FLAGS_RELEASE": "-O3 -DNDEBUG", "CMAKE_CXX_FLAGS_RELEASE": "-O3 -DNDEBUG"}
            if any(cache.get(key) != value for key, value in expected.items()):
                raise ValueError("unexpected Release flags; retain evidence and review the build")
            signatures[variant][f"{library}-compilers"] = [cache.get(f"CMAKE_{language}_COMPILER")
                                                         for language in ("C", "CXX")]
            if not all(signatures[variant][f"{library}-compilers"]):
                raise ValueError("missing compiler identity in CMake cache")
    if signatures["baseline"] != signatures["candidate"]:
        raise ValueError("A/B compiler or OpenSSL configuration differs")
    return signatures


def number(value, *, integer: bool = False, positive: bool = False) -> bool:
    return (type(value) in ((int,) if integer else (int, float))
            and math.isfinite(value) and value >= (1 if positive else 0))


def analyze_block(report: dict, manifest: dict, exit_code: int) -> dict:
    """Missing/partial evidence is a failure, never an inferred zero count."""
    errors, rows = [], []
    if exit_code != 0 or report.get("finished") is not True or report.get("all_transfers_pass") is not True:
        errors.append("diagnostic block did not finish successfully")
    if report.get("capture") != "none" or report.get("udp_capacity_controlled") is not True:
        errors.append("not an uninstrumented UDP-capacity-controlled block")
    arguments = report.get("arguments", {})
    if (any(arguments.get(key) != value for key, value in ARGUMENTS.items())
            or arguments.get("profile") != PROFILES
            or arguments.get("allow_small_udp_buffers") is not False):
        errors.append("measurement arguments changed")
    if report.get("build_manifest") != manifest:
        errors.append("reported build differs from preflight manifest")
    runs = report.get("runs", [])
    if len(runs) != len(cases()):
        errors.append("missing or extra transfers")
    packets = math.ceil(ARGUMENTS["bytes_per_connection"] / 1316)
    for index, entry in enumerate(runs):
        problems = []
        if index >= len(cases()) or any(entry.get(key) != value for key, value in cases()[index].items()):
            problems.append("unexpected case order/profile/kind")
        if entry.get("index") != index or entry.get("exit_code") != 0 or entry.get("transfer_pass") is not True:
            problems.append("failed/incomplete transfer")
        result = entry.get("result", {})
        integrity = result.get("integrity", {})
        if (integrity.get("deterministic_payload_verified") is not True
                or integrity.get("verified_connections") != 1
                or result.get("connections") != 1 or result.get("message_size_bytes") != 1316
                or result.get("requested_bytes_per_connection") != ARGUMENTS["bytes_per_connection"]
                or result.get("messages_per_connection") != packets
                or result.get("bytes_per_connection") != packets * 1316):
            problems.append("payload/transfer contract not proven")
        wire = result.get("wire_statistics", {})
        repeats = wire.get("retransmitted_packets")
        original = wire.get("sender_packets_unique")
        if (not number(repeats, integer=True) or not number(original, integer=True, positive=True)
                or original != packets or wire.get("receiver_packets_unique") != packets
                or wire.get("sender_packets_total") != original + repeats):
            problems.append("missing/inconsistent public packet counters")
        elif repeats != 0:
            problems.append("nonzero retransmissions: retain and investigate")
        udp = entry.get("system_udp_delta", {})
        if any(type(udp.get(key)) is not int or udp[key] != 0 for key in UDP_ERRORS):
            problems.append("missing/nonzero host UDP error deltas")
        for role in ("caller", "listener"):
            options = result.get("capacity_options", {}).get(role, [])
            expected = {"encryption": "none", "tlpktdrop": False, "pending_packets": 8192,
                        "requested_maxbw_bytes_per_second": 1250000000, "target_bits_per_second": 0,
                        "api_fc": 16384, "api_udp_rcvbuf": ARGUMENTS["udp_buffer"],
                        "api_udp_sndbuf": ARGUMENTS["udp_buffer"]}
            if len(options) != 1 or any(options[0].get(key) != value for key, value in expected.items()):
                problems.append(f"{role} capacity-option contract missing/changed")
        rate = result.get("rates", {}).get("useful_bits_per_second")
        cpu = result.get("peer_process_resources", {}).get("sender") or {}
        user, system = cpu.get("user_cpu_us"), cpu.get("system_cpu_us")
        if not number(rate, positive=True) or not number(user) or not number(system):
            problems.append("missing/nonfinite throughput or sender CPU observation")
        row = {"index": index, "kind": entry.get("kind"), "profile": entry.get("profile"),
               "transfer_pass": entry.get("transfer_pass") is True and entry.get("exit_code") == 0,
               "mbps": rate / 1e6 if number(rate, positive=True) else None,
               "sender_cpu_seconds": (user + system) / 1e6 if number(user) and number(system) else None,
               "original_packets": original, "retransmitted_packets": repeats,
               "retransmission_ratio": repeats / original if number(repeats, integer=True) and number(original, integer=True, positive=True) else None,
               "system_udp_delta": udp, "errors": problems}
        rows.append(row)
        errors.extend(f"case {index}: {problem}" for problem in problems)
    summaries = []
    for profile in PROFILES:
        selected = [row for row in rows if row["kind"] == "measurement" and row["profile"] == profile]
        summary = {"profile": profile, "attempts": len(selected),
                   "transfer_successes": sum(row["transfer_pass"] for row in selected),
                   "evidence_passes": sum(not row["errors"] for row in selected),
                   "conditional_on_complete_observations": True}
        for metric in ("mbps", "sender_cpu_seconds"):
            # Keep successful transfers even if they fail the zero-repeat gate.
            # Never promote warmups/controls or retry replacements into samples.
            values = [row[metric] for row in selected if row["transfer_pass"] and row[metric] is not None]
            summary[metric] = {"samples": len(values), "median": statistics.median(values) if values else None,
                               "mean": statistics.mean(values) if values else None,
                               "p95": sc.percentile(values, .95) if values else None}
        summaries.append(summary)
    return {"measurement_contract_pass": not errors, "errors": errors, "cases": rows, "summary": summaries}


def run_blocks(paths: dict, manifests: dict, out: Path, report: dict) -> int:
    out.mkdir(parents=True, exist_ok=False)
    report.update(complete=False, plan=plan(), blocks=[], performance_decision_requires_review=True)
    try:
        for index, variant in enumerate(plan()):
            name = f"{index:02}-{variant}-plain"
            command = [sys.executable, str(Path(diag.__file__).resolve()), "--build-manifest", str(paths[variant]),
                       "--output-directory", str(out / name)]
            for key, value in ARGUMENTS.items():
                command += ["--" + key.replace("_", "-"), str(value)]
            for profile in PROFILES:
                command += ["--profile", profile]
            block = {"name": name, "variant": variant, "command": command, "exit_code": None}
            report["blocks"].append(block)
            sc.write_report(out / "ab-report.json", report)
            with (out / f"{name}.log").open("x") as log:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            block["exit_code"] = completed.returncode
            try:
                diagnostic = json.loads((out / name / "report.json").read_text())
                block["analysis"] = analyze_block(diagnostic, manifests[variant], completed.returncode)
            except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
                block["analysis"] = {"measurement_contract_pass": False, "errors": [f"unusable report: {error}"]}
            print(json.dumps({"name": name, "exit_code": block["exit_code"],
                              "measurement_contract_pass": block["analysis"]["measurement_contract_pass"]}), flush=True)
            sc.write_report(out / "ab-report.json", report)
        report["complete"] = True
        report["measurement_contract_pass"] = all(block["analysis"]["measurement_contract_pass"] for block in report["blocks"])
        return 0 if report["measurement_contract_pass"] else 1
    finally:
        sc.write_report(out / "ab-report.json", report)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in REVISIONS:
        parser.add_argument(f"--{variant}-plain", type=Path, required=True, help="build-manifest.json")
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args(argv)
    if platform.system() != "Linux" or platform.machine() not in ("aarch64", "arm64"):
        parser.error("use the original dedicated Linux ARM64 VM; local smoke is not this qualification")
    environment = diag.host_metadata()
    paths = {variant: getattr(args, f"{variant}_plain").resolve() for variant in REVISIONS}
    manifests = {variant: json.loads(path.read_text()) for variant, path in paths.items()}
    harness = build.source_identity(Path(__file__).resolve().parents[1])
    validate_manifests(manifests, harness, environment)
    signatures = validate_build_files(paths, manifests)
    if any(int(environment["sysctls"].get(f"net/core/{key}_max") or 0) < ARGUMENTS["udp_buffer"] for key in ("rmem", "wmem")):
        parser.error("operator must record/configure/restore UDP maxima of at least 8 MiB")
    report = {"schema_version": 1, "environment": environment, "manifests": manifests,
              "toolchain_signatures": signatures, "harness_checkout": harness,
              "harness_files": [sc.program_identity(Path(module.__file__).resolve()) for module in (diag, sc)],
              "runner": sc.program_identity(Path(__file__).resolve()),
              "scope": "plain IPv4 loopback; one connection; no WAN, encryption, live-deadline or release approval"}
    return run_blocks(paths, manifests, args.output_directory.resolve(), report)


if __name__ == "__main__":
    raise SystemExit(main())
