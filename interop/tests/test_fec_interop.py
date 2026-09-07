from __future__ import annotations

import json
import sys
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_fec_interop  # noqa: E402
from srt_handshake_trace import (  # noqa: E402
    CallerListenerFaultProxy,
    CallerListenerSequenceProxy,
    RendezvousFault,
)


class FecInteropUnitTests(unittest.TestCase):
    def test_matrix_covers_all_geometries_and_sender_directions(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")

        scenarios = run_fec_interop.scenario_matrix(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 6)
        self.assertEqual(scenarios[0].caller, robotweax)
        self.assertEqual(scenarios[0].listener, reference)
        self.assertEqual(scenarios[1].caller, reference)
        self.assertEqual(scenarios[1].listener, robotweax)
        self.assertEqual(scenarios[2].caller, robotweax)
        self.assertEqual(scenarios[2].listener, reference)
        self.assertEqual(scenarios[3].caller, reference)
        self.assertEqual(scenarios[3].listener, robotweax)
        self.assertEqual(scenarios[4].caller, robotweax)
        self.assertEqual(scenarios[4].listener, reference)
        self.assertEqual(scenarios[5].caller, reference)
        self.assertEqual(scenarios[5].listener, robotweax)
        self.assertEqual(
            [scenario.geometry for scenario in scenarios],
            [
                "row",
                "row",
                "column",
                "column",
                "matrix",
                "matrix",
            ],
        )

    def test_rendezvous_uses_reference_stable_filter_order(self) -> None:
        scenarios = run_fec_interop.rendezvous_scenarios(
            Path("/robotweax"), Path("/haivision")
        )

        expected = {
            "row": "fec,arq:onreq,cols:10,layout:even,rows:1",
            "column": "fec,arq:onreq,cols:10,layout:even,rows:-5",
            "matrix": "fec,arq:onreq,cols:10,layout:even,rows:5",
        }
        for scenario in scenarios:
            self.assertEqual(
                scenario.profile.packet_filter,
                expected[scenario.profile.geometry],
            )

        probe = run_fec_interop.rendezvous_role_probe_scenario(
            Path("/robotweax"), Path("/haivision"), 0
        )
        self.assertEqual(
            probe.profile.packet_filter, expected["row"]
        )

    def test_peer_command_negotiates_profiled_fec_configuration(
        self,
    ) -> None:
        (
            row_scenario,
            _,
            column_scenario,
            _,
            matrix_scenario,
            _,
        ) = (
            run_fec_interop.scenario_matrix(
                Path("/robotweax"), Path("/haivision")
            )
        )
        command = run_fec_interop.peer_command(
            row_scenario,
            Path("/peer"),
            "caller",
            9_999,
            Path("/payload"),
            run_fec_interop.RunOptions(),
        )

        self.assertEqual(
            command[command.index("--packet-filter") + 1],
            "fec,cols:10,rows:1,layout:even,arq:onreq",
        )
        column_command = run_fec_interop.peer_command(
            column_scenario,
            Path("/peer"),
            "caller",
            9_999,
            Path("/payload"),
            run_fec_interop.RunOptions(),
        )
        self.assertEqual(
            column_command[
                column_command.index("--packet-filter") + 1
            ],
            "fec,cols:10,rows:-5,layout:even,arq:onreq",
        )
        matrix_command = run_fec_interop.peer_command(
            matrix_scenario,
            Path("/peer"),
            "caller",
            9_999,
            Path("/payload"),
            run_fec_interop.RunOptions(),
        )
        self.assertEqual(
            matrix_command[
                matrix_command.index("--packet-filter") + 1
            ],
            "fec,cols:10,rows:5,layout:even,arq:onreq",
        )
        self.assertEqual(
            command[command.index("--chunk-size") + 1], "1316"
        )
        self.assertEqual(
            command[command.index("--receive-size") + 1], "1452"
        )
        self.assertEqual(
            command[command.index("--transport") + 1], "live"
        )
        self.assertEqual(
            run_fec_interop.SOURCE_PACKET_SIZE,
            7 * run_fec_interop.MPEG_TS_PACKET_SIZE,
        )
        self.assertGreater(
            run_fec_interop.FEC_RECEIVE_CAPACITY,
            run_fec_interop.SOURCE_PACKET_SIZE,
        )

    def test_encrypted_matrix_profile_spans_three_keyed_groups(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_fec_interop.encrypted_matrix_scenarios(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 2)
        self.assertEqual(scenarios[0].caller, robotweax)
        self.assertEqual(scenarios[0].listener, reference)
        self.assertEqual(scenarios[1].caller, reference)
        self.assertEqual(scenarios[1].listener, robotweax)
        for scenario in scenarios:
            self.assertEqual(scenario.geometry, "encrypted-matrix")
            self.assertEqual(scenario.key_length, 32)
            self.assertEqual(scenario.byte_count_multiplier, 3)
            self.assertEqual(scenario.minimum_key_transitions, 2)
            self.assertEqual(scenario.expected_reconstructions, 9)
            self.assertEqual(
                scenario.fault_occurrences,
                (1, 2, 12, 66, 67, 77, 131, 132, 142),
            )
            self.assertEqual(
                scenario.expected_sequence_offsets,
                (0, 1, 11, 65, 66, 76, 130, 131, 141),
            )
            self.assertEqual(
                scenario.expected_control_packets(150), 45
            )

    def test_encrypted_matrix_command_uses_secret_environment_name(
        self,
    ) -> None:
        scenario = run_fec_interop.encrypted_matrix_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[0]
        options = run_fec_interop.RunOptions()
        command = run_fec_interop.peer_command(
            scenario,
            scenario.caller,
            "caller",
            9_999,
            Path("/payload"),
            options,
        )

        self.assertEqual(
            command[command.index("--passphrase-env") + 1],
            run_fec_interop.PASSPHRASE_ENVIRONMENT,
        )
        self.assertEqual(
            command[command.index("--pbkeylen") + 1], "32"
        )
        self.assertEqual(
            command[command.index("--km-refresh-rate") + 1], "50"
        )
        self.assertEqual(
            command[command.index("--km-preannounce") + 1], "20"
        )

    def test_encrypted_rendezvous_covers_every_geometry_across_rotation(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_fec_interop.encrypted_rendezvous_scenarios(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 12)
        self.assertEqual(
            [(item.sender, item.receiver) for item in scenarios],
            [
                (robotweax, reference),
                (reference, robotweax),
                (robotweax, reference),
                (reference, robotweax),
            ]
            * 3,
        )
        self.assertEqual(
            [item.start_order for item in scenarios],
            [
                "sender-first",
                "receiver-first",
                "receiver-first",
                "sender-first",
            ]
            * 3,
        )
        expected_geometries = [
            "matrix",
            "matrix",
            "matrix",
            "matrix",
            "row",
            "row",
            "row",
            "row",
            "column",
            "column",
            "column",
            "column",
        ]
        expected_parity = [45] * 4 + [15] * 4 + [30] * 4
        for item, geometry, parity in zip(
            scenarios,
            expected_geometries,
            expected_parity,
            strict=True,
        ):
            profile = item.profile
            self.assertEqual(
                profile.geometry,
                f"encrypted-{geometry}-rendezvous",
            )
            self.assertEqual(profile.key_length, 32)
            self.assertEqual(profile.byte_count_multiplier, 3)
            self.assertEqual(profile.minimum_key_transitions, 2)
            burst = profile.name.endswith("-burst")
            if burst and geometry == "row":
                self.assertEqual(profile.fault_occurrences, (5, 6))
                self.assertEqual(profile.expected_reconstructions, 0)
                reference_receiver = item.receiver == reference
                expected_arq_packets = 3 if reference_receiver else 2
                self.assertEqual(
                    profile.expected_filter_losses,
                    expected_arq_packets,
                )
                self.assertEqual(
                    profile.expected_arq_sequence_offsets,
                    tuple(range(expected_arq_packets)),
                )
                self.assertEqual(
                    profile.accept_reference_row_arq_expansion,
                    reference_receiver,
                )
                self.assertEqual(
                    profile.recovery_policy,
                    run_fec_interop.ARQ_HANDOFF,
                )
            elif burst:
                self.assertEqual(profile.fault_occurrences, (5, 6, 7))
                self.assertEqual(profile.expected_reconstructions, 3)
                self.assertEqual(profile.expected_filter_losses, 0)
                self.assertEqual(
                    profile.recovery_policy,
                    run_fec_interop.FEC_RECOVERY_ONLY,
                )
            else:
                self.assertEqual(profile.fault_occurrences, (5,))
                self.assertEqual(profile.expected_reconstructions, 1)
                self.assertEqual(profile.expected_filter_losses, 0)
                self.assertEqual(
                    profile.recovery_policy,
                    run_fec_interop.FEC_RECOVERY_ONLY,
                )
            self.assertEqual(
                profile.packet_filter,
                run_fec_interop.RENDEZVOUS_PACKET_FILTERS[geometry],
            )
            self.assertEqual(
                profile.expected_control_packets(150), parity
            )
            self.assertEqual(
                profile.accept_reference_deferred_key_responses,
                item.sender == reference,
            )

        command = run_fec_interop.rendezvous_peer_command(
            scenarios[0],
            scenarios[0].sender,
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/payload"),
            run_fec_interop.RunOptions(),
        )
        self.assertEqual(
            command[command.index("--passphrase-env") + 1],
            run_fec_interop.PASSPHRASE_ENVIRONMENT,
        )
        self.assertEqual(command[command.index("--pbkeylen") + 1], "32")

    def test_row_burst_requires_exact_arq_handoff_evidence(self) -> None:
        scenario = run_fec_interop.encrypted_rendezvous_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[7].profile
        sender = {
            "stats": {
                "pktSndFilterExtraTotal": 15,
                "pktRetransTotal": 2,
                "pktRecvNAKTotal": 1,
            }
        }
        receiver = {
            "stats": {
                "pktRcvFilterExtraTotal": 15,
                "pktRcvFilterSupplyTotal": 0,
                "pktRcvFilterLossTotal": 2,
            }
        }

        self.assertEqual(
            run_fec_interop.validate_filter_statistics(
                scenario, sender, receiver, 150
            ),
            (15, 0),
        )
        self.assertEqual(
            run_fec_interop.validate_arq_handoff_statistics(
                scenario, sender
            ),
            (2, 1),
        )

        observations = [
            {
                "action": "drop",
                "direction": "sender_to_receiver",
                "occurrence": occurrence,
                "packet_kind": "data",
                "filter_control": False,
                "message_number": index + 1,
                "sequence": 1_000 + index,
                "relay_ordinal": 10 + index,
                "key_selection": 1,
                "payload_bytes": 1_316,
                "payload_sha256": "a" * 64,
                "loss_report_observed": True,
                "loss_report_relay_ordinal": 20,
                "loss_report_delay_microseconds": 10,
                "retransmission_observed": True,
                "retransmission_flag": True,
                "retransmission_relay_ordinal": 30 + index,
                "retransmission_delay_microseconds": 20,
                "retransmission_key_selection": 1,
                "retransmission_payload_bytes": 1_316,
                "retransmission_payload_sha256": "a" * 64,
                "retransmission_ciphertext_matches": True,
            }
            for index, occurrence in enumerate(
                scenario.fault_occurrences
            )
        ]

        class Relay:
            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return observations

            @staticmethod
            def loss_report_observations() -> list[dict[str, object]]:
                return [
                    {
                        "direction": "receiver_to_sender",
                        "ranges": ((1_000, 1_001),),
                        "relay_ordinal": 20,
                    }
                ]

            @staticmethod
            def retransmission_observations(
                direction: str,
            ) -> list[dict[str, object]]:
                if direction != "sender_to_receiver":
                    return []
                return [
                    {
                        "direction": direction,
                        "sequence": 1_000 + index,
                        "message_number": index + 1,
                        "key_selection": 1,
                        "payload_bytes": 1_316,
                        "payload_sha256": "a" * 64,
                        "relay_ordinal": 30 + index,
                        "original_observed": True,
                        "original_relay_ordinal": 10 + index,
                        "original_key_selection": 1,
                        "original_payload_bytes": 1_316,
                        "original_payload_sha256": "a" * 64,
                        "ciphertext_matches_original": True,
                    }
                    for index in range(2)
                ]

            @staticmethod
            def arq_trace_complete() -> bool:
                return True

        self.assertEqual(
            len(
                run_fec_interop.validate_causal_recovery(
                    scenario, Relay()
                )
            ),
            2,
        )
        observations[1]["retransmission_ciphertext_matches"] = False
        with self.assertRaisesRegex(
            RuntimeError, "byte-identical ARQ handoff"
        ):
            run_fec_interop.validate_causal_recovery(
                scenario, Relay()
            )

        receiver["stats"]["pktRcvFilterSupplyTotal"] = 1
        with self.assertRaisesRegex(
            RuntimeError, "reported 1 FEC reconstructions"
        ):
            run_fec_interop.validate_filter_statistics(
                scenario, sender, receiver, 150
            )
        sender["stats"]["pktRetransTotal"] = 1
        with self.assertRaisesRegex(
            RuntimeError, "did not hand every loss to ARQ"
        ):
            run_fec_interop.validate_arq_handoff_statistics(
                scenario, sender
            )

    def test_reference_row_burst_expansion_is_exact_and_version_scoped(
        self,
    ) -> None:
        scenario = run_fec_interop.encrypted_rendezvous_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[6].profile
        self.assertTrue(scenario.accept_reference_row_arq_expansion)
        self.assertEqual(scenario.expected_sequence_offsets, (0, 1))
        self.assertEqual(
            scenario.expected_arq_sequence_offsets, (0, 1, 2)
        )
        self.assertEqual(scenario.expected_filter_losses, 3)

        class Relay:
            retransmitted = [1_000, 1_001, 1_002]

            @staticmethod
            def arq_trace_complete() -> bool:
                return True

            @staticmethod
            def loss_report_observations() -> list[dict[str, object]]:
                return [
                    {
                        "direction": "receiver_to_sender",
                        "ranges": ((1_000, 1_002),),
                        "relay_ordinal": 20,
                    }
                ]

            @classmethod
            def retransmission_observations(
                cls, direction: str
            ) -> list[dict[str, object]]:
                return [
                    {
                        "direction": direction,
                        "sequence": sequence,
                        "message_number": index + 1,
                        "key_selection": 1,
                        "payload_bytes": 1_316,
                        "payload_sha256": "a" * 64,
                        "relay_ordinal": 30 + index,
                        "original_observed": True,
                        "original_relay_ordinal": 10 + index,
                        "original_key_selection": 1,
                        "original_payload_bytes": 1_316,
                        "original_payload_sha256": "a" * 64,
                        "ciphertext_matches_original": True,
                    }
                    for index, sequence in enumerate(cls.retransmitted)
                ]

        self.assertEqual(
            run_fec_interop.validate_arq_wire_evidence(
                scenario, Relay(), 1_000
            ),
            (1, 3),
        )
        Relay.retransmitted[-1] = 1_003
        with self.assertRaisesRegex(
            RuntimeError, "unexpected ARQ retransmission"
        ):
            run_fec_interop.validate_arq_wire_evidence(
                scenario, Relay(), 1_000
            )

        with self.assertRaisesRegex(
            ValueError, "invalid FEC rendezvous scenario"
        ):
            run_fec_interop.RendezvousScenario(
                profile=replace(
                    scenario,
                    accept_reference_row_arq_expansion=False,
                    expected_arq_sequence_offsets=(0, 1),
                    expected_filter_losses=2,
                ),
                sender=Path("/robotweax"),
                receiver=Path("/haivision"),
                robotweax_peer=Path("/robotweax"),
            )

    def test_rollover_matrix_covers_every_geometry_and_direction(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_fec_interop.rollover_scenarios(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 6)
        self.assertEqual(
            [scenario.geometry for scenario in scenarios],
            [
                "row-rollover",
                "column-rollover",
                "matrix-rollover",
                "row-rollover",
                "column-rollover",
                "matrix-rollover",
            ],
        )
        self.assertEqual(
            [(scenario.caller, scenario.listener) for scenario in scenarios],
            [
                (robotweax, reference),
                (robotweax, reference),
                (robotweax, reference),
                (reference, robotweax),
                (reference, robotweax),
                (reference, robotweax),
            ],
        )
        for scenario in scenarios:
            self.assertTrue(scenario.rollover)
            self.assertEqual(
                scenario.translated_initial_sequence,
                run_fec_interop.ROLLOVER_TARGET_ISN,
            )
        self.assertEqual(scenarios[0].fault_occurrences, (8,))
        self.assertEqual(scenarios[1].fault_occurrences, (12,))
        self.assertEqual(
            scenarios[2].fault_occurrences, (1, 2, 12)
        )
        self.assertEqual(
            scenarios[2].expected_sequence_offsets, (0, 1, 11)
        )

        command = run_fec_interop.peer_command(
            scenarios[0],
            scenarios[0].caller,
            "caller",
            9_999,
            Path("/payload"),
            run_fec_interop.RunOptions(),
        )
        self.assertNotIn("--minimum-isn", command)
        self.assertNotIn("--isn", command)

    def test_rendezvous_matrix_covers_all_geometries_and_directions(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_fec_interop.rendezvous_scenarios(
            robotweax,
            reference,
        )

        self.assertEqual(len(scenarios), 6)
        self.assertEqual(
            [scenario.profile.geometry for scenario in scenarios],
            ["row", "row", "column", "column", "matrix", "matrix"],
        )
        self.assertEqual(
            [(scenario.sender, scenario.receiver) for scenario in scenarios],
            [
                (robotweax, reference),
                (reference, robotweax),
                (robotweax, reference),
                (reference, robotweax),
                (robotweax, reference),
                (reference, robotweax),
            ],
        )
        self.assertEqual(
            [scenario.start_order for scenario in scenarios],
            [
                "sender-first",
                "receiver-first",
                "sender-first",
                "receiver-first",
                "sender-first",
                "receiver-first",
            ],
        )
        self.assertTrue(
            all(
                "-fec-rendezvous-" in scenario.profile.name
                for scenario in scenarios
            )
        )
        self.assertEqual(scenarios[0].profile.fault_occurrences, (5,))
        self.assertEqual(
            scenarios[4].profile.fault_occurrences,
            (1, 2, 12),
        )

    def test_rendezvous_command_uses_live_fec_profile(self) -> None:
        scenarios = run_fec_interop.rendezvous_scenarios(
            Path("/robotweax"), Path("/haivision")
        )
        scenario = scenarios[0]
        command = run_fec_interop.rendezvous_peer_command(
            scenario,
            scenario.sender,
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/payload"),
            run_fec_interop.RunOptions(),
        )

        self.assertEqual(
            command[command.index("--local-port") + 1], "10001"
        )
        self.assertEqual(command[command.index("--port") + 1], "10002")
        self.assertEqual(
            command[command.index("--transport") + 1], "live"
        )
        self.assertEqual(
            command[command.index("--packet-filter") + 1],
            run_fec_interop.RENDEZVOUS_PACKET_FILTERS["row"],
        )
        self.assertEqual(
            command[command.index("--chunk-size") + 1], "1316"
        )
        self.assertEqual(
            command[command.index("--receive-size") + 1], "1452"
        )
        expected_delay = str(
            run_fec_interop.REFERENCE_RENDEZVOUS_SETTLE_MILLISECONDS
        )
        self.assertEqual(
            command[command.index("--sender-start-delay-ms") + 1],
            expected_delay,
        )

        reverse = run_fec_interop.rendezvous_peer_command(
            scenarios[1],
            scenarios[1].sender,
            "rendezvous-sender",
            10_003,
            10_004,
            Path("/payload"),
            run_fec_interop.RunOptions(),
        )
        self.assertNotIn("--sender-start-delay-ms", reverse)

    def test_rendezvous_role_validation_identifies_robotweax_side(
        self,
    ) -> None:
        scenarios = run_fec_interop.rendezvous_scenarios(
            Path("/robotweax"), Path("/haivision")
        )

        class Relay:
            @staticmethod
            def role_observation() -> dict[str, object]:
                return {
                    "sender_role": "initiator",
                    "receiver_role": "responder",
                    "sender_cookie": 1,
                    "receiver_cookie": 2,
                }

        sender_role, _ = run_fec_interop.validate_rendezvous_role(
            scenarios[0], Relay()
        )
        receiver_role, _ = run_fec_interop.validate_rendezvous_role(
            scenarios[1], Relay()
        )
        self.assertEqual(sender_role, "initiator")
        self.assertEqual(receiver_role, "responder")

    def test_rendezvous_role_probe_closes_random_coverage_gap(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_fec_interop.rendezvous_scenarios(
            robotweax,
            reference,
        )
        observed_roles = ["initiator"] * len(scenarios) + ["responder"]

        with patch.object(
            run_fec_interop,
            "run_rendezvous_scenario",
            side_effect=observed_roles,
        ) as run:
            failures = run_fec_interop.run_rendezvous_matrix(
                scenarios,
                robotweax,
                reference,
                run_fec_interop.RunOptions(),
                Path("/tmp"),
                {},
                role_probe_attempts=2,
            )

        self.assertEqual(failures, [])
        self.assertEqual(run.call_count, len(scenarios) + 1)
        probe = run.call_args_list[-1].args[0]
        self.assertIn("cookie-role-probe-1", probe.profile.name)
        self.assertEqual(probe.profile.geometry, "row-rendezvous-role-probe")

    def test_encrypted_rendezvous_role_probe_stays_encrypted(self) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_fec_interop.encrypted_rendezvous_scenarios(
            robotweax, reference
        )
        observed_roles = ["initiator"] * len(scenarios) + ["responder"]

        with patch.object(
            run_fec_interop,
            "run_rendezvous_scenario",
            side_effect=observed_roles,
        ) as run:
            failures = run_fec_interop.run_rendezvous_matrix(
                scenarios,
                robotweax,
                reference,
                run_fec_interop.RunOptions(),
                Path("/tmp"),
                {},
                role_probe_attempts=2,
                role_probe_factory=(
                    run_fec_interop.encrypted_rendezvous_role_probe_scenario
                ),
                coverage_name="encrypted FEC Rendezvous",
            )

        self.assertEqual(failures, [])
        probe = run.call_args_list[-1].args[0]
        self.assertEqual(probe.profile.key_length, 32)
        self.assertEqual(probe.profile.fault_occurrences, (5,))
        self.assertIn("cookie-role-probe-1", probe.profile.name)

        self.assertFalse(
            scenarios[0].profile.accept_reference_deferred_key_responses
        )
        self.assertTrue(
            scenarios[1].profile.accept_reference_deferred_key_responses
        )
        for attempt in range(4):
            directional_probe = (
                run_fec_interop.encrypted_rendezvous_role_probe_scenario(
                    robotweax, reference, attempt
                )
            )
            profile = directional_probe.profile
            accepts_deferred_response = (
                profile.accept_reference_deferred_key_responses
            )
            self.assertEqual(
                accepts_deferred_response,
                directional_probe.sender == reference,
            )

        with self.assertRaisesRegex(
            ValueError, "invalid FEC interoperability scenario"
        ):
            run_fec_interop.Scenario(
                name="over-broad-reference-exception",
                caller=reference,
                listener=robotweax,
                seed=1,
                geometry="encrypted-matrix",
                packet_filter=run_fec_interop.MATRIX_PACKET_FILTER,
                source_packets_per_group=50,
                control_packets_per_group=15,
                key_length=32,
                minimum_key_transitions=2,
                accept_reference_deferred_key_responses=True,
            )

        wrong_direction_policy = replace(
            scenarios[1].profile,
            accept_reference_deferred_key_responses=False,
        )
        with self.assertRaisesRegex(
            ValueError, "invalid FEC rendezvous scenario"
        ):
            run_fec_interop.RendezvousScenario(
                profile=wrong_direction_policy,
                sender=reference,
                receiver=robotweax,
                robotweax_peer=robotweax,
            )

    def test_rollover_validation_requires_data_ack_and_post_wrap_loss(
        self,
    ) -> None:
        scenario = run_fec_interop.rollover_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[0]
        target = run_fec_interop.ROLLOVER_TARGET_ISN
        source_packets = 50
        wire_packets = (
            source_packets
            + scenario.expected_control_packets(source_packets)
        )
        relay = CallerListenerSequenceProxy(9_999, target)
        relay._source_initial_sequence = 123
        relay._translation_delta = (
            target - relay._source_initial_sequence
        ) & run_fec_interop.SEQUENCE_MASK
        relay._translated_data_packets = wire_packets
        relay._translated_acknowledgements = 2
        relay._first_translated_sequence = target
        relay._last_translated_sequence = (
            target + wire_packets - 1
        ) & run_fec_interop.SEQUENCE_MASK
        relay._data_wrap_observed = True
        relay._ack_wrap_observed = True

        result = run_fec_interop.validate_rollover(
            scenario,
            relay,
            {"isn": 123},
            {"isn": target},
            [{"sequence": 1}],
            source_packets,
        )

        self.assertEqual(result["caller_isn"], 123)
        self.assertEqual(result["listener_isn"], target)
        self.assertEqual(result["wire_packets"], wire_packets)
        self.assertLess(result["end_sequence"], target)

        relay._ack_wrap_observed = False
        with self.assertRaisesRegex(
            RuntimeError, "rollover evidence is incomplete"
        ):
            run_fec_interop.validate_rollover(
                scenario,
                relay,
                {"isn": 123},
                {"isn": target},
                [{"sequence": 1}],
                source_packets,
            )

    def test_recursive_matrix_offsets_are_rollover_safe(self) -> None:
        scenario = run_fec_interop.rollover_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[2]
        target = run_fec_interop.ROLLOVER_TARGET_ISN

        class Relay:
            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [
                    {
                        "action": "drop",
                        "direction": "sender_to_receiver",
                        "occurrence": occurrence,
                        "packet_kind": "data",
                        "filter_control": False,
                        "message_number": index + 1,
                        "sequence": (
                            target + offset
                        ) & run_fec_interop.SEQUENCE_MASK,
                    }
                    for index, (occurrence, offset) in enumerate(
                        zip(
                            scenario.fault_occurrences,
                            scenario.expected_sequence_offsets,
                            strict=True,
                        )
                    )
                ]

        observations = run_fec_interop.validate_causal_recovery(
            scenario, Relay()
        )
        self.assertEqual(
            [item["sequence"] for item in observations],
            [
                target,
                (target + 1) & run_fec_interop.SEQUENCE_MASK,
                (target + 11) & run_fec_interop.SEQUENCE_MASK,
            ],
        )

    def test_sequence_translation_preserves_fec_control_payload(
        self,
    ) -> None:
        target = run_fec_interop.ROLLOVER_TARGET_ISN
        source = 123
        relay = CallerListenerSequenceProxy(9_999, target)
        relay._source_initial_sequence = source
        relay._translation_delta = (
            target - source
        ) & run_fec_interop.SEQUENCE_MASK
        recovery_header = bytes.fromhex("12345678")
        recovery_payload = b"fec-wire-parity"
        packet = (
            source.to_bytes(4, "big")
            + bytes(12)
            + recovery_header
            + recovery_payload
        )

        translated = relay._translated_payload(
            packet, "sender_to_receiver"
        )

        self.assertEqual(
            int.from_bytes(translated[:4], "big"), target
        )
        self.assertEqual(translated[16:], packet[16:])

    def test_filter_statistics_require_parity_and_reconstruction(
        self,
    ) -> None:
        scenario = run_fec_interop.Scenario(
            name="row-fec-test",
            caller=Path("/sender"),
            listener=Path("/receiver"),
            seed=1,
            geometry="row",
            packet_filter=run_fec_interop.ROW_PACKET_FILTER,
            source_packets_per_group=10,
            control_packets_per_group=1,
        )
        sender = {"stats": {"pktSndFilterExtraTotal": 5}}
        receiver = {
            "stats": {
                "pktRcvFilterExtraTotal": 5,
                "pktRcvFilterSupplyTotal": 1,
                "pktRcvFilterLossTotal": 0,
            }
        }

        self.assertEqual(
            run_fec_interop.validate_filter_statistics(
                scenario, sender, receiver, 50
            ),
            (5, 1),
        )
        receiver["stats"]["pktRcvFilterSupplyTotal"] = 0
        with self.assertRaisesRegex(
            RuntimeError, "reported 0 FEC reconstructions"
        ):
            run_fec_interop.validate_filter_statistics(
                scenario, sender, receiver, 50
            )

    def test_no_arq_statistics_require_zero_naks_and_retransmits(
        self,
    ) -> None:
        scenario = run_fec_interop.scenario_matrix(
            Path("/robotweax"), Path("/haivision")
        )[4]
        sender = {
            "stats": {
                "pktRetransTotal": 0,
                "pktRecvNAKTotal": 0,
            }
        }

        self.assertIsNone(
            run_fec_interop.validate_no_arq_statistics(
                scenario, sender
            )
        )
        sender["stats"]["pktRetransTotal"] = 1
        with self.assertRaisesRegex(
            RuntimeError, "FEC recovery used ARQ"
        ):
            run_fec_interop.validate_no_arq_statistics(
                scenario, sender
            )

    def test_column_statistics_require_one_parity_per_column(
        self,
    ) -> None:
        scenario = run_fec_interop.Scenario(
            name="column-fec-test",
            caller=Path("/sender"),
            listener=Path("/receiver"),
            seed=1,
            geometry="column",
            packet_filter=run_fec_interop.COLUMN_PACKET_FILTER,
            source_packets_per_group=50,
            control_packets_per_group=10,
        )
        sender = {"stats": {"pktSndFilterExtraTotal": 10}}
        receiver = {
            "stats": {
                "pktRcvFilterExtraTotal": 10,
                "pktRcvFilterSupplyTotal": 1,
                "pktRcvFilterLossTotal": 0,
            }
        }

        self.assertEqual(
            run_fec_interop.validate_filter_statistics(
                scenario, sender, receiver, 50
            ),
            (10, 1),
        )
        self.assertEqual(scenario.expected_control_packets(49), 0)
        self.assertEqual(scenario.expected_control_packets(50), 10)
        self.assertEqual(scenario.expected_control_packets(99), 10)
        self.assertEqual(scenario.expected_control_packets(100), 20)

    def test_matrix_profile_requires_recursive_recovery_statistics(
        self,
    ) -> None:
        scenario = run_fec_interop.scenario_matrix(
            Path("/robotweax"), Path("/haivision")
        )[4]
        sender = {"stats": {"pktSndFilterExtraTotal": 15}}
        receiver = {
            "stats": {
                "pktRcvFilterExtraTotal": 15,
                "pktRcvFilterSupplyTotal": 3,
                "pktRcvFilterLossTotal": 0,
            }
        }

        self.assertEqual(scenario.fault_occurrences, (1, 2, 12))
        self.assertEqual(
            scenario.expected_sequence_offsets, (0, 1, 11)
        )
        self.assertEqual(scenario.expected_reconstructions, 3)
        self.assertEqual(
            run_fec_interop.validate_filter_statistics(
                scenario, sender, receiver, 50
            ),
            (15, 3),
        )

    def test_matrix_causal_validation_requires_the_recursive_topology(
        self,
    ) -> None:
        scenario = run_fec_interop.scenario_matrix(
            Path("/robotweax"), Path("/haivision")
        )[4]

        class Relay:
            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [
                    {
                        "action": "drop",
                        "direction": "sender_to_receiver",
                        "occurrence": occurrence,
                        "packet_kind": "data",
                        "filter_control": False,
                        "message_number": index + 1,
                        "sequence": 1_000 + offset,
                    }
                    for index, (occurrence, offset) in enumerate(
                        zip(
                            scenario.fault_occurrences,
                            scenario.expected_sequence_offsets,
                            strict=True,
                        )
                    )
                ]

        self.assertEqual(
            [
                observation["sequence"]
                for observation in (
                    run_fec_interop.validate_causal_recovery(
                        scenario, Relay()
                    )
                )
            ],
            [1_000, 1_001, 1_011],
        )

        invalid = Relay.fault_observations()
        invalid[-1]["sequence"] = 1_010

        class InvalidRelay:
            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return invalid

        with self.assertRaisesRegex(
            RuntimeError, "dropped source offsets"
        ):
            run_fec_interop.validate_causal_recovery(
                scenario, InvalidRelay()
            )

    def test_filtercap_trace_requires_filter_extensions(self) -> None:
        events = [
            {
                "event": "srt_handshake_trace",
                "direction": direction,
                "extensions": [{"type": 7, "name": "FILTER"}],
            }
            for direction in (
                "sender_to_receiver",
                "receiver_to_sender",
            )
        ]
        trace = "\n".join(
            json.dumps(event) for event in events
        )

        self.assertEqual(
            run_fec_interop.filtercap_directions(trace),
            {"sender_to_receiver", "receiver_to_sender"},
        )

    def test_encrypted_rotation_requires_key_switches_and_km_exchange(
        self,
    ) -> None:
        scenario = run_fec_interop.encrypted_matrix_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[0]
        observations = [
            {"key_selection": selector}
            for selector in (1, 1, 1, 2, 2, 2, 1, 1, 1)
        ]
        trace = "\n".join(
            json.dumps(
                {
                    "event": "srt_runtime_key_material_trace",
                    "name": name,
                    "direction": direction,
                }
            )
            for name, direction in (
                ("KMREQ", "sender_to_receiver"),
                ("KMRSP", "receiver_to_sender"),
                ("KMREQ", "sender_to_receiver"),
                ("KMRSP", "receiver_to_sender"),
            )
        )
        receiver = {
            "stats": {"pktRcvUndecryptTotal": 0}
        }

        self.assertEqual(
            run_fec_interop.runtime_key_material_names(trace),
            [
                ("KMREQ", "sender_to_receiver"),
                ("KMRSP", "receiver_to_sender"),
                ("KMREQ", "sender_to_receiver"),
                ("KMRSP", "receiver_to_sender"),
            ],
        )
        self.assertEqual(
            run_fec_interop.validate_encrypted_rotation(
                scenario, receiver, observations, trace
            ),
            2,
        )

        for observation in observations[6:]:
            observation["key_selection"] = 2
        with self.assertRaisesRegex(
            RuntimeError, "observed only 1 data-key transition"
        ):
            run_fec_interop.validate_encrypted_rotation(
                scenario, receiver, observations, trace
            )

    def test_caller_listener_rotation_uses_the_complete_data_trace(
        self,
    ) -> None:
        scenario = run_fec_interop.encrypted_matrix_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[0]
        observations = [{"key_selection": 1}]
        trace = "\n".join(
            json.dumps(
                {
                    "event": "srt_runtime_key_material_trace",
                    "name": name,
                    "direction": direction,
                }
            )
            for name, direction in (
                ("KMREQ", "sender_to_receiver"),
                ("KMRSP", "receiver_to_sender"),
                ("KMREQ", "sender_to_receiver"),
                ("KMRSP", "receiver_to_sender"),
            )
        )
        receiver = {"stats": {"pktRcvUndecryptTotal": 0}}
        data_trace = {
            "packets": 150,
            "unencrypted_packets": 0,
            "transitions": 2,
            "selectors": [1, 2, 1],
            "destination_socket_ids": [73],
            "complete": True,
        }

        self.assertEqual(
            run_fec_interop.validate_encrypted_rotation(
                scenario,
                receiver,
                observations,
                trace,
                data_key_observation=data_trace,
            ),
            2,
        )

        data_trace["unencrypted_packets"] = 1
        with self.assertRaisesRegex(
            RuntimeError, "DATA key-selection trace is incomplete"
        ):
            run_fec_interop.validate_encrypted_rotation(
                scenario,
                receiver,
                observations,
                trace,
                data_key_observation=data_trace,
            )

    def test_rendezvous_rotation_requires_causal_transition_trace(
        self,
    ) -> None:
        scenario = run_fec_interop.encrypted_rendezvous_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[0].profile
        transitions = (
            (1, 100, 1),
            (5, 150, 2),
            (9, 200, 1),
        )
        events = [
            {
                "event": "srt_first_data_trace",
                "direction": "sender_to_receiver",
                "relay_ordinal": 1,
                "sequence": 100,
                "destination_socket_id": 7,
                "key_selection": 1,
            }
        ]
        events.extend(
            {
                "event": "srt_data_key_transition_trace",
                "direction": "sender_to_receiver",
                "relay_ordinal": ordinal,
                "sequence": sequence,
                "destination_socket_id": 7,
                "key_selection": selection,
            }
            for ordinal, sequence, selection in transitions
        )
        for ordinal, name, direction, selection, digest in (
            (2, "KMREQ", "sender_to_receiver", 2, "a" * 64),
            (3, "KMRSP", "receiver_to_sender", 2, "a" * 64),
            (6, "KMREQ", "sender_to_receiver", 1, "b" * 64),
            (7, "KMRSP", "receiver_to_sender", 1, "b" * 64),
        ):
            events.append(
                {
                    "event": "srt_runtime_key_material_trace",
                    "name": name,
                    "direction": direction,
                    "relay_ordinal": ordinal,
                    "key_material": {
                        "key_selection": selection,
                        "content_sha256": digest,
                    },
                }
            )
        trace = "\n".join(json.dumps(event) for event in events)
        receiver = {"stats": {"pktRcvUndecryptTotal": 0}}
        observations = [{"key_selection": 1}]

        self.assertEqual(
            run_fec_interop.validate_encrypted_rotation(
                scenario, receiver, observations, trace
            ),
            2,
        )

        missing_response = "\n".join(
            json.dumps(event)
            for event in events
            if not (
                event.get("name") == "KMRSP"
                and event.get("relay_ordinal") == 7
            )
        )
        with self.assertRaisesRegex(RuntimeError, "causal byte-identical"):
            run_fec_interop.validate_encrypted_rotation(
                scenario, receiver, observations, missing_response
            )

    def test_reference_rendezvous_sender_accepts_only_complete_deferred_kmrsp(
        self,
    ) -> None:
        scenarios = run_fec_interop.encrypted_rendezvous_scenarios(
            Path("/robotweax"), Path("/haivision")
        )
        robotweax_sender = scenarios[0].profile
        reference_sender = scenarios[1].profile
        transitions = (
            (1, 100, 1),
            (5, 150, 2),
            (9, 200, 1),
        )
        events: list[dict[str, object]] = [
            {
                "event": "srt_first_data_trace",
                "direction": "sender_to_receiver",
                "relay_ordinal": 1,
                "sequence": 100,
                "destination_socket_id": 7,
                "key_selection": 1,
            }
        ]
        events.extend(
            {
                "event": "srt_data_key_transition_trace",
                "direction": "sender_to_receiver",
                "relay_ordinal": ordinal,
                "sequence": sequence,
                "destination_socket_id": 7,
                "key_selection": selection,
            }
            for ordinal, sequence, selection in transitions
        )
        for ordinal, name, direction, digest in (
            (2, "KMREQ", "sender_to_receiver", "a" * 64),
            (6, "KMREQ", "sender_to_receiver", "b" * 64),
            (12, "KMRSP", "receiver_to_sender", "a" * 64),
            (13, "KMRSP", "receiver_to_sender", "b" * 64),
        ):
            events.append(
                {
                    "event": "srt_runtime_key_material_trace",
                    "name": name,
                    "direction": direction,
                    "relay_ordinal": ordinal,
                    "key_material": {
                        "key_selection": 3,
                        "content_sha256": digest,
                    },
                }
            )
        receiver = {"stats": {"pktRcvUndecryptTotal": 0}}
        observations = [{"key_selection": 1}]

        trace = "\n".join(json.dumps(event) for event in events)
        self.assertEqual(
            run_fec_interop.validate_encrypted_rotation(
                reference_sender, receiver, observations, trace
            ),
            2,
        )
        with self.assertRaisesRegex(RuntimeError, "causal byte-identical"):
            run_fec_interop.validate_encrypted_rotation(
                robotweax_sender, receiver, observations, trace
            )

        missing_response = "\n".join(
            json.dumps(event)
            for event in events
            if not (
                event.get("name") == "KMRSP"
                and event.get("relay_ordinal") == 13
            )
        )
        with self.assertRaisesRegex(RuntimeError, "causal byte-identical"):
            run_fec_interop.validate_encrypted_rotation(
                reference_sender,
                receiver,
                observations,
                missing_response,
            )

        mismatched_response = []
        for event in events:
            copied = json.loads(json.dumps(event))
            if (
                copied.get("name") == "KMRSP"
                and copied.get("relay_ordinal") == 13
            ):
                copied["key_material"]["content_sha256"] = "c" * 64
            mismatched_response.append(copied)
        with self.assertRaisesRegex(RuntimeError, "causal byte-identical"):
            run_fec_interop.validate_encrypted_rotation(
                reference_sender,
                receiver,
                observations,
                "\n".join(
                    json.dumps(event) for event in mismatched_response
                ),
            )

    def test_encrypted_rendezvous_binding_matches_conclusion(self) -> None:
        scenario = run_fec_interop.encrypted_rendezvous_scenarios(
            Path("/robotweax"), Path("/haivision")
        )[0]

        class Relay:
            @staticmethod
            def conclusion_socket_id(direction: str) -> int:
                return 11 if direction == "sender_to_receiver" else 22

        trace = json.dumps(
            {
                "event": "srt_first_data_trace",
                "direction": "sender_to_receiver",
                "sequence": 100,
                "destination_socket_id": 22,
            }
        )
        self.assertEqual(
            run_fec_interop.validate_encrypted_rendezvous_binding(
                scenario, Relay(), trace
            ),
            (11, 22),
        )
        with self.assertRaisesRegex(RuntimeError, "CONCLUSION"):
            run_fec_interop.validate_encrypted_rendezvous_binding(
                scenario,
                Relay(),
                trace.replace('"destination_socket_id": 22',
                              '"destination_socket_id": 23'),
            )

    def test_fault_metadata_distinguishes_source_from_fec_control(
        self,
    ) -> None:
        def packet(message_number: int) -> bytes:
            return (
                (42).to_bytes(4, "big")
                + message_number.to_bytes(4, "big")
                + bytes(12)
            )

        source = CallerListenerFaultProxy._packet_metadata(packet(17))
        control = CallerListenerFaultProxy._packet_metadata(packet(0))

        self.assertEqual(source["message_number"], 17)
        self.assertFalse(source["filter_control"])
        self.assertEqual(control["message_number"], 0)
        self.assertTrue(control["filter_control"])

    def test_fec_control_with_source_sequence_is_not_retransmission(
        self,
    ) -> None:
        fault = RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=1,
        )
        relay = CallerListenerFaultProxy(9_999, fault)
        source = {
            "packet_kind": "data",
            "sequence": 42,
            "message_number": 7,
            "filter_control": False,
            "payload_sha256": "source",
            "relay_monotonic_ns": 1_000,
            "relay_ordinal": 1,
        }
        relay._record_fault(
            0, fault, "sender_to_receiver", source
        )
        relay._match_retransmission(
            "sender_to_receiver",
            {
                "packet_kind": "data",
                "sequence": 42,
                "message_number": 0,
                "filter_control": True,
                "payload_sha256": "fec-control",
                "relay_monotonic_ns": 2_000,
                "relay_ordinal": 2,
            },
        )
        self.assertNotIn(
            "retransmission_observed",
            relay.fault_observations()[0],
        )

        relay._match_retransmission(
            "sender_to_receiver",
            {
                "packet_kind": "data",
                "sequence": 42,
                "message_number": 7,
                "filter_control": False,
                "payload_sha256": "source",
                "relay_monotonic_ns": 3_000,
                "relay_ordinal": 3,
            },
        )
        self.assertTrue(
            relay.fault_observations()[0][
                "retransmission_observed"
            ]
        )

    def test_zero_message_number_can_match_stream_retransmission(
        self,
    ) -> None:
        fault = RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=1,
        )
        relay = CallerListenerFaultProxy(9_999, fault)
        source = {
            "packet_kind": "data",
            "sequence": 84,
            "message_number": 0,
            "filter_control": True,
            "payload_sha256": "stream-source",
            "relay_monotonic_ns": 1_000,
            "relay_ordinal": 1,
        }
        relay._record_fault(
            0, fault, "sender_to_receiver", source
        )
        relay._match_retransmission(
            "sender_to_receiver",
            {
                **source,
                "relay_monotonic_ns": 2_000,
                "relay_ordinal": 2,
            },
        )

        self.assertTrue(
            relay.fault_observations()[0][
                "retransmission_observed"
            ]
        )


if __name__ == "__main__":
    unittest.main()
