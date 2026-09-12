#!/usr/bin/env python3
"""Forty fixed 16-MiB transfers: plain brackets and separate deadline diagnosis."""
from __future__ import annotations

import argparse
import contextlib
import copy
import json
import platform
import signal
import socket
from pathlib import Path

import pacer_deadline_diagnostics as dd
import run_pacer_deadline_ab as previous

common, diag, sc, build = previous.common, previous.diag, previous.sc, previous.build
REVISIONS = previous.REVISIONS
PROFILES = ["robotweax-self", "robotweax-to-haivision"]
ARGUMENTS = {**common.ARGUMENTS, "bytes_per_connection": 16 * 1024**2, "warmups": 0}


def plan() -> list[dict]:
    return [{"variant": variant, "capture": capture, "repetitions": repetitions}
            for variant, capture, repetitions in
            (("baseline", "none", 3), ("candidate", "none", 3),
             ("baseline", "pacer", 2), ("candidate", "pacer", 2),
             ("candidate", "none", 3), ("baseline", "none", 3))]


def key(variant: str, capture: str) -> str:
    return f"{variant}-{'plain' if capture == 'none' else 'diagnostic'}"


def block_arguments(spec: dict) -> dict:
    return {**ARGUMENTS, "capture": spec["capture"], "repetitions": spec["repetitions"]}


def validate_manifests(manifests: dict, harness: dict, environment: dict) -> None:
    if set(manifests) != {key(v, c) for v in REVISIONS for c in ("none", "pacer")}:
        raise ValueError("exactly four fresh builds are required")
    for capture in ("none", "pacer"):
        normalized = {}
        for variant, revision in REVISIONS.items():
            m = manifests[key(variant, capture)]
            overlay = m.get("pacer_deadline_diagnostics", {})
            if overlay.get("enabled", False) is not (capture == "pacer"):
                raise ValueError("plain/diagnostic build mismatch")
            if capture == "pacer":
                expected = dd.patched_sources(Path(m["sources"]["robotweax"]["path"]), revision)
                files = [{"path": name, "original_sha256": dd.hashlib.sha256(raw).hexdigest(),
                          "patched_sha256": dd.hashlib.sha256(patched).hexdigest()}
                         for name, (raw, patched) in expected.items()]
                if (overlay.get("source_revision") != revision or overlay.get("files") != files
                        or overlay.get("overlay_script", {}).get("sha256") != sc.file_sha256(Path(dd.__file__))
                        or overlay.get("collector_header", {}).get("sha256") != sc.file_sha256(dd.HEADER)):
                    raise ValueError("diagnostic source/anchor/header identity mismatch")
            normalized[variant] = copy.deepcopy(m)
            normalized[variant].pop("pacer_deadline_diagnostics", None)
        common.validate_manifests(normalized, harness, environment, revisions=REVISIONS)


def analyze_block(report: dict, manifest: dict, exit_code: int, spec: dict) -> dict:
    result = previous.analyze_block(report, manifest, exit_code,
                                    arguments=block_arguments(spec), profiles=PROFILES, capture=spec["capture"])
    if spec["capture"] == "pacer":
        for row, case in zip(result["cases"], report.get("runs", [])):
            row["profiling"] = case.get("profiling")
            if (case.get("profiling") or {}).get("valid") is not True:
                error = "missing/incomplete deadline diagnosis; preserve partial checkpoints"
                row["errors"].append(error)
                result["errors"].append(f"case {row['index']}: {error}")
        result["summary"] = []  # Diagnostic rates remain per-case, never in plain aggregates.
    result["measurement_contract_pass"] = not result["errors"]
    return result


