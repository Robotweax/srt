#!/usr/bin/env python3
"""Exact A/B diagnostic exports and reconciled worker counters; never plain rates."""
from __future__ import annotations
import io, json, re, subprocess, zipfile
from pathlib import Path
import scalability_scorecard as sc

ROOT = Path(__file__).resolve().parents[1]
REVISIONS = {
    "baseline": "8e1bdebed836cb7b732db852f51ef6a7b212e925",
    "candidate": "9207a1be4c3156eb2866bc00dbbd883ec38c1431",
}
HEADER = ROOT / "benchmarks/diagnostics/continuation_counters.hpp"
NAMES = tuple(re.findall(r"X\((\w+)\)", HEADER.read_text()))
RUNTIME = "src/compat/transport_runtime.cpp"
SCHEDULER = "src/compat/runtime_scheduler.cpp"
HELPER = "src/compat/paced_poll_continuation.hpp"
COMMON = [
    (
        "        const UdpIoResult received = socket.receive_from(datagram);\n",
        "        RWX_COUNT(rx_attempts);\n        const UdpIoResult received = socket.receive_from(datagram);\n",
    ),
    (
        "        if (received.error == Error::would_block) {\n            break;",
        "        if (received.error == Error::would_block) {\n            RWX_COUNT(rx_eagain);\n            break;",
    ),
    (
        "        if (received.error == Error::buffer_too_small) {\n            received_any = true;",
        "        if (received.error == Error::buffer_too_small) {\n            RWX_COUNT(rx_oversize);\n            received_any = true;",
    ),
    (
        "        if (!received) {\n            mark_connections_broken(received.system_error);",
        "        if (!received) {\n            RWX_COUNT(rx_errors);\n            mark_connections_broken(received.system_error);",
    ),
    (
        "        received_any = true;\n        const auto decoded = decode_packet(",
        "        RWX_COUNT(rx_datagrams);\n        received_any = true;\n        const auto decoded = decode_packet(",
    ),
    (
        "        if (decoded) {\n            dispatch(decoded.packet,",
        "        if (decoded) {\n            RWX_COUNT(rx_decode_ok);\n            if (decoded.packet.kind == PacketKind::data) { RWX_COUNT(rx_data); }\n            dispatch(decoded.packet,",
    ),
    (
        "    if (send_work || received_any) {\n",
        "    if (send_work || received_any) {\n        RWX_COUNT(channel_immediate_io);\n",
    ),
    (
        "        // Queue the final sub-millisecond pacing slice behind other work on",
        "        RWX_COUNT(channel_immediate_pacer);\n        // Queue the final sub-millisecond pacing slice behind other work on",
    ),
    (
        "    const auto delay = next_work_delay.has_value()",
        "    RWX_COUNT(channel_delayed);\n    const auto delay = next_work_delay.has_value()",
    ),
    (
        "RuntimePollResult ConnectionRuntime::poll() noexcept\n{\n",
        "RuntimePollResult ConnectionRuntime::poll() noexcept\n{\n    diagnostics::PollScope diagnostic_scope;\n",
    ),
    (
        "        if (!send_data(*packet, packet_time)) {\n            return {};\n        }\n",
        "        if (!send_data(*packet, packet_time)) {\n            return {};\n        }\n        ++diagnostic_scope.packets;\n",
    ),
    (
        "    const std::size_t application_payload_size = authenticated_data",
        "    if (packet.header.retransmitted) { RWX_COUNT(tx_retransmissions); }\n    else { RWX_COUNT(tx_originals); }\n    const std::size_t application_payload_size = authenticated_data",
    ),
]
BASELINE = [
    (
        "RuntimePollResult DatagramChannel::run_once() noexcept\n{\n",
        "RuntimePollResult DatagramChannel::run_once() noexcept\n{\n    RWX_COUNT(channel_calls);\n    RWX_COUNT(channel_full_polls);\n",
    )
]
CANDIDATE = [
    (
        "    const PacedPollContinuation::Time* test_now) noexcept\n{\n",
        "    const PacedPollContinuation::Time* test_now) noexcept\n{\n    RWX_COUNT(channel_calls);\n    if (!paced_poll_continuation_.pending()) { paced_poll_continuation_.diagnostic_missing_hint(); }\n",
    ),
    (
        "        // Yield through A's ready queue exactly once.",
        "        RWX_COUNT(channel_skips);\n        // Yield through A's ready queue exactly once.",
    ),
    (
        "    std::array<std::byte, 1500> datagram {};\n    bool received_any = false;",
        "    RWX_COUNT(channel_full_polls);\n    std::array<std::byte, 1500> datagram {};\n    bool received_any = false;",
    ),
    (
        "        const bool isolated = routes_.size() == 1U && setup_routes_.empty()\n            && listener_inbox_.expired();",
        "        const bool isolated = routes_.size() == 1U && setup_routes_.empty()\n            && listener_inbox_.expired();\n        if (routes_.empty()) { RWX_COUNT(route_none); }\n        else if (routes_.size() != 1U) { RWX_COUNT(route_multiple); }\n        else if (!setup_routes_.empty()) { RWX_COUNT(route_setup); }\n        else if (!listener_inbox_.expired()) { RWX_COUNT(route_listener); }\n        else { RWX_COUNT(route_isolated); }",
    ),
    (
        "        if (paced_poll_deadline.has_value()\n            && paced_poll_epoch_.load(std::memory_order_acquire) == epoch) {",
        "        if (!paced_poll_deadline.has_value()) { RWX_COUNT(arm_missing_hint); }\n        else if (paced_poll_epoch_.load(std::memory_order_acquire) != epoch) { RWX_COUNT(arm_epoch_changed); }\n        else {",
    ),
]
HELPER_HOOKS = [
    (
        "        pending_ = false;\n        if (pacing_deadline <= now || now >= Time::max() - maximum_deferral) {",
        "        diagnostic_spent_ = false;\n        RWX_COUNT(arm_calls);\n        if (pacing_deadline <= now) { RWX_COUNT(arm_due); }\n        else if (now >= Time::max() - maximum_deferral) { RWX_COUNT(arm_overflow); }\n        pending_ = false;\n        if (pacing_deadline <= now || now >= Time::max() - maximum_deferral) {",
    ),
    (
        "        pending_ = true;\n",
        "        pending_ = true;\n        RWX_COUNT(arm_success);\n",
    ),
    (
        "        const bool skip =\n            pending_ && epoch == epoch_ && now >= since_ && now < until_;",
        """        RWX_COUNT(take_calls);
        if (pending_) {
            if (now < since_) { RWX_COUNT(age_rollback); }
            else if (now < since_ + std::chrono::microseconds {1}) { RWX_COUNT(age_lt_1us); }
            else if (now < since_ + std::chrono::microseconds {2}) { RWX_COUNT(age_lt_2us); }
            else if (now < since_ + maximum_deferral) { RWX_COUNT(age_lt_5us); }
            else { RWX_COUNT(age_ge_5us); }
        }
        if (!pending_) { RWX_COUNT(take_no_pending); }
        else if (epoch != epoch_) { RWX_COUNT(take_epoch_changed); }
        else if (now < since_) { RWX_COUNT(take_rollback); }
        else if (now >= until_) {
            if (until_ == since_ + maximum_deferral) { RWX_COUNT(take_expired_cap); }
            else { RWX_COUNT(take_expired_pacer); }
        } else { RWX_COUNT(take_success); }
        const bool skip =
            pending_ && epoch == epoch_ && now >= since_ && now < until_;
        diagnostic_spent_ = skip;""",
    ),
    (
        "    [[nodiscard]] bool pending() const noexcept",
        "    void diagnostic_missing_hint() noexcept\n    {\n        if (diagnostic_spent_) { RWX_COUNT(channel_after_skip); }\n        else { RWX_COUNT(channel_no_hint); }\n        diagnostic_spent_ = false;\n    }\n\n    [[nodiscard]] bool pending() const noexcept",
    ),
    (
        "    bool pending_ = false;",
        "    bool pending_ = false;\n    bool diagnostic_spent_ = false;",
    ),
]


