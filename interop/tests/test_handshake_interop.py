from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_handshake_interop  # noqa: E402


class HandshakeInteropUnitTests(unittest.TestCase):
    def test_local_program_path_becomes_absolute(self) -> None:
        original_directory = Path.cwd()
        with tempfile.TemporaryDirectory() as directory:
            program = Path(directory) / "reference-peer"
            program.write_bytes(b"")
            try:
                os.chdir(directory)
                resolved = run_handshake_interop.resolve_program_path(
                    Path("./reference-peer")
                )
            finally:
                os.chdir(original_directory)

            self.assertTrue(resolved.is_absolute())
            self.assertEqual(resolved, program.resolve())


if __name__ == "__main__":
    unittest.main()
