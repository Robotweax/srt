#!/usr/bin/env python3
"""Robotweax AES-GCM release timing matrix for AEAD-12."""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

from interop_common import resolve_program_path


MAXIMUM_EGRESS_P99_9_MICROSECONDS = 20_000
MAXIMUM_BURST_DEPTH = 8
MAXIMUM_PCR_SPAN_RATE_ERROR_PPM = 5_000.0
MAXIMUM_REPLAY_TSBPD_PHASE_RANGE_MICROSECONDS = 2_000
BITRATE_BITS_PER_SECOND = 7_520_000
KEY_LENGTH = 16
MINIMUM_KEY_TRANSITIONS = 3


@dataclass(frozen=True)
class TimingScenario:
    name: str
    profile: str
    host: str
    connection_mode: str
    messages: int = 384
    latency_milliseconds: int = 300
    timeout_seconds: int = 15
    fault_profile: str = "loss-delay-reorder"
    key_refresh_rate: int = 64
    key_preannouncement: int = 20
    group_failover_after: int = 0
    group_unacknowledged_replay: bool = False
    group_path_outage: bool = False
    group_maximum_failover_delay_milliseconds: int = 0
    maximum_tsbpd_phase_range_microseconds: int | None = None

    @property
    def uses_group_sender(self) -> bool:
        return self.connection_mode == "backup-group"


def timing_scenarios() -> tuple[TimingScenario, ...]:
    single_socket = tuple(
        TimingScenario(
            name=f"{family}-{mode}-{profile}",
            profile=profile,
            host=host,
            connection_mode=mode,
        )
        for family, host in (("ipv4", "127.0.0.1"), ("ipv6", "::1"))
        for mode in ("caller-listener", "rendezvous")
        for profile in ("binary-1200", "ts-1316")
    )
    groups: list[TimingScenario] = []
    for profile in ("binary-1200", "ts-1316"):
        groups.extend(
            (
                TimingScenario(
                    name=f"ipv4-backup-group-baseline-{profile}",
                    profile=profile,
                    host="127.0.0.1",
                    connection_mode="backup-group",
                    latency_milliseconds=120,
                    fault_profile="none",
                    group_failover_after=192,
                ),
                TimingScenario(
                    name=f"ipv4-backup-group-replay-{profile}",
                    profile=profile,
                    host="127.0.0.1",
                    connection_mode="backup-group",
                    latency_milliseconds=500,
                    fault_profile="none",
                    key_refresh_rate=32,
                    key_preannouncement=10,
                    group_failover_after=96,
                    group_unacknowledged_replay=True,
                    maximum_tsbpd_phase_range_microseconds=(
                        MAXIMUM_REPLAY_TSBPD_PHASE_RANGE_MICROSECONDS
                    ),
                ),
                TimingScenario(
                    name=f"ipv4-backup-group-path-outage-{profile}",
                    profile=profile,
                    host="127.0.0.1",
                    connection_mode="backup-group",
                    messages=320,
                    latency_milliseconds=300,
                    fault_profile="none",
                    key_refresh_rate=32,
                    key_preannouncement=10,
                    group_failover_after=48,
                    group_path_outage=True,
                    group_maximum_failover_delay_milliseconds=1_500,
                ),
            )
        )
    return single_socket + tuple(groups)


def scenario_command(
    scenario: TimingScenario,
    timing_peer: Path,
    sender_peer: Path,
    group_sender_peer: Path,
    output: Path,
) -> list[str]:
    command = [
        sys.executable,
        str(Path(__file__).resolve().with_name("run_live_timing_interop.py")),
        "--timing-peer",
        str(timing_peer),
        "--sender-peer",
        str(group_sender_peer if scenario.uses_group_sender else sender_peer),
        "--connection-mode",
        scenario.connection_mode,
        "--profile",
        scenario.profile,
        "--messages",
        str(scenario.messages),
        "--bitrate-bps",
        str(BITRATE_BITS_PER_SECOND),
        "--latency-ms",
        str(scenario.latency_milliseconds),
        "--timeout-seconds",
        str(scenario.timeout_seconds),
        "--host",
        scenario.host,
        "--udp-host",
        scenario.host,
        "--pbkeylen",
        str(KEY_LENGTH),
        "--km-refresh-rate",
        str(scenario.key_refresh_rate),
        "--km-preannounce",
        str(scenario.key_preannouncement),
        "--minimum-key-transitions",
        str(MINIMUM_KEY_TRANSITIONS),
        "--crypto-mode",
        "gcm",
        "--maximum-egress-p99-9-us",
        str(MAXIMUM_EGRESS_P99_9_MICROSECONDS),
        "--maximum-burst-depth",
        str(MAXIMUM_BURST_DEPTH),
        "--output",
        str(output),
    ]
    if scenario.fault_profile != "none":
        command.extend(("--fault-profile", scenario.fault_profile))
    if scenario.profile == "ts-1316":
        command.extend(
            (
                "--maximum-pcr-span-rate-error-ppm",
                str(int(MAXIMUM_PCR_SPAN_RATE_ERROR_PPM)),
            )
        )
    if scenario.group_failover_after != 0:
        command.extend(
            (
                "--group-failover-after",
                str(scenario.group_failover_after),
            )
        )
    if scenario.group_unacknowledged_replay:
        command.append("--group-unacknowledged-replay")
    if scenario.group_path_outage:
        command.extend(
            (
                "--group-path-outage",
                "--group-maximum-failover-delay-ms",
                str(scenario.group_maximum_failover_delay_milliseconds),
            )
        )
    if scenario.maximum_tsbpd_phase_range_microseconds is not None:
        command.extend(
            (
                "--maximum-tsbpd-phase-range-us",
                str(scenario.maximum_tsbpd_phase_range_microseconds),
            )
        )
    return command


