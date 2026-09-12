#!/usr/bin/env python3
"""Exact-pin timing overlay and strict, partial-aware worker histogram analysis."""
from __future__ import annotations

import hashlib
import io
import json
import re
import subprocess
import zipfile
from pathlib import Path

import scalability_scorecard as sc

ROOT = Path(__file__).resolve().parents[1]
REVISIONS = ("8e1bdebed836cb7b732db852f51ef6a7b212e925", "11814d3a1ebc217dbe95613b679960ac3152ff59")
HEADER = ROOT / "benchmarks/diagnostics/pacer_deadline_diagnostics.hpp"
_header = HEADER.read_text()
COUNTERS = tuple(re.findall(r"X\((\w+)\)", _header.split("#define RWX_DD_COUNTERS(X)")[1].split("#define RWX_DD_HISTOGRAMS")[0]))
METRICS = tuple(re.findall(r"X\((\w+)\)", _header.split("#define RWX_DD_HISTOGRAMS(X)")[1].split("// clang-format on")[0]))
BOUNDS = (0, 1, 2, 4, 16, 64, 256, 1000, 2000, 5000, 10000, 20000, 50000, 100000, 250000, 500000, 1000000, 10000000)
SOURCE_HASHES = {
    "8e1bdebed836cb7b732db852f51ef6a7b212e925": {
        "src/compat/runtime_scheduler.hpp": "4dfaaf4e47b6b33615ed7abd2013ba84e4f9aa046c9fb5a0022e66e4fcf70c70",
        "src/compat/runtime_scheduler.cpp": "b5aa3a2bd0d20df0f792200a5b592b89c958c8fc327aa28d000276b245b713ad",
        "src/compat/transport_runtime.cpp": "6522669e023052bf3e3ff2f6e3aa2542db8ef978d6b5f152bc9d2ce03cf13245",
        "src/session.cpp": "7de437efae425a4bdf78c76d292f2bfe88f6e0ff735b7c4a32289310f2d99a78"
    },
    "11814d3a1ebc217dbe95613b679960ac3152ff59": {
        "src/compat/runtime_scheduler.hpp": "4dfaaf4e47b6b33615ed7abd2013ba84e4f9aa046c9fb5a0022e66e4fcf70c70",
        "src/compat/runtime_scheduler.cpp": "2f62ea1a09bb5960c05d17c8a37d894866a909fa5229d73973cdaef31afc7139",
        "src/compat/transport_runtime.cpp": "50705ca278d650b319c7183e77805856c793160e889317011fd38baf6a7895fd",
        "src/session.cpp": "7de437efae425a4bdf78c76d292f2bfe88f6e0ff735b7c4a32289310f2d99a78"
    }
}

