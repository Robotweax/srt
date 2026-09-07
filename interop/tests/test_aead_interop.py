from __future__ import annotations

import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_aead_interop  # noqa: E402
import run_encrypted_interop  # noqa: E402
import run_rendezvous_interop  # noqa: E402


class AeadInteropUnitTests(unittest.TestCase):
    def test_smallest_profile_is_bidirectional_aes128_gcm(self) -> None:
        options = run_encrypted_interop.RunOptions(
            byte_count=1,
            timeout_seconds=30,
            key_refresh_rate=256,
            key_preannouncement=64,
            crypto_mode="gcm",
            expected_crypto_mode="gcm",
        )

        scenarios, baseline, packet_count = (
            run_aead_interop.aead_baselines(
                Path("/robotweax"), Path("/haivision"), options
            )
        )

        self.assertEqual(packet_count, 191)
        self.assertEqual(baseline.byte_count, 191 * 1_200)
        self.assertEqual([scenario.key_length for scenario in scenarios], [16, 16])
        self.assertEqual(
            [scenario.name for scenario in scenarios],
            [
                "gcm-aes128-robotweax-to-haivision-no-rotation",
                "gcm-aes128-haivision-to-robotweax-no-rotation",
            ],
        )

    def test_peer_command_requires_and_verifies_gcm(self) -> None:
        options = run_encrypted_interop.RunOptions(
            byte_count=229_200,
            timeout_seconds=30,
            key_refresh_rate=256,
            key_preannouncement=64,
            crypto_mode="gcm",
            expected_crypto_mode="gcm",
        )

        command = run_encrypted_interop.peer_command(
            Path("/peer"),
            "caller",
            9_999,
            Path("/payload"),
            options,
            16,
        )

        self.assertEqual(
            command[command.index("--crypto-mode") + 1], "gcm"
        )
        self.assertEqual(
            command[command.index("--expect-crypto-mode") + 1], "gcm"
        )

    def test_ipv6_profile_uses_the_family_aware_gcm_ceiling(self) -> None:
        options = run_encrypted_interop.RunOptions(
            byte_count=1,
            timeout_seconds=30,
            key_refresh_rate=256,
            key_preannouncement=64,
            chunk_size=1_420,
            crypto_mode="gcm",
            expected_crypto_mode="gcm",
            maximum_payload_size=1_420,
            expected_maximum_payload_size=1_420,
        )
        scenarios, baseline, packet_count = (
            run_aead_interop.aead_baselines(
                Path("/robotweax"),
                Path("/haivision"),
                options,
                "ipv6-",
            )
        )
        command = run_encrypted_interop.peer_command(
            scenarios[0].caller,
            "caller",
            9_999,
            Path("/payload"),
            baseline,
            16,
            host="::1",
        )

        self.assertEqual(packet_count, 191)
        self.assertEqual(baseline.byte_count, 191 * 1_420)
        self.assertTrue(
            all(
                scenario.name.startswith("ipv6-gcm-")
                for scenario in scenarios
            )
        )
        self.assertEqual(command[command.index("--host") + 1], "::1")
        self.assertEqual(
            command[command.index("--payload-size") + 1], "1420"
        )
        self.assertEqual(
            command[command.index("--expect-payload-size") + 1], "1420"
        )

    def test_rendezvous_profile_covers_start_orders_faults_and_rotation(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_aead_interop.aead_rendezvous_matrix(
            robotweax,
            reference,
            256,
        )

        self.assertEqual(len(scenarios), 6)
        self.assertEqual(
            {scenario.start_order for scenario in scenarios[:4]},
            {"sender-first", "receiver-first"},
        )
        self.assertEqual(
            {scenario.sender for scenario in scenarios[:4]},
            {robotweax, reference},
        )
        self.assertEqual(
            [scenario.faults[0].action for scenario in scenarios[4:]],
            ["drop", "duplicate"],
        )
        self.assertTrue(
            all(scenario.crypto_mode == "gcm" for scenario in scenarios)
        )
        self.assertTrue(
            all(
                scenario.expected_crypto_mode == "gcm"
                for scenario in scenarios
            )
        )
        self.assertTrue(
            all(
                scenario.minimum_data_key_transitions == 2
                for scenario in scenarios
            )
        )

    def test_rendezvous_peer_command_selects_and_verifies_gcm(self) -> None:
        options = run_rendezvous_interop.RunOptions(
            byte_count=921_600,
            timeout_seconds=30,
            key_refresh_rate=256,
            key_preannouncement=64,
            chunk_size=1_420,
            maximum_payload_size=1_420,
            expected_maximum_payload_size=1_420,
        )
        command = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/payload"),
            options,
            16,
            crypto_mode="gcm",
            expected_crypto_mode="gcm",
            host="::1",
        )

        self.assertEqual(
            command[command.index("--crypto-mode") + 1], "gcm"
        )
        self.assertEqual(
            command[command.index("--expect-crypto-mode") + 1], "gcm"
        )
        self.assertEqual(command[command.index("--host") + 1], "::1")
        self.assertEqual(
            command[command.index("--payload-size") + 1], "1420"
        )
        self.assertEqual(
            command[command.index("--expect-payload-size") + 1], "1420"
        )


if __name__ == "__main__":
    unittest.main()
