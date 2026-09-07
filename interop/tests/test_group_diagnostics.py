from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_group_diagnostics as diagnostics


class GroupDiagnosticsTests(unittest.TestCase):
    def make_peers(self, root):
        peer = root / "peer"
        peer.write_text("fake test peer")
        peer.chmod(0o755)
        return peer

    def test_bounds_are_checked_before_launch(self):
        for count in (0, -1, 61):
            with self.assertRaisesRegex(ValueError, "1..60"):
                diagnostics.run_series(Path("missing"), Path("missing"),
                                       Path("unused"), count)

    def test_backup_selection_reaches_worker_and_metadata(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            peer = self.make_peers(root)
            with mock.patch.object(diagnostics, "invoke", return_value=1) as invoke:
                self.assertEqual(diagnostics.run_series(
                    peer, peer, root / "results", 1, "group-backup-baseline"), 1)
            command = invoke.call_args.args[0]
            self.assertEqual(command[command.index("--scenario") + 1],
                             "group-backup-baseline")
            metadata = json.loads((root / "results/environment.json").read_text())
            self.assertEqual((metadata["policy"], metadata["profile"]),
                             ("backup", "baseline"))

    def test_single_backup_case_uses_original_contract_and_barriers(self):
        with mock.patch.object(sys, "argv", ["diagnostics",
                "--robotweax-peer", "/robotweax", "--reference-peer", "/reference",
                "--output-directory", "/evidence", "--single-case",
                "--scenario", "group-backup-baseline"]), \
                mock.patch.object(diagnostics, "resolve_program_path", side_effect=lambda p: p), \
                mock.patch.object(diagnostics.group, "run_case") as run:
            self.assertEqual(diagnostics.main(), 0)
            self.assertEqual(run.call_args.args[0:2], (Path("/reference"), Path("/robotweax")))
            self.assertEqual(run.call_args.args[3:5], ("backup", True))
            self.assertEqual(run.call_args.kwargs,
                             {"profile": "baseline", "evidence_directory": Path("/evidence")})

    def test_failures_are_retained_and_do_not_stop_the_series(self):
        calls = []

        def invoke(command, directory):
            calls.append(directory.name)
            self.assertIn("--single-case", command)
            peers = directory / "peers"
            peers.mkdir()
            for name in ("case.json", "caller.stdout", "caller.stderr",
                         "listener.stdout", "listener.stderr"):
                (peers / name).write_text("evidence")
            return 1 if len(calls) == 1 else 0

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            peer = self.make_peers(root)
            with mock.patch.object(diagnostics, "invoke", side_effect=invoke):
                self.assertEqual(diagnostics.run_series(peer, peer, root / "results", 3), 1)
                result = json.loads((root / "results/summary.json").read_text())
                self.assertEqual([r["passed"] for r in result["runs"]], [False, True, True])
                self.assertEqual(result["completed"], 3)
                self.assertFalse(result["passed"])
                self.assertEqual(calls, ["case-001", "case-002", "case-003"])
                with self.assertRaisesRegex(RuntimeError, "stale evidence"):
                    diagnostics.run_series(peer, peer, root / "results", 3)

    def test_budget_exhaustion_is_incomplete_not_success(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            peer = self.make_peers(root)
            with mock.patch.object(diagnostics.time, "monotonic", side_effect=[0, 590]), \
                    mock.patch.object(diagnostics, "invoke") as invoke:
                self.assertEqual(diagnostics.run_series(peer, peer, root / "results", 60), 1)
                invoke.assert_not_called()
            result = json.loads((root / "results/summary.json").read_text())
            self.assertEqual(result["reason"], "series-budget-exhausted")
            self.assertEqual(result["completed"], 0)
            self.assertFalse(result["passed"])

    def test_success_requires_retained_peer_logs(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            peer = self.make_peers(root)
            with mock.patch.object(diagnostics, "invoke", return_value=0):
                self.assertEqual(diagnostics.run_series(peer, peer, root / "results", 1), 1)
            result = json.loads((root / "results/summary.json").read_text())
            self.assertEqual(result["runs"][0]["error"], "missing-peer-evidence")

    def test_timeout_reaps_the_process_group_before_returning(self):
        process = mock.Mock(pid=123)
        process.wait.side_effect = [diagnostics.subprocess.TimeoutExpired("case", 40), -9]
        with tempfile.TemporaryDirectory() as temp, \
                mock.patch.object(diagnostics.subprocess, "Popen", return_value=process), \
                mock.patch.object(diagnostics.os, "killpg") as kill:
            self.assertEqual(diagnostics.invoke(["case"], Path(temp)), 124)
            kill.assert_called_once_with(123, diagnostics.signal.SIGKILL)
            self.assertEqual(process.wait.call_count, 2)

    def test_persistent_directory_keeps_partial_evidence_on_exception(self):
        with tempfile.TemporaryDirectory() as temp:
            evidence = Path(temp) / "case"
            with self.assertRaisesRegex(RuntimeError, "injected"):
                with diagnostics.group.group_case_directory(evidence) as directory:
                    (directory / "listener.stderr").write_text("partial failure")
                    raise RuntimeError("injected")
            self.assertEqual((evidence / "listener.stderr").read_text(), "partial failure")
            with self.assertRaises(FileExistsError):
                with diagnostics.group.group_case_directory(evidence):
                    self.fail("stale directory accepted")

    def test_workflow_is_manual_only_and_uploads_no_binaries(self):
        root = Path(__file__).resolve().parents[2]
        workflow = (root / ".github/workflows/timing-diagnostics.yml").read_text()
        job = workflow.split("  group_receive_contract:\n", 1)[1]
        self.assertIn("if: github.event_name == 'workflow_dispatch' && "
                      "(inputs.investigation == 'group-receive-contract' || "
                      "inputs.investigation == 'group-backup-baseline' || "
                      "inputs.investigation == 'group-matrix-context' || "
                      "inputs.investigation == 'group-receive-stack' || "
                      "inputs.investigation == 'group-strace-stack')", job)
        self.assertIn("if: github.event_name != 'workflow_dispatch' || "
                      "inputs.investigation == 'timing'", workflow)
        self.assertIn('--scenario "$DIAGNOSTIC_SCENARIO"', job)
        self.assertIn("if: inputs.investigation != 'group-matrix-context'", job)
        self.assertIn("if: inputs.investigation == 'group-matrix-context'", job)
        self.assertIn("python3 interop/run_group_matrix_diagnostics.py", job)
        self.assertIn("ref: " + diagnostics.REFERENCE_COMMIT, job)
        self.assertIn("--repetitions 60", job)
        self.assertIn("if: always()", job)
        self.assertNotIn("continue-on-error", job)
        self.assertNotIn("actions/cache", job)
        upload = job.split("          path: |\n", 1)[1].split("          retention-days:")[0]
        self.assertEqual([line.strip() for line in upload.splitlines()], [
            "group-diagnostic-results/**/*." + suffix
            for suffix in ("json", "jsonl", "log", "stdout", "stderr")
        ] + ["group-diagnostic-metadata/*.txt"])


if __name__ == "__main__":
    unittest.main()
