from __future__ import annotations

import hashlib
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_encrypted_interop  # noqa: E402
import srt_handshake_trace  # noqa: E402


class EncryptedInteropUnitTests(unittest.TestCase):
    def test_payload_is_deterministic_and_seeded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "first"
            second = Path(directory) / "second"
            third = Path(directory) / "third"
            first_digest = run_encrypted_interop.write_deterministic_payload(
                first, 131_071, 42
            )
            second_digest = run_encrypted_interop.write_deterministic_payload(
                second, 131_071, 42
            )
            third_digest = run_encrypted_interop.write_deterministic_payload(
                third, 131_071, 43
            )

            self.assertEqual(first_digest, second_digest)
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertNotEqual(first_digest, third_digest)

    def test_peer_command_passes_only_the_secret_environment_name(self) -> None:
        options = run_encrypted_interop.RunOptions(
            byte_count=4_194_304,
            timeout_seconds=30,
            key_refresh_rate=1_000,
            key_preannouncement=400,
        )
        command = run_encrypted_interop.peer_command(
            Path("/tmp/peer"),
            "caller",
            9_999,
            Path("/tmp/payload"),
            options,
            32,
        )

        self.assertIn("--passphrase-env", command)
        self.assertIn(run_encrypted_interop.PASSPHRASE_ENVIRONMENT, command)
        self.assertIn("--km-refresh-rate", command)
        self.assertIn("1000", command)
        self.assertIn("--km-preannounce", command)
        self.assertIn("400", command)
        self.assertIn("--input-bw", command)
        self.assertIn("8000000", command)
        maximum_bandwidth = command.index("--max-bw")
        self.assertEqual(command[maximum_bandwidth + 1], "0")
        self.assertIn("--shutdown-grace-ms", command)
        self.assertIn("250", command)

        listener_command = run_encrypted_interop.peer_command(
            Path("/tmp/peer"),
            "listener",
            9_999,
            Path("/tmp/payload"),
            options,
            32,
        )
        self.assertNotIn("--input-bw", listener_command)
        self.assertNotIn("--max-bw", listener_command)
        self.assertIn("--shutdown-grace-ms", listener_command)
        listener_grace = listener_command.index("--shutdown-grace-ms")
        self.assertEqual(listener_command[listener_grace + 1], "250")

        ipv6_command = run_encrypted_interop.peer_command(
            Path("/tmp/peer"),
            "caller",
            9_999,
            Path("/tmp/payload"),
            options,
            32,
            "::1",
        )
        host = ipv6_command.index("--host")
        self.assertEqual(ipv6_command[host + 1], "::1")

    def test_matrix_covers_every_key_length_in_both_directions(self) -> None:
        scenarios = run_encrypted_interop.scenario_matrix(
            Path("/robotweax"), Path("/haivision")
        )

        self.assertEqual(len(scenarios), 6)
        self.assertEqual(
            [scenario.key_length for scenario in scenarios],
            [16, 16, 24, 24, 32, 32],
        )
        self.assertEqual(
            sum(scenario.caller == Path("/robotweax") for scenario in scenarios),
            3,
        )
        self.assertEqual(
            sum(scenario.caller == Path("/haivision") for scenario in scenarios),
            3,
        )

    def test_ipv6_matrix_names_expose_address_family(self) -> None:
        scenarios = run_encrypted_interop.scenario_matrix(
            Path("/robotweax"),
            Path("/haivision"),
            name_prefix="ipv6-",
        )

        self.assertEqual(len(scenarios), 6)
        self.assertTrue(
            all(
                scenario.name.startswith("ipv6-aes")
                for scenario in scenarios
            )
        )
        self.assertEqual(
            {scenario.key_length for scenario in scenarios},
            {16, 24, 32},
        )

    def test_baseline_stops_before_key_preannouncement(self) -> None:
        options = run_encrypted_interop.RunOptions(
            byte_count=4_194_304,
            timeout_seconds=30,
            key_refresh_rate=1_000,
            key_preannouncement=400,
        )

        scenarios, baseline, packet_count = (
            run_encrypted_interop.no_rotation_baselines(
                Path("/robotweax"), Path("/haivision"), options
            )
        )

        self.assertEqual(
            packet_count,
            options.key_refresh_rate - options.key_preannouncement - 1,
        )
        self.assertEqual(
            baseline.byte_count,
            packet_count * options.chunk_size,
        )
        self.assertEqual(len(scenarios), 2)
        self.assertEqual(
            [scenario.key_length for scenario in scenarios],
            [16, 16],
        )
        self.assertEqual(scenarios[0].caller, Path("/robotweax"))
        self.assertEqual(scenarios[0].listener, Path("/haivision"))
        self.assertEqual(scenarios[1].caller, Path("/haivision"))
        self.assertEqual(scenarios[1].listener, Path("/robotweax"))

    def test_ipv6_baseline_names_expose_address_family(self) -> None:
        options = run_encrypted_interop.RunOptions(
            byte_count=4_194_304,
            timeout_seconds=30,
            key_refresh_rate=1_000,
            key_preannouncement=400,
        )

        scenarios, _, _ = run_encrypted_interop.no_rotation_baselines(
            Path("/robotweax"),
            Path("/haivision"),
            options,
            name_prefix="ipv6-",
        )

        self.assertEqual(
            [scenario.name for scenario in scenarios],
            [
                "ipv6-aes128-robotweax-to-haivision-no-rotation",
                "ipv6-aes128-haivision-to-robotweax-no-rotation",
            ],
        )

    def test_baselines_report_failures_from_both_directions(self) -> None:
        options = run_encrypted_interop.RunOptions(
            byte_count=718_800,
            timeout_seconds=30,
            key_refresh_rate=1_000,
            key_preannouncement=400,
        )
        scenarios, _, packet_count = (
            run_encrypted_interop.no_rotation_baselines(
                Path("/robotweax"), Path("/haivision"), options
            )
        )

        with mock.patch.object(
            run_encrypted_interop,
            "run_scenario",
            side_effect=(
                RuntimeError("robotweax caller failed"),
                RuntimeError("haivision caller failed"),
            ),
        ) as run_scenario:
            with self.assertRaises(RuntimeError) as raised:
                run_encrypted_interop.run_no_rotation_baselines(
                    scenarios,
                    options,
                    Path("/work"),
                    {},
                    packet_count,
                )

        self.assertEqual(run_scenario.call_count, 2)
        self.assertIn("robotweax caller failed", str(raised.exception))
        self.assertIn("haivision caller failed", str(raised.exception))
        self.assertTrue(
            all(
                call.kwargs["trace_handshake"] is False
                for call in run_scenario.call_args_list
            )
        )

    def test_baselines_can_enable_handshake_diagnostics_explicitly(
        self,
    ) -> None:
        options = run_encrypted_interop.RunOptions(
            byte_count=718_800,
            timeout_seconds=30,
            key_refresh_rate=1_000,
            key_preannouncement=400,
        )
        scenarios, _, packet_count = (
            run_encrypted_interop.no_rotation_baselines(
                Path("/robotweax"), Path("/haivision"), options
            )
        )

        with mock.patch.object(
            run_encrypted_interop,
            "run_scenario",
        ) as run_scenario:
            run_encrypted_interop.run_no_rotation_baselines(
                scenarios,
                options,
                Path("/work"),
                {},
                packet_count,
                trace_handshake=True,
            )

        self.assertEqual(run_scenario.call_count, 2)
        self.assertTrue(
            all(
                call.kwargs["trace_handshake"] is True
                for call in run_scenario.call_args_list
            )
        )

    def test_failure_report_preserves_process_diagnostics(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "partial"
            output.write_bytes(b"x" * 42)
            report = run_encrypted_interop.render_failure(
                run_encrypted_interop.Scenario(
                    name="diagnostic",
                    caller=Path("/robotweax"),
                    listener=Path("/haivision"),
                    key_length=16,
                    seed=1,
                ),
                "listener timed out",
                output,
                caller_stdout=b'{"event":"complete"}\n',
                listener_stderr="receive stalled\n",
                handshake_trace='{"event":"srt_handshake_trace"}',
            )

        self.assertIn("partial_output_bytes=42", report)
        self.assertIn("--- caller stdout ---", report)
        self.assertIn('{"event":"complete"}', report)
        self.assertIn("--- listener stderr ---", report)
        self.assertIn("receive stalled", report)
        self.assertIn("--- secret-safe handshake trace ---", report)
        self.assertIn('"event":"srt_handshake_trace"', report)

    def test_handshake_trace_exposes_structure_without_key_bytes(self) -> None:
        salt = bytes(range(16))
        wrapped_keys = bytes([0xA5]) * 24
        key_material = (
            bytes([0x12, 0x20, 0x29, 0x01])
            + bytes(4)
            + bytes([2, 0, 2, 0, 0, 0, 4, 4])
            + salt
            + wrapped_keys
        )
        handshake_extension = struct.pack(
            ">IIHH", 0x010505, 0xBF, 120, 120
        )
        extensions = (
            struct.pack(">HH", 1, len(handshake_extension) // 4)
            + handshake_extension
            + struct.pack(">HH", 3, len(key_material) // 4)
            + key_material
        )
        datagram = (
            struct.pack(">IIII", 0x80000000, 0, 123, 77)
            + struct.pack(
                ">IIIIIiII4I",
                5,
                (2 << 16) | 3,
                1_000,
                1_500,
                25_600,
                -1,
                88,
                0x12345678,
                0,
                0,
                0,
                0,
            )
            + extensions
        )

        traced = srt_handshake_trace.describe_handshake_datagram(
            datagram, "caller_to_listener"
        )

        self.assertIsNotNone(traced)
        assert traced is not None
        self.assertEqual(traced["encryption_field"], 2)
        self.assertEqual(traced["extension_field"], "0x0003")
        self.assertEqual(traced["request_name"], "CONCLUSION")
        described_extensions = traced["extensions"]
        self.assertIsInstance(described_extensions, list)
        assert isinstance(described_extensions, list)
        self.assertEqual(
            [extension["name"] for extension in described_extensions],
            ["HSREQ", "KMREQ"],
        )
        key_description = described_extensions[1]["key_material"]
        self.assertEqual(key_description["signature"], "0x2029")
        self.assertEqual(key_description["key_words"], 4)
        self.assertEqual(
            key_description["content_sha256"],
            hashlib.sha256(key_material).hexdigest(),
        )
        rendered = json.dumps(traced, sort_keys=True)
        self.assertNotIn(salt.hex(), rendered)
        self.assertNotIn(wrapped_keys.hex(), rendered)

    def test_legacy_handshake_trace_decodes_directional_security_flags(
        self,
    ) -> None:
        payload = struct.pack(">II", 0x010203, 0x00000025)
        datagram = (
            struct.pack(">IIII", 0xFFFF0001, 0, 321, 77)
            + payload
        )

        traced = (
            srt_handshake_trace.describe_legacy_handshake_datagram(
                datagram, "caller_to_listener"
            )
        )

        self.assertIsNotNone(traced)
        assert traced is not None
        self.assertEqual(traced["name"], "HSREQ")
        self.assertEqual(traced["srt_version"], 0x010203)
        self.assertEqual(traced["flags"], "0x00000025")
        self.assertEqual(traced["content_bytes"], 8)
        self.assertNotIn("malformed", traced)

    def test_legacy_handshake_trace_marks_bad_body_shapes(self) -> None:
        header = struct.pack(">IIII", 0xFFFF0002, 0, 321, 77)

        truncated = (
            srt_handshake_trace.describe_legacy_handshake_datagram(
                header + b"1234567", "listener_to_caller"
            )
        )
        unaligned = (
            srt_handshake_trace.describe_legacy_handshake_datagram(
                header + b"123456789", "listener_to_caller"
            )
        )

        assert truncated is not None
        assert unaligned is not None
        self.assertIn("truncated", str(truncated["malformed"]))
        self.assertIn("word aligned", str(unaligned["malformed"]))

    def test_complete_event_parser_ignores_non_json_output(self) -> None:
        event = run_encrypted_interop.parse_complete(
            'diagnostic\n{"event":"complete","role":"caller","bytes":10}\n',
            "caller",
        )

        self.assertEqual(event["bytes"], 10)

    def test_nonnegative_statistic_rejects_missing_or_invalid_values(
        self,
    ) -> None:
        self.assertEqual(
            run_encrypted_interop.nonnegative_statistic(
                {"stats": {"pktRecvTotal": 599}}, "pktRecvTotal"
            ),
            599,
        )
        for event in (
            {},
            {"stats": None},
            {"stats": {}},
            {"stats": {"pktRecvTotal": -1}},
            {"stats": {"pktRecvTotal": True}},
            {"stats": {"pktRecvTotal": "599"}},
        ):
            self.assertIsNone(
                run_encrypted_interop.nonnegative_statistic(
                    event, "pktRecvTotal"
                )
            )

    def test_sender_statistics_require_all_packets_and_no_drops(
        self,
    ) -> None:
        complete = {
            "stats": {
                "pktSentTotal": 3_496,
                "pktSndDropTotal": 0,
            }
        }
        self.assertEqual(
            run_encrypted_interop.validate_sender_statistics(
                complete, 3_496
            ),
            3_496,
        )

        with self.assertRaisesRegex(RuntimeError, "dropped packets"):
            run_encrypted_interop.validate_sender_statistics(
                {
                    "stats": {
                        "pktSentTotal": 3_150,
                        "pktSndDropTotal": 346,
                    }
                },
                3_496,
            )
        with self.assertRaisesRegex(RuntimeError, "only 3150 packets"):
            run_encrypted_interop.validate_sender_statistics(
                {
                    "stats": {
                        "pktSentTotal": 3_150,
                        "pktSndDropTotal": 0,
                    }
                },
                3_496,
            )
        with self.assertRaisesRegex(RuntimeError, "unavailable"):
            run_encrypted_interop.validate_sender_statistics(
                {"stats": {"pktSentTotal": 3_496}},
                3_496,
            )

    def test_expected_live_packet_count_rounds_up(self) -> None:
        self.assertEqual(
            run_encrypted_interop.expected_live_packets(
                4_194_304, 1_200
            ),
            3_496,
        )
        self.assertEqual(
            run_encrypted_interop.expected_live_packets(1_200, 1_200),
            1,
        )
        with self.assertRaises(ValueError):
            run_encrypted_interop.expected_live_packets(1, 0)


if __name__ == "__main__":
    unittest.main()