def run_blocks(paths: dict, manifests: dict, out: Path, report: dict) -> int:
    out.mkdir(parents=True, exist_ok=False)
    report.update(complete=False, interrupted=False, plan=plan(), blocks=[],
                  scope="16-MiB mechanism diagnosis, not a replacement/retry of the 1-GiB experiment")
    watched = [getattr(signal, n) for n in ("SIGINT", "SIGTERM", "SIGHUP") if hasattr(signal, n)]
    saved = {sig: signal.getsignal(sig) for sig in watched}

    def interrupt(signum, _frame):
        for sig in watched:
            signal.signal(sig, signal.SIG_IGN)
        report["interruption_signal"] = signum
        raise KeyboardInterrupt

    for sig in watched:
        signal.signal(sig, interrupt)
    try:
        for index, spec in enumerate(plan()):
            own = key(spec["variant"], spec["capture"])
            name = f"{index:02}-{own}"
            args = ["--build-manifest", str(paths[own]), "--output-directory", str(out / name)]
            for option, value in block_arguments(spec).items():
                args += ["--" + option.replace("_", "-"), str(value)]
            for profile in PROFILES:
                args += ["--profile", profile]
            block = {"name": name, **spec, "driver_arguments": args, "exit_code": None}
            report["blocks"].append(block)
            sc.write_report(out / "diagnostic-report.json", report)
            with (out / f"{name}.log").open("x") as log:
                try:
                    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                        block["exit_code"] = diag.main(args)
                except SystemExit as error:
                    block["exit_code"] = error.code if type(error.code) is int else 1
                except Exception as error:
                    block["exit_code"] = 1
                    block["error"] = repr(error)
            try:
                raw = json.loads((out / name / "report.json").read_text())
                block["analysis"] = analyze_block(raw, manifests[own], block["exit_code"], spec)
            except (OSError, ValueError, TypeError, KeyError, OverflowError) as error:
                block["analysis"] = {"measurement_contract_pass": False, "errors": [f"unusable report: {error}"]}
            print(json.dumps({"name": name, "exit_code": block["exit_code"],
                              "measurement_contract_pass": block["analysis"]["measurement_contract_pass"]}), flush=True)
            sc.write_report(out / "diagnostic-report.json", report)
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
            sc.write_report(out / "diagnostic-report.json", report)
        finally:
            for sig, handler in saved.items():
                signal.signal(sig, handler)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in REVISIONS:
        for capture in ("none", "pacer"):
            parser.add_argument("--" + key(variant, capture), required=True, type=Path)
    parser.add_argument("--output-directory", required=True, type=Path)
    args = parser.parse_args(argv)
    if (platform.system() != "Linux" or platform.machine() not in ("arm64", "aarch64")
            or socket.gethostname() != "lima-srt-network-lab-runtime"):
        parser.error("use the original dedicated srt-network-lab-runtime Linux ARM64 VM")
    environment = diag.host_metadata()
    if environment["cpu_count"] != 4:
        parser.error("retain the original four-vCPU configuration")
    paths = {key(v, c): getattr(args, key(v, c).replace("-", "_")).resolve() for v in REVISIONS for c in ("none", "pacer")}
    manifests = {name: json.loads(path.read_text()) for name, path in paths.items()}
    harness = build.source_identity(Path(__file__).resolve().parents[1])
    validate_manifests(manifests, harness, environment)
    signatures = {}
    for capture in ("none", "pacer"):
        signatures[capture] = common.validate_build_files(
            {v: paths[key(v, capture)] for v in REVISIONS}, {v: manifests[key(v, capture)] for v in REVISIONS})
    if signatures["none"] != signatures["pacer"]:
        parser.error("plain and diagnostic compiler/OpenSSL configuration differs")
    if any(int(environment["sysctls"].get(f"net/core/{n}_max") or 0) < ARGUMENTS["udp_buffer"] for n in ("rmem", "wmem")):
        parser.error("operator must record/configure/restore UDP maxima of at least 8 MiB")
    report = {"schema_version": 1, "environment": environment, "harness_checkout": harness,
              "manifests": manifests, "toolchain_signatures": signatures,
              "harness_files": [sc.program_identity(Path(m.__file__)) for m in (dd, previous, common, diag, sc, build)],
              "runner": sc.program_identity(Path(__file__)), "collector_header": sc.program_identity(dd.HEADER)}
    return run_blocks(paths, manifests, args.output_directory.resolve(), report)


if __name__ == "__main__":
    raise SystemExit(main())