def expected_evidence(scenario: TimingScenario) -> dict[str, object]:
    family = "ipv6" if scenario.host == "::1" else "ipv4"
    return {
        "measurement.profile": scenario.profile,
        "measurement.fault_profile": scenario.fault_profile,
        "measurement.srt_address_family": family,
        "measurement.udp_address_family": family,
        "measurement.connection.mode": scenario.connection_mode,
        "measurement.security.encrypted": True,
        "measurement.security.crypto_mode": "gcm",
        "measurement.security.key_length": KEY_LENGTH,
        "measurement.security.key_refresh_rate_packets": scenario.key_refresh_rate,
        "measurement.security.key_preannouncement_packets": scenario.key_preannouncement,
        "sample_count": scenario.messages,
        "payload_sizes": [1_316 if scenario.profile == "ts-1316" else 1_200],
        "payload_integrity_compared": scenario.messages,
        "payload_integrity_uncompared": 0,
        "payload_integrity_failures": 0,
        "early_srt_release_count": 0,
        "early_udp_egress_count": 0,
        "tsbpd_deadline_regression_count": 0,
        "srt_release_regression_count": 0,
    }


def field_value(scorecard: dict[str, object], field: str) -> object:
    value: object = scorecard
    for key in field.split("."):
        if not isinstance(value, dict) or key not in value:
            return "<missing>"
        value = value[key]
    return value


def diagnostic_value(value: object) -> object:
    """Retain numeric measurements and known enums, never arbitrary strings."""
    if type(value) in (int, bool) or value is None:
        return value
    if type(value) is float:
        return value if math.isfinite(value) else "<non-finite>"
    if isinstance(value, str) and value in {
        "<missing>",
        "ipv4",
        "ipv6",
        "binary-1200",
        "ts-1316",
        "none",
        "loss-delay-reorder",
        "caller-listener",
        "rendezvous",
        "backup-group",
        "gcm",
        "ctr",
        "primary",
        "backup",
    }:
        return value
    if isinstance(value, list):
        return [diagnostic_value(item) for item in value[:16]]
    return "<redacted>"


