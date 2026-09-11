#!/usr/bin/env python3
"""Private, bounded transport diagnostic overlay and fail-closed trace analysis.

Normal sources/ABI are untouched. The builder exports an exact clean revision
and instruments only that fresh copy, recording original/patched hashes. Never
compare its throughput with an uninstrumented build as a performance result.
"""
from __future__ import annotations

import io
import json
import subprocess
import zipfile
from collections import Counter
from pathlib import Path

import scalability_scorecard as sc

ROOT = Path(__file__).resolve().parents[1]

# Exact, unique anchors: fail rather than silently instrument a different path.
HOOKS = {
    "src/send_buffer.cpp": [
        ("    slot->retransmission_queued = true;\n",
         '    slot->retransmission_queued = true;\n    RWX_TRACE("queued", this, sequence.value());\n'),
        ("        header.retransmitted = true;\n",
         '        header.retransmitted = true;\n        RWX_TRACE("selected", this, sequence.value());\n'),
        ("    const Error validation = validate_retransmission_range(range);\n",
         '    RWX_TRACE("request_nak", this, range.first.value(), range.last.value());\n'
         "    const Error validation = validate_retransmission_range(range);\n"),
        ("bool SendBuffer::request_retransmission_of_last_sent() noexcept\n{\n",
         'bool SendBuffer::request_retransmission_of_last_sent() noexcept\n{\n    RWX_TRACE("request_tail", this);\n'),
        ("std::size_t SendBuffer::request_retransmission_of_all_sent() noexcept\n{\n",
         'std::size_t SendBuffer::request_retransmission_of_all_sent() noexcept\n{\n    RWX_TRACE("request_all", this);\n'),
    ],
    "src/session.cpp": [
        ("    , peer_socket_id_(configuration.peer_socket_id)\n{\n",
         '    , peer_socket_id_(configuration.peer_socket_id)\n{\n'
         '    RWX_TRACE("bind", this, reinterpret_cast<std::uintptr_t>(&send_buffer_),\n'
         '        configuration.local_initial_sequence.value(), peer_socket_id_);\n'),
        ("        // Repeated ACKs can update the receive window while a lost flight\n",
         '        RWX_TRACE("ack_state", this, decoded.acknowledgement.next_sequence.value(),\n'
         '            send_buffer_.packets_in_flight(), acknowledgement_progress > 0, now_microseconds);\n'
         "        // Repeated ACKs can update the receive window while a lost flight\n"),
        ("        return result;\n    }\n    case ControlType::negative_acknowledgement: {\n",
         '        RWX_TRACE("ack", this, decoded.acknowledgement.next_sequence.value(),\n'
         '            static_cast<std::uint64_t>(decoded.acknowledgement.kind),\n'
         '            rtt_.smoothed_microseconds(), rtt_.variation_microseconds());\n'
         '        RWX_TRACE("ack_peer", this, decoded.acknowledgement.next_sequence.value(),\n'
         '            decoded.acknowledgement.round_trip_time_microseconds,\n'
         '            decoded.acknowledgement.round_trip_time_variance_microseconds,\n'
         '            decoded.acknowledgement.available_receive_buffer_packets);\n'
         "        return result;\n    }\n    case ControlType::negative_acknowledgement: {\n"),
        ("            std::size_t newly_queued_packets = 0;\n",
         '            RWX_TRACE("nak", this, loss.range.first.value(), loss.range.last.value(),\n'
         '                send_buffer_.first_sequence().value(), now_microseconds);\n'
         "            std::size_t newly_queued_packets = 0;\n"),
        ("    if (!sender_retransmission_timer_.poll(\n",
         '    const auto diagnostic_deadline = sender_retransmission_timer_.next_deadline(\n'
         '        rtt_.smoothed_microseconds(), rtt_.variation_microseconds());\n'
         "    if (!sender_retransmission_timer_.poll(\n"),
        ("    // A periodic NAK can select only a gap exposed by later DATA. It cannot\n",
         '    RWX_TRACE("timer", this, diagnostic_deadline.value_or(0), now_microseconds,\n'
         '        rtt_.smoothed_microseconds(), rtt_.variation_microseconds());\n'
         '    RWX_TRACE("timer_state", this, send_buffer_.first_sequence().value(),\n'
         '        send_buffer_.packets_in_flight(), sender_retransmission_timer_.timeout_multiplier(),\n'
         '        periodic_nak_enabled_);\n'
         "    // A periodic NAK can select only a gap exposed by later DATA. It cannot\n"),
    ],
    "src/compat/transport_runtime.cpp": [
        ("    const std::size_t application_payload_size = authenticated_data\n",
         '    RWX_TRACE("wire_send", &session_, packet.header.sequence.value(),\n'
         '        packet.header.retransmitted, packet.payload.size(), now);\n'
         "    const std::size_t application_payload_size = authenticated_data\n"),
        ("    const auto processed = session_.receive(\n        clear_packet, now, context);\n",
         "    const auto processed = session_.receive(\n        clear_packet, now, context);\n"
         '    if (clear_packet.kind == PacketKind::data) {\n'
         '        RWX_TRACE("data_receive", &session_, clear_packet.data.sequence.value(),\n'
         '            clear_packet.data.retransmitted, static_cast<std::uint64_t>(processed.error), now);\n'
         '    }\n'),
    ],
}


