from pathlib import Path
import json
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_group_stack_diagnostics as stack


class StackDiagnosticsTests(unittest.TestCase):
    def test_only_open_slow_receive_triggers(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "stderr"
            begin = dict(event="group_phase", operation="receive-payload", edge="begin",
                         index=14, monotonic_us=100)
            path.write_text(json.dumps(begin) + "\n")
            self.assertIsNone(stack.stalled_receive(path, 750099))
            self.assertEqual(stack.stalled_receive(path, 750100), begin)
            path.write_text(json.dumps(begin) + "\n" + json.dumps(dict(begin, edge="end")))
            self.assertIsNone(stack.stalled_receive(path, 900000))

    def test_rejects_unowned_or_invalid_process(self):
        for identity in ({}, {"pid": 1, "parent_pid": 22},
                         {"pid": 123, "parent_pid": 23}):
            self.assertFalse(stack.verified_target(identity, Path("/peer"), 22))

    def test_snapshot_is_bounded_and_does_not_dump_arguments_or_memory(self):
        command = stack.snapshot_command(42)
        self.assertEqual(command[:6], ["sudo", "-n", "timeout", "--signal=TERM", "--kill-after=1s", "2s"])
        self.assertIn("set print frame-arguments none", command)
        script = Path(stack.__file__).with_name("group_stack.gdb").read_text()
        self.assertIn("set may-call-functions off", script)
        self.assertIn("thread apply all bt 24", script)
        for forbidden in ("bt full", "generate-core", "info locals"):
            self.assertNotIn(forbidden, script)

    def test_no_stall_is_not_claimed_as_stack_coverage(self):
        process = mock.Mock(pid=123)
        process.poll.return_value = 0
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(stack.subprocess, "Popen", return_value=process), \
             mock.patch.object(stack.subprocess, "run") as debugger, \
             mock.patch.object(stack.os, "killpg"):
            result = stack.capture(["matrix"], Path("/reference"), Path(temp), {})
        self.assertEqual(result["coverage"], "no-stall-observed")
        self.assertFalse(result["snapshot_attempted"])
        debugger.assert_not_called()

    def test_one_verified_snapshot_is_retained(self):
        process = mock.Mock(pid=123)
        process.poll.side_effect = [None, 0, 0, 0]
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            peer = directory / "matrix/case-001/peers"
            peer.mkdir(parents=True)
            (peer / "listener.stderr").touch()
            (peer / "listener-process.json").write_text(json.dumps({"pid": 456, "parent_pid": 123}))
            def debug(command, **kwargs):
                kwargs["stdout"].write("GROUP_STACK_BEGIN\n#0 wait\nGROUP_STACK_END\n")
                return mock.Mock(returncode=0)
            with mock.patch.object(stack.subprocess, "Popen", return_value=process), \
                 mock.patch.object(stack, "stalled_receive", return_value={"index": 14}), \
                 mock.patch.object(stack, "verified_target", return_value=True), \
                 mock.patch.object(stack.subprocess, "run", side_effect=debug) as debugger, \
                 mock.patch.object(stack.os, "killpg"), mock.patch.object(stack.os, "kill"), \
                 mock.patch.object(stack.time, "sleep"):
                result = stack.capture(["matrix"], Path("/reference"), directory, {})
            self.assertEqual(result["coverage"], "stack-captured")
            self.assertEqual(debugger.call_count, 1)
            self.assertTrue((directory / "snapshot.json").is_file())

    def test_workflow_stack_mode_excludes_strace_and_serial_repetitions(self):
        workflow = Path(stack.__file__).parents[1] / ".github/workflows/timing-diagnostics.yml"
        text = workflow.read_text()
        self.assertIn("inputs.investigation != 'group-receive-stack'", text)
        self.assertIn("if: inputs.investigation == 'group-receive-stack'", text)
        self.assertIn('debug_flags=(-DCMAKE_CXX_FLAGS=-g)', text)
        self.assertIn('run_group_stack_diagnostics.py', text)


if __name__ == "__main__":
    unittest.main()
