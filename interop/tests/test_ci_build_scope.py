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
    def test_group_receive_workaround_is_reference_only(self) -> None:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        macro = "-DROBOTWEAX_SRT_REFERENCE_GROUP_RECEIVE=1"
        self.assertEqual(workflow.count(macro), 1)
        step = workflow.split(
            "      - name: Build reference connection-group peer locally\n", 1
        )[1].split("      - name:", 1)[0]
        self.assertIn(macro, step)
        self.assertIn("reference-build/libsrt.a", step)
        self.assertNotIn("ROBOTWEAX_SRT_REFERENCE_GROUP_RECEIVE",
                         (ROOT / "CMakeLists.txt").read_text())

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
                    self.assertIn("-DROBOTWEAX_SRT_BUILD_TESTS=ON", calls[0])
                    self.assertIn("-DBUILD_SHARED_LIBS=OFF", calls[0])
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
                        expected += ["robotweax_srt_scalability_peer",
                                     "robotweax_srt_group_throughput_peer"]
                    self.assertEqual(calls[1],
                                     ["--build", "build", "--parallel", "--target"]
                                     + expected)

    def test_failed_configuration_does_not_build_stale_targets(self) -> None:
        status, calls = self.run_preparation(True, True, configure_fails=True)
        self.assertNotEqual(status, 0)
        self.assertEqual(len(calls), 1)