HOOKS = {
    "src/compat/runtime_scheduler.hpp": [
        ("        std::shared_ptr<void> context;\n", "        std::shared_ptr<void> context;\n"
         "        std::chrono::steady_clock::time_point diagnostic_submitted {};\n"
         "        std::chrono::steady_clock::time_point diagnostic_deadline {};\n"
         "        bool diagnostic_timed = false;\n"),
    ],
    "src/compat/runtime_scheduler.cpp": [
        ("        shard.entries[tail] = std::move(task);\n",
         "        task.diagnostic_submitted = std::chrono::steady_clock::now();\n"
         "        task.diagnostic_timed = false;\n        shard.entries[tail] = std::move(task);\n"),
        ("        const std::size_t slot = shard.add_timer(std::move(task), deadline);\n",
         "        task.diagnostic_submitted = std::chrono::steady_clock::now();\n"
         "        task.diagnostic_deadline = deadline;\n        task.diagnostic_timed = true;\n"
         "        const std::size_t slot = shard.add_timer(std::move(task), deadline);\n"),
        ("void RuntimeScheduler::run(std::size_t shard_index) noexcept\n{\n",
         "void RuntimeScheduler::run(std::size_t shard_index) noexcept\n{\n"
         "    rwx_dd::recorder().start(shard_index, shards_.size());\n"),
        ("                    shard.ready.wait(lock);\n",
         "                    { rwx_dd::WaitScope diagnostic_wait; shard.ready.wait(lock); }\n"),
        ("                shard.ready.wait_until(lock, deadline);\n",
         "                { rwx_dd::WaitScope diagnostic_wait(deadline, true);\n"
         "                  shard.ready.wait_until(lock, deadline); }\n"),
        ("        task.function(task.context.get());\n",
         "        { rwx_dd::TaskScope diagnostic_task(task.diagnostic_submitted,\n"
         "              task.diagnostic_deadline, task.diagnostic_timed);\n"
         "          task.function(task.context.get()); }\n"),
    ],
    "src/compat/transport_runtime.cpp": [
        ("RuntimePollResult DatagramChannel::run_once() noexcept\n{\n",
         "RuntimePollResult DatagramChannel::run_once() noexcept\n{\n    RWX_DD_COUNT(channel_polls);\n"),
        ("        const UdpIoResult received = socket.receive_from(datagram);\n",
         "        RWX_DD_COUNT(rx_attempts);\n        const UdpIoResult received = socket.receive_from(datagram);\n"),
        ("        if (received.error == Error::would_block) {\n            break;\n",
         "        if (received.error == Error::would_block) {\n            RWX_DD_COUNT(rx_eagain);\n            break;\n"),
        ("        received_any = true;\n        const auto decoded = decode_packet(\n",
         "        RWX_DD_COUNT(rx_datagrams);\n        received_any = true;\n        const auto decoded = decode_packet(\n"),
        ("RuntimePollResult ConnectionRuntime::poll() noexcept\n{\n",
         "RuntimePollResult ConnectionRuntime::poll() noexcept\n{\n    rwx_dd::PollScope diagnostic_poll;\n"),
        ("                >= flow_window_packets_) {\n            break;\n",
         "                >= flow_window_packets_) {\n            RWX_DD_COUNT(flow_block);\n            break;\n"),
        ("            && !session_.has_pending_retransmission()) {\n            break;\n",
         "            && !session_.has_pending_retransmission()) {\n            RWX_DD_COUNT(crypto_block);\n            break;\n"),
        ("        const auto packet = session_.next_paced_data_packet(\n",
         "        const auto diagnostic_target = pacer_.query(packet_time, 0U).next_ready_microseconds;\n"
         "        const auto packet = session_.next_paced_data_packet(\n"),
        ("        if (!send_data(*packet, packet_time)) {\n            return {};\n        }\n",
         "        if (!send_data(*packet, packet_time)) {\n            return {};\n        }\n"
         "        ++diagnostic_poll.packets;\n"
         "        rwx_dd::sent(deadline_from_origin_microseconds(origin_, diagnostic_target),\n"
         "            diagnostic_target != 0U, packet->header.retransmitted);\n"),
    ],
    "src/session.cpp": [
        ("    if (!pacer.query(now_microseconds, flow_count).ready) {\n",
         "    if (!pacer.query(now_microseconds, flow_count).ready) {\n        RWX_DD_COUNT(pacer_block);\n"),
        ("    auto packet = send_buffer_.next_packet();\n    if (packet.has_value()) {\n",
         "    auto packet = send_buffer_.next_packet();\n    if (!packet.has_value()) { RWX_DD_COUNT(selection_empty); }\n    if (packet.has_value()) {\n"),
    ],
}


def instrument(source: str, hooks: list[tuple[str, str]]) -> str:
    for old, new in hooks:
        if source.count(old) != 1:
            raise ValueError(f"missing/ambiguous diagnostic anchor: {old[:90]!r}")
        source = source.replace(old, new)
    return source


