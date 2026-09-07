from __future__ import annotations

import argparse
import contextlib
import io
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_aead_timing_interop as aead_timing  # noqa: E402


def scorecard_for(
    scenario: aead_timing.TimingScenario,
) -> dict[str, object]:
    security: dict[str, object] = {
        "encrypted": True,
        "crypto_mode": "gcm",
        "key_length": aead_timing.KEY_LENGTH,
        "key_refresh_rate_packets": scenario.key_refresh_rate,
        "key_preannouncement_packets": scenario.key_preannouncement,
        "data_key_transitions": aead_timing.MINIMUM_KEY_TRANSITIONS,
        "acknowledged_key_updates": aead_timing.MINIMUM_KEY_TRANSITIONS,
    }
    if scenario.uses_group_sender:
        security["member_paths"] = {
            "primary": {
                "crypto_mode": "gcm",
                "data_key_transitions": 2,
                "receiver_socket_id": 41,
            },
            "backup": {
                "crypto_mode": "gcm",
                "data_key_transitions": 1,
                "receiver_socket_id": 42,
            },
        }
    pcr = scenario.profile == "ts-1316"
    return {
        "sample_count": scenario.messages,
        "payload_sizes": [1_316 if scenario.profile == "ts-1316" else 1_200],
        "payload_integrity_compared": scenario.messages,
        "payload_integrity_uncompared": 0,
        "payload_integrity_failures": 0,
        "early_srt_release_count": 0,
        "early_udp_egress_count": 0,
        "tsbpd_deadline_regression_count": 0,
        "srt_release_regression_count": 0,
        "maximum_burst_depth": 2,
        "egress_deadline_error_microseconds": {"p99_9": 2_500.0},
        "mapped_tsbpd_latency_microseconds": {
            "minimum": 120_000.0,
            "maximum": 120_800.0,
        },
        "pcr_span_count": 3 if pcr else 0,
        "pcr_absolute_span_rate_error_ppm": ({"maximum": 120.0} if pcr else {}),
        "measurement": {
            "profile": scenario.profile,
            "fault_profile": scenario.fault_profile,
            "srt_address_family": ("ipv6" if scenario.host == "::1" else "ipv4"),
            "udp_address_family": ("ipv6" if scenario.host == "::1" else "ipv4"),
            "connection": {"mode": scenario.connection_mode},
            "security": security,
        },
    }


