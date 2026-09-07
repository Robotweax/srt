from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]


class ReferenceProvenanceTests(unittest.TestCase):
    def setUp(self) -> None:
        self.target = json.loads((ROOT / "compat/srt-1.5.7-api.json").read_text())["compatibility_target"]
        self.workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        self.performance = (ROOT / "docs/performance.md").read_text()

    def test_bootstrap_primary_pin_matches_ci_and_manifest(self) -> None:
        bootstrap = (ROOT / "interop/bootstrap_ubuntu.sh").read_text()
        self.assertIn(f'REFERENCE_SRT_COMMIT: {self.target["commit"]}', self.workflow)
        self.assertIn(f'reference_commit="{self.target["commit"]}"', bootstrap)
        self.assertIn('rev-parse --verify HEAD', bootstrap)
        self.assertIn('diff --exit-code HEAD --', bootstrap)

    def test_provenance_lists_primary_and_separate_legacy_profiles(self) -> None:
        provenance = (ROOT / "THIRD_PARTY.md").read_text()
        for variable in ("REFERENCE_SRT_COMMIT", "REFERENCE_SRT_LEGACY_COMMIT",
                         "REFERENCE_SRT_SECURITY_COMMIT", "REFERENCE_SRT_AEAD_COMMIT"):
            pin = re.search(rf"^  {variable}: ([0-9a-f]{{40}})$", self.workflow, re.M).group(1)
            self.assertIn(pin, provenance)
        self.assertIn("SRT 1.5.7 compatibility baseline", provenance)
        self.assertIn("SRT 1.5.5 backward-compatibility profile", provenance)

    def test_benchmark_revision_comes_from_verified_checkout(self) -> None:
        blocks = re.findall(r"```sh\n(.*?)```", self.performance, re.S)
        comparison = next(block for block in blocks if "--reference-revision" in block)
        self.assertIn('--reference-revision "$reference_revision"', comparison)
        self.assertIn('reference_revision="$(git -C reference-srt rev-parse --verify HEAD)"', comparison)
        for block in blocks:
            if "--profile robotweax-self" in block:
                self.assertNotIn("reference-srt", block)

    @unittest.skipUnless(shutil.which("bash"), "shell recipe requires bash")
    def test_recipe_guards_reject_wrong_or_dirty_reference(self) -> None:
        blocks = re.findall(r"```sh\n(.*?)```", self.performance, re.S)
        guarded = [block for block in blocks if 'reference_revision=' in block]
        self.assertEqual(len(guarded), 2)
        # Execute the actual documentation guards, mocking only Git. Do not
        # build upstream code or run a benchmark as part of this unit test.
        fake_git = '''git() {
  if [ "$3" = "rev-parse" ]; then
    printf '%s\\n' "$TEST_HEAD"
  else
    return "$TEST_DIFF"
  fi
}
'''
        for block in guarded:
            prefix = block.split("test ! -e", 1)[0].split("tools/python", 1)[0]
            for head, dirty, expected in ((self.target["commit"], "0", 0),
                                          ("0" * 40, "0", 1),
                                          (self.target["commit"], "1", 1)):
                with self.subTest(head=head, dirty=dirty):
                    result = subprocess.run(
                        ["bash", "-c", fake_git + prefix], capture_output=True,
                        env={**os.environ, "TEST_HEAD": head, "TEST_DIFF": dirty},
                        timeout=5,
                    )
                    self.assertEqual(result.returncode, expected)


if __name__ == "__main__":
    unittest.main()
