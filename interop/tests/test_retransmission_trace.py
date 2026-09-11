from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import retransmission_trace as rt
import run_retransmission_ab as ab
import throughput_diagnostics as diag


class RetransmissionTraceTests(unittest.TestCase):
    def test_plan_retains_abba_plain_and_separate_matched_unpaced_traces(self):
        plan = ab.plan()
        self.assertEqual([b["variant"] for b in plan[:4]], ["baseline", "candidate", "candidate", "baseline"])
        self.assertTrue(all(b["capture"] == "none" and b["target_bps"] == 100000000 for b in plan[:4]))
        self.assertEqual([b["target_bps"] for b in plan[4:]], [100000000, 100000000, 0, 0])
        transfers = sum(len(diag.case_plan(["robotweax-self", "robotweax-to-haivision"],
                                           b["repetitions"], 1, True, b["capture"])) for b in plan)
        self.assertEqual(transfers, 48)
        self.assertEqual(diag.perf_command("transport", Path("unused"), ["driver"], "unused"), ["driver"])

    def manifests(self):
        return {(v, m): {"complete": True, "sources": {
                    "robotweax": {"dirty": False, "revision": revision},
                    "haivision": {"revision": "899348d8318eb9a3c5a5b6ec43c4a1114288773a"}},
                    "harness_source": {"sha256": "helper"}, "platform": "linux", "architecture": "arm64",
                    "crypto": "openssl", "linkage": "static",
                    "transport_trace": {"enabled": m == "transport", "overlay_script": {"sha256": "overlay"},
                                        "collector_header": {"sha256": "header"}}}
                for v, revision in ab.REVISIONS.items() for m in ("none", "transport")}

    def test_manifest_contract(self):
        ab.validate_manifests(self.manifests())
        for field, value in (("harness_source", {"sha256": "other"}), ("platform", "darwin"),
                             ("complete", False), ("transport_trace", {"enabled": True})):
            manifests = self.manifests()
            manifests["baseline", "none"][field] = value
            with self.assertRaises(ValueError):
                ab.validate_manifests(manifests)
        with self.assertRaises(ValueError):
            ab.validate_manifests({})

    def test_exact_hooks_match_both_experimental_revisions(self):
        # Source anchors also run in shallow CI checkouts via the current source;
        # the separate local overlay smoke tests pin both historical revisions.
        for relative, hooks in rt.HOOKS.items():
            original = (rt.ROOT / relative).read_text()
            changed = rt.instrument(original, hooks)
            self.assertIn("RWX_TRACE", changed)
            with self.assertRaises(ValueError):
                rt.instrument(changed, hooks)

    def test_ambiguous_anchor_fails_closed(self):
        with self.assertRaises(ValueError):
            rt.instrument("x x", [("x", "y")])

    def event(self, kind, a=0, b=0, c=0, d=0, obj=100, ns=None, pid=1):
        self.clock = getattr(self, "clock", 0) + 1000
        return dict(kind=kind, a=a, b=b, c=c, d=d, object=obj,
                    ns=self.clock if ns is None else ns, pid=pid, file="1-0.jsonl", index=self.clock)

    def base(self, seq=42):
        return [self.event("bind", a=200), self.event("wire_send", a=seq),
                self.event("ack", a=seq, c=1000, d=200)]

    def test_duplicate_nak_does_not_override_timer_queue_cause(self):
        e = self.base()
        e += [self.event("timer"), self.event("request_tail", obj=200),
              self.event("queued", a=42, obj=200), self.event("nak", a=42, b=42),
              self.event("request_nak", a=42, b=42, obj=200),
              self.event("selected", a=42, obj=200), self.event("wire_send", a=42, b=1)]
        result = rt.analyze_events(e, 1, 1, 1)
        self.assertTrue(result["valid"], result)
        self.assertEqual(result["retransmission_reasons"], {"tail": 1})
        self.assertEqual(result["retransmissions"][0]["lineage"]["trigger"]["kind"], "timer")

    def test_nak_and_timer_all_and_rollover_sequences(self):
        for reason, trigger in (("nak", "nak"), ("all", "timer")):
            e = self.base(seq=0x7fffffff)
            e += [self.event(trigger, a=0x7fffffff), self.event("request_" + reason, obj=200),
                  self.event("queued", a=0x7fffffff, obj=200),
                  self.event("selected", a=0x7fffffff, obj=200), self.event("wire_send", a=0x7fffffff, b=1)]
            result = rt.analyze_events(e, 1, 1, 1)
            self.assertTrue(result["valid"], result)
            self.assertEqual(result["retransmission_reasons"], {reason: 1})

    def test_missing_selection_or_original_is_not_guessed_from_latest_nak(self):
        e = self.base() + [self.event("nak", a=42), self.event("wire_send", a=42, b=1)]
        result = rt.analyze_events(e, 1, 1, 1)
        self.assertFalse(result["valid"])
        self.assertEqual(result["retransmission_reasons"], {"unknown": 1})

    def test_statistic_mismatch_invalidates_otherwise_complete_trace(self):
        result = rt.analyze_events(self.base(), 1, 1, 2)
        self.assertFalse(result["valid"])
        self.assertIn("retransmission coverage 0 != 2", result["errors"])

    def test_receiver_original_observed_before_retransmission_is_preserved(self):
        e = self.base()
        e += [self.event("data_receive", a=42, pid=2), self.event("nak", a=42),
              self.event("request_nak", obj=200), self.event("queued", a=42, obj=200),
              self.event("selected", a=42, obj=200), self.event("wire_send", a=42, b=1)]
        result = rt.analyze_events(e, 1, 1, 1)
        self.assertTrue(result["valid"])
        self.assertEqual(len(result["retransmissions"][0]["receiver_observations"]), 1)

    def test_missing_or_truncated_or_overflow_trace_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            with self.assertRaises(ValueError):
                rt.read_events(directory)
            path = directory / "1-0.jsonl"
            for tail in (None, {"end": True, "events": 0, "dropped": 1},
                         {"end": True, "events": 2, "dropped": 0}):
                lines = [{"schema": 1, "pid": 1}]
                if tail:
                    lines.append(tail)
                path.write_text("\n".join(json.dumps(e) for e in lines) + "\n")
                with self.assertRaises(ValueError):
                    rt.read_events(directory)

    def test_trace_reader_uses_pid_and_monotonic_timestamps(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for pid, ns in ((1, 20), (2, 10)):
                events = [{"schema": 1, "pid": pid}, self.event("ack", ns=ns),
                          {"end": True, "events": 1, "dropped": 0}]
                (directory / f"{pid}-0.jsonl").write_text("\n".join(json.dumps(e) for e in events) + "\n")
            self.assertEqual([e["pid"] for e in rt.read_events(directory)], [2, 1])
