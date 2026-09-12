#!/usr/bin/env python3
"""Exact-source aggregate-counter overlay; never a plain performance result."""
from __future__ import annotations

import io
import json
import re
import subprocess
import zipfile
from pathlib import Path

import scalability_scorecard as sc

ROOT = Path(__file__).resolve().parents[1]
SOURCE_REVISION = "8e1bdebed836cb7b732db852f51ef6a7b212e925"
HEADER = ROOT / "benchmarks/diagnostics/poll_wake_counters.hpp"
NAMES = tuple(re.findall(r"X\((\w+)\)", HEADER.read_text()))

# Each anchor must occur exactly once, in the unmodified pinned source.
HOOKS = {
    "src/compat/transport_runtime.cpp": [
        ("RuntimePollResult DatagramChannel::run_once() noexcept\n{\n",
         "RuntimePollResult DatagramChannel::run_once() noexcept\n{\n    RWX_COUNT(channel_polls);\n"),
        ("        const UdpIoResult received = socket.receive_from(datagram);\n",
         "        RWX_COUNT(rx_attempts);\n        const UdpIoResult received = socket.receive_from(datagram);\n"),
        ("        if (received.error == Error::would_block) {\n            break;\n",
         "        if (received.error == Error::would_block) {\n            RWX_COUNT(rx_eagain);\n            break;\n"),
        ("        if (received.error == Error::buffer_too_small) {\n            received_any = true;\n",
         "        if (received.error == Error::buffer_too_small) {\n            RWX_COUNT(rx_oversize);\n            received_any = true;\n"),
        ("        if (!received) {\n            mark_connections_broken(received.system_error);\n",
         "        if (!received) {\n            RWX_COUNT(rx_errors);\n            mark_connections_broken(received.system_error);\n"),
        ("        received_any = true;\n        const auto decoded = decode_packet(\n",
         "        RWX_COUNT(rx_datagrams);\n        received_any = true;\n        const auto decoded = decode_packet(\n"),
        ("        if (decoded) {\n            dispatch(decoded.packet,\n",
         "        if (decoded) {\n            RWX_COUNT(rx_decode_ok);\n"
         "            if (decoded.packet.kind == PacketKind::data) { RWX_COUNT(rx_data); }\n"
         "            else if (decoded.packet.control.type == ControlType::acknowledgement) { RWX_COUNT(rx_ack); }\n"
         "            dispatch(decoded.packet,\n"),
        ("    if (send_work || received_any) {\n",
         "    if (send_work || received_any) {\n        RWX_COUNT(channel_immediate_io);\n"),
        ("        // Queue the final sub-millisecond pacing slice behind other work on\n",
         "        RWX_COUNT(channel_immediate_pacer);\n        // Queue the final sub-millisecond pacing slice behind other work on\n"),
        ("    const auto delay = next_work_delay.has_value()\n",
         "    RWX_COUNT(channel_delayed);\n    const auto delay = next_work_delay.has_value()\n"),
        ("RuntimePollResult ConnectionRuntime::poll() noexcept\n{\n",
         "RuntimePollResult ConnectionRuntime::poll() noexcept\n{\n    diagnostics::PollScope rwx_poll_scope;\n"),
        ("                >= flow_window_packets_) {\n            break;\n",
         "                >= flow_window_packets_) {\n            RWX_COUNT(poll_flow_block);\n            break;\n"),
        ("            && !session_.has_pending_retransmission()) {\n            break;\n",
         "            && !session_.has_pending_retransmission()) {\n            RWX_COUNT(poll_crypto_block);\n            break;\n"),
        ("        if (!send_data(*packet, packet_time)) {\n            return {};\n        }\n",
         "        if (!send_data(*packet, packet_time)) {\n            return {};\n        }\n        ++rwx_poll_scope.packets;\n"),
        ("    const std::size_t application_payload_size = authenticated_data\n",
         "    if (packet.header.retransmitted) { RWX_COUNT(tx_retransmissions); }\n"
         "    else { RWX_COUNT(tx_originals); }\n"
         "    const std::size_t application_payload_size = authenticated_data\n"),
        ("std::uint64_t ConnectionRuntime::now_microseconds() const noexcept\n{\n",
         "std::uint64_t ConnectionRuntime::now_microseconds() const noexcept\n{\n    RWX_COUNT(runtime_now_calls);\n"),
        ("        ConnectionRuntime::Clock::now() - origin).count();\n",
         "        RWX_CLOCK(runtime_clock_reads, ConnectionRuntime::Clock::now()) - origin).count();\n"),
        ("void DatagramChannel::notify_send_work() noexcept\n{\n",
         "void DatagramChannel::notify_send_work() noexcept\n{\n    RWX_COUNT(channel_notify_calls);\n"),
        ("        if (!scheduled_timer_.valid()) {\n            if (task_active_) {\n",
         "        if (!scheduled_timer_.valid()) {\n            RWX_COUNT(channel_notify_coalesced);\n            if (task_active_) {\n"),
        ("            // The worker already owns the continuation. Record a follow-up in\n",
         "            RWX_COUNT(channel_notify_pending);\n            // The worker already owns the continuation. Record a follow-up in\n"),
    ],
    "src/session.cpp": [
        ("    std::size_t new_packet_wire_overhead) noexcept\n{\n",
         "    std::size_t new_packet_wire_overhead) noexcept\n{\n    RWX_COUNT(paced_selection_calls);\n"),
        ("    if (!pacer.query(now_microseconds, flow_count).ready) {\n",
         "    if (!pacer.query(now_microseconds, flow_count).ready) {\n        RWX_COUNT(selection_pacer_block);\n"),
        ("    auto packet = send_buffer_.next_packet();\n    if (packet.has_value()) {\n",
         "    auto packet = send_buffer_.next_packet();\n    if (!packet.has_value()) { RWX_COUNT(selection_empty); }\n    if (packet.has_value()) {\n"),
    ],
    "src/timing.cpp": [
        ("    return {\n        .ready = packets_in_flight < flow_window_packets_\n",
         "    RWX_COUNT(pacer_queries);\n"
         "    if (packets_in_flight >= flow_window_packets_) { RWX_COUNT(pacer_flow_block); }\n"
         "    else if (now_microseconds < next_send_microseconds_) { RWX_COUNT(pacer_time_block); }\n"
         "    return {\n        .ready = packets_in_flight < flow_window_packets_\n"),
    ],
    "src/compat/readiness.cpp": [
        ("void ReadinessSignal::notify() noexcept\n{\n",
         "void ReadinessSignal::notify() noexcept\n{\n    RWX_COUNT(readiness_notify_calls);\n"),
        ("    if (state.waiters.load() == 0U) {\n",
         "    if (state.waiters.load() == 0U) {\n        RWX_COUNT(readiness_no_waiters);\n"),
        ("    state.changed.notify_all();\n",
         "    RWX_COUNT(readiness_notify_all);\n    state.changed.notify_all();\n"),
        ("    state.waiters.fetch_add(1U);\n",
         "    state.waiters.fetch_add(1U);\n    RWX_COUNT(readiness_waits);\n"),
        ("    state.waiters.fetch_sub(1U);\n",
         "    RWX_COUNT(readiness_wait_returns);\n    state.waiters.fetch_sub(1U);\n"),
    ],
    "src/compat/runtime_scheduler.cpp": [
        ("    std::uint64_t affinity, Task task) noexcept\n{\n",
         "    std::uint64_t affinity, Task task) noexcept\n{\n    RWX_COUNT(scheduler_submit_calls);\n"),
        ("    shard.ready.notify_one();\n    return SubmitStatus::accepted;\n",
         "    RWX_COUNT(scheduler_submit_accepted);\n    RWX_COUNT(scheduler_notify_submit);\n"
         "    shard.ready.notify_one();\n    return SubmitStatus::accepted;\n"),
        ("    shard.ready.notify_one();\n    return {\n        .status = SubmitStatus::accepted,\n",
         "    RWX_COUNT(scheduler_notify_timer);\n    shard.ready.notify_one();\n    return {\n        .status = SubmitStatus::accepted,\n"),
        ("    shard.ready.notify_one();\n    return true;\n",
         "    RWX_COUNT(scheduler_notify_cancel);\n    shard.ready.notify_one();\n    return true;\n"),
        ("std::chrono::steady_clock::now() >= deadline",
         "RWX_CLOCK(scheduler_clock_reads, std::chrono::steady_clock::now()) >= deadline"),
        ("                    shard.ready.wait(lock);\n",
         "                    RWX_COUNT(scheduler_waits);\n                    shard.ready.wait(lock);\n                    RWX_COUNT(scheduler_wait_returns);\n"),
        ("                shard.ready.wait_until(lock, deadline);\n",
         "                RWX_COUNT(scheduler_waits);\n                shard.ready.wait_until(lock, deadline);\n                RWX_COUNT(scheduler_wait_returns);\n"),
        ("        task.function(task.context.get());\n",
         "        RWX_COUNT(scheduler_tasks);\n        task.function(task.context.get());\n"),
    ],
}


