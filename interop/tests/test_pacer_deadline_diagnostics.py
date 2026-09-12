from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import pacer_deadline_diagnostics as dd
import run_pacer_deadline_diagnostics as runner
import throughput_diagnostics as diag
import test_throughput_diagnostics_plain_ab as fixtures


def snapshot(complete=True):
    return {"schema": 1, "event": "snapshot", "ordinal": 1, "complete": complete,
            "overflow": False, "checkpoint_limit": False, "elapsed_ns": 1,
            "counters": dict.fromkeys(dd.COUNTERS, 0),
            "histograms": {k: {"count": 0, "sum": 0, "max": 0, "buckets": [0] * (len(dd.BOUNDS) + 1)} for k in dd.METRICS}}


def registration(pid=123, shard=0, shards=1):
    return {"schema": 1, "event": "register", "pid": pid, "tid": pid + shard,
            "serial": shard, "shard": shard, "shards": shards, "timer_slack_ns": 50000}


class DeadlineDiagnosticsTests(unittest.TestCase):
    def block_fixture(self, spec):
        args = runner.block_arguments(spec)
        cases = diag.case_plan(runner.PROFILES, spec["repetitions"], 0, True, spec["capture"])
        with patch.object(runner.common, "ARGUMENTS", args), patch.object(runner.common, "PROFILES", runner.PROFILES), \
                patch.object(runner.common, "cases", return_value=cases):
            report = fixtures.PlainCapacityABTests().diagnostic()
        report["capture"] = spec["capture"]
        for row in report["runs"]:
            resources = row["result"]["peer_process_resources"]
            resources["sender"].update(pid=100, voluntary_context_switches=20, involuntary_context_switches=2)
            resources["receiver"] = {**resources["sender"], "pid": 200}
            if spec["capture"] == "pacer":
                row["profiling"] = {"valid": True}
        return report

    def test_successful_plan_and_capture_failure_keep_separate_summaries(self):
        paths = {runner.key(v, c): Path("manifest") for v in runner.REVISIONS for c in ("none", "pacer")}
        manifests = dict.fromkeys(paths, {})
        for spec in runner.plan():
            raw = self.block_fixture(spec)
            checked = runner.analyze_block(raw, {}, 0, spec)
            self.assertTrue(checked["measurement_contract_pass"], checked["errors"])
            if spec["capture"] == "pacer":
                self.assertEqual(checked["summary"], [])
                raw["runs"][0]["profiling"] = {"valid": False, "workers": ["partial"]}
                failed = runner.analyze_block(raw, {}, 0, spec)
                self.assertFalse(failed["measurement_contract_pass"])
                self.assertEqual(len(failed["cases"]), 4)
                self.assertEqual(failed["cases"][0]["profiling"]["workers"], ["partial"])
            else:
                self.assertEqual([s["mbps"]["samples"] for s in checked["summary"]], [3, 3])
                raw["runs"][0]["system_udp_delta"].pop("InErrors")
                self.assertFalse(runner.analyze_block(raw, {}, 0, spec)["measurement_contract_pass"])
        with tempfile.TemporaryDirectory() as directory:
            def execute(args):
                out = Path(args[args.index("--output-directory") + 1]); out.mkdir()
                spec = runner.plan()[int(out.name[:2])]
                (out / "report.json").write_text(json.dumps(self.block_fixture(spec)))
                return 0
            report = {}
            with patch.object(runner.diag, "main", side_effect=execute):
                self.assertEqual(runner.run_blocks(paths, manifests, Path(directory) / "out", report), 0)
            self.assertTrue(report["complete"])
            self.assertEqual(sum(len(b["analysis"]["cases"]) for b in report["blocks"]), 40)

    def test_manifest_pins_overlay_identity_and_plain_guards(self):
        manifests = {}
        originals = fixtures.PlainCapacityABTests().manifests()
        fake_patches = {"source.cpp": (b"original", b"instrumented")}
        for variant, revision in runner.REVISIONS.items():
            for capture in ("none", "pacer"):
                m = copy.deepcopy(originals[variant])
                m["sources"]["robotweax"].update(revision=revision, path=str(dd.ROOT))
                if capture == "pacer":
                    m["pacer_deadline_diagnostics"] = {
                        "enabled": True, "source_revision": revision,
                        "files": [{"path": n, "original_sha256": dd.hashlib.sha256(a).hexdigest(),
                                   "patched_sha256": dd.hashlib.sha256(b).hexdigest()} for n, (a, b) in fake_patches.items()],
                        "overlay_script": {"sha256": runner.sc.file_sha256(Path(dd.__file__))},
                        "collector_header": {"sha256": runner.sc.file_sha256(dd.HEADER)}}
                manifests[runner.key(variant, capture)] = m
        args = ({"dirty": False, "revision": "common-harness"}, {"platform": "Linux-test", "architecture": "aarch64"})
        with patch.object(dd, "patched_sources", return_value=fake_patches):
            runner.validate_manifests(manifests, *args)
            mutations = [lambda m: m.pop("candidate-plain"),
                         lambda m: m["candidate-diagnostic"]["pacer_deadline_diagnostics"]["collector_header"].update(sha256="wrong"),
                         lambda m: m["candidate-diagnostic"]["pacer_deadline_diagnostics"].update(files=[]),
                         lambda m: m["baseline-plain"].update(pacer_deadline_diagnostics={"enabled": True}),
                         lambda m: m["candidate-plain"]["sources"]["robotweax"].update(revision="wrong"),
                         lambda m: m["baseline-diagnostic"].update(poll_counters={"enabled": True})]
            for mutate in mutations:
                changed = copy.deepcopy(manifests); mutate(changed)
                with self.assertRaises(ValueError):
                    runner.validate_manifests(changed, *args)

    def test_fixed_forty_case_plan_and_capture_separation(self):
        plan = runner.plan()
        self.assertEqual([b["variant"] for b in plan], ["baseline", "candidate", "baseline", "candidate", "candidate", "baseline"])
        kinds = [case["kind"] for b in plan for case in diag.case_plan(runner.PROFILES, b["repetitions"], 0, True, b["capture"])]
        self.assertEqual(len(kinds), 40)
        self.assertEqual(kinds.count("measurement"), 24)
        self.assertEqual(kinds.count("pacer"), 8)
        self.assertEqual(sum(k.startswith("control-") for k in kinds), 8)
        self.assertNotIn("warmup", kinds)
        self.assertEqual(runner.ARGUMENTS["bytes_per_connection"], 16 * 1024**2)
        self.assertEqual(diag.perf_command("pacer", Path("unused"), ["peer"], "unused"), ["peer"])

    def test_exact_source_hashes_and_unique_anchors(self):
        # Exercise the fixed-pin guard without requiring historical Git objects
        # in shallow CI or freezing future mainline library edits. The actual
        # historical exports are additionally built and tested independently.
        sources = {}
        for name, hooks in dd.HOOKS.items():
            original = "\n".join(old for old, _ in hooks).encode()
            sources[name] = original
            for revision in dd.REVISIONS:
                self.assertRegex(dd.SOURCE_HASHES[revision][name], r"^[0-9a-f]{64}$")
            changed = dd.instrument(original.decode(), hooks)
            self.assertNotEqual(changed, original.decode())
            with self.assertRaises(ValueError):
                dd.instrument(original.decode() * 2, hooks)
        hashes = {revision: {name: dd.hashlib.sha256(raw).hexdigest() for name, raw in sources.items()}
                  for revision in dd.REVISIONS}
        with patch.object(dd, "SOURCE_HASHES", hashes), patch.object(dd.subprocess, "check_output",
                side_effect=lambda command: sources[command[-1].split(":", 1)[1]]):
            for revision in dd.REVISIONS:
                exported = dd.patched_sources(dd.ROOT, revision)
                self.assertEqual(set(exported), set(sources))
                self.assertTrue(all(before != after for before, after in exported.values()))
        with self.assertRaises(ValueError):
            dd.patched_sources(dd.ROOT, "unreviewed")
        with patch.object(dd.subprocess, "check_output", return_value=b"changed"):
            with self.assertRaisesRegex(ValueError, "source hash"):
                dd.patched_sources(dd.ROOT, dd.REVISIONS[0])

    def test_header_and_analyzer_bucket_contract(self):
        text = dd.HEADER.read_text().split("bounds {")[1].split("};")[0]
        actual = tuple(int(n.replace("'", "")) for n in re.findall(r"[0-9][0-9']*", text))
        self.assertEqual(actual, dd.BOUNDS)
        self.assertEqual(len(dd.COUNTERS), len(set(dd.COUNTERS)))
        self.assertEqual(len(dd.METRICS), len(set(dd.METRICS)))

    def test_missing_nan_overflow_and_inconsistent_histograms_fail(self):
        dd.validate_snapshot(snapshot())
        mutations = [lambda s: s["counters"].pop("data_packets"),
                     lambda s: s["counters"].update(data_packets=float("nan")),
                     lambda s: s.update(overflow=True), lambda s: s.update(checkpoint_limit=True),
                     lambda s: s["histograms"]["data_per_task"].update(count=1),
                     lambda s: s["counters"].update(timer_tasks=1),
                     lambda s: s["histograms"]["task_elapsed_ns"].update(max=1)]
        for mutate in mutations:
            s = snapshot(); mutate(s)
            with self.assertRaises(ValueError):
                dd.validate_snapshot(s)
        with self.assertRaises(ValueError):
            dd.validate_snapshot(None)

    def test_partial_truncated_and_nonmonotonic_snapshots_are_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "worker.jsonl"
            prefix = json.dumps(registration()) + "\n" + json.dumps(snapshot(False)) + "\n"
            p.write_text(prefix + '{"event":')
            r = dd.read_worker(p)
            self.assertFalse(r["complete"])
            self.assertIsNotNone(r["last_snapshot"])
            self.assertIn("malformed/truncated line", r["errors"])
            final = snapshot(); final.update(ordinal=2, elapsed_ns=2)
            p.write_text(prefix + json.dumps(final) + "\n")
            self.assertTrue(dd.read_worker(p)["complete"])
            p.write_text(prefix + json.dumps(snapshot()) + "\n")
            self.assertIn("snapshot ordinal mismatch", dd.read_worker(p)["errors"])

    def test_missing_worker_or_pid_never_becomes_zero_coverage(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root / "pacer-deadline").mkdir()
            p = root / "pacer-deadline/worker.jsonl"
            p.write_text(json.dumps(registration(shards=2)) + "\n" + json.dumps(snapshot()) + "\n")
            result = {"sender_implementation": "robotweax", "receiver_implementation": "haivision",
                      "peer_process_resources": {"sender": {"pid": 123}},
                      "wire_statistics": {"sender_packets_unique": 1, "retransmitted_packets": 0}}
            checked = dd.analyze_case(root, result)
            self.assertFalse(checked["valid"])
            self.assertTrue(any("shard coverage" in e for e in checked["errors"]))
            self.assertTrue(any("packet coverage" in e for e in checked["errors"]))
            checked = dd.analyze_case(root, {})
            self.assertFalse(checked["valid"])
            self.assertEqual(len(checked["workers"]), 1)

    def test_quantiles_are_bucket_intervals(self):
        s = snapshot(); h = s["histograms"]["task_elapsed_ns"]
        h.update(count=2, sum=8000, max=4000); h["buckets"][dd.BOUNDS.index(5000)] = 2
        result = dd.aggregate_histograms([{"last_snapshot": s}])["task_elapsed_ns"]
        self.assertEqual(result["mean"], 4000)
        self.assertEqual(result["quantile_intervals"]["99"], [2001, 5000])

    def test_case_failure_keeps_partial_diagnostic_analysis(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "request.json"
            p.write_text(json.dumps({"capture": "pacer", "profile": "robotweax-self", "programs": {}, "index": 0,
                                    "options": {}, "kind": "pacer", "peer_arguments": []}))
            partial = {"valid": False, "workers": [{"last_snapshot": snapshot(False)}]}
            with patch.object(diag.sc, "RunOptions"), patch.object(diag.sc, "run_many_socket_profile", side_effect=RuntimeError("timeout")), \
                    patch.object(diag, "udp_snapshot", return_value={}), patch.object(diag.dd, "analyze_case", return_value=partial), \
                    patch.dict(os.environ, clear=False):
                self.assertEqual(diag.run_case(p), 1)
            case = json.loads((p.parent / "case.json").read_text())
            self.assertFalse(case["transfer_pass"])
            self.assertEqual(case["error"], "timeout")
            self.assertEqual(case["profiling"], partial)

    def test_failed_blocks_continue_but_interrupt_ends_plan(self):
        paths = {runner.key(v, c): Path("manifest") for v in runner.REVISIONS for c in ("none", "pacer")}
        for error, expected in ((RuntimeError("ordinary failure"), 6), (KeyboardInterrupt(), 1)):
            with tempfile.TemporaryDirectory() as directory, patch.object(runner.diag, "main", side_effect=error) as call:
                report = {}; code = runner.run_blocks(paths, {}, Path(directory) / "out", report)
                self.assertEqual(call.call_count, expected)
                self.assertNotEqual(code, 0)
                self.assertEqual(report["complete"], expected == 6)
                self.assertEqual(report["interrupted"], expected == 1)

    @unittest.skipUnless(os.name == "posix" and shutil.which("c++"), "POSIX C++ collector")
    def test_compiled_collector_final_and_abrupt_exit_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / "collector.cpp"; binary = root / "collector"
            source.write_text('#include "' + str(dd.HEADER) + '"\n#include <thread>\n#include <string_view>\n'
                'int main(int argc,char**) { std::thread worker([&] {\n'
                'namespace d = robotweax::srt::deadline_diagnostics; d::recorder().start(0,1);\n'
                '{ d::TaskScope task(d::Clock::now(), {}, false); d::PollScope poll;\n'
                'd::sent(d::Clock::now(), true, false); ++poll.packets; }\n'
                'if(argc>1) { d::recorder().checkpoint(d::Clock::now()+std::chrono::seconds(2)); std::_Exit(7); }\n'
                '}); worker.join(); }\n')
            built = subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", str(source), "-pthread", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            for partial in (False, True):
                out = root / str(partial); out.mkdir()
                finished = subprocess.run([str(binary), *(["partial"] if partial else [])],
                    env={**os.environ, "ROBOTWEAX_PACER_DEADLINE_DIR": str(out)}, capture_output=True, text=True)
                self.assertEqual(finished.returncode, 7 if partial else 0, finished.stderr)
                files = list(out.glob("*.jsonl")); self.assertEqual(len(files), 1)
                worker = dd.read_worker(files[0])
                self.assertEqual(worker["complete"], not partial, worker)
                self.assertEqual(worker["last_snapshot"]["counters"]["original_packets"], 1)
                self.assertEqual(worker["last_snapshot"]["histograms"]["data_per_task"]["sum"], 1)


if __name__ == "__main__":
    unittest.main()
