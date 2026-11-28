#!/usr/bin/env python3
"""Check an observed SRT library or archive against trusted artifact bytes."""

import argparse
import hashlib
import json
from pathlib import Path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as artifact:
        for block in iter(lambda: artifact.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--observed", type=Path, required=True)
    expected = parser.add_mutually_exclusive_group(required=True)
    expected.add_argument("--expected", type=Path)
    expected.add_argument("--sha256", dest="digest")
    args = parser.parse_args()
    try:
        observed = args.observed.resolve(strict=True)
        actual = sha256(observed)
        trusted = sha256(args.expected) if args.expected else args.digest.lower()
    except OSError as error:
        parser.exit(2, f"{error}\n")
    if len(trusted) != 64 or any(char not in "0123456789abcdef" for char in trusted):
        parser.error("--sha256 requires a 64-digit hexadecimal digest")
    match = actual == trusted
    print(json.dumps({"observed": str(observed), "observed_sha256": actual,
                      "expected_sha256": trusted, "match": match}, indent=2))
    return 0 if match else 1


if __name__ == "__main__":
    raise SystemExit(main())
