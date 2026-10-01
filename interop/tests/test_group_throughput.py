from __future__ import annotations

from pathlib import Path
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "benchmarks"))
import group_throughput as g


class GroupThroughputTests(unittest.TestCase):
    def result(self, copies=(10, 10)):
        return {"event": "result", "messages": 10, "members": 2,
                "useful_bytes": 13160, "sha256": "a" * 64,
                "seconds": 0.2, "delivery_span_seconds": 0.09,
                "cpu_seconds": 0.01, "latency_us_p50": 120000,
                "latency_us_p99": 121000,
                "member_stats": [{"sent_packets": n, "unique_sent_packets": n,
                                  "received_packets": 0, "sent_bytes": n * 1360,
                                  "received_bytes": 0, "retransmitted_packets": 0,
                                  "send_drops": 0, "receive_drops": 0} for n in copies]}

    def validate(self, sender=None, receiver=None, mode="broadcast"):
        return g.validate_result(sender or self.result(), receiver or self.result(),
                                 2, 10, 1316, mode)

    def test_useful_rate_does_not_multiply_by_broadcast_copies(self):
        metrics = self.validate()
        self.assertAlmostEqual(metrics["useful_mbps"], 0.5264)
        self.assertEqual(metrics["sent_packets"], 20)
        self.assertAlmostEqual(metrics["delivery_span_mbps"], 1.0528)

    def test_backup_idle_paths_do_not_invalidate_transfer(self):
        metrics = self.validate(sender=self.result((10, 0)), mode="backup")
        self.assertEqual(metrics["sent_packets"], 10)

    def test_rejects_payload_timestamp_digest_mismatch(self):
        received = self.result()
        received["sha256"] = "b" * 64
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            self.validate(receiver=received)

    def test_rejects_missing_copies_and_transport_drops(self):
        for changed in (self.result((10, 9)), self.result((0, 0))):
            with self.assertRaises(ValueError):
                self.validate(sender=changed)
        for key in ("send_drops", "receive_drops"):
            for role in ("sender", "receiver"):
                changed = self.result()
                changed["member_stats"][0][key] = 1
                with self.assertRaisesRegex(ValueError, "drop"):
                    self.validate(**{role: changed})

    def test_rejects_wrong_counts_missing_members_and_invalid_metrics(self):
        for key, value in (("messages", 9), ("useful_bytes", 0), ("members", 1),
                           ("seconds", 0), ("cpu_seconds", float("nan")),
                           ("latency_us_p99", float("inf")), ("delivery_span_seconds", -1),
                           ("sha256", "z" * 64), ("member_stats", [])):
            changed = self.result()
            changed[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.validate(receiver=changed)

    def test_failed_repeat_cannot_be_hidden_by_successful_median(self):
        cases = [{"mode": "broadcast", "members": 2, "size": 1316,
                  "label": "before", "qualified": True, "metrics": self.validate()},
                 {"mode": "broadcast", "members": 2, "size": 1316,
                  "label": "after", "qualified": False}]
        summary = g.summarize(cases)[0]
        self.assertFalse(summary["all_qualified"])
        self.assertEqual(summary["after"]["qualified_runs"], 0)

    def test_controller_reports_early_peer_exit_and_cleans_up(self):
        with tempfile.TemporaryDirectory() as directory:
            prefix = Path(directory) / "peer"
            peer = g.Peer([sys.executable, "-c", "raise SystemExit(3)"], prefix)
            try:
                with self.assertRaisesRegex(RuntimeError, "eof"):
                    peer.expect("ready", time.monotonic() + 5)
            finally:
                peer.close()
            self.assertEqual(peer.process.returncode, 3)
            self.assertFalse(peer.reader.is_alive())
            self.assertTrue(peer.stdout.closed)
            self.assertTrue(peer.stderr.closed)

    def test_controller_timeout_kills_stalled_peer(self):
        with tempfile.TemporaryDirectory() as directory:
            peer = g.Peer([sys.executable, "-c", "import time; time.sleep(30)"], Path(directory) / "peer")
            try:
                with self.assertRaisesRegex(RuntimeError, "timeout"):
                    peer.expect("ready", time.monotonic() + 0.05)
            finally:
                peer.close()
            self.assertIsNotNone(peer.process.returncode)
            self.assertFalse(peer.reader.is_alive())


if __name__ == "__main__":
    unittest.main()
