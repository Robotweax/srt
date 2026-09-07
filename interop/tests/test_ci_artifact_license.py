from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
NOTICES = ("LICENSE", "THIRD_PARTY.md")
PROGRAMS = (
    "build/robotweax_srt_interop_peer",
    "build-aead/robotweax_srt_interop_peer",
    "build-aead/robotweax_srt_group_peer",
    "build-aead/robotweax_srt_timing_peer",
    "build-aead/robotweax_srt_group_timing_sender",
    "build/robotweax_srt_handshake_probe",
    "build/robotweax_srt_group_peer",
    "build/robotweax_srt_scalability_peer",
    "robotweax-binding-peer",
)


@unittest.skipUnless(shutil.which("bash") and shutil.which("tar"),
                     "the Ubuntu packaging step requires bash and tar")
class CiArtifactLicenseTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="srt-artifact-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.output = self.root / "output"
        self.output.mkdir()
        self.archive = self.output / "robotweax-interop-tools.tar.gz"
        for notice in NOTICES:
            shutil.copyfile(ROOT / notice, self.root / notice)
        self.program(PROGRAMS[0])

    def program(self, path: str) -> None:
        destination = self.root / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        # Synthetic bytes exercise packaging, not executable behavior.
        destination.write_bytes(b"synthetic packaging fixture\n")
        destination.chmod(0o755)

    def package(self) -> subprocess.CompletedProcess[str]:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        step = workflow.split("      - name: Package Robotweax interoperability tools\n", 1)[1]
        step = step.split("      - name: Upload Robotweax interoperability tools\n", 1)[0]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        return subprocess.run(
            ["bash", "-c", script], cwd=self.root, text=True,
            capture_output=True, timeout=10,
            env={**os.environ, "RUNNER_TEMP": str(self.output)},
        )

    def test_minimal_and_full_archives_include_exact_license_bytes(self) -> None:
        for programs in (PROGRAMS[:1], PROGRAMS):
            with self.subTest(programs=len(programs)):
                for program in programs:
                    self.program(program)
                # These files must never leak into the program bundle.
                for excluded in ("reference-build/libsrt.so", "reference-api-peer",
                                 "reference-aead-api-peer", "build/unrelated"):
                    self.program(excluded)
                result = self.package()
                self.assertEqual(result.returncode, 0, result.stderr)
                with tarfile.open(self.archive, "r:gz") as archive:
                    self.assertEqual(set(archive.getnames()), set(NOTICES + programs))
                    self.assertTrue(all(member.isfile() for member in archive.getmembers()))
                    for notice in NOTICES:
                        self.assertEqual(archive.extractfile(notice).read(),
                                         (ROOT / notice).read_bytes())
                    for program in programs:
                        self.assertTrue(archive.getmember(program).mode & 0o111)

    def test_missing_or_empty_notice_fails_before_archive_creation(self) -> None:
        for notice in NOTICES:
            for missing in (True, False):
                with self.subTest(notice=notice, missing=missing):
                    path = self.root / notice
                    if missing:
                        path.unlink()
                    else:
                        path.write_bytes(b"")
                    result = self.package()
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("Refusing missing, empty, or symlink artifact entry", result.stderr)
                    self.assertFalse(self.archive.exists())
                    shutil.copyfile(ROOT / notice, path)

    @unittest.skipIf(os.name == "nt", "symlink fixture requires POSIX")
    def test_symlink_notice_or_program_is_rejected(self) -> None:
        for entry in ("LICENSE", PROGRAMS[0]):
            with self.subTest(entry=entry):
                path = self.root / entry
                original = path.read_bytes()
                path.unlink()
                path.symlink_to(ROOT / "LICENSE")
                result = self.package()
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(self.archive.exists())
                path.unlink()
                path.write_bytes(original)


if __name__ == "__main__":
    unittest.main()
