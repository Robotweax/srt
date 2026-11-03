#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reject invalid ARQ-first demo configurations before opening a connection."""
import argparse
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--message", required=True)
    parser.add_argument("--recovery", required=True)
    parser.add_argument("--bridge", required=True)
    args = parser.parse_args()
    cases = [
        [args.message, "caller", "--port", "9000", "--profile", "sensor", "--arq-first"],
        [args.message, "caller", "--port", "9000", "--profile", "control", "--arq-first"],
        [args.message, "rendezvous", "--port", "9000", "--local-port", "9001", "--arq-first"],
        [args.message, "caller", "--port", "9000", "--packet-filter",
         "fec,cols:8,rows:8,arq:onreq", "--arq-first"],
        [args.recovery, "receive", "--arq-first"],
        [args.recovery, "send", "--filter", "fec,cols:8,rows:1,arq:always", "--arq-first"],
        [args.bridge, "receive", "--udp-port", "9001", "--srt-port", "9000", "--arq-first"],
        [args.bridge, "send", "--udp-port", "9001", "--srt-port", "9000",
         "--srt-mode", "rendezvous", "--srt-local-port", "9002", "--arq-first"],
    ]
    for command in cases:
        result = subprocess.run(command, capture_output=True, text=True, timeout=2)
        if result.returncode == 0 or "--arq-first" not in result.stderr:
            raise RuntimeError(f"unexpected option result: {command!r}: {result}")
    print(f"{len(cases)} invalid ARQ-first configurations rejected before connect")


if __name__ == "__main__":
    main()
