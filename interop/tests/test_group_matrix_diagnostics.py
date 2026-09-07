from __future__ import annotations

import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_group_interop as group
import run_group_matrix_diagnostics as diagnostics

def phase_text():
    return "".join(json.dumps(dict(event="group_phase", operation="receive-payload",
        edge=edge, index=0, result=0, monotonic_us=index, unix_us=index)) + "\n"
        for index, edge in enumerate(("begin", "end")))


@unittest.skipUnless(os.name == "posix", "POSIX diagnostic runner")
class MatrixDiagnosticsTests(unittest.TestCase):
    def test_ci_matrix_order_and_fail_fast_are_unchanged(self):
        calls = []
        def case(*args, **kwargs):
            calls.append((args[3], bool(args[5]) if len(args) > 5 else False,
                          kwargs.get("profile", "baseline")))
        with mock.patch.object(sys, "argv", ["matrix", "--robotweax-peer", "/r",
                "--reference-peer", "/h", "--baseline-only"]), \
                mock.patch.object(group, "resolve_program_path", side_effect=lambda p: p), \
                mock.patch.object(group, "run_case", side_effect=case) as runner, \
                mock.patch.object(group, "run_peer_error_case") as peer_error:
            self.assertEqual(group.main(), 0)
            self.assertEqual(len(calls), 14)
            self.assertEqual(calls[:8], [(p, bonded, "baseline") for p in
                             ("broadcast", "backup") for bonded in (False, False, True, True)])
            self.assertEqual(calls[8:12], [(p, False, profile) for p in
                             ("broadcast", "backup") for profile in
                             ("reference-receive-contract", "receive-contract")])
            peer_error.assert_called_once()
            runner.side_effect = RuntimeError("injected stop")
            peer_error.reset_mock()
            with self.assertRaisesRegex(RuntimeError, "injected stop"):
                group.main()
            peer_error.assert_not_called()

    def test_recorder_retains_failure_and_refuses_stale_output(self):
        def fail():
            raise RuntimeError("original failure")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp) / "evidence"
            recorder = group.MatrixEvidence(root)
            with self.assertRaisesRegex(RuntimeError, "original failure"):
                recorder.run(fail)
            result = json.loads((root / "case-001/result.json").read_text())
            self.assertFalse(result["passed"])
            self.assertEqual(result["status"], "failed")
            self.assertIn("original failure", result["error"])
            self.assertGreaterEqual(result["finished_unix_ns"], result["started_unix_ns"])
            with self.assertRaises(FileExistsError):
                group.MatrixEvidence(root)

    def test_failed_matrix_is_not_replaced_by_later_success(self):
        calls = []
        def invoke(command, directory, timeout, environment):
            calls.append(directory.name)
            if directory.name.startswith("matrix-"):
                self.assertEqual(timeout, 120)
                self.assertIn("--baseline-only", command)
                return 1
            self.assertEqual(timeout, 40)
            peers = directory / "peers"
            peers.mkdir()
            for name in ("case.json", "caller.stdout", "caller.stderr",
                         "listener.stdout", "listener.stderr"):
                (peers / name).touch()
            return 0
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            peer = root / "peer"
            peer.write_text("test peer")
            peer.chmod(0o755)
            with mock.patch.object(diagnostics, "invoke", side_effect=invoke):
                self.assertEqual(diagnostics.run(peer, peer, root / "out", False), 1)
            self.assertEqual(calls, ["isolated-before", "matrix-control", "isolated-after", "matrix-phase"])
            results = json.loads((root / "out/summary.json").read_text())
            self.assertEqual([r["passed"] for r in results], [True, False, True, False])

    def test_missing_evidence_cannot_pass(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            peer = root / "peer"
            peer.write_text("test")
            peer.chmod(0o755)
            with mock.patch.object(diagnostics, "invoke", return_value=0):
                self.assertEqual(diagnostics.run(peer, peer, root / "out", False), 1)
            results = json.loads((root / "out/summary.json").read_text())
            self.assertTrue(all(not r["passed"] for r in results))

    def test_trace_is_required_not_silently_skipped(self):
        with mock.patch.object(diagnostics.platform, "system", return_value="Darwin"):
            with self.assertRaisesRegex(RuntimeError, "strace required"):
                diagnostics.run(Path("missing"), Path("missing"), Path("unused"))

    def test_complete_schedule_keeps_trace_separate(self):
        calls = []
        def invoke(command, directory, timeout, environment):
            calls.append(directory.name)
            matrix = directory.name.startswith("matrix-")
            phase = directory.name in ("matrix-phase", "matrix-strace")
            self.assertEqual(environment["ROBOTWEAX_SRT_GROUP_PHASE_TRACE"], "1" if phase else "0")
            for index in range(15 if matrix else 1):
                case = directory / "matrix" / f"case-{index + 1:03d}" if matrix else directory
                case.mkdir(parents=True, exist_ok=True)
                if matrix:
                    (case / "result.json").write_text('{"passed": true}')
                if index < 14:
                    peers = case / "peers"
                    peers.mkdir()
                    for name in ("case.json", "caller.stdout", "caller.stderr",
                                 "listener.stdout", "listener.stderr"):
                        (peers / name).touch()
                    if phase:
                        for role in ("caller", "listener"):
                            (peers / f"{role}.stderr").write_text(phase_text())
            if directory.name == "matrix-strace":
                self.assertEqual(command[0], "strace")
                (directory / "syscalls.log").write_text("synthetic trace")
            else:
                self.assertNotEqual(command[0], "strace")
            return 0
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            peer = root / "peer"
            peer.write_text("test peer")
            peer.chmod(0o755)
            with mock.patch.object(diagnostics.platform, "system", return_value="Linux"), \
                    mock.patch.object(diagnostics.shutil, "which", return_value="/strace"), \
                    mock.patch.object(diagnostics, "invoke", side_effect=invoke):
                self.assertEqual(diagnostics.run(peer, peer, root / "out"), 0)
            self.assertEqual(calls, ["isolated-before", "matrix-control",
                                    "isolated-after", "matrix-phase", "matrix-strace"])

    def test_trace_does_not_decode_payload_or_option_buffers(self):
        command = diagnostics.trace_command(["python", "matrix.py"], Path("trace.log"))
        self.assertEqual(command[-2:], ["python", "matrix.py"])
        raw = next(value for value in command if value.startswith("raw="))
        for name in ("sendto", "recvfrom", "sendmsg", "recvmsg", "sendmmsg",
                     "recvmmsg", "setsockopt", "getsockopt"):
            self.assertIn(name, raw)
        self.assertNotIn("read,write", " ".join(command))

    def test_phase_validation_rejects_missing_partial_or_control_instrumentation(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "peer.stderr"
            path.write_text(phase_text())
            diagnostics.validate_phases(path, True)
            with self.assertRaisesRegex(RuntimeError, "instrumented control"):
                diagnostics.validate_phases(path, False)
            path.write_text(phase_text().splitlines()[0] + "\n")
            with self.assertRaisesRegex(RuntimeError, "incomplete phase"):
                diagnostics.validate_phases(path, True)
            path.write_text("")
            diagnostics.validate_phases(path, False)
            with self.assertRaisesRegex(RuntimeError, "missing phase"):
                diagnostics.validate_phases(path, True)

    def test_stack_comparison_preserves_failure_and_requires_frames(self):
        for frames in (True, False):
            with self.subTest(frames=frames), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                peer = root / "peer"
                peer.write_text("peer")
                peer.chmod(0o755)
                calls = []
                def invoke(command, directory, timeout, environment):
                    calls.append(directory.name)
                    self.assertEqual(timeout, 120)
                    traced = directory.name.startswith("matrix-strace")
                    self.assertEqual(environment["ROBOTWEAX_SRT_GROUP_PHASE_TRACE"], "1" if traced else "0")
                    if directory.name == "matrix-strace":
                        self.assertNotIn("-k", command)
                        return 1
                    if traced:
                        self.assertIn("-k", command)
                        self.assertIn("--stack-trace-frame-limit=24", command)
                        (directory / "syscalls.log").write_text(" > /peer(receive+0x10)\n" if frames else "no frames\n")
                    for index in range(15):
                        case = directory / "matrix" / f"case-{index+1:03d}"
                        case.mkdir(parents=True)
                        (case / "result.json").write_text('{"passed":true}')
                        if index < 14:
                            peers = case / "peers"
                            peers.mkdir()
                            for name in ("case.json", "caller.stdout", "caller.stderr", "listener.stdout", "listener.stderr"):
                                (peers / name).write_text(phase_text() if traced and name.endswith("stderr") else "")
                    return 0
                with mock.patch.object(diagnostics.platform, "system", return_value="Linux"), \
                     mock.patch.object(diagnostics.shutil, "which", return_value="/strace"), \
                     mock.patch.object(diagnostics, "invoke", side_effect=invoke):
                    self.assertEqual(diagnostics.run(peer, peer, root / "out", stack_trace=True), 1)
                self.assertEqual(calls, ["matrix-control", "matrix-strace", "matrix-strace-stack"])
                records = json.loads((root / "out/summary.json").read_text())
                self.assertEqual([r["passed"] for r in records], [True, False, frames])
                self.assertEqual(records[-1]["stack_frame_lines"], int(frames))

    def test_stack_trace_cannot_silently_run_without_strace(self):
        with self.assertRaisesRegex(RuntimeError, "requires Linux strace"):
            diagnostics.run(Path("r"), Path("h"), Path("out"), trace=False, stack_trace=True)


if __name__ == "__main__":
    unittest.main()