def instrument(text: str, hooks: list[tuple[str, str]]) -> str:
    if "RWX_COUNT" in text or "RWX_TRACE" in text:
        raise ValueError("source already instrumented")
    for before, after in hooks:
        if text.count(before) != 1:
            raise ValueError(f"poll-counter anchor missing or ambiguous: {before!r}")
        text = text.replace(before, after, 1)
    return text


def export_overlay(source: Path, revision: str, destination: Path) -> dict:
    if revision != SOURCE_REVISION:
        raise ValueError(f"poll counters require exact source {SOURCE_REVISION}")
    archive = subprocess.check_output(["git", "-C", str(source), "archive", "--format=zip", revision])
    destination.mkdir(exist_ok=False)
    with zipfile.ZipFile(io.BytesIO(archive)) as files:
        for item in files.infolist():
            path = Path(item.filename)
            if path.is_absolute() or ".." in path.parts or (item.external_attr >> 16) & 0o170000 == 0o120000:
                raise ValueError("unexpected source export path")
        files.extractall(destination)
        for item in files.infolist():
            if not item.is_dir():
                (destination / item.filename).chmod(((item.external_attr >> 16) & 0o777) or 0o644)
    changes = {}
    for relative, hooks in HOOKS.items():
        path = destination / relative
        before = sc.file_sha256(path)
        prefix = "../" if "/compat/" in relative else ""
        path.write_text(f'#include "{prefix}diagnostic_poll_counters.hpp"\n' + instrument(path.read_text(), hooks))
        changes[relative] = {"original_sha256": before, "patched_sha256": sc.file_sha256(path)}
    (destination / "src/diagnostic_poll_counters.hpp").write_bytes(HEADER.read_bytes())
    return {"enabled": True, "source_revision": revision, "source_export": str(destination),
            "changes": changes, "overlay_script": sc.program_identity(Path(__file__).resolve()),
            "collector_header": sc.program_identity(HEADER), "scope": "thread lifetime; no wait durations"}


