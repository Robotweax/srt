#!/usr/bin/env python3
"""Validate and summarize a four-run sender-drop diagnostic result."""

import argparse
import itertools
import json
from pathlib import Path


RUNS = {
    "rep1": 1400,
    "rep2": 1400,
    "rep3": 1400,
    "rate2800": 2800,
}


def load_jsonl(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def fail(message):
    raise SystemExit(f"FAIL: {message}")


def qdisc_recovery(run):
    candidates = [
        snapshot
        for snapshot in run["kernelSnapshots"]
        if snapshot.get("phase") == "bandwidth-recovery"
    ]
    if len(candidates) < 2:
        fail("missing post-incident bandwidth recovery snapshot")
    return candidates[-1]


def neighboring_ack(acks, timestamp, before):
    candidates = [
        event for event in acks
        if (event["monotonicNs"] < timestamp) == before
    ]
    if not candidates:
        return None
    key = (lambda event: event["monotonicNs"])
    return max(candidates, key=key) if before else min(candidates, key=key)


def analyze_run(root, name, active_pps):
    directory = root / name
    if (directory / "runner.exit").read_text().strip() != "0":
        fail(f"runner failed: {name}")
    run = json.loads((directory / "run" / "run.json").read_text())
    sender_samples = load_jsonl(directory / "run" / "sender.jsonl")
    events = load_jsonl(directory / "sender-drop-trace.jsonl")
    submissions = [
        event for event in events
        if event["event"] == "udp-submit" and not event["retransmission"]
    ]
    acknowledgements = [event for event in events if event["event"] == "ack"]
    drops = [event for event in events if event["event"] == "tlpktdrop"]
    counter_events = [
        event for event in events
        if event["event"] == "tlpktdrop-counter"
    ]
    submission_by_sequence = {event["sequence"]: event for event in submissions}

    if len(submissions) != 2400 or len(submission_by_sequence) != 2400:
        fail(f"unexpected original UDP submissions: {name}")
    if run["payload"] != {
        "expectedMessages": 2400,
        "integrityVerified": True,
        "uniqueReceivedPackets": 2400,
        "uniqueSentPackets": 2400,
    }:
        fail(f"payload evidence is incomplete: {name}")
    if run["analyzer"]["totals"] != {
        "physicalSentPackets": 2400,
        "receiverLossPackets": 0,
        "retransmittedPackets": 0,
        "senderLossPackets": 0,
        "uniqueReceivedPackets": 2400,
        "uniqueSentPackets": 2400,
    }:
        fail(f"analyzer totals are incomplete: {name}")
    if not run["cleanup"]["verified"] or not run["cleanup"]["namespacesAbsent"]:
        fail(f"cleanup failed: {name}")
    for qdiscs in run["cleanup"]["qdiscReadback"].values():
        for qdisc in qdiscs:
            if qdisc.get("drops") != 0 or qdisc.get("requeues") != 0:
                fail(f"qdisc loss in {name}")

    missing_submissions = [
        event["sequence"] for event in drops
        if event["sequence"] not in submission_by_sequence
    ]
    if missing_submissions:
        fail(f"TLPKTDROP without prior UDP submission: {name}")
    if len({event["sequence"] for event in drops}) != len(drops):
        fail(f"duplicate TLPKTDROP sequence: {name}")
    if drops and min(event["ageUs"] for event in drops) < 1_020_000:
        fail(f"TLPKTDROP before the effective deadline: {name}")
    if active_pps == 2800 and drops:
        fail("doubled-rate control unexpectedly dropped retained packets")
    if active_pps == 1400 and not drops:
        fail(f"initial-rate run has no TLPKTDROP evidence: {name}")
    if sum(event["packetDelta"] for event in counter_events) != len(drops):
        fail(f"counter deltas do not match packet removals: {name}")
    public_drop_total = max(
        sample["raw"]["sender_drop_packet_total"]
        for sample in sender_samples
        if "raw" in sample
    )
    if public_drop_total != len(drops):
        fail(f"public sender-drop total does not match trace: {name}")

    batches = []
    for protocol_now, grouped in itertools.groupby(
        drops, key=lambda event: event["protocolNowUs"]
    ):
        values = list(grouped)
        batches.append({
            "protocolNowUs": protocol_now,
            "count": len(values),
            "firstSequence": values[0]["sequence"],
            "lastSequence": values[-1]["sequence"],
            "bufferedPacketsBefore": values[0]["bufferedPacketsBefore"],
            "packetsInFlightBefore": values[0]["packetsInFlightBefore"],
        })

    recovery = qdisc_recovery(run)
    sender_qdisc = recovery["namespaces"]["sender"]["qdisc"][0]
    first_drop_ns = drops[0]["monotonicNs"] if drops else None
    result = {
        "name": name,
        "activePacketsPerSecondEquivalent": active_pps,
        "udpSubmissions": len(submissions),
        "ackEvents": len(acknowledgements),
        "tlpktdropEvents": len(drops),
        "publicSenderDropPacketTotal": public_drop_total,
        "tlpktdropCounterEvents": counter_events,
        "allDroppedSequencesPreviouslySubmittedToUdp": not missing_submissions,
        "payloadIntegrityVerified": run["payload"]["integrityVerified"],
        "uniqueSentPackets": run["payload"]["uniqueSentPackets"],
        "uniqueReceivedPackets": run["payload"]["uniqueReceivedPackets"],
        "captureGate": run["captureGate"],
        "analyzer": run["analyzer"],
        "recovery": {
            "monotonicNs": recovery["monotonicNs"],
            "qdiscBacklogBytes": sender_qdisc["backlog"],
            "qdiscQueueLengthPackets": sender_qdisc["qlen"],
            "qdiscDrops": sender_qdisc["drops"],
            "qdiscRequeues": sender_qdisc["requeues"],
            "firstDropDelayNs": (
                first_drop_ns - recovery["monotonicNs"]
                if first_drop_ns is not None else None
            ),
        },
        "dropAgeUs": {
            "minimum": min((event["ageUs"] for event in drops), default=None),
            "maximum": max((event["ageUs"] for event in drops), default=None),
        },
        "dropBatches": batches,
    }
    if drops:
        result["sequenceRange"] = {
            "first": drops[0]["sequence"],
            "last": drops[-1]["sequence"],
        }
        result["ackImmediatelyBeforeFirstDrop"] = neighboring_ack(
            acknowledgements, first_drop_ns, True
        )
        result["ackImmediatelyAfterFirstDrop"] = neighboring_ack(
            acknowledgements, first_drop_ns, False
        )
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("result_root", type=Path)
    parser.add_argument("--output", type=Path)
    arguments = parser.parse_args()
    result = {
        "schemaVersion": 1,
        "runs": [
            analyze_run(arguments.result_root, name, active_pps)
            for name, active_pps in RUNS.items()
        ],
    }
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if arguments.output:
        arguments.output.write_text(rendered)
    else:
        print(rendered, end="")


if __name__ == "__main__":
    main()
