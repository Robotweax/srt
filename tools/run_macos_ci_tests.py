#!/usr/bin/env python3
"""Keep timing tests serial; overlap only isolated crypto fault probes."""

import argparse
import json
from pathlib import Path
import re
import subprocess


def plan(tests):
    names = [test["name"] for test in tests]
    if not names or any(not isinstance(name, str) or not name for name in names):
        raise ValueError("CTest inventory must contain named tests")
    if len(set(names)) != len(names):
        raise ValueError("CTest inventory contains duplicate names")
    probes = [test["name"] for test in tests
              if test["name"].startswith("robotweax_srt_crypto_setup_")
              and test.get("command")
              and Path(test["command"][0]).name == "robotweax_srt_crypto_setup_failure_tests"]
    if not probes:
        return [(1, [])]
    pattern = "^(" + "|".join(re.escape(name) for name in probes) + ")$"
    batches = []
    if len(probes) < len(names):
        batches.append((1, ["-E", pattern]))
    batches.append((2, ["-R", pattern]))
    return batches


def run(build, configuration):
    common = ["ctest", "--test-dir", str(build), "-C", configuration]
    inventory = subprocess.run(common + ["--show-only=json-v1"],
                               check=True, capture_output=True, text=True)
    batches = plan(json.loads(inventory.stdout)["tests"])
    failed = False
    for jobs, selection in batches:
        command = common + ["--parallel", str(jobs), "--no-tests=error",
                            "--output-on-failure"] + selection
        print("Running:", " ".join(command), flush=True)
        result = subprocess.run(command, check=False)
        failed |= result.returncode != 0
    return int(failed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--configuration", choices=("Debug", "Release"), required=True)
    args = parser.parse_args()
    try:
        return run(args.build, args.configuration)
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(2, f"macOS CTest batching failed: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
