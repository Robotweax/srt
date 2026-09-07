from __future__ import annotations

import json
import sys
import tempfile
import unittest
from dataclasses import asdict, replace
from pathlib import Path


INTEROP_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(INTEROP_DIRECTORY))

import live_timing  # noqa: E402


class LiveTimingTests(unittest.TestCase):
    def test_pcr_codec_covers_extension_and_wrap_boundary(self) -> None:
        for ticks in (
            0,
            299,
            300,
            live_timing.PCR_TICK_MODULUS - 1,
        ):
            encoded = live_timing.encode_pcr(ticks)
            self.assertEqual(len(encoded), 6)
            self.assertEqual(live_timing.decode_pcr(encoded), ticks)

        malformed = bytearray(live_timing.encode_pcr(1))
        malformed[4] &= 0x01
        with self.assertRaisesRegex(ValueError, "invalid MPEG-TS PCR"):
            live_timing.decode_pcr(bytes(malformed))

    def test_generator_emits_valid_packets_continuity_and_pcr_wrap(
        self,
    ) -> None:
        start = live_timing.PCR_TICK_MODULUS - 2_700
        messages = live_timing.generate_mpeg_ts_messages(
            live_timing.MpegTsProfile(
                packets_per_message=7,
                message_count=3,
                bitrate_bits_per_second=15_040_000,
                pcr_interval_packets=7,
                start_pcr_ticks=start,
                discontinuity_packets=frozenset({7}),
            )
        )
        self.assertTrue(all(len(message.payload) == 1_316 for message in messages))
        packets = [
            message.payload[offset : offset + live_timing.MPEG_TS_PACKET_BYTES]
            for message in messages
            for offset in range(0, len(message.payload), 188)
        ]
        parsed = [
            live_timing.parse_transport_packet(packet)
            for packet in packets
        ]
        self.assertEqual(
            [packet.continuity_counter for packet in parsed],
            list(range(16)) + list(range(5)),
        )
        self.assertTrue(all(packet.pid == 0x100 for packet in parsed))
        pcr_packets = [packet for packet in parsed if packet.pcr_ticks is not None]
        self.assertEqual(len(pcr_packets), 3)
        self.assertEqual(pcr_packets[0].pcr_ticks, start)
        self.assertEqual(pcr_packets[1].pcr_ticks, 16_200)
        self.assertTrue(pcr_packets[1].discontinuity)
        self.assertEqual(pcr_packets[2].pcr_ticks, 35_100)

    def test_message_profiles_are_not_limited_to_1316_bytes(self) -> None:
        for packets_per_message, expected_bytes in (
            (1, 188),
            (2, 376),
            (7, 1_316),
        ):
            messages = live_timing.generate_mpeg_ts_messages(
                live_timing.MpegTsProfile(
                    packets_per_message=packets_per_message,
                    message_count=2,
                    bitrate_bits_per_second=15_040_000,
                    pcr_interval_packets=packets_per_message,
                )
            )
            self.assertEqual(
                [len(message.payload) for message in messages],
                [expected_bytes, expected_bytes],
            )
        binary = live_timing.generate_binary_messages(
            777, 3, 6_216_000, seed=42
        )
        self.assertEqual([len(message.payload) for message in binary], [777] * 3)
        for message in binary:
            with self.assertRaisesRegex(ValueError, "exactly 188 bytes"):
                live_timing.parse_transport_packet(message.payload)

    def test_generator_tracks_payload_and_pcr_pid_continuity_separately(
        self,
    ) -> None:
        message = live_timing.generate_mpeg_ts_messages(
            live_timing.MpegTsProfile(
                packets_per_message=6,
                message_count=1,
                bitrate_bits_per_second=15_040_000,
                pid=0x100,
                pcr_pid=0x101,
                pcr_interval_packets=2,
            )
        )[0]
        packets = [
            live_timing.parse_transport_packet(
                message.payload[offset : offset + 188]
            )
            for offset in range(0, len(message.payload), 188)
        ]
        self.assertEqual(
            [(packet.pid, packet.continuity_counter) for packet in packets],
            [
                (0x101, 0),
                (0x100, 0),
                (0x101, 1),
                (0x100, 1),
                (0x101, 2),
                (0x100, 2),
            ],
        )
        self.assertEqual(
            [packet.pcr_ticks is not None for packet in packets],
            [True, False, True, False, True, False],
        )

    def test_scorecard_reports_deadlines_integrity_jitter_and_bursts(
        self,
    ) -> None:
        digest = "a" * 64
        samples = [
            live_timing.TimingSample(
                message_index=index,
                source_submission_microseconds=index * 50,
                tsbpd_deadline_microseconds=120_000 + index * 50,
                srt_release_microseconds=(
                    120_000 + index * 50 + offset
                ),
                udp_egress_microseconds=egress,
                payload_bytes=1_316,
                source_payload_sha256=digest,
                egress_payload_sha256=(
                    "b" * 64 if index == 2 else digest
                ),
            )
            for index, (offset, egress) in enumerate(
                ((-10, 120_040), (0, 120_140), (20, 120_190))
            )
        ]
        score = live_timing.score_timing(
            samples, burst_window_microseconds=200
        )
        self.assertEqual(score["sample_count"], 3)
        self.assertEqual(score["payload_sizes"], [1_316])
        self.assertEqual(score["early_srt_release_count"], 1)
        self.assertEqual(score["early_udp_egress_count"], 0)
        self.assertEqual(score["payload_integrity_compared"], 3)
        self.assertEqual(score["payload_integrity_uncompared"], 0)
        self.assertEqual(score["payload_integrity_failures"], 1)
        self.assertEqual(score["maximum_burst_depth"], 3)
        release = score["release_deadline_error_microseconds"]
        self.assertEqual(release["minimum"], -10.0)
        self.assertEqual(release["p50"], 0.0)
        self.assertEqual(release["p99"], 20.0)
        interval = score["egress_interval_error_microseconds"]
        self.assertEqual(interval["minimum"], 0.0)
        self.assertEqual(interval["maximum"], 50.0)
        absolute_interval = score[
            "absolute_egress_interval_error_microseconds"
        ]
        self.assertEqual(absolute_interval["maximum"], 50.0)
        self.assertEqual(
            score["measurement_span_microseconds"],
            {"source": 100, "udp_egress": 150, "error": 50},
        )
        self.assertEqual(score["tsbpd_deadline_regression_count"], 0)
        self.assertEqual(score["srt_release_regression_count"], 0)
        mapped_latency = score["mapped_tsbpd_latency_microseconds"]
        self.assertEqual(mapped_latency["minimum"], 120_000.0)
        self.assertEqual(mapped_latency["maximum"], 120_000.0)

    def test_scorecard_reports_mapped_deadline_regressions_separately(
        self,
    ) -> None:
        samples = [
            live_timing.TimingSample(
                message_index=0,
                source_submission_microseconds=100,
                tsbpd_deadline_microseconds=1_000,
                srt_release_microseconds=1_005,
                udp_egress_microseconds=1_010,
                payload_bytes=64,
            ),
            live_timing.TimingSample(
                message_index=1,
                source_submission_microseconds=200,
                tsbpd_deadline_microseconds=950,
                srt_release_microseconds=1_006,
                udp_egress_microseconds=1_011,
                payload_bytes=64,
            ),
        ]
        score = live_timing.score_timing(samples)
        self.assertEqual(score["tsbpd_deadline_regression_count"], 1)
        self.assertEqual(
            score["tsbpd_deadline_backward_step_microseconds"]["maximum"],
            50.0,
        )

    def test_scorecard_names_the_non_monotonic_time_axis(self) -> None:
        first = live_timing.TimingSample(
            message_index=0,
            source_submission_microseconds=200,
            tsbpd_deadline_microseconds=1_000,
            srt_release_microseconds=1_005,
            udp_egress_microseconds=1_010,
            payload_bytes=64,
        )
        second = live_timing.TimingSample(
            message_index=1,
            source_submission_microseconds=100,
            tsbpd_deadline_microseconds=1_100,
            srt_release_microseconds=1_105,
            udp_egress_microseconds=1_110,
            payload_bytes=64,
        )
        with self.assertRaisesRegex(
            ValueError, "explicit source times.*regressed by 100 us"
        ):
            live_timing.score_timing([first, second])

    def test_pcr_rate_score_is_wrap_and_discontinuity_safe(self) -> None:
        messages = live_timing.generate_mpeg_ts_messages(
            live_timing.MpegTsProfile(
                packets_per_message=1,
                message_count=5,
                bitrate_bits_per_second=1_504_000,
                pcr_interval_packets=1,
                start_pcr_ticks=(
                    live_timing.PCR_TICK_MODULUS - 27_000
                ),
                discontinuity_packets=frozenset({2}),
            )
        )
        samples = live_timing.synthetic_timing_samples(
            messages,
            120_000,
            egress_delay_microseconds=50,
        )
        score = live_timing.score_timing(samples)
        self.assertEqual(score["pcr_observation_count"], 5)
        self.assertEqual(score["pcr_rate_interval_count"], 3)
        absolute = score["pcr_absolute_rate_error_ppm"]
        self.assertAlmostEqual(absolute["maximum"], 0.0)
        self.assertEqual(score["pcr_span_count"], 2)
        span_absolute = score["pcr_absolute_span_rate_error_ppm"]
        self.assertAlmostEqual(span_absolute["maximum"], 0.0)

    def test_jsonl_schema_round_trips_and_rejects_invalid_events(self) -> None:
        message = live_timing.generate_binary_messages(
            64, 1, 512_000
        )[0]
        sample = live_timing.synthetic_timing_samples(
            [message], 120_000
        )[0]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "events.jsonl"
            path.write_text(
                json.dumps(asdict(sample)) + "\n",
                encoding="utf-8",
            )
            self.assertEqual(live_timing.load_timing_samples(path), [sample])
            path.write_text('{"message_index": 0}\n', encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "line 1"):
                live_timing.load_timing_samples(path)

    def test_sample_rejects_udp_egress_before_release(self) -> None:
        sample = live_timing.synthetic_timing_samples(
            live_timing.generate_binary_messages(64, 1, 512_000),
            120_000,
        )[0]
        with self.assertRaisesRegex(ValueError, "UDP egress precedes"):
            replace(
                sample,
                udp_egress_microseconds=(
                    sample.srt_release_microseconds - 1
                ),
            )
        with self.assertRaisesRegex(ValueError, "requires a PCR"):
            replace(sample, pcr_discontinuity=True)


if __name__ == "__main__":
    unittest.main()
