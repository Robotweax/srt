from __future__ import annotations

import copy
import json
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import run_bounded_send_ab as runner
import test_throughput_diagnostics_plain_ab as fixtures


class BoundedSendABTests(unittest.TestCase):
    def fixture(self, spec, variant="baseline", improvement=True):
        args = runner.arguments(spec)
        cases = runner.diag.case_plan(runner.PROFILES, spec["repetitions"], spec["warmups"], True, "none")
        with patch.object(runner.common, "ARGUMENTS", args), patch.object(runner.common, "PROFILES", runner.PROFILES), \
                patch.object(runner.common, "cases", return_value=cases):
            report = fixtures.PlainCapacityABTests().diagnostic()
        for case in report["runs"]:
            r = case["result"]
            connections = spec["connections"]
            r["connections"] = connections
            r["integrity"]["verified_connections"] = connections
            for k in ("sender_packets_unique", "sender_packets_total", "receiver_packets_unique"):
                r["wire_statistics"][k] *= connections
            for role, pid in (("sender", 100), ("receiver", 200)):
                r["peer_process_resources"][role] = {"pid": pid, "user_cpu_us": 1000000, "system_cpu_us": 500000,
                                                     "voluntary_context_switches": 10, "involuntary_context_switches": 2}
            r["timing"] = {f"{side}_completion_seconds": dict(zip(
                ("minimum", "p50", "p99", "p99_9", "maximum"), (1, 1.1, 1.2, 1.2, 1.3)))
                for side in ("caller", "listener")}
            if variant == "candidate" and improvement and case["kind"] == "measurement":
                r["rates"]["useful_bits_per_second"] *= 1.2
        return report

    def stage(self, spec=None, improvement=True):
        spec = spec or runner.plan()[0]
        return {**spec, "blocks": [{"variant": v, "analysis": runner.analyze_block(
            self.fixture(spec, v, improvement), {}, 0, spec)} for v in runner.common.plan()]}

    def test_fixed_conditional_plan_and_aggregate_byte_counts(self):
        sizes = []
        for spec in runner.plan():
            cases = runner.diag.case_plan(runner.PROFILES, spec["repetitions"], spec["warmups"], True, "none")
            sizes.append(len(cases) * 4)
        self.assertEqual(sizes, [24, 40, 40])
        self.assertEqual([s["connections"] for s in runner.plan()], [1, 1, 10])
        self.assertEqual(runner.REVISIONS["baseline"], "8e1bdebed836cb7b732db852f51ef6a7b212e925")
        self.assertEqual(runner.REVISIONS["candidate"], "36477473ad3f68e94b90573797fb5a45c04d7872")
        spec = runner.plan()[2]
        raw = self.fixture(spec)
        checked = runner.analyze_block(raw, {}, 0, spec)
        self.assertTrue(checked["measurement_contract_pass"], checked["errors"])
        row = checked["cases"][0]
        size = raw["runs"][0]["result"]["bytes_per_connection"]
        self.assertEqual(row["sender_cpu_seconds_per_gib"], 1.5 * 1024**3 / (size * 10))
        self.assertEqual(row["total_cpu_seconds_per_gib"], 3 * 1024**3 / (size * 10))

    def test_multi_stream_partial_payload_and_missing_cpu_or_cdf_fail(self):
        spec = runner.plan()[2]
        mutations = [lambda r: r["integrity"].update(verified_connections=9),
                     lambda r: r["wire_statistics"].update(sender_packets_unique=101990),
                     lambda r: r["peer_process_resources"]["receiver"].pop("system_cpu_us"),
                     lambda r: r["peer_process_resources"]["receiver"].update(pid=0),
                     lambda r: r["timing"].pop("listener_completion_seconds"),
                     lambda r: r["timing"]["caller_completion_seconds"].update(p50=100),
                     lambda r: r["timing"]["caller_completion_seconds"].update(p99=float("nan"))]
        for change in mutations:
            raw = self.fixture(spec)
            change(raw["runs"][0]["result"])
            self.assertFalse(runner.analyze_block(raw, {}, 0, spec)["measurement_contract_pass"])

    def test_stage_gate_requires_quality_controls_and_a_real_gain(self):
        self.assertTrue(runner.qualify(self.stage())["qualified"])
        self.assertFalse(runner.qualify(self.stage(improvement=False))["qualified"])
        fast = self.stage()
        for block in fast["blocks"]:
            for row in block["analysis"]["cases"]:
                row["sender_cpu_seconds_per_gib"] = .2
                row["total_cpu_seconds_per_gib"] = .4
        self.assertTrue(runner.qualify(fast)["qualified"])
        self.assertFalse(runner.common.number(0, positive=True))
        cases = [lambda s: s["blocks"][1]["analysis"].update(measurement_contract_pass=False),
                 lambda s: s["blocks"][1]["analysis"]["cases"][0].update(mbps=200),
                 lambda s: s["blocks"].insert(1, s["blocks"].pop(0)),
                 lambda s: s["blocks"].pop()]
        for change in cases:
            stage = self.stage(); change(stage)
            self.assertFalse(runner.qualify(stage)["qualified"])
        for metric, value in (("mbps", 90), ("sender_cpu_seconds_per_gib", 99),
                              ("total_cpu_seconds_per_gib", 99)):
            stage = self.stage()
            for block in stage["blocks"]:
                if block["variant"] == "candidate":
                    for row in block["analysis"]["cases"]:
                        if row["kind"] == "measurement":
                            row[metric] = value
            self.assertFalse(runner.qualify(stage)["qualified"], metric)

    def test_exact_pin_and_plain_manifest_guards(self):
        manifests = fixtures.PlainCapacityABTests().manifests()
        for v, pin in runner.REVISIONS.items():
            manifests[v]["sources"]["robotweax"]["revision"] = pin
        args = ({"dirty": False, "revision": "common-harness"}, {"platform": "Linux-test", "architecture": "aarch64"})
        runner.common.validate_manifests(manifests, *args, revisions=runner.REVISIONS)
        for change in (lambda m: m["candidate"]["sources"]["robotweax"].update(revision="11814d3"),
                       lambda m: m["candidate"].update(pacer_deadline_diagnostics={"enabled": True}),
                       lambda m: m["baseline"].update(poll_counters={"enabled": True})):
            bad = copy.deepcopy(manifests); change(bad)
            with self.assertRaises(ValueError):
                runner.common.validate_manifests(bad, *args, revisions=runner.REVISIONS)

    def execute(self, out, failure=None):
        calls = []
        def diagnostic(argv):
            dest = Path(argv[argv.index("--output-directory") + 1]); dest.mkdir()
            spec = next(s for s in runner.plan() if s["name"] == dest.parent.name)
            variant = "baseline" if "baseline" in dest.name else "candidate"
            raw = self.fixture(spec, variant, improvement=failure != "no-gain")
            code = 7 if failure == "exit" and not calls else 0
            if failure == "retransmission" and not calls:
                raw["runs"][0]["result"]["wire_statistics"]["retransmitted_packets"] = 1
                raw["runs"][0]["result"]["wire_statistics"]["sender_packets_total"] += 1
            (dest / "report.json").write_text(json.dumps(raw))
            calls.append(dest)
            return code
        report = {}
        with patch.object(runner.diag, "main", side_effect=diagnostic):
            code = runner.run_blocks(dict.fromkeys(runner.REVISIONS, Path("manifest")),
                                     dict.fromkeys(runner.REVISIONS, {}), out, report)
        return code, report, calls

    def test_success_runs_all_104_cases(self):
        with tempfile.TemporaryDirectory() as d:
            code, report, calls = self.execute(Path(d) / "out")
        self.assertEqual(code, 0); self.assertTrue(report["qualified"])
        self.assertEqual(len(calls), 12)
        self.assertEqual(sum(len(b["analysis"]["cases"]) for s in report["stages"] for b in s["blocks"]), 104)

    def test_failed_stage_finishes_abba_and_skips_later_stages(self):
        for failure, expected in (("exit", 1), ("retransmission", 1), ("no-gain", 2)):
            with tempfile.TemporaryDirectory() as d:
                code, report, calls = self.execute(Path(d) / "out", failure)
            self.assertEqual(code, expected)
            self.assertTrue(report["complete"]); self.assertFalse(report["qualified"])
            self.assertEqual(len(calls), 4)
            self.assertEqual([s["status"] for s in report["stages"]], ["completed", "skipped", "skipped"])
            if failure == "exit":
                self.assertEqual(report["stages"][0]["blocks"][0]["exit_code"], 7)

    def test_interrupt_restores_handlers_and_does_not_continue(self):
        saved = signal.getsignal(signal.SIGINT)
        with tempfile.TemporaryDirectory() as d, patch.object(runner.diag, "main", side_effect=KeyboardInterrupt):
            report = {}
            code = runner.run_blocks({"baseline": Path("manifest")}, {}, Path(d) / "out", report)
            self.assertTrue((Path(d) / "out/qualification-report.json").exists())
        self.assertEqual(code, 130); self.assertTrue(report["interrupted"])
        self.assertEqual(len(report["stages"]), 1)
        self.assertEqual(report["stages"][0]["blocks"][0]["exit_code"], 130)
        self.assertEqual(signal.getsignal(signal.SIGINT), saved)
        for suppress in (False, True):
            def interrupted_cleanup(_argv):
                try:
                    signal.raise_signal(signal.SIGTERM)
                except KeyboardInterrupt:
                    if suppress:
                        return 7
                    raise PermissionError("injected cleanup error after SIGTERM")
            with tempfile.TemporaryDirectory() as d, patch.object(runner.diag, "main", side_effect=interrupted_cleanup):
                report = {}
                code = runner.run_blocks({"baseline": Path("manifest")}, {}, Path(d) / "out", report)
            self.assertEqual(code, 143)
            block = report["stages"][0]["blocks"][0]
            self.assertEqual(block["exit_code"], 143)
            self.assertEqual(block["driver_exit_code_before_interruption"], 7 if suppress else 1)
            if not suppress:self.assertIn("injected cleanup error", report["interruption_cleanup_error"])
            self.assertEqual(signal.getsignal(signal.SIGINT), saved)


if __name__ == "__main__":
    unittest.main()
