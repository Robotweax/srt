"""Exercise real CLI parsing and request generation without starting peers."""
from __future__ import annotations

import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import run_pacer_continuation_ab as runner
import throughput_diagnostics as diag

FOUR_GIB = 4 * 1024**3


class DiagnosticCLIProbe:
    """Keep parser, manifest reads, plan, reports and peer argv construction real.

    Synthetic host metadata avoids changing sysctls. At the sole workload
    launch boundary, retain each request and return a failing exit status.
    No simulated result can therefore be mistaken for a successful transfer.
    """

    def __init__(self, root):
        self.root = root
        peer = root / "not-an-executable-peer"
        peer.write_bytes(b"CLI test fixture; never execute")
        self.manifest = {
            "complete": True,
            "programs": {
                name: diag.sc.program_identity(peer)
                for name in ("robotweax", "haivision")
            },
            "libraries": {},
        }
        self.path = root / "build-manifest.json"
        self.path.write_text(json.dumps(self.manifest))
        self.requests = []
        self.commands = []

    def stop_before_launch(self, command, directory, timeout):
        assert command[-2] == "--case-file"
        request = json.loads(Path(command[-1]).read_text())
        assert Path(command[-1]) == directory / "request.json"
        self.requests.append(request)
        options = diag.sc.RunOptions(**request["options"])
        for role in ("caller", "listener"):
            self.commands.append(
                diag.sc.many_socket_peer_command(
                    Path("never-executed"), role, 12345, options
                )
            )
        return 73

    @contextlib.contextmanager
    def isolated(self):
        environment = {
            "sysctls": {"net/core/rmem_max": 33554432, "net/core/wmem_max": 33554432}
        }
        with mock.patch.object(
            diag, "host_metadata", return_value=environment
        ), mock.patch.object(
            diag.platform, "system", return_value="Linux"
        ), mock.patch.object(
            diag.time, "sleep"
        ), mock.patch.object(
            diag, "run_bounded", side_effect=self.stop_before_launch
        ), contextlib.redirect_stdout(
            io.StringIO()
        ):
            yield


