from __future__ import annotations
import copy, json, signal, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import run_continuation_diagnostics as run
import test_pacer_continuation_ab as fixtures
import test_bounded_send_operator as operators


class OperatorTests(operators.BoundedSendOperatorTests):
    runner_module = "run_continuation_diagnostics"
    first_variant = "baseline-plain"
    modes = (
        "success",
        "runner-failure",
        "SIGINT",
        "SIGTERM",
        "SIGHUP",
        "restore-first-fails",
    )


class RunnerTests(unittest.TestCase):
    def profiling(self, result, profile, revision):
        roles = ["sender", "receiver"] if profile == "robotweax-self" else ["sender"]
        return {
            "valid": True,
            "source_revision": revision,
            "endpoints": {
                role: {
                    "pid": result["peer_process_resources"][role]["pid"],
                    "per_original_packet": dict.fromkeys(run.cd.NAMES, 1),
                    "totals": dict.fromkeys(run.cd.NAMES, 101990),
                    "skip_fraction": 0,
                    "take_success_fraction": None,
                }
                for role in roles
            },
        }

    def raw(self, spec):
        raw = fixtures.PacerContinuationABTests().raw(spec["variant"])
        raw["arguments"] = raw["arguments"] | run.arguments(spec)
        if spec["capture"] == "continuation":
            raw["capture"] = "continuation"
            raw["runs"] = [raw["runs"][1], raw["runs"][3]]
            for i, r in enumerate(raw["runs"]):
                r["index"] = i
                r["kind"] = "continuation"
                r["result"]["rates"]["useful_bits_per_second"] = 999999999999
                r["profiling"] = self.profiling(
                    r["result"], r["profile"], run.REVISIONS[spec["variant"]]
                )
        return raw

    def mock_counters(self, path, result, profile, revision):
        return self.profiling(result, profile, revision)

    def report(self):
        with patch.object(run.cd, "analyze_case", side_effect=self.mock_counters):
            return {
                "complete": True,
                "interrupted": False,
                "blocks": [
                    {
                        **s,
                        "analysis": run.analyze_block(
                            self.raw(s), {}, 0, s, Path("unused")
                        ),
                    }
                    for s in run.plan()
                ],
            }

    def test_fixed_plan_separate_plain_metrics_and_no_nomination(self):
        r = self.report()
        a = run.analyze_experiment(r)
        self.assertEqual(sum(len(b["analysis"]["cases"]) for b in r["blocks"]), 32)
        self.assertTrue(a["diagnosis_valid"], a)
        self.assertEqual(a["followup_candidates"], [])
        self.assertFalse(a["candidate_nomination_performed"])
        self.assertEqual(len(a["diagnostic_profiles"]), 3)
        for profile in a["plain_comparison"]["profiles"]:
            self.assertAlmostEqual(profile["candidate_over_baseline"]["mbps"], 1.2)

    def test_missing_counter_report_wrong_plan_controls_and_raw_wait_fail(self):
        for mutate in [
            lambda r: r.update(complete=False),
            lambda r: r["blocks"].pop(),
            lambda r: r["blocks"][1]["analysis"].update(
                measurement_contract_pass=False
            ),
            lambda r: r["blocks"][0]["analysis"]["cases"][0].update(mbps=9999),
        ]:
            r = self.report()
            mutate(r)
            a = run.analyze_experiment(r)
            self.assertFalse(a["diagnosis_valid"])
            self.assertTrue(a["errors"])
            self.assertEqual(a["followup_candidates"], [])
        spec = run.plan()[1]
        raw = self.raw(spec)
        with patch.object(
            run.cd, "analyze_case", return_value={"valid": False, "error": "partial"}
        ):
            self.assertFalse(
                run.analyze_block(raw, {}, 0, spec, Path("none"))[
                    "measurement_contract_pass"
                ]
            )
        raw["runs"][0]["peer_exit_status"] = None
        with patch.object(run.cd, "analyze_case", side_effect=self.mock_counters):
            self.assertFalse(
                run.analyze_block(raw, {}, 0, spec, Path("none"))[
                    "measurement_contract_pass"
                ]
            )

    def execute(self, out, mode=None):
        calls = []

        def diagnostic(argv):
            i = len(calls)
            spec = run.plan()[i]
            calls.append(spec)
            dest = Path(argv[argv.index("--output-directory") + 1])
            dest.mkdir()
            if mode == "interrupt" and i == 3:
                raise KeyboardInterrupt
            raw = self.raw(spec)
            (dest / "report.json").write_text(
                "invalid" if mode == "json" and i == 1 else json.dumps(raw)
            )
            return 7 if mode == "exit" and i == 1 else 0

        report = {}
        paths = {run.build_key(s): Path("manifest") for s in run.plan()}
        manifests = dict.fromkeys(paths, {})
        with patch.object(run.diag, "main", side_effect=diagnostic), patch.object(
            run.cd, "analyze_case", side_effect=self.mock_counters
        ):
            code = run.run_blocks(paths, manifests, out, report)
        return code, report, calls

    def test_runner_continues_eight_blocks_on_ordinary_failures(self):
        for mode in [None, "json", "exit"]:
            with tempfile.TemporaryDirectory() as d:
                code, r, calls = self.execute(Path(d) / "out", mode)
            self.assertEqual(len(calls), 8)
            self.assertTrue(r["complete"])
            self.assertEqual(code, 0 if mode is None else 1)
            self.assertEqual(r["analysis"]["followup_candidates"], [])
            if mode == "exit":
                self.assertEqual(r["blocks"][1]["exit_code"], 7)

    def test_interrupt_preserves_signal_and_secondary_cleanup_error(self):
        with tempfile.TemporaryDirectory() as d:
            code, r, calls = self.execute(Path(d) / "out", "interrupt")
        self.assertEqual(code, 130)
        self.assertEqual(len(calls), 4)
        self.assertFalse(r["complete"])
        self.assertTrue(r["analysis"]["errors"])
        for suppress in [False, True]:

            def diagnostic(argv):
                try:
                    signal.raise_signal(signal.SIGTERM)
                except KeyboardInterrupt:
                    if suppress:
                        return 7
                    raise PermissionError("secondary cleanup")

            with tempfile.TemporaryDirectory() as d, patch.object(
                run.diag, "main", side_effect=diagnostic
            ):
                r = {}
                code = run.run_blocks(
                    {"baseline-plain": Path("manifest")}, {}, Path(d) / "out", r
                )
            self.assertEqual(code, 143)
            self.assertEqual(len(r["blocks"]), 1)
            self.assertEqual(
                r["blocks"][0]["driver_exit_code_before_interruption"],
                7 if suppress else 1,
            )

    def manifests(self):
        seed = fixtures.fixtures.fixtures.PlainCapacityABTests().manifests()["baseline"]
        m = {}
        for variant, pin in run.REVISIONS.items():
            for mode in ["plain", "counters"]:
                name = variant + "-" + mode
                m[name] = copy.deepcopy(seed)
                m[name]["sources"]["robotweax"]["revision"] = pin
                if mode == "counters":
                    m[name]["continuation_diagnostics"] = {
                        "enabled": True,
                        "source_revision": pin,
                        "changes": run.cd.EXPECTED_CHANGES[pin],
                        "overlay_script": run.sc.program_identity(
                            Path(run.cd.__file__)
                        ),
                        "collector_header": run.sc.program_identity(run.cd.HEADER),
                    }
        return m

    def test_four_manifest_pins_collector_hashes_and_plain_guards(self):
        m = self.manifests()
        args = (
            {"dirty": False, "revision": "common-harness"},
            {"platform": "Linux-test", "architecture": "aarch64"},
        )
        run.validate_manifests(m, *args, check_exports=False)
        for mutate in [
            lambda x: x.pop("candidate-counters"),
            lambda x: x["baseline-plain"].update(
                continuation_diagnostics={"enabled": True}
            ),
            lambda x: x["candidate-counters"]["continuation_diagnostics"].update(
                source_revision="wrong"
            ),
            lambda x: x["candidate-counters"]["continuation_diagnostics"].update(
                changes={}
            ),
            lambda x: x["candidate-counters"]["continuation_diagnostics"][
                "collector_header"
            ].update(sha256="wrong"),
            lambda x: x["candidate-counters"].update(transport_trace={"enabled": True}),
        ]:
            bad = copy.deepcopy(m)
            mutate(bad)
            with self.assertRaises(ValueError):
                run.validate_manifests(bad, *args, check_exports=False)
