from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_timing_diagnostics as diagnostics
from test_aead_timing_interop import scorecard_for


def receiver_events(enabled: bool, count: int = 320) -> str:
    events = [{"event": "connected"}]
    for index in range(count):
        start = 100 + 100 * index
        event = {"event": "timing", "message_index": index,
                 "tsbpd_deadline_microseconds": start + 10,
                 "srt_release_microseconds": start + 20,
                 "udp_egress_microseconds": start + 30}
        if enabled:
            event.update(receive_start_microseconds=start,
                         udp_send_start_microseconds=start + 25)
        events.append(event)
    events.append({"event": "complete"})
    if enabled:
        for event, time in [(events[0], 10), (events[-1], 100 * count + 100)]:
            event.update(phase_clock_monotonic_before_microseconds=time,
                         phase_clock_realtime_microseconds=time + 1_000_000,
                         phase_clock_monotonic_after_microseconds=time + 1)
    return "\n".join(json.dumps(event) for event in events)


class TimingDiagnosticsTests(unittest.TestCase):
    def test_schedule_is_bounded_and_serial(self) -> None:
        self.assertEqual(diagnostics.schedule(2, 1), [
            ("control", 0), ("phase", 0), ("control", 1),
            ("phase", 1), ("strace", 0)])
        for counts in [(0, 0), (21, 0), (1, -1), (1, 6)]:
            with self.assertRaises(ValueError):
                diagnostics.schedule(*counts)

    def test_anchors_and_mode_are_fail_closed(self) -> None:
        for enabled in [False, True]:
            diagnostics.validate_receiver_evidence(receiver_events(enabled), enabled)
            with self.assertRaises(RuntimeError):
                diagnostics.validate_receiver_evidence(receiver_events(enabled), not enabled)
        with self.assertRaises(RuntimeError):
            diagnostics.validate_receiver_evidence('{}', True)

    def test_failure_is_retained_and_later_runs_are_not_skipped(self) -> None:
        scenario = next(s for s in diagnostics.aead.timing_scenarios()
                        if s.name == "ipv4-backup-group-path-outage-binary-1200")
        calls = []

        def invoke(command, directory, environment):
            calls.append(directory.name)
            enabled = "--write-phase-events" in command
            score = scorecard_for(scenario)
            score["release_deadline_error_microseconds"] = {"p99_9": 2000}
            score["measurement"]["phase_timing_enabled"] = enabled
            if len(calls) == 1:
                score["egress_deadline_error_microseconds"]["p99_9"] = 25_000
            (directory / "scorecard.json").write_text(json.dumps(score))
            (directory / "receiver.jsonl").write_text(receiver_events(enabled))
            if enabled:
                raw = [json.loads(line) for line in receiver_events(enabled).splitlines()]
                phases = diagnostics.timing.phase_observations(
                    [event for event in raw if event.get("event") == "timing"])
                (directory / "phases.jsonl").write_text(
                    "\n".join(json.dumps(event) for event in phases))
            return 1 if len(calls) == 1 else 0

        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            build = root / "build"
            build.mkdir()
            for name in ("timing_peer", "interop_peer", "group_timing_sender"):
                (build / f"robotweax_srt_{name}").touch()
            with mock.patch.object(diagnostics.os, "access", return_value=True), \
                 mock.patch.object(diagnostics, "invoke", side_effect=invoke):
                self.assertEqual(diagnostics.run_series(build, root / "results", 2, 0), 1)
                records = json.loads((root / "results/summary.json").read_text())
                self.assertEqual([r["passed"] for r in records], [False, True, True, True])
                self.assertEqual(len(calls), 4)
                with self.assertRaisesRegex(RuntimeError, "must be empty"):
                    diagnostics.run_series(build, root / "results", 2, 0)

    def test_missing_strace_is_not_silently_skipped(self) -> None:
        with mock.patch.object(diagnostics.platform, "system", return_value="Linux"), \
             mock.patch.object(diagnostics.shutil, "which", return_value=None):
            with self.assertRaisesRegex(RuntimeError, "no silent fallback"):
                diagnostics.run_series(Path("unused"), Path("unused"), 1, 1)

    def test_timeout_terminates_the_measurement_process_group(self) -> None:
        process = mock.Mock(pid=123)
        process.wait.side_effect = [diagnostics.subprocess.TimeoutExpired("peer", 90), -9]
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(diagnostics.subprocess, "Popen", return_value=process), \
             mock.patch.object(diagnostics.os, "killpg") as kill:
            self.assertEqual(diagnostics.invoke(["peer"], Path(temp), {}), 124)
            kill.assert_called_once_with(123, diagnostics.signal.SIGKILL)
            self.assertEqual(process.wait.call_count, 2)


if __name__ == "__main__":
    unittest.main()
