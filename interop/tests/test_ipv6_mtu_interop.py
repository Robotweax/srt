from __future__ import annotations

import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_ipv6_mtu_interop  # noqa: E402


class Ipv6MtuInteropUnitTests(unittest.TestCase):
    def test_matrix_covers_both_implementation_directions(self) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_ipv6_mtu_interop.scenario_matrix(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 2)
        self.assertEqual(scenarios[0].caller, robotweax)
        self.assertEqual(scenarios[0].listener, reference)
        self.assertEqual(scenarios[1].caller, reference)
        self.assertEqual(scenarios[1].listener, robotweax)
        self.assertTrue(
            all(scenario.negotiated_mss == 1_280 for scenario in scenarios)
        )

    def test_peer_command_passes_ipv6_mss_and_exact_chunk_size(self) -> None:
        options = run_ipv6_mtu_interop.RunOptions()
        command = run_ipv6_mtu_interop.peer_command(
            Path("/peer"),
            "caller",
            9_999,
            Path("/payload"),
            options,
            1_400,
            "#!::r=test,m=request",
            43,
            0x88,
        )

        self.assertEqual(
            command[command.index("--host") + 1], "::1"
        )
        self.assertEqual(
            command[command.index("--mss") + 1], "1400"
        )
        self.assertEqual(
            command[command.index("--chunk-size") + 1], "1216"
        )
        self.assertEqual(command[command.index("--ipttl") + 1], "43")
        self.assertEqual(command[command.index("--iptos") + 1], "136")
        self.assertEqual(
            command[command.index("--stream-id") + 1],
            "#!::r=test,m=request",
        )
        self.assertIn("--connect-callback", command)
        self.assertNotIn("--require-connect-callback", command)

        strict_command = run_ipv6_mtu_interop.peer_command(
            Path("/peer"),
            "caller",
            9_999,
            Path("/payload"),
            options,
            1_400,
            "#!::r=test,m=request",
            43,
            0x88,
            require_connect_callback=True,
        )
        self.assertIn("--require-connect-callback", strict_command)

    def test_listener_command_expects_the_caller_stream_id(self) -> None:
        command = run_ipv6_mtu_interop.peer_command(
            Path("/peer"),
            "listener",
            9_999,
            Path("/payload"),
            run_ipv6_mtu_interop.RunOptions(),
            1_280,
            "route",
            44,
            0x90,
        )
        self.assertEqual(
            command[command.index("--expect-stream-id") + 1],
            "route",
        )
        self.assertNotIn("--connect-callback", command)
        self.assertTrue(
            run_ipv6_mtu_interop.stream_id_was_verified(
                '{"event":"stream_id_match","bytes":5}', 5
            )
        )

    def test_connect_callback_requires_ipv6_success_contract(self) -> None:
        event = (
            '{"event":"connect_callback","valid":true,'
            '"error_code":0,"peer_family":10,"token":-1,'
            '"socket_state":5,"peer_name_available":true}'
        )
        self.assertTrue(
            run_ipv6_mtu_interop.connect_callback_was_valid(event)
        )
        self.assertFalse(
            run_ipv6_mtu_interop.connect_callback_was_valid(
                event.replace('"peer_family":10', '"peer_family":2')
            )
        )

    def test_reference_callback_is_diagnostic_but_robotweax_is_strict(
        self,
    ) -> None:
        delayed_reference = (
            '{"event":"connect_callback_observation",'
            '"completed":false}'
        )
        self.assertTrue(
            run_ipv6_mtu_interop.connect_callback_meets_policy(
                delayed_reference, required=False
            )
        )
        self.assertFalse(
            run_ipv6_mtu_interop.connect_callback_meets_policy(
                delayed_reference, required=True
            )
        )

    def test_network_option_validation_requires_exact_native_values(
        self,
    ) -> None:
        event = {"network": {"ipttl": 43, "iptos": 0x88}}
        run_ipv6_mtu_interop.validate_network_options(
            event, peer_name="peer", ipttl=43, iptos=0x88
        )
        with self.assertRaisesRegex(RuntimeError, "network options"):
            run_ipv6_mtu_interop.validate_network_options(
                event, peer_name="peer", ipttl=44, iptos=0x88
            )

    def test_robotweax_statistics_require_ipv6_wire_overhead(self) -> None:
        event = {
            "stats": {
                "byteMSS": 1_280,
                "pktSentTotal": 3,
                "byteSentTotal": 3_192,
                "byteSentUniqueTotal": 3_192,
            }
        }
        run_ipv6_mtu_interop.validate_robotweax_statistics(
            event,
            sender=True,
            byte_count=3_000,
            packet_count=3,
            negotiated_mss=1_280,
        )

        event["stats"]["byteSentTotal"] = 3_132
        with self.assertRaisesRegex(
            RuntimeError, "expected IPv6 wire total"
        ):
            run_ipv6_mtu_interop.validate_robotweax_statistics(
                event,
                sender=True,
                byte_count=3_000,
                packet_count=3,
                negotiated_mss=1_280,
            )

    def test_completion_readiness_requires_drained_buffers_and_known_flags(
        self,
    ) -> None:
        event = {
            "readiness": {
                "event": run_ipv6_mtu_interop.SRT_EPOLL_OUT,
                "snddata": 0,
                "rcvdata": 0,
            }
        }
        run_ipv6_mtu_interop.validate_completion_readiness(
            event, peer_name="peer"
        )

        event["readiness"]["event"] = (
            run_ipv6_mtu_interop.SRT_EPOLL_ERR
        )
        run_ipv6_mtu_interop.validate_completion_readiness(
            event, peer_name="terminal peer"
        )

        for invalid in (
            {},
            {"readiness": {"event": 0, "snddata": 0, "rcvdata": 0}},
            {
                "readiness": {
                    "event": run_ipv6_mtu_interop.SRT_EPOLL_IN,
                    "snddata": 0,
                    "rcvdata": 0,
                }
            },
            {"readiness": {"event": 0x20, "snddata": 0, "rcvdata": 0}},
            {
                "readiness": {
                    "event": run_ipv6_mtu_interop.SRT_EPOLL_OUT,
                    "snddata": 1,
                    "rcvdata": 0,
                }
            },
        ):
            with self.assertRaises(RuntimeError):
                run_ipv6_mtu_interop.validate_completion_readiness(
                    invalid, peer_name="peer"
                )

    def test_peer_version_requires_the_pinned_v1_5_5_value(self) -> None:
        run_ipv6_mtu_interop.validate_peer_version(
            {"peer_version": run_ipv6_mtu_interop.PINNED_SRT_VERSION},
            peer_name="peer",
        )
        for invalid in ({}, {"peer_version": "1.5.5"}, {"peer_version": 0}):
            with self.assertRaises(RuntimeError):
                run_ipv6_mtu_interop.validate_peer_version(
                    invalid, peer_name="peer"
                )

    def test_version_timing_options_require_reference_defaults(self) -> None:
        valid = {
            "version_timing": {
                "drift_tracer": True,
                "minimum_input_bandwidth": 0,
                "minimum_peer_version": 0x0001_0000,
                "connection_time": 100,
                "current_time": 200,
            }
        }
        run_ipv6_mtu_interop.validate_version_timing_options(
            valid, peer_name="peer"
        )
        invalid = {"version_timing": dict(valid["version_timing"])}
        invalid["version_timing"]["drift_tracer"] = False
        with self.assertRaises(RuntimeError):
            run_ipv6_mtu_interop.validate_version_timing_options(
                invalid, peer_name="peer"
            )

        invalid_time = {
            "version_timing": dict(valid["version_timing"])
        }
        invalid_time["version_timing"]["connection_time"] = 201
        with self.assertRaises(RuntimeError):
            run_ipv6_mtu_interop.validate_version_timing_options(
                invalid_time, peer_name="peer"
            )


if __name__ == "__main__":
    unittest.main()