def hooks(revision):
    if revision not in REVISIONS.values():
        raise ValueError("unreviewed continuation source revision")
    return {
        RUNTIME: COMMON
        + (BASELINE if revision == REVISIONS["baseline"] else CANDIDATE),
        SCHEDULER: [
            (
                "void RuntimeScheduler::run(std::size_t shard_index) noexcept\n{\n",
                "void RuntimeScheduler::run(std::size_t shard_index) noexcept\n{\n    diagnostics::continuation_counters().worker(shard_index);\n",
            )
        ],
        **({HELPER: HELPER_HOOKS} if revision == REVISIONS["candidate"] else {}),
    }


def instrument(text, changes):
    if "RWX_COUNT" in text or "diagnostic_continuation_counters" in text:
        raise ValueError("source already instrumented")
    for before, after in changes:
        if text.count(before) != 1:
            raise ValueError(
                "missing or ambiguous continuation anchor: " + repr(before)
            )
        text = text.replace(before, after, 1)
    return '#include "../diagnostic_continuation_counters.hpp"\n' + text


# Full unmodified source identities, in addition to unique anchors.
PIN_HASHES = {
    "8e1bdebed836cb7b732db852f51ef6a7b212e925": {
        "src/compat/transport_runtime.cpp": "6522669e023052bf3e3ff2f6e3aa2542db8ef978d6b5f152bc9d2ce03cf13245",
        "src/compat/runtime_scheduler.cpp": "b5aa3a2bd0d20df0f792200a5b592b89c958c8fc327aa28d000276b245b713ad",
    },
    "9207a1be4c3156eb2866bc00dbbd883ec38c1431": {
        "src/compat/transport_runtime.cpp": "723713fdf46e354238d04fd53e52bb60192a426d4ff928840895efa5fdda5cea",
        "src/compat/runtime_scheduler.cpp": "b5aa3a2bd0d20df0f792200a5b592b89c958c8fc327aa28d000276b245b713ad",
        "src/compat/paced_poll_continuation.hpp": "523c2226793983a66f27ea0e6b639e27c0d1c118faab1e32a55d22a5cdef6672",
    },
}
EXPECTED_CHANGES = {
    "8e1bdebed836cb7b732db852f51ef6a7b212e925": {
        "src/compat/transport_runtime.cpp": {
            "original_sha256": "6522669e023052bf3e3ff2f6e3aa2542db8ef978d6b5f152bc9d2ce03cf13245",
            "patched_sha256": "153acb132912d3b48522d868d1d01f81257010f1b8fae5d4eef516dfe67cca64",
        },
        "src/compat/runtime_scheduler.cpp": {
            "original_sha256": "b5aa3a2bd0d20df0f792200a5b592b89c958c8fc327aa28d000276b245b713ad",
            "patched_sha256": "1e5ba434846859894cfd4fe3114adaea635be563b029a218491347dfbfb92880",
        },
    },
    "9207a1be4c3156eb2866bc00dbbd883ec38c1431": {
        "src/compat/transport_runtime.cpp": {
            "original_sha256": "723713fdf46e354238d04fd53e52bb60192a426d4ff928840895efa5fdda5cea",
            "patched_sha256": "2dfe8d02b3b9c937101639bc06cf396cf1ef6eb74891017b932adc5c363f0bf5",
        },
        "src/compat/runtime_scheduler.cpp": {
            "original_sha256": "b5aa3a2bd0d20df0f792200a5b592b89c958c8fc327aa28d000276b245b713ad",
            "patched_sha256": "1e5ba434846859894cfd4fe3114adaea635be563b029a218491347dfbfb92880",
        },
        "src/compat/paced_poll_continuation.hpp": {
            "original_sha256": "523c2226793983a66f27ea0e6b639e27c0d1c118faab1e32a55d22a5cdef6672",
            "patched_sha256": "750bae76268c5fedd181c02f2d95f36d3b73f4506b6ed22ee92f9fc58ea8a9de",
        },
    },
}