class ThroughputDiagnosticCLITests(unittest.TestCase):
    @unittest.skipUnless(
        sys.platform in ("linux", "darwin"), "supported diagnostic host"
    )
    def test_script_entry_accepts_four_gib_before_deliberate_manifest_stop(self):
        # Exercise a fresh interpreter as the lab's preflight does. An explicitly
        # incomplete manifest stops before host inspection, output or workload.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            manifest = root / "incomplete.json"
            manifest.write_text('{"complete": false}')
            output = root / "results"
            argv = [
                sys.executable,
                "-B",
                diag.__file__,
                "--build-manifest",
                str(manifest),
                "--output-directory",
                str(output),
            ]
            stage = {
                "connections": 1,
                "bytes_per_connection": FOUR_GIB,
                "repetitions": 2,
                "warmups": 0,
            }
            for key, value in runner.budget.arguments(stage).items():
                argv += ["--" + key.replace("_", "-"), str(value)]
            for profile in runner.PROFILES:
                argv += ["--profile", profile]
            result = subprocess.run(argv, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertIn("build manifest is incomplete", result.stderr)
            self.assertNotIn("argument --bytes-per-connection:", result.stderr)
            self.assertFalse(output.exists())

    def test_real_cli_accepts_bounds_and_preserves_default(self):
        for volume in (None, 1316, 1024**3, FOUR_GIB):
            with self.subTest(volume=volume), tempfile.TemporaryDirectory() as tmp:
                probe = DiagnosticCLIProbe(Path(tmp))
                output = Path(tmp) / "results"
                argv = [
                    "--build-manifest",
                    str(probe.path),
                    "--output-directory",
                    str(output),
                    "--profile",
                    "robotweax-self",
                    "--repetitions",
                    "1",
                    "--warmups",
                    "0",
                    "--cooldown-seconds",
                    "0",
                ]
                if volume is not None:
                    argv += ["--bytes-per-connection", str(volume)]
                with probe.isolated():
                    self.assertEqual(diag.main(argv), 1)
                expected = 128 * 1024**2 if volume is None else volume
                self.assertEqual(len(probe.requests), 3)
                self.assertTrue(
                    all(
                        r["options"]["bytes_per_connection"] == expected
                        for r in probe.requests
                    )
                )
                report = json.loads((output / "report.json").read_text())
                self.assertEqual(report["arguments"]["bytes_per_connection"], expected)
                self.assertFalse(report["all_transfers_pass"])
                self.assertTrue(all(r["exit_code"] == 73 for r in report["runs"]))

    def test_real_cli_rejects_outside_bounds_before_manifest_or_output(self):
        for volume in ("1315", str(FOUR_GIB + 1), "invalid"):
            with self.subTest(volume=volume), tempfile.TemporaryDirectory() as tmp:
                output = Path(tmp) / "results"
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr), mock.patch.object(
                    diag, "run_bounded"
                ) as launch, mock.patch.object(Path, "read_text") as read:
                    with self.assertRaises(SystemExit) as error:
                        diag.main(
                            [
                                "--build-manifest",
                                str(Path(tmp) / "absent.json"),
                                "--output-directory",
                                str(output),
                                "--bytes-per-connection",
                                volume,
                            ]
                        )
                self.assertEqual(error.exception.code, 2)
                self.assertIn("argument --bytes-per-connection", stderr.getvalue())
                read.assert_not_called()
                launch.assert_not_called()
                self.assertFalse(output.exists())

    def check_real_abba_requests(self):
        """Also reused by the delivery tests after applying its fixed contract."""
        with tempfile.TemporaryDirectory() as tmp:
            probe = DiagnosticCLIProbe(Path(tmp))
            output = Path(tmp) / "results"
            report = {}
            with probe.isolated():
                code = runner.run_blocks(
                    dict.fromkeys(runner.REVISIONS, probe.path),
                    dict.fromkeys(runner.REVISIONS, probe.manifest),
                    output,
                    report,
                )
            self.assertEqual(code, 1)
            self.assertEqual([b["variant"] for b in report["blocks"]], runner.plan())
            self.assertEqual([b["exit_code"] for b in report["blocks"]], [1] * 4)
            self.assertFalse(report["analysis"]["comparison_valid"])
            self.assertEqual(report["analysis"]["followup_candidates"], [])
            self.assertEqual(len(probe.requests), 24)
            expected_cases = (
                diag.case_plan(list(runner.PROFILES), 2, 0, True, "none") * 4
            )
            self.assertEqual(
                [{k: r[k] for k in ("profile", "kind")} for r in probe.requests],
                expected_cases,
            )
            for request in probe.requests:
                self.assertEqual(request["options"]["bytes_per_connection"], FOUR_GIB)
                self.assertEqual(request["options"]["connections"], 1)
                self.assertEqual(request["options"]["timeout_seconds"], 45)
                self.assertEqual(request["capture"], "none")
            self.assertEqual(len(probe.commands), 48)
            for command in probe.commands:
                self.assertEqual(command[command.index("--messages") + 1], "3263653")
                self.assertEqual(command[command.index("--message-size") + 1], "1316")
            self.assertEqual(24 * 3263653 * 1316, 103079216352)
            for block in report["blocks"]:
                raw = json.loads((output / block["name"] / "report.json").read_text())
                self.assertFalse(raw["all_transfers_pass"])
                self.assertEqual([r["exit_code"] for r in raw["runs"]], [73] * 6)

    def test_four_gib_abba_reaches_all_24_real_cli_requests(self):
        with mock.patch.object(
            runner,
            "STAGE",
            {
                "connections": 1,
                "bytes_per_connection": FOUR_GIB,
                "repetitions": 2,
                "warmups": 0,
            },
        ):
            self.check_real_abba_requests()


if __name__ == "__main__":
    unittest.main()
