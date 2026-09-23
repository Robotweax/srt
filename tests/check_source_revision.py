#!/usr/bin/env python3
"""Reject an integration source checkout outside its qualified revision."""
import argparse
from pathlib import Path
import subprocess


def verify(source: Path, revision: str) -> None:
    actual = subprocess.run(
        ["git", "-C", str(source), "rev-parse", "HEAD"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()
    if actual != revision:
        raise RuntimeError(f"unqualified source revision at {source}: expected {revision}, got {actual}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("revision")
    args = parser.parse_args()
    try:
        verify(args.source, args.revision)
    except (RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"source revision check failed: {error}\n")