def export_overlay(source, revision, destination):
    changes = hooks(revision)
    archive = subprocess.check_output(
        ["git", "-C", str(source), "archive", "--format=zip", revision]
    )
    destination.mkdir(exist_ok=False)
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        for item in z.infolist():
            p = Path(item.filename)
            if (
                p.is_absolute()
                or ".." in p.parts
                or (item.external_attr >> 16) & 0o170000 == 0o120000
            ):
                raise ValueError("unexpected export path")
        z.extractall(destination)
        for item in z.infolist():
            if not item.is_dir():
                (destination / item.filename).chmod(
                    ((item.external_attr >> 16) & 0o777) or 0o644
                )
    patched = {}
    for relative, entries in changes.items():
        p = destination / relative
        before = sc.file_sha256(p)
        if before != PIN_HASHES[revision][relative]:
            raise ValueError("unreviewed source bytes: " + relative)
        p.write_text(instrument(p.read_text(), entries))
        patched[relative] = {
            "original_sha256": before,
            "patched_sha256": sc.file_sha256(p),
        }
    target = destination / "src/diagnostic_continuation_counters.hpp"
    target.write_bytes(HEADER.read_bytes())
    return {
        "enabled": True,
        "source_revision": revision,
        "source_export": str(destination),
        "changes": patched,
        "overlay_script": sc.program_identity(Path(__file__).resolve()),
        "collector_header": sc.program_identity(HEADER),
        "scope": "worker lifetime including setup/shutdown; no added clock reads; instrumented rates are not plain rates",
    }


