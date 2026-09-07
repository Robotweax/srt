from __future__ import annotations

import socket
import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_binding_interop  # noqa: E402


class BindingInteropUnitTests(unittest.TestCase):
    def test_shared_matrix_covers_both_directions_and_address_modes(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_binding_interop.shared_scenario_matrix(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 6)
        self.assertEqual(
            {scenario.caller for scenario in scenarios},
            {robotweax, reference},
        )
        self.assertEqual(
            {scenario.name.split("-")[1] for scenario in scenarios},
            {"ipv4", "ipv6", "dual"},
        )
        dual_stack = [
            scenario
            for scenario in scenarios
            if "dual-stack" in scenario.name
        ]
        self.assertEqual(len(dual_stack), 2)
        self.assertTrue(
            all(scenario.local_host == "::" for scenario in dual_stack)
        )
        self.assertTrue(
            all(
                scenario.peer_host == "::ffff:127.0.0.1"
                and scenario.ipv6_only == 0
                for scenario in dual_stack
            )
        )

    def test_acquired_matrix_covers_both_families_and_directions(self) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_binding_interop.acquired_scenario_matrix(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 4)
        self.assertEqual(
            {scenario.caller for scenario in scenarios},
            {robotweax, reference},
        )
        self.assertEqual(
            {scenario.local_host for scenario in scenarios},
            {"127.0.0.1", "::1"},
        )

    def test_shared_command_preserves_arbitrary_and_ts_sized_chunks(
        self,
    ) -> None:
        scenario = run_binding_interop.SharedScenario(
            name="shared-dual-stack-robotweax-to-haivision",
            caller=Path("/robotweax"),
            listener=Path("/haivision"),
            local_host="::",
            listener_host="::",
            peer_host="::ffff:127.0.0.1",
            ipv6_only=0,
            seed=1,
        )
        command = run_binding_interop.shared_caller_command(
            scenario,
            9_000,
            9_001,
            9_002,
            Path("/first"),
            Path("/second"),
            run_binding_interop.RunOptions(),
        )

        self.assertEqual(command[command.index("--local-host") + 1], "::")
        self.assertEqual(
            command[command.index("--host") + 1],
            "::ffff:127.0.0.1",
        )
        self.assertEqual(command[command.index("--chunk-size") + 1], "1200")
        self.assertEqual(
            command[command.index("--second-chunk-size") + 1], "1316"
        )
        self.assertEqual(command[command.index("--ipv6-only") + 1], "0")

    def test_ipv4_listener_omits_ipv6_only(self) -> None:
        command = run_binding_interop.listener_command(
            Path("/peer"),
            "127.0.0.1",
            9_000,
            Path("/output"),
            100,
            run_binding_interop.RunOptions(),
            None,
        )
        self.assertNotIn("--ipv6-only", command)

    def test_acquired_command_requests_exact_ipv6_mode(self) -> None:
        scenario = run_binding_interop.AcquiredScenario(
            name="acquired-ipv6",
            caller=Path("/robotweax"),
            listener=Path("/haivision"),
            local_host="::1",
            peer_host="::1",
            ipv6_only=1,
            seed=1,
        )
        command = run_binding_interop.acquired_caller_command(
            scenario,
            9_000,
            Path("/input"),
            run_binding_interop.RunOptions(),
        )
        self.assertEqual(command[command.index("--ipv6-only") + 1], "1")

    def test_event_parser_ignores_non_json_diagnostics(self) -> None:
        event = run_binding_interop.parse_event(
            "diagnostic\n"
            '{"event":"shared_complete","first_bytes":123}\n',
            "shared_complete",
        )
        self.assertEqual(event["first_bytes"], 123)
        with self.assertRaisesRegex(RuntimeError, "did not report"):
            run_binding_interop.parse_event("diagnostic", "complete")

    def test_family_and_ipv6_mode_validation_are_strict(self) -> None:
        self.assertEqual(
            run_binding_interop.expected_family("127.0.0.1"),
            socket.AF_INET,
        )
        self.assertEqual(
            run_binding_interop.expected_family("::ffff:127.0.0.1"),
            socket.AF_INET6,
        )
        run_binding_interop.validate_ipv6_only(
            {"ipv6_only": 0}, 0, "dual-stack"
        )
        with self.assertRaisesRegex(RuntimeError, "SRTO_IPV6ONLY"):
            run_binding_interop.validate_ipv6_only(
                {"ipv6_only": 1}, 0, "dual-stack"
            )

    def test_compatibility_version_is_pinned(self) -> None:
        run_binding_interop.validate_compatibility_version(
            {"srt_version": run_binding_interop.PINNED_SRT_VERSION},
            "peer",
        )
        with self.assertRaisesRegex(RuntimeError, "compatibility version"):
            run_binding_interop.validate_compatibility_version(
                {"srt_version": 0x01_05_04}, "peer"
            )

    def test_acquired_release_wait_is_bounded_by_the_runner_contract(
        self,
    ) -> None:
        self.assertTrue(
            run_binding_interop.valid_acquired_release_wait(
                {"release_wait_us": 0}
            )
        )
        self.assertTrue(
            run_binding_interop.valid_acquired_release_wait(
                {"release_wait_us": 3_000_000}
            )
        )
        for invalid in (
            {},
            {"release_wait_us": -1},
            {"release_wait_us": 3_000_001},
            {"release_wait_us": 1.0},
        ):
            self.assertFalse(
                run_binding_interop.valid_acquired_release_wait(invalid)
            )


if __name__ == "__main__":
    unittest.main()