class AeadTimingInteropTests(unittest.TestCase):
    def test_failure_details_preserve_metrics_and_limits(self) -> None:
        scenario = aead_timing.timing_scenarios()[0]
        scorecard = scorecard_for(scenario)
        scorecard["tsbpd_deadline_regression_count"] = 1
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "scorecard.json"
            output.write_text(json.dumps(scorecard), encoding="utf-8")
            details = aead_timing.failure_details(
                scenario, output, RuntimeError("gate failed")
            )
        self.assertIn(f"FAIL {scenario.name}: gate failed", details)
        self.assertIn('"tsbpd_deadline_regression_count": 1', details)
        self.assertIn('"tsbpd_deadline_regression_count": 0', details)
        self.assertIn('"maximum_egress_p99_9_microseconds": 20000', details)
        self.assertIn('"maximum_burst_depth": 8', details)

    def test_unavailable_scorecard_does_not_hide_original_failure(self) -> None:
        scenario = aead_timing.timing_scenarios()[0]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "scorecard.json"
            for content in (None, "not json", "[]"):
                with self.subTest(content=content):
                    if content is not None:
                        output.write_text(content, encoding="utf-8")
                    details = aead_timing.failure_details(
                        scenario, output, RuntimeError("original failure")
                    )
                    self.assertIn("original failure", details)
                    self.assertIn("Scorecard unavailable:", details)
                    self.assertIn(scenario.name, details)

    def test_main_reports_parent_and_child_failures_and_continues(self) -> None:
        scenarios = aead_timing.timing_scenarios()[:2]
        arguments = argparse.Namespace(
            timing_peer=Path("timing-peer"),
            sender_peer=Path("sender-peer"),
            group_sender_peer=Path("group-sender-peer"),
            failure_artifacts=None,
        )
        for child_status in (0, 1):
            with self.subTest(child_status=child_status):
                calls = []

                def run(command, **kwargs):
                    scenario = scenarios[len(calls)]
                    calls.append(command)
                    scorecard = scorecard_for(scenario)
                    if len(calls) == 1:
                        scorecard["tsbpd_deadline_regression_count"] = 1
                    output = Path(command[command.index("--output") + 1])
                    output.write_text(json.dumps(scorecard), encoding="utf-8")
                    return subprocess.CompletedProcess(
                        command,
                        child_status if len(calls) == 1 else 0,
                        stdout="child stdout",
                        stderr="child stderr",
                    )

                stdout, stderr = io.StringIO(), io.StringIO()
                with (
                    mock.patch.object(
                        aead_timing, "parse_arguments", return_value=arguments
                    ),
                    mock.patch.object(
                        aead_timing,
                        "resolve_program_path",
                        side_effect=lambda p: p,
                    ),
                    mock.patch.object(
                        aead_timing, "timing_scenarios", return_value=scenarios
                    ),
                    mock.patch.object(aead_timing.subprocess, "run", side_effect=run),
                    contextlib.redirect_stdout(stdout),
                    contextlib.redirect_stderr(stderr),
                ):
                    self.assertEqual(aead_timing.main(), 1)
                self.assertEqual(len(calls), 2)
                self.assertIn(f"FAIL {scenarios[0].name}:", stderr.getvalue())
                self.assertIn('"tsbpd_deadline_regression_count": 1', stderr.getvalue())
                self.assertIn(f"PASS {scenarios[1].name}", stdout.getvalue())
                if child_status:
                    self.assertIn("child exit_code=1", stderr.getvalue())
                self.assertNotIn("child stdout", stderr.getvalue())
                self.assertNotIn("child stderr", stderr.getvalue())

    def test_diagnostics_report_all_fields_and_exact_boundaries(self) -> None:
        scenario = aead_timing.timing_scenarios()[0]
        card = scorecard_for(scenario)
        card["maximum_burst_depth"] = aead_timing.MAXIMUM_BURST_DEPTH
        card["egress_deadline_error_microseconds"]["p99_9"] = 20_000
        aead_timing.validate_scorecard(scenario, card)
        card["maximum_burst_depth"] = 9
        card["egress_deadline_error_microseconds"]["p99_9"] = 20_001
        card["payload_integrity_failures"] = 1
        with self.assertRaises(RuntimeError) as raised:
            aead_timing.validate_scorecard(scenario, card)
        message = str(raised.exception)
        self.assertIn(scenario.name, message)
        for expected in (
            "maximum_burst_depth: actual=9; expected <= 8",
            "egress_deadline_error_microseconds.p99_9: actual=20001; expected <= 20000",
            "payload_integrity_failures: actual=1; expected == 0",
        ):
            self.assertIn(expected, message)

    def test_missing_boolean_and_nonfinite_metrics_fail_closed(self) -> None:
        scenario = aead_timing.timing_scenarios()[0]
        for value in (None, True, "secret-value", float("nan"), float("inf")):
            with self.subTest(value=value):
                card = scorecard_for(scenario)
                card["egress_deadline_error_microseconds"]["p99_9"] = value
                with self.assertRaisesRegex(RuntimeError, "expected finite number"):
                    aead_timing.validate_scorecard(scenario, card)
        card = scorecard_for(scenario)
        del card["measurement"]
        with self.assertRaisesRegex(RuntimeError, "<missing>"):
            aead_timing.validate_scorecard(scenario, card)

    def test_pcr_phase_and_member_diagnostics(self) -> None:
        scenario = next(
            s
            for s in aead_timing.timing_scenarios()
            if s.group_unacknowledged_replay and s.profile == "ts-1316"
        )
        card = scorecard_for(scenario)
        card["pcr_absolute_span_rate_error_ppm"]["maximum"] = 5_001
        card["mapped_tsbpd_latency_microseconds"]["maximum"] = 122_001
        paths = card["measurement"]["security"]["member_paths"]
        paths["backup"]["receiver_socket_id"] = 41
        paths["backup"]["data_key_transitions"] = 0
        failures = "\n".join(aead_timing.scorecard_violations(scenario, card))
        self.assertIn("actual=5001; expected <= 5000.0", failures)
        self.assertIn("actual=2001.0; expected <= 2000", failures)
        self.assertIn("backup.data_key_transitions: actual=0; expected >= 1", failures)
        self.assertIn("two distinct integer identities", failures)

    def test_each_expected_profile_and_integrity_field_is_checked(self) -> None:
        scenario = aead_timing.timing_scenarios()[0]
        for field in aead_timing.expected_evidence(scenario):
            with self.subTest(field=field):
                card = scorecard_for(scenario)
                target = card
                keys = field.split(".")
                for key in keys[:-1]:
                    target = target[key]
                del target[keys[-1]]
                with self.assertRaises(RuntimeError) as raised:
                    aead_timing.validate_scorecard(scenario, card)
                self.assertIn(f"{field}: actual=", str(raised.exception))

    def test_failure_artifacts_survive_cleanup_without_raw_data(self) -> None:
        scenarios = aead_timing.timing_scenarios()[:2]
        for failure in (
            "parent",
            "child",
            "timeout",
            "missing",
            "invalid-json",
            "os-error",
            "retention-error",
            "none",
        ):
            with self.subTest(
                failure=failure
            ), tempfile.TemporaryDirectory() as directory:
                root = Path(directory) / "artifacts"
                if failure == "retention-error":
                    root.write_text("not a directory", encoding="utf-8")
                args = argparse.Namespace(
                    timing_peer=Path("timing"),
                    sender_peer=Path("sender"),
                    group_sender_peer=Path("group"),
                    failure_artifacts=root,
                )
                outputs = []

                def run(command, **kwargs):
                    scenario = scenarios[len(outputs)]
                    output = Path(command[command.index("--output") + 1])
                    outputs.append(output)
                    card = scorecard_for(scenario)
                    bad = len(outputs) == 1 and failure != "none"
                    if bad:
                        card["maximum_burst_depth"] = 9
                        card["passphrase"] = "TOP-SECRET"
                        card["payload"] = "RAW-PAYLOAD"
                        card["measurement"]["security"]["key"] = "PRIVATE-KEY"
                        card["measurement"]["profile"] = "HIDDEN-STRING"
                    if not bad or failure not in ("missing", "os-error"):
                        output.write_text(
                            "RAW-INVALID-JSON"
                            if bad and failure == "invalid-json"
                            else json.dumps(card),
                            encoding="utf-8",
                        )
                    if bad and failure == "timeout":
                        raise subprocess.TimeoutExpired(
                            ["SECRET-COMMAND"],
                            30,
                            output=b"SECRET-STDOUT",
                            stderr=b"SECRET-STDERR",
                        )
                    if bad and failure == "os-error":
                        raise OSError("SECRET-PATH")
                    return subprocess.CompletedProcess(
                        command,
                        7 if bad and failure == "child" else 0,
                        stdout="SECRET-STDOUT",
                        stderr="SECRET-STDERR",
                    )

                stdout, stderr = io.StringIO(), io.StringIO()
                with (
                    mock.patch.object(
                        aead_timing, "parse_arguments", return_value=args
                    ),
                    mock.patch.object(
                        aead_timing, "resolve_program_path", side_effect=lambda p: p
                    ),
                    mock.patch.object(
                        aead_timing, "timing_scenarios", return_value=scenarios
                    ),
                    mock.patch.object(aead_timing.subprocess, "run", side_effect=run),
                    contextlib.redirect_stdout(stdout),
                    contextlib.redirect_stderr(stderr),
                ):
                    self.assertEqual(aead_timing.main(), 0 if failure == "none" else 1)
                self.assertEqual(len(outputs), 2)
                self.assertTrue(all(not path.exists() for path in outputs))
                self.assertIn(f"PASS {scenarios[1].name}", stdout.getvalue())
                if failure == "none":
                    self.assertFalse(root.exists())
                    continue
                if failure == "retention-error":
                    self.assertIn("artifact retention failed", stderr.getvalue())
                    self.assertIn(
                        "maximum_burst_depth: actual=9; expected <= 8",
                        stderr.getvalue(),
                    )
                    continue
                logs = list(root.glob("run-*/*.log"))
                cards = list(root.glob("run-*/*.json"))
                self.assertEqual(len(logs), 1)
                self.assertEqual(
                    len(cards),
                    0 if failure in ("missing", "invalid-json", "os-error") else 1,
                )
                retained = stderr.getvalue() + "".join(
                    p.read_text() for p in (*logs, *cards)
                )
                for secret in (
                    "TOP-SECRET",
                    "RAW-PAYLOAD",
                    "PRIVATE-KEY",
                    "HIDDEN-STRING",
                    "SECRET-COMMAND",
                    "SECRET-STDOUT",
                    "SECRET-STDERR",
                    "SECRET-PATH",
                    "RAW-INVALID-JSON",
                ):
                    self.assertNotIn(secret, retained)
                if cards:
                    projected = json.loads(cards[0].read_text())
                    self.assertEqual(projected["maximum_burst_depth"], 9)
                    self.assertIn(
                        "maximum_burst_depth: actual=9; expected <= 8", retained
                    )
                if failure == "child":
                    self.assertIn("child exit_code=7", retained)
                if failure == "timeout":
                    self.assertIn("child timeout_seconds=30", retained)

    def test_matrix_covers_payload_families_connections_and_group_faults(
        self,
    ) -> None:
        scenarios = aead_timing.timing_scenarios()

        self.assertEqual(len(scenarios), 14)
        self.assertEqual(
            {
                (scenario.host, scenario.connection_mode, scenario.profile)
                for scenario in scenarios
                if not scenario.uses_group_sender
            },
            {
                (host, mode, profile)
                for host in ("127.0.0.1", "::1")
                for mode in ("caller-listener", "rendezvous")
                for profile in ("binary-1200", "ts-1316")
            },
        )
        group_scenarios = [
            scenario for scenario in scenarios if scenario.uses_group_sender
        ]
        self.assertEqual(len(group_scenarios), 6)
        self.assertEqual(
            sum(scenario.group_unacknowledged_replay for scenario in scenarios),
            2,
        )
        self.assertEqual(sum(scenario.group_path_outage for scenario in scenarios), 2)

    def test_commands_keep_gcm_limits_and_profile_specific_pcr(self) -> None:
        scenario = next(
            scenario
            for scenario in aead_timing.timing_scenarios()
            if scenario.name == "ipv4-backup-group-replay-ts-1316"
        )
        command = aead_timing.scenario_command(
            scenario,
            Path("timing-peer"),
            Path("sender-peer"),
            Path("group-sender-peer"),
            Path("scorecard.json"),
        )

        self.assertEqual(
            command[command.index("--sender-peer") + 1],
            "group-sender-peer",
        )
        self.assertEqual(command[command.index("--crypto-mode") + 1], "gcm")
        self.assertEqual(command[command.index("--pbkeylen") + 1], "16")
        self.assertEqual(
            command[command.index("--maximum-egress-p99-9-us") + 1],
            "20000",
        )
        self.assertIn("--maximum-pcr-span-rate-error-ppm", command)
        self.assertIn("--maximum-tsbpd-phase-range-us", command)
        self.assertIn("--group-unacknowledged-replay", command)

    def test_scorecard_validation_is_fail_closed(self) -> None:
        for scenario in aead_timing.timing_scenarios():
            with self.subTest(scenario=scenario.name):
                aead_timing.validate_scorecard(scenario, scorecard_for(scenario))

        scenario = aead_timing.timing_scenarios()[0]
        scorecard = scorecard_for(scenario)
        scorecard["early_srt_release_count"] = 1
        with self.assertRaisesRegex(RuntimeError, "release"):
            aead_timing.validate_scorecard(scenario, scorecard)
        scorecard = scorecard_for(scenario)
        measurement = scorecard["measurement"]
        assert isinstance(measurement, dict)
        security = measurement["security"]
        assert isinstance(security, dict)
        security["crypto_mode"] = "ctr"
        with self.assertRaisesRegex(RuntimeError, "profile evidence"):
            aead_timing.validate_scorecard(scenario, scorecard)

        replay = next(
            candidate
            for candidate in aead_timing.timing_scenarios()
            if candidate.group_unacknowledged_replay
        )
        scorecard = scorecard_for(replay)
        scorecard["mapped_tsbpd_latency_microseconds"] = {
            "minimum": 120_000.0,
            "maximum": 122_001.0,
        }
        with self.assertRaisesRegex(RuntimeError, "TSBPD phase"):
            aead_timing.validate_scorecard(replay, scorecard)


if __name__ == "__main__":
    unittest.main()