def patched_sources(source: Path, revision: str) -> dict[str, tuple[bytes, bytes]]:
    if revision not in REVISIONS:
        raise ValueError("deadline overlay accepts only the two reviewed library pins")
    result = {}
    for relative, hooks in HOOKS.items():
        original = subprocess.check_output(["git", "-C", str(source), "show", f"{revision}:{relative}"])
        if hashlib.sha256(original).hexdigest() != SOURCE_HASHES[revision][relative]:
            raise ValueError(f"source hash mismatch: {relative}")
        changed = instrument(original.decode(), hooks)
        if relative.endswith(".cpp"):
            changed = '#include "diagnostics/pacer_deadline_diagnostics.hpp"\nnamespace rwx_dd = robotweax::srt::deadline_diagnostics;\n' + changed
        result[relative] = (original, changed.encode())
    return result


def export_overlay(source: Path, revision: str, destination: Path) -> dict:
    patches = patched_sources(source, revision)
    raw = subprocess.check_output(["git", "-C", str(source), "archive", "--format=zip", revision])
    destination.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(io.BytesIO(raw)) as archive:
        for member in archive.infolist():
            if not (destination / member.filename).resolve().is_relative_to(destination.resolve()):
                raise ValueError("unsafe source archive path")
        archive.extractall(destination)
    installed = destination / "src/diagnostics/pacer_deadline_diagnostics.hpp"
    installed.parent.mkdir(parents=True, exist_ok=True)
    installed.write_bytes(HEADER.read_bytes())
    files = []
    for relative, (original, changed) in patches.items():
        path = destination / relative
        if path.read_bytes() != original:
            raise ValueError("export differs from reviewed source")
        path.write_bytes(changed)
        files.append({"path": relative, "original_sha256": hashlib.sha256(original).hexdigest(),
                      "patched_sha256": hashlib.sha256(changed).hexdigest()})
    return {"enabled": True, "source_revision": revision, "files": files,
            "overlay_script": sc.program_identity(Path(__file__).resolve()),
            "collector_header": sc.program_identity(HEADER),
            "scope": "worker-local timing histograms; private Task metadata; 1s checkpoints; not plain capacity"}


def integer(value) -> bool:
    return type(value) is int and 0 <= value <= 2**64 - 1


def validate_snapshot(row: dict) -> None:
    if not isinstance(row, dict):
        raise ValueError("snapshot is not an object")
    if (row.get("schema") != 1 or row.get("event") != "snapshot"
            or type(row.get("complete")) is not bool or not integer(row.get("elapsed_ns"))
            or not integer(row.get("ordinal")) or not 1 <= row["ordinal"] <= 129
            or row.get("overflow") is not False or row.get("checkpoint_limit") is not False):
        raise ValueError("invalid/overflowed snapshot")
    counts, hist = row.get("counters", {}), row.get("histograms", {})
    if set(counts) != set(COUNTERS) or any(not integer(v) for v in counts.values()) or set(hist) != set(METRICS):
        raise ValueError("missing/invalid counter or histogram")
    for name, h in hist.items():
        if (not isinstance(h, dict) or any(not integer(h.get(k)) for k in ("count", "sum", "max"))
                or not isinstance(h.get("buckets"), list) or len(h["buckets"]) != len(BOUNDS) + 1
                or any(not integer(v) for v in h["buckets"]) or sum(h["buckets"]) != h["count"]
                or h["sum"] > h["count"] * h["max"] or h["max"] > h["sum"]):
            raise ValueError(f"invalid histogram: {name}")
        minimum = sum(n * (0 if i == 0 else BOUNDS[i - 1] + 1) for i, n in enumerate(h["buckets"]))
        maximum = sum(n * (BOUNDS[i] if i < len(BOUNDS) else h["max"]) for i, n in enumerate(h["buckets"]))
        if not minimum <= h["sum"] <= maximum:
            raise ValueError(f"histogram sum outside bucket bounds: {name}")
        if h["count"]:
            last = max(i for i, n in enumerate(h["buckets"]) if n)
            if not (0 if last == 0 else BOUNDS[last - 1] + 1) <= h["max"] <= (BOUNDS[last] if last < len(BOUNDS) else 2**64 - 1):
                raise ValueError(f"histogram maximum outside populated bucket: {name}")
    if counts["original_packets"] + counts["retransmitted_packets"] != counts["data_packets"]:
        raise ValueError("DATA counter mismatch")
    tasks = counts["ready_tasks"] + counts["timer_tasks"]
    equalities = {"data_per_task": tasks, "task_elapsed_ns": tasks,
                  "ready_queue_ns": counts["ready_tasks"], "timer_queue_ns": counts["timer_tasks"],
                  "timer_dispatch_lateness_ns": counts["timer_tasks"],
                  "timer_remaining_on_submit_ns": counts["timer_tasks"],
                  "timer_overdue_on_submit_ns": counts["timer_tasks"],
                  "data_per_poll": counts["connection_polls"], "task_to_send_ns": counts["data_packets"],
                  "wait_requested_ns": counts["timed_wait_calls"],
                  "wait_elapsed_ns": counts["timed_wait_returns"], "wait_lateness_ns": counts["timed_wait_returns"],
                  "pacer_send_lateness_ns": counts["data_packets"] - counts["data_without_pacer_deadline"],
                  "pacer_lateness_at_task_start_ns": counts["data_packets"] - counts["data_without_pacer_deadline"],
                  "pacer_remaining_at_task_start_ns": counts["data_packets"] - counts["data_without_pacer_deadline"],
                  "pacer_send_earliness_ns": counts["data_packets"] - counts["data_without_pacer_deadline"]}
    if any(hist[k]["count"] != v for k, v in equalities.items()):
        raise ValueError("counter/histogram coverage mismatch")
    if hist["data_per_task"]["sum"] != counts["data_packets"] or hist["data_per_poll"]["sum"] != counts["data_packets"]:
        raise ValueError("DATA activation coverage mismatch")
    if counts["timed_wait_calls"] != counts["timed_wait_returns"] or counts["wait_calls"] != counts["wait_returns"]:
        raise ValueError("unbalanced completed waits")
    if counts["early_wait_returns"] > counts["timed_wait_returns"]:
        raise ValueError("early wait count mismatch")


