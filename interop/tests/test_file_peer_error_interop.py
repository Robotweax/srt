from __future__ import annotations

import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_file_peer_error_interop  # noqa: E402


class FilePeerErrorInteropTests(unittest.TestCase):
    def setUp(self) -> None:
        self.robotweax = Path("/tmp/robotweax-peer")
        self.reference = Path("/tmp/reference-peer")

    def test_matrix_covers_both_implementation_directions(self) -> None:
        scenarios = run_file_peer_error_interop.scenario_matrix(
            self.robotweax, self.reference
        )
        self.assertEqual(len(scenarios), 2)
        self.assertEqual(
            {(item.sender, item.receiver) for item in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )

    def test_commands_select_public_file_helpers_and_expectations(self) -> None:
        sender = run_file_peer_error_interop.peer_command(
            self.robotweax,
            "caller",
            12_345,
            Path("/tmp/input.bin"),
            1_000_000,
            30,
            2_000,
        )
        receiver = run_file_peer_error_interop.peer_command(
            self.reference,
            "listener",
            12_345,
            Path("/dev/full"),
            1_000_000,
            30,
            2_000,
        )
        for command in (sender, receiver):
            self.assertIn("--file-api", command)
            self.assertEqual(
                command[command.index("--transport") + 1], "file"
            )
            self.assertEqual(
                command[command.index("--congestion") + 1], "file"
            )
        self.assertIn("--expect-peer-error", sender)
        self.assertNotIn("--expect-file-write-error", sender)
        self.assertIn("--input", sender)
        self.assertIn("--expect-file-write-error", receiver)
        self.assertNotIn("--expect-peer-error", receiver)
        self.assertIn("--output", receiver)
        self.assertEqual(
            receiver[receiver.index("--shutdown-grace-ms") + 1],
            "2000",
        )

    def test_command_rejects_an_unsupported_role(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported role"):
            run_file_peer_error_interop.peer_command(
                self.robotweax,
                "rendezvous-sender",
                12_345,
                Path("/tmp/input.bin"),
                1_000_000,
                30,
                2_000,
            )

    def test_event_validation_requires_both_bounded_matches(self) -> None:
        scenario = run_file_peer_error_interop.Scenario(
            "peer-error", self.robotweax, self.reference, 1
        )
        sender = (
            '{"event":"peer_error","matched":true,"offset":65536}'
        )
        receiver = (
            '{"event":"file_write_error","matched":true,"offset":0}'
        )
        self.assertEqual(
            run_file_peer_error_interop.validate_events(
                scenario, sender, receiver, 1_000_000
            ),
            (65_536, 0),
        )
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            run_file_peer_error_interop.validate_events(
                scenario,
                sender,
                '{"event":"file_write_error",'
                '"matched":false,"offset":0}',
                1_000_000,
            )

    def test_ready_parser_is_strict(self) -> None:
        self.assertEqual(
            run_file_peer_error_interop.parse_ready_port(
                'noise\n{"event":"ready","port":12345}\n'
            ),
            12_345,
        )
        self.assertIsNone(
            run_file_peer_error_interop.parse_ready_port(
                '{"event":"complete","port":12345}'
            )
        )
        self.assertIsNone(
            run_file_peer_error_interop.parse_ready_port(
                '{"event":"ready","port":70000}'
            )
        )


if __name__ == "__main__":
    unittest.main()
