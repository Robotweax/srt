from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_receive_contract_interop  # noqa: E402


class ReceiveContractInteropUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.robotweax = Path("/robotweax")
        self.reference = Path("/haivision")
        self.options = run_receive_contract_interop.RunOptions()
        self.scenarios = run_receive_contract_interop.scenario_matrix(
            self.robotweax, self.reference
        )

    def test_matrix_covers_both_modes_and_directions(self) -> None:
        self.assertEqual(len(self.scenarios), 4)
        self.assertEqual(
            {
                (
                    scenario.message_api,
                    scenario.sender,
                    scenario.receiver,
                )
                for scenario in self.scenarios
            },
            {
                (True, self.robotweax, self.reference),
                (True, self.reference, self.robotweax),
                (False, self.robotweax, self.reference),
                (False, self.reference, self.robotweax),
            },
        )
        self.assertTrue(self.scenarios[0].reference_short_buffer_loss)
        self.assertFalse(
            any(
                scenario.reference_short_buffer_loss
                for scenario in self.scenarios[1:]
            )
        )

    def test_message_commands_use_contract_barriers_and_shutdown(self) -> None:
        scenario = self.scenarios[0]
        sender = run_receive_contract_interop.peer_command(
            scenario, "caller", 9_999, Path("/input"), self.options
        )
        receiver = run_receive_contract_interop.peer_command(
            scenario, "listener", 9_999, Path("/output"), self.options
        )
        self.assertEqual(sender[0], str(self.robotweax))
        self.assertIn("--receive-contract-sender", sender)
        self.assertIn("--receive-contract-receiver", receiver)
        self.assertIn("--expect-peer-shutdown", receiver)
        self.assertNotIn("--expect-eof", receiver)
        self.assertEqual(sender[sender.index("--transport") + 1], "live")
        self.assertEqual(sender[sender.index("--message-api") + 1], "true")

    def test_stream_commands_use_non_aligned_buffers_and_eof(self) -> None:
        scenario = self.scenarios[2]
        receiver = run_receive_contract_interop.peer_command(
            scenario, "listener", 9_999, Path("/output"), self.options
        )
        self.assertIn("--expect-eof", receiver)
        self.assertNotIn("--expect-peer-shutdown", receiver)
        self.assertEqual(
            receiver[receiver.index("--receive-size") + 1], "997"
        )
        self.assertEqual(
            receiver[receiver.index("--transport") + 1], "file"
        )
        self.assertEqual(
            receiver[receiver.index("--message-api") + 1], "false"
        )
        self.assertEqual(
            receiver[receiver.index("--congestion") + 1], "file"
        )

    def test_receive_contract_requires_exact_errors_and_timing(self) -> None:
        event = {
            "event": "receive_contract",
            "role": "listener",
            "matched": True,
            "message_api": True,
            "nonblocking_result": -1,
            "nonblocking_error": 6_002,
            "nonblocking_elapsed_us": 120,
            "timeout_result": -1,
            "timeout_error": 6_003,
            "timeout_elapsed_us": 150_250,
            "timeout_configured_ms": 150,
        }
        self.assertTrue(
            run_receive_contract_interop.has_valid_receive_contract(
                json.dumps(event), message_api=True
            )
        )
        for field, replacement in (
            ("message_api", False),
            ("nonblocking_error", 6_003),
            ("nonblocking_elapsed_us", 500_000),
            ("timeout_error", 6_002),
            ("timeout_elapsed_us", 49_999),
            ("timeout_configured_ms", 151),
        ):
            with self.subTest(field=field):
                invalid = dict(event)
                invalid[field] = replacement
                self.assertFalse(
                    run_receive_contract_interop.has_valid_receive_contract(
                        json.dumps(invalid), message_api=True
                    )
                )

    def test_roleless_ready_event_can_be_selected(self) -> None:
        output = '{"event":"ready","port":9999,"srt_version":66821}'
        self.assertIsNotNone(
            run_receive_contract_interop.find_event(output, "ready")
        )
        self.assertIsNone(
            run_receive_contract_interop.find_event(
                output, "ready", "listener"
            )
        )

    def test_message_terminal_drain_requires_live_buffer_rejection(
        self,
    ) -> None:
        event = {
            "event": "terminal_drain_ready",
            "role": "listener",
            "matched": True,
            "message_api": True,
            "socket_state": 6,
            "short_result": -1,
            "short_error": 5_009,
            "short_buffer_bytes": 188,
        }
        self.assertTrue(
            run_receive_contract_interop.has_valid_terminal_drain(
                json.dumps(event), message_api=True
            )
        )
        event["short_error"] = 2_001
        self.assertFalse(
            run_receive_contract_interop.has_valid_terminal_drain(
                json.dumps(event), message_api=True
            )
        )

    def test_stream_terminal_drain_does_not_claim_message_probe(self) -> None:
        event = {
            "event": "terminal_drain_ready",
            "role": "listener",
            "matched": True,
            "message_api": False,
            "socket_state": 6,
            "short_result": 0,
            "short_error": 0,
            "short_buffer_bytes": 188,
        }
        self.assertTrue(
            run_receive_contract_interop.has_valid_terminal_drain(
                json.dumps(event), message_api=False
            )
        )
        event["socket_state"] = 5
        self.assertFalse(
            run_receive_contract_interop.has_valid_terminal_drain(
                json.dumps(event), message_api=False
            )
        )

    def test_terminal_result_is_mode_specific(self) -> None:
        message = json.dumps(
            {
                "event": "peer_shutdown",
                "role": "listener",
                "matched": True,
                "bytes": 1_316,
                "message_api": True,
                "terminal": "connection-lost",
                "result": -1,
                "error": 2_001,
                "socket_state": 6,
            }
        )
        stream = json.dumps(
            {"event": "eof", "role": "listener", "bytes": 4_267}
        )
        self.assertTrue(
            run_receive_contract_interop.has_valid_terminal_result(
                message, message_api=True, byte_count=1_316
            )
        )
        self.assertTrue(
            run_receive_contract_interop.has_valid_terminal_result(
                stream, message_api=False, byte_count=4_267
            )
        )
        self.assertFalse(
            run_receive_contract_interop.has_valid_terminal_result(
                message, message_api=False, byte_count=1_316
            )
        )

    def test_reference_short_buffer_loss_classifier_is_fail_closed(
        self,
    ) -> None:
        scenario = self.scenarios[0]
        sender = json.dumps(
            {"event": "complete", "role": "caller", "bytes": 1_316}
        )
        receive_contract = {
            "event": "receive_contract",
            "role": "listener",
            "matched": True,
            "message_api": True,
            "nonblocking_result": -1,
            "nonblocking_error": 6_002,
            "nonblocking_elapsed_us": 50,
            "timeout_result": -1,
            "timeout_error": 6_003,
            "timeout_elapsed_us": 150_100,
            "timeout_configured_ms": 150,
        }
        terminal_drain = {
            "event": "terminal_drain_ready",
            "role": "listener",
            "matched": True,
            "message_api": True,
            "socket_state": 6,
            "short_result": -1,
            "short_error": 5_009,
            "short_buffer_bytes": 188,
        }
        failure = {
            "event": "failure",
            "role": "listener",
            "bytes": 0,
            "srt_version": (
                run_receive_contract_interop.PINNED_REFERENCE_VERSION
            ),
        }
        receiver = "\n".join(
            json.dumps(event)
            for event in (receive_contract, terminal_drain, failure)
        )
        arguments = {
            "scenario": scenario,
            "sender_returncode": 0,
            "receiver_returncode": 6,
            "sender_stdout": sender,
            "receiver_stdout": receiver,
            "receiver_stderr": (
                "receive failed after 0 bytes: Connection was broken"
            ),
            "output_bytes": 0,
        }
        self.assertTrue(
            run_receive_contract_interop.has_expected_reference_short_buffer_loss(
                **arguments
            )
        )
        for field, replacement in (
            ("receiver_returncode", 0),
            ("receiver_stderr", "different failure"),
            ("output_bytes", 1),
            (
                "receiver_stdout",
                receiver.replace('"short_error": 5009', '"short_error": 2001'),
            ),
            (
                "receiver_stdout",
                receiver.replace(
                    '"srt_version": '
                    f'{run_receive_contract_interop.PINNED_REFERENCE_VERSION}',
                    '"srt_version": 0',
                ),
            ),
        ):
            with self.subTest(field=field):
                invalid = dict(arguments)
                invalid[field] = replacement
                self.assertFalse(
                    run_receive_contract_interop.has_expected_reference_short_buffer_loss(
                        **invalid
                    )
                )
        invalid_scenario = dict(arguments)
        invalid_scenario["scenario"] = self.scenarios[1]
        self.assertFalse(
            run_receive_contract_interop.has_expected_reference_short_buffer_loss(
                **invalid_scenario
            )
        )


if __name__ == "__main__":
    unittest.main()
