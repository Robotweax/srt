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


def validate_drop_mapping(submissions, drops, counters, name):
    """Prove submission-before-removal and one exact counter per removed range."""
    submitted = {event["sequence"]: event for event in submissions}
    removed = {event["sequence"]: event for event in drops}
    if len(removed) != len(drops):
        fail(f"duplicate TLPKTDROP sequence: {name}")
    for drop in drops:
        submit = submitted.get(drop["sequence"])
        if (submit is None or submit["monotonicNs"] <= 0
                or submit["monotonicNs"] >= drop["monotonicNs"]):
            fail(f"TLPKTDROP without prior UDP submission: {name}")
        if (drop["enqueueUs"] > drop["cutoffUs"]
                or drop["protocolNowUs"] < drop["enqueueUs"]
                or drop["ageUs"] != drop["protocolNowUs"] - drop["enqueueUs"]):
            fail(f"inconsistent drop age/cutoff: {name}")
    matched = set()
    mask = (1 << 31) - 1
    for counter in counters:
        first, last = counter["firstSequence"], counter["lastSequence"]
        if not (0 <= first <= mask and 0 <= last <= mask):
            fail(f"invalid counter sequence: {name}")
        count = ((last - first) & mask) + 1
        if count > len(drops) or count != counter["packetDelta"]:
            fail(f"counter range/count mismatch: {name}")
        payload_bytes = 0
        for offset in range(count):
            sequence = (first + offset) & mask
            drop = removed.get(sequence)
            if drop is None or sequence in matched:
                fail(f"counter range missing or overlapping removals: {name}")
            if (drop["protocolNowUs"] != counter["protocolNowUs"]
                    or drop["monotonicNs"] > counter["monotonicNs"]):
                fail(f"counter precedes removal or uses another poll: {name}")
            matched.add(sequence)
            payload_bytes += drop["payloadBytes"]
        if payload_bytes != counter["payloadByteDelta"]:
            fail(f"counter byte delta mismatch: {name}")
    if matched != set(removed):
        fail(f"removals without exact counter coverage: {name}")


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

    validate_drop_mapping(submissions, drops, counter_events, name)
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
        "allDroppedSequencesPreviouslySubmittedToUdp": True,
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
