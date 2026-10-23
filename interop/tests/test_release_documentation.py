"""Release docs must match the installed product before costly qualification."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("check_documentation", ROOT / "tests/package_consumer/check_documentation.py")
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)

class ReleaseDocumentationTests(unittest.TestCase):
    def test_finished_current_docs_allow_historical_candidate_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "docs").mkdir()
            (root / "README.md").write_text("| Project release | `0.3.1` |\n")
            notes = root / "docs/release-notes-0.3.1.md"
            notes.write_text("# Robotweax SRT 0.3.1 — Release\n")
            (root / "docs/release-notes-0.3.0.md").write_text("Historical release candidate result\n")
            self.assertEqual(checker.validate_release(root, "0.3.1"), [])
            notes.write_text("# Robotweax SRT 0.3.1 — Release candidate\n")
            self.assertTrue(checker.validate_release(root, "0.3.1"))
            notes.write_text("# Robotweax SRT 0.3.0 — Release\n")
            self.assertTrue(checker.validate_release(root, "0.3.1"))
            notes.unlink()
            self.assertTrue(checker.validate_release(root, "0.3.1"))

    def test_current_source_documentation_is_finished_and_version_matched(self):
        self.assertEqual(checker.validate_release(ROOT, "0.3.1"), [])
