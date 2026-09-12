from __future__ import annotations

import copy
import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import run_pacer_deadline_ab as ab
import test_throughput_diagnostics_plain_ab as fixtures


class PacerDeadlineABTests(unittest.TestCase):
    def manifests(self):
        with patch.object(ab.common, "REVISIONS", ab.REVISIONS):
            return fixtures.PlainCapacityABTests().manifests()

    def diagnostic(self, manifest=None):
        with patch.object(ab.common, "ARGUMENTS", ab.ARGUMENTS), patch.object(ab.common, "PROFILES", ab.PROFILES):
            result = fixtures.PlainCapacityABTests().diagnostic(manifest)
        for case in result["runs"]:
            resources = case["result"]["peer_process_resources"]
            resources["sender"].update(pid=100, voluntary_context_switches=20, involuntary_context_switches=2)
            resources["receiver"] = {"pid": 200, "user_cpu_us": 500000, "system_cpu_us": 500000,
                                     "voluntary_context_switches": 10, "involuntary_context_switches": 1}
        return result

    def test_fixed_72_transfer_plan_and_separate_library_pins(self):
        cases = ab.cases() * len(ab.common.plan())
        self.assertEqual(len(cases), 72)
        self.assertEqual(sum(c["kind"] == "measurement" for c in cases), 48)
        self.assertEqual(sum(c["kind"] == "warmup" for c in cases), 16)
        self.assertEqual(sum(c["kind"].startswith("control-") for c in cases), 8)
        self.assertEqual(ab.ARGUMENTS["bytes_per_connection"], 1024**3)
        self.assertEqual(ab.REVISIONS["baseline"], "8e1bdebed836cb7b732db852f51ef6a7b212e925")
        self.assertNotEqual(ab.REVISIONS, ab.common.REVISIONS)
        self.assertTrue(all(len(pin) == 40 for pin in ab.REVISIONS.values()))

    def test_exact_pair_and_instrumentation_guards(self):
        manifests = self.manifests()
        args = ({"dirty": False, "revision": "common-harness"}, {"platform": "Linux-test", "architecture": "aarch64"})
        ab.common.validate_manifests(manifests, *args, revisions=ab.REVISIONS)
        for mutation in (lambda m: m["candidate"]["sources"]["robotweax"].update(revision=ab.common.REVISIONS["candidate"]),
                         lambda m: m["candidate"].update(poll_counters={"enabled": True}),
                         lambda m: m["baseline"].update(transport_trace={"enabled": True}),
                         lambda m: m["candidate"]["harness_checkout"].update(revision="different-harness")):
            altered = copy.deepcopy(manifests)
            mutation(altered)
            with self.assertRaises(ValueError):
                ab.common.validate_manifests(altered, *args, revisions=ab.REVISIONS)

    def test_all_four_directions_and_both_process_resources_are_required(self):
        report = self.diagnostic()
        analysis = ab.analyze_block(report, {}, 0)
        self.assertTrue(analysis["measurement_contract_pass"], analysis["errors"])
        self.assertEqual(len(analysis["cases"]), 18)
        self.assertEqual({s["profile"] for s in analysis["summary"]}, set(ab.PROFILES))
        for role in ("sender", "receiver"):
            for key in ("pid", "user_cpu_us", "system_cpu_us", "voluntary_context_switches", "involuntary_context_switches"):
                altered = copy.deepcopy(report)
                altered["runs"][2]["result"]["peer_process_resources"][role].pop(key)
                self.assertFalse(ab.analyze_block(altered, {}, 0)["measurement_contract_pass"], (role, key))

    def test_retransmissions_and_missing_udp_fail_in_every_case(self):
        for index in range(len(ab.cases())):
            for mode in ("retransmission", "missing-udp"):
                report = self.diagnostic()
                row = report["runs"][index]
                if mode == "missing-udp":
                    row["system_udp_delta"].pop("InErrors")
                else:
                    row["result"]["wire_statistics"]["retransmitted_packets"] = 1
                    row["result"]["wire_statistics"]["sender_packets_total"] += 1
                self.assertFalse(ab.analyze_block(report, {}, 0)["measurement_contract_pass"])

    def test_summary_keeps_failed_contract_samples_and_excludes_warmups(self):
        report = self.diagnostic()
        for row in report["runs"]:
            if row["kind"] != "measurement":
                row["result"]["peer_process_resources"]["receiver"]["user_cpu_us"] = 999999999
        report["runs"][2]["system_udp_delta"]["InErrors"] = 1
        summary = ab.analyze_block(report, {}, 0)["summary"][0]
        self.assertEqual(summary["receiver_cpu_seconds"]["median"], 1)
        self.assertEqual(summary["receiver_cpu_seconds"]["samples"], 3)
        self.assertEqual(summary["evidence_passes"], 2)

    def test_runner_continues_after_failure_but_stops_on_interrupt(self):
        for mode in ("pass", "fail", "missing", "interrupt"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as tmp:
                manifests = self.manifests()
                paths = {v: Path(tmp) / f"{v}.json" for v in ab.REVISIONS}
                report = {}
                before = signal.getsignal(signal.SIGINT)

                def execute(args):
                    out = Path(args[args.index("--output-directory") + 1])
                    variant = "candidate" if "candidate" in out.name else "baseline"
                    first = out.name.startswith("00")
                    if first and mode == "interrupt":
                        raise KeyboardInterrupt
                    if not (first and mode == "missing"):
                        out.mkdir()
                        (out / "report.json").write_text(json.dumps(self.diagnostic(manifests[variant])))
                    return 1 if first and mode == "fail" else 0

                with patch.object(ab.diag, "main", side_effect=execute) as run:
                    status = ab.run_blocks(paths, manifests, Path(tmp) / "results", report)
                self.assertEqual(status, 0 if mode == "pass" else 130 if mode == "interrupt" else 1)
                self.assertEqual(run.call_count, 1 if mode == "interrupt" else 4)
                self.assertEqual(report["complete"], mode != "interrupt")
                self.assertEqual(report["interrupted"], mode == "interrupt")
                self.assertEqual(signal.getsignal(signal.SIGINT), before)
                self.assertTrue(report["performance_decision_requires_review"])

    def test_rejects_other_hosts(self):
        for system, machine, hostname in (("Darwin", "arm64", "lima-srt-network-lab-runtime"),
                                          ("Linux", "x86_64", "lima-srt-network-lab-runtime"),
                                          ("Linux", "aarch64", "different-vm")):
            with patch.object(ab.platform, "system", return_value=system), patch.object(ab.platform, "machine", return_value=machine), patch.object(ab.socket, "gethostname", return_value=hostname):
                with self.assertRaises(SystemExit):
                    ab.main(["--baseline-plain", "unused", "--candidate-plain", "unused", "--output-directory", "unused"])

    @unittest.skipUnless(os.name == "posix", "POSIX workload process groups")
    def test_real_interrupt_cleans_case_and_signal_ignoring_peer(self):
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=signum), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                ready = root / "ready.json"
                peer_ready = root / "peer-ready"
                workload = root / "workload.py"
                peer_code = ("import signal,time; from pathlib import Path; "
                             "signal.signal(signal.SIGTERM,signal.SIG_IGN); "
                             "signal.signal(signal.SIGINT,signal.SIG_IGN); "
                             f"Path({str(peer_ready)!r}).write_text('ready'); time.sleep(60)")
                workload.write_text("import os,signal,subprocess,sys,time,json\n"
                    "signal.signal(signal.SIGTERM,signal.SIG_IGN)\n"
                    "signal.signal(signal.SIGINT,signal.SIG_IGN)\n"
                    f"p=subprocess.Popen([sys.executable,'-c',{peer_code!r}])\n"
                    "deadline=time.monotonic()+3\n"
                    f"while not os.path.exists({str(peer_ready)!r}) and time.monotonic()<deadline:time.sleep(.01)\n"
                    f"assert os.path.exists({str(peer_ready)!r})\n"
                    f"open({str(ready)!r},'w').write(json.dumps({{'case':os.getpid(),'peer':p.pid,'group':os.getpgrp()}}))\n"
                    "time.sleep(60)\n")
                driver = root / "driver.py"
                driver.write_text(f"import sys\nfrom pathlib import Path\nsys.path.insert(0,{str(Path(ab.__file__).parent)!r})\n"
                    "import run_pacer_deadline_ab as ab\n"
                    f"root=Path({str(root)!r})\n"
                    "def diagnostic(args):\n"
                    " out=root/'case';out.mkdir()\n"
                    " return ab.diag.run_bounded([sys.executable,str(root/'workload.py')],out,120)\n"
                    "ab.diag.main=diagnostic\n"
                    "raise SystemExit(ab.run_blocks({'baseline':root/'a','candidate':root/'b'},{},root/'results',{}))\n")
                process = subprocess.Popen([sys.executable, str(driver)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                ids = {}
                try:
                    deadline = time.monotonic() + 5
                    while not ready.exists() and time.monotonic() < deadline:
                        time.sleep(.01)
                    ids = json.loads(ready.read_text())
                    self.assertNotEqual(ids["group"], os.getpgrp())
                    process.send_signal(signum)
                    self.assertEqual(process.wait(timeout=10), 128 + signum)
                    def active(pid):
                        result = subprocess.run(["ps", "-o", "stat=", "-p", str(pid)], capture_output=True, text=True)
                        return bool(result.stdout.strip()) and not result.stdout.strip().startswith("Z")
                    deadline = time.monotonic() + 2
                    while any(active(ids[k]) for k in ("case", "peer")) and time.monotonic() < deadline:
                        time.sleep(.01)
                    self.assertFalse(any(active(ids[k]) for k in ("case", "peer")))
                    report = json.loads((root / "results/ab-report.json").read_text())
                    self.assertTrue(report["interrupted"])
                    self.assertFalse(report["complete"])
                    self.assertEqual(len(report["blocks"]), 1)
                    self.assertEqual(report["blocks"][0]["exit_code"], 128 + signum)
                finally:
                    if process.poll() is None:
                        process.send_signal(signal.SIGTERM)
                        try:
                            process.wait(timeout=10)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                    if ids:
                        try:
                            os.killpg(ids["group"], signal.SIGKILL)
                        except ProcessLookupError:
                            pass


if __name__ == "__main__":
    unittest.main()
