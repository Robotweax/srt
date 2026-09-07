from __future__ import annotations

import json
import sys
import unittest
from dataclasses import replace
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_encrypted_file_interop  # noqa: E402
import run_file_interop  # noqa: E402
import srt_handshake_trace  # noqa: E402


class EncryptedFileInteropTests(unittest.TestCase):
    def setUp(self) -> None:
        self.robotweax = Path("/tmp/robotweax-peer")
        self.reference = Path("/tmp/reference-peer")
        self.scenario = run_encrypted_file_interop.scenario_matrix(
            self.robotweax,
            self.reference,
            4 * 1_024 * 1_024 + 379,
            512,
            200,
            3,
        )[0]
        self.rendezvous_scenario = (
            run_encrypted_file_interop.rendezvous_scenario_matrix(
                self.robotweax,
                self.reference,
                512,
                200,
                3,
            )[0]
        )
        self.options = run_file_interop.RunOptions(
            byte_count=4 * 1_024 * 1_024 + 379,
            timeout_seconds=45,
            maximum_bandwidth_bytes_per_second=20_000_000,
            shutdown_grace_milliseconds=250,
        )

    @staticmethod
    def trace(pair_count: int = 3, key_words: int = 4) -> str:
        events: list[str] = []
        for index in range(pair_count):
            identity = {
                "content_length_matches": True,
                "key_words": key_words,
                "key_selection": 1 + index % 2,
                "content_sha256": f"{index + 1:064x}",
            }
            for name, direction in (
                ("KMREQ", "sender_to_receiver"),
                ("KMRSP", "receiver_to_sender"),
            ):
                events.append(
                    json.dumps(
                        {
                            "event": "srt_runtime_key_material_trace",
                            "name": name,
                            "direction": direction,
                            "key_material": identity,
                        }
                    )
                )
        return "\n".join(events)

    @staticmethod
    def observation(**updates: object) -> dict[str, object]:
        result: dict[str, object] = {
            "packets": 2_048,
            "unencrypted_packets": 0,
            "transitions": 3,
            "selectors": [1, 2, 1, 2],
            "destination_socket_ids": [73],
            "complete": True,
        }
        result.update(updates)
        return result

    @staticmethod
    def listener_complete(undecryptable: int = 0) -> dict[str, object]:
        return {
            "stats": {
                "pktRcvUndecryptTotal": undecryptable,
                "pktRcvLossTotal": 0,
            }
        }

    @staticmethod
    def caller_complete(
        sender_loss: int = 0,
        retransmissions: int = 0,
    ) -> dict[str, object]:
        return {
            "stats": {
                "pktSndLossTotal": sender_loss,
                "pktRetransTotal": retransmissions,
            }
        }

    def test_matrix_covers_all_key_sizes_and_both_directions(self) -> None:
        scenarios = run_encrypted_file_interop.scenario_matrix(
            self.robotweax,
            self.reference,
            self.options.byte_count,
            512,
            200,
            3,
        )

        self.assertEqual(len(scenarios), 16)
        self.assertEqual(
            {scenario.key_length for scenario in scenarios},
            {16, 24, 32},
        )
        self.assertEqual(
            {(scenario.caller, scenario.listener) for scenario in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        self.assertTrue(all(
            scenario.minimum_key_transitions == 3
            and scenario.sender_chunk_size > scenario.receiver_size
            for scenario in scenarios
        ))

        helpers = [scenario for scenario in scenarios if scenario.file_api]
        buffers = [scenario for scenario in scenarios if not scenario.file_api]
        self.assertEqual(len(helpers), 6)
        self.assertEqual(len(buffers), 10)
        self.assertEqual(
            {
                (
                    scenario.key_length,
                    scenario.caller,
                    scenario.listener,
                )
                for scenario in helpers
            },
            {
                (key_length, caller, listener)
                for key_length in (16, 24, 32)
                for caller, listener in (
                    (self.robotweax, self.reference),
                    (self.reference, self.robotweax),
                )
            },
        )
        self.assertTrue(all(
            scenario.file_size_to_eof and not scenario.expect_eof
            for scenario in helpers
        ))
        self.assertTrue(all(
            scenario.allow_reference_file_api_statistics_lag
                == (scenario.caller == self.reference)
            for scenario in helpers
        ))
        self.assertTrue(all(scenario.expect_eof for scenario in buffers))
        self.assertTrue(all(
            not scenario.allow_reference_file_api_statistics_lag
            for scenario in buffers
        ))

        faults = [scenario for scenario in scenarios if scenario.fault]
        self.assertEqual(len(faults), 4)
        self.assertEqual(
            {scenario.recovery for scenario in faults},
            {"NAK", "RTO/LATEREXMIT"},
        )
        self.assertTrue(
            all(
                scenario.key_length == 32
                and scenario.fault is not None
                and scenario.fault.direction == "sender_to_receiver"
                for scenario in faults
            )
        )
        expected_tail = (
            self.options.byte_count
            + run_file_interop.FILE_PAYLOAD_SIZE
            - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        tail_faults = [
            scenario
            for scenario in faults
            if scenario.recovery == "RTO/LATEREXMIT"
        ]
        self.assertTrue(
            all(
                scenario.sender_chunk_size
                    == run_file_interop.FILE_PAYLOAD_SIZE
                and scenario.fault is not None
                and scenario.fault.occurrence == expected_tail
                for scenario in tail_faults
            )
        )

    def test_caller_listener_matrix_has_independent_base_and_fault_sets(
        self,
    ) -> None:
        arguments = (
            self.robotweax,
            self.reference,
            self.options.byte_count,
            512,
            200,
            3,
        )
        base = run_encrypted_file_interop.base_scenario_matrix(*arguments)
        faults = (
            run_encrypted_file_interop
            .caller_listener_fault_scenario_matrix(*arguments)
        )

        self.assertEqual(len(base), 12)
        self.assertEqual(len(faults), 4)
        self.assertTrue(all(scenario.fault is None for scenario in base))
        self.assertTrue(all(scenario.fault is not None for scenario in faults))
        self.assertEqual(
            run_encrypted_file_interop.scenario_matrix(*arguments),
            base + faults,
        )

    def test_profiles_select_disjoint_execution_sections(self) -> None:
        self.assertEqual(
            run_encrypted_file_interop.profile_sections("all"),
            ("base", "faults", "rollover", "rendezvous"),
        )
        for profile in ("base", "rendezvous", "faults", "rollover"):
            with self.subTest(profile=profile):
                self.assertEqual(
                    run_encrypted_file_interop.profile_sections(profile),
                    (profile,),
                )
        with self.assertRaisesRegex(ValueError, "unknown"):
            run_encrypted_file_interop.profile_sections("future")

    def test_peer_commands_select_file_crypto_rotation_and_eof(self) -> None:
        listener = run_file_interop.peer_command(
            self.reference,
            "listener",
            9_001,
            Path("/tmp/output"),
            self.scenario,
            self.options,
        )
        caller = run_file_interop.peer_command(
            self.robotweax,
            "caller",
            9_001,
            Path("/tmp/input"),
            self.scenario,
            self.options,
        )

        for command in (listener, caller):
            self.assertIn("file", command)
            self.assertIn("--passphrase-env", command)
            self.assertIn(run_file_interop.PASSPHRASE_ENVIRONMENT, command)
            self.assertIn("--pbkeylen", command)
            self.assertIn("16", command)
            self.assertIn("--km-refresh-rate", command)
            self.assertIn("512", command)
            self.assertIn("--km-preannounce", command)
            self.assertIn("200", command)
        self.assertIn("--expect-eof", listener)
        self.assertNotIn("--expect-eof", caller)

    def test_encrypted_file_helper_commands_use_public_api(self) -> None:
        scenario = next(
            item
            for item in run_encrypted_file_interop.scenario_matrix(
                self.robotweax,
                self.reference,
                self.options.byte_count,
                512,
                200,
                3,
            )
            if item.file_api
        )
        caller = run_file_interop.peer_command(
            scenario.caller,
            "caller",
            9_001,
            Path("/tmp/input"),
            scenario,
            self.options,
        )
        listener = run_file_interop.peer_command(
            scenario.listener,
            "listener",
            9_001,
            Path("/tmp/output"),
            scenario,
            self.options,
        )

        for command in (caller, listener):
            self.assertIn("--file-api", command)
            self.assertIn("--passphrase-env", command)
            self.assertIn("--pbkeylen", command)
            self.assertIn("--km-refresh-rate", command)
            self.assertIn("--km-preannounce", command)
            self.assertNotIn("--expect-eof", command)
        self.assertIn("--file-size-to-eof", caller)
        self.assertNotIn("--file-size-to-eof", listener)
        self.assertEqual(
            caller[caller.index("--chunk-size") + 1],
            str(scenario.sender_chunk_size),
        )
        self.assertEqual(
            listener[listener.index("--chunk-size") + 1],
            str(scenario.receiver_size),
        )
        self.assertEqual(
            listener[listener.index("--shutdown-grace-ms") + 1],
            str(
                run_file_interop
                .FILE_API_RECEIVER_SHUTDOWN_GRACE_MILLISECONDS
            ),
        )

    def test_wrong_passphrase_matrix_is_bidirectional_and_bounded(
        self,
    ) -> None:
        scenarios = (
            run_encrypted_file_interop
            .passphrase_mismatch_scenario_matrix(
                self.robotweax, self.reference
            )
        )

        self.assertEqual(len(scenarios), 2)
        self.assertEqual(
            {(scenario.caller, scenario.listener) for scenario in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        self.assertTrue(all(
            scenario.byte_count
                == run_encrypted_file_interop.PASSPHRASE_MISMATCH_BYTES
            and scenario.key_length == 32
            and scenario.file_api
            and scenario.file_size_to_eof
            and scenario.fault is None
            and not scenario.allow_reference_file_api_statistics_lag
            for scenario in scenarios
        ))

    def test_wrong_passphrase_rejection_is_fail_closed(self) -> None:
        scenario = (
            run_encrypted_file_interop
            .passphrase_mismatch_scenario_matrix(
                self.robotweax, self.reference
            )[0]
        )

        class Relay:
            @staticmethod
            def data_key_observation(
                direction: str,
            ) -> dict[str, object]:
                self.assertEqual(direction, "sender_to_receiver")
                return {
                    "packets": 0,
                    "unencrypted_packets": 0,
                    "transitions": 0,
                    "selectors": [],
                    "destination_socket_ids": [],
                    "complete": True,
                }

            @staticmethod
            def error() -> OSError | None:
                return None

        relay = Relay()
        rejection = run_encrypted_file_interop.PASSPHRASE_REJECTION_LINE
        run_encrypted_file_interop.validate_passphrase_mismatch(
            scenario,
            3,
            "",
            rejection + "\n",
            True,
            False,
            0,
            relay,
            False,
        )
        reference_stderr = (
            "12:00:00/reference!W:SRT.cn: ERROR:BADSECRET\n"
            + rejection
            + "\n"
        )
        run_encrypted_file_interop.validate_passphrase_mismatch(
            scenario,
            3,
            "",
            reference_stderr,
            True,
            False,
            0,
            relay,
            True,
        )

        mutations = (
            {"caller_returncode": 0},
            {"caller_stdout": "unexpected\n"},
            {"caller_stderr": rejection.replace("10", "9") + "\n"},
            {"caller_stderr": rejection + "\n" + rejection + "\n"},
            {"listener_was_waiting": False},
            {"output_exists": True},
            {"output_bytes": 1},
        )
        common = {
            "scenario": scenario,
            "caller_returncode": 3,
            "caller_stdout": "",
            "caller_stderr": rejection + "\n",
            "listener_was_waiting": True,
            "output_exists": False,
            "output_bytes": 0,
            "relay": relay,
            "caller_is_reference": False,
        }
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(
                RuntimeError
            ):
                run_encrypted_file_interop.validate_passphrase_mismatch(
                    **{**common, **mutation}
                )

        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            run_encrypted_file_interop.validate_passphrase_mismatch(
                **{
                    **common,
                    "caller_stderr": rejection + "\n",
                    "caller_is_reference": True,
                }
            )

        class DataRelay(Relay):
            @staticmethod
            def data_key_observation(
                direction: str,
            ) -> dict[str, object]:
                observation = Relay.data_key_observation(direction)
                observation["packets"] = 1
                return observation

        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            run_encrypted_file_interop.validate_passphrase_mismatch(
                **{**common, "relay": DataRelay()}
            )

    def test_reference_file_helper_statistics_lag_is_version_scoped(
        self,
    ) -> None:
        scenario = next(
            item
            for item in run_encrypted_file_interop.scenario_matrix(
                self.robotweax,
                self.reference,
                self.options.byte_count,
                512,
                200,
                3,
            )
            if item.file_api and item.caller == self.reference
        )
        byte_count = 745_851
        sender = {
            "srt_version": 0x010505,
            "stats": {
                "pktSentTotal": 544,
                "pktSentUniqueTotal": 544,
                "pktRetransTotal": 0,
                "byteSentTotal": 766_225,
                "byteSentUniqueTotal": 766_225,
                "byteRetransTotal": 0,
            },
        }
        receiver = {
            "stats": {
                "pktRecvTotal": 547,
                "pktRecvUniqueTotal": 547,
                "byteRecvTotal": 769_919,
                "byteRecvUniqueTotal": 769_919,
            }
        }

        for accepted_version in (0x010505, 0x010507):
            with self.subTest(accepted_version=accepted_version):
                self.assertEqual(
                    run_file_interop.validate_wire_statistics(
                        scenario,
                        {**sender, "srt_version": accepted_version},
                        receiver,
                        byte_count,
                    ),
                    {"packets": 3, "payload_bytes": 3_562},
                )

        for invalid in (
            replace(scenario, allow_reference_file_api_statistics_lag=False),
            scenario,
        ):
            invalid_sender = {
                **sender,
                "srt_version": (
                    0x010506
                    if invalid is scenario
                    else sender["srt_version"]
                ),
            }
            with self.assertRaises(RuntimeError):
                run_file_interop.validate_wire_statistics(
                    invalid, invalid_sender, receiver, byte_count
                )

        excessive_packet_lag = {
            **sender,
            "stats": {
                **sender["stats"],
                "pktSentTotal": 543,
                "pktSentUniqueTotal": 543,
            },
        }
        with self.assertRaisesRegex(RuntimeError, "wire bytes"):
            run_file_interop.validate_wire_statistics(
                scenario, excessive_packet_lag, receiver, byte_count
            )

        excessive_payload_lag = {
            **sender,
            "stats": {
                **sender["stats"],
                "byteSentTotal": 760_000,
                "byteSentUniqueTotal": 760_000,
            },
        }
        with self.assertRaisesRegex(RuntimeError, "wire bytes"):
            run_file_interop.validate_wire_statistics(
                scenario, excessive_payload_lag, receiver, byte_count
            )

    def test_reference_file_helper_statistics_profile_is_fail_closed(
        self,
    ) -> None:
        common = {
            "name": "invalid-reference-statistics-profile",
            "caller": self.reference,
            "listener": self.robotweax,
            "seed": 1,
            "sender_chunk_size": 1_456,
            "receiver_size": 997,
            "allow_reference_file_api_statistics_lag": True,
        }
        for invalid in (
            common,
            {**common, "file_api": True, "file_size_to_eof": True},
            {
                **common,
                "file_api": True,
                "file_size_to_eof": True,
                "key_length": 16,
                "fault": srt_handshake_trace.RendezvousFault(
                    action="drop",
                    direction="sender_to_receiver",
                    occurrence=1,
                ),
            },
        ):
            with self.assertRaises(ValueError):
                run_file_interop.Scenario(**invalid)

    def test_rendezvous_matrix_covers_all_keys_and_directions(self) -> None:
        scenarios = (
            run_encrypted_file_interop.rendezvous_scenario_matrix(
                self.robotweax,
                self.reference,
                512,
                200,
                3,
            )
        )

        self.assertEqual(len(scenarios), 6)
        self.assertEqual(
            {scenario.key_length for scenario in scenarios},
            {16, 24, 32},
        )
        self.assertEqual(
            {(scenario.sender, scenario.receiver) for scenario in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        self.assertTrue(
            all(
                scenario.robotweax_peer == self.robotweax
                and scenario.minimum_key_transitions == 3
                and scenario.maximum_bandwidth_bytes_per_second
                    == run_encrypted_file_interop
                    .RENDEZVOUS_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND
                and scenario.expect_eof
                and scenario.fault is None
                and scenario.sender_chunk_size > scenario.receiver_size
                and scenario.maximum_bandwidth_bytes_per_second
                    == run_encrypted_file_interop
                    .RENDEZVOUS_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND
                for scenario in scenarios
            )
        )
        expected_bytes = (
            512 * 3 + 128
        ) * run_file_interop.FILE_PAYLOAD_SIZE + 379
        self.assertEqual(
            {scenario.byte_count for scenario in scenarios},
            {expected_bytes},
        )

    def test_rendezvous_fault_matrix_combines_rotation_and_recovery(
        self,
    ) -> None:
        scenarios = (
            run_encrypted_file_interop.rendezvous_fault_scenario_matrix(
                self.robotweax,
                self.reference,
                512,
                200,
                3,
            )
        )

        self.assertEqual(len(scenarios), 8)
        self.assertEqual(
            {(scenario.sender, scenario.receiver) for scenario in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        expected_bytes = (
            512 * 3 + 128
        ) * run_file_interop.FILE_PAYLOAD_SIZE + 379
        expected_tail = (
            expected_bytes + run_file_interop.FILE_PAYLOAD_SIZE - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        self.assertTrue(
            all(
                scenario.robotweax_peer == self.robotweax
                and scenario.key_length == 32
                and scenario.key_refresh_rate == 512
                and scenario.key_preannouncement == 200
                and scenario.minimum_key_transitions == 3
                and scenario.byte_count == expected_bytes
                and scenario.expect_eof
                and scenario.fault is not None
                and all(
                    fault.action == "drop"
                    and fault.direction == "sender_to_receiver"
                    for fault in run_file_interop.scenario_faults(scenario)
                )
                and scenario.maximum_bandwidth_bytes_per_second
                    == run_encrypted_file_interop
                    .RENDEZVOUS_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND
                for scenario in scenarios
            )
        )
        middle = [
            scenario
            for scenario in scenarios
            if scenario.name.endswith("rotation-loss-recovery")
        ]
        tails = [
            scenario
            for scenario in scenarios
            if scenario.recovery == "RTO/LATEREXMIT"
        ]
        bursts = [
            scenario
            for scenario in scenarios
            if scenario.name.endswith("rotation-burst-loss")
        ]
        sustained = [
            scenario
            for scenario in scenarios
            if scenario.name.endswith("rotation-sustained-loss")
        ]
        self.assertEqual(
            {(scenario.sender, scenario.recovery) for scenario in middle},
            {
                (self.robotweax, "NAK"),
                (self.reference, "CAUSAL"),
            },
        )
        self.assertTrue(all(
            scenario.fault is not None
            and scenario.fault.occurrence == 2 * 512 + 17
            for scenario in middle
        ))
        self.assertTrue(all(
            scenario.sender_chunk_size
                == run_file_interop.FILE_PAYLOAD_SIZE
            and scenario.fault is not None
            and scenario.fault.occurrence == expected_tail
            for scenario in tails
        ))
        self.assertEqual(len(bursts), 2)
        self.assertEqual(len(sustained), 2)
        self.assertEqual(
            {
                tuple(
                    fault.occurrence
                    for fault in run_file_interop.scenario_faults(scenario)
                )
                for scenario in bursts
            },
            {(2 * 512 + 31, 2 * 512 + 32, 2 * 512 + 33)},
        )
        self.assertEqual(
            {
                tuple(
                    fault.occurrence
                    for fault in run_file_interop.scenario_faults(scenario)
                )
                for scenario in sustained
            },
            {
                tuple(
                    512 // 2 + (512 // 4) * index
                    for index in range(9)
                )
            },
        )
        self.assertTrue(all(
            scenario.minimum_fault_key_selectors == 1
            for scenario in bursts
        ))
        self.assertTrue(all(
            scenario.minimum_fault_key_selectors == 2
            for scenario in sustained
        ))
        self.assertEqual(
            {(scenario.sender, scenario.recovery) for scenario in bursts},
            {
                (self.robotweax, "NAK"),
                (self.reference, "CAUSAL"),
            },
        )
        self.assertEqual(
            {(scenario.sender, scenario.recovery) for scenario in sustained},
            {
                (self.robotweax, "NAK"),
                (self.reference, "CAUSAL"),
            },
        )

    def test_encrypted_fault_key_selector_evidence_is_fail_closed(
        self,
    ) -> None:
        scenario = next(
            item
            for item in (
                run_encrypted_file_interop
                .rendezvous_fault_scenario_matrix(
                    self.robotweax,
                    self.reference,
                    512,
                    200,
                    3,
                )
            )
            if item.name.endswith("rotation-sustained-loss")
        )
        observations = [
            {
                "action": "drop",
                "key_selection": 1 if index < 4 else 2,
            }
            for index in range(9)
        ]
        run_file_interop.validate_encrypted_fault_key_selectors(
            scenario, observations
        )

        mutations = {
            "missing observation": observations[:-1],
            "missing selector": [
                *observations[:-1],
                {"action": "drop"},
            ],
            "invalid selector": [
                *observations[:-1],
                {"action": "drop", "key_selection": 0},
            ],
            "non-scalar selector": [
                *observations[:-1],
                {"action": "drop", "key_selection": [1]},
            ],
            "single selector": [
                {"action": "drop", "key_selection": 1}
                for _ in observations
            ],
        }
        for name, mutation in mutations.items():
            with self.subTest(name=name), self.assertRaisesRegex(
                RuntimeError, "key-selector evidence is incomplete"
            ):
                run_file_interop.validate_encrypted_fault_key_selectors(
                    scenario, mutation
                )

    def test_encrypted_rollover_matrix_uses_authentic_sender_sequences(
        self,
    ) -> None:
        scenarios = (
            run_encrypted_file_interop.rollover_scenario_matrix(
                self.robotweax,
                self.reference,
                512,
                200,
                3,
            )
        )

        self.assertEqual(len(scenarios), 2)
        self.assertEqual(
            [scenario.recovery for scenario in scenarios],
            ["NAK", "RTO/LATEREXMIT"],
        )
        self.assertTrue(
            all(
                scenario.caller == self.robotweax
                and scenario.listener == self.reference
                and scenario.rollover
                and scenario.minimum_initial_sequence
                    == run_file_interop.ROLLOVER_MINIMUM_ISN
                and scenario.translated_initial_sequence is None
                and scenario.key_length == 32
                and scenario.key_refresh_rate == 512
                and scenario.key_preannouncement == 200
                and scenario.minimum_key_transitions == 3
                and scenario.maximum_bandwidth_bytes_per_second
                    == run_encrypted_file_interop
                    .ROLLOVER_TRACE_MAXIMUM_BANDWIDTH_BYTES_PER_SECOND
                and scenario.expect_eof
                for scenario in scenarios
            )
        )
        self.assertEqual(
            [scenario.fault.occurrence for scenario in scenarios],
            [
                run_file_interop.ROLLOVER_NAK_OCCURRENCE,
                run_file_interop.ROLLOVER_PACKET_COUNT,
            ],
        )
        self.assertTrue(
            all(
                scenario.name.startswith("filecc-aes256-rollover-")
                for scenario in scenarios
            )
        )

    def test_encrypted_rollover_command_selects_isn_and_crypto(self) -> None:
        scenario = (
            run_encrypted_file_interop.rollover_scenario_matrix(
                self.robotweax,
                self.reference,
                512,
                200,
                3,
            )[0]
        )
        command = run_file_interop.peer_command(
            self.robotweax,
            "caller",
            9_001,
            Path("/tmp/input"),
            scenario,
            self.options,
        )

        self.assertEqual(
            command[command.index("--minimum-isn") + 1],
            str(run_file_interop.ROLLOVER_MINIMUM_ISN),
        )
        self.assertEqual(command[command.index("--pbkeylen") + 1], "32")
        self.assertEqual(
            command[command.index("--km-refresh-rate") + 1],
            "512",
        )
        self.assertEqual(
            command[command.index("--km-preannounce") + 1],
            "200",
        )

    def test_encrypted_rendezvous_role_probe_is_bounded_and_complete(
        self,
    ) -> None:
        probes = [
            run_encrypted_file_interop.rendezvous_role_probe(
                self.robotweax,
                self.reference,
                attempt,
                100,
                500_000,
            )
            for attempt in range(4)
        ]

        self.assertEqual(
            {(probe.sender, probe.receiver) for probe in probes},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        self.assertEqual(
            {probe.start_order for probe in probes},
            {"sender-first", "receiver-first"},
        )
        self.assertTrue(
            all(
                probe.key_length == 32
                and probe.key_refresh_rate == 64
                and probe.key_preannouncement == 20
                and probe.minimum_key_transitions == 1
                and probe.expect_eof
                and probe.byte_count
                    == 256 * run_file_interop.FILE_PAYLOAD_SIZE + 379
                and probe.maximum_bandwidth_bytes_per_second == 500_000
                for probe in probes
            )
        )

    def test_rendezvous_commands_select_file_crypto_rotation_and_eof(
        self,
    ) -> None:
        sender = run_file_interop.rendezvous_peer_command(
            self.robotweax,
            "rendezvous-sender",
            9_001,
            9_002,
            Path("/tmp/input"),
            self.rendezvous_scenario,
            self.options,
        )
        receiver = run_file_interop.rendezvous_peer_command(
            self.reference,
            "rendezvous-receiver",
            9_002,
            9_001,
            Path("/tmp/output"),
            self.rendezvous_scenario,
            self.options,
        )

        for command in (sender, receiver):
            self.assertEqual(command[command.index("--transport") + 1], "file")
            self.assertEqual(command[command.index("--congestion") + 1], "file")
            self.assertEqual(
                command[command.index("--passphrase-env") + 1],
                run_file_interop.PASSPHRASE_ENVIRONMENT,
            )
            self.assertEqual(command[command.index("--pbkeylen") + 1], "16")
            self.assertEqual(
                command[command.index("--km-refresh-rate") + 1],
                "512",
            )
            self.assertEqual(
                command[command.index("--km-preannounce") + 1],
                "200",
            )
        self.assertNotIn("--expect-eof", sender)
        self.assertIn("--expect-eof", receiver)

    def test_data_key_observation_is_unique_and_transition_aware(self) -> None:
        relay = srt_handshake_trace.CallerListenerFaultProxy(9_001)
        for sequence, selector in ((10, 1), (11, 1), (12, 2), (13, 1)):
            relay._record_data_key_selection(
                "sender_to_receiver",
                {
                    "packet_kind": "data",
                    "sequence": sequence,
                    "message_number": 1,
                    "key_selection": selector,
                    "destination_socket_id": 73,
                },
            )
        relay._record_data_key_selection(
            "sender_to_receiver",
            {
                "packet_kind": "data",
                "sequence": 10,
                "message_number": 1,
                "key_selection": 1,
                "destination_socket_id": 73,
            },
        )
        relay._record_data_key_selection(
            "sender_to_receiver",
            {
                "packet_kind": "data",
                "sequence": 14,
                "message_number": 0,
                "filter_control": True,
                "key_selection": 0,
                "destination_socket_id": 73,
            },
        )

        self.assertEqual(
            relay.data_key_observation("sender_to_receiver"),
            {
                "packets": 4,
                "unencrypted_packets": 0,
                "transitions": 2,
                "selectors": [1, 2, 1],
                "destination_socket_ids": [73],
                "complete": True,
            },
        )

    def test_data_key_observation_rejects_mutated_retransmission(self) -> None:
        relay = srt_handshake_trace.CallerListenerFaultProxy(9_001)
        original = {
            "packet_kind": "data",
            "sequence": 10,
            "message_number": 1,
            "key_selection": 1,
            "destination_socket_id": 73,
        }
        relay._record_data_key_selection("sender_to_receiver", original)
        relay._record_data_key_selection(
            "sender_to_receiver",
            {**original, "key_selection": 2},
        )

        self.assertFalse(
            relay.data_key_observation("sender_to_receiver")["complete"]
        )

    def test_data_key_observation_retains_bounded_rollover_trace(
        self,
    ) -> None:
        relay = srt_handshake_trace.CallerListenerFaultProxy(9_001)
        for sequence in range(
            srt_handshake_trace.MAX_DATA_KEY_IDENTITIES
        ):
            relay._record_data_key_selection(
                "sender_to_receiver",
                {
                    "packet_kind": "data",
                    "sequence": sequence,
                    "message_number": 1,
                    "key_selection": 1,
                    "destination_socket_id": 73,
                },
            )

        observation = relay.data_key_observation("sender_to_receiver")
        self.assertTrue(observation["complete"])
        self.assertEqual(
            observation["packets"],
            srt_handshake_trace.MAX_DATA_KEY_IDENTITIES,
        )
        relay._record_data_key_selection(
            "sender_to_receiver",
            {
                "packet_kind": "data",
                "sequence": srt_handshake_trace.MAX_DATA_KEY_IDENTITIES,
                "message_number": 1,
                "key_selection": 1,
                "destination_socket_id": 73,
            },
        )
        self.assertFalse(
            relay.data_key_observation("sender_to_receiver")["complete"]
        )

    def test_data_key_observation_rejects_malformed_data(self) -> None:
        relay = srt_handshake_trace.CallerListenerFaultProxy(9_001)
        relay._record_data_key_selection(
            "sender_to_receiver",
            {
                "packet_kind": "data",
                "sequence": 10,
                "message_number": 1,
                "key_selection": None,
                "destination_socket_id": 73,
            },
        )

        self.assertFalse(
            relay.data_key_observation("sender_to_receiver")["complete"]
        )

    def test_encrypted_validator_requires_complete_causal_evidence(self) -> None:
        class Relay:
            def data_key_observation(self, direction: str) -> dict[str, object]:
                self.direction = direction
                return EncryptedFileInteropTests.observation()

        relay = Relay()
        result = run_file_interop.validate_encrypted_stream(
            self.scenario,
            self.caller_complete(),
            self.listener_complete(),
            relay,
            self.trace(),
        )

        self.assertEqual(relay.direction, "sender_to_receiver")
        self.assertEqual(result["transitions"], 3)
        self.assertEqual(result["acknowledged_key_material"], 3)

    def test_encrypted_validator_checks_independent_reverse_confirmation(self) -> None:
        class Relay:
            @staticmethod
            def data_key_observation(direction: str) -> dict[str, object]:
                return EncryptedFileInteropTests.observation()

        reverse = [json.loads(line) for line in self.trace(1).splitlines()]
        for event in reverse:
            event["direction"] = (
                "receiver_to_sender" if event["name"] == "KMREQ"
                else "sender_to_receiver"
            )
            event["key_material"]["content_sha256"] = "a" * 64

        def validate(events: list[dict[str, object]], forward: str | None = None):
            return run_file_interop.validate_encrypted_stream(
                self.scenario, self.caller_complete(), self.listener_complete(),
                Relay(), "\n".join(json.dumps(event) for event in events)
                + "\n" + (self.trace() if forward is None else forward),
            )

        result = validate(reverse)
        self.assertEqual(result["acknowledged_key_material"], 3)
        self.assertEqual(result["transitions"], 3)
        with self.assertRaisesRegex(RuntimeError, "acknowledged key-material"):
            validate(reverse, self.trace(2))
        for mutation in (reverse[:1], reverse[1:], list(reversed(reverse))):
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                RuntimeError, "invalid initial reverse"
            ):
                validate(mutation)
        shared = json.loads(json.dumps(reverse))
        for event in shared:
            event["key_material"]["content_sha256"] = f"{1:064x}"
        with self.assertRaisesRegex(RuntimeError, "invalid initial reverse"):
            validate(shared)
        rotated = json.loads(json.dumps(reverse))
        for event in rotated:
            event["key_material"]["key_selection"] = 3
        with self.assertRaisesRegex(RuntimeError, "invalid initial reverse"):
            validate(rotated)
        second = json.loads(json.dumps(reverse))
        for event in second:
            event["key_material"]["content_sha256"] = "b" * 64
        with self.assertRaisesRegex(RuntimeError, "invalid initial reverse"):
            validate(reverse + second)

    def test_encrypted_validator_rejects_incomplete_data_evidence(self) -> None:
        for mutation in (
            {"complete": False},
            {"packets": 1_535},
            {"unencrypted_packets": 1},
            {"transitions": 2, "selectors": [1, 2, 1]},
            {"selectors": [1, 2, 0, 2]},
            {"destination_socket_ids": [73, 74]},
        ):
            class Relay:
                def data_key_observation(
                    self, direction: str
                ) -> dict[str, object]:
                    return EncryptedFileInteropTests.observation(**mutation)

            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                RuntimeError, "DATA-key evidence is incomplete"
            ):
                run_file_interop.validate_encrypted_stream(
                    self.scenario,
                    self.caller_complete(),
                    self.listener_complete(),
                    Relay(),
                    self.trace(),
                )

    def test_encrypted_validator_rejects_bad_crypto_evidence(self) -> None:
        class Relay:
            @staticmethod
            def data_key_observation(direction: str) -> dict[str, object]:
                return EncryptedFileInteropTests.observation()

        with self.assertRaisesRegex(RuntimeError, "undecryptable"):
            run_file_interop.validate_encrypted_stream(
                self.scenario,
                self.caller_complete(),
                self.listener_complete(1),
                Relay(),
                self.trace(),
            )
        with self.assertRaisesRegex(RuntimeError, "acknowledged key-material"):
            run_file_interop.validate_encrypted_stream(
                self.scenario,
                self.caller_complete(),
                self.listener_complete(),
                Relay(),
                "\n".join(self.trace().splitlines()[:-1]),
            )
        malformed = self.trace().replace('"key_words": 4', '"key_words": 8')
        with self.assertRaisesRegex(RuntimeError, "malformed runtime key"):
            run_file_interop.validate_encrypted_stream(
                self.scenario,
                self.caller_complete(),
                self.listener_complete(),
                Relay(),
                malformed,
            )
        with self.assertRaisesRegex(RuntimeError, "trace is malformed"):
            run_file_interop.validate_encrypted_stream(
                self.scenario,
                self.caller_complete(),
                self.listener_complete(),
                Relay(),
                self.trace() + "\nnot-json",
            )
        with self.assertRaisesRegex(RuntimeError, "trace is incomplete"):
            run_file_interop.validate_encrypted_stream(
                self.scenario,
                self.caller_complete(),
                self.listener_complete(),
                Relay(),
                self.trace()
                + "\n"
                + json.dumps(
                    {
                        "event": "srt_arq_trace_truncated",
                        "omitted": 1,
                    }
                ),
            )

    def test_encrypted_validator_rejects_uncontrolled_loss(self) -> None:
        class Relay:
            @staticmethod
            def data_key_observation(direction: str) -> dict[str, object]:
                return EncryptedFileInteropTests.observation()

        for caller, listener in (
            (self.caller_complete(sender_loss=1), self.listener_complete()),
            (self.caller_complete(retransmissions=1), self.listener_complete()),
            (
                self.caller_complete(),
                {
                    "stats": {
                        "pktRcvUndecryptTotal": 0,
                        "pktRcvLossTotal": 1,
                    }
                },
            ),
        ):
            with (
                self.subTest(caller=caller, listener=listener),
                self.assertRaisesRegex(RuntimeError, "uncontrolled"),
            ):
                run_file_interop.validate_encrypted_stream(
                    self.scenario,
                    caller,
                    listener,
                    Relay(),
                    self.trace(),
                )

    def test_encrypted_fault_validator_requires_exact_retransmission(
        self,
    ) -> None:
        scenario = next(
            item
            for item in run_encrypted_file_interop.scenario_matrix(
                self.robotweax,
                self.reference,
                self.options.byte_count,
                512,
                200,
                3,
            )
            if item.recovery == "NAK"
        )

        class Relay:
            @staticmethod
            def data_key_observation(direction: str) -> dict[str, object]:
                return EncryptedFileInteropTests.observation()

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [{"action": "drop", "key_selection": 1}]

        run_file_interop.validate_encrypted_stream(
            scenario,
            self.caller_complete(sender_loss=1, retransmissions=1),
            {
                "stats": {
                    "pktRcvUndecryptTotal": 0,
                    "pktRcvLossTotal": 1,
                }
            },
            Relay(),
            self.trace(key_words=8),
        )
        for retransmissions in (0, 2):
            with (
                self.subTest(retransmissions=retransmissions),
                self.assertRaisesRegex(RuntimeError, "unplanned"),
            ):
                run_file_interop.validate_encrypted_stream(
                    scenario,
                    self.caller_complete(
                        sender_loss=1,
                        retransmissions=retransmissions,
                    ),
                    {
                        "stats": {
                            "pktRcvUndecryptTotal": 0,
                            "pktRcvLossTotal": 1,
                        }
                    },
                    Relay(),
                    self.trace(key_words=8),
                )

    def test_encrypted_rendezvous_initial_recovery_is_fail_closed(
        self,
    ) -> None:
        scenario = next(
            item
            for item in (
                run_encrypted_file_interop
                .rendezvous_fault_scenario_matrix(
                    self.robotweax,
                    self.reference,
                    512,
                    200,
                    3,
                )
            )
            if item.sender == self.robotweax
            and item.recovery == "NAK"
        )
        initial_sequence = 100
        fault_sequence = 200

        class Relay:
            mutation = "valid"

            @staticmethod
            def data_key_observation(direction: str) -> dict[str, object]:
                return EncryptedFileInteropTests.observation()

            @classmethod
            def arq_trace_complete(cls) -> bool:
                return cls.mutation != "incomplete"

            @classmethod
            def role_observation(cls) -> dict[str, object]:
                if cls.mutation == "role":
                    return {
                        "sender_role": "initiator",
                        "receiver_role": "initiator",
                    }
                return {
                    "sender_role": (
                        "responder"
                        if cls.mutation == "complementary-role"
                        else "initiator"
                    ),
                    "receiver_role": (
                        "initiator"
                        if cls.mutation == "complementary-role"
                        else "responder"
                    ),
                }

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [
                    {
                        "action": "drop",
                        "key_selection": 1,
                        "sequence": fault_sequence,
                        "message_number": 2,
                    }
                ]

            @classmethod
            def retransmission_observations(
                cls, direction: str
            ) -> list[dict[str, object]]:
                common = {
                    "direction": direction,
                    "key_selection": 1,
                    "original_key_selection": 1,
                    "payload_bytes": 1_456,
                    "original_payload_bytes": 1_456,
                    "payload_sha256": "a" * 64,
                    "original_payload_sha256": "a" * 64,
                    "original_observed": True,
                    "ciphertext_matches_original": True,
                }
                events = [
                    {
                        **common,
                        "sequence": (
                            initial_sequence + 1
                            if cls.mutation == "sequence"
                            else initial_sequence
                        ),
                        "message_number": 1,
                        "original_relay_ordinal": 3,
                        "relay_ordinal": 6,
                    },
                    {
                        **common,
                        "sequence": fault_sequence,
                        "message_number": 2,
                        "original_relay_ordinal": 7,
                        "relay_ordinal": 10,
                    },
                ]
                if cls.mutation == "ciphertext":
                    events[0]["ciphertext_matches_original"] = False
                if cls.mutation == "extra-retransmission":
                    events.append(
                        {
                            **events[0],
                            "sequence": initial_sequence + 2,
                            "message_number": 3,
                            "relay_ordinal": 11,
                        }
                    )
                return events

            @classmethod
            def loss_report_observations(cls) -> list[dict[str, object]]:
                sequence = (
                    initial_sequence + 1
                    if cls.mutation == "loss-report"
                    else initial_sequence
                )
                event = {
                    "direction": "receiver_to_sender",
                    "ranges": ((sequence, sequence),),
                    "relay_ordinal": (
                        7 if cls.mutation == "late-loss-report" else 5
                    ),
                }
                return (
                    [event, dict(event)]
                    if cls.mutation == "duplicate-loss-report"
                    else [event]
                )

        trace = "\n".join(
            (
                json.dumps(
                    {
                        "event": "srt_handshake_trace",
                        "direction": "sender_to_receiver",
                        "request": 0,
                        "initial_sequence": initial_sequence,
                    }
                ),
                self.trace(key_words=8),
            )
        )
        complete = {
            "srt_version": 0x010505,
            "stats": {
                "pktSndLossTotal": 2,
                "pktRetransTotal": 2,
            },
        }
        receiver = {
            "srt_version": 0x010505,
            "stats": {
                "pktRcvUndecryptTotal": 0,
                "pktRcvLossTotal": 2,
            },
        }

        run_file_interop.validate_encrypted_stream(
            scenario, complete, receiver, Relay(), trace
        )
        Relay.mutation = "complementary-role"
        run_file_interop.validate_encrypted_stream(
            scenario, complete, receiver, Relay(), trace
        )
        for mutation in (
            "role",
            "sequence",
            "loss-report",
            "incomplete",
            "ciphertext",
            "extra-retransmission",
            "late-loss-report",
            "duplicate-loss-report",
        ):
            Relay.mutation = mutation
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                RuntimeError, "unplanned"
            ):
                run_file_interop.validate_encrypted_stream(
                    scenario, complete, receiver, Relay(), trace
                )

    def test_encrypted_rollover_nak_accepts_only_exact_ack_race(
        self,
    ) -> None:
        scenario = next(
            item
            for item in run_encrypted_file_interop.rollover_scenario_matrix(
                self.robotweax,
                self.reference,
                512,
                200,
                3,
            )
            if item.recovery == "NAK"
        )
        initial_sequence = run_file_interop.ROLLOVER_MINIMUM_ISN + 5_000
        extra_sequence = initial_sequence + 36
        fault_sequence = 5_452

        class Relay:
            mutation = "valid"

            @staticmethod
            def data_key_observation(direction: str) -> dict[str, object]:
                return EncryptedFileInteropTests.observation()

            @classmethod
            def arq_trace_complete(cls) -> bool:
                return cls.mutation != "incomplete"

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [
                    {
                        "action": "drop",
                        "direction": "sender_to_receiver",
                        "occurrence": (
                            run_file_interop.ROLLOVER_NAK_OCCURRENCE
                        ),
                        "key_selection": 2,
                        "sequence": fault_sequence,
                        "message_number": 17_000,
                        "relay_ordinal": 997,
                    }
                ]

            @staticmethod
            def retransmission_observations(
                direction: str,
            ) -> list[dict[str, object]]:
                common = {
                    "direction": direction,
                    "original_observed": True,
                    "ciphertext_matches_original": True,
                }
                return [
                    {
                        **common,
                        "sequence": extra_sequence,
                        "message_number": 36,
                        "key_selection": 1,
                        "original_key_selection": 1,
                        "payload_bytes": run_file_interop.FILE_PAYLOAD_SIZE,
                        "original_payload_bytes": (
                            run_file_interop.FILE_PAYLOAD_SIZE
                        ),
                        "payload_sha256": "a" * 64,
                        "original_payload_sha256": "a" * 64,
                        "original_relay_ordinal": 57,
                        "relay_ordinal": 62,
                        "retransmission_delay_microseconds": 37_204,
                        "prior_cumulative_ack_next_sequence": (
                            extra_sequence + 1
                        ),
                        "prior_cumulative_ack_relay_ordinal": 61,
                    },
                    {
                        **common,
                        "sequence": fault_sequence,
                        "message_number": 17_000,
                        "key_selection": 2,
                        "original_key_selection": 2,
                        "payload_bytes": run_file_interop.FILE_PAYLOAD_SIZE,
                        "original_payload_bytes": (
                            run_file_interop.FILE_PAYLOAD_SIZE
                        ),
                        "payload_sha256": "b" * 64,
                        "original_payload_sha256": "b" * 64,
                        "original_relay_ordinal": 997,
                        "relay_ordinal": 1_002,
                    },
                ]

            @classmethod
            def loss_report_observations(cls) -> list[dict[str, object]]:
                ranges = ((fault_sequence, fault_sequence),)
                ordinal = 1_001
                direction = "receiver_to_sender"
                if cls.mutation == "missing":
                    return []
                if cls.mutation == "wide-range":
                    ranges = ((fault_sequence, fault_sequence + 1),)
                if cls.mutation == "wrong-sequence":
                    ranges = ((extra_sequence, extra_sequence),)
                if cls.mutation == "late":
                    ordinal = 1_002
                if cls.mutation == "wrong-direction":
                    direction = "sender_to_receiver"
                report = {
                    "direction": direction,
                    "ranges": ranges,
                    "relay_ordinal": ordinal,
                }
                return (
                    [report, dict(report)]
                    if cls.mutation == "duplicate"
                    else [report]
                )

        trace = "\n".join(
            (
                json.dumps(
                    {
                        "event": "srt_handshake_trace",
                        "direction": "sender_to_receiver",
                        "request": -1,
                        "initial_sequence": initial_sequence,
                    }
                ),
                self.trace(key_words=8),
            )
        )
        complete = {
            "srt_version": run_file_interop.PINNED_REFERENCE_SRT_VERSION,
            "stats": {
                "pktSndLossTotal": 1,
                "pktRetransTotal": 2,
            },
        }
        receiver = {
            "srt_version": run_file_interop.PINNED_REFERENCE_SRT_VERSION,
            "stats": {
                "pktRcvUndecryptTotal": 0,
                "pktRcvLossTotal": 1,
            },
        }

        run_file_interop.validate_encrypted_stream(
            scenario, complete, receiver, Relay(), trace
        )
        for mutation in (
            "missing",
            "wide-range",
            "wrong-sequence",
            "late",
            "wrong-direction",
            "duplicate",
            "incomplete",
        ):
            Relay.mutation = mutation
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                RuntimeError, "unplanned"
            ):
                run_file_interop.validate_encrypted_stream(
                    scenario, complete, receiver, Relay(), trace
                )

    def test_encrypted_rollover_rto_batch_is_fail_closed(self) -> None:
        scenario = next(
            item
            for item in run_encrypted_file_interop.rollover_scenario_matrix(
                self.robotweax,
                self.reference,
                512,
                200,
                3,
            )
            if item.recovery == "RTO/LATEREXMIT"
        )
        initial_sequence = run_file_interop.ROLLOVER_MINIMUM_ISN + 5_000
        batch_sequence = 2_368
        batch_message_number = 13_916
        fault_sequence = 5_952

        class Relay:
            mutation = "valid"

            @staticmethod
            def data_key_observation(direction: str) -> dict[str, object]:
                return EncryptedFileInteropTests.observation()

            @classmethod
            def arq_trace_complete(cls) -> bool:
                return cls.mutation != "incomplete"

            @classmethod
            def fault_observations(cls) -> list[dict[str, object]]:
                return [
                    {
                        "action": "drop",
                        "direction": "sender_to_receiver",
                        "occurrence": (
                            run_file_interop.ROLLOVER_PACKET_COUNT
                        ),
                        "key_selection": 2,
                        "sequence": fault_sequence,
                        "message_number": 17_500,
                        "relay_ordinal": 1_000,
                    }
                ]

            @classmethod
            def retransmission_observations(
                cls, direction: str
            ) -> list[dict[str, object]]:
                common = {
                    "direction": direction,
                    "original_observed": True,
                    "ciphertext_matches_original": True,
                }
                planned = {
                    **common,
                    "sequence": fault_sequence,
                    "message_number": 17_500,
                    "key_selection": 2,
                    "original_key_selection": 2,
                    "payload_bytes": 1_077,
                    "original_payload_bytes": 1_077,
                    "payload_sha256": "b" * 64,
                    "original_payload_sha256": "b" * 64,
                    "original_relay_ordinal": 1_000,
                    "relay_ordinal": 1_002,
                }
                batch_size = (
                    run_file_interop
                    .MAX_NATIVE_ROLLOVER_INCIDENTAL_RETRANSMISSIONS
                    + 1
                    if cls.mutation == "too-many"
                    else 2
                )
                batch = [
                    {
                        **common,
                        "sequence": (
                            batch_sequence + index
                        ) & run_file_interop.SEQUENCE_MASK,
                        "message_number": batch_message_number + index,
                        "key_selection": 1 if index == 0 else 2,
                        "original_key_selection": (
                            1 if index == 0 else 2
                        ),
                        "payload_bytes": run_file_interop.FILE_PAYLOAD_SIZE,
                        "original_payload_bytes": (
                            run_file_interop.FILE_PAYLOAD_SIZE
                        ),
                        "payload_sha256": f"{index:x}" * 64,
                        "original_payload_sha256": f"{index:x}" * 64,
                        "original_relay_ordinal": 100 + index,
                        "relay_ordinal": 102 + index,
                        "retransmission_delay_microseconds": (
                            20_000 if index == 0 else 3_000
                        ),
                        "prior_cumulative_ack_next_sequence": batch_sequence,
                        "prior_cumulative_ack_relay_ordinal": 99,
                    }
                    for index in range(batch_size)
                ]
                events = [planned, *batch]
                if cls.mutation == "sequence-gap":
                    events[2]["sequence"] = batch_sequence + 2
                if cls.mutation == "message-gap":
                    events[2]["message_number"] = batch_message_number + 2
                if cls.mutation == "original-gap":
                    events[2]["original_relay_ordinal"] = 103
                if cls.mutation == "retry-gap":
                    events[2]["relay_ordinal"] = 105
                if cls.mutation == "partial-ack":
                    events[2]["prior_cumulative_ack_relay_ordinal"] = None
                if cls.mutation == "ack-past-batch":
                    events[1]["prior_cumulative_ack_next_sequence"] = (
                        batch_sequence + 1
                    )
                if cls.mutation == "ciphertext":
                    events[1]["ciphertext_matches_original"] = False
                if cls.mutation == "short-head-delay":
                    events[1]["retransmission_delay_microseconds"] = 1
                if cls.mutation == "retry-after-fault":
                    events[2]["relay_ordinal"] = 1_000
                if cls.mutation == "short-payload":
                    events[2]["payload_bytes"] = (
                        run_file_interop.FILE_PAYLOAD_SIZE - 1
                    )
                if cls.mutation in {
                    "ack-races",
                    "ack-race-too-late",
                    "ack-race-too-fast",
                }:
                    for index, event in enumerate(events[1:]):
                        event.update(
                            {
                                "prior_cumulative_ack_next_sequence": (
                                    int(event["sequence"]) + 1
                                ) & run_file_interop.SEQUENCE_MASK,
                                "prior_cumulative_ack_relay_ordinal": (
                                    int(event["original_relay_ordinal"])
                                    + 1
                                ),
                                "retransmission_delay_microseconds": 20_000,
                            }
                        )
                    if cls.mutation == "ack-race-too-late":
                        events[1]["relay_ordinal"] = (
                            int(
                                events[1][
                                    "prior_cumulative_ack_relay_ordinal"
                                ]
                            )
                            + run_file_interop
                            .MAX_NATIVE_ROLLOVER_ACK_RACE_RELAY_DATAGRAMS
                            + 1
                        )
                    if cls.mutation == "ack-race-too-fast":
                        events[1]["retransmission_delay_microseconds"] = (
                            run_file_interop
                            .MINIMUM_NATIVE_ROLLOVER_ACK_RACE_DELAY_MICROSECONDS
                            - 1
                        )
                if cls.mutation in {
                    "startup-no-ack",
                    "startup-outside-bound",
                    "startup-late-retry",
                }:
                    startup_offset = (
                        run_file_interop
                        .MAX_NATIVE_ROLLOVER_STARTUP_DATA_PACKETS
                        if cls.mutation == "startup-outside-bound"
                        else 9
                    )
                    for index, event in enumerate(events[1:]):
                        event.update(
                            {
                                "sequence": (
                                    initial_sequence
                                    + startup_offset
                                    + index
                                ) & run_file_interop.SEQUENCE_MASK,
                                "message_number": 10 + index,
                                "original_relay_ordinal": 19 + index,
                                "relay_ordinal": 27 + index,
                                "prior_cumulative_ack_next_sequence": None,
                                "prior_cumulative_ack_relay_ordinal": None,
                            }
                        )
                    if cls.mutation == "startup-late-retry":
                        for index, event in enumerate(events[1:]):
                            event["relay_ordinal"] = (
                                run_file_interop
                                .MAX_NATIVE_ROLLOVER_STARTUP_RELAY_ORDINAL
                                + 1
                                + index
                            )
                return events

            @classmethod
            def loss_report_observations(cls) -> list[dict[str, object]]:
                if cls.mutation != "loss-report":
                    return []
                return [
                    {
                        "direction": "receiver_to_sender",
                        "ranges": ((batch_sequence, batch_sequence),),
                        "relay_ordinal": 101,
                    }
                ]

        trace = "\n".join(
            (
                json.dumps(
                    {
                        "event": "srt_handshake_trace",
                        "direction": "sender_to_receiver",
                        "request": -1,
                        "initial_sequence": initial_sequence,
                    }
                ),
                self.trace(key_words=8),
            )
        )
        complete = {
            "srt_version": run_file_interop.PINNED_REFERENCE_SRT_VERSION,
            "stats": {
                "pktSndLossTotal": 0,
                "pktRetransTotal": 3,
            },
        }
        receiver = {
            "srt_version": run_file_interop.PINNED_REFERENCE_SRT_VERSION,
            "stats": {
                "pktRcvUndecryptTotal": 0,
                "pktRcvLossTotal": 0,
            },
        }

        run_file_interop.validate_encrypted_stream(
            scenario, complete, receiver, Relay(), trace
        )
        Relay.mutation = "startup-no-ack"
        run_file_interop.validate_encrypted_stream(
            scenario, complete, receiver, Relay(), trace
        )
        Relay.mutation = "ack-races"
        run_file_interop.validate_encrypted_stream(
            scenario, complete, receiver, Relay(), trace
        )
        for mutation in (
            "sequence-gap",
            "message-gap",
            "original-gap",
            "retry-gap",
            "partial-ack",
            "ack-past-batch",
            "loss-report",
            "incomplete",
            "ciphertext",
            "too-many",
            "short-head-delay",
            "retry-after-fault",
            "short-payload",
            "startup-outside-bound",
            "startup-late-retry",
            "ack-race-too-late",
            "ack-race-too-fast",
        ):
            Relay.mutation = mutation
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                RuntimeError, "unplanned"
            ):
                run_file_interop.validate_encrypted_stream(
                    scenario, complete, receiver, Relay(), trace
                )

    def test_long_encrypted_rto_targets_exact_flight_tail(self) -> None:
        scenario = next(
            item
            for item in run_encrypted_file_interop.scenario_matrix(
                self.robotweax,
                self.reference,
                self.options.byte_count,
                512,
                200,
                3,
            )
            if item.recovery == "RTO/LATEREXMIT"
        )
        packet_count = (
            self.options.byte_count
            + run_file_interop.FILE_PAYLOAD_SIZE
            - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        tail_bytes = (
            self.options.byte_count
            - (packet_count - 1) * run_file_interop.FILE_PAYLOAD_SIZE
        )

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [
                    {
                        "action": "drop",
                        "direction": "sender_to_receiver",
                        "occurrence": packet_count,
                        "payload_bytes": tail_bytes,
                        "retransmission_observed": True,
                        "retransmission_relay_ordinal": packet_count + 2,
                        "retransmission_delay_microseconds": 20_000,
                        "retransmission_payload_bytes": tail_bytes,
                        "retransmission_ciphertext_matches": True,
                    }
                ]

        observation = run_file_interop.validate_fault_recovery(
            scenario,
            Relay(),
            self.options.byte_count,
        )
        self.assertEqual(observation["payload_bytes"], tail_bytes)

        if scenario.fault is None:
            self.fail("RTO scenario lacks its configured tail fault")
        early = replace(
            scenario,
            fault=replace(
                scenario.fault,
                occurrence=packet_count - 1,
            ),
        )

        class EarlyRelay(Relay):
            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                event = dict(Relay.fault_observations()[0])
                event["occurrence"] = packet_count - 1
                return [event]

        with self.assertRaisesRegex(RuntimeError, "sender RTO"):
            run_file_interop.validate_fault_recovery(
                early,
                EarlyRelay(),
                self.options.byte_count,
            )

    def test_stream_eof_requires_exact_zero_byte_event(self) -> None:
        event = {
            "event": "eof",
            "role": "listener",
            "bytes": self.options.byte_count,
        }
        run_file_interop.validate_stream_eof(
            self.scenario,
            json.dumps(event),
            self.options.byte_count,
        )

        for key, replacement in (
            ("bytes", self.options.byte_count - 1),
            ("role", "caller"),
            ("unexpected", True),
        ):
            mutated = {**event, key: replacement}
            with self.subTest(key=key), self.assertRaisesRegex(
                RuntimeError, "File Stream EOF evidence"
            ):
                run_file_interop.validate_stream_eof(
                    self.scenario,
                    json.dumps(mutated),
                    self.options.byte_count,
                )
        with self.assertRaisesRegex(
            RuntimeError, "File Stream EOF evidence"
        ):
            duplicate = "\n".join((json.dumps(event), json.dumps(event)))
            run_file_interop.validate_stream_eof(
                self.scenario,
                duplicate,
                self.options.byte_count,
            )

    def test_clear_scenario_rejects_partial_crypto_configuration(self) -> None:
        partial = replace(self.scenario, key_length=0)
        with self.assertRaisesRegex(ValueError, "complete key-rotation"):
            run_file_interop.peer_command(
                self.robotweax,
                "caller",
                9_001,
                Path("/tmp/input"),
                partial,
                self.options,
            )

        partial_rendezvous = replace(
            self.rendezvous_scenario,
            key_length=0,
        )
        with self.assertRaisesRegex(ValueError, "complete key-rotation"):
            run_file_interop.rendezvous_peer_command(
                self.robotweax,
                "rendezvous-sender",
                9_001,
                9_002,
                Path("/tmp/input"),
                partial_rendezvous,
                self.options,
            )


if __name__ == "__main__":
    unittest.main()
