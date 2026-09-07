from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest.mock import patch


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_aead_group_interop as group_interop  # noqa: E402


class AeadGroupInteropTests(unittest.TestCase):
    def test_matrix_is_gcm_only_and_covers_both_group_policies(self) -> None:
        peer = Path(__file__).resolve()
        with (
            patch.object(
                sys,
                "argv",
                [
                    "run_aead_group_interop.py",
                    "--robotweax-peer",
                    str(peer),
                ],
            ),
            patch.object(group_interop, "run_case") as run_case,
        ):
            self.assertEqual(group_interop.main(), 0)

        self.assertEqual(run_case.call_count, 3)
        policies = [call.args[3] for call in run_case.call_args_list]
        profiles = [call.kwargs["profile"] for call in run_case.call_args_list]
        self.assertEqual(policies, ["broadcast", "broadcast", "backup"])
        self.assertEqual(profiles, ["baseline", "late-join", "baseline"])
        for call in run_case.call_args_list:
            environment = call.kwargs["environment"]
            self.assertEqual(
                environment[group_interop.CRYPTO_MODE_ENVIRONMENT], "gcm"
            )
            self.assertEqual(
                len(environment[group_interop.PASSPHRASE_ENVIRONMENT]), 48
            )


if __name__ == "__main__":
    unittest.main()