def read_worker(path: Path) -> dict:
    if path.stat().st_size > 2 * 1024**2:
        raise ValueError("worker output exceeds diagnostic bound")
    rows, errors = [], []
    for line in path.read_text().splitlines():
        try:
            rows.append(json.loads(line))
        except ValueError:
            errors.append("malformed/truncated line")
            break
    if not rows or not isinstance(rows[0], dict):
        raise ValueError("missing worker registration")
    header = rows[0]
    if (header.get("schema") != 1 or header.get("event") != "register"
            or any(not integer(header.get(k)) for k in ("pid", "tid", "serial", "shard", "shards"))
            or not 1 <= header["shards"] <= 64 or header["shard"] >= header["shards"]
            or not header["pid"] or not header["tid"] or type(header.get("timer_slack_ns")) is not int
            or header["timer_slack_ns"] < -1):
        raise ValueError("invalid worker identity")
    last = None
    for row in rows[1:]:
        try:
            validate_snapshot(row)
            if row["ordinal"] != (last["ordinal"] + 1 if last else 1):
                raise ValueError("snapshot ordinal mismatch")
            if last and (last["complete"] or row["elapsed_ns"] < last["elapsed_ns"]
                         or any(row["counters"][k] < last["counters"][k] for k in COUNTERS)):
                raise ValueError("nonmonotonic/extra snapshot")
            if last and any(row["histograms"][k][field] < last["histograms"][k][field]
                            for k in METRICS for field in ("count", "sum", "max")):
                raise ValueError("nonmonotonic histogram")
            if last and any(new < old for k in METRICS for new, old in
                            zip(row["histograms"][k]["buckets"], last["histograms"][k]["buckets"])):
                raise ValueError("nonmonotonic histogram bucket")
            last = row
        except (ValueError, TypeError, KeyError) as error:
            errors.append(str(error))
            break
    if last is None or not last["complete"]:
        errors.append("missing final worker snapshot; earlier checkpoint is partial")
    return {"file": path.name, "identity": header, "last_snapshot": last,
            "complete": not errors, "errors": errors}


