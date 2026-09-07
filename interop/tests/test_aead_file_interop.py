from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_aead_file_interop  # noqa: E402
import run_file_interop  # noqa: E402


class AeadFileInteropUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.robotweax = Path("/robotweax")
        self.reference = Path("/haivision")
        self.options = run_file_interop.RunOptions(
            byte_count=553_339,
            timeout_seconds=45,
            maximum_bandwidth_bytes_per_second=1_000_000,
            shutdown_grace_milliseconds=250,
        )

    def test_caller_listener_matrix_covers_stream_helpers_and_loss(self) -> None:
        scenarios = run_aead_file_interop.caller_listener_matrix(
            self.robotweax,
            self.options.byte_count,
            128,
            32,
            1_440,
        )

        self.assertEqual(len(scenarios), 6)
        self.assertEqual(
            {(item.caller, item.listener) for item in scenarios},
            {(self.robotweax, self.robotweax)},
        )
        self.assertEqual(sum(item.file_api for item in scenarios), 2)
        self.assertEqual(sum(item.fault is not None for item in scenarios), 2)
        helpers = [item for item in scenarios if item.file_api]
        streams = [item for item in scenarios if not item.file_api]
        self.assertTrue(
            all(
                item.file_size_to_eof and not item.expect_eof
                for item in helpers
            )
        )
        self.assertTrue(all(item.expect_eof for item in streams))
        self.assertTrue(all(item.crypto_mode == "gcm" for item in scenarios))
        self.assertTrue(
            all(item.expected_crypto_mode == "gcm" for item in scenarios)
        )
        self.assertTrue(
            all(
                item.maximum_payload_size
                == run_aead_file_interop.GCM_FILE_IPV4_PAYLOAD_SIZE
                for item in scenarios
            )
        )

    def test_file_peer_command_selects_and_verifies_gcm(self) -> None:
        scenario = run_aead_file_interop.caller_listener_matrix(
            self.robotweax,
            self.options.byte_count,
            128,
            32,
            1_440,
        )[0]
        command = run_file_interop.peer_command(
            scenario.caller,
            "caller",
            9_999,
            Path("/payload"),
            scenario,
            self.options,
        )

        self.assertEqual(command[command.index("--transport") + 1], "file")
        self.assertEqual(command[command.index("--congestion") + 1], "file")
        self.assertEqual(command[command.index("--crypto-mode") + 1], "gcm")
        self.assertEqual(
            command[command.index("--expect-crypto-mode") + 1], "gcm"
        )
        self.assertEqual(command[command.index("--payload-size") + 1], "1440")
        self.assertEqual(
            command[command.index("--expect-payload-size") + 1], "1440"
        )

    def test_rendezvous_matrix_covers_both_start_orders(
        self,
    ) -> None:
        scenarios = run_aead_file_interop.rendezvous_matrix(
            self.robotweax,
            self.options.byte_count,
            128,
            32,
            1_440,
        )

        self.assertEqual(len(scenarios), 2)
        self.assertEqual(
            {(item.sender, item.receiver) for item in scenarios},
            {(self.robotweax, self.robotweax)},
        )
        self.assertEqual(
            {item.start_order for item in scenarios},
            {"sender-first", "receiver-first"},
        )
        self.assertTrue(all(item.expect_eof for item in scenarios))
        self.assertTrue(all(item.crypto_mode == "gcm" for item in scenarios))
        self.assertTrue(all(item.fault is not None for item in scenarios))
        self.assertTrue(all(item.recovery == "NAK" for item in scenarios))

    def test_ipv6_matrix_uses_the_family_aware_payload_ceiling(self) -> None:
        scenarios = run_aead_file_interop.caller_listener_matrix(
            self.robotweax,
            self.options.byte_count,
            128,
            32,
            run_aead_file_interop.GCM_FILE_IPV6_PAYLOAD_SIZE,
            "ipv6-",
        )
        options = run_file_interop.RunOptions(
            byte_count=self.options.byte_count,
            timeout_seconds=45,
            maximum_bandwidth_bytes_per_second=1_000_000,
            shutdown_grace_milliseconds=250,
            host="::1",
        )
        command = run_file_interop.peer_command(
            scenarios[0].caller,
            "caller",
            9_999,
            Path("/payload"),
            scenarios[0],
            options,
        )

        self.assertTrue(
            all(item.name.startswith("ipv6-") for item in scenarios)
        )
        self.assertEqual(command[command.index("--host") + 1], "::1")
        self.assertEqual(command[command.index("--payload-size") + 1], "1420")

    def test_pinned_reference_boundary_requires_tsbpd(self) -> None:
        work = Path("/work")
        environment = {
            run_file_interop.PASSPHRASE_ENVIRONMENT: "secret"
        }
        completed = mock.Mock(
            returncode=3,
            stderr=(
                "Enable TSBPD to use AES GCM.\n"
                "setsockflag 62 failed: Operation not supported\n"
            ),
        )
        with (
            mock.patch.object(Path, "write_bytes", return_value=1),
            mock.patch.object(
                run_aead_file_interop.subprocess,
                "run",
                return_value=completed,
            ) as run,
        ):
            run_aead_file_interop.verify_reference_file_gcm_boundary(
                self.reference,
                "127.0.0.1",
                work,
                environment,
                1_440,
                128,
                32,
            )

        command = run.call_args.args[0]
        self.assertEqual(command[0], str(self.reference))
        self.assertEqual(command[1], "caller")
        self.assertEqual(command[command.index("--transport") + 1], "file")
        self.assertEqual(command[command.index("--crypto-mode") + 1], "gcm")
        self.assertEqual(run.call_args.kwargs["timeout"], 5)

    def test_pinned_reference_boundary_fails_closed_if_behavior_changes(
        self,
    ) -> None:
        environment = {
            run_file_interop.PASSPHRASE_ENVIRONMENT: "secret"
        }
        for completed in (
            mock.Mock(returncode=0, stderr=""),
            mock.Mock(returncode=3, stderr="unrelated failure"),
        ):
            with (
                self.subTest(completed=completed),
                mock.patch.object(Path, "write_bytes", return_value=1),
                mock.patch.object(
                    run_aead_file_interop.subprocess,
                    "run",
                    return_value=completed,
                ),
                self.assertRaises(RuntimeError),
            ):
                run_aead_file_interop.verify_reference_file_gcm_boundary(
                    self.reference,
                    "127.0.0.1",
                    Path("/work"),
                    environment,
                    1_440,
                    128,
                    32,
                )


if __name__ == "__main__":
    unittest.main()