def read_counters(directory: Path) -> list[dict]:
    rows, identities = [], set()
    for path in sorted(directory.glob("*.jsonl")):
        if path.is_symlink() or path.stat().st_size > 32768:
            raise ValueError("unexpected counter file")
        lines = [json.loads(line) for line in path.read_text().splitlines()]
        if len(lines) != 2:
            raise ValueError(f"missing registration or completion: {path.name}")
        header, row = lines
        if not isinstance(header, dict) or not isinstance(row, dict):
            raise ValueError("counter records must be objects")
        identity = tuple(row.get(k) for k in ("pid", "tid", "serial"))
        if (header.get("schema") != 1 or row.get("schema") != 1
                or any(type(v) is not int for v in identity)
                or identity[0] <= 0 or identity[1] <= 0 or identity[2] < 0
                or tuple(header.get(k) for k in ("pid", "tid", "serial")) != identity
                or identity in identities or row.get("complete") is not True
                or row.get("overflow") is not False):
            raise ValueError(f"invalid counter registration/completion: {path.name}")
        values = row.get("counters", {})
        if (not isinstance(values, dict) or set(values) != set(NAMES)
                or any(type(v) is not int or not 0 <= v < 2**64 for v in values.values())):
            raise ValueError("missing, unknown or invalid counter")
        if values["rx_attempts"] != sum(values[k] for k in ("rx_datagrams", "rx_eagain", "rx_oversize", "rx_errors")):
            raise ValueError("receive outcomes do not reconcile")
        if values["channel_polls"] != sum(values[k] for k in ("channel_immediate_io", "channel_immediate_pacer", "channel_delayed")):
            raise ValueError("channel outcomes do not reconcile")
        if values["connection_polls"] != sum(values[k] for k in NAMES if k.startswith("batch_")):
            raise ValueError("poll histogram does not reconcile")
        minimum = sum(values[k] * n for k, n in (("batch_1", 1), ("batch_2_4", 2),
                      ("batch_5_16", 5), ("batch_17_64", 17), ("batch_over_64", 65)))
        maximum = sum(values[k] * n for k, n in (("batch_1", 1), ("batch_2_4", 4),
                      ("batch_5_16", 16), ("batch_17_64", 64)))
        if values["poll_packets"] < minimum or (not values["batch_over_64"] and values["poll_packets"] > maximum):
            raise ValueError("packet count outside histogram bounds")
        if values["readiness_notify_calls"] != values["readiness_no_waiters"] + values["readiness_notify_all"]:
            raise ValueError("readiness outcomes do not reconcile")
        for prefix in ("readiness", "scheduler"):
            if values[prefix + "_waits"] != values[prefix + "_wait_returns"]:
                raise ValueError("unfinished condition-variable call")
        identities.add(identity)
        rows.append({**row, "file": path.name})
    return rows


