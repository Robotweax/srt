from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import run_tlpktdrop_interop as tlpktdrop  # noqa: E402
import srt_handshake_trace  # noqa: E402


class TlpktdropInteropTests(unittest.TestCase):
    def test_matrix_covers_maximum_payload_rollover_and_disabled_mode(
        self,
    ) -> None:
        robotweax = Path("/robotweax")
        reference = Path("/haivision")
        scenarios = tlpktdrop.scenario_matrix(robotweax, reference)

        self.assertEqual(len(scenarios), 10)
        self.assertEqual(sum(item.rollover for item in scenarios), 2)
        self.assertEqual(sum(not item.tlpktdrop for item in scenarios), 2)
        self.assertEqual(
            {item.profile for item in scenarios},
            {"binary-1200", "binary-max-1456", "ts-1316"},
        )
        self.assertEqual(
            {item.sender for item in scenarios},
            {robotweax, reference},
        )

    def test_profiles_remain_payload_agnostic_and_ts_aligned(self) -> None:
        binary = tlpktdrop.generated_messages("binary-max-1456")
        transport = tlpktdrop.generated_messages("ts-1316")

        self.assertEqual([len(item.payload) for item in binary], [1_456] * 3)
        self.assertEqual([len(item.payload) for item in transport], [1_316] * 3)
        self.assertTrue(
            all(
                size <= tlpktdrop.SRT_LIVE_MAX_PAYLOAD_SIZE
                for size in tlpktdrop.PROFILE_MESSAGE_SIZE.values()
            )
        )
        self.assertTrue(all(len(item.payload) % 188 == 0 for item in transport))
        with self.assertRaisesRegex(ValueError, "unknown"):
            tlpktdrop.generated_messages("mpeg-only")

    def test_peer_commands_enable_explicit_source_time_and_tlpktdrop(self) -> None:
        caller = tlpktdrop.peer_command(
            Path("peer"),
            "caller",
            9_001,
            Path("input"),
            3_600,
            1_200,
            tlpktdrop=True,
        )
        listener = tlpktdrop.peer_command(
            Path("peer"),
            "listener",
            9_001,
            Path("output"),
            2_400,
            1_200,
            tlpktdrop=False,
        )

        self.assertIn("--explicit-source-time", caller)
        self.assertIn("--source-pacing", caller)
        self.assertEqual(caller[caller.index("--tlpktdrop") + 1], "on")
        self.assertEqual(listener[listener.index("--tlpktdrop") + 1], "off")
        self.assertEqual(caller[caller.index("--payload-size") + 1], "1200")
        self.assertEqual(listener[listener.index("--payload-size") + 1], "1200")
        self.assertNotIn("--explicit-source-time", listener)

    def test_fault_requires_persistent_data_drop(self) -> None:
        with self.assertRaisesRegex(ValueError, "DATA drop"):
            srt_handshake_trace.RendezvousFault(
                action="delay",
                direction="sender_to_receiver",
                occurrence=1,
                delay_milliseconds=1,
                suppress_retransmissions=True,
            )

    def test_ack_metadata_contains_cumulative_sequence(self) -> None:
        packet = struct.pack(
            ">IIIIII",
            0x8002_0000,
            17,
            123,
            456,
            0x7FFF_FFFF,
            0,
        )
        metadata = (
            srt_handshake_trace.RendezvousTraceProxy._packet_metadata(packet)
        )

        self.assertEqual(metadata["control_type"], 2)
        self.assertEqual(metadata["acknowledgement_number"], 17)
        self.assertEqual(metadata["next_sequence"], 0x7FFF_FFFF)
        self.assertEqual(metadata["acknowledgement_payload_bytes"], 8)

    def test_wrap_safe_ack_crossing(self) -> None:
        covers = (
            srt_handshake_trace.RendezvousTraceProxy
            ._sequence_is_acknowledged
        )
        self.assertTrue(covers(0, srt_handshake_trace.SEQUENCE_MASK))
        self.assertTrue(covers(42, 41))
        self.assertFalse(covers(41, 41))
        self.assertFalse(covers(40, 41))


if __name__ == "__main__":
    unittest.main()
