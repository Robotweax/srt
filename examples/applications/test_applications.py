#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""End-to-end sample tests; every child is drained, bounded and cleaned up."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import sys


from process_support import Process, ports


def validate_summaries(scenario: str, summaries: list[dict], events: list[dict]):
    for summary in summaries:
        expected, unique = summary["expected"], summary["unique"]
        assert expected > 0 and 0 < unique <= expected, summary
        assert summary["missing"] == expected - unique, summary
        assert unique - 1 <= summary["highest_sequence"] < expected, summary
        assert summary["duplicates"] == 0, summary
        if scenario.startswith("device") and summary["channel"] == "sensor":
            # Sensor has a 100-ms TTL, a 20-ms gap deadline and no ARQ.
            # These are stream-health checks, not a loss-free delivery promise;
            # match the bounded Sensor socket test's majority/freshness checks.
            assert unique > expected // 2, summary
            assert summary["highest_sequence"] >= expected - max(4, expected // 8), summary
            if scenario == "device-impaired":
                assert unique < expected, summary
        else:
            assert unique == expected, summary
    if scenario.startswith("device"):
        sensor_stats = [e for e in events if e.get("event") == "stats" and e["channel"] == "sensor"]
        assert sensor_stats, events
        assert all(e["retransmitted"] == 0 for e in sensor_stats), sensor_stats


def run(binary_dir: Path, scenario: str, crypto="ctr"):
    suffix = ".exe" if os.name == "nt" else ""
    name = "contribution" if scenario.startswith("contribution") else "device" if scenario.startswith("device") else "recovery"
    executable = str(binary_dir / f"robotweax_srt_{name}{suffix}")
    base = ports(3)
    endpoint = f"127.0.0.1:{base}"
    common = ["--count", "160", "--interval-ms", "5", "--passphrase-env", "SRT_SAMPLE_SECRET", "--crypto", crypto]
    processes: list[Process] = []
    def start(command):
        process = Process(command, {"SRT_SAMPLE_SECRET": "synthetic-sample-test-passphrase"})
        processes.append(process)
        return process
    try:
        if name == "device":
            receiver = start([executable, "device", "--host", "127.0.0.1", "--port", str(base), *common])
            receiver.ready(3)
            operator_port = base
            if scenario == "device-impaired":
                operator_port = ports(3)
                for offset in range(3):
                    proxy = start([sys.executable, str(Path(__file__).with_name("impairment_proxy.py")),
                        "--listen", f"127.0.0.1:{operator_port + offset}", "--target", f"127.0.0.1:{base + offset}",
                        "--duration", "12", "--drop-every", "16", "--burst", "2" if offset == 1 else "1",
                        "--impair-direction", "forward" if offset == 2 else "reverse"])
                    proxy.ready()
            sender = start([executable, "operator", "--host", "127.0.0.1", "--port", str(operator_port), *common])
        elif name == "contribution":
            receiver = start([executable, "receive", "--endpoint", endpoint, *common])
            receiver.ready()
            sender = start([executable, "send", "--policy", "broadcast" if scenario.endswith("broadcast") else "backup",
                "--path", f"127.0.0.1:0,{endpoint},100", "--path", f"127.0.0.1:0,{endpoint},50",
                "--fail-after", "40", "--restore-after", "100", *common])
        else:
            impaired = scenario != "recovery"
            if scenario == "recovery-fec":
                common += ["--filter", "fec,cols:4,rows:1,arq:never"]
            receiver = start([executable, "receive", "--endpoint", endpoint, *common])
            receiver.ready()
            target = endpoint
            if impaired:
                target = f"127.0.0.1:{base + 1}"
                proxy = start([sys.executable, str(Path(__file__).with_name("impairment_proxy.py")),
                    "--listen", target, "--target", endpoint, "--duration", "8", "--drop-every", "16"])
                proxy.ready()
            sender = start([executable, "send", "--endpoint", target, *common])
        sent = sender.finish()
        received = receiver.finish()
        summaries = [e for e in sent + received if e.get("event") == "summary"]
        assert len(summaries) == (3 if name == "device" else 1), summaries
        validate_summaries(scenario, summaries, sent + received)
        if name == "device":
            control = next(e for e in summaries if e["channel"] == "control")
            assert control["executed"] == 8, control
            assert sum(e.get("event") == "executed_ack" for e in sent) == 8
        if name == "contribution":
            assert any(e.get("event") == "path_closed" for e in sent)
            assert any(e.get("event") == "path_rejoining" for e in sent)
            # Replacement must actually reach CONNECTED, not merely be requested.
            paths = [e for e in sent if e.get("event") == "path" and e["socket_state"] == 5]
            assert len({e["member"] for e in paths}) >= 3, paths
            snapshots = [e for e in sent if e.get("event") == "path_snapshot"]
            assert any(len(e["members"]) == 1 for e in snapshots), snapshots
            rejoin = next(i for i, e in enumerate(sent) if e.get("event") == "path_rejoining")
            restored = next(e for e in sent[rejoin + 1:] if e.get("event") == "path_snapshot")
            assert len(restored["members"]) == 2, restored
            assert {m["token"] for m in restored["members"]} == {1, 2}, restored
        if scenario == "recovery-fec":
            assert max(e.get("fec_supplied", 0) for e in received) > 0
        if scenario == "recovery-arq":
            assert max(e.get("retransmitted", 0) for e in sent) > 0
        print(json.dumps({"scenario": scenario, "crypto": crypto, "summaries": summaries}))
    except Exception:
        # Keep transport counters available when an acceptance assertion fails.
        for process in processes:
            print(process.output(), file=sys.stderr)
        raise
    finally:
        for process in processes:
            process.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary-dir", type=Path, required=True)
    parser.add_argument("--scenario", choices=["recovery", "recovery-fec", "recovery-arq", "contribution-backup", "contribution-broadcast", "device", "device-impaired"], required=True)
    parser.add_argument("--crypto", choices=["ctr", "gcm"], default="ctr")
    arguments = parser.parse_args()
    run(arguments.binary_dir.resolve(), arguments.scenario, arguments.crypto)
