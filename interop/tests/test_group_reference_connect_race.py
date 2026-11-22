from __future__ import annotations

import io
import json
from contextlib import redirect_stdout
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_group_interop as group

# Caller and listener lines as recorded for Robotweax/srt issue 417.
CALLER = (
    "group connect failed: Success; member=983868868; "
    "endpoint_ids=983868869,983868868; endpoint_errors=0,0\n"
)


def listener(**changes):
    event = dict(event="group_transfer_failure", operation="receive-range",
                 reason="api", group=1635095287, message_index=0, result=-1,
                 expected_bytes=188, api_error=6003)
    event.update(changes)
    return (
        "1795342706424385/1W:socket: UDP receive buffer: requested=12288000\n"
        + json.dumps(event) + "\n"
        + "broadcast receive/reply failed: Non-blocking call failure: "
          "transmission timed out\n"
    )


class ReferenceConnectRaceSignatureTests(unittest.TestCase):
    def test_recorded_signature_is_recognised(self):
        self.assertTrue(
            group.is_reference_group_connect_race(4, CALLER, listener()))

    def test_other_outcomes_are_not_the_signature(self):
        cases = {
            "caller exit status": (3, CALLER, listener()),
            "caller succeeded": (0, CALLER, listener()),
            "endpoint error": (
                4, CALLER.replace("endpoint_errors=0,0", "endpoint_errors=0,2001"),
                listener()),
            "library error": (
                4, CALLER.replace("Success", "Connection setup failure"), listener()),
            "invalid member": (4, CALLER.replace("member=983868868", "member=-1"),
                               listener()),
            "member not an endpoint": (
                4, CALLER.replace("member=983868868", "member=7"), listener()),
            "invalid endpoint": (
                4, CALLER.replace("endpoint_ids=983868869", "endpoint_ids=-1"),
                listener()),
            "no caller line": (4, "", listener()),
            "listener received payload": (4, CALLER, listener(message_index=3)),
            "listener metadata failure": (
                4, CALLER, listener(reason="message-or-member-metadata")),
            "listener other error": (4, CALLER, listener(api_error=2001)),
            "listener send failure": (4, CALLER, listener(operation="send-range")),
            "no listener event": (4, CALLER, "not json\n{broken\n"),
        }
        for name, arguments in cases.items():
            with self.subTest(name):
                self.assertFalse(group.is_reference_group_connect_race(*arguments))


class ReferenceConnectRaceRetryTests(unittest.TestCase):
    def run_helper(self, outcomes):
        calls = []

        def case(*args, **kwargs):
            calls.append((args, kwargs))
            outcome = outcomes[len(calls) - 1]
            if outcome is not None:
                raise outcome

        output = io.StringIO()
        with mock.patch.object(group, "run_case", side_effect=case), \
                redirect_stdout(output):
            try:
                group.run_reference_bonded_caller_case(
                    group.MatrixEvidence(None), "listener", "caller", "name",
                    "backup", False, True)
                error = None
            except Exception as caught:  # noqa: BLE001 - the test inspects it
                error = caught
        return calls, output.getvalue(), error

    def test_pass_runs_once_without_notice(self):
        calls, output, error = self.run_helper([None])
        self.assertIsNone(error)
        self.assertEqual(len(calls), 1)
        self.assertEqual(output, "")

    def test_race_is_repeated_once_with_the_same_arguments(self):
        first = group.ReferenceGroupConnectRace("first failure text")
        calls, output, error = self.run_helper([first, None])
        self.assertIsNone(error)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0], calls[1])
        self.assertIn("RETRY", output)
        self.assertIn("issue 417", output)
        self.assertIn("first failure text", output)

    def test_second_race_fails(self):
        calls, _, error = self.run_helper([
            group.ReferenceGroupConnectRace("first"),
            group.ReferenceGroupConnectRace("second")])
        self.assertIsInstance(error, group.ReferenceGroupConnectRace)
        self.assertEqual(str(error), "second")
        self.assertEqual(len(calls), 2)

    def test_other_failure_is_not_repeated(self):
        calls, output, error = self.run_helper([RuntimeError("other")])
        self.assertEqual(type(error), RuntimeError)
        self.assertEqual(len(calls), 1)
        self.assertNotIn("RETRY", output)

    def test_other_failure_after_a_race_fails(self):
        calls, _, error = self.run_helper([
            group.ReferenceGroupConnectRace("first"), RuntimeError("other")])
        self.assertEqual(type(error), RuntimeError)
        self.assertEqual(len(calls), 2)

    def test_only_the_bonded_reference_caller_case_is_repeated(self):
        names = []
        for stop_after in range(1, 5):
            calls = []
            seen = []

            def limited(*args, **kwargs):
                seen.append(args[2])
                if len(set(seen)) == stop_after:
                    calls.append(args[2])
                    raise group.ReferenceGroupConnectRace("race")

            with mock.patch.object(sys, "argv", [
                    "matrix", "--robotweax-peer", "/r", "--reference-peer",
                    "/h", "--baseline-only"]), \
                    mock.patch.object(group, "resolve_program_path",
                                      side_effect=lambda p: p), \
                    mock.patch.object(group, "run_case", side_effect=limited), \
                    mock.patch.object(group, "run_peer_error_case"), \
                    redirect_stdout(io.StringIO()):
                with self.assertRaises(group.ReferenceGroupConnectRace):
                    group.main()
            names.append((seen[-1], len(calls)))
        self.assertEqual(names, [
            ("Robotweax group caller -> Haivision mirror group", 1),
            ("Haivision group caller -> Robotweax mirror group", 1),
            ("Robotweax group caller -> Haivision bonded listeners", 1),
            ("Haivision group caller -> Robotweax bonded listeners", 2),
        ])


if __name__ == "__main__":
    unittest.main()