def read_counters(directory):
    rows = []
    seen = set()
    if not directory.is_dir():
        raise ValueError("missing continuation counter directory")
    for path in sorted(directory.iterdir()):
        if (
            path.is_symlink()
            or not path.is_file()
            or path.suffix != ".jsonl"
            or path.stat().st_size > 32768
        ):
            raise ValueError("unexpected counter file")
        lines = [json.loads(s) for s in path.read_text().splitlines()]
        if len(lines) != 2 or not all(isinstance(x, dict) for x in lines):
            raise ValueError("missing registration/completion")
        header, row = lines
        identity = tuple(row.get(k) for k in ["pid", "tid", "serial"])
        if (
            header.get("schema") != 2
            or row.get("schema") != 2
            or any(type(v) is not int for v in identity)
            or min(identity[:2]) <= 0
            or identity[2] < 0
            or identity in seen
            or tuple(header.get(k) for k in ["pid", "tid", "serial"]) != identity
            or row.get("complete") is not True
            or row.get("overflow") is not False
        ):
            raise ValueError("invalid counter identity/completion")
        c = row.get("counters")
        if (
            not isinstance(c, dict)
            or set(c) != set(NAMES)
            or any(type(v) is not int or not 0 <= v < 2**64 for v in c.values())
        ):
            raise ValueError("invalid counter schema/value")
        if type(row.get("worker_shard")) is not int or row["worker_shard"] < 0:
            raise ValueError("missing worker shard")
        if c["rx_attempts"] != sum(
            c[k] for k in ["rx_datagrams", "rx_eagain", "rx_oversize", "rx_errors"]
        ):
            raise ValueError("receive outcomes do not reconcile")
        if c["channel_calls"] != c["channel_full_polls"] + c["channel_skips"]:
            raise ValueError("channel full/skip partition")
        if c["channel_full_polls"] != sum(
            c[k]
            for k in [
                "channel_immediate_io",
                "channel_immediate_pacer",
                "channel_delayed",
            ]
        ):
            raise ValueError("channel full outcomes")
        if c["connection_polls"] != sum(c[k] for k in NAMES if k.startswith("batch_")):
            raise ValueError("poll histogram count")
        lo = sum(
            c[k] * n
            for k, n in [
                ("batch_1", 1),
                ("batch_2_4", 2),
                ("batch_5_16", 5),
                ("batch_17_64", 17),
                ("batch_over_64", 65),
            ]
        )
        hi = sum(
            c[k] * n
            for k, n in [
                ("batch_1", 1),
                ("batch_2_4", 4),
                ("batch_5_16", 16),
                ("batch_17_64", 64),
            ]
        )
        if c["poll_packets"] < lo or (
            not c["batch_over_64"] and c["poll_packets"] > hi
        ):
            raise ValueError("poll histogram bounds")
        if c["arm_calls"] != c["arm_due"] + c["arm_overflow"] + c["arm_success"]:
            raise ValueError("arm outcomes")
        if c["take_calls"] != sum(
            c[k]
            for k in [
                "take_no_pending",
                "take_epoch_changed",
                "take_rollback",
                "take_expired_cap",
                "take_expired_pacer",
                "take_success",
            ]
        ):
            raise ValueError("take outcomes")
        if c["take_calls"] - c["take_no_pending"] != sum(
            c[k] for k in NAMES if k.startswith("age_")
        ):
            raise ValueError("age histogram count")
        seen.add(identity)
        rows.append({**row, "file": path.name})
    return rows


