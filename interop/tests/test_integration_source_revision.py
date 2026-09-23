import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("source_revision", ROOT / "tests/check_source_revision.py")
revision = importlib.util.module_from_spec(spec)
spec.loader.exec_module(revision)


class IntegrationSourceRevisionTests(unittest.TestCase):
    def test_accepts_only_the_selected_git_commit(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory)
            subprocess.run(["git", "init", "-q", str(source)], check=True)
            subprocess.run(["git", "-C", str(source), "-c", "user.name=Fixture",
                            "-c", "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false",
                            "commit", "-q", "--allow-empty", "-m", "fixture"], check=True)
            commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
            revision.verify(source, commit)
            with self.assertRaisesRegex(RuntimeError, "unqualified source revision"):
                revision.verify(source, "0" * 40)
            with self.assertRaises(subprocess.CalledProcessError):
                revision.verify(source / "missing", commit)
