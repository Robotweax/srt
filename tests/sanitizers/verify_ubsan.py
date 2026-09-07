"""Require the configured UBSan canary to diagnose UB and terminate nonzero."""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess
import sys


def validation_error(control: subprocess.CompletedProcess[str],
                     overflow: subprocess.CompletedProcess[str]) -> str | None:
    if control.returncode != 0 or control.stdout or control.stderr:
        return "healthy control failed or produced unexpected output"
    if overflow.returncode == 0:
        return "overflow returned success: UBSan is absent or recoverable"
    if re.search(
        r"ubsan_canary\.cpp:\d+(?::\d+)?: runtime error: signed integer overflow",
        overflow.stderr,
    ) is None:
        return "nonzero exit without the expected UBSan overflow diagnostic"
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    arguments = parser.parse_args()
    executable = str(arguments.executable.resolve())
    try:
        control = subprocess.run(
            [executable], capture_output=True, text=True, timeout=10,
        )
        overflow = subprocess.run(
            [executable, "--overflow"], capture_output=True, text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL UBSan enforcement: {error}", file=sys.stderr)
        return 1
    error = validation_error(control, overflow)
    if error is not None:
        print(f"FAIL UBSan enforcement: {error}", file=sys.stderr)
        for name, result in (("control", control), ("overflow", overflow)):
            print(f"{name}: exit={result.returncode}\n"
                  f"{result.stdout}{result.stderr}", file=sys.stderr)
        return 1
    print(f"PASS UBSan enforcement: healthy exit=0, overflow exit="
          f"{overflow.returncode}, signed-overflow diagnostic verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