def aggregate_histograms(workers: list[dict]) -> dict:
    result = {}
    for name in METRICS:
        values = [w["last_snapshot"]["histograms"][name] for w in workers if w["last_snapshot"]]
        h = {"count": sum(v["count"] for v in values), "sum": sum(v["sum"] for v in values),
             "max": max((v["max"] for v in values), default=0),
             "buckets": [sum(v["buckets"][i] for v in values) for i in range(len(BOUNDS) + 1)]}
        h["mean"] = h["sum"] / h["count"] if h["count"] else None
        h["unit"] = "packets" if name.startswith("data_per_") else "nanoseconds"
        h["quantile_intervals"] = {}
        for percent in (50, 95, 99):
            interval, cumulative = None, 0
            for i, n in enumerate(h["buckets"]):
                cumulative += n
                if h["count"] and cumulative * 100 >= h["count"] * percent:
                    interval = [0 if i == 0 else BOUNDS[i - 1] + 1, BOUNDS[i] if i < len(BOUNDS) else None]
                    break
            h["quantile_intervals"][str(percent)] = interval
        result[name] = h
    return result


def analyze_case(directory: Path, result: dict) -> dict:
    workers, errors = [], []
    for path in sorted((directory / "pacer-deadline").glob("*.jsonl")):
        try:
            workers.append(read_worker(path))
        except (ValueError, TypeError, KeyError, OSError) as error:
            errors.append(f"{path.name}: {error}")
    if not workers:
        errors.append("no registered scheduler workers")
    identities = [(w["identity"]["pid"], w["identity"]["tid"], w["identity"]["serial"]) for w in workers]
    if len(identities) != len(set(identities)):
        errors.append("duplicate worker identity")
    errors += [f"{w['file']}: {error}" for w in workers for error in w["errors"]]
    endpoints = {}
    expected_pids = set()
    for role in ("sender", "receiver"):
        if result.get(f"{role}_implementation") != "robotweax":
            continue
        pid = (result.get("peer_process_resources", {}).get(role) or {}).get("pid")
        if not integer(pid) or pid == 0:
            errors.append(f"missing {role} PID; retain partial worker data without attribution")
            continue
        expected_pids.add(pid)
        own = [w for w in workers if w["identity"]["pid"] == pid]
        if not own:
            errors.append(f"no workers for {role} PID {pid}")
        elif (len({w["identity"]["shards"] for w in own}) != 1
              or len(own) != own[0]["identity"]["shards"]
              or {w["identity"]["shard"] for w in own} != set(range(own[0]["identity"]["shards"]))):
            errors.append(f"missing/duplicate shard coverage for {role}")
        totals = {k: sum(w["last_snapshot"]["counters"][k] for w in own if w["last_snapshot"]) for k in COUNTERS}
        originals = totals["original_packets"]
        endpoints[role] = {"pid": pid, "workers": len(own), "totals": totals,
                           "per_original": {k: v / originals if originals else None for k, v in totals.items()},
                           "histograms": aggregate_histograms(own)}
        if role == "sender":
            wire = result.get("wire_statistics", {})
            if (not integer(wire.get("sender_packets_unique")) or not wire["sender_packets_unique"]
                    or not integer(wire.get("retransmitted_packets"))
                    or totals["original_packets"] != wire.get("sender_packets_unique")
                    or totals["retransmitted_packets"] != wire.get("retransmitted_packets")):
                errors.append("sender diagnostic/public packet coverage mismatch")
    if not expected_pids:
        errors.append("missing completed endpoint identities; diagnostics remain partial")
    elif {w["identity"]["pid"] for w in workers} != expected_pids:
        errors.append("unexpected/missing Robotweax process")
    return {"valid": not errors, "errors": errors, "workers": workers, "endpoints": endpoints,
            "histogram_inclusive_bounds": list(BOUNDS), "last_bucket_unbounded": True,
            "snapshot_scope": "whole worker lifetime through last recorded callback; partial != completion",
            "timing_scope": "send timestamp is after successful send_data return; includes send bookkeeping",
            "instrumented_rates_are_not_plain_capacity": True}
