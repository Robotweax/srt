import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_aead_fec_interop  # noqa: E402
import run_fec_interop  # noqa: E402


class AeadFecInteropTests(unittest.TestCase):
    def test_matrix_respects_the_pinned_receiver_boundary(self) -> None:
        robotweax = Path("/tmp/robotweax-peer")
        reference = Path("/tmp/reference-peer")

        scenarios = run_aead_fec_interop.aead_fec_scenarios(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 4)
        self.assertEqual(
            [scenario.geometry for scenario in scenarios],
            ["gcm-row", "gcm-row", "gcm-column", "gcm-matrix"],
        )
        self.assertEqual(scenarios[0].listener, reference)
        self.assertEqual(scenarios[0].fault_occurrences, (1,))
        self.assertTrue(
            all(scenario.listener == robotweax for scenario in scenarios[1:])
        )
        self.assertTrue(
            all(scenario.crypto_mode == "gcm" for scenario in scenarios)
        )
        self.assertTrue(
            all(scenario.key_length == 16 for scenario in scenarios)
        )
        self.assertTrue(
            all(
                scenario.minimum_key_transitions == 2
                for scenario in scenarios
            )
        )
    def test_peer_command_selects_and_verifies_gcm(self) -> None:
        robotweax = Path("/tmp/robotweax-peer")
        reference = Path("/tmp/reference-peer")
        scenario = run_aead_fec_interop.aead_fec_scenarios(
            robotweax, reference
        )[0]

        command = run_fec_interop.peer_command(
            scenario,
            robotweax,
            "caller",
            14_500,
            Path("/tmp/input.bin"),
            run_fec_interop.RunOptions(),
        )

        self.assertEqual(command[command.index("--crypto-mode") + 1], "gcm")
        self.assertEqual(
            command[command.index("--expect-crypto-mode") + 1], "gcm"
        )
        self.assertEqual(
            command[command.index("--packet-filter") + 1],
            run_fec_interop.ROW_PACKET_FILTER,
        )


if __name__ == "__main__":
    unittest.main()
