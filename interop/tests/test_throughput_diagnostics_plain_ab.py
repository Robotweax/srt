from __future__ import annotations

import copy
import json
import math
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import run_plain_capacity_ab as ab


class PlainCapacityABTests(unittest.TestCase):
    def manifests(self):
        return {variant: {
            "complete": True, "sources": {
                "robotweax": {"dirty": False, "revision": revision},
                "haivision": {"dirty": False, "revision": ab.build.REFERENCE_REVISION}},
            "harness_checkout": {"dirty": False, "revision": "common-harness"},
            "harness_source": {"sha256": ab.PEER_SHA256},
            "platform": "Linux-test", "architecture": "aarch64", "crypto": "openssl", "linkage": "static",
            "programs": {"robotweax": {}, "haivision": {}},
            "libraries": {"robotweax": {}, "haivision": {}},
        } for variant, revision in ab.REVISIONS.items()}

    def validate(self, manifests):
        ab.validate_manifests(manifests, {"dirty": False, "revision": "common-harness"},
                              {"platform": "Linux-test", "architecture": "aarch64"})

    def diagnostic(self, manifest=None):
        packets = math.ceil(ab.ARGUMENTS["bytes_per_connection"] / 1316)
        runs = []
        for index, case in enumerate(ab.cases()):
            options = {"encryption": "none", "tlpktdrop": False, "pending_packets": 8192,
                       "requested_maxbw_bytes_per_second": 1250000000, "target_bits_per_second": 0,
                       "api_fc": 16384, "api_udp_rcvbuf": ab.ARGUMENTS["udp_buffer"],
                       "api_udp_sndbuf": ab.ARGUMENTS["udp_buffer"]}
            runs.append({**case, "index": index, "exit_code": 0, "transfer_pass": True,
                         "system_udp_delta": dict.fromkeys(ab.UDP_ERRORS, 0),
                         "result": {"connections": 1, "message_size_bytes": 1316,
                                    "requested_bytes_per_connection": ab.ARGUMENTS["bytes_per_connection"],
                                    "messages_per_connection": packets, "bytes_per_connection": packets * 1316,
                                    "integrity": {"deterministic_payload_verified": True, "verified_connections": 1},
                                    "wire_statistics": {"sender_packets_unique": packets, "sender_packets_total": packets,
                                                        "receiver_packets_unique": packets, "retransmitted_packets": 0},
                                    "rates": {"useful_bits_per_second": 100e6},
                                    "peer_process_resources": {"sender": {"user_cpu_us": 1000000, "system_cpu_us": 500000}},
                                    "capacity_options": {"caller": [copy.deepcopy(options)], "listener": [copy.deepcopy(options)]}}})
        return {"finished": True, "all_transfers_pass": True, "capture": "none", "udp_capacity_controlled": True,
                "arguments": {**ab.ARGUMENTS, "profile": ab.PROFILES, "allow_small_udp_buffers": False},
                "build_manifest": manifest or {}, "runs": runs}

    def test_fixed_plan_contains_40_cases_with_24_measurements(self):
        self.assertEqual(ab.plan(), ["baseline", "candidate", "candidate", "baseline"])
        cases = ab.cases() * len(ab.plan())
        self.assertEqual(len(cases), 40)
        self.assertEqual(sum(c["kind"] == "measurement" for c in cases), 24)
        self.assertEqual(sum(c["kind"] == "warmup" for c in cases), 8)
        self.assertEqual(sum(c["profile"] == "haivision-self" for c in cases), 8)
        self.assertEqual(ab.ARGUMENTS["target_bps"], 0)
        self.assertEqual(ab.REVISIONS["candidate"], "09c852b40374d21e60e68b8aadaaabb5aa7294b8")

    def test_accepts_exact_clean_plain_pair(self):
        self.validate(self.manifests())

    def test_rejects_wrong_dirty_old_or_traced_sources(self):
        for keys, value in (
            (("complete",), False),
            (("sources", "robotweax", "revision"), "40387f2"),
            (("sources", "robotweax", "dirty"), True),
            (("sources", "haivision", "dirty"), True),
            (("sources", "haivision", "revision"), "wrong"),
            (("transport_trace",), {"enabled": True}),
            (("harness_checkout", "dirty"), True),
            (("harness_checkout", "revision"), "old-harness"),
            (("harness_source", "sha256"), "changed-peer"),
            (("platform",), "Darwin"), (("architecture",), "x86_64"),
            (("crypto",), "none"), (("linkage",), "shared"),
            (("programs",), {"robotweax": {}}),
        ):
            with self.subTest(keys=keys):
                manifests = self.manifests()
                target = manifests["candidate"]
                for key in keys[:-1]:
                    target = target[key]
                target[keys[-1]] = value
                with self.assertRaises(ValueError):
                    self.validate(manifests)

    def test_requires_both_manifests_and_clean_runner(self):
        manifests = self.manifests()
        with self.assertRaises(ValueError):
            self.validate({"candidate": manifests["candidate"]})
        with self.assertRaisesRegex(ValueError, "clean committed"):
            ab.validate_manifests(manifests, {"dirty": True}, {})

    def build_files(self, directory):
        manifests, paths = self.manifests(), {}
        for variant in ab.REVISIONS:
            root = directory / variant
            root.mkdir()
            paths[variant] = root / "build-manifest.json"
            for name in ("compiler.log", "cmake.log", "openssl.log", "crypto-flags.log"):
                (root / name).write_text(name)
            for library in ("robotweax", "haivision"):
                (root / f"{library}-cache.txt").write_text(
                    "CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_C_FLAGS:STRING=-g\nCMAKE_CXX_FLAGS:STRING=-g\n"
                    "CMAKE_C_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\nCMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG\n"
                    "CMAKE_C_COMPILER:FILEPATH=/usr/bin/cc\nCMAKE_CXX_COMPILER:STRING=/usr/bin/c++\n")
                for key in ("programs", "libraries"):
                    artifact = root / f"{key}-{library}"
                    artifact.write_bytes(b"test build only")
                    manifests[variant][key][library] = ab.sc.program_identity(artifact)
        return paths, manifests

    def test_build_preflight_validates_hashes_flags_and_toolchain(self):
        with tempfile.TemporaryDirectory() as tmp:
            paths, manifests = self.build_files(Path(tmp))
            signatures = ab.validate_build_files(paths, manifests)
            self.assertEqual(signatures["baseline"], signatures["candidate"])
            (paths["candidate"].parent / "programs-robotweax").write_bytes(b"changed binary")
            with self.assertRaisesRegex(ValueError, "changed since build"):
                ab.validate_build_files(paths, manifests)

    def test_build_preflight_rejects_toolchain_or_optimization_changes(self):
        for filename, text in (("compiler.log", "other compiler"), ("openssl.log", "other OpenSSL"),
                               ("robotweax-cache.txt", "CMAKE_BUILD_TYPE:STRING=Debug\n")):
            with self.subTest(filename=filename), tempfile.TemporaryDirectory() as tmp:
                paths, manifests = self.build_files(Path(tmp))
                (paths["candidate"].parent / filename).write_text(text)
                with self.assertRaises(ValueError):
                    ab.validate_build_files(paths, manifests)

    def test_complete_zero_repeat_block_passes(self):
        analysis = ab.analyze_block(self.diagnostic(), {}, 0)
        self.assertTrue(analysis["measurement_contract_pass"], analysis["errors"])
        self.assertEqual(len(analysis["cases"]), 10)

    def test_nonzero_retransmissions_fail_including_controls_and_warmups(self):
        for index in range(10):
            with self.subTest(index=index):
                report = self.diagnostic()
                wire = report["runs"][index]["result"]["wire_statistics"]
                wire["retransmitted_packets"] = 2
                wire["sender_packets_total"] += 2
                analysis = ab.analyze_block(report, {}, 0)
                self.assertFalse(analysis["measurement_contract_pass"])
                self.assertIn("nonzero retransmissions", " ".join(analysis["errors"]))
                self.assertGreater(analysis["cases"][index]["retransmission_ratio"], 0)

    def test_missing_counters_are_not_zero_and_inconsistent_totals_fail(self):
        for key, value in (("retransmitted_packets", None), ("retransmitted_packets", False),
                           ("retransmitted_packets", -1), ("sender_packets_unique", 0),
                           ("sender_packets_total", 1), ("receiver_packets_unique", None)):
            with self.subTest(key=key, value=value):
                report = self.diagnostic()
                report["runs"][2]["result"]["wire_statistics"][key] = value
                self.assertFalse(ab.analyze_block(report, {}, 0)["measurement_contract_pass"])

    def test_udp_missing_reset_or_errors_fail(self):
        for udp in ({}, {**dict.fromkeys(ab.UDP_ERRORS, 0), "InErrors": 1},
                    {**dict.fromkeys(ab.UDP_ERRORS, 0), "SndbufErrors": -1}):
            report = self.diagnostic()
            report["runs"][2]["system_udp_delta"] = udp
            self.assertFalse(ab.analyze_block(report, {}, 0)["measurement_contract_pass"])

    def test_reduced_api_udp_buffer_fails_even_with_large_kernel_maximum(self):
        report = self.diagnostic()
        report["runs"][2]["result"]["capacity_options"]["listener"][0]["api_udp_rcvbuf"] = 262144
        self.assertFalse(ab.analyze_block(report, {}, 0)["measurement_contract_pass"])

    def test_rejects_missing_payload_cpu_or_changed_options(self):
        for key, value in (("integrity", {}), ("connections", 2), ("bytes_per_connection", 0),
                           ("peer_process_resources", {}), ("capacity_options", {}),
                           ("rates", {"useful_bits_per_second": float("nan")})):
            report = self.diagnostic()
            report["runs"][2]["result"][key] = value
            self.assertFalse(ab.analyze_block(report, {}, 0)["measurement_contract_pass"])

    def test_missing_extra_reordered_or_failed_runs_fail(self):
        for mode in ("missing", "extra", "reordered", "failed", "exit"):
            report = self.diagnostic()
            if mode == "missing":
                report["runs"].pop()
            elif mode == "extra":
                report["runs"].append(copy.deepcopy(report["runs"][-1]))
            elif mode == "reordered":
                report["runs"].reverse()
            elif mode == "failed":
                report["runs"][2]["transfer_pass"] = False
            self.assertFalse(ab.analyze_block(report, {}, 1 if mode == "exit" else 0)["measurement_contract_pass"])

    def test_changed_block_arguments_or_build_fail(self):
        for key, value in (("capture", "transport"), ("finished", False), ("udp_capacity_controlled", False),
                           ("build_manifest", {"different": True}), ("arguments", {})):
            report = self.diagnostic()
            report[key] = value
            self.assertFalse(ab.analyze_block(report, {}, 0)["measurement_contract_pass"])

    def test_summary_excludes_controls_and_warmups_but_retains_repeat_failures(self):
        report = self.diagnostic()
        for run in report["runs"]:
            run["result"]["rates"]["useful_bits_per_second"] = 9999e6
        for index, rate in zip((2, 3, 4), (100, 200, 300)):
            report["runs"][index]["result"]["rates"]["useful_bits_per_second"] = rate * 1e6
        wire = report["runs"][2]["result"]["wire_statistics"]
        wire["retransmitted_packets"] = 1
        wire["sender_packets_total"] += 1
        summary = ab.analyze_block(report, {}, 0)["summary"][0]
        self.assertEqual(summary["mbps"], {"samples": 3, "median": 200, "mean": 200, "p95": 290})
        self.assertEqual(summary["sender_cpu_seconds"]["median"], 1.5)
        self.assertEqual(summary["evidence_passes"], 2)

    def test_all_failed_summary_is_null_not_zero(self):
        report = self.diagnostic()
        for run in report["runs"]:
            run["transfer_pass"] = False
        summary = ab.analyze_block(report, {}, 0)["summary"][0]
        self.assertIsNone(summary["mbps"]["median"])
        self.assertEqual(summary["mbps"]["samples"], 0)

    def run_fake(self, tmp, mode):
        manifests = self.manifests()
        paths = {variant: Path(tmp) / f"{variant}.json" for variant in ab.REVISIONS}
        report = {}

        def execute(command, **kwargs):
            out = Path(command[command.index("--output-directory") + 1])
            variant = "candidate" if "candidate" in out.name else "baseline"
            self.assertNotIn("--allow-small-udp-buffers", command)
            self.assertEqual(command[command.index("--capture") + 1], "none")
            self.assertEqual(command[command.index("--repetitions") + 1], "3")
            self.assertEqual(command[command.index("--target-bps") + 1], "0")
            self.assertEqual(command[command.index("--cooldown-seconds") + 1], "30")
            if out.name.startswith("00") and mode == "interrupt":
                raise KeyboardInterrupt
            if not (out.name.startswith("00") and mode == "missing"):
                out.mkdir()
                (out / "report.json").write_text(json.dumps(self.diagnostic(manifests[variant])))
            return subprocess.CompletedProcess(command, 1 if out.name.startswith("00") and mode == "failed" else 0)

        with patch.object(ab.subprocess, "run", side_effect=execute) as run:
            if mode == "interrupt":
                with self.assertRaises(KeyboardInterrupt):
                    ab.run_blocks(paths, manifests, Path(tmp) / "results", report)
            else:
                status = ab.run_blocks(paths, manifests, Path(tmp) / "results", report)
                self.assertEqual(status, 0 if mode == "pass" else 1)
                self.assertEqual(run.call_count, 4)  # no retry, even after a failed/missing block
        return json.loads((Path(tmp) / "results/ab-report.json").read_text())

    def test_runner_pass_never_grants_performance_release_approval(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = self.run_fake(tmp, "pass")
            self.assertTrue(report["complete"])
            self.assertTrue(report["measurement_contract_pass"])
            self.assertTrue(report["performance_decision_requires_review"])

    def test_runner_preserves_failures_missing_reports_and_continues_abba(self):
        for mode in ("missing", "failed"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                report = self.run_fake(tmp, mode)
                self.assertTrue(report["complete"])
                self.assertFalse(report["measurement_contract_pass"])
                self.assertEqual(len(report["blocks"]), 4)
                self.assertFalse(report["blocks"][0]["analysis"]["measurement_contract_pass"])

    def test_interrupt_keeps_incomplete_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            report = self.run_fake(tmp, "interrupt")
            self.assertFalse(report["complete"])
            self.assertIsNone(report["blocks"][0]["exit_code"])

    def test_rejects_macos_and_x86_qualification(self):
        for system, machine in (("Darwin", "arm64"), ("Linux", "x86_64")):
            with patch.object(ab.platform, "system", return_value=system), patch.object(ab.platform, "machine", return_value=machine):
                with self.assertRaises(SystemExit):
                    ab.main(["--baseline-plain", "unused", "--candidate-plain", "unused", "--output-directory", "unused"])

    def test_ci_selection_is_python_only(self):
        sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
        import ci_changes
        for path in ("benchmarks/run_plain_capacity_ab.py", "interop/tests/test_throughput_diagnostics_plain_ab.py"):
            selected = ci_changes.classify([path])
            self.assertTrue(selected.python)
            self.assertFalse(selected.full)
            self.assertFalse(selected.cpp)
            self.assertFalse(selected.portable)
            self.assertFalse(selected.interop)
            self.assertEqual(selected.reference_profiles(), ())


if __name__ == "__main__":
    unittest.main()