def scorecard_violations(
    scenario: TimingScenario, scorecard: dict[str, object]
) -> list[str]:
    failures: list[str] = []

    def fail(field: str, actual: object, expected: str) -> None:
        failures.append(
            f"{field}: actual={json.dumps(diagnostic_value(actual))}; expected {expected}"
        )

    def number(field: str, integer: bool = False) -> int | float | None:
        value = field_value(scorecard, field)
        valid_type = type(value) is int if integer else type(value) in (int, float)
        if not valid_type or (type(value) is float and not math.isfinite(value)):
            fail(field, value, "integer" if integer else "finite number")
            return None
        return value

    def bound(
        field: str, limit: int | float, minimum: bool = False, integer: bool = True
    ) -> None:
        value = number(field, integer=integer)
        if value is not None and (value < limit if minimum else value > limit):
            fail(field, value, f'{">=" if minimum else "<="} {limit}')

    for field, expected in expected_evidence(scenario).items():
        actual = field_value(scorecard, field)
        if type(actual) is not type(expected) or actual != expected:
            fail(field, actual, f"== {json.dumps(expected)}")
    for field in ("data_key_transitions", "acknowledged_key_updates"):
        bound(f"measurement.security.{field}", MINIMUM_KEY_TRANSITIONS, minimum=True)
    bound("maximum_burst_depth", MAXIMUM_BURST_DEPTH)
    bound(
        "egress_deadline_error_microseconds.p99_9",
        MAXIMUM_EGRESS_P99_9_MICROSECONDS,
        integer=False,
    )
    if scenario.profile == "ts-1316":
        bound("pcr_span_count", 1, minimum=True)
        bound(
            "pcr_absolute_span_rate_error_ppm.maximum",
            MAXIMUM_PCR_SPAN_RATE_ERROR_PPM,
            integer=False,
        )
    elif field_value(scorecard, "pcr_span_count") != 0:
        fail("pcr_span_count", field_value(scorecard, "pcr_span_count"), "== 0")
    if scenario.maximum_tsbpd_phase_range_microseconds is not None:
        maximum = number("mapped_tsbpd_latency_microseconds.maximum")
        minimum = number("mapped_tsbpd_latency_microseconds.minimum")
        if maximum is not None and minimum is not None:
            phase = maximum - minimum
            if phase > scenario.maximum_tsbpd_phase_range_microseconds:
                fail(
                    "mapped_tsbpd_latency_microseconds.range (TSBPD phase)",
                    phase,
                    f"<= {scenario.maximum_tsbpd_phase_range_microseconds}",
                )
    if scenario.uses_group_sender:
        paths = field_value(scorecard, "measurement.security.member_paths")
        if not isinstance(paths, dict) or set(paths) != {"primary", "backup"}:
            fail(
                "measurement.security.member_paths", paths, "exactly primary and backup"
            )
        identities = []
        for member in ("primary", "backup"):
            prefix = f"measurement.security.member_paths.{member}"
            mode = field_value(scorecard, f"{prefix}.crypto_mode")
            if mode != "gcm":
                fail(f"{prefix}.crypto_mode", mode, '== "gcm"')
            bound(f"{prefix}.data_key_transitions", 1, minimum=True)
            identities.append(number(f"{prefix}.receiver_socket_id", integer=True))
        if None not in identities and identities[0] == identities[1]:
            fail(
                "measurement.security.member_paths.receiver_socket_id",
                identities,
                "two distinct integer identities",
            )
    return failures


def validate_scorecard(scenario: TimingScenario, scorecard: dict[str, object]) -> None:
    failures = scorecard_violations(scenario, scorecard)
    if failures:
        raise RuntimeError(
            f"AEAD timing {scenario.name}: profile evidence or quality gate failed\n"
            + "\n".join(failures)
        )


def diagnostic_scorecard(
    scenario: TimingScenario, scorecard: dict[str, object]
) -> dict[str, object]:
    """An allowlisted projection, not a copy of raw process/measurement data."""
    fields = list(expected_evidence(scenario)) + [
        "maximum_burst_depth",
        "pcr_span_count",
        "measurement.security.data_key_transitions",
        "measurement.security.acknowledged_key_updates",
    ]
    for metric in (
        "egress_deadline_error_microseconds",
        "release_deadline_error_microseconds",
        "mapped_tsbpd_latency_microseconds",
        "release_to_egress_microseconds",
        "egress_interval_error_microseconds",
        "absolute_egress_interval_error_microseconds",
        "pcr_absolute_span_rate_error_ppm",
    ):
        fields.extend(
            f"{metric}.{statistic}"
            for statistic in ("minimum", "maximum", "mean", "p50", "p99", "p99_9")
        )
    for member in ("primary", "backup"):
        fields.extend(
            f"measurement.security.member_paths.{member}.{field}"
            for field in ("crypto_mode", "data_key_transitions", "receiver_socket_id")
        )
    result: dict[str, object] = {}
    for field in fields:
        value = field_value(scorecard, field)
        if value == "<missing>":
            continue
        target = result
        keys = field.split(".")
        for key in keys[:-1]:
            target = target.setdefault(key, {})
        target[keys[-1]] = diagnostic_value(value)
    return result


