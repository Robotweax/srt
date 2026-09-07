from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "examples"))
import test_crypto_modes as harness  # noqa: E402


def result(code: int, diagnostic: str) -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess([], code, stdout="", stderr=diagnostic)


class DemoCryptoModeTests(unittest.TestCase):
    def test_ci_exercises_aead_demos_for_example_changes(self) -> None:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        job = workflow.split("\n  aead_platform:\n", 1)[1].split(
            "\n  aead_", 1)[0]
        selection = job.split("    strategy:", 1)[0]
        self.assertIn("needs.changes.outputs.examples == 'true'", selection)
        self.assertIn("-DROBOTWEAX_SRT_BUILD_EXAMPLES=${{ needs.changes.outputs.examples }}", job)
        self.assertIn("--target robotweax_srt_message_demo robotweax_srt_file_demo robotweax_srt_group_demo", job)
        self.assertIn("-L example", job)

    def test_individual_connection_rejection(self) -> None:
        for kind in ("message", "file"):
            self.assertTrue(harness.connection_rejected(kind, result(
                1, "error: srt_connect failed: Connection rejected")))

    def test_group_requires_both_crypto_rejections(self) -> None:
        diagnostic = (
            "ENDPOINT_REJECT token=1001 reason=17 description=Conflicting cryptographic modes\n"
            "ENDPOINT_REJECT token=1002 reason=17 description=Conflicting cryptographic modes\n"
            "error: srt_connect_group failed: Connection setup failure\n"
        )
        for kind in ("group-broadcast", "group-backup"):
            self.assertTrue(harness.connection_rejected(kind, result(1, diagnostic)))
            for invalid in (diagnostic.replace("token=1002", "token=1001"),
                            diagnostic.replace("reason=17", "reason=16"),
                            "error: srt_connect_group failed: Connection setup failure"):
                self.assertFalse(harness.connection_rejected(kind, result(1, invalid)))
            for code in (0, -9, 2):
                self.assertFalse(harness.connection_rejected(kind, result(code, diagnostic)))

    def test_unrelated_errors_are_not_rejections(self) -> None:
        for kind in ("message", "file", "group-broadcast", "group-backup"):
            for diagnostic in ("", "dyld: Library not loaded", "unknown option --crypto",
                               "error: srt_connect failed: Connection timed out",
                               "error: srt_recvmsg2 failed: Connection rejected"):
                with self.subTest(kind=kind, diagnostic=diagnostic):
                    self.assertFalse(harness.connection_rejected(kind, result(1, diagnostic)))


if __name__ == "__main__":
    unittest.main()