def analyze_case(directory, result, profile, revision):
    try:
        hooks(revision)
        for side in ["caller", "listener"]:
            if "continuation counters:" in sc.read_output(
                directory / f"many-socket-{side}.stderr"
            ):
                raise ValueError("collector output failure")
        rows = read_counters(directory / "continuation-counters")
        expected = {
            role: result["peer_process_resources"][role]["pid"]
            for role, impl in zip(
                ["sender", "receiver"], sc.PROFILE_IMPLEMENTATIONS[profile]
            )
            if impl == "robotweax"
        }
        if not expected or {r["pid"] for r in rows} != set(expected.values()):
            raise ValueError("missing or unexpected Robotweax endpoint PID")
        packets = result["wire_statistics"]["sender_packets_unique"]
        if type(packets) is not int or packets <= 0:
            raise ValueError("missing original packet denominator")
        endpoints = {}
        for role, pid in expected.items():
            own = [r for r in rows if r["pid"] == pid]
            if (
                len(own) != 2
                or {r["worker_shard"] for r in own} != {0, 1}
                or any(r["counters"]["worker_starts"] != 1 for r in own)
            ):
                raise ValueError("incomplete two-shard worker coverage")
            c = {k: sum(r["counters"][k] for r in own) for k in NAMES}
            if revision == REVISIONS["baseline"]:
                special = [
                    k
                    for k in NAMES
                    if k.startswith(("route_", "arm_", "take_", "age_"))
                ] + ["channel_skips", "channel_no_hint", "channel_after_skip"]
                if any(c[k] for k in special):
                    raise ValueError("candidate counters in baseline")
            else:
                if (
                    c["channel_calls"]
                    != c["channel_no_hint"] + c["channel_after_skip"] + c["take_calls"]
                    or c["take_no_pending"] != 0
                ):
                    raise ValueError("candidate entry partition")
                if c["channel_full_polls"] != sum(
                    c[k] for k in NAMES if k.startswith("route_")
                ):
                    raise ValueError("route eligibility partition")
                if (
                    c["channel_immediate_pacer"]
                    != c["arm_missing_hint"] + c["arm_epoch_changed"] + c["arm_calls"]
                ):
                    raise ValueError("arm opportunity partition")
                if (
                    c["channel_skips"] != c["take_success"]
                    or c["channel_after_skip"] > c["channel_skips"]
                    or c["take_calls"] > c["arm_success"]
                ):
                    raise ValueError("one-shot lifecycle mismatch")
            if role == "sender":
                if (
                    c["tx_originals"] != packets
                    or c["tx_retransmissions"]
                    != result["wire_statistics"]["retransmitted_packets"]
                    or c["poll_packets"] != c["tx_originals"] + c["tx_retransmissions"]
                ):
                    raise ValueError("sender DATA counters do not reconcile")
            elif c["rx_data"] < result["wire_statistics"]["receiver_packets_unique"]:
                raise ValueError("receiver DATA coverage incomplete")
            endpoints[role] = {
                "pid": pid,
                "threads": own,
                "totals": c,
                "per_original_packet": {k: v / packets for k, v in c.items()},
                "skip_fraction": c["channel_skips"] / c["channel_calls"]
                if c["channel_calls"]
                else None,
                "take_success_fraction": c["take_success"] / c["take_calls"]
                if c["take_calls"]
                else None,
            }
        return {
            "valid": True,
            "source_revision": revision,
            "endpoints": endpoints,
            "scope": "worker lifetime, including setup/shutdown; ages from existing timestamps; no causal time-cost attribution",
        }
    except (OSError, ValueError, KeyError, TypeError, OverflowError) as error:
        return {"valid": False, "error": str(error)}


def validate_overlay(manifest, revision):
    overlay = manifest.get("continuation_diagnostics", {})
    if overlay.get("enabled") is not True or overlay.get("source_revision") != revision:
        raise ValueError("wrong continuation overlay pin")
    if overlay.get("changes") != EXPECTED_CHANGES[revision]:
        raise ValueError("overlay source hashes differ")
    for key, path in [
        ("overlay_script", Path(__file__).resolve()),
        ("collector_header", HEADER),
    ]:
        identity = overlay.get(key, {})
        if (
            identity.get("sha256") != sc.file_sha256(path)
            or identity.get("size_bytes") != path.stat().st_size
        ):
            raise ValueError("overlay/collector differs from common harness")


def verify_export(directory, revision):
    """Check every exported source against the pinned Git tree, including unpatched files."""
    hooks(revision)
    tree = subprocess.check_output(
        ["git", "-C", str(ROOT), "ls-tree", "-r", "-z", revision]
    )
    expected = {}
    for item in tree.split(b"\0"):
        if not item:
            continue
        identity, path = item.split(b"\t", 1)
        mode, kind, oid = identity.split()
        if kind != b"blob" or mode not in [b"100644", b"100755"]:
            raise ValueError("unexpected pinned source type")
        expected[path.decode()] = oid.decode()
    collector = "src/diagnostic_continuation_counters.hpp"
    files = {
        p.relative_to(directory).as_posix(): p
        for p in directory.rglob("*")
        if p.is_file() or p.is_symlink()
    }
    if set(files) != set(expected) | {collector}:
        raise ValueError("missing/extra exported source file")
    import hashlib

    for name, path in files.items():
        if path.is_symlink():
            raise ValueError("symlink in exported source")
        data = path.read_bytes()
        if name == collector:
            if data != HEADER.read_bytes():
                raise ValueError("changed exported collector")
        elif name in EXPECTED_CHANGES[revision]:
            if (
                hashlib.sha256(data).hexdigest()
                != EXPECTED_CHANGES[revision][name]["patched_sha256"]
            ):
                raise ValueError("changed patched source: " + name)
        elif (
            hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
            != expected[name]
        ):
            raise ValueError("changed unpatched source: " + name)
    return len(files)
