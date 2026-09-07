from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_peer_error_interop  # noqa: E402
import srt_handshake_trace  # noqa: E402


class FakeSocket:
    def __init__(self) -> None:
        self.sent: list[tuple[bytes, tuple[str, int]]] = []

    def sendto(self, payload: bytes, target: tuple[str, int]) -> int:
        self.sent.append((payload, target))
        return len(payload)

    def close(self) -> None:
        pass


def control_packet(
    destination: int, next_sequence: int = 18
) -> bytes:
    return b"".join(
        value.to_bytes(4, "big")
        for value in (
            0x8002_0000,
            1,
            2,
            destination,
            next_sequence,
        )
    )


def data_packet(sequence: int, timestamp: int, destination: int) -> bytes:
    return b"".join(
        value.to_bytes(4, "big")
        for value in (sequence, 0x2000_0001, timestamp, destination)
    ) + b"payload"


class PeerErrorInteropTests(unittest.TestCase):
    def setUp(self) -> None:
        self.robotweax = Path("/tmp/robotweax-peer")
        self.reference = Path("/tmp/reference-peer")

    def test_deployed_peer_error_datagram_has_exact_layout(self) -> None:
        datagram = srt_handshake_trace.build_peer_error_datagram(
            0x1234_5678, 0x90AB_CDEF
        )
        self.assertEqual(len(datagram), 20)
        self.assertEqual(
            datagram.hex(),
            "8008000000000fa090abcdef1234567800000000",
        )
        with self.assertRaisesRegex(ValueError, "unsigned 32-bit"):
            srt_handshake_trace.build_peer_error_datagram(-1, 0)

    def test_proxy_injects_once_before_first_runtime_ack(self) -> None:
        proxy = srt_handshake_trace.CallerListenerPeerErrorProxy(
            10_001, ack_hold_milliseconds=25
        )
        fake_socket = FakeSocket()
        proxy._socket = fake_socket  # type: ignore[assignment]
        proxy._source = ("127.0.0.1", 10_002)

        stale_acknowledgement = control_packet(0x1234_5678, 17)
        acknowledgement = control_packet(0x1234_5678, 18)
        first_data = data_packet(17, 99, 0x8765_4321)
        second_data = data_packet(18, 100, 0x8765_4321)
        with mock.patch.object(
            srt_handshake_trace.time, "sleep"
        ) as sleep:
            proxy._forward(first_data, "sender_to_receiver")
            proxy._forward(
                stale_acknowledgement, "receiver_to_sender"
            )
            proxy._forward(acknowledgement, "receiver_to_sender")
            proxy._forward(second_data, "sender_to_receiver")

        sleep.assert_called_once_with(0.025)

        self.assertEqual(len(fake_socket.sent), 5)
        injected, injected_target = fake_socket.sent[2]
        self.assertEqual(injected_target, proxy._source)
        self.assertEqual(
            injected,
            srt_handshake_trace.build_peer_error_datagram(
                0x1234_5678, 2
            ),
        )
        self.assertEqual(fake_socket.sent[0][0], first_data)
        self.assertEqual(
            fake_socket.sent[1][0], stale_acknowledgement
        )
        self.assertEqual(fake_socket.sent[3][0], acknowledgement)
        self.assertEqual(fake_socket.sent[4][0], second_data)
        self.assertEqual(
            proxy.injection_observation(),
            {
                "control_type": 8,
                "error_code": 4_000,
                "destination_socket_id": 0x1234_5678,
                "timestamp": 2,
                "datagram_bytes": 20,
                "trigger_acknowledgement_number": 1,
                "first_data_sequence": 17,
                "trigger_ack_next_sequence": 18,
                "trigger_ack_payload_bytes": 4,
                "ack_hold_milliseconds": 25,
                "relay_monotonic_ns": proxy.injection_observation()[
                    "relay_monotonic_ns"
                ],
                "ack_release_monotonic_ns": proxy.injection_observation()[
                    "ack_release_monotonic_ns"
                ],
            },
        )

    def test_proxy_rejects_an_invalid_ack_hold(self) -> None:
        with self.assertRaisesRegex(ValueError, "ACK hold"):
            srt_handshake_trace.CallerListenerPeerErrorProxy(
                10_001, ack_hold_milliseconds=-1
            )

    def test_proxy_waits_for_data_before_a_runtime_ack(self) -> None:
        proxy = srt_handshake_trace.CallerListenerPeerErrorProxy(10_001)
        proxy._socket = FakeSocket()  # type: ignore[assignment]
        proxy._source = ("127.0.0.1", 10_002)
        acknowledgement = control_packet(123)
        proxy._forward(acknowledgement, "receiver_to_sender")
        self.assertIsNone(proxy.injection_observation())
        proxy._forward(
            data_packet(17, 99, 0x8765_4321),
            "sender_to_receiver",
        )
        self.assertIsNone(proxy.injection_observation())
        proxy._forward(acknowledgement, "receiver_to_sender")
        self.assertIsNotNone(proxy.injection_observation())

    def test_proxy_matches_a_causal_ack_across_sequence_rollover(
        self,
    ) -> None:
        proxy = srt_handshake_trace.CallerListenerPeerErrorProxy(
            10_001, ack_hold_milliseconds=0
        )
        proxy._socket = FakeSocket()  # type: ignore[assignment]
        proxy._source = ("127.0.0.1", 10_002)
        proxy._forward(
            data_packet(0x7FFF_FFFF, 99, 0x8765_4321),
            "sender_to_receiver",
        )
        proxy._forward(
            control_packet(123, 0), "receiver_to_sender"
        )
        self.assertEqual(
            proxy.injection_observation()["trigger_ack_next_sequence"],
            0,
        )

    def test_matrix_covers_message_stream_and_both_directions(self) -> None:
        scenarios = run_peer_error_interop.scenario_matrix(
            self.robotweax, self.reference
        )
        self.assertEqual(len(scenarios), 4)
        self.assertEqual(
            {scenario.transport for scenario in scenarios},
            {"live", "file"},
        )
        self.assertEqual(
            [
                scenario.name
                for scenario in scenarios
                if scenario.expect_peer_error_observation is None
            ],
            ["peer-error-stream-haivision-to-robotweax"],
        )
        for transport in ("live", "file"):
            selected = [
                scenario
                for scenario in scenarios
                if scenario.transport == transport
            ]
            self.assertEqual(
                {(item.sender, item.receiver) for item in selected},
                {
                    (self.robotweax, self.reference),
                    (self.reference, self.robotweax),
                },
            )

    def test_sender_command_bounds_buffer_and_requires_peer_error(self) -> None:
        scenario = run_peer_error_interop.scenario_matrix(
            self.robotweax, self.reference
        )[0]
        command = run_peer_error_interop.peer_command(
            scenario,
            "caller",
            12_345,
            Path("/tmp/input.bin"),
            1_000_000,
            30,
        )
        self.assertIn("--expect-injected-peer-error", command)
        self.assertEqual(
            command[command.index("--send-buffer") + 1], "1472"
        )
        self.assertEqual(
            command[command.index("--max-bw") + 1], "2000000"
        )
        self.assertNotIn(
            "--require-missing-injected-peer-error", command
        )

        reference_stream_command = run_peer_error_interop.peer_command(
            run_peer_error_interop.scenario_matrix(
                self.robotweax, self.reference
            )[3],
            "caller",
            12_345,
            Path("/tmp/input.bin"),
            1_000_000,
            30,
        )
        self.assertIn(
            "--allow-missing-injected-peer-error",
            reference_stream_command,
        )
        self.assertNotIn(
            "--require-missing-injected-peer-error",
            reference_stream_command,
        )

    def test_relay_trace_includes_the_injected_control_evidence(self) -> None:
        proxy = srt_handshake_trace.CallerListenerPeerErrorProxy(10_001)
        fake_socket = FakeSocket()
        proxy._socket = fake_socket  # type: ignore[assignment]
        proxy._source = ("127.0.0.1", 10_002)
        empty_trace = proxy.render("peer-error-empty-test")
        self.assertIn('"first_data_sequence": null', empty_trace)
        self.assertIn('"last_ack_next_sequence": null', empty_trace)
        proxy._forward(
            data_packet(17, 99, 0x8765_4321),
            "sender_to_receiver",
        )
        proxy._forward(control_packet(123), "receiver_to_sender")
        trace = proxy.render("peer-error-test")
        self.assertIn('"event": "srt_peer_error_injection"', trace)
        self.assertIn('"destination_socket_id": 123', trace)
        self.assertIn('"error_code": 4000', trace)

    def test_relay_trace_exposes_pending_causal_evidence(self) -> None:
        proxy = srt_handshake_trace.CallerListenerPeerErrorProxy(10_001)
        fake_socket = FakeSocket()
        proxy._socket = fake_socket  # type: ignore[assignment]
        proxy._source = ("127.0.0.1", 10_002)
        proxy._forward(
            data_packet(17, 99, 0x8765_4321),
            "sender_to_receiver",
        )
        proxy._forward(
            control_packet(123, 17), "receiver_to_sender"
        )

        trace = proxy.render("peer-error-pending-test")

        self.assertIn('"event": "srt_peer_error_causal_state"', trace)
        self.assertIn('"first_data_sequence": 17', trace)
        self.assertIn('"last_ack_next_sequence": 17', trace)
        self.assertNotIn('"event": "srt_peer_error_injection"', trace)

    def test_evidence_requires_one_recovered_error_and_completion(
        self,
    ) -> None:
        scenario = run_peer_error_interop.scenario_matrix(
            self.robotweax, self.reference
        )[0]
        sender = "\n".join(
            (
                '{"event":"peer_error","matched":true,"count":1,'
                '"recovered":true,"expected_missing":false,'
                '"missing_allowed":false,'
                '"offset":1200}',
                '{"event":"complete","role":"caller",'
                '"bytes":10000}',
            )
        )
        receiver = (
            '{"event":"complete","role":"listener","bytes":10000}'
        )
        injection = {
            "control_type": 8,
            "error_code": 4_000,
            "destination_socket_id": 10,
            "datagram_bytes": 20,
            "trigger_acknowledgement_number": 20,
            "first_data_sequence": 100,
            "trigger_ack_next_sequence": 101,
            "trigger_ack_payload_bytes": 4,
            "ack_hold_milliseconds": 25,
            "relay_monotonic_ns": 1_000_000_000,
            "ack_release_monotonic_ns": 1_025_000_000,
        }
        self.assertEqual(
            run_peer_error_interop.validate_evidence(
                scenario, sender, receiver, injection, 10_000
            ),
            1_200,
        )
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            run_peer_error_interop.validate_evidence(
                scenario,
                sender.replace('"count":1', '"count":2'),
                receiver,
                injection,
                10_000,
            )
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            run_peer_error_interop.validate_evidence(
                scenario,
                sender,
                receiver,
                {
                    **injection,
                    "ack_release_monotonic_ns": 1_024_999_999,
                },
                10_000,
            )
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            run_peer_error_interop.validate_evidence(
                scenario,
                sender,
                receiver,
                {
                    **injection,
                    "trigger_ack_next_sequence": 100,
                },
                10_000,
            )

    def test_reference_stream_admits_only_the_two_exact_observations(
        self,
    ) -> None:
        scenario = run_peer_error_interop.scenario_matrix(
            self.robotweax, self.reference
        )[3]
        sender = "\n".join(
            (
                '{"event":"peer_error","matched":true,"count":0,'
                '"recovered":false,"expected_missing":false,'
                '"missing_allowed":true,'
                '"offset":0}',
                '{"event":"complete","role":"caller",'
                '"bytes":10000}',
            )
        )
        receiver = (
            '{"event":"complete","role":"listener","bytes":10000}'
        )
        injection = {
            "control_type": 8,
            "error_code": 4_000,
            "destination_socket_id": 10,
            "datagram_bytes": 20,
            "trigger_acknowledgement_number": 20,
            "first_data_sequence": 100,
            "trigger_ack_next_sequence": 101,
            "trigger_ack_payload_bytes": 4,
            "ack_hold_milliseconds": 25,
            "relay_monotonic_ns": 1_000_000_000,
            "ack_release_monotonic_ns": 1_025_000_000,
        }
        self.assertEqual(
            run_peer_error_interop.validate_evidence(
                scenario, sender, receiver, injection, 10_000
            ),
            0,
        )
        observed = sender.replace(
            '"count":0,"recovered":false',
            '"count":1,"recovered":true',
        ).replace('"offset":0', '"offset":1200')
        self.assertEqual(
            run_peer_error_interop.validate_evidence(
                scenario, observed, receiver, injection, 10_000
            ),
            1_200,
        )
        with self.assertRaisesRegex(RuntimeError, "inconsistent"):
            run_peer_error_interop.validate_evidence(
                scenario,
                sender.replace('"count":0', '"count":2'),
                receiver,
                injection,
                10_000,
            )


if __name__ == "__main__":
    unittest.main()
