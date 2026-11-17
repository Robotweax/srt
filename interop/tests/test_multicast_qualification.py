from __future__ import annotations
import importlib.util
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
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

    def test_key_lifecycle_regressions_are_required(self):
        self.assertEqual(set(qualification.validate_native(
            self.transcript(qualification.KEY_REQUIRED), qualification.KEY_REQUIRED)),
            qualification.KEY_REQUIRED)
        with self.assertRaises(ValueError):
            qualification.validate_native(self.transcript([]), qualification.KEY_REQUIRED)

    def test_rejects_silent_subset(self):
        with self.assertRaises(ValueError):
            qualification.validate_native(self.transcript(sorted(qualification.REQUIRED)[:-1]))

    def test_rejects_skip_even_if_process_succeeded(self):
        count = len(qualification.REQUIRED)
        for transcript in [self.transcript()+"[SKIP] unsupported\n",
                           self.transcript().replace(f"{count}/{count} tests passed", f"{count}/{count} tests passed (1 skipped)")]:
            with self.subTest(transcript=transcript), self.assertRaises(ValueError):
                qualification.validate_native(transcript)

    def test_tolerates_only_recorded_environment_skip_of_non_required_case(self):
        names = sorted(qualification.REQUIRED)
        skipped, reason = next(iter(qualification.ENVIRONMENT_SKIPS.items()))
        self.assertFalse(set(qualification.ENVIRONMENT_SKIPS) & qualification.REQUIRED)
        transcript = (self.transcript(names) .replace(f"{len(names)}/{len(names)} tests passed",
                      f"[RUN] {skipped}\n[SKIP] {skipped}: {reason}\n"
                      f"{len(names)}/{len(names) + 1} tests passed (1 skipped)"))
        self.assertEqual(set(qualification.validate_native(
            transcript, tolerated=qualification.ENVIRONMENT_SKIPS)), qualification.REQUIRED)
        self.assertEqual(qualification.environment_skips(
            transcript, qualification.ENVIRONMENT_SKIPS), {skipped: reason})
        with self.assertRaises(ValueError):
            qualification.validate_native(transcript)
        for broken in [transcript.replace(reason, "different reason"),
                       transcript.replace("(1 skipped)", ""),
                       transcript.replace(f"[RUN] {skipped}\n", ""),
                       transcript.replace(f"[SKIP] {skipped}: {reason}", f"[SKIP] {names[0]}: {reason}")]:
            with self.subTest(transcript=broken), self.assertRaises(ValueError):
                qualification.validate_native(broken, tolerated=qualification.ENVIRONMENT_SKIPS)
        with self.assertRaises(ValueError):
            qualification.validate_native(
                transcript.replace(f"[SKIP] {skipped}: {reason}", f"[SKIP] {names[0]}: {reason}")
                .replace(f"[RUN] {skipped}\n", ""),
                tolerated={**qualification.ENVIRONMENT_SKIPS, names[0]: reason})

    def test_rejects_failure_or_invalid_summary(self):
        count = len(qualification.REQUIRED)
        for transcript in [self.transcript()+"[FAIL] failed\n", self.transcript()+"20/20 tests passed\n",
                           self.transcript().replace(f"{count}/{count}", f"{count-1}/{count}"), "0/0 tests passed\n"]:
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

    @unittest.skipUnless(os.name == "posix", "configure script runs in POSIX bash")
    def test_actual_configure_script_handles_each_backend(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        step = workflow.split("      - name: Configure multicast qualification\n", 1)[1].split("      - name:", 1)[0]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fake = root / "cmake"
            fake.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$CAPTURE"\n')
            fake.chmod(0o755)
            for backend in ["openssl", "mbedtls", "bcrypt"]:
                with self.subTest(backend=backend):
                    capture = root / "args.txt"
                    result = subprocess.run(["/bin/bash", "-c", script.replace("${{ matrix.backend }}", backend)],
                                            cwd=root, env={**os.environ, "PATH": str(root)+os.pathsep+os.environ["PATH"], "CAPTURE": str(capture)},
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    options = capture.read_text().splitlines()
                    self.assertIn("-DBUILD_SHARED_LIBS=OFF", options)
                    self.assertIn("-DROBOTWEAX_SRT_CRYPTO_BACKEND="+backend, options)
                    self.assertNotIn("", options)

    def test_linux_native_jobs_prepare_multicast_interface(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        for name in ["linux_release", "portable_release", "debug_matrix",
                     "aead_platform", "sanitizers", "thread_sanitizer", "mbedtls_backend"]:
            with self.subTest(job=name):
                job = workflow.split("\n  "+name+":\n", 1)[1]
                job = re.split(r"\n  [a-z_]+:\n", job, maxsplit=1)[0]
                self.assertIn("uses: ./.github/actions/multicast-interface", job)
                self.assertIn("if: runner.os == 'Linux'", job)

    def test_selected_example_builds_include_multicast_sample(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        target_lists = [line for line in workflow.splitlines()
                        if "robotweax_srt_contribution robotweax_srt_recovery robotweax_srt_device" in line]
        self.assertEqual(len(target_lists), 3)
        for targets in target_lists:
            self.assertIn("robotweax_srt_multicast", targets)

    def test_epoll_only_container_does_not_require_multicast_privileges(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        job = workflow.split("\n  gcc_release:\n", 1)[1].split("\n  live_timing:\n", 1)[0]
        self.assertIn("--include epoll", job)
        self.assertNotIn("uses: ./.github/actions/multicast-interface", job)


if __name__ == "__main__":
    unittest.main()
