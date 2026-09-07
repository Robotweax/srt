from __future__ import annotations

import json
import socket
import struct
import sys
import tempfile
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import interop_common  # noqa: E402
import run_rendezvous_interop  # noqa: E402
import srt_handshake_trace  # noqa: E402


class RendezvousInteropUnitTests(unittest.TestCase):
    def setUp(self) -> None:
        self.options = run_rendezvous_interop.RunOptions(
            byte_count=4_194_304,
            timeout_seconds=30,
            key_refresh_rate=1_000,
            key_preannouncement=400,
        )

    def test_reserved_endpoint_ports_cannot_be_acquired_by_relay(
        self,
    ) -> None:
        with interop_common.reserved_udp_ports(2) as endpoint_ports:
            proxy = srt_handshake_trace.RendezvousTraceProxy(
                endpoint_ports[0], endpoint_ports[1]
            )
        try:
            self.assertEqual(len(set(endpoint_ports)), 2)
            self.assertTrue(
                set(endpoint_ports).isdisjoint(
                    {proxy.sender_port, proxy.receiver_port}
                )
            )
        finally:
            proxy.close()

    @staticmethod
    def make_fake_fault_proxy(
        fault: (
            srt_handshake_trace.RendezvousFault
            | tuple[srt_handshake_trace.RendezvousFault, ...]
        ),
    ) -> tuple[
        srt_handshake_trace.RendezvousTraceProxy,
        object,
        object,
    ]:
        class FakeSocket:
            def __init__(self) -> None:
                self.sent: list[tuple[bytes, tuple[str, int]]] = []

            def sendto(
                self, payload: bytes, target: tuple[str, int]
            ) -> None:
                self.sent.append((payload, target))

        proxy = object.__new__(
            srt_handshake_trace.RendezvousTraceProxy
        )
        srt_handshake_trace._HandshakeTraceRecorder.__init__(proxy)
        sender_socket = FakeSocket()
        receiver_socket = FakeSocket()
        proxy._sender_socket = sender_socket
        proxy._receiver_socket = receiver_socket
        proxy._sender_target = ("127.0.0.1", 10_001)
        proxy._receiver_target = ("127.0.0.1", 10_002)
        proxy._initialize_fault_state(fault)
        return proxy, sender_socket, receiver_socket

    @staticmethod
    def data_packet(
        sequence: int,
        *,
        message_number: int = 0,
        destination_socket_id: int = 0,
        key_selection: int = 0,
        retransmitted: bool = False,
        payload: bytes = b"payload",
    ) -> bytes:
        message_word = (
            (message_number & 0x03FF_FFFF)
            | (key_selection & 0x03) << 27
            | int(retransmitted) << 26
        )
        return (
            struct.pack(
                ">IIII", sequence, message_word, 0, destination_socket_id
            )
            + payload
        )

    @staticmethod
    def handshake_packet(
        request: int,
        cookie: int = 0x20,
        socket_id: int = 10,
    ) -> bytes:
        return (
            struct.pack(">IIII", 0x8000_0000, 0, 0, 0)
            + struct.pack(
                ">IIIIIiII4I",
                5,
                0,
                1_000,
                1_500,
                25_600,
                request,
                socket_id,
                cookie,
                0,
                0,
                0,
                0,
            )
        )

    @staticmethod
    def runtime_key_material_packet(
        subtype: int,
        key_selection: int = 3,
    ) -> bytes:
        key_words = 8
        salt_words = 4
        key_count = 2 if key_selection == 3 else 1
        content = bytearray(
            16
            + salt_words * 4
            + key_count * key_words * 4
            + 8
        )
        content[0] = 0x21
        content[1:3] = b"\x20\x29"
        content[3] = key_selection
        content[8] = 2
        content[10] = 2
        content[14] = salt_words
        content[15] = key_words
        return (
            struct.pack(
                ">IIII",
                0xFFFF_0000 | subtype,
                0,
                123,
                77,
            )
            + content
        )

    def test_matrix_covers_clear_and_every_key_length_both_ways(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_rendezvous_interop.scenario_matrix(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 8)
        self.assertEqual(
            [scenario.key_length for scenario in scenarios],
            [0, 0, 16, 16, 24, 24, 32, 32],
        )
        self.assertEqual(
            sum(scenario.sender == robotweax for scenario in scenarios), 4
        )
        self.assertEqual(
            sum(scenario.sender == reference for scenario in scenarios), 4
        )

    def test_ipv6_matrix_covers_baseline_encryption_and_faults_both_ways(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_rendezvous_interop.ipv6_scenario_matrix(
            robotweax, reference
        )

        self.assertEqual(len(scenarios), 6)
        self.assertTrue(
            all(scenario.name.startswith("ipv6-") for scenario in scenarios)
        )
        self.assertEqual(
            [scenario.key_length for scenario in scenarios],
            [0, 0, 32, 32, 32, 32],
        )
        self.assertEqual(
            {scenario.sender for scenario in scenarios},
            {robotweax, reference},
        )
        self.assertTrue(all(scenario.trace_roles for scenario in scenarios))
        fault_scenarios = [
            scenario for scenario in scenarios if scenario.faults
        ]
        self.assertEqual(len(fault_scenarios), 2)
        self.assertEqual(
            {
                (scenario.sender, scenario.faults[0].action)
                for scenario in fault_scenarios
            },
            {
                (robotweax, "drop"),
                (reference, "reorder"),
            },
        )

    def test_runtime_key_material_trace_is_structural_and_secret_safe(
        self,
    ) -> None:
        packet = self.runtime_key_material_packet(3)
        description = (
            srt_handshake_trace
            .describe_runtime_key_material_datagram(
                packet, "sender_to_receiver"
            )
        )
        self.assertIsNotNone(description)
        assert description is not None
        self.assertEqual(description["name"], "KMREQ")
        self.assertEqual(description["subtype"], 3)
        self.assertEqual(description["destination_socket_id"], 77)
        material = description["key_material"]
        self.assertIsInstance(material, dict)
        assert isinstance(material, dict)
        self.assertEqual(material["key_selection"], 3)
        self.assertEqual(material["content_bytes"], 104)
        self.assertTrue(material["content_length_matches"])
        self.assertIn("content_sha256", material)
        self.assertNotIn("salt", material)
        self.assertNotIn("wrapped_keys", material)

    def test_runtime_key_material_trace_preserves_event_order(
        self,
    ) -> None:
        proxy, _, _ = self.make_fake_fault_proxy(())
        proxy._record_runtime_key_material(
            self.runtime_key_material_packet(3),
            "sender_to_receiver",
        )
        proxy._record_runtime_key_material(
            self.runtime_key_material_packet(4),
            "receiver_to_sender",
        )

        events = []
        for line in proxy.render("rotation").splitlines():
            event = json.loads(line)
            if event.get("event") == "srt_runtime_key_material_trace":
                events.append(event)

        self.assertEqual(
            [
                (event["ordinal"], event["name"], event["direction"])
                for event in events
            ],
            [
                (1, "KMREQ", "sender_to_receiver"),
                (2, "KMRSP", "receiver_to_sender"),
            ],
        )

    def test_rendezvous_commands_bind_and_connect_opposite_ports(
        self,
    ) -> None:
        sender = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/input"),
            self.options,
            16,
        )
        receiver = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-receiver",
            10_002,
            10_001,
            Path("/output"),
            self.options,
            16,
        )

        sender_local = sender.index("--local-port")
        sender_peer = sender.index("--port")
        receiver_local = receiver.index("--local-port")
        receiver_peer = receiver.index("--port")
        self.assertEqual(sender[sender_local + 1], "10001")
        self.assertEqual(sender[sender_peer + 1], "10002")
        self.assertEqual(receiver[receiver_local + 1], "10002")
        self.assertEqual(receiver[receiver_peer + 1], "10001")
        self.assertIn("--input", sender)
        self.assertNotIn("--output", sender)
        self.assertIn("--output", receiver)
        self.assertNotIn("--input", receiver)
        self.assertIn("--input-bw", sender)
        input_bandwidth = sender.index("--input-bw")
        self.assertEqual(sender[input_bandwidth + 1], "8000000")
        self.assertNotIn("--input-bw", receiver)
        self.assertIn("--shutdown-grace-ms", sender)
        self.assertIn("--shutdown-grace-ms", receiver)

    def test_rendezvous_command_uses_selected_ipv6_host(self) -> None:
        command = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/input"),
            self.options,
            32,
            host="::1",
        )

        local_host = command.index("--local-host")
        peer_host = command.index("--host")
        self.assertEqual(command[local_host + 1], "::1")
        self.assertEqual(command[peer_host + 1], "::1")

    def test_rendezvous_command_sets_reorder_tolerance(self) -> None:
        command = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-receiver",
            10_002,
            10_001,
            Path("/output"),
            self.options,
            0,
            maximum_reorder_tolerance_packets=4,
            shutdown_grace_extra_milliseconds=250,
        )

        option = command.index("--loss-max-ttl")
        self.assertEqual(command[option + 1], "4")
        grace = command.index("--shutdown-grace-ms")
        self.assertEqual(command[grace + 1], "500")

    def test_source_pacing_is_explicit_and_sender_only(self) -> None:
        command = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/input"),
            self.options,
            0,
            source_pacing=True,
        )
        self.assertIn("--source-pacing", command)

        with self.assertRaisesRegex(
            ValueError, "source pacing requires a sender role"
        ):
            run_rendezvous_interop.peer_command(
                Path("/peer"),
                "rendezvous-receiver",
                10_002,
                10_001,
                Path("/output"),
                self.options,
                0,
                source_pacing=True,
            )

    def test_timing_matrix_covers_each_implementation_starting_first(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_rendezvous_interop.timing_matrix(
            robotweax, reference, 350
        )

        self.assertEqual(len(scenarios), 4)
        self.assertTrue(all(scenario.key_length == 32 for scenario in scenarios))
        self.assertTrue(all(scenario.trace_roles for scenario in scenarios))
        self.assertTrue(
            all(
                scenario.start_delay_milliseconds == 350
                for scenario in scenarios
            )
        )
        self.assertEqual(
            {
                (scenario.sender == robotweax, scenario.start_order)
                for scenario in scenarios
            },
            {
                (True, "sender-first"),
                (True, "receiver-first"),
                (False, "sender-first"),
                (False, "receiver-first"),
            },
        )

    def test_role_probe_cycles_direction_and_start_order(self) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = [
            run_rendezvous_interop.role_probe_scenario(
                robotweax, reference, attempt, 275
            )
            for attempt in range(4)
        ]

        self.assertEqual(
            [
                (scenario.sender, scenario.start_order)
                for scenario in scenarios
            ],
            [
                (robotweax, "sender-first"),
                (robotweax, "receiver-first"),
                (reference, "sender-first"),
                (reference, "receiver-first"),
            ],
        )
        self.assertTrue(all(scenario.key_length == 0 for scenario in scenarios))
        self.assertTrue(all(scenario.trace_roles for scenario in scenarios))

    def test_fault_matrix_covers_every_action_and_data_direction(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = run_rendezvous_interop.fault_matrix(
            robotweax, reference, 40
        )

        self.assertEqual(len(scenarios), 20)
        fault_plan = [
            fault
            for scenario in scenarios
            for fault in scenario.faults
        ]
        self.assertEqual(
            {fault.action for fault in fault_plan},
            {
                "drop",
                "duplicate",
                "late_duplicate",
                "delay",
                "reorder",
            },
        )
        handshake = [
            scenario
            for scenario in scenarios
            if len(scenario.faults) == 1
            and scenario.faults[0].packet_kind == "handshake"
        ]
        self.assertEqual(len(handshake), 4)
        self.assertTrue(
            all(
                scenario.faults[0].handshake_request == -1
                for scenario in handshake
            )
        )
        data = [
            scenario
            for scenario in scenarios
            if len(scenario.faults) == 1
            and scenario.faults[0].packet_kind == "data"
        ]
        self.assertEqual(len(data), 10)
        for action in (
            "drop",
            "duplicate",
            "late_duplicate",
            "delay",
            "reorder",
        ):
            action_scenarios = [
                scenario
                for scenario in data
                if scenario.faults[0].action == action
            ]
            self.assertEqual(
                {scenario.sender for scenario in action_scenarios},
                {robotweax, reference},
            )
        drop = [
            scenario
            for scenario in data
            if scenario.faults[0].action == "drop"
        ]
        recovery_latency = (
            run_rendezvous_interop
            .LOSS_RECOVERY_LATENCY_MILLISECONDS
        )
        self.assertTrue(
            all(
                scenario.latency_milliseconds
                == recovery_latency
                for scenario in drop
            )
        )
        reorder = [
            scenario
            for scenario in data
            if scenario.statistics_profile == "reorder"
        ]
        self.assertEqual(len(reorder), 2)
        self.assertTrue(
            all(
                scenario.maximum_reorder_tolerance_packets == 4
                and scenario.source_pacing
                for scenario in reorder
            )
        )
        self.assertEqual(
            {
                (
                    scenario.receiver,
                    scenario.tail_fault_distance_packets,
                    scenario.latency_milliseconds,
                )
                for scenario in reorder
            },
            {
                (reference, 32, 300),
                (robotweax, 2, 300),
            },
        )
        belated = [
            scenario
            for scenario in data
            if scenario.statistics_profile == "belated"
        ]
        self.assertEqual(len(belated), 2)
        self.assertTrue(
            all(
                scenario.latency_milliseconds == 100
                and scenario.tail_fault_distance_packets == 8
                and scenario.faults[0].delay_milliseconds == 250
                for scenario in belated
            )
        )
        burst = [
            scenario
            for scenario in scenarios
            if "burst-drop" in scenario.name
        ]
        self.assertEqual(len(burst), 2)
        self.assertEqual(
            {scenario.sender for scenario in burst},
            {robotweax, reference},
        )
        for scenario in burst:
            self.assertEqual(
                scenario.latency_milliseconds,
                recovery_latency,
            )
            self.assertEqual(
                [
                    (fault.action, fault.occurrence)
                    for fault in scenario.faults
                ],
                [("drop", 999), ("drop", 1_000), ("drop", 1_001)],
            )
        compound = [
            scenario
            for scenario in scenarios
            if "drop-reorder-rotation" in scenario.name
        ]
        self.assertEqual(len(compound), 2)
        self.assertEqual(
            {scenario.sender for scenario in compound},
            {robotweax, reference},
        )
        for scenario in compound:
            self.assertEqual(
                scenario.latency_milliseconds,
                recovery_latency,
            )
            self.assertEqual(
                [
                    (fault.action, fault.occurrence)
                    for fault in scenario.faults
                ],
                [("drop", 998), ("reorder", 1_002)],
            )
        sustained = [
            scenario
            for scenario in scenarios
            if "sustained-faults" in scenario.name
        ]
        sustained_latency = (
            run_rendezvous_interop
            .SUSTAINED_FAULT_LATENCY_MILLISECONDS
        )
        self.assertEqual(len(sustained), 2)
        self.assertEqual(
            {scenario.sender for scenario in sustained},
            {robotweax, reference},
        )
        for scenario in sustained:
            self.assertEqual(
                scenario.latency_milliseconds,
                sustained_latency,
            )
            self.assertEqual(scenario.byte_count_multiplier, 1)
            self.assertEqual(scenario.key_refresh_rate, 500)
            self.assertEqual(scenario.key_preannouncement, 200)
            self.assertEqual(
                scenario.minimum_fault_key_transitions, 5
            )
            self.assertEqual(len(scenario.faults), 12)
            self.assertEqual(
                [fault.action for fault in scenario.faults],
                [
                    "drop",
                    "delay",
                    "reorder",
                    "delay",
                    "drop",
                    "delay",
                    "reorder",
                    "delay",
                    "drop",
                    "delay",
                    "reorder",
                    "delay",
                ],
            )
            self.assertEqual(
                [fault.occurrence for fault in scenario.faults],
                [
                    495,
                    505,
                    995,
                    1_005,
                    1_495,
                    1_505,
                    1_995,
                    2_005,
                    2_495,
                    2_505,
                    2_995,
                    3_005,
                ],
            )
            self.assertTrue(
                all(
                    fault.delay_milliseconds
                    == (40 if fault.action == "delay" else 0)
                    for fault in scenario.faults
                )
            )
        self.assertTrue(all(scenario.trace_roles for scenario in scenarios))

    def test_sustained_scenario_accelerates_rotation_and_counts_key_changes(
        self,
    ) -> None:
        scenario = run_rendezvous_interop.Scenario(
            name="sustained",
            sender=Path("/robotweax"),
            receiver=Path("/haivision"),
            key_length=32,
            seed=1,
            key_refresh_rate=500,
            key_preannouncement=200,
        )
        accelerated = run_rendezvous_interop.options_for_scenario(
            scenario, self.options
        )
        self.assertEqual(
            accelerated.byte_count, self.options.byte_count
        )
        self.assertEqual(accelerated.key_refresh_rate, 500)
        self.assertEqual(accelerated.key_preannouncement, 200)
        sustained_latency = (
            run_rendezvous_interop
            .SUSTAINED_FAULT_LATENCY_MILLISECONDS
        )
        command = run_rendezvous_interop.peer_command(
            scenario.sender,
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/input"),
            accelerated,
            scenario.key_length,
            sustained_latency,
        )
        latency_option = command.index("--latency-ms")
        grace_option = command.index("--shutdown-grace-ms")
        refresh_option = command.index("--km-refresh-rate")
        preannounce_option = command.index("--km-preannounce")
        self.assertEqual(command[latency_option + 1], "800")
        self.assertEqual(command[grace_option + 1], "1050")
        self.assertEqual(command[refresh_option + 1], "500")
        self.assertEqual(command[preannounce_option + 1], "200")
        self.assertEqual(
            run_rendezvous_interop.observed_fault_key_transitions(
                [
                    {"packet_kind": "data", "key_selection": 1},
                    {"packet_kind": "data", "key_selection": 2},
                    {"packet_kind": "data", "key_selection": 2},
                    {"packet_kind": "data", "key_selection": 1},
                    {"packet_kind": "control", "key_selection": 2},
                ]
            ),
            2,
        )

    def test_scenario_key_rotation_overrides_must_be_valid_pairs(
        self,
    ) -> None:
        arguments = {
            "name": "invalid-rotation",
            "sender": Path("/robotweax"),
            "receiver": Path("/haivision"),
            "key_length": 32,
            "seed": 1,
        }
        with self.assertRaises(ValueError):
            run_rendezvous_interop.Scenario(
                **arguments,
                key_refresh_rate=500,
            )
        with self.assertRaises(ValueError):
            run_rendezvous_interop.Scenario(
                **arguments,
                key_refresh_rate=500,
                key_preannouncement=250,
            )

    def test_statistics_profile_fault_is_materialized_near_tail(
        self,
    ) -> None:
        scenario = run_rendezvous_interop.Scenario(
            name="tail-reorder",
            sender=Path("/robotweax"),
            receiver=Path("/haivision"),
            key_length=0,
            seed=1,
            faults=(
                srt_handshake_trace.RendezvousFault(
                    action="reorder",
                    direction="sender_to_receiver",
                    occurrence=1,
                ),
            ),
            maximum_reorder_tolerance_packets=4,
            tail_fault_distance_packets=8,
            statistics_profile="reorder",
        )
        options = run_rendezvous_interop.RunOptions(
            byte_count=120_000,
            timeout_seconds=30,
            key_refresh_rate=1_000,
            key_preannouncement=400,
            chunk_size=1_200,
        )

        fault_plan = run_rendezvous_interop.fault_plan_for_scenario(
            scenario, options
        )
        self.assertEqual(fault_plan[0].occurrence, 92)
        self.assertEqual(scenario.faults[0].occurrence, 1)

    def test_statistics_profiles_distinguish_reorder_and_belated(
        self,
    ) -> None:
        reorder = run_rendezvous_interop.Scenario(
            name="reorder-statistics",
            sender=Path("/robotweax"),
            receiver=Path("/haivision"),
            key_length=0,
            seed=1,
            faults=(
                srt_handshake_trace.RendezvousFault(
                    action="reorder",
                    direction="sender_to_receiver",
                    occurrence=1,
                ),
            ),
            maximum_reorder_tolerance_packets=4,
            tail_fault_distance_packets=8,
            statistics_profile="reorder",
        )
        reorder_summary = (
            run_rendezvous_interop
            .validate_receiver_statistics_profile(
                reorder,
                {
                    "stats": {
                        "pktReorderDistance": 1,
                        "pktReorderTolerance": 2,
                        "pktRcvBelated": 0,
                        "pktRecvTotal": 100,
                        "pktRecvUniqueTotal": 100,
                    }
                },
                {
                    "action": "reorder",
                    "sequence": 10,
                    "retransmitted": False,
                    "reorder_released": True,
                    "reorder_partner_sequence": 11,
                    "reorder_partner_retransmitted": False,
                    "reorder_partner_forwarded_first": True,
                },
            )
        )
        self.assertEqual(reorder_summary["reorder_distance"], 1)
        with self.assertRaisesRegex(
            RuntimeError, "not fully explained"
        ):
            run_rendezvous_interop.validate_receiver_statistics_profile(
                reorder,
                {
                    "stats": {
                        "pktReorderDistance": 1,
                        "pktReorderTolerance": 2,
                        "pktRcvBelated": 1,
                        "pktRecvTotal": 101,
                        "pktRecvUniqueTotal": 100,
                    }
                },
                {
                    "action": "reorder",
                    "sequence": 10,
                    "retransmitted": False,
                    "reorder_released": True,
                    "reorder_partner_sequence": 11,
                    "reorder_partner_retransmitted": False,
                    "reorder_partner_forwarded_first": True,
                },
            )

        retransmission_summary = (
            run_rendezvous_interop
            .validate_receiver_statistics_profile(
                reorder,
                {
                    "stats": {
                        "pktReorderDistance": 1,
                        "pktReorderTolerance": 1,
                        "pktRcvBelated": 1,
                        "pktRecvTotal": 101,
                        "pktRecvUniqueTotal": 100,
                    }
                },
                {
                    "action": "reorder",
                    "sequence": 10,
                    "retransmitted": False,
                    "reorder_released": True,
                    "reorder_partner_sequence": 11,
                    "reorder_partner_retransmitted": False,
                    "reorder_partner_forwarded_first": True,
                },
                forwarded_retransmissions=1,
            )
        )
        self.assertEqual(
            retransmission_summary["retransmission_candidates"], 1
        )

        belated = run_rendezvous_interop.Scenario(
            name="belated-statistics",
            sender=Path("/robotweax"),
            receiver=Path("/haivision"),
            key_length=0,
            seed=2,
            faults=(
                srt_handshake_trace.RendezvousFault(
                    action="late_duplicate",
                    direction="sender_to_receiver",
                    occurrence=1,
                    delay_milliseconds=250,
                ),
            ),
            tail_fault_distance_packets=8,
            statistics_profile="belated",
        )
        belated_summary = (
            run_rendezvous_interop
            .validate_receiver_statistics_profile(
                belated,
                {
                    "stats": {
                        "pktRcvBelated": 1,
                        "pktRcvAvgBelatedTime": 150.0,
                        "pktRecvTotal": 101,
                        "pktRecvUniqueTotal": 100,
                    }
                },
                {
                    "late_duplicate_released": True,
                    "delay_elapsed_milliseconds": 250.5,
                },
            )
        )
        self.assertEqual(belated_summary["belated_packets"], 1)
        with self.assertRaisesRegex(
            RuntimeError, "physical versus unique"
        ):
            run_rendezvous_interop.validate_receiver_statistics_profile(
                belated,
                {
                    "stats": {
                        "pktRcvBelated": 1,
                        "pktRcvAvgBelatedTime": 150.0,
                        "pktRecvTotal": 100,
                        "pktRecvUniqueTotal": 100,
                    }
                },
                {
                    "late_duplicate_released": True,
                    "delay_elapsed_milliseconds": 250.5,
                },
            )

    def test_encrypted_drop_requires_original_wire_retransmission(
        self,
    ) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=999,
        )
        observation = {
            "sequence": 123,
            "key_selection": 1,
            "retransmission_observed": True,
            "retransmission_key_selection": 1,
            "retransmission_ciphertext_matches": True,
        }
        validate = (
            run_rendezvous_interop.encrypted_drop_retransmission_failure
        )
        self.assertIsNone(
            validate((fault,), [observation])
        )

        missing = dict(observation)
        missing["retransmission_observed"] = False
        self.assertIn(
            "no observed retransmission",
            validate((fault,), [missing]),
        )

        changed_key = dict(observation)
        changed_key["retransmission_key_selection"] = 2
        self.assertIn(
            "changed key selector",
            validate((fault,), [changed_key]),
        )

        changed_ciphertext = dict(observation)
        changed_ciphertext[
            "retransmission_ciphertext_matches"
        ] = False
        self.assertIn(
            "changed ciphertext",
            validate((fault,), [changed_ciphertext]),
        )

    def test_fault_configuration_is_strict(self) -> None:
        with self.assertRaises(ValueError):
            srt_handshake_trace.RendezvousFault(
                action="corrupt",
                direction="sender_to_receiver",
                occurrence=1,
            )
        with self.assertRaises(ValueError):
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=0,
            )
        with self.assertRaises(ValueError):
            srt_handshake_trace.RendezvousFault(
                action="delay",
                direction="sender_to_receiver",
                occurrence=1,
            )
        with self.assertRaises(ValueError):
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=1,
                delay_milliseconds=40,
            )
        with self.assertRaises(ValueError):
            srt_handshake_trace.RendezvousFault(
                action="late_duplicate",
                direction="sender_to_receiver",
                occurrence=1,
            )
        with self.assertRaises(ValueError):
            srt_handshake_trace.RendezvousFault(
                action="delay",
                direction="receiver_to_sender",
                occurrence=1,
                packet_kind="data",
                control_type=2,
                delay_milliseconds=40,
            )

    def test_fault_proxy_drops_exact_occurrence(self) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=2,
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(fault)
        first = self.data_packet(1)
        second = self.data_packet(2)

        proxy._forward(first, "sender_to_receiver")
        proxy._forward(second, "sender_to_receiver")

        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [first],
        )
        self.assertEqual(
            proxy.fault_observation()["sequence"],
            2,
        )

    def test_drop_fault_records_matching_encrypted_retransmission(
        self,
    ) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=1,
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(fault)
        original = self.data_packet(
            10,
            key_selection=1,
            payload=b"ciphertext",
        )
        retransmission = self.data_packet(
            10,
            key_selection=1,
            retransmitted=True,
            payload=b"ciphertext",
        )

        proxy._forward(original, "sender_to_receiver")
        proxy._forward(retransmission, "sender_to_receiver")

        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [retransmission],
        )
        observation = proxy.fault_observation()
        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["key_selection"], 1)
        self.assertFalse(observation["retransmitted"])
        self.assertTrue(observation["retransmission_observed"])
        self.assertEqual(
            observation["retransmission_key_selection"], 1
        )
        self.assertTrue(
            observation["retransmission_ciphertext_matches"]
        )

    def test_persistent_drop_suppresses_retries_until_cumulative_ack(
        self,
    ) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="sender_to_receiver",
            occurrence=1,
            suppress_retransmissions=True,
        )
        proxy, sender_socket, receiver_socket = (
            self.make_fake_fault_proxy(fault)
        )
        original = self.data_packet(10, payload=b"missing")
        retransmission = self.data_packet(
            10,
            retransmitted=True,
            payload=b"missing",
        )
        later = self.data_packet(11, payload=b"later")
        acknowledgement = struct.pack(
            ">IIIII",
            0x8002_0000,
            7,
            0,
            0,
            12,
        )

        proxy._forward(original, "sender_to_receiver")
        proxy._forward(retransmission, "sender_to_receiver")
        proxy._forward(later, "sender_to_receiver")
        proxy._forward(acknowledgement, "receiver_to_sender")

        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [later],
        )
        self.assertEqual(
            [payload for payload, _ in sender_socket.sent],
            [acknowledgement],
        )
        observation = proxy.fault_observation()
        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["suppressed_retransmissions"], 1)
        self.assertTrue(observation["cumulative_ack_observed"])
        self.assertEqual(observation["cumulative_ack_next_sequence"], 12)
        self.assertNotIn("retransmission_observed", observation)

    def test_fault_proxy_applies_compound_plan_to_original_packets(
        self,
    ) -> None:
        faults = (
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=2,
            ),
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=3,
            ),
            srt_handshake_trace.RendezvousFault(
                action="reorder",
                direction="sender_to_receiver",
                occurrence=5,
            ),
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=7,
            ),
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(faults)
        packets = [
            self.data_packet(sequence) for sequence in range(1, 9)
        ]

        for packet in packets:
            proxy._forward(packet, "sender_to_receiver")

        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [
                packets[0],
                packets[3],
                packets[5],
                packets[4],
                packets[7],
            ],
        )
        observations = proxy.fault_observations()
        self.assertEqual(
            [
                (
                    observation["plan_index"],
                    observation["action"],
                    observation["sequence"],
                )
                for observation in observations
            ],
            [
                (1, "drop", 2),
                (2, "drop", 3),
                (3, "reorder", 5),
                (4, "drop", 7),
            ],
        )

    def test_retransmission_does_not_advance_later_faults(self) -> None:
        faults = (
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=1,
            ),
            srt_handshake_trace.RendezvousFault(
                action="drop",
                direction="sender_to_receiver",
                occurrence=2,
            ),
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(faults)
        original_first = self.data_packet(1)
        retransmitted_first = self.data_packet(
            1, retransmitted=True
        )
        original_second = self.data_packet(2)

        proxy._forward(original_first, "sender_to_receiver")
        proxy._forward(retransmitted_first, "sender_to_receiver")
        proxy._forward(original_second, "sender_to_receiver")

        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [retransmitted_first],
        )
        self.assertEqual(
            [
                observation["sequence"]
                for observation in proxy.fault_observations()
            ],
            [1, 2],
        )

    def test_reorder_waits_for_the_next_original_data_packet(
        self,
    ) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="reorder",
            direction="sender_to_receiver",
            occurrence=2,
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(fault)
        original_first = self.data_packet(1)
        original_second = self.data_packet(2)
        retransmitted_first = self.data_packet(
            1, retransmitted=True
        )
        original_third = self.data_packet(3)

        proxy._forward(original_first, "sender_to_receiver")
        proxy._forward(original_second, "sender_to_receiver")
        proxy._forward(retransmitted_first, "sender_to_receiver")
        proxy._forward(original_third, "sender_to_receiver")

        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [
                original_first,
                retransmitted_first,
                original_third,
                original_second,
            ],
        )
        observation = proxy.fault_observation()
        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertTrue(observation["reorder_released"])
        self.assertEqual(observation["reorder_partner_sequence"], 3)
        self.assertFalse(
            observation["reorder_partner_retransmitted"]
        )
        self.assertTrue(
            observation["reorder_partner_forwarded_first"]
        )
        self.assertEqual(
            proxy.forwarded_data_retransmissions(
                "sender_to_receiver"
            ),
            1,
        )

    def test_forwarded_data_observation_requires_a_cumulative_ack(
        self,
    ) -> None:
        proxy, sender_socket, receiver_socket = self.make_fake_fault_proxy(())
        data = self.data_packet(41, destination_socket_id=77)
        acknowledgement = struct.pack(
            ">IIIII",
            0x8002_0000,
            7,
            0,
            0,
            42,
        )

        proxy._forward(data, "sender_to_receiver")

        pending = proxy.forwarded_data_observation("sender_to_receiver")
        self.assertEqual(pending["count"], 1)
        self.assertEqual(pending["first_sequence"], 41)
        self.assertEqual(pending["destination_socket_id"], 77)
        self.assertTrue(pending["destination_socket_id_consistent"])
        self.assertFalse(pending["acknowledged"])
        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent], [data]
        )

        proxy._forward(acknowledgement, "receiver_to_sender")

        admitted = proxy.forwarded_data_observation("sender_to_receiver")
        self.assertTrue(admitted["acknowledged"])
        self.assertEqual(admitted["acknowledgement_next_sequence"], 42)
        self.assertEqual(
            admitted["acknowledgement_forwarded_data_count"], 1
        )
        self.assertEqual(
            [payload for payload, _ in sender_socket.sent],
            [acknowledgement],
        )

    def test_forwarded_data_observation_tracks_contiguous_messages(
        self,
    ) -> None:
        proxy, _, _ = self.make_fake_fault_proxy(())

        proxy._forward(
            self.data_packet(
                41, message_number=3, destination_socket_id=77
            ),
            "sender_to_receiver",
        )
        proxy._forward(
            self.data_packet(
                42, message_number=5, destination_socket_id=77
            ),
            "sender_to_receiver",
        )
        proxy._forward(
            self.data_packet(
                44, message_number=6, destination_socket_id=78
            ),
            "sender_to_receiver",
        )

        observation = proxy.forwarded_data_observation(
            "sender_to_receiver"
        )
        self.assertFalse(observation["sequences_contiguous"])
        self.assertFalse(observation["message_numbers_contiguous"])
        self.assertFalse(observation["destination_socket_id_consistent"])

    def test_conclusion_socket_id_requires_one_endpoint_identity(self) -> None:
        proxy, _, _ = self.make_fake_fault_proxy(())
        proxy._record(
            self.handshake_packet(1, socket_id=77),
            "receiver_to_sender",
        )
        proxy._record(
            self.handshake_packet(-1, socket_id=77),
            "receiver_to_sender",
        )

        self.assertEqual(
            proxy.conclusion_socket_id("receiver_to_sender"), 77
        )
        self.assertIsNone(proxy.conclusion_socket_id("sender_to_receiver"))

        proxy._record(
            self.handshake_packet(-1, socket_id=78),
            "receiver_to_sender",
        )
        self.assertIsNone(
            proxy.conclusion_socket_id("receiver_to_sender")
        )

    def test_conclusion_socket_id_supports_caller_listener_directions(
        self,
    ) -> None:
        proxy, _, _ = self.make_fake_fault_proxy(())
        proxy._record(
            self.handshake_packet(-1, socket_id=81),
            "caller_to_listener",
        )
        proxy._record(
            self.handshake_packet(-1, socket_id=82),
            "listener_to_caller",
        )

        self.assertEqual(
            proxy.conclusion_socket_id("caller_to_listener"), 81
        )
        self.assertEqual(
            proxy.conclusion_socket_id("listener_to_caller"), 82
        )

    def test_conclusion_socket_id_rejects_an_unknown_direction(self) -> None:
        proxy, _, _ = self.make_fake_fault_proxy(())

        with self.assertRaisesRegex(
            ValueError, "unsupported relay direction 'sideways'"
        ):
            proxy.conclusion_socket_id("sideways")

    def test_fault_proxy_duplicates_exact_occurrence(self) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="duplicate",
            direction="sender_to_receiver",
            occurrence=2,
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(fault)
        first = self.data_packet(1)
        second = self.data_packet(2)

        proxy._forward(first, "sender_to_receiver")
        proxy._forward(second, "sender_to_receiver")

        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [first, second, second],
        )

    def test_fault_proxy_releases_a_late_duplicate(self) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="late_duplicate",
            direction="sender_to_receiver",
            occurrence=2,
            delay_milliseconds=40,
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(fault)
        first = self.data_packet(1)
        second = self.data_packet(2)

        proxy._forward(first, "sender_to_receiver")
        proxy._forward(second, "sender_to_receiver")
        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [first, second],
        )
        proxy._flush_late_duplicates(force=True)
        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [first, second, second],
        )
        observation = proxy.fault_observation()
        self.assertTrue(observation["late_duplicate_released"])
        self.assertGreaterEqual(
            observation["delay_elapsed_milliseconds"], 0
        )

    def test_fault_proxy_delay_preserves_direction_order(self) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="delay",
            direction="sender_to_receiver",
            occurrence=2,
            delay_milliseconds=40,
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(fault)
        packets = [self.data_packet(sequence) for sequence in (1, 2, 3)]

        for packet in packets:
            proxy._forward(packet, "sender_to_receiver")
        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [packets[0]],
        )
        proxy._flush_delay(force=True)
        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            packets,
        )
        observation = proxy.fault_observation()
        self.assertTrue(observation["delay_released"])
        self.assertEqual(observation["delayed_datagrams"], 2)

    def test_fault_proxy_delays_only_selected_control_type(self) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="delay",
            direction="receiver_to_sender",
            occurrence=1,
            packet_kind="control",
            control_type=2,
            delay_milliseconds=40,
        )
        proxy, sender_socket, _ = self.make_fake_fault_proxy(fault)
        keepalive = struct.pack(">IIII", 0x8001_0000, 0, 0, 0)
        acknowledgement = struct.pack(
            ">IIIII", 0x8002_0000, 0, 0, 0, 17
        )

        proxy._forward(keepalive, "receiver_to_sender")
        proxy._forward(acknowledgement, "receiver_to_sender")
        self.assertEqual(
            [payload for payload, _ in sender_socket.sent],
            [keepalive],
        )
        proxy._flush_delay(force=True)
        self.assertEqual(
            [payload for payload, _ in sender_socket.sent],
            [keepalive, acknowledgement],
        )
        observation = proxy.fault_observation()
        self.assertEqual(observation["control_type"], 2)
        self.assertTrue(observation["delay_released"])

    def test_fault_proxy_reorders_one_adjacent_pair(self) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="reorder",
            direction="sender_to_receiver",
            occurrence=2,
        )
        proxy, _, receiver_socket = self.make_fake_fault_proxy(fault)
        packets = [self.data_packet(sequence) for sequence in (1, 2, 3)]

        for packet in packets:
            proxy._forward(packet, "sender_to_receiver")

        self.assertEqual(
            [payload for payload, _ in receiver_socket.sent],
            [packets[0], packets[2], packets[1]],
        )

    def test_handshake_fault_filters_request_and_accepts_either_direction(
        self,
    ) -> None:
        fault = srt_handshake_trace.RendezvousFault(
            action="drop",
            direction="either",
            occurrence=1,
            packet_kind="handshake",
            handshake_request=-1,
        )
        proxy, sender_socket, _ = self.make_fake_fault_proxy(fault)
        wave = self.handshake_packet(0)
        conclusion = self.handshake_packet(-1)

        proxy._forward(wave, "receiver_to_sender")
        proxy._forward(conclusion, "receiver_to_sender")

        self.assertEqual(
            [payload for payload, _ in sender_socket.sent],
            [wave],
        )
        observation = proxy.fault_observation()
        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["request"], -1)
        self.assertEqual(observation["direction"], "receiver_to_sender")

    def test_cookie_role_contest_matches_wrapping_boundaries(self) -> None:
        resolve = srt_handshake_trace.resolve_rendezvous_role

        self.assertEqual(resolve(20, 10), "initiator")
        self.assertEqual(resolve(10, 20), "responder")
        self.assertEqual(resolve(0xFFFF_FFFF, 1), "responder")
        self.assertEqual(resolve(1, 0xFFFF_FFFF), "initiator")
        self.assertEqual(resolve(0x8000_0000, 0), "responder")
        self.assertEqual(resolve(0, 0x8000_0000), "initiator")
        self.assertEqual(resolve(42, 42), "unresolved")

    def test_cookie_role_observation_uses_both_waveahands(self) -> None:
        observation = srt_handshake_trace.rendezvous_role_observation(
            [
                {
                    "direction": "sender_to_receiver",
                    "request": 0,
                    "syn_cookie": "0x00000020",
                },
                {
                    "direction": "receiver_to_sender",
                    "request": 0,
                    "syn_cookie": "0x00000010",
                },
            ]
        )

        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["sender_role"], "initiator")
        self.assertEqual(observation["receiver_role"], "responder")
        self.assertIsNone(
            srt_handshake_trace.rendezvous_role_observation(
                [
                    {
                        "direction": "sender_to_receiver",
                        "request": 0,
                        "syn_cookie": "0x00000020",
                    }
                ]
            )
        )

    def test_two_ended_trace_proxy_forwards_and_observes_roles(
        self,
    ) -> None:
        sender = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        receiver = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(sender.close)
        self.addCleanup(receiver.close)
        try:
            sender.bind(("127.0.0.1", 0))
            receiver.bind(("127.0.0.1", 0))
            proxy = srt_handshake_trace.RendezvousTraceProxy(
                sender.getsockname()[1],
                receiver.getsockname()[1],
            )
        except OSError as error:
            self.skipTest(f"loopback UDP is unavailable: {error}")
        self.addCleanup(proxy.close)
        sender.settimeout(1)
        receiver.settimeout(1)

        proxy.start()
        sender_wave = self.handshake_packet(0, 0x20, 10)
        receiver_wave = self.handshake_packet(0, 0x10, 20)
        sender.sendto(
            sender_wave,
            ("127.0.0.1", proxy.sender_port),
        )
        received, _ = receiver.recvfrom(65_535)
        self.assertEqual(received, sender_wave)
        receiver.sendto(
            receiver_wave,
            ("127.0.0.1", proxy.receiver_port),
        )
        received, _ = sender.recvfrom(65_535)
        self.assertEqual(received, receiver_wave)

        observation = proxy.role_observation()
        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["sender_role"], "initiator")
        self.assertEqual(observation["receiver_role"], "responder")

    def test_ipv6_trace_proxy_forwards_and_observes_roles(self) -> None:
        sender = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        receiver = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
        self.addCleanup(sender.close)
        self.addCleanup(receiver.close)
        try:
            sender.bind(("::1", 0))
            receiver.bind(("::1", 0))
            proxy = srt_handshake_trace.RendezvousTraceProxy(
                sender.getsockname()[1],
                receiver.getsockname()[1],
                host="::1",
            )
        except OSError as error:
            self.skipTest(f"IPv6 loopback UDP is unavailable: {error}")
        self.addCleanup(proxy.close)
        sender.settimeout(1)
        receiver.settimeout(1)

        proxy.start()
        sender_wave = self.handshake_packet(0, 0x20, 10)
        receiver_wave = self.handshake_packet(0, 0x10, 20)
        sender.sendto(sender_wave, ("::1", proxy.sender_port))
        received, _ = receiver.recvfrom(65_535)
        self.assertEqual(received, sender_wave)
        receiver.sendto(receiver_wave, ("::1", proxy.receiver_port))
        received, _ = sender.recvfrom(65_535)
        self.assertEqual(received, receiver_wave)

        observation = proxy.role_observation()
        self.assertIsNotNone(observation)
        assert observation is not None
        self.assertEqual(observation["sender_role"], "initiator")
        self.assertEqual(observation["receiver_role"], "responder")

    def test_clear_command_omits_all_key_material_options(self) -> None:
        command = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-receiver",
            10_001,
            10_002,
            Path("/output"),
            self.options,
            0,
        )

        self.assertNotIn("--passphrase-env", command)
        self.assertNotIn("--pbkeylen", command)
        self.assertNotIn("--km-refresh-rate", command)
        self.assertNotIn("--km-preannounce", command)
        self.assertNotIn("--latency-ms", command)

    def test_command_applies_scenario_specific_latency(self) -> None:
        command = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/input"),
            self.options,
            32,
            300,
        )

        latency_option = command.index("--latency-ms")
        self.assertEqual(command[latency_option + 1], "300")
        shutdown_grace_option = command.index(
            "--shutdown-grace-ms"
        )
        self.assertEqual(
            command[shutdown_grace_option + 1], "550"
        )

        default_command = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/input"),
            self.options,
            32,
        )
        default_shutdown_grace_option = default_command.index(
            "--shutdown-grace-ms"
        )
        self.assertEqual(
            default_command[
                default_shutdown_grace_option + 1
            ],
            "250",
        )

    def test_encrypted_command_passes_only_secret_environment_name(
        self,
    ) -> None:
        command = run_rendezvous_interop.peer_command(
            Path("/peer"),
            "rendezvous-sender",
            10_001,
            10_002,
            Path("/input"),
            self.options,
            32,
        )

        secret_option = command.index("--passphrase-env")
        self.assertEqual(
            command[secret_option + 1],
            run_rendezvous_interop.PASSPHRASE_ENVIRONMENT,
        )
        key_option = command.index("--pbkeylen")
        self.assertEqual(command[key_option + 1], "32")
        refresh_option = command.index("--km-refresh-rate")
        self.assertEqual(command[refresh_option + 1], "1000")
        preannounce_option = command.index("--km-preannounce")
        self.assertEqual(command[preannounce_option + 1], "400")

    def test_failure_report_preserves_both_peer_diagnostics(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "partial"
            output.write_bytes(b"x" * 17)
            report = run_rendezvous_interop.render_failure(
                run_rendezvous_interop.Scenario(
                    name="diagnostic",
                    sender=Path("/robotweax"),
                    receiver=Path("/haivision"),
                    key_length=16,
                    seed=1,
                ),
                "peers timed out",
                output,
                sender_stdout='{"event":"ready"}\n',
                receiver_stderr="connect failed\n",
                handshake_trace='{"request_name":"WAVEAHAND"}',
            )

        self.assertIn("partial_output_bytes=17", report)
        self.assertIn("--- sender stdout ---", report)
        self.assertIn('{"event":"ready"}', report)
        self.assertIn("--- receiver stderr ---", report)
        self.assertIn("connect failed", report)
        self.assertIn(
            "--- secret-safe rendezvous handshake trace ---",
            report,
        )
        self.assertIn('"request_name":"WAVEAHAND"', report)

    def test_file_mismatch_diagnostic_maps_offsets_to_packets(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as directory:
            expected = Path(directory) / "expected"
            actual = Path(directory) / "actual"
            expected.write_bytes(b"abcdefghijkl")
            actual.write_bytes(b"abcXefghiYkl")

            diagnostic = interop_common.describe_file_mismatch(
                expected,
                actual,
                4,
                preview_bytes=4,
            )

        self.assertIn("first_mismatch_offset=3", diagnostic)
        self.assertIn("packet_occurrence=1", diagnostic)
        self.assertIn("packet_offset=3", diagnostic)
        self.assertIn("expected_preview=64656667", diagnostic)
        self.assertIn("actual_preview=58656667", diagnostic)
        self.assertIn(
            "mismatching_packet_occurrences=[1, 3]",
            diagnostic,
        )


if __name__ == "__main__":
    unittest.main()
