from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import run_send_dispatch_factorial as runner
import test_bounded_send_ab as fixtures
import test_bounded_send_operator as operator_fixtures


class FactorialOperatorTests(operator_fixtures.BoundedSendOperatorTests):
    runner_module = "run_send_dispatch_factorial"
    first_variant = "a"
    modes = ("success", "runner-failure", "SIGINT", "SIGTERM", "SIGHUP", "restore-first-fails")


class SendDispatchFactorialTests(unittest.TestCase):
    def raw(self, variant="a", gain=True):
        report = fixtures.BoundedSendABTests().fixture(runner.STAGE)
        for case in report["runs"]:
            if case["kind"] == "measurement" and gain:
                case["result"]["rates"]["useful_bits_per_second"] *= {"a": 1, "b": .8, "c": .88, "d": 1.1}[variant]
        return report

    def report(self, gain=True):
        return {"complete": True, "interrupted": False,
                "blocks": [{"variant": v, "analysis": runner.analyze_block(self.raw(v, gain), {}, 0)} for v in runner.plan()]}

    def test_fixed_48_case_plan_and_pins(self):
        report = self.report()
        self.assertEqual(runner.plan(), ["a", "b", "c", "d", "d", "c", "b", "a"])
        self.assertEqual(sum(len(b["analysis"]["cases"]) for b in report["blocks"]), 48)
        self.assertEqual(runner.REVISIONS["b"], "11814d3a1ebc217dbe95613b679960ac3152ff59")
        self.assertEqual(runner.REVISIONS["d"], "52d48ee9439d190b920692bd2981729365416b02")

    def test_known_component_effects_and_interaction(self):
        result = runner.analyze_experiment(self.report())
        self.assertTrue(result["comparison_valid"])
        self.assertEqual(result["followup_candidates"], ["d"])
        self.assertFalse(result["long_transfer_qualification_performed"])
        for p in result["profiles"]:
            for contrast, value in (("d_over_a", 1.1), ("c_over_b", 1.1), ("b_over_a", .8), ("c_over_d", .8)):
                self.assertAlmostEqual(p["contrasts"][contrast]["mbps"], value)
            self.assertAlmostEqual(p["budget_interaction_c_over_b_div_d_over_a"]["mbps"], 1)

    def test_cpu_gate_checks_both_profiles_and_endpoints(self):
        for key in ("mbps", "sender_cpu_seconds_per_gib", "total_cpu_seconds_per_gib"):
            report = self.report()
            for b in report["blocks"]:
                if b["variant"] == "d":
                    for row in b["analysis"]["cases"]:
                        if row["kind"] == "measurement" and row["profile"] == runner.PROFILES[1]:
                            row[key] = 90 if key == "mbps" else 100
            result = runner.analyze_experiment(report)
            self.assertTrue(result["comparison_valid"])
            self.assertFalse(result["candidate_gates"]["d"]["eligible_for_long_transfer_followup"])

    def test_controls_require_all_16_and_stability(self):
        for change in (lambda rows: rows[0].update(mbps=200), lambda rows: rows.pop(0)):
            report = self.report();change(report["blocks"][0]["analysis"]["cases"])
            result = runner.analyze_experiment(report)
            self.assertFalse(result["comparison_valid"])
            self.assertEqual(result["followup_candidates"], [])

    def test_incomplete_reordered_and_failed_blocks_prevent_comparison(self):
        for change in (lambda r: r.update(complete=False), lambda r: r.update(interrupted=True),
                       lambda r: r["blocks"].pop(), lambda r: r["blocks"].insert(1, r["blocks"].pop(0)),
                       lambda r: r["blocks"][2]["analysis"].update(measurement_contract_pass=False)):
            report = self.report();change(report)
            self.assertFalse(runner.analyze_experiment(report)["comparison_valid"])

    def test_fractional_cpu_and_zero_context_denominator(self):
        report = self.report()
        for block in report["blocks"]:
            for row in block["analysis"]["cases"]:
                for k in runner.CORE_METRICS[1:]:row[k] = .4
                row["sender_involuntary_context_switches_per_gib"] = 0
        result = runner.analyze_experiment(report)
        self.assertTrue(result["comparison_valid"])
        self.assertIsNone(result["profiles"][0]["contrasts"]["d_over_a"]["sender_involuntary_context_switches_per_gib"])
        for value in (None, -1, float("nan"), float("inf")):
            bad = copy.deepcopy(report);bad["blocks"][0]["analysis"]["cases"][1]["sender_cpu_seconds_per_gib"] = value
            self.assertFalse(runner.analyze_experiment(bad)["measurement_contract_pass"])

    def execute(self, directory, failure=None, gain=True):
        calls = []
        def diagnostic(argv):
            dest = Path(argv[argv.index("--output-directory") + 1]);dest.mkdir()
            variant = dest.name.split("-")[1]
            raw = self.raw(variant, gain)
            first = not calls;calls.append(dest)
            if failure == "interrupt" and len(calls) == 2:raise KeyboardInterrupt
            if failure == "retransmission" and first:
                raw["runs"][0]["result"]["wire_statistics"].update(retransmitted_packets=1, sender_packets_total=101991)
            (dest / "report.json").write_text("invalid" if failure == "json" and first else json.dumps(raw))
            return 7 if failure == "exit" and first else 0
        report = {}
        with patch.object(runner.diag, "main", side_effect=diagnostic):
            code = runner.run_blocks(dict.fromkeys(runner.REVISIONS, Path("manifest")),
                                     dict.fromkeys(runner.REVISIONS, {}), directory, report)
        return code, report, calls

    def test_success_and_no_gain_both_finish_valid_comparison(self):
        for gain in (True, False):
            with tempfile.TemporaryDirectory() as d:
                code, report, calls = self.execute(Path(d) / "out", gain=gain)
            self.assertEqual(code, 0);self.assertEqual(len(calls), 8)
            self.assertTrue(report["complete"])
            self.assertEqual(report["analysis"]["followup_candidates"], ["d"] if gain else [])
            self.assertFalse(report["long_transfer_qualification_performed"])

    def test_ordinary_failures_keep_all_eight_blocks_and_original_codes(self):
        for failure in ("exit", "retransmission", "json"):
            with tempfile.TemporaryDirectory() as d:
                code, report, calls = self.execute(Path(d) / "out", failure)
            self.assertEqual(code, 1);self.assertEqual(len(calls), 8)
            self.assertTrue(report["complete"])
            self.assertFalse(report["analysis"]["comparison_valid"])
            self.assertEqual(report["blocks"][0]["exit_code"], 7 if failure == "exit" else 0)

    def test_interrupt_stops_plan_preserves_active_code_and_handlers(self):
        handler = signal.getsignal(signal.SIGINT)
        with tempfile.TemporaryDirectory() as d:
            code, report, calls = self.execute(Path(d) / "out", "interrupt")
            self.assertTrue((Path(d) / "out/factorial-report.json").exists())
        self.assertEqual(code, 130);self.assertEqual(len(calls), 2)
        self.assertTrue(report["interrupted"]);self.assertFalse(report["complete"])
        self.assertEqual(report["blocks"][-1]["exit_code"], 130)
        self.assertIs(signal.getsignal(signal.SIGINT), handler)

    def test_all_four_manifest_pins_and_plain_guards(self):
        seed = fixtures.fixtures.PlainCapacityABTests().manifests()["baseline"]
        manifests = {v: copy.deepcopy(seed) for v in runner.REVISIONS}
        for v, pin in runner.REVISIONS.items():manifests[v]["sources"]["robotweax"]["revision"] = pin
        args = ({"dirty": False, "revision": "common-harness"}, {"platform": "Linux-test", "architecture": "aarch64"})
        runner.common.validate_manifests(manifests, *args, revisions=runner.REVISIONS)
        for v in runner.REVISIONS:
            bad = copy.deepcopy(manifests);bad[v]["sources"]["robotweax"]["revision"] = "wrong"
            with self.assertRaises(ValueError):runner.common.validate_manifests(bad, *args, revisions=runner.REVISIONS)
            bad = copy.deepcopy(manifests);bad[v]["poll_counters"] = {"enabled": True}
            with self.assertRaises(ValueError):runner.common.validate_manifests(bad, *args, revisions=runner.REVISIONS)

    def test_four_builds_require_matching_toolchains_and_unchanged_binaries(self):
        with tempfile.TemporaryDirectory() as d:
            paths, manifests = {}, {}
            for variant in runner.REVISIONS:
                root = Path(d) / variant;root.mkdir();paths[variant] = root / "manifest.json"
                binary = root / "peer";binary.write_bytes(b"fixed peer")
                identity = {"path": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
                manifests[variant] = {"programs": {"robotweax": identity}, "libraries": {"robotweax": identity}}
                for name in ("compiler.log", "cmake.log", "openssl.log", "crypto-flags.log"):(root / name).write_text("fixed")
                cache = "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_C_FLAGS:STRING=-g\nCMAKE_CXX_FLAGS:STRING=-g\nCMAKE_C_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\nCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\nCMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\n"
                for library in ("robotweax", "haivision"):(root / f"{library}-cache.txt").write_text(cache)
            self.assertEqual(set(runner.common.validate_build_files(paths, manifests)), set(runner.REVISIONS))
            (paths["d"].parent / "compiler.log").write_text("changed")
            with self.assertRaises(ValueError):runner.common.validate_build_files(paths, manifests)
            (paths["d"].parent / "compiler.log").write_text("fixed")
            (paths["c"].parent / "peer").write_bytes(b"changed")
            with self.assertRaises(ValueError):runner.common.validate_build_files(paths, manifests)


if __name__ == "__main__":
    unittest.main()
