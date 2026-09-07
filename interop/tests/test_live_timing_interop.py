from __future__ import annotations

import json
import socket
import struct
import subprocess
import sys
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import live_timing  # noqa: E402
import run_live_timing_interop as timing_interop  # noqa: E402


class LiveTimingInteropTests(unittest.TestCase):
    def test_requested_raw_evidence_survives_validation_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            receiver = root / "receiver.jsonl"
            sidecar = root / "receiver.jsonl.path-outage.json"
            with self.assertRaisesRegex(RuntimeError, "validation rejected"):
                with tempfile.TemporaryDirectory(dir=root) as scratch:
                    directory = Path(scratch)
                    with timing_interop.preserve_measurement_evidence(directory, receiver):
                        (directory / "timing-listener.stdout").write_text('{"event":"ready"}\n')
                        (directory / "path-outage-evidence.json").write_text('{"outage":{}}\n')
                        raise RuntimeError("validation rejected")
            self.assertFalse(directory.exists())
            self.assertEqual(receiver.read_text(), '{"event":"ready"}\n')
            self.assertEqual(sidecar.read_text(), '{"outage":{}}\n')

    def test_raw_evidence_is_opt_in_and_handles_early_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            receiver = directory / "receiver.jsonl"
            with timing_interop.preserve_measurement_evidence(directory, receiver):
                pass
            self.assertFalse(receiver.exists())
            (directory / "timing-listener.stdout").write_text("raw")
            with timing_interop.preserve_measurement_evidence(directory, None):
                pass
            self.assertFalse(receiver.exists())
            with timing_interop.preserve_measurement_evidence(directory, receiver):
                pass
            self.assertEqual(receiver.read_text(), "raw")

    def test_main_keeps_raw_log_when_measurement_validation_raises(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            receiver = Path(temp) / "receiver.jsonl"
            scorecard = Path(temp) / "scorecard.json"
            def rejected_measurement(*args, **kwargs):
                directory = args[9]
                (directory / "timing-listener.stdout").write_text("raw failed transfer\n")
                (directory / "path-outage-evidence.json").write_text('{"outage":{}}\n')
                raise RuntimeError("identities rejected")
            with mock.patch.object(sys, "argv", [
                "harness", "--timing-peer", sys.executable, "--sender-peer", sys.executable,
                "--write-receiver-log", str(receiver), "--output", str(scorecard),
            ]), mock.patch.object(timing_interop, "run_measurement", side_effect=rejected_measurement):
                with self.assertRaisesRegex(RuntimeError, "identities rejected"):
                    timing_interop.main()
            self.assertEqual(receiver.read_text(), "raw failed transfer\n")
            self.assertTrue(receiver.with_name(receiver.name + ".path-outage.json").is_file())
            self.assertFalse(scorecard.exists())

    def test_phase_timing_boundaries_and_first_gap(self) -> None:
        events = [
            {"message_index": 0, "receive_start_microseconds": 100,
             "srt_release_microseconds": 120, "udp_send_start_microseconds": 125,
             "udp_egress_microseconds": 130, "tsbpd_deadline_microseconds": 110},
            {"message_index": 1, "receive_start_microseconds": 140,
             "srt_release_microseconds": 140, "udp_send_start_microseconds": 140,
             "udp_egress_microseconds": 140, "tsbpd_deadline_microseconds": 135},
        ]
        result = timing_interop.phase_observations(events)
        self.assertEqual(result[0]["receive_call_microseconds"], 20)
        self.assertEqual(result[0]["post_receive_microseconds"], 5)
        self.assertEqual(result[0]["udp_send_call_microseconds"], 5)
        self.assertIsNone(result[0]["between_receives_microseconds"])
        self.assertEqual(result[1]["between_receives_microseconds"], 10)
        self.assertEqual(result[1]["receive_call_microseconds"], 0)

    def test_phase_timing_rejects_missing_and_invalid_boundaries(self) -> None:
        event = {"message_index": 0, "receive_start_microseconds": 100,
                 "srt_release_microseconds": 120,
                 "udp_send_start_microseconds": 125,
                 "udp_egress_microseconds": 130,
                 "tsbpd_deadline_microseconds": 110}
        for field in event:
            with self.subTest(missing=field), self.assertRaises(RuntimeError):
                timing_interop.phase_observations(
                    [{key: value for key, value in event.items() if key != field}]
                )
        for field, value in [("receive_start_microseconds", 0),
                             ("srt_release_microseconds", 99),
                             ("udp_send_start_microseconds", 119),
                             ("udp_egress_microseconds", 124),
                             ("message_index", 1)]:
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                timing_interop.phase_observations([{**event, field: value}])
        with self.assertRaises(RuntimeError):
            timing_interop.phase_observations([event, {**event, "message_index": 1}])

    def test_phase_timing_is_opt_in(self) -> None:
        args = (Path("peer"), "127.0.0.1", 9001, "127.0.0.1", 9002,
                16, 1316, 120, 10)
        self.assertNotIn("--phase-timing", timing_interop.timing_receiver_command(*args))
        self.assertIn("--phase-timing", timing_interop.timing_receiver_command(
            *args, phase_timing=True))

    @staticmethod
    def _data_packet(
        sequence: int,
        key_selection: int,
        *,
        retransmitted: bool = False,
    ) -> bytes:
        message_word = (
            1
            | (key_selection & 0x03) << 27
            | int(retransmitted) << 26
        )
        return struct.pack(">IIII", sequence, message_word, 0, 7) + b"data"

    @staticmethod
    def _key_material_packet(subtype: int, key_selection: int) -> bytes:
        key_words = 8
        salt_words = 4
        content = bytearray(16 + salt_words * 4 + key_words * 4 + 8)
        content[0] = 0x21
        content[1:3] = b"\x20\x29"
        content[3] = key_selection
        content[8] = 2
        content[10] = 2
        content[14] = salt_words
        content[15] = key_words
        return struct.pack(">IIII", 0xFFFF_0000 | subtype, 0, 0, 7) + content

    def test_failed_sender_is_reported_before_receiver_wait(self) -> None:
        failed = subprocess.CompletedProcess(
            ["sender-peer"], 5, stdout="", stderr="send drain timed out"
        )
        with self.assertRaisesRegex(
            RuntimeError, "live timing sender exited with status 5"
        ):
            timing_interop.require_successful_process(failed, "sender")

        successful = subprocess.CompletedProcess(
            ["sender-peer"], 0, stdout="complete", stderr=""
        )
        timing_interop.require_successful_process(successful, "sender")

    def test_sustained_fault_plan_scales_with_message_count(self) -> None:
        plan = timing_interop.fault_plan(
            "loss-delay-reorder", 4_096
        )
        self.assertEqual(
            [(fault.action, fault.occurrence) for fault in plan],
            [("delay", 1_024), ("drop", 2_048), ("reorder", 3_072)],
        )
        self.assertEqual(plan[0].delay_milliseconds, 30)
        self.assertEqual(timing_interop.fault_plan("none", 1), ())
        burst = timing_interop.fault_plan("fec-burst-drop", 8)
        self.assertEqual(
            [fault.occurrence for fault in burst],
            list(timing_interop.FEC_BURST_DROP_OCCURRENCES),
        )
        compound = timing_interop.fault_plan(
            "fec-burst-delay-reorder", 96
        )
        self.assertEqual(
            [(fault.action, fault.occurrence) for fault in compound],
            [
                ("drop", 5),
                ("drop", 6),
                ("drop", 7),
                ("delay", 32),
                ("reorder", 64),
            ],
        )
        self.assertEqual(compound[3].delay_milliseconds, 30)
        with self.assertRaisesRegex(ValueError, "at least eight"):
            timing_interop.fault_plan("loss-delay-reorder", 7)
        with self.assertRaisesRegex(ValueError, "at least eight"):
            timing_interop.fault_plan("fec-burst-drop", 7)
        with self.assertRaisesRegex(ValueError, "at least 32"):
            timing_interop.fault_plan("fec-burst-delay-reorder", 31)

    def test_fault_measurement_rejects_nonloopback_relay_binding(self) -> None:
        with self.assertRaisesRegex(
            ValueError, "require a loopback SRT endpoint"
        ):
            timing_interop.run_measurement(
                Path("timing-peer"),
                Path("sender-peer"),
                "binary-1200",
                8,
                7_520_000,
                300,
                15,
                "192.0.2.1",
                "127.0.0.1",
                Path("."),
                fault_profile="fec-source-drop",
                fec=timing_interop.FecProfile("row"),
            )

    def test_fec_burst_rejects_row_geometry(self) -> None:
        with self.assertRaisesRegex(
            ValueError, "requires Column or Matrix"
        ):
            timing_interop.run_measurement(
                Path("timing-peer"),
                Path("sender-peer"),
                "binary-1200",
                8,
                7_520_000,
                300,
                15,
                "127.0.0.1",
                "127.0.0.1",
                Path("."),
                fault_profile="fec-burst-drop",
                fec=timing_interop.FecProfile("row"),
            )

    def test_fault_validation_requires_complete_causal_evidence(self) -> None:
        plan = timing_interop.fault_plan("loss-delay-reorder", 32)
        observations: list[dict[str, object]] = [
            {
                "plan_index": 1,
                "action": "delay",
                "direction": "sender_to_receiver",
                "occurrence": 8,
                "packet_kind": "data",
                "delay_milliseconds": 30,
                "delay_released": True,
                "delayed_datagrams": 2,
                "delay_elapsed_milliseconds": 30.1,
            },
            {
                "plan_index": 2,
                "action": "drop",
                "direction": "sender_to_receiver",
                "occurrence": 16,
                "packet_kind": "data",
                "key_selection": 2,
                "later_data_observed_before_retransmission": True,
                "loss_report_observed": True,
                "retransmission_observed": True,
                "retransmission_flag": True,
                "retransmission_ciphertext_matches": True,
                "retransmission_key_selection": 2,
            },
            {
                "plan_index": 3,
                "action": "reorder",
                "direction": "sender_to_receiver",
                "occurrence": 24,
                "packet_kind": "data",
                "sequence": 100,
                "reorder_released": True,
                "reorder_partner_sequence": 101,
                "reorder_partner_retransmitted": False,
                "reorder_partner_forwarded_first": True,
            },
        ]

        class Relay:
            def __init__(self, values: list[dict[str, object]]) -> None:
                self.values = values

            @staticmethod
            def error() -> None:
                return None

            def fault_observations(self) -> list[dict[str, object]]:
                return self.values

        validated = timing_interop.validate_fault_observations(
            plan, Relay(observations)
        )
        self.assertEqual(len(validated), 3)

        for index, field, error in (
            (0, "delay_released", "delay fault"),
            (1, "retransmission_ciphertext_matches", "drop fault"),
            (2, "reorder_released", "reorder fault"),
        ):
            with self.subTest(field=field):
                mutated = [dict(observation) for observation in observations]
                mutated[index].pop(field)
                with self.assertRaisesRegex(RuntimeError, error):
                    timing_interop.validate_fault_observations(
                        plan, Relay(mutated)
                    )

    def test_fec_validation_requires_filter_recovery_without_arq(self) -> None:
        fec = timing_interop.FecProfile("matrix")
        plan = timing_interop.fault_plan("fec-source-drop", 100)
        sender = {
            "stats": {
                "pktSndFilterExtraTotal": 30,
                "pktRetransTotal": 0,
                "pktRecvNAKTotal": 0,
            }
        }
        receiver = {
            "receiver_filter_extra_packets": 30,
            "receiver_filter_supply_packets": 1,
            "receiver_filter_loss_packets": 0,
        }
        observation = {
            "plan_index": 1,
            "action": "drop",
            "direction": "sender_to_receiver",
            "occurrence": timing_interop.FEC_SOURCE_DROP_OCCURRENCE,
            "packet_kind": "data",
            "filter_control": False,
            "retransmitted": False,
            "sequence": 100,
            "message_number": 1,
            "destination_socket_id": 2,
            "key_selection": 1,
            "payload_bytes": 1_200,
            "payload_sha256": "a" * 64,
            "later_data_observed_before_retransmission": True,
        }
        trace = "\n".join(
            json.dumps(
                {
                    "event": "srt_handshake_trace",
                    "direction": direction,
                    "extensions": [{"name": "FILTER"}],
                }
            )
            for direction in ("sender_to_receiver", "receiver_to_sender")
        )

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [dict(observation)]

            @staticmethod
            def render(_: str) -> str:
                return trace

        faults, evidence = timing_interop.validate_fec_evidence(
            fec, plan, sender, receiver, Relay(), 100, 1_200
        )
        self.assertEqual(len(faults), 1)
        self.assertEqual(evidence["expected_control_packets"], 30)
        self.assertEqual(evidence["receiver_reconstructed_packets"], 1)

        for target, field, value in (
            (receiver, "receiver_filter_supply_packets", 0),
            (receiver, "receiver_filter_supply_packets", 2),
            (sender["stats"], "pktRecvNAKTotal", 1),
            (sender["stats"], "pktRetransTotal", 1),
            (sender["stats"], "pktSndFilterExtraTotal", 31),
            (receiver, "receiver_filter_extra_packets", 29),
        ):
            with self.subTest(field=field):
                original = target[field]
                target[field] = value
                try:
                    with self.assertRaisesRegex(
                        RuntimeError, "do not prove reconstruction"
                    ):
                        timing_interop.validate_fec_evidence(
                            fec, plan, sender, receiver, Relay(), 100, 1_200
                        )
                finally:
                    target[field] = original

        for field, value, error in (
            ("loss_report_observed", False, "no-ARQ"),
            ("payload_sha256", "invalid", "exact source DATA"),
        ):
            with self.subTest(field=field):
                original = observation.get(field)
                observation[field] = value
                try:
                    with self.assertRaisesRegex(
                        RuntimeError, error
                    ):
                        timing_interop.validate_fec_evidence(
                            fec, plan, sender, receiver, Relay(), 100, 1_200
                        )
                finally:
                    if original is None:
                        observation.pop(field)
                    else:
                        observation[field] = original

        complete_trace = trace
        trace = complete_trace.splitlines()[0]
        with self.assertRaisesRegex(RuntimeError, "both directions"):
            timing_interop.validate_fec_evidence(
                fec, plan, sender, receiver, Relay(), 100, 1_200
            )
        trace = complete_trace

    def test_fec_burst_validation_requires_three_contiguous_drops(
        self,
    ) -> None:
        fec = timing_interop.FecProfile("column")
        plan = timing_interop.fault_plan("fec-burst-drop", 100)
        sender = {
            "stats": {
                "pktSndFilterExtraTotal": 20,
                "pktRetransTotal": 0,
                "pktRecvNAKTotal": 0,
            }
        }
        receiver = {
            "receiver_filter_extra_packets": 20,
            "receiver_filter_supply_packets": 3,
            "receiver_filter_loss_packets": 0,
        }
        observations = [
            {
                "plan_index": index + 1,
                "action": "drop",
                "direction": "sender_to_receiver",
                "occurrence": occurrence,
                "packet_kind": "data",
                "filter_control": False,
                "retransmitted": False,
                "sequence": 100 + index,
                "message_number": 10 + index,
                "destination_socket_id": 2,
                "key_selection": 1,
                "payload_bytes": 1_316,
                "payload_sha256": chr(ord("a") + index) * 64,
                "later_data_observed_before_retransmission": True,
            }
            for index, occurrence in enumerate(
                timing_interop.FEC_BURST_DROP_OCCURRENCES
            )
        ]
        trace = "\n".join(
            json.dumps(
                {
                    "event": "srt_handshake_trace",
                    "direction": direction,
                    "extensions": [{"name": "FILTER"}],
                }
            )
            for direction in ("sender_to_receiver", "receiver_to_sender")
        )

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [dict(observation) for observation in observations]

            @staticmethod
            def render(_: str) -> str:
                return trace

        faults, evidence = timing_interop.validate_fec_evidence(
            fec, plan, sender, receiver, Relay(), 100, 1_316
        )
        self.assertEqual(len(faults), 3)
        self.assertEqual(evidence["expected_reconstructed_packets"], 3)
        self.assertEqual(evidence["receiver_reconstructed_packets"], 3)
        self.assertEqual(len(evidence["source_drops"]), 3)

        mutations = (
            (0, "plan_index", 2, "exact source DATA"),
            (0, "retransmitted", True, "exact source DATA"),
            (1, "sequence", 104, "not contiguous"),
            (1, "message_number", 14, "not contiguous"),
            (1, "destination_socket_id", 3, "socket identities"),
            (2, "payload_bytes", 1_200, "exact source DATA"),
        )
        for index, field, value, error in mutations:
            with self.subTest(field=field):
                original = observations[index][field]
                observations[index][field] = value
                try:
                    with self.assertRaisesRegex(RuntimeError, error):
                        timing_interop.validate_fec_evidence(
                            fec,
                            plan,
                            sender,
                            receiver,
                            Relay(),
                            100,
                            1_316,
                        )
                finally:
                    observations[index][field] = original

        removed = observations.pop()
        try:
            with self.assertRaisesRegex(RuntimeError, "2/3 planned faults"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            observations.append(removed)

        receiver["receiver_filter_supply_packets"] = 2
        try:
            with self.assertRaisesRegex(RuntimeError, "do not prove"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            receiver["receiver_filter_supply_packets"] = 3

    def test_fec_compound_validation_requires_all_causal_faults(
        self,
    ) -> None:
        fec = timing_interop.FecProfile("matrix")
        plan = timing_interop.fault_plan(
            "fec-burst-delay-reorder", 100
        )
        sender = {
            "stats": {
                "pktSndFilterExtraTotal": 30,
                "pktRetransTotal": 2,
                "pktRecvNAKTotal": 0,
            }
        }
        receiver = {
            "receiver_filter_extra_packets": 30,
            "receiver_filter_supply_packets": 3,
            "receiver_filter_loss_packets": 0,
        }
        observations: list[dict[str, object]] = [
            {
                "plan_index": index + 1,
                "action": "drop",
                "direction": "sender_to_receiver",
                "occurrence": occurrence,
                "packet_kind": "data",
                "filter_control": False,
                "retransmitted": False,
                "sequence": 100 + index,
                "message_number": 10 + index,
                "destination_socket_id": 2,
                "key_selection": 1,
                "payload_bytes": 1_316,
                "payload_sha256": chr(ord("a") + index) * 64,
                "later_data_observed_before_retransmission": True,
            }
            for index, occurrence in enumerate(
                timing_interop.FEC_BURST_DROP_OCCURRENCES
            )
        ]
        observations.extend(
            [
                {
                    "plan_index": 4,
                    "action": "delay",
                    "direction": "sender_to_receiver",
                    "occurrence": 33,
                    "packet_kind": "data",
                    "filter_control": False,
                    "retransmitted": False,
                    "sequence": 200,
                    "message_number": 50,
                    "destination_socket_id": 2,
                    "key_selection": 1,
                    "payload_bytes": 1_316,
                    "payload_sha256": "d" * 64,
                    "delay_milliseconds": 30,
                    "delay_released": True,
                    "delayed_datagrams": 2,
                    "delay_elapsed_milliseconds": 30.1,
                },
                {
                    "plan_index": 5,
                    "action": "reorder",
                    "direction": "sender_to_receiver",
                    "occurrence": 66,
                    "packet_kind": "data",
                    "filter_control": False,
                    "retransmitted": False,
                    "sequence": 300,
                    "message_number": 75,
                    "destination_socket_id": 2,
                    "key_selection": 2,
                    "payload_bytes": 1_316,
                    "payload_sha256": "e" * 64,
                    "reorder_released": True,
                    "reorder_partner_sequence": 301,
                    "reorder_partner_retransmitted": False,
                    "reorder_partner_forwarded_first": True,
                },
            ]
        )
        trace = "\n".join(
            json.dumps(
                {
                    "event": "srt_handshake_trace",
                    "direction": direction,
                    "extensions": [{"name": "FILTER"}],
                }
            )
            for direction in ("sender_to_receiver", "receiver_to_sender")
        )
        retransmissions: list[dict[str, object]] = [
            {
                "direction": "sender_to_receiver",
                "sequence": 400 + index,
                "message_number": 90 + index,
                "key_selection": 1,
                "payload_bytes": 1_316,
                "payload_sha256": format(15 - index, "x") * 64,
                "relay_ordinal": 500 + index,
                "original_observed": True,
                "original_relay_ordinal": 400 + index,
                "retransmission_delay_microseconds": 30_000,
                "original_key_selection": 1,
                "original_payload_bytes": 1_316,
                "original_payload_sha256": format(15 - index, "x") * 64,
                "ciphertext_matches_original": True,
            }
            for index in range(2)
        ]
        trace_complete = True
        loss_reports: list[dict[str, object]] = []

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def fault_observations() -> list[dict[str, object]]:
                return [dict(observation) for observation in observations]

            @staticmethod
            def arq_trace_complete() -> bool:
                return trace_complete

            @staticmethod
            def loss_report_observations() -> list[dict[str, object]]:
                return [dict(event) for event in loss_reports]

            @staticmethod
            def retransmission_observations(
                direction: str,
            ) -> list[dict[str, object]]:
                if direction != "sender_to_receiver":
                    return []
                return [dict(event) for event in retransmissions]

            @staticmethod
            def render(_: str) -> str:
                return trace

        faults, evidence = timing_interop.validate_fec_evidence(
            fec, plan, sender, receiver, Relay(), 100, 1_316
        )
        self.assertEqual(len(faults), 5)
        self.assertEqual(evidence["expected_reconstructed_packets"], 3)
        self.assertEqual(
            [fault["action"] for fault in evidence["companion_faults"]],
            ["delay", "reorder"],
        )
        self.assertEqual(evidence["sender_retransmissions"], 2)
        self.assertEqual(evidence["compound_delay_flight_datagrams"], 2)
        self.assertEqual(
            len(evidence["non_drop_sender_rto_retransmissions"]), 2
        )

        mutations = (
            (3, "delay_released", False, "companion delay"),
            (4, "reorder_partner_forwarded_first", False, "companion reorder"),
            (4, "reorder_partner_sequence", 302, "companion reorder"),
            (3, "destination_socket_id", 3, "socket identities"),
            (4, "loss_report_observed", False, "companion reorder"),
            (4, "plan_index", 4, "exact source DATA"),
        )
        for index, field, value, error in mutations:
            with self.subTest(field=field):
                original = observations[index].get(field)
                observations[index][field] = value
                try:
                    with self.assertRaisesRegex(RuntimeError, error):
                        timing_interop.validate_fec_evidence(
                            fec,
                            plan,
                            sender,
                            receiver,
                            Relay(),
                            100,
                            1_316,
                        )
                finally:
                    if original is None:
                        observations[index].pop(field)
                    else:
                        observations[index][field] = original

        original_retransmission = dict(retransmissions[0])
        retransmissions[0]["sequence"] = observations[0]["sequence"]
        retransmissions[0]["message_number"] = observations[0][
            "message_number"
        ]
        try:
            with self.assertRaisesRegex(RuntimeError, "source-drop identity"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            retransmissions[0].clear()
            retransmissions[0].update(original_retransmission)

        for planned_index in (3, 4):
            with self.subTest(companion_identity=planned_index):
                companion = observations[planned_index]
                retransmissions[0].update(
                    {
                        "sequence": companion["sequence"],
                        "message_number": companion["message_number"],
                        "key_selection": companion["key_selection"],
                        "payload_bytes": companion["payload_bytes"],
                        "payload_sha256": companion["payload_sha256"],
                        "original_key_selection": companion[
                            "key_selection"
                        ],
                        "original_payload_bytes": companion[
                            "payload_bytes"
                        ],
                        "original_payload_sha256": companion[
                            "payload_sha256"
                        ],
                    }
                )
                try:
                    _, companion_evidence = (
                        timing_interop.validate_fec_evidence(
                            fec,
                            plan,
                            sender,
                            receiver,
                            Relay(),
                            100,
                            1_316,
                        )
                    )
                    self.assertEqual(
                        companion_evidence[
                            "non_drop_sender_rto_retransmissions"
                        ][0]["planned_companion_action"],
                        companion["action"],
                    )
                finally:
                    retransmissions[0].clear()
                    retransmissions[0].update(original_retransmission)

        retransmissions[0].update(
            {
                "sequence": observations[3]["sequence"],
                "message_number": observations[3]["message_number"],
                "key_selection": observations[3]["key_selection"],
                "original_key_selection": observations[3]["key_selection"],
            }
        )
        try:
            with self.assertRaisesRegex(RuntimeError, "planned original DATA"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            retransmissions[0].clear()
            retransmissions[0].update(original_retransmission)

        original_sequence = retransmissions[1]["sequence"]
        original_message_number = retransmissions[1]["message_number"]
        retransmissions[1]["sequence"] = retransmissions[0]["sequence"]
        retransmissions[1]["message_number"] = retransmissions[0][
            "message_number"
        ]
        try:
            with self.assertRaisesRegex(RuntimeError, "repeated an unrelated"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            retransmissions[1]["sequence"] = original_sequence
            retransmissions[1]["message_number"] = original_message_number

        retransmissions[1]["relay_ordinal"] = retransmissions[0][
            "relay_ordinal"
        ]
        try:
            with self.assertRaisesRegex(RuntimeError, "relay ordinal"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            retransmissions[1]["relay_ordinal"] = 501

        retransmissions[0]["ciphertext_matches_original"] = False
        try:
            with self.assertRaisesRegex(RuntimeError, "immutable original"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            retransmissions[0]["ciphertext_matches_original"] = True

        for retransmission in retransmissions:
            retransmission["retransmission_delay_microseconds"] = 29_999
        try:
            with self.assertRaisesRegex(RuntimeError, "sender RTO age"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            for retransmission in retransmissions:
                retransmission["retransmission_delay_microseconds"] = 30_000

        removed = retransmissions.pop()
        try:
            with self.assertRaisesRegex(RuntimeError, "counter/trace mismatch"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            retransmissions.append(removed)

        excess = [dict(retransmissions[0]) for _ in range(9)]
        retransmissions.extend(excess)
        sender["stats"]["pktRetransTotal"] = len(retransmissions)
        try:
            with self.assertRaisesRegex(RuntimeError, "causal delay flight"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            del retransmissions[-len(excess) :]
            sender["stats"]["pktRetransTotal"] = 2

        loss_reports.append(
            {
                "direction": "receiver_to_sender",
                "ranges": ((400, 400),),
            }
        )
        try:
            with self.assertRaisesRegex(RuntimeError, "unexpected loss report"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            loss_reports.clear()

        trace_complete = False
        try:
            with self.assertRaisesRegex(RuntimeError, "trace was truncated"):
                timing_interop.validate_fec_evidence(
                    fec, plan, sender, receiver, Relay(), 100, 1_316
                )
        finally:
            trace_complete = True

    def test_timing_receiver_keeps_final_ack_path_alive(self) -> None:
        command = timing_interop.timing_receiver_command(
            Path("timing-peer"),
            "127.0.0.1",
            9_001,
            "127.0.0.1",
            9_002,
            16,
            1_316,
            40,
            10,
        )
        option = command.index("--shutdown-grace-ms")
        self.assertEqual(command[option + 1], "250")

        kernel_command = timing_interop.timing_receiver_command(
            Path("timing-peer"),
            "127.0.0.1",
            9_001,
            "127.0.0.1",
            9_002,
            16,
            1_316,
            40,
            10,
            udp_timestamp_source="linux-software",
        )
        timestamp_source = kernel_command.index("--udp-timestamp-source")
        self.assertEqual(
            kernel_command[timestamp_source + 1], "linux-software"
        )
        self.assertNotIn("--udp-timestamp-source", command)
        with self.assertRaisesRegex(ValueError, "unknown UDP timestamp"):
            timing_interop.timing_receiver_command(
                Path("timing-peer"),
                "127.0.0.1",
                9_001,
                "127.0.0.1",
                9_002,
                16,
                1_316,
                40,
                10,
                udp_timestamp_source="hardware",
            )

    def test_group_outage_sender_outlives_receiver_timing_teardown(
        self,
    ) -> None:
        self.assertEqual(
            timing_interop.group_outage_sender_shutdown_grace(300),
            800,
        )
        self.assertEqual(
            timing_interop.group_outage_sender_shutdown_grace(0),
            500,
        )
        with self.assertRaisesRegex(ValueError, "nonnegative"):
            timing_interop.group_outage_sender_shutdown_grace(-1)

    def test_sender_uses_explicit_source_time_with_source_pacing(self) -> None:
        messages = timing_interop.generated_messages(
            "ts-1316", 2, 15_040_000
        )
        command = timing_interop.sender_command(
            Path("sender-peer"),
            "127.0.0.1",
            9_001,
            Path("timing.input"),
            messages,
            15_040_000,
            120,
            10,
        )
        self.assertIn("--source-pacing", command)
        self.assertIn("--explicit-source-time", command)

    def test_rendezvous_commands_bind_both_public_endpoints(self) -> None:
        messages = timing_interop.generated_messages(
            "binary-1200", 2, 9_600_000
        )
        sender = timing_interop.sender_command(
            Path("sender-peer"),
            "127.0.0.1",
            9_003,
            Path("timing.input"),
            messages,
            9_600_000,
            120,
            10,
            connection_mode="rendezvous",
            local_port=9_001,
        )
        receiver = timing_interop.timing_receiver_command(
            Path("timing-peer"),
            "127.0.0.1",
            9_002,
            "127.0.0.1",
            9_004,
            2,
            1_200,
            120,
            10,
            connection_mode="rendezvous",
            peer_port=9_005,
        )
        self.assertEqual(sender[1], "rendezvous-sender")
        self.assertEqual(
            sender[sender.index("--local-port") + 1], "9001"
        )
        self.assertIn("--rendezvous", receiver)
        self.assertEqual(
            receiver[receiver.index("--peer-port") + 1], "9005"
        )
        with self.assertRaisesRegex(ValueError, "exactly one local port"):
            timing_interop.sender_command(
                Path("sender-peer"),
                "127.0.0.1",
                9_003,
                Path("timing.input"),
                messages,
                9_600_000,
                120,
                10,
                connection_mode="rendezvous",
            )
        with self.assertRaisesRegex(ValueError, "exactly one peer port"):
            timing_interop.timing_receiver_command(
                Path("timing-peer"),
                "127.0.0.1",
                9_002,
                "127.0.0.1",
                9_004,
                2,
                1_200,
                120,
                10,
                peer_port=9_005,
            )

    def test_backup_group_commands_bind_two_members_and_one_receiver(
        self,
    ) -> None:
        messages = timing_interop.generated_messages(
            "binary-1200", 4, 9_600_000
        )
        sender = timing_interop.group_sender_command(
            Path("group-sender"),
            "127.0.0.1",
            9_001,
            Path("timing.input"),
            messages,
            9_600_000,
            10,
            2,
        )
        receiver = timing_interop.timing_receiver_command(
            Path("timing-peer"),
            "127.0.0.1",
            9_001,
            "127.0.0.1",
            9_002,
            4,
            1_200,
            120,
            10,
            connection_mode="backup-group",
        )
        self.assertEqual(
            sender[sender.index("--primary-port") + 1], "9001"
        )
        self.assertEqual(
            sender[sender.index("--backup-port") + 1], "9001"
        )
        self.assertEqual(
            sender[sender.index("--failover-after") + 1], "2"
        )
        self.assertEqual(
            sender[sender.index("--minimum-stability-ms") + 1], "5000"
        )
        self.assertEqual(
            receiver[receiver.index("--group-members") + 1], "2"
        )
        self.assertEqual(
            receiver[receiver.index("--max-message-size") + 1], "1316"
        )
        self.assertNotIn("--passphrase-env", sender)
        self.assertNotIn("--undrained-failover", sender)
        with self.assertRaisesRegex(ValueError, "interior failover"):
            timing_interop.group_sender_command(
                Path("group-sender"),
                "127.0.0.1",
                9_001,
                Path("timing.input"),
                messages,
                9_600_000,
                10,
                4,
            )
        ipv6_sender = timing_interop.group_sender_command(
            Path("group-sender"),
            "::1",
            9_001,
            Path("timing.input"),
            messages,
            9_600_000,
            10,
            2,
            backup_port=9_002,
        )
        self.assertEqual(ipv6_sender[ipv6_sender.index("--host") + 1], "::1")
        self.assertEqual(
            ipv6_sender[ipv6_sender.index("--backup-port") + 1], "9002"
        )
        with self.assertRaises(ValueError):
            timing_interop.group_sender_command(
                Path("group-sender"),
                "localhost",
                9_001,
                Path("timing.input"),
                messages,
                9_600_000,
                10,
                2,
            )

    def test_encrypted_backup_group_command_configures_each_member(self) -> None:
        security = timing_interop.SecurityProfile(
            key_length=32,
            key_refresh_rate=64,
            key_preannouncement=20,
            minimum_key_transitions=3,
            crypto_mode="gcm",
        )
        messages = timing_interop.generated_messages(
            "ts-1316", 384, 7_520_000
        )
        sender = timing_interop.group_sender_command(
            Path("group-sender"),
            "127.0.0.1",
            9_001,
            Path("timing.input"),
            messages,
            7_520_000,
            15,
            192,
            backup_port=9_002,
            security=security,
        )
        receiver = timing_interop.timing_receiver_command(
            Path("timing-peer"),
            "127.0.0.1",
            9_003,
            "127.0.0.1",
            9_004,
            384,
            1_316,
            120,
            15,
            connection_mode="backup-group",
            security=security,
        )
        self.assertEqual(
            sender[sender.index("--primary-port") + 1], "9001"
        )
        self.assertEqual(
            sender[sender.index("--backup-port") + 1], "9002"
        )
        self.assertEqual(
            sender[sender.index("--passphrase-env") + 1],
            timing_interop.PASSPHRASE_ENVIRONMENT,
        )
        self.assertEqual(sender[sender.index("--pbkeylen") + 1], "32")
        self.assertEqual(
            sender[sender.index("--km-refresh-rate") + 1], "64"
        )
        self.assertEqual(
            sender[sender.index("--km-preannounce") + 1], "20"
        )
        self.assertEqual(
            sender[sender.index("--crypto-mode") + 1], "gcm"
        )
        self.assertEqual(
            sender[sender.index("--minimum-stability-ms") + 1], "5000"
        )
        self.assertEqual(
            receiver[receiver.index("--group-members") + 1], "2"
        )
        self.assertEqual(
            receiver[receiver.index("--passphrase-env") + 1],
            timing_interop.PASSPHRASE_ENVIRONMENT,
        )
        self.assertEqual(receiver[receiver.index("--pbkeylen") + 1], "32")
        self.assertEqual(
            receiver[receiver.index("--crypto-mode") + 1], "gcm"
        )
        self.assertNotIn("--packet-filter", receiver)
        self.assertNotIn("--rendezvous", receiver)

        replay_sender = timing_interop.group_sender_command(
            Path("group-sender"),
            "127.0.0.1",
            9_001,
            Path("timing.input"),
            messages,
            7_520_000,
            15,
            192,
            backup_port=9_002,
            security=security,
            unacknowledged_replay=True,
        )
        self.assertIn("--undrained-failover", replay_sender)

        outage_sender = timing_interop.group_sender_command(
            Path("group-sender"),
            "127.0.0.1",
            9_001,
            Path("timing.input"),
            messages,
            7_520_000,
            15,
            192,
            backup_port=9_002,
            security=security,
            external_path_outage=True,
            shutdown_grace_milliseconds=1_450,
            peer_idle_timeout_milliseconds=1_000,
        )
        self.assertIn("--external-path-outage", outage_sender)
        self.assertEqual(
            outage_sender[
                outage_sender.index("--minimum-stability-ms") + 1
            ],
            "60",
        )
        self.assertEqual(
            outage_sender[
                outage_sender.index("--shutdown-grace-ms") + 1
            ],
            "1450",
        )
        self.assertEqual(
            outage_sender[
                outage_sender.index("--peer-idle-timeout-ms") + 1
            ],
            "1000",
        )
        with self.assertRaisesRegex(ValueError, "mutually exclusive"):
            timing_interop.group_sender_command(
                Path("group-sender"),
                "127.0.0.1",
                9_001,
                Path("timing.input"),
                messages,
                7_520_000,
                15,
                192,
                security=security,
                unacknowledged_replay=True,
                external_path_outage=True,
                peer_idle_timeout_milliseconds=1_000,
            )

    def test_group_path_outage_blackholes_only_the_primary_path(
        self,
    ) -> None:
        coordinator = timing_interop.GroupPathOutageCoordinator(2)
        first = self._data_packet(100, 1)
        trigger = self._data_packet(101, 1)
        replacement_retransmission = self._data_packet(
            101, 1, retransmitted=True
        )
        replacement = self._data_packet(102, 1)
        control = self._key_material_packet(3, 1)

        self.assertTrue(
            coordinator.should_forward(
                "primary", control, "listener_to_caller"
            )
        )
        self.assertTrue(
            coordinator.should_forward(
                "primary", first, "caller_to_listener"
            )
        )
        self.assertFalse(
            coordinator.should_forward(
                "primary", trigger, "caller_to_listener"
            )
        )
        self.assertFalse(
            coordinator.should_forward(
                "primary", control, "listener_to_caller"
            )
        )
        self.assertTrue(
            coordinator.should_forward(
                "backup",
                replacement_retransmission,
                "caller_to_listener",
            )
        )
        self.assertTrue(
            coordinator.should_forward(
                "backup", replacement, "caller_to_listener"
            )
        )

        observation = coordinator.observation()
        self.assertEqual(observation["primary_original_data_count"], 2)
        self.assertEqual(observation["dropped_primary_datagrams"], 2)
        self.assertEqual(observation["dropped_primary_data_datagrams"], 1)
        self.assertEqual(observation["outage_sequence"], 101)
        self.assertEqual(observation["first_backup_sequence"], 102)
        self.assertGreaterEqual(
            observation["first_backup_data_delay_microseconds"], 0
        )

    @staticmethod
    def _backup_group_path_outage_evidence() -> tuple[
        str,
        dict[str, object],
        list[dict[str, object]],
        object,
    ]:
        origin = 1_000_000
        outage_after = 6
        transition = 8
        sender_events: list[dict[str, object]] = [
            {
                "event": "group_connected",
                "primary_member": 10,
                "backup_member": 11,
            }
        ]
        for index in range(10):
            sender_events.append(
                {
                    "event": "group_send",
                    "message_index": index,
                    "source_time_microseconds": origin + index * 1_250,
                    "running_member": 10 if index < transition else 11,
                    "member_count": 2,
                }
            )
        sender_events.append(
            {
                "event": "group_failover",
                "after_messages": transition,
                "outage_after_messages": outage_after,
                "closed_member": 10,
                "replacement_member": 11,
                "failure_injection": "external-path-outage",
                "primary_closed": False,
                "primary_drained": False,
            }
        )
        sender_complete: dict[str, object] = {
            "event": "complete",
            "role": "group-timing-sender",
            "bytes": 12_000,
            "messages": 10,
            "primary_closed": False,
            "primary_drained": False,
            "external_path_outage": True,
        }
        receiver_members = [20, 20, 21, 20, 21, 20, 21, 21, 21, 21]
        receiver_events = [
            {
                "message_index": index,
                "group_running_member": receiver_members[index],
                "group_running_member_count": 1,
                "group_member_count": 2,
            }
            for index in range(10)
        ]

        class PathRelay:
            def __init__(
                self, destination: int, sequences: tuple[int, ...]
            ) -> None:
                self.destination = destination
                self.sequences = sequences

            @staticmethod
            def error() -> None:
                return None

            def data_packet_observations(
                self, direction: str
            ) -> tuple[dict[str, object], ...]:
                if direction != "caller_to_listener":
                    return ()
                return tuple(
                    {
                        "destination_socket_id": self.destination,
                        "sequence": sequence,
                        "retransmitted": False,
                    }
                    for sequence in self.sequences
                )

        class Relay:
            primary = PathRelay(20, (95, 96, 97, 98, 99, 100))
            backup = PathRelay(21, (200,))

            @staticmethod
            def outage_observation() -> dict[str, object]:
                return {
                    "trigger_original_data_count": outage_after,
                    "primary_original_data_count": outage_after,
                    "dropped_primary_datagrams": 3,
                    "dropped_primary_data_datagrams": 2,
                    "outage_started_monotonic_ns": 1_000_000,
                    "outage_sequence": 100,
                    "first_backup_data_monotonic_ns": 1_200_000,
                    "first_backup_sequence": 200,
                    "first_backup_data_delay_microseconds": 200,
                }

        return (
            "\n".join(json.dumps(event) for event in sender_events),
            sender_complete,
            receiver_events,
            Relay(),
        )

    def test_group_path_outage_trigger_ignores_retransmissions(self) -> None:
        sender, complete, receiver, relay = self._backup_group_path_outage_evidence()
        original_packets = relay.primary.data_packet_observations("caller_to_listener")
        # The coordinator counts original DATA, while the wire trace also
        # contains retransmissions preceding the outage-triggering packet.
        packets = (original_packets[0],
                   {**original_packets[0], "retransmitted": True},
                   *original_packets[1:])
        relay.primary.data_packet_observations = lambda direction: packets
        observation = timing_interop.validate_backup_group_path_outage(
            sender, complete, receiver, relay, 6, 1_000_000, 1_200, 960_000, 4)
        self.assertEqual(observation["outage_sequence"], 100)

    def test_group_path_outage_still_rejects_invalid_wire_evidence(self) -> None:
        sender, complete, receiver, relay = self._backup_group_path_outage_evidence()
        original = relay.primary.data_packet_observations("caller_to_listener")
        for name, packets in (
            ("wrong original trigger", (*original[:-1], {**original[-1], "sequence": 101})),
            ("missing original trigger", (*original[:-1], {**original[-1], "retransmitted": True})),
            ("unknown original flag", (*original[:-1], {**original[-1], "retransmitted": None})),
            ("foreign retransmission destination", (
                original[0], {**original[0], "retransmitted": True, "destination_socket_id": 22},
                *original[1:])),
        ):
            with self.subTest(name=name):
                relay.primary.data_packet_observations = lambda direction: packets
                with self.assertRaisesRegex(RuntimeError, "identities"):
                    timing_interop.validate_backup_group_path_outage(
                        sender, complete, receiver, relay, 6, 1_000_000, 1_200, 960_000, 4)

    def test_outage_snapshot_retains_incomplete_trace_reason(self) -> None:
        _, _, _, relay = self._backup_group_path_outage_evidence()
        with mock.patch.object(relay.primary, "data_packet_observations",
                               side_effect=RuntimeError("trace is incomplete")):
            evidence = timing_interop.snapshot_path_outage_evidence(relay)
            self.assertEqual(evidence["primary"], {"error": "trace is incomplete"})
            self.assertEqual(evidence["backup"][0]["destination_socket_id"], 21)
            with self.assertRaisesRegex(RuntimeError, "trace is incomplete"):
                relay.primary.data_packet_observations("caller_to_listener")

    def test_group_path_outage_requires_bounded_causal_evidence(
        self,
    ) -> None:
        sender, complete, receiver, relay = (
            self._backup_group_path_outage_evidence()
        )
        observation = timing_interop.validate_backup_group_path_outage(
            sender,
            complete,
            receiver,
            relay,
            6,
            1_000_000,
            1_200,
            960_000,
            4,
        )
        self.assertEqual(observation["sender_transition_after_messages"], 8)
        self.assertEqual(
            observation["first_backup_data_delay_microseconds"], 200
        )
        self.assertEqual(
            observation["sender_active_transition_delay_microseconds"],
            3_750,
        )
        self.assertEqual(observation["receiver_member_transitions"], 5)

        ambiguous_receiver = [
            {
                **event,
                "group_running_member": 0,
                "group_running_member_count": 2,
            }
            for event in receiver
        ]
        ambiguous_observation = (
            timing_interop.validate_backup_group_path_outage(
                sender,
                complete,
                ambiguous_receiver,
                relay,
                6,
                1_000_000,
                1_200,
                960_000,
                4,
            )
        )
        self.assertEqual(
            ambiguous_observation["receiver_member_source_attribution"],
            "not-exposed-by-peer-api",
        )
        self.assertIsNone(
            ambiguous_observation["receiver_member_transitions"]
        )

        for name, mutated_complete, mutated_receiver in (
            (
                "wrong role",
                {**complete, "role": "caller"},
                receiver,
            ),
            (
                "unknown receiver member",
                complete,
                [
                    event
                    if index != 5
                    else {**event, "group_running_member": 22}
                    for index, event in enumerate(receiver)
                ],
            ),
            (
                "receiver returned to primary after outage",
                complete,
                [
                    event
                    if index != 7
                    else {**event, "group_running_member": 20}
                    for index, event in enumerate(receiver)
                ],
            ),
        ):
            with self.subTest(name=name):
                with self.assertRaises(RuntimeError):
                    timing_interop.validate_backup_group_path_outage(
                        sender,
                        mutated_complete,
                        mutated_receiver,
                        relay,
                        6,
                        1_000_000,
                        1_200,
                        960_000,
                        4,
                    )

        relay.primary.destination = 21
        try:
            with self.assertRaisesRegex(RuntimeError, "identities"):
                timing_interop.validate_backup_group_path_outage(
                    sender,
                    complete,
                    receiver,
                    relay,
                    6,
                    1_000_000,
                    1_200,
                    960_000,
                    4,
                )
        finally:
            relay.primary.destination = 20

        original_observation = relay.outage_observation
        relay.outage_observation = lambda: {
            **original_observation(),
            "first_backup_data_delay_microseconds": 5_001,
            "first_backup_data_monotonic_ns": 2_001_000,
        }
        with self.assertRaisesRegex(RuntimeError, "relay evidence"):
            timing_interop.validate_backup_group_path_outage(
                sender,
                complete,
                receiver,
                relay,
                6,
                1_000_000,
                1_200,
                960_000,
                4,
            )

    @staticmethod
    def _backup_group_timing_evidence() -> tuple[
        str, dict[str, object], list[dict[str, object]]
    ]:
        origin = 1_000_000
        sender_events: list[dict[str, object]] = [
            {
                "event": "group_connected",
                "primary_member": 10,
                "backup_member": 11,
            }
        ]
        for index in range(6):
            sender_events.append(
                {
                    "event": "group_send",
                    "message_index": index,
                    "source_time_microseconds": origin + index * 1_250,
                    "running_member": 10 if index < 3 else 11,
                    "member_count": 2 if index < 3 else 1,
                }
            )
        sender_events.append(
            {
                "event": "group_failover",
                "after_messages": 3,
                "closed_member": 10,
                "replacement_member": 11,
                "primary_drained": True,
            }
        )
        receiver_events = [
            {
                "message_index": index,
                "group_running_member": 20 if index < 3 else 21,
                "group_member_count": 2 if index < 3 else 1,
            }
            for index in range(6)
        ]
        return (
            "\n".join(json.dumps(event) for event in sender_events),
            {
                "messages": 6,
                "primary_closed": True,
                "primary_drained": True,
            },
            receiver_events,
        )

    def test_backup_group_timing_requires_bounded_receiver_settlement(
        self,
    ) -> None:
        sender, complete, receiver = self._backup_group_timing_evidence()
        observation = timing_interop.validate_backup_group_timing(
            sender,
            complete,
            receiver,
            3,
            1_000_000,
            1_200,
            960_000,
        )
        self.assertEqual(observation["member_transitions"], 1)
        self.assertEqual(observation["sender_primary_member"], 10)
        self.assertEqual(observation["receiver_primary_member"], 20)
        self.assertNotEqual(
            observation["sender_primary_member"],
            observation["receiver_primary_member"],
        )

        buffered_status_transition = [
            {
                **event,
                "group_running_member": 20 if index == 0 else 21,
            }
            for index, event in enumerate(receiver)
        ]
        buffered_observation = (
            timing_interop.validate_backup_group_timing(
                sender,
                complete,
                buffered_status_transition,
                3,
                1_000_000,
                1_200,
                960_000,
            )
        )
        self.assertEqual(
            buffered_observation["receiver_transition_after_messages"], 1
        )

        settling_status_transition = [
            {
                **event,
                "group_running_member": (20, 21, 20, 21, 21, 21)[index],
            }
            for index, event in enumerate(receiver)
        ]
        settling_observation = timing_interop.validate_backup_group_timing(
            sender,
            complete,
            settling_status_transition,
            3,
            1_000_000,
            1_200,
            960_000,
        )
        self.assertEqual(
            settling_observation["receiver_member_transitions"], 3
        )
        self.assertEqual(
            settling_observation["receiver_transition_after_messages"], 3
        )

        replay_sender = sender.replace(
            '"primary_drained": true', '"primary_drained": false'
        )
        replay_complete = {**complete, "primary_drained": False}
        replay_receiver = [
            {**event, "group_running_member": 20 + index % 2}
            for index, event in enumerate(receiver)
        ]
        replay_observation = timing_interop.validate_backup_group_timing(
            replay_sender,
            replay_complete,
            replay_receiver,
            3,
            1_000_000,
            1_200,
            960_000,
            unacknowledged_replay=True,
        )
        self.assertEqual(replay_observation["receiver_member_transitions"], 5)
        self.assertEqual(
            replay_observation["receiver_observed_members"], [20, 21]
        )

        mutations = (
            ("missing failover", sender.rsplit("\n", 1)[0], receiver),
            (
                "early sender transition",
                sender.replace(
                    '"running_member": 10, "member_count": 2}',
                    '"running_member": 11, "member_count": 2}',
                    1,
                ),
                receiver,
            ),
            (
                "no receiver transition",
                sender,
                [
                    {**event, "group_running_member": 20}
                    for event in receiver
                ],
            ),
            (
                "receiver returned to primary after failover",
                sender,
                [
                    event
                    if index != 4
                    else {**event, "group_running_member": 20}
                    for index, event in enumerate(receiver)
                ],
            ),
            (
                "receiver settled after failover",
                sender,
                [
                    {
                        **event,
                        "group_running_member": 20 if index < 4 else 21,
                    }
                    for index, event in enumerate(receiver)
                ],
            ),
            (
                "ambiguous receiver members",
                sender,
                [
                    event
                    if index != 1
                    else {**event, "group_member_count": 3}
                    for index, event in enumerate(receiver)
                ],
            ),
        )
        for name, mutated_sender, mutated_receiver in mutations:
            with self.subTest(name=name):
                with self.assertRaises(RuntimeError):
                    timing_interop.validate_backup_group_timing(
                        mutated_sender,
                        complete,
                        mutated_receiver,
                        3,
                        1_000_000,
                        1_200,
                        960_000,
                    )

    def test_unacknowledged_group_replay_requires_exact_reencrypted_prefix(
        self,
    ) -> None:
        def packet(
            sequence: int,
            message_number: int,
            destination: int,
            digest_character: str,
        ) -> dict[str, object]:
            return {
                "direction": "caller_to_listener",
                "relay_ordinal": message_number,
                "sequence": sequence,
                "message_number": message_number,
                "packet_position": 3,
                "in_order": True,
                "key_selection": 1,
                "retransmitted": False,
                "timestamp": 10_000 + message_number * 1_250,
                "destination_socket_id": destination,
                "payload_bytes": 1_200,
                "ciphertext_sha256": digest_character * 64,
            }

        primary_packets = [
            packet(100, 1, 20, "a"),
            packet(101, 2, 20, "b"),
        ]
        backup_packets = [
            packet(100, 1, 21, "c"),
            packet(101, 2, 21, "d"),
            packet(102, 3, 21, "e"),
            packet(103, 4, 21, "f"),
        ]
        for event in backup_packets:
            event["timestamp"] = int(event["timestamp"]) + 400_000

        class PathRelay:
            def __init__(self, packets: list[dict[str, object]]) -> None:
                self.packets = packets

            @staticmethod
            def error() -> None:
                return None

            def data_packet_observations(
                self, direction: str
            ) -> tuple[dict[str, object], ...]:
                return tuple(
                    dict(event)
                    for event in self.packets
                    if event["direction"] == direction
                )

        class PrimaryRelay(PathRelay):
            @staticmethod
            def acknowledgement_observation() -> dict[str, object]:
                acknowledgement = {
                    "direction": "listener_to_caller",
                    "acknowledgement_number": 7,
                    "next_sequence": 102,
                    "payload_bytes": 28,
                    "relay_ordinal": 9,
                }
                return {
                    "suppressed": 1,
                    "forwarded": 0,
                    "acknowledgements": [acknowledgement],
                }

        class GroupRelay:
            def __init__(self) -> None:
                self.primary = PrimaryRelay(primary_packets)
                self.backup = PathRelay(backup_packets)

        relay = GroupRelay()
        observation = timing_interop.validate_unacknowledged_group_replay(
            relay, 2, 4
        )
        self.assertEqual(observation["replayed_messages"], 2)
        self.assertTrue(observation["metadata_preserved"])
        self.assertEqual(
            observation["member_local_timestamp_rebase_microseconds"],
            400_000,
        )
        self.assertTrue(observation["replacement_ciphertext_distinct"])
        timing_interop.validate_group_replay_receiver_binding(
            {"receiver_observed_members": [20, 21]}, observation
        )
        with self.assertRaisesRegex(RuntimeError, "traced members"):
            timing_interop.validate_group_replay_receiver_binding(
                {"receiver_observed_members": [20, 22]}, observation
            )

        original_backup_first = dict(relay.backup.packets[0])
        relay.backup.packets[0] = {
            **relay.backup.packets[0],
            "ciphertext_sha256": "a" * 64,
        }
        with self.assertRaisesRegex(RuntimeError, "re-encrypt"):
            timing_interop.validate_unacknowledged_group_replay(relay, 2, 4)

        relay.backup.packets[0] = original_backup_first
        relay.backup.packets[0]["timestamp"] = 99
        with self.assertRaisesRegex(RuntimeError, "member-local timebase"):
            timing_interop.validate_unacknowledged_group_replay(relay, 2, 4)

    def test_replay_proxy_suppresses_only_receiver_runtime_acks(self) -> None:
        acknowledgement = struct.pack(
            ">IIIII", 0x8002_0000, 7, 123, 21, 456
        )
        acknowledgement_confirmation = struct.pack(
            ">IIII", 0x8006_0000, 7, 124, 21
        )
        malformed_acknowledgement = acknowledgement[:16]
        data = self._data_packet(456, 1)
        proxy = timing_interop.AckSuppressingTimingTraceProxy(9_001)
        try:
            proxy._record_wire(acknowledgement, "listener_to_caller")
            self.assertFalse(
                proxy._should_forward_wire(
                    acknowledgement, "listener_to_caller"
                )
            )
            self.assertTrue(
                proxy._should_forward_wire(
                    acknowledgement, "caller_to_listener"
                )
            )
            self.assertTrue(
                proxy._should_forward_wire(
                    acknowledgement_confirmation, "listener_to_caller"
                )
            )
            self.assertTrue(
                proxy._should_forward_wire(
                    malformed_acknowledgement, "listener_to_caller"
                )
            )
            self.assertTrue(
                proxy._should_forward_wire(data, "caller_to_listener")
            )
            observation = proxy.acknowledgement_observation()
        finally:
            proxy.close()
        self.assertEqual(observation["suppressed"], 1)
        self.assertEqual(observation["forwarded"], 0)
        self.assertEqual(
            observation["acknowledgements"],
            [
                {
                    "direction": "listener_to_caller",
                    "acknowledgement_number": 7,
                    "next_sequence": 456,
                    "payload_bytes": 4,
                    "relay_ordinal": 1,
                }
            ],
        )

    def test_encrypted_commands_keep_secret_out_of_arguments(self) -> None:
        security = timing_interop.SecurityProfile(
            key_length=32,
            key_refresh_rate=64,
            key_preannouncement=20,
            minimum_key_transitions=3,
        )
        messages = timing_interop.generated_messages(
            "binary-1200", 4, 7_680_000
        )
        sender = timing_interop.sender_command(
            Path("sender-peer"),
            "127.0.0.1",
            9_001,
            Path("timing.input"),
            messages,
            7_680_000,
            120,
            10,
            security=security,
        )
        receiver = timing_interop.timing_receiver_command(
            Path("timing-peer"),
            "127.0.0.1",
            9_001,
            "127.0.0.1",
            9_002,
            4,
            1_200,
            120,
            10,
            security=security,
        )
        for command in (sender, receiver):
            passphrase = command.index("--passphrase-env")
            self.assertEqual(
                command[passphrase + 1],
                timing_interop.PASSPHRASE_ENVIRONMENT,
            )
            key_length = command.index("--pbkeylen")
            self.assertEqual(command[key_length + 1], "32")
        self.assertEqual(
            sender[sender.index("--km-refresh-rate") + 1], "64"
        )
        self.assertEqual(
            sender[sender.index("--km-preannounce") + 1], "20"
        )
        self.assertNotIn("--km-refresh-rate", receiver)
        self.assertNotIn("--km-preannounce", receiver)

    def test_fec_commands_are_symmetric_and_geometry_bounded(self) -> None:
        messages = timing_interop.generated_messages(
            "ts-1316", 100, 7_520_000
        )
        profiles = {
            "row": (
                "fec,arq:onreq,cols:10,layout:even,rows:1",
                10,
                4,
            ),
            "column": (
                "fec,arq:onreq,cols:10,layout:even,rows:-5",
                20,
                0,
            ),
            "matrix": (
                "fec,arq:onreq,cols:10,layout:even,rows:5",
                30,
                4,
            ),
        }
        for name, (packet_filter, controls, partial_controls) in profiles.items():
            with self.subTest(name=name):
                fec = timing_interop.FecProfile(name)
                self.assertEqual(fec.packet_filter, packet_filter)
                sender = timing_interop.sender_command(
                    Path("sender-peer"),
                    "127.0.0.1",
                    9_001,
                    Path("timing.input"),
                    messages,
                    7_520_000,
                    300,
                    15,
                    fec=fec,
                )
                receiver = timing_interop.timing_receiver_command(
                    Path("timing-peer"),
                    "127.0.0.1",
                    9_001,
                    "127.0.0.1",
                    9_002,
                    100,
                    1_316,
                    300,
                    15,
                    fec=fec,
                )
                for command in (sender, receiver):
                    option = command.index("--packet-filter")
                    self.assertEqual(command[option + 1], fec.packet_filter)
                self.assertEqual(
                    fec.expected_control_packets(100), controls
                )
                self.assertEqual(
                    fec.expected_control_packets(49), partial_controls
                )
        self.assertEqual(
            timing_interop.FecProfile("row").expected_control_packets(384),
            38,
        )
        self.assertEqual(
            timing_interop.FecProfile("column").expected_control_packets(384),
            70,
        )
        self.assertEqual(
            timing_interop.FecProfile("matrix").expected_control_packets(384),
            108,
        )
        with self.assertRaisesRegex(ValueError, "unknown timing FEC"):
            timing_interop.FecProfile("unknown")

    def test_security_profile_is_fail_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "clear timing"):
            timing_interop.SecurityProfile(key_refresh_rate=64)
        with self.assertRaisesRegex(ValueError, "clear timing"):
            timing_interop.SecurityProfile(crypto_mode="gcm")
        for profile in (
            timing_interop.SecurityProfile,
            lambda: timing_interop.SecurityProfile(
                key_length=20,
                key_refresh_rate=64,
                key_preannouncement=20,
                minimum_key_transitions=3,
            ),
            lambda: timing_interop.SecurityProfile(
                key_length=32,
                key_refresh_rate=64,
                key_preannouncement=32,
                minimum_key_transitions=3,
            ),
        ):
            with self.subTest(profile=profile):
                if profile is timing_interop.SecurityProfile:
                    self.assertFalse(profile().encrypted)
                else:
                    with self.assertRaisesRegex(
                        ValueError, "invalid encrypted"
                    ):
                        profile()

    def test_sender_source_timeline_is_required_and_rate_checked(self) -> None:
        output = json.dumps(
            {
                "event": "source_timeline",
                "origin_microseconds": 1_234_567,
                "bytes_per_second": 940_000,
            }
        )
        self.assertEqual(
            timing_interop.parse_sender_source_timeline(output, 940_000),
            1_234_567,
        )
        with self.assertRaisesRegex(RuntimeError, "0 explicit"):
            timing_interop.parse_sender_source_timeline("", 940_000)
        with self.assertRaisesRegex(RuntimeError, "source rate differs"):
            timing_interop.parse_sender_source_timeline(output, 1)

    def test_receiver_output_requires_latency_timing_and_completion(
        self,
    ) -> None:
        output = "\n".join(
            (
                json.dumps(
                    {
                        "event": "connected",
                        "receiver_latency_microseconds": 120_000,
                    }
                ),
                json.dumps(
                    {
                        "event": "timing",
                        "message_index": 0,
                    }
                ),
                json.dumps({"event": "complete", "messages": 1}),
            )
        )
        latency, events, complete = timing_interop.parse_receiver_output(
            output, 1
        )
        self.assertEqual(latency, 120_000)
        self.assertEqual(len(events), 1)
        self.assertEqual(complete["messages"], 1)

        with self.assertRaisesRegex(RuntimeError, "connected events"):
            timing_interop.parse_receiver_output(
                json.dumps({"event": "complete", "messages": 0}), 0
            )
        with self.assertRaisesRegex(RuntimeError, "observations"):
            timing_interop.parse_receiver_output(output, 2)

        group_output = output.replace(
            '"receiver_latency_microseconds": 120000',
            '"receiver_latency_microseconds": 120000, "group_members": 2',
        )
        timing_interop.parse_receiver_output(
            group_output, 1, expected_group_members=2
        )
        with self.assertRaisesRegex(RuntimeError, "reported group"):
            timing_interop.parse_receiver_output(group_output, 1)
        with self.assertRaisesRegex(RuntimeError, "requested group"):
            timing_interop.parse_receiver_output(
                output, 1, expected_group_members=2
            )

    def test_kernel_udp_timestamp_evidence_is_exact_and_correlated(
        self,
    ) -> None:
        events = [
            {
                "message_index": index,
                "udp_userspace_realtime_nanoseconds": userspace,
                "udp_kernel_software_tx_realtime_nanoseconds": kernel,
                "udp_kernel_timestamp_id": index,
            }
            for index, (userspace, kernel) in enumerate(
                (
                    (1_000_000, 1_000_100),
                    (2_000_000, 2_000_300),
                    (3_000_000, 2_999_900),
                )
            )
        ]
        complete = {
            "event": "complete",
            "messages": 3,
            "udp_kernel_timestamp_source": "linux-software-tx",
            "udp_kernel_timestamp_clock": "CLOCK_REALTIME",
            "udp_kernel_timestamp_hardware": False,
            "udp_kernel_timestamps": 3,
        }
        evidence = timing_interop.validate_udp_timestamp_evidence(
            events, complete, "linux-software"
        )
        self.assertEqual(evidence["correlated_messages"], 3)
        self.assertFalse(evidence["hardware"])
        self.assertEqual(
            evidence[
                "kernel_tx_minus_userspace_send_completion_nanoseconds"
            ],
            {
                "minimum": -100,
                "p50": 100,
                "p99": 300,
                "maximum": 300,
            },
        )
        self.assertEqual(
            timing_interop.validate_udp_timestamp_evidence(
                [], {"event": "complete", "messages": 0}, "userspace"
            ),
            {"requested": False},
        )

        mutations = (
            ("udp_kernel_timestamp_id", 2, "not correlated"),
            (
                "udp_kernel_software_tx_realtime_nanoseconds",
                0,
                "not correlated",
            ),
            ("udp_kernel_unexpected", 1, "metadata differs"),
        )
        for field, value, error in mutations:
            with self.subTest(field=field):
                mutated = [dict(event) for event in events]
                mutated[0][field] = value
                with self.assertRaisesRegex(RuntimeError, error):
                    timing_interop.validate_udp_timestamp_evidence(
                        mutated, complete, "linux-software"
                    )

        hardware = dict(complete)
        hardware["udp_kernel_timestamp_hardware"] = True
        with self.assertRaisesRegex(RuntimeError, "evidence differs"):
            timing_interop.validate_udp_timestamp_evidence(
                events, hardware, "linux-software"
            )
        missing = dict(complete)
        missing.pop("udp_kernel_timestamp_clock")
        with self.assertRaisesRegex(RuntimeError, "metadata differs"):
            timing_interop.validate_udp_timestamp_evidence(
                events, missing, "linux-software"
            )
        with self.assertRaisesRegex(RuntimeError, "unexpectedly reported"):
            timing_interop.validate_udp_timestamp_evidence(
                events, complete, "userspace"
            )
        with self.assertRaisesRegex(ValueError, "unknown UDP timestamp"):
            timing_interop.validate_udp_timestamp_evidence(
                events, complete, "hardware"
            )

    def test_receiver_requires_exact_negotiated_security(self) -> None:
        connected = {
            "event": "connected",
            "receiver_latency_microseconds": 120_000,
            "receiver_key_length": 32,
            "receiver_key_state": timing_interop.SECURED_KEY_STATE,
            "receiver_crypto_mode": 2,
        }
        output = "\n".join(
            (
                json.dumps(connected),
                json.dumps(
                    {
                        "event": "complete",
                        "messages": 0,
                        "receiver_undecryptable_packets": 0,
                    }
                ),
            )
        )
        latency, events, complete = timing_interop.parse_receiver_output(
            output, 0, 32, expected_crypto_mode="gcm"
        )
        self.assertEqual(latency, 120_000)
        self.assertEqual(events, [])
        self.assertEqual(complete["receiver_undecryptable_packets"], 0)
        connected["receiver_key_state"] = 1
        with self.assertRaisesRegex(RuntimeError, "requested security"):
            timing_interop.parse_receiver_output(
                "\n".join(
                    (
                        json.dumps(connected),
                        json.dumps(
                            {
                                "event": "complete",
                                "messages": 0,
                                "receiver_undecryptable_packets": 0,
                            }
                        ),
                    )
                ),
                0,
                32,
                expected_crypto_mode="gcm",
            )
        connected["receiver_key_state"] = timing_interop.SECURED_KEY_STATE
        connected["receiver_crypto_mode"] = 1
        with self.assertRaisesRegex(RuntimeError, "requested crypto mode"):
            timing_interop.parse_receiver_output(
                "\n".join(
                    (
                        json.dumps(connected),
                        json.dumps(
                            {
                                "event": "complete",
                                "messages": 0,
                                "receiver_undecryptable_packets": 0,
                            }
                        ),
                    )
                ),
                0,
                32,
                expected_crypto_mode="gcm",
            )
        connected["receiver_crypto_mode"] = 2
        with self.assertRaisesRegex(RuntimeError, "unexpectedly reported"):
            timing_interop.parse_receiver_output(output, 0, 32)

        with self.assertRaisesRegex(RuntimeError, "undecryptable packets"):
            timing_interop.parse_receiver_output(
                "\n".join(
                    (
                        json.dumps(connected),
                        json.dumps(
                            {
                                "event": "complete",
                                "messages": 0,
                                "receiver_undecryptable_packets": 1,
                            }
                        ),
                    )
                ),
                0,
                32,
                expected_crypto_mode="gcm",
            )

    def test_receiver_requires_exact_negotiated_fec_and_statistics(
        self,
    ) -> None:
        fec = timing_interop.FecProfile("matrix")
        connected = {
            "event": "connected",
            "receiver_latency_microseconds": 300_000,
            "receiver_packet_filter_matched": True,
            "receiver_packet_filter_bytes": len(fec.packet_filter),
        }
        completion = {
            "event": "complete",
            "messages": 0,
            "receiver_filter_extra_packets": 15,
            "receiver_filter_supply_packets": 1,
            "receiver_filter_loss_packets": 0,
        }
        latency, events, complete = timing_interop.parse_receiver_output(
            "\n".join((json.dumps(connected), json.dumps(completion))),
            0,
            expected_packet_filter=fec.packet_filter,
        )
        self.assertEqual(latency, 300_000)
        self.assertEqual(events, [])
        self.assertEqual(complete, completion)

        connected["receiver_packet_filter_bytes"] -= 1
        with self.assertRaisesRegex(RuntimeError, "requested FEC filter"):
            timing_interop.parse_receiver_output(
                "\n".join((json.dumps(connected), json.dumps(completion))),
                0,
                expected_packet_filter=fec.packet_filter,
            )

        connected["receiver_packet_filter_bytes"] += 1
        completion["receiver_filter_supply_packets"] = -1
        with self.assertRaisesRegex(RuntimeError, "is negative"):
            timing_interop.parse_receiver_output(
                "\n".join((json.dumps(connected), json.dumps(completion))),
                0,
                expected_packet_filter=fec.packet_filter,
            )

        clear_connected = {
            "event": "connected",
            "receiver_latency_microseconds": 300_000,
        }
        with self.assertRaisesRegex(RuntimeError, "filter statistics"):
            timing_interop.parse_receiver_output(
                "\n".join(
                    (json.dumps(clear_connected), json.dumps(completion))
                ),
                0,
            )

    @staticmethod
    def _rotation_trace(
        *, missing_response: bool = False, fault_relay: bool = False
    ) -> str:
        digest = "a" * 64
        forward_direction = (
            "sender_to_receiver" if fault_relay else "caller_to_listener"
        )
        reverse_direction = (
            "receiver_to_sender" if fault_relay else "listener_to_caller"
        )
        events: list[dict[str, object]] = [
            {
                "event": "srt_first_data_trace",
                "direction": forward_direction,
                "relay_ordinal": 10,
                "sequence": 100,
                "destination_socket_id": 7,
                "key_selection": 1,
            },
            *(
                {
                    "event": "srt_data_key_transition_trace",
                    "direction": forward_direction,
                    "relay_ordinal": ordinal,
                    "sequence": 100 + index,
                    "destination_socket_id": 7,
                    "key_selection": selection,
                }
                for index, (ordinal, selection) in enumerate(
                    ((10, 1), (20, 2), (30, 1), (40, 2))
                )
            ),
        ]
        for index, (selection, request_ordinal) in enumerate(
            ((2, 15), (1, 25), (2, 35))
        ):
            identity = {
                "key_selection": selection,
                "content_sha256": f"{index:x}" + digest[1:],
            }
            events.append(
                {
                    "event": "srt_runtime_key_material_trace",
                    "name": "KMREQ",
                    "direction": forward_direction,
                    "relay_ordinal": request_ordinal,
                    "key_material": identity,
                }
            )
            if not (missing_response and index == 1):
                events.append(
                    {
                        "event": "srt_runtime_key_material_trace",
                        "name": "KMRSP",
                        "direction": reverse_direction,
                        "relay_ordinal": request_ordinal + 1,
                        "key_material": identity,
                    }
                )
        return "\n".join(json.dumps(event) for event in events)

    def test_rotation_trace_requires_causal_acknowledged_updates(self) -> None:
        security = timing_interop.SecurityProfile(
            key_length=32,
            key_refresh_rate=64,
            key_preannouncement=20,
            minimum_key_transitions=3,
        )

        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def render(_: str) -> str:
                return LiveTimingInteropTests._rotation_trace()

        observation = timing_interop.validate_security_trace(
            security, Relay()
        )
        self.assertEqual(observation["data_key_transitions"], 3)
        self.assertEqual(observation["acknowledged_key_updates"], 3)

        class FaultRelay(Relay):
            @staticmethod
            def render(_: str) -> str:
                return LiveTimingInteropTests._rotation_trace(
                    fault_relay=True
                )

        fault_observation = timing_interop.validate_security_trace(
            security, FaultRelay()
        )
        self.assertEqual(fault_observation, observation)

        class MissingResponseRelay(Relay):
            @staticmethod
            def render(_: str) -> str:
                return LiveTimingInteropTests._rotation_trace(
                    missing_response=True
                )

        with self.assertRaisesRegex(RuntimeError, "causal byte-identical"):
            timing_interop.validate_security_trace(
                security, MissingResponseRelay()
            )

        class InconsistentDestinationRelay(Relay):
            @staticmethod
            def render(_: str) -> str:
                events = [
                    json.loads(line)
                    for line in LiveTimingInteropTests._rotation_trace().splitlines()
                ]
                transition = next(
                    event
                    for event in events
                    if event.get("event")
                    == "srt_data_key_transition_trace"
                    and event.get("relay_ordinal") == 30
                )
                transition["destination_socket_id"] = 8
                return "\n".join(json.dumps(event) for event in events)

        with self.assertRaisesRegex(RuntimeError, "repeated key rotation"):
            timing_interop.validate_security_trace(
                security, InconsistentDestinationRelay()
            )

        class TruncatedRelay(Relay):
            @staticmethod
            def render(_: str) -> str:
                return "\n".join(
                    (
                        LiveTimingInteropTests._rotation_trace(),
                        json.dumps(
                            {
                                "event":
                                    "srt_data_key_transition_trace_truncated"
                            }
                        ),
                    )
                )

        with self.assertRaisesRegex(RuntimeError, "trace is incomplete"):
            timing_interop.validate_security_trace(
                security, TruncatedRelay()
            )

    def test_backup_group_rotation_requires_distinct_causal_member_paths(
        self,
    ) -> None:
        security = timing_interop.SecurityProfile(
            key_length=32,
            key_refresh_rate=64,
            key_preannouncement=20,
            minimum_key_transitions=3,
        )

        class PathRelay:
            def __init__(self, destination_socket_id: int) -> None:
                self.trace = LiveTimingInteropTests._rotation_trace().replace(
                    '"destination_socket_id": 7',
                    f'"destination_socket_id": {destination_socket_id}',
                )

            @staticmethod
            def error() -> None:
                return None

            def render(self, _: str) -> str:
                return self.trace

        class GroupRelay:
            def __init__(self, backup_socket_id: int = 8) -> None:
                self.primary = PathRelay(7)
                self.backup = PathRelay(backup_socket_id)

        observation = timing_interop.validate_group_security_trace(
            security, GroupRelay()
        )
        self.assertGreaterEqual(observation["data_key_transitions"], 3)
        self.assertEqual(set(observation["member_paths"]), {"primary", "backup"})
        self.assertEqual(
            observation["member_paths"]["primary"]["receiver_socket_id"],
            7,
        )
        self.assertEqual(
            observation["member_paths"]["backup"]["receiver_socket_id"],
            8,
        )

        with self.assertRaisesRegex(RuntimeError, "share one receiver"):
            timing_interop.validate_group_security_trace(
                security, GroupRelay(backup_socket_id=7)
            )

        missing_response = GroupRelay()
        missing_response.backup.trace = self._rotation_trace(
            missing_response=True
        ).replace(
            '"destination_socket_id": 7',
            '"destination_socket_id": 8',
        )
        with self.assertRaisesRegex(RuntimeError, "causal byte-identical"):
            timing_interop.validate_group_security_trace(
                security, missing_response
            )

        aggregate_shortfall = timing_interop.SecurityProfile(
            key_length=32,
            key_refresh_rate=64,
            key_preannouncement=20,
            minimum_key_transitions=7,
        )
        with self.assertRaisesRegex(RuntimeError, "aggregate key rotation"):
            timing_interop.validate_group_security_trace(
                aggregate_shortfall, GroupRelay()
            )

        with self.assertRaisesRegex(RuntimeError, "no member wire traces"):
            timing_interop.validate_group_security_trace(security, None)

        with self.assertRaisesRegex(RuntimeError, "unexpectedly used"):
            timing_interop.validate_group_security_trace(
                timing_interop.SecurityProfile(), GroupRelay()
            )

    def test_rendezvous_trace_requires_roles_and_both_conclusions(self) -> None:
        class Relay:
            role = {
                "sender_role": "initiator",
                "receiver_role": "responder",
            }
            conclusions = {
                "sender_to_receiver": 7,
                "receiver_to_sender": 8,
            }

            @staticmethod
            def error() -> None:
                return None

            def role_observation(self) -> dict[str, object]:
                return dict(self.role)

            def conclusion_socket_id(self, direction: str) -> int | None:
                return self.conclusions.get(direction)

            @staticmethod
            def render(_: str) -> str:
                return json.dumps(
                    {
                        "event": "srt_first_data_trace",
                        "direction": "sender_to_receiver",
                        "sequence": 100,
                        "destination_socket_id": 8,
                    }
                )

        relay = Relay()
        observation = timing_interop.validate_connection_trace(
            "rendezvous", relay
        )
        self.assertEqual(observation["sender_cookie_role"], "initiator")
        self.assertEqual(observation["receiver_conclusion_socket_id"], 8)

        # Socket IDs are endpoint-local. Separate peer processes may assign
        # the same positive numeric ID without creating an ambiguous wire
        # identity; DATA binding below remains directional.
        relay.conclusions["sender_to_receiver"] = 8
        equal_id_observation = timing_interop.validate_connection_trace(
            "rendezvous", relay
        )
        self.assertEqual(
            equal_id_observation["sender_conclusion_socket_id"], 8
        )
        self.assertEqual(
            equal_id_observation["receiver_conclusion_socket_id"], 8
        )

        relay.role["receiver_role"] = "initiator"
        with self.assertRaisesRegex(RuntimeError, "cookie contest"):
            timing_interop.validate_connection_trace("rendezvous", relay)
        relay.role["receiver_role"] = "responder"
        relay.conclusions.pop("receiver_to_sender")
        with self.assertRaisesRegex(RuntimeError, "positive CONCLUSION"):
            timing_interop.validate_connection_trace("rendezvous", relay)

        relay.conclusions["receiver_to_sender"] = 0
        with self.assertRaisesRegex(RuntimeError, "positive CONCLUSION"):
            timing_interop.validate_connection_trace("rendezvous", relay)

    def test_rendezvous_trace_binds_data_to_receiver_conclusion(self) -> None:
        class Relay:
            @staticmethod
            def error() -> None:
                return None

            @staticmethod
            def role_observation() -> dict[str, object]:
                return {
                    "sender_role": "responder",
                    "receiver_role": "initiator",
                }

            @staticmethod
            def conclusion_socket_id(direction: str) -> int:
                return {
                    "sender_to_receiver": 7,
                    "receiver_to_sender": 8,
                }[direction]

            destination_socket_id = 8
            extra_event: dict[str, object] | None = None

            @classmethod
            def render(cls, _: str) -> str:
                events = [
                    {
                        "event": "srt_first_data_trace",
                        "direction": "sender_to_receiver",
                        "sequence": 100,
                        "destination_socket_id": cls.destination_socket_id,
                    }
                ]
                if cls.extra_event is not None:
                    events.append(cls.extra_event)
                return "\n".join(json.dumps(event) for event in events)

        observation = timing_interop.validate_connection_trace(
            "rendezvous", Relay()
        )
        self.assertEqual(observation["first_data_sequence"], 100)

        Relay.destination_socket_id = 7
        with self.assertRaisesRegex(RuntimeError, "bound to its receiver"):
            timing_interop.validate_connection_trace("rendezvous", Relay())

        Relay.destination_socket_id = 8
        Relay.extra_event = {
            "event": "srt_control_trace_truncated",
            "omitted": 1,
        }
        with self.assertRaisesRegex(RuntimeError, "trace is incomplete"):
            timing_interop.validate_connection_trace("rendezvous", Relay())

    def test_fault_trace_records_original_selectors_and_key_ordinals(
        self,
    ) -> None:
        proxy = timing_interop.TimingFaultTraceProxy(9_001, ())
        try:
            proxy._record(
                self._data_packet(100, 1), "sender_to_receiver"
            )
            proxy._relay_ordinal = 1
            proxy._record(
                self._data_packet(101, 1), "sender_to_receiver"
            )
            proxy._relay_ordinal = 2
            proxy._record(
                self._data_packet(102, 2), "sender_to_receiver"
            )
            proxy._relay_ordinal = 3
            proxy._record(
                self._data_packet(103, 1, retransmitted=True),
                "sender_to_receiver",
            )
            proxy._relay_ordinal = 4
            proxy._record_runtime_key_material(
                self._key_material_packet(3, 2),
                "sender_to_receiver",
            )
            events = [
                json.loads(line)
                for line in proxy.render("encrypted-fault").splitlines()
            ]
        finally:
            proxy.close()

        transitions = [
            event
            for event in events
            if event.get("event") == "srt_data_key_transition_trace"
        ]
        self.assertEqual(
            [
                (
                    event["relay_ordinal"],
                    event["sequence"],
                    event["key_selection"],
                )
                for event in transitions
            ],
            [(1, 100, 1), (3, 102, 2)],
        )
        first_data = [
            event
            for event in events
            if event.get("event") == "srt_first_data_trace"
        ]
        self.assertEqual(len(first_data), 1)
        self.assertEqual(first_data[0]["relay_ordinal"], 1)
        key_events = [
            event
            for event in events
            if event.get("event") == "srt_runtime_key_material_trace"
        ]
        self.assertEqual(len(key_events), 1)
        self.assertEqual(key_events[0]["relay_ordinal"], 5)
        self.assertEqual(key_events[0]["name"], "KMREQ")
        self.assertNotIn("salt", key_events[0]["key_material"])

    def test_rendezvous_trace_records_causal_rotation_ordinals(self) -> None:
        relay_sockets = (mock.Mock(), mock.Mock())
        faults = timing_interop.fault_plan("fec-source-drop", 384)
        with mock.patch.object(
            timing_interop.socket,
            "socket",
            side_effect=relay_sockets,
        ):
            proxy = timing_interop.TimingRendezvousTraceProxy(
                9_001, 9_002, faults
            )
        try:
            self.assertEqual(proxy._faults, faults)
            proxy._record(
                self._data_packet(100, 1), "sender_to_receiver"
            )
            proxy._relay_ordinal = 1
            proxy._record_runtime_key_material(
                self._key_material_packet(3, 2),
                "sender_to_receiver",
            )
            proxy._relay_ordinal = 2
            proxy._record(
                self._data_packet(101, 2), "sender_to_receiver"
            )
            events = [
                json.loads(line)
                for line in proxy.render("rendezvous-timing").splitlines()
            ]
        finally:
            proxy.close()

        transitions = [
            event
            for event in events
            if event.get("event") == "srt_data_key_transition_trace"
        ]
        self.assertEqual(
            [event["relay_ordinal"] for event in transitions], [1, 3]
        )
        key_event = next(
            event
            for event in events
            if event.get("event") == "srt_runtime_key_material_trace"
        )
        self.assertEqual(key_event["relay_ordinal"], 2)

    def test_timing_trace_proxy_forwards_ipv6_datagrams(self) -> None:
        if not socket.has_ipv6:
            self.skipTest("IPv6 is unavailable")
        try:
            target = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            caller = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            target.bind(("::1", 0))
            caller.bind(("::1", 0))
            target.settimeout(2.0)
            caller.settimeout(2.0)
            proxy = timing_interop.TimingTraceProxy(
                target.getsockname()[1],
                host="0:0:0:0:0:0:0:1",
            )
        except (OSError, PermissionError) as error:
            for relay_socket in (
                locals().get("target"),
                locals().get("caller"),
            ):
                if relay_socket is not None:
                    relay_socket.close()
            self.skipTest(f"IPv6 loopback UDP is unavailable: {error}")
        try:
            proxy.start()
            caller.sendto(b"caller", ("::1", proxy.port))
            payload, relay_address = target.recvfrom(65_535)
            self.assertEqual(payload, b"caller")
            target.sendto(b"listener", relay_address)
            payload, _ = caller.recvfrom(65_535)
            self.assertEqual(payload, b"listener")
        finally:
            proxy.close()
            target.close()
            caller.close()
        self.assertIsNone(proxy.error())

    def test_timing_fault_proxy_forwards_ipv6_datagrams(self) -> None:
        if not socket.has_ipv6:
            self.skipTest("IPv6 is unavailable")
        try:
            target = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            caller = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            target.bind(("::1", 0))
            caller.bind(("::1", 0))
            target.settimeout(2.0)
            caller.settimeout(2.0)
            proxy = timing_interop.TimingFaultTraceProxy(
                target.getsockname()[1],
                (),
                host="0:0:0:0:0:0:0:1",
            )
        except (OSError, PermissionError) as error:
            for relay_socket in (
                locals().get("target"),
                locals().get("caller"),
            ):
                if relay_socket is not None:
                    relay_socket.close()
            self.skipTest(f"IPv6 loopback UDP is unavailable: {error}")
        try:
            self.assertEqual(
                proxy._target,
                ("::1", target.getsockname()[1], 0, 0),
            )
            proxy.start()
            caller.sendto(b"caller", ("::1", proxy.port))
            payload, relay_address = target.recvfrom(65_535)
            self.assertEqual(payload, b"caller")
            target.sendto(b"listener", relay_address)
            payload, _ = caller.recvfrom(65_535)
            self.assertEqual(payload, b"listener")
        finally:
            proxy.close()
            target.close()
            caller.close()
        self.assertIsNone(proxy.error())

    def test_timing_rendezvous_proxy_forwards_ipv6_datagrams(self) -> None:
        if not socket.has_ipv6:
            self.skipTest("IPv6 is unavailable")
        try:
            sender = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            receiver = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
            sender.bind(("::1", 0))
            receiver.bind(("::1", 0))
            sender.settimeout(2.0)
            receiver.settimeout(2.0)
            proxy = timing_interop.TimingRendezvousTraceProxy(
                sender.getsockname()[1],
                receiver.getsockname()[1],
                host="0:0:0:0:0:0:0:1",
            )
        except (OSError, PermissionError) as error:
            for relay_socket in (
                locals().get("sender"),
                locals().get("receiver"),
            ):
                if relay_socket is not None:
                    relay_socket.close()
            self.skipTest(f"IPv6 loopback UDP is unavailable: {error}")
        try:
            proxy.start()
            sender.sendto(b"sender", ("::1", proxy.sender_port))
            payload, relay_address = receiver.recvfrom(65_535)
            self.assertEqual(payload, b"sender")
            self.assertEqual(relay_address[1], proxy.receiver_port)
            receiver.sendto(b"receiver", relay_address)
            payload, relay_address = sender.recvfrom(65_535)
            self.assertEqual(payload, b"receiver")
            self.assertEqual(relay_address[1], proxy.sender_port)
        finally:
            proxy.close()
            sender.close()
            receiver.close()
        self.assertIsNone(proxy.error())

    def test_timing_trace_proxy_canonicalizes_ipv6_target(self) -> None:
        relay_socket = mock.Mock()
        with mock.patch.object(
            timing_interop.socket, "socket", return_value=relay_socket
        ) as socket_factory:
            proxy = timing_interop.TimingTraceProxy(
                9_001, host="0:0:0:0:0:0:0:1"
            )
        try:
            socket_factory.assert_called_once_with(
                socket.AF_INET6, socket.SOCK_DGRAM
            )
            relay_socket.bind.assert_called_once_with(("::1", 0))
            self.assertEqual(proxy._target, ("::1", 9_001, 0, 0))
        finally:
            proxy.close()

    def test_combines_real_release_and_udp_observations_for_ts(self) -> None:
        messages = timing_interop.generated_messages(
            "ts-1316", 3, 15_040_000
        )
        events = [
            {
                "message_index": index,
                "tsbpd_deadline_microseconds": 1_000_000 + index * 700,
                "srt_release_microseconds": 1_000_005 + index * 700,
                "udp_egress_microseconds": 1_000_015 + index * 700,
                "payload_bytes": len(message.payload),
            }
            for index, message in enumerate(messages)
        ]
        samples = timing_interop.combine_observations(
            messages,
            events,
            [message.payload for message in messages],
            880_000,
            mpeg_ts=True,
        )
        self.assertEqual(len(samples), 3)
        self.assertEqual(
            samples[0].source_submission_microseconds, 880_000
        )
        self.assertEqual(samples[0].pcr_ticks, 0)
        self.assertEqual(
            samples[0].source_payload_sha256,
            samples[0].egress_payload_sha256,
        )
        score = live_timing.score_timing(samples)
        self.assertEqual(score["payload_integrity_failures"], 0)
        self.assertEqual(score["early_srt_release_count"], 0)
        self.assertEqual(score["pcr_observation_count"], 3)

    def test_binary_profile_stays_payload_agnostic(self) -> None:
        messages = timing_interop.generated_messages(
            "binary-1200", 1, 9_600_000
        )
        event = {
            "message_index": 0,
            "tsbpd_deadline_microseconds": 500_000,
            "srt_release_microseconds": 500_001,
            "udp_egress_microseconds": 500_002,
            "payload_bytes": 1_200,
        }
        sample = timing_interop.combine_observations(
            messages,
            [event],
            [messages[0].payload],
            460_000,
            mpeg_ts=False,
        )[0]
        self.assertIsNone(sample.pcr_ticks)
        self.assertEqual(sample.payload_bytes, 1_200)

    def test_timing_limits_cover_deadline_phase_burst_and_pcr_span(self) -> None:
        messages = timing_interop.generated_messages(
            "ts-1316", 16, 15_040_000
        )
        samples = live_timing.synthetic_timing_samples(messages, 120_000)
        scorecard = live_timing.score_timing(samples)
        timing_interop.enforce_limits(
            scorecard,
            maximum_egress_p99_9_microseconds=0,
            maximum_tsbpd_phase_range_microseconds=0,
            maximum_burst_depth=2,
            maximum_pcr_span_rate_error_ppm=0.01,
        )
        with self.assertRaisesRegex(RuntimeError, "p99.9"):
            timing_interop.enforce_limits(
                scorecard,
                maximum_egress_p99_9_microseconds=-1,
                maximum_tsbpd_phase_range_microseconds=None,
                maximum_burst_depth=None,
                maximum_pcr_span_rate_error_ppm=None,
            )
        with self.assertRaisesRegex(RuntimeError, "TSBPD phase"):
            timing_interop.enforce_limits(
                scorecard,
                maximum_egress_p99_9_microseconds=None,
                maximum_tsbpd_phase_range_microseconds=-1,
                maximum_burst_depth=None,
                maximum_pcr_span_rate_error_ppm=None,
            )
        with self.assertRaisesRegex(RuntimeError, "burst depth"):
            timing_interop.enforce_limits(
                scorecard,
                maximum_egress_p99_9_microseconds=None,
                maximum_tsbpd_phase_range_microseconds=None,
                maximum_burst_depth=1,
                maximum_pcr_span_rate_error_ppm=None,
            )
        with self.assertRaisesRegex(RuntimeError, "PCR span"):
            timing_interop.enforce_limits(
                scorecard,
                maximum_egress_p99_9_microseconds=None,
                maximum_tsbpd_phase_range_microseconds=None,
                maximum_burst_depth=None,
                maximum_pcr_span_rate_error_ppm=-1,
            )

    def test_backup_replay_phase_step_is_not_reported_as_clock_drift(
        self,
    ) -> None:
        messages = timing_interop.generated_messages(
            "ts-1316", 384, 7_520_000
        )
        samples = live_timing.synthetic_timing_samples(messages, 500_000)
        phase_step_microseconds = 915
        failover_after_messages = 96
        shifted_samples = [
            replace(
                sample,
                tsbpd_deadline_microseconds=(
                    sample.tsbpd_deadline_microseconds
                    + (
                        phase_step_microseconds
                        if sample.message_index >= failover_after_messages
                        else 0
                    )
                ),
                srt_release_microseconds=(
                    sample.srt_release_microseconds
                    + (
                        phase_step_microseconds
                        if sample.message_index >= failover_after_messages
                        else 0
                    )
                ),
                udp_egress_microseconds=(
                    sample.udp_egress_microseconds
                    + (
                        phase_step_microseconds
                        if sample.message_index >= failover_after_messages
                        else 0
                    )
                ),
            )
            for sample in samples
        ]
        scorecard = live_timing.score_timing(shifted_samples)

        mapped_latency = scorecard["mapped_tsbpd_latency_microseconds"]
        self.assertEqual(
            mapped_latency["maximum"] - mapped_latency["minimum"],
            phase_step_microseconds,
        )
        self.assertLess(
            scorecard["pcr_absolute_span_rate_error_ppm"]["maximum"],
            5_000,
        )
        timing_interop.enforce_limits(
            scorecard,
            maximum_egress_p99_9_microseconds=5_000,
            maximum_tsbpd_phase_range_microseconds=2_000,
            maximum_burst_depth=2,
            maximum_pcr_span_rate_error_ppm=5_000,
        )

    def test_scorecard_is_written_before_quality_limits_are_enforced(self) -> None:
        scorecard: dict[str, object] = {
            "sample_count": 4_096,
            "maximum_burst_depth": 9,
        }
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "scorecard.json"
            timing_interop.write_scorecard(scorecard, output)
            self.assertEqual(json.loads(output.read_text()), scorecard)
        with self.assertRaisesRegex(RuntimeError, "burst depth"):
            timing_interop.enforce_limits(
                scorecard,
                maximum_egress_p99_9_microseconds=None,
                maximum_tsbpd_phase_range_microseconds=None,
                maximum_burst_depth=8,
                maximum_pcr_span_rate_error_ppm=None,
            )

    def test_combiner_rejects_loss_reordering_and_size_mismatch(self) -> None:
        messages = timing_interop.generated_messages(
            "binary-1200", 1, 9_600_000
        )
        event = {
            "message_index": 1,
            "tsbpd_deadline_microseconds": 500_000,
            "srt_release_microseconds": 500_001,
            "udp_egress_microseconds": 500_002,
            "payload_bytes": 1_200,
        }
        with self.assertRaisesRegex(RuntimeError, "not ordered"):
            timing_interop.combine_observations(
                messages,
                [event],
                [messages[0].payload],
                460_000,
                mpeg_ts=False,
            )
        event["message_index"] = 0
        event["payload_bytes"] = 1_199
        with self.assertRaisesRegex(RuntimeError, "has 1200 bytes"):
            timing_interop.combine_observations(
                messages,
                [event],
                [messages[0].payload],
                460_000,
                mpeg_ts=False,
            )
        event["payload_bytes"] = 1_200
        corrupted = bytearray(messages[0].payload)
        corrupted[0] ^= 0xFF
        with self.assertRaisesRegex(RuntimeError, "differs from"):
            timing_interop.combine_observations(
                messages,
                [event],
                [bytes(corrupted)],
                460_000,
                mpeg_ts=False,
            )
        with self.assertRaisesRegex(ValueError, "counts differ"):
            timing_interop.combine_observations(
                messages, [], [], 460_000, mpeg_ts=False
            )

    def test_udp_capture_preserves_datagram_boundaries(self) -> None:
        try:
            capture_context = timing_interop.UdpCapture("127.0.0.1", 2)
        except PermissionError as error:
            self.skipTest(f"loopback UDP is unavailable: {error}")
        with capture_context as capture:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                sender.sendto(b"first", ("127.0.0.1", capture.port))
                sender.sendto(b"second", ("127.0.0.1", capture.port))
            self.assertEqual(capture.wait(2.0), [b"first", b"second"])


if __name__ == "__main__":
    unittest.main()