def instrument(text: str, hooks: list[tuple[str, str]]) -> str:
    if "RWX_TRACE" in text:
        raise ValueError("source already instrumented")
    for before, after in hooks:
        if text.count(before) != 1:
            raise ValueError(f"diagnostic anchor missing or ambiguous: {before!r}")
        text = text.replace(before, after, 1)
    return text


def export_overlay(source: Path, revision: str, destination: Path) -> dict:
    """Export committed files only; never patch the user's checkout."""
    archive = subprocess.check_output(["git", "-C", str(source), "archive", "--format=zip", revision])
    destination.mkdir(exist_ok=False)
    with zipfile.ZipFile(io.BytesIO(archive)) as files:
        for item in files.infolist():
            path = Path(item.filename)
            if path.is_absolute() or ".." in path.parts or (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("unexpected path or symlink in source export")
        files.extractall(destination)
        for item in files.infolist():
            if not item.is_dir():
                (destination / item.filename).chmod(((item.external_attr >> 16) & 0o777) or 0o644)
    changes = {}
    for relative, hooks in HOOKS.items():
        path = destination / relative
        before = sc.file_sha256(path)
        prefix = "../" if "/compat/" in relative else ""
        text = f'#include "{prefix}diagnostic_transport_trace.hpp"\n' + instrument(path.read_text(), hooks)
        path.write_text(text)
        changes[relative] = {"original_sha256": before, "patched_sha256": sc.file_sha256(path)}
    header = ROOT / "benchmarks/diagnostics/transport_trace.hpp"
    (destination / "src/diagnostic_transport_trace.hpp").write_bytes(header.read_bytes())
    return {"enabled": True, "source_revision": revision, "source_export": str(destination),
            "changes": changes, "overlay_script": sc.program_identity(Path(__file__).resolve()),
            "collector_header": sc.program_identity(header), "capacity_per_thread": 524288}


def read_events(directory: Path) -> list[dict]:
    events = []
    paths = sorted(directory.glob("*.jsonl"))
    if not paths:
        raise ValueError("no transport trace files")
    for path in paths:
        lines = [json.loads(line) for line in path.read_text().splitlines()]
        if len(lines) < 2 or lines[0].get("schema") != 1 or lines[-1].get("end") is not True:
            raise ValueError(f"missing trace header/trailer: {path.name}")
        if lines[-1].get("dropped") != 0 or lines[-1].get("events") != len(lines) - 2:
            raise ValueError(f"overflow or incomplete trace: {path.name}")
        for index, event in enumerate(lines[1:-1]):
            if not all(isinstance(event.get(key), int) for key in ("ns", "object", "a", "b", "c", "d")):
                raise ValueError("invalid transport trace event")
            events.append({**event, "pid": lines[0]["pid"], "file": path.name, "index": index})
    return sorted(events, key=lambda e: (e["ns"], e["file"], e["index"]))


def analyze_events(events: list[dict], sender_pid: int, expected_originals: int,
                   expected_retransmissions: int) -> dict:
    mapping = {(e["pid"], e["a"]): e["object"] for e in events if e["kind"] == "bind"}
    causes, pending, selected, originals, latest_ack, latest_timer = {}, {}, {}, {}, {}, {}
    latest_nak, latest_ack_state = {}, {}
    rows, errors = [], []
    counts = Counter()
    wire_originals = []
    received = {}
    for e in events:
        if e["kind"] == "data_receive" and e["pid"] != sender_pid and e["c"] == 0:
            received.setdefault(e["a"], []).append(e)
        if e["pid"] != sender_pid:
            continue
        kind = e["kind"]
        buffer_event = kind.startswith("request_") or kind in ("queued", "selected")
        session = mapping.get((sender_pid, e["object"])) if buffer_event else e["object"]
        if session is None:
            errors.append("unmapped send buffer")
            continue
        key = (session, e["a"])
        counts[kind] += 1
        if kind.startswith("request_"):
            causes[session] = e
        elif kind == "queued":
            # Only a newly inserted queue entry has this event. Repeated NAKs
            # must NOT overwrite the first enqueue cause of a pending packet.
            cause = causes.get(session)
            if cause is None:
                errors.append("queue entry without request")
            trigger = latest_nak.get(session) if cause and cause["kind"] == "request_nak" else latest_timer.get(session)
            if trigger is None:
                errors.append("queue request without accepted NAK or elapsed timer")
            pending[key] = {"request": cause, "queued": e, "trigger": trigger}
        elif kind == "selected":
            selected[key] = pending.pop(key, None)
        elif kind == "ack":
            latest_ack[session] = e
        elif kind == "ack_state":
            latest_ack_state[session] = e
        elif kind == "nak":
            latest_nak[session] = e
        elif kind == "timer":
            latest_timer[session] = e
        elif kind == "wire_send":
            if not e["b"]:
                originals[key] = e
                wire_originals.append(e)
            else:
                lineage = selected.pop(key, None)
                if not lineage or not lineage.get("request") or key not in originals:
                    errors.append("retransmission without original/request/selection")
                rows.append({"sequence": e["a"], "session": session, "send": e,
                             "original": originals.get(key), "lineage": lineage,
                             "latest_ack": latest_ack.get(session), "ack_state": latest_ack_state.get(session)})
    if len(wire_originals) != expected_originals:
        errors.append(f"original coverage {len(wire_originals)} != {expected_originals}")
    if len(rows) != expected_retransmissions:
        errors.append(f"retransmission coverage {len(rows)} != {expected_retransmissions}")
    if not latest_ack:
        errors.append("no accepted sender ACKs")
    reasons = Counter()
    for row in rows:
        request = (row["lineage"] or {}).get("request")
        reason = request["kind"].removeprefix("request_") if request else "unknown"
        reasons[reason] += 1
        row["reason"] = reason
        row["receiver_observations"] = received.get(row["sequence"], [])
        ack = row["latest_ack"]
        row["ack_age_us"] = (row["send"]["ns"] - ack["ns"]) / 1000 if ack else None
        original = row["original"]
        row["original_age_us"] = (row["send"]["ns"] - original["ns"]) / 1000 if original else None
    return {"valid": not errors, "errors": sorted(set(errors)), "counts": dict(counts),
            "retransmission_reasons": dict(reasons), "retransmissions": rows,
            "original_packets": len(wire_originals), "retransmitted_packets": len(rows),
            "scope": "single cleartext connection; successful userspace UDP sends, not a network loss rate; Haivision receiver internals uninstrumented"}


def analyze_case(directory: Path, result: dict) -> dict:
    try:
        events = read_events(directory / "transport-trace")
        sender_pid = result["peer_process_resources"]["sender"]["pid"]
        caller = next(e for e in sc.parse_json_events(sc.read_output(directory / "many-socket-caller.stdout"))
                      if e.get("event") == "complete")
        stats = caller["stats"]
        analysis = analyze_events(events, sender_pid, stats["pktSentUniqueTotal"], stats["pktRetransTotal"])
        receiver_pid = result["peer_process_resources"]["receiver"]["pid"]
        if result["receiver_implementation"] == "robotweax":
            receiver_events = [e for e in events if e["pid"] == receiver_pid and e["kind"] == "data_receive" and e["c"] == 0]
            if len({e["a"] for e in receiver_events}) != result["messages_per_connection"]:
                analysis["errors"].append("incomplete Robotweax receive-sequence coverage")
                analysis["valid"] = False
        sc.write_report(directory / "retransmissions.json", analysis)
        return {key: value for key, value in analysis.items() if key != "retransmissions"}
    except (ValueError, KeyError, OSError, StopIteration) as error:
        return {"valid": False, "error": str(error)}
