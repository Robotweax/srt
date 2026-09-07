from __future__ import annotations

from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/sanitizers"))
import verify_ubsan  # noqa: E402


def result(code: int, stderr: str = "") -> subprocess.CompletedProcess[str]:
    return subprocess.CompletedProcess([], code, stdout="", stderr=stderr)


DIAGNOSTIC = (
    "/tmp/ubsan_canary.cpp:11:35: runtime error: signed integer overflow: "
    "2147483647 + 1 cannot be represented in type 'int'\n"
)


class UbsanEnforcementTests(unittest.TestCase):
    def test_accepts_fatal_signed_overflow(self) -> None:
        self.assertIsNone(verify_ubsan.validation_error(
            result(0), result(1, DIAGNOSTIC),
        ))

    def test_rejects_recoverable_diagnostic(self) -> None:
        self.assertIsNotNone(verify_ubsan.validation_error(
            result(0), result(0, DIAGNOSTIC),
        ))

    def test_rejects_uninstrumented_success(self) -> None:
        self.assertIsNotNone(verify_ubsan.validation_error(result(0), result(0)))

    def test_rejects_unrelated_failure_or_missing_runtime(self) -> None:
        for diagnostic in (
            "", "AddressSanitizer:DEADLYSIGNAL", "dyld: Library not loaded",
            DIAGNOSTIC.replace("ubsan_canary.cpp", "unrelated.cpp"),
        ):
            with self.subTest(diagnostic=diagnostic):
                self.assertIsNotNone(verify_ubsan.validation_error(
                    result(0), result(1, diagnostic),
                ))

    def test_rejects_broken_healthy_control(self) -> None:
        for control in (result(1), result(0, DIAGNOSTIC)):
            self.assertIsNotNone(verify_ubsan.validation_error(
                control, result(1, DIAGNOSTIC),
            ))

    def test_ci_uses_fatal_flags_and_checks_canary_before_tests(self) -> None:
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        sanitizer = workflow.split("\n  sanitizers:\n", 1)[1].split(
            "\n  thread_sanitizer:\n", 1,
        )[0]
        environment, steps = sanitizer.split("    steps:\n", 1)
        for variable in ("CFLAGS", "CXXFLAGS"):
            self.assertIn(
                f"{variable}: -fsanitize=address,undefined "
                "-fno-sanitize-recover=undefined -fno-omit-frame-pointer",
                environment,
            )
        self.assertIn("UBSAN_OPTIONS: halt_on_error=1", environment)
        self.assertNotIn("        env:\n", steps)
        self.assertIn(
            "cmake --build build --target robotweax_srt_ubsan_canary", steps,
        )
        self.assertLess(
            steps.index("python3 tests/sanitizers/verify_ubsan.py"),
            steps.index("ctest --test-dir build"),
        )
        self.assertNotIn("continue-on-error", sanitizer)
        fuzz = workflow.split("\n  fuzz_smoke:\n", 1)[1].split(
            "    steps:\n", 1,
        )[0]
        self.assertIn("UBSAN_OPTIONS: halt_on_error=1", fuzz)

    def test_canary_is_excluded_from_default_build_and_ctest(self) -> None:
        cmake = (ROOT / "CMakeLists.txt").read_text()
        self.assertIn(
            "add_executable(robotweax_srt_ubsan_canary EXCLUDE_FROM_ALL\n"
            "        tests/sanitizers/ubsan_canary.cpp)", cmake,
        )
        # Only the explicitly named build target may reference this probe.
        self.assertEqual(cmake.count("robotweax_srt_ubsan_canary"), 1)


if __name__ == "__main__":
    unittest.main()
