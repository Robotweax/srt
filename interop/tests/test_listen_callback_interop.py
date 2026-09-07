from __future__ import annotations

import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_listen_callback_interop  # noqa: E402


class ListenCallbackInteropUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.robotweax = Path("/robotweax")
        self.reference = Path("/haivision")
        self.options = run_listen_callback_interop.RunOptions()
        self.scenarios = run_listen_callback_interop.scenario_matrix(
            self.robotweax, self.reference
        )

    def test_matrix_uses_each_implementation_as_listener(self) -> None:
        self.assertEqual(len(self.scenarios), 2)
        self.assertEqual(self.scenarios[0].listener, self.robotweax)
        self.assertEqual(self.scenarios[0].caller, self.reference)
        self.assertEqual(self.scenarios[1].listener, self.reference)
        self.assertEqual(self.scenarios[1].caller, self.robotweax)

    def test_accept_commands_pass_exact_stream_id_to_both_roles(self) -> None:
        scenario = self.scenarios[0]
        listener = run_listen_callback_interop.peer_command(
            scenario.listener,
            "listener",
            9_999,
            Path("/output"),
            scenario,
            self.options,
        )
        caller = run_listen_callback_interop.peer_command(
            scenario.caller,
            "caller",
            9_999,
            Path("/input"),
            scenario,
            self.options,
        )
        self.assertEqual(
            listener[listener.index("--listen-callback-stream-id") + 1],
            scenario.stream_id,
        )
        self.assertEqual(
            listener[listener.index("--expect-stream-id") + 1],
            scenario.stream_id,
        )
        self.assertEqual(
            caller[caller.index("--stream-id") + 1],
            scenario.stream_id,
        )
        self.assertIn("--connect-callback", caller)
        self.assertNotIn("--require-connect-callback", caller)

        robotweax_caller_scenario = self.scenarios[1]
        robotweax_caller = run_listen_callback_interop.peer_command(
            robotweax_caller_scenario.caller,
            "caller",
            9_999,
            Path("/input"),
            robotweax_caller_scenario,
            self.options,
        )
        self.assertIn("--require-connect-callback", robotweax_caller)
        self.assertEqual(
            listener[
                listener.index("--listen-callback-passphrase-env") + 1
            ],
            run_listen_callback_interop.PASSPHRASE_ENVIRONMENT,
        )
        self.assertEqual(
            caller[caller.index("--passphrase-env") + 1],
            run_listen_callback_interop.PASSPHRASE_ENVIRONMENT,
        )
        self.assertNotIn("--expect-reject", caller)

    def test_rejection_commands_require_application_reason(self) -> None:
        scenario = self.scenarios[1]
        listener = run_listen_callback_interop.peer_command(
            scenario.listener,
            "listener",
            9_999,
            Path("/output"),
            scenario,
            self.options,
            reject=True,
        )
        caller = run_listen_callback_interop.peer_command(
            scenario.caller,
            "caller",
            9_999,
            Path("/input"),
            scenario,
            self.options,
            reject=True,
        )
        expected = str(
            run_listen_callback_interop.APPLICATION_REJECTION_REASON
        )
        self.assertEqual(
            listener[listener.index("--listen-callback-reject") + 1],
            expected,
        )
        self.assertEqual(
            caller[caller.index("--expect-reject") + 1], expected
        )
        self.assertNotIn("--expect-stream-id", listener)
        self.assertNotIn("--connect-callback", caller)

    def test_callback_event_requires_every_contract_field(self) -> None:
        scenario = self.scenarios[0]
        event = {
            "event": "listen_callback",
            "valid": True,
            "handshake_version": 5,
            "peer_family": 2,
            "stream_id_bytes": len(scenario.stream_id),
            "option_updated": True,
            "security_updated": True,
            "rejected": False,
            "reject_reason": 0,
        }
        import json

        self.assertTrue(
            run_listen_callback_interop.callback_was_valid(
                json.dumps(event), scenario, False
            )
        )
        event["option_updated"] = False
        self.assertFalse(
            run_listen_callback_interop.callback_was_valid(
                json.dumps(event), scenario, False
            )
        )

    def test_connect_callback_event_requires_completion_contract(self) -> None:
        import json

        event = {
            "event": "connect_callback",
            "valid": True,
            "error_code": 0,
            "peer_family": 2,
            "token": -1,
            "socket_state": 5,
            "peer_name_available": True,
        }
        self.assertTrue(
            run_listen_callback_interop.connect_callback_was_valid(
                json.dumps(event)
            )
        )
        event["token"] = 0
        self.assertFalse(
            run_listen_callback_interop.connect_callback_was_valid(
                json.dumps(event)
            )
        )

    def test_reference_callback_error_is_diagnostic_only(self) -> None:
        output = (
            '{"event":"connect_callback","valid":false,'
            '"error_code":2001,"peer_family":10,"token":-1,'
            '"socket_state":6,"peer_name_available":false}'
        )
        self.assertTrue(
            run_listen_callback_interop.connect_callback_meets_policy(
                output, required=False
            )
        )
        self.assertFalse(
            run_listen_callback_interop.connect_callback_meets_policy(
                output, required=True
            )
        )


if __name__ == "__main__":
    unittest.main()
