from __future__ import annotations
import importlib.util
from pathlib import Path
import subprocess
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("qualify_multicast", ROOT / "tools/qualify_multicast.py")
qualification = importlib.util.module_from_spec(spec)
spec.loader.exec_module(qualification)


class MulticastQualificationTests(unittest.TestCase):
    def transcript(self, names=None):
        names = sorted(qualification.REQUIRED if names is None else names)
        return "".join(f"[RUN] {name}\n[PASS] {name}\n" for name in names) + f"{len(names)}/{len(names)} tests passed\n"

    def test_accepts_complete_execution(self):
        self.assertEqual(set(qualification.validate_native(self.transcript())), qualification.REQUIRED)

    def test_rejects_silent_subset(self):
        with self.assertRaises(ValueError):
            qualification.validate_native(self.transcript(sorted(qualification.REQUIRED)[:-1]))

    def test_rejects_skip_even_if_process_succeeded(self):
        for transcript in [self.transcript()+"[SKIP] unsupported\n",
                           self.transcript().replace("20/20 tests passed", "20/20 tests passed (1 skipped)")]:
            with self.subTest(transcript=transcript), self.assertRaises(ValueError):
                qualification.validate_native(transcript)

    def test_rejects_failure_or_invalid_summary(self):
        for transcript in [self.transcript()+"[FAIL] failed\n", self.transcript()+"20/20 tests passed\n",
                           self.transcript().replace("20/20", "19/20"), "0/0 tests passed\n"]:
            with self.subTest(transcript=transcript), self.assertRaises(ValueError):
                qualification.validate_native(transcript)

    def test_manual_selection_only_enables_multicast(self):
        result = subprocess.check_output([sys.executable, str(ROOT / "interop/ci_changes.py"),
                                          "--multicast-only"], text=True)
        enabled = {line.split("=", 1)[0] for line in result.splitlines() if line.endswith("=true")}
        self.assertEqual(enabled, {"multicast"})

    def test_workflow_preserves_backend_matrix_and_required_driver(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        job = workflow.split("\n  multicast_qualification:\n", 1)[1].split("\n  ci_gate:\n", 1)[0]
        self.assertEqual(job.count("backend: openssl"), 3)
        self.assertIn("backend: mbedtls", job)
        self.assertIn("backend: bcrypt", job)
        self.assertIn("python3 tools/qualify_multicast.py", job)
        self.assertIn("if: needs.changes.outputs.multicast == 'true'", job)


if __name__ == "__main__":
    unittest.main()
