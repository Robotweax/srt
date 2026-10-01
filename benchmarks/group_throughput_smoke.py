#!/usr/bin/env python3
"""Real UDP integrity smoke for the group benchmark; no performance assertions."""
import argparse
from pathlib import Path

import group_throughput as g


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--peer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = {"complete": False, "profile": "integrity-smoke", "cases": []}
    try:
        directory = args.output.parent / (args.output.stem + "-logs")
        directory.mkdir(parents=True, exist_ok=True)
        for mode in ("broadcast", "backup"):
            for members in (1, 16, 17, 64):
                result = g.case(args.peer.resolve(), mode, members, 128, 1316,
                                directory / f"{mode}-{members}", 20)
                report["cases"].append({"mode": mode, "members": members, **result})
        report["complete"] = True
        return 0
    finally:
        g.write_json(args.output, report)


if __name__ == "__main__":
    raise SystemExit(main())