def analyze_case(directory: Path, result: dict, profile: str) -> dict:
    try:
        for role in ("caller", "listener"):
            if "poll counters:" in sc.read_output(directory / f"many-socket-{role}.stderr"):
                raise ValueError("collector reported an output failure")
        rows = read_counters(directory / "poll-counters")
        implementations = sc.PROFILE_IMPLEMENTATIONS[profile]
        expected = {role: result["peer_process_resources"][role]["pid"]
                    for role, impl in zip(("sender", "receiver"), implementations) if impl == "robotweax"}
        if {r["pid"] for r in rows} != set(expected.values()):
            raise ValueError("missing Robotweax endpoint or unexpected PID")
        packets = result["wire_statistics"]["sender_packets_unique"]
        if type(packets) is not int or packets <= 0:
            raise ValueError("missing original packet denominator")
        endpoints = {}
        for role, pid in expected.items():
            own = [r for r in rows if r["pid"] == pid]
            totals = {k: sum(r["counters"][k] for r in own) for k in NAMES}
            if role == "sender":
                if (totals["tx_originals"] != packets or totals["tx_retransmissions"] != result["wire_statistics"]["retransmitted_packets"]
                        or totals["poll_packets"] != totals["tx_originals"] + totals["tx_retransmissions"]):
                    raise ValueError("sender packet counters do not reconcile with peer completion")
            elif totals["rx_data"] < result["wire_statistics"]["receiver_packets_unique"]:
                raise ValueError("receiver data coverage below peer completion")
            endpoints[role] = {"pid": pid, "threads": own, "totals": totals,
                               "per_original_packet": {k: v / packets for k, v in totals.items()}}
        return {"valid": True, "endpoints": endpoints, "reference_only": not expected,
                "scope": "registered thread lifetimes, including setup/shutdown; notify calls are not wakeups or wait durations"}
    except (OSError, ValueError, KeyError, TypeError) as error:
        return {"valid": False, "error": str(error)}


def measurement_contract(entry: dict) -> bool:
    """Evidence validity and the fixed loss-free transfer contract are separate."""
    if not entry.get("transfer_pass") or entry.get("profiling", {}).get("valid") is not True:
        return False
    result = entry.get("result", {})
    wire = result.get("wire_statistics", {})
    originals = wire.get("sender_packets_unique")
    expected = result.get("connections", 0) * result.get("messages_per_connection", 0)
    if (type(originals) is not int or originals <= 0 or originals != expected
            or wire.get("receiver_packets_unique") != originals
            or wire.get("sender_packets_total") != originals
            or wire.get("retransmitted_packets") != 0):
        return False
    delta = entry.get("system_udp_delta", {})
    return all(type(delta.get(k)) is int and delta[k] == 0
               for k in ("InErrors", "RcvbufErrors", "SndbufErrors", "InCsumErrors"))
