#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare ARQ, Row, Column and Matrix recovery with the same impairment settings."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys

from process_support import Process, ports


def experiment(args, name, packet_filter, impaired):
    port = ports(2)
    executable = str(args.binary_dir / ("robotweax_srt_recovery.exe" if os.name == "nt" else "robotweax_srt_recovery"))
    common = ["--count", "320", "--interval-ms", "5", "--latency-ms", "200", "--crypto", args.crypto]
    if args.passphrase_env:
        common += ["--passphrase-env", args.passphrase_env]
    if packet_filter:
        common += ["--filter", packet_filter]
    processes = []
    def start(command):
        process = Process(command)
        processes.append(process)
        return process
    try:
        receiver = start([executable, "receive", "--endpoint", f"127.0.0.1:{port}", *common])
        receiver.ready()
        proxy = start([sys.executable, str(Path(__file__).with_name("impairment_proxy.py")),
            "--listen", f"127.0.0.1:{port + 1}", "--target", f"127.0.0.1:{port}", "--duration", "7",
            "--drop-every", str(args.drop_every if impaired else 0), "--burst", str(args.burst),
            "--delay-ms", str(args.delay_ms if impaired else 0),
            "--reorder-every", str(args.reorder_every if impaired else 0), "--reorder-ms", "30"])
        proxy.ready()
        sender = start([executable, "send", "--endpoint", f"127.0.0.1:{port + 1}", *common])
        sent, received = sender.finish(), receiver.finish()
        proxy_events = proxy.finish()
        summary = next(e for e in received if e["event"] == "summary")
        tx = [e for e in sent if e["event"] == "stats"][-1]
        rx = [e for e in received if e["event"] == "stats"][-1]
        return {"scenario": name, "filter": packet_filter or "none", "crypto": args.crypto,
            "impairment": {"drop_every": args.drop_every if impaired else 0, "burst": args.burst,
                "delay_ms": args.delay_ms if impaired else 0, "reorder_every": args.reorder_every if impaired else 0},
            "summary": summary, "sender_stats": tx, "receiver_stats": rx,
            "proxy": next(e for e in proxy_events if e["event"] == "proxy_summary")}
    finally:
        for process in processes:
            process.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--drop-every", type=int, default=32)
    parser.add_argument("--burst", type=int, default=1)
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--reorder-every", type=int, default=0)
    parser.add_argument("--crypto", choices=["ctr", "gcm"], default="ctr")
    parser.add_argument("--passphrase-env")
    parser.add_argument("--output", type=Path, help="write the full comparison as JSON")
    args = parser.parse_args()
    args.binary_dir = args.binary_dir.resolve()
    if args.drop_every < 0 or args.burst < 1 or (args.drop_every and args.burst > args.drop_every):
        parser.error("invalid drop period/burst")
    if not 0 <= args.delay_ms <= 100 or args.reorder_every < 0:
        parser.error("delay must be 0..100 ms; reorder period must be nonnegative")
    scenarios = [("baseline", "", False), ("arq", "", True),
        ("row", "fec,cols:4,rows:1,arq:never", True),
        ("column", "fec,cols:4,rows:-4,layout:even,arq:never", True),
        ("matrix", "fec,cols:4,rows:4,layout:even,arq:onreq", True)]
    results = []
    for name, packet_filter, impaired in scenarios:
        result = experiment(args, name, packet_filter, impaired)
        results.append(result)
        print(json.dumps(result), flush=True)
    if args.output:
        args.output.write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
