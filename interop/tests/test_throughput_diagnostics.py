from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import prepare_throughput as build
import throughput_diagnostics as diag


class ThroughputDiagnosticsTests(unittest.TestCase):
    def test_bounded_arguments(self):
        parse = diag.bounded_int(1, 10)
        self.assertEqual(parse("5"), 5)
        for value in ("0", "11"):
            with self.assertRaises(argparse.ArgumentTypeError):
                parse(value)

    def test_serial_plan_retains_controls_and_warmups(self):
        plan = diag.case_plan(["robotweax-self", "robotweax-to-haivision"], 5, 1, True, "none")
        self.assertEqual(len(plan), 14)
        self.assertEqual(plan[0]["kind"], "control-before")
        self.assertEqual(plan[-1]["kind"], "control-after")
        self.assertEqual(sum(p["kind"] == "measurement" for p in plan), 10)

    def test_profile_plan_never_labels_instrumented_runs_as_baseline(self):
        for capture in ("cpu", "scheduler"):
            plan = diag.case_plan(["robotweax-self"], 1, 2, True, capture)
            self.assertEqual(plan, [{"profile": "robotweax-self", "kind": capture}])

    def test_cpu_capture_uses_software_clock_dwarf_and_child_inheritance(self):
        command = diag.perf_command("cpu", Path("/tmp/cpu.data"), ["python", "driver"], "/usr/bin/perf")
        self.assertIn("cpu-clock", command)
        self.assertIn("dwarf,8192", command)
        self.assertNotIn("-a", command)
        self.assertEqual(command[-3:], ["--", "python", "driver"])
        self.assertEqual(diag.perf_command("none", Path("x"), ["driver"], "perf"), ["driver"])

    def test_peer_controls_are_explicit_not_environment_driven(self):
        args = argparse.Namespace(pending_packets=8192, udp_buffer=8388608, target_bps=0)
        command = diag.peer_arguments(args)
        options = dict(zip(command[::2], command[1::2]))
        self.assertEqual(options["--max-bandwidth"], "1250000000")
        self.assertEqual(options["--flow-window"], "16384")
        self.assertEqual(options["--send-buffer"], "33554432")
        self.assertEqual(options["--receive-buffer"], "33554432")

    def test_pid_sample_counts_ignore_stacks_and_other_processes(self):
        text = " 100 rwx-capacity\n 200 hvs-capacity\n 100 rwx-capacity\n 900 python\n ffffffff runtime::poll\n"
        self.assertEqual(diag.sample_counts(text, [100, 200, 300]), {100: 2, 200: 1, 300: 0})
        self.assertEqual(diag.sample_counts("rwx-capacity 100\nhvs-capacity 200\n", [100, 200]), {100: 1, 200: 1})

    def test_summary_does_not_hide_failures_or_mix_profiled_rates(self):
        def entry(kind, passed, rate=0):
            return {"profile": "robotweax-self", "kind": kind, "transfer_pass": passed,
                    "result": {"rates": {"useful_bits_per_second": rate * 1e6}}}
        summary = diag.summarize([entry("measurement", True, 100), entry("measurement", False),
                                  entry("measurement", True, 300), entry("cpu", True, 999),
                                  entry("warmup", True, 999), entry("control-before", True, 999)])[0]
        self.assertEqual(summary["attempts"], 3)
        self.assertEqual(summary["failed"], 1)
        self.assertEqual(summary["median_mbps"], 200)
        self.assertEqual(summary["mean_mbps"], 200)
        self.assertEqual(summary["p95_mbps"], 290)
        self.assertTrue(summary["conditional_on_complete_transfers"])

    def test_all_failed_summary_has_null_not_zero_measurements(self):
        summary = diag.summarize([{"profile": "robotweax-self", "kind": "measurement", "transfer_pass": False}])[0]
        self.assertIsNone(summary["median_mbps"])
        self.assertEqual(summary["failed"], 1)

    def request(self, directory):
        request = {"profile": "robotweax-self", "programs": {"robotweax": "/tmp/peer"},
                   "options": {"host": "127.0.0.1", "connections": 1, "bytes_per_connection": 13160,
                               "message_size": 1316, "timeout_seconds": 10, "latency_milliseconds": 120,
                               "shutdown_grace_milliseconds": 500, "sampling_interval_seconds": .02},
                   "index": 0, "kind": "measurement", "target_bps": 0, "peer_arguments": []}
        path = directory / "request.json"
        path.write_text(json.dumps(request))
        return path

    def test_case_failure_is_preserved_with_udp_delta(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(diag.sc, "run_many_socket_profile", side_effect=RuntimeError("payload mismatch")), mock.patch.object(diag, "udp_snapshot", side_effect=[{"InErrors": 7}, {"InErrors": 9}]):
            directory = Path(tmp)
            self.assertEqual(diag.run_case(self.request(directory)), 1)
            result = json.loads((directory / "case.json").read_text())
            self.assertFalse(result["transfer_pass"])
            self.assertEqual(result["error"], "payload mismatch")
            self.assertEqual(result["system_udp_delta"], {"InErrors": 2})

    def test_case_rejects_old_peer_without_option_readbacks(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(diag.sc, "run_many_socket_profile", return_value={"rates": {"useful_bits_per_second": 1}}), mock.patch.object(diag.sc, "read_output", return_value=""), mock.patch.object(diag, "udp_snapshot", return_value={}):
            directory = Path(tmp)
            self.assertEqual(diag.run_case(self.request(directory)), 1)
            result = json.loads((directory / "case.json").read_text())
            self.assertIn("readbacks", result["error"])

    def test_case_success_requires_both_role_readbacks(self):
        with tempfile.TemporaryDirectory() as tmp, mock.patch.object(diag.sc, "run_many_socket_profile", return_value={"rates": {"useful_bits_per_second": 1}}), mock.patch.object(diag.sc, "read_output", return_value='{"event":"capacity-options"}\n'), mock.patch.object(diag, "udp_snapshot", return_value={}):
            directory = Path(tmp)
            self.assertEqual(diag.run_case(self.request(directory)), 0)
            result = json.loads((directory / "case.json").read_text())
            self.assertTrue(result["transfer_pass"])
            self.assertTrue(result["rate_pass"])

    def test_missing_profile_is_not_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertFalse(diag.render_capture("cpu", Path(tmp), {}, "perf")["valid"])
            self.assertFalse(diag.render_capture("cpu", Path(tmp), {"peer_process_resources": {"sender": None}}, "perf")["valid"])

    def test_cpu_render_requires_samples_from_both_endpoint_processes(self):
        result = {"peer_process_resources": {"sender": {"pid": 100}, "receiver": {"pid": 200}}}
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "perf.data").touch()
            for samples, valid in (("rwx-capacity 100\nhvs-capacity 200\n", True),
                                   ("rwx-capacity 100\n", False)):
                completed = subprocess.CompletedProcess([], 0, stdout=samples, stderr="")
                with mock.patch.object(diag.subprocess, "run", return_value=completed) as run:
                    capture = diag.render_capture("cpu", directory, result, "perf")
                self.assertEqual(capture["valid"], valid)
                self.assertIn("comm,pid,dso,symbol", run.call_args_list[0].args[0])
                self.assertTrue(capture["call_stack_quality_requires_review"])

    def test_scheduler_pid_presence_never_qualifies_worker_runtime(self):
        result = {"peer_process_resources": {"sender": {"pid": 100}, "receiver": {"pid": 200}}}
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "perf.data").touch()
            for content, mentioned in (("rwx-capacity[100] hvs-capacity[200]", True),
                                       ("rwx-capacity[1000] hvs-capacity[200]", False)):
                def render(command, **kwargs):
                    self.assertIn("100,200", command)
                    kwargs["stdout"].write(content)
                    return subprocess.CompletedProcess(command, 0)
                with mock.patch.object(diag.subprocess, "run", side_effect=render):
                    capture = diag.render_capture("scheduler", directory, result, "perf")
                self.assertFalse(capture["valid"])
                self.assertEqual(capture["peer_pid_mentions"], mentioned)
                self.assertTrue(capture["render_succeeded"])
                self.assertFalse(capture["worker_coverage_validated"])
                self.assertFalse(capture["runtime_coverage_validated"])

    def test_scheduler_millisecond_only_native_summary_is_not_a_pass(self):
        # Shape of the observed ARM64 renderer failure, with synthetic PIDs.
        summary = """Samples of sched_switch event do not have callchains.
Runtime summary
comm parent sched-in run-time min-run avg-run max-run stddev migrations
                 :100[100] -1 1 3.442 3.442 3.442 3.442 0.00 0
                 :200[200] -1 1 0.766 0.766 0.766 0.766 0.00 0
"""
        result = {"peer_process_resources": {
            "sender": {"pid": 100, "user_cpu_us": 4_800_000, "system_cpu_us": 1_200_000},
            "receiver": {"pid": 200, "user_cpu_us": 200_000, "system_cpu_us": 170_000}}}
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "perf.data").touch()
            def render(command, **kwargs):
                kwargs["stdout"].write(summary)
                return subprocess.CompletedProcess(command, 0)
            with mock.patch.object(diag.subprocess, "run", side_effect=render):
                capture = diag.render_capture("scheduler", directory, result, "perf")
            self.assertTrue(capture["peer_pid_mentions"])
            self.assertFalse(capture["valid"])
            self.assertIn("worker", capture["error"])
            self.assertEqual((directory / "perf-scheduler.txt").read_text(), summary)

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_bounded_command_preserves_nonzero_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            status = diag.run_bounded([sys.executable, "-c", "print('evidence'); raise SystemExit(7)"], Path(tmp), 5)
            self.assertEqual(status, 7)
            self.assertIn("evidence", (Path(tmp) / "command.log").read_text())

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_bounded_command_times_out(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(subprocess.TimeoutExpired):
                diag.run_bounded([sys.executable, "-c", "import time; time.sleep(60)"], Path(tmp), .1)

    @unittest.skipUnless(os.name == "posix", "POSIX process groups")
    def test_failed_profiler_cannot_leave_workload_running(self):
        with tempfile.TemporaryDirectory() as tmp:
            marker = Path(tmp) / "unexpected-workload"
            child = f"import time,pathlib; time.sleep(.4); pathlib.Path({str(marker)!r}).touch()"
            wrapper = f"import subprocess,sys; subprocess.Popen([sys.executable, '-c', {child!r}]); raise SystemExit(9)"
            self.assertEqual(diag.run_bounded([sys.executable, "-c", wrapper], Path(tmp), 5), 9)
            time.sleep(.5)
            self.assertFalse(marker.exists())

    def test_scheduler_requires_explicit_system_wide_consent(self):
        with mock.patch.object(diag.platform, "system", return_value="Linux"):
            with self.assertRaises(SystemExit) as context:
                diag.main(["--build-manifest", "/missing", "--output-directory", "/missing", "--capture", "scheduler"])
            self.assertEqual(context.exception.code, 2)

    def test_missing_perf_does_not_silently_run_unprofiled(self):
        with mock.patch.object(diag.platform, "system", return_value="Linux"), mock.patch.object(diag.shutil, "which", return_value=None):
            with self.assertRaises(SystemExit) as context:
                diag.main(["--build-manifest", "/missing", "--output-directory", "/missing", "--capture", "cpu"])
            self.assertEqual(context.exception.code, 2)

    def test_changed_library_is_rejected_before_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            library = directory / "lib.a"
            library.write_bytes(b"changed")
            manifest = directory / "build.json"
            manifest.write_text(json.dumps({"complete": True, "programs": {}, "libraries": {"robotweax": {"path": str(library), "sha256": "wrong"}}}))
            with self.assertRaises(SystemExit):
                diag.main(["--build-manifest", str(manifest), "--output-directory", str(directory / "results")])
            self.assertFalse((directory / "results").exists())

    def test_dirty_source_rejected_unless_explicit(self):
        with mock.patch.object(build.subprocess, "check_output", side_effect=["abc\n", " M file.cpp\n"]):
            with self.assertRaises(ValueError):
                build.source_identity(Path("/src"))
        with mock.patch.object(build.subprocess, "check_output", side_effect=["abc\n", " M file.cpp\n"]):
            self.assertTrue(build.source_identity(Path("/src"), allow_dirty=True)["dirty"])


if __name__ == "__main__":
    unittest.main()
