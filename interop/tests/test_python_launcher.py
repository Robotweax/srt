#!/usr/bin/env python3

import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
PYTHON_LAUNCHER = REPOSITORY_ROOT / "tools" / "python"


@unittest.skipUnless(os.name == "posix", "the launcher is POSIX-only")
class PythonLauncherTests(unittest.TestCase):
    def test_launcher_selects_a_supported_interpreter(self) -> None:
        environment = os.environ.copy()
        environment.pop("ROBOTWEAX_SRT_PYTHON", None)
        result = subprocess.run(
            [
                str(PYTHON_LAUNCHER),
                "-c",
                "import sys; print(f'{sys.version_info.major}."
                "{sys.version_info.minor}')",
            ],
            cwd=REPOSITORY_ROOT,
            env=environment,
            check=True,
            capture_output=True,
            text=True,
        )
        version = tuple(int(part) for part in result.stdout.strip().split("."))
        self.assertGreaterEqual(version, (3, 10))

    def test_supported_explicit_interpreter_is_used(self) -> None:
        environment = os.environ.copy()
        environment["ROBOTWEAX_SRT_PYTHON"] = sys.executable
        result = subprocess.run(
            [str(PYTHON_LAUNCHER), "-c", "import sys; print(sys.executable)"],
            cwd=REPOSITORY_ROOT,
            env=environment,
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertEqual(
            Path(result.stdout.strip()).resolve(),
            Path(sys.executable).resolve(),
        )

    def test_unsupported_explicit_interpreter_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            unsupported = Path(directory) / "unsupported-python"
            unsupported.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
            unsupported.chmod(0o755)
            environment = os.environ.copy()
            environment["ROBOTWEAX_SRT_PYTHON"] = str(unsupported)
            result = subprocess.run(
                [str(PYTHON_LAUNCHER), "--version"],
                cwd=REPOSITORY_ROOT,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            )

        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("Python 3.10 or newer", result.stderr)
        self.assertIn(str(unsupported), result.stderr)


if __name__ == "__main__":
    unittest.main()