def failure_details(scenario: TimingScenario, output: Path, error: Exception) -> str:
    """Preserve measurements before the temporary scorecard is removed."""
    # RuntimeError messages here are constructed by this harness. Do not print
    # TimeoutExpired.cmd/output or JSON/OSError messages with arbitrary content.
    reason = str(error) if isinstance(error, RuntimeError) else type(error).__name__
    details = [f"FAIL {scenario.name}: {reason}"]
    try:
        scorecard = json.loads(output.read_text(encoding="utf-8"))
        if not isinstance(scorecard, dict):
            raise ValueError("scorecard is not an object")
    except (OSError, ValueError) as scorecard_error:
        details.append(f"Scorecard unavailable: {type(scorecard_error).__name__}")
    else:
        details.extend(
            violation
            for violation in scorecard_violations(scenario, scorecard)
            if violation not in reason.splitlines()
        )
        details.extend(
            (
                "--- measured scorecard ---",
                json.dumps(
                    diagnostic_scorecard(scenario, scorecard), indent=2, sort_keys=True
                ),
            )
        )
    details.extend(
        (
            "--- expected profile and limits ---",
            json.dumps(
                {
                    "sample_count": scenario.messages,
                    "payload_sizes": [
                        1_316 if scenario.profile == "ts-1316" else 1_200
                    ],
                    "payload_integrity_compared": scenario.messages,
                    "payload_integrity_uncompared": 0,
                    "payload_integrity_failures": 0,
                    "early_srt_release_count": 0,
                    "early_udp_egress_count": 0,
                    "tsbpd_deadline_regression_count": 0,
                    "srt_release_regression_count": 0,
                    "maximum_burst_depth": MAXIMUM_BURST_DEPTH,
                    "maximum_egress_p99_9_microseconds": (
                        MAXIMUM_EGRESS_P99_9_MICROSECONDS
                    ),
                    "maximum_pcr_span_rate_error_ppm": (
                        MAXIMUM_PCR_SPAN_RATE_ERROR_PPM
                        if scenario.profile == "ts-1316"
                        else None
                    ),
                    "maximum_tsbpd_phase_range_microseconds": (
                        scenario.maximum_tsbpd_phase_range_microseconds
                    ),
                },
                indent=2,
                sort_keys=True,
            ),
        )
    )
    return "\n".join(details)


def retain_failure(
    directory: Path, scenario: TimingScenario, output: Path, details: str
) -> None:
    """Only explicitly constructed diagnostics may leave the temporary tree."""
    (directory / f"{scenario.name}.log").write_text(details + "\n", encoding="utf-8")
    try:
        scorecard = json.loads(output.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return
    if isinstance(scorecard, dict):
        (directory / f"{scenario.name}.json").write_text(
            json.dumps(
                diagnostic_scorecard(scenario, scorecard), indent=2, sort_keys=True
            )
            + "\n",
            encoding="utf-8",
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timing-peer", type=Path, required=True)
    parser.add_argument("--sender-peer", type=Path, required=True)
    parser.add_argument("--group-sender-peer", type=Path, required=True)
    parser.add_argument(
        "--failure-artifacts",
        type=Path,
        help="Retain allowlisted failure scorecards and diagnostic logs here",
    )
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    timing_peer = resolve_program_path(arguments.timing_peer)
    sender_peer = resolve_program_path(arguments.sender_peer)
    group_sender_peer = resolve_program_path(arguments.group_sender_peer)
    failures: list[str] = []
    artifact_directory: Path | None = None
    with tempfile.TemporaryDirectory(
        prefix="robotweax-srt-aead-timing-"
    ) as temporary_directory:
        directory = Path(temporary_directory)
        for scenario in timing_scenarios():
            output = directory / f"{scenario.name}.json"
            try:
                result = subprocess.run(
                    scenario_command(
                        scenario,
                        timing_peer,
                        sender_peer,
                        group_sender_peer,
                        output,
                    ),
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout=scenario.timeout_seconds + 15,
                )
                if result.returncode != 0:
                    raise RuntimeError(f"child exit_code={result.returncode}")
                scorecard = json.loads(output.read_text(encoding="utf-8"))
                if not isinstance(scorecard, dict):
                    raise RuntimeError("AEAD timing scorecard is not an object")
                validate_scorecard(scenario, scorecard)
                print(f"PASS {scenario.name}")
            except (
                OSError,
                RuntimeError,
                ValueError,
                json.JSONDecodeError,
                subprocess.TimeoutExpired,
            ) as error:
                details = failure_details(scenario, output, error)
                if isinstance(error, subprocess.TimeoutExpired):
                    details += (
                        f"\nchild timeout_seconds={scenario.timeout_seconds + 15}"
                    )
                details += (
                    "\nRaw stdout/stderr omitted (may contain credentials or payloads)."
                )
                failures.append(details)
                if arguments.failure_artifacts is not None:
                    try:
                        if artifact_directory is None:
                            arguments.failure_artifacts.mkdir(
                                parents=True, exist_ok=True
                            )
                            artifact_directory = Path(
                                tempfile.mkdtemp(
                                    prefix="run-", dir=arguments.failure_artifacts
                                )
                            )
                        retain_failure(artifact_directory, scenario, output, details)
                    except OSError as artifact_error:
                        failures.append(
                            f"FAIL {scenario.name}: artifact retention failed: "
                            f"{type(artifact_error).__name__}"
                        )
    if failures:
        print(
            "AES-GCM timing failures:\n\n" + "\n\n".join(failures),
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
