#!/usr/bin/env python3
"""Serial matched-rate ABBA plus separate matched/unpaced causal traces.

Run on the original dedicated Linux VM after four clean builds. Does not build,
change sysctls, retry failures, remove results or start hosted CI.
"""
from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path

import scalability_scorecard as sc
import throughput_diagnostics as diag

REVISIONS = {"baseline": "a5c428b725b5373651041b0e30678406b2d5c38c",
             "candidate": "40387f2dc70b7701b266be1c91f048f2d4fa3b61"}


def plan() -> list[dict]:
    plain = [{"variant": v, "capture": "none", "target_bps": 100_000_000, "repetitions": 3}
             for v in ("baseline", "candidate", "candidate", "baseline")]
    traces = [{"variant": v, "capture": "transport", "target_bps": rate, "repetitions": 1}
              for rate in (100_000_000, 0) for v in REVISIONS]
    return plain + traces


def validate_manifests(manifests: dict) -> None:
    if set(manifests) != {(v, m) for v in REVISIONS for m in ("none", "transport")}:
        raise ValueError("exactly four A/B plain/trace manifests are required")
    helpers, platforms, overlays = set(), set(), set()
    for (variant, mode), manifest in manifests.items():
        if manifest.get("poll_counters", {}).get("enabled", False):
            raise ValueError("poll counters cannot be used in this experiment")
        if not manifest.get("complete") or manifest["sources"]["robotweax"]["dirty"]:
            raise ValueError("all builds must be complete and based on clean revisions")
        if manifest["sources"]["robotweax"]["revision"] != REVISIONS[variant]:
            raise ValueError("unexpected A/B source revision")
        if manifest["sources"]["haivision"]["revision"] != "899348d8318eb9a3c5a5b6ec43c4a1114288773a":
            raise ValueError("unexpected Haivision reference")
        overlay = manifest.get("transport_trace", {})
        if bool(overlay.get("enabled")) != (mode == "transport"):
            raise ValueError("plain/trace builds are not interchangeable")
        helpers.add(manifest["harness_source"]["sha256"])
        platforms.add((manifest["platform"], manifest["architecture"], manifest["crypto"], manifest["linkage"]))
        if mode == "transport":
            overlays.add((overlay["overlay_script"]["sha256"], overlay["collector_header"]["sha256"]))
    if len(helpers) != 1 or len(platforms) != 1 or len(overlays) != 1:
        raise ValueError("A/B builds do not share helper/platform/overlay")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in REVISIONS:
        for mode in ("plain", "trace"):
            parser.add_argument(f"--{variant}-{mode}", type=Path, required=True, help="build-manifest.json")
    parser.add_argument("--output-directory", type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("this qualification handoff targets the original Linux VM; use individual diagnostics for local smoke")
    paths = {(v, capture): getattr(args, f"{v}_{mode}").resolve()
             for v in REVISIONS for mode, capture in (("plain", "none"), ("trace", "transport"))}
    manifests = {key: json.loads(path.read_text()) for key, path in paths.items()}
    validate_manifests(manifests)
    limits = diag.host_metadata()["sysctls"]
    if any(int(limits[f"net/core/{key}_max"] or 0) < 8 * 1024**2 for key in ("rmem", "wmem")):
        parser.error("UDP maxima below 8 MiB; operator must record, temporarily configure and restore the VM limits")
    out = args.output_directory.resolve()
    out.mkdir(parents=True, exist_ok=False)
    report = {"complete": False, "plan": plan(), "blocks": [], "environment": diag.host_metadata(),
              "manifests": {f"{v}-{m}": value for (v, m), value in manifests.items()},
              "runner": sc.program_identity(Path(__file__).resolve())}
    try:
        for index, block in enumerate(report["plan"]):
            name = f"{index:02}-{block['variant']}-{block['capture']}-{block['target_bps']}"
            command = [sys.executable, str(Path(diag.__file__).resolve()),
                       "--build-manifest", str(paths[block["variant"], block["capture"]]),
                       "--output-directory", str(out / name), "--capture", block["capture"],
                       "--repetitions", str(block["repetitions"]), "--target-bps", str(block["target_bps"]),
                       "--pacing-burst-packets", "4" if block["target_bps"] else "0",
                       "--cooldown-seconds", "30"]
            report["blocks"].append({"name": name, "command": command, "exit_code": None})
            sc.write_report(out / "ab-report.json", report)
            with (out / f"{name}.log").open("x") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            report["blocks"][-1]["exit_code"] = result.returncode
            print(json.dumps(report["blocks"][-1]), flush=True)
            sc.write_report(out / "ab-report.json", report)
        report["complete"] = True
        report["all_blocks_pass"] = all(b["exit_code"] == 0 for b in report["blocks"])
        return 0 if report["all_blocks_pass"] else 1
    finally:
        sc.write_report(out / "ab-report.json", report)


if __name__ == "__main__":
    raise SystemExit(main())
