#!/usr/bin/env python3
"""Manual same-host ABBA qualification; no retries or performance pass claims."""
from __future__ import annotations

import argparse
import json
import platform
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "benchmarks"))
import scalability_scorecard as sc


def plan():
    for index, variant in enumerate(("baseline", "candidate", "candidate", "baseline")):
        for rate in (20_000_000, 100_000_000):
            for connections in (1, 10):
                yield index, variant, rate, connections


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    sources = {v: getattr(args, v).resolve() for v in ("baseline", "candidate")}
    report = {"platform": platform.platform(), "sources": {}, "cases": [],
              "limits": "Shared hosted runner; CPU is both peers combined where available. Not a dedicated-lab capacity result."}
    def save():
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    def invoke(command, log, timeout=900):
        with log.open("w") as stream:
            return subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                  timeout=timeout).returncode
    peers = {}
    for variant, source in sources.items():
        report["sources"][variant] = subprocess.check_output(
            ["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
        save()
        build = out / f"build-{variant}"
        commands = [
            ["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_BUILD_TYPE=Release",
             "-DBUILD_SHARED_LIBS=OFF", "-DROBOTWEAX_SRT_BUILD_TESTS=OFF",
             "-DROBOTWEAX_SRT_BUILD_TOOLS=ON", "-DROBOTWEAX_SRT_BUILD_BENCHMARKS=ON"],
            ["cmake", "--build", str(build), "--config", "Release", "--parallel", "2",
             "--target", "robotweax_srt_interop_peer", "robotweax_srt_timing_peer",
             "robotweax_srt_scalability_peer"],
        ]
        for i, command in enumerate(commands):
            if invoke(command, out / f"{variant}-build-{i}.log"):
                raise SystemExit(f"{variant} build failed; see logs")
        directory = build / "Release" if platform.system() == "Windows" else build
        suffix = ".exe" if platform.system() == "Windows" else ""
        peers[variant] = {name: directory / f"robotweax_srt_{name}_peer{suffix}"
                          for name in ("interop", "timing", "scalability")}
    failed = False
    for index, variant, rate, connections in plan():
        directory = out / f"{index}-{variant}-{rate}-{connections}"
        directory.mkdir()
        case = dict(index=index, variant=variant, rate=rate, connections=connections)
        try:
            case["capacity"] = sc.run_many_socket_profile(
                "robotweax-self", {"robotweax": peers[variant]["scalability"]},
                sc.RunOptions("127.0.0.1", connections, 8 * 1024**2, 1316, 45, 120, 500, .02),
                directory, iteration=index, warmup=False,
                peer_arguments=("--target-bps", str(rate)))
            case["passed"] = True
        except Exception as error:
            case.update(passed=False, error=str(error))
            failed = True
        report["cases"].append(case)
        save()
        if connections == 1:
            for fault in ("none", "loss-delay-reorder"):
                score = directory / f"timing-{fault}.json"
                command = [sys.executable, str(ROOT / "interop/run_live_timing_interop.py"),
                           "--timing-peer", str(peers[variant]["timing"]),
                           "--sender-peer", str(peers[variant]["interop"]),
                           "--profile", "binary-1200", "--messages", "3000",
                           "--bitrate-bps", str(rate), "--fault-profile", fault,
                           "--output", str(score)]
                code = invoke(command, directory / f"timing-{fault}.log", timeout=120)
                report["cases"].append(dict(index=index, variant=variant, rate=rate,
                                            timing_fault=fault, passed=code == 0,
                                            scorecard=str(score.relative_to(out))))
                failed |= code != 0
                save()
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
