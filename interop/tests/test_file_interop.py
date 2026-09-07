from __future__ import annotations

import errno
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_file_interop  # noqa: E402
import srt_handshake_trace  # noqa: E402


class FileInteropTests(unittest.TestCase):
    def setUp(self) -> None:
        self.robotweax = Path("/tmp/robotweax-peer")
        self.reference = Path("/tmp/reference-peer")
        self.options = run_file_interop.RunOptions(
            byte_count=2_097_531,
            timeout_seconds=45,
            maximum_bandwidth_bytes_per_second=20_000_000,
            shutdown_grace_milliseconds=250,
        )

    def test_matrix_covers_both_implementation_directions(self) -> None:
        scenarios = run_file_interop.scenario_matrix(
            self.robotweax, self.reference
        )
        self.assertEqual(len(scenarios), 8)
        self.assertEqual(
            {(item.caller, item.listener) for item in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        self.assertTrue(
            all(item.sender_chunk_size > item.receiver_size for item in scenarios)
        )
        for caller, listener in (
            (self.robotweax, self.reference),
            (self.reference, self.robotweax),
        ):
            directional = [
                item
                for item in scenarios
                if item.caller == caller and item.listener == listener
            ]
            self.assertEqual(
                [item.recovery for item in directional],
                [None, None, "NAK", "RTO/LATEREXMIT"],
            )
            helpers = [item for item in directional if item.file_api]
            self.assertEqual(len(helpers), 1)
            self.assertTrue(helpers[0].file_size_to_eof)

    def test_fault_relay_retries_transient_udp_backpressure(
        self,
    ) -> None:
        class TransientSocket:
            def __init__(self) -> None:
                self.calls = 0

            def sendto(
                self, payload: bytes, target: tuple[str, int]
            ) -> int:
                self.calls += 1
                if self.calls == 1:
                    raise BlockingIOError(
                        errno.EAGAIN, "temporarily unavailable"
                    )
                return len(payload)

        outbound = TransientSocket()
        with mock.patch.object(
            srt_handshake_trace.select,
            "select",
            return_value=((), (outbound,), ()),
        ):
            srt_handshake_trace.RendezvousTraceProxy._send_datagram(
                outbound, ("127.0.0.1", 9_999), b"ack"
            )
        self.assertEqual(outbound.calls, 2)

    def test_fault_relay_bounds_persistent_udp_backpressure(
        self,
    ) -> None:
        class BlockedSocket:
            @staticmethod
            def sendto(
                payload: bytes, target: tuple[str, int]
            ) -> int:
                raise BlockingIOError(
                    errno.EAGAIN, "temporarily unavailable"
                )

        with (
            mock.patch.object(
                srt_handshake_trace.time,
                "monotonic",
                side_effect=(10.0, 10.6),
            ),
            self.assertRaises(BlockingIOError),
        ):
            srt_handshake_trace.RendezvousTraceProxy._send_datagram(
                BlockedSocket(), ("127.0.0.1", 9_999), b"ack"
            )

    def test_rendezvous_matrix_covers_both_sender_directions(self) -> None:
        scenarios = run_file_interop.rendezvous_scenario_matrix(
            self.robotweax, self.reference
        )
        self.assertEqual(len(scenarios), 2)
        self.assertEqual(
            {(item.sender, item.receiver) for item in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        self.assertTrue(
            all(
                item.sender_chunk_size > item.receiver_size
                for item in scenarios
            )
        )
        self.assertTrue(
            all(item.robotweax_peer == self.robotweax for item in scenarios)
        )

    def test_ipv6_rendezvous_matrix_covers_exact_transfer_and_loss(
        self,
    ) -> None:
        scenarios = run_file_interop.ipv6_rendezvous_scenario_matrix(
            self.robotweax,
            self.reference,
        )
        self.assertEqual(len(scenarios), 4)
        self.assertEqual(
            {(item.sender, item.receiver) for item in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        self.assertEqual(
            [item.recovery for item in scenarios],
            [None, None, "NAK", "CAUSAL"],
        )
        faulted = [
            item for item in scenarios if item.fault is not None
        ]
        self.assertEqual(len(faulted), 2)
        self.assertTrue(
            all(
                item.byte_count
                == (
                    run_file_interop.RESILIENCE_TRANSFER_PACKETS
                    * run_file_interop.FILE_PAYLOAD_SIZE
                    + 379
                )
                for item in faulted
            )
        )
        self.assertTrue(
            all(
                run_file_interop.scenario_faults(item)[0].occurrence
                == run_file_interop.RENDEZVOUS_SINGLE_LOSS_OCCURRENCE
                for item in faulted
            )
        )

    def test_rendezvous_resilience_matrix_covers_fault_rate_and_eof(
        self,
    ) -> None:
        scenarios = (
            run_file_interop.rendezvous_resilience_scenario_matrix(
                self.robotweax,
                self.reference,
            )
        )
        self.assertEqual(len(scenarios), 9)
        dynamic = scenarios[0]
        self.assertEqual(dynamic.sender, self.robotweax)
        self.assertEqual(
            dynamic.maximum_bandwidth_bytes_per_second,
            run_file_interop.DYNAMIC_MAXBW_FIRST_BYTES_PER_SECOND,
        )
        self.assertEqual(
            dynamic.second_maximum_bandwidth_bytes_per_second,
            run_file_interop.DYNAMIC_MAXBW_SECOND_BYTES_PER_SECOND,
        )
        for sender, receiver, expected_recovery in (
            (
                self.robotweax,
                self.reference,
                ["NAK", "NAK", "RTO/LATEREXMIT", None],
            ),
            (
                self.reference,
                self.robotweax,
                ["CAUSAL", "CAUSAL", "RTO/LATEREXMIT", None],
            ),
        ):
            directional = [
                item
                for item in scenarios
                if (
                    item.sender == sender
                    and item.receiver == receiver
                    and item is not dynamic
                )
            ]
            self.assertEqual(
                [len(run_file_interop.scenario_faults(item))
                 for item in directional],
                [1, 3, 1, 0],
            )
            self.assertEqual(
                [item.recovery for item in directional],
                expected_recovery,
            )
            self.assertEqual(
                [
                    fault.occurrence
                    for fault in run_file_interop.scenario_faults(
                        directional[0]
                    )
                ],
                [run_file_interop.RENDEZVOUS_SINGLE_LOSS_OCCURRENCE],
            )
            self.assertEqual(
                [
                    fault.occurrence
                    for fault in run_file_interop.scenario_faults(
                        directional[1]
                    )
                ],
                list(
                    run_file_interop.RENDEZVOUS_BURST_LOSS_OCCURRENCES
                ),
            )
            rto = directional[-2]
            self.assertEqual(
                rto.byte_count,
                run_file_interop.FILE_PAYLOAD_SIZE + 997,
            )
            self.assertEqual(
                run_file_interop.scenario_faults(rto)[0].occurrence,
                2,
            )
            self.assertTrue(directional[-1].expect_eof)

    def test_rollover_matrix_covers_native_and_translated_recovery(
        self,
    ) -> None:
        scenarios = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )
        self.assertEqual(len(scenarios), 4)
        self.assertEqual(
            {(item.caller, item.listener) for item in scenarios},
            {
                (self.robotweax, self.reference),
                (self.reference, self.robotweax),
            },
        )
        for caller, listener in (
            (self.robotweax, self.reference),
            (self.reference, self.robotweax),
        ):
            directional = [
                item
                for item in scenarios
                if item.caller == caller and item.listener == listener
            ]
            self.assertEqual(
                [item.recovery for item in directional],
                ["NAK", "RTO/LATEREXMIT"],
            )
            self.assertTrue(all(item.rollover for item in directional))
        robotweax_sender = [
            item for item in scenarios if item.caller == self.robotweax
        ]
        self.assertTrue(
            all(
                item.sender_chunk_size
                == run_file_interop.FILE_PAYLOAD_SIZE
                for item in robotweax_sender
            )
        )
        self.assertTrue(
            all(
                item.minimum_initial_sequence
                == run_file_interop.ROLLOVER_MINIMUM_ISN
                for item in robotweax_sender
            )
        )
        reference_sender = [
            item for item in scenarios if item.caller == self.reference
        ]
        self.assertEqual(
            [
                item.translated_initial_sequence
                for item in reference_sender
            ],
            [
                run_file_interop.TRANSLATED_ROLLOVER_ISN,
                run_file_interop.TRANSLATED_ROLLOVER_RTO_ISN,
            ],
        )

    def test_resilience_matrix_covers_dynamic_burst_and_ack_delay(
        self,
    ) -> None:
        scenarios = run_file_interop.resilience_scenario_matrix(
            self.robotweax, self.reference
        )
        self.assertEqual(len(scenarios), 5)
        dynamic = scenarios[0]
        self.assertEqual(dynamic.caller, self.robotweax)
        self.assertEqual(
            dynamic.maximum_bandwidth_bytes_per_second,
            run_file_interop
            .DYNAMIC_MAXBW_FIRST_BYTES_PER_SECOND,
        )
        self.assertEqual(
            dynamic.second_maximum_bandwidth_bytes_per_second,
            run_file_interop
            .DYNAMIC_MAXBW_SECOND_BYTES_PER_SECOND,
        )
        burst_scenarios = scenarios[1:3]
        self.assertTrue(
            all(
                len(run_file_interop.scenario_faults(item)) == 3
                for item in burst_scenarios
            )
        )
        delayed_scenarios = scenarios[3:]
        self.assertTrue(
            all(
                run_file_interop.scenario_faults(item)[0]
                    .control_type
                    == 2
                and item.flow_window_packets
                    == run_file_interop
                    .RESILIENCE_FLOW_WINDOW_PACKETS
                for item in delayed_scenarios
            )
        )

    def test_rendezvous_role_probes_cycle_direction_and_start_order(
        self,
    ) -> None:
        probes = [
            run_file_interop.rendezvous_role_probe_scenario(
                self.robotweax,
                self.reference,
                attempt,
                131_072,
                100,
            )
            for attempt in range(4)
        ]
        self.assertEqual(
            [item.sender for item in probes],
            [
                self.robotweax,
                self.robotweax,
                self.reference,
                self.reference,
            ],
        )
        self.assertEqual(
            [item.start_order for item in probes],
            [
                "sender-first",
                "receiver-first",
                "sender-first",
                "receiver-first",
            ],
        )
        self.assertTrue(
            all(
                run_file_interop.rendezvous_byte_count(
                    item, self.options
                )
                == 131_072
                for item in probes
            )
        )
        prefixed = run_file_interop.rendezvous_role_probe_scenario(
            self.robotweax,
            self.reference,
            0,
            131_072,
            100,
            "filecc-ipv6-role-probe",
        )
        self.assertEqual(prefixed.name, "filecc-ipv6-role-probe-1")

    def test_tail_drop_targets_second_packet_of_short_flight(self) -> None:
        scenarios = run_file_interop.scenario_matrix(
            self.robotweax, self.reference
        )
        tail = next(
            item
            for item in scenarios
            if item.recovery == "RTO/LATEREXMIT"
        )
        self.assertEqual(
            run_file_interop.scenario_byte_count(tail, self.options),
            run_file_interop.FILE_PAYLOAD_SIZE + 997,
        )
        self.assertGreater(
            run_file_interop.scenario_byte_count(tail, self.options),
            run_file_interop.FILE_PAYLOAD_SIZE,
        )
        self.assertLessEqual(
            run_file_interop.scenario_byte_count(tail, self.options),
            2 * run_file_interop.FILE_PAYLOAD_SIZE,
        )
        self.assertEqual(tail.fault.occurrence, 2)

    def test_peer_command_selects_file_profile_and_controller(self) -> None:
        scenario = run_file_interop.scenario_matrix(
            self.robotweax, self.reference
        )[0]
        command = run_file_interop.peer_command(
            self.robotweax,
            "caller",
            12_345,
            Path("/tmp/input.bin"),
            scenario,
            self.options,
        )
        self.assertIn("--transport", command)
        self.assertEqual(command[command.index("--transport") + 1], "file")
        self.assertIn("--congestion", command)
        self.assertEqual(command[command.index("--congestion") + 1], "file")
        self.assertEqual(
            command[command.index("--chunk-size") + 1],
            str(scenario.sender_chunk_size),
        )
        self.assertEqual(
            command[command.index("--receive-size") + 1],
            str(scenario.receiver_size),
        )
        self.assertIn("--input", command)
        self.assertNotIn("--file-api", command)
        self.assertNotIn("--file-size-to-eof", command)

    def test_file_helper_commands_use_directional_block_sizes(self) -> None:
        scenario = next(
            item
            for item in run_file_interop.scenario_matrix(
                self.robotweax, self.reference
            )
            if item.file_api
        )
        caller = run_file_interop.peer_command(
            scenario.caller,
            "caller",
            12_345,
            Path("/tmp/input.bin"),
            scenario,
            self.options,
        )
        listener = run_file_interop.peer_command(
            scenario.listener,
            "listener",
            12_345,
            Path("/tmp/output.bin"),
            scenario,
            self.options,
        )
        self.assertIn("--file-api", caller)
        self.assertIn("--file-api", listener)
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
            caller[caller.index("--shutdown-grace-ms") + 1],
            str(self.options.shutdown_grace_milliseconds),
        )
        self.assertEqual(
            listener[listener.index("--shutdown-grace-ms") + 1],
            str(
                run_file_interop
                .FILE_API_RECEIVER_SHUTDOWN_GRACE_MILLISECONDS
            ),
        )

    def test_size_to_eof_requires_file_api(self) -> None:
        scenario = run_file_interop.Scenario(
            name="invalid-size-to-eof",
            caller=self.robotweax,
            listener=self.reference,
            seed=1,
            sender_chunk_size=1_456,
            receiver_size=997,
            file_size_to_eof=True,
        )
        with self.assertRaisesRegex(
            ValueError, "size-to-EOF requires the public file API"
        ):
            run_file_interop.peer_command(
                self.robotweax,
                "caller",
                12_345,
                Path("/tmp/input.bin"),
                scenario,
                self.options,
            )

    def test_rollover_sender_command_searches_via_read_only_isn(
        self,
    ) -> None:
        scenario = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )[0]
        command = run_file_interop.peer_command(
            self.robotweax,
            "caller",
            12_345,
            Path("/tmp/input.bin"),
            scenario,
            self.options,
        )
        self.assertEqual(
            command[command.index("--minimum-isn") + 1],
            str(run_file_interop.ROLLOVER_MINIMUM_ISN),
        )
        self.assertEqual(
            command[command.index("--isn-search-limit") + 1],
            "100000",
        )

    def test_rollover_listener_does_not_receive_caller_isn_options(
        self,
    ) -> None:
        scenario = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )[0]
        command = run_file_interop.peer_command(
            self.reference,
            "listener",
            12_345,
            Path("/tmp/output.bin"),
            scenario,
            self.options,
        )
        self.assertNotIn("--minimum-isn", command)
        self.assertNotIn("--isn-search-limit", command)

    def test_translated_rollover_rto_targets_wrapped_short_flight_tail(
        self,
    ) -> None:
        scenario = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )[3]
        self.assertEqual(
            scenario.translated_initial_sequence,
            run_file_interop.SEQUENCE_MASK,
        )
        self.assertEqual(
            run_file_interop.scenario_byte_count(
                scenario, self.options
            ),
            run_file_interop.FILE_PAYLOAD_SIZE + 997,
        )
        self.assertEqual(scenario.fault.occurrence, 2)

    def test_native_rollover_rto_uses_packet_aligned_send_chunks(
        self,
    ) -> None:
        scenario = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )[1]
        self.assertEqual(
            scenario.sender_chunk_size,
            run_file_interop.FILE_PAYLOAD_SIZE,
        )
        self.assertEqual(
            scenario.fault.occurrence,
            run_file_interop.ROLLOVER_PACKET_COUNT,
        )

    def test_dynamic_and_flow_options_are_role_aware(self) -> None:
        dynamic = run_file_interop.resilience_scenario_matrix(
            self.robotweax, self.reference
        )[0]
        caller = run_file_interop.peer_command(
            self.robotweax,
            "caller",
            12_345,
            Path("/tmp/input.bin"),
            dynamic,
            self.options,
        )
        listener = run_file_interop.peer_command(
            self.reference,
            "listener",
            12_345,
            Path("/tmp/output.bin"),
            dynamic,
            self.options,
        )
        self.assertEqual(
            caller[caller.index("--max-bw") + 1],
            str(
                run_file_interop
                .DYNAMIC_MAXBW_FIRST_BYTES_PER_SECOND
            ),
        )
        self.assertEqual(
            caller[caller.index("--second-max-bw") + 1],
            str(
                run_file_interop
                .DYNAMIC_MAXBW_SECOND_BYTES_PER_SECOND
            ),
        )
        self.assertNotIn("--second-max-bw", listener)

        delayed = run_file_interop.resilience_scenario_matrix(
            self.robotweax, self.reference
        )[3]
        for role, program, path in (
            ("caller", self.robotweax, Path("/tmp/input.bin")),
            ("listener", self.reference, Path("/tmp/output.bin")),
        ):
            command = run_file_interop.peer_command(
                program,
                role,
                12_345,
                path,
                delayed,
                self.options,
            )
            self.assertEqual(
                command[command.index("--flow-window") + 1],
                str(
                    run_file_interop
                    .RESILIENCE_FLOW_WINDOW_PACKETS
                ),
            )

    def test_rendezvous_command_selects_explicit_file_profile(
        self,
    ) -> None:
        scenario = run_file_interop.rendezvous_scenario_matrix(
            self.robotweax, self.reference
        )[0]
        command = run_file_interop.rendezvous_peer_command(
            self.robotweax,
            "rendezvous-sender",
            12_345,
            12_346,
            Path("/tmp/input.bin"),
            scenario,
            self.options,
        )
        self.assertEqual(
            command[command.index("--local-port") + 1], "12345"
        )
        self.assertEqual(command[command.index("--port") + 1], "12346")
        self.assertEqual(
            command[command.index("--transport") + 1], "file"
        )
        self.assertEqual(
            command[command.index("--congestion") + 1], "file"
        )
        self.assertEqual(
            command[command.index("--chunk-size") + 1],
            str(scenario.sender_chunk_size),
        )
        self.assertEqual(
            command[command.index("--receive-size") + 1],
            str(scenario.receiver_size),
        )
        self.assertEqual(
            command[command.index("--max-bw") + 1],
            str(self.options.maximum_bandwidth_bytes_per_second),
        )
        self.assertNotIn("--input-bw", command)
        self.assertIn("--input", command)

    def test_rendezvous_receiver_command_uses_output_path(self) -> None:
        scenario = run_file_interop.rendezvous_scenario_matrix(
            self.robotweax, self.reference
        )[1]
        command = run_file_interop.rendezvous_peer_command(
            self.robotweax,
            "rendezvous-receiver",
            12_346,
            12_345,
            Path("/tmp/output.bin"),
            scenario,
            self.options,
        )
        self.assertIn("--output", command)
        self.assertNotIn("--input", command)

    def test_rendezvous_command_accepts_ipv6_loopback(self) -> None:
        scenario = run_file_interop.ipv6_rendezvous_scenario_matrix(
            self.robotweax,
            self.reference,
        )[0]
        command = run_file_interop.rendezvous_peer_command(
            self.robotweax,
            "rendezvous-sender",
            12_345,
            12_346,
            Path("/tmp/input.bin"),
            scenario,
            self.options,
            "::1",
        )
        self.assertEqual(
            command[command.index("--local-host") + 1],
            "::1",
        )
        self.assertEqual(command[command.index("--host") + 1], "::1")

    def test_rendezvous_role_matrix_propagates_ipv6_host(self) -> None:
        scenarios = run_file_interop.ipv6_rendezvous_scenario_matrix(
            self.robotweax,
            self.reference,
        )[:2]
        with (
            tempfile.TemporaryDirectory() as directory,
            mock.patch.object(
                run_file_interop,
                "run_rendezvous_scenario",
                side_effect=("initiator", "responder"),
            ) as run_scenario,
        ):
            failures = run_file_interop.run_rendezvous_role_matrix(
                scenarios,
                self.robotweax,
                self.reference,
                self.options,
                Path(directory),
                0,
                131_072,
                100,
                host="::1",
                coverage_name="ipv6-role-coverage",
                probe_name_prefix="ipv6-role-probe",
            )
        self.assertEqual(failures, [])
        self.assertEqual(run_scenario.call_count, 2)
        self.assertTrue(
            all(
                call.args[-1] == "::1"
                for call in run_scenario.call_args_list
            )
        )

    def test_rendezvous_resilience_commands_apply_sender_rate_and_eof(
        self,
    ) -> None:
        scenarios = (
            run_file_interop.rendezvous_resilience_scenario_matrix(
                self.robotweax,
                self.reference,
            )
        )
        dynamic = scenarios[0]
        sender = run_file_interop.rendezvous_peer_command(
            dynamic.sender,
            "rendezvous-sender",
            12_345,
            12_346,
            Path("/tmp/input.bin"),
            dynamic,
            self.options,
        )
        receiver = run_file_interop.rendezvous_peer_command(
            dynamic.receiver,
            "rendezvous-receiver",
            12_346,
            12_345,
            Path("/tmp/output.bin"),
            dynamic,
            self.options,
        )
        self.assertEqual(
            sender[sender.index("--max-bw") + 1],
            str(run_file_interop.DYNAMIC_MAXBW_FIRST_BYTES_PER_SECOND),
        )
        self.assertEqual(
            sender[sender.index("--second-max-bw") + 1],
            str(run_file_interop.DYNAMIC_MAXBW_SECOND_BYTES_PER_SECOND),
        )
        self.assertNotIn("--second-max-bw", receiver)

        eof = scenarios[-1]
        eof_sender = run_file_interop.rendezvous_peer_command(
            eof.sender,
            "rendezvous-sender",
            12_345,
            12_346,
            Path("/tmp/input.bin"),
            eof,
            self.options,
        )
        eof_receiver = run_file_interop.rendezvous_peer_command(
            eof.receiver,
            "rendezvous-receiver",
            12_346,
            12_345,
            Path("/tmp/output.bin"),
            eof,
            self.options,
        )
        self.assertNotIn("--expect-eof", eof_sender)
        self.assertIn("--expect-eof", eof_receiver)

    def test_rendezvous_command_rejects_caller_role(self) -> None:
        scenario = run_file_interop.rendezvous_scenario_matrix(
            self.robotweax, self.reference
        )[0]
        with self.assertRaisesRegex(
            ValueError, "unsupported rendezvous role"
        ):
            run_file_interop.rendezvous_peer_command(
                self.robotweax,
                "caller",
                12_345,
                12_346,
                Path("/tmp/input.bin"),
                scenario,
                self.options,
            )

    def test_ready_event_parser_is_strict(self) -> None:
        self.assertEqual(
            run_file_interop.parse_ready_port(
                'noise\n{"event":"ready","port":12345}\n'
            ),
            12_345,
        )
        self.assertIsNone(
            run_file_interop.parse_ready_port(
                '{"event":"complete","port":12345}\n'
            )
        )
        self.assertIsNone(
            run_file_interop.parse_ready_port(
                '{"event":"ready","port":70000}\n'
            )
        )

    def test_listener_command_uses_output_path(self) -> None:
        scenario = run_file_interop.scenario_matrix(
            self.robotweax, self.reference
        )[1]
        command = run_file_interop.peer_command(
            self.robotweax,
            "listener",
            12_346,
            Path("/tmp/output.bin"),
            scenario,
            self.options,
        )
        self.assertIn("--output", command)
        self.assertNotIn("--input", command)

    def test_peer_command_rejects_unknown_role(self) -> None:
        scenario = run_file_interop.scenario_matrix(
            self.robotweax, self.reference
        )[0]
        with self.assertRaisesRegex(ValueError, "unsupported role"):
            run_file_interop.peer_command(
                self.robotweax,
                "rendezvous-sender",
                12_345,
                Path("/tmp/input.bin"),
                scenario,
                self.options,
            )

    def test_statistics_require_complete_loss_free_transfer(self) -> None:
        scenario = run_file_interop.scenario_matrix(
            self.robotweax, self.reference
        )[0]
        required = (
            self.options.byte_count
            + run_file_interop.FILE_PAYLOAD_SIZE
            - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        sent, received = run_file_interop.validate_statistics(
            scenario,
            {
                "stats": {
                    "pktSentTotal": required,
                    "pktSndDropTotal": 0,
                }
            },
            {"stats": {"pktRecvTotal": required}},
            self.options.byte_count,
        )
        self.assertEqual(sent, required)
        self.assertEqual(received, required)

        with self.assertRaisesRegex(RuntimeError, "dropped packets"):
            run_file_interop.validate_statistics(
                scenario,
                {
                    "stats": {
                        "pktSentTotal": required,
                        "pktSndDropTotal": 1,
                    }
                },
                {"stats": {"pktRecvTotal": required}},
                self.options.byte_count,
            )

    def test_wire_statistics_require_unique_plus_retransmission_totals(
        self,
    ) -> None:
        scenario = run_file_interop.Scenario(
            name="wire-statistics",
            caller=self.robotweax,
            listener=self.reference,
            seed=1,
            sender_chunk_size=1_456,
            receiver_size=2_048,
        )
        sender = {
            "stats": {
                "pktSentTotal": 2,
                "pktSentUniqueTotal": 2,
                "pktRetransTotal": 0,
                "byteSentTotal": 3_000,
                "byteSentUniqueTotal": 3_000,
                "byteRetransTotal": 0,
            }
        }
        receiver = {
            "stats": {
                "pktRecvTotal": 2,
                "pktRecvUniqueTotal": 2,
                "byteRecvTotal": 3_000,
                "byteRecvUniqueTotal": 3_000,
            }
        }

        run_file_interop.validate_wire_statistics(
            scenario, sender, receiver, 2_912
        )
        sender["stats"]["pktSentTotal"] = 3
        with self.assertRaisesRegex(
            RuntimeError, "unique plus retransmitted"
        ):
            run_file_interop.validate_wire_statistics(
                scenario, sender, receiver, 2_912
            )

    def test_nak_wire_statistics_require_receiver_loss_bytes(
        self,
    ) -> None:
        scenario = run_file_interop.Scenario(
            name="nak-wire-statistics",
            caller=self.robotweax,
            listener=self.reference,
            seed=1,
            sender_chunk_size=1_456,
            receiver_size=2_048,
            fault=run_file_interop.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=1,
            ),
            recovery="NAK",
        )
        sender = {
            "stats": {
                "pktSentTotal": 3,
                "pktSentUniqueTotal": 2,
                "pktRetransTotal": 1,
                "byteSentTotal": 4_500,
                "byteSentUniqueTotal": 3_000,
                "byteRetransTotal": 1_500,
            }
        }
        receiver = {
            "stats": {
                "pktRecvTotal": 2,
                "pktRecvUniqueTotal": 2,
                "byteRecvTotal": 3_000,
                "byteRecvUniqueTotal": 3_000,
                "pktRcvLossTotal": 1,
                "byteRcvLossTotal": 1_500,
            }
        }

        run_file_interop.validate_wire_statistics(
            scenario, sender, receiver, 2_912
        )
        receiver["stats"]["byteRcvLossTotal"] = 0
        with self.assertRaisesRegex(
            RuntimeError, "receiver loss"
        ):
            run_file_interop.validate_wire_statistics(
                scenario, sender, receiver, 2_912
            )

    def test_causal_wire_statistics_count_only_nak_recoveries(
        self,
    ) -> None:
        faults = tuple(
            run_file_interop.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=occurrence,
            )
            for occurrence in (1, 2, 3)
        )
        scenario = run_file_interop.Scenario(
            name="mixed-rto-nak-wire-statistics",
            caller=self.reference,
            listener=self.robotweax,
            seed=1,
            sender_chunk_size=1_456,
            receiver_size=2_048,
            fault=faults,
            recovery="CAUSAL",
        )
        observations = [
            {
                "action": "drop",
                "later_data_observed_before_retransmission": exposed,
            }
            for exposed in (False, False, True)
        ]
        sender = {
            "stats": {
                "pktSentTotal": 5,
                "pktSentUniqueTotal": 2,
                "pktRetransTotal": 3,
                "byteSentTotal": 7_500,
                "byteSentUniqueTotal": 3_000,
                "byteRetransTotal": 4_500,
            }
        }
        receiver = {
            "stats": {
                "pktRecvTotal": 2,
                "pktRecvUniqueTotal": 2,
                "byteRecvTotal": 3_000,
                "byteRecvUniqueTotal": 3_000,
                "pktRcvLossTotal": 1,
                "byteRcvLossTotal": 1_500,
            }
        }

        self.assertEqual(
            run_file_interop.nak_recovery_count(
                scenario, observations
            ),
            1,
        )
        run_file_interop.validate_wire_statistics(
            scenario, sender, receiver, 2_912, observations
        )

        receiver["stats"]["pktRcvLossTotal"] = 0
        with self.assertRaisesRegex(RuntimeError, "receiver loss"):
            run_file_interop.validate_wire_statistics(
                scenario, sender, receiver, 2_912, observations
            )

        strict_nak = run_file_interop.Scenario(
            name="strict-nak-wire-statistics",
            caller=self.robotweax,
            listener=self.reference,
            seed=2,
            sender_chunk_size=1_456,
            receiver_size=2_048,
            fault=faults,
            recovery="NAK",
        )
        receiver["stats"]["pktRcvLossTotal"] = 2
        with self.assertRaisesRegex(RuntimeError, "receiver loss"):
            run_file_interop.validate_wire_statistics(
                strict_nak, sender, receiver, 2_912, observations
            )
        receiver["stats"]["pktRcvLossTotal"] = 3
        run_file_interop.validate_wire_statistics(
            strict_nak, sender, receiver, 2_912, observations
        )

    def test_fault_statistics_require_a_retransmission(self) -> None:
        scenario = next(
            item
            for item in run_file_interop.scenario_matrix(
                self.robotweax, self.reference
            )
            if item.recovery == "NAK"
        )
        required = (
            self.options.byte_count
            + run_file_interop.FILE_PAYLOAD_SIZE
            - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        with self.assertRaisesRegex(RuntimeError, "retransmission"):
            run_file_interop.validate_statistics(
                scenario,
                {
                    "stats": {
                        "pktSentTotal": required,
                        "pktSndDropTotal": 0,
                        "pktRetransTotal": 0,
                    }
                },
                {"stats": {"pktRecvTotal": required}},
                self.options.byte_count,
            )

    def test_fault_statistics_require_nak_but_allow_rto_noise(
        self,
    ) -> None:
        scenarios = run_file_interop.scenario_matrix(
            self.robotweax, self.reference
        )
        for recovery, nak_count, loss_count in (
            ("NAK", 1, 1),
            ("RTO/LATEREXMIT", 2, 3),
        ):
            scenario = next(
                item
                for item in scenarios
                if item.recovery == recovery
            )
            byte_count = run_file_interop.scenario_byte_count(
                scenario, self.options
            )
            required = (
                byte_count + run_file_interop.FILE_PAYLOAD_SIZE - 1
            ) // run_file_interop.FILE_PAYLOAD_SIZE
            run_file_interop.validate_statistics(
                scenario,
                {
                    "stats": {
                        "pktSentTotal": required + 1,
                        "pktSndDropTotal": 0,
                        "pktRetransTotal": 1,
                        "pktRecvNAKTotal": nak_count,
                        "pktSndLossTotal": loss_count,
                    }
                },
                {
                    "stats": {
                        "pktRecvTotal": required,
                        "pktSentNAKTotal": nak_count,
                    }
                },
                byte_count,
            )
        nak_scenario = next(
            item for item in scenarios if item.recovery == "NAK"
        )
        required = (
            self.options.byte_count
            + run_file_interop.FILE_PAYLOAD_SIZE
            - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        with self.assertRaisesRegex(
            RuntimeError, "did not report NAK recovery"
        ):
            run_file_interop.validate_statistics(
                nak_scenario,
                {
                    "stats": {
                        "pktSentTotal": required + 1,
                        "pktSndDropTotal": 0,
                        "pktRetransTotal": 1,
                        "pktRecvNAKTotal": 0,
                        "pktSndLossTotal": 0,
                    }
                },
                {
                    "stats": {
                        "pktRecvTotal": required,
                        "pktSentNAKTotal": 0,
                    }
                },
                self.options.byte_count,
            )

    def test_causal_gap_statistics_require_reported_nak(self) -> None:
        scenario = next(
            item
            for item in (
                run_file_interop.rendezvous_resilience_scenario_matrix(
                    self.robotweax,
                    self.reference,
                )
            )
            if item.recovery == "CAUSAL"
        )
        byte_count = run_file_interop.scenario_byte_count(
            scenario, self.options
        )
        required = (
            byte_count + run_file_interop.FILE_PAYLOAD_SIZE - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        sender_stats = {
            "stats": {
                "pktSentTotal": required + 1,
                "pktSndDropTotal": 0,
                "pktRetransTotal": 1,
                "pktRecvNAKTotal": 0,
                "pktSndLossTotal": 0,
            }
        }
        receiver_stats = {
            "stats": {
                "pktRecvTotal": required,
                "pktSentNAKTotal": 0,
            }
        }
        observations = [
            {
                "later_data_observed_before_retransmission": True,
            }
        ]
        with self.assertRaisesRegex(
            RuntimeError, "did not report NAK recovery"
        ):
            run_file_interop.validate_statistics(
                scenario,
                sender_stats,
                receiver_stats,
                byte_count,
                observations,
            )
        sender_stats["stats"]["pktRecvNAKTotal"] = 1
        sender_stats["stats"]["pktSndLossTotal"] = 1
        receiver_stats["stats"]["pktSentNAKTotal"] = 1
        run_file_interop.validate_statistics(
            scenario,
            sender_stats,
            receiver_stats,
            byte_count,
            observations,
        )

    def test_burst_fault_plan_requires_every_nak_retransmission(
        self,
    ) -> None:
        scenario = run_file_interop.resilience_scenario_matrix(
            self.robotweax, self.reference
        )[1]
        observations = [
            {
                "action": fault.action,
                "direction": fault.direction,
                "occurrence": fault.occurrence,
                "sequence": 100 + index,
                "payload_bytes": 1_456,
                "retransmission_observed": True,
                "retransmission_payload_bytes": 1_456,
                "retransmission_ciphertext_matches": True,
                "loss_report_observed": True,
                "loss_report_relay_ordinal": 10 + index,
                "retransmission_relay_ordinal": 20 + index,
            }
            for index, fault in enumerate(
                run_file_interop.scenario_faults(scenario)
            )
        ]

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return observations

        validated = run_file_interop.validate_fault_plan(
            scenario,
            Relay(),
            run_file_interop.scenario_byte_count(
                scenario, self.options
            ),
        )
        self.assertEqual(len(validated), 3)
        observations[1]["retransmission_observed"] = False
        with self.assertRaisesRegex(
            RuntimeError, "was not retransmitted"
        ):
            run_file_interop.validate_fault_plan(
                scenario,
                Relay(),
                run_file_interop.scenario_byte_count(
                    scenario, self.options
                ),
            )

    def test_rendezvous_causal_plan_requires_rto_for_unexposed_gap(
        self,
    ) -> None:
        scenario = next(
            item
            for item in (
                run_file_interop.rendezvous_resilience_scenario_matrix(
                    self.robotweax,
                    self.reference,
                )
            )
            if item.recovery == "CAUSAL"
        )
        fault = run_file_interop.scenario_faults(scenario)[0]
        observation = {
            "action": fault.action,
            "direction": fault.direction,
            "occurrence": fault.occurrence,
            "sequence": 100,
            "payload_bytes": 1_456,
            "retransmission_observed": True,
            "retransmission_payload_bytes": 1_456,
            "retransmission_ciphertext_matches": True,
            "loss_report_observed": False,
            "later_data_observed_before_retransmission": False,
            "retransmission_relay_ordinal": 20,
            "retransmission_delay_microseconds": 40_000,
        }

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [observation]

        self.assertEqual(
            run_file_interop.validate_fault_plan(
                scenario,
                Relay(),
                run_file_interop.scenario_byte_count(
                    scenario, self.options
                ),
            ),
            [observation],
        )
        observation["retransmission_delay_microseconds"] = 1
        with self.assertRaisesRegex(RuntimeError, "delayed sender RTO"):
            run_file_interop.validate_fault_plan(
                scenario,
                Relay(),
                run_file_interop.scenario_byte_count(
                    scenario, self.options
                ),
            )
        observation["retransmission_delay_microseconds"] = 40_000
        observation["loss_report_observed"] = True
        observation["loss_report_relay_ordinal"] = 19
        with self.assertRaisesRegex(RuntimeError, "delayed sender RTO"):
            run_file_interop.validate_fault_plan(
                scenario,
                Relay(),
                run_file_interop.scenario_byte_count(
                    scenario, self.options
                ),
            )

    def test_rendezvous_causal_plan_requires_nak_for_exposed_gap(
        self,
    ) -> None:
        scenario = next(
            item
            for item in (
                run_file_interop.rendezvous_resilience_scenario_matrix(
                    self.robotweax,
                    self.reference,
                )
            )
            if item.recovery == "CAUSAL"
        )
        fault = run_file_interop.scenario_faults(scenario)[0]
        observation = {
            "action": fault.action,
            "direction": fault.direction,
            "occurrence": fault.occurrence,
            "sequence": 100,
            "payload_bytes": 1_456,
            "relay_ordinal": 10,
            "later_data_observed_before_retransmission": True,
            "first_later_data_sequence": 101,
            "first_later_data_relay_ordinal": 11,
            "loss_report_observed": True,
            "loss_report_relay_ordinal": 12,
            "retransmission_observed": True,
            "retransmission_relay_ordinal": 13,
            "retransmission_delay_microseconds": 400,
            "retransmission_payload_bytes": 1_456,
            "retransmission_ciphertext_matches": True,
        }

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [observation]

        self.assertEqual(
            run_file_interop.validate_fault_plan(
                scenario,
                Relay(),
                run_file_interop.scenario_byte_count(
                    scenario, self.options
                ),
            ),
            [observation],
        )
        observation["loss_report_observed"] = False
        with self.assertRaisesRegex(RuntimeError, "matching NAK"):
            run_file_interop.validate_fault_plan(
                scenario,
                Relay(),
                run_file_interop.scenario_byte_count(
                    scenario, self.options
                ),
            )

    def test_ack_delay_plan_requires_complete_release_window(
        self,
    ) -> None:
        scenario = run_file_interop.resilience_scenario_matrix(
            self.robotweax, self.reference
        )[3]
        fault = run_file_interop.scenario_faults(scenario)[0]
        observation = {
            "action": fault.action,
            "direction": fault.direction,
            "occurrence": fault.occurrence,
            "control_type": 2,
            "delay_released": True,
            "delayed_datagrams": 128,
            "delay_elapsed_milliseconds": 205.0,
        }

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [observation]

        self.assertEqual(
            run_file_interop.validate_fault_plan(
                scenario,
                Relay(),
                run_file_interop.scenario_byte_count(
                    scenario, self.options
                ),
            ),
            [observation],
        )
        observation["delay_released"] = False
        with self.assertRaisesRegex(
            RuntimeError, "delayed control window"
        ):
            run_file_interop.validate_fault_plan(
                scenario,
                Relay(),
                run_file_interop.scenario_byte_count(
                    scenario, self.options
                ),
            )

    def test_dynamic_maxbw_validation_requires_two_rate_corridors(
        self,
    ) -> None:
        scenario = run_file_interop.resilience_scenario_matrix(
            self.robotweax, self.reference
        )[0]
        phase_bytes = scenario.byte_count // 2
        output = "\n".join(
            (
                json.dumps(
                    {
                        "event": "rate_change",
                        "bytes": phase_bytes,
                        "old_max_bw": 150_000,
                        "new_max_bw": 600_000,
                    }
                ),
                json.dumps(
                    {
                        "event": "rate_phases",
                        "first_bytes": phase_bytes,
                        "first_elapsed_us":
                            phase_bytes * 1_000_000 // 150_000,
                        "first_max_bw": 150_000,
                        "second_bytes": phase_bytes,
                        "second_elapsed_us":
                            phase_bytes * 1_000_000 // 600_000,
                        "second_max_bw": 600_000,
                    }
                ),
            )
        )
        first, second = (
            run_file_interop.validate_dynamic_maximum_bandwidth(
                scenario, output
            )
        )
        self.assertGreater(second, first * 3)
        with self.assertRaisesRegex(
            RuntimeError, "expected corridors"
        ):
            run_file_interop.validate_dynamic_maximum_bandwidth(
                scenario,
                output.replace(
                    f'"second_elapsed_us": '
                    f'{phase_bytes * 1_000_000 // 600_000}',
                    f'"second_elapsed_us": '
                    f'{phase_bytes * 1_000_000 // 200_000}',
                ),
            )

    def test_caller_listener_fault_proxy_drops_and_observes_resend(
        self,
    ) -> None:
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[tuple[bytes, tuple[str, int]]] = []

            def sendto(
                self, payload: bytes, target: tuple[str, int]
            ) -> None:
                self.sent.append((payload, target))

        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=1,
        )
        proxy = object.__new__(
            srt_handshake_trace.CallerListenerFaultProxy
        )
        srt_handshake_trace._HandshakeTraceRecorder.__init__(proxy)
        fake_socket = FakeSocket()
        proxy._socket = fake_socket
        proxy._target = ("127.0.0.1", 10_001)
        proxy._source = ("127.0.0.1", 10_002)
        proxy._initialize_fault_state(fault)
        packet = (
            (17).to_bytes(4, "big")
            + (0).to_bytes(4, "big")
            + bytes(8)
            + b"filecc"
        )
        proxy._forward(packet, "sender_to_receiver")
        self.assertEqual(fake_socket.sent, [])
        loss_report = (
            (0x8003_0000).to_bytes(4, "big")
            + bytes(12)
            + (17).to_bytes(4, "big")
        )
        proxy._forward(loss_report, "receiver_to_sender")
        proxy._forward(packet, "sender_to_receiver")
        self.assertEqual(
            fake_socket.sent[-1],
            (packet, ("127.0.0.1", 10_001)),
        )
        proxy._forward(b"ack", "receiver_to_sender")
        self.assertEqual(
            fake_socket.sent[-1],
            (b"ack", ("127.0.0.1", 10_002)),
        )
        observation = proxy.fault_observation()
        self.assertIsNotNone(observation)
        self.assertTrue(observation["retransmission_observed"])
        self.assertFalse(observation["retransmission_flag"])
        self.assertTrue(observation["loss_report_observed"])
        self.assertLess(
            observation["loss_report_relay_ordinal"],
            observation["retransmission_relay_ordinal"],
        )
        self.assertEqual(observation["sequence"], 17)

    def test_caller_listener_records_clear_flag_retransmission_bytes(
        self,
    ) -> None:
        proxy = srt_handshake_trace.CallerListenerFaultProxy(10_001)
        original = {
            "packet_kind": "data",
            "sequence": 17,
            "message_number": 1,
            "key_selection": 2,
            "payload_bytes": 1_456,
            "payload_sha256": "a" * 64,
            "relay_ordinal": 1,
            "relay_monotonic_ns": 1_000_000_000,
        }

        proxy._record_retransmission("sender_to_receiver", original)
        proxy._record_retransmission(
            "sender_to_receiver",
            {
                **original,
                "relay_ordinal": 3,
                "relay_monotonic_ns": 1_020_000_000,
            },
        )

        self.assertEqual(
            proxy.retransmission_observations("sender_to_receiver"),
            [
                {
                    "direction": "sender_to_receiver",
                    "sequence": 17,
                    "message_number": 1,
                    "key_selection": 2,
                    "payload_bytes": 1_456,
                    "payload_sha256": "a" * 64,
                    "relay_ordinal": 3,
                    "original_observed": True,
                    "original_relay_ordinal": 1,
                    "retransmission_delay_microseconds": 20_000,
                    "prior_cumulative_ack_next_sequence": None,
                    "prior_cumulative_ack_relay_ordinal": None,
                    "original_key_selection": 2,
                    "original_payload_bytes": 1_456,
                    "original_payload_sha256": "a" * 64,
                    "ciphertext_matches_original": True,
                }
            ],
        )

    def test_file_rendezvous_proxy_observes_clear_flag_retransmission(
        self,
    ) -> None:
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[tuple[bytes, tuple[str, int]]] = []

            def sendto(
                self, payload: bytes, target: tuple[str, int]
            ) -> None:
                self.sent.append((payload, target))

        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=1,
        )
        proxy = object.__new__(
            srt_handshake_trace.FileRendezvousTraceProxy
        )
        srt_handshake_trace._HandshakeTraceRecorder.__init__(proxy)
        proxy._sender_socket = FakeSocket()
        proxy._receiver_socket = FakeSocket()
        proxy._sender_target = ("127.0.0.1", 10_001)
        proxy._receiver_target = ("127.0.0.1", 10_002)
        proxy._initialize_fault_state(fault)
        packet = (
            (17).to_bytes(4, "big")
            + (0).to_bytes(4, "big")
            + bytes(8)
            + b"filecc-rendezvous"
        )
        proxy._forward(packet, "sender_to_receiver")
        loss_report = (
            (0x8003_0000).to_bytes(4, "big")
            + bytes(12)
            + (17).to_bytes(4, "big")
        )
        proxy._forward(loss_report, "receiver_to_sender")
        proxy._forward(packet, "sender_to_receiver")

        observation = proxy.fault_observation()
        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertTrue(observation["retransmission_observed"])
        self.assertFalse(observation["retransmission_flag"])
        self.assertTrue(observation["loss_report_observed"])
        self.assertFalse(
            observation[
                "later_data_observed_before_retransmission"
            ]
        )
        self.assertEqual(observation["sequence"], 17)

    def test_file_rendezvous_proxy_records_wrap_safe_gap_evidence(
        self,
    ) -> None:
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[tuple[bytes, tuple[str, int]]] = []

            def sendto(
                self, payload: bytes, target: tuple[str, int]
            ) -> None:
                self.sent.append((payload, target))

        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=1,
        )
        proxy = object.__new__(
            srt_handshake_trace.FileRendezvousTraceProxy
        )
        srt_handshake_trace._HandshakeTraceRecorder.__init__(proxy)
        proxy._sender_socket = FakeSocket()
        proxy._receiver_socket = FakeSocket()
        proxy._sender_target = ("127.0.0.1", 10_001)
        proxy._receiver_target = ("127.0.0.1", 10_002)
        proxy._initialize_fault_state(fault)

        def packet(sequence: int) -> bytes:
            return (
                sequence.to_bytes(4, "big")
                + (0).to_bytes(4, "big")
                + bytes(8)
                + b"filecc-rendezvous"
            )

        dropped_sequence = srt_handshake_trace.SEQUENCE_MASK
        proxy._forward(
            packet(dropped_sequence), "sender_to_receiver"
        )
        proxy._forward(packet(0), "sender_to_receiver")
        loss_report = (
            (0x8003_0000).to_bytes(4, "big")
            + bytes(12)
            + dropped_sequence.to_bytes(4, "big")
        )
        proxy._forward(loss_report, "receiver_to_sender")
        proxy._forward(
            packet(dropped_sequence), "sender_to_receiver"
        )

        observation = proxy.fault_observation()
        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertTrue(
            observation[
                "later_data_observed_before_retransmission"
            ]
        )
        self.assertEqual(observation["first_later_data_sequence"], 0)
        self.assertLess(
            observation["first_later_data_relay_ordinal"],
            observation["loss_report_relay_ordinal"],
        )
        self.assertLess(
            observation["loss_report_relay_ordinal"],
            observation["retransmission_relay_ordinal"],
        )

    def test_dropped_later_data_does_not_expose_a_filecc_gap(
        self,
    ) -> None:
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[tuple[bytes, tuple[str, int]]] = []

            def sendto(
                self, payload: bytes, target: tuple[str, int]
            ) -> None:
                self.sent.append((payload, target))

        faults = tuple(
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=occurrence,
            )
            for occurrence in (1, 2, 3)
        )
        proxy = object.__new__(
            srt_handshake_trace.FileRendezvousTraceProxy
        )
        srt_handshake_trace._HandshakeTraceRecorder.__init__(proxy)
        proxy._sender_socket = FakeSocket()
        proxy._receiver_socket = FakeSocket()
        proxy._sender_target = ("127.0.0.1", 10_001)
        proxy._receiver_target = ("127.0.0.1", 10_002)
        proxy._initialize_fault_state(faults)

        def packet(sequence: int) -> bytes:
            return (
                sequence.to_bytes(4, "big")
                + (0).to_bytes(4, "big")
                + bytes(8)
                + b"filecc-rendezvous"
            )

        packets = [packet(sequence) for sequence in (17, 18, 19)]
        for payload in packets:
            proxy._forward(payload, "sender_to_receiver")

        self.assertEqual(proxy._receiver_socket.sent, [])
        self.assertEqual(
            [
                observation[
                    "later_data_observed_before_retransmission"
                ]
                for observation in proxy.fault_observations()
            ],
            [False, False, False],
        )

        for payload in packets:
            proxy._forward(payload, "sender_to_receiver")

        observations = proxy.fault_observations()
        self.assertTrue(
            all(
                observation["retransmission_observed"]
                for observation in observations
            )
        )
        self.assertEqual(
            [
                observation[
                    "later_data_observed_before_retransmission"
                ]
                for observation in observations
            ],
            [False, False, False],
        )
        self.assertEqual(
            [payload for payload, _ in proxy._receiver_socket.sent],
            packets,
        )

    def test_file_rendezvous_fault_occurrences_ignore_clear_flag_repeats(
        self,
    ) -> None:
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[tuple[bytes, tuple[str, int]]] = []

            def sendto(
                self, payload: bytes, target: tuple[str, int]
            ) -> None:
                self.sent.append((payload, target))

        faults = tuple(
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=occurrence,
            )
            for occurrence in (2, 3)
        )
        proxy = object.__new__(
            srt_handshake_trace.FileRendezvousTraceProxy
        )
        srt_handshake_trace._HandshakeTraceRecorder.__init__(proxy)
        proxy._sender_socket = FakeSocket()
        proxy._receiver_socket = FakeSocket()
        proxy._sender_target = ("127.0.0.1", 10_001)
        proxy._receiver_target = ("127.0.0.1", 10_002)
        proxy._initialize_fault_state(faults)

        def packet(sequence: int) -> bytes:
            return (
                sequence.to_bytes(4, "big")
                + (0).to_bytes(4, "big")
                + bytes(8)
                + b"filecc-rendezvous"
            )

        proxy._forward(packet(17), "sender_to_receiver")
        proxy._forward(packet(17), "sender_to_receiver")
        proxy._forward(packet(18), "sender_to_receiver")
        proxy._forward(packet(18), "sender_to_receiver")
        proxy._forward(packet(19), "sender_to_receiver")

        observations = proxy.fault_observations()
        self.assertEqual(
            [item["sequence"] for item in observations],
            [18, 19],
        )
        self.assertEqual(proxy._fault_matches, [2, 3])

    def test_file_rendezvous_records_clear_flag_retransmission_bytes(
        self,
    ) -> None:
        proxy = object.__new__(
            srt_handshake_trace.FileRendezvousTraceProxy
        )
        srt_handshake_trace._HandshakeTraceRecorder.__init__(proxy)
        proxy._initialize_fault_state(None)
        original = {
            "packet_kind": "data",
            "sequence": 17,
            "message_number": 1,
            "key_selection": 2,
            "payload_bytes": 1_456,
            "payload_sha256": "a" * 64,
            "relay_ordinal": 1,
        }

        proxy._record_retransmission("sender_to_receiver", original)
        proxy._record_retransmission(
            "sender_to_receiver",
            {**original, "relay_ordinal": 3},
        )

        self.assertEqual(
            proxy.retransmission_observations("sender_to_receiver"),
            [
                {
                    "direction": "sender_to_receiver",
                    "sequence": 17,
                    "message_number": 1,
                    "key_selection": 2,
                    "payload_bytes": 1_456,
                    "payload_sha256": "a" * 64,
                    "relay_ordinal": 3,
                    "original_observed": True,
                    "original_relay_ordinal": 1,
                    "retransmission_delay_microseconds": None,
                    "prior_cumulative_ack_next_sequence": None,
                    "prior_cumulative_ack_relay_ordinal": None,
                    "original_key_selection": 2,
                    "original_payload_bytes": 1_456,
                    "original_payload_sha256": "a" * 64,
                    "ciphertext_matches_original": True,
                }
            ],
        )

    def test_sequence_translation_rewrites_every_reliability_field(
        self,
    ) -> None:
        delta = 0x7FFF_FF00 - 17
        handshake = (
            (0x8000_0000).to_bytes(4, "big")
            + bytes(20)
            + (17).to_bytes(4, "big")
            + bytes(36)
        )
        translated = srt_handshake_trace.translate_sequence_datagram(
            handshake, delta
        )
        self.assertEqual(
            int.from_bytes(translated[24:28], "big"),
            0x7FFF_FF00,
        )

        data = (
            (18).to_bytes(4, "big")
            + bytes(12)
            + b"payload"
        )
        translated = srt_handshake_trace.translate_sequence_datagram(
            data, delta
        )
        self.assertEqual(
            int.from_bytes(translated[:4], "big"),
            0x7FFF_FF01,
        )

        acknowledgement = (
            (0x8002_0000).to_bytes(4, "big")
            + bytes(12)
            + (0).to_bytes(4, "big")
        )
        translated = srt_handshake_trace.translate_sequence_datagram(
            acknowledgement, -delta
        )
        self.assertEqual(
            int.from_bytes(translated[16:20], "big"),
            (0 - delta) & 0x7FFF_FFFF,
        )

        loss_report = (
            (0x8003_0000).to_bytes(4, "big")
            + bytes(12)
            + (0xFFFF_FFFE).to_bytes(4, "big")
            + (1).to_bytes(4, "big")
        )
        translated = srt_handshake_trace.translate_sequence_datagram(
            loss_report, 2
        )
        self.assertEqual(
            int.from_bytes(translated[16:20], "big"),
            0x8000_0000,
        )
        self.assertEqual(
            int.from_bytes(translated[20:24], "big"),
            3,
        )

        drop_request = (
            (0x8007_0000).to_bytes(4, "big")
            + bytes(12)
            + (0x7FFF_FFFF).to_bytes(4, "big")
            + (0).to_bytes(4, "big")
        )
        translated = srt_handshake_trace.translate_sequence_datagram(
            drop_request, 1
        )
        self.assertEqual(
            translated[16:24],
            bytes(4) + (1).to_bytes(4, "big"),
        )

    def test_sequence_proxy_matches_nak_and_retransmission_after_wrap(
        self,
    ) -> None:
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[tuple[bytes, tuple[str, int]]] = []

            def sendto(
                self, payload: bytes, target: tuple[str, int]
            ) -> None:
                self.sent.append((payload, target))

        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=2,
        )
        proxy = srt_handshake_trace.CallerListenerSequenceProxy(
            10_001,
            0x7FFF_FFFF,
            fault,
        )
        fake_socket = FakeSocket()
        proxy._socket = fake_socket
        proxy._source = ("127.0.0.1", 10_002)

        handshake = (
            (0x8000_0000).to_bytes(4, "big")
            + bytes(20)
            + (17).to_bytes(4, "big")
            + bytes(36)
        )
        proxy._forward(handshake, "sender_to_receiver")
        self.assertEqual(
            int.from_bytes(fake_socket.sent[-1][0][24:28], "big"),
            0x7FFF_FFFF,
        )

        first = (
            (17).to_bytes(4, "big")
            + bytes(12)
            + b"first"
        )
        wrapped = (
            (18).to_bytes(4, "big")
            + bytes(12)
            + b"wrapped"
        )
        proxy._forward(first, "sender_to_receiver")
        sent_before_drop = len(fake_socket.sent)
        proxy._forward(wrapped, "sender_to_receiver")
        self.assertEqual(len(fake_socket.sent), sent_before_drop)

        loss_report = (
            (0x8003_0000).to_bytes(4, "big")
            + bytes(12)
            + bytes(4)
        )
        proxy._forward(loss_report, "receiver_to_sender")
        self.assertEqual(
            int.from_bytes(fake_socket.sent[-1][0][16:20], "big"),
            18,
        )
        proxy._forward(wrapped, "sender_to_receiver")
        self.assertEqual(
            int.from_bytes(fake_socket.sent[-1][0][:4], "big"),
            0,
        )

        observation = proxy.fault_observation()
        self.assertIsNotNone(observation)
        self.assertEqual(observation["sequence"], 0)
        self.assertTrue(observation["loss_report_observed"])
        self.assertTrue(observation["retransmission_observed"])
        sequence_observation = proxy.sequence_observation()
        self.assertTrue(sequence_observation["data_wrap_observed"])

    def test_rollover_validation_requires_native_and_translated_evidence(
        self,
    ) -> None:
        native = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )[0]
        native_packets = (
            run_file_interop.scenario_byte_count(native, self.options)
            + run_file_interop.FILE_PAYLOAD_SIZE
            - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        native_isn = run_file_interop.ROLLOVER_MINIMUM_ISN
        native_result = run_file_interop.validate_rollover(
            native,
            srt_handshake_trace.CallerListenerFaultProxy(10_001),
            {"isn": native_isn},
            {"isn": native_isn},
            run_file_interop.scenario_byte_count(native, self.options),
            {"sequence": 1},
        )
        self.assertEqual(native_result["packets"], native_packets)
        self.assertLess(native_result["end_sequence"], native_packets)

        traced_native = srt_handshake_trace.CallerListenerFaultProxy(
            10_001
        )
        traced_native._entries["listener-conclusion"] = {
            "direction": "receiver_to_sender",
            "request": -1,
            "initial_sequence": native_isn,
        }
        traced_result = run_file_interop.validate_rollover(
            native,
            traced_native,
            {"isn": native_isn},
            {},
            run_file_interop.scenario_byte_count(native, self.options),
            {"sequence": 1},
        )
        self.assertEqual(traced_result["listener_isn"], native_isn)
        traced_native._entries["conflicting-listener-conclusion"] = {
            "direction": "receiver_to_sender",
            "request": -1,
            "initial_sequence": native_isn + 1,
        }
        with self.assertRaisesRegex(
            RuntimeError, "listener SRTO_ISN is unavailable"
        ):
            run_file_interop.validate_rollover(
                native,
                traced_native,
                {"isn": native_isn},
                {},
                run_file_interop.scenario_byte_count(
                    native, self.options
                ),
                {"sequence": 1},
            )

        translated = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )[2]
        translated_packets = (
            run_file_interop.scenario_byte_count(
                translated, self.options
            )
            + run_file_interop.FILE_PAYLOAD_SIZE
            - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        relay = srt_handshake_trace.CallerListenerSequenceProxy(
            10_001,
            run_file_interop.TRANSLATED_ROLLOVER_ISN,
        )
        relay._source_initial_sequence = 123
        relay._translation_delta = (
            run_file_interop.TRANSLATED_ROLLOVER_ISN - 123
        ) & run_file_interop.SEQUENCE_MASK
        relay._translated_data_packets = translated_packets + 1
        relay._translated_acknowledgements = 2
        relay._first_translated_sequence = (
            run_file_interop.TRANSLATED_ROLLOVER_ISN
        )
        relay._last_translated_sequence = 191
        relay._data_wrap_observed = True
        relay._ack_wrap_observed = True
        translated_result = run_file_interop.validate_rollover(
            translated,
            relay,
            {"isn": 123},
            {"isn": run_file_interop.TRANSLATED_ROLLOVER_ISN},
            run_file_interop.scenario_byte_count(
                translated, self.options
            ),
            {"sequence": 63},
        )
        self.assertEqual(
            translated_result["wire_isn"],
            run_file_interop.TRANSLATED_ROLLOVER_ISN,
        )
        relay._ack_wrap_observed = False
        with self.assertRaisesRegex(
            RuntimeError, "evidence is incomplete"
        ):
            run_file_interop.validate_rollover(
                translated,
                relay,
                {"isn": 123},
                {
                    "isn":
                        run_file_interop.TRANSLATED_ROLLOVER_ISN
                },
                run_file_interop.scenario_byte_count(
                    translated, self.options
                ),
                {"sequence": 63},
            )

    def test_matching_loss_report_requires_strict_order(self) -> None:
        self.assertTrue(
            run_file_interop.matching_loss_report_precedes_retransmission(
                {
                    "loss_report_relay_ordinal": 3,
                    "retransmission_relay_ordinal": 4,
                }
            )
        )
        self.assertFalse(
            run_file_interop.matching_loss_report_precedes_retransmission(
                {
                    "loss_report_relay_ordinal": 5,
                    "retransmission_relay_ordinal": 4,
                }
            )
        )

    def test_tail_recovery_validation_requires_the_flight_tail(self) -> None:
        scenario = next(
            item
            for item in run_file_interop.scenario_matrix(
                self.robotweax, self.reference
            )
            if item.recovery == "RTO/LATEREXMIT"
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
                        "occurrence": 2,
                        "payload_bytes": 997,
                        "relay_monotonic_ns": 1_000_000,
                        "retransmission_observed": True,
                        "retransmission_relay_ordinal": 4,
                        "retransmission_delay_microseconds": 20_000,
                        "retransmission_payload_bytes": 997,
                        "retransmission_ciphertext_matches": True,
                    }
                ]

        observation = run_file_interop.validate_fault_recovery(
            scenario,
            Relay(),
            run_file_interop.FILE_PAYLOAD_SIZE + 997,
        )
        self.assertEqual(observation["payload_bytes"], 997)
        rendezvous = next(
            item
            for item in (
                run_file_interop.rendezvous_resilience_scenario_matrix(
                    self.robotweax,
                    self.reference,
                )
            )
            if item.recovery == "RTO/LATEREXMIT"
        )
        self.assertEqual(
            run_file_interop.validate_fault_recovery(
                rendezvous,
                Relay(),
                run_file_interop.FILE_PAYLOAD_SIZE + 997,
            )["payload_bytes"],
            997,
        )
        with self.assertRaisesRegex(RuntimeError, "sender RTO"):
            run_file_interop.validate_fault_recovery(
                scenario,
                Relay(),
                run_file_interop.FILE_PAYLOAD_SIZE + 998,
            )

    def test_rollover_rto_validation_targets_the_final_wrapped_packet(
        self,
    ) -> None:
        scenario = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )[1]
        byte_count = run_file_interop.scenario_byte_count(
            scenario, self.options
        )
        packet_count = (
            byte_count + run_file_interop.FILE_PAYLOAD_SIZE - 1
        ) // run_file_interop.FILE_PAYLOAD_SIZE
        tail_bytes = (
            byte_count
            - (packet_count - 1)
            * run_file_interop.FILE_PAYLOAD_SIZE
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
                        "sequence": 5_952,
                        "payload_bytes": tail_bytes,
                        "relay_monotonic_ns": 1_000_000,
                        "retransmission_observed": True,
                        "retransmission_relay_ordinal": packet_count + 2,
                        "retransmission_delay_microseconds": 20_000,
                        "retransmission_payload_bytes": tail_bytes,
                        "retransmission_ciphertext_matches": True,
                    }
                ]

        observation = run_file_interop.validate_fault_recovery(
            scenario, Relay(), byte_count
        )
        self.assertEqual(observation["payload_bytes"], tail_bytes)

    def test_translated_rollover_rto_accepts_the_short_wrapped_tail(
        self,
    ) -> None:
        scenario = run_file_interop.rollover_scenario_matrix(
            self.robotweax, self.reference
        )[3]
        byte_count = run_file_interop.scenario_byte_count(
            scenario, self.options
        )
        tail_bytes = byte_count - run_file_interop.FILE_PAYLOAD_SIZE

        class Relay:
            payload_bytes = tail_bytes

            @staticmethod
            def error() -> None:
                return None

            @classmethod
            def fault_observations(cls) -> list[dict[str, object]]:
                return [
                    {
                        "action": "drop",
                        "direction": "sender_to_receiver",
                        "occurrence": 2,
                        "sequence": 0,
                        "payload_bytes": cls.payload_bytes,
                        "relay_monotonic_ns": 1_000_000,
                        "retransmission_observed": True,
                        "retransmission_relay_ordinal": 4,
                        "retransmission_delay_microseconds": 20_000,
                        "retransmission_payload_bytes": cls.payload_bytes,
                        "retransmission_ciphertext_matches": True,
                    }
                ]

        observation = run_file_interop.validate_fault_recovery(
            scenario, Relay(), byte_count
        )
        self.assertEqual(observation["payload_bytes"], tail_bytes)

        Relay.payload_bytes += 1
        with self.assertRaisesRegex(RuntimeError, "sender RTO"):
            run_file_interop.validate_fault_recovery(
                scenario, Relay(), byte_count
            )

    def test_failure_report_contains_payload_location(self) -> None:
        scenario = run_file_interop.scenario_matrix(
            self.robotweax, self.reference
        )[0]
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            expected = work / "expected"
            actual = work / "actual"
            expected.write_bytes(b"a" * 2_000)
            actual.write_bytes(b"a" * 1_500 + b"b" + b"a" * 499)
            report = run_file_interop.render_failure(
                scenario,
                "digest mismatch",
                expected,
                actual,
            )
        self.assertIn("first_mismatch_offset=1500", report)
        self.assertIn("packet_occurrence=2", report)


if __name__ == "__main__":
    unittest.main()
