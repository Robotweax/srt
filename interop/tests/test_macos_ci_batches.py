import importlib.util
import json
from pathlib import Path
import re
import subprocess
import shutil
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("macos_batches", ROOT / "tools/run_macos_ci_tests.py")
batches = importlib.util.module_from_spec(spec)
spec.loader.exec_module(batches)


def probe(name):
    return {"name": name, "command": ["/build/robotweax_srt_crypto_setup_failure_tests", "0", "3", "0"]}


class MacosCiBatchesTests(unittest.TestCase):
    def test_each_test_runs_once_and_only_isolated_probes_overlap(self):
        tests = [{"name": "native_timing"}, {"name": "package_consumer"},
                 probe("robotweax_srt_crypto_setup_0_0_3"),
                 probe("robotweax_srt_crypto_setup_optional_fallback"),
                 {"name": "robotweax_srt_crypto_setup_other", "command": ["/build/other"]}]
        ownership = {test["name"]: [] for test in tests}
        for jobs, selection in batches.plan(tests):
            for test in tests:
                included = bool(re.search(selection[1], test["name"]))
                if included == (selection[0] == "-R"):
                    ownership[test["name"]].append(jobs)
        for name, jobs in ownership.items():
            self.assertEqual(len(jobs), 1, name)
        self.assertEqual(ownership["native_timing"], [1])
        self.assertEqual(ownership["package_consumer"], [1])
        self.assertEqual(ownership["robotweax_srt_crypto_setup_other"], [1])
        self.assertEqual(ownership["robotweax_srt_crypto_setup_0_0_3"], [2])

    def test_no_probes_retains_serial_full_suite(self):
        self.assertEqual(batches.plan([{"name": "native"}]), [(1, [])])

    def test_only_probes_does_not_schedule_empty_serial_batch(self):
        planned = batches.plan([probe("robotweax_srt_crypto_setup_0_0_3")])
        self.assertEqual(len(planned), 1)
        self.assertEqual(planned[0][0], 2)

    def test_empty_or_duplicate_inventory_fails(self):
        for tests in ([], [{"name": ""}], [{"name": "a"}, {"name": "a"}]):
            with self.assertRaises(ValueError):
                batches.plan(tests)

    def test_failure_in_either_batch_is_not_hidden_and_both_run(self):
        inventory = json.dumps({"tests": [{"name": "native"}, probe("robotweax_srt_crypto_setup_0_0_3")]})
        for statuses in ((0, 0), (1, 0), (0, 1), (-9, 0)):
            with self.subTest(statuses=statuses):
                results = [subprocess.CompletedProcess([], 0, stdout=inventory)]
                results += [subprocess.CompletedProcess([], value) for value in statuses]
                with patch.object(batches.subprocess, "run", side_effect=results) as calls:
                    self.assertEqual(batches.run(Path("build"), "Debug"), int(any(statuses)))
                    self.assertEqual(calls.call_count, 3)
                    for invocation in calls.call_args_list[1:]:
                        argv = invocation.args[0]
                        self.assertIn("--no-tests=error", argv)
                        self.assertNotIn("--timeout", argv)
                        self.assertEqual(argv[argv.index("-C") + 1], "Debug")

    def test_regex_escapes_literal_names(self):
        planned = batches.plan([probe("robotweax_srt_crypto_setup_a.b")])
        self.assertIsNone(re.search(planned[0][1][1], "robotweax_srt_crypto_setup_aXb"))

    def test_failed_discovery_does_not_run_partial_coverage(self):
        with patch.object(batches.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "ctest")) as calls:
            with self.assertRaises(subprocess.CalledProcessError):
                batches.run(Path("build"), "Release")
            self.assertEqual(calls.call_count, 1)

    def test_full_macos_jobs_use_batches_and_keep_other_platform_tests(self):
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        for job in ("portable_release", "debug_matrix"):
            block = workflow.split("  " + job + ":\n", 1)[1]
            block = re.split(r"\n  [a-z_]+:\n", block, maxsplit=1)[0]
            step = block.split("      - name: Test macOS bounded batches\n", 1)[1]
            self.assertIn("runner.os == 'macOS'", step.split("run:", 1)[0])
            self.assertIn("tools/run_macos_ci_tests.py", step)
            original = block.split("      - name: Test\n", 1)[1].split("      - name:", 1)[0]
            self.assertIn("runner.os != 'macOS'", original)
            self.assertIn("ctest --test-dir build", original)
            self.assertIn("--parallel ${{ runner.os == 'macOS' && '3' || '' }}", block)

    @unittest.skipUnless(shutil.which("ctest"), "requires the CTest regex engine")
    def test_ctest_engine_assigns_every_actual_registration_once(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            names = ["native", "robotweax_srt_crypto_setup_0_0_3", "robotweax_srt_crypto_setup_optional_fallback"]
            fixture = build / "robotweax_srt_crypto_setup_failure_tests"
            (build / "CTestTestfile.cmake").write_text("\n".join(
                f'add_test({name} "{fixture.as_posix()}")' for name in names))
            common = ["ctest", "--test-dir", str(build), "--show-only=json-v1"]
            tests = json.loads(subprocess.check_output(common, text=True))["tests"]
            selected = []
            for _, selection in batches.plan(tests):
                inventory = json.loads(subprocess.check_output(common + selection, text=True))
                selected.extend(test["name"] for test in inventory["tests"])
            self.assertCountEqual(selected, names)


if __name__ == "__main__":
    unittest.main()
