from __future__ import annotations

import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]


def preparation_script() -> str:
    workflow = (ROOT / ".github/workflows/ci.yml").read_text()
    preparation = workflow.split("  reference_interop_prepare:\n", 1)[1]
    step = preparation.split("      - name: Build Robotweax SRT\n", 1)[1]
    step = step.split("      - name:", 1)[0]
    return textwrap.dedent(step.split("        run: |\n", 1)[1])


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"),
                     "reference preparation runs in POSIX bash")
class CiBuildScopeTests(unittest.TestCase):
    def run_preparation(self, handshake: bool, benchmark: bool,
                        configure_fails: bool = False) -> tuple[int, list]:
        with tempfile.TemporaryDirectory(prefix="srt-build-scope-") as directory:
            root = Path(directory)
            log = root / "calls.jsonl"
            cmake = root / "cmake"
            cmake.write_text(
                f"#!{sys.executable}\n"
                "import json, os, sys\n"
                "with open(os.environ['CALL_LOG'], 'a') as output:\n"
                "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                "if '-S' in sys.argv and os.environ['CONFIGURE_FAILS'] == '1':\n"
                "    sys.exit(1)\n"
            )
            cmake.chmod(0o755)
            result = subprocess.run(
                ["bash", "-c", preparation_script()], cwd=root,
                capture_output=True, text=True, timeout=10,
                env={
                    **os.environ,
                    "PATH": str(root) + os.pathsep + os.environ["PATH"],
                    "CALL_LOG": str(log),
                    "HANDSHAKE": str(handshake).lower(),
                    "BENCHMARK": str(benchmark).lower(),
                    "CONFIGURE_FAILS": "1" if configure_fails else "0",
                },
            )
            calls = [json.loads(line) for line in log.read_text().splitlines()]
            return result.returncode, calls

    def test_only_needed_peers_are_built_for_each_selection(self) -> None:
        for handshake in (False, True):
            for benchmark in (False, True):
                with self.subTest(handshake=handshake, benchmark=benchmark):
                    status, calls = self.run_preparation(handshake, benchmark)
                    self.assertEqual(status, 0)
                    self.assertEqual(len(calls), 2)
                    self.assertIn("-DROBOTWEAX_SRT_BUILD_TESTS=OFF", calls[0])
                    self.assertIn("-DROBOTWEAX_SRT_BUILD_TOOLS=ON", calls[0])
                    self.assertIn(
                        "-DROBOTWEAX_SRT_BUILD_BENCHMARKS=" + str(benchmark).lower(),
                        calls[0],
                    )
                    expected = ["robotweax_srt_interop_peer"]
                    if handshake:
                        expected += ["robotweax_srt_handshake_probe",
                                     "robotweax_srt_group_peer"]
                    if benchmark:
                        expected += ["robotweax_srt_scalability_peer"]
                    self.assertEqual(calls[1],
                                     ["--build", "build", "--parallel", "--target"]
                                     + expected)

    def test_failed_configuration_does_not_build_stale_targets(self) -> None:
        status, calls = self.run_preparation(True, True, configure_fails=True)
        self.assertNotEqual(status, 0)
        self.assertEqual(len(calls), 1)


class CiTestRegistrationTests(unittest.TestCase):
    def test_unfiltered_suite_is_registered_without_duplicate_compat_run(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn("add_test(NAME robotweax_srt_tests COMMAND robotweax_srt_tests)",
                      cmake)
        self.assertNotIn("NAME robotweax_srt_compat_tests", cmake)
        self.assertNotIn("COMMAND robotweax_srt_tests compat_", cmake)
        # Process/lifetime probes are independent coverage, not duplicates.
        for name in ("robotweax_srt_lifecycle_tests",
                     "robotweax_srt_process_exit_tests"):
            self.assertIn("NAME " + name, cmake)


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"),
                     "selected consumer steps use bash on each runner")
class CiConsumerScopeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = (ROOT / ".github/workflows/ci.yml").read_text()

    def job(self, name: str) -> str:
        block = self.workflow.split("\n  " + name + ":\n", 1)[1]
        return re.split(r"\n  [a-z_]+:\n", block, maxsplit=1)[0]

    def step(self, job: str, name: str) -> str:
        return self.job(job).split("      - name: " + name + "\n", 1)[1].split(
            "      - name:", 1)[0]

    def execute(self, step: str, examples: bool, package: bool) -> tuple[int, list]:
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        with tempfile.TemporaryDirectory(prefix="srt-consumer-scope-") as directory:
            root = Path(directory)
            log = root / "calls.jsonl"
            for name in ("cmake", "ctest"):
                program = root / name
                program.write_text(
                    f"#!{sys.executable}\n"
                    "import json, os, sys\n"
                    "with open(os.environ['CALL_LOG'], 'a') as output:\n"
                    "    output.write(json.dumps(sys.argv[1:]) + '\\n')\n"
                )
                program.chmod(0o755)
            result = subprocess.run(
                ["bash", "-c", script], cwd=root, text=True,
                capture_output=True, timeout=10,
                env={**os.environ, "CALL_LOG": str(log),
                     "PATH": str(root) + os.pathsep + os.environ["PATH"],
                     "EXAMPLES": str(examples).lower(),
                     "PACKAGE": str(package).lower()},
            )
            return result.returncode, (
                [json.loads(line) for line in log.read_text().splitlines()]
                if log.exists() else []
            )

    def test_demo_package_and_mixed_steps_select_exact_targets_and_labels(self) -> None:
        for job in ("linux_release", "portable_release"):
            for examples, package, labels in ((True, False, "example"),
                                              (False, True, "abi|package"),
                                              (True, True, "example|abi|package")):
                with self.subTest(job=job, examples=examples, package=package):
                    build = self.step(job, "Build selected public consumers")
                    status, calls = self.execute(build, examples, package)
                    self.assertEqual(status, 0)
                    targets = ["robotweax_srt"]
                    if examples:
                        targets += ["robotweax_srt_message_demo", "robotweax_srt_file_demo",
                                    "robotweax_srt_group_demo", "robotweax_srt_udp_bridge"]
                    self.assertEqual(calls, [["--build", "build", "--config", "Release",
                                             "--parallel", "--target"] + targets])
                    test = self.step(job, "Test selected public consumers")
                    status, calls = self.execute(test, examples, package)
                    self.assertEqual(status, 0)
                    self.assertEqual(calls, [["--test-dir", "build", "-C", "Release",
                                             "-L", labels, "--parallel", "1",
                                             "--no-tests=error", "--output-on-failure"]])

    def test_empty_selection_fails_instead_of_running_zero_tests(self) -> None:
        for job in ("linux_release", "portable_release"):
            status, calls = self.execute(
                self.step(job, "Test selected public consumers"), False, False)
            self.assertNotEqual(status, 0)
            self.assertEqual(calls, [])

    def test_full_release_and_aead_conditions_remain_explicit(self) -> None:
        for job in ("linux_release", "portable_release"):
            for step in ("Build", "Test"):
                self.assertIn("if: needs.changes.outputs.core_tests == 'true'",
                              self.step(job, step))
            self.assertIn("-DROBOTWEAX_SRT_BUILD_TOOLS=${{ needs.changes.outputs.core_tests }}",
                          self.step(job, "Configure"))
        for step in ("Test static ABI and package consumers", "Configure shared AES-GCM preview",
                     "Build shared AES-GCM preview", "Test shared ABI, exports, and package consumers"):
            self.assertIn("if: needs.changes.outputs.aead_platform == 'true'",
                          self.step("aead_platform", step))
        for step in ("Build AES-GCM public demos", "Test explicit CTR and GCM demo modes"):
            self.assertIn("if: needs.changes.outputs.examples == 'true'",
                          self.step("aead_platform", step))


if __name__ == "__main__":
    unittest.main()