class CiTestRegistrationTests(unittest.TestCase):
    def test_native_partitions_do_not_duplicate_or_omit_cases(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text()
        registrations = re.findall(
            r"add_test\(NAME\s+(\w+)\s+COMMAND\s+robotweax_srt_tests\b([^)]*)\)",
            cmake,
        )
        commands = {name: arguments.split() for name, arguments in registrations}
        empty_selection_checks = {
            "robotweax_srt_rejects_empty_test_selection": ["robotweax_nonexistent_test_filter"],
            "robotweax_srt_rejects_empty_exclusion_selection": ["--exclude", '""'],
            "robotweax_srt_rejects_invalid_test_arguments": ["--unknown"],
            "robotweax_srt_rejects_incomplete_include_selection": ["--include"],
            "robotweax_srt_rejects_empty_include_exclusion_selection": [
                "--include", "compat_runtime_", "--exclude", "compat_runtime_"],
        }
        partitions = {
            "robotweax_srt_tests", "robotweax_srt_transport_runtime_tests",
            "robotweax_srt_rotation_tests",
            "robotweax_srt_delayed_key_response_tests",
            "robotweax_srt_key_length_tests", "robotweax_srt_gcm_profile_tests",
            "robotweax_srt_sensor_gcm_aes128_tests",
            "robotweax_srt_sensor_gcm_aes192_tests",
            "robotweax_srt_sensor_gcm_aes256_tests",
            "robotweax_srt_group_retention_prefix_tests",
            "robotweax_srt_group_retention_oversized_tests",
            "robotweax_srt_stale_drop_coverage_tests",
            "robotweax_srt_stale_nak_tests",
            "robotweax_srt_native_reuseport_tests",
        }
        self.assertEqual(
            len(registrations), len(partitions) + len(empty_selection_checks),
        )
        self.assertEqual(set(commands), {
            *partitions, *empty_selection_checks,
        })
        for name, arguments in empty_selection_checks.items():
            self.assertEqual(commands[name], arguments)
        properties = re.search(
            r"set_tests_properties\((robotweax_srt_rejects_empty_test_selection.*?)\)",
            cmake, re.DOTALL,
        )
        self.assertIsNotNone(properties)
        for name in empty_selection_checks:
            self.assertIn(name, properties.group(1))
        self.assertIn("PROPERTIES WILL_FAIL TRUE", properties.group(1))
        self.assertEqual(commands["robotweax_srt_tests"], [
            "--exclude", "compat_runtime_",
            "compat_group_closed_member_retains_bounded_copy",
            "key_length", "srt_compat_gcm_",
            "srt_compat_sensor_gcm_bounded_sample_stream_",
            "srt_compat_bind_acquire",
            "compat_runtime_keeps_live_data_flowing_while_a_key_response_is_late",
            "stale_drop_coverage", "stale_nak",
        ])
        self.assertEqual(commands["robotweax_srt_transport_runtime_tests"], [
            "--include", "compat_runtime_", "--exclude", "compat_runtime_rotation_",
            "key_length", "compat_runtime_keeps_live_data_flowing_while_a_key_response_is_late",
            "stale_drop_coverage", "stale_nak",
        ])
        bounded_properties = re.search(
            r"set_tests_properties\(\s*(robotweax_srt_tests.*?)\)",
            cmake, re.DOTALL,
        )
        self.assertIsNotNone(bounded_properties)
        profile_partitions = {
            "robotweax_srt_gcm_profile_tests",
            "robotweax_srt_sensor_gcm_aes128_tests",
            "robotweax_srt_sensor_gcm_aes192_tests",
            "robotweax_srt_sensor_gcm_aes256_tests",
        }
        stale_partitions = {
            "robotweax_srt_stale_drop_coverage_tests",
            "robotweax_srt_stale_nak_tests",
        }
        reuseport_partitions = {"robotweax_srt_native_reuseport_tests"}
        for name in partitions - profile_partitions - stale_partitions - reuseport_partitions:
            self.assertIn(name, bounded_properties.group(1))
        stale_properties = re.search(
            r"set_tests_properties\(\s*(robotweax_srt_stale_drop_coverage_tests.*?)\)",
            cmake, re.DOTALL,
        )
        self.assertIsNotNone(stale_properties)
        for name in stale_partitions:
            self.assertIn(name, stale_properties.group(1))
        self.assertIn("TIMEOUT 30", stale_properties.group(1))
        self.assertIn('LABELS "nak"', stale_properties.group(1))
        self.assertEqual(commands["robotweax_srt_native_reuseport_tests"],
                         ["--include", "srt_compat_bind_acquire"])
        for name in profile_partitions | reuseport_partitions:
            profile_properties = re.search(
                r"set_tests_properties\(" + name + r"\s+PROPERTIES\s+([^)]*)\)",
                cmake,
            )
            self.assertIsNotNone(profile_properties)
            self.assertIn("TIMEOUT 30", profile_properties.group(1))
            self.assertIn("integration", profile_properties.group(1))
            self.assertIn("network" if name in reuseport_partitions else "encryption",
                          profile_properties.group(1))
        self.assertIn("PROPERTIES TIMEOUT 30", bounded_properties.group(1))
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        native_labels = re.findall(r'LABELS\s+"([^"]+)"', stale_properties.group(1))
        thin_consumer_labels = re.findall(r'-L "([^"]*(?:abi|package)[^"]*)"', workflow)
        self.assertTrue(thin_consumer_labels)
        for labels in native_labels:
            for label in labels.split(";"):
                for selection in thin_consumer_labels:
                    self.assertIsNone(re.search(selection, label),
                                      f"unbuilt native partition selected by {selection}: {label}")
        optional_step = workflow.split("- name: Test optional retransmission limit", 1)[1]
        optional_step = optional_step.split("\n  sanitizers:", 1)[0]
        selection = re.search(r'-R "([^"]+)"', optional_step)
        self.assertIsNotNone(selection)
        self.assertIsNotNone(re.search(selection.group(1), "robotweax_srt_tests"))
        self.assertIsNotNone(re.search(selection.group(1), "robotweax_srt_transport_runtime_tests"))
        self.assertIsNotNone(re.search(selection.group(1), "robotweax_srt_key_length_tests"))
        for name in profile_partitions | stale_partitions | {"robotweax_srt_delayed_key_response_tests"}:
            self.assertIsNotNone(re.search(selection.group(1), name))
        target = re.search(
            r"add_executable\(robotweax_srt_tests\s+([^)]*)\)", cmake,
        )
        self.assertIsNotNone(target)
        cases = []
        for source in target.group(1).split():
            cases.extend(re.findall(
                r"\bTEST\s*\(\s*(\w+)\s*\)",
                (ROOT / source).read_text(),
            ))
        self.assertTrue(cases)
        coverage = {case: [] for case in cases}
        for name in partitions:
            arguments = commands[name]
            included_cases = cases
            if arguments[0] == "--include":
                self.assertGreaterEqual(len(arguments), 2)
                included_cases = [case for case in cases if arguments[1] in case]
                arguments = arguments[2:]
            if not arguments:
                selected = included_cases
            elif arguments[0] == "--exclude":
                self.assertGreater(len(arguments), 1)
                selected = [case for case in included_cases
                            if not any(value in case for value in arguments[1:])]
            else:
                self.assertEqual(len(arguments), 1)
                self.assertTrue(arguments[0])
                selected = [case for case in included_cases if arguments[0] in case]
            self.assertTrue(selected, f"empty native partition: {name}")
            for case in selected:
                coverage[case].append(name)
        # Check the actual source cases: changing filters must neither omit
        # coverage nor run a case in multiple partitions.
        for case, owners in coverage.items():
            self.assertEqual(len(owners), 1, f"{case}: {owners}")
        self.assertNotIn("NAME robotweax_srt_compat_tests", cmake)
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
